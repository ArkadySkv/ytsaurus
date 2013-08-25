#pragma once

#include "public.h"
#include "node.h"
#include "type_handler.h"
#include "node_proxy.h"
#include "lock.h"

#include <ytlib/misc/small_vector.h>
#include <ytlib/misc/thread_affinity.h>
#include <ytlib/misc/id_generator.h>

#include <ytlib/ytree/ypath_service.h>
#include <ytlib/ytree/tree_builder.h>

#include <ytlib/meta_state/meta_state_manager.h>
#include <ytlib/meta_state/composite_meta_state.h>
#include <ytlib/meta_state/map.h>
#include <ytlib/meta_state/mutation.h>

#include <server/object_server/object_manager.h>

#include <server/cell_master/public.h>

#include <server/transaction_server/transaction.h>
#include <server/transaction_server/transaction_manager.h>

#include <server/security_server/public.h>

#include <server/cypress_server/cypress_manager.pb.h>

namespace NYT {
namespace NCypressServer {

////////////////////////////////////////////////////////////////////////////////

class TCypressManager
    : public NMetaState::TMetaStatePart
{
public:
    explicit TCypressManager(
        TCypressManagerConfigPtr config,
        NCellMaster::TBootstrap* bootstrap);

    void Initialize();

    void RegisterHandler(INodeTypeHandlerPtr handler);
    INodeTypeHandlerPtr FindHandler(NObjectClient::EObjectType type);
    INodeTypeHandlerPtr GetHandler(NObjectClient::EObjectType type);
    INodeTypeHandlerPtr GetHandler(const TCypressNodeBase* node);

    NMetaState::TMutationPtr CreateUpdateAccessStatisticsMutation(
        const NProto::TMetaReqUpdateAccessStatistics& request);

    typedef NRpc::TTypedServiceRequest<NCypressClient::NProto::TReqCreate> TReqCreate;
    typedef NRpc::TTypedServiceResponse<NCypressClient::NProto::TRspCreate> TRspCreate;

    //! Creates a new node and registers it.
    TCypressNodeBase* CreateNode(
        INodeTypeHandlerPtr handler,
        ICypressNodeFactoryPtr factory,
        TReqCreate* request,
        TRspCreate* response);

    //! Clones a node and registers its clone.
    TCypressNodeBase* CloneNode(
        TCypressNodeBase* sourceNode,
        ICypressNodeFactoryPtr factory);

    //! Returns the root node.
    TCypressNodeBase* GetRootNode() const;

    //! Creates a resolver that provides a view in the context of a given transaction.
    NYTree::INodeResolverPtr CreateResolver(NTransactionServer::TTransaction* transaction = nullptr);

    //! Similar to |FindNode| provided by |DECLARE_METAMAP_ACCESSORS| but
    //! specially optimized for the case of null transaction.
    TCypressNodeBase* FindNode(
        TCypressNodeBase* trunkNode,
        NTransactionServer::TTransaction* transaction);

    TCypressNodeBase* GetVersionedNode(
        TCypressNodeBase* trunkNode,
        NTransactionServer::TTransaction* transaction);

    ICypressNodeProxyPtr GetVersionedNodeProxy(
        TCypressNodeBase* trunkNode,
        NTransactionServer::TTransaction* transaction = nullptr);

    TCypressNodeBase* LockVersionedNode(
        TCypressNodeBase* trunkNode,
        NTransactionServer::TTransaction* transaction,
        const TLockRequest& request,
        bool recursive = false);

    void SetModified(
        TCypressNodeBase* trunkNode,
        NTransactionServer::TTransaction* transaction);

    void SetAccessed(TCypressNodeBase* trunkNode);

    typedef TSmallVector<TCypressNodeBase*, 1> TSubtreeNodes;
    TSubtreeNodes ListSubtreeNodes(
        TCypressNodeBase* trunkNode,
        NTransactionServer::TTransaction* transaction,
        bool includeRoot = true);

    bool IsOrphaned(TCypressNodeBase* trunkNode);

    DECLARE_METAMAP_ACCESSORS(Node, TCypressNodeBase, TVersionedNodeId);

private:
    typedef TCypressManager TThis;

    class TNodeTypeHandler;
    class TYPathResolver;
    class TRootService;

    class TNodeMapTraits
    {
    public:
        explicit TNodeMapTraits(TCypressManager* cypressManager);

        std::unique_ptr<TCypressNodeBase> Create(const TVersionedNodeId& id) const;

    private:
        TCypressManager* CypressManager;

    };

    TCypressManagerConfigPtr Config;
    NCellMaster::TBootstrap* Bootstrap;

    NMetaState::TMetaStateMap<TVersionedNodeId, TCypressNodeBase, TNodeMapTraits> NodeMap;

    std::vector<INodeTypeHandlerPtr> TypeToHandler;

    TNodeId RootNodeId;
    TCypressNodeBase* RootNode;

    TAccessTrackerPtr AccessTracker;
    
    
    void RegisterNode(std::unique_ptr<TCypressNodeBase> node);

    void DestroyNode(TCypressNodeBase* trunkNode);

    // TMetaStatePart overrides.
    virtual void OnRecoveryComplete() override;

    void DoClear();
    virtual void Clear() override;

    void SaveKeys(NCellMaster::TSaveContext& context) const;
    void SaveValues(NCellMaster::TSaveContext& context) const;
    
    virtual void OnBeforeLoaded() override;
    void LoadKeys(NCellMaster::TLoadContext& context);
    void LoadValues(NCellMaster::TLoadContext& context);
    virtual void OnAfterLoaded() override;

    void InitBuiltin();

    void OnTransactionCommitted(NTransactionServer::TTransaction* transaction);
    void OnTransactionAborted(NTransactionServer::TTransaction* transaction);

    void ReleaseLocks(NTransactionServer::TTransaction* transaction);
    void MergeNodes(NTransactionServer::TTransaction* transaction);
    void MergeNode(
        NTransactionServer::TTransaction* transaction,
        TCypressNodeBase* branchedNode);
    void RemoveBranchedNodes(NTransactionServer::TTransaction* transaction);
    void RemoveBranchedNode(TCypressNodeBase* branchedNode);
    void PromoteLocks(NTransactionServer::TTransaction* transaction);
    void PromoteLock(TLock* lock, NTransactionServer::TTransaction* parentTransaction);

    TError ValidateLock(
        TCypressNodeBase* trunkNode,
        NTransactionServer::TTransaction* transaction,
        const TLockRequest& request,
        bool* isMandatory);
    TError ValidateLock(
        TCypressNodeBase* trunkNode,
        NTransactionServer::TTransaction* transaction,
        const TLockRequest& request);
    bool IsRedundantLock(
        const TLock& existingLock,
        const TLockRequest& request);

    static bool IsParentTransaction(
        NTransactionServer::TTransaction* transaction,
        NTransactionServer::TTransaction* parent);
    static bool IsConcurrentTransaction(
        NTransactionServer::TTransaction* transaction1,
        NTransactionServer::TTransaction* transaction2);

    TCypressNodeBase* AcquireLock(
        TCypressNodeBase* trunkNode,
        NTransactionServer::TTransaction* transaction,
        const TLockRequest& request);
    TLock* DoAcquireLock(
        TCypressNodeBase* trunkNode,
        NTransactionServer::TTransaction* transaction,
        const TLockRequest& request);
    void ReleaseLock(
        TCypressNodeBase* trunkNode,
        NTransactionServer::TTransaction* transaction);

    void ListSubtreeNodes(
        TCypressNodeBase* trunkNode,
        NTransactionServer::TTransaction* transaction,
        bool includeRoot,
        TSubtreeNodes* subtreeNodes);

    TCypressNodeBase* BranchNode(
       TCypressNodeBase* originatingNode,
       NTransactionServer::TTransaction* transaction,
       ELockMode mode);

    NYPath::TYPath GetNodePath(
       TCypressNodeBase* trunkNode,
       NTransactionServer::TTransaction* transaction);

    virtual void OnActiveQuorumEstablished() override;
    virtual void OnStopLeading() override;
    
    void UpdateAccessStatistics(const NProto::TMetaReqUpdateAccessStatistics& request);


    DECLARE_THREAD_AFFINITY_SLOT(StateThread);

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NCypressServer
} // namespace NYT
