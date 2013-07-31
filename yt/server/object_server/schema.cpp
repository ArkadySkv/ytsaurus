#include "stdafx.h"
#include "schema.h"
#include "private.h"
#include "type_handler.h"

#include <server/cell_master/bootstrap.h>

#include <server/object_server/type_handler_detail.h>

namespace NYT {
namespace NObjectServer {

using namespace NTransactionServer;
using namespace NSecurityServer;
using namespace NObjectClient;
using namespace NYTree;
using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

TSchemaObject::TSchemaObject(const TObjectId& id)
    : TNonversionedObjectBase(id)
    , Acd_(this)
{ }

void TSchemaObject::Save(NCellMaster::TSaveContext& context) const
{
    TNonversionedObjectBase::Save(context);

    using NYT::Save;
    Save(context, Acd_);
}

void TSchemaObject::Load(NCellMaster::TLoadContext& context)
{
    TNonversionedObjectBase::Load(context);

    using NYT::Load;
    Load(context, Acd_);
}

////////////////////////////////////////////////////////////////////////////////

class TSchemaProxy
    : public TNonversionedObjectProxyBase<TSchemaObject>
{
public:
    TSchemaProxy(
        NCellMaster::TBootstrap* bootstrap,
        TSchemaObject* object)
        : TBase(bootstrap, object)
    {
        Logger = ObjectServerLogger;
    }

private:
    typedef TNonversionedObjectProxyBase<TSchemaObject> TBase;

    virtual bool GetSystemAttribute(const Stroka& key, NYson::IYsonConsumer* consumer) override
    {
        if (key == "type") {
            auto type = TypeFromSchemaType(TypeFromId(GetId()));
            BuildYsonFluently(consumer)
                .Value(Sprintf("schema:%s", ~CamelCaseToUnderscoreCase(type.ToString())));
            return true;
        }

        return TBase::GetSystemAttribute(key, consumer);
    }

};

IObjectProxyPtr CreateSchemaProxy(NCellMaster::TBootstrap* bootstrap, TSchemaObject* object)
{
    return New<TSchemaProxy>(bootstrap, object);
}

////////////////////////////////////////////////////////////////////////////////

class TSchemaTypeHandler
    : public TObjectTypeHandlerBase<TSchemaObject>
{
public:
    TSchemaTypeHandler(
        NCellMaster::TBootstrap* bootstrap,
        EObjectType type)
        : TBase(bootstrap)
        , Type(type)
    { }

    virtual EObjectType GetType() const override
    {
        return SchemaTypeFromType(Type);
    }

    virtual TObjectBase* FindObject(const TObjectId& id) override
    {
        auto objectManager = Bootstrap->GetObjectManager();
        auto* object = objectManager->GetSchema(Type);
        return id == object->GetId() ? object : nullptr;
    }

    virtual void Destroy(TObjectBase* object) override
    {
        UNUSED(object);
        YUNREACHABLE();
    }

    virtual TNullable<TTypeCreationOptions> GetCreationOptions() const override
    {
        return Null;
    }

    virtual EPermissionSet GetSupportedPermissions() const override
    {
        auto permissions = NonePermissions;

        auto objectManager = Bootstrap->GetObjectManager();
        auto handler = objectManager->GetHandler(Type);

        if (!IsVersionedType(Type)) {
            permissions |= handler->GetSupportedPermissions();
        }

        auto options = handler->GetCreationOptions();
        if (options) {
            permissions |= EPermission::Create;
        }

        return permissions;
    }

private:
    typedef TObjectTypeHandlerBase<TSchemaObject> TBase;

    EObjectType Type;

    virtual Stroka DoGetName(TSchemaObject* object) override
    {
        UNUSED(object);
        return Sprintf("%s schema", ~FormatEnum(Type).Quote());
    }

    virtual IObjectProxyPtr DoGetProxy(
        TSchemaObject* object,
        NTransactionServer::TTransaction* transaction) override
    {
        UNUSED(transaction);
        UNUSED(object);
        auto objectManager = Bootstrap->GetObjectManager();
        return objectManager->GetSchemaProxy(Type);
    }

    virtual void DoUnstage(
        TSchemaObject* object,
        NTransactionServer::TTransaction* transaction,
        bool recursive) override
    {
        UNUSED(object);
        UNUSED(transaction);
        UNUSED(recursive);
        YUNREACHABLE();
    }

    virtual NSecurityServer::TAccessControlDescriptor* DoFindAcd(TSchemaObject* object) override
    {
        return &object->Acd();
    }

    virtual TObjectBase* DoGetParent(TSchemaObject* object) override
    {
        UNUSED(object);
        return nullptr;
    }

};

IObjectTypeHandlerPtr CreateSchemaTypeHandler(NCellMaster::TBootstrap* bootstrap, EObjectType type)
{
    return New<TSchemaTypeHandler>(bootstrap, type);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NObjectServer
} // namespace NYT
