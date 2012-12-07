#include "stdafx.h"
#include "operation_controller_detail.h"
#include "private.h"
#include "chunk_list_pool.h"
#include "chunk_pool.h"
#include "job_resources.h"

#include <ytlib/transaction_client/transaction.h>

#include <ytlib/chunk_client//chunk_list_ypath_proxy.h>

#include <ytlib/object_client/object_ypath_proxy.h>

#include <ytlib/cypress_client/cypress_ypath_proxy.h>

#include <ytlib/ytree/fluent.h>
#include <ytlib/ytree/convert.h>

#include <ytlib/formats/format.h>

#include <ytlib/chunk_client/chunk_meta_extensions.h>

#include <ytlib/transaction_client/transaction_ypath_proxy.h>
#include <ytlib/transaction_client/transaction_manager.h>

#include <ytlib/scheduler/config.h>

#include <ytlib/table_client/helpers.h>

#include <ytlib/meta_state/rpc_helpers.h>

#include <cmath>

namespace NYT {
namespace NScheduler {

using namespace NCypressClient;
using namespace NTransactionClient;
using namespace NFileClient;
using namespace NTableClient;
using namespace NChunkClient;
using namespace NObjectClient;
using namespace NYTree;
using namespace NYson;
using namespace NYPath;
using namespace NFormats;
using namespace NJobProxy;
using namespace NScheduler::NProto;

////////////////////////////////////////////////////////////////////

TOperationControllerBase::TTask::TTask(TOperationControllerBase* controller)
    : Controller(controller)
    , CachedPendingJobCount(0)
    , CachedTotalNeededResources(ZeroNodeResources())
    , Logger(Controller->Logger)
{ }

int TOperationControllerBase::TTask::GetPendingJobCount() const
{
    return GetChunkPoolOutput()->GetPendingJobCount();
}

int TOperationControllerBase::TTask::GetPendingJobCountDelta()
{
    int oldValue = CachedPendingJobCount;
    int newValue = GetPendingJobCount();
    CachedPendingJobCount = newValue;
    return newValue - oldValue;
}

TNodeResources TOperationControllerBase::TTask::GetTotalNeededResourcesDelta()
{
    auto oldValue = CachedTotalNeededResources;
    auto newValue = GetTotalNeededResources();
    CachedTotalNeededResources = newValue;
    newValue -= oldValue;
    return newValue;
}

TNodeResources TOperationControllerBase::TTask::GetTotalNeededResources() const
{
    i64 count = GetPendingJobCount();
    // NB: Don't call GetAvgNeededResources if there are no pending jobs.
    return count == 0 ? ZeroNodeResources() : GetAvgNeededResources() * count;
}

i64 TOperationControllerBase::TTask::GetLocality(const Stroka& address) const
{
    return GetChunkPoolOutput()->GetLocality(address);
}

bool TOperationControllerBase::TTask::IsStrictlyLocal() const
{
    return false;
}

int TOperationControllerBase::TTask::GetPriority() const
{
    return 0;
}

void TOperationControllerBase::TTask::AddInput(TChunkStripePtr stripe)
{
    GetChunkPoolInput()->Add(stripe);
    AddInputLocalityHint(stripe);
    AddPendingHint();
}

void TOperationControllerBase::TTask::AddInput(const std::vector<TChunkStripePtr>& stripes)
{
    FOREACH (auto stripe, stripes) {
        AddInput(stripe);
    }
}

void TOperationControllerBase::TTask::FinishInput()
{
    LOG_DEBUG("Task input finished (Task: %s)", ~GetId());

    GetChunkPoolInput()->Finish();
    AddPendingHint();
}

TJobPtr TOperationControllerBase::TTask::ScheduleJob(ISchedulingContext* context)
{
    if (!Controller->HasEnoughChunkLists(GetChunkListCountPerJob())) {
        return NULL;
    }

    auto joblet = New<TJoblet>(this, Controller->JobIndexGenerator.Next());

    auto address = context->GetNode()->GetAddress();
    auto* chunkPoolOutput = GetChunkPoolOutput();
    joblet->OutputCookie = chunkPoolOutput->Extract(address);
    if (joblet->OutputCookie == IChunkPoolOutput::NullCookie) {
        return NULL;
    }

    joblet->InputStripeList = chunkPoolOutput->GetStripeList(joblet->OutputCookie);

    // Compute the actual utilization for this joblet and check it
    // against the the limits. This is the last chance to give up.
    auto neededResources = GetNeededResources(joblet);
    auto node = context->GetNode();
    if (!node->HasEnoughResources(neededResources)) {
        chunkPoolOutput->Failed(joblet->OutputCookie);
        return NULL;
    }

    LOG_DEBUG("Job chunks extracted (TotalCount: %d, LocalCount: %d, DataSize: %" PRId64 ", RowCount: %" PRId64 ")",
        joblet->InputStripeList->TotalChunkCount,
        joblet->InputStripeList->LocalChunkCount,
        joblet->InputStripeList->TotalDataSize,
        joblet->InputStripeList->TotalRowCount);

    auto job = context->BeginStartJob(Controller->Operation);
    joblet->Job = job;

    auto* jobSpec = joblet->Job->GetSpec();
    BuildJobSpec(joblet, jobSpec);
    *jobSpec->mutable_resource_utilization() = neededResources;
    context->EndStartJob(job);

    Controller->RegisterJobInProgress(joblet);

    OnJobStarted(joblet);

    return joblet->Job;
}

bool TOperationControllerBase::TTask::IsPending() const
{
    return GetChunkPoolOutput()->GetPendingJobCount() > 0;
}

bool TOperationControllerBase::TTask::IsCompleted() const
{
    return GetChunkPoolOutput()->IsCompleted();
}

i64 TOperationControllerBase::TTask::GetTotalDataSize() const
{
    return GetChunkPoolOutput()->GetTotalDataSize();
}

i64 TOperationControllerBase::TTask::GetCompletedDataSize() const
{
    return GetChunkPoolOutput()->GetCompletedDataSize();
}

i64 TOperationControllerBase::TTask::GetPendingDataSize() const
{
    return GetChunkPoolOutput()->GetPendingDataSize();
}

void TOperationControllerBase::TTask::OnJobStarted(TJobletPtr joblet)
{
    UNUSED(joblet);
}

void TOperationControllerBase::TTask::OnJobCompleted(TJobletPtr joblet)
{
    GetChunkPoolOutput()->Completed(joblet->OutputCookie);
}

void TOperationControllerBase::TTask::ReleaseFailedJobResources(TJobletPtr joblet)
{
    auto* chunkPoolOutput = GetChunkPoolOutput();

    Controller->ReleaseChunkLists(joblet->ChunkListIds);

    auto list = chunkPoolOutput->GetStripeList(joblet->OutputCookie);
    FOREACH (const auto& stripe, list->Stripes) {
        AddInputLocalityHint(stripe);
    }

    chunkPoolOutput->Failed(joblet->OutputCookie);

    AddPendingHint();
}

void TOperationControllerBase::TTask::OnJobFailed(TJobletPtr joblet)
{
    ReleaseFailedJobResources(joblet);
}

void TOperationControllerBase::TTask::OnJobAborted(TJobletPtr joblet)
{
    ReleaseFailedJobResources(joblet);
}

void TOperationControllerBase::TTask::OnTaskCompleted()
{
    LOG_DEBUG("Task completed (Task: %s)", ~GetId());
}

void TOperationControllerBase::TTask::AddPendingHint()
{
    Controller->AddTaskPendingHint(this);
}

void TOperationControllerBase::TTask::AddInputLocalityHint(TChunkStripePtr stripe)
{
    Controller->AddTaskLocalityHint(this, stripe);
}

void TOperationControllerBase::TTask::AddSequentialInputSpec(
    NScheduler::NProto::TJobSpec* jobSpec,
    TJobletPtr joblet,
    bool enableTableIndex)
{
    auto* inputSpec = jobSpec->add_input_specs();
    auto list = joblet->InputStripeList;
    FOREACH (const auto& stripe, list->Stripes) {
        AddInputChunks(inputSpec, stripe, list->PartitionTag, enableTableIndex);
    }
    UpdateInputSpecTotals(jobSpec, joblet);
}

void TOperationControllerBase::TTask::AddParallelInputSpec(
    NScheduler::NProto::TJobSpec* jobSpec,
    TJobletPtr joblet,
    bool enableTableIndex)
{
    auto list = joblet->InputStripeList;
    FOREACH (const auto& stripe, list->Stripes) {
        auto* inputSpec = jobSpec->add_input_specs();
        AddInputChunks(inputSpec, stripe, list->PartitionTag, enableTableIndex);
    }
    UpdateInputSpecTotals(jobSpec, joblet);
}

void TOperationControllerBase::TTask::UpdateInputSpecTotals(
    NScheduler::NProto::TJobSpec* jobSpec,
    TJobletPtr joblet)
{
    auto list = joblet->InputStripeList;
    jobSpec->set_input_uncompressed_data_size(
        jobSpec->input_uncompressed_data_size() +
        list->TotalDataSize);
    jobSpec->set_input_row_count(
        jobSpec->input_row_count() +
        list->TotalRowCount);
}

void TOperationControllerBase::TTask::AddFinalOutputSpecs(
    NScheduler::NProto::TJobSpec* jobSpec,
    TJobletPtr joblet)
{
    FOREACH (const auto& table, Controller->OutputTables) {
        auto* outputSpec = jobSpec->add_output_specs();
        outputSpec->set_channels(table.Channels.Data());
        outputSpec->set_replication_factor(table.ReplicationFactor);
        auto chunkListId = Controller->ExtractChunkList();
        joblet->ChunkListIds.push_back(chunkListId);
        *outputSpec->mutable_chunk_list_id() = chunkListId.ToProto();
    }
}

void TOperationControllerBase::TTask::AddIntermediateOutputSpec(
    NScheduler::NProto::TJobSpec* jobSpec,
    TJobletPtr joblet)
{
    auto* outputSpec = jobSpec->add_output_specs();
    outputSpec->set_channels("[]");
    auto chunkListId = Controller->ExtractChunkList();
    joblet->ChunkListIds.push_back(chunkListId);
    *outputSpec->mutable_chunk_list_id() = chunkListId.ToProto();
}

void TOperationControllerBase::TTask::AddInputChunks(
    NScheduler::NProto::TTableInputSpec* inputSpec,
    TChunkStripePtr stripe,
    TNullable<int> partitionTag,
    bool enableTableIndex)
{
    FOREACH (const auto& stripeChunk, stripe->Chunks) {
        auto* inputChunk = inputSpec->add_chunks();
        *inputChunk = *stripeChunk;
        if (!enableTableIndex) {
            inputChunk->clear_table_index();
        }
        if (partitionTag) {
            inputChunk->set_partition_tag(partitionTag.Get());
        }
    }
}

TNodeResources TOperationControllerBase::TTask::GetAvgNeededResources() const
{
    return GetMinNeededResources();
}

TNodeResources TOperationControllerBase::TTask::GetNeededResources(TJobletPtr joblet) const
{
    UNUSED(joblet);
    return GetMinNeededResources();
}

////////////////////////////////////////////////////////////////////

TOperationControllerBase::TOperationControllerBase(
    TSchedulerConfigPtr config,
    IOperationHost* host,
    TOperation* operation)
    : Config(config)
    , Host(host)
    , Operation(operation)
    , ObjectProxy(host->GetMasterChannel())
    , Logger(OperationLogger)
    , CancelableContext(New<TCancelableContext>())
    , CancelableControlInvoker(CancelableContext->CreateInvoker(Host->GetControlInvoker()))
    , CancelableBackgroundInvoker(CancelableContext->CreateInvoker(Host->GetBackgroundInvoker()))
    , Active(false)
    , Running(false)
    , TotalInputChunkCount(0)
    , TotalInputDataSize(0)
    , TotalInputRowCount(0)
    , TotalInputValueCount(0)
    , UsedResources(ZeroNodeResources())
    , PendingTaskInfos(MaxTaskPriority + 1)
    , CachedPendingJobCount(0)
    , CachedNeededResources(ZeroNodeResources())
{
    Logger.AddTag(Sprintf("OperationId: %s", ~operation->GetOperationId().ToString()));
}

void TOperationControllerBase::Initialize()
{
    VERIFY_THREAD_AFFINITY(ControlThread);
    
    LOG_INFO("Initializing operation");

    FOREACH (const auto& path, GetInputTablePaths()) {
        TInputTable table;
        table.Path = path;
        InputTables.push_back(table);
    }

    FOREACH (const auto& path, GetOutputTablePaths()) {
        TOutputTable table;
        table.Path = path;
        if (path.Attributes().Get<bool>("overwrite", false)) {
            table.Clear = true;
            table.Overwrite = true;
            table.LockMode = ELockMode::Exclusive;
        }
        OutputTables.push_back(table);
    }

    FOREACH (const auto& path, GetFilePaths()) {
        TUserFile file;
        file.Path = path;
        Files.push_back(file);
    }

    try {
        DoInitialize();
    } catch (const std::exception& ex) {
        LOG_INFO(ex, "Operation has failed to initialize");
        Active = false;
        throw;
    }

    Active = true;

    LOG_INFO("Operation initialized");
}

void TOperationControllerBase::DoInitialize()
{ }

TFuture<void> TOperationControllerBase::Prepare()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    auto this_ = MakeStrong(this);
    auto pipeline = StartAsyncPipeline(CancelableBackgroundInvoker)
        ->Add(BIND(&TThis::StartIOTransactions, MakeStrong(this)))
        ->Add(BIND(&TThis::OnIOTransactionsStarted, MakeStrong(this)), CancelableControlInvoker)
        ->Add(BIND(&TThis::GetObjectIds, MakeStrong(this)))
        ->Add(BIND(&TThis::OnObjectIdsReceived, MakeStrong(this)))
        ->Add(BIND(&TThis::RequestInputs, MakeStrong(this)))
        ->Add(BIND(&TThis::OnInputsReceived, MakeStrong(this)))
        ->Add(BIND(&TThis::CompletePreparation, MakeStrong(this)));
     pipeline = CustomizePreparationPipeline(pipeline);
     return pipeline
        ->Add(BIND(&TThis::OnPreparationCompleted, MakeStrong(this)))
        ->Run()
        .Apply(BIND([=] (TValueOrError<void> result) -> TFuture<void> {
            if (result.IsOK()) {
                if (this_->Active) {
                    this_->Running = true;
                }
                return MakeFuture();
            } else {
                LOG_WARNING(result, "Operation has failed to prepare");
                this_->Active = false;
                this_->Host->OnOperationFailed(this_->Operation, result);
                // This promise is never fulfilled.
                return NewPromise<void>();
            }
        }));
}

TFuture<void> TOperationControllerBase::Revive()
{
    try {
        Initialize();
    } catch (const std::exception& ex) {
        OnOperationFailed(TError("Operation has failed to initialize")
            << ex);
        // This promise is never fulfilled.
        return NewPromise<void>();
    }
    return Prepare();
}

TFuture<void> TOperationControllerBase::Commit()
{
    VERIFY_THREAD_AFFINITY_ANY();

    YCHECK(Active);

    LOG_INFO("Committing operation");

    auto this_ = MakeStrong(this);
    return StartAsyncPipeline(CancelableBackgroundInvoker)
        ->Add(BIND(&TThis::CommitOutputs, MakeStrong(this)))
        ->Add(BIND(&TThis::OnOutputsCommitted, MakeStrong(this)))
        ->Run()
        .Apply(BIND([=] (TValueOrError<void> result) -> TFuture<void> {
            Active = false;
            if (result.IsOK()) {
                LOG_INFO("Operation committed");
                return MakeFuture();
            } else {
                LOG_WARNING(result, "Operation has failed to commit");
                this_->Host->OnOperationFailed(this_->Operation, result);
                return NewPromise<void>();
            }
        }));
}

void TOperationControllerBase::OnJobStarted(TJobPtr job)
{
    UsedResources += job->ResourceUtilization();
}

void TOperationControllerBase::OnJobRunning(TJobPtr job, const NProto::TJobStatus& status)
{
    UsedResources -= job->ResourceUtilization();
    job->ResourceUtilization() = status.resource_utilization();
    UsedResources += job->ResourceUtilization();
}

void TOperationControllerBase::OnJobCompleted(TJobPtr job)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    JobCounter.Completed(1);

