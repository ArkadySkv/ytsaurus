#pragma once

#include "public.h"

#include <ytlib/rpc/public.h>

namespace NYT {
namespace NScheduler {

////////////////////////////////////////////////////////////////////////////////

//! Creates a channel pointing to the scheduler of a given cell.
NRpc::IChannelPtr CreateSchedulerChannel(
    TSchedulerConnectionConfigPtr config,
    NRpc::IChannelPtr masterChannel);

////////////////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT
