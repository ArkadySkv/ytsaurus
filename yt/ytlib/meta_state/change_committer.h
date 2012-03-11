#pragma once

#include "public.h"
#include "meta_version.h"
#include "meta_state_manager_proxy.h"

#include <ytlib/election/election_manager.h>
#include <ytlib/misc/thread_affinity.h>
#include <ytlib/actions/signal.h>
#include <ytlib/profiling/profiler.h>

namespace NYT {
namespace NMetaState {

////////////////////////////////////////////////////////////////////////////////

//! A common base for TFollowerCommitter and TLeaderCommitter.
class TCommitter
    : public TRefCounted
{
public:
    TCommitter(
        TDecoratedMetaState* metaState,
        IInvoker* epochControlInvoker,
        IInvoker* epochStateInvoker);

    DECLARE_ENUM(EResult,
        (Committed)
        (MaybeCommitted)
        (LateChanges)
        (OutOfOrderChanges)
    );
    typedef TFuture<EResult> TResult;

protected:
    DECLARE_THREAD_AFFINITY_SLOT(ControlThread);
    DECLARE_THREAD_AFFINITY_SLOT(StateThread);

    TDecoratedMetaStatePtr MetaState;
    IInvoker::TPtr EpochControlInvoker;
    IInvoker::TPtr EpochStateInvoker;
    NProfiling::TRateCounter CommitCounter;
    NProfiling::TRateCounter BatchCommitCounter;

};

////////////////////////////////////////////////////////////////////////////////

//! Manages commits carried out by a leader.
class TLeaderCommitter
    : public TCommitter
{
public:
    typedef TIntrusivePtr<TLeaderCommitter> TPtr;

    //! Creates an instance.
    TLeaderCommitter(
        TLeaderCommitterConfig* config,
        TCellManager* cellManager,
        TDecoratedMetaState* metaState,
        TChangeLogCache* changeLogCache,
        TFollowerTracker* followerTracker,
        const TEpoch& epoch,
        IInvoker* epochControlInvoker,
        IInvoker* epochStateInvoker);

    //! Initializes the instance.
    /*!
     *  \note Thread affinity: ControlThread
     */
    void Start();

    //! Releases all resources.
    /*!
     *  \note Thread affinity: ControlThread
     */
    void Stop();

    //! Returns the version to be sent to followers in a ping.
    /*!
     *  During recovery this is equal to the reachable version.
     *  After recovery this is equal to the version resulting from applying all
     *  changes in the latest batch.
     *  
     *  \note Thread affinity: ControlThread
     */
    TMetaVersion GetFollowerPingVersion() const;

    //! Initiates a new distributed commit.
    /*!
     *  \param changeAction An action that will be called in the context of
     *  the state thread and will update the state.
     *  \param changeData A serialized representation of the change that
     *  will be sent down to follower.
     *  \return An asynchronous flag indicating the outcome of the distributed commit.
     *  
     *  The current implementation regards a distributed commit as completed when the update is
     *  received, applied, and flushed to the changelog by a quorum of replicas.
     *  
     *  \note Thread affinity: StateThread
     */
    TResult::TPtr Commit(
        IAction::TPtr changeAction,
        const TSharedRef& changeData);

    //! Force to send all pending changes.
    /*!
     *  \param rotateChangeLog True iff the changelog will be rotated immediately.
     *  \note Thread affinity: StateThread
     */
    void Flush(bool rotateChangeLog);

    //! Raised in the state thread each time a change is applied locally.
    DEFINE_SIGNAL(void(), ChangeApplied);

private:
    class TBatch;
    typedef TIntrusivePtr<TBatch> TBatchPtr;

    typedef TMetaStateManagerProxy TProxy;

    void OnBatchTimeout(TBatchPtr batch);
    TIntrusivePtr<TBatch> GetOrCreateBatch(const TMetaVersion& version);
    TResult::TPtr BatchChange(
        const TMetaVersion& version,
        const TSharedRef& changeData,
        TFuture<TVoid>::TPtr changeLogResult);
    void FlushCurrentBatch(bool rotateChangeLog);

    TLeaderCommitterConfigPtr Config;
    TCellManagerPtr CellManager;
    TChangeLogCachePtr ChangeLogCache;
    TFollowerTrackerPtr FollowerTracker;
    TEpoch Epoch;

    TMetaVersion FollowerPingVersion;

    //! Protects the rest.
    TSpinLock BatchSpinLock;
    TBatchPtr CurrentBatch;
    TDelayedInvoker::TCookie BatchTimeoutCookie;
};

////////////////////////////////////////////////////////////////////////////////

//! Manages commits carried out by a follower.
class TFollowerCommitter
    : public TCommitter
{
public:
    typedef TIntrusivePtr<TFollowerCommitter> TPtr;

    //! Creates an instance.
    TFollowerCommitter(
        TDecoratedMetaState* metaState,
        IInvoker* epochControlInvoker,
        IInvoker* epochStateInvoker);

    //! Commits a bunch of changes at a follower.
    /*!
     *  \param expectedVersion A version that the state is currently expected to have.
     *  \param changes A bunch of serialized changes to apply.
     *  \return An asynchronous flag indicating the outcome of the local commit.
     *  
     *  The current implementation regards a local commit as completed when the update is
     *  flushed to the local changelog.
     *  
     *  \note Thread affinity: ControlThread
     */
    TResult::TPtr Commit(
        const TMetaVersion& expectedVersion,
        const std::vector<TSharedRef>& changes);

private:
    TResult::TPtr DoCommit(
        const TMetaVersion& expectedVersion,
        const std::vector<TSharedRef>& changes);

};

////////////////////////////////////////////////////////////////////////////////


} // namespace NMetaState
} // namespace NYT
