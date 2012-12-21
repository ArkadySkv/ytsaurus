#pragma once

#include "table_node.h"

#include <ytlib/ytree/ypath_service.h>
#include <ytlib/ytree/public.h>

#include <ytlib/table_client/schema.h>

#include <ytlib/table_client/table_ypath.pb.h>
#include <server/chunk_server/public.h>

#include <server/cypress_server/node_proxy_detail.h>

#include <server/cell_master/public.h>

namespace NYT {
namespace NTableServer {

////////////////////////////////////////////////////////////////////////////////

class TTableNodeProxy
    : public NCypressServer::TCypressNodeProxyBase<NYTree::IEntityNode, TTableNode>
{
public:
    TTableNodeProxy(
        NCypressServer::INodeTypeHandlerPtr typeHandler,
        NCellMaster::TBootstrap* bootstrap,
        NTransactionServer::TTransaction* transaction,
        NCypressServer::ICypressNode* trunkNode);

    virtual TResolveResult Resolve(
        const NYPath::TYPath& path,
        NRpc::IServiceContextPtr context) override;
    
    virtual bool IsWriteRequest(NRpc::IServiceContextPtr context) const override;

    virtual NCypressServer::TClusterResources GetResourceUsage() const override;

private:
    typedef NCypressServer::TCypressNodeProxyBase<NYTree::IEntityNode, TTableNode> TBase;

    class TFetchChunkVisitor;
    typedef TIntrusivePtr<TFetchChunkVisitor> TFetchChunkProcessorPtr;

    virtual void ListSystemAttributes(std::vector<TAttributeInfo>* attributes) const override;
    virtual bool GetSystemAttribute(const Stroka& key, NYson::IYsonConsumer* consumer) const override;
    virtual TAsyncError GetSystemAttributeAsync(const Stroka& key, NYson::IYsonConsumer* consumer) const override;
    virtual void ValidateUserAttributeUpdate(
        const Stroka& key,
        const TNullable<NYTree::TYsonString>& oldValue,
        const TNullable<NYTree::TYsonString>& newValue) override;
    virtual bool SetSystemAttribute(const Stroka& key, const NYTree::TYsonString& value) override;

    virtual void DoInvoke(NRpc::IServiceContextPtr context) override;

    void ParseYPath(
        const NYPath::TYPath& path,
        NTableClient::TChannel* channel,
        NTableClient::NProto::TReadLimit* lowerBound,
        NTableClient::NProto::TReadLimit* upperBound);

    NChunkServer::TChunkList* EnsureNodeMutable(TTableNode* node);
    void ClearNode(TTableNode* node);

    DECLARE_RPC_SERVICE_METHOD(NTableClient::NProto, GetChunkListForUpdate);
    DECLARE_RPC_SERVICE_METHOD(NTableClient::NProto, Fetch);
    DECLARE_RPC_SERVICE_METHOD(NTableClient::NProto, SetSorted);
    DECLARE_RPC_SERVICE_METHOD(NTableClient::NProto, Clear);

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableServer
} // namespace NYT

