#include "stdafx.h"
#include "scheduler.h"
#include "private.h"
#include "scheduler_strategy.h"
#include "null_strategy.h"
#include "fifo_strategy.h"
#include "operation_controller.h"
#include "map_controller.h"
#include "merge_controller.h"
#include "scheduler_proxy.h"

#include <ytlib/misc/thread_affinity.h>
#include <ytlib/misc/periodic_invoker.h>
#include <ytlib/misc/string.h>

#include <ytlib/actions/action_queue.h>

#include <ytlib/logging/tagged_logger.h>

#include <ytlib/transaction_client/transaction_manager.h>

#include <ytlib/cypress/cypress_service_proxy.h>

#include <ytlib/cell_scheduler/config.h>
#include <ytlib/cell_scheduler/bootstrap.h>

#include <ytlib/scheduler/scheduler_service.pb.h>

#include <ytlib/cypress/cypress_ypath_proxy.h>
#include <ytlib/cypress/id.h>

#include <ytlib/object_server/object_ypath_proxy.h>

#include <ytlib/ytree/ypath_proxy.h>
#include <ytlib/ytree/ypath_client.h>
#include <ytlib/ytree/serialize.h>
#include <ytlib/ytree/fluent.h>

namespace NYT {
namespace NScheduler {

using namespace NCellScheduler;
using namespace NTransactionClient;
using namespace NCypress;
using namespace NYTree;
using namespace NObjectServer;
using namespace NProto;

////////////////////////////////////////////////////////////////////

NLog::TLogger& Logger = SchedulerLogger;
NProfiling::TProfiler& Profiler = SchedulerProfiler;

////////////////////////////////////////////////////////////////////

class TScheduler::TImpl
    : public NRpc::TServiceBase
    , public IOperationHost
{
public:
    TImpl(
        TSchedulerConfigPtr config,
        NCellScheduler::TBootstrap* bootstrap)
        : NRpc::TServiceBase(
            ~bootstrap->GetControlInvoker(),
            TSchedulerServiceProxy::GetServiceName(),
            SchedulerLogger.GetCategory())
        , Config(config)
        , Bootstrap(bootstrap)
        , CypressProxy(bootstrap->GetMasterChannel())
        , BackgroundQueue(New<TActionQueue>("Background"))
    {
        YASSERT(config);
        YASSERT(bootstrap);
        VERIFY_INVOKER_AFFINITY(GetControlInvoker(), ControlThread);

        RegisterMethod(RPC_SERVICE_METHOD_DESC(StartOperation));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(AbortOperation));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(WaitForOperation));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(Heartbeat));
    }

    void Start()
    {
        InitStrategy();
        RegisterAtMaster();
        StartRefresh();
        LoadOperations();
    }

    NYTree::TYPathServiceProducer CreateOrchidProducer()
    {
        // TODO(babenko): virtualize
        auto producer = BIND(&TImpl::BuildOrchidYson, MakeStrong(this));
        return BIND([=] () {
            return IYPathService::FromProducer(producer);
        });
    }

