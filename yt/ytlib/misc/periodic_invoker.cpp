#include "stdafx.h"
#include "periodic_invoker.h"

#include <ytlib/actions/invoker_util.h>
#include <ytlib/actions/bind.h>

#include <util/random/random.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

TPeriodicInvoker::TPeriodicInvoker(
    IInvokerPtr invoker,
    TClosure callback,
    TDuration period,
    TDuration splay)
    : Invoker(invoker)
    , Callback(callback)
    , Period(period)
    , Splay(splay)
    , Started(false)
    , Busy(false)
    , OutOfBandRequested(false)
{ }

void TPeriodicInvoker::Start()
{
    if (!AtomicCas(&Started, true, false))
        return;

    PostDelayedCallback(RandomDuration(Splay));
}

void TPeriodicInvoker::Stop()
{
    if (!AtomicCas(&Started, false, true))
        return;

    TDelayedInvoker::CancelAndClear(Cookie);
}

void TPeriodicInvoker::ScheduleOutOfBand()
{
    if (!AtomicGet(Started))
        return;

    if (AtomicGet(Busy)) {
        AtomicSet(OutOfBandRequested, true);
    } else {
        PostCallback();
    }
}

void TPeriodicInvoker::ScheduleNext()
{
    if (!AtomicGet(Started))
        return;

    YCHECK(AtomicCas(&Busy, false, true));

    if (AtomicCas(&OutOfBandRequested, false, true)) {
        PostCallback();
    } else {
        PostDelayedCallback(Period);
    }
}

void TPeriodicInvoker::PostDelayedCallback(TDuration delay)
{
    TDelayedInvoker::CancelAndClear(Cookie);
    Cookie = TDelayedInvoker::Submit(
        BIND(&TPeriodicInvoker::PostCallback, MakeStrong(this)),
        delay);
}

void TPeriodicInvoker::PostCallback()
{
    auto this_ = MakeStrong(this);
    bool result = Invoker->Invoke(BIND([this, this_] () {
        if (AtomicGet(Started) && !AtomicGet(Busy)) {
            AtomicSet(Busy, true);
            TDelayedInvoker::CancelAndClear(Cookie);
            Callback.Run();
        }
    }));
    if (!result) {
        PostDelayedCallback(Period);
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