    UsedResources -= job->ResourceUtilization();

    auto joblet = GetJobInProgress(job);
    joblet->Task->OnJobCompleted(joblet);
    
    RemoveJobInProgress(job);

    LogProgress();

    if (joblet->Task->IsCompleted()) {
        joblet->Task->OnTaskCompleted();
    }

    if (JobCounter.GetRunning() == 0 && GetPendingJobCount() == 0) {
        OnOperationCompleted();
    }
}

void TOperationControllerBase::OnJobFailed(TJobPtr job)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    JobCounter.Failed(1);

    UsedResources -= job->ResourceUtilization();

    auto joblet = GetJobInProgress(job);
    joblet->Task->OnJobFailed(joblet);

    RemoveJobInProgress(job);

    LogProgress();

    if (JobCounter.GetFailed() >= Config->FailedJobsLimit) {
        OnOperationFailed(TError("Failed jobs limit %d has been reached",
            Config->FailedJobsLimit));
    }

    FOREACH (const auto& chunkId, job->Result().failed_chunk_ids()) {
        OnChunkFailed(TChunkId::FromProto(chunkId));
    }
}

void TOperationControllerBase::OnJobAborted(TJobPtr job)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    JobCounter.Aborted(1);

    UsedResources -= job->ResourceUtilization();

    auto joblet = GetJobInProgress(job);
    joblet->Task->OnJobAborted(joblet);

    RemoveJobInProgress(job);

    LogProgress();
}

void TOperationControllerBase::OnChunkFailed(const TChunkId& chunkId)
{
    if (InputChunkIds.find(chunkId) == InputChunkIds.end()) {
        LOG_WARNING("Intermediate chunk %s has failed", ~chunkId.ToString());
        OnIntermediateChunkFailed(chunkId);
    } else {
        LOG_WARNING("Input chunk %s has failed", ~chunkId.ToString());
        OnInputChunkFailed(chunkId);
    }
}

void TOperationControllerBase::OnInputChunkFailed(const TChunkId& chunkId)
{
    OnOperationFailed(TError("Unable to read input chunk %s", ~chunkId.ToString()));
}

void TOperationControllerBase::OnIntermediateChunkFailed(const TChunkId& chunkId)
{
    OnOperationFailed(TError("Unable to read intermediate chunk %s", ~chunkId.ToString()));
}

void TOperationControllerBase::Abort()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    LOG_INFO("Aborting operation");

    Running = false;
    Active = false;
    CancelableContext->Cancel();

    AbortTransactions();

    LOG_INFO("Operation aborted");
}

void TOperationControllerBase::OnNodeOnline(TExecNodePtr node)
{
    UNUSED(node);
}

void TOperationControllerBase::OnNodeOffline(TExecNodePtr node)
{
    UNUSED(node);
}

TJobPtr TOperationControllerBase::ScheduleJob(
    ISchedulingContext* context,
    bool isStarving)
{
    VERIFY_THREAD_AFFINITY(ControlThread);
 
    if (!Running) {
        LOG_TRACE("Operation is not running, scheduling request ignored");
        return NULL;
    }

    if (GetPendingJobCount() == 0) {
        LOG_TRACE("No pending jobs left, scheduling request ignored");
        return NULL;
    }

    // Make a course check to see if the node has enough resources.
    auto node = context->GetNode();
    if (!HasEnoughResources(node)) {
        return NULL;
    }

    auto job = DoScheduleJob(context, isStarving);
    if (!job) {
        return NULL;
    }

    JobCounter.Start(1);
    LogProgress();
    return job;
}

void TOperationControllerBase::OnTaskUpdated(TTaskPtr task)
{
    int oldJobCount = CachedPendingJobCount;
    int newJobCount = CachedPendingJobCount + task->GetPendingJobCountDelta();
    CachedPendingJobCount = newJobCount;

    CachedNeededResources += task->GetTotalNeededResourcesDelta();

    LOG_DEBUG_IF(newJobCount != oldJobCount, "Pending job count updated: %d -> %d (Task: %s, NeededResources: {%s})",
        oldJobCount,
        newJobCount,
        ~task->GetId(),
        ~FormatResources(CachedNeededResources));
}

void TOperationControllerBase::AddTaskPendingHint(TTaskPtr task)
{
    if (!task->IsStrictlyLocal() && task->GetPendingJobCount() > 0) {
        auto* info = GetPendingTaskInfo(task);
        if (info->GlobalTasks.insert(task).second) {
            LOG_DEBUG("Task pending hint added (Task: %s)",
                ~task->GetId());
        }
    }
    OnTaskUpdated(task);
}

void TOperationControllerBase::DoAddTaskLocalityHint(TTaskPtr task, const Stroka& address)
{
    auto* info = GetPendingTaskInfo(task);
    if (info->AddressToLocalTasks[address].insert(task).second) {
        LOG_TRACE("Task locality hint added (Task: %s, Address: %s)",
            ~task->GetId(),
            ~address);
    }
}

TOperationControllerBase::TPendingTaskInfo* TOperationControllerBase::GetPendingTaskInfo(TTaskPtr task)
{
    int priority = task->GetPriority();
    YASSERT(priority >= 0 && priority <= MaxTaskPriority);
    return &PendingTaskInfos[priority];
}

void TOperationControllerBase::AddTaskLocalityHint(TTaskPtr task, const Stroka& address)
{
    DoAddTaskLocalityHint(task, address);
    OnTaskUpdated(task);
}

void TOperationControllerBase::AddTaskLocalityHint(TTaskPtr task, TChunkStripePtr stripe)
{
    FOREACH (const auto& chunk, stripe->Chunks) {
        FOREACH (const auto& address, chunk->node_addresses()) {
            DoAddTaskLocalityHint(task, address);
        }
    }
    OnTaskUpdated(task);
}

