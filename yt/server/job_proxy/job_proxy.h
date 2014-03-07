#pragma once

#include "public.h"
#include "private.h"
#include "pipes.h"
#include "job.h"

#include <core/concurrency/periodic_executor.h>

#include <core/logging/tagged_logger.h>

#include <server/job_agent/public.h>

#include <server/exec_agent/supervisor_service_proxy.h>

namespace NYT {
namespace NJobProxy {

////////////////////////////////////////////////////////////////////////////////

class TJobProxy
    : public IJobHost
{
public:
    TJobProxy(
        TJobProxyConfigPtr config,
        const NJobAgent::TJobId& jobId);

    //! Runs the job. Blocks until the job is complete.
    void Run();

private:
    NJobTrackerClient::NProto::TJobResult DoRun();
    void SendHeartbeat();
    void OnHeartbeatResponse(NExecAgent::TSupervisorServiceProxy::TRspOnJobProgressPtr rsp);

    void RetrieveJobSpec();
    void ReportResult(const NJobTrackerClient::NProto::TJobResult& result);

    TJobProxyConfigPtr Config;
    NJobAgent::TJobId JobId;

    NLog::TTaggedLogger Logger;

    std::unique_ptr<NExecAgent::TSupervisorServiceProxy> SupervisorProxy;

    NRpc::IChannelPtr MasterChannel;

    NChunkClient::IBlockCachePtr BlockCache;
    NNodeTrackerClient::TNodeDirectoryPtr NodeDirectory;

    TJobPtr Job;

    volatile i64 JobProxyMemoryLimit;

    NConcurrency::TPeriodicExecutorPtr HeartbeatExecutor;
    NConcurrency::TPeriodicExecutorPtr MemoryWatchdogExecutor;

    NJobTrackerClient::NProto::TJobSpec JobSpec;
    NNodeTrackerClient::NProto::TNodeResources ResourceUsage;

    // IJobHost implementation.
    virtual TJobProxyConfigPtr GetConfig() override;
    virtual const NJobTrackerClient::NProto::TJobSpec& GetJobSpec() const override;

    virtual const NNodeTrackerClient::NProto::TNodeResources& GetResourceUsage() const override;
    virtual void SetResourceUsage(const NNodeTrackerClient::NProto::TNodeResources& usage) override;
    void OnResourcesUpdated(NExecAgent::TSupervisorServiceProxy::TRspUpdateResourceUsagePtr rsp);

    virtual void ReleaseNetwork() override;

    virtual NRpc::IChannelPtr GetMasterChannel() const override;

    virtual NChunkClient::IBlockCachePtr GetBlockCache() const override;
    virtual NNodeTrackerClient::TNodeDirectoryPtr GetNodeDirectory() const override;

    void CheckMemoryUsage();

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
