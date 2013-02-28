#pragma once

#include "ypath_service.h"
#include "yson_producer.h"
#include "tree_builder.h"
#include "forwarding_yson_consumer.h"
#include "attributes.h"
#include "permission.h"

#include <ytlib/misc/assert.h>

#include <ytlib/yson/yson_consumer.h>
#include <ytlib/yson/yson_writer.h>

#include <ytlib/ytree/node.h>
#include <ytlib/ytree/ypath.pb.h>

#include <ytlib/logging/log.h>

#include <ytlib/rpc/service.h>

namespace NYT {
namespace NYTree {

////////////////////////////////////////////////////////////////////////////////

class TYPathServiceBase
    : public virtual IYPathService
{
public:
    virtual void Invoke(NRpc::IServiceContextPtr context) override;
    virtual TResolveResult Resolve(const TYPath& path, NRpc::IServiceContextPtr context) override;
    virtual Stroka GetLoggingCategory() const override;
    virtual bool IsWriteRequest(NRpc::IServiceContextPtr context) const override;
    virtual void SerializeAttributes(
        NYson::IYsonConsumer* consumer,
        const TAttributeFilter& filter) const override;

protected:
    NLog::TLogger Logger;

    void GuardedInvoke(NRpc::IServiceContextPtr context);
    virtual bool DoInvoke(NRpc::IServiceContextPtr context);

    virtual TResolveResult ResolveSelf(const TYPath& path, NRpc::IServiceContextPtr context);
    virtual TResolveResult ResolveAttributes(const TYPath& path, NRpc::IServiceContextPtr context);
    virtual TResolveResult ResolveRecursive(const TYPath& path, NRpc::IServiceContextPtr context);

};

////////////////////////////////////////////////////////////////////////////////

#define DECLARE_SUPPORTS_VERB(verb, ...) \
    class TSupports##verb \
    { \
    protected: \
        DECLARE_RPC_SERVICE_METHOD(NProto, verb); \
        virtual void verb##Self(TReq##verb* request, TRsp##verb* response, TCtx##verb##Ptr context); \
        virtual void verb##Recursive(const TYPath& path, TReq##verb* request, TRsp##verb* response, TCtx##verb##Ptr context); \
        virtual void verb##Attribute(const TYPath& path, TReq##verb* request, TRsp##verb* response, TCtx##verb##Ptr context); \
    private: \
        __VA_ARGS__ \
    }

DECLARE_SUPPORTS_VERB(GetKey);
DECLARE_SUPPORTS_VERB(Get);
DECLARE_SUPPORTS_VERB(Set);
DECLARE_SUPPORTS_VERB(List);
DECLARE_SUPPORTS_VERB(Remove);
DECLARE_SUPPORTS_VERB(Exists, void Reply(TCtxExistsPtr context, bool value); );

#undef DECLARE_SUPPORTS_VERB

////////////////////////////////////////////////////////////////////////////////

class TSupportsPermissions
{
protected:
    virtual ~TSupportsPermissions();

