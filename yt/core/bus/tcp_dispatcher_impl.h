#pragma once

#include "private.h"
#include "tcp_dispatcher.h"

#include <core/misc/error.h>
#include <core/misc/address.h>
#include <core/misc/random.h>

#include <core/concurrency/action_queue_detail.h>
#include <core/concurrency/event_count.h>

#include <util/thread/lfqueue.h>

#include <contrib/libev/ev++.h>

#include <atomic>

namespace NYT {
namespace NBus {

////////////////////////////////////////////////////////////////////////////////

TNetworkAddress GetLocalBusAddress(int port);
bool IsLocalServiceAddress(const Stroka& address);

////////////////////////////////////////////////////////////////////////////////

struct IEventLoopObject
    : public virtual TRefCounted
{
    virtual void SyncInitialize() = 0;
    virtual void SyncFinalize() = 0;
    virtual Stroka GetLoggingId() const = 0;
};

DEFINE_REFCOUNTED_TYPE(IEventLoopObject)

////////////////////////////////////////////////////////////////////////////////

class TTcpDispatcherThread
    : public NConcurrency::TSingleQueueEVSchedulerThread
{
public:
    explicit TTcpDispatcherThread(const Stroka& threadName);

    void Shutdown();

    const ev::loop_ref& GetEventLoop() const;

    TAsyncError AsyncRegister(IEventLoopObjectPtr object);
    TAsyncError AsyncUnregister(IEventLoopObjectPtr object);

    void AsyncPostEvent(TTcpConnectionPtr connection, EConnectionEvent event);

    TTcpDispatcherStatistics& Statistics(ETcpInterfaceType interfaceType);

private:
    friend class TTcpDispatcherInvokerQueue;

    std::vector<TTcpDispatcherStatistics> Statistics_;

    struct TEventEntry
    {
        TEventEntry()
        { }

        TEventEntry(TTcpConnectionPtr connection, EConnectionEvent event)
            : Connection(std::move(connection))
            , Event(event)
        { }

        TTcpConnectionPtr Connection;
        EConnectionEvent Event;
    };

    TLockFreeQueue<TEventEntry> EventQueue;
    ev::async EventWatcher;

    yhash_set<IEventLoopObjectPtr> Objects;


    void OnEvent(ev::async&, int);

    void DoRegister(IEventLoopObjectPtr object);
    void DoUnregister(IEventLoopObjectPtr object);

};

DEFINE_REFCOUNTED_TYPE(TTcpDispatcherThread)

////////////////////////////////////////////////////////////////////////////////

class TTcpDispatcher::TImpl
{
public:
    static TImpl* Get();

    void Shutdown();

    TTcpDispatcherStatistics GetStatistics(ETcpInterfaceType interfaceType) const;

    TTcpDispatcherThreadPtr AllocateThread();
    
private:
    friend TTcpDispatcher;
    
    TImpl();

    std::vector<TTcpDispatcherThreadPtr> Threads;

    TRandomGenerator ThreadIdGenerator;
    TSpinLock SpinLock;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NBus
} // namespace NYT
