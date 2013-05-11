#pragma once

#include "type_handler.h"

#include <server/object_server/public.h>
#include <server/cypress_server/public.h>
#include <server/cell_master/public.h>

namespace NYT {
namespace NCypressServer {

////////////////////////////////////////////////////////////////////////////////

DECLARE_FLAGGED_ENUM(EVirtualNodeOptions,
    ((None)            (0x0000))
    ((RequireLeader)   (0x0001))
    ((RedirectSelf)    (0x0002))
);

typedef
    TCallback< NYTree::IYPathServicePtr(TCypressNodeBase*, NTransactionServer::TTransaction*) >
    TYPathServiceProducer;

INodeTypeHandlerPtr CreateVirtualTypeHandler(
    NCellMaster::TBootstrap* bootstrap,
    NObjectClient::EObjectType objectType,
    TYPathServiceProducer producer,
    EVirtualNodeOptions options);

INodeTypeHandlerPtr CreateVirtualTypeHandler(
    NCellMaster::TBootstrap* bootstrap,
    NObjectClient::EObjectType objectType,
    NYTree::IYPathServicePtr service,
    EVirtualNodeOptions options);

////////////////////////////////////////////////////////////////////////////////

} // namespace NCypressServer
} // namespace NYT
