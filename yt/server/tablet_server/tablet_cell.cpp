#include "stdafx.h"
#include "tablet_cell.h"
#include "tablet.h"

#include <ytlib/tablet_client/config.h>

#include <server/cell_master/serialize.h>

namespace NYT {
namespace NTabletServer {

using namespace NElection;
using namespace NNodeTrackerServer;
using namespace NCellMaster;

////////////////////////////////////////////////////////////////////////////////

void TTabletCell::TPeer::Persist(NCellMaster::TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, Address);
    Persist(context, Node);
    Persist(context, SlotIndex);
    Persist(context, LastSeenTime);
}

////////////////////////////////////////////////////////////////////////////////

TTabletCell::TTabletCell(const TTabletCellId& id)
    : TNonversionedObjectBase(id)
    , State_(ETabletCellState::Starting)
    , Size_(-1)
    , ConfigVersion_(0)
{ }

void TTabletCell::Save(TSaveContext& context) const
{
    TNonversionedObjectBase::Save(context);

    using NYT::Save;
    Save(context, State_);
    Save(context, Size_);
    Save(context, Peers_);
    Save(context, ConfigVersion_);
    Save(context, *Config_);
    Save(context, *Options_);
    Save(context, Tablets_);
}

void TTabletCell::Load(TLoadContext& context)
{
    TNonversionedObjectBase::Load(context);

    using NYT::Load;
    Load(context, State_);
    Load(context, Size_);
    Load(context, Peers_);
    Load(context, ConfigVersion_);
    Load(context, *Config_);
    Load(context, *Options_);
    Load(context, Tablets_);
}

TPeerId TTabletCell::FindPeerId(const Stroka& address) const
{
    for (int index = 0; index < static_cast<int>(Peers_.size()); ++index) {
        if (Peers_[index].Address == address) {
            return index;
        }
    }
    return InvalidPeerId;
}

TPeerId TTabletCell::GetPeerId(const Stroka& address) const
{
    auto peerId = FindPeerId(address);
    YCHECK(peerId != InvalidPeerId);
    return peerId;
}

TPeerId TTabletCell::FindPeerId(TNode* node) const
{
    for (int index = 0; index < static_cast<int>(Peers_.size()); ++index) {
        if (Peers_[index].Node == node) {
            return index;
        }
    }
    return InvalidPeerId;
}

TPeerId TTabletCell::GetPeerId(TNode* node) const
{
    auto peerId = FindPeerId(node);
    YCHECK(peerId != InvalidPeerId);
    return peerId;
}

void TTabletCell::AssignPeer(TNode* node, TPeerId peerId)
{
    auto& peer = Peers_[peerId];
    peer.Address = node->GetAddress();
}

void TTabletCell::RevokePeer(TPeerId peerId)
{
    auto& peer = Peers_[peerId];
    YCHECK(peer.Address);
    peer.Address = Null;
    YCHECK(!peer.Node);
}

void TTabletCell::AttachPeer(TNode* node, TPeerId peerId, int slotIndex)
{
    auto& peer = Peers_[peerId];
    YCHECK(peer.Address);
    YCHECK(*peer.Address == node->GetAddress());

    YCHECK(!peer.Node);
    peer.Node = node;

    YCHECK(peer.SlotIndex == -1);
    peer.SlotIndex = slotIndex;
}

void TTabletCell::DetachPeer(TNode* node)
{
    auto peerId = FindPeerId(node);
    if (peerId != InvalidPeerId) {
        Peers_[peerId].Node = nullptr;
        Peers_[peerId].SlotIndex = -1;
    }
}

void TTabletCell::UpdatePeerSeenTime(TPeerId peerId, TInstant when)
{
    auto& peer = Peers_[peerId];
    peer.LastSeenTime = when;
}

int TTabletCell::GetOnlinePeerCount() const
{
    int result = 0;
    for (const auto& peer : Peers_) {
        if (peer.Address && peer.Node) {
            ++result;
        }
    }
    return result;
}

ETabletCellHealth TTabletCell::GetHealth() const
{
    int leaderCount = 0;
    int followerCount = 0;
    for (const auto& peer : Peers_) {
        if (peer.Node) {
            const auto& slot = peer.Node->TabletSlots()[peer.SlotIndex];
            switch (slot.PeerState) {
                case EPeerState::Leading:
                    ++leaderCount;
                    break;
                case EPeerState::Following:
                    ++followerCount;
                    break;
                default:
                    break;
            }
        }
    }

    if (leaderCount == 1 && followerCount == Size_ - 1) {
        return ETabletCellHealth::Good;
    }

    if (Tablets_.empty()) {
        return ETabletCellHealth::Initializing;
    }

    if (leaderCount == 1 && followerCount >= Size_ / 2) {
        return ETabletCellHealth::Degraded;
    }

    return ETabletCellHealth::Failed;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletServer
} // namespace NYT