bool TOperationControllerBase::HasEnoughResources(TExecNodePtr node)
{
    return Dominates(
        node->ResourceLimits() + node->ResourceUtilizationDiscount(),
        node->ResourceUtilization() + GetMinNeededResources());
}

bool TOperationControllerBase::HasEnoughResources(TTaskPtr task, TExecNodePtr node)
{
    return node->HasEnoughResources(task->GetMinNeededResources());
}

TJobPtr TOperationControllerBase::DoScheduleJob(
    ISchedulingContext* context,
    bool isStarving)
{
    // First try to find a local task for this node.
    auto now = TInstant::Now();
    auto node = context->GetNode();
    auto address = node->GetAddress();
    for (int priority = static_cast<int>(PendingTaskInfos.size()) - 1; priority >= 0; --priority) {
        auto& info = PendingTaskInfos[priority];
        auto localTasksIt = info.AddressToLocalTasks.find(address);
        if (localTasksIt == info.AddressToLocalTasks.end()) {
            continue;
        }

        i64 bestLocality = 0;
        TTaskPtr bestTask = NULL;

        auto& localTasks = localTasksIt->second;
        auto it = localTasks.begin();
        while (it != localTasks.end()) {
            auto jt = it++;
            auto task = *jt;

            // Make sure that the task is ready to launch jobs.
            // Remove pending hint if not.
            i64 locality = task->GetLocality(address);
            if (locality <= 0) {
                localTasks.erase(jt);
                LOG_TRACE("Task locality hint removed (Task: %s, Address: %s)",
                    ~task->GetId(),
                    ~address);
                continue;
            }

            if (locality <= bestLocality) {
                continue;
            }

            if (!HasEnoughResources(task, node)) {
                continue;
            }

            if (task->GetPendingJobCount() == 0) {
                OnTaskUpdated(task);
                continue;
            }

            bestLocality = locality;
            bestTask = task;
        }

        if (bestTask) {
            auto job = bestTask->ScheduleJob(context);
            if (job) {
                auto delayedTime = bestTask->GetDelayedTime();
                LOG_DEBUG("Scheduled a local job (Task: %s, Address: %s, Priority: %d, Locality: %" PRId64 ", Delay: %s)",
                    ~bestTask->GetId(),
                    ~address,
                    priority,
                    bestLocality,
                    delayedTime ? ~ToString(now - delayedTime.Get()) : "Null");
                bestTask->SetDelayedTime(Null);
                OnTaskUpdated(bestTask);
                OnJobStarted(job);
                return job;
            }
        }
    }

    // Next look for other (global) tasks.
    for (int priority = static_cast<int>(PendingTaskInfos.size()) - 1; priority >= 0; --priority) {
        auto& info = PendingTaskInfos[priority];
        auto& globalTasks = info.GlobalTasks;
        auto it = globalTasks.begin();
        while (it != globalTasks.end()) {
            auto jt = it++;
            auto task = *jt;

            // Make sure that the task is ready to launch jobs.
            // Remove pending hint if not.
            if (task->GetPendingJobCount() == 0) {
                LOG_DEBUG("Task pending hint removed (Task: %s)", ~task->GetId());
                globalTasks.erase(jt);
                OnTaskUpdated(task);
                continue;
            }

            if (!HasEnoughResources(task, node)) {
                continue;
            }

            // Use delayed execution unless starving.
            bool mustWait = false;
            auto delayedTime = task->GetDelayedTime();
            if (delayedTime) {
                mustWait = delayedTime.Get() + task->GetLocalityTimeout() > now;
            } else {
                task->SetDelayedTime(now);
                mustWait = true;
            }
            if (!isStarving && mustWait) {
                continue;
            }

            auto job = task->ScheduleJob(context);
            if (job) {
                LOG_DEBUG("Scheduled a non-local job (Task: %s, Address: %s, Priority: %d, Delay: %s)",
                    ~task->GetId(),
                    ~address,
                    priority,
                    delayedTime ? ~ToString(now - delayedTime.Get()) : "Null");
                OnTaskUpdated(task);
                OnJobStarted(job);
                return job;
            }
        }
    }

    return NULL;
}

TCancelableContextPtr TOperationControllerBase::GetCancelableContext()
{
    return CancelableContext;
}

IInvokerPtr TOperationControllerBase::GetCancelableControlInvoker()
{
    return CancelableControlInvoker;
}

IInvokerPtr TOperationControllerBase::GetCancelableBackgroundInvoker()
{
    return CancelableBackgroundInvoker;
}

int TOperationControllerBase::GetPendingJobCount()
{
    return CachedPendingJobCount;
}

NProto::TNodeResources TOperationControllerBase::GetUsedResources()
{
    return UsedResources;
}

NProto::TNodeResources TOperationControllerBase::GetNeededResources()
{
    return CachedNeededResources;
}

void TOperationControllerBase::OnOperationCompleted()
{
    VERIFY_THREAD_AFFINITY_ANY();

    YCHECK(Active);
    LOG_INFO("Operation completed");

    JobCounter.Finalize();

    Running = false;

    Host->OnOperationCompleted(Operation);
}

void TOperationControllerBase::OnOperationFailed(const TError& error)
{
    VERIFY_THREAD_AFFINITY_ANY();

    if (!Active)
        return;

    LOG_WARNING(error, "Operation failed");

    Running = false;
    Active = false;

    Host->OnOperationFailed(Operation, error);
}

void TOperationControllerBase::AbortTransactions()
{
    LOG_INFO("Aborting transactions");

    Operation->GetSchedulerTransaction()->Abort();

    // No need to abort the others.
}

TObjectServiceProxy::TInvExecuteBatch TOperationControllerBase::CommitOutputs()
{
    VERIFY_THREAD_AFFINITY(BackgroundThread);

    LOG_INFO("Committing outputs");

    auto batchReq = ObjectProxy.ExecuteBatch();

    FOREACH (const auto& table, OutputTables) {
        auto path = FromObjectId(table.ObjectId);
        // Split large outputs into separate requests.
        {
            TChunkListYPathProxy::TReqAttachPtr req;
            int reqSize = 0;
            auto flushReq = [&] () {
                if (req) {
                    batchReq->AddRequest(req, "attach_out");
                    reqSize = 0;
                    req.Reset();
                }
            };

            FOREACH (const auto& pair, table.OutputChunkTreeIds) {
                if (!req) {
                    req = TChunkListYPathProxy::Attach(FromObjectId(table.OutputChunkListId));
                    NMetaState::GenerateRpcMutationId(req);
                }
                *req->add_children_ids() = pair.second.ToProto();
                ++reqSize;
                if (reqSize >= Config->MaxChildrenPerAttachRequest) {
                    flushReq();
                }
            }

            flushReq();
        }
        if (table.KeyColumns) {
            LOG_INFO("Table %s will be marked as sorted by %s",
                ~table.Path.GetPath(),
                ~ConvertToYsonString(table.KeyColumns.Get(), EYsonFormat::Text).Data());
            auto req = TTableYPathProxy::SetSorted(path);
            SetTransactionId(req, OutputTransaction);
            ToProto(req->mutable_key_columns(), table.KeyColumns.Get());
            NMetaState::GenerateRpcMutationId(req);
            batchReq->AddRequest(req, "set_out_sorted");
        }
    }

    {
        auto req = TTransactionYPathProxy::Commit(FromObjectId(InputTransaction->GetId()));
        NMetaState::GenerateRpcMutationId(req);
        batchReq->AddRequest(req, "commit_in_tx");
    }

    {
        auto req = TTransactionYPathProxy::Commit(FromObjectId(OutputTransaction->GetId()));
        NMetaState::GenerateRpcMutationId(req);
        batchReq->AddRequest(req, "commit_out_tx");
    }

    {
        auto req = TTransactionYPathProxy::Commit(FromObjectId(Operation->GetSchedulerTransaction()->GetId()));
        NMetaState::GenerateRpcMutationId(req);
        batchReq->AddRequest(req, "commit_scheduler_tx");
    }

    // We don't need pings any longer, detach the transactions.
    Operation->GetSchedulerTransaction()->Detach();
    InputTransaction->Detach();
    OutputTransaction->Detach();

    return batchReq->Invoke();
}

