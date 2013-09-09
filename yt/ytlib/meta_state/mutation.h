#pragma once

#include "public.h"
#include "mutation_context.h"

#include <core/misc/error.h>
#include <core/actions/cancelable_context.h>

namespace NYT {
namespace NMetaState {

////////////////////////////////////////////////////////////////////////////////

class TMutation
    : public TIntrinsicRefCounted
{
public:
    TMutation(
        IMetaStateManagerPtr metaStateManager,
        IInvokerPtr stateInvoker);

    void Commit();
    bool PostCommit();

    TMutationPtr SetType(const Stroka& type);

    TMutationPtr SetId(const TMutationId& id);

    TMutationPtr SetRequestData(const TSharedRef& data);
    template <class TRequest>
    TMutationPtr SetRequestData(const TRequest& request);

    TMutationPtr SetAction(TClosure action);

    TMutationPtr OnSuccess(TClosure onSuccess);
    TMutationPtr OnSuccess(TCallback<void(const TMutationResponse&)> onSuccess);
    template <class TResponse>
    TMutationPtr OnSuccess(TCallback<void(const TResponse&)> onSuccess);

    TMutationPtr OnError(TCallback<void(const TError&)> onError);

private:
    IMetaStateManagerPtr MetaStateManager;
    IInvokerPtr StateInvoker;

    TMutationRequest Request;
    TCallback<void(const TMutationResponse&)> OnSuccess_;
    TCallback<void(const TError&)> OnError_;

    void OnCommitted(TErrorOr<TMutationResponse> response);

};

template <class TTarget, class TRequest, class TResponse>
TMutationPtr CreateMutation(
    IMetaStateManagerPtr metaStateManager,
    IInvokerPtr invoker,
    TTarget* target,
    const TRequest& request,
    TResponse (TTarget::* method)(const TRequest& request));

////////////////////////////////////////////////////////////////////////////////

} // namespace NMetaState
} // namespace NYT

#define MUTATION_INL_H_
#include "mutation-inl.h"
#undef MUTATION_INL_H_
