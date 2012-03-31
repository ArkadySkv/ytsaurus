#pragma once

#include "table_node.h"
#include <ytlib/table_server/table_ypath.pb.h>

#include <ytlib/ytree/ypath_service.h>
#include <ytlib/cypress/node_proxy_detail.h>
#include <ytlib/table_client/schema.h>
#include <ytlib/cell_master/public.h>

namespace NYT {
namespace NTableServer {

////////////////////////////////////////////////////////////////////////////////

class TTableNodeProxy
    : public NCypress::TCypressNodeProxyBase<NYTree::IEntityNode, TTableNode>
{
public:
    typedef TIntrusivePtr<TTableNodeProxy> TPtr;

    TTableNodeProxy(
        NCypress::INodeTypeHandler* typeHandler,
        NCellMaster::TBootstrap* bootstrap,
        const NObjectServer::TTransactionId& transactionId,
        const NCypress::TNodeId& nodeId);

    virtual bool IsWriteRequest(NRpc::IServiceContext* context) const;

private:
    typedef NCypress::TCypressNodeProxyBase<NYTree::IEntityNode, TTableNode> TBase;

    virtual void GetSystemAttributes(std::vector<TAttributeInfo>* attributes);
    virtual bool GetSystemAttribute(const Stroka& name, NYTree::IYsonConsumer* consumer);

    virtual void DoInvoke(NRpc::IServiceContext* context);

    TResolveResult ResolveRecursive(const NYTree::TYPath& path, const Stroka& verb);

    void TraverseChunkTree(
        yvector<NChunkServer::TChunkId>* chunkIds,
        const NChunkServer::TChunkList *chunkTreeRef);

    void ParseYPath(
        const NYTree::TYPath& path,
        NTableClient::TChannel* channel);

    DECLARE_RPC_SERVICE_METHOD(NProto, GetChunkListForUpdate);
    DECLARE_RPC_SERVICE_METHOD(NProto, Fetch);

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableServer
} // namespace NYT

