#include "stdafx.h"
#include "scheduler_service.h"
#include "scheduler.h"
#include "private.h"

#include <ytlib/rpc/service_detail.h>

#include <ytlib/security_client/rpc_helpers.h>

#include <ytlib/scheduler/scheduler_service_proxy.h>

#include <server/cell_scheduler/bootstrap.h>

namespace NYT {
namespace NScheduler {

using namespace NRpc;
using namespace NCellScheduler;
using namespace NTransactionClient;
using namespace NSecurityClient;
using namespace NYTree;

////////////////////////////////////////////////////////////////////

class TSchedulerService
    : public TServiceBase
{
public:
    TSchedulerService(TBootstrap* bootstrap)
        : TServiceBase(
            bootstrap->GetControlInvoker(),
            TSchedulerServiceProxy::GetServiceName(),
            SchedulerLogger.GetCategory())
        , Bootstrap(bootstrap)
    {
        RegisterMethod(RPC_SERVICE_METHOD_DESC(StartOperation));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(AbortOperation));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(WaitForOperation));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(SuspendOperation));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(ResumeOperation));
    }

private:
    typedef TSchedulerService TThis;

    TBootstrap* Bootstrap;

    DECLARE_RPC_SERVICE_METHOD(NProto, StartOperation)
    {
        auto type = EOperationType(request->type());
        auto transactionId = FromProto<TTransactionId>(request->transaction_id());
        auto mutationId = FromProto<NMetaState::TMutationId>(request->mutation_id());

        auto maybeUser = FindAuthenticatedUser(context);
        auto user = maybeUser ? *maybeUser : RootUserName;

        IMapNodePtr spec;
        try {
            spec = ConvertToNode(TYsonString(request->spec()))->AsMap();
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error parsing operation spec")
                << ex;
        }

        context->SetRequestInfo("Type: %s, TransactionId: %s, MutationId: %s",
            ~type.ToString(),
            ~ToString(transactionId),
            ~ToString(mutationId));

        auto scheduler = Bootstrap->GetScheduler();
        scheduler->ValidateConnected();
        scheduler->StartOperation(
            type,
            transactionId,
            mutationId,
            spec,
            user)
            .Subscribe(BIND([=] (TErrorOr<TOperationPtr> result) {
                if (!result.IsOK()) {
                    context->Reply(result);
                    return;
                }
                auto operation = result.Value();
                auto id = operation->GetOperationId();
                ToProto(response->mutable_operation_id(), id);
                context->SetResponseInfo("OperationId: %s", ~ToString(id));
                context->Reply();
            }));
    }

    DECLARE_RPC_SERVICE_METHOD(NProto, AbortOperation)
    {
        auto operationId = FromProto<TOperationId>(request->operation_id());

        context->SetRequestInfo("OperationId: %s", ~ToString(operationId));

        auto scheduler = Bootstrap->GetScheduler();
        scheduler->ValidateConnected();

        auto operation = scheduler->GetOperationOrThrow(operationId);
        scheduler
            ->AbortOperation(
                operation,
                TError("Operation aborted by user request"))
            .Subscribe(BIND([=] () {
                context->Reply();
            }));
    }

    DECLARE_RPC_SERVICE_METHOD(NProto, SuspendOperation)
    {
        auto operationId = FromProto<TOperationId>(request->operation_id());

        context->SetRequestInfo("OperationId: %s", ~ToString(operationId));

        auto scheduler = Bootstrap->GetScheduler();
        scheduler->ValidateConnected();

        auto operation = scheduler->GetOperationOrThrow(operationId);
        scheduler
            ->SuspendOperation(operation)
            .Subscribe(BIND([=] (TError error) {
                context->Reply(error);
            }));
    }

    DECLARE_RPC_SERVICE_METHOD(NProto, ResumeOperation)
    {
        auto operationId = FromProto<TOperationId>(request->operation_id());

        context->SetRequestInfo("OperationId: %s", ~ToString(operationId));

        auto scheduler = Bootstrap->GetScheduler();
        scheduler->ValidateConnected();

        auto operation = scheduler->GetOperationOrThrow(operationId);
        scheduler
            ->ResumeOperation(operation)
            .Subscribe(BIND([=] (TError error) {
                context->Reply(error);
            }));
    }

    DECLARE_RPC_SERVICE_METHOD(NProto, WaitForOperation)
    {
        auto operationId = FromProto<TOperationId>(request->operation_id());
        auto timeout = TDuration(request->timeout());
        context->SetRequestInfo("OperationId: %s, Timeout: %s",
            ~ToString(operationId),
            ~ToString(timeout));

        auto scheduler = Bootstrap->GetScheduler();
        scheduler->ValidateConnected();

        auto operation = scheduler->GetOperationOrThrow(operationId);
        auto this_ = MakeStrong(this);
        operation->GetFinished().Subscribe(
            timeout,
            BIND(&TThis::OnOperationWaitResult, this_, context, operation, true),
            BIND(&TThis::OnOperationWaitResult, this_, context, operation, false));
    }

    void OnOperationWaitResult(
        TCtxWaitForOperationPtr context,
        TOperationPtr operation,
        bool maybeFinished)
    {
        context->SetResponseInfo("MaybeFinished: %s", ~FormatBool(maybeFinished));
        context->Response().set_maybe_finished(maybeFinished);
        context->Reply();
    }

};

IServicePtr CreateSchedulerService(TBootstrap* bootstrap)
{
    return New<TSchedulerService>(bootstrap);
}

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

