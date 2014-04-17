#include "stdafx.h"
#include "connection.h"
#include "config.h"
#include "private.h"

#include <core/concurrency/scheduler.h>

#include <core/rpc/bus_channel.h>
#include <core/rpc/retrying_channel.h>
#include <core/rpc/caching_channel_factory.h>

#include <ytlib/hydra/peer_channel.h>
#include <ytlib/hydra/config.h>

#include <ytlib/scheduler/scheduler_channel.h>

#include <ytlib/chunk_client/block_cache.h>
#include <ytlib/chunk_client/client_block_cache.h>
#include <ytlib/chunk_client/chunk_replica.h>

#include <ytlib/hive/cell_directory.h>

#include <ytlib/tablet_client/table_mount_cache.h>
#include <ytlib/tablet_client/wire_protocol.h>

#include <ytlib/query_client/callbacks.h>
#include <ytlib/query_client/helpers.h>
#include <ytlib/query_client/plan_context.h>
#include <ytlib/query_client/plan_fragment.h>
#include <ytlib/query_client/query_service_proxy.h>

#include <ytlib/transaction_client/timestamp_provider.h>
#include <ytlib/transaction_client/remote_timestamp_provider.h>

#include <ytlib/driver/dispatcher.h>

#include <ytlib/object_client/helpers.h>

#include <ytlib/node_tracker_client/node_directory.h>

#include <util/random/random.h>

// TODO(babenko): consider removing
#include <ytlib/object_client/object_service_proxy.h>
#include <ytlib/table_client/table_ypath_proxy.h>
#include <ytlib/table_client/chunk_meta_extensions.h>
#include <ytlib/new_table_client/chunk_meta_extensions.h>
#include <ytlib/new_table_client/schemaful_reader.h>

