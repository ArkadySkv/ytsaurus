﻿#include "stdafx.h"
#include "table_reader.h"
#include "config.h"
#include "table_chunk_reader.h"
#include "private.h"

#include <core/misc/sync.h>

#include <core/concurrency/scheduler.h>

#include <core/ytree/ypath_proxy.h>

#include <ytlib/chunk_client/block_cache.h>
#include <ytlib/chunk_client/chunk_meta_extensions.h>
#include <ytlib/chunk_client/old_multi_chunk_sequential_reader.h>

#include <ytlib/cypress_client/cypress_ypath_proxy.h>
#include <ytlib/cypress_client/rpc_helpers.h>

#include <ytlib/transaction_client/transaction_manager.h>

#include <ytlib/node_tracker_client/node_directory.h>

namespace NYT {
namespace NTableClient {

using namespace NYTree;
using namespace NObjectClient;
using namespace NCypressClient;
using namespace NChunkClient;
using namespace NNodeTrackerClient;
using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

TAsyncTableReader::TAsyncTableReader(
    TTableReaderConfigPtr config,
    NRpc::IChannelPtr masterChannel,
    NTransactionClient::TTransactionPtr transaction,
    NChunkClient::IBlockCachePtr blockCache,
    const NYPath::TRichYPath& richPath)
    : Config(config)
    , MasterChannel(masterChannel)
    , Transaction(transaction)
    , TransactionId(transaction ? transaction->GetId() : NullTransactionId)
    , BlockCache(blockCache)
    , NodeDirectory(New<TNodeDirectory>())
    , RichPath(richPath.Normalize())
    , IsOpen(false)
    , IsReadStarted_(false)
    , ObjectProxy(masterChannel)
    , Logger(TableClientLogger)
{
    YCHECK(masterChannel);

    Logger.AddTag(Sprintf("Path: %s, TransactihonId: %s",
        ~RichPath.GetPath(),
        ~ToString(TransactionId)));
}

void TAsyncTableReader::Open()
{
    YCHECK(!IsOpen);

    LOG_INFO("Opening table reader");

    const auto& path = RichPath.GetPath();
    auto batchReq = ObjectProxy.ExecuteBatch();

    {
        auto req = TYPathProxy::Get(path + "/@type");
        SetTransactionId(req, TransactionId);
        SetSuppressAccessTracking(req, Config->SuppressAccessTracking);
        batchReq->AddRequest(req, "get_type");
    }

    {
        auto req = TTableYPathProxy::Fetch(path);
        InitializeFetchRequest(req.Get(), RichPath);
        req->add_extension_tags(TProtoExtensionTag<NChunkClient::NProto::TMiscExt>::Value);
        SetTransactionId(req, TransactionId);
        SetSuppressAccessTracking(req, Config->SuppressAccessTracking);
        // ToDo(psushin): enable ignoring lost chunks.
        batchReq->AddRequest(req, "fetch");
    }

    auto batchRsp = WaitFor(batchReq->Invoke());
    THROW_ERROR_EXCEPTION_IF_FAILED(*batchRsp, "Error fetching table info");

    {
        auto rsp = batchRsp->GetResponse<TYPathProxy::TRspGet>("get_type");
        THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error getting object type");

        auto type = ConvertTo<EObjectType>(TYsonString(rsp->value()));
        if (type != EObjectType::Table) {
            THROW_ERROR_EXCEPTION("Invalid type of %s: expected %s, actual %s",
                ~RichPath.GetPath(),
                ~FormatEnum(EObjectType(EObjectType::Table)).Quote(),
                ~FormatEnum(type).Quote());
        }
    }

    {
        auto rsp = batchRsp->GetResponse<TTableYPathProxy::TRspFetch>("fetch");
        THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error fetching table chunks");

        NodeDirectory->MergeFrom(rsp->node_directory());
        auto chunkSpecs = FromProto<NChunkClient::NProto::TChunkSpec>(rsp->chunks());

        auto provider = New<TTableChunkReaderProvider>(
            chunkSpecs,
            Config,
            New<TChunkReaderOptions>());

        Reader = New<TTableChunkSequenceReader>(
            Config,
            MasterChannel,
            BlockCache,
            NodeDirectory,
            std::move(chunkSpecs),
            provider);
	    auto error = WaitFor(Reader->AsyncOpen());

    	THROW_ERROR_EXCEPTION_IF_FAILED(error);
    }

    if (Transaction) {
        ListenTransaction(Transaction);
    }

    IsOpen = true;

    LOG_INFO("Table reader opened");
}

bool TAsyncTableReader::FetchNextItem()
{
    YCHECK(IsOpen);

    if (Reader->GetFacade()) {
        if (IsReadStarted_) {
            return Reader->FetchNext();
        }
        IsReadStarted_ = true;
        return true;
    }
    return false;
}

TAsyncError TAsyncTableReader::GetReadyEvent()
{
    if (IsAborted()) {
        return MakeFuture(TError("Transaction aborted"));
    }
    return Reader->GetReadyEvent();
}

bool TAsyncTableReader::IsValid() const
{
    return Reader->GetFacade() != nullptr;
}

const TRow& TAsyncTableReader::GetRow() const
{
    return Reader->GetFacade()->GetRow();
}

i64 TAsyncTableReader::GetSessionRowIndex() const
{
    return Reader->GetProvider()->GetRowIndex();
}

i64 TAsyncTableReader::GetSessionRowCount() const
{
    return Reader->GetProvider()->GetRowCount();
}

i64 TAsyncTableReader::GetTableRowIndex() const
{
    return Reader->GetFacade()->GetTableRowIndex();
}

std::vector<NChunkClient::TChunkId> TAsyncTableReader::GetFailedChunkIds() const
{
    return Reader->GetFailedChunkIds();
}

int TAsyncTableReader::GetTableIndex() const
{
    return Reader->GetFacade()->GetTableIndex();
}

NChunkClient::NProto::TDataStatistics TAsyncTableReader::GetDataStatistics() const
{
    return Reader->GetProvider()->GetDataStatistics();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
