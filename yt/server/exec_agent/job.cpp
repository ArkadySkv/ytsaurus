#include "stdafx.h"
#include "job.h"
#include "environment_manager.h"
#include "slot.h"
#include "environment.h"
#include "private.h"
#include "slot_manager.h"
#include "config.h"

#include <ytlib/misc/fs.h>
#include <ytlib/misc/assert.h>

#include <ytlib/ytree/serialize.h>

#include <ytlib/transaction_client/transaction.h>

#include <ytlib/file_client/file_ypath_proxy.h>

#include <ytlib/table_client/table_producer.h>
#include <ytlib/table_client/table_chunk_reader.h>
#include <ytlib/table_client/sync_reader.h>
#include <ytlib/table_client/config.h>

#include <ytlib/file_client/config.h>
#include <ytlib/file_client/file_reader.h>
#include <ytlib/file_client/file_chunk_reader.h>

#include <ytlib/chunk_client/multi_chunk_sequential_reader.h>
#include <ytlib/chunk_client/client_block_cache.h>

#include <ytlib/node_tracker_client/node_directory.h>
#include <ytlib/node_tracker_client/helpers.h>

#include <ytlib/security_client/public.h>

#include <server/chunk_holder/chunk.h>
#include <server/chunk_holder/location.h>
#include <server/chunk_holder/chunk_cache.h>
#include <server/chunk_holder/block_store.h>

#include <server/job_proxy/config.h>

#include <server/job_agent/job.h>

#include <server/scheduler/config.h>
#include <server/scheduler/job_resources.h>

#include <server/cell_node/bootstrap.h>
#include <server/cell_node/config.h>

#include <server/chunk_holder/config.h>

namespace NYT {
namespace NExecAgent {

using namespace NRpc;
using namespace NJobProxy;
using namespace NYTree;
using namespace NYson;
using namespace NChunkClient;
using namespace NTableClient;
using namespace NTableClient::NProto;
using namespace NCellNode;
using namespace NChunkHolder;
using namespace NCellNode;
using namespace NNodeTrackerClient;
using namespace NNodeTrackerClient::NProto;
using namespace NJobTrackerClient;
using namespace NJobTrackerClient::NProto;
using namespace NScheduler;
using namespace NScheduler::NProto;

using NNodeTrackerClient::TNodeDirectory;
using NScheduler::NProto::TUserJobSpec;

////////////////////////////////////////////////////////////////////////////////

class TJob
    : public NJobAgent::IJob
{
    DEFINE_SIGNAL(void(), ResourcesReleased);

public:
    TJob(
        const TJobId& jobId,
        const TNodeResources& resourceLimits,
        TJobSpec&& jobSpec,
        TBootstrap* bootstrap)
        : JobId(jobId)
        , ResourceLimits(resourceLimits)
        , Bootstrap(bootstrap)
        , ResourceUsage(resourceLimits)
        , Logger(ExecAgentLogger)
        , JobState(EJobState::Waiting)
        , JobPhase(EJobPhase::Created)
        , FinalJobState(EJobState::Completed)
        , Progress_(0.0)
        , NodeDirectory(New<TNodeDirectory>())
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        JobSpec.Swap(&jobSpec);

        UserJobSpec = nullptr;
        if (JobSpec.HasExtension(TMapJobSpecExt::map_job_spec_ext)) {
            const auto& jobSpecExt = JobSpec.GetExtension(TMapJobSpecExt::map_job_spec_ext);
            UserJobSpec = &jobSpecExt.mapper_spec();
        } else if (JobSpec.HasExtension(TReduceJobSpecExt::reduce_job_spec_ext)) {
            const auto& jobSpecExt = JobSpec.GetExtension(TReduceJobSpecExt::reduce_job_spec_ext);
            UserJobSpec = &jobSpecExt.reducer_spec();
        } else if (JobSpec.HasExtension(TPartitionJobSpecExt::partition_job_spec_ext)) {
            const auto& jobSpecExt = JobSpec.GetExtension(TPartitionJobSpecExt::partition_job_spec_ext);
            if (jobSpecExt.has_mapper_spec()) {
                UserJobSpec = &jobSpecExt.mapper_spec();
            }
        }

        if (UserJobSpec) {
            // Adjust memory usage according to memory_reserve.
            auto memoryUsage = ResourceUsage.memory();
            memoryUsage -= UserJobSpec->memory_limit();
            memoryUsage += UserJobSpec->memory_reserve();
            ResourceUsage.set_memory(memoryUsage);
        }

        NodeDirectory->AddDescriptor(InvalidNodeId, Bootstrap->GetLocalDescriptor());

        Logger.AddTag(Sprintf("JobId: %s", ~ToString(jobId)));
    }

