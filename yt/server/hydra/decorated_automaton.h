#pragma once

#include "private.h"
#include "mutation_context.h"

#include <core/misc/ref.h>
#include <core/misc/ring_queue.h>

#include <core/concurrency/thread_affinity.h>

#include <core/actions/invoker.h>

#include <core/rpc/public.h>

#include <core/logging/tagged_logger.h>

#include <core/profiling/profiler.h>

#include <ytlib/hydra/version.h>
#include <ytlib/hydra/hydra_manager.pb.h>

#include <server/election/election_manager.h>

#include <atomic>

namespace NYT {
namespace NHydra {

////////////////////////////////////////////////////////////////////////////////

struct TEpochContext
    : public NElection::TEpochContext
{
    IInvokerPtr EpochSystemAutomatonInvoker;
    IInvokerPtr EpochUserAutomatonInvoker;
    IInvokerPtr EpochControlInvoker;
    TChangelogRotationPtr ChangelogRotation;
    TLeaderRecoveryPtr LeaderRecovery;
    TFollowerRecoveryPtr FollowerRecovery;
    TLeaderCommitterPtr LeaderCommitter;
    TFollowerCommitterPtr FollowerCommitter;
    TFollowerTrackerPtr FollowerTracker;
    bool IsActiveLeader = false;
};

DEFINE_REFCOUNTED_TYPE(TEpochContext)

////////////////////////////////////////////////////////////////////////////////

class TDecoratedAutomaton
    : public TRefCounted
{
public:
    TDecoratedAutomaton(
        TDistributedHydraManagerConfigPtr config,
        NElection::TCellManagerPtr cellManager,
        IAutomatonPtr automaton,
        IInvokerPtr automatonInvoker,
        IInvokerPtr controlInvoker,
        ISnapshotStorePtr snapshotStore,
        IChangelogStorePtr changelogStore,
        NProfiling::TProfiler profiler);

    void OnStartLeading();
    void OnLeaderRecoveryComplete();
    void OnStopLeading();
    void OnStartFollowing();
    void OnFollowerRecoveryComplete();
    void OnStopFollowing();

    DEFINE_BYVAL_RO_PROPERTY(EPeerState, State);

    IInvokerPtr CreateGuardedUserInvoker(IInvokerPtr underlyingInvoker);
    IInvokerPtr GetSystemInvoker();

    TVersion GetLoggedVersion() const;
    void SetLoggedVersion(TVersion version);

    void SetChangelog(IChangelogPtr changelog);

    i64 GetLoggedDataSize() const;

    TVersion GetAutomatonVersion() const;

    IAutomatonPtr GetAutomaton();

    void Clear();
    void LoadSnapshot(int snapshotId, TInputStream* input);

    void ApplyMutationDuringRecovery(const TSharedRef& recordData);
    void RotateChangelogDuringRecovery();

    void LogMutationAtLeader(
        const TMutationRequest& request,
        TSharedRef* recordData,
        TAsyncError* logResult,
        TPromise<TErrorOr<TMutationResponse>> commitResult);

    void LogMutationAtFollower(
        const TSharedRef& recordData,
        TAsyncError* logResult);

    TFuture<TErrorOr<TRemoteSnapshotParams>> BuildSnapshot();

    TAsyncError RotateChangelog(TEpochContextPtr epochContext);

    void CommitMutations(TVersion version);

    void RegisterKeptResponse(
        const TMutationId& mutationId,
        const TMutationResponse& response);
    TNullable<TMutationResponse> FindKeptResponse(const TMutationId& mutationId);

    TMutationContext* GetMutationContext();

private:
    class TUserLockGuard;
    class TSystemLockGuard;
    class TGuardedUserInvoker;
    class TSystemInvoker;
    class TSnapshotBuilder;

    TDistributedHydraManagerConfigPtr Config_;
    NElection::TCellManagerPtr CellManager_;
    IAutomatonPtr Automaton_;

    IInvokerPtr AutomatonInvoker_;
    IInvokerPtr ControlInvoker_;

    std::atomic<int> UserLock_;
    std::atomic<int> SystemLock_;
    IInvokerPtr SystemInvoker_;

    ISnapshotStorePtr SnapshotStore_;
    IChangelogStorePtr ChangelogStore_;

    NRpc::TResponseKeeperPtr ResponseKeeper_;

    TEpochId Epoch_;
    TMutationContext* MutationContext_;
    IChangelogPtr Changelog_;

    TSpinLock VersionSpinLock_;
    TVersion LoggedVersion_;
    TVersion AutomatonVersion_;

    TVersion SnapshotVersion_;
    TPromise<TErrorOr<TRemoteSnapshotParams>> SnapshotParamsPromise_;

    struct TPendingMutation
    {
        TVersion Version;
        TMutationRequest Request;
        TInstant Timestamp;
        ui64 RandomSeed;
        TPromise<TErrorOr<TMutationResponse>> CommitPromise;
    };

    NProto::TMutationHeader MutationHeader_; // pooled instance
    TRingQueue<TPendingMutation> PendingMutations_;

    NProfiling::TAggregateCounter BatchCommitTimeCounter_;

    NLog::TTaggedLogger Logger;
    NProfiling::TProfiler Profiler;


    void DoApplyMutation(const TSharedRef& recordData);
    void DoApplyMutation(TMutationContext* context);

    bool TryAcquireUserLock();
    void ReleaseUserLock();
    void AcquireSystemLock();
    void ReleaseSystemLock();

    void Reset();

    void DoRotateChangelog();

    void SaveSnapshot(TOutputStream* output);
    void MaybeStartSnapshotBuilder();


    DECLARE_THREAD_AFFINITY_SLOT(AutomatonThread);
    DECLARE_THREAD_AFFINITY_SLOT(ControlThread);

};

DEFINE_REFCOUNTED_TYPE(TDecoratedAutomaton)

////////////////////////////////////////////////////////////////////////////////

} // namespace NHydra
} // namespace NYT
