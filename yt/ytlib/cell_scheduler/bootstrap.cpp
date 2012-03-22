#include "stdafx.h"
#include "bootstrap.h"
#include "config.h"

#include <ytlib/misc/ref_counted_tracker.h>

#include <ytlib/actions/action_queue.h>

#include <ytlib/bus/nl_server.h>

#include <ytlib/election/leader_channel.h>

#include <ytlib/orchid/orchid_service.h>

#include <ytlib/monitoring/monitoring_manager.h>
#include <ytlib/monitoring/ytree_integration.h>
#include <ytlib/monitoring/http_server.h>
#include <ytlib/monitoring/http_integration.h>

#include <ytlib/ytree/virtual.h>
#include <ytlib/ytree/yson_file_service.h>

#include <ytlib/profiling/profiling_manager.h>

#include <ytlib/scheduler/scheduler.h>
#include <ytlib/scheduler/config.h>

namespace NYT {
namespace NCellScheduler {

using namespace NRpc;
using namespace NBus;
using namespace NYTree;
using namespace NMonitoring;
using namespace NOrchid;
using namespace NProfiling;
using namespace NCypress;
using namespace NScheduler;
using namespace NTransactionClient;

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger Logger("SchedulerBootstrap");

////////////////////////////////////////////////////////////////////////////////

TBootstrap::TBootstrap(
    const Stroka& configFileName,
    TCellSchedulerConfigPtr config)
    : ConfigFileName(configFileName)
    , Config(config)
{ }

TBootstrap::~TBootstrap()
{ }

void TBootstrap::Run()
{
    PeerAddress = Sprintf("%s:%d", ~HostName(), Config->RpcPort);

    LOG_INFO("Starting scheduler (PeerAddress: %s, MasterAddresses: [%s])",
        ~PeerAddress,
        ~JoinToString(Config->Masters->Addresses));

    MasterChannel = CreateLeaderChannel(~Config->Masters);

    auto controlQueue = New<TActionQueue>("Control");
    ControlInvoker = controlQueue->GetInvoker();

    BusServer = CreateNLBusServer(~New<TNLBusServerConfig>(Config->RpcPort));

    auto rpcServer = CreateRpcServer(~BusServer);

    TransactionManager = New<TTransactionManager>(
        Config->TransactionManager,
        MasterChannel);

    Scheduler = New<TScheduler>(Config->Scheduler, this);

    auto monitoringManager = New<TMonitoringManager>();
    monitoringManager->Register(
        "ref_counted",
        FromMethod(&TRefCountedTracker::GetMonitoringInfo, TRefCountedTracker::Get()));
    monitoringManager->Register(
        "bus_server",
        FromMethod(&IBusServer::GetMonitoringInfo, BusServer));
    monitoringManager->Start();

    auto orchidFactory = NYTree::GetEphemeralNodeFactory();
    auto orchidRoot = orchidFactory->CreateMap();
    SyncYPathSetNode(
        ~orchidRoot,
        "monitoring",
        ~NYTree::CreateVirtualNode(~CreateMonitoringProducer(~monitoringManager)));
    SyncYPathSetNode(
        ~orchidRoot,
        "profiling",
        ~CreateVirtualNode(
            ~TProfilingManager::Get()->GetRoot()
            ->Via(TProfilingManager::Get()->GetInvoker())));
    SyncYPathSetNode(
        ~orchidRoot,
        "config",
        ~NYTree::CreateVirtualNode(~NYTree::CreateYsonFileProducer(ConfigFileName)));
    SyncYPathSetNode(
        ~orchidRoot,
        "scheduler",
        ~NYTree::CreateVirtualNode(~Scheduler->CreateOrchidProducer()));

    auto orchidService = New<TOrchidService>(
        ~orchidRoot,
        controlQueue->GetInvoker());
    rpcServer->RegisterService(~orchidService);

    ::THolder<NHttp::TServer> httpServer(new NHttp::TServer(Config->MonitoringPort));
    httpServer->Register(
        "/orchid",
        ~NMonitoring::GetYPathHttpHandler(~orchidRoot->Via(controlQueue->GetInvoker())));

    rpcServer->RegisterService(Scheduler->GetService());

    LOG_INFO("Listening for HTTP requests on port %d", Config->MonitoringPort);
    httpServer->Start();

    LOG_INFO("Listening for RPC requests on port %d", Config->RpcPort);
    rpcServer->Start();

    Scheduler->Start();

    Sleep(TDuration::Max());
}

TCellSchedulerConfigPtr TBootstrap::GetConfig() const
{
    return Config;
}

IChannel::TPtr TBootstrap::GetMasterChannel() const
{
    return MasterChannel;
}

Stroka TBootstrap::GetPeerAddress() const
{
    return PeerAddress;
}

IInvoker::TPtr TBootstrap::GetControlInvoker() const
{
    return ControlInvoker;
}

TTransactionManager::TPtr TBootstrap::GetTransactionManager() const
{
    return TransactionManager;
}

TSchedulerPtr TBootstrap::GetScheduler() const
{
    return Scheduler;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NCellScheduler
} // namespace NYT