    virtual void Start() override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);
        YCHECK(!Slot);

        if (JobState != EJobState::Waiting)
            return;
        JobState = EJobState::Running;

        auto slotManager = Bootstrap->GetSlotManager();
        Slot = slotManager->AcquireSlot();

        VERIFY_INVOKER_AFFINITY(Slot->GetInvoker(), JobThread);

        Slot->GetInvoker()->Invoke(BIND(
            &TJob::DoStart,
            MakeWeak(this)));
    }

    virtual void Abort(const TError& error) override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (JobState == EJobState::Waiting) {
            YCHECK(!Slot);
            SetResult(TError("Job aborted by scheduler"));
            JobState = EJobState::Aborted;
            JobPhase = EJobPhase::Finished;
            SetResourceUsage(ZeroNodeResources());
            ResourcesReleased_.Fire();
        } else {
            Slot->GetInvoker()->Invoke(BIND(
                &TJob::DoAbort,
                MakeStrong(this),
                error,
                EJobState::Aborted));
        }
    }

    virtual const TJobId& GetId() const override
    {
        return JobId;
    }

    virtual const TJobSpec& GetSpec() const override
    {
        return JobSpec;
    }

    virtual EJobState GetState() const override
    {
        TGuard<TSpinLock> guard(ResultLock);
        return JobState;
    }

    virtual EJobPhase GetPhase() const override
    {
        return JobPhase;
    }

    virtual TNodeResources GetResourceUsage() const override
    {
        TGuard<TSpinLock> guard(ResourcesLock);
        return ResourceUsage;
    }

    virtual void SetResourceUsage(const TNodeResources& newUsage) override
    {
        TGuard<TSpinLock> guard(ResourcesLock);
        ResourceUsage = newUsage;
    }

    virtual TJobResult GetResult() const override
    {
        TGuard<TSpinLock> guard(ResultLock);
        return JobResult.Get();
    }

    virtual void SetResult(const TJobResult& jobResult) override
    {
        TGuard<TSpinLock> guard(ResultLock);

        if (JobState == EJobState::Completed ||
            JobState == EJobState::Aborted ||
            JobState == EJobState::Failed)
        {
            return;
        }

        if (JobResult.HasValue() && JobResult->error().code() != TError::OK) {
            return;
        }

        JobResult = jobResult;

        auto resultError = FromProto(jobResult.error());
        if (resultError.IsOK()) {
            return;
        } else if (IsFatalError(resultError)) {
            resultError.Attributes().Set("fatal", true);
            ToProto(JobResult->mutable_error(), resultError);
            FinalJobState = EJobState::Failed;
        } else if (IsRetriableSystemError(resultError)) {
            FinalJobState = EJobState::Aborted;
        } else {
            FinalJobState = EJobState::Failed;
        }
    }

    virtual double GetProgress() const override
    {
        return Progress_;
    }

    virtual void SetProgress(double value) override
    {
        TGuard<TSpinLock> guard(ResultLock);
        if (JobState == EJobState::Running) {
            Progress_ = value;
        }
    }

