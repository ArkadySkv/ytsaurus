#pragma once

#include "type_handler.h"
#include "object_detail.h"
#include "object_manager.h"

#include <ytlib/ytree/serialize.h>
#include <ytlib/meta_state/map.h>
#include <ytlib/cell_master/public.h>

namespace NYT {
namespace NObjectServer {

////////////////////////////////////////////////////////////////////////////////

template <class TObject>
class TObjectTypeHandlerBase
    : public IObjectTypeHandler
{
public:
    typedef typename NMetaState::TMetaStateMap<TObjectId, TObject> TMap;

    TObjectTypeHandlerBase(NCellMaster::TBootstrap* bootstrap, TMap* map)
        : Bootstrap(bootstrap)
        , Map(map)
    {
        YASSERT(map);
    }

    virtual bool Exists(const TObjectId& id)
    {
        return Map->Contains(id);
    }

    virtual i32 RefObject(const TObjectId& id)
    {
        auto& obj = Map->Get(id);
        return obj.RefObject();
    }

    virtual i32 UnrefObject(const TObjectId& id)
    {
        auto& obj = Map->Get(id);
        i32 result = obj.UnrefObject();
        if (result == 0) {
			// Remove the object from the map but keep it alive for a while.
			TAutoPtr<TObject> objHolder(Map->Release(id));
            OnObjectDestroyed(obj);
        }
        return result;
    }

    virtual i32 GetObjectRefCounter(const TObjectId& id)
    {
        auto& obj = Map->Get(id);
        return obj.GetObjectRefCounter();
    }

    virtual IObjectProxy::TPtr GetProxy(const TVersionedObjectId& id)
    {
        return New< TUnversionedObjectProxyBase<TObject> >(Bootstrap, id.ObjectId, Map);
    }

    virtual TObjectId CreateFromManifest(
        const NObjectServer::TTransactionId& transactionId,
        NYTree::IMapNode* manifest)
    {
        UNUSED(transactionId);
        UNUSED(manifest);
        ythrow yexception() << Sprintf("Object cannot be created from a manifest (Type: %s)",
            ~GetType().ToString());
    }

    virtual bool IsTransactionRequired() const
    {
        return true;
    }

protected:
    NCellMaster::TBootstrap* Bootstrap;
    // We store map by a raw pointer. In most cases this should be OK.
    TMap* Map;

    virtual void OnObjectDestroyed(TObject& obj)
    {
        UNUSED(obj);
    }
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NObjectServer
} // namespace NYT

