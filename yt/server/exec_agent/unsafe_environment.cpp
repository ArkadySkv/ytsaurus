﻿#include "stdafx.h"
#include "unsafe_environment.h"
#include "environment.h"
#include "private.h"

#include <ytlib/misc/thread_affinity.h>
#include <ytlib/misc/proc.h>

#include <ytlib/logging/tagged_logger.h>

#include <server/job_proxy/public.h>

#include <util/system/execpath.h>

#include <fcntl.h>

#ifndef _win_

#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>

#endif

namespace NYT {
namespace NExecAgent {

using namespace NJobProxy;

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = ExecAgentLogger;

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
        const Stroka& workingDirectory,
        TUnsafeEnvironmentBuilder* envBuilder)
        : ProxyPath(proxyPath)
        , WorkingDirectory(workingDirectory)
        , JobId(jobId)
        , Logger(ExecAgentLogger)
        , ProcessId(-1)
        , EnvironmentBuilder(envBuilder)
        , OnExit(NewPromise<TError>())
        , ControllerThread(ThreadFunc, this)
    {
        Logger.AddTag(Sprintf("JobId: %s", ~ToString(jobId)));
    }

    void Run()
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        LOG_INFO("Starting job proxy in unsafe environment (WorkDir: %s)",
            ~WorkingDirectory);

        std::vector<int> fileIds = GetAllDescriptors();

        auto storeStrings = [](std::initializer_list<const char*> strings) -> std::vector<std::vector<char>> {
            std::vector<std::vector<char>> result;
            for (auto item : strings) {
                result.push_back(std::vector<char>(item, item + strlen(item) + 1));
            }
            return result;
        };

        std::vector<std::vector<char>> argContainer = storeStrings({
            ~ProxyPath,
            "--job-proxy",
            "--config",
            ~ProxyConfigFileName,
            "--job-id",
            ~ToString(JobId),
            "--working-dir",
            ~ToString(WorkingDirectory),
            "--memory-limit",
            ~ToString(MemoryLimit)
        });

        std::vector<char *> args;
        for (auto& x : argContainer) {
            args.push_back(&x[0]);
        }
        args.push_back(NULL);

        int err_code = posix_spawn(&ProcessId,
                                   ~ProxyPath,
                                   NULL,
                                   NULL,
                                   &args[0],
                                   NULL);
        if (err_code != 0) {
            // Failed to exec job proxy
            _exit(EJobProxyExitCode::ExecFailed);
        }

        if (ProcessId < 0) {
            THROW_ERROR_EXCEPTION("Failed to start job proxy: fork failed")
                << TError::FromSystem();
        }

        LOG_INFO("Job proxy started (ProcessId: %d)",
            ProcessId);

        // Unref is called in the thread.
        Ref();
        ControllerThread.Start();

        ControllerThread.Detach();
    }

    // Safe to call multiple times
    void Kill(int uid, const TError& error) throw()
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        LOG_INFO(error, "Killing job in unsafe environment (UID: %d)", uid);

        SetError(error);

        int pid = ProcessId;

        if (pid > 0) {
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

        if (uid > 0) {
            try {
                KillallByUser(uid);
            } catch (const std::exception& ex) {
                LOG_FATAL(TError(ex));
            }
        }

        LOG_INFO("Job killed");
    }

    void SubscribeExited(const TCallback<void(TError)>& callback)
    {
        OnExit.Subscribe(callback);
    }

    void UnsubscribeExited(const TCallback<void(TError)>& callback)
    {
        YUNIMPLEMENTED();
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

        int status = 0;
        {
            int pid = ProcessId;
            int result = waitpid(pid, &status, WUNTRACED);

            // Set ProcessId back to -1, so that we don't try to kill it ever after.
            ProcessId = -1;
            if (result < 0) {
                SetError(TError("Failed to wait for job proxy to finish: waitpid failed")
                    << TError::FromSystem());
                OnExit.Set(Error);
                return;
            }
            YASSERT(result == pid);
        }

        auto statusError = StatusToError(status);
        auto wrappedError = statusError.IsOK()
            ? TError()
            : TError("Job proxy failed") << statusError;
        SetError(wrappedError);

        LOG_INFO(wrappedError, "Job proxy finished");

        OnExit.Set(Error);
    }


    const Stroka ProxyPath;
    const Stroka WorkingDirectory;
    const TJobId JobId;

    NLog::TTaggedLogger Logger;

    int ProcessId;
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
    TUnsafeProxyController(const TJobId& jobId)
        : Logger(ExecAgentLogger)
        , OnExit(NewPromise<TError>())
        , ControllerThread(ThreadFunc, this)
    {
        Logger.AddTag(Sprintf("JobId: %s", ~ToString(jobId)));
    }

    void Run()
    {
        ControllerThread.Start();
        ControllerThread.Detach();

        LOG_INFO("Running dummy job");
    }

    void Kill(int uid, const TError& error)
    {
        LOG_INFO("Killing dummy job");
        OnExit.Get();
    }

    void SubscribeExited(const TCallback<void(TError)>& callback)
    {
        OnExit.Subscribe(callback);
    }

    void UnsubscribeExited(const TCallback<void(TError)>& callback)
    {
        YUNIMPLEMENTED();
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
    const Stroka& workingDirectory)
{
#ifndef _win_
    return New<TUnsafeProxyController>(ProxyPath, jobId, workingDirectory, this);
#else
    UNUSED(config);
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
