#pragma once

#include "public.h"
#include "meta_state_manager.h"

#include <ytlib/misc/thread_affinity.h>
#include <ytlib/misc/lease_manager.h>
#include <ytlib/misc/configurable.h>

namespace NYT {
namespace NMetaState {

////////////////////////////////////////////////////////////////////////////////

class TFollowerTracker
    : public TRefCounted
{
public:
    TFollowerTracker(
        TFollowerTrackerConfig* config,
        TCellManager* cellManager,
        IInvoker* epochControlInvoker);

    void Start();
    void Stop();
    bool HasActiveQuorum() const;
    bool IsFollowerActive(TPeerId followerId) const;
    void ProcessPing(TPeerId followerId, EPeerStatus status);

private:
    struct TFollowerState
    {
        EPeerStatus Status;
        TLeaseManager::TLease Lease;
    };

    void ChangeFollowerStatus(int followerId, EPeerStatus  status);
    void ResetFollowerState(int followerId);
    void OnLeaseExpired(TPeerId followerId);

    TFollowerTrackerConfigPtr Config;
    TCellManagerPtr CellManager;
    IInvoker::TPtr EpochControlInvoker;
    yvector<TFollowerState> FollowerStates;
    int ActiveFollowerCount;

    DECLARE_THREAD_AFFINITY_SLOT(ControlThread);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NMetaState
} // namespace NYT