void TOperationControllerBase::OnOutputsCommitted(TObjectServiceProxy::TRspExecuteBatchPtr batchRsp)
{
    VERIFY_THREAD_AFFINITY(BackgroundThread);

    THROW_ERROR_EXCEPTION_IF_FAILED(*batchRsp, "Error committing outputs");

    {
        auto rsps = batchRsp->GetResponses("attach_out");
        FOREACH (auto rsp, rsps) {
            THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error attaching chunk trees");
        }
    }

    {
        auto rsps = batchRsp->GetResponses("set_out_sorted");
        FOREACH (auto rsp, rsps) {
            THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error marking output table as sorted");
        }
    }

    {
        auto rsp = batchRsp->GetResponse("commit_in_tx");
        THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error committing input transaction");
    }

    {
        auto rsp = batchRsp->GetResponse("commit_out_tx");
        THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error committing output transaction");
    }

    {
        auto rsp = batchRsp->GetResponse("commit_scheduler_tx");
        THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error committing scheduler transaction");
    }

    LOG_INFO("Outputs committed");
}

TObjectServiceProxy::TInvExecuteBatch TOperationControllerBase::StartIOTransactions()
{
    VERIFY_THREAD_AFFINITY(BackgroundThread);

    LOG_INFO("Starting IO transactions");

    auto batchReq = ObjectProxy.ExecuteBatch();
    const auto& schedulerTransactionId = Operation->GetSchedulerTransaction()->GetId();

    {
        auto req = TTransactionYPathProxy::CreateObject(FromObjectId(schedulerTransactionId));
        req->set_type(EObjectType::Transaction);
        NMetaState::GenerateRpcMutationId(req);
        batchReq->AddRequest(req, "start_in_tx");
    }

    {
        auto req = TTransactionYPathProxy::CreateObject(FromObjectId(schedulerTransactionId));
        req->set_type(EObjectType::Transaction);
        NMetaState::GenerateRpcMutationId(req);
        batchReq->AddRequest(req, "start_out_tx");
    }

    return batchReq->Invoke();
}

void TOperationControllerBase::OnIOTransactionsStarted(TObjectServiceProxy::TRspExecuteBatchPtr batchRsp)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    THROW_ERROR_EXCEPTION_IF_FAILED(*batchRsp, "Error starting IO transactions");

    {
        auto rsp = batchRsp->GetResponse<TTransactionYPathProxy::TRspCreateObject>("start_in_tx");
        THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error starting input transaction");
        auto id = TTransactionId::FromProto(rsp->object_id());
        LOG_INFO("Input transaction is %s", ~id.ToString());
        InputTransaction = Host->GetTransactionManager()->Attach(id, true);
    }

    {
        auto rsp = batchRsp->GetResponse<TTransactionYPathProxy::TRspCreateObject>("start_out_tx");
        THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error starting output transaction");
        auto id = TTransactionId::FromProto(rsp->object_id());
        LOG_INFO("Output transaction is %s", ~id.ToString());
        OutputTransaction = Host->GetTransactionManager()->Attach(id, true);
    }
}

TObjectServiceProxy::TInvExecuteBatch TOperationControllerBase::GetObjectIds()
{
    VERIFY_THREAD_AFFINITY(BackgroundThread);

    LOG_INFO("Getting object ids");

    auto batchReq = ObjectProxy.ExecuteBatch();

    FOREACH (const auto& table, InputTables) {
        auto req = TObjectYPathProxy::GetId(table.Path.GetPath());
        SetTransactionId(req, InputTransaction);
        req->set_allow_nonempty_path_suffix(true);
        batchReq->AddRequest(req, "get_in_id");
    }

    FOREACH (const auto& table, OutputTables) {
        auto req = TObjectYPathProxy::GetId(table.Path.GetPath());
        SetTransactionId(req, InputTransaction);
        // TODO(babenko): should we allow this?
        req->set_allow_nonempty_path_suffix(true);
        batchReq->AddRequest(req, "get_out_id");
    }

    return batchReq->Invoke();
}

void TOperationControllerBase::OnObjectIdsReceived(TObjectServiceProxy::TRspExecuteBatchPtr batchRsp)
{
    VERIFY_THREAD_AFFINITY(BackgroundThread);

    THROW_ERROR_EXCEPTION_IF_FAILED(*batchRsp, "Error getting object ids");

    {
        auto getInIdRsps = batchRsp->GetResponses<TObjectYPathProxy::TRspGetId>("get_in_id");
        for (int index = 0; index < static_cast<int>(InputTables.size()); ++index) {
            auto& table = InputTables[index];
            {
                auto rsp = getInIdRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error getting id for input table %s",
                    ~table.Path.GetPath());
                table.ObjectId = TObjectId::FromProto(rsp->object_id());
            }
        }
    }

    {
        auto getOutIdRsps = batchRsp->GetResponses<TObjectYPathProxy::TRspGetId>("get_out_id");
        for (int index = 0; index < static_cast<int>(OutputTables.size()); ++index) {
            auto& table = OutputTables[index];
            {
                auto rsp = getOutIdRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error getting id for output table %s",
                    ~table.Path.GetPath());
                table.ObjectId = TObjectId::FromProto(rsp->object_id());
            }
        }
    }

    LOG_INFO("Object ids received");
}

