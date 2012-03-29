#include "stdafx.h"
#include "change_committer.h"
#include "meta_version.h"
#include "decorated_meta_state.h"
#include "change_log_cache.h"
#include "follower_tracker.h"

#include <ytlib/misc/serialize.h>
#include <ytlib/misc/foreach.h>
#include <ytlib/logging/tagged_logger.h>

namespace NYT {
namespace NMetaState {

using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger Logger("MetaState");
static NProfiling::TProfiler Profiler("/meta_state");

////////////////////////////////////////////////////////////////////////////////

TCommitter::TCommitter(
    TDecoratedMetaState* metaState,
    IInvoker* epochControlInvoker,
    IInvoker* epochStateInvoker)
    : MetaState(metaState)
    , EpochControlInvoker(epochControlInvoker)
    , EpochStateInvoker(epochStateInvoker)
    , CommitCounter("commit_rate")
    , BatchCommitCounter("commit_batch_rate")
    , CommitTimeCounter("commit_time")
{
    YASSERT(metaState);
    YASSERT(epochControlInvoker);
    YASSERT(epochStateInvoker);
    VERIFY_INVOKER_AFFINITY(epochControlInvoker, ControlThread);
    VERIFY_INVOKER_AFFINITY(epochStateInvoker, StateThread);
}

////////////////////////////////////////////////////////////////////////////////

class TLeaderCommitter::TBatch
    : public TRefCounted
{
public:
    TBatch(
        TLeaderCommitterPtr committer,
        const TMetaVersion& startVersion)
        : Committer(committer)
        , Result(New<TResult>())
        , StartVersion(startVersion)
        // The local commit is also counted.
        , CommitCount(0)
        , IsSent(false)
        , Logger(MetaStateLogger)
    {
        Logger.AddTag(Sprintf("StartVersion: %s", ~StartVersion.ToString()));
    }

    TResult::TPtr AddChange(const TSharedRef& changeData)
    {
        VERIFY_THREAD_AFFINITY(Committer->StateThread);
        YASSERT(!IsSent);

        TMetaVersion currentVersion(
            StartVersion.SegmentId,
            StartVersion.RecordCount + BatchedChanges.size());
        BatchedChanges.push_back(changeData);

        LOG_DEBUG("Change is added to batch (Version: %s)", ~currentVersion.ToString());

        return Result;
    }

    void SetLastChangeLogResult(TFuture<TVoid>::TPtr result)
    {
        LogResult = result;
    }

    void FlushChanges(bool rotateChangeLog)
    {
        Logger.AddTag(Sprintf("ChangeCount: %d", static_cast<int>(BatchedChanges.size())));
        Committer->EpochControlInvoker->Invoke(FromMethod(
            &TBatch::DoFlushChanges,
            MakeStrong(this),
            rotateChangeLog));
    }

    int GetChangeCount() const
    {
        VERIFY_THREAD_AFFINITY(Committer->StateThread);
        YASSERT(!IsSent);

        return static_cast<int>(BatchedChanges.size());
    }

private:
    void DoFlushChanges(bool rotateChangeLog)
    {
        VERIFY_THREAD_AFFINITY(Committer->ControlThread);

        IsSent = true;

        if (!BatchedChanges.empty()) {
            Profiler.Enqueue("commit_batch_size", BatchedChanges.size());


            YASSERT(LogResult);
            auto cellManager = Committer->CellManager;

            Awaiter = New<TParallelAwaiter>(
                ~Committer->EpochControlInvoker,
                &Profiler,
                "commit_batch_time");

            Awaiter->Await(
                LogResult,
                EscapeYPath(cellManager->SelfAddress()),
                FromMethod(&TBatch::OnLocalCommit, MakeStrong(this)));

            LOG_DEBUG("Sending batched changes to followers");
            for (TPeerId id = 0; id < cellManager->GetPeerCount(); ++id) {
                if (id == cellManager->SelfId()) continue;

                LOG_DEBUG("Sending changes to follower %d", id);

                auto request =
                    cellManager->GetMasterProxy<TProxy>(id)
                    ->ApplyChanges()
                    ->SetTimeout(Committer->Config->RpcTimeout);
                request->set_segment_id(StartVersion.SegmentId);
                request->set_record_count(StartVersion.RecordCount);
                request->set_epoch(Committer->Epoch.ToProto());
                FOREACH (const auto& change, BatchedChanges) {
                    request->Attachments().push_back(change);
                }
                Awaiter->Await(
                    request->Invoke(),
                    EscapeYPath(cellManager->GetPeerAddress(id)),
                    FromMethod(&TBatch::OnRemoteCommit, MakeStrong(this), id));
            }
            LOG_DEBUG("Batched changes sent");

            Awaiter->Complete(FromMethod(&TBatch::OnCompleted, MakeStrong(this)));

        }
        
        // This is the version the next batch will have.
        Committer->MetaState->SetPingVersion(
            rotateChangeLog
            ? TMetaVersion(StartVersion.SegmentId + 1, 0)
            : TMetaVersion(StartVersion.SegmentId, StartVersion.RecordCount + BatchedChanges.size()));
    }

    bool CheckCommitQuorum()
    {
        VERIFY_THREAD_AFFINITY(Committer->ControlThread);

        if (CommitCount < Committer->CellManager->GetQuorum())
            return false;

        Result->Set(EResult::Committed);
        Awaiter->Cancel();
        
        LOG_DEBUG("Changes are committed by quorum");

        return true;
    }

    void OnRemoteCommit(TProxy::TRspApplyChanges::TPtr response, TPeerId peerId)
    {
        VERIFY_THREAD_AFFINITY(Committer->ControlThread);

        if (!response->IsOK()) {
            LOG_WARNING("Error committing changes by follower %d\n%s",
                peerId,
                ~response->GetError().ToString());
            return;
        }

        if (response->committed()) {
            LOG_DEBUG("Changes are committed by follower %d", peerId);

            ++CommitCount;
            CheckCommitQuorum();
        } else {
            LOG_DEBUG("Changes are acknowledged by follower %d", peerId);
        }
    }
    
    void OnLocalCommit(TVoid)
    {
        VERIFY_THREAD_AFFINITY(Committer->ControlThread);

        LOG_DEBUG("Changes are committed locally");
        ++CommitCount;
        CheckCommitQuorum();
    }

    void OnCompleted()
    {
        VERIFY_THREAD_AFFINITY(Committer->ControlThread);

        if (CheckCommitQuorum())
            return;

        LOG_WARNING("Changes are uncertain (CommitCount: %d)", CommitCount);

        Result->Set(EResult::MaybeCommitted);
    }

    TLeaderCommitterPtr Committer;
    TResult::TPtr Result;
    TMetaVersion StartVersion;
    int CommitCount;
    volatile bool IsSent;
    NLog::TTaggedLogger Logger;

    TParallelAwaiter::TPtr Awaiter;
    TFuture<TVoid>::TPtr LogResult;
    std::vector<TSharedRef> BatchedChanges;

};

////////////////////////////////////////////////////////////////////////////////

TLeaderCommitter::TLeaderCommitter(
    TLeaderCommitterConfig* config,
    TCellManager* cellManager,
    TDecoratedMetaState* decoratedState,
    TChangeLogCache* changeLogCache,
    TFollowerTracker* followerTracker,
    const TEpoch& epoch,
    IInvoker* epochControlInvoker,
    IInvoker* epochStateInvoker)
    : TCommitter(decoratedState, epochControlInvoker, epochStateInvoker)
    , Config(config)
    , CellManager(cellManager)
    , ChangeLogCache(changeLogCache)
    , FollowerTracker(followerTracker)
    , Epoch(epoch)
{
    YASSERT(config);
    YASSERT(cellManager);
    YASSERT(changeLogCache);
    YASSERT(followerTracker);
}

void TLeaderCommitter::Start()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    // Do nothing.
}

void TLeaderCommitter::Stop()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    // Kill the cyclic reference.
    TGuard<TSpinLock> guard(BatchSpinLock);
    CurrentBatch.Reset();
    TDelayedInvoker::CancelAndClear(BatchTimeoutCookie);
}

