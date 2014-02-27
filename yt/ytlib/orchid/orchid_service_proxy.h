#pragma once

#include <core/rpc/client.h>

#include <ytlib/orchid/orchid_service.pb.h>

namespace NYT {
namespace NOrchid {

////////////////////////////////////////////////////////////////////////////////

class TOrchidServiceProxy
    : public NRpc::TProxyBase
{
public:
    typedef TIntrusivePtr<TOrchidServiceProxy> TPtr;

    static Stroka GetServiceName()
    {
        return "OrchidService";
    }

    TOrchidServiceProxy(NRpc::IChannelPtr channel)
        : TProxyBase(channel, GetServiceName())
    { }

    DEFINE_RPC_PROXY_METHOD(NProto, Execute);

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NOrchid
} // namespace NYT
