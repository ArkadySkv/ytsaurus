#include "stdafx.h"
#include "unsafe_environment.h"
#include "environment.h"
#include "private.h"

#include <server/exec_agent/slot.h>

#include <core/concurrency/thread_affinity.h>

#include <core/misc/process.h>

#include <core/logging/tagged_logger.h>

#include <server/job_proxy/public.h>

#include <util/system/execpath.h>

#include <fcntl.h>

#ifndef _win_

#include <sys/types.h>
#include <sys/wait.h>

#endif

namespace NYT {
namespace NExecAgent {

using namespace NJobProxy;
using namespace NCGroup;

////////////////////////////////////////////////////////////////////////////////

static auto& Logger = ExecAgentLogger;

////////////////////////////////////////////////////////////////////////////////

class TUnsafeEnvironmentBuilder
    : public IEnvironmentBuilder
{
public:
    TUnsafeEnvironmentBuilder()
        : ProxyPath(GetExecPath())
    { }

    IProxyControllerPtr CreateProxyController(
        NYTree::INodePtr config,
        const TJobId& jobId,
        const TSlot& slot,
        const Stroka& workingDirectory) override;

private:
    friend class TUnsafeProxyController;

    Stroka ProxyPath;
};

////////////////////////////////////////////////////////////////////////////////

#ifndef _win_

class TUnsafeProxyController
    : public IProxyController
{
public:
    TUnsafeProxyController(
        const Stroka& proxyPath,
        const TJobId& jobId,
        const TSlot& slot,
        const Stroka& workingDirectory,
        TUnsafeEnvironmentBuilder* envBuilder)
        : ProxyPath(proxyPath)
        , WorkingDirectory(workingDirectory)
        , JobId(jobId)
        , Slot(slot)
        , Logger(ExecAgentLogger)
        , Process(proxyPath)
        , Waited(false)
        , EnvironmentBuilder(envBuilder)
        , OnExit(NewPromise<TError>())
        , ControllerThread(ThreadFunc, this)
    {
        Logger.AddTag(Sprintf("JobId: %s", ~ToString(jobId)));
    }

    virtual TAsyncError Run() override
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        LOG_INFO("Starting job proxy in unsafe environment (WorkDir: %s)",
            ~WorkingDirectory);

        Process.AddArgument("--job-proxy");
        Process.AddArgument("--config");
        Process.AddArgument(ProxyConfigFileName);
        Process.AddArgument("--job-id");
        Process.AddArgument(ToString(JobId));
        Process.AddArgument("--working-dir");
        Process.AddArgument(WorkingDirectory);
        Process.AddArgument("--close-all-fds");
        for (const auto& path : Slot.GetCGroupPaths()) {
            Process.AddArgument("--cgroup");
            Process.AddArgument(path);
        }

        LOG_INFO("Spawning a job proxy (Path: %s)", ~ProxyPath);

        auto error = Process.Spawn();
        if (!error.IsOK()) {
            THROW_ERROR_EXCEPTION("Failed to start job proxy: Spawn failed")
                << error;
        }

        LOG_INFO("Job proxy started (ProcessId: %d)",
            Process.GetProcessId());

        // Unref is called in the thread.
        Ref();
        ControllerThread.Start();

        ControllerThread.Detach();

        return OnExit;
    }

    // Safe to call multiple times
    virtual void Kill(const TNonOwningCGroup& group, const TError& error) throw() override
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        LOG_INFO(error, "Killing job in unsafe environment (ProcessGroup: %s)", ~group.GetFullPath().Quote());

        SetError(error);

        int pid = Process.GetProcessId();

        if ((pid > 0) && !Waited) {
            auto result = kill(pid, 9);
            if (result != 0) {
                switch (errno) {
                    case ESRCH:
                        // Process group doesn't exist already.
                        return;
                    default:
                        LOG_FATAL("Failed to kill job proxy: kill failed (errno: %s)", strerror(errno));
                        break;
                }
            }
        }

