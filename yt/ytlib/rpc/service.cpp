#include "stdafx.h"
#include "service.h"
#include "private.h"
#include "dispatcher.h"
#include "server_detail.h"
#include "message.h"

#include <ytlib/misc/string.h>

#include <ytlib/ytree/ypath_client.h>

#include <ytlib/bus/bus.h>

#include <ytlib/rpc/rpc.pb.h>

namespace NYT {
namespace NRpc {

using namespace NBus;
using namespace NYPath;
using namespace NRpc::NProto;

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = RpcServerLogger;
static NProfiling::TProfiler& Profiler = RpcServerProfiler;

////////////////////////////////////////////////////////////////////////////////

TServiceBase::TMethodDescriptor::TMethodDescriptor(
    const Stroka& verb,
    THandler handler)
    : Verb(verb)
    , Handler(MoveRV(handler))
    , OneWay(false)
{ }

TServiceBase::TRuntimeMethodInfo::TRuntimeMethodInfo(
    const TMethodDescriptor& descriptor,
    const TYPath& profilingPath)
    : Descriptor(descriptor)
    , ProfilingPath(profilingPath)
    , RequestCounter(profilingPath + "/request_count")
    , QueueSizeCounter(profilingPath + "/queue_size")
{ }

TServiceBase::TActiveRequest::TActiveRequest(
    const TRequestId& id,
    IBusPtr replyBus,
    TRuntimeMethodInfoPtr runtimeInfo,
    const NProfiling::TTimer& timer)
    : Id(id)
    , ReplyBus(MoveRV(replyBus))
    , RuntimeInfo(runtimeInfo)
    , RunningSync(false)
    , Completed(false)
    , Timer(timer)
{ }

////////////////////////////////////////////////////////////////////////////////

class TServiceBase::TServiceContext
    : public TServiceContextBase
{
public:
    TServiceContext(
        TServiceBasePtr service,
        TActiveRequestPtr activeRequest,
        const NProto::TRequestHeader& header,
        IMessagePtr requestMessage,
        IBusPtr replyBus,
        const Stroka& loggingCategory)
        : TServiceContextBase(header, requestMessage)
        , Service(MoveRV(service))
        , ActiveRequest(MoveRV(activeRequest))
        , ReplyBus(MoveRV(replyBus))
        , Logger(loggingCategory)
    {
        YCHECK(RequestMessage);
        YCHECK(ReplyBus);
        YCHECK(Service);
    }

private:
    TServiceBasePtr Service;
    TActiveRequestPtr ActiveRequest;
    IBusPtr ReplyBus;
    NLog::TLogger Logger;

    virtual void DoReply(IMessagePtr responseMessage) override
    {
        Service->OnResponse(ActiveRequest, MoveRV(responseMessage));
    }

    virtual void LogRequest() override
    {
        Stroka str;
        AppendInfo(str, Sprintf("RequestId: %s", ~RequestId.ToString()));
        AppendInfo(str, RequestInfo);
        LOG_DEBUG("%s <- %s",
            ~Verb,
            ~str);
    }

    virtual void LogResponse(const TError& error) override
    {
        Stroka str;
        AppendInfo(str, Sprintf("RequestId: %s", ~RequestId.ToString()));
        AppendInfo(str, Sprintf("Error: %s", ~ToString(error)));
        AppendInfo(str, ResponseInfo);
        LOG_DEBUG("%s -> %s",
            ~Verb,
            ~str);
    }

};

////////////////////////////////////////////////////////////////////////////////

TServiceBase::TServiceBase(
    IInvokerPtr defaultInvoker,
    const Stroka& serviceName,
    const Stroka& loggingCategory)
    : DefaultInvoker(MoveRV(defaultInvoker))
    , ServiceName(serviceName)
    , LoggingCategory(loggingCategory)
    , RequestCounter("/services/" + ServiceName + "/request_rate")
{
    YCHECK(DefaultInvoker);
}

TServiceBase::~TServiceBase()
{ }

Stroka TServiceBase::GetServiceName() const
{
    return ServiceName;
}

void TServiceBase::OnRequest(
    const TRequestHeader& header,
    IMessagePtr message,
    IBusPtr replyBus)
{
    Profiler.Increment(RequestCounter);

    const auto& verb = header.verb();
    bool oneWay = header.one_way();
    auto requestId = TRequestId::FromProto(header.request_id());

    TGuard<TSpinLock> guard(SpinLock);

    auto runtimeInfo = FindMethodInfo(verb);
    if (!runtimeInfo) {
        guard.Release();

        auto error = TError(
            EErrorCode::NoSuchVerb,
            "Unknown verb %s:%s",
            ~ServiceName,
            ~verb)
            << TErrorAttribute("request_id", requestId);
        LOG_WARNING(error);
        if (!oneWay) {
            auto errorMessage = CreateErrorResponseMessage(requestId, error);
            replyBus->Send(errorMessage);
        }

        return;
    }

    if (runtimeInfo->Descriptor.OneWay != oneWay) {
        guard.Release();

        auto error = TError(
            EErrorCode::ProtocolError,
            "One-way flag mismatch for verb %s:%s: expected %s, actual %s",
            ~ServiceName,
            ~verb,
            ~FormatBool(runtimeInfo->Descriptor.OneWay),
            ~FormatBool(oneWay))
            << TErrorAttribute("request_id", requestId);
        LOG_WARNING(error);
        if (!header.one_way()) {
            auto errorMessage = CreateErrorResponseMessage(requestId, error);
            replyBus->Send(errorMessage);
        }

        return;
    }

    Profiler.Increment(runtimeInfo->RequestCounter, +1);
    auto timer = Profiler.TimingStart(runtimeInfo->ProfilingPath + "/time");

    auto activeRequest = New<TActiveRequest>(
        requestId,
        replyBus,
        runtimeInfo,
        timer);

    auto context = New<TServiceContext>(
        this,
        activeRequest,
        header,
        message,
        replyBus,
        LoggingCategory);

    if (!oneWay) {
        YCHECK(ActiveRequests.insert(activeRequest).second);
        Profiler.Increment(runtimeInfo->QueueSizeCounter, +1);
    }

    guard.Release();

    auto handler = runtimeInfo->Descriptor.Handler;
    const auto& options = runtimeInfo->Descriptor.Options;
    if (options.HeavyRequest) {
        auto invoker = TDispatcher::Get()->GetPoolInvoker();
        handler
            .AsyncVia(MoveRV(invoker))
            .Run(context, options)
            .Subscribe(BIND(
                &TServiceBase::OnInvocationPrepared,
                MakeStrong(this),
                MoveRV(activeRequest),
                context));
    } else {
        auto preparedHandler = handler.Run(context, options);
        OnInvocationPrepared(
            MoveRV(activeRequest),
            MoveRV(context),
            MoveRV(preparedHandler));
    }
}

void TServiceBase::OnInvocationPrepared(
    TActiveRequestPtr activeRequest,
    IServiceContextPtr context,
    TClosure handler)
{
    auto preparedHandler = PrepareHandler(context, MoveRV(handler));

    auto wrappedHandler = BIND([=] () {
        auto& timer = activeRequest->Timer;
        auto& runtimeInfo = activeRequest->RuntimeInfo;

        {
            // No need for a lock here.
            activeRequest->RunningSync = true;
            Profiler.TimingCheckpoint(timer, "wait");
        }

        preparedHandler.Run();

        {
            TGuard<TSpinLock> guard(activeRequest->SpinLock);

            YCHECK(activeRequest->RunningSync);
            activeRequest->RunningSync = false;

            if (!activeRequest->Completed) {
                Profiler.TimingCheckpoint(timer, "sync");
            }

            if (runtimeInfo->Descriptor.OneWay) {
                Profiler.TimingStop(timer);
            }
        }
    });

    const auto& runtimeInfo = activeRequest->RuntimeInfo;
    auto invoker = runtimeInfo->Descriptor.Invoker;
    if (!invoker) {
        invoker = DefaultInvoker;
    }

    if (!invoker->Invoke(MoveRV(wrappedHandler))) {
        context->Reply(TError(EErrorCode::Unavailable, "Service unavailable"));
    }
}

TClosure TServiceBase::PrepareHandler(
    IServiceContextPtr context,
    TClosure handler)
{
    return context->Wrap(handler);
}

void TServiceBase::OnResponse(TActiveRequestPtr activeRequest, IMessagePtr message)
{
    bool active;
    {
        TGuard<TSpinLock> guard(SpinLock);

        active = ActiveRequests.erase(activeRequest) == 1;
    }

    {
        TGuard<TSpinLock> guard(activeRequest->SpinLock);

        YCHECK(!activeRequest->Completed);
        activeRequest->Completed = true;

        if (active) {
            Profiler.Increment(activeRequest->RuntimeInfo->QueueSizeCounter, -1);
            activeRequest->ReplyBus->Send(MoveRV(message));
        }

        auto& timer = activeRequest->Timer;

        if (activeRequest->RunningSync) {
            Profiler.TimingCheckpoint(timer, "sync");
        }
        Profiler.TimingCheckpoint(timer, "async");
        Profiler.TimingStop(timer);
    }
}

void TServiceBase::RegisterMethod(const TMethodDescriptor& descriptor)
{
    TGuard<TSpinLock> guard(SpinLock);
    auto path = "/services/" + ServiceName + "/methods/" +  descriptor.Verb;
    auto info = New<TRuntimeMethodInfo>(descriptor, path);
    // Failure here means that such verb is already registered.
    YCHECK(RuntimeMethodInfos.insert(std::make_pair(descriptor.Verb, info)).second);
}

void TServiceBase::CancelActiveRequests(const TError& error)
{
    yhash_set<TActiveRequestPtr> requestsToCancel;
    {
        TGuard<TSpinLock> guard(SpinLock);
        requestsToCancel.swap(ActiveRequests);
    }

    FOREACH (auto activeRequest, requestsToCancel) {
        Profiler.Increment(activeRequest->RuntimeInfo->QueueSizeCounter, -1);

        auto errorMessage = CreateErrorResponseMessage(activeRequest->Id, error);
        activeRequest->ReplyBus->Send(errorMessage);
    }
}

TServiceBase::TRuntimeMethodInfoPtr TServiceBase::FindMethodInfo(const Stroka& method)
{
    auto it = RuntimeMethodInfos.find(method);
    return it == RuntimeMethodInfos.end() ? NULL : it->second;
}

TServiceBase::TRuntimeMethodInfoPtr TServiceBase::GetMethodInfo(const Stroka& method)
{
    auto info = FindMethodInfo(method);
    YCHECK(info);
    return info;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NRpc
} // namespace NYT
