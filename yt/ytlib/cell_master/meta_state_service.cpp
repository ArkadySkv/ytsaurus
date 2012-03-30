#include "stdafx.h"
#include "meta_state_service.h"
#include "bootstrap.h"
#include "world_initializer.h"

namespace NYT {
namespace NCellMaster {

using namespace NMetaState;

////////////////////////////////////////////////////////////////////////////////

TMetaStateServiceBase::TMetaStateServiceBase(
    TBootstrap* bootstrap,
    const Stroka& serviceName,
    const Stroka& loggingCategory)
    : NRpc::TServiceBase(
        ~bootstrap->GetStateInvoker(),
        serviceName,
        loggingCategory)
    , Bootstrap(bootstrap)
{
    YASSERT(bootstrap);
}

void TMetaStateServiceBase::InvokeHandler(
    TRuntimeMethodInfo* runtimeInfo,
    const TClosure& handler,
    NRpc::IServiceContext* context)
{
    if (Bootstrap->GetMetaStateManager()->GetStateStatusAsync() != EPeerStatus::Leading) {
        context->Reply(TError(NRpc::EErrorCode::Unavailable, "Not an active leader"));
        return;
    }

    NRpc::IServiceContext::TPtr context_ = context;
    runtimeInfo->Invoker->Invoke(BIND([=] ()
        {
            if (Bootstrap->GetMetaStateManager()->GetStateStatusAsync() != EPeerStatus::Leading ||
                !Bootstrap->GetMetaStateManager()->HasActiveQuorum())
            {
                context_->Reply(TError(NRpc::EErrorCode::Unavailable, "Not an active leader"));
                return;
            }

            if (!Bootstrap->GetWorldInitializer()->IsInitialized()) {
                context_->Reply(TError(NRpc::EErrorCode::Unavailable, "Cell is not initialized yet, please try again later"));
                return;
            }

            handler.Run();
        }));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NCellMaster
} // namespace NYT