private:
    TJobId JobId;
    TJobSpec JobSpec;
    const TUserJobSpec* UserJobSpec;
    TNodeResources ResourceLimits;
    NCellNode::TBootstrap* Bootstrap;

    TSpinLock ResourcesLock;
    TNodeResources ResourceUsage;

    NLog::TTaggedLogger Logger;

    TSlotPtr Slot;

    EJobState JobState;
    EJobPhase JobPhase;

    EJobState FinalJobState;

    double Progress_;

    std::vector<NChunkHolder::TCachedChunkPtr> CachedChunks;

    // Special node directory used to read cached chunks.
    TNodeDirectoryPtr NodeDirectory;

    IProxyControllerPtr ProxyController;

    // Protects #JobResult and #JobState.
    TSpinLock ResultLock;
    TNullable<TJobResult> JobResult;

    NJobProxy::TJobProxyConfigPtr ProxyConfig;


    DECLARE_THREAD_AFFINITY_SLOT(ControlThread);
    DECLARE_THREAD_AFFINITY_SLOT(JobThread);


    void DoStart()
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        if (JobPhase > EJobPhase::Cleanup)
            return;
        YCHECK(JobPhase == EJobPhase::Created);
        JobPhase = EJobPhase::PreparingConfig;

        {
            INodePtr ioConfigNode;
            try {
                auto* schedulerJobSpecExt = JobSpec.MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
                ioConfigNode = ConvertToNode(TYsonString(schedulerJobSpecExt->io_config()));
            } catch (const std::exception& ex) {
                auto wrappedError = TError("Error deserializing job IO configuration")
                    << ex;
                DoAbort(wrappedError, EJobState::Failed);
                return;
            }

            auto ioConfig = New<TJobIOConfig>();
            try {
                ioConfig->Load(ioConfigNode);
            } catch (const std::exception& ex) {
                auto error = TError("Error validating job IO configuration")
                    << ex;
                DoAbort(error, EJobState::Failed);
                return;
            }

            auto proxyConfig = CloneYsonSerializable(Bootstrap->GetJobProxyConfig());
            proxyConfig->JobIO = ioConfig;
            proxyConfig->UserId = Slot->GetUserId();

            auto proxyConfigPath = NFS::CombinePaths(
                Slot->GetWorkingDirectory(),
                ProxyConfigFileName);

            TFile file(proxyConfigPath, CreateAlways | WrOnly | Seq | CloseOnExec);
            TFileOutput output(file);
            TYsonWriter writer(&output, EYsonFormat::Pretty);
            proxyConfig->Save(&writer);
        }

        JobPhase = EJobPhase::PreparingProxy;

        Stroka environmentType = "default";
        try {
            auto environmentManager = Bootstrap->GetEnvironmentManager();
            ProxyController = environmentManager->CreateProxyController(
                //XXX(psushin): execution environment type must not be directly
                // selectable by user -- it is more of the global cluster setting
                //jobSpec.operation_spec().environment(),
                environmentType,
                JobId,
                Slot->GetWorkingDirectory());
        } catch (const std::exception& ex) {
            auto wrappedError = TError(
                "Failed to create proxy controller for environment %s",
                ~environmentType.Quote())
                << ex;
            DoAbort(wrappedError, EJobState::Failed);
            return;
        }

        JobPhase = EJobPhase::PreparingSandbox;
        Slot->InitSandbox();

        PrepareUserJob().Subscribe(
            BIND(&TJob::RunJobProxy, MakeStrong(this))
            .Via(Slot->GetInvoker()));
    }

    void DoAbort(const TError& error, EJobState resultState)
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        if (JobPhase > EJobPhase::Cleanup) {
            JobState = resultState;
            return;
        }

        JobState = EJobState::Aborting;

        YCHECK(JobPhase < EJobPhase::Cleanup);

        const auto jobPhase = JobPhase;
        JobPhase = EJobPhase::Cleanup;

        if (resultState == EJobState::Failed) {
            LOG_ERROR(error, "Job failed, aborting");
        } else {
            LOG_INFO(error, "Aborting job");
        }

        if (jobPhase >= EJobPhase::Running) {
            // NB: Kill() never throws.
            ProxyController->Kill(Slot->GetUserId(), error);
        }

        if (jobPhase >= EJobPhase::PreparingSandbox) {
            LOG_INFO("Cleaning slot");
            Slot->Clean();
        }

        SetResult(error);
        JobPhase = EJobPhase::Finished;
        JobState = resultState;

        LOG_INFO("Job aborted");

        FinalizeJob();
    }


    TFuture<void> PrepareUserJob()
    {
        if (!UserJobSpec) {
            return MakeFuture();
        }

        auto awaiter = New<TParallelAwaiter>(Slot->GetInvoker());

        FOREACH (const auto& descriptor, UserJobSpec->regular_files()) {
            awaiter->Await(DownloadRegularFile(descriptor));
        }

        FOREACH (const auto& descriptor, UserJobSpec->table_files()) {
            awaiter->Await(DownloadTableFile(descriptor));
        }

        return awaiter->Complete();
    }


    void RunJobProxy()
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        if (JobPhase > EJobPhase::Cleanup)
            return;

        YCHECK(JobPhase == EJobPhase::PreparingSandbox);

        try {
            JobPhase = EJobPhase::Running;
            ProxyController->Run();
        } catch (const std::exception& ex) {
            DoAbort(ex, EJobState::Failed);
            return;
        }

        ProxyController->SubscribeExited(BIND(
            &TJob::OnProxyFinished,
            MakeWeak(this)).Via(Slot->GetInvoker()));
    }

    void OnProxyFinished(TError exitError)
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        if (JobPhase > EJobPhase::Cleanup)
            return;

        YCHECK(JobPhase < EJobPhase::Cleanup);

        if (!exitError.IsOK()) {
            DoAbort(exitError, EJobState::Failed);
            return;
        }

        if (!IsResultSet()) {
            DoAbort(
                TError("Job proxy exited successfully but job result has not been set"),
                EJobState::Failed);
            return;
        }

        // NB: we should explicitly call Kill() to clean up possible child processes.
        ProxyController->Kill(Slot->GetUserId(), TError());

        JobPhase = EJobPhase::Cleanup;
        Slot->Clean();

        JobPhase = EJobPhase::Finished;

        {
            TGuard<TSpinLock> guard(ResultLock);
            JobState = FinalJobState;
        }

        FinalizeJob();
    }


    void FinalizeJob()
    {
        Slot->Release();
        SetResourceUsage(ZeroNodeResources());
        ResourcesReleased_.Fire();
    }


    void SetResult(const TError& error)
    {
        TJobResult jobResult;
        ToProto(jobResult.mutable_error(), error);
        SetResult(jobResult);
    }

    bool IsResultSet() const
    {
        TGuard<TSpinLock> guard(ResultLock);
        return JobResult.HasValue();
    }

    TFuture<void> DownloadChunks(const NChunkClient::NProto::TRspFetch& fetchRsp)
    {
        auto awaiter = New<TParallelAwaiter>(Slot->GetInvoker());
        auto chunkCache = Bootstrap->GetChunkCache();
        auto this_ = MakeStrong(this);

        FOREACH (const auto chunk, fetchRsp.chunks()) {
            auto chunkId = FromProto<TChunkId>(chunk.chunk_id());

            if (IsErasureChunkId(chunkId)) {
                DoAbort(
                    TError("Cannot download erasure chunk (ChunkId: %s)", ~ToString(chunkId)),
                    EJobState::Failed);
                break;
            }

            awaiter->Await(
                chunkCache->DownloadChunk(chunkId),
                BIND([=](NChunkHolder::TChunkCache::TDownloadResult result) {
                    if (!result.IsOK()) {
                        auto wrappedError = TError(
                            "Failed to download chunk (ChunkId: %s)",
                            ~ToString(chunkId))
                            << result;
                        this_->DoAbort(wrappedError, EJobState::Failed);
                        return;
                    }
                    this_->CachedChunks.push_back(result.Value());
                }));
        }

        return awaiter->Complete();
    }

    std::vector<NChunkClient::NProto::TChunkSpec>
    PatchCachedChunkReplicas(const NChunkClient::NProto::TRspFetch& fetchRsp)
    {
        std::vector<NChunkClient::NProto::TChunkSpec> chunks;
        chunks.insert(chunks.end(), fetchRsp.chunks().begin(), fetchRsp.chunks().end());
        FOREACH (auto& chunk, chunks) {
            chunk.clear_replicas();
            chunk.add_replicas(ToProto<ui32>(TChunkReplica(InvalidNodeId, 0)));
        }
        return chunks;
    }

    TFuture<void> DownloadRegularFile(const TRegularFileDescriptor& descriptor)
    {
        if (descriptor.file().chunks_size() == 1) {
            const auto& chunk = descriptor.file().chunks(0);
            auto miscExt = GetProtoExtension<NChunkClient::NProto::TMiscExt>(chunk.extensions());
            auto compressionCodecId = NCompression::ECodec(miscExt.compression_codec());
            auto chunkId = FromProto<TChunkId>(chunk.chunk_id());
            if (!IsErasureChunkId(chunkId) && (compressionCodecId == NCompression::ECodec::None)) {
                LOG_INFO("Downloading symlinked user file (FileName: %s, ChunkId: %s)",
                    ~descriptor.file_name(),
                    ~ToString(chunkId));

                auto awaiter = New<TParallelAwaiter>(Slot->GetInvoker());
                auto chunkCache = Bootstrap->GetChunkCache();
                awaiter->Await(
                    chunkCache->DownloadChunk(chunkId),
                    BIND(&TJob::OnSymlinkChunkDownloaded, MakeWeak(this), descriptor));

                return awaiter->Complete();
            }
        }

        LOG_INFO("Downloading regular user file (FileName: %s, ChunkCount: %d)",
            ~descriptor.file_name(),
            static_cast<int>(descriptor.file().chunks_size()));

        return DownloadChunks(descriptor.file()).Apply(BIND(
            &TJob::OnFileChunksDownloaded,
            MakeWeak(this),
            descriptor).Via(Slot->GetInvoker()));
    }

    void OnSymlinkChunkDownloaded(
        const TRegularFileDescriptor& descriptor,
        TChunkCache::TDownloadResult result)
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        if (JobPhase > EJobPhase::Cleanup)
            return;
        YCHECK(JobPhase == EJobPhase::PreparingSandbox);

        auto fileName = descriptor.file_name();

        if (!result.IsOK()) {
            auto wrappedError = TError(
                "Failed to download user file %s",
                ~fileName.Quote())
                << result;
            DoAbort(wrappedError, EJobState::Failed);
            return;
        }

        CachedChunks.push_back(result.Value());

        try {
            Slot->MakeLink(
                fileName,
                CachedChunks.back()->GetFileName(),
                descriptor.executable());
        } catch (const std::exception& ex) {
            auto wrappedError = TError(
                "Failed to create a symlink for %s",
                ~fileName.Quote())
                << ex;
            DoAbort(wrappedError, EJobState::Failed);
            return;
        }

        LOG_INFO("User file downloaded successfully (FileName: %s)",
            ~fileName);
    }

    void OnFileChunksDownloaded(const TRegularFileDescriptor& descriptor)
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        if (JobPhase > EJobPhase::Cleanup)
            return;
        YCHECK(JobPhase == EJobPhase::PreparingSandbox);

        auto chunks = PatchCachedChunkReplicas(descriptor.file());
        auto config = New<NFileClient::TFileReaderConfig>();

        auto provider = New<NFileClient::TFileChunkReaderProvider>(config);
        auto reader = New<NFileClient::TFileChunkSequenceReader>(
            config,
            Bootstrap->GetMasterChannel(),
            Bootstrap->GetBlockStore()->GetBlockCache(),
            NodeDirectory,
            std::move(chunks),
            provider);

        auto fileName = descriptor.file_name();

        try {
            Sync(~reader, &NFileClient::TFileChunkSequenceReader::AsyncOpen);
            auto producer = [&] (TOutputStream* output) {
                auto* facade = reader->GetFacade();
                while (facade) {
                    auto block = facade->GetBlock();
                    output->Write(block.Begin(),block.Size());

                    if (!reader->FetchNext()) {
                        Sync(~reader, &NFileClient::TFileChunkSequenceReader::GetReadyEvent);
                    }
                    facade = reader->GetFacade();
                }
            };

            Slot->MakeFile(fileName, producer);
        } catch (const std::exception& ex) {
            auto wrappedError = TError(
                "Failed to write regular user file (FileName: %s)",
                ~fileName)
                << ex;
            DoAbort(wrappedError, EJobState::Failed);
            return;
        }

        LOG_INFO("Regular user file downloaded successfully (FileName: %s)",
            ~fileName);
    }


    TFuture<void> DownloadTableFile(const TTableFileDescriptor& descriptor)
    {
        LOG_INFO("Downloading table user file (FileName: %s, ChunkCount: %d)",
            ~descriptor.file_name(),
            static_cast<int>(descriptor.table().chunks_size()));

        return DownloadChunks(descriptor.table()).Apply(BIND(
            &TJob::OnTableChunksDownloaded,
            MakeWeak(this),
            descriptor).Via(Slot->GetInvoker()));
    }

    void OnTableChunksDownloaded(const TTableFileDescriptor& descriptor)
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        if (JobPhase > EJobPhase::Cleanup)
            return;
        YCHECK(JobPhase == EJobPhase::PreparingSandbox);

        auto chunks = PatchCachedChunkReplicas(descriptor.table());
        auto config = New<TTableReaderConfig>();

        auto readerProvider = New<TTableChunkReaderProvider>(
            chunks,
            config);
        auto asyncReader = New<TTableChunkSequenceReader>(
            config,
            Bootstrap->GetMasterChannel(),
            Bootstrap->GetBlockStore()->GetBlockCache(),
            NodeDirectory,
            std::move(chunks),
            readerProvider);

        auto syncReader = CreateSyncReader(asyncReader);
        auto format = ConvertTo<NFormats::TFormat>(TYsonString(descriptor.format()));
        auto fileName = descriptor.file_name();
        try {
            syncReader->Open();

            auto producer = [&] (TOutputStream* output) {
                auto consumer = CreateConsumerForFormat(
                    format,
                    NFormats::EDataType::Tabular,
                    output);
                ProduceYson(syncReader, ~consumer);
            };

            Slot->MakeFile(fileName, producer);
        } catch (const std::exception& ex) {
            auto wrappedError = TError(
                "Failed to write user table file %s",
                ~fileName.Quote())
                << ex;
            DoAbort(wrappedError, EJobState::Failed);
            return;
        }

        LOG_INFO("User table file downloaded successfully (FileName: %s)",
            ~fileName);
    }

    static bool IsFatalError(const TError& error)
    {
        return
            error.FindMatching(NTableClient::EErrorCode::SortOrderViolation) ||
            error.FindMatching(NSecurityClient::EErrorCode::AuthenticationError) ||
            error.FindMatching(NSecurityClient::EErrorCode::AuthorizationError) ||
            error.FindMatching(NSecurityClient::EErrorCode::AccountIsOverLimit);
    }

    static bool IsRetriableSystemError(const TError& error)
    {
        return
            error.FindMatching(NChunkClient::EErrorCode::AllTargetNodesFailed) ||
            error.FindMatching(NChunkClient::EErrorCode::MasterCommunicationFailed) ||
            error.FindMatching(NTableClient::EErrorCode::MasterCommunicationFailed);
    }

};

NJobAgent::IJobPtr CreateUserJob(
    const TJobId& jobId,
    const TNodeResources& resourceLimits,
    TJobSpec&& jobSpec,
    TBootstrap* bootstrap)
{
    return New<TJob>(
        jobId,
        resourceLimits,
        std::move(jobSpec),
        bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NExecAgent
} // namespace NYT

