#include "stdafx.h"
#include "object_manager.h"
#include "object.h"
#include "config.h"
#include "private.h"
#include "gc.h"
#include "attribute_set.h"
#include "schema.h"
#include "master.h"

#include <ytlib/misc/delayed_invoker.h>

#include <ytlib/ypath/tokenizer.h>

#include <ytlib/rpc/message.h>
#include <ytlib/rpc/server_detail.h>

#include <ytlib/meta_state/rpc_helpers.h>

#include <ytlib/object_client/object_service_proxy.h>

#include <ytlib/cypress_client/cypress_ypath_proxy.h>

#include <server/cell_master/serialization_context.h>

#include <server/transaction_server/transaction_manager.h>
#include <server/transaction_server/transaction.h>

#include <server/cypress_server/cypress_manager.h>

#include <server/chunk_server/chunk.h>
#include <server/chunk_server/chunk_list.h>

#include <server/cell_master/bootstrap.h>
#include <server/cell_master/meta_state_facade.h>

#include <server/security_server/user.h>
#include <server/security_server/group.h>
#include <server/security_server/acl.h>
#include <server/security_server/security_manager.h>

namespace NYT {
namespace NObjectServer {

using namespace NYTree;
using namespace NYPath;
using namespace NMetaState;
using namespace NRpc;
using namespace NBus;
using namespace NCypressServer;
using namespace NCypressClient;
using namespace NTransactionServer;
using namespace NSecurityServer;
using namespace NChunkServer;
using namespace NObjectClient;
using namespace NMetaState;
using namespace NCellMaster;

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = ObjectServerLogger;
static NProfiling::TProfiler& Profiler = ObjectServerProfiler;
static TDuration ProfilingPeriod = TDuration::MilliSeconds(100);

////////////////////////////////////////////////////////////////////////////////

//! A wrapper that is used to postpone the reply until the mutation is committed by quorum.
class TObjectManager::TServiceContextWrapper
    : public NRpc::TServiceContextWrapper
{
public:
    explicit TServiceContextWrapper(IServiceContextPtr underlyingContext)
        : NRpc::TServiceContextWrapper(std::move(underlyingContext))
        , Replied(false)
    { }

    virtual bool IsReplied() const override
    {
        return Replied;
    }

    virtual void Reply(const TError& error) override
    {
        YCHECK(!Replied);
        Replied = true;
        Error = error;
    }

    virtual void Reply(IMessagePtr responseMessage) override
    {
        UNUSED(responseMessage);
        YUNREACHABLE();
    }

    virtual const TError& GetError() const override
    {
        return Error;
    }

    IMessagePtr GetResponseMessage()
    {
        YCHECK(Replied);
        if (!ResponseMessage) {
            ResponseMessage = CreateResponseMessage(this);
        }
        return ResponseMessage;
    }

private:
    bool Replied;
    TError Error;
    IMessagePtr ResponseMessage;

};

////////////////////////////////////////////////////////////////////////////////

class TObjectManager::TRootService
    : public IYPathService
{
public:
    explicit TRootService(TBootstrap* bootstrap)
        : Bootstrap(bootstrap)
    {
        Logger = ObjectServerLogger;
    }

    virtual TResolveResult Resolve(
        const TYPath& path,
        NRpc::IServiceContextPtr context) override
    {
        auto cypressManager = Bootstrap->GetCypressManager();
        auto objectManager = Bootstrap->GetObjectManager();
        auto transactionManager = Bootstrap->GetTransactionManager();

        TTransaction* transaction = nullptr;
        auto transactionId = GetTransactionId(context);
        if (transactionId != NullTransactionId) {
            transaction = transactionManager->FindTransaction(transactionId);
            if (!transaction) {
                THROW_ERROR_EXCEPTION("No such transaction: %s", ~ToString(transactionId));
            }
            if (transaction->GetState() != ETransactionState::Active) {
                THROW_ERROR_EXCEPTION("Transaction is not active: %s", ~ToString(transactionId));
            }
        }

        NYPath::TTokenizer tokenizer(path);
        switch (tokenizer.Advance()) {
            case NYPath::ETokenType::EndOfStream:
                return TResolveResult::There(objectManager->GetMasterProxy(), tokenizer.GetSuffix());

            case NYPath::ETokenType::Slash: {
                auto root = cypressManager->GetVersionedNodeProxy(
                    cypressManager->GetRootNode(),
                    transaction);
                return TResolveResult::There(root, tokenizer.GetSuffix());
            }

            case NYPath::ETokenType::Literal: {
                const auto& token = tokenizer.GetToken();
                if (!token.has_prefix(ObjectIdPathPrefix)) {
                    tokenizer.ThrowUnexpected();
                }

                TStringBuf objectIdString(token.begin() + ObjectIdPathPrefix.length(), token.end());
                TObjectId objectId;
                if (!TObjectId::FromString(objectIdString, &objectId)) {
                    THROW_ERROR_EXCEPTION("Error parsing object id: %s", ~objectIdString);
                }

                auto* object = objectManager->FindObject(objectId);
                if (!IsObjectAlive(object)) {
                    THROW_ERROR_EXCEPTION("No such object: %s", ~ToString(objectId));
                }

                auto proxy = objectManager->GetProxy(object, transaction);
                return TResolveResult::There(proxy, tokenizer.GetSuffix());
            }

            default:
                tokenizer.ThrowUnexpected();
                YUNREACHABLE();
        }
    }

    virtual void Invoke(IServiceContextPtr context) override
    {
        UNUSED(context);
        YUNREACHABLE();
    }

    virtual Stroka GetLoggingCategory() const override
    {
        return NObjectServer::Logger.GetCategory();
    }

    virtual bool IsWriteRequest(IServiceContextPtr context) const override
    {
        UNUSED(context);
        YUNREACHABLE();
    }

    // TODO(panin): remove this when getting rid of IAttributeProvider
    virtual void SerializeAttributes(
        NYson::IYsonConsumer* consumer,
        const TAttributeFilter& filter) const override
    {
        YUNREACHABLE();
    }

private:
    TBootstrap* Bootstrap;

};

////////////////////////////////////////////////////////////////////////////////

TObjectManager::TObjectManager(
    TObjectManagerConfigPtr config,
    TBootstrap* bootstrap)
    : TMetaStatePart(
        bootstrap->GetMetaStateFacade()->GetManager(),
        bootstrap->GetMetaStateFacade()->GetState())
    , Config(config)
    , Bootstrap(bootstrap)
    , TypeToEntry(MaxObjectType)
    , RootService(New<TRootService>(bootstrap))
    , GarbageCollector(New<TGarbageCollector>(config, bootstrap))
    , CreatedObjectCount(0)
    , DestroyedObjectCount(0)
    , LockedObjectCount(0)
{
    YCHECK(config);
    YCHECK(bootstrap);

    {
        NCellMaster::TLoadContext context;
        context.SetBootstrap(Bootstrap);

        RegisterLoader(
            "ObjectManager.Keys",
            SnapshotVersionValidator(),
            BIND(&TObjectManager::LoadKeys, MakeStrong(this)),
            context);
        RegisterLoader(
            "ObjectManager.Values",
            SnapshotVersionValidator(),
            BIND(&TObjectManager::LoadValues, MakeStrong(this)),
            context);
        RegisterLoader(
            "ObjectManager.Schemas",
            SnapshotVersionValidator(),
            BIND(&TObjectManager::LoadSchemas, MakeStrong(this)),
            context);
    }
    {
        NCellMaster::TSaveContext context;

        RegisterSaver(
            ESerializationPriority::Keys,
            "ObjectManager.Keys",
            CurrentSnapshotVersion,
            BIND(&TObjectManager::SaveKeys, MakeStrong(this)),
            context);
        RegisterSaver(
            ESerializationPriority::Values,
            "ObjectManager.Values",
            CurrentSnapshotVersion,
            BIND(&TObjectManager::SaveValues, MakeStrong(this)),
            context);
        RegisterSaver(
            ESerializationPriority::Values,
            "ObjectManager.Schemas",
            CurrentSnapshotVersion,
            BIND(&TObjectManager::SaveSchemas, MakeStrong(this)),
            context);
    }

    RegisterHandler(CreateMasterTypeHandler(Bootstrap));
    
    RegisterMethod(BIND(&TObjectManager::ReplayVerb, Unretained(this)));
    RegisterMethod(BIND(&TObjectManager::DestroyObjects, Unretained(this)));

    MasterObjectId = MakeWellKnownId(EObjectType::Master, Config->CellId);

    LOG_INFO("CellId: %d", static_cast<int>(config->CellId));
    LOG_INFO("MasterObjectId: %s", ~ToString(MasterObjectId));

    ProflilingInvoker = New<TPeriodicInvoker>(
        Bootstrap->GetMetaStateFacade()->GetInvoker(),
        BIND(&TObjectManager::OnProfiling, MakeWeak(this)),
        ProfilingPeriod);
    ProflilingInvoker->Start();
}

void TObjectManager::Initialize()
{ }

IYPathServicePtr TObjectManager::GetRootService()
{
    VERIFY_THREAD_AFFINITY_ANY();

    return RootService;
}

TObjectBase* TObjectManager::GetMasterObject()
{
    VERIFY_THREAD_AFFINITY_ANY();

    return ~MasterObject;
}

IObjectProxyPtr TObjectManager::GetMasterProxy()
{
    VERIFY_THREAD_AFFINITY_ANY();

    return MasterProxy;
}

TObjectBase* TObjectManager::FindSchema(EObjectType type)
{
    VERIFY_THREAD_AFFINITY_ANY();

    int typeValue = static_cast<int>(type);
    if (typeValue < 0 || typeValue > MaxObjectType) {
        return nullptr;
    }

    return ~TypeToEntry[typeValue].SchemaObject;
}

TObjectBase* TObjectManager::GetSchema(EObjectType type)
{
    VERIFY_THREAD_AFFINITY_ANY();

    auto* schema = FindSchema(type);
    YCHECK(schema);
    return schema;
}

IObjectProxyPtr TObjectManager::GetSchemaProxy(EObjectType type)
{
    VERIFY_THREAD_AFFINITY_ANY();

    int typeValue = static_cast<int>(type);
    YCHECK(typeValue >= 0 && typeValue <= MaxObjectType);
    
    const auto& entry = TypeToEntry[typeValue];
    YCHECK(entry.SchemaProxy);
    return entry.SchemaProxy;
}

void TObjectManager::RegisterHandler(IObjectTypeHandlerPtr handler)
{
    // No thread affinity check here.
    // This will be called during init-time only but from an unspecified thread.
    YCHECK(handler);

    auto type = handler->GetType();
    int typeValue = static_cast<int>(type);
    YCHECK(typeValue >= 0 && typeValue <= MaxObjectType);
    YCHECK(!TypeToEntry[typeValue].Handler);

    RegisteredTypes.push_back(type);
    auto& entry = TypeToEntry[typeValue];
    entry.Handler = handler;
    if (TypeHasSchema(type)) {
        auto schemaType = SchemaTypeFromType(type);
        auto& schemaEntry = TypeToEntry[static_cast<int>(schemaType)];
        schemaEntry.Handler = CreateSchemaTypeHandler(Bootstrap, type);
        LOG_INFO("Type registered (Type: %s, SchemaObjectId: %s)",
            ~type.ToString(),
            ~ToString(MakeSchemaObjectId(type, GetCellId())));
    } else {
        LOG_INFO("Type registered (Type: %s)",
            ~type.ToString());
    }
}

IObjectTypeHandlerPtr TObjectManager::FindHandler(EObjectType type) const
{
    VERIFY_THREAD_AFFINITY_ANY();

    int typeValue = static_cast<int>(type);
    if (typeValue < 0 || typeValue > MaxObjectType) {
        return nullptr;
    }

    return TypeToEntry[typeValue].Handler;
}

IObjectTypeHandlerPtr TObjectManager::GetHandler(EObjectType type) const
{
    VERIFY_THREAD_AFFINITY_ANY();

    auto handler = FindHandler(type);
    YASSERT(handler);
    return handler;
}

IObjectTypeHandlerPtr TObjectManager::GetHandler(TObjectBase* object) const
{
    return GetHandler(object->GetType());
}

const std::vector<EObjectType> TObjectManager::GetRegisteredTypes() const
{
    return RegisteredTypes;
}

TCellId TObjectManager::GetCellId() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return Config->CellId;
}

const TGuid& TObjectManager::GetCellGuid() const
{
    VERIFY_THREAD_AFFINITY(StateThread);

    if (CachedCellGuild.IsEmpty()) {
        auto rootService = const_cast<TObjectManager*>(this)->GetRootService();
        CachedCellGuild = ConvertTo<TGuid>(SyncYPathGet(rootService, "//sys/@cell_guid"));
    }

    return CachedCellGuild;
}

TObjectId TObjectManager::GenerateId(EObjectType type)
{
    VERIFY_THREAD_AFFINITY(StateThread);

    auto* mutationContext = Bootstrap
        ->GetMetaStateFacade()
        ->GetManager()
        ->GetMutationContext();

    const auto& version = mutationContext->GetVersion();

    auto random = mutationContext->RandomGenerator().Generate<ui64>();

    int typeValue = static_cast<int>(type);
    YASSERT(typeValue >= 0 && typeValue <= MaxObjectType);

    auto cellId = GetCellId();

    TObjectId id(
        random,
        (cellId << 16) + typeValue,
        version.RecordCount,
        version.SegmentId);

    ++CreatedObjectCount;

    LOG_DEBUG_UNLESS(IsRecovery(), "Object created (Type: %s, Id: %s)",
        ~type.ToString(),
        ~ToString(id));

    return id;
}

void TObjectManager::RefObject(TObjectBase* object)
{
    VERIFY_THREAD_AFFINITY(StateThread);
    YASSERT(object->IsTrunk());

    int refCounter = object->RefObject();
    LOG_DEBUG_UNLESS(IsRecovery(), "Object referenced (Id: %s, RefCounter: %d)",
        ~ToString(object->GetId()),
        refCounter);
}

void TObjectManager::UnrefObject(TObjectBase* object)
{
    VERIFY_THREAD_AFFINITY(StateThread);
    YASSERT(object->IsTrunk());

    int refCounter = object->UnrefObject();
    LOG_DEBUG_UNLESS(IsRecovery(), "Object unreferenced (Id: %s, RefCounter: %d)",
        ~ToString(object->GetId()),
        refCounter);

    if (refCounter == 0) {
        GarbageCollector->Enqueue(object);
    }
}

void TObjectManager::LockObject(TObjectBase* object)
{
    VERIFY_THREAD_AFFINITY(StateThread);

    int lockCounter = object->LockObject();
    if (lockCounter == 1) {
        ++LockedObjectCount;
    }
}

void TObjectManager::UnlockObject(TObjectBase* object)
{
    VERIFY_THREAD_AFFINITY(StateThread);

    int lockCounter = object->UnlockObject();
    if (lockCounter == 0) {
        --LockedObjectCount;
        if (!object->IsAlive()) {
            GarbageCollector->Unlock(object);
        }
    }
}

void TObjectManager::InitWellKnownSingletons()
{
    MasterObject = new TMasterObject(MasterObjectId);
    MasterObject->RefObject();
    MasterProxy = CreateMasterProxy(Bootstrap, ~MasterObject);

    FOREACH (auto type, RegisteredTypes)  {
        auto& entry = TypeToEntry[static_cast<int>(type)];
        if (TypeHasSchema(type)) {
            entry.SchemaObject = new TSchemaObject(MakeSchemaObjectId(type, GetCellId()));
            entry.SchemaObject->RefObject();
            entry.SchemaProxy = CreateSchemaProxy(Bootstrap, ~entry.SchemaObject);
        }
    }
}

void TObjectManager::SaveKeys(const NCellMaster::TSaveContext& context) const
{
    Attributes.SaveKeys(context);
}

void TObjectManager::SaveValues(const NCellMaster::TSaveContext& context) const
{
    Attributes.SaveValues(context);
    GarbageCollector->Save(context);
}

void TObjectManager::SaveSchemas(const NCellMaster::TSaveContext& context) const
{
    // Make sure the ordering of RegisteredTypes does not matter.
    auto types = RegisteredTypes;
    std::sort(types.begin(), types.end());

    FOREACH (auto type, types) {
        if (TypeHasSchema(type)) {
            Save(context, type);
            const auto& entry = TypeToEntry[static_cast<int>(type)];
            entry.SchemaObject->Save(context);
        }
    }

    // Write a sentinel.
    Save(context, EObjectType(EObjectType::Null));
}

void TObjectManager::LoadKeys(const NCellMaster::TLoadContext& context)
{
    VERIFY_THREAD_AFFINITY(StateThread);

    Attributes.LoadKeys(context);
}

void TObjectManager::LoadValues(const NCellMaster::TLoadContext& context)
{
    VERIFY_THREAD_AFFINITY(StateThread);

    Attributes.LoadValues(context);
    GarbageCollector->Load(context);
}

void TObjectManager::LoadSchemas(const NCellMaster::TLoadContext& context)
{
    VERIFY_THREAD_AFFINITY(StateThread);

    InitWellKnownSingletons();

    while (true) {
        EObjectType type;
        Load(context, type);
        if (type == EObjectType::Null)
            break;

        const auto& entry = TypeToEntry[static_cast<int>(type)];
        entry.SchemaObject->Load(context);
    }
}

void TObjectManager::Clear()
{
    VERIFY_THREAD_AFFINITY(StateThread);

    InitWellKnownSingletons();
    Attributes.Clear();
    GarbageCollector->Clear();
    CreatedObjectCount = 0;
    DestroyedObjectCount = 0;
    LockedObjectCount = 0;
}

void TObjectManager::OnRecoveryStarted()
{
    Profiler.SetEnabled(false);
}

void TObjectManager::OnRecoveryComplete()
{
    Profiler.SetEnabled(true);

    GarbageCollector->UnlockAll();
    LockedObjectCount = 0;
}

void TObjectManager::OnActiveQuorumEstablished()
{
    GarbageCollector->StartSweep();
}

void TObjectManager::OnStopLeading()
{
    GarbageCollector->StopSweep();
}

TObjectBase* TObjectManager::FindObject(const TObjectId& id)
{
    VERIFY_THREAD_AFFINITY(StateThread);

    auto handler = FindHandler(TypeFromId(id));
    if (!handler) {
        return nullptr;
    }

    return handler->FindObject(id);
}

TObjectBase* TObjectManager::GetObject(const TObjectId& id)
{
    VERIFY_THREAD_AFFINITY(StateThread);

    auto* object = FindObject(id);
    YCHECK(object);
    return object;
}

IObjectProxyPtr TObjectManager::GetProxy(
    TObjectBase* object,
    TTransaction* transaction)
{
    VERIFY_THREAD_AFFINITY(StateThread);
    YCHECK(object);
    YCHECK(object->IsAlive());

    const auto& id = object->GetId();
    auto handler = FindHandler(TypeFromId(id));
    if (!handler) {
        return nullptr;
    }

    return handler->GetProxy(object, transaction);
}

TAttributeSet* TObjectManager::CreateAttributes(const TVersionedObjectId& id)
{
    VERIFY_THREAD_AFFINITY(StateThread);

    auto result = new TAttributeSet();
    Attributes.Insert(id, result);
    return result;
}

void TObjectManager::RemoveAttributes(const TVersionedObjectId& id)
{
    VERIFY_THREAD_AFFINITY(StateThread);

    Attributes.Remove(id);
}

void TObjectManager::BranchAttributes(
    const TVersionedObjectId& originatingId,
    const TVersionedObjectId& branchedId)
{
    VERIFY_THREAD_AFFINITY(StateThread);
    UNUSED(originatingId);
    UNUSED(branchedId);
    // We don't store empty deltas at the moment
}

void TObjectManager::MergeAttributes(
    const TVersionedObjectId& originatingId,
    const TVersionedObjectId& branchedId)
{
    VERIFY_THREAD_AFFINITY(StateThread);

    auto* originatingAttributes = FindAttributes(originatingId);
    const auto* branchedAttributes = FindAttributes(branchedId);
    if (!branchedAttributes) {
        return;
    }

    if (!originatingAttributes) {
        auto attributeSet = Attributes.Release(branchedId);
        Attributes.Insert(originatingId, attributeSet.Release());
    } else {
        FOREACH (const auto& pair, branchedAttributes->Attributes()) {
            if (!pair.second && !originatingId.IsBranched()) {
                originatingAttributes->Attributes().erase(pair.first);
            } else {
                originatingAttributes->Attributes()[pair.first] = pair.second;
            }
        }
        Attributes.Remove(branchedId);
    }
}

void TObjectManager::ExecuteVerb(
    const TVersionedObjectId& id,
    bool isWrite,
    IServiceContextPtr context,
    TCallback<void(IServiceContextPtr)> action)
{
    VERIFY_THREAD_AFFINITY(StateThread);

    auto securityManager = Bootstrap->GetSecurityManager();
    auto* user = securityManager->GetAuthenticatedUser();

    LOG_INFO_UNLESS(IsRecovery(), "ExecuteVerb: %s %s (ObjectId: %s, IsWrite: %s, User: %s)",
        ~context->GetVerb(),
        ~context->GetPath(),
        ~ToString(id),
        ~FormatBool(isWrite),
        ~user->GetName());

    auto profilingPath = "/types/" +
        TypeFromId(id.ObjectId).ToString() +
        "/verbs/" +
        context->GetVerb() +
        "/time";

    if (IsRecovery() || !isWrite || MetaStateManager->GetMutationContext()) {
        PROFILE_TIMING (profilingPath) {
            action.Run(context);
        }
    } else {
        if (!Bootstrap->GetMetaStateFacade()->IsActiveLeader()) {
            context->Reply(TError(
                NRpc::EErrorCode::Unavailable,
                "Not an active leader"));
            return;
        }

        NProto::TMetaReqExecute executeReq;
        ToProto(executeReq.mutable_object_id(), id.ObjectId);
        ToProto(executeReq.mutable_transaction_id(),  id.TransactionId);
        ToProto(executeReq.mutable_user_id(), user->GetId());

        auto requestMessage = context->GetRequestMessage();
        const auto& requestParts = requestMessage->GetParts();
        FOREACH (const auto& part, requestParts) {
            executeReq.add_request_parts(part.Begin(), part.Size());
        }

        // Capture everything needed in lambdas below.
        auto wrappedContext = New<TServiceContextWrapper>(context);
        auto mutationId = GetRpcMutationId(context);
        auto metaStateManager = MetaStateManager;

        Bootstrap
            ->GetMetaStateFacade()
            ->CreateMutation()
            ->SetRequestData(executeReq)
            ->SetId(mutationId)
            ->SetAction(BIND([=] () {
                PROFILE_TIMING (profilingPath) {
                    action.Run(wrappedContext);
                }
                if (mutationId != NullMutationId) {
                    auto responseMessage = wrappedContext->GetResponseMessage();
                    auto responseData = PackMessage(responseMessage);
                    metaStateManager->GetMutationContext()->SetResponseData(responseData);
                }
            }))
            ->OnSuccess(BIND([=] (const TMutationResponse& response) {
                auto responseMessage =
                    response.Applied
                    ? wrappedContext->GetResponseMessage()
                    : UnpackMessage(response.Data);
                context->Reply(responseMessage);
            }))
            ->OnError(CreateRpcErrorHandler(context))
            ->Commit();
    }
}

TMutationPtr TObjectManager::CreateDestroyObjectsMutation(const NProto::TMetaReqDestroyObjects& request)
{
    return Bootstrap
        ->GetMetaStateFacade()
        ->CreateMutation(this, request, &TThis::DestroyObjects);
}

TFuture<void> TObjectManager::GCCollect()
{
    VERIFY_THREAD_AFFINITY(StateThread);

    return GarbageCollector->Collect();
}

TObjectBase* TObjectManager::CreateObject(
    TTransaction* transaction,
    TAccount* account,
    EObjectType type,
    IAttributeDictionary* attributes,
    IObjectTypeHandler::TReqCreateObject* request,
    IObjectTypeHandler::TRspCreateObject* response)
{
    auto handler = FindHandler(type);
    if (!handler) {
        THROW_ERROR_EXCEPTION("Unknown object type: %s",
            ~type.ToString());
    }

    auto options = handler->GetCreationOptions();
    if (!options) {
        THROW_ERROR_EXCEPTION("Type does not support creating new instances: %s",
            ~type.ToString());
    }

    switch (options->TransactionMode) {
        case EObjectTransactionMode::Required:
            if (!transaction) {
                THROW_ERROR_EXCEPTION("Cannot create an instance of %s outside of a transaction",
                    ~FormatEnum(type).Quote());
            }
            break;

        case EObjectTransactionMode::Forbidden:
            if (transaction) {
                THROW_ERROR_EXCEPTION("Cannot create an instance of %s inside of a transaction",
                    ~FormatEnum(type).Quote());
            }
            break;

        case EObjectTransactionMode::Optional:
            break;

        default:
            YUNREACHABLE();
    }

    switch (options->AccountMode) {
        case EObjectAccountMode::Required:
            if (!account) {
                THROW_ERROR_EXCEPTION("Cannot create an instance of %s without an account",
                    ~FormatEnum(type).Quote());
            }
            break;

        case EObjectAccountMode::Forbidden:
            if (account) {
                THROW_ERROR_EXCEPTION("Cannot create an instance of %s with an account",
                    ~FormatEnum(type).Quote());
            }
            break;

        case EObjectAccountMode::Optional:
            break;

        default:
            YUNREACHABLE();
    }

    auto securityManager = Bootstrap->GetSecurityManager();
    auto* user = securityManager->GetAuthenticatedUser();

    auto* schema = FindSchema(type);
    if (schema) {
        securityManager->ValidatePermission(schema, user, EPermission::Create);
    }

    auto* object = handler->Create(
        transaction,
        account,
        attributes,
        request,
        response);
    const auto& objectId = object->GetId();

    auto attributeKeys = attributes->List();
    if (!attributeKeys.empty()) {
        // Copy attributes. Quick and dirty.
        auto* attributeSet = FindAttributes(TVersionedObjectId(objectId));
        if (!attributeSet) {
            attributeSet = CreateAttributes(TVersionedObjectId(objectId));
        }

        FOREACH (const auto& key, attributeKeys) {
            YCHECK(attributeSet->Attributes().insert(std::make_pair(
                key,
                attributes->GetYson(key))).second);
        }
    }

    if (transaction && options->SupportsStaging) {
        YCHECK(transaction->StagedObjects().insert(object).second);
        RefObject(object);
    }

    auto* acd = securityManager->FindAcd(object);
    if (acd) {
        acd->SetOwner(user);
    }

    return object;
}

void TObjectManager::UnstageObject(
    TTransaction* transaction,
    TObjectBase* object,
    bool recursive)
{
    if (transaction->StagedObjects().erase(object) != 1) {
        THROW_ERROR_EXCEPTION("Object %s does not belong to transaction %s",
            ~ToString(object->GetId()),
            ~ToString(transaction->GetId()));
    }

    auto handler = GetHandler(object);
    handler->Unstage(object, transaction, recursive);
    UnrefObject(object);
}

void TObjectManager::ReplayVerb(const NProto::TMetaReqExecute& request)
{
    VERIFY_THREAD_AFFINITY(StateThread);

    auto objectId = FromProto<TObjectId>(request.object_id());
    auto transactionId = FromProto<TTransactionId>(request.transaction_id());
    auto userId = FromProto<TUserId>(request.user_id());

    auto transactionManager = Bootstrap->GetTransactionManager();
    auto* transaction =
        transactionId == NullTransactionId
        ? nullptr
        : transactionManager->GetTransaction(transactionId);

    auto securityManager = Bootstrap->GetSecurityManager();
    auto* user = securityManager->GetUser(userId);
    TAuthenticatedUserGuard userGuard(securityManager, user);

    std::vector<TSharedRef> parts(request.request_parts_size());
    for (int partIndex = 0; partIndex < request.request_parts_size(); ++partIndex) {
        // Construct a non-owning TSharedRef to avoid copying.
        // This is feasible since the message will outlive the request.
        const auto& part = request.request_parts(partIndex);
        parts[partIndex] = TSharedRef::FromRefNonOwning(TRef(const_cast<char*>(part.begin()), part.size()));
    }

    auto requestMessage = CreateMessageFromParts(std::move(parts));
    auto context = CreateYPathContext(
        requestMessage,
        "",
        TYPathResponseHandler());

    auto* object = GetObject(objectId);
    auto proxy = GetProxy(object, transaction);
    proxy->Invoke(context);
}

void TObjectManager::DestroyObjects(const NProto::TMetaReqDestroyObjects& request)
{
    FOREACH (const auto& protoId, request.object_ids()) {
        auto id = FromProto<TObjectId>(protoId);
        auto type = TypeFromId(id);
        auto handler = GetHandler(type);
        auto* object = handler->GetObject(id);

        // NB: The order of Dequeue/Destroy/CheckEmpty calls matters.
        // CheckEmpty will raise CollectPromise when GC becomes empty.
        // To enable cascaded GC sweep we don't want this to happen
        // if some ids are added during DestroyObject.
        GarbageCollector->Dequeue(object);
        handler->Destroy(object);
        ++DestroyedObjectCount;

        LOG_DEBUG_UNLESS(IsRecovery(), "Object destroyed (Type: %s, Id: %s)",
            ~type.ToString(),
            ~ToString(id));
    }

    GarbageCollector->CheckEmpty();
}

void TObjectManager::OnProfiling()
{
    VERIFY_THREAD_AFFINITY(StateThread);

    Profiler.Enqueue("/gc_queue_size", GarbageCollector->GetGCQueueSize());
    Profiler.Enqueue("/gc_lock_queue_size", GarbageCollector->GetLockedGCQueueSize());
    Profiler.Enqueue("/created_object_count", CreatedObjectCount);
    Profiler.Enqueue("/destroyed_object_count", DestroyedObjectCount);
    Profiler.Enqueue("/locked_object_count", LockedObjectCount);
}

DEFINE_METAMAP_ACCESSORS(TObjectManager, Attributes, TAttributeSet, TVersionedObjectId, Attributes)

////////////////////////////////////////////////////////////////////////////////

} // namespace NObjectServer
} // namespace NYT

