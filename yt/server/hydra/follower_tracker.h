#pragma once

#include "private.h"

#include <core/concurrency/periodic_executor.h>
#include <core/concurrency/thread_affinity.h>

#include <core/logging/tagged_logger.h>

#include <ytlib/hydra/hydra_service_proxy.h>

namespace NYT {
namespace NHydra {

////////////////////////////////////////////////////////////////////////////////

class TFollowerTracker
    : public TRefCounted
{
public:
    TFollowerTracker(
        TFollowerTrackerConfigPtr config,
        NElection::TCellManagerPtr cellManager,
        TDecoratedAutomatonPtr decoratedAutomaton,
        const TEpochId& epochId,
        IInvokerPtr epochControlInvoker);

    void Start();

    bool IsFollowerActive(TPeerId followerId) const;

    void ResetFollower(TPeerId followerId);

    TFuture<void> GetActiveQuorum();

private:
    TFollowerTrackerConfigPtr Config;
    NElection::TCellManagerPtr CellManager;
    TDecoratedAutomatonPtr DecoratedAutomaton;
    TEpochId EpochId;
    IInvokerPtr EpochControlInvoker;

    std::vector<EPeerState> PeerStates;
    int ActivePeerCount;
    TPromise<void> ActiveQuorumPromise;

    NLog::TTaggedLogger Logger;


    void SendPing(TPeerId followerId);
    void SchedulePing(TPeerId followerId);
    void OnPingResponse(TPeerId followerId, THydraServiceProxy::TRspPingFollowerPtr rsp);
    
    void SetFollowerState(TPeerId followerId, EPeerState state);
    void OnPeerActivated();
    void OnPeerDeactivated();

    DECLARE_THREAD_AFFINITY_SLOT(ControlThread);

};

DEFINE_REFCOUNTED_TYPE(TFollowerTracker)

////////////////////////////////////////////////////////////////////////////////

} // namespace NHydra
} // namespace NYT
