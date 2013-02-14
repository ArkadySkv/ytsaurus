#include "stdafx.h"
#include "tcp_server.h"
#include "tcp_dispatcher_impl.h"
#include "server.h"
#include "config.h"
#include "bus.h"
#include "tcp_connection.h"

#include <ytlib/misc/thread_affinity.h>
#include <ytlib/misc/address.h>

#include <ytlib/logging/tagged_logger.h>

#include <ytlib/rpc/error.h>

#include <errno.h>

#ifndef _win_
    #include <netinet/tcp.h>
    #include <sys/socket.h>
    #include <sys/un.h>
    #include <sys/types.h>
    #include <sys/stat.h>
#endif

namespace NYT {
namespace NBus {

////////////////////////////////////////////////////////////////////////////////

static NProfiling::TProfiler& Profiler = BusProfiler;
static NProfiling::TAggregateCounter AcceptTime("/accept_time");

////////////////////////////////////////////////////////////////////////////////

class TBusServerBase
    : public IEventLoopObject
{
public:
    TBusServerBase(
        TTcpBusServerConfigPtr config,
        IMessageHandlerPtr handler)
        : Config(config)
        , Handler(handler)
        , Logger(BusLogger)
        , ServerSocket(INVALID_SOCKET)
        , ServerFd(INVALID_SOCKET)
    {
        VERIFY_THREAD_AFFINITY_ANY();
        YCHECK(handler);
    }

    // IEventLoopObject implementation.

    virtual void SyncInitialize()
    {
        VERIFY_THREAD_AFFINITY(EventLoop);

        // This may throw.
        OpenServerSocket();

        const auto& eventLoop = TTcpDispatcher::TImpl::Get()->GetEventLoop();
        AcceptWatcher.Reset(new ev::io(eventLoop));
        AcceptWatcher->set<TBusServerBase, &TBusServerBase::OnAccept>(this);
        AcceptWatcher->start(ServerFd, ev::READ);
    }

    virtual void SyncFinalize()
    {
        VERIFY_THREAD_AFFINITY(EventLoop);

        AcceptWatcher.Destroy();

        CloseServerSocket();

        FOREACH (auto connection, Connections) {
            connection->Terminate(TError(NRpc::EErrorCode::TransportError, "Bus server terminated"));
        }
    }

    virtual Stroka GetLoggingId() const
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return Sprintf("Port: %d", Config->Port);
    }

protected:
    TTcpBusServerConfigPtr Config;
    IMessageHandlerPtr Handler;

    NLog::TTaggedLogger Logger;

    THolder<ev::io> AcceptWatcher;

    int ServerSocket;
    int ServerFd;

    yhash_set<TTcpConnectionPtr> Connections;

    DECLARE_THREAD_AFFINITY_SLOT(EventLoop);


    virtual void CreateServerSocket() = 0;

    virtual void InitClientSocket(SOCKET clientSocket)
    {
        {
            int value = 1;
            setsockopt(clientSocket, IPPROTO_TCP, TCP_NODELAY, (const char*) &value, sizeof(value));
        }
        {
            int value = 1;
            setsockopt(clientSocket, SOL_SOCKET, SO_KEEPALIVE, (const char*) &value, sizeof(value));
        }
    }


    void OnConnectionTerminated(TTcpConnectionPtr connection, TError error)
    {
        VERIFY_THREAD_AFFINITY_ANY();
        UNUSED(error);

        YCHECK(Connections.erase(connection) == 1);
    }


    void OpenServerSocket()
    {
        LOG_DEBUG("Opening server socket");

        CreateServerSocket();

        InitSocket(ServerSocket);

        if (listen(ServerSocket, SOMAXCONN) == SOCKET_ERROR) {
            int error = LastSystemError();
            CloseServerSocket();
            THROW_ERROR_EXCEPTION("Failed to listen to server socket")
                << TError::FromSystem(error);
        }

        LOG_DEBUG("Server socket opened");
    }

    void CloseServerSocket()
    {
        if (ServerFd != INVALID_SOCKET) {
            close(ServerFd);
            LOG_DEBUG("Server socket closed");
        }
        ServerSocket = INVALID_SOCKET;
        ServerFd = INVALID_SOCKET;
    }

    void InitSocket(SOCKET socket)
    {
        // TODO(babenko): check results
#ifdef _win_
        unsigned long dummy = 1;
        ioctlsocket(socket, FIONBIO, &dummy);
#else
        fcntl(socket, F_SETFL, O_NONBLOCK);
        fcntl(socket, F_SETFD, FD_CLOEXEC);
#endif
    }


    void OnAccept(ev::io&, int revents)
    {
        VERIFY_THREAD_AFFINITY(EventLoop);

        if (revents & ev::ERROR) {
            LOG_WARNING("Accept error");
            return;
        }

        while (true) {
            TNetworkAddress clientAddress;
            socklen_t clientAddressLen = clientAddress.GetLength();
            SOCKET clientSocket;
            PROFILE_AGGREGATED_TIMING (AcceptTime) {
#ifdef _linux_
                clientSocket = accept4(
                    ServerSocket,
                    clientAddress.GetSockAddr(),
                    &clientAddressLen,
                    SOCK_CLOEXEC);
#else
                clientSocket = accept(
                    ServerSocket,
                    clientAddress.GetSockAddr(),
                    &clientAddressLen);
#endif
            }

            if (clientSocket == INVALID_SOCKET) {
                auto error = LastSystemError();
                if (IsSocketError(error)) {
                    auto wrappedError = TError(
                        NRpc::EErrorCode::TransportError,
                        "Error accepting connection")
                        << TError::FromSystem(error);
                    LOG_WARNING(wrappedError);
                }
                break;
            }

            LOG_DEBUG("Connection accepted");

            InitClientSocket(clientSocket);
            InitSocket(clientSocket);

            auto connection = New<TTcpConnection>(
                EConnectionType::Server,
                TConnectionId::Create(),
                clientSocket,
                ToString(clientAddress, true),
                0,
                Handler);
            connection->SubscribeTerminated(BIND(
                &TBusServerBase::OnConnectionTerminated,
                MakeWeak(this),
                connection));
            YCHECK(Connections.insert(connection).second);
            TTcpDispatcher::TImpl::Get()->AsyncRegister(connection);
        }
    }


    bool IsSocketError(ssize_t result)
    {
#ifdef _WIN32
        return result != WSAEINPROGRESS && result != WSAEWOULDBLOCK;
#else
        YCHECK(result != EINTR);
        return result != EINPROGRESS && result != EWOULDBLOCK;
#endif
    }
};

class TTcpBusServer
    : public TBusServerBase
{
public:
    TTcpBusServer(
        TTcpBusServerConfigPtr config,
        IMessageHandlerPtr handler)
        : TBusServerBase(config, handler)
    {
        Logger.AddTag(Sprintf("Port: %d", Config->Port));
    }


    // IEventLoopObject implementation.

    virtual Stroka GetLoggingId() const override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return Sprintf("Port: %d", Config->Port);
    }

private:
    virtual void CreateServerSocket() override
    {
        int type = SOCK_STREAM;

#ifdef _linux_
        type |= SOCK_CLOEXEC;
#endif

        ServerSocket = socket(AF_INET6, type, IPPROTO_TCP);
        if (ServerSocket == INVALID_SOCKET) {
            THROW_ERROR_EXCEPTION("Failed to create a server socket")
                << TError::FromSystem();
        }

#ifdef _WIN32
        ServerFd = _open_osfhandle(ServerSocket, 0);
#else
        ServerFd = ServerSocket;
#endif

        // TODO(babenko): check for errors
        {
            int flag = 0;
            setsockopt(ServerSocket, IPPROTO_IPV6, IPV6_V6ONLY, (const char*) &flag, sizeof(flag));
        }

        {
            int flag = 1;
            setsockopt(ServerSocket, SOL_SOCKET, SO_REUSEADDR, (const char*) &flag, sizeof(flag));
        }

        {
            sockaddr_in6 serverAddress;
            memset(&serverAddress, 0, sizeof(serverAddress));
            serverAddress.sin6_family = AF_INET6;
            serverAddress.sin6_addr = in6addr_any;
            serverAddress.sin6_port = htons(Config->Port);
            if (bind(ServerSocket, (sockaddr*)&serverAddress, sizeof(serverAddress)) != 0) {
                CloseServerSocket();
                THROW_ERROR_EXCEPTION("Failed to bind a server socket to port %d", Config->Port)
                    << TError::FromSystem();
            }
        }
    }

    virtual void InitClientSocket(SOCKET clientSocket) override
    {
        TBusServerBase::InitClientSocket(clientSocket);

#ifdef _linux_
        {
            int priority = Config->Priority;
            setsockopt(clientSocket, SOL_SOCKET, SO_PRIORITY, (const char*) &priority, sizeof(priority));
        }
#endif
    }
};

class TLocalBusServer
    : public TBusServerBase
{
public:
    TLocalBusServer(
        TTcpBusServerConfigPtr config,
        IMessageHandlerPtr handler)
        : TBusServerBase(config, handler)
    {
        Logger.AddTag(Sprintf("LocalPort: %d", Config->Port));
    }


    // IEventLoopObject implementation.

    virtual Stroka GetLoggingId() const override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return Sprintf("LocalPort: %d", Config->Port);
    }

private:
    Stroka Path;

    virtual void CreateServerSocket() override
    {
        int type = SOCK_STREAM;

#ifdef _linux_
        type |= SOCK_CLOEXEC;
#endif

        ServerSocket = socket(AF_UNIX, type, 0);
        if (ServerSocket == INVALID_SOCKET) {
            THROW_ERROR_EXCEPTION("Failed to create a local server socket")
                << TError::FromSystem();
        }

        ServerFd = ServerSocket;

        {
            auto netAddress = GetLocalBusAddress(Config->Port);
            if (bind(ServerSocket, netAddress.GetSockAddr(), netAddress.GetLength()) != 0) {
                CloseServerSocket();
                THROW_ERROR_EXCEPTION("Failed to bind a local server socket")
                    << TError::FromSystem();
            }
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

//! A lightweight proxy controlling the lifetime of #TTcpBusServer.
/*!
 *  When the last strong reference vanishes, it unregisters the underlying
 *  server instance.
 */
template <class TServer>
class TTcpBusServerProxy
    : public IBusServer
{
public:
    explicit TTcpBusServerProxy(TTcpBusServerConfigPtr config)
        : Config(config)
        , Running(false)
    {
        YCHECK(config);
    }

    ~TTcpBusServerProxy()
    {
        Stop();
    }

    virtual void Start(IMessageHandlerPtr handler)
    {
        TGuard<TSpinLock> guard(SpinLock);

        YCHECK(!Running);

        auto server = New<TServer>(Config, handler);
        auto error = TTcpDispatcher::TImpl::Get()->AsyncRegister(server).Get();
        if (!error.IsOK()) {
            THROW_ERROR error;
        }

        Server = server;
        Running = true;
    }

    virtual void Stop()
    {
        TGuard<TSpinLock> guard(SpinLock);

        if (!Running) {
            return;
        }

        auto error = TTcpDispatcher::TImpl::Get()->AsyncUnregister(Server).Get();
        // Shutdown will hopefully never fail.
        YCHECK(error.IsOK());

        Server.Reset();
        Running = false;
    }

private:
    TTcpBusServerConfigPtr Config;

    TSpinLock SpinLock;
    bool Running;
    TIntrusivePtr<TServer> Server;

};

////////////////////////////////////////////////////////////////////////////////

class TCompositeBusServer
    : public IBusServer
{
public:
    explicit TCompositeBusServer(const std::vector<IBusServerPtr>& servers)
        : Servers(servers)
    { }

    virtual void Start(IMessageHandlerPtr handler) override
    {
        FOREACH (auto server, Servers) {
            server->Start(handler);
        }
    }

    virtual void Stop() override
    {
        FOREACH (auto server, Servers) {
            server->Stop();
        }
    }

private:
    std::vector<IBusServerPtr> Servers;

};

IBusServerPtr CreateTcpBusServer(TTcpBusServerConfigPtr config)
{
    std::vector<IBusServerPtr> servers;
    servers.push_back(New< TTcpBusServerProxy<TTcpBusServer> >(config));
#ifdef _linux_
    servers.push_back(New< TTcpBusServerProxy<TLocalBusServer> >(config));
#endif
    return New<TCompositeBusServer>(servers);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NBus
} // namespace NYT

