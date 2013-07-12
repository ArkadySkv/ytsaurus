#pragma once

#include "public.h"

#include <ytlib/misc/periodic_invoker.h>

#include <ytlib/profiling/public.h>

namespace NYT {
namespace NProfiling {

////////////////////////////////////////////////////////////////////////////////

#if !defined(_win_) && !defined(_darwin_)
    #define RESOURCE_TRACKER_ENABLED
#endif

class TResourceTracker
    : public TRefCounted
{
public:
    explicit TResourceTracker(IInvokerPtr invoker);

    void Start();

private:
    i64 TicksPerSecond;
    TInstant LastUpdateTime;

    struct TJiffies
    {
        i64 PreviousUser;
        i64 PreviousSystem;
    };

    yhash_map<Stroka, TJiffies> ThreadNameToJiffies;

    TPeriodicInvokerPtr PeriodicInvoker;

    void EnqueueUsage();

    void EnqueueCpuUsage();
    void EnqueueMemoryUsage();

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NProfiling
} // namespace NYT
