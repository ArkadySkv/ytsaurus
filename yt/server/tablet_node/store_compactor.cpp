#include "stdafx.h"
#include "store_compactor.h"
#include "config.h"
#include "tablet_slot_manager.h"
#include "tablet_slot.h"
#include "tablet_manager.h"
#include "tablet.h"
#include "store.h"
#include "partition.h"
#include "tablet_reader.h"
#include "config.h"
#include "private.h"

#include <core/concurrency/action_queue.h>
#include <core/concurrency/scheduler.h>
#include <core/concurrency/async_semaphore.h>

#include <core/logging/tagged_logger.h>

#include <core/ytree/attribute_helpers.h>

#include <ytlib/tablet_client/config.h>

#include <ytlib/transaction_client/transaction_manager.h>

#include <ytlib/new_table_client/versioned_row.h>
#include <ytlib/new_table_client/versioned_reader.h>
#include <ytlib/new_table_client/versioned_chunk_writer.h>

#include <ytlib/chunk_client/config.h>

#include <ytlib/api/client.h>
#include <ytlib/api/transaction.h>

#include <server/hydra/hydra_manager.h>
#include <server/hydra/mutation.h>

#include <server/cell_node/bootstrap.h>

namespace NYT {
namespace NTabletNode {

using namespace NConcurrency;
using namespace NYTree;
using namespace NHydra;
using namespace NVersionedTableClient;
using namespace NApi;
using namespace NChunkClient;
using namespace NTabletNode::NProto;

////////////////////////////////////////////////////////////////////////////////

static auto& Logger = TabletNodeLogger;
static const size_t MaxRowsPerRead = 1024;
static const size_t MaxRowsPerWrite = 1024;

////////////////////////////////////////////////////////////////////////////////

class TStoreCompactor
    : public TRefCounted
{
public:
    TStoreCompactor(
        TStoreCompactorConfigPtr config,
        NCellNode::TBootstrap* bootstrap)
        : Config_(config)
        , Bootstrap_(bootstrap)
        , ThreadPool_(New<TThreadPool>(Config_->ThreadPoolSize, "StoreCompact"))
        , Semaphore_(Config_->MaxConcurrentCompactions)
    { }

    void Start()
    {
        auto tabletSlotManager = Bootstrap_->GetTabletSlotManager();
        tabletSlotManager->SubscribeScanSlot(BIND(&TStoreCompactor::ScanSlot, MakeStrong(this)));
    }

private:
    TStoreCompactorConfigPtr Config_;
    NCellNode::TBootstrap* Bootstrap_;

    TThreadPoolPtr ThreadPool_;
    TAsyncSemaphore Semaphore_;


    void ScanSlot(TTabletSlotPtr slot)
    {
        if (slot->GetAutomatonState() != EPeerState::Leading)
            return;

        auto tabletManager = slot->GetTabletManager();
        auto tablets = tabletManager->Tablets().GetValues();
        for (auto* tablet : tablets) {
            ScanTablet(slot, tablet);
        }
    }

    void ScanTablet(TTabletSlotPtr slot, TTablet* tablet)
    {
        ScanEden(slot, tablet->GetEden());

        for (auto& partition : tablet->Partitions()) {
            ScanPartition(slot, partition.get());
        }
    }

    void ScanEden(TTabletSlotPtr slot, TPartition* eden)
    {
        if (eden->GetState() != EPartitionState::None)
            return;

        std::vector<IStorePtr> stores;
        for (auto store : eden->Stores()) {
            if (store->GetState() == EStoreState::Persistent) {
                stores.push_back(std::move(store));
            }
        }

        i64 dataSize = eden->GetTotalDataSize();
        int storeCount = static_cast<int>(eden->Stores().size());

        // Check if partitioning is needed.
        auto* tablet = eden->GetTablet();
        const auto& config = tablet->GetConfig();
        if (dataSize <= config->MaxEdenDataSize && storeCount <= config->MaxEdenStoreCount)
            return;

        auto guard = TAsyncSemaphoreGuard::TryAcquire(&Semaphore_);
        if (!guard)
            return;

        // Limit the number of chunks to process at once.
        if (storeCount > Config_->MaxChunksPerCompaction) {
            stores.erase(
                stores.begin() + Config_->MaxChunksPerCompaction,
                stores.end());
        }

        for (auto store : stores) {
            store->SetState(EStoreState::Compacting);
        }

        eden->SetState(EPartitionState::Compacting);

        std::vector<TOwningKey> pivotKeys;
        for (const auto& partition : tablet->Partitions()) {
            pivotKeys.push_back(partition->GetPivotKey());
        }

        tablet->GetEpochAutomatonInvoker(EAutomatonThreadQueue::Write)->Invoke(BIND(
            &TStoreCompactor::PartitionEden,
            MakeStrong(this),
            Passed(std::move(guard)),
            eden,
            pivotKeys,
            stores));
    }

    void ScanPartition(TTabletSlotPtr slot, TPartition* partition)
    {
        if (partition->GetState() != EPartitionState::None)
            return;

        std::vector<IStorePtr> allStores;
        for (auto store : partition->Stores()) {
            if (store->GetState() == EStoreState::Persistent) {
                allStores.push_back(std::move(store));
            }
        }

        // Don't compact partitions whose data size exceeds the limit.
        // Let Partition Balancer do its job.
        auto* tablet = partition->GetTablet();
        const auto& config = tablet->GetConfig();
        if (partition->GetTotalDataSize() > config->MaxPartitionDataSize)
            return;

        auto stores = PickStoresForCompaction(allStores);
        if (stores.empty())
            return;

        auto guard = TAsyncSemaphoreGuard::TryAcquire(&Semaphore_);
        if (!guard)
            return;

        for (auto store : stores) {
            store->SetState(EStoreState::Compacting);
        }

        partition->SetState(EPartitionState::Compacting);

        tablet->GetEpochAutomatonInvoker(EAutomatonThreadQueue::Write)->Invoke(BIND(
            &TStoreCompactor::CompactPartition,
            MakeStrong(this),
            Passed(std::move(guard)),
            partition,
            stores));
    }


    std::vector<IStorePtr> PickStoresForCompaction(std::vector<IStorePtr>& allStores)
    {
        // TODO(babenko): need some smart code here
        const int Limit = 3;
        int n = static_cast<int>(allStores.size());
        if (n <= Limit) {
            return std::vector<IStorePtr>();
        }

        return std::vector<IStorePtr>(
            allStores.begin(),
            allStores.begin() + std::min(n, Config_->MaxChunksPerCompaction));
    }


    void PartitionEden(
        TAsyncSemaphoreGuard /*guard*/,
        TPartition* eden,
        const std::vector<TOwningKey>& pivotKeys,
        const std::vector<IStorePtr>& stores)
    {
        // Capture everything needed below.
        // NB: Avoid accessing tablet from pool invoker.
        auto* tablet = eden->GetTablet();
        auto* slot = tablet->GetSlot();
        auto tabletManager = slot->GetTabletManager();
        auto tabletId = tablet->GetId();
        auto writerOptions = tablet->GetWriterOptions();
        auto tabletPivotKey = tablet->GetPivotKey();
        auto nextTabletPivotKey = tablet->GetNextPivotKey();
        auto keyColumns = tablet->KeyColumns();
        auto schema = tablet->Schema();

        YCHECK(tabletPivotKey == pivotKeys[0]);

        NLog::TTaggedLogger Logger(TabletNodeLogger);
        Logger.AddTag(Sprintf("TabletId: %s",
            ~ToString(tabletId)));

        auto automatonInvoker = GetCurrentInvoker();
        auto poolInvoker = ThreadPool_->GetInvoker();

        try {
            i64 dataSize = 0;
            for (auto store : stores) {
                dataSize += store->GetDataSize();
            }

            LOG_INFO("Eden partitioning started (PartitionCount: %d, DataSize: % " PRId64 ", ChunkCount: %d)",
                static_cast<int>(pivotKeys.size()),
                dataSize,
                static_cast<int>(stores.size()));

            auto reader = CreateVersionedTabletReader(
                tablet,
                stores,
                tabletPivotKey,
                nextTabletPivotKey,
                AllCommittedTimestamp);

            SwitchTo(poolInvoker);

            ITransactionPtr transaction;
            {
                LOG_INFO("Creating Eden partitioning transaction");
                NTransactionClient::TTransactionStartOptions options;
                options.AutoAbort = false;
                auto attributes = CreateEphemeralAttributes();
                attributes->Set("title", Sprintf("Eden partitioning, tablet %s",
                    ~ToString(tabletId)));
                options.Attributes = attributes.get();
                auto transactionOrError = WaitFor(Bootstrap_->GetMasterClient()->StartTransaction(
                    NTransactionClient::ETransactionType::Master,
                    options));
                THROW_ERROR_EXCEPTION_IF_FAILED(transactionOrError);
                transaction = transactionOrError.Value();
            }

            std::vector<TVersionedRow> writeRows;
            writeRows.reserve(MaxRowsPerWrite);

            int currentPartitionIndex = 0;
            TOwningKey currentPivotKey;
            TOwningKey nextPivotKey;

            int currentPartitionRowCount = 0;
            int readRowCount = 0;
            int writeRowCount = 0;
            IVersionedMultiChunkWriterPtr currentWriter;

            TReqCommitTabletStoresUpdate updateStoresRequest;
            ToProto(updateStoresRequest.mutable_tablet_id(), tabletId);

            for (auto store : stores) {
                auto* descriptor = updateStoresRequest.add_stores_to_remove();
                ToProto(descriptor->mutable_store_id(), store->GetId());
            }

            auto startPartition = [&] () {
                YCHECK(!currentWriter);

                LOG_INFO("Started writing partition (PartitionIndex: %d, Keys: %s .. %s)",
                    currentPartitionIndex,
                    ~ToString(currentPivotKey),
                    ~ToString(nextPivotKey));

                currentWriter = CreateVersionedMultiChunkWriter(
                    Config_->Writer,
                    writerOptions,
                    schema,
                    keyColumns,
                    Bootstrap_->GetMasterClient()->GetMasterChannel(),
                    transaction->GetId());

                {
                    auto result = WaitFor(currentWriter->Open());
                    THROW_ERROR_EXCEPTION_IF_FAILED(result);
                }
            };

            auto flushOutputRows = [&] () {
                if (writeRows.empty())
                    return;

                writeRowCount += writeRows.size();
                if (!currentWriter->Write(writeRows)) {
                    auto result = WaitFor(currentWriter->GetReadyEvent());
                    THROW_ERROR_EXCEPTION_IF_FAILED(result);
                }
                writeRows.clear();
            };

            auto writeOutputRow = [&] (TVersionedRow row) {
                if (writeRows.size() ==  writeRows.capacity()) {
                    flushOutputRows();
                }
                writeRows.push_back(row);
                ++currentPartitionRowCount;
            };

            auto finishPartition = [&] () {
                YCHECK(currentWriter);

                flushOutputRows();

                {
                    auto result = WaitFor(currentWriter->Close());
                    THROW_ERROR_EXCEPTION_IF_FAILED(result);
                }

                LOG_INFO("Finished writing partition (PartitionIndex: %d, RowCount: %d)",
                    currentPartitionIndex,
                    currentPartitionRowCount);

                for (const auto& chunkSpec : currentWriter->GetWrittenChunks()) {
                    auto* descriptor = updateStoresRequest.add_stores_to_add();
                    descriptor->mutable_store_id()->CopyFrom(chunkSpec.chunk_id());
                    descriptor->mutable_chunk_meta()->CopyFrom(chunkSpec.chunk_meta());
                }

                currentWriter.Reset();
                currentPartitionRowCount = 0;
                ++currentPartitionIndex;
            };

            std::vector<TVersionedRow> readRows;
            readRows.reserve(MaxRowsPerRead);
            int currentRowIndex = 0;

            auto peekInputRow = [&] () -> TVersionedRow {
                if (currentRowIndex == static_cast<int>(readRows.size())) {
                    // readRows will be invalidated, must flush writeRows.
                    flushOutputRows();
                    currentRowIndex = 0;
                    while (true) {
                        if (!reader->Read(&readRows)) {
                            return TVersionedRow();
                        }
                        readRowCount += readRows.size();
                        if (!readRows.empty())
                            break;
                        auto result = WaitFor(reader->GetReadyEvent());
                        THROW_ERROR_EXCEPTION_IF_FAILED(result);
                    }
                }
                return readRows[currentRowIndex];
            };

            auto skipInputRow = [&] () {
                ++currentRowIndex;
            };

            {
                auto result = WaitFor(reader->Open());
                THROW_ERROR_EXCEPTION_IF_FAILED(result);
            }

            for (auto it = pivotKeys.begin(); it != pivotKeys.end(); ++it) {
                currentPivotKey = *it;
                nextPivotKey = it == pivotKeys.end() - 1 ? nextTabletPivotKey : *(it + 1);

                startPartition();
                
                while (true) {
                    auto row = peekInputRow();
                    if (!row)
                        break;

                    YCHECK(CompareRows(currentPivotKey.Begin(), currentPivotKey.End(), row.BeginKeys(), row.EndKeys()) <= 0);

                    if (CompareRows(nextPivotKey.Begin(), nextPivotKey.End(), row.BeginKeys(), row.EndKeys()) <= 0)
                        break;

                    skipInputRow();
                    writeOutputRow(row);
                }

                finishPartition();
            }
            
            SwitchTo(automatonInvoker);

            YCHECK(readRowCount == writeRowCount);
            LOG_INFO("Eden partitioning completed (RowCount: %d)",
                readRowCount);

            CreateMutation(slot->GetHydraManager(), updateStoresRequest)
                ->Commit();

            // Just abandon the transaction, hopefully it won't expire before the chunk is attached.
        } catch (const std::exception& ex) {
            LOG_ERROR(ex, "Error partitioning Eden, backing off");

            SwitchTo(automatonInvoker);

            for (auto store : stores) {
                YCHECK(store->GetState() == EStoreState::Compacting);
                tabletManager->BackoffStore(store, EStoreState::CompactionFailed);
            }
        }

        YCHECK(eden->GetState() == EPartitionState::Compacting);
        eden->SetState(EPartitionState::None);
    }

    void CompactPartition(
        TAsyncSemaphoreGuard /*guard*/,
        TPartition* partition,
        const std::vector<IStorePtr>& stores)
    {
        // Capture everything needed below.
        // NB: Avoid accessing tablet from pool invoker.
        auto* tablet = partition->GetTablet();
        auto* slot = tablet->GetSlot();
        auto tabletManager = slot->GetTabletManager();
        auto tabletId = tablet->GetId();
        auto writerOptions = tablet->GetWriterOptions();
        auto tabletPivotKey = tablet->GetPivotKey();
        auto nextTabletPivotKey = tablet->GetNextPivotKey();
        auto keyColumns = tablet->KeyColumns();
        auto schema = tablet->Schema();

        NLog::TTaggedLogger Logger(TabletNodeLogger);
        Logger.AddTag(Sprintf("TabletId: %s, PartitionRange: %s .. %s",
            ~ToString(tabletId),
            ~ToString(partition->GetPivotKey()),
            ~ToString(partition->GetNextPivotKey())));

        auto automatonInvoker = GetCurrentInvoker();
        auto poolInvoker = ThreadPool_->GetInvoker();

        try {
            i64 dataSize = 0;
            for (auto store : stores) {
                dataSize += store->GetDataSize();
            }

            LOG_INFO("Partition compaction started (DataSize: % " PRId64 ", ChunkCount: %d)",
                dataSize,
                static_cast<int>(stores.size()));

            auto reader = CreateVersionedTabletReader(
                tablet,
                stores,
                tabletPivotKey,
                nextTabletPivotKey,
                AllCommittedTimestamp);

            SwitchTo(poolInvoker);

            auto transactionManager = Bootstrap_->GetMasterClient()->GetTransactionManager();
        
            ITransactionPtr transaction;
            {
                LOG_INFO("Creating partition compaction transaction");
                NTransactionClient::TTransactionStartOptions options;
                options.AutoAbort = false;
                auto attributes = CreateEphemeralAttributes();
                attributes->Set("title", Sprintf("Partition compaction, tablet %s",
                    ~ToString(tabletId)));
                options.Attributes = attributes.get();
                auto transactionOrError = WaitFor(Bootstrap_->GetMasterClient()->StartTransaction(
                    NTransactionClient::ETransactionType::Master,
                    options));
                THROW_ERROR_EXCEPTION_IF_FAILED(transactionOrError);
                transaction = transactionOrError.Value();
            }

            TReqCommitTabletStoresUpdate updateStoresRequest;
            ToProto(updateStoresRequest.mutable_tablet_id(), tabletId);

            for (auto store : stores) {
                auto* descriptor = updateStoresRequest.add_stores_to_remove();
                ToProto(descriptor->mutable_store_id(), store->GetId());
            }

            auto writer = CreateVersionedMultiChunkWriter(
                Config_->Writer,
                writerOptions,
                schema,
                keyColumns,
                Bootstrap_->GetMasterClient()->GetMasterChannel(),
                transaction->GetId());

            {
                auto result = WaitFor(reader->Open());
                THROW_ERROR_EXCEPTION_IF_FAILED(result);
            }

            {
                auto result = WaitFor(writer->Open());
                THROW_ERROR_EXCEPTION_IF_FAILED(result);
            }

            std::vector<TVersionedRow> rows;

            int readRowCount = 0;
            int writeRowCount = 0;

            while (reader->Read(&rows)) {
                readRowCount += rows.size();

                if (rows.empty()) {
                    auto result = WaitFor(reader->GetReadyEvent());
                    THROW_ERROR_EXCEPTION_IF_FAILED(result);
                    continue;
                }

                writeRowCount += rows.size();
                if (!writer->Write(rows)) {
                    auto result = WaitFor(writer->GetReadyEvent());
                    THROW_ERROR_EXCEPTION_IF_FAILED(result);
                }
            }

            {
                auto result = WaitFor(writer->Close());
                THROW_ERROR_EXCEPTION_IF_FAILED(result);
            }

            for (const auto& chunkSpec : writer->GetWrittenChunks()) {
                auto* descriptor = updateStoresRequest.add_stores_to_add();
                descriptor->mutable_store_id()->CopyFrom(chunkSpec.chunk_id());
                descriptor->mutable_chunk_meta()->CopyFrom(chunkSpec.chunk_meta());
            }

            SwitchTo(automatonInvoker);

            YCHECK(readRowCount == writeRowCount);
            LOG_INFO("Partition compaction completed (RowCount: %d)",
                readRowCount);

            CreateMutation(slot->GetHydraManager(), updateStoresRequest)
                ->Commit();

            // Just abandon the transaction, hopefully it won't expire before the chunk is attached.
        } catch (const std::exception& ex) {
            LOG_ERROR(ex, "Error compacting partition, backing off");

            SwitchTo(automatonInvoker);

            for (auto store : stores) {
                YCHECK(store->GetState() == EStoreState::Compacting);
                tabletManager->BackoffStore(store, EStoreState::CompactionFailed);
            }
        }

        YCHECK(partition->GetState() == EPartitionState::Compacting);
        partition->SetState(EPartitionState::None);
    }

};

void StartStoreCompactor(
    TStoreCompactorConfigPtr config,
    NCellNode::TBootstrap* bootstrap)
{
    New<TStoreCompactor>(config, bootstrap)->Start();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletNode
} // namespace NYT