namespace NYT {
namespace NApi {

using namespace NConcurrency;
using namespace NRpc;
using namespace NYPath;
using namespace NHive;
using namespace NHydra;
using namespace NChunkClient;
using namespace NTabletClient;
using namespace NQueryClient;
using namespace NVersionedTableClient;
using namespace NTransactionClient;
using namespace NObjectClient;
using namespace NTableClient;  // TODO(babenko): consider removing
using namespace NTableClient::NProto;
using namespace NVersionedTableClient::NProto;
using namespace NNodeTrackerClient;

////////////////////////////////////////////////////////////////////////////////

static auto& Logger = ApiLogger;

////////////////////////////////////////////////////////////////////////////////

class TQueryResponseReader
    : public ISchemafulReader
{
public:
    explicit TQueryResponseReader(TQueryServiceProxy::TInvExecute asyncResponse)
        : AsyncResponse_(std::move(asyncResponse))
    { }

    virtual TAsyncError Open(const TTableSchema& schema) override
    {
        return AsyncResponse_.Apply(BIND(
            &TQueryResponseReader::OnResponse,
            MakeStrong(this),
            schema));
    }

    virtual bool Read(std::vector<TUnversionedRow>* rows) override
    {
        return RowsetReader_->Read(rows);
    }

    virtual TAsyncError GetReadyEvent() override
    {
        return RowsetReader_->GetReadyEvent();
    }

private:
    TQueryServiceProxy::TInvExecute AsyncResponse_;

    std::unique_ptr<TWireProtocolReader> ProtocolReader_;
    ISchemafulReaderPtr RowsetReader_;

    
    TError OnResponse(
        const TTableSchema& schema,
        TQueryServiceProxy::TRspExecutePtr response)
    {
        if (!response->IsOK()) {
            return response->GetError();
        }

        YCHECK(!ProtocolReader_);
        ProtocolReader_.reset(new TWireProtocolReader(response->encoded_response()));

        YCHECK(!RowsetReader_);
        RowsetReader_ = ProtocolReader_->CreateSchemafulRowsetReader();

        auto asyncResult = RowsetReader_->Open(schema);
        YCHECK(asyncResult.IsSet()); // this reader is sync
        return asyncResult.Get();
    }


};

////////////////////////////////////////////////////////////////////////////////

class TConnection
    : public IConnection
    , public IPrepareCallbacks
    , public ICoordinateCallbacks
{
public:
    explicit TConnection(TConnectionConfigPtr config)
        : Config_(config)
    {
        auto channelFactory = GetBusChannelFactory();

        auto leaderChannel = CreateLeaderChannel(
            Config_->Masters,
            channelFactory);
        MasterChannel_ = CreateRetryingChannel(
            Config_->Masters,
            leaderChannel);
        MasterChannel_->SetDefaultTimeout(Config_->Masters->RpcTimeout);

        SchedulerChannel_ = CreateSchedulerChannel(
            Config_->Scheduler,
            channelFactory,
            MasterChannel_);

        NodeChannelFactory_ = CreateCachingChannelFactory(GetBusChannelFactory());

        TimestampProvider_ = CreateRemoteTimestampProvider(
            Config_->TimestampProvider,
            channelFactory);

        CellDirectory_ = New<TCellDirectory>(
            Config_->CellDirectory,
            channelFactory);
        CellDirectory_->RegisterCell(config->Masters);

        BlockCache_ = CreateClientBlockCache(
            Config_->BlockCache);

        TableMountCache_ = New<TTableMountCache>(
            Config_->TableMountCache,
            MasterChannel_,
            CellDirectory_);
    }


    // IConnection implementation.

    virtual TConnectionConfigPtr GetConfig() override
    {
        return Config_;
    }

    virtual IChannelPtr GetMasterChannel() override
    {
        return MasterChannel_;
    }

    virtual IChannelPtr GetSchedulerChannel() override
    {
        return SchedulerChannel_;
    }

    virtual IChannelFactoryPtr GetNodeChannelFactory() override
    {
        return NodeChannelFactory_;
    }

    virtual IBlockCachePtr GetBlockCache() override
    {
        return BlockCache_;
    }

    virtual TTableMountCachePtr GetTableMountCache() override
    {
        return TableMountCache_;
    }

    virtual ITimestampProviderPtr GetTimestampProvider() override
    {
        return TimestampProvider_;
    }

    virtual TCellDirectoryPtr GetCellDirectory() override
    {
        return CellDirectory_;
    }

    virtual IPrepareCallbacks* GetQueryPrepareCallbacks() override
    {
        return this;
    }

    virtual ICoordinateCallbacks* GetQueryCoordinateCallbacks() override
    {
        return this;
    }


    // IPrepareCallbacks implementation.

    virtual TFuture<TErrorOr<TDataSplit>> GetInitialSplit(
        const TYPath& path,
        TPlanContextPtr context) override
    {
        return BIND(&TConnection::DoGetInitialSplit, MakeStrong(this))
            .Guarded()
            .AsyncVia(NDriver::TDispatcher::Get()->GetLightInvoker())
            .Run(path, std::move(context));
    }


    // ICoordinateCallbacks implementation.

    virtual ISchemafulReaderPtr GetReader(
        const TDataSplit& /*split*/,
        TPlanContextPtr /*context*/) override
    {
        YUNREACHABLE();
    }

    virtual bool CanSplit(const TDataSplit& split) override
    {
        return TypeFromId(GetObjectIdFromDataSplit(split)) == EObjectType::Table;
    }

    virtual TFuture<TErrorOr<std::vector<TDataSplit>>> SplitFurther(
        const TDataSplit& split,
        TPlanContextPtr context) override
    {
        return
            BIND(&TConnection::DoSplitFurther, MakeStrong(this))
                .Guarded()
                .AsyncVia(NDriver::TDispatcher::Get()->GetLightInvoker())
                .Run(split, std::move(context));
    }

    virtual TGroupedDataSplits Regroup(
        const TDataSplits& splits,
        TPlanContextPtr context) override
    {
        std::map<Stroka, TDataSplits> groups;
        TGroupedDataSplits result;

        auto nodeDirectory = context->GetNodeDirectory();

        for (const auto& split : splits) {
            auto replicas = FromProto<TChunkReplica, TChunkReplicaList>(split.replicas());
            if (replicas.empty()) {
                auto objectId = GetObjectIdFromDataSplit(split);
                THROW_ERROR_EXCEPTION("No alive replicas for split %s",
                    ~ToString(objectId));
            }
            auto replica = replicas[RandomNumber(replicas.size())];
            auto descriptor = nodeDirectory->GetDescriptor(replica);

            groups[descriptor.Address].push_back(split);
        }

        result.reserve(groups.size());
        for (const auto& group : groups) {
            result.emplace_back(std::move(group.second));
        }

        return result;
    }

    virtual ISchemafulReaderPtr Delegate(
        const TPlanFragment& fragment,
        const TDataSplit& collocatedSplit) override
    {
        auto replicas = FromProto<TChunkReplica, TChunkReplicaList>(collocatedSplit.replicas());
        YCHECK(!replicas.empty());
        auto replica = replicas[RandomNumber(replicas.size())];

        auto descriptor = fragment.GetContext()->GetNodeDirectory()->GetDescriptor(replica);
        auto address = descriptor.Address;

        LOG_DEBUG("Delegating fragment (FragmentId: %s, Address: %s)",
            ~ToString(fragment.Id()),
            ~address);

        auto channel = NodeChannelFactory_->CreateChannel(address);

        TQueryServiceProxy proxy(channel);
        auto req = proxy.Execute();

        fragment.GetContext()->GetNodeDirectory()->DumpTo(req->mutable_node_directory());
        ToProto(req->mutable_plan_fragment(), fragment);
        return New<TQueryResponseReader>(req->Invoke());
    }

private:
    TConnectionConfigPtr Config_;

    IChannelPtr MasterChannel_;
    IChannelPtr SchedulerChannel_;
    IChannelFactoryPtr NodeChannelFactory_;
    IBlockCachePtr BlockCache_;
    TTableMountCachePtr TableMountCache_;
    ITimestampProviderPtr TimestampProvider_;
    TCellDirectoryPtr CellDirectory_;

    TDataSplit DoGetInitialSplit(
        const TYPath& path,
        TPlanContextPtr context)
    {
        LOG_DEBUG("Getting initial split (Path: %s)",
            ~path);

        auto asyncInfoOrError = TableMountCache_->GetTableInfo(path);
        auto infoOrError = WaitFor(asyncInfoOrError);
        THROW_ERROR_EXCEPTION_IF_FAILED(infoOrError);
        const auto& info = infoOrError.Value();

        TDataSplit result;
        SetObjectId(&result, info->TableId);
        SetTableSchema(&result, info->Schema);
        SetKeyColumns(&result, info->KeyColumns);
        SetTimestamp(&result, context->GetTimestamp());
        return result;
    }

    std::vector<TDataSplit> DoSplitFurther(
        const TDataSplit& split,
        TPlanContextPtr context)
    {
        auto objectId = GetObjectIdFromDataSplit(split);

        std::vector<TDataSplit> subsplits;
        switch (TypeFromId(objectId)) {
            case EObjectType::Table:
                subsplits = DoSplitTableFurther(split, std::move(context));
                break;

            default:
                YUNREACHABLE();
        }

        LOG_DEBUG("Subsplits built (ObjectId: %s, SubsplitCount: %d)",
            ~ToString(objectId),
            static_cast<int>(subsplits.size()));

        return subsplits;
    }

    std::vector<TDataSplit> DoSplitTableFurther(
        const TDataSplit& split,
        TPlanContextPtr context)
    {
        auto tableId = GetObjectIdFromDataSplit(split);
        auto tableInfoOrError = WaitFor(TableMountCache_->GetTableInfo(FromObjectId(tableId)));
        THROW_ERROR_EXCEPTION_IF_FAILED(tableInfoOrError);
        const auto& tableInfo = tableInfoOrError.Value();

        return tableInfo->Sorted
            ? DoSplitSortedTableFurther(split, std::move(context))
            : DoSplitUnsortedTableFurther(split, std::move(context), std::move(tableInfo));
    }

    std::vector<TDataSplit> DoSplitSortedTableFurther(
        const TDataSplit& split,
        TPlanContextPtr context)
    {
        auto tableId = GetObjectIdFromDataSplit(split);
        LOG_DEBUG("Splitting sorted table further into chunks (TableId: %s)",
            ~ToString(tableId));

        // TODO(babenko): refactor and optimize
        TObjectServiceProxy proxy(MasterChannel_);

        auto req = TTableYPathProxy::Fetch(FromObjectId(tableId));
        req->set_fetch_all_meta_extensions(true);

        auto rsp = WaitFor(proxy.Execute(req));
        THROW_ERROR_EXCEPTION_IF_FAILED(*rsp);

        context->GetNodeDirectory()->MergeFrom(rsp->node_directory());

        auto chunkSpecs = FromProto<NChunkClient::NProto::TChunkSpec>(rsp->chunks());
        auto keyColumns = GetKeyColumnsFromDataSplit(split);
        auto schema = GetTableSchemaFromDataSplit(split);

        for (auto& chunkSpec : chunkSpecs) {
            auto chunkKeyColumns = FindProtoExtension<TKeyColumnsExt>(chunkSpec.chunk_meta().extensions());
            auto chunkSchema = FindProtoExtension<TTableSchemaExt>(chunkSpec.chunk_meta().extensions());

            // TODO(sandello): One day we should validate consistency.
            // Now we just check we do _not_ have any of these.
            YCHECK(!chunkKeyColumns);
            YCHECK(!chunkSchema);

            SetKeyColumns(&chunkSpec, keyColumns);
            SetTableSchema(&chunkSpec, schema);

            auto boundaryKeys = FindProtoExtension<TOldBoundaryKeysExt>(chunkSpec.chunk_meta().extensions());
            if (boundaryKeys) {
                auto chunkLowerBound = NYT::FromProto<TOwningKey>(boundaryKeys->start());
                auto chunkUpperBound = NYT::FromProto<TOwningKey>(boundaryKeys->end());
                // Boundary keys are exact, so advance right bound to its successor.
                chunkUpperBound = GetKeySuccessor(chunkUpperBound.Get());
                SetLowerBound(&chunkSpec, chunkLowerBound);
                SetUpperBound(&chunkSpec, chunkUpperBound);
            }
        }

        return chunkSpecs;
    }

    std::vector<TDataSplit> DoSplitUnsortedTableFurther(
        const TDataSplit& split,
        TPlanContextPtr context,
        TTableMountInfoPtr tableInfo)
    {
        auto tableId = GetObjectIdFromDataSplit(split);
        LOG_DEBUG("Splitting unsorted table further into tablets (TableId: %s)",
            ~ToString(tableId));

        if (tableInfo->Tablets.empty()) {
            THROW_ERROR_EXCEPTION("Table %s is neither sorted nor has tablets",
                ~ToString(tableId));
        }

        auto lowerBound = GetLowerBoundFromDataSplit(split);
        auto upperBound = GetUpperBoundFromDataSplit(split);
        auto keyColumns = GetKeyColumnsFromDataSplit(split);
        auto schema = GetTableSchemaFromDataSplit(split);

        // Run binary search to find the relevant tablets.
        auto startIt = std::upper_bound(
            tableInfo->Tablets.begin(),
            tableInfo->Tablets.end(),
            lowerBound,
            [] (const TOwningKey& key, const TTabletInfoPtr& tabletInfo) {
                return key < tabletInfo->PivotKey;
            }) - 1;

        auto nodeDirectory = context->GetNodeDirectory();

        std::vector<TDataSplit> subsplits;
        for (auto it = startIt; it != tableInfo->Tablets.end(); ++it) {
            const auto& tabletInfo = *it;
            if (upperBound <= tabletInfo->PivotKey)
                break;

            if (tabletInfo->State != ETabletState::Mounted) {
                // TODO(babenko): learn to work with unmounted tablets
                THROW_ERROR_EXCEPTION("Tablet %s is not mounted",
                    ~ToString(tabletInfo->TabletId));
            }

            TDataSplit subsplit;
            SetObjectId(&subsplit, tabletInfo->TabletId);   
            SetKeyColumns(&subsplit, keyColumns);
            SetTableSchema(&subsplit, schema);
            
            auto pivotKey = tabletInfo->PivotKey;
            auto nextPivotKey = (it + 1 == tableInfo->Tablets.end()) ? MaxKey() : (*(it + 1))->PivotKey;

            SetLowerBound(&subsplit, std::max(lowerBound, pivotKey));
            SetUpperBound(&subsplit, std::min(upperBound, nextPivotKey));

            SetTimestamp(&subsplit, context->GetTimestamp());

            for (const auto& tabletReplica : tabletInfo->Replicas) {
                nodeDirectory->AddDescriptor(tabletReplica.Id, tabletReplica.Descriptor);
                TChunkReplica chunkReplica(tabletReplica.Id, 0);
                subsplit.add_replicas(ToProto<ui32>(chunkReplica));
            }

            subsplits.push_back(std::move(subsplit));
        }
        return subsplits;
    }

};

IConnectionPtr CreateConnection(TConnectionConfigPtr config)
{
    return New<TConnection>(config);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NApi
} // namespace NYT
