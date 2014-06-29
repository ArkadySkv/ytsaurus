#include "stdafx.h"
#include "file_writer.h"
#include "config.h"
#include "client.h"
#include "private.h"

#include <core/concurrency/scheduler.h>

#include <core/logging/tagged_logger.h>

#include <ytlib/object_client/object_service_proxy.h>

#include <ytlib/cypress_client/cypress_ypath_proxy.h>

#include <ytlib/file_client/file_ypath_proxy.h>
#include <ytlib/file_client/file_chunk_writer.h>

#include <ytlib/chunk_client/private.h>
#include <ytlib/chunk_client/chunk_spec.h>
#include <ytlib/chunk_client/multi_chunk_sequential_writer.h>
#include <ytlib/chunk_client/dispatcher.h>

#include <ytlib/transaction_client/transaction_manager.h>
#include <ytlib/transaction_client/transaction_listener.h>
#include <ytlib/transaction_client/helpers.h>

#include <ytlib/hydra/rpc_helpers.h>

namespace NYT {
namespace NApi {

using namespace NConcurrency;
using namespace NYTree;
using namespace NYPath;
using namespace NCypressClient;
using namespace NObjectClient;
using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NTransactionClient;
using namespace NFileClient;

////////////////////////////////////////////////////////////////////////////////

class TFileWriter
    : public TTransactionListener
    , public IFileWriter
{
public:
    TFileWriter(
        IClientPtr client,
        const TYPath& path,
        const TFileWriterOptions& options,
        TFileWriterConfigPtr config)
        : Client_(client)
        , Path_(path)
        , Options_(options)
        , Config_(config ? config : New<TFileWriterConfig>())
        , Logger(ApiLogger)
    {
        if (Options_.TransactionId != NullTransactionId) {
            auto transactionManager = Client_->GetTransactionManager();
            TTransactionAttachOptions attachOptions(Options_.TransactionId);
            attachOptions.AutoAbort = false;
            Transaction_ = transactionManager->Attach(attachOptions);
            ListenTransaction(Transaction_);
        }

        Logger.AddTag(Sprintf("Path: %s, TransactionId: %s",
            ~Path_,
            ~ToString(Options_.TransactionId)));
    }

    virtual TAsyncError Open() override
    {
        return BIND(&TFileWriter::DoOpen, MakeStrong(this))
            .Guarded()
            .AsyncVia(NChunkClient::TDispatcher::Get()->GetWriterInvoker())
            .Run();
    }

    virtual TAsyncError Write(const TRef& data) override
    {
        return BIND(&TFileWriter::DoWrite, MakeStrong(this))
            .Guarded()
            .AsyncVia(NChunkClient::TDispatcher::Get()->GetWriterInvoker())
            .Run(data);
    }

    virtual TAsyncError Close() override
    {
        return BIND(&TFileWriter::DoClose, MakeStrong(this))
            .Guarded()
            .AsyncVia(NChunkClient::TDispatcher::Get()->GetWriterInvoker())
            .Run();
    }

private:
    IClientPtr Client_;
    TYPath Path_;
    TFileWriterOptions Options_;
    TFileWriterConfigPtr Config_;

    TTransactionPtr Transaction_;
    TTransactionPtr UploadTransaction_;

    typedef TOldMultiChunkSequentialWriter<TFileChunkWriterProvider> TWriter;
    TIntrusivePtr<TWriter> Writer_;
    
    NLog::TTaggedLogger Logger;


    void DoOpen()
    {
        CheckAborted();

        LOG_INFO("Creating upload transaction");

        {
            NTransactionClient::TTransactionStartOptions options;
            options.ParentId = Transaction_ ? Transaction_->GetId() : NullTransactionId;
            options.EnableUncommittedAccounting = false;
            auto attributes = CreateEphemeralAttributes();
            attributes->Set("title", Sprintf("File upload to %s", ~Path_));
            options.Attributes = attributes.get();

            auto transactionManager = Client_->GetTransactionManager();
            auto transactionOrError = WaitFor(transactionManager->Start(
                ETransactionType::Master,
                options));
            THROW_ERROR_EXCEPTION_IF_FAILED(transactionOrError, "Error creating upload transaction");
            UploadTransaction_ = transactionOrError.Value();
        }

        LOG_INFO("Upload transaction created (TransactionId: %s)",
            ~ToString(UploadTransaction_->GetId()));

        ListenTransaction(UploadTransaction_);

        LOG_INFO("Requesting file info");

        TObjectServiceProxy proxy(Client_->GetMasterChannel());
        auto batchReq = proxy.ExecuteBatch();

        {
            auto req = TCypressYPathProxy::Get(Path_);
            SetTransactionId(req, UploadTransaction_);
            TAttributeFilter attributeFilter(EAttributeFilterMode::MatchingOnly);
            attributeFilter.Keys.push_back("type");
            attributeFilter.Keys.push_back("replication_factor");
            attributeFilter.Keys.push_back("account");
            attributeFilter.Keys.push_back("compression_codec");
            attributeFilter.Keys.push_back("erasure_codec");
            ToProto(req->mutable_attribute_filter(), attributeFilter);
            batchReq->AddRequest(req, "get_attributes");
        }

        {
            auto req = TFileYPathProxy::PrepareForUpdate(Path_);
            req->set_mode(Options_.Append ? EUpdateMode::Append : EUpdateMode::Overwrite);
            NHydra::GenerateMutationId(req);
            SetTransactionId(req, UploadTransaction_);
            batchReq->AddRequest(req, "prepare_for_update");
        }

        auto batchRsp = WaitFor(batchReq->Invoke());
        THROW_ERROR_EXCEPTION_IF_FAILED(*batchRsp, "Error requesting file info");

        auto writerOptions = New<TMultiChunkWriterOptions>();
        {
            auto rsp = batchRsp->GetResponse<TYPathProxy::TRspGet>("get_attributes");
            THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error getting file attributes");

            auto node = ConvertToNode(TYsonString(rsp->value()));
            const auto& attributes = node->Attributes();

            auto type = attributes.Get<EObjectType>("type");
            if (type != EObjectType::File) {
                THROW_ERROR_EXCEPTION("Invalid type of %s: expected %s, actual %s",
                    ~Path_,
                    ~FormatEnum(EObjectType(EObjectType::File)).Quote(),
                    ~FormatEnum(type).Quote());
            }

            writerOptions->ReplicationFactor = attributes.Get<int>("replication_factor");
            writerOptions->Account = attributes.Get<Stroka>("account");
            writerOptions->CompressionCodec = attributes.Get<NCompression::ECodec>("compression_codec");
            writerOptions->ErasureCodec = attributes.Get<NErasure::ECodec>("erasure_codec", NErasure::ECodec::None);
        }

        TChunkListId chunkListId;
        {
            auto rsp = batchRsp->GetResponse<TFileYPathProxy::TRspPrepareForUpdate>("prepare_for_update");
            THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error preparing file for update");
            chunkListId = FromProto<TChunkListId>(rsp->chunk_list_id());
        }

        LOG_INFO("File info received (Account: %s, ChunkListId: %s)",
            ~writerOptions->Account,
            ~ToString(chunkListId));

        auto provider = New<TFileChunkWriterProvider>(
            Config_,
            writerOptions);

        Writer_ = New<TWriter>(
            Config_,
            writerOptions,
            provider,
            Client_->GetMasterChannel(),
            UploadTransaction_->GetId(),
            chunkListId);

        {
            auto result = WaitFor(Writer_->Open());
            THROW_ERROR_EXCEPTION_IF_FAILED(result);
        }
    }

    void DoWrite(const TRef& data)
    {
        CheckAborted();

        while (!Writer_->GetCurrentWriter()) {
            auto result = WaitFor(Writer_->GetReadyEvent());
            THROW_ERROR_EXCEPTION_IF_FAILED(result);
        }
        
        Writer_->GetCurrentWriter()->Write(data);
    }

    void DoClose()
    {
        CheckAborted();

        LOG_INFO("Closing file writer and committing upload transaction");

        {
            auto result = WaitFor(Writer_->Close());
            THROW_ERROR_EXCEPTION_IF_FAILED(result, "Failed to close file writer");
        }

        {
            auto result = WaitFor(UploadTransaction_->Commit(NHydra::NullMutationId));
            THROW_ERROR_EXCEPTION_IF_FAILED(result, "Failed to commit upload transaction");
        }
    }

};

IFileWriterPtr CreateFileWriter(
    IClientPtr client,
    const TYPath& path,
    const TFileWriterOptions& options,
    TFileWriterConfigPtr config)
{
    return New<TFileWriter>(
        client,
        path,
        options,
        config);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NApi
} // namespace NYT