private:
    typedef TImpl TThis;

    DECLARE_THREAD_AFFINITY_SLOT(ControlThread);

    TSchedulerConfigPtr Config;
    NCellScheduler::TBootstrap* Bootstrap;
    NCypress::TCypressServiceProxy CypressProxy;
    TActionQueue::TPtr BackgroundQueue;

    TAutoPtr<ISchedulerStrategy> Strategy;

    NTransactionClient::ITransaction::TPtr BootstrapTransaction;

    TPeriodicInvoker::TPtr TransactionRefreshInvoker;
    TPeriodicInvoker::TPtr NodesRefreshInvoker;

    typedef yhash_map<Stroka, TExecNodePtr> TExecNodeMap;
    TExecNodeMap ExecNodes;

    typedef yhash_map<TOperationId, TOperationPtr> TOperationMap;
    TOperationMap Operations;

    typedef yhash_map<TJobId, TJobPtr> TJobMap;
    TJobMap Jobs;

    typedef TValueOrError<TOperationPtr> TStartResult;

    TFuture< TStartResult >::TPtr StartOperation(
        EOperationType type,
        const TTransactionId& transactionId,
        const NYTree::IMapNodePtr spec)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        // Create operation object.
        auto operationId = TOperationId::Create();
        auto operation = New<TOperation>(
            operationId,
            type,
            transactionId,
            spec,
            TInstant::Now());

        LOG_INFO("Starting operation %s (Type: %s, TransactionId: %s)",
            ~operationId.ToString(),
            ~type.ToString(),
            ~transactionId.ToString());

        // The operation owns the controller but not vice versa.
        // Hence we use raw pointers inside controllers.
        operation->SetController(CreateController(operation.Get()));

        operation->SetState(EOperationState::Initializing);
        try {
            InitializeOperation(operation);
        } catch (const std::exception& ex) {
            return MakeFuture(TStartResult(TError("Operation failed to start\n%s", ex.what())));
        }

        // Create a node in Cypress that will represent the operation.
        LOG_INFO("Creating operation node %s", ~operationId.ToString());
        auto setReq = TYPathProxy::Set(GetOperationPath(operationId));
        setReq->set_value(SerializeToYson(BIND(&TImpl::BuildOperationYson, MakeStrong(this), operation)));

        return CypressProxy.Execute(setReq)->Apply(
            BIND(
                &TImpl::OnOperationNodeCreated,
                MakeStrong(this),
                operation)
            .AsyncVia(GetControlInvoker()));
    }

    void InitializeOperation(TOperationPtr operation)
    {
        if (GetExecNodeCount() == 0) {
            ythrow yexception() << "No online exec nodes";
        }

        operation->GetController()->Initialize();
    }

    TValueOrError<TOperationPtr> OnOperationNodeCreated(
        TOperationPtr operation,
        NYTree::TYPathProxy::TRspSet::TPtr rsp)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto id = operation->GetOperationId();
        if (!rsp->IsOK()) {
            auto error = rsp->GetError();
            LOG_ERROR("Error creating operation node %s\n%s",
                ~id.ToString(),
                ~error.ToString());
            return error;
        }

        RegisterOperation(operation);
        LOG_INFO("Operation %s has started", ~id.ToString());

        PrepareOperation(operation);

        return operation;
    }

    void PrepareOperation(TOperationPtr operation)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        YASSERT(operation->GetState() == EOperationState::Initializing);
        operation->SetState(EOperationState::Preparing);

        // Run async preparation.
        LOG_INFO("Preparing operation %s", ~operation->GetOperationId().ToString());
        operation ->GetController()->Prepare()->Subscribe(
            BIND(&TImpl::OnOperationPrepared, MakeStrong(this), operation)
            .Via(GetControlInvoker()));
    }

    void OnOperationPrepared(TOperationPtr operation, TVoid)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (operation->GetState() != EOperationState::Preparing)
            return;

        operation->SetState(EOperationState::Running);

        LOG_INFO("Operation %s has been prepared and is now running", 
            ~operation->GetOperationId().ToString());

        // From this moment on the controller is fully responsible for the
        // operation's fate. It will eventually call #OnOperationCompleted or
        // #OnOperationFailed to inform the scheduler about the outcome.
    }


    void ReviveOperation(TOperationPtr operation)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        RegisterOperation(operation);

        YASSERT(operation->GetState() == EOperationState::Initializing);
        operation->SetState(EOperationState::Reviving);

        // Run async revival.
        LOG_INFO("Reviving operation %s", ~operation->GetOperationId().ToString());
        operation ->GetController()->Revive()->Subscribe(
            BIND(&TImpl::OnOperationRevived, MakeStrong(this), operation)
            .Via(GetControlInvoker()));
    }

    void OnOperationRevived(TOperationPtr operation, TVoid)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (operation->GetState() != EOperationState::Reviving)
            return;

        operation->SetState(EOperationState::Running);

        LOG_INFO("Operation %s has been revived and is now running", 
            ~operation->GetOperationId().ToString());
    }


    DECLARE_ENUM(EAbortReason,
        (TransactionExpired)
        (UserRequest)
    );

    void AbortOperation(TOperationPtr operation, EAbortReason reason)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto state = operation->GetState();
        if (state == EOperationState::Preparing || state == EOperationState::Running) {
            LOG_INFO("Aborting operation %s (State: %s, Reason: %s)",
                ~operation->GetOperationId().ToString(),
                ~state.ToString(),
                ~reason.ToString());
                
            operation->GetController()->OnOperationAborted();
            SetOperationFinished(
                operation,
                EOperationState::Aborted,
                TError("Operation aborted (Reason: %s)", ~reason.ToString()));
        }
    }


    TOperationPtr FindOperation(const TOperationId& id)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto it = Operations.find(id);
        return it == Operations.end() ? NULL : it->second;
    }

    TOperationPtr GetOperation(const TOperationId& id)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto operation = FindOperation(id);
        if (!operation) {
            // TODO(babenko): error code
            ythrow yexception() << Sprintf("No such operation %s", ~id.ToString());
        }
        return operation;
    }

    TExecNodePtr FindNode(const Stroka& address)
    {
        auto it = ExecNodes.find(address);
        return it == ExecNodes.end() ? NULL : it->second;
    }

    TJobPtr FindJob(const TJobId& jobId)
    {
        auto it = Jobs.find(jobId);
        return it == Jobs.end() ? NULL : it->second;
    }


    void RegisterNode(TExecNodePtr node)
    {
        YVERIFY(ExecNodes.insert(MakePair(node->GetAddress(), node)).second);    
    }

    void UnregisterNode(TExecNodePtr node)
    {
        YVERIFY(ExecNodes.erase(node->GetAddress()) == 1);
    }

    
    void RegisterOperation(TOperationPtr operation)
    {
        YVERIFY(Operations.insert(MakePair(operation->GetOperationId(), operation)).second);
        Strategy->OnOperationStarted(operation);

        LOG_DEBUG("Registered operation %s", ~operation->GetOperationId().ToString());
    }

    void UnregisterOperation(TOperationPtr operation)
    {
        // Take a copy, the collection will be modified.
        auto jobs = operation->Jobs();
        FOREACH (auto job, jobs) {
            UnregisterJob(job);
        }
        YVERIFY(Operations.erase(operation->GetOperationId()) == 1);
        Strategy->OnOperationFinished(operation);

        RemoveOperationNode(operation);

        LOG_DEBUG("Unregistered operation %s", ~operation->GetOperationId().ToString());
    }

    void SetOperationFinished(
        TOperationPtr operation,
        EOperationState state,
        const TError& error)
    {
        TOperationResult result;
        *result.mutable_error() = error.ToProto();
        operation->GetFinished()->Set(result);
        operation->SetState(state);
    }

    void RemoveOperationNode(TOperationPtr operation)
    {
        // Remove information about the operation from Cypress.
        auto id = operation->GetOperationId();
        LOG_INFO("Removing operation node %s", ~id.ToString());
        auto req = TYPathProxy::Remove(GetOperationPath(id));
        CypressProxy.Execute(req)->Subscribe(
            BIND(&TImpl::OnOperationNodeRemoved, MakeStrong(this), operation)
            .Via(GetControlInvoker()));
    }

    void OnOperationNodeRemoved(
        TOperationPtr operation,
        TYPathProxy::TRspRemove::TPtr rsp)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        // TODO(babenko): retry failed attempts
        if (!rsp->IsOK()) {
            LOG_WARNING("Error removing operation node %s\n%s",
                ~operation->GetOperationId().ToString(),
                ~rsp->GetError().ToString());
            return;
        }

        LOG_INFO("Operation node %s removed successfully",
            ~operation->GetOperationId().ToString());
    }

    
    void RegisterJob(TJobPtr job)
    {
        YVERIFY(Jobs.insert(MakePair(job->GetId(), job)).second);
        YVERIFY(job->GetOperation()->Jobs().insert(job).second);
        YVERIFY(job->GetNode()->Jobs().insert(job).second);
        LOG_DEBUG("Registered job %s (OperationId: %s)",
            ~job->GetId().ToString(),
            ~job->GetOperation()->GetOperationId().ToString());
    }

    void UnregisterJob(TJobPtr job)
    {
        YVERIFY(Jobs.erase(job->GetId()) == 1);
        YVERIFY(job->GetOperation()->Jobs().erase(job) == 1);
        YVERIFY(job->GetNode()->Jobs().erase(job) == 1);
        LOG_DEBUG("Unregistered job %s (OperationId: %s)",
            ~job->GetId().ToString(),
            ~job->GetOperation()->GetOperationId().ToString());
    }

    void OnJobRunning(TJobPtr job)
    {
        auto operation = job->GetOperation();
        if (operation->GetState() == EOperationState::Running) {
            operation->GetController()->OnJobRunning(job);
        }
    }

    void OnJobCompleted(TJobPtr job, const NProto::TJobResult& result)
    {
        job->Result() = result;
        auto operation = job->GetOperation();
        if (operation->GetState() == EOperationState::Running) {
            operation->GetController()->OnJobCompleted(job);
        }
        UnregisterJob(job);
    }

    void OnJobFailed(TJobPtr job, const NProto::TJobResult& result)
    {
        job->Result() = result;
        auto operation = job->GetOperation();
        if (operation->GetState() == EOperationState::Running) {
            operation->GetController()->OnJobFailed(job);
        }
        UnregisterJob(job);
    }

    void OnJobFailed(TJobPtr job, const TError& error)
    {
        NProto::TJobResult result;
        *result.mutable_error() = error.ToProto();
        OnJobFailed(job, result);
    }


    void InitStrategy()
    {
        Strategy = CreateStrategy(Config->Strategy);
    }

    TAutoPtr<ISchedulerStrategy> CreateStrategy(ESchedulerStrategy strategy)
    {
        switch (strategy) {
            case ESchedulerStrategy::Null:
                return CreateNullStrategy();
            case ESchedulerStrategy::Fifo:
                return CreateFifoStrategy();
            default:
                YUNREACHABLE();
        }
    }


    void RegisterAtMaster()
    {
        // TODO(babenko): Currently we use succeed-or-die strategy. Add retries later.

        // Take the lock to prevent multiple instances of scheduler from running simultaneously.
        // To this aim, we create an auxiliary transaction that takes care of this lock.
        // We never commit or commit this transaction, so it gets aborted (and the lock gets released)
        // when the scheduler dies.
        try {
            BootstrapTransaction = Bootstrap->GetTransactionManager()->Start();
        } catch (const std::exception& ex) {
            ythrow yexception() << Sprintf("Failed to start bootstrap transaction\n%s", ex.what());
        }

        LOG_INFO("Taking lock");
        {
            auto req = TCypressYPathProxy::Lock(WithTransaction(
                "/sys/scheduler/lock",
                BootstrapTransaction->GetId()));
            req->set_mode(ELockMode::Exclusive);
            auto rsp = CypressProxy.Execute(req)->Get();
            if (!rsp->IsOK()) {
                ythrow yexception() << Sprintf("Failed to take scheduler lock, check for another running scheduler instances\n%s",
                    ~rsp->GetError().ToString());
            }
        }
        LOG_INFO("Lock taken");

        LOG_INFO("Publishing scheduler address");
        {
            auto req = TYPathProxy::Set("/sys/scheduler/runtime@address");
            req->set_value(SerializeToYson(Bootstrap->GetPeerAddress()));
            auto rsp = CypressProxy.Execute(req)->Get();
            if (!rsp->IsOK()) {
                ythrow yexception() << Sprintf("Failed to publish scheduler address\n%s",
                    ~rsp->GetError().ToString());
            }
        }
        LOG_INFO("Scheduler address published");
    }

    void LoadOperations()
    {
        LOG_INFO("Requesting operations list");
        std::vector<TOperationId> operationIds;
        {
            auto req = TYPathProxy::List("/sys/operations");
            auto rsp = CypressProxy.Execute(req)->Get();
            if (!rsp->IsOK()) {
                ythrow yexception() << Sprintf("Failed to get operations list\n%s",
                    ~rsp->GetError().ToString());
            }
            LOG_INFO("Found %d operations", rsp->keys_size());
            FOREACH (const auto& key, rsp->keys()) {
                operationIds.push_back(TOperationId::FromString(key));
            }
        }

        LOG_INFO("Requesting operations info");
        {
            auto batchReq = CypressProxy.ExecuteBatch();
            FOREACH (const auto& operationId, operationIds) {
                auto req = TYPathProxy::Get(CombineYPaths(
                    "/sys/operations",
                    operationId.ToString(),
                    "@"));
                batchReq->AddRequest(req);
            }
            auto batchRsp = batchReq->Invoke()->Get();
            if (!batchRsp->IsOK()) {
                ythrow yexception() << Sprintf("Failed to get operations info\n%s",
                    ~batchRsp->GetError().ToString());
            }

            for (int index = 0; index < batchRsp->GetSize(); ++index) {
                auto rsp = batchRsp->GetResponse<TYPathProxy::TRspGet>(index);
                if (!rsp->IsOK()) {
                    ythrow yexception() << Sprintf("Failed to get operation info\n%s",
                        ~rsp->GetError().ToString());
                }

                auto operation = ParseOperationYson(operationIds[index], rsp->value());
                operation->SetController(CreateController(operation.Get()));
                Bootstrap->GetControlInvoker()->Invoke(BIND(
                    &TThis::ReviveOperation,
                    MakeStrong(this),
                    operation));
            }
        }
        LOG_INFO("Operations loaded successfully")
    }

    void StartRefresh()
    {
        TransactionRefreshInvoker = New<TPeriodicInvoker>(
            BIND(&TImpl::RefreshTransactions, MakeWeak(this))
            .Via(GetControlInvoker()),
            Config->TransactionsRefreshPeriod);
        TransactionRefreshInvoker->Start();

        NodesRefreshInvoker = New<TPeriodicInvoker>(
            BIND(&TImpl::RefreshExecNodes, MakeWeak(this))
            .Via(GetControlInvoker()),
            Config->NodesRefreshPeriod);
        NodesRefreshInvoker->Start();
    }


    void RefreshTransactions()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        // Check if any operations are running.
        if (Operations.empty())
            return;

        // Collect all transactions that are used by currently running operations.
        yhash_set<TTransactionId> transactionIds;
        FOREACH (const auto& pair, Operations) {
            transactionIds.insert(pair.second->GetTransactionId());
        }

        // Invoke GetId verbs for these transactions to see if they are alive.
        std::vector<TTransactionId> transactionIdsList;
        auto batchReq = CypressProxy.ExecuteBatch();
        FOREACH (const auto& id, transactionIds) {
            auto checkReq = TObjectYPathProxy::GetId(FromObjectId(id));
            transactionIdsList.push_back(id);
            batchReq->AddRequest(checkReq);
        }

        LOG_INFO("Refreshing %d transactions", batchReq->GetSize());
        batchReq->Invoke()->Subscribe(
            BIND(&TImpl::OnTransactionsRefreshed, MakeStrong(this), Passed(MoveRV(transactionIdsList)))
            .Via(GetControlInvoker()));
    }

    void OnTransactionsRefreshed(
        const std::vector<TTransactionId>& transactionIds,
        NCypress::TCypressServiceProxy::TRspExecuteBatch::TPtr rsp)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (!rsp->IsOK()) {
            LOG_ERROR("Error refreshing transactions\n%s", ~rsp->GetError().ToString());
            return;
        }

        LOG_INFO("Transactions refreshed successfully");

        // Collect the list of dead transactions.
        yhash_set<TTransactionId> deadTransactionIds;
        for (int index = 0; index < rsp->GetSize(); ++index) {
            if (!rsp->GetResponse(index)->IsOK()) {
                YVERIFY(deadTransactionIds.insert(transactionIds[index]).second);
            }
        }

        // Collect the list of operations corresponding to dead transactions.
        std::vector<TOperationPtr> deadOperations;
        FOREACH (const auto& pair, Operations) {
            auto operation = pair.second;
            if (deadTransactionIds.find(operation->GetTransactionId()) != deadTransactionIds.end()) {
                deadOperations.push_back(operation);
            }
        }

        // Abort dead operations.
        FOREACH (auto operation, deadOperations) {
            switch (operation->GetState()) {
                case EOperationState::Preparing:
                case EOperationState::Running:
                case EOperationState::Reviving:
                    LOG_INFO("Operation %s belongs to an expired transaction %s, aborting",
                        ~operation->GetOperationId().ToString(),
                        ~operation->GetTransactionId().ToString());
                    AbortOperation(operation, EAbortReason::TransactionExpired);
                    break;

                case EOperationState::Completed:
                case EOperationState::Aborted:
                case EOperationState::Failed:
                    LOG_INFO("Operation %s belongs to an expired transaction %s, sweeping",
                        ~operation->GetOperationId().ToString(),
                        ~operation->GetTransactionId().ToString());
                    break;

                default:
                    YUNREACHABLE();
            }
            UnregisterOperation(operation);
        }
    }


    void RefreshExecNodes()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        // Get the list of online nodes from the master.
        LOG_INFO("Refreshing exec nodes");
        auto req = TYPathProxy::Get("/sys/holders@online");
        CypressProxy.Execute(req)->Subscribe(
            BIND(&TImpl::OnExecNodesRefreshed, MakeStrong(this))
            .Via(GetControlInvoker()));
    }

    void OnExecNodesRefreshed(NYTree::TYPathProxy::TRspGet::TPtr rsp)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (!rsp->IsOK()) {
            LOG_ERROR("Error refreshing exec nodes\n%s", ~rsp->GetError().ToString());
            return;
        }

        auto onlineAddresses = DeserializeFromYson< yvector<Stroka> >(rsp->value());
        LOG_INFO("Exec nodes refreshed successfully, %d nodes found",
            static_cast<int>(onlineAddresses.size()));

        // Examine the list of nodes returned by master and figure out the difference.

        yhash_set<TExecNodePtr> deadNodes;
        FOREACH (const auto& pair, ExecNodes) {
            YVERIFY(deadNodes.insert(pair.second).second);
        }

        
        FOREACH (const auto& address, onlineAddresses) {
            auto node = FindNode(address);
            if (node) {
                YVERIFY(deadNodes.erase(node) == 1);
            } else {
                LOG_INFO("Node %s is online", ~address.Quote());
                auto node = New<TExecNode>(address);
                RegisterNode(node);
            }
        }

        FOREACH (auto node, deadNodes) {
            LOG_INFO("Node %s is offline", ~node->GetAddress().Quote());
            UnregisterNode(node);
        }
    }


    static NYTree::TYPath GetOperationPath(const TOperationId& id)
    {
        return CombineYPaths("/sys/operations", id.ToString());
    }

    IOperationControllerPtr CreateController(TOperation* operation)
    {
        switch (operation->GetType()) {
            case EOperationType::Map:
                return CreateMapController(this, operation);
                break;
            case EOperationType::Merge:
                return CreateMergeController(this, operation);
                break;
            default:
                YUNREACHABLE();
        }
    }
    

    // IOperationHost methods
    virtual NRpc::IChannel::TPtr GetMasterChannel()
    {
        return Bootstrap->GetMasterChannel();
    }

    virtual TTransactionManager::TPtr GetTransactionManager()
    {
        return Bootstrap->GetTransactionManager();
    }

    virtual IInvoker::TPtr GetControlInvoker()
    {
        return Bootstrap->GetControlInvoker();
    }

    virtual IInvoker::TPtr GetBackgroundInvoker()
    {
        return BackgroundQueue->GetInvoker();
    }

    virtual int GetExecNodeCount()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        return static_cast<int>(ExecNodes.size());
    }

    virtual TJobPtr CreateJob(
        TOperationPtr operation,
        TExecNodePtr node,
        const NProto::TJobSpec& spec)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        // The job does not get registered immediately.
        // Instead we wait until this job is returned back to us by the strategy.
        auto job = New<TJob>(
            TJobId::Create(),
            operation.Get(),
            node,
            spec,
            TInstant::Now());
        return job;
    }

    virtual void OnOperationCompleted(
        TOperationPtr operation)
    {
        VERIFY_THREAD_AFFINITY_ANY();
        GetControlInvoker()->Invoke(BIND(
            &TImpl::DoOperationCompleted,
            MakeStrong(this),
            operation));
    }

    void DoOperationCompleted(TOperationPtr operation)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto state = operation->GetState();
        if (state != EOperationState::Preparing && state != EOperationState::Running) {
            // Operation is being aborted.
            return;
        }

        LOG_INFO("Operation %s has completed",
            ~operation->GetOperationId().ToString());

        SetOperationFinished(operation, EOperationState::Completed, TError());

        // The operation will remain in this state until it is swept.
    }

    virtual void OnOperationFailed(
        TOperationPtr operation,
        const TError& error)
    {
        VERIFY_THREAD_AFFINITY_ANY();
        GetControlInvoker()->Invoke(BIND(
            &TImpl::DoOperationFailed,
            MakeStrong(this),
            operation,
            error));
    }

    void DoOperationFailed(TOperationPtr operation, const TError& error)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto state = operation->GetState();
        if (state != EOperationState::Preparing && state != EOperationState::Running) {
            // Safe to call OnOperationFailed multiple times, just ignore it.
            return;
        }

        LOG_INFO("Operation %s has failed\n%s",
            ~operation->GetOperationId().ToString(),
            ~error.GetMessage());

        SetOperationFinished(operation, EOperationState::Failed, error);

        // The operation will remain in this state until it is swept.
    }



    void BuildOrchidYson(IYsonConsumer* consumer)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        BuildYsonFluently(consumer)
            .BeginMap()
                .Item("operations").DoMapFor(Operations, [=] (TFluentMap fluent, TOperationMap::value_type pair) {
                    fluent.Item(pair.first.ToString());
                    BuildOperationYson(pair.second, consumer);
                })
                .Item("jobs").DoMapFor(Jobs, [=] (TFluentMap fluent, TJobMap::value_type pair) {
                    fluent.Item(pair.first.ToString());
                    BuildJobYson(pair.second, consumer);
                })
                .Item("exec_nodes").DoMapFor(ExecNodes, [=] (TFluentMap fluent, TExecNodeMap::value_type pair) {
                    fluent.Item(pair.first);
                    BuildExecNodeYson(pair.second, consumer);
                })
            .EndMap();
    }

    void BuildOperationYson(TOperationPtr operation, IYsonConsumer* consumer)
    {
        BuildYsonFluently(consumer)
            .WithAttributes().BeginMap()
            .EndMap()
            .BeginAttributes()
                .Item("operation_type").Scalar(CamelCaseToUnderscoreCase(operation->GetType().ToString()))
                .Item("transaction_id").Scalar(operation->GetTransactionId())
                .Item("spec").Node(operation->GetSpec())
                // TODO(babenko): serialize start time
            .EndAttributes();
    }

    TOperationPtr ParseOperationYson(const TOperationId& operationId, const TYson& yson)
    {
        // TODO(babenko): simplify
        auto node = DeserializeFromYson(yson)->AsMap();
        auto attributes = CreateEphemeralAttributes();
        attributes->MergeFrom(node);

        return New<TOperation>(
            operationId,
            attributes->Get<EOperationType>("operation_type"),
            attributes->Get<TTransactionId>("transaction_id"),
            attributes->Get<INode>("spec")->AsMap(),
            // TODO(babenko): parse start time
            TInstant::Now());
    }

    void BuildJobYson(TJobPtr job, IYsonConsumer* consumer)
    {
        BuildYsonFluently(consumer)
            .WithAttributes().BeginMap()
            .EndMap()
            .BeginAttributes()
                .Item("type").Scalar(CamelCaseToUnderscoreCase(EJobType(job->Spec().type()).ToString()))
                .Item("state").Scalar(CamelCaseToUnderscoreCase(job->GetState().ToString()))
                //.DoIf(!job->Result().IsOK(), [=] (TFluentMap fluent) {
                //    auto error = TError::FromProto(job->Result().error());
                //    fluent.Item("result").BeginMap()
                //        .Item("code").Scalar(error.GetCode())
                //        .Item("message").Scalar(error.GetMessage())
                //    .EndMap();
                //})
            .EndAttributes();
    }

    void BuildExecNodeYson(TExecNodePtr node, IYsonConsumer* consumer)
    {
        BuildYsonFluently(consumer)
            .BeginMap()
                .Item("utilization").BeginMap()
                    .Item("total_slot_count").Scalar(node->Utilization().total_slot_count())
                    .Item("free_slot_count").Scalar(node->Utilization().free_slot_count())
                .EndMap()
                .Item("job_count").Scalar(static_cast<int>(node->Jobs().size()))
            .EndMap();
    }


    // RPC handlers
    DECLARE_RPC_SERVICE_METHOD(NProto, StartOperation)
    {
        auto type = EOperationType(request->type());
        auto transactionId = TTransactionId::FromProto(request->transaction_id());

        IMapNodePtr spec;
        try {
            spec = DeserializeFromYson(request->spec())->AsMap();
        } catch (const std::exception& ex) {
            ythrow yexception() << Sprintf("Error parsing operation spec\n%s", ex.what());
        }
        
        context->SetRequestInfo("Type: %s, TransactionId: %s",
            ~type.ToString(),
            ~transactionId.ToString());

        StartOperation(
            type,
            transactionId,
            spec)
        ->Subscribe(BIND([=] (TValueOrError<TOperationPtr> result) {
            if (!result.IsOK()) {
                context->Reply(result);
                return;
            }
            auto operation = result.Value();
            auto id = operation->GetOperationId();
            *response->mutable_operation_id() = id.ToProto();
            context->SetResponseInfo("OperationId: %s", ~id.ToString());
            context->Reply();
        }));
    }

    DECLARE_RPC_SERVICE_METHOD(NProto, AbortOperation)
    {
        auto operationId = TTransactionId::FromProto(request->operation_id());

        context->SetRequestInfo("OperationId: %s", ~operationId.ToString());

        auto operation = GetOperation(operationId);
        AbortOperation(operation, EAbortReason::UserRequest);
    }

    DECLARE_RPC_SERVICE_METHOD(NProto, WaitForOperation)
    {
        auto operationId = TTransactionId::FromProto(request->operation_id());

        context->SetRequestInfo("OperationId: %s", ~operationId.ToString());

        auto operation = GetOperation(operationId);
        // TODO(babenko): const&
        operation->GetFinished()->Subscribe(BIND([=] (TOperationResult result) {
            *response->mutable_result() = result;
            context->Reply();
        }));
    }

    DECLARE_RPC_SERVICE_METHOD(NProto, Heartbeat)
    {
        auto address = request->address();
        auto utilization = request->utilization();

        context->SetRequestInfo("Address: %s, JobCount: %d, TotalSlotCount: %d, FreeSlotCount: %d",
            ~address,
            request->jobs_size(),
            utilization.total_slot_count(),
            utilization.free_slot_count());

        auto node = FindNode(address);
        if (!node) {
            // TODO(babenko): error code
            context->Reply(TError("Node is not registered, heartbeat ignored"));
            return;
        }

        node->Utilization() = utilization;

        auto missingJobs = node->Jobs();

        PROFILE_TIMING ("analysis_time") {
            FOREACH (const auto& jobStatus, request->jobs()) {
                auto jobId = TJobId::FromProto(jobStatus.job_id());
                auto state = EJobState(jobStatus.state());
            
                NLog::TTaggedLogger Logger(SchedulerLogger);
                Logger.AddTag(Sprintf("Address: %s, JobId: %s",
                    ~address,
                    ~jobId.ToString()));

                auto job = FindJob(jobId);

                auto operation = job ? job->GetOperation() : NULL;
                if (operation) {
                    Logger.AddTag(Sprintf("OperationId: %s", ~operation->GetOperationId().ToString()));
                }

                if (job) {
                    // Check if the job is running on a proper node.
                    auto expectedAddress = job->GetNode()->GetAddress();
                    if (address != expectedAddress) {
                        // Job has moved from one node to another. No idea how this could happen.
                        if (state == EJobState::Completed || state == EJobState::Failed) {
                            *response->add_jobs_to_remove() = jobId.ToProto();
                            LOG_WARNING("Job status report was expected from %s, removal scheduled",
                                ~expectedAddress);
                        } else {
                            *response->add_jobs_to_remove() = jobId.ToProto();
                            LOG_WARNING("Job status report was expected from %s, abort scheduled",
                                ~expectedAddress);
                        }
                        continue;
                    }

                    // Mark the job as no longer missing.
                    YVERIFY(missingJobs.erase(job) == 1);

                    job->SetState(state);
                }

                switch (state) {
                    case EJobState::Completed:
                        if (job) {
                            LOG_INFO("Job completed, removal scheduled");
                            OnJobCompleted(job, jobStatus.result());
                        } else {
                            LOG_WARNING("Unknown job has completed, removal scheduled");
                        }
                        *response->add_jobs_to_remove() = jobId.ToProto();
                        break;

                    case EJobState::Failed:
                        if (job) {
                            LOG_INFO("Job failed, removal scheduled");
                            OnJobFailed(job, jobStatus.result());
                        } else {
                            LOG_INFO("Unknown job has failed, removal scheduled");
                        }
                        *response->add_jobs_to_remove() = jobId.ToProto();
                        break;

                    case EJobState::Aborted:
                        if (job) {
                            LOG_WARNING("Job has aborted unexpectedly, removal scheduled");
                            OnJobFailed(job, TError("Job has aborted unexpectedly"));
                        } else {
                            LOG_INFO("Job aborted, removal scheduled");
                        }
                        *response->add_jobs_to_remove() = jobId.ToProto();
                        break;

                    case EJobState::Running:
                        if (job) {
                            LOG_DEBUG("Job is running");
                            OnJobRunning(job);
                        } else {
                            LOG_WARNING("Unknown job is running, abort scheduled");
                            *response->add_jobs_to_abort() = jobId.ToProto();
                        }
                        break;

                    case EJobState::Aborting:
                        if (job) {
                            LOG_WARNING("Job has started aborting unexpectedly");
                            OnJobFailed(job, TError("Job has aborted unexpectedly"));
                        } else {
                            LOG_DEBUG("Job is aborting");
                        }
                        break;

                    default:
                        YUNREACHABLE();
                }
            }

            // Check for missing jobs.
            FOREACH (auto job, missingJobs) {
                LOG_ERROR("Job is missing (Address: %s, JobId: %s, OperationId: %s)",
                    ~address,
                    ~job->GetId().ToString(),
                    ~job->GetOperation()->GetOperationId().ToString());
                OnJobFailed(job, TError("Job has vanished"));
            }
        }

        std::vector<TJobPtr> jobsToStart;
        std::vector<TJobPtr> jobsToAbort;
        PROFILE_TIMING ("schedule_time") {
            Strategy->ScheduleJobs(node, &jobsToStart, &jobsToAbort);
        }

        FOREACH (auto job, jobsToStart) {
            LOG_INFO("Scheduling job start on %s (JobType: %s, JobId: %s, OperationId: %s)",
                ~address,
                ~EJobType(job->Spec().type()).ToString(),
                ~job->GetId().ToString(),
                ~job->GetOperation()->GetOperationId().ToString());
            auto* jobInfo = response->add_jobs_to_start();
            *jobInfo->mutable_job_id() = job->GetId().ToProto();
            *jobInfo->mutable_spec() = job->Spec();
            RegisterJob(job);
        }

        FOREACH (auto job, jobsToAbort) {
            LOG_INFO("Scheduling job abort on %s (JobId: %s, OperationId: %s)",
                ~address,
                ~job->GetId().ToString(),
                ~job->GetOperation()->GetOperationId().ToString());
            *response->add_jobs_to_remove() = job->GetId().ToProto();
            UnregisterJob(job);
        }

        context->Reply();
    }

};

TScheduler::TScheduler(
    TSchedulerConfigPtr config,
    TBootstrap* bootstrap)
    : Impl(New<TImpl>(config, bootstrap))
{ }

void TScheduler::Start()
{
    Impl->Start();
}

NRpc::IService::TPtr TScheduler::GetService()
{
    return Impl;
}

NYTree::TYPathServiceProducer TScheduler::CreateOrchidProducer()
{
    return Impl->CreateOrchidProducer();
}

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

