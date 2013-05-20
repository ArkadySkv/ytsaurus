#pragma once

#include "public.h"

#include <ytlib/misc/property.h>

#include <ytlib/node_tracker_client/node_directory.h>
#include <ytlib/node_tracker_client/node_tracker_service.pb.h>

#include <server/chunk_server/chunk_replica.h>

#include <server/transaction_server/public.h>

#include <server/cell_master/public.h>

namespace NYT {
namespace NNodeTrackerServer {

////////////////////////////////////////////////////////////////////////////////

DECLARE_ENUM(ENodeState,
    // Not registered.
    (Offline)
    // Registered but did not report the first heartbeat yet.
    (Registered)
    // Registered and reported the first heartbeat.
    (Online)
);

class TNode
{
    // Import third-party types into the scope.
    typedef NChunkServer::TChunkPtrWithIndex TChunkPtrWithIndex;
    typedef NChunkServer::TChunkId TChunkId;
    typedef NChunkServer::TChunk TChunk;
    typedef NChunkServer::TJobPtr TJobPtr;

    DEFINE_BYVAL_RO_PROPERTY(TNodeId, Id);
    DEFINE_BYVAL_RW_PROPERTY(ENodeState, State);
    DEFINE_BYVAL_RW_PROPERTY(bool, UnregisterPending);
    
    DEFINE_BYREF_RW_PROPERTY(NNodeTrackerClient::NProto::TNodeStatistics, Statistics);

    DEFINE_BYREF_RW_PROPERTY(NNodeTrackerClient::NProto::TNodeResources, ResourceLimits);
    DEFINE_BYREF_RW_PROPERTY(NNodeTrackerClient::NProto::TNodeResources, ResourceUsage);

    // Lease tracking.
    DEFINE_BYVAL_RW_PROPERTY(NTransactionServer::TTransaction*, Transaction);

    // Chunk Manager stuff.
    DEFINE_BYVAL_RW_PROPERTY(bool, Decommissioned); // kept in sync with |GetConfig()->Decommissioned|.
    DEFINE_BYREF_RW_PROPERTY(yhash_set<TChunkPtrWithIndex>, StoredReplicas);
    DEFINE_BYREF_RW_PROPERTY(yhash_set<TChunkPtrWithIndex>, SafelyStoredReplicas);
    DEFINE_BYREF_RW_PROPERTY(yhash_set<TChunkPtrWithIndex>, CachedReplicas);
    DEFINE_BYREF_RW_PROPERTY(yhash_set<TChunkPtrWithIndex>, UnapprovedReplicas);
    DEFINE_BYREF_RW_PROPERTY(yhash_set<TJobPtr>, Jobs);
    DEFINE_BYVAL_RW_PROPERTY(int, HintedSessionCount);


    //! Indexed by priority.
    typedef std::vector<yhash_set<NChunkClient::TChunkIdWithIndex>> TChunkReplicationQueues;
    DEFINE_BYREF_RW_PROPERTY(TChunkReplicationQueues, ChunkReplicationQueues);

    typedef yhash_set<NChunkClient::TChunkIdWithIndex> TChunkRemovalQueue;
    DEFINE_BYREF_RW_PROPERTY(TChunkRemovalQueue, ChunkRemovalQueue);

public:
    TNode(
        TNodeId id,
        const TNodeDescriptor& descriptor,
        TNodeConfigPtr config);
    explicit TNode(TNodeId id);

    ~TNode();

    const TNodeDescriptor& GetDescriptor() const;
    const Stroka& GetAddress() const;

    const TNodeConfigPtr& GetConfig() const;

    void Save(const NCellMaster::TSaveContext& context) const;
    void Load(const NCellMaster::TLoadContext& context);

    // Chunk Manager stuff.
    void AddReplica(TChunkPtrWithIndex replica, bool cached);
    void RemoveReplica(TChunkPtrWithIndex replica, bool cached);
    bool HasReplica(TChunkPtrWithIndex, bool cached) const;

    void MarkReplicaUnapproved(TChunkPtrWithIndex replica);
    bool HasUnapprovedReplica(TChunkPtrWithIndex replica) const;
    void ApproveReplica(TChunkPtrWithIndex replica);

    int GetTotalSessionCount() const;

private:
    TNodeDescriptor Descriptor_;
    TNodeConfigPtr Config_;

    void Init();

};

TNodeId GetObjectId(const TNode* node);
bool CompareObjectsForSerialization(const TNode* lhs, const TNode* rhs);

////////////////////////////////////////////////////////////////////////////////

} // namespace NNodeTrackerServer
} // namespace NYT
