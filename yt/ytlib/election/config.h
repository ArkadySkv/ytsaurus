#pragma once

#include "public.h"

#include <ytlib/misc/error.h>

#include <ytlib/ytree/yson_serializable.h>

namespace NYT {
namespace NElection {

////////////////////////////////////////////////////////////////////////////////

class TCellConfig
    : public TYsonSerializable
{
public:
    //! RPC interface port number.
    int RpcPort;

    //! Master server addresses.
    std::vector<Stroka> Addresses;

    TCellConfig()
    {
        RegisterParameter("rpc_port", RpcPort)
            .Default(9000);
        RegisterParameter("addresses", Addresses)
            .NonEmpty();

        RegisterValidator([&] () {
            if (Addresses.size() % 2 != 1) {
                THROW_ERROR_EXCEPTION("Number of masters must be odd");
            }
        });
    }
};

////////////////////////////////////////////////////////////////////////////////

class TElectionManagerConfig
    : public TYsonSerializable
{
public:
    TDuration VotingRoundInterval;
    TDuration RpcTimeout;
    TDuration FollowerPingInterval;
    TDuration FollowerPingTimeout;
    TDuration ReadyToFollowTimeout;
    TDuration PotentialFollowerTimeout;

    TElectionManagerConfig()
    {
        RegisterParameter("voting_round_interval", VotingRoundInterval)
            .GreaterThan(TDuration())
            .Default(TDuration::MilliSeconds(100));
        RegisterParameter("rpc_timeout", RpcTimeout)
            .GreaterThan(TDuration())
            .Default(TDuration::MilliSeconds(1000));
        RegisterParameter("follower_ping_interval", FollowerPingInterval)
            .GreaterThan(TDuration())
            .Default(TDuration::MilliSeconds(1000));
        RegisterParameter("follower_ping_timeout", FollowerPingTimeout)
            .GreaterThan(TDuration())
            .Default(TDuration::MilliSeconds(5000));
        RegisterParameter("ready_to_follow_timeout", ReadyToFollowTimeout)
            .GreaterThan(TDuration())
            .Default(TDuration::MilliSeconds(5000));
        RegisterParameter("potential_follower_timeout", PotentialFollowerTimeout)
            .GreaterThan(TDuration())
            .Default(TDuration::MilliSeconds(5000));
    }
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NElection
} // namespace NYT
