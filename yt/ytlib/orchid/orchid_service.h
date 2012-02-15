#pragma once

#include "common.h"
#include "orchid_service_proxy.h"

#include <ytlib/ytree/public.h>
#include <ytlib/rpc/service.h>
#include <ytlib/rpc/server.h>

namespace NYT {
namespace NOrchid {

////////////////////////////////////////////////////////////////////////////////

class TOrchidService
    : public NRpc::TServiceBase
{
public:
    typedef TIntrusivePtr<TOrchidService> TPtr;

    //! Creates an instance.
    TOrchidService(
        NYTree::INode* root,
        IInvoker* invoker);

private:
    typedef TOrchidService TThis;

    NYTree::IYPathService::TPtr RootService;

    DECLARE_RPC_SERVICE_METHOD(NProto, Execute);

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NOrchid
} // namespace NYT

