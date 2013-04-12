#include "stdafx.h"
#include "private.h"

namespace NYT {
namespace NNodeTrackerServer {

////////////////////////////////////////////////////////////////////////////////

NLog::TLogger NodeTrackerServerLogger("NodeTracker");
NProfiling::TProfiler NodeTrackerServerProfiler("/node_tracker");

////////////////////////////////////////////////////////////////////////////////

} // namespace NNodeTrackerServer
} // namespace NYT
