#include "stdafx.h"
#include "cell_manager.h"

#include <ytlib/ytree/serialize.h>
#include <ytlib/misc/address.h>
#include <ytlib/rpc/channel.h>

namespace NYT {
namespace NElection {

using namespace NYTree;

///////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = ElectionLogger;

////////////////////////////////////////////////////////////////////////////////

NRpc::TChannelCache TCellManager::ChannelCache;

////////////////////////////////////////////////////////////////////////////////

TCellManager::TCellManager(TCellConfigPtr config)
    : Config(config)
    , OrderedAddresses()
{
    OrderedAddresses = Config->Addresses;
    std::sort(OrderedAddresses.begin(), OrderedAddresses.end());
    
    SelfAddress_ = BuildServiceAddress(GetLocalHostName(), Config->RpcPort);
    SelfId_ = std::distance(
        OrderedAddresses.begin(),
        std::find(OrderedAddresses.begin(), OrderedAddresses.end(), SelfAddress_));

    if (SelfId_ == OrderedAddresses.size()) {
        LOG_FATAL("Self is absent in the list of masters (SelfAddress: %s)",
            ~SelfAddress_);
    }
}

i32 TCellManager::GetQuorum() const
{
    return GetPeerCount() / 2 + 1;
}

i32 TCellManager::GetPeerCount() const
{
    return OrderedAddresses.size();
}

Stroka TCellManager::GetPeerAddress(TPeerId id) const
{
    return OrderedAddresses[id];
}

NRpc::IChannelPtr TCellManager::GetMasterChannel(TPeerId id) const
{
    return ChannelCache.GetChannel(GetPeerAddress(id));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NElection
} // namespace NYT
