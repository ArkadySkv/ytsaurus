#pragma once

#include <ytlib/misc/common.h>
#include <ytlib/misc/guid.h>

namespace NYT {
namespace NElection {

////////////////////////////////////////////////////////////////////////////////

class TCellManager;
typedef TIntrusivePtr<TCellManager> TCellManagerPtr;

class TCellConfig;
typedef TIntrusivePtr<TCellConfig> TCellConfigPtr;

class TElectionManagerConfig;
typedef TIntrusivePtr<TElectionManagerConfig> TElectionManagerConfigPtr;

struct IElectionCallbacks;
typedef TIntrusivePtr<IElectionCallbacks> IElectionCallbacksPtr;

struct TEpochContext;
typedef TIntrusivePtr<TEpochContext> TEpochContextPtr;

class TElectionManager;
typedef TIntrusivePtr<TElectionManager> TElectionManagerPtr;

class TCellManager;
typedef TIntrusivePtr<TCellManager> TCellManagerPtr;

////////////////////////////////////////////////////////////////////////////////

typedef TGuid TEpochId;
typedef i64 TPeerPriority;

typedef int TPeerId;
const TPeerId InvalidPeerId = -1;

////////////////////////////////////////////////////////////////////////////////

DECLARE_ENUM(EErrorCode,
    ((InvalidState)  (800))
    ((InvalidLeader) (801))
    ((InvalidEpoch)  (802))
);

DECLARE_ENUM(EPeerState,
    (Stopped)
    (Voting)
    (Leading)
    (Following)
);

////////////////////////////////////////////////////////////////////////////////

} // namespace NElection
} // namespace NYT
