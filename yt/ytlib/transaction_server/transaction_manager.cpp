#include "stdafx.h"
#include "transaction_manager.h"
#include "common.h"
#include "transaction.h"
#include "transaction_ypath_proxy.h"
#include "transaction_ypath.pb.h"

#include <ytlib/cell_master/load_context.h>
#include <ytlib/misc/string.h>
#include <ytlib/ytree/ypath_client.h>
#include <ytlib/ytree/ephemeral.h>
#include <ytlib/ytree/serialize.h>
#include <ytlib/ytree/fluent.h>
#include <ytlib/cypress/cypress_manager.h>
#include <ytlib/cypress/cypress_service_proxy.h>
#include <ytlib/object_server/type_handler_detail.h>

namespace NYT {
namespace NTransactionServer {

using namespace NCellMaster;
using namespace NObjectServer;
using namespace NMetaState;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = TransactionServerLogger;

////////////////////////////////////////////////////////////////////////////////

class TTransactionManager::TTransactionProxy
    : public NObjectServer::TUnversionedObjectProxyBase<TTransaction>
{
public:
    TTransactionProxy(TTransactionManager* owner, const TTransactionId& id)
        : TBase(~owner->ObjectManager, id, &owner->TransactionMap)
        , TYPathServiceBase(TransactionServerLogger.GetCategory())
        , Owner(owner)
    { }

    virtual bool IsWriteRequest(NRpc::IServiceContext* context) const
    {
        DECLARE_YPATH_SERVICE_WRITE_METHOD(Commit);
        DECLARE_YPATH_SERVICE_WRITE_METHOD(Abort);
        DECLARE_YPATH_SERVICE_WRITE_METHOD(CreateObject);
        DECLARE_YPATH_SERVICE_WRITE_METHOD(ReleaseObject);
        return TBase::IsWriteRequest(context);
    }

private:
    typedef TUnversionedObjectProxyBase<TTransaction> TBase;

    TTransactionManager::TPtr Owner;

    virtual void GetSystemAttributes(std::vector<TAttributeInfo>* attributes)
    {
        attributes->push_back("state");
        attributes->push_back("parent_id");
        attributes->push_back("nested_transaction_ids");
        attributes->push_back("created_object_ids");
        TBase::GetSystemAttributes(attributes);
    }

    virtual bool GetSystemAttribute(const Stroka& name, NYTree::IYsonConsumer* consumer)
    {
        const auto& transaction = GetTypedImpl();
        
        if (name == "state") {
            BuildYsonFluently(consumer)
                .Scalar(CamelCaseToUnderscoreCase(transaction.GetState().ToString()));
            return true;
        }

        if (name == "parent_id") {
            BuildYsonFluently(consumer)
                .Scalar(transaction.GetParentId().ToString());
            return true;
        }

        if (name == "nested_transaction_ids") {
            BuildYsonFluently(consumer)
                .DoListFor(transaction.NestedTransactions(), [=] (TFluentList fluent, TTransaction* transaction)
                    {
                        fluent.Item().Scalar(transaction->GetId().ToString());
                    });
            return true;
        }

        if (name == "created_object_ids") {
            BuildYsonFluently(consumer)
                .DoListFor(transaction.CreatedObjectIds(), [=] (TFluentList fluent, TTransactionId id)
            {
                fluent.Item().Scalar(id.ToString());
            });
            return true;
        }

        return TBase::GetSystemAttribute(name, consumer);
    }

    virtual void DoInvoke(NRpc::IServiceContext* context)
    {
        DISPATCH_YPATH_SERVICE_METHOD(Commit);
        DISPATCH_YPATH_SERVICE_METHOD(Abort);
        DISPATCH_YPATH_SERVICE_METHOD(RenewLease);
        DISPATCH_YPATH_SERVICE_METHOD(CreateObject);
        DISPATCH_YPATH_SERVICE_METHOD(ReleaseObject);
        TBase::DoInvoke(context);
    }

    DECLARE_RPC_SERVICE_METHOD(NProto, Commit)
    {
        UNUSED(request);
        UNUSED(response);

        Owner->Commit(GetTypedImpl());
        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NProto, Abort)
    {
        UNUSED(request);
        UNUSED(response);

        Owner->Abort(GetTypedImpl());
        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NProto, RenewLease)
    {
        UNUSED(request);
        UNUSED(response);

        Owner->RenewLease(GetId());
        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NProto, CreateObject)
    {
        auto type = EObjectType(request->type());

        context->SetRequestInfo("TransactionId: %s, Type: %s",
            ~GetId().ToString(),
            ~type.ToString());

        auto handler = Owner->ObjectManager->FindHandler(type);
        if (!handler) {
            ythrow yexception() << "Unknown object type";
        }

        NYTree::INodePtr manifestNode =
            request->has_manifest()
            ? DeserializeFromYson(request->manifest())
            : GetEphemeralNodeFactory()->CreateMap();

        if (manifestNode->GetType() != NYTree::ENodeType::Map) {
            ythrow yexception() << "Manifest must be a map";
        }

        if (handler->IsTransactionRequired() && GetId() == NullTransactionId) {
            ythrow yexception() << Sprintf("Cannot create an instance outside of a transaction (Type: %s)",
                ~type.ToString());
        }

        auto objectId = handler->CreateFromManifest(
            GetId(),
            ~manifestNode->AsMap());

        if (GetId() != NullTransactionId) {
            auto& transaction = GetTypedImpl();
            YVERIFY(transaction.CreatedObjectIds().insert(objectId).second);
            Owner->ObjectManager->RefObject(objectId);
        }

        response->set_object_id(objectId.ToProto());

        context->SetResponseInfo("ObjectId: %s", ~objectId.ToString());

        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NTransactionServer::NProto, ReleaseObject)
    {
        UNUSED(response);

        auto objectId = TObjectId::FromProto(request->object_id());

        context->SetRequestInfo("ObjectId: %s", ~objectId.ToString());

        auto& transaction = GetTypedImpl();
        if (transaction.CreatedObjectIds().erase(objectId) != 1) {
            ythrow yexception() << Sprintf("Transaction does not own the object (ObjectId: %s)", ~objectId.ToString());
        }

        Owner->ObjectManager->UnrefObject(objectId);

        context->Reply();
    }
};

////////////////////////////////////////////////////////////////////////////////

class TTransactionManager::TTransactionTypeHandler
    : public TObjectTypeHandlerBase<TTransaction>
{
public:
    TTransactionTypeHandler(TTransactionManager* owner)
        : TObjectTypeHandlerBase(~owner->ObjectManager, &owner->TransactionMap)
        , Owner(owner)
    { }

    virtual EObjectType GetType()
    {
        return EObjectType::Transaction;
    }

    virtual TObjectId CreateFromManifest(
        const TTransactionId& transactionId,
        IMapNode* manifestNode)
    {
        auto manifest = New<TTransactionManifest>();
        manifest->LoadAndValidate(manifestNode);

        auto* parent =
            transactionId == NullTransactionId
            ? NULL
            : &Owner->GetTransaction(transactionId);

        auto id = Owner->Start(parent, ~manifest).GetId();
        auto proxy = ObjectManager->GetProxy(id);
        proxy->Attributes().MergeFrom(manifestNode);
        return id;
    }

    virtual IObjectProxy::TPtr GetProxy(const TVersionedObjectId& id)
    {
        return New<TTransactionProxy>(Owner, id.ObjectId);
    }

    virtual bool IsTransactionRequired() const
    {
        return false;
    }

private:
    TTransactionManager* Owner;
};

////////////////////////////////////////////////////////////////////////////////

TTransactionManager::TTransactionManager(
    TConfig* config,
    IMetaStateManager* metaStateManager,
    TCompositeMetaState* metaState,
    TObjectManager* objectManager)
    : TMetaStatePart(metaStateManager, metaState)
    , Config(config)
    , ObjectManager(objectManager)
{
    YASSERT(metaStateManager);
    YASSERT(metaState);
    YASSERT(objectManager);

    TLoadContext context(NULL); // TODO(roizner): use real bootstrap

    metaState->RegisterLoader(
        "TransactionManager.Keys.1",
        FromMethod(&TTransactionManager::LoadKeys, TPtr(this)));
    metaState->RegisterLoader(
        "TransactionManager.Values.1",
        FromMethod(&TTransactionManager::LoadValues, TPtr(this), context));
    metaState->RegisterSaver(
        "TransactionManager.Keys.1",
        FromMethod(&TTransactionManager::SaveKeys, TPtr(this)),
        ESavePhase::Keys);
    metaState->RegisterSaver(
        "TransactionManager.Values.1",
        FromMethod(&TTransactionManager::SaveValues, TPtr(this)),
        ESavePhase::Values);

    metaState->RegisterPart(this);

    objectManager->RegisterHandler(~New<TTransactionTypeHandler>(this));

    VERIFY_INVOKER_AFFINITY(metaStateManager->GetStateInvoker(), StateThread);
}

TTransaction& TTransactionManager::Start(TTransaction* parent, TTransactionManifest* manifest)
{
    VERIFY_THREAD_AFFINITY(StateThread);
    YASSERT(manifest);

    auto id = ObjectManager->GenerateId(EObjectType::Transaction);

    auto* transaction = new TTransaction(id);
    TransactionMap.Insert(id, transaction);

    // Every active transaction has a fake reference to it.
    ObjectManager->RefObject(id);
    
    if (parent) {
        transaction->SetParent(parent);
        YVERIFY(parent->NestedTransactions().insert(transaction).second);
        ObjectManager->RefObject(id);
    }

    if (IsLeader()) {
        CreateLease(*transaction, manifest);
    }

    transaction->SetState(ETransactionState::Active);

    TransactionStarted_.Fire(*transaction);

    LOG_INFO_IF(!IsRecovery(), "Transaction started (TransactionId: %s, ParentId: %s)",
        ~id.ToString(),
        parent ? ~parent->GetId().ToString() : "None");

    return *transaction;
}

void TTransactionManager::Commit(TTransaction& transaction)
{
    VERIFY_THREAD_AFFINITY(StateThread);

    auto id = transaction.GetId();

    if (!transaction.NestedTransactions().empty()) {
        ythrow yexception() << "Cannot commit since the transaction has nested transactions in progress";
    }

    if (IsLeader()) {
        CloseLease(transaction);
    }

    transaction.SetState(ETransactionState::Committed);

    TransactionCommitted_.Fire(transaction);

    FinishTransaction(transaction);

    LOG_INFO_IF(!IsRecovery(), "Transaction committed (TransactionId: %s)", ~id.ToString());
}

void TTransactionManager::Abort(TTransaction& transaction)
{
    VERIFY_THREAD_AFFINITY(StateThread);

    auto id = transaction.GetId();

    // Make a copy, the set will be modified.
    auto nestedTransactions = transaction.NestedTransactions();
    FOREACH (auto* transaction, nestedTransactions) {
        Abort(*transaction);
    }
    YASSERT(transaction.NestedTransactions().empty());

    if (IsLeader()) {
        CloseLease(transaction);
    }

    transaction.SetState(ETransactionState::Aborted);

    TransactionAborted_.Fire(transaction);

    FinishTransaction(transaction);

    LOG_INFO_IF(!IsRecovery(), "Transaction aborted (TransactionId: %s)", ~id.ToString());
}

void TTransactionManager::FinishTransaction(TTransaction& transaction)
{
    auto transactionId = transaction.GetId();

    if (transaction.GetParent()) {
        auto* parent = transaction.GetParent();
        YVERIFY(parent->NestedTransactions().erase(&transaction) == 1);
        ObjectManager->UnrefObject(transactionId);
    }

    FOREACH (const auto& createdId, transaction.CreatedObjectIds()) {
        ObjectManager->UnrefObject(createdId);
    }

    // Kill the fake reference.
    ObjectManager->UnrefObject(transactionId);
}

void TTransactionManager::RenewLease(const TTransactionId& id)
{
    VERIFY_THREAD_AFFINITY(StateThread);

    auto it = LeaseMap.find(id);
    YASSERT(it != LeaseMap.end());
    TLeaseManager::RenewLease(it->second);
}

void TTransactionManager::SaveKeys(TOutputStream* output)
{
    VERIFY_THREAD_AFFINITY(StateThread);

    TransactionMap.SaveKeys(output);
}

void TTransactionManager::SaveValues(TOutputStream* output)
{
    VERIFY_THREAD_AFFINITY(StateThread);

    TransactionMap.SaveValues(output);
}

void TTransactionManager::LoadKeys(TInputStream* input)
{
    VERIFY_THREAD_AFFINITY(StateThread);

    TransactionMap.LoadKeys(input);
}

void TTransactionManager::LoadValues(TInputStream* input, TLoadContext context)
{
    VERIFY_THREAD_AFFINITY(StateThread);

    TransactionMap.LoadValues(input, context);
}

void TTransactionManager::Clear()
{
    VERIFY_THREAD_AFFINITY(StateThread);

    TransactionMap.Clear();
}

void TTransactionManager::OnLeaderRecoveryComplete()
{
    FOREACH (const auto& pair, TransactionMap) {
        const auto& id = pair.first;
        const auto& transaction = *pair.second;
        auto proxy = ObjectManager->GetProxy(id);
        auto manifestNode = proxy->Attributes().ToMap();
        auto manifest = New<TTransactionManifest>();
        manifest->LoadAndValidate(~manifestNode);
        CreateLease(transaction, ~manifest);
    }
}

void TTransactionManager::OnStopLeading()
{
    FOREACH (const auto& pair, LeaseMap) {
        TLeaseManager::CloseLease(pair.second);
    }
    LeaseMap.clear();
}

void TTransactionManager::CreateLease(const TTransaction& transaction, TTransactionManifest* manifest)
{
    auto timeout = manifest->Timeout ? manifest->Timeout.Get() : Config->DefaultTransactionTimeout;
    auto lease = TLeaseManager::CreateLease(
        timeout,
        ~FromMethod(&TThis::OnTransactionExpired, TPtr(this), transaction.GetId())
        ->Via(MetaStateManager->GetEpochStateInvoker()));
    YVERIFY(LeaseMap.insert(MakePair(transaction.GetId(), lease)).second);
}

void TTransactionManager::CloseLease(const TTransaction& transaction)
{
    auto it = LeaseMap.find(transaction.GetId());
    YASSERT(it != LeaseMap.end());
    TLeaseManager::CloseLease(it->second);
    LeaseMap.erase(it);
}

void TTransactionManager::OnTransactionExpired(const TTransactionId& id)
{
    VERIFY_THREAD_AFFINITY(StateThread);

    auto proxy = ObjectManager->FindProxy(id);
    if (!proxy)
        return;
    LOG_INFO("Transaction expired (TransactionId: %s)", ~id.ToString());

    auto req = TTransactionYPathProxy::Abort();
    ExecuteVerb(~proxy, ~req);
}

TObjectManager* TTransactionManager::GetObjectManager() const
{
    return ~ObjectManager;
}

IObjectProxy::TPtr TTransactionManager::GetRootTransactionProxy()
{
    return New<TTransactionProxy>(this, NullTransactionId);
}

DEFINE_METAMAP_ACCESSORS(TTransactionManager, Transaction, TTransaction, TTransactionId, TransactionMap)

std::vector<TTransactionId> TTransactionManager::GetTransactionPath(const TTransactionId& transactionId) const
{
    std::vector<TTransactionId> path;
    path.push_back(transactionId);
    auto currentId = transactionId;
    while (currentId != NullTransactionId) {
        const auto& transaction = GetTransaction(currentId);
        currentId = transaction.GetParentId();
        path.push_back(currentId);
    }
    return path;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTransactionServer
} // namespace NYT
