#include "stdafx.h"

#include <core/misc/crash_handler.h>
#include <core/misc/address.h>
#include <core/misc/proc.h>

#include <core/build.h>

#include <core/logging/log_manager.h>

#include <core/profiling/profiling_manager.h>

#include <core/tracing/trace_manager.h>

#include <core/ytree/yson_serializable.h>

#include <ytlib/shutdown.h>

#include <ytlib/misc/tclap_helpers.h>

#include <ytlib/scheduler/config.h>

#include <ytlib/chunk_client/dispatcher.h>

#include <server/data_node/config.h>

#include <server/cell_master/bootstrap.h>
#include <server/cell_master/config.h>
#include <server/cell_node/bootstrap.h>
#include <server/cell_node/config.h>
#include <server/cell_node/bootstrap.h>

#include <server/cell_scheduler/config.h>
#include <server/cell_scheduler/bootstrap.h>

#include <server/job_proxy/config.h>
#include <server/job_proxy/job_proxy.h>

#include <tclap/CmdLine.h>

#include <util/system/sigset.h>
#include <util/system/execpath.h>
#include <util/folder/dirut.h>

#include <contrib/tclap/tclap/CmdLine.h>

namespace NYT {

using namespace NYTree;
using namespace NYson;
using namespace NElection;
using namespace NScheduler;
using namespace NJobProxy;

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger Logger("Server");

////////////////////////////////////////////////////////////////////////////////

DECLARE_ENUM(EExitCode,
    ((OK)(0))
    ((OptionsError)(1))
    ((BootstrapError)(2))
);

////////////////////////////////////////////////////////////////////////////////

struct TArgsParser
{
public:
    TArgsParser()
        : CmdLine("Command line", ' ', GetVersion())
        , CellNode("", "node", "start cell node")
        , CellMaster("", "master", "start cell master")
        , Scheduler("", "scheduler", "start scheduler")
        , JobProxy("", "job-proxy", "start job proxy")
        , Cleaner("", "cleaner", "start cleaner")
        , Killer("", "killer", "start killer")
        , CloseAllFds("", "close-all-fds", "close all file descriptors")
        , DirToRemove("", "dir-to-remove", "directory to remove (for cleaner mode)", false, "", "DIR")
        , Uid("", "uid", "uid of processes to kill (for killer mode)", false, -1, "UID")
        , JobId("", "job-id", "job id (for job proxy mode)", false, "", "ID")
        , WorkingDirectory("", "working-dir", "working directory", false, "", "DIR")
        , Config("", "config", "configuration file", false, "", "FILE")
        , ConfigTemplate("", "config-template", "print configuration file template")
    {
        CmdLine.add(CellNode);
        CmdLine.add(CellMaster);
        CmdLine.add(Scheduler);
        CmdLine.add(JobProxy);
        CmdLine.add(Cleaner);
        CmdLine.add(Killer);
        CmdLine.add(CloseAllFds);
        CmdLine.add(DirToRemove);
        CmdLine.add(Uid);
        CmdLine.add(JobId);
        CmdLine.add(WorkingDirectory);
        CmdLine.add(Config);
        CmdLine.add(ConfigTemplate);
    }

    TCLAP::CmdLine CmdLine;

    TCLAP::SwitchArg CellNode;
    TCLAP::SwitchArg CellMaster;
    TCLAP::SwitchArg Scheduler;
    TCLAP::SwitchArg JobProxy;
    TCLAP::SwitchArg Cleaner;
    TCLAP::SwitchArg Killer;
    TCLAP::SwitchArg CloseAllFds;

