﻿#pragma once

#include "public.h"

#include <ytlib/exec_agent/supervisor_service.pb.h>
#include <ytlib/rpc/service.h>
#include <ytlib/rpc/client.h>

namespace NYT {
namespace NExecAgent {

////////////////////////////////////////////////////////////////////////////////

class TSupervisorServiceProxy
    : public NRpc::TProxyBase
{
public:
    typedef TIntrusivePtr<TSupervisorServiceProxy> TPtr;

    static Stroka GetServiceName()
    {
        return "SupervisorService";
    }

    TSupervisorServiceProxy(NRpc::IChannel* channel)
        : TProxyBase(channel, GetServiceName())
    { }

    DEFINE_RPC_PROXY_METHOD(NProto, GetJobSpec);
    DEFINE_ONE_WAY_RPC_PROXY_METHOD(NProto, OnJobFinished);
    DEFINE_ONE_WAY_RPC_PROXY_METHOD(NProto, OnProgress);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NExecAgent
} // namespace NYT