void TLeaderCommitter::Flush(bool rotateChangeLog)
{
    VERIFY_THREAD_AFFINITY(StateThread);

    TGuard<TSpinLock> guard(BatchSpinLock);
    // If no current batch exists but the changelog is about to be rotated
    // we have to create a dummy batch and flush it to update ping version.
    if (rotateChangeLog && !CurrentBatch) {
        auto version = MetaState->GetVersion();
        GetOrCreateBatch(version);
    }
    if (CurrentBatch) {
        FlushCurrentBatch(rotateChangeLog);
    }
}

TLeaderCommitter::TResult::TPtr TLeaderCommitter::Commit(
    IAction::TPtr changeAction,
    const TSharedRef& changeData)
{
    VERIFY_THREAD_AFFINITY(StateThread);
    YASSERT(changeAction);

    PROFILE_AGGREGATED_TIMING (CommitTimeCounter) {
        auto version = MetaState->GetVersion();
        LOG_DEBUG("Starting commit at version %s", ~version.ToString());

        auto logResult = MetaState->LogChange(version, changeData);
        auto batchResult = BatchChange(version, changeData, logResult);

        MetaState->ApplyChange(changeAction);

        LOG_DEBUG("Change is applied locally at version %s", ~version.ToString());

        ChangeApplied_.Fire();

        Profiler.Increment(CommitCounter);

        return batchResult;
    }
}

