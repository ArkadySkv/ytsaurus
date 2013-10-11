#include "job_controller.h"
#include "config.h"
#include "private.h"

#include <ytlib/misc/fs.h>

#include <ytlib/node_tracker_client/helpers.h>

#include <server/scheduler/job_resources.h>

#include <server/chunk_holder/master_connector.h>

#include <server/exec_agent/slot_manager.h>
#include <server/exec_agent/public.h>

#include <server/cell_node/bootstrap.h>

namespace NYT {
namespace NJobAgent {

using namespace NNodeTrackerClient;
using namespace NNodeTrackerClient::NProto;
using namespace NJobTrackerClient::NProto;
using namespace NYTree;
using namespace NCellNode;

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = JobTrackerServerLogger;

////////////////////////////////////////////////////////////////////////////////

TJobController::TJobController(
    TJobControllerConfigPtr config,
    TBootstrap* bootstrap)
    : Config(config)
    , Bootstrap(bootstrap)
    , StartScheduled(false)
    , ResourcesUpdatedFlag(false)
{
    YCHECK(config);
    YCHECK(bootstrap);
}

void TJobController::RegisterFactory(EJobType type, TJobFactory factory)
{
    YCHECK(Factories.insert(std::make_pair(type, factory)).second);
}

TJobFactory TJobController::GetFactory(EJobType type)
{
    auto it = Factories.find(type);
    YCHECK(it != Factories.end());
    return it->second;
}

IJobPtr TJobController::FindJob(const TJobId& jobId)
{
    auto it = Jobs.find(jobId);
    return it == Jobs.end() ? nullptr : it->second;
}

IJobPtr TJobController::GetJobOrThrow(const TJobId& jobId)
{
    auto job = FindJob(jobId);
    if (!job) {
        THROW_ERROR_EXCEPTION("No such job %s", ~ToString(jobId));
    }
    return job;
}

std::vector<IJobPtr> TJobController::GetJobs()
{
    std::vector<IJobPtr> result;
    FOREACH (const auto& pair, Jobs) {
        result.push_back(pair.second);
    }
    return result;
}

TNodeResources TJobController::GetResourceLimits()
{
    TNodeResources result;
    result.set_user_slots(Bootstrap->GetSlotManager()->GetSlotCount());
    result.set_cpu(Config->ResourceLimits->Cpu);
    result.set_network(Config->ResourceLimits->Network);
    result.set_replication_slots(Config->ResourceLimits->ReplicationSlots);
    result.set_removal_slots(Config->ResourceLimits->RemovalSlots);
    result.set_repair_slots(Config->ResourceLimits->RepairSlots);

    const auto& tracker = Bootstrap->GetMemoryUsageTracker();
    result.set_memory(tracker.GetFree() + tracker.GetUsed(EMemoryConsumer::Job));

    return result;
}

TNodeResources TJobController::GetResourceUsage(bool includeWaiting)
{
    auto result = ZeroNodeResources();
    FOREACH (const auto& pair, Jobs) {
        if (includeWaiting || pair.second->GetState() != EJobState::Waiting) {
            auto usage = pair.second->GetResourceUsage();
            result += usage;
        }
    }
    return result;
}

void TJobController::StartWaitingJobs()
{
    auto& tracker = Bootstrap->GetMemoryUsageTracker();

    FOREACH (const auto& pair, Jobs) {
        auto job = pair.second;
        if (job->GetState() != EJobState::Waiting)
            continue;

        auto usedResources = GetResourceUsage(false);
        {
            auto memoryToRelease = tracker.GetUsed(EMemoryConsumer::Job) - usedResources.memory();
            YCHECK(memoryToRelease >= 0);
            tracker.Release(EMemoryConsumer::Job, memoryToRelease);
        }

        auto spareResources = GetResourceLimits() - usedResources;
        auto jobResources = job->GetResourceUsage();

        if (Dominates(spareResources, jobResources)) {
            auto error = tracker.TryAcquire(EMemoryConsumer::Job, jobResources.memory());

            if (error.IsOK()) {
                LOG_INFO("Starting job (JobId: %s)", ~ToString(job->GetId()));

                job->SubscribeResourcesReleased(
                    BIND(&TJobController::OnResourcesReleased, MakeWeak(this))
                        .Via(Bootstrap->GetControlInvoker()));

                job->Start();
            } else {
                LOG_DEBUG(error, "Not enough memory to start waiting job (JobId: %s)",
                    ~ToString(job->GetId()));
            }
        } else {
            LOG_DEBUG("Not enough resources to start waiting job (JobId: %s, SpareResources: %s, JobResources: %s)",
                ~ToString(job->GetId()),
                ~FormatResources(spareResources),
                ~FormatResources(jobResources));
        }
    }

    if (ResourcesUpdatedFlag) {
        ResourcesUpdatedFlag = false;
        ResourcesUpdated_.Fire();
    }

    StartScheduled = false;
}

IJobPtr TJobController::CreateJob(
    const TJobId& jobId,
    const NNodeTrackerClient::NProto::TNodeResources& resourceLimits,
    TJobSpec&& jobSpec)
{
    auto type = EJobType(jobSpec.type());

    auto factory = GetFactory(type);

    auto job = factory.Run(
        jobId,
        resourceLimits,
        std::move(jobSpec));

    LOG_INFO("Job created (JobId: %s, Type: %s)",
        ~ToString(jobId),
        ~type.ToString());

    YCHECK(Jobs.insert(std::make_pair(jobId, job)).second);
    ScheduleStart();

    return job;
}

void TJobController::ScheduleStart()
{
    if (!StartScheduled) {
        Bootstrap->GetControlInvoker()->Invoke(BIND(
            &TJobController::StartWaitingJobs,
            MakeWeak(this)));
        StartScheduled = true;
    }
}

void TJobController::AbortJob(IJobPtr job)
{
    LOG_INFO("Job abort requested (JobId: %s)",
        ~ToString(job->GetId()));

    job->Abort(TError(NExecAgent::EErrorCode::AbortByScheduler, "Job aborted by scheduler"));
}

void TJobController::RemoveJob(IJobPtr job)
{
    LOG_INFO("Job removed (JobId: %s)",
        ~ToString(job->GetId()));

    YCHECK(job->GetPhase() > EJobPhase::Cleanup);
    YCHECK(job->GetResourceUsage() == ZeroNodeResources());
    YCHECK(Jobs.erase(job->GetId()) == 1);
}

void TJobController::OnResourcesReleased()
{
    ResourcesUpdatedFlag = true;
    ScheduleStart();
}

void TJobController::UpdateJobResourceUsage(IJobPtr job, const TNodeResources& usage)
{
    if (job->GetState() != EJobState::Running) {
        // Outdated request.
        return;
    }

    auto oldUsage = job->GetResourceUsage();
    auto delta = usage - oldUsage;

    if (!Dominates(GetResourceLimits(), GetResourceUsage(false) + delta)) {
        job->Abort(TError(
            NExecAgent::EErrorCode::ResourceOverdraft,
            "Failed to increase resource usage (OldUsage: {%s}, NewUsage: {%s})",
            ~FormatResources(oldUsage),
            ~FormatResources(usage)));
        return;
    }

    if (delta.memory() > 0) {
        auto& tracker = Bootstrap->GetMemoryUsageTracker();
        auto error = tracker.TryAcquire(EMemoryConsumer::Job, delta.memory());
        if (!error.IsOK()) {
            job->Abort(TError(
                NExecAgent::EErrorCode::ResourceOverdraft,
                "Failed to increase resource usage (OldUsage: {%s}, NewUsage: {%s})",
                ~FormatResources(oldUsage),
                ~FormatResources(usage))
                << error);
            return;
        }
    }

    job->SetResourceUsage(usage);

    if (!Dominates(delta, ZeroNodeResources())) {
        OnResourcesReleased();
    }
}

void TJobController::UpdateJobProgress(IJobPtr job, double progress)
{
    if (job->GetState() != EJobState::Running) {
        // Outdated request.
        return;
    }

    job->SetProgress(progress);
}

void TJobController::SetJobResult(IJobPtr job, const TJobResult& result)
{
    job->SetResult(result);
}

void TJobController::PrepareHeartbeat(TReqHeartbeat* request)
{
    auto masterConnector = Bootstrap->GetMasterConnector();
    request->set_node_id(masterConnector->GetNodeId());
    ToProto(request->mutable_node_descriptor(), Bootstrap->GetLocalDescriptor());
    *request->mutable_resource_limits() = GetResourceLimits();
    *request->mutable_resource_usage() = GetResourceUsage();

    FOREACH (const auto& pair, Jobs) {
        auto job = pair.second;
        auto type = EJobType(job->GetSpec().type());
        auto state = job->GetState();
        auto* jobStatus = request->add_jobs();
        ToProto(jobStatus->mutable_job_id(), job->GetId());
        jobStatus->set_job_type(type);
        jobStatus->set_state(state);
        jobStatus->set_phase(job->GetPhase());
        jobStatus->set_progress(job->GetProgress());
        switch (state) {
            case EJobState::Running:
                *jobStatus->mutable_resource_usage() = job->GetResourceUsage();
                break;

            case EJobState::Completed:
            case EJobState::Aborted:
            case EJobState::Failed:
                *jobStatus->mutable_result() = job->GetResult();
                break;

            default:
                break;
        }
    }
}

void TJobController::ProcessHeartbeat(TRspHeartbeat* response)
{
    FOREACH (const auto& protoJobId, response->jobs_to_remove()) {
        auto jobId = FromProto<TJobId>(protoJobId);
        auto job = FindJob(jobId);
        if (job) {
            RemoveJob(job);
        } else {
            LOG_WARNING("Requested to remove a non-existing job (JobId: %s)",
                ~ToString(jobId));
        }
    }

    FOREACH (const auto& protoJobId, response->jobs_to_abort()) {
        auto jobId = FromProto<TJobId>(protoJobId);
        auto job = FindJob(jobId);
        if (job) {
            AbortJob(job);
        } else {
            LOG_WARNING("Requested to abort a non-existing job (JobId: %s)",
                ~ToString(jobId));
        }
    }

    FOREACH (auto& info, *response->mutable_jobs_to_start()) {
        auto jobId = FromProto<TJobId>(info.job_id());
        const auto& resourceLimits = info.resource_limits();
        auto& spec = *info.mutable_spec();
        CreateJob(jobId, resourceLimits, std::move(spec));
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
} // namespace NJobAgent
