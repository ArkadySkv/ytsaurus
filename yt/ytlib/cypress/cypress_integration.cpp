#include "stdafx.h"
#include "cypress_integration.h"

#include <ytlib/cypress/virtual.h>
#include <ytlib/cypress/cypress_manager.h>
#include <ytlib/ytree/virtual.h>
#include <ytlib/ytree/fluent.h>
#include <ytlib/misc/string.h>
#include <ytlib/cell_master/bootstrap.h>

namespace NYT {
namespace NCypress {

using namespace NYTree;
using namespace NCellMaster;

////////////////////////////////////////////////////////////////////////////////

class TVirtualNodeMap
    : public TVirtualMapBase
{
public:
    TVirtualNodeMap(TBootstrap* bootstrap)
        : Bootstrap(bootstrap)
    { }

private:
    TBootstrap* Bootstrap;

    virtual yvector<Stroka> GetKeys(size_t sizeLimit) const
    {
        const auto& ids = Bootstrap->GetCypressManager()->GetNodeIds(sizeLimit);
        return ConvertToStrings(ids.begin(), ids.end(), sizeLimit);
    }

    virtual size_t GetSize() const
    {
        return Bootstrap->GetCypressManager()->GetNodeCount();
    }

    virtual IYPathServicePtr GetItemService(const Stroka& key) const
    {
        auto id = TVersionedNodeId::FromString(key);
        return Bootstrap->GetCypressManager()->FindVersionedNodeProxy(id.ObjectId, id.TransactionId);
    }
};

INodeTypeHandler::TPtr CreateNodeMapTypeHandler(TBootstrap* bootstrap)
{
    YASSERT(bootstrap);

    return CreateVirtualTypeHandler(
        bootstrap,
        EObjectType::NodeMap,
        ~New<TVirtualNodeMap>(bootstrap));
}

////////////////////////////////////////////////////////////////////////////////

class TVirtualLockMap
    : public TVirtualMapBase
{
public:
    TVirtualLockMap(TBootstrap* bootstrap)
        : Bootstrap(bootstrap)
    { }

private:
    TBootstrap* Bootstrap;

    virtual yvector<Stroka> GetKeys(size_t sizeLimit) const
    {
        const auto& ids = Bootstrap->GetCypressManager()->GetLockIds(sizeLimit);
        return ConvertToStrings(ids.begin(), ids.end(), sizeLimit);
    }

    virtual size_t GetSize() const
    {
        return Bootstrap->GetCypressManager()->GetLockCount();
    }

    virtual IYPathServicePtr GetItemService(const Stroka& key) const
    {
        auto id = TLockId::FromString(key);
        auto* lock = Bootstrap->GetCypressManager()->FindLock(id);
        if (!lock) {
            return NULL;
        }

        return IYPathService::FromProducer(FromFunctor([=] (IYsonConsumer* consumer)
            {
                BuildYsonFluently(consumer)
                    .BeginMap()
                        .Item("node_id").Scalar(lock->GetNodeId().ToString())
                        .Item("transaction_id").Scalar(lock->GetTransactionId().ToString())
                        .Item("mode").Scalar(lock->GetMode().ToString())
                    .EndMap();
            }));
    }
};

INodeTypeHandler::TPtr CreateLockMapTypeHandler(TBootstrap* bootstrap)
{
    YASSERT(bootstrap);

    return CreateVirtualTypeHandler(
        bootstrap,
        EObjectType::LockMap,
        ~New<TVirtualLockMap>(bootstrap));
}

////////////////////////////////////////////////////////////////////////////////
} // namespace NChunkServer
} // namespace NYT
