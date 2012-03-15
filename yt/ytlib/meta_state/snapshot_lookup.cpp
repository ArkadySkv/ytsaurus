#include "stdafx.h"
#include "snapshot_lookup.h"
#include "common.h"
#include "meta_state_manager_proxy.h"
#include "cell_manager.h"

#include <ytlib/misc/thread_affinity.h>

namespace NYT {
namespace NMetaState {

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

i32 TSnapshotLookup::LookupLatestSnapshot(i32 maxSnapshotId)
{
    CurrentSnapshotId = NonexistingSnapshotId;
    ResultPromise = New< TFuture<i32> >();
    auto awaiter = New<TParallelAwaiter>();

    LOG_INFO("Looking up for the latest snapshot <= %d", maxSnapshotId);
    for (TPeerId peerId = 0; peerId < CellManager->GetPeerCount(); ++peerId) {
        LOG_INFO("Requesting snapshot from peer %d", peerId);

        auto request =
            CellManager->GetMasterProxy<TProxy>(peerId)
            ->LookupSnapshot()
            ->SetTimeout(Config->RpcTimeout);
        request->set_max_snapshot_id(maxSnapshotId);
        awaiter->Await(
            request->Invoke(),
            FromMethod(
            &TSnapshotLookup::OnLookupSnapshotResponse,
            this,
            peerId));
    }
    LOG_INFO("Snapshot lookup requests sent");

    awaiter->Complete(FromMethod(
        &TSnapshotLookup::OnLookupSnapshotComplete,
        this));

    return ResultPromise->Get();
}

void TSnapshotLookup::OnLookupSnapshotResponse(
    TProxy::TRspLookupSnapshot::TPtr response,
    TPeerId peerId)
{
    VERIFY_THREAD_AFFINITY_ANY();

    if (!response->IsOK()) {
        LOG_WARNING("Error looking up snapshots at peer %d\n%s",
            peerId,
            ~response->GetError().ToString());
        return;
    }

    i32 snapshotId = response->snapshot_id();
    if (snapshotId == NonexistingSnapshotId) {
        LOG_INFO("Peer %d has no suitable snapshot", peerId);
    } else {
        LOG_INFO("Peer %d reported snapshot %d",
            peerId,
            snapshotId);
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

    ResultPromise->Set(CurrentSnapshotId);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NMetaState
} // namespace NYT
