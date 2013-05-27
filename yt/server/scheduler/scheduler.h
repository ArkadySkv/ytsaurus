#pragma once    

#include "public.h"

#include <ytlib/rpc/service_detail.h>

#include <ytlib/ytree/public.h>

#include <ytlib/transaction_client/public.h>

#include <ytlib/meta_state/public.h>

#include <ytlib/node_tracker_client/node_directory.h>

#include <ytlib/job_tracker_client/job_tracker_service.pb.h>

#include <server/cell_scheduler/public.h>

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

    void Initialize();

    NYTree::TYPathServiceProducer CreateOrchidProducer();

    std::vector<TOperationPtr> GetOperations();
    std::vector<TExecNodePtr> GetExecNodes();

    IInvokerPtr GetSnapshotIOInvoker();

    bool IsConnected();
    void ValidateConnected();

    TOperationPtr FindOperation(const TOperationId& id);
    TOperationPtr GetOperationOrThrow(const TOperationId& id);

    TExecNodePtr FindNode(const Stroka& address);
    TExecNodePtr GetNode(const Stroka& address);
    TExecNodePtr GetOrCreateNode(const NNodeTrackerClient::TNodeDescriptor& descriptor);

    typedef TValueOrError<TOperationPtr> TStartResult;
    TFuture<TStartResult> StartOperation(
        EOperationType type,
        const NTransactionClient::TTransactionId& transactionId,
        const NMetaState::TMutationId& mutationId,
        NYTree::IMapNodePtr spec,
        const Stroka& user);

    TFuture<void> AbortOperation(
        TOperationPtr operation,
        const TError& error);

    TAsyncError SuspendOperation(TOperationPtr operation);
    TAsyncError ResumeOperation(TOperationPtr operation);

    typedef
        NRpc::TTypedServiceContext<
            NJobTrackerClient::NProto::TReqHeartbeat,
            NJobTrackerClient::NProto::TRspHeartbeat>
        TCtxHeartbeat;
    typedef TIntrusivePtr<TCtxHeartbeat> TCtxHeartbeatPtr;
    void ProcessHeartbeat(
        TExecNodePtr node,
        TCtxHeartbeatPtr context);

private:
    class TImpl;
    TIntrusivePtr<TImpl> Impl;

    class TSchedulingContext;

};

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

