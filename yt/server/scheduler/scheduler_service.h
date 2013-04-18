#pragma once

#include "public.h"

#include <ytlib/rpc/public.h>

#include <server/cell_scheduler/public.h>

namespace NYT {
namespace NScheduler {

////////////////////////////////////////////////////////////////////

NRpc::IServicePtr CreateSchedulerService(NCellScheduler::TBootstrap* bootstrap);

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

