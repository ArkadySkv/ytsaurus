#pragma once

#include "public.h"
#include "meta_state_manager_proxy.h"

#include <ytlib/rpc/client.h>
#include <ytlib/actions/parallel_awaiter.h>
#include <ytlib/actions/future.h>

namespace NYT {
namespace NMetaState {

////////////////////////////////////////////////////////////////////////////////

class TSnapshotLookup
    : private TNonCopyable
{
public:
    TSnapshotLookup(
        TPersistentStateManagerConfigPtr config,
        TCellManagerPtr cellManager);

    i32 LookupLatestSnapshot(i32 maxSnapshotId);

private:
    typedef TMetaStateManagerProxy TProxy;
    typedef TProxy::EErrorCode EErrorCode;

    TPersistentStateManagerConfigPtr Config;
    TCellManagerPtr CellManager;
    i32 CurrentSnapshotId;
    TFuture<i32>::TPtr ResultPromise;

    void OnLookupSnapshotResponse(
        TPeerId peerId,
        TProxy::TRspLookupSnapshot::TPtr response);
    void OnLookupSnapshotComplete();
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NMetaState
} // namespace NYT
