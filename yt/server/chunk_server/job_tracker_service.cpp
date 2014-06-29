#include "stdafx.h"
#include "job_tracker_service.h"
#include "chunk_manager.h"
#include "job.h"
#include "chunk.h"
#include "private.h"

#include <core/misc/string.h>
#include <core/misc/protobuf_helpers.h>

#include <ytlib/hydra/rpc_helpers.h>

#include <ytlib/job_tracker_client/job_tracker_service_proxy.h>

#include <ytlib/node_tracker_client/helpers.h>

#include <ytlib/chunk_client/job.pb.h>

#include <server/cell_master/hydra_service.h>

#include <server/node_tracker_server/node_tracker.h>
#include <server/node_tracker_server/node.h>
#include <server/node_tracker_server/node_directory_builder.h>

#include <server/cell_master/bootstrap.h>

namespace NYT {
namespace NChunkServer {

using namespace NJobTrackerClient;
using namespace NNodeTrackerClient;
using namespace NNodeTrackerServer;
using namespace NJobTrackerClient::NProto;
using namespace NChunkClient::NProto;
using namespace NCellMaster;

////////////////////////////////////////////////////////////////////////////////

static auto& Logger = ChunkServerLogger;

////////////////////////////////////////////////////////////////////////////////

class TJobTrackerService
    : public NCellMaster::THydraServiceBase
{
public:
    explicit TJobTrackerService(TBootstrap* bootstrap)
        : THydraServiceBase(
            bootstrap,
            TJobTrackerServiceProxy::GetServiceName(),
            ChunkServerLogger.GetCategory())
    {
        RegisterMethod(RPC_SERVICE_METHOD_DESC(Heartbeat));
    }

private:
    DECLARE_RPC_SERVICE_METHOD(NJobTrackerClient::NProto, Heartbeat)
    {
        ValidateActiveLeader();

        auto nodeId = request->node_id();
        const auto& resourceLimits = request->resource_limits();
        const auto& resourceUsage = request->resource_usage();

        context->SetRequestInfo("NodeId: %d, ResourceUsage: {%s}",
            nodeId,
            ~FormatResourceUsage(resourceUsage, resourceLimits));

        auto nodeTracker = Bootstrap->GetNodeTracker();
        auto chunkManager = Bootstrap->GetChunkManager();

        auto* node = nodeTracker->GetNodeOrThrow(nodeId);

        if (node->GetState() != ENodeState::Online) {
            context->Reply(TError(
                NNodeTrackerClient::EErrorCode::InvalidState,
                "Cannot process a heartbeat in %s state",
                ~FormatEnum(node->GetState())));
            return;
        }

        node->ResourceLimits() = resourceLimits;
        node->ResourceUsage() = resourceUsage;

        std::vector<TJobPtr> currentJobs;
        for (const auto& jobStatus : request->jobs()) {
            auto jobId = FromProto<TJobId>(jobStatus.job_id());
            auto state = EJobState(jobStatus.state());
            auto jobType = EJobType(jobStatus.job_type());
            if (jobType <= EJobType::MasterFirst || jobType >= EJobType::MasterLast) {
                // Create a foreign job.
                auto job = TJob::CreateForeign(
                    jobId,
                    jobStatus.resource_usage());
                currentJobs.push_back(job);
            } else {
                // Lookup the master job.
                auto job = chunkManager->FindJob(jobId);
                if (job) {
                    job->SetState(state);
                    if (state == EJobState::Completed || state == EJobState::Failed) {
                        job->Error() = FromProto<TError>(jobStatus.result().error());
                    }
                    currentJobs.push_back(job);
                } else {
                    switch (state) {
                        case EJobState::Completed:
                            LOG_WARNING("Unknown job has completed, removal scheduled (JobId: %s)",
                                ~ToString(jobId));
                            ToProto(response->add_jobs_to_remove(), jobId);
                            break;

                        case EJobState::Failed:
                            LOG_INFO("Unknown job has failed, removal scheduled (JobId: %s)",
                                ~ToString(jobId));
                            ToProto(response->add_jobs_to_remove(), jobId);
                            break;

                        case EJobState::Aborted:
                            LOG_INFO("Job aborted, removal scheduled (JobId: %s)",
                                ~ToString(jobId));
                            ToProto(response->add_jobs_to_remove(), jobId);
                            break;

                        case EJobState::Running:
                            LOG_WARNING("Unknown job is running, abort scheduled (JobId: %s)",
                                ~ToString(jobId));
                            ToProto(response->add_jobs_to_abort(), jobId);
                            break;

                        case EJobState::Waiting:
                            LOG_WARNING("Unknown job is waiting, abort scheduled (JobId: %s)",
                                ~ToString(jobId));
                            ToProto(response->add_jobs_to_abort(), jobId);
                            break;

                        default:
                            YUNREACHABLE();
                    }
                }
            }
        }

        std::vector<TJobPtr> jobsToStart;
        std::vector<TJobPtr> jobsToAbort;
        std::vector<TJobPtr> jobsToRemove;
        chunkManager->ScheduleJobs(
            node,
            currentJobs,
            &jobsToStart,
            &jobsToAbort,
            &jobsToRemove);

        for (auto job : jobsToStart) {
            const auto& chunkIdWithIndex = job->GetChunkIdWithIndex();

            auto* jobInfo = response->add_jobs_to_start();
            ToProto(jobInfo->mutable_job_id(), job->GetJobId());
            *jobInfo->mutable_resource_limits() = job->ResourceUsage();

            auto* jobSpec = jobInfo->mutable_spec();
            jobSpec->set_type(job->GetType());

            auto* chunkjobSpecExt = jobSpec->MutableExtension(TChunkJobSpecExt::chunk_job_spec_ext);
            ToProto(chunkjobSpecExt->mutable_chunk_id(), EncodeChunkId(chunkIdWithIndex));

            switch (job->GetType()) {
                case EJobType::ReplicateChunk: {
                    auto* replciateChunkJobSpecExt = jobSpec->MutableExtension(TReplicateChunkJobSpecExt::replicate_chunk_job_spec_ext);
                    SerializeDescriptors(replciateChunkJobSpecExt->mutable_targets(), job->TargetAddresses());
                    break;
                }

                case EJobType::RemoveChunk:
                    break;

                case EJobType::RepairChunk: {
                    auto* chunk = chunkManager->GetChunk(chunkIdWithIndex.Id);

                    auto* repairChunkJobSpecExt = jobSpec->MutableExtension(TRepairChunkJobSpecExt::repair_chunk_job_spec_ext);
                    repairChunkJobSpecExt->set_erasure_codec(chunk->GetErasureCodec());
                    ToProto(repairChunkJobSpecExt->mutable_erased_indexes(), job->ErasedIndexes());

                    NNodeTrackerServer::TNodeDirectoryBuilder builder(repairChunkJobSpecExt->mutable_node_directory());
                    const auto& replicas = chunk->StoredReplicas();
                    builder.Add(replicas);
                    ToProto(repairChunkJobSpecExt->mutable_replicas(), replicas);

                    SerializeDescriptors(repairChunkJobSpecExt->mutable_targets(), job->TargetAddresses());
                    break;
                }

                case EJobType::SealChunk: {
                    auto* chunk = chunkManager->GetChunk(chunkIdWithIndex.Id);

                    auto* sealChunkJobSpecExt = jobSpec->MutableExtension(TSealChunkJobSpecExt::seal_chunk_job_spec_ext);

                    sealChunkJobSpecExt->set_record_count(chunk->GetSealedRecordCount());

                    NNodeTrackerServer::TNodeDirectoryBuilder builder(sealChunkJobSpecExt->mutable_node_directory());
                    const auto& replicas = chunk->StoredReplicas();
                    builder.Add(replicas);
                    ToProto(sealChunkJobSpecExt->mutable_replicas(), replicas);
                    break;
                }

                default:
                    YUNREACHABLE();
            }
        }

        for (auto job : jobsToAbort) {
            ToProto(response->add_jobs_to_abort(), job->GetJobId());
        }

        for (auto job : jobsToRemove) {
            ToProto(response->add_jobs_to_remove(), job->GetJobId());
        }

        context->Reply();
    }

    void SerializeDescriptors(
        ::google::protobuf::RepeatedPtrField<NNodeTrackerClient::NProto::TNodeDescriptor>* protoDescriptors,
        const std::vector<Stroka>& addresses)
    {
        auto nodeTracker = Bootstrap->GetNodeTracker();
        for (const auto& address : addresses) {
            auto* target = nodeTracker->GetNodeByAddress(address);
            NNodeTrackerClient::ToProto(protoDescriptors->Add(), target->GetDescriptor());
        }
    }

};

NRpc::IServicePtr CreateJobTrackerService(TBootstrap* boostrap)
{
    return New<TJobTrackerService>(boostrap);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkServer
} // namespace NYT
