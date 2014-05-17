#include "stdafx.h"
#include "user_proxy.h"
#include "user.h"
#include "security_manager.h"
#include "subject_proxy_detail.h"

#include <ytlib/security_client/user_ypath.pb.h>

namespace NYT {
namespace NSecurityServer {

using namespace NYTree;
using namespace NObjectServer;

////////////////////////////////////////////////////////////////////////////////

class TUserProxy
    : public TSubjectProxy<TUser>
{
public:
    TUserProxy(NCellMaster::TBootstrap* bootstrap, TUser* user)
        : TBase(bootstrap, user)
    { }

private:
    typedef TSubjectProxy<TUser> TBase;

    virtual void ValidateRemoval() override
    {
        auto securityManager = Bootstrap->GetSecurityManager();
        if (GetThisTypedImpl() == securityManager->GetRootUser() ||
            GetThisTypedImpl() == securityManager->GetGuestUser())
        {
            THROW_ERROR_EXCEPTION("Cannot remove a built-in user");
        }
    }

    virtual void ListSystemAttributes(std::vector<NYTree::ISystemAttributeProvider::TAttributeInfo>* attributes) override
    {
        attributes->push_back("banned");
        attributes->push_back("request_rate_limit");
        attributes->push_back("access_time");
        attributes->push_back("request_counter");
        attributes->push_back("request_rate");
        TBase::ListSystemAttributes(attributes);
    }

    virtual bool GetBuiltinAttribute(const Stroka& key, NYson::IYsonConsumer* consumer) override
    {
        auto* user = this->GetThisTypedImpl();
        auto securityManager = Bootstrap->GetSecurityManager();

        if (key == "banned") {
            BuildYsonFluently(consumer)
                .Value(user->GetBanned());
            return true;
        }

        if (key == "request_rate_limit") {
            BuildYsonFluently(consumer)
                .Value(user->GetRequestRateLimit());
            return true;
        }

        if (key == "access_time") {
            BuildYsonFluently(consumer)
                .Value(user->GetAccessTime());
            return true;
        }

        if (key == "request_counter") {
            BuildYsonFluently(consumer)
                .Value(user->GetRequestCounter());
            return true;
        }

        if (key == "request_rate") {
            BuildYsonFluently(consumer)
                .Value(securityManager->GetRequestRate(user));
            return true;
        }

        return TBase::GetBuiltinAttribute(key, consumer);
    }

    virtual bool SetBuiltinAttribute(const Stroka& key, const NYTree::TYsonString& value) override
    {
        auto* user = this->GetThisTypedImpl();
        auto securityManager = this->Bootstrap->GetSecurityManager();

        if (key == "banned") {
            auto banned = ConvertTo<bool>(value);
            securityManager->SetUserBanned(user, banned);
            return true;
        }

        if (key == "request_rate_limit") {
            auto limit = ConvertTo<double>(value);
            if (limit < 0) {
                THROW_ERROR_EXCEPTION("\"request_rate_limit\" cannot be negative");
            }
            user->SetRequestRateLimit(limit);
            return true;
        }

        return TBase::SetBuiltinAttribute(key, value);
    }

};

IObjectProxyPtr CreateUserProxy(
    NCellMaster::TBootstrap* bootstrap,
    TUser* user)
{
    return New<TUserProxy>(bootstrap, user);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NSecurityServer
} // namespace NYT

