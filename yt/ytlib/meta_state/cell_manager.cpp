#include "stdafx.h"
#include "cell_manager.h"

#include <util/system/hostname.h>

namespace NYT {
namespace NMetaState {

///////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = MetaStateLogger;

////////////////////////////////////////////////////////////////////////////////

NRpc::TChannelCache TCellManager::ChannelCache;

////////////////////////////////////////////////////////////////////////////////

TCellManager::TCellManager(TCellConfig* config)
    : Config(config)
    , OrderedAddresses()
{
    OrderedAddresses = Config->Addresses;
    std::sort(OrderedAddresses.begin(), OrderedAddresses.end());
    
    SelfAddress_ = Sprintf("%s:%d", GetHostName(), Config->RpcPort);
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

////////////////////////////////////////////////////////////////////////////////

} // namespace NMetaState
} // namespace NYT
