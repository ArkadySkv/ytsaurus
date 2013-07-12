#pragma once

#include "common.h"
#include "invoker.h"
#include "callback.h"

#include <ytlib/misc/property.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

// TODO(babenko): move to public.h
class TActionQueue;
typedef TIntrusivePtr<TActionQueue> TActionQueuePtr;

class TFairShareActionQueue;
typedef TIntrusivePtr<TFairShareActionQueue> TFairShareActionQueuePtr;

class TPrioritizedActionQueue;
typedef TIntrusivePtr<TPrioritizedActionQueue> TPrioritizedActionQueuePtr;

class TThreadPool;
typedef TIntrusivePtr<TThreadPool> TThreadPoolPtr;

class TExecutorThreadWithQueue;
typedef TIntrusivePtr<TExecutorThreadWithQueue> TExecutorThreadWithQueuePtr;

////////////////////////////////////////////////////////////////////////////////

class TActionQueue
    : public TRefCounted
{
public:
    explicit TActionQueue(const Stroka& threadName = "<ActionQueue>");

    virtual ~TActionQueue();

    void Shutdown();

    IInvokerPtr GetInvoker();

    static TCallback<TActionQueuePtr()> CreateFactory(const Stroka& threadName);

private:
    TExecutorThreadWithQueuePtr Impl;

};

////////////////////////////////////////////////////////////////////////////////

class TFairShareActionQueue
    : public TRefCounted
{
public:
    explicit TFairShareActionQueue(
        const Stroka& threadName,
        const std::vector<Stroka>& bucketNames);

    virtual ~TFairShareActionQueue();

    void Shutdown();

    IInvokerPtr GetInvoker(int index);

private:
    class TImpl;
    TIntrusivePtr<TImpl> Impl;

};

////////////////////////////////////////////////////////////////////////////////

class TThreadPool
    : public TRefCounted
{
public:
    TThreadPool(
        int threadCount,
        const Stroka& threadNamePrefix);
    virtual ~TThreadPool();

    void Shutdown();

    IInvokerPtr GetInvoker();

    static TCallback<TThreadPoolPtr()> CreateFactory(
        int queueCount,
        const Stroka& threadName);

private:
    class TImpl;
    TIntrusivePtr<TImpl> Impl;

};

////////////////////////////////////////////////////////////////////////////////

//! Returns an invoker that executes all queues actions in the
//! context of #underlyingInvoker (possibly in different threads)
//! but in a serialized fashion (i.e. all queued actions are executed
//! in the proper order and no two actions are executed in parallel).
IInvokerPtr CreateSerializedInvoker(IInvokerPtr underlyingInvoker);

////////////////////////////////////////////////////////////////////////////////

//! Creates a wrapper around IInvoker that supports action reordering.
//! Actions with the highest priority are executed first.
IPrioritizedInvokerPtr CreatePrioritizedInvoker(IInvokerPtr underlyingInvoker);

//! Creates a wrapper around IInvoker that implements IPrioritizedInvoker but
//! does not perform any actual reordering. Priorities passed to #IPrioritizedInvoker::Invoke
//! are ignored.
IPrioritizedInvokerPtr CreateFakePrioritizedInvoker(IInvokerPtr underlyingInvoker);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

