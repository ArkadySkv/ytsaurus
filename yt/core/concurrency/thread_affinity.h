#pragma once

#include "public.h"

#include <core/misc/preprocessor.h>

#include <core/actions/invoker_util.h>

#include <util/system/thread.h>

#include <atomic>

namespace NYT {
namespace NConcurrency {

////////////////////////////////////////////////////////////////////////////////

/*!
 * Allows to annotate certain functions with thread affinity.
 * The checks are performed at run-time to ensure that each function
 * invocation that is annotated with a particular affinity slot
 * takes place in one thread.
 *
 * The usage is as follows.
 * - For each thread that may invoke your functions declare a slot with
 *   \code
 *   DECLARE_THREAD_AFFINITY_SLOT(Thread);
 *   \endcode
 * - Write
 *   \code
 *   VERIFY_THREAD_AFFINITY(Thread);
 *   \endcode
 *   at the beginning of each function in the group.
 *
 * Please refer to the unit test for an actual usage example
 * (unittests/thread_affinity_ut.cpp).
 */

class TThreadAffinitySlot
{
public:
    TThreadAffinitySlot()
        : BoundId(InvalidThreadId)
    { }

    void Check(TThreadId threadId = GetCurrentThreadId())
    {
        YCHECK(threadId != InvalidThreadId);
        auto expectedId = InvalidThreadId;
        if (!BoundId.compare_exchange_strong(expectedId, threadId)) {
            YCHECK(expectedId == threadId);
        }
    }

private:
    std::atomic<TThreadId> BoundId;

};

#ifdef ENABLE_THREAD_AFFINITY_CHECK

#define DECLARE_THREAD_AFFINITY_SLOT(slot) \
    mutable ::NYT::NConcurrency::TThreadAffinitySlot PP_CONCAT(slot, __Slot)

#define VERIFY_THREAD_AFFINITY(slot) \
    PP_CONCAT(slot, __Slot).Check()

// TODO: remove this dirty hack.
static_assert(sizeof(TSpinLock) == sizeof(TAtomic),
    "Current implementation assumes that TSpinLock fits within implementation.");

#define VERIFY_SPINLOCK_AFFINITY(spinLock) \
    YCHECK(*reinterpret_cast<const TAtomic*>(&(spinLock)) != 0);

#define VERIFY_INVOKER_AFFINITY(invoker, slot) \
    PP_CONCAT(slot, __Slot).Check((invoker)->GetThreadId());

#else

// Expand macros to null but take care of the trailing semicolon.
#define DECLARE_THREAD_AFFINITY_SLOT(slot)     struct PP_CONCAT(TNullThreadAffinitySlot__,  __LINE__) { }
#define VERIFY_THREAD_AFFINITY(slot)           do { } while (0)
#define VERIFY_SPINLOCK_AFFINITY(spinLock)     do { } while (0)
#define VERIFY_INVOKER_AFFINITY(invoker, slot) do { } while (0)

#endif

//! This is a mere declaration and intentionally does not check anything.
#define VERIFY_THREAD_AFFINITY_ANY()           do { } while (0)

////////////////////////////////////////////////////////////////////////////////

} // namespace NConcurrency
} // namespace NYT