TLeaderCommitter::TResult::TPtr TLeaderCommitter::BatchChange(
    const TMetaVersion& version,
    const TSharedRef& changeData,
    TFuture<TVoid>::TPtr changeLogResult)
{
    TGuard<TSpinLock> guard(BatchSpinLock);
    auto batch = GetOrCreateBatch(version);
    auto result = batch->AddChange(changeData);
    batch->SetLastChangeLogResult(changeLogResult);
    if (batch->GetChangeCount() >= Config->MaxBatchSize) {
        FlushCurrentBatch(false);
    }
    return result;
}

void TLeaderCommitter::FlushCurrentBatch(bool rotateChangeLog)
{
    VERIFY_SPINLOCK_AFFINITY(BatchSpinLock);
    YASSERT(CurrentBatch);

    CurrentBatch->FlushChanges(rotateChangeLog);
    TDelayedInvoker::CancelAndClear(BatchTimeoutCookie);
    CurrentBatch.Reset();
    Profiler.Increment(BatchCommitCounter);
}

TLeaderCommitter::TBatchPtr TLeaderCommitter::GetOrCreateBatch(
    const TMetaVersion& version)
{
    VERIFY_THREAD_AFFINITY(StateThread);
    VERIFY_SPINLOCK_AFFINITY(BatchSpinLock);

    if (!CurrentBatch) {
        YASSERT(!BatchTimeoutCookie);
        CurrentBatch = New<TBatch>(MakeStrong(this), version);
        BatchTimeoutCookie = TDelayedInvoker::Submit(
            FromMethod(
                &TLeaderCommitter::OnBatchTimeout,
                MakeStrong(this),
                CurrentBatch)
            ->Via(~EpochControlInvoker),
            Config->MaxBatchDelay);
    }

    return CurrentBatch;
}

void TLeaderCommitter::OnBatchTimeout(TBatchPtr batch)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    TGuard<TSpinLock> guard(BatchSpinLock);
    if (batch != CurrentBatch)
        return;

    LOG_DEBUG("Flushing batched changes");

    FlushCurrentBatch(false);
}

////////////////////////////////////////////////////////////////////////////////

TFollowerCommitter::TFollowerCommitter(
    TDecoratedMetaState* metaState,
    IInvoker* epochControlInvoker,
    IInvoker* epochStateInvoker)
    : TCommitter(metaState, epochControlInvoker, epochStateInvoker)
{ }

TCommitter::TResult::TPtr TFollowerCommitter::Commit(
    const TMetaVersion& expectedVersion,
    const std::vector<TSharedRef>& changes)
{
    VERIFY_THREAD_AFFINITY(ControlThread);
    YASSERT(!changes.empty());

    PROFILE_AGGREGATED_TIMING (CommitTimeCounter) {
        Profiler.Increment(CommitCounter, changes.size());
        Profiler.Increment(BatchCommitCounter);

        return
            FromMethod(
                &TFollowerCommitter::DoCommit,
                MakeStrong(this),
                expectedVersion,
                changes)
            ->AsyncVia(EpochStateInvoker)
            ->Do();
    }
}

TCommitter::TResult::TPtr TFollowerCommitter::DoCommit(
    const TMetaVersion& expectedVersion,
    const std::vector<TSharedRef>& changes)
{
    VERIFY_THREAD_AFFINITY(StateThread);

    auto currentVersion = MetaState->GetVersion();
    if (currentVersion > expectedVersion) {
        LOG_WARNING("Late changes received by follower, ignored: expected %s but got %s",
            ~currentVersion.ToString(),
            ~expectedVersion.ToString());
        return New<TResult>(EResult::LateChanges);
    }

    if (currentVersion != expectedVersion) {
        LOG_WARNING("Out-of-order changes received by follower, restarting: expected %s but got %s",
            ~currentVersion.ToString(),
            ~expectedVersion.ToString());
        return New<TResult>(EResult::OutOfOrderChanges);
    }

    LOG_DEBUG("Applying %d changes at version %s",
        static_cast<int>(changes.size()),
        ~currentVersion.ToString());

    TAsyncChangeLog::TAppendResult::TPtr result;
    FOREACH (const auto& change, changes) {
        result = MetaState->LogChange(currentVersion, change);
        MetaState->ApplyChange(change);
        ++currentVersion.RecordCount;
    }

    return result->Apply(FromFunctor([] (TVoid) -> TCommitter::EResult {
        return TCommitter::EResult::Committed;
    }));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NMetaState
} // namespace NYT
