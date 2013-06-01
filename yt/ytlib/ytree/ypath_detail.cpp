#include "stdafx.h"
#include "ypath_detail.h"
#include "ypath_client.h"
#include "node_detail.h"

#include <ytlib/ytree/convert.h>
#include <ytlib/ytree/attribute_helpers.h>
#include <ytlib/ytree/system_attribute_provider.h>

#include <ytlib/ypath/tokenizer.h>

#include <ytlib/bus/message.h>

#include <ytlib/rpc/rpc.pb.h>
#include <ytlib/rpc/server_detail.h>
#include <ytlib/rpc/message.h>

namespace NYT {
namespace NYTree {

using namespace NBus;
using namespace NRpc;
using namespace NYPath;
using namespace NRpc::NProto;

////////////////////////////////////////////////////////////////////////////////

IYPathService::TResolveResult TYPathServiceBase::Resolve(
    const TYPath& path,
    IServiceContextPtr context)
{
    NYPath::TTokenizer tokenizer(path);
    switch (tokenizer.Advance()) {
        case NYPath::ETokenType::EndOfStream:
            return ResolveSelf(tokenizer.GetSuffix(), context);

        case NYPath::ETokenType::Slash: {
            if (tokenizer.Advance() == NYPath::ETokenType::At) {
                return ResolveAttributes(tokenizer.GetSuffix(), context);
            } else {
                return ResolveRecursive(tokenizer.GetInput(), context);
            }
        }

        default:
            tokenizer.ThrowUnexpected();
            YUNREACHABLE();
    }
}

IYPathService::TResolveResult TYPathServiceBase::ResolveSelf(
    const TYPath& path,
    IServiceContextPtr context)
{
    UNUSED(context);
    return TResolveResult::Here(path);
}

IYPathService::TResolveResult TYPathServiceBase::ResolveAttributes(
    const TYPath& path,
    IServiceContextPtr context)
{
    UNUSED(path);
    UNUSED(context);
    THROW_ERROR_EXCEPTION("Object cannot have attributes");
}

IYPathService::TResolveResult TYPathServiceBase::ResolveRecursive(
    const TYPath& path,
    IServiceContextPtr context)
{
    UNUSED(path);
    UNUSED(context);
    THROW_ERROR_EXCEPTION("Object cannot have children");
}

void TYPathServiceBase::Invoke(IServiceContextPtr context)
{
    GuardedInvoke(context);
}

void TYPathServiceBase::GuardedInvoke(IServiceContextPtr context)
{
    try {
        if (!DoInvoke(context)) {
            ThrowVerbNotSuppored(context->GetVerb());
        }
    } catch (const std::exception& ex) {
        context->Reply(ex);
    }
}

bool TYPathServiceBase::DoInvoke(IServiceContextPtr context)
{
    UNUSED(context);
    return false;
}

Stroka TYPathServiceBase::GetLoggingCategory() const
{
    return Logger.GetCategory();
}

bool TYPathServiceBase::IsWriteRequest(IServiceContextPtr context) const
{
    UNUSED(context);
    return false;
}

void TYPathServiceBase::SerializeAttributes(
    NYson::IYsonConsumer* consumer,
    const TAttributeFilter& filter)
{
    UNUSED(consumer);
    UNUSED(filter);
}

////////////////////////////////////////////////////////////////////////////////

#define IMPLEMENT_SUPPORTS_VERB_RESOLVE(verb, defaultBehaviour) \
    DEFINE_RPC_SERVICE_METHOD(TSupports##verb, verb) \
    { \
        NYPath::TTokenizer tokenizer(context->GetPath()); \
        switch (tokenizer.Advance()) { \
            case NYPath::ETokenType::EndOfStream: \
                verb##Self(request, response, context); \
                break; \
            \
            case NYPath::ETokenType::Slash: \
                if (tokenizer.Advance() == NYPath::ETokenType::At) { \
                    verb##Attribute(tokenizer.GetSuffix(), request, response, context); \
                } else { \
                    verb##Recursive(tokenizer.GetInput(), request, response, context); \
                } \
                break; \
            \
            default: \
                defaultBehaviour; \
        } \
    }

#define IMPLEMENT_SUPPORTS_VERB(verb) \
    IMPLEMENT_SUPPORTS_VERB_RESOLVE( \
        verb, \
        { \
            tokenizer.ThrowUnexpected(); \
            YUNREACHABLE(); \
        } \
    ) \
    \
    void TSupports##verb::verb##Attribute(const TYPath& path, TReq##verb* request, TRsp##verb* response, TCtx##verb##Ptr context) \
    { \
        UNUSED(path); \
        UNUSED(request); \
        UNUSED(response); \
        NYTree::ThrowVerbNotSuppored(context->GetVerb(), Stroka("attribute")); \
    } \
    \
    void TSupports##verb::verb##Self(TReq##verb* request, TRsp##verb* response, TCtx##verb##Ptr context) \
    { \
        UNUSED(request); \
        UNUSED(response); \
        NYTree::ThrowVerbNotSuppored(context->GetVerb(), Stroka("self")); \
    } \
    \
    void TSupports##verb::verb##Recursive(const TYPath& path, TReq##verb* request, TRsp##verb* response, TCtx##verb##Ptr context) \
    { \
        UNUSED(path); \
        UNUSED(request); \
        UNUSED(response); \
        NYTree::ThrowVerbNotSuppored(context->GetVerb(), Stroka("recursive")); \
    }

IMPLEMENT_SUPPORTS_VERB(GetKey)
IMPLEMENT_SUPPORTS_VERB(Get)
IMPLEMENT_SUPPORTS_VERB(Set)
IMPLEMENT_SUPPORTS_VERB(List)
IMPLEMENT_SUPPORTS_VERB(Remove)

IMPLEMENT_SUPPORTS_VERB_RESOLVE(Exists, { Reply(context, false); })

#undef IMPLEMENT_SUPPORTS_VERB
#undef IMPLEMENT_SUPPORTS_VERB_RESOLVE

void TSupportsExists::Reply(TCtxExistsPtr context, bool value)
{
    context->Response().set_value(value);
    context->SetResponseInfo("Result: %s", ~FormatBool(value));
    context->Reply();
}

void TSupportsExists::ExistsAttribute(
    const TYPath& path,
    TReqExists* request,
    TRspExists* response,
    TCtxExistsPtr context)
{
    UNUSED(path);
    UNUSED(request);
    UNUSED(response);

    context->SetRequestInfo("");

    Reply(context, false);
}

void TSupportsExists::ExistsSelf(
    TReqExists* request,
    TRspExists* response,
    TCtxExistsPtr context)
{
    UNUSED(request);
    UNUSED(response);

    context->SetRequestInfo("");

    Reply(context, true);
}

void TSupportsExists::ExistsRecursive(
    const TYPath& path,
    TReqExists* request,
    TRspExists* response,
    TCtxExistsPtr context)
{
    UNUSED(path);
    UNUSED(request);
    UNUSED(response);

    context->SetRequestInfo("");

    Reply(context, false);
}

////////////////////////////////////////////////////////////////////////////////

TSupportsPermissions::~TSupportsPermissions()
{ }

void TSupportsPermissions::ValidatePermission(
    EPermissionCheckScope scope,
    EPermission permission)
{
    UNUSED(scope);
    UNUSED(permission);
}

////////////////////////////////////////////////////////////////////////////////

static TFuture<bool> TrueFuture = MakeFuture(true);
static TFuture<bool> FalseFuture = MakeFuture(false);

IYPathService::TResolveResult TSupportsAttributes::ResolveAttributes(
    const TYPath& path,
    IServiceContextPtr context)
{
    const auto& verb = context->GetVerb();
    if (verb != "Get" &&
        verb != "Set" &&
        verb != "List" &&
        verb != "Remove" &&
        verb != "Exists")
    {
        ThrowVerbNotSuppored(verb);
    }

    return TResolveResult::Here("/@" + path);
}

TFuture< TErrorOr<TYsonString> > TSupportsAttributes::DoFindAttribute(const Stroka& key)
{
    auto userAttributes = GetUserAttributes();
    auto systemAttributeProvider = GetSystemAttributeProvider();

    if (userAttributes) {
        auto userYson = userAttributes->FindYson(key);
        if (userYson) {
            return MakeFuture(TErrorOr<TYsonString>(userYson.Get()));
        }
    }

    if (systemAttributeProvider) {
        TStringStream syncStream;
        NYson::TYsonWriter syncWriter(&syncStream);
        if (systemAttributeProvider->GetSystemAttribute(key, &syncWriter)) {
            TYsonString systemYson(syncStream.Str());
            return MakeFuture(TErrorOr<TYsonString>(systemYson));
        }

        auto onAsyncAttribute = [] (
            TStringStream* stream,
            NYson::TYsonWriter* writer,
            TError error) ->
            TErrorOr<TYsonString>
        {
            if (error.IsOK()) {
                return TYsonString(stream->Str());
            } else {
                return error;
            }
        };

        std::unique_ptr<TStringStream> asyncStream(new TStringStream());
        std::unique_ptr<NYson::TYsonWriter> asyncWriter(new NYson::TYsonWriter(~asyncStream));
        auto asyncResult = systemAttributeProvider->GetSystemAttributeAsync(key, ~asyncWriter);
        if (asyncResult) {
            return asyncResult.Apply(BIND(
                onAsyncAttribute,
                Owned(asyncStream.release()),
                Owned(asyncWriter.release())));
        }
    }

    return Null;
}

TErrorOr<TYsonString> TSupportsAttributes::DoGetAttributeFragment(
    const TYPath& path,
    TErrorOr<TYsonString> wholeYsonOrError)
{
    if (!wholeYsonOrError.IsOK()) {
        return wholeYsonOrError;
    }
    auto node = ConvertToNode<TYsonString>(wholeYsonOrError.Value());
    try {
        return SyncYPathGet(node, path, TAttributeFilter::All);
    } catch (const std::exception& ex) {
        return ex;
    }
}

TFuture< TErrorOr<TYsonString> > TSupportsAttributes::DoGetAttribute(const TYPath& path)
{
    ValidatePermission(EPermissionCheckScope::This, EPermission::Read);

    auto userAttributes = GetUserAttributes();
    auto systemAttributeProvider = GetSystemAttributeProvider();

    NYPath::TTokenizer tokenizer(path);

    if (tokenizer.Advance() == NYPath::ETokenType::EndOfStream) {
        TStringStream stream;
        NYson::TYsonWriter writer(&stream);

        writer.OnBeginMap();

        if (systemAttributeProvider) {
            std::vector<ISystemAttributeProvider::TAttributeInfo> systemAttributes;
            systemAttributeProvider->ListSystemAttributes(&systemAttributes);
            FOREACH (const auto& attribute, systemAttributes) {
                if (attribute.IsPresent) {
                    writer.OnKeyedItem(attribute.Key);
                    if (attribute.IsOpaque) {
                        writer.OnEntity();
                    } else {
                        YCHECK(systemAttributeProvider->GetSystemAttribute(attribute.Key, &writer));
                    }
                }
            }
        }

        if (userAttributes) {
            FOREACH (const auto& key, userAttributes->List()) {
                writer.OnKeyedItem(key);
                Consume(userAttributes->GetYson(key), &writer);
            }
        }

        writer.OnEndMap();
        TYsonString yson(stream.Str());
        return MakeFuture(TErrorOr<TYsonString>(yson));
    } else {
        tokenizer.Expect(NYPath::ETokenType::Literal);
        auto key = tokenizer.GetLiteralValue();

        auto ysonOrError = DoFindAttribute(key);
        if (!ysonOrError) {
            return MakeFuture(TErrorOr<TYsonString>(TError(
                NYTree::EErrorCode::ResolveError,
                "Attribute %s is not found",
                ~ToYPathLiteral(key).Quote())));
        }

        if (tokenizer.Advance() == NYPath::ETokenType::EndOfStream) {
            return ysonOrError;
        }

        auto suffixPath = tokenizer.GetInput();
        return ysonOrError.Apply(BIND(&TSupportsAttributes::DoGetAttributeFragment, suffixPath));
   }
}

void TSupportsAttributes::GetAttribute(
    const TYPath& path,
    TReqGet* request,
    TRspGet* response,
    TCtxGetPtr context)
{
    DoGetAttribute(path).Subscribe(BIND([=] (TErrorOr<TYsonString> ysonOrError) {
        if (ysonOrError.IsOK()) {
            response->set_value(ysonOrError.Value().Data());
            context->Reply();
        } else {
            context->Reply(ysonOrError);
        }
    }));
}

TErrorOr<TYsonString> TSupportsAttributes::DoListAttributeFragment(
    const TYPath& path,
    TErrorOr<TYsonString> wholeYsonOrError)
{
    if (!wholeYsonOrError.IsOK()) {
        return wholeYsonOrError;
    }

    auto node = ConvertToNode(wholeYsonOrError.Value());

    std::vector<Stroka> listedKeys;
    try {
        listedKeys = SyncYPathList(node, path);
    } catch (const std::exception& ex) {
        return ex;
    }

    TStringStream stream;
    NYson::TYsonWriter writer(&stream);
    writer.OnBeginList();
    FOREACH (const auto& listedKey, listedKeys) {
        writer.OnListItem();
        writer.OnStringScalar(listedKey);
    }
    writer.OnEndList();

    return TYsonString(stream.Str());
}

TFuture< TErrorOr<TYsonString> > TSupportsAttributes::DoListAttribute(const TYPath& path)
{
    ValidatePermission(EPermissionCheckScope::This, EPermission::Read);

    auto userAttributes = GetUserAttributes();
    auto systemAttributeProvider = GetSystemAttributeProvider();

    NYPath::TTokenizer tokenizer(path);

    if (tokenizer.Advance() == NYPath::ETokenType::EndOfStream) {
        TStringStream stream;
        NYson::TYsonWriter writer(&stream);
        writer.OnBeginList();

        if (userAttributes) {
            auto userKeys = userAttributes->List();
            FOREACH (const auto& key, userKeys) {
                writer.OnListItem();
                writer.OnStringScalar(key);
            }
        }

        if (systemAttributeProvider) {
            std::vector<ISystemAttributeProvider::TAttributeInfo> systemAttributes;
            systemAttributeProvider->ListSystemAttributes(&systemAttributes);
            FOREACH (const auto& attribute, systemAttributes) {
                if (attribute.IsPresent) {
                    writer.OnListItem();
                    writer.OnStringScalar(attribute.Key);
                }
            }
        }

        writer.OnEndList();

        TYsonString yson(stream.Str());
        return MakeFuture(TErrorOr<TYsonString>(yson));
    } else  {
        tokenizer.Expect(NYPath::ETokenType::Literal);
        auto key = tokenizer.GetLiteralValue();

        auto ysonOrError = DoFindAttribute(key);
        if (!ysonOrError) {
            return MakeFuture(TErrorOr<TYsonString>(TError(
                NYTree::EErrorCode::ResolveError,
                "Attribute %s is not found",
                ~ToYPathLiteral(key))));
        }

        auto pathSuffix = tokenizer.GetSuffix();
        return ysonOrError.Apply(BIND(&TSupportsAttributes::DoListAttributeFragment, pathSuffix));
    }
}

void TSupportsAttributes::ListAttribute(
    const TYPath& path,
    TReqList* request,
    TRspList* response,
    TCtxListPtr context)
{
    UNUSED(request);

    DoListAttribute(path).Subscribe(BIND([=] (TErrorOr<TYsonString> ysonOrError) {
        if (ysonOrError.IsOK()) {
            response->set_keys(ysonOrError.Value().Data());
            context->Reply();
        } else {
            context->Reply(ysonOrError);
        }
    }));
}

bool TSupportsAttributes::DoExistsAttributeFragment(
    const TYPath& path,
    TErrorOr<TYsonString> wholeYsonOrError)
{
    if (!wholeYsonOrError.IsOK()) {
        return false;
    }
    auto node = ConvertToNode<TYsonString>(wholeYsonOrError.Value());
    try {
        return SyncYPathExists(node, path);
    } catch (const std::exception&) {
        return false;
    }
}

TFuture<bool> TSupportsAttributes::DoExistsAttribute(const TYPath& path)
{
    ValidatePermission(EPermissionCheckScope::This, EPermission::Read);

    auto userAttributes = GetUserAttributes();
    auto systemAttributeProvider = GetSystemAttributeProvider();

    NYPath::TTokenizer tokenizer(path);
    if (tokenizer.Advance() == NYPath::ETokenType::EndOfStream) {
        return TrueFuture;
    }

    tokenizer.Expect(NYPath::ETokenType::Literal);
    auto key = tokenizer.GetLiteralValue();

    if (tokenizer.Advance() == NYPath::ETokenType::EndOfStream) {
        if (userAttributes) {
            auto userYson = userAttributes->FindYson(key);
            if (userYson) {
                return TrueFuture;
            }
        }

        if (systemAttributeProvider) {
            std::vector<ISystemAttributeProvider::TAttributeInfo> systemAttributes;
            systemAttributeProvider->ListSystemAttributes(&systemAttributes);
            FOREACH (const auto& attribute, systemAttributes) {
                if (attribute.Key == key && attribute.IsPresent) {
                    return TrueFuture;
                }
            }
        }

        return FalseFuture;
    } else {
        auto ysonOrError = DoFindAttribute(key);
        if (!ysonOrError) {
            return FalseFuture;
        }

        auto pathSuffix = tokenizer.GetInput();
        return ysonOrError.Apply(BIND(&TSupportsAttributes::DoExistsAttributeFragment, pathSuffix));
    }
}

void TSupportsAttributes::ExistsAttribute(
    const TYPath& path,
    TReqExists* request,
    TRspExists* response,
    TCtxExistsPtr context)
{
    UNUSED(request);

    context->SetRequestInfo("");

    DoExistsAttribute(path).Subscribe(BIND([=] (bool result) {
        response->set_value(result);
        context->SetResponseInfo("Result: %s", ~FormatBool(result));
        context->Reply();
    }));
}

void TSupportsAttributes::DoSetAttribute(const TYPath& path, const TYsonString& newYson)
{
    ValidatePermission(EPermissionCheckScope::This, EPermission::Write);

    auto userAttributes = GetUserAttributes();
    auto systemAttributeProvider = GetSystemAttributeProvider();

    NYPath::TTokenizer tokenizer(path);

    if (tokenizer.Advance() == NYPath::ETokenType::EndOfStream) {
        auto newAttributes = ConvertToAttributes(newYson);

        if (systemAttributeProvider) {
            std::vector<ISystemAttributeProvider::TAttributeInfo> systemAttributes;
            systemAttributeProvider->ListSystemAttributes(&systemAttributes);

            FOREACH (const auto& attribute, systemAttributes) {
                Stroka key(attribute.Key);
                auto newAttributeYson = newAttributes->FindYson(key);
                if (newAttributeYson) {
                    if (!attribute.IsPresent) {
                        ThrowCannotSetSystemAttribute(key);
                    }
                    GuardedSetSystemAttribute(key, newAttributeYson.Get());
                    YCHECK(newAttributes->Remove(key));
                }
            }
        }

        auto newUserKeys = newAttributes->List();

        if (!userAttributes) {
             if (!newUserKeys.empty()) {
                 THROW_ERROR_EXCEPTION("User attributes are not supported");
             }
             return;
        }

        auto oldUserKeys = userAttributes->List();

        FOREACH (const auto& key, newUserKeys) {
            auto newAttributeYson = newAttributes->GetYson(key);
            auto oldAttributeYson = userAttributes->FindYson(key);
            GuardedValidateUserAttributeUpdate(key, oldAttributeYson, newAttributeYson);
            userAttributes->SetYson(key, newAttributeYson);
        }

        FOREACH (const auto& key, oldUserKeys) {
            if (!newAttributes->FindYson(key)) {
                auto oldAttributeYson = userAttributes->GetYson(key);
                GuardedValidateUserAttributeUpdate(key, oldAttributeYson, Null);
                userAttributes->Remove(key);
            }
        }
    } else {
        tokenizer.Expect(NYPath::ETokenType::Literal);
        auto key = tokenizer.GetLiteralValue();

        if (key.Empty()) {
            THROW_ERROR_EXCEPTION("Attribute key cannot be empty");
        }

        const ISystemAttributeProvider::TAttributeInfo* attribute = nullptr;
        std::vector<ISystemAttributeProvider::TAttributeInfo> systemAttributes;
        if (systemAttributeProvider) {
            systemAttributeProvider->ListSystemAttributes(&systemAttributes);
            FOREACH (const auto& currentAttribute, systemAttributes) {
                if (currentAttribute.Key == key) {
                    attribute = &currentAttribute;
                    break;
                }
            }
        }

        if (attribute) {
            if (tokenizer.Advance() == NYPath::ETokenType::EndOfStream) {
                GuardedSetSystemAttribute(key, newYson);
            } else {
                TStringStream stream;
                NYson::TYsonWriter writer(&stream);
                if (!systemAttributeProvider->GetSystemAttribute(key, &writer)) {
                    ThrowNoSuchSystemAttribute(key);
                }

                TYsonString oldWholeYson(stream.Str());
                auto wholeNode = ConvertToNode(oldWholeYson);
                SyncYPathSet(wholeNode, tokenizer.GetInput(), newYson);
                auto newWholeYson = ConvertToYsonString(wholeNode);

                GuardedSetSystemAttribute(key, newWholeYson);
            }
        } else {
            if (!userAttributes) {
                THROW_ERROR_EXCEPTION("User attributes are not supported");
            }
            
            auto oldWholeYson = userAttributes->FindYson(key);
            if (tokenizer.Advance() == NYPath::ETokenType::EndOfStream) {
                GuardedValidateUserAttributeUpdate(key, oldWholeYson, newYson);
                userAttributes->SetYson(key, newYson);
            } else {
                if (!oldWholeYson) {
                    ThrowNoSuchUserAttribute(key);
                }

                auto wholeNode = ConvertToNode(oldWholeYson.Get());
                SyncYPathSet(wholeNode, tokenizer.GetInput(), newYson);
                auto newWholeYson = ConvertToYsonString(wholeNode);

                GuardedValidateUserAttributeUpdate(key, oldWholeYson, newWholeYson);
                userAttributes->SetYson(key, newWholeYson);
            }
        }
    }

    OnUserAttributesUpdated();
}

void TSupportsAttributes::SetAttribute(
    const TYPath& path,
    TReqSet* request,
    TRspSet* response,
    TCtxSetPtr context)
{
    UNUSED(response);
    UNUSED(response);

    context->SetRequestInfo("");

    DoSetAttribute(path, TYsonString(request->value()));
    
    context->Reply();
}

void TSupportsAttributes::DoRemoveAttribute(const TYPath& path)
{
    ValidatePermission(EPermissionCheckScope::This, EPermission::Write);

    auto userAttributes = GetUserAttributes();
    auto systemAttributeProvider = GetSystemAttributeProvider();

    NYPath::TTokenizer tokenizer(path);
    tokenizer.Advance();
    tokenizer.Expect(NYPath::ETokenType::Literal);

    if (tokenizer.GetToken() == WildcardToken) {
        if (userAttributes) {
            auto userKeys = userAttributes->List();
            FOREACH (const auto& key, userKeys) {
                GuardedValidateUserAttributeUpdate(key, userAttributes->GetYson(key), Null);
            }
            FOREACH (const auto& key, userKeys) {
                YCHECK(userAttributes->Remove(key));
            }
        }
    } else {
        tokenizer.Expect(NYPath::ETokenType::Literal);
        auto key = tokenizer.GetLiteralValue();

        auto userYson = userAttributes ? userAttributes->FindYson(key) : TNullable<TYsonString>(Null);
        if (tokenizer.Advance() == NYPath::ETokenType::EndOfStream) {
            if (!userYson) {
                if (systemAttributeProvider) {
                    auto* attributeInfo = systemAttributeProvider->FindSystemAttributeInfo(key);
                    if (attributeInfo) {
                        ThrowCannotRemoveAttribute(key);
                    } else {
                        ThrowNoSuchUserAttribute(key);
                    }
                } else {
                    ThrowNoSuchUserAttribute(key);
                }
            }

            GuardedValidateUserAttributeUpdate(key, userYson, Null);
            YCHECK(userAttributes->Remove(key));
        } else {
            if (userYson) {
                auto userNode = ConvertToNode(userYson);
                SyncYPathRemove(userNode, tokenizer.GetInput());
                auto updatedUserYson = ConvertToYsonString(userNode);

                GuardedValidateUserAttributeUpdate(key, userYson, updatedUserYson);
                userAttributes->SetYson(key, updatedUserYson);
            } else {
                TStringStream stream;
                NYson::TYsonWriter writer(&stream);
                if (!systemAttributeProvider || !systemAttributeProvider->GetSystemAttribute(key, &writer)) {
                    ThrowNoSuchSystemAttribute(key);
                }

                TYsonString systemYson(stream.Str());
                auto systemNode = ConvertToNode(systemYson);
                SyncYPathRemove(systemNode, tokenizer.GetInput());
                auto updatedSystemYson = ConvertToYsonString(systemNode);

                GuardedSetSystemAttribute(key, updatedSystemYson);
            }
        }
    }

    OnUserAttributesUpdated();
}

void TSupportsAttributes::RemoveAttribute(
    const TYPath& path,
    TReqRemove* request,
    TRspRemove* response,
    TCtxRemovePtr context)
{
    UNUSED(request);
    UNUSED(response);

    context->SetRequestInfo("");

    DoRemoveAttribute(path);

    context->Reply();
}

void TSupportsAttributes::ValidateUserAttributeUpdate(
    const Stroka& key,
    const TNullable<TYsonString>& oldValue,
    const TNullable<TYsonString>& newValue)
{
    UNUSED(key);
    UNUSED(oldValue);
    UNUSED(newValue);
}

void TSupportsAttributes::OnUserAttributesUpdated()
{}

IAttributeDictionary* TSupportsAttributes::GetUserAttributes()
{
    return nullptr;
}

ISystemAttributeProvider* TSupportsAttributes::GetSystemAttributeProvider()
{
    return nullptr;
}

void TSupportsAttributes::GuardedSetSystemAttribute(const Stroka& key, const TYsonString& yson)
{
    bool result;
    try {
        result = GetSystemAttributeProvider()->SetSystemAttribute(key, yson);
    } catch (const std::exception& ex) {
        THROW_ERROR_EXCEPTION("Error setting system attribute %s",
            ~ToYPathLiteral(key).Quote())
            << ex;
    }

    if (!result) {
        ThrowCannotSetSystemAttribute(key);
    }
}

void TSupportsAttributes::GuardedValidateUserAttributeUpdate(
    const Stroka& key,
    const TNullable<TYsonString>& oldValue,
    const TNullable<TYsonString>& newValue)
{
    try {
        ValidateUserAttributeUpdate(key, oldValue, newValue);
    } catch (const std::exception& ex) {
        if (newValue) {
            THROW_ERROR_EXCEPTION("Error setting user attribute %s",
                ~ToYPathLiteral(key).Quote())
                << ex;
        } else {
            THROW_ERROR_EXCEPTION("Error removing user attribute %s",
                ~ToYPathLiteral(key).Quote())
                << ex;
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

class TNodeSetterBase::TAttributesSetter
    : public TForwardingYsonConsumer
{
public:
    explicit TAttributesSetter(IAttributeDictionary* attributes)
        : Attributes(attributes)
    { }

private:
    IAttributeDictionary* Attributes;

    TStringStream AttributeStream;
    std::unique_ptr<NYson::TYsonWriter> AttributeWriter;

    virtual void OnMyKeyedItem(const TStringBuf& key) override
    {
        Stroka localKey(key);
        AttributeWriter.reset(new NYson::TYsonWriter(&AttributeStream));
        Forward(
            ~AttributeWriter,
            BIND ([=] () {
                AttributeWriter.reset();
                Attributes->SetYson(localKey, TYsonString(AttributeStream.Str()));
                AttributeStream.clear();
            }));
    }
};

////////////////////////////////////////////////////////////////////////////////

TNodeSetterBase::TNodeSetterBase(INodePtr node, ITreeBuilder* builder)
    : Node(node)
    , TreeBuilder(builder)
    , NodeFactory(node->CreateFactory())
{
    Node->MutableAttributes()->Clear();
}

TNodeSetterBase::~TNodeSetterBase()
{ }

void TNodeSetterBase::ThrowInvalidType(ENodeType actualType)
{
    THROW_ERROR_EXCEPTION("Invalid node type: expected %s, actual %s",
        ~GetExpectedType().ToString(),
        ~actualType.ToString());
}

void TNodeSetterBase::OnMyStringScalar(const TStringBuf& value)
{
    UNUSED(value);

    ThrowInvalidType(ENodeType::String);
}

void TNodeSetterBase::OnMyIntegerScalar(i64 value)
{
    UNUSED(value);

    ThrowInvalidType(ENodeType::Integer);
}

void TNodeSetterBase::OnMyDoubleScalar(double value)
{
    UNUSED(value);

    ThrowInvalidType(ENodeType::Double);
}

void TNodeSetterBase::OnMyEntity()
{
    ThrowInvalidType(ENodeType::Entity);
}

void TNodeSetterBase::OnMyBeginList()
{
    ThrowInvalidType(ENodeType::List);
}

void TNodeSetterBase::OnMyBeginMap()
{
    ThrowInvalidType(ENodeType::Map);
}

void TNodeSetterBase::OnMyBeginAttributes()
{
    AttributesSetter.reset(new TAttributesSetter(Node->MutableAttributes()));
    Forward(~AttributesSetter, TClosure(), NYson::EYsonType::MapFragment);
}

void TNodeSetterBase::OnMyEndAttributes()
{
    AttributesSetter.reset();
}

////////////////////////////////////////////////////////////////////////////////

class TYPathServiceContext
    : public TServiceContextBase
{
public:
    TYPathServiceContext(
        const TRequestHeader& header,
        IMessagePtr requestMessage,
        TYPathResponseHandler responseHandler,
        const Stroka& loggingCategory)
        : TServiceContextBase(header, requestMessage)
        , ResponseHandler(responseHandler)
        , Logger(loggingCategory)
    { }

protected:
    TYPathResponseHandler ResponseHandler;
    NLog::TLogger Logger;

    virtual void DoReply(IMessagePtr responseMessage) override
    {
        if (!ResponseHandler.IsNull()) {
            ResponseHandler.Run(responseMessage);
        }
    }

    virtual void LogRequest() override
    {
        Stroka str;
        AppendInfo(str, RequestInfo);
        LOG_DEBUG("%s %s <- %s",
            ~GetVerb(),
            ~GetPath(),
            ~str);
    }

    virtual void LogResponse(const TError& error) override
    {
        Stroka str;
        AppendInfo(str, Sprintf("Error: %s", ~ToString(error)));
        AppendInfo(str, ResponseInfo);
        LOG_DEBUG("%s %s -> %s",
            ~GetVerb(),
            ~GetPath(),
            ~str);
    }

};

IServiceContextPtr CreateYPathContext(
    IMessagePtr requestMessage,
    const Stroka& loggingCategory,
    TYPathResponseHandler responseHandler)
{
    YASSERT(requestMessage);

    NRpc::NProto::TRequestHeader requestHeader;
    YCHECK(ParseRequestHeader(requestMessage, &requestHeader));
    return New<TYPathServiceContext>(
        requestHeader,
        requestMessage,
        responseHandler,
        loggingCategory);
}

////////////////////////////////////////////////////////////////////////////////

class TRootService
    : public IYPathService
{
public:
    explicit TRootService(IYPathServicePtr underlyingService)
        : UnderlyingService(underlyingService)
    { }

    virtual void Invoke(IServiceContextPtr context) override
    {
        UNUSED(context);
        YUNREACHABLE();
    }

    virtual TResolveResult Resolve(const
        TYPath& path,
        IServiceContextPtr context) override
    {
        UNUSED(context);

        NYPath::TTokenizer tokenizer(path);
        if (tokenizer.Advance() != NYPath::ETokenType::Slash) {
            THROW_ERROR_EXCEPTION("YPath must start with \"/\"");
        }

        return TResolveResult::There(UnderlyingService, tokenizer.GetSuffix());
    }

    virtual Stroka GetLoggingCategory() const override
    {
        return UnderlyingService->GetLoggingCategory();
    }

    virtual bool IsWriteRequest(IServiceContextPtr context) const override
    {
        UNUSED(context);
        YUNREACHABLE();
    }

    // TODO(panin): remove this when getting rid of IAttributeProvider
    virtual void SerializeAttributes(
        NYson::IYsonConsumer* consumer,
        const TAttributeFilter& filter) override
    {
        UnderlyingService->SerializeAttributes(consumer, filter);
    }

private:
    IYPathServicePtr UnderlyingService;

};

IYPathServicePtr CreateRootService(IYPathServicePtr underlyingService)
{
    return New<TRootService>(underlyingService);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT
