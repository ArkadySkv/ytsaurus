#include "stdafx.h"
#include "object_detail.h"
#include "object_manager.h"
#include "object_service.h"
#include "attribute_set.h"

#include <ytlib/misc/string.h>
#include <ytlib/misc/enum.h>

#include <ytlib/ytree/fluent.h>
#include <ytlib/ytree/yson_string.h>
#include <ytlib/ytree/exception_helpers.h>

#include <ytlib/ypath/tokenizer.h>

#include <ytlib/rpc/message.h>
#include <ytlib/rpc/rpc.pb.h>

#include <ytlib/cypress_client/cypress_ypath_proxy.h>

#include <ytlib/meta_state/meta_state_manager.h>

#include <server/cell_master/bootstrap.h>
#include <server/cell_master/meta_state_facade.h>
#include <server/cell_master/config.h>
#include <server/cell_master/serialization_context.h>

#include <server/cypress_server/virtual.h>

#include <server/transaction_server/transaction.h>

#include <server/security_server/account.h>
#include <server/security_server/security_manager.h>
#include <server/security_server/acl.h>
#include <server/security_server/user.h>

#include <server/object_server/type_handler.h>

#include <stdexcept>

namespace NYT {
namespace NObjectServer {

using namespace NRpc;
using namespace NYPath;
using namespace NYTree;
using namespace NYson;
using namespace NCellMaster;
using namespace NCypressClient;
using namespace NObjectClient;
using namespace NMetaState;
using namespace NSecurityClient;
using namespace NSecurityServer;

////////////////////////////////////////////////////////////////////////////////

TStagedObject::TStagedObject()
    : StagingTransaction_(nullptr)
    , StagingAccount_(nullptr)
{ }

void TStagedObject::Save(NCellMaster::TSaveContext& context) const
{
    SaveObjectRef(context, StagingTransaction_);
    SaveObjectRef(context, StagingAccount_);
}

void TStagedObject::Load(NCellMaster::TLoadContext& context)
{
    LoadObjectRef(context, StagingTransaction_);
    LoadObjectRef(context, StagingAccount_);
}

bool TStagedObject::IsStaged() const
{
    return StagingTransaction_ && StagingAccount_;
}

////////////////////////////////////////////////////////////////////////////////

TUserAttributeDictionary::TUserAttributeDictionary(
    TObjectManagerPtr objectManager,
    const TObjectId& objectId)
    : ObjectManager(std::move(objectManager))
    , ObjectId(objectId)
{ }

std::vector<Stroka> TUserAttributeDictionary::List() const
{
    std::vector<Stroka> keys;
    const auto* attributeSet = ObjectManager->FindAttributes(TVersionedObjectId(ObjectId));
    if (attributeSet) {
        FOREACH (const auto& pair, attributeSet->Attributes()) {
            // Attribute cannot be empty (i.e. deleted) in null transaction.
            YASSERT(pair.second);
            keys.push_back(pair.first);
        }
    }
    return keys;
}

TNullable<TYsonString> TUserAttributeDictionary::FindYson(const Stroka& key) const
{
    const auto* attributeSet = ObjectManager->FindAttributes(TVersionedObjectId(ObjectId));
    if (!attributeSet) {
        return Null;
    }
    auto it = attributeSet->Attributes().find(key);
    if (it == attributeSet->Attributes().end()) {
        return Null;
    }
    // Attribute cannot be empty (i.e. deleted) in null transaction.
    YASSERT(it->second);
    return it->second;
}

void TUserAttributeDictionary::SetYson(
    const Stroka& key,
    const NYTree::TYsonString& value)
{
    auto* attributeSet = ObjectManager->GetOrCreateAttributes(TVersionedObjectId(ObjectId));
    attributeSet->Attributes()[key] = value;
}

bool TUserAttributeDictionary::Remove(const Stroka& key)
{
    auto* attributeSet = ObjectManager->FindAttributes(TVersionedObjectId(ObjectId));
    if (!attributeSet) {
        return false;
    }
    auto it = attributeSet->Attributes().find(key);
    if (it == attributeSet->Attributes().end()) {
        return false;
    }
    // Attribute cannot be empty (i.e. deleted) in null transaction.
    YASSERT(it->second);
    attributeSet->Attributes().erase(it);
    if (attributeSet->Attributes().empty()) {
        ObjectManager->RemoveAttributes(TVersionedObjectId(ObjectId));
    }
    return true;
}

////////////////////////////////////////////////////////////////////////////////

TObjectProxyBase::TObjectProxyBase(
    TBootstrap* bootstrap,
    TObjectBase* object)
    : Bootstrap(bootstrap)
    , Object(object)
{
    YASSERT(bootstrap);
    YASSERT(object);
}

TObjectProxyBase::~TObjectProxyBase()
{ }

const TObjectId& TObjectProxyBase::GetId() const
{
    return Object->GetId();
}

const IAttributeDictionary& TObjectProxyBase::Attributes() const
{
    return *const_cast<TObjectProxyBase*>(this)->GetUserAttributes();
}

IAttributeDictionary* TObjectProxyBase::MutableAttributes()
{
    return GetUserAttributes();
}

DEFINE_RPC_SERVICE_METHOD(TObjectProxyBase, GetId)
{
    context->SetRequestInfo("");

    ToProto(response->mutable_object_id(), GetId());

    context->Reply();
}

DEFINE_RPC_SERVICE_METHOD(TObjectProxyBase, CheckPermission)
{
    auto userName = request->user();
    auto permission = EPermission(request->permission());
    context->SetRequestInfo("User: %s, Permission: %s",
        ~userName,
        ~permission.ToString());

    auto securityManager = Bootstrap->GetSecurityManager();
    auto objectManager = Bootstrap->GetObjectManager();

    auto* user = securityManager->FindUserByName(userName);
    if (!IsObjectAlive(user)) {
        THROW_ERROR_EXCEPTION("No such user %s", ~userName.Quote());
    }

    auto result = securityManager->CheckPermission(Object, user, permission);

    response->set_action(result.Action);
    if (result.Object) {
        ToProto(response->mutable_object_id(), result.Object->GetId());
    }
    if (result.Subject) {
        response->set_subject(result.Subject->GetName());
    }

    context->SetResponseInfo("Action: %s, Object: %s, Subject: %s",
        ~permission.ToString(),
        result.Object ? ~ToString(result.Object->GetId()) : "<Null>",
        result.Subject ? ~ToString(result.Subject->GetId()) : "<Null>");
    context->Reply();
}

void TObjectProxyBase::Invoke(IServiceContextPtr context)
{
    Bootstrap->GetObjectManager()->ExecuteVerb(
        GetVersionedId(),
        IsWriteRequest(context),
        context,
        BIND(&TObjectProxyBase::GuardedInvoke, MakeStrong(this)));
}

void TObjectProxyBase::SerializeAttributes(
    IYsonConsumer* consumer,
    const TAttributeFilter& filter,
    bool sortKeys)
{
    if (filter.Mode == EAttributeFilterMode::None)
        return;

    if (filter.Mode == EAttributeFilterMode::MatchingOnly && filter.Keys.empty())
        return;

    class TAttributesConsumer
        : public IYsonConsumer
    {
    public:
        explicit TAttributesConsumer(IYsonConsumer* underlyingConsumer)
            : UnderlyingConsumer(underlyingConsumer)
            , HasAttributes(false)
        { }

        ~TAttributesConsumer()
        {
            if (HasAttributes) {
                UnderlyingConsumer->OnEndAttributes();
            }
        }

        virtual void OnStringScalar(const TStringBuf& value) override
        {
            UnderlyingConsumer->OnStringScalar(value);
        }

        virtual void OnIntegerScalar(i64 value) override
        {
            UnderlyingConsumer->OnIntegerScalar(value);
        }

        virtual void OnDoubleScalar(double value) override
        {
            UnderlyingConsumer->OnDoubleScalar(value);
        }

        virtual void OnEntity() override
        {
            UnderlyingConsumer->OnEntity();
        }

        virtual void OnBeginList() override
        {
            UnderlyingConsumer->OnBeginList();
        }

        virtual void OnListItem() override
        {
            UnderlyingConsumer->OnListItem();
        }

        virtual void OnEndList() override
        {
            UnderlyingConsumer->OnEndList();
        }

        virtual void OnBeginMap() override
        {
            UnderlyingConsumer->OnBeginList();
        }

        virtual void OnKeyedItem(const TStringBuf& key) override
        {
            if (!HasAttributes) {
                UnderlyingConsumer->OnBeginAttributes();
                HasAttributes = true;
            }
            UnderlyingConsumer->OnKeyedItem(key);
        }

        virtual void OnEndMap() override
        {
            UnderlyingConsumer->OnEndMap();
        }

        virtual void OnBeginAttributes() override
        {
            UnderlyingConsumer->OnBeginAttributes();
        }

        virtual void OnEndAttributes() override
        {
            UnderlyingConsumer->OnEndAttributes();
        }

        virtual void OnRaw(const TStringBuf& yson, EYsonType type) override
        {
            UnderlyingConsumer->OnRaw(yson, type);
        }

    private:
        IYsonConsumer* UnderlyingConsumer;
        bool HasAttributes;

    };

    TAttributesConsumer attributesConsumer(consumer);

    const auto& userAttributes = Attributes();

    switch (filter.Mode) {
        case EAttributeFilterMode::All: {
            std::vector<ISystemAttributeProvider::TAttributeInfo> systemAttributes;
            ListSystemAttributes(&systemAttributes);

            auto userKeys = userAttributes.List();

            // TODO(babenko): this is not exactly totally sorted keys, but should be fine.
            if (sortKeys) {
                std::sort(
                    userKeys.begin(),
                    userKeys.end());

                std::sort(
                    systemAttributes.begin(),
                    systemAttributes.end(),
                    [] (const ISystemAttributeProvider::TAttributeInfo& lhs, const ISystemAttributeProvider::TAttributeInfo& rhs) {
                        return lhs.Key < rhs.Key;
                });
            }

            FOREACH (const auto& key, userKeys) {
                attributesConsumer.OnKeyedItem(key);
                attributesConsumer.OnRaw(userAttributes.GetYson(key).Data(), EYsonType::Node);
            }

            FOREACH (const auto& attribute, systemAttributes) {
                if (attribute.IsPresent){
                    attributesConsumer.OnKeyedItem(attribute.Key);
                    if (attribute.IsOpaque) {
                        attributesConsumer.OnEntity();
                    } else {
                        YCHECK(GetSystemAttribute(attribute.Key, &attributesConsumer));
                    }
                }
            }
            break;
        }

        case EAttributeFilterMode::MatchingOnly: {
            auto keys = filter.Keys;
            
            if (sortKeys) {
                std::sort(keys.begin(), keys.end());
            }

            FOREACH (const auto& key, keys) {
                if (!GetSystemAttribute(key, &attributesConsumer)) {
                    auto value = userAttributes.FindYson(key);
                    if (value) {
                        attributesConsumer.OnKeyedItem(key);
                        attributesConsumer.OnRaw(value->Data(), EYsonType::Node);
                    }
                }
            }

            break;
        }

        default:
            YUNREACHABLE();
    }
}

void TObjectProxyBase::GuardedInvoke(IServiceContextPtr context)
{
    try {
        BeforeInvoke(context);
        if (!DoInvoke(context)) {
            ThrowVerbNotSuppored(context->GetVerb());
        }
        AfterInvoke(context);
    } catch (const TNotALeaderException&) {
        ForwardToLeader(context);
    } catch (const std::exception& ex) {
        context->Reply(ex);
    }
}

void TObjectProxyBase::BeforeInvoke(IServiceContextPtr /*context*/)
{ }

void TObjectProxyBase::AfterInvoke(IServiceContextPtr /*context*/)
{ }

bool TObjectProxyBase::DoInvoke(IServiceContextPtr context)
{
    DISPATCH_YPATH_SERVICE_METHOD(GetId);
    DISPATCH_YPATH_SERVICE_METHOD(Get);
    DISPATCH_YPATH_SERVICE_METHOD(List);
    DISPATCH_YPATH_SERVICE_METHOD(Set);
    DISPATCH_YPATH_SERVICE_METHOD(Remove);
    DISPATCH_YPATH_SERVICE_METHOD(Exists);
    DISPATCH_YPATH_SERVICE_METHOD(CheckPermission);
    return TYPathServiceBase::DoInvoke(context);
}

bool TObjectProxyBase::IsWriteRequest(IServiceContextPtr context) const
{
    DECLARE_YPATH_SERVICE_WRITE_METHOD(Set);
    DECLARE_YPATH_SERVICE_WRITE_METHOD(Remove);
    return TYPathServiceBase::IsWriteRequest(context);
}

IAttributeDictionary* TObjectProxyBase::GetUserAttributes()
{
    if (!UserAttributes) {
        UserAttributes = DoCreateUserAttributes();
    }
    return ~UserAttributes;
}

ISystemAttributeProvider* TObjectProxyBase::GetSystemAttributeProvider()
{
    return this;
}

std::unique_ptr<IAttributeDictionary> TObjectProxyBase::DoCreateUserAttributes()
{
    return std::unique_ptr<IAttributeDictionary>(new TUserAttributeDictionary(
        Bootstrap->GetObjectManager(),
        GetId()));
}

void TObjectProxyBase::ListSystemAttributes(std::vector<TAttributeInfo>* attributes)
{
    auto* acd = FindThisAcd();
    bool hasAcd = acd;
    bool hasOwner = acd && acd->GetOwner();

    attributes->push_back("id");
    attributes->push_back("type");
    attributes->push_back("ref_counter");
    attributes->push_back("weak_ref_counter");
    attributes->push_back(TAttributeInfo("supported_permissions", true, true));
    attributes->push_back(TAttributeInfo("inherit_acl", hasAcd, true));
    attributes->push_back(TAttributeInfo("acl", hasAcd, true));
    attributes->push_back(TAttributeInfo("owner", hasOwner, false));
    attributes->push_back(TAttributeInfo("effective_acl", true, true));
}

bool TObjectProxyBase::GetSystemAttribute(const Stroka& key, IYsonConsumer* consumer)
{
    auto objectManager = Bootstrap->GetObjectManager();
    auto securityManager = Bootstrap->GetSecurityManager();

    if (key == "id") {
        BuildYsonFluently(consumer)
            .Value(ToString(GetId()));
        return true;
    }

    if (key == "type") {
        BuildYsonFluently(consumer)
            .Value(CamelCaseToUnderscoreCase(TypeFromId(GetId()).ToString()));
        return true;
    }

    if (key == "ref_counter") {
        BuildYsonFluently(consumer)
            .Value(Object->GetObjectRefCounter());
        return true;
    }

    if (key == "weak_ref_counter") {
        BuildYsonFluently(consumer)
            .Value(Object->GetObjectWeakRefCounter());
        return true;
    }

    if (key == "supported_permissions") {
        auto handler = objectManager->GetHandler(Object);
        auto permissions = handler->GetSupportedPermissions();
        BuildYsonFluently(consumer)
            .Value(permissions.Decompose());
        return true;
    }

    auto* acd = FindThisAcd();
    if (acd) {
        if (key == "inherit_acl") {
            BuildYsonFluently(consumer)
                .Value(acd->GetInherit());
            return true;
        }

        if (key == "acl") {
            BuildYsonFluently(consumer)
                .Value(acd->Acl());
            return true;
        }

        if (key == "owner" && acd->GetOwner()) {
            BuildYsonFluently(consumer)
                .Value(acd->GetOwner()->GetName());
            return true;
        }
    }

    if (key == "effective_acl") {
        BuildYsonFluently(consumer)
            .Value(securityManager->GetEffectiveAcl(Object));
        return true;
    }

    return false;
}

TAsyncError TObjectProxyBase::GetSystemAttributeAsync(const Stroka& key, IYsonConsumer* consumer)
{
    return Null;
}

bool TObjectProxyBase::SetSystemAttribute(const Stroka& key, const TYsonString& value)
{
    auto securityManager = Bootstrap->GetSecurityManager();
    auto* acd = FindThisAcd();
    if (acd) {
        if (key == "inherit_acl") {
            ValidateNoTransaction();
            ValidatePermission(EPermissionCheckScope::This, EPermission::Administer);

            acd->SetInherit(ConvertTo<bool>(value));
            return true;
        }

        if (key == "acl") {
            ValidateNoTransaction();
            ValidatePermission(EPermissionCheckScope::This, EPermission::Administer);

            auto supportedPermissions = securityManager->GetSupportedPermissions(Object);
            auto valueNode = ConvertToNode(value);
            TAccessControlList newAcl;
            Deserilize(newAcl, supportedPermissions, valueNode, securityManager);

            acd->ClearEntries();
            FOREACH (const auto& ace, newAcl.Entries) {
                acd->AddEntry(ace);
            }

            return true;
        }

        if (key == "owner") {
            ValidateNoTransaction();

            auto name = ConvertTo<Stroka>(value);
            auto* owner = securityManager->FindSubjectByName(name);
            if (!IsObjectAlive(owner)) {
                THROW_ERROR_EXCEPTION("No such subject %s", ~name.Quote());
            }

            auto* user = securityManager->GetAuthenticatedUser();
            if (user != securityManager->GetRootUser() && user != owner) {
                THROW_ERROR_EXCEPTION(
                    NSecurityClient::EErrorCode::AuthorizationError,
                    "Access denied: can only set owner to self");
            }

            acd->SetOwner(owner);

            return true;
        }
    }
    return false;
}

TObjectBase* TObjectProxyBase::GetSchema(EObjectType type)
{
    auto objectManager = Bootstrap->GetObjectManager();
    return objectManager->GetSchema(type);
}

TObjectBase* TObjectProxyBase::GetThisSchema()
{
    return GetSchema(Object->GetType());
}

void TObjectProxyBase::ValidateTransaction()
{
    if (!GetVersionedId().IsBranched()) {
        THROW_ERROR_EXCEPTION("Operation cannot be performed outside of a transaction");
    }
}

void TObjectProxyBase::ValidateNoTransaction()
{
    if (GetVersionedId().IsBranched()) {
        THROW_ERROR_EXCEPTION("Operation cannot be performed in transaction");
    }
}

void TObjectProxyBase::ValidatePermission(EPermissionCheckScope scope, EPermission permission)
{
    YCHECK(scope == EPermissionCheckScope::This);
    ValidatePermission(Object, permission);
}

void TObjectProxyBase::ValidatePermission(TObjectBase* object, EPermission permission)
{
    YCHECK(object);
    auto securityManager = Bootstrap->GetSecurityManager();
    auto* user = securityManager->GetAuthenticatedUser();
    securityManager->ValidatePermission(object, user, permission);
}

bool TObjectProxyBase::IsRecovery() const
{
    return Bootstrap->GetMetaStateFacade()->GetManager()->IsRecovery();
}

bool TObjectProxyBase::IsLeader() const
{
    return Bootstrap->GetMetaStateFacade()->GetManager()->IsLeader();
}

void TObjectProxyBase::ValidateActiveLeader() const
{
    Bootstrap->GetMetaStateFacade()->ValidateActiveLeader();
}

void TObjectProxyBase::ForwardToLeader(IServiceContextPtr context)
{
    auto metaStateManager = Bootstrap->GetMetaStateFacade()->GetManager();
    auto epochContext = metaStateManager->GetEpochContext();

    LOG_DEBUG("Forwarding request to leader");

    auto cellManager = metaStateManager->GetCellManager();
    auto channel = cellManager->GetMasterChannel(epochContext->LeaderId);

    // Update request path to include the current object id and transaction id.
    auto requestMessage = context->GetRequestMessage();
    NRpc::NProto::TRequestHeader requestHeader;
    YCHECK(ParseRequestHeader(requestMessage, &requestHeader));
    auto versionedId = GetVersionedId();
    requestHeader.set_path(FromObjectId(versionedId.ObjectId) + requestHeader.path());
    SetTransactionId(&requestHeader, versionedId.TransactionId);
    auto updatedRequestMessage = SetRequestHeader(requestMessage, requestHeader);

    TObjectServiceProxy proxy(channel);
    // TODO(babenko): timeout?
    // TODO(babenko): prerequisite transactions?
    // TODO(babenko): authenticated user?
    proxy.SetDefaultTimeout(Bootstrap->GetConfig()->MetaState->RpcTimeout);
    auto batchReq = proxy.ExecuteBatch();
    batchReq->AddRequestMessage(updatedRequestMessage);
    batchReq->Invoke().Subscribe(
        BIND(&TObjectProxyBase::OnLeaderResponse, MakeStrong(this), context));
}

void TObjectProxyBase::OnLeaderResponse(IServiceContextPtr context, TObjectServiceProxy::TRspExecuteBatchPtr batchRsp)
{
    auto responseMessage = batchRsp->GetResponseMessage(0);
    NRpc::NProto::TResponseHeader responseHeader;
    YCHECK(ParseResponseHeader(responseMessage, &responseHeader));
    auto error = FromProto(responseHeader.error());
    LOG_DEBUG(error, "Received response for forwarded request");
    context->Reply(responseMessage);
}

////////////////////////////////////////////////////////////////////////////////

TNontemplateNonversionedObjectProxyBase::TNontemplateNonversionedObjectProxyBase(
    NCellMaster::TBootstrap* bootstrap,
    TObjectBase* object)
    : TObjectProxyBase(bootstrap, object)
{ }

bool TNontemplateNonversionedObjectProxyBase::IsWriteRequest(IServiceContextPtr context) const
{
    DECLARE_YPATH_SERVICE_WRITE_METHOD(Remove);
    return TObjectProxyBase::IsWriteRequest(context);
}

bool TNontemplateNonversionedObjectProxyBase::DoInvoke(IServiceContextPtr context)
{
    DISPATCH_YPATH_SERVICE_METHOD(Remove);
    return TObjectProxyBase::DoInvoke(context);
}

void TNontemplateNonversionedObjectProxyBase::GetSelf(TReqGet* request, TRspGet* response, TCtxGetPtr context)
{
    UNUSED(request);

    response->set_value("#");
    context->Reply();
}

void TNontemplateNonversionedObjectProxyBase::ValidateRemoval()
{
    THROW_ERROR_EXCEPTION("Object cannot be removed explicitly");
}

void TNontemplateNonversionedObjectProxyBase::RemoveSelf(TReqRemove* request, TRspRemove* response, TCtxRemovePtr context)
{
    UNUSED(request);
    UNUSED(response);

    ValidateRemoval();

    if (Object->GetObjectRefCounter() != 1) {
        THROW_ERROR_EXCEPTION("Object is in use");
    }

    auto objectManager = Bootstrap->GetObjectManager();
    objectManager->UnrefObject(Object);

    context->Reply();
}

TVersionedObjectId TNontemplateNonversionedObjectProxyBase::GetVersionedId() const
{
    return TVersionedObjectId(Object->GetId());
}

TAccessControlDescriptor* TNontemplateNonversionedObjectProxyBase::FindThisAcd()
{
    auto securityManager = Bootstrap->GetSecurityManager();
    return securityManager->FindAcd(Object);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NObjectServer
} // namespace NYT

