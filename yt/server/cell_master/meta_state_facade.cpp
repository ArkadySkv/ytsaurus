#include "stdafx.h"
#include "meta_state_facade.h"
#include "config.h"

#include <ytlib/ytree/ypath_proxy.h>
#include <ytlib/ytree/ypath_client.h>

#include <ytlib/ypath/token.h>

#include <ytlib/rpc/bus_channel.h>

#include <ytlib/cypress_client/cypress_ypath_proxy.h>

#include <ytlib/object_client/object_service_proxy.h>

#include <ytlib/transaction_client/transaction_ypath_proxy.h>

#include <ytlib/meta_state/composite_meta_state.h>
#include <ytlib/meta_state/persistent_state_manager.h>

#include <ytlib/logging/log.h>

#include <server/cell_master/bootstrap.h>

#include <server/cypress_server/cypress_manager.h>
#include <server/cypress_server/node_detail.h>

namespace NYT {
namespace NCellMaster {

using namespace NRpc;
using namespace NMetaState;
using namespace NYTree;
using namespace NYPath;
using namespace NCypressServer;
using namespace NCypressClient;
using namespace NTransactionClient;
using namespace NObjectServer;

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger Logger("Bootstrap");

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
        , Root(NULL)
    {
        YCHECK(config);
        YCHECK(bootstrap);

        StateQueue = New<TFairShareActionQueue>(EStateThreadQueue::GetDomainNames(), "MetaState");

        MetaState = New<TCompositeMetaState>();

        MetaStateManager = CreatePersistentStateManager(
            Config->MetaState,
            Bootstrap->GetControlInvoker(),
            StateQueue->GetInvoker(EStateThreadQueue::Default),
            MetaState,
            Bootstrap->GetRpcServer());

        MetaStateManager->SubscribeStartLeading(BIND(&TImpl::OnStartEpoch, MakeWeak(this)));
        MetaStateManager->SubscribeStartFollowing(BIND(&TImpl::OnStartEpoch, MakeWeak(this)));

        MetaStateManager->SubscribeStopLeading(BIND(&TImpl::OnStopEpoch, MakeWeak(this)));
        MetaStateManager->SubscribeStopFollowing(BIND(&TImpl::OnStopEpoch, MakeWeak(this)));

        MetaStateManager->SubscribeActiveQuorumEstablished(BIND(&TImpl::OnActiveQuorumEstablished, MakeWeak(this)));

        for (int queueIndex = 0; queueIndex < EStateThreadQueue::GetDomainSize(); ++queueIndex) {
            GuardedInvokers.push_back(MetaStateManager->CreateGuardedStateInvoker(StateQueue->GetInvoker(queueIndex)));
        }
    }

    void Start()
    {
        MetaStateManager->Start();
    }

    TCompositeMetaStatePtr GetState() const
    {
        return MetaState;
    }

    IMetaStateManagerPtr GetManager() const
    {
        return MetaStateManager;
    }

    IInvokerPtr GetInvoker(EStateThreadQueue queue = EStateThreadQueue::Default) const
    {
        return StateQueue->GetInvoker(queue);
    }

    IInvokerPtr GetEpochInvoker(EStateThreadQueue queue = EStateThreadQueue::Default) const
    {
        return EpochInvokers[queue];
    }

    IInvokerPtr GetGuardedInvoker(EStateThreadQueue queue = EStateThreadQueue::Default) const
    {
        return GuardedInvokers[queue];
    }

    bool IsActiveLeader()
    {
        return 
            MetaStateManager->GetStateStatus() == EPeerStatus::Leading &&
            MetaStateManager->HasActiveQuorum();
    }

    void ValidateActiveLeader()
    {
        if (MetaStateManager->GetStateStatus() != EPeerStatus::Leading) {
			throw TNotALeaderException()
                <<= ERROR_SOURCE_LOCATION()
                >>= TError(EErrorCode::Unavailable, "Not a leader");
		}
        if (!MetaStateManager->HasActiveQuorum()) {
            THROW_ERROR_EXCEPTION(EErrorCode::Unavailable, "No active quorum");
        }
    }

    bool IsInitialized() const
    {
        if (!Root) {
            auto cypressManager = Bootstrap->GetCypressManager();
            const auto& rootId = cypressManager->GetRootNodeId();
            Root = dynamic_cast<TMapNode*>(cypressManager->FindNode(rootId));
            LOG_FATAL_IF(!Root, "Missing Cypress root node %s; a possible reason could be that cell id has been changed",
                ~ToString(rootId));
        }
        return !Root->KeyToChild().empty();
    }

    void ValidateInitialized()
    {
        if (!IsInitialized()) {
            THROW_ERROR_EXCEPTION(EErrorCode::Unavailable, "Not initialized yet");
        }
    }

private:
    TCellMasterConfigPtr Config;
    TBootstrap* Bootstrap;

    TFairShareActionQueuePtr StateQueue;
    TCompositeMetaStatePtr MetaState;
    IMetaStateManagerPtr MetaStateManager;
    std::vector<IInvokerPtr> GuardedInvokers;
    std::vector<IInvokerPtr> EpochInvokers;

    mutable TMapNode* Root;


    void OnStartEpoch()
    {
        YCHECK(EpochInvokers.empty());

        auto cancelableContext = MetaStateManager->GetEpochContext()->CancelableContext;
        for (int queueIndex = 0; queueIndex < EStateThreadQueue::GetDomainSize(); ++queueIndex) {
            EpochInvokers.push_back(cancelableContext->CreateInvoker(StateQueue->GetInvoker(queueIndex)));
        }
    }

    void OnStopEpoch()
    {
        EpochInvokers.clear();
    }


    void OnActiveQuorumEstablished()
    {
        // NB: Initialization cannot be carried out here since not all subsystems
        // are fully initialized yet.
        // We'll post an initialization callback to the state invoker instead.
        GetEpochInvoker()->Invoke(BIND(&TImpl::InitializeIfNeeded, MakeStrong(this)));
    }

    void InitializeIfNeeded()
    {
        if (!IsInitialized()) {
            Initialize();
        }
    }

    // TODO(babenko): move initializer to a separate class
    void Initialize()
    {
        LOG_INFO("World initialization started");

        try {
            auto objectManager = Bootstrap->GetObjectManager();
            auto cypressManager = Bootstrap->GetCypressManager();
            auto rootService = objectManager->GetRootService();

            auto cellId = objectManager->GetCellId();
            auto cellGuid = TGuid::Create();

            // Abort all existing transactions to avoid collisions with previous (failed)
            // initialization attempts.
            AbortTransactions();

            // All initialization will be happening within this transaction.
            auto transactionId = StartTransaction();

            CreateNode(
                rootService,
                "//sys",
                transactionId,
                EObjectType::MapNode,
                BuildYsonStringFluently()
                    .BeginMap()
                        .Item("cell_id").Value(cellId)
                        .Item("cell_guid").Value(cellGuid)
                    .EndMap());

            CreateNode(
                rootService,
                "//sys/scheduler",
                transactionId,
                EObjectType::MapNode,
                BuildYsonStringFluently()
                    .BeginMap()
                        .Item("opaque").Value(true)
                    .EndMap());

            CreateNode(
                rootService,
                "//sys/scheduler/lock",
                transactionId,
                EObjectType::MapNode);

            CreateNode(
                rootService,
                "//sys/scheduler/pools",
                transactionId,
                EObjectType::MapNode,
                BuildYsonStringFluently()
                    .BeginMap()
                        .Item("opaque").Value(true)
                    .EndMap());

            CreateNode(
                rootService,
                "//sys/scheduler/orchid",
                transactionId,
                EObjectType::Orchid);

            CreateNode(
                rootService,
                "//sys/operations",
                transactionId,
                EObjectType::MapNode,
                BuildYsonStringFluently()
                    .BeginMap()
                        .Item("opaque").Value(true)
                    .EndMap());

            CreateNode(
                rootService,
                "//sys/nodes",
                transactionId,
                EObjectType::NodeMap,
                BuildYsonStringFluently()
                    .BeginMap()
                        .Item("opaque").Value(true)
                    .EndMap());

            CreateNode(
                rootService,
                "//sys/masters",
                transactionId,
                EObjectType::MapNode,
                BuildYsonStringFluently()
                    .BeginMap()
                        .Item("opaque").Value(true)
                    .EndMap());

            FOREACH (const auto& address, Bootstrap->GetConfig()->MetaState->Cell->Addresses) {
                auto addressPath = "/" + ToYPathLiteral(address);

                CreateNode(
                    rootService,
                    "//sys/masters" + addressPath,
                    transactionId,
                    EObjectType::MapNode);

                CreateNode(
                    rootService,
                    "//sys/masters" + addressPath + "/orchid",
                    transactionId,
                    EObjectType::Orchid,
                    BuildYsonStringFluently()
                        .BeginMap()
                            .Item("remote_address").Value(address)
                        .EndMap());
            }

            CreateNode(
                rootService,
                "//sys/chunks",
                transactionId,
                EObjectType::ChunkMap);

            CreateNode(
                rootService,
                "//sys/lost_chunks",
                transactionId,
                EObjectType::LostChunkMap);

            CreateNode(
                rootService,
                "//sys/vital_lost_chunks",
                transactionId,
                EObjectType::LostVitalChunkMap);

            CreateNode(
                rootService,
                "//sys/overreplicated_chunks",
                transactionId,
                EObjectType::OverreplicatedChunkMap);

            CreateNode(
                rootService,
                "//sys/underreplicated_chunks",
                transactionId,
                EObjectType::UnderreplicatedChunkMap);

            CreateNode(
                rootService,
                "//sys/chunk_lists",
                transactionId,
                EObjectType::ChunkListMap);

            CreateNode(
                rootService,
                "//sys/transactions",
                transactionId,
                EObjectType::TransactionMap);

            CreateNode(
                rootService,
                "//sys/accounts",
                transactionId,
                EObjectType::AccountMap);

            CreateNode(
                rootService,
                "//tmp",
                transactionId,
                EObjectType::MapNode,
                BuildYsonStringFluently()
                    .BeginMap()
                        .Item("opaque").Value(true)
                        .Item("account").Value("tmp")
                    .EndMap());

            CreateNode(
                rootService,
                "//home",
                transactionId,
                EObjectType::MapNode,
                BuildYsonStringFluently()
                    .BeginMap()
                        .Item("opaque").Value(true)
                    .EndMap());

            CommitTransaction(transactionId);
        } catch (const std::exception& ex) {
            LOG_ERROR(ex, "World initialization failed");
        }

        LOG_INFO("World initialization completed");
    }

    void AbortTransactions()
    {
        auto transactionIds = Bootstrap->GetTransactionManager()->GetTransactionIds();
        auto service = Bootstrap->GetObjectManager()->GetRootService();
        FOREACH (const auto& transactionId, transactionIds) {
            auto req = TTransactionYPathProxy::Abort(FromObjectId(transactionId));
            SyncExecuteVerb(service, req);
        }
    }

    TTransactionId StartTransaction()
    {
        auto service = Bootstrap->GetObjectManager()->GetRootService();
        auto req = TTransactionYPathProxy::CreateObject(RootTransactionPath);
        req->set_type(EObjectType::Transaction);
        auto rsp = SyncExecuteVerb(service, req);
        return TTransactionId::FromProto(rsp->object_id());
    }

    void CommitTransaction(const TTransactionId& transactionId)
    {
        auto service = Bootstrap->GetObjectManager()->GetRootService();
        auto req = TTransactionYPathProxy::Commit(FromObjectId(transactionId));
        SyncExecuteVerb(service, req);
    }

    static void CreateNode(
        IYPathServicePtr service,
        const TYPath& path,
        const TTransactionId& transactionId,
        EObjectType type,
        const TYsonString& attributes = TYsonString("{}"))
    {
        auto req = TCypressYPathProxy::Create(path);
        SetTransactionId(req, transactionId);
        req->set_type(type);
        ToProto(req->mutable_node_attributes(), *ConvertToAttributes(attributes));
        SyncExecuteVerb(service, req);
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

TCompositeMetaStatePtr TMetaStateFacade::GetState() const
{
    return Impl->GetState();
}

IMetaStateManagerPtr TMetaStateFacade::GetManager() const
{
    return Impl->GetManager();
}

bool TMetaStateFacade::IsInitialized() const
{
    return Impl->IsInitialized();
}

IInvokerPtr TMetaStateFacade::GetInvoker(EStateThreadQueue queue) const
{
    return Impl->GetInvoker(queue);
}

IInvokerPtr TMetaStateFacade::GetEpochInvoker(EStateThreadQueue queue) const
{
    return Impl->GetEpochInvoker(queue);
}

IInvokerPtr TMetaStateFacade::GetGuardedInvoker(EStateThreadQueue queue) const
{
    return Impl->GetGuardedInvoker(queue);
}

void TMetaStateFacade::Start()
{
    Impl->Start();
}

TMutationPtr TMetaStateFacade::CreateMutation(EStateThreadQueue queue)
{
    return New<TMutation>(
        GetManager(),
        GetGuardedInvoker(queue));
}

bool TMetaStateFacade::IsActiveLeader()
{
    return Impl->IsActiveLeader();
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

