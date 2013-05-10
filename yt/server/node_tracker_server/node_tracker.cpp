#include "stdafx.h"
#include "node_tracker.h"
#include "config.h"
#include "node.h"
#include "private.h"

#include <ytlib/misc/id_generator.h>
#include <ytlib/misc/address.h>

#include <ytlib/ytree/convert.h>

#include <ytlib/ypath/token.h>

#include <ytlib/cypress_client/cypress_ypath_proxy.h>

#include <ytlib/object_client/public.h>

#include <server/chunk_server/job.h>

#include <server/cypress_server/cypress_manager.h>

#include <server/transaction_server/transaction_manager.h>
#include <server/transaction_server/transaction.h>

#include <server/object_server/object_manager.h>
#include <server/object_server/attribute_set.h>

#include <server/cell_master/bootstrap.h>
#include <server/cell_master/meta_state_facade.h>
#include <server/cell_master/serialization_context.h>

namespace NYT {
namespace NNodeTrackerServer {

using namespace NYTree;
using namespace NYPath;
using namespace NNodeTrackerClient;
using namespace NNodeTrackerClient::NProto;
using namespace NMetaState;
using namespace NCellMaster;
using namespace NObjectClient;
using namespace NCypressClient;
using namespace NNodeTrackerServer::NProto;
using namespace NTransactionServer;

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = NodeTrackerServerLogger;

////////////////////////////////////////////////////////////////////////////////

class TNodeTracker::TImpl
    : public TMetaStatePart
{
public:
    TImpl(
        TNodeTrackerConfigPtr config,
        TBootstrap* bootstrap)
        : TMetaStatePart(
            bootstrap->GetMetaStateFacade()->GetManager(),
            bootstrap->GetMetaStateFacade()->GetState())
        , Config(config)
        , Bootstrap(bootstrap)
        , OnlineNodeCount(0)
        , RegisteredNodeCount(0)
        , Profiler(NodeTrackerServerProfiler)
    {
        YCHECK(config);
        YCHECK(bootstrap);

        RegisterMethod(BIND(&TImpl::RegisterNode, Unretained(this)));
        RegisterMethod(BIND(&TImpl::UnregisterNode, Unretained(this)));
        RegisterMethod(BIND(&TImpl::FullHeartbeat, Unretained(this)));
        RegisterMethod(BIND(&TImpl::IncrementalHeartbeat, Unretained(this)));

        {
            NCellMaster::TLoadContext context;
            context.SetBootstrap(Bootstrap);

            RegisterLoader(
                "NodeTracker.Keys",
                SnapshotVersionValidator(),
                BIND(&TImpl::LoadKeys, MakeStrong(this)),
                context);
            RegisterLoader(
                "NodeTracker.Values",
                SnapshotVersionValidator(),
                BIND(&TImpl::LoadValues, MakeStrong(this)),
                context);
        }

        {
            NCellMaster::TSaveContext context;

            RegisterSaver(
                ESerializationPriority::Keys,
                "NodeTracker.Keys",
                CurrentSnapshotVersion,
                BIND(&TImpl::SaveKeys, MakeStrong(this)),
                context);
            RegisterSaver(
                ESerializationPriority::Values,
                "NodeTracker.Values",
                CurrentSnapshotVersion,
                BIND(&TImpl::SaveValues, MakeStrong(this)),
                context);
        }

        SubscribeNodeConfigUpdated(BIND(&TImpl::OnNodeConfigUpdated, Unretained(this)));
    }

    void Initialize()
    {
        auto transactionManager = Bootstrap->GetTransactionManager();
        transactionManager->SubscribeTransactionCommitted(BIND(&TImpl::OnTransactionFinished, MakeWeak(this)));
        transactionManager->SubscribeTransactionAborted(BIND(&TImpl::OnTransactionFinished, MakeWeak(this)));
    }


    TMutationPtr CreateRegisterNodeMutation(
        const TMetaReqRegisterNode& request)
    {
        return Bootstrap
            ->GetMetaStateFacade()
            ->CreateMutation(this, request, &TThis::RegisterNode);
    }

    TMutationPtr CreateUnregisterNodeMutation(
        const TMetaReqUnregisterNode& request)
    {
        return Bootstrap
            ->GetMetaStateFacade()
            ->CreateMutation(this, request, &TThis::UnregisterNode);
    }

    TMutationPtr CreateFullHeartbeatMutation(
        TCtxFullHeartbeatPtr context)
    {
        return Bootstrap
            ->GetMetaStateFacade()
            ->CreateMutation(EStateThreadQueue::Heartbeat)
            ->SetRequestData(context->GetRequestBody())
            ->SetType(context->Request().GetTypeName())
            ->SetAction(BIND(&TThis::FullHeartbeatWithContext, MakeStrong(this), context));
    }

    TMutationPtr CreateIncrementalHeartbeatMutation(
        const TMetaReqIncrementalHeartbeat& request)
    {
        return Bootstrap
            ->GetMetaStateFacade()
            ->CreateMutation(this, request, &TThis::IncrementalHeartbeat, EStateThreadQueue::Heartbeat);
    }


    void RefreshNodeConfig(TNode* node)
    {
        auto attributes = DoFindNodeConfig(node->GetAddress());
        if (!attributes)
            return;

        if (!ReconfigureYsonSerializable(node->GetConfig(), attributes))
            return;

        LOG_INFO_UNLESS(IsRecovery(), "Node configuration updated (Address: %s)", ~node->GetAddress());

        NodeConfigUpdated_.Fire(node);
    }


    DECLARE_METAMAP_ACCESSORS(Node, TNode, TNodeId);

    DEFINE_SIGNAL(void(TNode* node), NodeRegistered);
    DEFINE_SIGNAL(void(TNode* node), NodeUnregistered);
    DEFINE_SIGNAL(void(TNode* node), NodeConfigUpdated);
    DEFINE_SIGNAL(void(TNode* node, const TMetaReqFullHeartbeat& request), FullHeartbeat);
    DEFINE_SIGNAL(void(TNode* node, const TMetaReqIncrementalHeartbeat& request), IncrementalHeartbeat);


    TNode* FindNodeByAddress(const Stroka& address)
    {
        auto it = AddressToNodeMap.find(address);
        return it == AddressToNodeMap.end() ? nullptr : it->second;
    }

    TNode* GetNodeByAddress(const Stroka& address)
    {
        auto* node = FindNodeByAddress(address);
        YCHECK(node);
        return node;
    }

    TNode* FindNodeByHostName(const Stroka& hostName)
    {
        auto it = HostNameToNodeMap.find(hostName);
        return it == AddressToNodeMap.end() ? nullptr : it->second;
    }

    TNode* GetNodeOrThrow(TNodeId id)
    {
        auto* node = FindNode(id);
        if (!node) {
            THROW_ERROR_EXCEPTION(
                NNodeTrackerClient::EErrorCode::NoSuchNode,
                "Invalid or expired node id %d",
                id);
        }
        return node;
    }


    TNodeConfigPtr FindNodeConfigByAddress(const Stroka& address)
    {
        auto attributes = DoFindNodeConfig(address);
        if (!attributes) {
            return nullptr;
        }

        try {
            return ConvertTo<TNodeConfigPtr>(attributes);
        } catch (const std::exception& ex) {
            LOG_WARNING(ex, "Error parsing configuration of node %s, defaults will be used", ~address);
            return nullptr;
        }
    }

    TNodeConfigPtr GetNodeConfigByAddress(const Stroka& address)
    {
        auto config = FindNodeConfigByAddress(address);
        return config ? config : New<TNodeConfig>();
    }

    
    TTotalNodeStatistics GetTotalNodeStatistics()
    {
        TTotalNodeStatistics result;
        FOREACH (const auto& pair, NodeMap) {
            const auto* node = pair.second;
            const auto& statistics = node->Statistics();
            result.AvailbaleSpace += statistics.total_available_space();
            result.UsedSpace += statistics.total_used_space();
            result.ChunkCount += statistics.total_chunk_count();
            result.SessionCount += statistics.total_session_count();
            result.OnlineNodeCount++;
        }
        return result;
    }

    int GetRegisteredNodeCount()
    {
        return RegisteredNodeCount;
    }

    int GetOnlineNodeCount()
    {
        return OnlineNodeCount;
    }

private:
    typedef TImpl TThis;

    TNodeTrackerConfigPtr Config;
    TBootstrap* Bootstrap;

    int OnlineNodeCount;
    int RegisteredNodeCount;

    NProfiling::TProfiler& Profiler;

    TIdGenerator NodeIdGenerator;

    TMetaStateMap<TNodeId, TNode> NodeMap;
    yhash_map<Stroka, TNode*> AddressToNodeMap;
    yhash_multimap<Stroka, TNode*> HostNameToNodeMap;
    yhash_map<TTransaction*, TNode*> TransactionToNodeMap;


    TNodeId GenerateNodeId()
    {
        TNodeId id;
        while (true) {
            id = NodeIdGenerator.Next();
            // Beware of sentinels!
            if (id == InvalidNodeId) {
                // Just wait for the next attempt.
            } else if (id > MaxNodeId) {
                NodeIdGenerator.Reset();
            } else {
                break;
            }
        }
        return id;
    }

    IMapNodePtr DoFindNodeConfig(const Stroka& address)
    {
        auto cypressManager = Bootstrap->GetCypressManager();
        auto resolver = cypressManager->CreateResolver();

        auto nodesNode = resolver->ResolvePath("//sys/nodes");
        YCHECK(nodesNode);

        auto nodesMap = nodesNode->AsMap();
        auto nodeNode = nodesMap->FindChild(address);
        if (!nodeNode) {
            return nullptr;
        }

        return nodeNode->Attributes().ToMap();
    }


    TMetaRspRegisterNode RegisterNode(const TMetaReqRegisterNode& request)
    {
        auto descriptor = FromProto<NNodeTrackerClient::TNodeDescriptor>(request.node_descriptor());
        const auto& statistics = request.statistics();
        const auto& address = descriptor.Address;

        // Kick-out any previous incarnation.
        {
            auto* existingNode = FindNodeByAddress(descriptor.Address);
            if (existingNode) {
                LOG_INFO_UNLESS(IsRecovery(), "Node kicked out due to address conflict (Address: %s, ExistingId: %d)",
                    ~address,
                    existingNode->GetId());
                DoUnregisterNode(existingNode);
            }
        }

        auto* node = DoRegisterNode(descriptor, statistics);

        TMetaRspRegisterNode response;
        response.set_node_id(node->GetId());
        return response;
    }

    void UnregisterNode(const TMetaReqUnregisterNode& request)
    {
        auto nodeId = request.node_id();

        // Allow nodeId to be invalid, just ignore such obsolete requests.
        auto* node = FindNode(nodeId);
        if (!node)
            return;

        DoUnregisterNode(node);
    }


    void FullHeartbeatWithContext(TCtxFullHeartbeatPtr context)
    {
        return FullHeartbeat(context->Request());
    }

    void FullHeartbeat(const TMetaReqFullHeartbeat& request)
    {
        PROFILE_TIMING ("/full_heartbeat_time") {
            auto nodeId = request.node_id();
            const auto& statistics = request.statistics();

            auto* node = GetNode(nodeId);

            LOG_DEBUG_UNLESS(IsRecovery(), "Full heartbeat received (NodeId: %d, Address: %s, State: %s, %s)",
                nodeId,
                ~node->GetAddress(),
                ~node->GetState().ToString(),
                ~ToString(statistics));

            node->Statistics() = statistics;

            YCHECK(node->GetState() == ENodeState::Registered);
            UpdateNodeCounters(node, -1);
            node->SetState(ENodeState::Online);
            UpdateNodeCounters(node, +1);

            RenewNodeLease(node);

            LOG_INFO_UNLESS(IsRecovery(), "Node online (NodeId: %d, Address: %s)",
                nodeId,
                ~node->GetAddress());

            FullHeartbeat_.Fire(node, request);
        }
    }


    void IncrementalHeartbeat(const TMetaReqIncrementalHeartbeat& request)
    {
        PROFILE_TIMING ("/incremental_heartbeat_time") {
            auto nodeId = request.node_id();
            const auto& statistics = request.statistics();

            auto* node = GetNode(nodeId);

            LOG_DEBUG_UNLESS(IsRecovery(), "Incremental heartbeat received (NodeId: %d, Address: %s, State: %s, %s)",
                nodeId,
                ~node->GetAddress(),
                ~node->GetState().ToString(),
                ~ToString(statistics));

            YCHECK(node->GetState() == ENodeState::Online);
            node->Statistics() = statistics;

            RenewNodeLease(node);
            
            IncrementalHeartbeat_.Fire(node, request);
        }
    }


    void SaveKeys(const NCellMaster::TSaveContext& context) const
    {
        NodeMap.SaveKeys(context);
    }

    void SaveValues(const NCellMaster::TSaveContext& context) const
    {
        Save(context, NodeIdGenerator);
        NodeMap.SaveValues(context);
    }

    void LoadKeys(const NCellMaster::TLoadContext& context)
    {
        NodeMap.LoadKeys(context);
    }

    void LoadValues(const NCellMaster::TLoadContext& context)
    {
        Load(context, NodeIdGenerator);
        NodeMap.LoadValues(context);
    }

    virtual void Clear() override
    {
        NodeIdGenerator.Reset();
        NodeMap.Clear();
        AddressToNodeMap.clear();
        HostNameToNodeMap.clear();
        TransactionToNodeMap.clear();
        OnlineNodeCount = 0;
        RegisteredNodeCount = 0;
    }

    virtual void OnAfterLoaded() override
    {
        // Reconstruct address maps, recompute statistics.
        AddressToNodeMap.clear();
        HostNameToNodeMap.clear();

        OnlineNodeCount = 0;
        RegisteredNodeCount = 0;

        auto metaStateFacade = Bootstrap->GetMetaStateFacade();
        auto invoker = metaStateFacade->GetEpochInvoker();

        FOREACH (const auto& pair, NodeMap) {
            auto* node = pair.second;
            const auto& address = node->GetAddress();

            YCHECK(AddressToNodeMap.insert(std::make_pair(address, node)).second);
            HostNameToNodeMap.insert(std::make_pair(Stroka(GetServiceHostName(address)), node));
            YCHECK(TransactionToNodeMap.insert(std::make_pair(node->GetTransaction(), node)).second);

            UpdateNodeCounters(node, +1);
            RegisterLeaseTransaction(node);

            // Make this a postponed call since Cypress Manager might not be ready yet to handle
            // such requests.
        
            invoker->Invoke(BIND(&TImpl::RefreshNodeConfig, MakeStrong(this), node));
        }
    }


    virtual void OnRecoveryStarted() override
    {
        Profiler.SetEnabled(false);

        // Reset runtime info.
        FOREACH (const auto& pair, NodeMap) {
            auto* node = pair.second;

            node->SetHintedSessionCount(0);
            
            FOREACH (auto& queue, node->ChunkReplicationQueues()) {
                queue.clear();
            }

            node->ChunkRemovalQueue().clear();
        }
    }

    virtual void OnRecoveryComplete() override
    {
        Profiler.SetEnabled(true);
    }


    void UpdateNodeCounters(TNode* node, int delta)
    {
        switch (node->GetState()) {
            case ENodeState::Registered:
                RegisteredNodeCount += delta;
                break;
            case ENodeState::Online:
                OnlineNodeCount += delta;
                break;
            default:
                break;
        }
    }


    void RegisterLeaseTransaction(TNode* node)
    {
        auto* transaction = node->GetTransaction();
        YCHECK(transaction);
        YCHECK(TransactionToNodeMap.insert(std::make_pair(transaction, node)).second);
    }

    void UnregisterLeaseTransaction(TNode* node)
    {
        auto* transaction = node->GetTransaction();
        if (!transaction)
            return;

        YCHECK(TransactionToNodeMap.erase(transaction) == 1);
        node->SetTransaction(nullptr);
    }

    void RenewNodeLease(TNode* node)
    {
        auto* transaction = node->GetTransaction();
        if (!transaction)
            return;

        auto timeout = GetLeaseTimeout(node);
        transaction->SetTimeout(timeout);

        if (IsLeader()) {
            auto transactionManager = Bootstrap->GetTransactionManager();
            transactionManager->PingTransaction(transaction);
        }
    }

    TDuration GetLeaseTimeout(TNode* node)
    {
        switch (node->GetState()) {
            case ENodeState::Registered:
                return Config->RegisteredNodeTimeout;
            case ENodeState::Online:
                return Config->OnlineNodeTimeout;
            default:
                YUNREACHABLE();
        }
    }

    void OnTransactionFinished(TTransaction* transaction)
    {
        auto it = TransactionToNodeMap.find(transaction);
        if (it == TransactionToNodeMap.end())
            return;

        auto* node = it->second;
        LOG_INFO_UNLESS(IsRecovery(), "Node lease expired (NodeId: %d, Address: %s)",
            node->GetId(),
            ~node->GetAddress());

        UnregisterLeaseTransaction(node);

        if (IsLeader()) {
            PostUnregisterCommit(node);
        }
    }


    void RegisterNodeInCypress(TNode* node)
    {
        // We're already in the state thread but need to postpone the planned changes and enqueue a callback.
        // Doing otherwise will turn node registration and Cypress update into a single
        // logged change, which is undesirable.
        auto metaStateFacade = Bootstrap->GetMetaStateFacade();
        BIND(&TImpl::DoRegisterNodeInCypress, MakeStrong(this), node->GetId())
            .Via(metaStateFacade->GetEpochInvoker())
            .Run();
    }

    void DoRegisterNodeInCypress(TNodeId nodeId)
    {
        auto* node = FindNode(nodeId);
        if (!node)
            return;

        auto* transaction = node->GetTransaction();
        if (!transaction)
            return;

        const auto& address = node->GetAddress();
        auto addressToken = ToYPathLiteral(address);

        auto objectManager = Bootstrap->GetObjectManager();
        auto rootService = objectManager->GetRootService();

        {
            auto req = TCypressYPathProxy::Create("//sys/nodes/" + addressToken);
            req->set_type(EObjectType::CellNode);
            req->set_ignore_existing(true);

            ExecuteVerb(rootService, req).Subscribe(
                BIND(&TImpl::CheckCypressResponse<TCypressYPathProxy::TRspCreate>, MakeStrong(this)));
        }

        {
            auto req = TCypressYPathProxy::Create("//sys/nodes/" + addressToken + "/orchid");
            req->set_type(EObjectType::Orchid);
            req->set_ignore_existing(true);

            auto attributes = CreateEphemeralAttributes();
            attributes->Set("remote_address", address);
            ToProto(req->mutable_node_attributes(), *attributes);

            ExecuteVerb(rootService, req).Subscribe(
                BIND(&TImpl::CheckCypressResponse<TCypressYPathProxy::TRspCreate>, MakeStrong(this)));
        }

        {
            auto req = TCypressYPathProxy::Lock("//sys/nodes/" + addressToken);
            req->set_mode(ELockMode::Shared);
            SetTransactionId(req, transaction->GetId());

            ExecuteVerb(rootService, req).Subscribe(
                BIND(&TImpl::CheckCypressResponse<TCypressYPathProxy::TRspLock>, MakeStrong(this)));
        }
    }

    template <class TResponse>
    void CheckCypressResponse(TIntrusivePtr<TResponse> rsp)
    {
        if (!rsp->IsOK()) {
            LOG_ERROR(*rsp, "Error registering node in Cypress");
        }
    }


    TNode* DoRegisterNode(const TNodeDescriptor& descriptor, const TNodeStatistics& statistics)
    {
        PROFILE_TIMING ("/node_register_time") {
            const auto& address = descriptor.Address;
            auto config = GetNodeConfigByAddress(address);
            auto nodeId = GenerateNodeId();

            auto* node = new TNode(nodeId, descriptor, config);
            node->SetState(ENodeState::Registered);
            node->Statistics() = statistics;

            NodeMap.Insert(nodeId, node);
            AddressToNodeMap.insert(std::make_pair(address, node));
            HostNameToNodeMap.insert(std::make_pair(Stroka(GetServiceHostName(address)), node));
            
            UpdateNodeCounters(node, +1);

            // Create lease transaction.
            auto transactionManager = Bootstrap->GetTransactionManager();
            auto timeout = GetLeaseTimeout(node);
            auto* transaction = transactionManager->StartTransaction(nullptr, timeout);
            node->SetTransaction(transaction);
            RegisterLeaseTransaction(node);

            // Set "title" attribute.
            auto objectManager = Bootstrap->GetObjectManager();
            auto* attributeSet = objectManager->GetOrCreateAttributes(TVersionedObjectId(transaction->GetId()));
            auto title = ConvertToYsonString(TRawString(Sprintf("Lease for node %s", ~node->GetAddress())));
            YCHECK(attributeSet->Attributes().insert(std::make_pair("title", title)).second);
            
            if (IsLeader()) {
                RegisterNodeInCypress(node);
            }

            LOG_INFO_UNLESS(IsRecovery(), "Node registered (NodeId: %d, Address: %s, %s)",
                nodeId,
                ~address,
                ~ToString(statistics));

            NodeRegistered_.Fire(node);

            return node;
        }
    }

    void DoUnregisterNode(TNode* node)
    {
        PROFILE_TIMING ("/node_unregister_time") {
            auto nodeId = node->GetId();

            LOG_INFO_UNLESS(IsRecovery(), "Node unregistered (NodeId: %d, Address: %s)",
                nodeId,
                ~node->GetAddress());

            auto* transaction = node->GetTransaction();
            if (transaction && transaction->GetState() == ETransactionState::Active) {
                auto transactionManager = Bootstrap->GetTransactionManager();
                transactionManager->AbortTransaction(transaction);
            }

            UnregisterLeaseTransaction(node);
            
            const auto& address = node->GetAddress();
            YCHECK(AddressToNodeMap.erase(address) == 1);
            {
                auto hostNameRange = HostNameToNodeMap.equal_range(Stroka(GetServiceHostName(address)));
                for (auto it = hostNameRange.first; it != hostNameRange.second; ++it) {
                    if (it->second == node) {
                        HostNameToNodeMap.erase(it);
                        break;
                    }
                }
            }

            UpdateNodeCounters(node, -1);

            NodeUnregistered_.Fire(node);

            NodeMap.Remove(nodeId);
        }
    }


    void PostUnregisterCommit(TNode* node)
    {
        auto nodeId = node->GetId();

        TMetaReqUnregisterNode message;
        message.set_node_id(nodeId);

        auto invoker = Bootstrap->GetMetaStateFacade()->GetEpochInvoker();
        CreateUnregisterNodeMutation(message)
            ->OnSuccess(BIND(&TThis::OnUnregisterCommitSucceeded, MakeStrong(this), nodeId).Via(invoker))
            ->OnError(BIND(&TThis::OnUnregisterCommitFailed, MakeStrong(this), nodeId).Via(invoker))
            ->PostCommit();
    }

    void OnUnregisterCommitSucceeded(TNodeId nodeId)
    {
        LOG_INFO("Node unregister commit succeeded (NodeId: %d)",
            nodeId);
    }

    void OnUnregisterCommitFailed(TNodeId nodeId, const TError& error)
    {
        LOG_ERROR(error, "Node unregister commit failed (NodeId: %d)",
            nodeId);
    }


    void OnNodeConfigUpdated(TNode* node)
    {
        if (node->GetConfig()->Banned) {
            LOG_INFO("Node banned (Address: %s)", ~node->GetAddress());
            PostUnregisterCommit(node);
        }
    }

};

DEFINE_METAMAP_ACCESSORS(TNodeTracker::TImpl, Node, TNode, TNodeId, NodeMap)

///////////////////////////////////////////////////////////////////////////////

TNodeTracker::TNodeTracker(
    TNodeTrackerConfigPtr config,
    TBootstrap* bootstrap)
    : Impl(New<TImpl>(config, bootstrap))
{ }

void TNodeTracker::Initialize()
{
    Impl->Initialize();
}

TNodeTracker::~TNodeTracker()
{ }

TNode* TNodeTracker::FindNodeByAddress(const Stroka& address)
{
    return Impl->FindNodeByAddress(address);
}

TNode* TNodeTracker::GetNodeByAddress(const Stroka& address)
{
    return Impl->GetNodeByAddress(address);
}

TNode* TNodeTracker::FindNodeByHostName(const Stroka& hostName)
{
    return Impl->FindNodeByHostName(hostName);
}

TNode* TNodeTracker::GetNodeOrThrow(TNodeId id)
{
    return Impl->GetNodeOrThrow(id);
}

TNodeConfigPtr TNodeTracker::FindNodeConfigByAddress(const Stroka& address)
{
    return Impl->FindNodeConfigByAddress(address);
}

TNodeConfigPtr TNodeTracker::GetNodeConfigByAddress(const Stroka& address)
{
    return Impl->GetNodeConfigByAddress(address);
}

TMutationPtr TNodeTracker::CreateRegisterNodeMutation(
    const TMetaReqRegisterNode& request)
{
    return Impl->CreateRegisterNodeMutation(request);
}

TMutationPtr TNodeTracker::CreateUnregisterNodeMutation(
    const TMetaReqUnregisterNode& request)
{
    return Impl->CreateUnregisterNodeMutation(request);
}

TMutationPtr TNodeTracker::CreateFullHeartbeatMutation(
    TCtxFullHeartbeatPtr context)
{
    return Impl->CreateFullHeartbeatMutation(context);
}

TMutationPtr TNodeTracker::CreateIncrementalHeartbeatMutation(
    const TMetaReqIncrementalHeartbeat& request)
{
    return Impl->CreateIncrementalHeartbeatMutation(request);
}

void TNodeTracker::RefreshNodeConfig(TNode* node)
{
    return Impl->RefreshNodeConfig(node);
}

TTotalNodeStatistics TNodeTracker::GetTotalNodeStatistics()
{
    return Impl->GetTotalNodeStatistics();
}

int TNodeTracker::GetRegisteredNodeCount()
{
    return Impl->GetRegisteredNodeCount();
}

int TNodeTracker::GetOnlineNodeCount()
{
    return Impl->GetOnlineNodeCount();
}

DELEGATE_METAMAP_ACCESSORS(TNodeTracker, Node, TNode, TNodeId, *Impl)

DELEGATE_SIGNAL(TNodeTracker, void(TNode* node), NodeRegistered, *Impl);
DELEGATE_SIGNAL(TNodeTracker, void(TNode* node), NodeUnregistered, *Impl);
DELEGATE_SIGNAL(TNodeTracker, void(TNode* node), NodeConfigUpdated, *Impl);
DELEGATE_SIGNAL(TNodeTracker, void(TNode* node, const TMetaReqFullHeartbeat& request), FullHeartbeat, *Impl);
DELEGATE_SIGNAL(TNodeTracker, void(TNode* node, const TMetaReqIncrementalHeartbeat& request), IncrementalHeartbeat, *Impl);

///////////////////////////////////////////////////////////////////////////////

} // namespace NNodeTrackerServer
} // namespace NYT