    TCLAP::ValueArg<Stroka> DirToRemove;
    TCLAP::ValueArg<int> Uid;
    TCLAP::ValueArg<Stroka> JobId;
    TCLAP::ValueArg<Stroka> WorkingDirectory;
    TCLAP::ValueArg<Stroka> Config;
    TCLAP::SwitchArg ConfigTemplate;
};

////////////////////////////////////////////////////////////////////////////////

EExitCode GuardedMain(int argc, const char* argv[])
{
    NYT::NConcurrency::SetCurrentThreadName("Bootstrap");

    TArgsParser parser;

    parser.CmdLine.parse(argc, argv);

    // Figure out the mode: cell master, cell node, scheduler or job proxy.
    bool isCellMaster = parser.CellMaster.getValue();
    bool isCellNode = parser.CellNode.getValue();
    bool isScheduler = parser.Scheduler.getValue();
    bool isJobProxy = parser.JobProxy.getValue();
    bool isCleaner = parser.Cleaner.getValue();
    bool isKiller = parser.Killer.getValue();

    bool doCloseAllFds = parser.CloseAllFds.getValue();

    bool printConfigTemplate = parser.ConfigTemplate.getValue();

    Stroka configFileName = parser.Config.getValue();

    Stroka workingDirectory = parser.WorkingDirectory.getValue();

    int modeCount = 0;
    if (isCellNode) {
        ++modeCount;
    }
    if (isCellMaster) {
        ++modeCount;
    }
    if (isScheduler) {
        ++modeCount;
    }
    if (isJobProxy) {
        ++modeCount;
    }

    if (isCleaner) {
        ++modeCount;
    }

    if (isKiller) {
        ++modeCount;
    }

    if (modeCount != 1) {
        TCLAP::StdOutput().usage(parser.CmdLine);
        return EExitCode::OptionsError;
    }

    if (doCloseAllFds) {
        CloseAllDescriptors();
    }

    if (!workingDirectory.empty()) {
        ChDir(workingDirectory);
    }

    if (isCleaner) {
        Stroka path = parser.DirToRemove.getValue();
        if (path.empty() || path[0] != '/') {
            THROW_ERROR_EXCEPTION("A path should be absolute. Path: %s", ~path);
        }
        int counter = 0;
        size_t nextSlash = 0;
        while (nextSlash != Stroka::npos) {
            nextSlash = path.find('/', nextSlash + 1);
            ++counter;
        }

        if (counter <= 3) {
            THROW_ERROR_EXCEPTION("A path should contain at least 4 slashes. Path: %s", ~path);
        }

        RemoveDirAsRoot(path);

        return EExitCode::OK;
    }

    if (isKiller) {
        int uid = parser.Uid.getValue();
        KillallByUid(uid);

        return EExitCode::OK;
    }

    INodePtr configNode;

    if (!printConfigTemplate) {
        if (configFileName.empty()) {
            THROW_ERROR_EXCEPTION("Missing --config option");
        }

        // Parse configuration file.
        try {
            TIFStream configStream(configFileName);
            configNode = ConvertToNode(&configStream);
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error reading server configuration")
                << ex;
        }

        // Deserialize as a generic server config.
        auto config = New<TServerConfig>();
        config->Load(configNode);

        // Configure singletons.
        NLog::TLogManager::Get()->Configure(configFileName, "/logging");
        TAddressResolver::Get()->Configure(config->AddressResolver);
        NChunkClient::TDispatcher::Get()->Configure(config->ChunkClientDispatcher);
        NTracing::TTraceManager::Get()->Configure(configFileName, "/tracing");
        NProfiling::TProfilingManager::Get()->Start();
    }

    // Start an appropriate server.
    if (isCellNode) {
        NConcurrency::SetCurrentThreadName("NodeMain");

        auto config = New<NCellNode::TCellNodeConfig>();
        if (printConfigTemplate) {
            TYsonWriter writer(&Cout, EYsonFormat::Pretty);
            config->Save(&writer);
            return EExitCode::OK;
        }

        try {
            config->Load(configNode);
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error parsing cell node configuration")
                << ex;
        }

        // TODO(babenko): This memory leak is intentional.
        // We should avoid destroying bootstrap since some of the subsystems
        // may be holding a reference to it and continue running some actions in background threads.
        auto* bootstrap = new NCellNode::TBootstrap(configFileName, config);
        bootstrap->Run();
    }

    if (isCellMaster) {
        NConcurrency::SetCurrentThreadName("MasterMain");

        auto config = New<NCellMaster::TCellMasterConfig>();
        if (printConfigTemplate) {
            TYsonWriter writer(&Cout, EYsonFormat::Pretty);
            config->Save(&writer);
            return EExitCode::OK;
        }

        try {
            config->Load(configNode);
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error parsing cell master configuration")
                << ex;
        }

        // TODO(babenko): This memory leak is intentional.
        // We should avoid destroying bootstrap since some of the subsystems
        // may be holding a reference to it and continue running some actions in background threads.
        auto* bootstrap = new NCellMaster::TBootstrap(configFileName, config);
        bootstrap->Run();
    }

    if (isScheduler) {
        NConcurrency::SetCurrentThreadName("SchedulerMain");

        auto config = New<NCellScheduler::TCellSchedulerConfig>();
        if (printConfigTemplate) {
            TYsonWriter writer(&Cout, EYsonFormat::Pretty);
            config->Save(&writer);
            return EExitCode::OK;
        }

        try {
            config->Load(configNode);
            config->Validate();
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error parsing cell scheduler configuration")
                << ex;
        }

        // TODO(babenko): This memory leak is intentional.
        // We should avoid destroying bootstrap since some of the subsystems
        // may be holding a reference to it and continue running some actions in background threads.
        auto* bootstrap = new NCellScheduler::TBootstrap(configFileName, config);
        bootstrap->Run();
    }

    if (isJobProxy) {
        NConcurrency::SetCurrentThreadName("JobProxyMain");

        auto config = New<NJobProxy::TJobProxyConfig>();
        if (printConfigTemplate) {
            TYsonWriter writer(&Cout, EYsonFormat::Pretty);
            config->Save(&writer);
            return EExitCode::OK;
        }

        TJobId jobId;
        try {
            jobId = TGuid::FromString(parser.JobId.getValue());
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error parsing job id")
                << ex;
        }

        try {
            config->Load(configNode);
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error parsing job proxy configuration")
                << ex;
        }

        auto jobProxy = New<TJobProxy>(config, jobId);
        jobProxy->Run();
    }

    return EExitCode::OK;
}

int Main(int argc, const char* argv[])
{
    InstallCrashSignalHandler();

    // If you ever try to remove this I will kill you. I promise. /@babenko
    GetExecPath();

#ifdef _unix_
    sigset_t sigset;
    SigEmptySet(&sigset);
    SigAddSet(&sigset, SIGHUP);
    SigProcMask(SIG_BLOCK, &sigset, NULL);

    signal(SIGPIPE, SIG_IGN);

#ifndef _darwin_
    uid_t ruid, euid, suid;
    YCHECK(getresuid(&ruid, &euid, &suid) == 0);
    if (euid == 0) {
        // if effective uid == 0 (e. g. set-uid-root), make
        // saved = effective, effective = real
        YCHECK(setresuid(ruid, ruid, euid) == 0);
    }
#endif /* ! _darwin_ */
#endif /* _unix_ */

    int exitCode;
    try {
        exitCode = GuardedMain(argc, argv);
    } catch (const std::exception& ex) {
        LOG_ERROR(ex, "Server startup failed");
        exitCode = EExitCode::BootstrapError;
    }

    Shutdown();

    return exitCode;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

int main(int argc, const char* argv[])
{
    return NYT::Main(argc, argv);
}
