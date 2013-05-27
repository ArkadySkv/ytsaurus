#include "stdafx.h"
#include "future.h"

#include <ytlib/misc/delayed_invoker.h>

namespace NYT {

///////////////////////////////////////////////////////////////////////////////

TPromise<void> NewPromise()
{
    return TPromise<void>(New< NYT::NDetail::TPromiseState<void> >(false));
}

TPromise<void> MakePromise()
{
    return TPromise<void>(New< NYT::NDetail::TPromiseState<void> >(true));
}

TFuture<void> MakeDelayed(TDuration delay)
{
    auto promise = NewPromise();
    TDelayedInvoker::Submit(
        BIND([=] () mutable { promise.Set(); }),
        delay);
    return promise;
}

///////////////////////////////////////////////////////////////////////////////

} // namespace NYT
