#include "stdafx.h"
#include "snapshot_lookup.h"
#include "private.h"
#include "config.h"
#include "meta_state_manager_proxy.h"

#include <ytlib/concurrency/thread_affinity.h>

#include <ytlib/actions/invoker_util.h>

#include <ytlib/election/cell_manager.h>

namespace NYT {
namespace NMetaState {

using namespace NElection;

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = MetaStateLogger;

////////////////////////////////////////////////////////////////////////////////

TSnapshotLookup::TSnapshotLookup(
    TPersistentStateManagerConfigPtr config,
    TCellManagerPtr cellManager)
    : Config(config)
    , CellManager(cellManager)
{
    YASSERT(config);
    YASSERT(cellManager);
}

int TSnapshotLookup::GetLatestSnapshotId(i32 maxSnapshotId)
{
    CurrentSnapshotId = NonexistingSnapshotId;
    Promise = NewPromise<i32>();

    auto awaiter = New<TParallelAwaiter>(GetSyncInvoker());

    LOG_INFO("Looking up for the latest snapshot <= %d", maxSnapshotId);
    for (TPeerId peerId = 0; peerId < CellManager->GetPeerCount(); ++peerId) {
        LOG_INFO("Requesting snapshot from peer %d", peerId);

        TProxy proxy(CellManager->GetMasterChannel(peerId));
        proxy.SetDefaultTimeout(Config->RpcTimeout);

        auto request = proxy.LookupSnapshot();
        request->set_max_snapshot_id(maxSnapshotId);
        awaiter->Await(
            request->Invoke(),
            BIND(
                &TSnapshotLookup::OnLookupSnapshotResponse,
                this,
                peerId));
    }
    LOG_INFO("Snapshot lookup requests sent");

    awaiter->Complete(BIND(
        &TSnapshotLookup::OnLookupSnapshotComplete,
        this));

    return Promise.Get();
}

void TSnapshotLookup::OnLookupSnapshotResponse(
    TPeerId peerId,
    TProxy::TRspLookupSnapshotPtr response)
{
    VERIFY_THREAD_AFFINITY_ANY();

    if (!response->IsOK()) {
        LOG_WARNING(response->GetError(), "Error looking up snapshots at peer %d",
            peerId);
        return;
    }

    int snapshotId = response->snapshot_id();
    if (snapshotId == NonexistingSnapshotId) {
        LOG_INFO("Peer %d has no suitable snapshot", peerId);
        return;
    }

    LOG_INFO("Peer %d reported snapshot %d",
        peerId,
        snapshotId);

    {
        TGuard<TSpinLock> guard(SpinLock);
        CurrentSnapshotId = std::max(CurrentSnapshotId, snapshotId);            
    }
}

void TSnapshotLookup::OnLookupSnapshotComplete()
{
    VERIFY_THREAD_AFFINITY_ANY();

    if (CurrentSnapshotId == NonexistingSnapshotId) {
        LOG_INFO("Snapshot lookup complete, no suitable snapshot is found");
    } else {
        LOG_INFO("Snapshot lookup complete, the latest snapshot is %d", CurrentSnapshotId);
    }

    Promise.Set(CurrentSnapshotId);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NMetaState
} // namespace NYT