        // Wait until job proxy finishes.
        OnExit.Get();

        try {
            RunKiller(group.GetFullPath());
        } catch (const std::exception& ex) {
            LOG_FATAL(TError(ex));
        }

        LOG_INFO("Job killed");
    }

private:
    void SetError(const TError& error)
    {
        TGuard<TSpinLock> guard(SpinLock);
        if (Error.IsOK()) {
            Error = error;
        }
    }

    static void* ThreadFunc(void* param)
    {
        auto controller = MakeStrong(static_cast<TUnsafeProxyController*>(param));
        controller->Unref();
        controller->ThreadMain();
        return NULL;
    }

    void ThreadMain()
    {
        LOG_INFO("Waiting for job proxy to finish");

        auto error = Process.Wait();
        Waited = true;

        auto wrappedError = error.IsOK()
            ? TError()
            : TError("Job proxy failed") << error;
        SetError(wrappedError);
        LOG_INFO(wrappedError, "Job proxy finished");

        OnExit.Set(Error);
    }


    const Stroka ProxyPath;
    const Stroka WorkingDirectory;
    const TJobId JobId;
    const TSlot& Slot;

    NLog::TTaggedLogger Logger;

    TProcess Process;
    bool Waited;
    TIntrusivePtr<TUnsafeEnvironmentBuilder> EnvironmentBuilder;

    TSpinLock SpinLock;
    TError Error;

    TPromise<TError> OnExit;

    TThread ControllerThread;

    DECLARE_THREAD_AFFINITY_SLOT(JobThread);
};

#else

//! Dummy stub for windows.
class TUnsafeProxyController
    : public IProxyController
{
public:
    explicit TUnsafeProxyController(const TJobId& jobId)
        : Logger(ExecAgentLogger)
        , OnExit(NewPromise<TError>())
        , ControllerThread(ThreadFunc, this)
    {
        Logger.AddTag(Sprintf("JobId: %s", ~ToString(jobId)));
    }

    TAsyncError Run()
    {
        ControllerThread.Start();
        ControllerThread.Detach();

        LOG_INFO("Running dummy job");

        return OnExit;
    }

    virtual void Kill(const TNonOwningCGroup& group, const TError& error) throw() override
    {
        LOG_INFO("Killing dummy job");
        OnExit.Get();
    }

private:
    static void* ThreadFunc(void* param)
    {
        TIntrusivePtr<TUnsafeProxyController> controller(reinterpret_cast<TUnsafeProxyController*>(param));
        controller->ThreadMain();
        return NULL;
    }

    void ThreadMain()
    {
        // We don't have jobs support for Windows.
        // Just wait for a couple of seconds and report the failure.
        // This might help with scheduler debugging under Windows.
        Sleep(TDuration::Seconds(5));
        LOG_INFO("Dummy job finished");
        OnExit.Set(TError("Jobs are not supported under Windows"));
    }

    NLog::TTaggedLogger Logger;
    TPromise<TError> OnExit;
    TThread ControllerThread;
};

#endif

////////////////////////////////////////////////////////////////////////////////

IProxyControllerPtr TUnsafeEnvironmentBuilder::CreateProxyController(
    NYTree::INodePtr config,
    const TJobId& jobId,
    const TSlot& slot,
    const Stroka& workingDirectory)
{
#ifndef _win_
    return New<TUnsafeProxyController>(ProxyPath, jobId, slot, workingDirectory, this);
#else
    UNUSED(config);
    UNUSED(slot);
    UNUSED(workingDirectory);
    return New<TUnsafeProxyController>(jobId);
#endif
}

////////////////////////////////////////////////////////////////////////////////

IEnvironmentBuilderPtr CreateUnsafeEnvironmentBuilder()
{
    return New<TUnsafeEnvironmentBuilder>();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NExecAgent
} // namespace NYT
