#ifndef MUTATION_INL_H_
#error "Direct inclusion of this file is not allowed, include mutation.h"
#endif

#include "meta_state_manager.h"

#include <core/misc/protobuf_helpers.h>

namespace NYT {
namespace NMetaState {

////////////////////////////////////////////////////////////////////////////////

template <class TResponse>
TMutationPtr TMutation::OnSuccess(TCallback<void(const TResponse&)> onSuccess)
{
    YASSERT(!OnSuccess_);
    OnSuccess_ = BIND([=] (const TMutationResponse& mutationResponse) {
        TResponse response;
        YCHECK(DeserializeFromProtoWithEnvelope(&response, mutationResponse.Data));
        onSuccess.Run(response);
    });
    return this;
}

template <class TRequest>
TMutationPtr TMutation::SetRequestData(const TRequest& request)
{
    TSharedRef requestData;
    YCHECK(SerializeToProtoWithEnvelope(request, &requestData));
    SetRequestData(requestData);
    Request.Type = request.GetTypeName();
    return this;
}

template <class TResponse>
struct TMutationFactory
{
    template <class TTarget, class TRequest>
    static TMutationPtr Create(
        IMetaStateManagerPtr metaStateManager,
        IInvokerPtr invoker,
        TTarget* target,
        const TRequest& request,
        TResponse (TTarget::* method)(const TRequest& request))
    {
        return
            New<TMutation>(std::move(metaStateManager), std::move(invoker))
            ->SetRequestData(request)
            ->SetAction(BIND([=] () {
                TResponse response((target->*method)(request));

                TSharedRef responseData;
                YCHECK(SerializeToProtoWithEnvelope(response, &responseData));

                auto* context = metaStateManager->GetMutationContext();
                YASSERT(context);

                context->SetResponseData(responseData);
            }));
    }
};

template <>
struct TMutationFactory<void>
{
    template <class TTarget, class TRequest>
    static TMutationPtr Create(
        IMetaStateManagerPtr metaStateManager,
        IInvokerPtr invoker,
        TTarget* target,
        const TRequest& request,
        void (TTarget::* method)(const TRequest& request))
    {
        return
            New<TMutation>(std::move(metaStateManager), std::move(invoker))
            ->SetRequestData(request)
            ->SetAction(BIND(method, Unretained(target), request));
    }
};

template <class TTarget, class TRequest, class TResponse>
TMutationPtr CreateMutation(
    IMetaStateManagerPtr metaStateManager,
    IInvokerPtr invoker,
    TTarget* target,
    const TRequest& request,
    TResponse (TTarget::* method)(const TRequest& request))
{
    return TMutationFactory<TResponse>::Create(
        std::move(metaStateManager),
        std::move(invoker),
        target,
        request,
        method);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NMetaState
} // namespace NYT