    virtual void ValidatePermission(
        EPermissionCheckScope scope,
        EPermission permission);

};

////////////////////////////////////////////////////////////////////////////////

class TSupportsAttributes
    : public virtual TYPathServiceBase
    , public virtual TSupportsGet
    , public virtual TSupportsList
    , public virtual TSupportsSet
    , public virtual TSupportsRemove
    , public virtual TSupportsExists
    , public virtual TSupportsPermissions
{
protected:
    //! Can be |nullptr|.
    virtual IAttributeDictionary* GetUserAttributes();

    //! Can be |nullptr|.
    virtual ISystemAttributeProvider* GetSystemAttributeProvider();

    virtual TResolveResult ResolveAttributes(
        const NYPath::TYPath& path,
        NRpc::IServiceContextPtr context) override;

    virtual void GetAttribute(
        const TYPath& path,
        TReqGet* request,
        TRspGet* response,
        TCtxGetPtr context) override;

    virtual void ListAttribute(
        const TYPath& path,
        TReqList* request,
        TRspList* response,
        TCtxListPtr context) override;

    virtual void ExistsAttribute(
        const TYPath& path,
        TReqExists* request,
        TRspExists* response,
        TCtxExistsPtr context) override;

    virtual void SetAttribute(
        const TYPath& path,
        TReqSet* request,
        TRspSet* response,
        TCtxSetPtr context) override;

    virtual void RemoveAttribute(
        const TYPath& path,
        TReqRemove* request,
        TRspRemove* response,
        TCtxRemovePtr context) override;

    // This method is called before the attribute with the corresponding key
    // is updated (added, removed or changed).
    virtual void ValidateUserAttributeUpdate(
        const Stroka& key,
        const TNullable<NYTree::TYsonString>& oldValue,
        const TNullable<NYTree::TYsonString>& newValue);

private:
    TFuture< TValueOrError<TYsonString> > DoFindAttribute(const Stroka& key);

    static TValueOrError<TYsonString> DoGetAttributeFragment(const TYPath& path, TValueOrError<TYsonString> wholeYsonOrError);
    TFuture< TValueOrError<TYsonString> > DoGetAttribute(const TYPath& path);

    static bool DoExistsAttributeFragment(const TYPath& path, TValueOrError<TYsonString> wholeYsonOrError);
    TFuture<bool> DoExistsAttribute(const TYPath& path);

    static TValueOrError<TYsonString> DoListAttributeFragment(const TYPath& path, TValueOrError<TYsonString> wholeYsonOrError);
    TFuture< TValueOrError<TYsonString> > DoListAttribute(const TYPath& path);

    void DoSetAttribute(const TYPath& path, const TYsonString& newYson);

    void DoRemoveAttribute(const TYPath& path);

    void GuardedSetSystemAttribute(const Stroka& key, const TYsonString& value);

    void GuardedValidateUserAttributeUpdate(
        const Stroka& key,
        const TNullable<TYsonString>& oldValue,
        const TNullable<TYsonString>& newValue);

};

////////////////////////////////////////////////////////////////////////////////

class TNodeSetterBase
    : public TForwardingYsonConsumer
{
protected:
    TNodeSetterBase(INodePtr node, ITreeBuilder* builder);
    ~TNodeSetterBase();

    void ThrowInvalidType(ENodeType actualType);
    virtual ENodeType GetExpectedType() = 0;

    virtual void OnMyStringScalar(const TStringBuf& value) override;
    virtual void OnMyIntegerScalar(i64 value) override;
    virtual void OnMyDoubleScalar(double value) override;
    virtual void OnMyEntity() override;

    virtual void OnMyBeginList() override;

    virtual void OnMyBeginMap() override;

    virtual void OnMyBeginAttributes() override;
    virtual void OnMyEndAttributes() override;

protected:
    class TAttributesSetter;

    INodePtr Node;
    ITreeBuilder* TreeBuilder;
    INodeFactoryPtr NodeFactory;
    THolder<TAttributesSetter> AttributesSetter;

};

////////////////////////////////////////////////////////////////////////////////

template <class TValue>
class TNodeSetter
{ };

#define DECLARE_SCALAR_TYPE(name, type) \
    template <> \
    class TNodeSetter<I##name##Node> \
        : public TNodeSetterBase \
    { \
    public: \
        TNodeSetter(I##name##Node* wholeNode, ITreeBuilder* builder) \
            : TNodeSetterBase(wholeNode, builder) \
            , Node(wholeNode) \
        { } \
    \
    private: \
        I##name##NodePtr Node; \
        \
        virtual ENodeType GetExpectedType() override \
        { \
            return ENodeType::name; \
        } \
        \
        virtual void On##name##Scalar(NDetail::TScalarTypeTraits<type>::TConsumerType newWholeYson) override \
        { \
            Node->SetValue(type(newWholeYson)); \
        } \
    }

DECLARE_SCALAR_TYPE(String, Stroka);
DECLARE_SCALAR_TYPE(Integer,  i64);
DECLARE_SCALAR_TYPE(Double, double);

#undef DECLARE_SCALAR_TYPE

////////////////////////////////////////////////////////////////////////////////

template <>
class TNodeSetter<IMapNode>
    : public TNodeSetterBase
{
public:
    TNodeSetter(IMapNode* map, ITreeBuilder* builder)
        : TNodeSetterBase(map, builder)
        , Map(map)
    { }

private:
    typedef TNodeSetter<IMapNode> TThis;

    IMapNodePtr Map;
    Stroka ItemKey;

    virtual ENodeType GetExpectedType() override
    {
        return ENodeType::Map;
    }

    virtual void OnMyBeginMap() override
    {
        Map->Clear();
    }

    virtual void OnMyKeyedItem(const TStringBuf& key) override
    {
        ItemKey = key;
        TreeBuilder->BeginTree();
        Forward(TreeBuilder, BIND(&TThis::OnForwardingFinished, this));
    }

    void OnForwardingFinished()
    {
        YCHECK(Map->AddChild(TreeBuilder->EndTree(), ItemKey));
        ItemKey.clear();
    }

    virtual void OnMyEndMap() override
    {
        // Just do nothing.
    }
};

////////////////////////////////////////////////////////////////////////////////

template <>
class TNodeSetter<IListNode>
    : public TNodeSetterBase
{
public:
    TNodeSetter(IListNode* list, ITreeBuilder* builder)
        : TNodeSetterBase(list, builder)
        , List(list)
    { }

private:
    typedef TNodeSetter<IListNode> TThis;

    IListNodePtr List;

    virtual ENodeType GetExpectedType() override
    {
        return ENodeType::List;
    }

    virtual void OnMyBeginList() override
    {
        List->Clear();
    }

    virtual void OnMyListItem() override
    {
        TreeBuilder->BeginTree();
        Forward(TreeBuilder, BIND(&TThis::OnForwardingFinished, this));
    }

    void OnForwardingFinished()
    {
        List->AddChild(TreeBuilder->EndTree());
    }

    virtual void OnMyEndList() override
    {
        // Just do nothing.
    }
};

////////////////////////////////////////////////////////////////////////////////

template <>
class TNodeSetter<IEntityNode>
    : public TNodeSetterBase
{
public:
    TNodeSetter(IEntityNode* entity, ITreeBuilder* builder)
        : TNodeSetterBase(entity, builder)
    { }

private:
    virtual ENodeType GetExpectedType() override
    {
        return ENodeType::Entity;
    }

    virtual void OnMyEntity() override
    {
        // Just do nothing.
    }
};

////////////////////////////////////////////////////////////////////////////////

template <class TNode>
void SetNodeFromProducer(
    TNode* node,
    TYsonProducer producer,
    ITreeBuilder* builder)
{
    YCHECK(node);
    YCHECK(builder);

    TNodeSetter<TNode> setter(node, builder);
    producer.Run(&setter);
}

////////////////////////////////////////////////////////////////////////////////

typedef TCallback<void(NBus::IMessagePtr)> TYPathResponseHandler;

NRpc::IServiceContextPtr CreateYPathContext(
    NBus::IMessagePtr requestMessage,
    const Stroka& loggingCategory,
    TYPathResponseHandler responseHandler);

IYPathServicePtr CreateRootService(IYPathServicePtr underlyingService);

////////////////////////////////////////////////////////////////////////////////

#define DISPATCH_YPATH_SERVICE_METHOD(method) \
    if (context->GetVerb() == #method) { \
        ::NYT::NRpc::THandlerInvocationOptions options; \
        method##Thunk(context, options).Run(); \
        return true; \
    }


#define DISPATCH_YPATH_HEAVY_SERVICE_METHOD(method) \
    if (context->GetVerb() == #method) { \
        ::NYT::NRpc::THandlerInvocationOptions options; \
        options.HeavyResponse = true; \
        method##Thunk(context, options).Run(); \
        return true; \
    }

#define DECLARE_YPATH_SERVICE_WRITE_METHOD(method) \
    if (context->GetVerb() == #method) { \
        return true; \
    }

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT
