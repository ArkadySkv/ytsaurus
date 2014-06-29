#include "stdafx.h"

#include "multi_chunk_sequential_writer_base.h"

#include "writer.h"
#include "chunk_list_ypath_proxy.h"
#include "chunk_replica.h"
#include "chunk_writer_base.h"
#include "chunk_ypath_proxy.h"
#include "config.h"
#include "dispatcher.h"
#include "erasure_writer.h"
#include "private.h"
#include "replication_writer.h"

#include <ytlib/hydra/rpc_helpers.h>

#include <ytlib/node_tracker_client/node_directory.h>

#include <ytlib/object_client/helpers.h>
#include <ytlib/object_client/master_ypath_proxy.h>
#include <ytlib/object_client/object_service_proxy.h>

#include <core/concurrency/scheduler.h>
#include <core/concurrency/parallel_awaiter.h>

#include <core/erasure/codec.h>

#include <core/misc/address.h>

#include <core/rpc/channel.h>

namespace NYT {
namespace NChunkClient {

using namespace NChunkClient::NProto;
using namespace NConcurrency;
using namespace NErasure;
using namespace NHydra;
using namespace NNodeTrackerClient;
using namespace NObjectClient;
using namespace NRpc;
using namespace NTransactionClient;

////////////////////////////////////////////////////////////////////////////////

TMultiChunkSequentialWriterBase::TMultiChunkSequentialWriterBase(
    TMultiChunkWriterConfigPtr config,
    TMultiChunkWriterOptionsPtr options,
    IChannelPtr masterChannel,
    const TTransactionId& transactionId,
    const TChunkListId& parentChunkListId)
    : Config_(config)
    , Options_(options)
    , MasterChannel_(masterChannel)
    , TransactionId_(transactionId)
    , ParentChunkListId_(parentChunkListId)
    , NodeDirectory_(New<TNodeDirectory>())
    , UploadReplicationFactor_(std::min(Options_->ReplicationFactor, Config_->UploadReplicationFactor))
    , Progress_(0)
    , Closing_(false)
    , ReadyEvent_(MakeFuture(TError()))
    , CompletionError_(NewPromise<TError>())
    , CloseChunksAwaiter_(New<TParallelAwaiter>(TDispatcher::Get()->GetWriterInvoker()))
    , Logger(ChunkWriterLogger)
{
    YCHECK(Config_);
    YCHECK(MasterChannel_);

    Logger.AddTag(Sprintf("TransactionId: %s", ~ToString(TransactionId_)));
}

TAsyncError TMultiChunkSequentialWriterBase::Open()
{
    ReadyEvent_= BIND(&TMultiChunkSequentialWriterBase::DoOpen, MakeStrong(this))
        .AsyncVia(TDispatcher::Get()->GetWriterInvoker())
        .Run();

    return ReadyEvent_;
}

TAsyncError TMultiChunkSequentialWriterBase::Close()
{
    YCHECK(!Closing_);
    Closing_ = true;

    if (CompletionError_.IsSet()) {
        return CompletionError_.ToFuture();
    }

    FinishSession(CurrentSession_);
    CurrentSession_.Reset();

    BIND(&TMultiChunkSequentialWriterBase::DoClose, MakeWeak(this))
        .AsyncVia(TDispatcher::Get()->GetWriterInvoker())
        .Run();

    ReadyEvent_ = CompletionError_.ToFuture();
    return ReadyEvent_;
}

TAsyncError TMultiChunkSequentialWriterBase::GetReadyEvent()
{
    if (CurrentSession_.IsActive()) {
        return CurrentSession_.FrontalWriter->GetReadyEvent();
    } else {
        return ReadyEvent_;
    }
}

void TMultiChunkSequentialWriterBase::SetProgress(double progress)
{
    Progress_ = progress;
}

const std::vector<TChunkSpec>& TMultiChunkSequentialWriterBase::GetWrittenChunks() const
{
    return WrittenChunks_;
}

TNodeDirectoryPtr TMultiChunkSequentialWriterBase::GetNodeDirectory() const
{
    return NodeDirectory_;
}

TDataStatistics TMultiChunkSequentialWriterBase::GetDataStatistics() const
{
    auto writer = CurrentSession_.FrontalWriter;
    if (writer) {
        return DataStatistics_ + writer->GetDataStatistics();
    } else {
        return DataStatistics_;
    }
}

TError TMultiChunkSequentialWriterBase::DoOpen()
{
    CreateNextSession();
    NextSessionReady_ = MakeFuture();
    return InitCurrentSession();
}

void TMultiChunkSequentialWriterBase::CreateNextSession()
{
    LOG_DEBUG("Creating chunk (ReplicationFactor: %d, UploadReplicationFactor: %d)",
        Options_->ReplicationFactor,
        UploadReplicationFactor_);

    TObjectServiceProxy objectProxy(MasterChannel_);

    auto req = TMasterYPathProxy::CreateObjects();
    ToProto(req->mutable_transaction_id(), TransactionId_);

    req->set_type(Options_->ErasureCodec == ECodec::None
        ? EObjectType::Chunk
        : EObjectType::ErasureChunk);

    req->set_account(Options_->Account);
    GenerateMutationId(req);

    auto* reqExt = req->MutableExtension(NProto::TReqCreateChunkExt::create_chunk_ext);
    if (Config_->PreferLocalHost) {
        reqExt->set_preferred_host_name(TAddressResolver::Get()->GetLocalHostName());
    }
    reqExt->set_replication_factor(Options_->ReplicationFactor);
    reqExt->set_upload_replication_factor(UploadReplicationFactor_);
    reqExt->set_movable(Config_->ChunksMovable);
    reqExt->set_vital(Options_->ChunksVital);
    reqExt->set_erasure_codec(Options_->ErasureCodec);

    auto rsp = WaitFor(objectProxy.Execute(req));

    if (!rsp->IsOK()) {
        CompletionError_.TrySet(TError(EErrorCode::MasterCommunicationFailed, "Error creating chunk") << *rsp);
        return;
    }

    NextSession_.ChunkId = NYT::FromProto<TChunkId>(rsp->object_ids(0));
    const auto& rspExt = rsp->GetExtension(NProto::TRspCreateChunkExt::create_chunk_ext);

    NodeDirectory_->MergeFrom(rspExt.node_directory());

    NextSession_.Replicas = NYT::FromProto<TChunkReplica>(rspExt.replicas());
    if (NextSession_.Replicas.size() < UploadReplicationFactor_) {
        CompletionError_.TrySet(TError(
            "Not enough data nodes available: %d received, %d needed",
            static_cast<int>(NextSession_.Replicas.size()),
            UploadReplicationFactor_));
        return;
    }

    LOG_DEBUG("Chunk created (ChunkId: %s)", ~ToString(NextSession_.ChunkId));

    if (Options_->ErasureCodec == ECodec::None) {
        auto targets = NodeDirectory_->GetDescriptors(NextSession_.Replicas);
        NextSession_.UnderlyingWriter = CreateReplicationWriter(Config_, NextSession_.ChunkId, targets);
    } else {
        auto* erasureCodec = GetCodec(Options_->ErasureCodec);
        int totalPartCount = erasureCodec->GetTotalPartCount();

        YCHECK(NextSession_.Replicas.size() == totalPartCount);

        std::vector<IWriterPtr> writers;
        for (int index = 0; index < totalPartCount; ++index) {
            auto partId = ErasurePartIdFromChunkId(NextSession_.ChunkId, index);
            auto target = NodeDirectory_->GetDescriptor(NextSession_.Replicas[index]);
            std::vector<TNodeDescriptor> targets(1, target);
            writers.push_back(CreateReplicationWriter(Config_, partId, targets));
        }

        NextSession_.UnderlyingWriter = CreateErasureWriter(Config_, erasureCodec, writers);
    }

    NextSession_.UnderlyingWriter->Open();
}

void TMultiChunkSequentialWriterBase::SwitchSession()
{
    ReadyEvent_ = BIND(
        &TMultiChunkSequentialWriterBase::DoSwitchSession,
        MakeStrong(this),
        CurrentSession_)
    .AsyncVia(TDispatcher::Get()->GetWriterInvoker())
    .Run();

    CurrentSession_.Reset();
}

TError TMultiChunkSequentialWriterBase::DoSwitchSession(const TSession& session)
{
    if (Config_->SyncChunkSwitch) {
        // Wait until session is finished.
        WaitFor(FinishSession(session));
    } else {
        // Do not wait, fire and move on.
        FinishSession(session);
    }

    return InitCurrentSession();
}

TFuture<void> TMultiChunkSequentialWriterBase::FinishSession(const TSession& session)
{
    auto sessionFinishedEvent = BIND(
        &TMultiChunkSequentialWriterBase::DoFinishSession,
        MakeWeak(this), 
        session)
    .AsyncVia(TDispatcher::Get()->GetWriterInvoker())
    .Run();

    CloseChunksAwaiter_->Await(sessionFinishedEvent);

    return sessionFinishedEvent;
}

void TMultiChunkSequentialWriterBase::DoFinishSession(const TSession& session)
{
    if (session.FrontalWriter->GetDataSize() == 0) {
        LOG_DEBUG("Canceling empty chunk (ChunkId: %s)", ~ToString(session.ChunkId));
        return;
    }

    // Reserve next sequential slot in WrittenChunks_.
    WrittenChunks_.push_back(TChunkSpec());
    auto& chunkSpec = WrittenChunks_.back();

    LOG_DEBUG("Finishing chunk (ChunkId: %s)", ~ToString(session.ChunkId));

    auto error = WaitFor(session.FrontalWriter->Close());

    if (!error.IsOK()) {
        CompletionError_.TrySet(TError("Failed to close chunk (ChunkId: %s)", ~ToString(session.ChunkId)) << error);
        return;
    }

    LOG_DEBUG("Chunk closed (ChunkId: %s)", ~ToString(session.ChunkId));

    std::vector<TChunkReplica> replicas;
    for (int index : session.UnderlyingWriter->GetWrittenIndexes()) {
        replicas.push_back(session.Replicas[index]);
    }

    *chunkSpec.mutable_chunk_meta() = session.FrontalWriter->GetSchedulerMeta();
    ToProto(chunkSpec.mutable_chunk_id(), session.ChunkId);
    NYT::ToProto(chunkSpec.mutable_replicas(), replicas);

    DataStatistics_ += session.FrontalWriter->GetDataStatistics();

    auto req = TChunkYPathProxy::Confirm(FromObjectId(session.ChunkId));
    GenerateMutationId(req);
    *req->mutable_chunk_info() = session.UnderlyingWriter->GetChunkInfo();
    *req->mutable_chunk_meta() = session.FrontalWriter->GetMasterMeta();
    NYT::ToProto(req->mutable_replicas(), replicas);

    TObjectServiceProxy objectProxy(MasterChannel_);
    auto rsp = WaitFor(objectProxy.Execute(req));

    if (!rsp->IsOK()) {
        CompletionError_.TrySet(TError("Failed to confirm chunk (ChunkId: %s)", ~ToString(session.ChunkId)) << *rsp);
        return;
    }

    LOG_DEBUG("Chunk confirmed (ChunkId: %s)", ~ToString(session.ChunkId));
}

TError TMultiChunkSequentialWriterBase::InitCurrentSession()
{
    WaitFor(NextSessionReady_);

    if (CompletionError_.IsSet()) {
        return CompletionError_.Get();
    }

    CurrentSession_ = NextSession_;
    NextSession_.Reset();

    CurrentSession_.FrontalWriter = CreateFrontalWriter(CurrentSession_.UnderlyingWriter);

    NextSessionReady_ = BIND(&TMultiChunkSequentialWriterBase::CreateNextSession, MakeWeak(this))
        .AsyncVia(TDispatcher::Get()->GetWriterInvoker())
        .Run();

    return TError();
}

bool TMultiChunkSequentialWriterBase::VerifyActive()
{
    YCHECK(!Closing_);
    YCHECK(CurrentSession_.IsActive());

    if (CompletionError_.IsSet()) {
        ReadyEvent_ = CompletionError_.ToFuture();
        return false;
    }

    return true;
}

bool TMultiChunkSequentialWriterBase::TrySwitchSession()
{
    if (CurrentSession_.FrontalWriter->GetMetaSize() > Config_->MaxMetaSize) {
        LOG_DEBUG("Switching to next chunk: meta is too large (ChunkMetaSize: %" PRId64 ")",
            CurrentSession_.FrontalWriter->GetMetaSize());

        SwitchSession();
        return true;
    } 

    if (CurrentSession_.FrontalWriter->GetDataSize() > Config_->DesiredChunkSize) {
        i64 currentDataSize = DataStatistics_.compressed_data_size() + CurrentSession_.FrontalWriter->GetDataSize();
        i64 expectedInputSize = static_cast<i64>(currentDataSize * std::max(0.0, 1.0 - Progress_));

        if (expectedInputSize > Config_->DesiredChunkSize ||
            CurrentSession_.FrontalWriter->GetDataSize() > 2 * Config_->DesiredChunkSize)
        {
            LOG_DEBUG("Switching to next chunk: data is too large (CurrentSessionSize: %" PRId64 ", ExpectedInputSize: %" PRId64 ", DesiredChunkSize: %" PRId64 ")",
                CurrentSession_.FrontalWriter->GetDataSize(),
                expectedInputSize,
                Config_->DesiredChunkSize);

            SwitchSession();
            return true;
        }
    }

    return false;
}

void TMultiChunkSequentialWriterBase::DoClose()
{
    WaitFor(CloseChunksAwaiter_->Complete());

    if (CompletionError_.IsSet()) {
        return;
    }

    if (ParentChunkListId_ == NullChunkListId) {
        LOG_DEBUG("Chunk sequence writer closed, no chunks attached");
        CompletionError_.TrySet(TError());
        return;
    }


    LOG_DEBUG("Attaching %d chunks", static_cast<int>(WrittenChunks_.size()));

    auto req = TChunkListYPathProxy::Attach(FromObjectId(ParentChunkListId_));
    GenerateMutationId(req);
    for (const auto& chunkSpec : WrittenChunks_) {
        *req->add_children_ids() = chunkSpec.chunk_id();
    }

    TObjectServiceProxy objectProxy(MasterChannel_);
    auto rsp = WaitFor(objectProxy.Execute(req));

    if (!rsp->IsOK()) {
        CompletionError_.TrySet(TError(
            EErrorCode::MasterCommunicationFailed, 
            "Error attaching chunks to chunk list %s",
            ~ToString(ParentChunkListId_)) << *rsp);
        return;
    }

    LOG_DEBUG("Chunks attached, chunk sequence writer closed");
    CompletionError_.TrySet(TError());
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
