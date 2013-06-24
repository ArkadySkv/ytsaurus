#include "stdafx.h"
#include "node.h"

#include <server/chunk_server/job.h>
#include <server/chunk_server/chunk.h>

#include <server/transaction_server/transaction.h>

#include <server/cell_master/serialization_context.h>

namespace NYT {
namespace NNodeTrackerServer {

using namespace NChunkClient;
using namespace NChunkServer;

////////////////////////////////////////////////////////////////////////////////

TNode::TNode(
    TNodeId id,
    const TNodeDescriptor& descriptor,
    TNodeConfigPtr config)
    : Id_(id)
    , Descriptor_(descriptor)
    , Config_(config)
{
    Init();
}

TNode::TNode(TNodeId id)
    : Id_(id)
    , Config_(New<TNodeConfig>())
{
    Init();
}

void TNode::Init()
{
    UnregisterPending_ = false;
    VisitMark_ = 0;
    LoadRank_ = -1;
    Transaction_ = nullptr;
    Decommissioned_ = Config_->Decommissioned;
    ChunkReplicationQueues_.resize(ReplicationPriorityCount);
    ResetSessionHints();
}

TNode::~TNode()
{ }

const TNodeDescriptor& TNode::GetDescriptor() const
{
    return Descriptor_;
}

const Stroka& TNode::GetAddress() const
{
    return Descriptor_.Address;
}

const TNodeConfigPtr& TNode::GetConfig() const
{
    return Config_;
}

void TNode::Save(NCellMaster::TSaveContext& context) const
{
    using NYT::Save;
    Save(context, Descriptor_.Address);
    Save(context, State_);
    Save(context, Statistics_);
    SaveObjectRef(context, Transaction_);
    SaveObjectRefs(context, StoredReplicas_);
    SaveObjectRefs(context, CachedReplicas_);
    SaveObjectRefs(context, UnapprovedReplicas_);
}

void TNode::Load(NCellMaster::TLoadContext& context)
{
    using NYT::Load;
    Load(context, Descriptor_.Address);
    Load(context, State_);
    Load(context, Statistics_);
    LoadObjectRef(context, Transaction_);
    LoadObjectRefs(context, StoredReplicas_);
    LoadObjectRefs(context, CachedReplicas_);
    LoadObjectRefs(context, UnapprovedReplicas_);
}

void TNode::AddReplica(TChunkPtrWithIndex replica, bool cached)
{
    if (cached) {
        YCHECK(CachedReplicas_.insert(replica).second);
    } else {
        YCHECK(StoredReplicas_.insert(replica).second);
    }
}

void TNode::RemoveReplica(TChunkPtrWithIndex replica, bool cached)
{
    if (cached) {
        YCHECK(CachedReplicas_.erase(replica) == 1);
    } else {
        YCHECK(StoredReplicas_.erase(replica) == 1);
        UnapprovedReplicas_.erase(replica);
    }
}

bool TNode::HasReplica(TChunkPtrWithIndex replica, bool cached) const
{
    if (cached) {
        return CachedReplicas_.find(replica) != CachedReplicas_.end();
    } else {
        return StoredReplicas_.find(replica) != StoredReplicas_.end();
    }
}

void TNode::MarkReplicaUnapproved(TChunkPtrWithIndex replica)
{
    YASSERT(HasReplica(replica, false));
    YCHECK(UnapprovedReplicas_.insert(replica).second);
}

bool TNode::HasUnapprovedReplica(TChunkPtrWithIndex replica) const
{
    return UnapprovedReplicas_.find(replica) != UnapprovedReplicas_.end();
}

void TNode::ApproveReplica(TChunkPtrWithIndex replica)
{
    YASSERT(HasReplica(replica, false));
    YCHECK(UnapprovedReplicas_.erase(replica) == 1);
}

void TNode::ResetSessionHints()
{
    HintedUserSessionCount_ = 0;
    HintedReplicationSessionCount_ = 0;
    HintedRepairSessionCount_ = 0;
}

void TNode::AddSessionHint(EWriteSessionType sessionType)
{
    switch (sessionType) {
        case EWriteSessionType::User:
            ++HintedUserSessionCount_;
            break;
        case EWriteSessionType::Replication:
            ++HintedReplicationSessionCount_;
            break;
        case EWriteSessionType::Repair:
            ++HintedRepairSessionCount_;
            break;
        default:
            YUNREACHABLE();
    }
}

bool TNode::HasSpareSession(EWriteSessionType sessionType) const
{
    switch (sessionType) {
        case EWriteSessionType::User:
            return true;
        case EWriteSessionType::Replication:
            return Statistics_.total_replication_session_count() + HintedReplicationSessionCount_ <
                   Statistics_.max_replication_session_count();
        case EWriteSessionType::Repair:
            return Statistics_.total_repair_session_count() + HintedRepairSessionCount_ <
                   Statistics_.max_repair_session_count();
        default:
            YUNREACHABLE();
    }
}

int TNode::GetTotalSessionCount() const
{
    return
        Statistics_.total_user_session_count() + HintedUserSessionCount_ +
        Statistics_.total_replication_session_count() + HintedReplicationSessionCount_ +
        Statistics_.total_repair_session_count() + HintedRepairSessionCount_;
}

TAtomic TNode::GenerateVisitMark()
{
    static TAtomic result = 0;
    return AtomicIncrement(result);
}

////////////////////////////////////////////////////////////////////////////////

TNodeId GetObjectId(const TNode* node)
{
    return node->GetId();
}

bool CompareObjectsForSerialization(const TNode* lhs, const TNode* rhs)
{
    return GetObjectId(lhs) < GetObjectId(rhs);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NNodeTrackerServer
} // namespace NYT

