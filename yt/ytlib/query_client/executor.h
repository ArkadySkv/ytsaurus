#pragma once

#include "public.h"

#include <core/rpc/public.h>

#include <core/misc/common.h>
#include <core/misc/error.h>

namespace NYT {
namespace NQueryClient {

////////////////////////////////////////////////////////////////////////////////

struct IExecutor
    : public virtual TRefCounted
{
    virtual TAsyncError Execute(
        const TPlanFragment& fragment,
        IWriterPtr writer) = 0;

};

DEFINE_REFCOUNTED_TYPE(IExecutor)

////////////////////////////////////////////////////////////////////////////////

IExecutorPtr CreateEvaluator(
    IInvokerPtr invoker,
    IEvaluateCallbacks* callbacks);

IExecutorPtr CreateCoordinator(
    IInvokerPtr invoker,
    ICoordinateCallbacks* callbacks);

////////////////////////////////////////////////////////////////////////////////

} // namespace NQueryClient
} // namespace NYT

