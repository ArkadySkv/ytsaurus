#pragma once

#include "public.h"

#include <server/cell_scheduler/public.h>
#include <ytlib/rpc/public.h>
#include <ytlib/ytree/public.h>

namespace NYT {
namespace NScheduler {

////////////////////////////////////////////////////////////////////

class TScheduler
    : public TRefCounted
{
public:
    TScheduler(
        TSchedulerConfigPtr config,
        NCellScheduler::TBootstrap* bootstrap);

    ~TScheduler();

    void Start();

    NRpc::IServicePtr GetService();
    NYTree::TYPathServiceProducer CreateOrchidProducer();

    std::vector<TOperationPtr> GetOperations();
    std::vector<TExecNodePtr> GetExecNodes();

    IInvokerPtr GetSnapshotIOInvoker();

private:
    class TImpl;
    TIntrusivePtr<TImpl> Impl;

    class TSchedulingContext;

};

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

