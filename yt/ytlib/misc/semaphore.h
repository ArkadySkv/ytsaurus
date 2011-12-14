﻿#pragma once

#include "common.h"
#include "thread_affinity.h"
#include "../actions/future.h"

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

//! Custom semaphore class with async aquire operation.
/*!
 *  Can be used by a single acquire thread and possibly multiple 
 *  release threads. The acquirer must not call AsyncAquire again, 
 *  until returned future is set.
 */
class TAsyncSemaphore
{
public:
    explicit TAsyncSemaphore(int maxFreeSlots);

    //! Increases the counter.
    /*!
     *  Returns 'true' if the semaphore counter has increased.
     */
    void Release(int slots = 1);

    /*!
     *  Must be called from single thread.
     *  The client must not call AsyncAquire again, until returned
     *  future is set.
     */
    TFuture<TVoid>::TPtr AsyncAcquire(int slots = 1);

private:
    TSpinLock SpinLock;

    const int MaxFreeSlots;
    int FreeSlotCount;
    int RequestedSlots;

    TFuture<TVoid>::TPtr AcquireEvent;
    TFuture<TVoid>::TPtr StaticResult;
    DECLARE_THREAD_AFFINITY_SLOT(ClientThread);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT 