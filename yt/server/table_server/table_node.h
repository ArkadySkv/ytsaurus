#pragma once

#include <ytlib/misc/property.h>

#include <server/cypress_server/node_detail.h>

#include <server/cell_master/public.h>

#include <server/security_server/cluster_resources.h>

namespace NYT {
namespace NTableServer {

////////////////////////////////////////////////////////////////////////////////

DECLARE_ENUM(ETableUpdateMode,
    (None)
    (Append)
    (Overwrite)
);

class TTableNode
    : public NCypressServer::TCypressNodeBase
{
    DEFINE_BYVAL_RW_PROPERTY(NChunkServer::TChunkList*, ChunkList);
    DEFINE_BYVAL_RW_PROPERTY(ETableUpdateMode, UpdateMode);
    DEFINE_BYVAL_RW_PROPERTY(int, ReplicationFactor);

public:
    explicit TTableNode(const NCypressServer::TVersionedNodeId& id);

    virtual int GetOwningReplicationFactor() const override;

    virtual NObjectClient::EObjectType GetObjectType() const;

    virtual NSecurityServer::TClusterResources GetResourceUsage() const override;

    virtual void Save(const NCellMaster::TSaveContext& context) const;
    virtual void Load(const NCellMaster::TLoadContext& context);
};

////////////////////////////////////////////////////////////////////////////////

NCypressServer::INodeTypeHandlerPtr CreateTableTypeHandler(NCellMaster::TBootstrap* bootstrap);

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableServer
} // namespace NYT

