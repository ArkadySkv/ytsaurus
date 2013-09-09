#pragma once

#include "private.h"
#include "meta_version.h"

#include <core/misc/ref.h>

#include <core/concurrency/thread_affinity.h>

#include <core/actions/invoker.h>

namespace NYT {
namespace NMetaState {

////////////////////////////////////////////////////////////////////////////////

class TDecoratedMetaState
    : public TRefCounted
{
public:
    TDecoratedMetaState(
        TPersistentStateManagerConfigPtr config,
        IMetaStatePtr state,
        IInvokerPtr stateInvoker,
        IInvokerPtr controlInvoker,
        TSnapshotStorePtr snapshotStore,
        TChangeLogCachePtr changeLogCache);

    //! Initializes the instance.
    void Start();

    void OnStartLeading();
    void OnLeaderRecoveryComplete();
    void OnStopLeading();
    void OnStartFollowing();
    void OnFollowerRecoveryComplete();
    void OnStopFollowing();


    DEFINE_BYVAL_RO_PROPERTY(EPeerStatus, Status);

    //! Returns the wrapper invoker used for updating the state.
    /*!
     *  \note Thread affinity: any
     */
    IInvokerPtr CreateGuardedUserInvoker(IInvokerPtr underlyingInvoker);

    //! Returns the invoker used for performing recovery actions.
    /*!
     *  \note Thread affinity: any
     */
    IInvokerPtr GetSystemInvoker();

    //! Returns the current version of the state.
    /*!
     * \note Thread affinity: StateThread
     */
    TMetaVersion GetVersion() const;

    //! Same as #GetVersion but can be called from an arbitrary thread.
    /*!
     * \note Thread affinity: any
     */
    TMetaVersion GetVersionAsync() const;

    //! Returns the maximum reachable version of the state that
    //! can be obtained by reading the local snapshots, changelogs.
    /*!
     *  It is always no smaller than #GetVersion.
     *
     *  \note Thread affinity: any
     */
    TMetaVersion GetReachableVersionAsync() const;

    //! Returns the version that is sent to followers via pings.
    /*!
     *  During recovery this is equal to the reachable version.
     *  After recovery this is equal to the version resulting from applying all
     *  mutations in the latest batch.
     *
     *  \note Thread affinity: ControlThread
     */
    TMetaVersion GetPingVersion() const;

    //! Updates the ping version.
    /*!
     *  \note Thread affinity: ControlThread
     */
    void SetPingVersion(const TMetaVersion& version);

    //! Returns the underlying state.
    /*!
     * \note Thread affinity: any
     */
    IMetaStatePtr GetState();

    //! Delegates the call to IMetaState::Clear.
    /*!
     * \note Thread affinity: StateThread
     */
    void Clear();

    //! Delegates the call to IMetaState::Save.
    /*!
     * \note Thread affinity: StateThread
     */
    void Save(TOutputStream* output);

    //! Delegates the call to IMetaState::Load and updates the version.
    /*!
     * \note Thread affinity: StateThread
     */
    void Load(i32 segmentId, TInputStream* input);

    //! Checks if the mutation with this particular id was already applied.
    //! Fill mutation response data on success.
    bool FindKeptResponse(const TMutationId& id, TSharedRef* data);

    //! Invokes IMetaState::ApplyMutation and updates the version.
    /*!
     * \note Thread affinity: StateThread
     */
    void ApplyMutation(TMutationContext* context) throw();

    //! Deserializes the mutation, invokes IMetaState::ApplyMutation, and updates the version.
    void ApplyMutation(const TSharedRef& recordData) throw();

    //! Appends a new record into an appropriate changelog.
    /*!
     * \note Thread affinity: StateThread
     */
    TFuture<void> LogMutation(
        const TMetaVersion& version,
        const TSharedRef& recordData);

    //! Finalizes the current changelog, advances the segment, and creates a new changelog.
    /*!
     * \note Thread affinity: StateThread
     */
    void RotateChangeLog(const TEpochId& epochId);

    //! Updates the version so as to switch to a new segment.
    /*!
     * \note Thread affinity: StateThread
     */
    void AdvanceSegment();

    //! Returns the current mutation context or |nullptr| if no mutation is currently being applied.
    TMutationContext* GetMutationContext();

private:
    class TGuardedUserInvoker;
    class TSystemInvoker;

    IMetaStatePtr State;

    IInvokerPtr StateInvoker;
    TAtomic UserEnqueueLock;
    TAtomic SystemLock;
    IInvokerPtr SystemInvoker;

    TSnapshotStorePtr SnapshotStore;
    TChangeLogCachePtr ChangeLogCache;

    TResponseKeeperPtr ResponseKeeper;

    bool Started;
    TEpochId Epoch;
    TMutationContext* MutationContext;
    TCachedAsyncChangeLogPtr CurrentChangeLog;

    TSpinLock VersionSpinLock;
    TMetaVersion Version;
    TMetaVersion ReachableVersion;
    TMetaVersion PingVersion;

    void IncrementRecordCount();
    void ComputeReachableVersion();
    void UpdateVersion(const TMetaVersion& newVersion);
    TCachedAsyncChangeLogPtr GetCurrentChangeLog();

    bool AcquireUserEnqueueLock();
    void ReleaseUserEnqueueLock();
    void AcquireSystemLock();
    void ReleaseSystemLock();

    DECLARE_THREAD_AFFINITY_SLOT(StateThread);
    DECLARE_THREAD_AFFINITY_SLOT(ControlThread);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NMetaState
} // namespace NYT
