#pragma once

#include "public.h"
#include "node.h"

#include <ytlib/ytree/node.h>
#include <ytlib/ytree/system_attribute_provider.h>

#include <ytlib/rpc/service_detail.h>

#include <ytlib/cypress_client/cypress_ypath.pb.h>

#include <server/object_server/object_proxy.h>

#include <server/security_server/cluster_resources.h>

#include <server/transaction_server/public.h>

namespace NYT {
namespace NCypressServer {

////////////////////////////////////////////////////////////////////////////////

//! Extends NYTree::INodeFactory by adding Cypress-specific functionality.
struct ICypressNodeFactory
    : public NYTree::INodeFactory
{
    typedef NRpc::TTypedServiceRequest<NCypressClient::NProto::TReqCreate> TReqCreate;
    typedef NRpc::TTypedServiceResponse<NCypressClient::NProto::TRspCreate> TRspCreate;

    virtual NTransactionServer::TTransaction* GetTransaction() = 0;

    virtual NSecurityServer::TAccount* GetNewNodeAccount() = 0;
    virtual NSecurityServer::TAccount* GetClonedNodeAccount(
        TCypressNodeBase* sourceNode) = 0;

    virtual ICypressNodeProxyPtr CreateNode(
        NObjectClient::EObjectType type,
        NYTree::IAttributeDictionary* attributes,
        TReqCreate* request,
        TRspCreate* response) = 0;

    virtual TCypressNodeBase* CloneNode(
        TCypressNodeBase* sourceNode) = 0;

};

////////////////////////////////////////////////////////////////////////////////

//! Extends NYTree::INode by adding functionality that is common to all
//! logical Cypress nodes.
struct ICypressNodeProxy
    : public virtual NYTree::INode
    , public virtual NYTree::ISystemAttributeProvider
    , public virtual NObjectServer::IObjectProxy
{
    //! Returns the transaction for which the proxy is created.
    virtual NTransactionServer::TTransaction* GetTransaction() const = 0;

    //! Returns the trunk node for which the proxy is created.
    virtual TCypressNodeBase* GetTrunkNode() const = 0;

    //! Returns resources used by the object.
    /*!
     *  This is displayed in @resource_usage attribute and is not used for accounting.
     *
     *  \see #ICypressNode::GetResourceUsage
     */
    virtual NSecurityServer::TClusterResources GetResourceUsage() const = 0;

    //! "Covariant" extension of NYTree::INode::CreateFactory.
    virtual ICypressNodeFactoryPtr CreateCypressFactory(
        bool preserveAccount) const = 0;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NCypressServer
} // namespace NYT
