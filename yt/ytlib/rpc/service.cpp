#include "stdafx.h"
#include "service.h"
#include "rpc.pb.h"

#include <ytlib/logging/log.h>
#include <ytlib/ytree/ypath_client.h>

namespace NYT {
namespace NRpc {

using namespace NBus;
using namespace NProto;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger Logger("Rpc");
static NProfiling::TProfiler Profiler("rpc/server");

////////////////////////////////////////////////////////////////////////////////

void IServiceContext::Reply(NBus::IMessage* message)
{
    auto parts = message->GetParts();
    YASSERT(!parts.empty());

    TResponseHeader header;
    if (!DeserializeProtobuf(&header, parts[0])) {
        LOG_FATAL("Error deserializing response header");
    }

    TError error(
        header.error_code(),
        header.has_error_message() ? header.error_message() : "");

    if (error.IsOK()) {
        YASSERT(parts.ysize() >= 2);

        SetResponseBody(parts[1]);

        parts.erase(parts.begin(), parts.begin() + 2);
        ResponseAttachments() = MoveRV(parts);
    }

    Reply(error);
}

////////////////////////////////////////////////////////////////////////////////

TServiceBase::TRuntimeMethodInfo::TRuntimeMethodInfo(
    const TMethodDescriptor& info,
    IInvoker* invoker,
    const NYTree::TYPath& path)
    : Descriptor(info)
    , Invoker(invoker)
    , RequestCounter(CombineYPaths(path, "request_rate"))
{ }

////////////////////////////////////////////////////////////////////////////////

TServiceBase::TServiceBase(
    IInvoker* defaultInvoker,
    const Stroka& serviceName,
    const Stroka& loggingCategory)
    : DefaultInvoker(defaultInvoker)
    , ServiceName(serviceName)
    , LoggingCategory(loggingCategory)
    , RequestCounter(CombineYPaths(ServiceName, "request_rate"))
{
    YASSERT(defaultInvoker);
}

TServiceBase::~TServiceBase()
{ }

Stroka TServiceBase::GetServiceName() const
{
    return ServiceName;
}

Stroka TServiceBase::GetLoggingCategory() const
{
    return LoggingCategory;
}

void TServiceBase::OnBeginRequest(IServiceContext* context)
{
    YASSERT(context);

    Profiler.Increment(RequestCounter);

    Stroka verb = context->GetVerb();

    TGuard<TSpinLock> guard(SpinLock);

    auto methodIt = RuntimeMethodInfos.find(verb);
    if (methodIt == RuntimeMethodInfos.end()) {
        guard.Release();

        Stroka message = Sprintf("Verb %s is not registered (ServiceName: %s)",
            ~verb.Quote(),
            ~ServiceName);
        LOG_WARNING("%s", ~message);
        if (!context->IsOneWay()) {
            context->Reply(TError(EErrorCode::NoSuchVerb, message));
        }

        return;
    }

    auto runtimeInfo = methodIt->second;
    if (runtimeInfo->Descriptor.OneWay != context->IsOneWay()) {
        guard.Release();

        Stroka message = Sprintf("One-way flag mismatch (Expected: %s, Actual: %s, ServiceName: %s, Verb: %s)",
            ~ToString(runtimeInfo->Descriptor.OneWay),
            ~ToString(context->IsOneWay()),
            ~ServiceName,
            ~verb);
        LOG_WARNING("%s", ~message);
        if (!context->IsOneWay()) {
            context->Reply(TError(EErrorCode::NoSuchVerb, message));
        }

        return;
    }

    Profiler.Increment(runtimeInfo->RequestCounter);
    auto timer = Profiler.TimingStart(CombineYPaths(
        ServiceName,
        "methods",
        context->GetVerb(),
        "time"));

    auto activeRequest = New<TActiveRequest>(runtimeInfo, timer);

    if (!context->IsOneWay()) {
        YVERIFY(ActiveRequests.insert(MakePair(context, activeRequest)).second);
    }

    guard.Release();

    auto handler = runtimeInfo->Descriptor.Handler;
    auto guardedHandler = context->Wrap(~handler->Bind(context));
    auto wrappedHandler = FromFunctor([=] () {
        auto& timer = activeRequest->Timer;

        {
            // No need for a lock here.
            activeRequest->RunningSync = true;
            Profiler.TimingCheckpoint(timer, "wait");
        }

        guardedHandler->Do();

        {
            TGuard<TSpinLock> guard(activeRequest->SpinLock);

            YASSERT(activeRequest->RunningSync);
            activeRequest->RunningSync = false;

            if (!activeRequest->Completed) {
                Profiler.TimingCheckpoint(timer, "sync");
            }

            if (runtimeInfo->Descriptor.OneWay) {
                Profiler.TimingStop(timer);
            }
        }
    });

    InvokeHandler(~runtimeInfo, wrappedHandler, context);
}

void TServiceBase::OnEndRequest(IServiceContext* context)
{
    YASSERT(context);
    YASSERT(!context->IsOneWay());

    TGuard<TSpinLock> guard(SpinLock);

    auto it = ActiveRequests.find(context);
    if (it == ActiveRequests.end())
        return;

    auto& activeRequest = it->second;

    {
        TGuard<TSpinLock> guard(activeRequest->SpinLock);

        YASSERT(!activeRequest->Completed);
        activeRequest->Completed = true;

        auto& timer = activeRequest->Timer;

        if (activeRequest->RunningSync) {
            Profiler.TimingCheckpoint(timer, "sync");
        }
        Profiler.TimingCheckpoint(timer, "async");
        Profiler.TimingStop(timer);
    }

    ActiveRequests.erase(it);
}

void TServiceBase::RegisterMethod(const TMethodDescriptor& descriptor)
{
    RegisterMethod(descriptor, ~DefaultInvoker);
}

void TServiceBase::RegisterMethod(const TMethodDescriptor& descriptor, IInvoker* invoker)
{
    YASSERT(invoker);

    TGuard<TSpinLock> guard(SpinLock);
    auto info = New<TRuntimeMethodInfo>(
        descriptor,
        invoker,
        CombineYPaths(ServiceName, "methods", descriptor.Verb));
    // Failure here means that such verb is already registered.
    YVERIFY(RuntimeMethodInfos.insert(MakePair(descriptor.Verb, info)).second);
}

void TServiceBase::InvokeHandler(
    TRuntimeMethodInfo* runtimeInfo,
    IAction::TPtr handler,
    IServiceContext* context)
{
    UNUSED(context);

    runtimeInfo->Invoker->Invoke(handler);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NRpc
} // namespace NYT
