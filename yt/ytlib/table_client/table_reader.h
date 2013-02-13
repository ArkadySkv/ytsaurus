﻿#pragma once

#include "public.h"
#include "sync_reader.h"

#include <ytlib/logging/tagged_logger.h>

#include <ytlib/misc/thread_affinity.h>

#include <ytlib/ypath/rich.h>

#include <ytlib/transaction_client/public.h>
#include <ytlib/transaction_client/transaction_listener.h>

#include <ytlib/cypress_client/public.h>

#include <ytlib/object_client/object_service_proxy.h>

#include <ytlib/table_client/table_ypath_proxy.h>

#include <ytlib/chunk_client/public.h>

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

//! A client-side facade for reading tables.
/*!
 *  The client must first call #Open. This positions the reader before the first row.
 *
 *  Then the client it must iteratively fetch rows by calling #NextRow.
 *  When no more rows can be fetched, the latter returns False.
 *
 */
class TTableReader
    : public NTransactionClient::TTransactionListener
    , public ISyncReader
{
public:
    //! Initializes an instance.
    TTableReader(
        TTableReaderConfigPtr config,
        NRpc::IChannelPtr masterChannel,
        NTransactionClient::ITransactionPtr transaction,
        NChunkClient::IBlockCachePtr blockCache,
        const NYPath::TRichYPath& richPath);

    virtual void Open() override;

    virtual const TRow* GetRow() override;
    virtual const TNonOwningKey& GetKey() const override;
    virtual const NYTree::TYsonString& GetRowAttributes() const override;

    virtual i64 GetRowIndex() const override;
    virtual i64 GetRowCount() const override;
    virtual std::vector<NChunkClient::TChunkId> GetFailedChunks() const override;

private:
    TTableReaderConfigPtr Config;
    NRpc::IChannelPtr MasterChannel;
    NTransactionClient::ITransactionPtr Transaction;
    NTransactionClient::TTransactionId TransactionId;
    NChunkClient::IBlockCachePtr BlockCache;
    NYPath::TRichYPath RichPath;
    bool IsOpen;
    bool IsReadingStarted;
    NObjectClient::TObjectServiceProxy Proxy;
    NLog::TTaggedLogger Logger;

    TTableChunkSequenceReaderPtr Reader;
    NCypressClient::TNodeId NodeId;

    DECLARE_THREAD_AFFINITY_SLOT(Client);

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
