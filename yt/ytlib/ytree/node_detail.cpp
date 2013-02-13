#include "stdafx.h"
#include "node_detail.h"

#include "convert.h"
#include "ypath_detail.h"
#include "ypath_service.h"
#include "tree_visitor.h"
#include "tree_builder.h"
#include "ypath_client.h"

#include <ytlib/yson/yson_writer.h>
#include <ytlib/yson/tokenizer.h>

#include <ytlib/ypath/token.h>
#include <ytlib/ypath/tokenizer.h>

#include <ytlib/misc/protobuf_helpers.h>

namespace NYT {
namespace NYTree {

using namespace NRpc;
using namespace NYPath;
using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

bool TNodeBase::IsWriteRequest(IServiceContextPtr context) const
{
    DECLARE_YPATH_SERVICE_WRITE_METHOD(Set);
    DECLARE_YPATH_SERVICE_WRITE_METHOD(Remove);
    return TYPathServiceBase::IsWriteRequest(context);
}

void TNodeBase::DoInvoke(IServiceContextPtr context)
{
    DISPATCH_YPATH_SERVICE_METHOD(GetKey);
    DISPATCH_YPATH_SERVICE_METHOD(Get);
    DISPATCH_YPATH_SERVICE_METHOD(Set);
    DISPATCH_YPATH_SERVICE_METHOD(Remove);
    DISPATCH_YPATH_SERVICE_METHOD(List);
    DISPATCH_YPATH_SERVICE_METHOD(Exists);
    TYPathServiceBase::DoInvoke(context);
}

void TNodeBase::GetSelf(TReqGet* request, TRspGet* response, TCtxGetPtr context)
{
    TNullable< std::vector<Stroka> > attributesToVisit;

    auto attributeFilter =
        request->has_attribute_filter()
        ? FromProto(request->attribute_filter())
        : TAttributeFilter::None;

    TStringStream stream;
    TYsonWriter writer(&stream);

    VisitTree(this, &writer, attributeFilter);

    response->set_value(stream.Str());
    context->Reply();
}

void TNodeBase::GetKeySelf(TReqGetKey* request, TRspGetKey* response, TCtxGetKeyPtr context)
{
    UNUSED(request);

    context->SetResponseInfo("Get Key request");
    
    auto parent = GetParent();
    if (!parent) {
        THROW_ERROR_EXCEPTION("Node has no parent");
    }

    Stroka key;
    switch (parent->GetType()) {
        case ENodeType::Map:
            key = parent->AsMap()->GetChildKey(this);
            break;

        case ENodeType::List:
            key = ToString(parent->AsList()->GetChildIndex(this));
            break;

        default:
            YUNREACHABLE();
    }

    response->set_value(key);

    context->Reply();
}

void TNodeBase::RemoveSelf(TReqRemove* request, TRspRemove* response, TCtxRemovePtr context)
{
    UNUSED(request);
    UNUSED(response);

    auto parent = GetParent();
    if (!parent) {
        THROW_ERROR_EXCEPTION("Cannot remove the root");
    }

    if (!request->recursive() && GetType() == ENodeType::Composite && AsComposite()->GetChildCount() > 0) {
        THROW_ERROR_EXCEPTION("Cannot remove non-empty composite node when recursive option is not set.");
    }

    parent->AsComposite()->RemoveChild(this);
    context->Reply();
}

IYPathService::TResolveResult TNodeBase::ResolveRecursive(
    const NYPath::TYPath& path,
    IServiceContextPtr context)
{
    UNUSED(context);

    if (context->GetVerb() == "Exists") {
        return TResolveResult::Here(path);
    }
    ThrowCannotHaveChildren(this);
    YUNREACHABLE();
}

////////////////////////////////////////////////////////////////////////////////

void TCompositeNodeMixin::SetRecursive(
    const TYPath& path,
    TReqSet* request,
    TRspSet* response,
    TCtxSetPtr context)
{
    UNUSED(response);

    auto factory = CreateFactory();
    auto value = ConvertToNode(TYsonString(request->value()), ~factory);
    SetChild(path, value);

    context->Reply();
}

void TCompositeNodeMixin::RemoveRecursive(
    const TYPath& path,
    TSupportsRemove::TReqRemove* request,
    TSupportsRemove::TRspRemove* response,
    TSupportsRemove::TCtxRemovePtr context)
{
    UNUSED(request);
    UNUSED(response);

    NYPath::TTokenizer tokenizer(path);
    tokenizer.Advance();
    if (tokenizer.GetType() != NYPath::ETokenType::Literal) {
        tokenizer.ThrowUnexpected();
    }

    tokenizer.Advance();
    tokenizer.Expect(NYPath::ETokenType::EndOfStream);

    Clear();

    context->Reply();
}

////////////////////////////////////////////////////////////////////////////////

IYPathService::TResolveResult TMapNodeMixin::ResolveRecursive(
    const TYPath& path,
    IServiceContextPtr context)
{
    const auto& verb = context->GetVerb();

    NYPath::TTokenizer tokenizer(path);
    tokenizer.Advance();
    tokenizer.Expect(NYPath::ETokenType::Literal);

    if (tokenizer.GetToken() == WildcardToken) {
        if (verb != "Remove") {
            THROW_ERROR_EXCEPTION("\"%s\" is only allowed for Remove verb",
                WildcardToken);
        }

        tokenizer.Advance();
        tokenizer.Expect(NYPath::ETokenType::EndOfStream);

        return IYPathService::TResolveResult::Here("/" + path);
    } else {
        auto key = tokenizer.GetLiteralValue();
        if (key.Empty()) {
            THROW_ERROR_EXCEPTION("Child key cannot be empty");
        }

        auto child = FindChild(key);
        if (!child) {
            if (verb == "Exists" ||
                ((verb == "Set" || verb == "Create" || verb == "Copy") &&
                 tokenizer.Advance() == NYPath::ETokenType::EndOfStream))
            {
                return IYPathService::TResolveResult::Here("/" + path);
            } else {
                ThrowNoSuchChildKey(this, key);
            }
        }

        return IYPathService::TResolveResult::There(child, tokenizer.GetSuffix());
    }
}

void TMapNodeMixin::ListSelf(TReqList* request, TRspList* response, TCtxListPtr context)
{
    auto attributeFilter =
        request->has_attribute_filter()
        ? FromProto(request->attribute_filter())
        : TAttributeFilter::None;

    TStringStream stream;
    TYsonWriter writer(&stream);
    
    writer.OnBeginList();
    FOREACH (const auto& pair, GetChildren()) {
        const auto& key = pair.First();
        const auto& node = pair.Second();
        writer.OnListItem();
        node->SerializeAttributes(&writer, attributeFilter);
        writer.OnStringScalar(key);
    }
    writer.OnEndList();

    response->set_keys(stream.Str());

    context->Reply();
}

void TMapNodeMixin::SetChild(const TYPath& path, INodePtr value)
{
    NYPath::TTokenizer tokenizer(path);
    tokenizer.Advance();
    tokenizer.Expect(NYPath::ETokenType::Literal);
    auto key = tokenizer.GetLiteralValue();
    
    tokenizer.Advance();
    tokenizer.Expect(NYPath::ETokenType::EndOfStream);

    AddChild(value, key);
}

////////////////////////////////////////////////////////////////////////////////

IYPathService::TResolveResult TListNodeMixin::ResolveRecursive(
    const TYPath& path,
    IServiceContextPtr context)
{
    NYPath::TTokenizer tokenizer(path);
    tokenizer.Advance();
    tokenizer.Expect(NYPath::ETokenType::Literal);

    const auto& token = tokenizer.GetToken();
    if (token == WildcardToken ||
        token == ListBeginToken ||
        token == ListEndToken)
    {
        tokenizer.Advance();
        tokenizer.Expect(NYPath::ETokenType::EndOfStream);

        return IYPathService::TResolveResult::Here("/" + path);
    } else if (token.has_prefix(ListBeforeToken) ||
               token.has_prefix(ListAfterToken))
    {
        auto indexToken = ExtractListIndex(token);
        int index = ParseListIndex(indexToken);
        AdjustChildIndex(index);
        
        tokenizer.Advance();
        tokenizer.Expect(NYPath::ETokenType::EndOfStream);

        return IYPathService::TResolveResult::Here("/" + path);
    } else {
        int index = ParseListIndex(token);
        int adjustedIndex = AdjustChildIndex(index);
        auto child = FindChild(adjustedIndex);
        const auto& verb = context->GetVerb();
        if (!child && verb == "Exists") {
        	return IYPathService::TResolveResult::Here("/" + path);
        }
        return IYPathService::TResolveResult::There(child, tokenizer.GetSuffix());
    }
}

void TListNodeMixin::SetChild(const TYPath& path, INodePtr value)
{
    int beforeIndex = -1;

    NYPath::TTokenizer tokenizer(path);
    tokenizer.Advance();
    tokenizer.Expect(NYPath::ETokenType::Literal);

    const auto& token = tokenizer.GetToken();
    if (token.has_prefix(ListBeginToken)) {
        beforeIndex = 0;
    } else if (token.has_prefix(ListEndToken)) {
        beforeIndex = GetChildCount();
    } else if (token.has_prefix(ListBeforeToken) ||
               token.has_prefix(ListAfterToken))
    {
        auto indexToken = ExtractListIndex(token);
        int index = ParseListIndex(indexToken);
        beforeIndex = AdjustChildIndex(index);
        if (token.has_prefix(ListAfterToken)) {
            ++beforeIndex;
        }
    } else {
        tokenizer.ThrowUnexpected();
    }

    tokenizer.Advance();
    tokenizer.Expect(NYPath::ETokenType::EndOfStream);

    AddChild(value, beforeIndex);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT

