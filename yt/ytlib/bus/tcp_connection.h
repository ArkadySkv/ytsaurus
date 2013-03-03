#pragma once

#include "private.h"
#include "tcp_dispatcher_impl.h"
#include "bus.h"
#include "packet.h"
#include "message.h"

#include <ytlib/misc/thread_affinity.h>
#include <ytlib/misc/address.h>

#include <ytlib/actions/future.h>

#include <ytlib/logging/tagged_logger.h>

#include <queue>

#include <util/thread/lfqueue.h>

#include <contrib/libuv/src/unix/ev/ev++.h>

#ifndef _WIN32
    #include <sys/uio.h>
#endif

namespace NYT {
namespace NBus {

////////////////////////////////////////////////////////////////////////////////

class TTcpConnection
    : public IBus
    , public IEventLoopObject
{
public:
    TTcpConnection(
        TTcpBusConfigPtr config,
        EConnectionType type,
        const TConnectionId& id,
        int socket,
        const Stroka& address,
        int priority,
        IMessageHandlerPtr handler);

    ~TTcpConnection();

    const TConnectionId& GetId() const;

    // IEventLoopObject implementation.
    virtual void SyncInitialize() override;
    virtual void SyncFinalize() override;
    virtual Stroka GetLoggingId() const override;

    // IBus implementation.
    virtual TAsyncError Send(IMessagePtr message) override;
    virtual void Terminate(const TError& error) override;

    void SyncProcessEvent(EConnectionEvent event);

    DECLARE_SIGNAL(void(TError), Terminated);

private:
    struct TQueuedMessage
    {
        TQueuedMessage()
        { }

        explicit TQueuedMessage(IMessagePtr message)
            : Promise(NewPromise<TError>())
            , Message(std::move(message))
            , PacketId(TPacketId::Create())
        { }

        TAsyncErrorPromise Promise;
        IMessagePtr Message;
        TPacketId PacketId;
    };

    struct TQueuedPacket
    {
        TQueuedPacket(EPacketType type, const TPacketId& packetId, IMessagePtr message, i64 size)
            : Type(type)
            , PacketId(packetId)
            , Message(std::move(message))
            , Size(size)
        { }

        EPacketType Type;
        TPacketId PacketId;
        IMessagePtr Message;
        i64 Size;
    };

    struct TUnackedMessage
    {
        TUnackedMessage()
        { }

        TUnackedMessage(const TPacketId& packetId, TAsyncErrorPromise promise)
            : PacketId(packetId)
            , Promise(std::move(promise))
        { }

        TPacketId PacketId;
        TAsyncErrorPromise Promise;
    };

    struct TEncodedPacket
    {
        TPacketEncoder Encoder;
        TQueuedPacket* Packet;
    };

    struct TEncodedFragment
    {
        TRef Data;
        bool IsLastInPacket;
    };

    DECLARE_ENUM(EState,
        (Resolving)
        (Opening)
        (Open)
        (Closed)
    );

    TTcpDispatcher::TImpl* Dispatcher;
    TTcpBusConfigPtr Config;
    EConnectionType Type;
    TConnectionId Id;
    int Socket;
    int Fd;
    Stroka Address;
    int Priority;
    IMessageHandlerPtr Handler;

    NLog::TTaggedLogger Logger;

    // Only used for client sockets.
    int Port;
    TFuture< TValueOrError<TNetworkAddress> > AsyncAddress;

    TSpinLock SpinLock;
    EState State;
    TError TerminationError;

    THolder<ev::io> SocketWatcher;

    TBlob ReadBuffer;
    TPacketDecoder Decoder;

    TPromise<TError> TerminatedPromise;

    TLockFreeQueue<TQueuedMessage> QueuedMessages;
    std::queue<TQueuedPacket*> QueuedPackets;
    std::queue<TEncodedPacket*> EncodedPackets;
    std::deque<TEncodedFragment> EncodedFragments;
#ifdef _WIN32
    std::vector<WSABUF> SendVector;
#else
    std::vector<struct iovec> SendVector;
#endif

    std::queue<TUnackedMessage> UnackedMessages;

    DECLARE_THREAD_AFFINITY_SLOT(EventLoop);

    void Cleanup();

    void SyncOpen();
    static bool IsLocal(const TStringBuf& hostName);
    void SyncResolve();
    void SyncClose(const TError& error);

    void InitFd();
    void ConnectSocket(const TNetworkAddress& netAddress);
    void CloseSocket();

    void OnAddressResolved();
    void OnAddressResolved(const TNetworkAddress& netAddress);

    void OnSocket(ev::io&, int revents);

    int GetSocketError() const;
    bool IsSocketError(ssize_t result);

    void OnSocketRead();
    bool ReadSocket(char* buffer, size_t size, size_t* bytesRead);
    bool CheckReadError(ssize_t result);
    bool AdvanceDecoder(size_t size);
    bool OnPacketReceived();
    bool OnAckPacketReceived();
    bool OnMessagePacketReceived();

    void EnqueuePacket(EPacketType type, const TPacketId& packetId, IMessagePtr message = NULL);
    void OnSocketWrite();
    bool HasUnsentData() const;
    bool WriteFragments(size_t* bytesWritten);
    void FlushWrittenFragments(size_t bytesWritten);
    bool EncodeMoreFragments();
    bool CheckWriteError(ssize_t result);
    void OnPacketSent();
    void OnAckPacketSent(const TEncodedPacket& packet);
    void OnMessagePacketSent(const TEncodedPacket& packet);
    void OnMessageEnqueued();
    void ProcessOutcomingMessages();
    void DiscardOutcomingMessages();
    void UpdateSocketWatcher();

    void OnTerminated();

    TTcpDispatcherStatistics& Statistics();
    void UpdateConnectionCount(int delta);
    void UpdatePendingOut(int countDelta, i64 sizeDelta);

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NBus
} // namespace NYT
