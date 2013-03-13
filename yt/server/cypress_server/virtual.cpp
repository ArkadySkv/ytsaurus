#include "stdafx.h"
#include "virtual.h"

#include <ytlib/misc/singleton.h>

#include <ytlib/ypath/tokenizer.h>

#include <server/cypress_server/node_detail.h>
#include <server/cypress_server/node_proxy_detail.h>

#include <server/cell_master/bootstrap.h>

namespace NYT {
namespace NCypressServer {

using namespace NRpc;
using namespace NYTree;
using namespace NCellMaster;
using namespace NTransactionServer;
using namespace NObjectServer;

////////////////////////////////////////////////////////////////////////////////

class TVirtualNode
    : public TCypressNodeBase
{
public:
    explicit TVirtualNode(const TVersionedNodeId& id)
        : TCypressNodeBase(id)
    { }
};

////////////////////////////////////////////////////////////////////////////////

class TFailedLeaderValidationWrapper
    : public IYPathService
{
public:
    explicit TFailedLeaderValidationWrapper(TBootstrap* bootstrap)
        : Bootstrap(bootstrap)
    { }

    virtual TResolveResult Resolve(const TYPath& path, IServiceContextPtr context) override
    {
        UNUSED(context);

        return TResolveResult::Here(path);
    }

    virtual void Invoke(IServiceContextPtr context) override
    {
        UNUSED(context);

        Bootstrap->GetMetaStateFacade()->ValidateActiveLeader();
    }

    virtual Stroka GetLoggingCategory() const override
    {
        return "";
    }

    virtual bool IsWriteRequest(IServiceContextPtr context) const override
    {
        UNUSED(context);

        return false;
    }

    // TODO(panin): remove this when getting rid of IAttributeProvider
    virtual void SerializeAttributes(
        NYson::IYsonConsumer* consumer,
        const TAttributeFilter& filter) override
    {
        YUNREACHABLE();
    }

private:
    TBootstrap* Bootstrap;
};

class TLeaderValidatorWrapper
    : public IYPathService
{
public:
    TLeaderValidatorWrapper(
        TBootstrap* bootstrap,
        IYPathServicePtr underlyingService)
        : Bootstrap(bootstrap)
        , UnderlyingService(underlyingService)
    { }

    virtual TResolveResult Resolve(const TYPath& path, IServiceContextPtr context) override
    {
        if (!Bootstrap->GetMetaStateFacade()->IsActiveLeader()) {
            return TResolveResult::There(
                New<TFailedLeaderValidationWrapper>(Bootstrap),
                path);
        }
        return UnderlyingService->Resolve(path, context);
    }

    virtual void Invoke(IServiceContextPtr context) override
    {
        Bootstrap->GetMetaStateFacade()->ValidateActiveLeader();
        UnderlyingService->Invoke(context);
    }

    virtual Stroka GetLoggingCategory() const override
    {
        return UnderlyingService->GetLoggingCategory();
    }

    virtual bool IsWriteRequest(IServiceContextPtr context) const override
    {
        return UnderlyingService->IsWriteRequest(context);
    }

    // TODO(panin): remove this when getting rid of IAttributeProvider
    virtual void SerializeAttributes(
        NYson::IYsonConsumer* consumer,
        const TAttributeFilter& filter) override
    {
        UnderlyingService->SerializeAttributes(consumer, filter);
    }

private:
    TBootstrap* Bootstrap;
    IYPathServicePtr UnderlyingService;

};

////////////////////////////////////////////////////////////////////////////////

class TVirtualNodeProxy
    : public TCypressNodeProxyBase<TNontemplateCypressNodeProxyBase, IEntityNode, TVirtualNode>
{
public:
    TVirtualNodeProxy(
        INodeTypeHandlerPtr typeHandler,
        TBootstrap* bootstrap,
        TTransaction* transaction,
        TVirtualNode* trunkNode,
        IYPathServicePtr service,
        bool requireLeader)
        : TBase(
            typeHandler,
            bootstrap,
            transaction,
            trunkNode)
        , Service(service)
    { }

    virtual TResolveResult Resolve(const TYPath& path, IServiceContextPtr context) override
    {
        NYPath::TTokenizer tokenizer(path);
        if (tokenizer.Advance() == NYPath::ETokenType::Ampersand) {
            return TBase::Resolve(tokenizer.GetSuffix(), context);
        }

        if (RequireLeader && !Bootstrap->GetMetaStateFacade()->IsActiveLeader()) {
            return TResolveResult::Here(path);
        }

        return TResolveResult::There(Service, path);
    }

private:
    typedef TCypressNodeProxyBase<TNontemplateCypressNodeProxyBase, IEntityNode, TVirtualNode> TBase;

    IYPathServicePtr Service;
    bool RequireLeader;

    virtual bool DoInvoke(NRpc::IServiceContextPtr context) override
    {
        auto metaStateFacade = Bootstrap->GetMetaStateFacade();
        if (RequireLeader && !metaStateFacade->GetManager()->GetMutationContext()) {
            metaStateFacade->ValidateActiveLeader();
        }
        return TBase::DoInvoke(context);
    }

};

////////////////////////////////////////////////////////////////////////////////

class TVirtualNodeTypeHandler
    : public TCypressNodeTypeHandlerBase<TVirtualNode>
{
public:
    typedef TVirtualNodeTypeHandler TThis;

    TVirtualNodeTypeHandler(
        TBootstrap* bootstrap,
        TYPathServiceProducer producer,
        EObjectType objectType,
        bool requireLeader)
        : TCypressNodeTypeHandlerBase<TVirtualNode>(bootstrap)
        , Producer(producer)
        , ObjectType(objectType)
        , RequireLeader(requireLeader)
    { }

    virtual EObjectType GetObjectType() override
    {
        return ObjectType;
    }

    virtual ENodeType GetNodeType() override
    {
        return ENodeType::Entity;
    }

private:
    TYPathServiceProducer Producer;
    EObjectType ObjectType;
    bool RequireLeader;

    virtual ICypressNodeProxyPtr DoGetProxy(
        TVirtualNode* trunkNode,
        TTransaction* transaction) override
    {
        auto service = Producer.Run(trunkNode, transaction);
        return New<TVirtualNodeProxy>(
            this,
            Bootstrap,
            transaction,
            trunkNode,
            service,
            RequireLeader);
    }


};

INodeTypeHandlerPtr CreateVirtualTypeHandler(
    TBootstrap* bootstrap,
    EObjectType objectType,
    TYPathServiceProducer producer,
    bool requireLeader)
{
    return New<TVirtualNodeTypeHandler>(
        bootstrap,
        producer,
        objectType,
        requireLeader);
}

INodeTypeHandlerPtr CreateVirtualTypeHandler(
    TBootstrap* bootstrap,
    EObjectType objectType,
    IYPathServicePtr service,
    bool requireLeader)
{
    return CreateVirtualTypeHandler(
        bootstrap,
        objectType,
        BIND([=] (TCypressNodeBase* trunkNode, TTransaction* transaction) -> IYPathServicePtr {
            UNUSED(trunkNode);
            UNUSED(transaction);
            return service;
        }),
        requireLeader);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NCypressServer
} // namespace NYT
