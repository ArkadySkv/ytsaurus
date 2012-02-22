#pragma once

#include "common.h"
#include "node.h"
#include "type_handler.h"
#include "node_proxy.h"
#include "lock.h"

#include <ytlib/misc/thread_affinity.h>
#include <ytlib/transaction_server/transaction.h>
#include <ytlib/transaction_server/transaction_manager.h>
#include <ytlib/ytree/ypath_service.h>
#include <ytlib/ytree/tree_builder.h>
#include <ytlib/misc/id_generator.h>
#include <ytlib/meta_state/meta_state_manager.h>
#include <ytlib/meta_state/composite_meta_state.h>
#include <ytlib/meta_state/map.h>
#include <ytlib/meta_state/meta_change.h>
#include <ytlib/object_server/object_manager.h>

namespace NYT {
namespace NCypress {

////////////////////////////////////////////////////////////////////////////////

struct ICypressNodeProxy;

class TCypressManager
    : public NMetaState::TMetaStatePart
{
public:
    typedef TCypressManager TThis;
    typedef TIntrusivePtr<TThis> TPtr;

    TCypressManager(
        NMetaState::IMetaStateManager* metaStateManager,
        NMetaState::TCompositeMetaState* metaState,
        NTransactionServer::TTransactionManager* transactionManager,
        NObjectServer::TObjectManager* objectManager);

    void RegisterHandler(INodeTypeHandler* handler);
    INodeTypeHandler* GetHandler(EObjectType type);

	//! Returns the id of the root node.
	/*!
	 *  \note
	 *  This id depends on cell id.
	 */
    TNodeId GetRootNodeId();

	//! Returns a service producer that is absolutely safe to use from any thread.
	/*!
	 *  The producer first makes a coarse check to ensure that the peer is leading.
	 *  If it passes, then the request is forwarded to the state thread and
	 *  another (rigorous) check is made.
	 */
	NYTree::TYPathServiceProducer GetRootServiceProducer();

    // TODO: killme
    NObjectServer::TObjectManager* GetObjectManager() const;
    NTransactionServer::TTransactionManager* GetTransactionManager() const;
    NMetaState::IMetaStateManager* GetMetaStateManager() const;

    const ICypressNode* FindVersionedNode(
        const TNodeId& nodeId,
        const TTransactionId& transactionId) const;

    const ICypressNode& GetVersionedNode(
        const TNodeId& nodeId,
        const TTransactionId& transactionId) const;

    ICypressNode* FindVersionedNode(
        const TNodeId& nodeId,
        const TTransactionId& transactionId,
        ELockMode requestedMode = ELockMode::Exclusive);

    ICypressNode& GetVersionedNode(
        const TNodeId& nodeId,
        const TTransactionId& transactionId,
        ELockMode requestedMode = ELockMode::Exclusive);

    TIntrusivePtr<ICypressNodeProxy> FindVersionedNodeProxy(
        const TNodeId& nodeId,
        const TTransactionId& transactionId);

    TIntrusivePtr<ICypressNodeProxy> GetVersionedNodeProxy(
        const TNodeId& nodeId,
        const TTransactionId& transactionId);

    TLockId LockVersionedNode(
        const TNodeId& nodeId,
        const TTransactionId& transactionId,
        ELockMode requestedMode = ELockMode::Exclusive);

    TNodeId CreateNode(
        EObjectType type,
        const TTransactionId& transactionId);

    TNodeId CreateDynamicNode(
        const TTransactionId& transactionId,
        NObjectServer::EObjectType type,
        NYTree::IMapNode* manifest);

    void RegisterNode(
        const TTransactionId& transactionId,
        TAutoPtr<ICypressNode> node);

    DECLARE_METAMAP_ACCESSORS(Lock, TLock, TLockId);
    DECLARE_METAMAP_ACCESSORS(Node, ICypressNode, TVersionedNodeId);

private:
    class TLockTypeHandler;
    class TNodeTypeHandler;
    class TYPathProcessor;
    class TRootProxy;

    class TNodeMapTraits
    {
    public:
        TNodeMapTraits(TCypressManager* cypressManager);

        TAutoPtr<ICypressNode> Create(const TVersionedNodeId& id) const;

    private:
        TCypressManager::TPtr CypressManager;
    };
    
    NTransactionServer::TTransactionManager::TPtr TransactionManager;
    NObjectServer::TObjectManager::TPtr ObjectManager;

    NMetaState::TMetaStateMap<TVersionedNodeId, ICypressNode, TNodeMapTraits> NodeMap;
    NMetaState::TMetaStateMap<TLockId, TLock> LockMap;

    yvector<INodeTypeHandler::TPtr> TypeToHandler;

    yhash_map<TNodeId, INodeBehavior::TPtr> NodeBehaviors;

    i32 RefNode(const TNodeId& nodeId);
    i32 UnrefNode(const TNodeId& nodeId);
    i32 GetNodeRefCounter(const TNodeId& nodeId);

    void SaveKeys(TOutputStream* output); // TODO(roizner): make const once new actions are ready
    void SaveValues(TOutputStream* output); // TODO(roizner): make const once new actions are ready
    void LoadKeys(TInputStream* input);
    void LoadValues(TInputStream* input);
    virtual void Clear();

    virtual void OnLeaderRecoveryComplete();
    virtual void OnStopLeading();

    void OnTransactionCommitted(NTransactionServer::TTransaction& transaction);
    void OnTransactionAborted(NTransactionServer::TTransaction& transaction);

    void ReleaseLocks(const NTransactionServer::TTransaction& transaction);
    void MergeBranchedNodes(const NTransactionServer::TTransaction& transaction);
    void MergeBranchedNode(
        const NTransactionServer::TTransaction& transaction,
        const TNodeId& nodeId);
    void RemoveBranchedNodes(const NTransactionServer::TTransaction& transaction);
    void UnrefOriginatingNodes(const NTransactionServer::TTransaction& transaction);

    INodeTypeHandler* GetHandler(const ICypressNode& node);

    void CreateNodeBehavior(const TNodeId& id);
    void DestroyNodeBehavior(const TNodeId& id);

    void ValidateLock(
        const TNodeId& nodeId,
        const TTransactionId& transactionId,
        ELockMode requestedMode,
        bool* isMandatory = NULL);

    static bool AreCompetingLocksCompatible(ELockMode existingMode, ELockMode requestedMode);
    static bool AreConcurrentLocksCompatible(ELockMode existingMode, ELockMode requestedMode);

    static bool IsLockRecursive(ELockMode mode);
   
    TLockId AcquireLock(
        const TNodeId& nodeId,
        const TTransactionId& transactionId,
        ELockMode mode);

    void ReleaseLock(const TLockId& lockId);

   ICypressNode& BranchNode(
       ICypressNode& node,
       const TTransactionId& transactionId,
       ELockMode mode);

    template <class TImpl, class TProxy>
    TNodeId CreateNode(
        const TTransactionId& transactionId,
        EObjectType type);

    DECLARE_THREAD_AFFINITY_SLOT(StateThread);

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NCypress
} // namespace NYT
