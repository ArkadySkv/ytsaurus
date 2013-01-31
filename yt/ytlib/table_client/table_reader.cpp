﻿#include "stdafx.h"
#include "table_reader.h"
#include "config.h"
#include "table_chunk_reader.h"
#include "multi_chunk_sequential_reader.h"
#include "private.h"

#include <ytlib/misc/sync.h>

#include <ytlib/chunk_client/block_cache.h>
#include <ytlib/chunk_client/chunk_meta_extensions.h>

#include <ytlib/cypress_client/cypress_ypath_proxy.h>

#include <ytlib/transaction_client/transaction.h>

namespace NYT {
namespace NTableClient {

using namespace NCypressClient;
using namespace NTableClient;

////////////////////////////////////////////////////////////////////////////////

TTableReader::TTableReader(
    TTableReaderConfigPtr config,
    NRpc::IChannelPtr masterChannel,
    NTransactionClient::ITransactionPtr transaction,
    NChunkClient::IBlockCachePtr blockCache,
    const NYPath::TRichYPath& richPath)
    : Config(config)
    , MasterChannel(masterChannel)
    , Transaction(transaction)
    , TransactionId(transaction ? transaction->GetId() : NullTransactionId)
    , BlockCache(blockCache)
    , RichPath(richPath)
    , IsOpen(false)
    , IsReadingStarted(false)
    , Proxy(masterChannel)
    , Logger(TableReaderLogger)
{
    YCHECK(masterChannel);

    Logger.AddTag(Sprintf("Path: %s, TransactihonId: %s",
        ~ToString(RichPath),
        ~TransactionId.ToString()));
}

void TTableReader::Open()
{
    VERIFY_THREAD_AFFINITY(Client);
    YASSERT(!IsOpen);

    LOG_INFO("Opening table reader");

    LOG_INFO("Fetching table info");

    auto fetchReq = TTableYPathProxy::Fetch(RichPath);
    SetTransactionId(fetchReq, TransactionId);
    fetchReq->add_extension_tags(TProtoExtensionTag<NChunkClient::NProto::TMiscExt>::Value);
    // ToDo(psushin): enable ignoring lost chunks.

    auto fetchRsp = Proxy.Execute(fetchReq).Get();
    if (!fetchRsp->IsOK()) {
        THROW_ERROR_EXCEPTION("Error fetching table info")
            << fetchRsp->GetError();
    }

    auto inputChunks = FromProto<NProto::TInputChunk>(fetchRsp->chunks());

    auto provider = New<TTableChunkReaderProvider>(Config);
    Reader = New<TTableChunkSequenceReader>(
        Config,
        MasterChannel,
        BlockCache,
        std::move(inputChunks),
        provider);
    Sync(~Reader, &TTableChunkSequenceReader::AsyncOpen);

    if (Transaction) {
        ListenTransaction(Transaction);
    }

    IsOpen = true;

    LOG_INFO("Table reader opened");
}

const TRow* TTableReader::GetRow()
{
    VERIFY_THREAD_AFFINITY(Client);
    YASSERT(IsOpen);

    CheckAborted();

    if (Reader->IsValid() && IsReadingStarted) {
        if (!Reader->FetchNextItem()) {
            Sync(~Reader, &TTableChunkSequenceReader::GetReadyEvent);
        }
    }
    IsReadingStarted = true;

    return Reader->IsValid() ? &(Reader->CurrentReader()->GetRow()) : NULL;
}

const TNonOwningKey& TTableReader::GetKey() const
{
    YUNREACHABLE();
}

i64 TTableReader::GetRowIndex() const
{
    return Reader->GetItemIndex();
}

i64 TTableReader::GetRowCount() const
{
    return Reader->GetItemCount();
}

std::vector<NChunkClient::TChunkId> TTableReader::GetFailedChunks() const
{
    return Reader->GetFailedChunks();
}

const NYTree::TYsonString& TTableReader::GetRowAttributes() const
{
    return Reader->CurrentReader()->GetRowAttributes();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
