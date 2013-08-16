#include "stdafx.h"
#include "account_proxy.h"
#include "account.h"
#include "security_manager.h"

#include <ytlib/yson/consumer.h>
#include <ytlib/ytree/exception_helpers.h>

#include <ytlib/security_client/account_ypath.pb.h>

#include <server/object_server/object_detail.h>

#include <server/cell_master/bootstrap.h>

namespace NYT {
namespace NSecurityServer {

using namespace NYTree;
using namespace NRpc;
using namespace NObjectServer;

////////////////////////////////////////////////////////////////////////////////

class TAccountProxy
    : public NObjectServer::TNonversionedObjectProxyBase<TAccount>
{
public:
    TAccountProxy(NCellMaster::TBootstrap* bootstrap, TAccount* account)
        : TBase(bootstrap, account)
    { }

private:
    typedef NObjectServer::TNonversionedObjectProxyBase<TAccount> TBase;

    virtual void ValidateRemoval() override
    {
        auto securityManager = Bootstrap->GetSecurityManager();
        if (GetThisTypedImpl() == securityManager->GetSysAccount() ||
            GetThisTypedImpl() == securityManager->GetTmpAccount())
        {
            THROW_ERROR_EXCEPTION("Cannot remove a built-in account");
        }
    }

    virtual void ListSystemAttributes(std::vector<TAttributeInfo>* attributes) override
    {
        attributes->push_back("name");
        attributes->push_back("resource_usage");
        attributes->push_back("committed_resource_usage");
        attributes->push_back("resource_limits");
        attributes->push_back("over_disk_space_limit");
        TBase::ListSystemAttributes(attributes);
    }

    virtual bool GetSystemAttribute(const Stroka& key, NYson::IYsonConsumer* consumer) override
    {
        const auto* account = GetThisTypedImpl();

        if (key == "name") {
            BuildYsonFluently(consumer)
                .Value(account->GetName());
            return true;
        }

        if (key == "resource_usage") {
            BuildYsonFluently(consumer)
                .Value(account->ResourceUsage());
            return true;
        }

        if (key == "committed_resource_usage") {
            BuildYsonFluently(consumer)
                .Value(account->CommittedResourceUsage());
            return true;
        }

        if (key == "resource_limits") {
            BuildYsonFluently(consumer)
                .Value(account->ResourceLimits());
            return true;
        }

        if (key == "over_disk_space_limit") {
            BuildYsonFluently(consumer)
                .Value(account->IsOverDiskSpaceLimit());
            return true;
        }

        return TBase::GetSystemAttribute(key, consumer);
    }

    virtual bool SetSystemAttribute(const Stroka& key, const NYTree::TYsonString& value) override
    {
        auto* account = GetThisTypedImpl();
        auto securityManager = Bootstrap->GetSecurityManager();

        if (key == "resource_limits") {
            account->ResourceLimits() = ConvertTo<TClusterResources>(value);
            return true;
        }

        if (key == "name") {
            auto newName = ConvertTo<Stroka>(value);
            securityManager->RenameAccount(account, newName);
            return true;
        }
        
        if (key == "committed_resource_usage" ||
            key == "resource_usage" ||
            key == "over_disk_space_limit")
        {
            NYTree::ThrowCannotSetSystemAttribute(key);
        }

        return TBase::SetSystemAttribute(key, value);
    }

};

IObjectProxyPtr CreateAccountProxy(
    NCellMaster::TBootstrap* bootstrap,
    TAccount* account)
{
    return New<TAccountProxy>(bootstrap, account);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NSecurityServer
} // namespace NYT

