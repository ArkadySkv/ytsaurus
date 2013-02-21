#include "stdafx.h"
#include "object_service.h"
#include "private.h"
#include "object_manager.h"
#include "config.h"

#include <ytlib/ytree/ypath_detail.h>

#include <ytlib/rpc/message.h>

#include <ytlib/actions/parallel_awaiter.h>

#include <server/transaction_server/transaction.h>
#include <server/transaction_server/transaction_manager.h>

#include <server/cell_master/bootstrap.h>
#include <server/cell_master/meta_state_facade.h>

namespace NYT {
namespace NObjectServer {

using namespace NMetaState;
using namespace NRpc;
using namespace NBus;
using namespace NYTree;
using namespace NCypressServer;
using namespace NTransactionServer;
using namespace NCellMaster;

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = ObjectServerLogger;

////////////////////////////////////////////////////////////////////////////////

class TObjectService::TExecuteSession
    : public TIntrinsicRefCounted
{
public:
    TExecuteSession(TObjectService* owner, TCtxExecutePtr context)
        : Owner(owner)
        , Context(context)
        , Awaiter(New<TParallelAwaiter>())
        , ReplyLock(0)
        , CurrentRequestIndex(0)
        , CurrentRequestPartIndex(0)
    {
        auto& request = Context->Request();
        int requestCount = request.part_counts_size();
        Context->SetRequestInfo("RequestCount: %d", requestCount);
        ResponseMessages.resize(requestCount);
    }

    void Run()
    {
        Continue();
    }

private:
    TObjectService* Owner;
    TCtxExecutePtr Context;

    TParallelAwaiterPtr Awaiter;
    std::vector<IMessagePtr> ResponseMessages;
    TAtomic ReplyLock;
    int CurrentRequestIndex;
    int CurrentRequestPartIndex;

    void Continue()
    {
        auto startTime = TInstant::Now();
        auto& request = Context->Request();
        const auto& attachments = request.Attachments();
        auto rootService = Owner->Bootstrap->GetObjectManager()->GetRootService();

        auto awaiter = Awaiter;
        if (!awaiter)
            return;

        // Validate prerequisite transactions.
        auto transactionManager = Owner->Bootstrap->GetTransactionManager();
        FOREACH (const auto& protoId, request.prerequisite_transaction_ids()) {
            auto id = TTransactionId::FromProto(protoId);
            const auto* transaction = transactionManager->FindTransaction(id);
            if (!transaction) {
                Reply(TError(
                    "Prerequisite transaction is missing: %s",
                    ~ToString(id)));
                return;
            }
            if (transaction->GetState() != ETransactionState::Active) {
                Reply(TError(
                    "Prerequisite transaction is not active: %s",
                    ~ToString(id)));
                return;
            }
        }

        // Execute another portion of requests.
        while (CurrentRequestIndex < request.part_counts_size()) {
            int partCount = request.part_counts(CurrentRequestIndex);
            if (partCount == 0) {
                // Skip empty requests.
                ++CurrentRequestIndex;
                continue;
            }

            std::vector<TSharedRef> requestParts(
                attachments.begin() + CurrentRequestPartIndex,
                attachments.begin() + CurrentRequestPartIndex + partCount);
            auto requestMessage = CreateMessageFromParts(std::move(requestParts));

            NRpc::NProto::TRequestHeader requestHeader;
            if (!ParseRequestHeader(requestMessage, &requestHeader)) {
                Reply(TError(
                    EErrorCode::ProtocolError,
                    "Error parsing request header"));
                return;
            }

            const auto& path = requestHeader.path();
            const auto& verb = requestHeader.verb();

            if (AtomicGet(ReplyLock) != 0)
                return;

            LOG_DEBUG("Execute[%d] <- %s %s",
                CurrentRequestIndex,
                ~verb,
                ~path);

            awaiter->Await(
                ExecuteVerb(rootService, requestMessage),
                BIND(&TExecuteSession::OnResponse, MakeStrong(this), CurrentRequestIndex));

            ++CurrentRequestIndex;
            CurrentRequestPartIndex += partCount;

            if (TInstant::Now() > startTime + Owner->Config->YieldTimeout) {
                YieldAndContinue();
                return;
            }
        }

        awaiter->Complete().Subscribe(BIND(&TExecuteSession::OnComplete, MakeStrong(this)));
    }

    void YieldAndContinue()
    {
        LOG_DEBUG("Yielding state thread (RequestId: %s)",
            ~Context->GetUntypedContext()->GetRequestId().ToString());

        auto invoker = Owner->Bootstrap->GetMetaStateFacade()->GetGuardedInvoker();
        if (!invoker->Invoke(BIND(&TExecuteSession::Continue, MakeStrong(this)))) {
            Reply(TError(
                EErrorCode::Unavailable,
                "Yield error, only %d of out %d requests were served",
                CurrentRequestIndex,
                Context->Request().part_counts_size()));
        }
    }

    void OnResponse(int requestIndex, IMessagePtr responseMessage)
    {
        NRpc::NProto::TResponseHeader responseHeader;
        YCHECK(ParseResponseHeader(responseMessage, &responseHeader));

        auto error = FromProto(responseHeader.error());

        LOG_DEBUG("Execute[%d] -> Error: %s",
            requestIndex,
            ~ToString(error));

        if (error.GetCode() == EErrorCode::Unavailable) {
            Reply(error);
        } else {
            // No sync is needed, requestIndexes are distinct.
            ResponseMessages[requestIndex] = responseMessage;
        }
    }

    void OnComplete()
    {
        // No sync is needed: OnComplete is called after all OnResponses.
        auto& response = Context->Response();

        FOREACH (const auto& responseMessage, ResponseMessages) {
            if (!responseMessage) {
                // Skip empty responses.
                response.add_part_counts(0);
                continue;
            }

            const auto& responseParts = responseMessage->GetParts();
            response.add_part_counts(static_cast<int>(responseParts.size()));
            response.Attachments().insert(
                response.Attachments().end(),
                responseParts.begin(),
                responseParts.end());
        }

        Reply(TError());
    }

    void Reply(const TError& error)
    {
        // Make sure that we only reply once.
        if (!AtomicTryLock(&ReplyLock))
            return;

        Awaiter->Cancel();
        Awaiter.Reset();

        Context->Reply(error);
    }

};

////////////////////////////////////////////////////////////////////////////////

TObjectService::TObjectService(
    TObjectManagerConfigPtr config,
    TBootstrap* bootstrap)
    : TMetaStateServiceBase(
        bootstrap,
        NObjectClient::TObjectServiceProxy::GetServiceName(),
        ObjectServerLogger.GetCategory())
    , Config(config)
{
    RegisterMethod(RPC_SERVICE_METHOD_DESC(Execute));
    RegisterMethod(RPC_SERVICE_METHOD_DESC(GCCollect));
}

DEFINE_RPC_SERVICE_METHOD(TObjectService, Execute)
{
    UNUSED(request);
    UNUSED(response);

    New<TExecuteSession>(this, context)->Run();
}

DEFINE_RPC_SERVICE_METHOD(TObjectService, GCCollect)
{
    UNUSED(request);
    UNUSED(response);

    Bootstrap->GetObjectManager()->GCCollect().Subscribe(BIND([=] () {
        context->Reply();
    }));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NObjectServer
} // namespace NYT
