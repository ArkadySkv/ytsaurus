#include "stdafx.h"
#include "security_manager.h"
#include "private.h"
#include "account.h"
#include "account_proxy.h"
#include "user.h"
#include "user_proxy.h"
#include "group.h"
#include "group_proxy.h"
#include "acl.h"

#include <ytlib/meta_state/map.h>
#include <ytlib/meta_state/composite_meta_state.h>

#include <server/object_server/type_handler_detail.h>

#include <server/transaction_server/transaction.h>

#include <server/cell_master/bootstrap.h>
#include <server/cell_master/meta_state_facade.h>
#include <server/cell_master/serialization_context.h>

#include <server/transaction_server/transaction.h>

#include <server/cypress_server/node.h>

namespace NYT {
namespace NSecurityServer {

using namespace NMetaState;
using namespace NCellMaster;
using namespace NObjectClient;
using namespace NObjectServer;
using namespace NTransactionServer;
using namespace NYTree;
using namespace NCypressServer;
using namespace NSecurityClient;
using namespace NObjectServer;

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = SecurityServerLogger;

////////////////////////////////////////////////////////////////////////////////

TPermissionCheckResult::TPermissionCheckResult()
    : Action(ESecurityAction::Undefined)
    , Object(nullptr)
    , Subject(nullptr)
{ }

////////////////////////////////////////////////////////////////////////////////

TAuthenticatedUserGuard::TAuthenticatedUserGuard(TSecurityManagerPtr securityManager, TUser* user)
    : SecurityManager(securityManager)
    , IsNull(user == nullptr)
{
    if (user) {
        SecurityManager->PushAuthenticatedUser(user);
    }
}

TAuthenticatedUserGuard::~TAuthenticatedUserGuard()
{
    if (!IsNull) {
        SecurityManager->PopAuthenticatedUser();
    }
}

////////////////////////////////////////////////////////////////////////////////

class TSecurityManager::TAccountTypeHandler
    : public TObjectTypeHandlerBase<TAccount>
{
public:
    explicit TAccountTypeHandler(TImpl* owner);

    virtual EObjectType GetType() const override
    {
        return EObjectType::Account;
    }

    virtual TNullable<TTypeCreationOptions> GetCreationOptions() const override
    {
        return TTypeCreationOptions(
            EObjectTransactionMode::Forbidden,
            EObjectAccountMode::Forbidden);
    }

    virtual TObjectBase* Create(
        TTransaction* transaction,
        TAccount* account,
        IAttributeDictionary* attributes,
        TReqCreateObject* request,
        TRspCreateObject* response) override;

    virtual EPermissionSet GetSupportedPermissions() const override
    {
        return EPermissionSet(
            EPermissionSet::Read |
            EPermissionSet::Write |
            EPermission::Use);
    }

private:
    TImpl* Owner;

    virtual Stroka DoGetName(TAccount* object) override
    {
        return Sprintf("account %s", ~object->GetName().Quote());
    }

    virtual IObjectProxyPtr DoGetProxy(TAccount* account, TTransaction* transaction) override;
    
    virtual void DoDestroy(TAccount* account) override;

    virtual TAccessControlDescriptor* DoFindAcd(TAccount* account) override
    {
        return &account->Acd();
    }

};

////////////////////////////////////////////////////////////////////////////////

class TSecurityManager::TUserTypeHandler
    : public TObjectTypeHandlerBase<TUser>
{
public:
    explicit TUserTypeHandler(TImpl* owner);

    virtual EObjectType GetType() const override
    {
        return EObjectType::User;
    }

    virtual TNullable<TTypeCreationOptions> GetCreationOptions() const override
    {
        return TTypeCreationOptions(
            EObjectTransactionMode::Forbidden,
            EObjectAccountMode::Forbidden);
    }

    virtual TObjectBase* Create(
        TTransaction* transaction,
        TAccount* account,
        IAttributeDictionary* attributes,
        TReqCreateObject* request,
        TRspCreateObject* response) override;

private:
    TImpl* Owner;

    virtual Stroka DoGetName(TUser* user) override
    {
        return Sprintf("user %s", ~user->GetName().Quote());
    }

    virtual IObjectProxyPtr DoGetProxy(TUser* user, TTransaction* transaction) override;

    virtual void DoDestroy(TUser* user) override;

};

////////////////////////////////////////////////////////////////////////////////

class TSecurityManager::TGroupTypeHandler
    : public TObjectTypeHandlerBase<TGroup>
{
public:
    explicit TGroupTypeHandler(TImpl* owner);

    virtual EObjectType GetType() const override
    {
        return EObjectType::Group;
    }

    virtual TNullable<TTypeCreationOptions> GetCreationOptions() const override
    {
        return TTypeCreationOptions(
            EObjectTransactionMode::Forbidden,
            EObjectAccountMode::Forbidden);
    }

    virtual TObjectBase* Create(
        TTransaction* transaction,
        TAccount* account,
        IAttributeDictionary* attributes,
        TReqCreateObject* request,
        TRspCreateObject* response) override;

private:
    TImpl* Owner;

    virtual Stroka DoGetName(TGroup* group) override
    {
        return Sprintf("group %s", ~group->GetName().Quote());
    }

    virtual IObjectProxyPtr DoGetProxy(TGroup* group, TTransaction* transaction) override;

    virtual void DoDestroy(TGroup* group) override;

};

/////////////////////////////////////////////////////////////////////////// /////

class TSecurityManager::TImpl
    : public TMetaStatePart
{
public:
    explicit TImpl(NCellMaster::TBootstrap* bootstrap)
        : TMetaStatePart(
            bootstrap->GetMetaStateFacade()->GetManager(),
            bootstrap->GetMetaStateFacade()->GetState())
        , Bootstrap(bootstrap)
        , SysAccount(nullptr)
        , TmpAccount(nullptr)
        , RootUser(nullptr)
        , GuestUser(nullptr)
        , EveryoneGroup(nullptr)
        , UsersGroup(nullptr)
    {
        YCHECK(bootstrap);

        {
            NCellMaster::TLoadContext context;
            context.SetBootstrap(bootstrap);

            RegisterLoader(
                "SecurityManager.Keys",
                SnapshotVersionValidator(),
                BIND(&TImpl::LoadKeys, MakeStrong(this)),
                context);
            RegisterLoader(
                "SecurityManager.Values",
                SnapshotVersionValidator(),
                BIND(&TImpl::LoadValues, MakeStrong(this)),
                context);
        }

        {
            NCellMaster::TSaveContext context;

            RegisterSaver(
                ESerializationPriority::Keys,
                "SecurityManager.Keys",
                CurrentSnapshotVersion,
                BIND(&TImpl::SaveKeys, MakeStrong(this)),
                context);
            RegisterSaver(
                ESerializationPriority::Values,
                "SecurityManager.Values",
                CurrentSnapshotVersion,
                BIND(&TImpl::SaveValues, MakeStrong(this)),
                context);
        }

        {
            auto cellId = Bootstrap->GetObjectManager()->GetCellId();

            SysAccountId = MakeWellKnownId(EObjectType::Account, cellId, 0xffffffffffffffff);
            TmpAccountId = MakeWellKnownId(EObjectType::Account, cellId, 0xfffffffffffffffe);

            RootUserId = MakeWellKnownId(EObjectType::User, cellId, 0xffffffffffffffff);
            GuestUserId = MakeWellKnownId(EObjectType::User, cellId, 0xfffffffffffffffe);

            EveryoneGroupId = MakeWellKnownId(EObjectType::Group, cellId, 0xffffffffffffffff);
            UsersGroupId = MakeWellKnownId(EObjectType::Group, cellId, 0xfffffffffffffffe);
        }
    }

    void Initialize()
    {
        auto objectManager = Bootstrap->GetObjectManager();
        objectManager->RegisterHandler(New<TAccountTypeHandler>(this));
        objectManager->RegisterHandler(New<TUserTypeHandler>(this));
        objectManager->RegisterHandler(New<TGroupTypeHandler>(this));
    }


    DECLARE_METAMAP_ACCESSORS(Account, TAccount, TAccountId);
    DECLARE_METAMAP_ACCESSORS(User, TUser, TUserId);
    DECLARE_METAMAP_ACCESSORS(Group, TGroup, TGroupId);


    TAccount* CreateAccount(const Stroka& name)
    {
        if (FindAccountByName(name)) {
            THROW_ERROR_EXCEPTION("Account already exists: %s", ~name);
        }

        auto objectManager = Bootstrap->GetObjectManager();
        auto id = objectManager->GenerateId(EObjectType::Account);
        return DoCreateAccount(id, name);
    }

    void DestroyAccount(TAccount* account)
    {
        YCHECK(AccountNameMap.erase(account->GetName()) == 1);
    }

    TAccount* FindAccountByName(const Stroka& name)
    {
        auto it = AccountNameMap.find(name);
        return it == AccountNameMap.end() ? nullptr : it->second;
    }


    TAccount* GetSysAccount()
    {
        YCHECK(SysAccount);
        return SysAccount;
    }

    TAccount* GetTmpAccount()
    {
        YCHECK(TmpAccount);
        return TmpAccount;
    }


    void SetAccount(TCypressNodeBase* node, TAccount* account)
    {
        YCHECK(node);
        YCHECK(account);

        auto* oldAccount = node->GetAccount();
        if (oldAccount == account)
            return;

        auto objectManager = Bootstrap->GetObjectManager();

        bool isAccountingEnabled = IsUncommittedAccountingEnabled(node);

        if (oldAccount) {
            if (isAccountingEnabled) {
                UpdateResourceUsage(node, oldAccount, -1);
            }
            objectManager->UnrefObject(oldAccount);
        }

        node->SetAccount(account);
        node->CachedResourceUsage() = node->GetResourceUsage();

        if (isAccountingEnabled) {
            UpdateResourceUsage(node, account, +1);
        }
        objectManager->RefObject(account);
    }

    void ResetAccount(TCypressNodeBase* node)
    {
        auto* account = node->GetAccount();
        if (!account)
            return;

        auto objectManager = Bootstrap->GetObjectManager();

        bool isAccountingEnabled = IsUncommittedAccountingEnabled(node);

        if (isAccountingEnabled) {
            UpdateResourceUsage(node, account, -1);
        }

        node->CachedResourceUsage() = ZeroClusterResources();
        node->SetAccount(nullptr);

        objectManager->UnrefObject(account);
    }


    void UpdateAccountNodeUsage(TCypressNodeBase* node)
    {
        auto* account = node->GetAccount();
        if (!account)
            return;

        if (!IsUncommittedAccountingEnabled(node))
            return;

        UpdateResourceUsage(node, account, -1);

        node->CachedResourceUsage() = node->GetResourceUsage();

        UpdateResourceUsage(node, account, +1);
    }

    void UpdateAccountStagingUsage(
        TTransaction* transaction,
        TAccount* account,
        const TClusterResources& delta)
    {
        if (!IsStagedAccountingEnabled(transaction))
            return;

        account->ResourceUsage() += delta;

        auto* transactionUsage = GetTransactionAccountUsage(transaction, account);
        *transactionUsage += delta;
    }


    void DestroySubject(TSubject* subject)
    {
        FOREACH (auto* group , subject->MemberOf()) {
            YCHECK(group->Members().erase(subject) == 1);
        }

        FOREACH (const auto& pair, subject->LinkedObjects()) {
            auto* acd = GetAcd(pair.first);
            acd->OnSubjectDestroyed(subject, GuestUser);
        }
    }


    TUser* CreateUser(const Stroka& name)
    {
        if (FindUserByName(name)) {
            THROW_ERROR_EXCEPTION("User already exists: %s", ~name);
        }

        if (FindGroupByName(name)) {
            THROW_ERROR_EXCEPTION("Group with such name already exists: %s", ~name);
        }

        auto objectManager = Bootstrap->GetObjectManager();
        auto id = objectManager->GenerateId(EObjectType::User);
        return DoCreateUser(id, name);
    }

    void DestroyUser(TUser* user)
    {
        YCHECK(UserNameMap.erase(user->GetName()) == 1);
        DestroySubject(user);
    }

    TUser* FindUserByName(const Stroka& name)
    {
        auto it = UserNameMap.find(name);
        return it == UserNameMap.end() ? nullptr : it->second;
    }


    TUser* GetRootUser()
    {
        YCHECK(RootUser);
        return RootUser;
    }

    TUser* GetGuestUser()
    {
        YCHECK(GuestUser);
        return GuestUser;
    }


    TGroup* CreateGroup(const Stroka& name)
    {
        if (FindGroupByName(name)) {
            THROW_ERROR_EXCEPTION("Group already exists: %s", ~name);
        }

        if (FindUserByName(name)) {
            THROW_ERROR_EXCEPTION("User with such name already exists: %s", ~name);
        }

        auto objectManager = Bootstrap->GetObjectManager();
        auto id = objectManager->GenerateId(EObjectType::Group);
        return DoCreateGroup(id, name);
    }

    void DestroyGroup(TGroup* group)
    {
        YCHECK(GroupNameMap.erase(group->GetName()) == 1);

        FOREACH (auto* subject, group->Members()) {
            YCHECK(subject->MemberOf().erase(group) == 1);
        }

        DestroySubject(group);

        RecomputeMembershipClosure();
    }

    TGroup* FindGroupByName(const Stroka& name)
    {
        auto it = GroupNameMap.find(name);
        return it == GroupNameMap.end() ? nullptr : it->second;
    }


    TGroup* GetEveryoneGroup()
    {
        YCHECK(EveryoneGroup);
        return EveryoneGroup;
    }

    TGroup* GetUsersGroup()
    {
        YCHECK(UsersGroup);
        return UsersGroup;
    }


    TSubject* FindSubjectByName(const Stroka& name)
    {
        auto* user = FindUserByName(name);
        if (user) {
            return user;
        }

        auto* group = FindGroupByName(name);
        if (group) {
            return group;
        }

        return nullptr;
    }

    void AddMember(TGroup* group, TSubject* member)
    {
        ValidateMembershipUpdate(group, member);

        if (group->Members().find(member) != group->Members().end()) {
            THROW_ERROR_EXCEPTION("Member %s is already present in group %s",
                ~member->GetName().Quote(),
                ~group->GetName().Quote());
        }

        if (member->GetType() == EObjectType::Group) {
            auto* memberGroup = member->AsGroup();
            if (group->RecursiveMemberOf().find(memberGroup) != group->RecursiveMemberOf().end()) {
                THROW_ERROR_EXCEPTION("Adding group %s to group %s would produce a cycle",
                    ~memberGroup->GetName().Quote(),
                    ~group->GetName().Quote());
            }
        }

        DoAddMember(group, member);
    }

    void RemoveMember(TGroup* group, TSubject* member)
    {
        ValidateMembershipUpdate(group, member);

        if (group->Members().find(member) == group->Members().end()) {
            THROW_ERROR_EXCEPTION("Member %s is not present in group %s",
                ~member->GetName().Quote(),
                ~group->GetName().Quote());
        }

        DoRemoveMember(group, member);
    }


    EPermissionSet GetSupportedPermissions(TObjectBase* object)
    {
        auto objectManager = Bootstrap->GetObjectManager();
        auto handler = objectManager->GetHandler(object);
        return handler->GetSupportedPermissions();
    }

    TAccessControlDescriptor* FindAcd(TObjectBase* object)
    {
        auto objectManager = Bootstrap->GetObjectManager();
        auto handler = objectManager->GetHandler(object);
        return handler->FindAcd(object);
    }

    TAccessControlDescriptor* GetAcd(TObjectBase* object)
    {
        auto* acd = FindAcd(object);
        YCHECK(acd);
        return acd;
    }

    TAccessControlList GetEffectiveAcl(NObjectServer::TObjectBase* object)
    {
        TAccessControlList result;
        auto objectManager = Bootstrap->GetObjectManager();
        while (object) {
            auto handler = objectManager->GetHandler(object);
            auto* acd = handler->FindAcd(object);
            if (acd) {
                result.Entries.insert(result.Entries.end(), acd->Acl().Entries.begin(), acd->Acl().Entries.end());
                if (!acd->GetInherit()) {
                    break;
                }
            }

            object = handler->GetParent(object);
        }

        return result;
    }


    void PushAuthenticatedUser(TUser* user)
    {
        AuthenticatedUserStack.push_back(user);
    }

    void PopAuthenticatedUser()
    {
        AuthenticatedUserStack.pop_back();
    }

    TUser* GetAuthenticatedUser()
    {
        return AuthenticatedUserStack.back();
    }


    TPermissionCheckResult CheckPermission(
        TObjectBase* object,
        TUser* user,
        EPermission permission)
    {
        TPermissionCheckResult result;

        // Fast lane: "root" needs to authorization.
        if (user == RootUser) {
            result.Action = ESecurityAction::Allow;
            return result;
        }

        // Slow lane: check ACLs through the object hierarchy.
        auto objectManager = Bootstrap->GetObjectManager();
        auto* currentObject = object;
        while (currentObject) {
            auto handler = objectManager->GetHandler(currentObject);
            auto* acd = handler->FindAcd(currentObject);

            // Check the current ACL, if any.
            if (acd) {
                FOREACH (const auto& ace, acd->Acl().Entries) {
                    if (CheckPermissionMatch(ace.Permissions, permission)) {
                        FOREACH (auto* subject, ace.Subjects) {
                            if (CheckSubjectMatch(subject, user)) {
                                result.Action = ace.Action;
                                result.Object = currentObject;
                                result.Subject = subject;
                                // At least one denying ACE is found, deny the request.
                                if (result.Action == ESecurityAction::Deny) {
                                    LOG_WARNING_UNLESS(IsRecovery(), "Permission check failed: explicit denying ACE found (CheckObjectId: %s, Permission: %s, User: %s, AclObjectId: %s, AclSubject: %s)",
                                        ~ToString(object->GetId()),
                                        ~permission.ToString(),
                                        ~user->GetName(),
                                        ~ToString(result.Object->GetId()),
                                        ~result.Subject->GetName());
                                    return result;
                                }
                            }
                        }
                    }
                }

                // Proceed to the parent object unless the current ACL explicitly forbids inheritance.
                if (!acd->GetInherit()) {
                    break;
                }
            }

            currentObject = handler->GetParent(currentObject);
        }

        // No allowing ACE, deny the request.
        if (result.Action == ESecurityAction::Undefined) {
            LOG_WARNING_UNLESS(IsRecovery(), "Permission check failed: no matching ACE found (CheckObjectId: %s, Permission: %s, User: %s)",
                ~ToString(object->GetId()),
                ~permission.ToString(),
                ~user->GetName());
            result.Action = ESecurityAction::Deny;
            return result;
        } else {
            YASSERT(result.Action == ESecurityAction::Allow);
            LOG_DEBUG_UNLESS(IsRecovery(), "Permission check succeeded: explicit allowing ACE found (CheckObjectId: %s, Permission: %s, User: %s, AclObjectId: %s, AclSubject: %s)",
                ~ToString(object->GetId()),
                ~permission.ToString(),
                ~user->GetName(),
                ~ToString(result.Object->GetId()),
                ~result.Subject->GetName());
            return result;
        }
    }

    void ValidatePermission(
        TObjectBase* object,
        TUser* user,
        EPermission permission)
    {
        auto result = CheckPermission(object, user, permission);
        if (result.Action == ESecurityAction::Deny) {
            auto objectManager = Bootstrap->GetObjectManager();
            TError error;
            if (result.Object && result.Subject) {
                error = TError(
                    NSecurityClient::EErrorCode::AuthorizationError,
                    "Access denied: %s permission for %s is denied for %s by ACE at %s",
                    ~FormatEnum(permission).Quote(),
                    ~objectManager->GetHandler(object)->GetName(object),
                    ~result.Subject->GetName().Quote(),
                    ~objectManager->GetHandler(object)->GetName(result.Object));
            } else {
                error = TError(
                    NSecurityClient::EErrorCode::AuthorizationError,
                    "Access denied: %s permission for %s is not allowed by any matching ACE",
                    ~FormatEnum(permission).Quote(),
                    ~objectManager->GetHandler(object)->GetName(object));
            }
            error.Attributes().Set("permission", ~FormatEnum(permission));
            error.Attributes().Set("user", user->GetName());
            error.Attributes().Set("object", ~ToString(object->GetId()));
            if (result.Object) {
                error.Attributes().Set("denied_by", result.Object->GetId());
            }
            if (result.Subject) {
                error.Attributes().Set("denied_for", result.Subject->GetId());
            }
            THROW_ERROR(error);
        }
    }

    void ValidatePermission(
        TObjectBase* object,
        EPermission permission)
    {
        ValidatePermission(
            object,
            GetAuthenticatedUser(),
            permission);
    }


private:
    friend class TAccountTypeHandler;
    friend class TUserTypeHandler;
    friend class TGroupTypeHandler;


    NCellMaster::TBootstrap* Bootstrap;

    NMetaState::TMetaStateMap<TAccountId, TAccount> AccountMap;
    yhash_map<Stroka, TAccount*> AccountNameMap;

    TAccountId SysAccountId;
    TAccount* SysAccount;

    TAccountId TmpAccountId;
    TAccount* TmpAccount;

    NMetaState::TMetaStateMap<TUserId, TUser> UserMap;
    yhash_map<Stroka, TUser*> UserNameMap;

    TUserId RootUserId;
    TUser* RootUser;

    TUserId GuestUserId;
    TUser* GuestUser;

    NMetaState::TMetaStateMap<TGroupId, TGroup> GroupMap;
    yhash_map<Stroka, TGroup*> GroupNameMap;

    TGroupId EveryoneGroupId;
    TGroup* EveryoneGroup;

    TGroupId UsersGroupId;
    TGroup* UsersGroup;


    std::vector<TUser*> AuthenticatedUserStack;


    static bool IsUncommittedAccountingEnabled(TCypressNodeBase* node)
    {
        auto* transaction = node->GetTransaction();
        return !transaction || transaction->GetUncommittedAccountingEnabled();
    }

    static bool IsStagedAccountingEnabled(TTransaction* transaction)
    {
        return transaction->GetStagedAccountingEnabled();
    }

    static void UpdateResourceUsage(TCypressNodeBase* node, TAccount* account, int delta)
    {
        auto resourceUsage = node->CachedResourceUsage() * delta;

        account->ResourceUsage() += resourceUsage;
        if (node->IsTrunk()) {
            account->CommittedResourceUsage() += resourceUsage;
        }

        auto* transactionUsage = FindTransactionAccountUsage(node);
        if (transactionUsage) {
            *transactionUsage += resourceUsage;
        }
    }

    static TClusterResources* FindTransactionAccountUsage(TCypressNodeBase* node)
    {
        auto* account = node->GetAccount();
        auto* transaction = node->GetTransaction();
        if (!transaction) {
            return nullptr;
        }

        return GetTransactionAccountUsage(transaction, account);
    }

    static TClusterResources* GetTransactionAccountUsage(TTransaction* transaction, TAccount* account)
    {
        auto it = transaction->AccountResourceUsage().find(account);
        if (it == transaction->AccountResourceUsage().end()) {
            auto pair = transaction->AccountResourceUsage().insert(std::make_pair(account, ZeroClusterResources()));
            YCHECK(pair.second);
            return &pair.first->second;
        } else {
            return &it->second;
        }
    }


    TAccount* DoCreateAccount(const TAccountId& id, const Stroka& name)
    {
        auto* account = new TAccount(id);
        account->SetName(name);

        AccountMap.Insert(id, account);
        YCHECK(AccountNameMap.insert(std::make_pair(account->GetName(), account)).second);

        // Make the fake reference.
        YCHECK(account->RefObject() == 1);

        return account;
    }
    
    TUser* DoCreateUser(const TUserId& id, const Stroka& name)
    {
        auto* user = new TUser(id);
        user->SetName(name);

        UserMap.Insert(id, user);
        YCHECK(UserNameMap.insert(std::make_pair(user->GetName(), user)).second);

        // Make the fake reference.
        YCHECK(user->RefObject() == 1);

        // Every user except for "guest" is a member of "users" group.
        // "guest is a member of "everyone" group.
        if (id == GuestUserId) {
            DoAddMember(EveryoneGroup, user);
        } else {
            DoAddMember(UsersGroup, user);
        }

        return user;
    }

    TGroup* DoCreateGroup(const TGroupId& id, const Stroka& name)
    {
        auto* group = new TGroup(id);
        group->SetName(name);

        GroupMap.Insert(id, group);
        YCHECK(GroupNameMap.insert(std::make_pair(group->GetName(), group)).second);

        // Make the fake reference.
        YCHECK(group->RefObject() == 1);

        return group;
    }


    void PropagateRecursiveMemberOf(TSubject* subject, TGroup* ancestorGroup)
    {
        bool added = subject->RecursiveMemberOf().insert(ancestorGroup).second;
        if (added && subject->GetType() == EObjectType::Group) {
            auto* subjectGroup = subject->AsGroup();
            FOREACH (auto* member, subjectGroup->Members()) {
                PropagateRecursiveMemberOf(member, ancestorGroup);
            }
        }
    }

    void RecomputeMembershipClosure()
    {
        FOREACH (const auto& pair, UserMap) {
            pair.second->RecursiveMemberOf().clear();
        }

        FOREACH (const auto& pair, GroupMap) {
            pair.second->RecursiveMemberOf().clear();
        }

        FOREACH (const auto& pair, GroupMap) {
            auto* group = pair.second;
            FOREACH (auto* member, group->Members()) {
                PropagateRecursiveMemberOf(member, group);
            }
        }
    }


    void DoAddMember(TGroup* group, TSubject* member)
    {
        YCHECK(group->Members().insert(member).second);
        YCHECK(member->MemberOf().insert(group).second);

        RecomputeMembershipClosure();
    }

    void DoRemoveMember(TGroup* group, TSubject* member)
    {
        YCHECK(group->Members().erase(member) == 1);
        YCHECK(member->MemberOf().erase(group) == 1);

        RecomputeMembershipClosure();
    }


    void ValidateMembershipUpdate(TGroup* group, TSubject* member)
    {
        if (group == EveryoneGroup || group == UsersGroup) {
            THROW_ERROR_EXCEPTION("Cannot modify a built-in group");
        }

        ValidatePermission(group, EPermission::Write);
        ValidatePermission(member, EPermission::Write);
    }


    static bool CheckSubjectMatch(TSubject* subject, TUser* user)
    {
        switch (subject->GetType()) {
            case EObjectType::User:
                return subject == user;

            case EObjectType::Group: {
                auto* subjectGroup = subject->AsGroup();
                return user->RecursiveMemberOf().find(subjectGroup) != user->RecursiveMemberOf().end();
                                     }

            default:
                YUNREACHABLE();
        }
    }

    static bool CheckPermissionMatch(EPermissionSet permissions, EPermission requestedPermission)
    {
        return permissions & requestedPermission;
    }


    void SaveKeys(const NCellMaster::TSaveContext& context) const
    {
        AccountMap.SaveKeys(context);
        UserMap.SaveKeys(context);
        GroupMap.SaveKeys(context);
    }

    void SaveValues(const NCellMaster::TSaveContext& context) const
    {
        AccountMap.SaveValues(context);
        UserMap.SaveValues(context);
        GroupMap.SaveValues(context);
    }

    void LoadKeys(const NCellMaster::TLoadContext& context)
    {
        AccountMap.LoadKeys(context);
        // COMPAT(babenko)
        if (context.GetVersion() >= 8) {
            UserMap.LoadKeys(context);
            GroupMap.LoadKeys(context);
        }

        SysAccount = GetAccount(SysAccountId);
        TmpAccount = GetAccount(TmpAccountId);

        RootUser = GetUser(RootUserId);
        GuestUser = GetUser(GuestUserId);

        EveryoneGroup = GetGroup(EveryoneGroupId);
        UsersGroup = GetGroup(UsersGroupId);

        InitAuthenticatedUser();
    }

    void LoadValues(const NCellMaster::TLoadContext& context)
    {
        AccountMap.LoadValues(context);
        // COMPAT(babenko)
        if (context.GetVersion() >= 8) {
            UserMap.LoadValues(context);
            GroupMap.LoadValues(context);
        }

        // Reconstruct account name map.
        AccountNameMap.clear();
        FOREACH (const auto& pair, AccountMap) {
            auto* account = pair.second;
            YCHECK(AccountNameMap.insert(std::make_pair(account->GetName(), account)).second);
        }

        // Reconstruct user name map.
        UserNameMap.clear();
        FOREACH (const auto& pair, UserMap) {
            auto* user = pair.second;
            YCHECK(UserNameMap.insert(std::make_pair(user->GetName(), user)).second);
        }

        // Reconstruct group name map.
        GroupNameMap.clear();
        FOREACH (const auto& pair, GroupNameMap) {
            auto* group = pair.second;
            YCHECK(GroupNameMap.insert(std::make_pair(group->GetName(), group)).second);
        }
    }


    virtual void Clear() override
    {
        AccountMap.Clear();
        AccountNameMap.clear();
        
        UserMap.Clear();
        UserNameMap.clear();

        GroupMap.Clear();
        GroupNameMap.clear();


        // Initialize built-in groups.
        // users
        UsersGroup = DoCreateGroup(UsersGroupId, UsersGroupName);

        // everyone
        EveryoneGroup = DoCreateGroup(EveryoneGroupId, EveryoneGroupName);
        DoAddMember(EveryoneGroup, UsersGroup);


        // Initialize built-in users.
        // root
        RootUser = DoCreateUser(RootUserId, RootUserName);

        // guest
        GuestUser = DoCreateUser(GuestUserId, GuestUserName);


        // Initialize built-in accounts.
        // sys, 1 TB disk space, 100000 nodes, usage allowed for: root
        SysAccount = DoCreateAccount(SysAccountId, SysAccountName);
        SysAccount->ResourceLimits() = TClusterResources((i64) 1024 * 1024 * 1024 * 1024, 100000);
        SysAccount->Acd().AddEntry(TAccessControlEntry(
            ESecurityAction::Allow,
            RootUser,
            EPermission::Use));

        // tmp, 1 TB disk space, 100000 nodes, usage allowed for: users
        TmpAccount = DoCreateAccount(TmpAccountId, TmpAccountName);
        TmpAccount->ResourceLimits() = TClusterResources((i64) 1024 * 1024 * 1024 * 1024, 100000);
        TmpAccount->Acd().AddEntry(TAccessControlEntry(
            ESecurityAction::Allow,
            UsersGroup,
            EPermission::Use));

        InitAuthenticatedUser();
        InitDefaultSchemaAcds();
    }

    void InitAuthenticatedUser()
    {
        AuthenticatedUserStack.clear();
        AuthenticatedUserStack.push_back(RootUser);
    }

    void InitDefaultSchemaAcds()
    {
        auto objectManager = Bootstrap->GetObjectManager();
        FOREACH (auto type, objectManager->GetRegisteredTypes()) {
            if (TypeHasSchema(type)) {
                auto* schema = objectManager->GetSchema(type);
                auto* acd = GetAcd(schema);
                if (!TypeIsVersioned(type)) {
                    acd->AddEntry(TAccessControlEntry(
                        ESecurityAction::Allow,
                        GetUsersGroup(),
                        EPermissionSet(EPermission::Write)));
                    acd->AddEntry(TAccessControlEntry(
                        ESecurityAction::Allow,
                        GetEveryoneGroup(),
                        EPermissionSet(EPermission::Read)));
                }
                acd->AddEntry(TAccessControlEntry(
                    ESecurityAction::Allow,
                    GetUsersGroup(),
                    EPermissionSet(EPermission::Create)));
            }
        }
    }


    bool IsRecovery() const
    {
        return Bootstrap->GetMetaStateFacade()->GetManager()->IsRecovery();
    }
};

DEFINE_METAMAP_ACCESSORS(TSecurityManager::TImpl, Account, TAccount, TAccountId, AccountMap)
DEFINE_METAMAP_ACCESSORS(TSecurityManager::TImpl, User, TUser, TUserId, UserMap)
DEFINE_METAMAP_ACCESSORS(TSecurityManager::TImpl, Group, TGroup, TGroupId, GroupMap)

///////////////////////////////////////////////////////////////////////////////

TSecurityManager::TAccountTypeHandler::TAccountTypeHandler(TImpl* owner)
    : TObjectTypeHandlerBase(owner->Bootstrap, &owner->AccountMap)
    , Owner(owner)
{ }

TObjectBase* TSecurityManager::TAccountTypeHandler::Create(
    TTransaction* transaction,
    TAccount* account,
    IAttributeDictionary* attributes,
    TReqCreateObject* request,
    TRspCreateObject* response)
{
    UNUSED(transaction);
    UNUSED(account);
    UNUSED(request);
    UNUSED(response);

    auto name = attributes->Get<Stroka>("name");
    auto* newAccount = Owner->CreateAccount(name);
    return newAccount;
}

IObjectProxyPtr TSecurityManager::TAccountTypeHandler::DoGetProxy(
    TAccount* account,
    TTransaction* transaction)
{
    UNUSED(transaction);
    return CreateAccountProxy(Owner->Bootstrap, account);
}

void TSecurityManager::TAccountTypeHandler::DoDestroy(TAccount* account)
{
    Owner->DestroyAccount(account);
}

///////////////////////////////////////////////////////////////////////////////

TSecurityManager::TUserTypeHandler::TUserTypeHandler(TImpl* owner)
    : TObjectTypeHandlerBase(owner->Bootstrap, &owner->UserMap)
    , Owner(owner)
{ }

TObjectBase* TSecurityManager::TUserTypeHandler::Create(
    TTransaction* transaction,
    TAccount* account,
    IAttributeDictionary* attributes,
    TReqCreateObject* request,
    TRspCreateObject* response)
{
    UNUSED(transaction);
    UNUSED(account);
    UNUSED(request);
    UNUSED(response);

    auto name = attributes->Get<Stroka>("name");
    auto* newUser = Owner->CreateUser(name);
    return newUser;
}

IObjectProxyPtr TSecurityManager::TUserTypeHandler::DoGetProxy(
    TUser* user,
    TTransaction* transaction)
{
    UNUSED(transaction);
    return CreateUserProxy(Owner->Bootstrap, user);
}

void TSecurityManager::TUserTypeHandler::DoDestroy(TUser* user)
{
    Owner->DestroyUser(user);
}

///////////////////////////////////////////////////////////////////////////////

TSecurityManager::TGroupTypeHandler::TGroupTypeHandler(TImpl* owner)
    : TObjectTypeHandlerBase(owner->Bootstrap, &owner->GroupMap)
    , Owner(owner)
{ }

TObjectBase* TSecurityManager::TGroupTypeHandler::Create(
    TTransaction* transaction,
    TAccount* account,
    IAttributeDictionary* attributes,
    TReqCreateObject* request,
    TRspCreateObject* response)
{
    UNUSED(transaction);
    UNUSED(account);
    UNUSED(request);
    UNUSED(response);

    auto name = attributes->Get<Stroka>("name");
    auto* newGroup = Owner->CreateGroup(name);
    return newGroup;
}

IObjectProxyPtr TSecurityManager::TGroupTypeHandler::DoGetProxy(
    TGroup* group,
    TTransaction* transaction)
{
    UNUSED(transaction);
    return CreateGroupProxy(Owner->Bootstrap, group);
}

void TSecurityManager::TGroupTypeHandler::DoDestroy(TGroup* group)
{
    Owner->DestroyGroup(group);
}

///////////////////////////////////////////////////////////////////////////////

TSecurityManager::TSecurityManager(NCellMaster::TBootstrap* bootstrap)
    : Impl(New<TImpl>(bootstrap))
{ }

TSecurityManager::~TSecurityManager()
{ }

void TSecurityManager::Initialize()
{
    return Impl->Initialize();
}

TAccount* TSecurityManager::FindAccountByName(const Stroka& name)
{
    return Impl->FindAccountByName(name);
}

TAccount* TSecurityManager::GetSysAccount()
{
    return Impl->GetSysAccount();
}

TAccount* TSecurityManager::GetTmpAccount()
{
    return Impl->GetTmpAccount();
}

void TSecurityManager::SetAccount(TCypressNodeBase* node, TAccount* account)
{
    Impl->SetAccount(node, account);
}

void TSecurityManager::ResetAccount(TCypressNodeBase* node)
{
    Impl->ResetAccount(node);
}

void TSecurityManager::UpdateAccountNodeUsage(TCypressNodeBase* node)
{
    Impl->UpdateAccountNodeUsage(node);
}

void TSecurityManager::UpdateAccountStagingUsage(
    TTransaction* transaction,
    TAccount* account,
    const TClusterResources& delta)
{
    Impl->UpdateAccountStagingUsage(transaction, account, delta);
}

TUser* TSecurityManager::FindUserByName(const Stroka& name)
{
    return Impl->FindUserByName(name);
}

TUser* TSecurityManager::GetRootUser()
{
    return Impl->GetRootUser();
}

TUser* TSecurityManager::GetGuestUser()
{
    return Impl->GetGuestUser();
}

TGroup* TSecurityManager::FindGroupByName(const Stroka& name)
{
    return Impl->FindGroupByName(name);
}

TGroup* TSecurityManager::GetEveryoneGroup()
{
    return Impl->GetEveryoneGroup();
}

TGroup* TSecurityManager::GetUsersGroup()
{
    return Impl->GetUsersGroup();
}

TSubject* TSecurityManager::FindSubjectByName(const Stroka& name)
{
    return Impl->FindSubjectByName(name);
}

void TSecurityManager::AddMember(TGroup* group, TSubject* member)
{
    Impl->AddMember(group, member);
}

void TSecurityManager::RemoveMember(TGroup* group, TSubject* member)
{
    Impl->RemoveMember(group, member);
}

EPermissionSet TSecurityManager::GetSupportedPermissions(TObjectBase* object)
{
    return Impl->GetSupportedPermissions(object);
}

TAccessControlDescriptor* TSecurityManager::FindAcd(TObjectBase* object)
{
    return Impl->FindAcd(object);
}

TAccessControlDescriptor* TSecurityManager::GetAcd(TObjectBase* object)
{
    return Impl->GetAcd(object);
}

TAccessControlList TSecurityManager::GetEffectiveAcl(TObjectBase* object)
{
    return Impl->GetEffectiveAcl(object);
}

void TSecurityManager::PushAuthenticatedUser(TUser* user)
{
    Impl->PushAuthenticatedUser(user);
}

void TSecurityManager::PopAuthenticatedUser()
{
    Impl->PopAuthenticatedUser();
}

TUser* TSecurityManager::GetAuthenticatedUser()
{
    return Impl->GetAuthenticatedUser();
}

TPermissionCheckResult TSecurityManager::CheckPermission(
    TObjectBase* object,
    TUser* user,
    EPermission permission)
{
    return Impl->CheckPermission(
        object,
        user,
        permission);
}

void TSecurityManager::ValidatePermission(
    TObjectBase* object,
    TUser* user,
    EPermission permission)
{
    Impl->ValidatePermission(
        object,
        user,
        permission);
}

void TSecurityManager::ValidatePermission(
    TObjectBase* object,
    EPermission permission)
{
    Impl->ValidatePermission(
        object,
        permission);
}

DELEGATE_METAMAP_ACCESSORS(TSecurityManager, Account, TAccount, TAccountId, *Impl)
DELEGATE_METAMAP_ACCESSORS(TSecurityManager, User, TUser, TUserId, *Impl)
DELEGATE_METAMAP_ACCESSORS(TSecurityManager, Group, TGroup, TGroupId, *Impl)

///////////////////////////////////////////////////////////////////////////////

} // namespace NSecurityServer
} // namespace NYT
