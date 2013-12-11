#include "stdafx.h"
#include "meta_state_facade.h"
#include "automaton.h"
#include "config.h"

#include <core/ytree/ypath_proxy.h>
#include <core/ytree/ypath_client.h>

#include <core/ypath/token.h>

#include <core/rpc/bus_channel.h>
#include <core/rpc/server.h>

#include <core/concurrency/fiber.h>

#include <core/logging/log.h>

#include <ytlib/cypress_client/cypress_ypath_proxy.h>
#include <ytlib/cypress_client/rpc_helpers.h>

#include <ytlib/object_client/object_service_proxy.h>
#include <ytlib/object_client/master_ypath_proxy.h>
#include <ytlib/object_client/helpers.h>

#include <ytlib/transaction_client/transaction_ypath_proxy.h>

#include <ytlib/election/cell_manager.h>

#include <server/election/election_manager.h>

#include <server/hydra/composite_automaton.h>
#include <server/hydra/changelog.h>
#include <server/hydra/file_changelog.h>
#include <server/hydra/snapshot.h>
#include <server/hydra/file_snapshot.h>
#include <server/hydra/distributed_hydra_manager.h>

#include <server/hive/transaction_supervisor.h>

#include <server/cell_master/bootstrap.h>

#include <server/cypress_server/cypress_manager.h>
#include <server/cypress_server/node_detail.h>

#include <server/security_server/security_manager.h>
#include <server/security_server/acl.h>
#include <server/security_server/group.h>

namespace NYT {
namespace NCellMaster {

using namespace NConcurrency;
using namespace NRpc;
using namespace NElection;
using namespace NHydra;
using namespace NYTree;
using namespace NYPath;
using namespace NCypressServer;
using namespace NCypressClient;
using namespace NTransactionClient;
using namespace NTransactionClient::NProto;
using namespace NHive;
using namespace NHive::NProto;
using namespace NObjectClient;
using namespace NObjectServer;
using namespace NSecurityServer;
using NHydra::EPeerState;

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger Logger("Bootstrap");
static const TDuration InitRetryPeriod = TDuration::Seconds(3);

////////////////////////////////////////////////////////////////////////////////

class TMetaStateFacade::TImpl
    : public TRefCounted
{
public:
    TImpl(
        TCellMasterConfigPtr config,
        TBootstrap* bootstrap)
        : Config(config)
        , Bootstrap(bootstrap)
    {
        YCHECK(Config);
        YCHECK(Bootstrap);

        AutomatonQueue = New<TFairShareActionQueue>("Automaton", EAutomatonThreadQueue::GetDomainNames());
        Automaton = New<TMasterAutomaton>(Bootstrap);

        HydraManager = CreateDistributedHydraManager(
            Config->HydraManager,
            Bootstrap->GetControlInvoker(),
            AutomatonQueue->GetInvoker(EAutomatonThreadQueue::Default),
            Automaton,
            Bootstrap->GetRpcServer(),
            Bootstrap->GetCellManager(),
            Bootstrap->GetChangelogStore(),
            Bootstrap->GetSnapshotStore());

        HydraManager->SubscribeStartLeading(BIND(&TImpl::OnStartEpoch, MakeWeak(this)));
        HydraManager->SubscribeStartFollowing(BIND(&TImpl::OnStartEpoch, MakeWeak(this)));

        HydraManager->SubscribeStopLeading(BIND(&TImpl::OnStopEpoch, MakeWeak(this)));
        HydraManager->SubscribeStopFollowing(BIND(&TImpl::OnStopEpoch, MakeWeak(this)));

        HydraManager->SubscribeLeaderActive(BIND(&TImpl::OnLeaderActive, MakeWeak(this)));

        for (int index = 0; index < EAutomatonThreadQueue::GetDomainSize(); ++index) {
            auto unguardedInvoker = AutomatonQueue->GetInvoker(index);
            GuardedInvokers.push_back(HydraManager->CreateGuardedAutomatonInvoker(unguardedInvoker));
        }

    }

    void Start()
    {
        HydraManager->Start();
    }

    TMasterAutomatonPtr GetAutomaton() const
    {
        return Automaton;
    }

    IHydraManagerPtr GetManager() const
    {
        return HydraManager;
    }

    IInvokerPtr GetInvoker(EAutomatonThreadQueue queue = EAutomatonThreadQueue::Default) const
    {
        return AutomatonQueue->GetInvoker(queue);
    }

    IInvokerPtr GetEpochInvoker(EAutomatonThreadQueue queue = EAutomatonThreadQueue::Default) const
    {
        return EpochInvokers[queue];
    }

    IInvokerPtr GetGuardedInvoker(EAutomatonThreadQueue queue = EAutomatonThreadQueue::Default) const
    {
        return GuardedInvokers[queue];
    }

    void ValidateActiveLeader()
    {
        if (!HydraManager->IsActiveLeader()) {
            throw TNotALeaderException()
                <<= ERROR_SOURCE_LOCATION()
                >>= TError(NRpc::EErrorCode::Unavailable, "Not an active leader");
        }
    }

    bool IsInitialized() const
    {
        auto cypressManager = Bootstrap->GetCypressManager();
        auto* root = dynamic_cast<TMapNode*>(cypressManager->GetRootNode());
        YCHECK(root);
        return !root->KeyToChild().empty();
    }

    void ValidateInitialized()
    {
        if (!IsInitialized()) {
            THROW_ERROR_EXCEPTION(NRpc::EErrorCode::Unavailable, "Not initialized");
        }
    }

private:
    TCellMasterConfigPtr Config;
    TBootstrap* Bootstrap;

    TFairShareActionQueuePtr AutomatonQueue;
    TMasterAutomatonPtr Automaton;
    IHydraManagerPtr HydraManager;
    std::vector<IInvokerPtr> GuardedInvokers;
    std::vector<IInvokerPtr> EpochInvokers;

    void OnStartEpoch()
    {
        YCHECK(EpochInvokers.empty());

        auto cancelableContext = HydraManager->GetEpochContext()->CancelableContext;
        for (int index = 0; index < EAutomatonThreadQueue::GetDomainSize(); ++index) {
            EpochInvokers.push_back(cancelableContext->CreateInvoker(AutomatonQueue->GetInvoker(index)));
        }
    }

    void OnStopEpoch()
    {
        EpochInvokers.clear();
    }


    void OnLeaderActive()
    {
        // NB: Initialization cannot be carried out here since not all subsystems
        // are fully initialized yet.
        // We'll post an initialization callback to the state invoker instead.
        ScheduleInitialize();
    }

    void InitializeIfNeeded()
    {
        if (!IsInitialized()) {
            Initialize();
        }
    }

    void ScheduleInitialize(TDuration delay = TDuration::Zero())
    {
        TDelayedExecutor::Submit(
            BIND(&TImpl::InitializeIfNeeded, MakeStrong(this))
                .Via(GetEpochInvoker()),
            delay);
    }

    // TODO(babenko): move initializer to a separate class
    void Initialize()
    {
        LOG_INFO("World initialization started");

        try {
            // Check for pre-existing transactions to avoid collisions with previous (failed)
            // initialization attempts.
            auto transactionManager = Bootstrap->GetTransactionManager();
            if (transactionManager->Transactions().GetSize() > 0) {
                LOG_INFO("World initialization aborted: transactions found");
                AbortTransactions();
                ScheduleInitialize(InitRetryPeriod);
                return;
            }

            auto objectManager = Bootstrap->GetObjectManager();
            auto cypressManager = Bootstrap->GetCypressManager();
            auto securityManager = Bootstrap->GetSecurityManager();

            // All initialization will be happening within this transaction.
            auto transactionId = StartTransaction();

            CreateNode(
                "//sys",
                transactionId,
                EObjectType::MapNode,
                BuildYsonStringFluently()
                    .BeginMap()
                        .Item("cell_id").Value(Bootstrap->GetCellId())
                        .Item("cell_guid").Value(Bootstrap->GetCellGuid())
                    .EndMap());

            CreateNode(
                "//sys/schemas",
                transactionId,
                EObjectType::MapNode,
                BuildYsonStringFluently()
                    .BeginMap()
                        .Item("opaque").Value(true)
                    .EndMap());

            for (auto type : objectManager->GetRegisteredTypes()) {
                if (HasSchema(type)) {
                    CreateNode(
                        "//sys/schemas/" + ToYPathLiteral(FormatEnum(type)),
                        transactionId,
                        EObjectType::Link,
                        BuildYsonStringFluently()
                            .BeginMap()
                                .Item("target_id").Value(objectManager->GetSchema(type)->GetId())
                            .EndMap());
                }
            }

            CreateNode(
                "//sys/scheduler",
                transactionId,
                EObjectType::MapNode,
                BuildYsonStringFluently()
                    .BeginMap()
                        .Item("opaque").Value(true)
                    .EndMap());

            CreateNode(
                "//sys/scheduler/lock",
                transactionId,
                EObjectType::MapNode);

            CreateNode(
                "//sys/pools",
                transactionId,
                EObjectType::MapNode,
                BuildYsonStringFluently()
                    .BeginMap()
                        .Item("opaque").Value(true)
                    .EndMap());

            CreateNode(
                "//sys/scheduler/instances",
                transactionId,
                EObjectType::MapNode,
                BuildYsonStringFluently()
                    .BeginMap()
                        .Item("opaque").Value(true)
                    .EndMap());

            CreateNode(
                "//sys/scheduler/orchid",
                transactionId,
                EObjectType::Orchid);

            CreateNode(
                "//sys/operations",
                transactionId,
                EObjectType::MapNode,
                BuildYsonStringFluently()
                    .BeginMap()
                        .Item("opaque").Value(true)
                    .EndMap());

            CreateNode(
                "//sys/proxies",
                transactionId,
                EObjectType::MapNode,
                BuildYsonStringFluently()
                    .BeginMap()
                        .Item("opaque").Value(true)
                    .EndMap());

            CreateNode(
                "//sys/nodes",
                transactionId,
                EObjectType::CellNodeMap,
                BuildYsonStringFluently()
                    .BeginMap()
                        .Item("opaque").Value(true)
                    .EndMap());

            CreateNode(
                "//sys/masters",
                transactionId,
                EObjectType::MapNode,
                BuildYsonStringFluently()
                    .BeginMap()
                        .Item("opaque").Value(true)
                    .EndMap());

            for (const auto& address : Config->Masters->Addresses) {
                auto addressPath = "/" + ToYPathLiteral(address);

                CreateNode(
                    "//sys/masters" + addressPath,
                    transactionId,
                    EObjectType::MapNode);

                CreateNode(
                    "//sys/masters" + addressPath + "/orchid",
                    transactionId,
                    EObjectType::Orchid,
                    BuildYsonStringFluently()
                        .BeginMap()
                            .Item("remote_address").Value(address)
                        .EndMap());
            }

            CreateNode(
                "//sys/locks",
                transactionId,
                EObjectType::LockMap);

            CreateNode(
                "//sys/chunks",
                transactionId,
                EObjectType::ChunkMap);

            CreateNode(
                "//sys/lost_chunks",
                transactionId,
                EObjectType::LostChunkMap);

            CreateNode(
                "//sys/lost_vital_chunks",
                transactionId,
                EObjectType::LostVitalChunkMap);

            CreateNode(
                "//sys/overreplicated_chunks",
                transactionId,
                EObjectType::OverreplicatedChunkMap);

            CreateNode(
                "//sys/underreplicated_chunks",
                transactionId,
                EObjectType::UnderreplicatedChunkMap);

            CreateNode(
                "//sys/data_missing_chunks",
                transactionId,
                EObjectType::DataMissingChunkMap);

            CreateNode(
                "//sys/parity_missing_chunks",
                transactionId,
                EObjectType::ParityMissingChunkMap);

            CreateNode(
                "//sys/chunk_lists",
                transactionId,
                EObjectType::ChunkListMap);

            CreateNode(
                "//sys/transactions",
                transactionId,
                EObjectType::TransactionMap);

            CreateNode(
                "//sys/topmost_transactions",
                transactionId,
                EObjectType::TopmostTransactionMap);

            CreateNode(
                "//sys/accounts",
                transactionId,
                EObjectType::AccountMap);

            CreateNode(
                "//sys/users",
                transactionId,
                EObjectType::UserMap);

            CreateNode(
                "//sys/groups",
                transactionId,
                EObjectType::GroupMap);

            CreateNode(
                "//sys/tablet_cells",
                transactionId,
                EObjectType::TabletCellMap);

            CreateNode(
                "//sys/tablets",
                transactionId,
                EObjectType::TabletMap);

            CreateNode(
                "//tmp",
                transactionId,
                EObjectType::MapNode,
                BuildYsonStringFluently()
                    .BeginMap()
                        .Item("opaque").Value(true)
                        .Item("account").Value("tmp")
                        .Item("acl").BeginList()
                            .Item().Value(TAccessControlEntry(
                                ESecurityAction::Allow,
                                securityManager->GetUsersGroup(),
                                EPermissionSet(EPermission::Read | EPermission::Write)))
                        .EndList()
                    .EndMap());

            CreateNode(
                "//home",
                transactionId,
                EObjectType::MapNode,
                BuildYsonStringFluently()
                    .BeginMap()
                        .Item("opaque").Value(true)
                    .EndMap());

            CommitTransaction(transactionId);

            LOG_INFO("World initialization completed");
        } catch (const std::exception& ex) {
            LOG_ERROR(ex, "World initialization failed");
        }
    }

    void AbortTransactions()
    {
        auto transactionManager = Bootstrap->GetTransactionManager();
        auto transactionIds = ToObjectIds(transactionManager->Transactions().GetValues());

        auto transactionSupervisor = Bootstrap->GetTransactionSupervisor();

        for (const auto& transactionId : transactionIds) {
            TReqAbortTransaction req;
            ToProto(req.mutable_transaction_id(), transactionId);
            transactionSupervisor
                ->CreateAbortTransactionMutation(req)
                ->Commit();
        }
    }

    TTransactionId StartTransaction()
    {
        auto service = Bootstrap->GetObjectManager()->GetRootService();
        auto req = TMasterYPathProxy::CreateObjects();
        req->set_type(EObjectType::Transaction);

        auto* reqExt = req->MutableExtension(TReqStartTransactionExt::create_transaction_ext);

        auto attributes = CreateEphemeralAttributes();
        attributes->Set("title", "World initialization");
        ToProto(reqExt->mutable_attributes(), *attributes);

        auto rsp = WaitFor(ExecuteVerb(service, req));
        THROW_ERROR_EXCEPTION_IF_FAILED(*rsp);

        return FromProto<TTransactionId>(rsp->object_ids(0));
    }

    void CommitTransaction(const TTransactionId& transactionId)
    {
        auto service = Bootstrap->GetObjectManager()->GetRootService();
        auto req = TTransactionYPathProxy::Commit(FromObjectId(transactionId));
        auto rsp = WaitFor(ExecuteVerb(service, req));
        THROW_ERROR_EXCEPTION_IF_FAILED(*rsp);
    }

    void CreateNode(
        const TYPath& path,
        const TTransactionId& transactionId,
        EObjectType type,
        const TYsonString& attributes = TYsonString("{}"))
    {
        auto service = Bootstrap->GetObjectManager()->GetRootService();
        auto req = TCypressYPathProxy::Create(path);
        SetTransactionId(req, transactionId);
        req->set_type(type);
        ToProto(req->mutable_node_attributes(), *ConvertToAttributes(attributes));
        auto rsp = WaitFor(ExecuteVerb(service, req));
        THROW_ERROR_EXCEPTION_IF_FAILED(*rsp);
    }

};

////////////////////////////////////////////////////////////////////////////////

TMetaStateFacade::TMetaStateFacade(
    TCellMasterConfigPtr config,
    TBootstrap* bootstrap)
    : Impl(New<TImpl>(config, bootstrap))
{ }

TMetaStateFacade::~TMetaStateFacade()
{ }

TMasterAutomatonPtr TMetaStateFacade::GetAutomaton() const
{
    return Impl->GetAutomaton();
}

IHydraManagerPtr TMetaStateFacade::GetManager() const
{
    return Impl->GetManager();
}

IInvokerPtr TMetaStateFacade::GetInvoker(EAutomatonThreadQueue queue) const
{
    return Impl->GetInvoker(queue);
}

IInvokerPtr TMetaStateFacade::GetEpochInvoker(EAutomatonThreadQueue queue) const
{
    return Impl->GetEpochInvoker(queue);
}

IInvokerPtr TMetaStateFacade::GetGuardedInvoker(EAutomatonThreadQueue queue) const
{
    return Impl->GetGuardedInvoker(queue);
}

void TMetaStateFacade::Start()
{
    Impl->Start();
}

TMutationPtr TMetaStateFacade::CreateMutation(EAutomatonThreadQueue queue)
{
    return New<TMutation>(
        GetManager(),
        GetGuardedInvoker(queue));
}

void TMetaStateFacade::ValidateActiveLeader()
{
    return Impl->ValidateActiveLeader();
}

bool TMetaStateFacade::IsInitialized()
{
    return Impl->IsInitialized();
}

void TMetaStateFacade::ValidateInitialized()
{
    return Impl->ValidateInitialized();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NCellMaster
} // namespace NYT

