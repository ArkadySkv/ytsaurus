#pragma once

#include "public.h"

#include <ytlib/misc/error.h>

namespace NYT {
namespace NRpc {

////////////////////////////////////////////////////////////////////////////////

typedef TCallback< TFuture< TErrorOr<IChannelPtr> >()> TChannelProducer;

//! Creates a channel with a dynamically discovered endpoint.
/*!
 *  Upon the first request to the created channel,
 *  the producer is called to discover the actual endpoint.
 *  This endpoint is cached and reused until some request fails with RPC error code.
 *  In the latter case the endpoint is rediscovered.
 */
IChannelPtr CreateRoamingChannel(
    TNullable<TDuration> defaultTimeout,
    bool retryEnabled,
    TChannelProducer producer);

////////////////////////////////////////////////////////////////////////////////

} // namespace NRpc
} // namespace NYT