TObjectServiceProxy::TInvExecuteBatch TOperationControllerBase::RequestInputs()
{
    VERIFY_THREAD_AFFINITY(BackgroundThread);

    LOG_INFO("Requesting inputs");

    auto batchReq = ObjectProxy.ExecuteBatch();

    FOREACH (const auto& table, InputTables) {
        auto path = FromObjectId(table.ObjectId);
        {
            auto req = TCypressYPathProxy::Lock(path);
            SetTransactionId(req, InputTransaction);
            req->set_mode(ELockMode::Snapshot);
            NMetaState::GenerateRpcMutationId(req);
            batchReq->AddRequest(req, "lock_in");
        }
        {
            // NB: Use table.Path here, otherwise path suffix is ignored.
            auto req = TTableYPathProxy::Fetch(table.Path.GetPath());
            SetTransactionId(req, InputTransaction);
            req->set_fetch_node_addresses(true);
            req->set_fetch_all_meta_extensions(true);
            req->set_negate(table.NegateFetch);
            batchReq->AddRequest(req, "fetch_in");
        }
        {
            auto req = TYPathProxy::Get(path);
            SetTransactionId(req, InputTransaction);
            TAttributeFilter attributeFilter(EAttributeFilterMode::MatchingOnly);
            attributeFilter.Keys.push_back("sorted");
            attributeFilter.Keys.push_back("sorted_by");
            *req->mutable_attribute_filter() = ToProto(attributeFilter);
            batchReq->AddRequest(req, "get_in_attributes");
        }
    }

    FOREACH (const auto& table, OutputTables) {
        auto path = FromObjectId(table.ObjectId);
        {
            auto req = TCypressYPathProxy::Lock(path);
            SetTransactionId(req, OutputTransaction);
            req->set_mode(table.LockMode);
            NMetaState::GenerateRpcMutationId(req);
            batchReq->AddRequest(req, "lock_out");
        }
        {
            auto req = TYPathProxy::Get(path);
            SetTransactionId(req, OutputTransaction);
            TAttributeFilter attributeFilter(EAttributeFilterMode::MatchingOnly);
            attributeFilter.Keys.push_back("channels");
            attributeFilter.Keys.push_back("row_count");
            attributeFilter.Keys.push_back("replication_factor");
            *req->mutable_attribute_filter() = ToProto(attributeFilter);
            batchReq->AddRequest(req, "get_out_attributes");
        }
        if (table.Clear) {
            LOG_INFO("Output table %s will be cleared", ~table.Path.GetPath());
            auto req = TTableYPathProxy::Clear(path);
            SetTransactionId(req, OutputTransaction);
            NMetaState::GenerateRpcMutationId(req);
            batchReq->AddRequest(req, "clear_out");
        } else {
            // Even if |Clear| is False we still add a dummy request
            // to keep "clear_out" requests aligned with output tables.
            batchReq->AddRequest(NULL, "clear_out");
        }
        {
            auto req = TTableYPathProxy::GetChunkListForUpdate(path);
            SetTransactionId(req, OutputTransaction);
            batchReq->AddRequest(req, "get_out_chunk_list");
        }
    }

    FOREACH (const auto& file, Files) {
        auto path = file.Path.GetPath();
        {
            auto req = TFileYPathProxy::FetchFile(path);
            SetTransactionId(req, InputTransaction->GetId());
            batchReq->AddRequest(req, "fetch_files");
        }
    }

    RequestCustomInputs(batchReq);

    return batchReq->Invoke();
}

void TOperationControllerBase::OnInputsReceived(TObjectServiceProxy::TRspExecuteBatchPtr batchRsp)
{
    VERIFY_THREAD_AFFINITY(BackgroundThread);

    THROW_ERROR_EXCEPTION_IF_FAILED(*batchRsp, "Error requesting inputs");

    {
        auto fetchInRsps = batchRsp->GetResponses<TTableYPathProxy::TRspFetch>("fetch_in");
        auto lockInRsps = batchRsp->GetResponses<TCypressYPathProxy::TRspLock>("lock_in");
        auto getInAttributesRsps = batchRsp->GetResponses<TYPathProxy::TRspGet>("get_in_attributes");
        for (int index = 0; index < static_cast<int>(InputTables.size()); ++index) {
            auto& table = InputTables[index];
            {
                auto rsp = lockInRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error locking input table %s",
                    ~table.Path.GetPath());
                
                LOG_INFO("Input table %s locked",
                    ~table.Path.GetPath());
            }
            {
                auto rsp = fetchInRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error fetching input input table %s",
                    ~table.Path.GetPath());
                
                table.FetchResponse = rsp;
                FOREACH (const auto& chunk, rsp->chunks()) {
                    auto chunkId = TChunkId::FromProto(chunk.slice().chunk_id());
                    if (chunk.node_addresses_size() == 0) {
                        THROW_ERROR_EXCEPTION("Chunk %s in input table %s is lost",
                            ~chunkId.ToString(),
                            ~table.Path.GetPath());
                    }
                    InputChunkIds.insert(chunkId);
                }
                LOG_INFO("Input table %s has %d chunks",
                    ~table.Path.GetPath(),
                    rsp->chunks_size());
            }
            {
                auto rsp = getInAttributesRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error getting attributes for input table %s",
                    ~table.Path.GetPath());
                
                auto node = ConvertToNode(TYsonString(rsp->value()));
                const auto& attributes = node->Attributes();
                
                if (attributes.Get<bool>("sorted")) {
                    table.KeyColumns = attributes.Get< std::vector<Stroka> >("sorted_by");
                    LOG_INFO("Input table %s is sorted by %s",
                        ~table.Path.GetPath(),
                        ~ConvertToYsonString(table.KeyColumns.Get(), EYsonFormat::Text).Data());
                } else {
                    LOG_INFO("Input table %s is not sorted",
                        ~table.Path.GetPath());
                }
            }
        }
    }

    {
        auto lockOutRsps = batchRsp->GetResponses<TCypressYPathProxy::TRspLock>("lock_out");
        auto clearOutRsps = batchRsp->GetResponses<TTableYPathProxy::TRspClear>("clear_out");
        auto getOutChunkListRsps = batchRsp->GetResponses<TTableYPathProxy::TRspGetChunkListForUpdate>("get_out_chunk_list");
        auto getOutAttributesRsps = batchRsp->GetResponses<TYPathProxy::TRspGet>("get_out_attributes");
        for (int index = 0; index < static_cast<int>(OutputTables.size()); ++index) {
            auto& table = OutputTables[index];
            {
                auto rsp = lockOutRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error locking output table %s",
                    ~table.Path.GetPath());
                
                LOG_INFO("Output table %s locked",
                    ~table.Path.GetPath());
            }
            {
                auto rsp = getOutAttributesRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error getting attributes for output table %s",
                    ~table.Path.GetPath());
                
                auto node = ConvertToNode(TYsonString(rsp->value()));
                const auto& attributes = node->Attributes();
                
                table.Channels = attributes.GetYson("channels");
                LOG_INFO("Output table %s has channels %s",
                    ~table.Path.GetPath(),
                    ~ConvertToYsonString(table.Channels, EYsonFormat::Text).Data());
                
                i64 initialRowCount = attributes.Get<i64>("row_count");
                if (initialRowCount > 0 && table.Clear && !table.Overwrite) {
                    THROW_ERROR_EXCEPTION("Output table %s must be empty (use \"overwrite\" attribute to force clearing it)",
                        ~table.Path.GetPath());
                }

                table.ReplicationFactor = attributes.Get<int>("replication_factor");
            }
            if (table.Clear) {
                auto rsp = clearOutRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error clearing output table %s",
                    ~table.Path.GetPath());
                
                LOG_INFO("Output table %s cleared",
                    ~table.Path.GetPath());
            }
            {
                auto rsp = getOutChunkListRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error getting output chunk list for table %s",
                    ~table.Path.GetPath());
                
                table.OutputChunkListId = TChunkListId::FromProto(rsp->chunk_list_id());
                LOG_INFO("Output table %s has output chunk list %s",
                    ~table.Path.GetPath(),
                    ~table.OutputChunkListId.ToString());
            }
        }
    }

    {
        auto fetchFilesRsps = batchRsp->GetResponses<TFileYPathProxy::TRspFetchFile>("fetch_files");
        for (int index = 0; index < static_cast<int>(Files.size()); ++index) {
            auto& file = Files[index];
            {
                auto rsp = fetchFilesRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error fetching files");
                
                file.FetchResponse = rsp;
                LOG_INFO("File %s consists of chunk %s",
                    ~file.Path.GetPath(),
                    ~TChunkId::FromProto(rsp->chunk_id()).ToString());
            }
        }
    }

    OnCustomInputsRecieved(batchRsp);

    LOG_INFO("Inputs received");
}

void TOperationControllerBase::RequestCustomInputs(TObjectServiceProxy::TReqExecuteBatchPtr batchReq)
{
    UNUSED(batchReq);
}

void TOperationControllerBase::OnCustomInputsRecieved(TObjectServiceProxy::TRspExecuteBatchPtr batchRsp)
{
    UNUSED(batchRsp);
}

TFuture<void> TOperationControllerBase::CompletePreparation()
{
    VERIFY_THREAD_AFFINITY(BackgroundThread);

    FOREACH (const auto& table, InputTables) {
        FOREACH (const auto& chunk, table.FetchResponse->chunks()) {
            i64 chunkDataSize;
            i64 chunkRowCount;
            i64 chunkValueCount;
            NTableClient::GetStatistics(chunk, &chunkDataSize, &chunkRowCount, &chunkValueCount);

            TotalInputDataSize += chunkDataSize;
            TotalInputRowCount += chunkRowCount;
            TotalInputValueCount += chunkValueCount;
            ++TotalInputChunkCount;
        }
    }

    LOG_INFO("Input totals collected (ChunkCount: %d, DataSize: %" PRId64 ", RowCount: % " PRId64 ", ValueCount: %" PRId64 ")",
        TotalInputChunkCount,
        TotalInputDataSize,
        TotalInputRowCount,
        TotalInputValueCount);

    // Check for empty inputs.
    if (TotalInputChunkCount == 0) {
        LOG_INFO("Empty input");
        OnOperationCompleted();
        return NewPromise<void>();
    }

    ChunkListPool = New<TChunkListPool>(
        Config,
        Host->GetMasterChannel(),
        CancelableControlInvoker,
        Operation);

    return MakeFuture();
}

void TOperationControllerBase::OnPreparationCompleted()
{
    if (!Active)
        return;

    LOG_INFO("Preparation completed");
}

TAsyncPipeline<void>::TPtr TOperationControllerBase::CustomizePreparationPipeline(TAsyncPipeline<void>::TPtr pipeline)
{
    return pipeline;
}

void TOperationControllerBase::ReleaseChunkList(const TChunkListId& id)
{
    std::vector<TChunkListId> ids;
    ids.push_back(id);
    ReleaseChunkLists(ids);
}

void TOperationControllerBase::ReleaseChunkLists(const std::vector<TChunkListId>& ids)
{
    auto batchReq = ObjectProxy.ExecuteBatch();
    FOREACH (const auto& id, ids) {
        auto req = TTransactionYPathProxy::ReleaseObject();
        *req->mutable_object_id() = id.ToProto();
        NMetaState::GenerateRpcMutationId(req);
        batchReq->AddRequest(req);
    }

    // Fire-and-forget.
    // The subscriber is only needed to log the outcome.
    batchReq->Invoke().Subscribe(
        BIND(&TThis::OnChunkListsReleased, MakeStrong(this)));
}

void TOperationControllerBase::OnChunkListsReleased(TObjectServiceProxy::TRspExecuteBatchPtr batchRsp)
{
    if (!batchRsp->IsOK()) {
        LOG_WARNING(*batchRsp, "Error releasing chunk lists");
    }
}

std::vector<TRefCountedInputChunkPtr> TOperationControllerBase::CollectInputChunks()
{
    // TODO(babenko): set row_attributes
    std::vector<TRefCountedInputChunkPtr> result;
    for (int tableIndex = 0; tableIndex < InputTables.size(); ++tableIndex) {
        const auto& table = InputTables[tableIndex];
        FOREACH (const auto& inputChunk, table.FetchResponse->chunks()) {
            result.push_back(New<TRefCountedInputChunk>(inputChunk, tableIndex));
        }
    }
    return result;
}

std::vector<TChunkStripePtr> TOperationControllerBase::SliceInputChunks(
    TNullable<int> jobCount,
    i64 jobSliceDataSize)
{
    auto inputChunks = CollectInputChunks();

    i64 sliceDataSize =
        jobCount
        ? std::min(jobSliceDataSize, TotalInputDataSize / jobCount.Get() + 1)
        : jobSliceDataSize;

    YCHECK(sliceDataSize > 0);

    // Ensure that no input chunk has size larger than sliceSize.
    std::vector<TChunkStripePtr> stripes;
    FOREACH (auto inputChunk, inputChunks) {
        auto chunkId = TChunkId::FromProto(inputChunk->slice().chunk_id());
        
        i64 dataSize;
        GetStatistics(*inputChunk, &dataSize);

        if (dataSize > sliceDataSize) {
            int sliceCount = (int) std::ceil((double) dataSize / (double) sliceDataSize);
            auto slicedInputChunks = SliceChunkEvenly(inputChunk, sliceCount);
            FOREACH (auto slicedInputChunk, slicedInputChunks) {
                auto stripe = New<TChunkStripe>(slicedInputChunk);
                stripes.push_back(stripe);
            }
            LOG_TRACE("Slicing chunk (ChunkId: %s, SliceCount: %d)",
                ~chunkId.ToString(),
                sliceCount);
        } else {
            auto stripe = New<TChunkStripe>(inputChunk);
            stripes.push_back(stripe);
            LOG_TRACE("Taking whole chunk (ChunkId: %s)",
                ~chunkId.ToString());
        }
    }


    LOG_DEBUG("Sliced chunks prepared (InputChunkCount: %d, SlicedChunkCount: %d, JobCount: %s, JobSliceDataSize: %" PRId64 ", SliceDataSize: %" PRId64 ")",
        static_cast<int>(inputChunks.size()),
        static_cast<int>(stripes.size()),
        ~ToString(jobCount),
        jobSliceDataSize,
        sliceDataSize);

    return stripes;
}

std::vector<Stroka> TOperationControllerBase::CheckInputTablesSorted(const TNullable< std::vector<Stroka> >& keyColumns)
{
    YCHECK(!InputTables.empty());

    FOREACH (const auto& table, InputTables) {
        if (!table.KeyColumns) {
            THROW_ERROR_EXCEPTION("Input table %s is not sorted",
                ~table.Path.GetPath());
        }
    }

    if (keyColumns) {
        FOREACH (const auto& table, InputTables) {
            if (!CheckKeyColumnsCompatible(table.KeyColumns.Get(), keyColumns.Get())) {
                THROW_ERROR_EXCEPTION("Input table %s is sorted by columns %s that are not compatible with the requested columns %s",
                    ~table.Path.GetPath(),
                    ~ConvertToYsonString(table.KeyColumns.Get(), EYsonFormat::Text).Data(),
                    ~ConvertToYsonString(keyColumns.Get(), EYsonFormat::Text).Data());
            }
        }
        return keyColumns.Get();
    } else {
        const auto& referenceTable = InputTables[0];
        FOREACH (const auto& table, InputTables) {
            if (table.KeyColumns != referenceTable.KeyColumns) {
                THROW_ERROR_EXCEPTION("Key columns do not match: input table %s is sorted by columns %s while input table %s is sorted by columns %s",
                    ~table.Path.GetPath(),
                    ~ConvertToYsonString(table.KeyColumns.Get(), EYsonFormat::Text).Data(),
                    ~referenceTable.Path.GetPath(),
                    ~ConvertToYsonString(referenceTable.KeyColumns.Get(), EYsonFormat::Text).Data());
            }
        }
        return referenceTable.KeyColumns.Get();
    }
}

