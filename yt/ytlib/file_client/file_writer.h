#pragma once

#include "public.h"

#include <ytlib/misc/ref.h>

#include <ytlib/ytree/public.h>

#include <ytlib/cypress_client/public.h>

#include <ytlib/chunk_client/public.h>
#include <ytlib/chunk_client/multi_chunk_sequential_writer.h>

#include <ytlib/transaction_client/public.h>
#include <ytlib/transaction_client/transaction_listener.h>

#include <ytlib/rpc/public.h>

#include <ytlib/ypath/rich.h>

#include <ytlib/logging/tagged_logger.h>

namespace NYT {
namespace NFileClient {

////////////////////////////////////////////////////////////////////////////////

//! A client-side facade for writing files.
/*!
 *  The client must call #AsyncOpen and then feed the data in by calling #AsyncWrite.
 *  Finally it must call #AsyncClose.
 */
class TAsyncWriter
    : public NTransactionClient::TTransactionListener
{
public:
    TAsyncWriter(
        TFileWriterConfigPtr config,
        NRpc::IChannelPtr masterChannel,
        NTransactionClient::ITransactionPtr transaction,
        NTransactionClient::TTransactionManagerPtr transactionManager,
        const NYPath::TRichYPath& richPath);

    ~TAsyncWriter();

    TAsyncError AsyncOpen();
    TAsyncError AsyncWrite(const TRef& data);
    void Close();

private:
    typedef TAsyncWriter TThis;
    typedef NChunkClient::TMultiChunkSequentialWriter<TFileChunkWriter> TWriter;

    TFileWriterConfigPtr Config;
    NRpc::IChannelPtr MasterChannel;

    NTransactionClient::ITransactionPtr Transaction;
    NTransactionClient::TTransactionManagerPtr TransactionManager;
    NTransactionClient::ITransactionPtr UploadTransaction;
    NYPath::TRichYPath RichPath;

    TIntrusivePtr<TWriter> Writer;

    NLog::TTaggedLogger Logger;

    NCypressClient::TNodeId NodeId;

    TAsyncError OnUploadTransactionStarted(
        TErrorOr<NTransactionClient::ITransactionPtr> transactionOrError);
    TAsyncError OnFileInfoReceived(NObjectClient::TObjectServiceProxy::TRspExecuteBatchPtr batchRsp);

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NFileClient
} // namespace NYT