bool TOperationControllerBase::CheckKeyColumnsCompatible(
    const std::vector<Stroka>& fullColumns,
    const std::vector<Stroka>& prefixColumns)
{
    if (fullColumns.size() < prefixColumns.size()) {
        return false;
    }

    for (int index = 0; index < static_cast<int>(prefixColumns.size()); ++index) {
        if (fullColumns[index] != prefixColumns[index]) {
            return false;
        }
    }

    return true;
}

void TOperationControllerBase::RegisterOutputChunkTree(
    const NChunkServer::TChunkTreeId& chunkTreeId,
    int key,
    int tableIndex)
{
    auto& table = OutputTables[tableIndex];
    table.OutputChunkTreeIds.insert(std::make_pair(key, chunkTreeId));

    LOG_DEBUG("Output chunk tree registered (Table: %d, ChunkTreeId: %s, Key: %d)",
        tableIndex,
        ~chunkTreeId.ToString(),
        key);
}

void TOperationControllerBase::RegisterOutputChunkTrees(
    TJobletPtr joblet,
    int key)
{
    for (int tableIndex = 0; tableIndex < static_cast<int>(OutputTables.size()); ++tableIndex) {
        RegisterOutputChunkTree(joblet->ChunkListIds[tableIndex], key, tableIndex);
    }
}

TChunkStripePtr TOperationControllerBase::BuildIntermediateChunkStripe(
    google::protobuf::RepeatedPtrField<NTableClient::NProto::TInputChunk>* inputChunks)
{
    auto stripe = New<TChunkStripe>();
    FOREACH (auto& inputChunk, *inputChunks) {
        stripe->Chunks.push_back(New<TRefCountedInputChunk>(MoveRV(inputChunk)));
    }
    return stripe;
}

bool TOperationControllerBase::HasEnoughChunkLists(int requestedCount)
{
    return ChunkListPool->HasEnough(requestedCount);
}

TChunkListId TOperationControllerBase::ExtractChunkList()
{
    return ChunkListPool->Extract();
}

void TOperationControllerBase::RegisterJobInProgress(TJobletPtr joblet)
{
    YCHECK(JobsInProgress.insert(MakePair(joblet->Job, joblet)).second);
}

TOperationControllerBase::TJobletPtr TOperationControllerBase::GetJobInProgress(TJobPtr job)
{
    auto it = JobsInProgress.find(job);
    YCHECK(it != JobsInProgress.end());
    return it->second;
}

void TOperationControllerBase::RemoveJobInProgress(TJobPtr job)
{
    YCHECK(JobsInProgress.erase(job) == 1);
}

void TOperationControllerBase::BuildProgressYson(IYsonConsumer* consumer)
{
    BuildYsonMapFluently(consumer)
        .Item("jobs").BeginMap()
            .Item("total").Scalar(JobCounter.GetCompleted() + JobCounter.GetRunning() + GetPendingJobCount())
            .Item("pending").Scalar(GetPendingJobCount())
            .Item("running").Scalar(JobCounter.GetRunning())
            .Item("completed").Scalar(JobCounter.GetCompleted())
            .Item("failed").Scalar(JobCounter.GetFailed())
            .Item("aborted").Scalar(JobCounter.GetAborted())
            .Item("lost").Scalar(JobCounter.GetLost())
        .EndMap();
}

void TOperationControllerBase::BuildResultYson(IYsonConsumer* consumer)
{
    auto error = FromProto(Operation->Result().error());
    BuildYsonFluently(consumer)
        .BeginMap()
            .Item("error").Scalar(error)
        .EndMap();
}

std::vector<TRichYPath> TOperationControllerBase::GetFilePaths() const
{
    return std::vector<TRichYPath>();
}

int TOperationControllerBase::SuggestJobCount(
    i64 totalDataSize,
    i64 minDataSizePerJob,
    i64 maxDataSizePerJob,
    TNullable<int> configJobCount,
    int chunkCount)
{
    int minSuggestion = static_cast<int>(std::ceil((double) totalDataSize / maxDataSizePerJob));
    int maxSuggestion = static_cast<int>(std::ceil((double) totalDataSize / minDataSizePerJob));
    int result = configJobCount.Get(minSuggestion);
    result = std::min(result, chunkCount);
    result = std::min(result, maxSuggestion);
    result = std::max(result, 1);
    result = std::min(result, Config->MaxJobCount);
    return result;
}

void TOperationControllerBase::InitUserJobSpec(
    NScheduler::NProto::TUserJobSpec* proto,
    TUserJobSpecPtr config,
    const std::vector<TUserFile>& files)
{
    proto->set_shell_command(config->Command);

    {
        // Set input and output format.
        TFormat inputFormat(EFormatType::Yson);
        TFormat outputFormat(EFormatType::Yson);

        if (config->Format) {
            inputFormat = outputFormat = config->Format.Get();
        }

        if (config->InputFormat) {
            inputFormat = config->InputFormat.Get();
        }

        if (config->OutputFormat) {
            outputFormat = config->OutputFormat.Get();
        }

        proto->set_input_format(ConvertToYsonString(inputFormat).Data());
        proto->set_output_format(ConvertToYsonString(outputFormat).Data());
    }

    auto fillEnvironment = [&] (yhash_map<Stroka, Stroka>& env) {
        FOREACH(const auto& pair, env) {
            proto->add_environment(Sprintf("%s=%s", ~pair.first, ~pair.second));
        }
    };

    // Global environment.
    fillEnvironment(Config->Environment);

    // Local environment.
    fillEnvironment(config->Environment);

    proto->add_environment(Sprintf("YT_OPERATION_ID=%s", 
        ~Operation->GetOperationId().ToString()));

    // TODO(babenko): think about per-job files
    FOREACH (const auto& file, files) {
        *proto->add_files() = *file.FetchResponse;
    }
}

void TOperationControllerBase::AddUserJobEnvironment(
    NScheduler::NProto::TUserJobSpec* proto,
    TJobletPtr joblet)
{
    proto->add_environment(Sprintf("YT_JOB_INDEX=%d", joblet->JobIndex));
    proto->add_environment(Sprintf("YT_JOB_ID=%s", ~joblet->Job->GetId().ToString()));
    if (joblet->StartRowIndex >= 0) {
        proto->add_environment(Sprintf("YT_START_ROW_INDEX=%" PRId64, joblet->StartRowIndex));
    }
}

void TOperationControllerBase::InitIntermediateInputConfig(TJobIOConfigPtr config)
{
    // Disable master requests.
    config->TableReader->AllowFetchingSeedsFromMaster = false;
}

void TOperationControllerBase::InitIntermediateOutputConfig(TJobIOConfigPtr config)
{
    // Don't replicate intermediate output.
    config->TableWriter->ReplicationFactor = 1;
    config->TableWriter->UploadReplicationFactor = 1;

    // Cache blocks on nodes.
    config->TableWriter->EnableNodeCaching = true;

    // Don't move intermediate chunks.
    config->TableWriter->ChunksMovable = false;
    config->TableWriter->ChunksVital = false;
}


void TOperationControllerBase::InitFinalOutputConfig(TJobIOConfigPtr config)
{
    UNUSED(config);
}

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

