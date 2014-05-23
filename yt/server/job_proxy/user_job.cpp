﻿#include "stdafx.h"
#include "private.h"
#include "job_detail.h"
#include "config.h"
#include "user_job.h"
#include "user_job_io.h"
#include "stderr_output.h"
#include "table_output.h"
#include "pipes.h"

#include <core/formats/format.h>
#include <core/formats/parser.h>

#include <core/yson/writer.h>

#include <core/ytree/convert.h>

#include <core/rpc/channel.h>

#include <core/actions/invoker_util.h>

#include <core/misc/proc.h>
#include <core/misc/protobuf_helpers.h>
#include <core/misc/pattern_formatter.h>

#include <core/concurrency/periodic_executor.h>

#include <ytlib/table_client/table_producer.h>
#include <ytlib/table_client/table_consumer.h>
#include <ytlib/table_client/sync_reader.h>
#include <ytlib/table_client/sync_writer.h>

#include <ytlib/transaction_client/public.h>

#include <ytlib/cgroup/cgroup.h>

#include <util/folder/dirut.h>

#include <util/stream/null.h>

#include <errno.h>

#ifdef _linux_
    #include <core/misc/ioprio.h>

    #include <unistd.h>
    #include <signal.h>
    #include <fcntl.h>

    #include <sys/types.h>
    #include <sys/time.h>
    #include <sys/wait.h>
    #include <sys/resource.h>
    #include <sys/stat.h>
    #include <sys/epoll.h>
#endif

namespace NYT {
namespace NJobProxy {

using namespace NYTree;
using namespace NYson;
using namespace NTableClient;
using namespace NFormats;
using namespace NScheduler;
using namespace NScheduler::NProto;
using namespace NTransactionClient;
using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

static auto& Logger = JobProxyLogger;

////////////////////////////////////////////////////////////////////////////////

#ifdef _linux_

static i64 MemoryLimitBoost = (i64) 2 * 1024 * 1024 * 1024;

class TUserJob
    : public TJob
{
public:
    TUserJob(
        IJobHost* host,
        const NScheduler::NProto::TUserJobSpec& userJobSpec,
        std::unique_ptr<TUserJobIO> userJobIO,
        NJobAgent::TJobId jobId)
        : TJob(host)
        , JobIO(std::move(userJobIO))
        , UserJobSpec(userJobSpec)
        , InitCompleted(false)
        , InputThread(InputThreadFunc, (void*) this)
        , OutputThread(OutputThreadFunc, (void*) this)
        , MemoryUsage(UserJobSpec.memory_reserve())
        , ProcessId(-1)
        , JobId(jobId)
        , CpuAccounting(ToString(jobId))
        , BlockIO(ToString(jobId))
    {
        auto config = host->GetConfig();
        MemoryWatchdogExecutor = New<TPeriodicExecutor>(
            GetSyncInvoker(),
            BIND(&TUserJob::CheckMemoryUsage, MakeWeak(this)),
            config->MemoryWatchdogPeriod);
    }

    virtual NJobTrackerClient::NProto::TJobResult Run() override
    {
        // ToDo(psushin): use tagged logger here.
        LOG_DEBUG("Starting job process");

        InitPipes();

        InitCompleted = true;

        try {
            CpuAccounting.Create();
            BlockIO.Create();
        } catch (const TErrorException& e) {
            LOG_ERROR("Unable to create a cgroup to track job resource consumption");
        }

        ProcessStartTime = TInstant::Now();
        ProcessId = fork();
        if (ProcessId < 0) {
            THROW_ERROR_EXCEPTION("Failed to start the job: fork failed")
                << TError::FromSystem();
        }

        NJobTrackerClient::NProto::TJobResult result;

        if (ProcessId == 0) {
            // Child process.
            StartJob();
            YUNREACHABLE();
        }

        LOG_INFO("Job process started");

        MemoryWatchdogExecutor->Start();
        DoJobIO();
        MemoryWatchdogExecutor->Stop();

        LOG_INFO(JobExitError, "Job process completed");
        ToProto(result.mutable_error(), JobExitError);

        if (CpuAccounting.IsCreated()) {
            CpuAccountingStats = CpuAccounting.GetStats();
            try {
                CpuAccounting.Destroy();
            } catch (const TErrorException& e) {
                LOG_ERROR("Unable to remove a cgroup");
            }
        }

        if (BlockIO.IsCreated()) {
            BlockIOStats = BlockIO.GetStats();
            try {
                BlockIO.Destroy();
            } catch (const TErrorException& e) {
                LOG_ERROR("Unable to remove a cgroup");
            }
        }

        if (ErrorOutput) {
            auto stderrChunkId = ErrorOutput->GetChunkId();
            if (stderrChunkId != NChunkServer::NullChunkId) {
                auto* schedulerResultExt = result.MutableExtension(TSchedulerJobResultExt::scheduler_job_result_ext);
                ToProto(schedulerResultExt->mutable_stderr_chunk_id(), stderrChunkId);
                LOG_INFO("Stderr chunk generated (ChunkId: %s)", ~ToString(stderrChunkId));
            }
        }

        if (JobExitError.IsOK()) {
            JobIO->PopulateResult(&result);
        }

        return result;
    }

    virtual double GetProgress() const override
    {
        return InitCompleted ? JobIO->GetProgress() : 0;
    }

    virtual std::vector<NChunkClient::TChunkId> GetFailedChunkIds() const override
    {
        return JobIO->GetFailedChunkIds();
    }

private:
    void InitPipes()
    {
        LOG_DEBUG("Initializing pipes");

        // We use the following convention for designating input and output file descriptors
        // in job processes:
        // fd == 3 * (N - 1) for the N-th input table (if exists)
        // fd == 3 * (N - 1) + 1 for the N-th output table (if exists)
        // fd == 2 for the error stream
        // e. g.
        // 0 - first input table
        // 1 - first output table
        // 2 - error stream
        // 3 - second input
        // 4 - second output
        // etc.
        //
        // A special option (ToDo(psushin): which one?) enables concatenating
        // all input streams into fd == 0.

        int maxReservedDescriptor = 0;

        if (UserJobSpec.use_yamr_descriptors()) {
            maxReservedDescriptor = 2 + JobIO->GetOutputCount();
        } else {
            maxReservedDescriptor = std::max(
                JobIO->GetInputCount(),
                JobIO->GetOutputCount()) * 3;
        }

        YASSERT(maxReservedDescriptor > 0);

        // To avoid descriptor collisions between pipes on this, proxy side,
        // and "standard" descriptor numbers in forked job (see comments above)
        // we ensure that enough lower descriptors are allocated before creating pipes.

        std::vector<int> reservedDescriptors;
        auto createPipe = [&] (int fd[2]) {
            while (true) {
                SafePipe(fd);
                if (fd[0] < maxReservedDescriptor || fd[1] < maxReservedDescriptor) {
                    reservedDescriptors.push_back(fd[0]);
                    reservedDescriptors.push_back(fd[1]);
                } else {
                    break;
                }
            }
        };

        int pipe[2];
        createPipe(pipe);

        // Configure stderr pipe.
        TOutputStream* stdErrOutput;
        if (UserJobSpec.has_stderr_transaction_id()) {
            auto stderrTransactionId = FromProto<TTransactionId>(UserJobSpec.stderr_transaction_id());
            ErrorOutput = JobIO->CreateErrorOutput(
                stderrTransactionId,
                UserJobSpec.max_stderr_size());
            stdErrOutput = ~ErrorOutput;
        } else {
            stdErrOutput = &NullErrorOutput;
        }
        OutputPipes.push_back(New<TOutputPipe>(pipe, stdErrOutput, STDERR_FILENO));

        // Make pipe for each input and each output table.
        {
            YCHECK(!UserJobSpec.use_yamr_descriptors() || JobIO->GetInputCount() == 1);

            auto format = ConvertTo<TFormat>(TYsonString(UserJobSpec.input_format()));
            for (int i = 0; i < JobIO->GetInputCount(); ++i) {
                std::unique_ptr<TBlobOutput> buffer(new TBlobOutput());
                auto consumer = CreateConsumerForFormat(
                    format,
                    EDataType::Tabular,
                    ~buffer);

                createPipe(pipe);
                InputPipes.push_back(New<TInputPipe>(
                    pipe,
                    JobIO->CreateTableInput(i, ~consumer),
                    std::move(buffer),
                    std::move(consumer),
                    3 * i));
            }
        }

        {
            auto format = ConvertTo<TFormat>(TYsonString(UserJobSpec.output_format()));
            int outputCount = JobIO->GetOutputCount();
            TableOutput.resize(outputCount);

            Writers.reserve(outputCount);
            for (int i = 0; i < outputCount; ++i) {
                auto writer = JobIO->CreateTableOutput(i);
                Writers.push_back(writer);
            }

            for (int i = 0; i < outputCount; ++i) {
                std::unique_ptr<IYsonConsumer> consumer(new TTableConsumer(Writers, i));
                auto parser = CreateParserForFormat(format, EDataType::Tabular, ~consumer);
                TableOutput[i].reset(new TTableOutput(
                    std::move(parser),
                    std::move(consumer)));
                createPipe(pipe);

                int jobDescriptor = UserJobSpec.use_yamr_descriptors()
                    ? 3 + i
                    : 3 * i + 1;

                OutputPipes.push_back(New<TOutputPipe>(pipe, ~TableOutput[i], jobDescriptor));
            }
        }

        // Close reserved descriptors.
        FOREACH (int fd, reservedDescriptors) {
            SafeClose(fd);
        }

        LOG_DEBUG("Pipes initialized");
    }

    static void* InputThreadFunc(void* param)
    {
        NConcurrency::SetCurrentThreadName("JobProxyInput");
        TIntrusivePtr<TUserJob> job = (TUserJob*)param;
        job->ProcessPipes(job->InputPipes);
        return NULL;
    }

    static void* OutputThreadFunc(void* param)
    {
        NConcurrency::SetCurrentThreadName("JobProxyOutput");
        TIntrusivePtr<TUserJob> job = (TUserJob*)param;
        job->ProcessPipes(job->OutputPipes);
        return NULL;
    }

    void SetError(const TError& error)
    {
        if (error.IsOK()) {
            return;
        }

        TGuard<TSpinLock> guard(SpinLock);
        if (JobExitError.IsOK()) {
            JobExitError = TError("User job failed");
        };

        JobExitError.InnerErrors().push_back(error);
    }

    void ProcessPipes(std::vector<IDataPipePtr>& pipes)
    {
        // TODO(babenko): rewrite using libuv
        try {
            int activePipeCount = pipes.size();

            FOREACH (auto& pipe, pipes) {
                pipe->PrepareProxyDescriptors();
            }

            const int fdCountHint = 10;
            int epollFd = epoll_create(fdCountHint);
            if (epollFd < 0) {
                THROW_ERROR_EXCEPTION("Error during job IO: epoll_create failed")
                    << TError::FromSystem();
            }

            FOREACH (auto& pipe, pipes) {
                epoll_event evAdd;
                evAdd.data.u64 = 0ULL;
                evAdd.events = pipe->GetEpollFlags();
                evAdd.data.ptr = ~pipe;

                if (epoll_ctl(epollFd, EPOLL_CTL_ADD, pipe->GetEpollDescriptor(), &evAdd) != 0) {
                    THROW_ERROR_EXCEPTION("Error during job IO: epoll_ctl failed")
                        << TError::FromSystem();
                }
            }

            const int maxEvents = 10;
            epoll_event events[maxEvents];
            memset(events, 0, maxEvents * sizeof(epoll_event));

            while (activePipeCount > 0) {
                {
                    TGuard<TSpinLock> guard(SpinLock);
                    if (!JobExitError.IsOK()) {
                        break;
                    }
                }

                LOG_TRACE("Waiting on epoll, %d pipes active", activePipeCount);

                int epollResult = epoll_wait(epollFd, &events[0], maxEvents, -1);

                if (epollResult < 0) {
                    if (errno == EINTR) {
                        errno = 0;
                        continue;
                    }
                    THROW_ERROR_EXCEPTION("Error during job IO: epoll_wait failed")
                        << TError::FromSystem();
                }

                for (int pipeIndex = 0; pipeIndex < epollResult; ++pipeIndex) {
                    auto pipe = reinterpret_cast<IDataPipe*>(events[pipeIndex].data.ptr);
                    if (!pipe->ProcessData(events[pipeIndex].events)) {
                        --activePipeCount;
                    }
                }
            }

            SafeClose(epollFd);
        } catch (const std::exception& ex) {
            SetError(TError(ex));
        } catch (...) {
            SetError(TError("Unknown error during job IO"));
        }

        FOREACH (auto& pipe, pipes) {
            // Close can throw exception which will cause JobProxy death.
            // For now let's assume it is unrecoverable.
            // Anyway, system seems to be in a very bad state if this happens.
            pipe->CloseHandles();
        }
    }

    void DoJobIO()
    {
        InputThread.Start();
        OutputThread.Start();
        OutputThread.Join();

        int status = 0;
        int waitpidResult = waitpid(ProcessId, &status, 0);
        if (waitpidResult < 0) {
            SetError(TError("waitpid failed") << TError::FromSystem());
        } else {
            SetError(StatusToError(status));
        }

        auto finishPipe = [&] (IDataPipePtr pipe) {
            try {
                pipe->Finish();
            } catch (const std::exception& ex) {
                SetError(TError(ex));
            }
        };

        // Stderr output pipe finishes first.
        FOREACH (auto& pipe, OutputPipes) {
            finishPipe(pipe);
        }

        FOREACH (auto& pipe, InputPipes) {
            finishPipe(pipe);
        }

        FOREACH(auto& writer, Writers) {
            try {
                writer->Close();
            } catch (const std::exception& ex) {
                SetError(TError(ex));
            }
        }

        // If user process fais, InputThread may be blocked on epoll
        // because reading end of input pipes is left open to
        // check that all data was consumed. That is why we should join the
        // thread after pipe finish.
        InputThread.Join();
    }

    // Called from the forked process.
    void StartJob()
    {
        auto host = Host.Lock();
        YCHECK(host);

        try {
            FOREACH (auto& pipe, InputPipes) {
                pipe->PrepareJobDescriptors();
            }

            FOREACH (auto& pipe, OutputPipes) {
                pipe->PrepareJobDescriptors();
            }

            if (UserJobSpec.use_yamr_descriptors()) {
                // This hack is to work around the fact that output pipe accepts single job descriptor,
                // whilst yamr convention requires fds 1 and 3 to be the same.
                SafeDup2(3, 1);
            }

            // ToDo(psushin): handle errors.
            auto config = host->GetConfig();
            ChDir(config->SandboxName);

            TPatternFormatter formatter;
            formatter.AddProperty("SandboxPath", GetCwd());

            std::vector<Stroka> envHolders;
            envHolders.reserve(UserJobSpec.environment_size());

            std::vector<const char*> envp(UserJobSpec.environment_size() + 1);
            for (int i = 0; i < UserJobSpec.environment_size(); ++i) {
                envHolders.push_back(formatter.Format(UserJobSpec.environment(i)));
                envp[i] = ~envHolders.back();
            }
            envp[UserJobSpec.environment_size()] = NULL;

            if (UserJobSpec.enable_vm_limit()) {
                auto memoryLimit = static_cast<rlim_t>(UserJobSpec.memory_limit() * config->MemoryLimitMultiplier);
                memoryLimit += MemoryLimitBoost;
                struct rlimit rlimit = {memoryLimit, RLIM_INFINITY};

                auto res = setrlimit(RLIMIT_AS, &rlimit);
                if (res) {
                    fprintf(stderr, "Failed to set resource limits (MemoryLimit: %" PRId64 ")\n%s",
                        rlimit.rlim_max,
                        strerror(errno));
                    _exit(EJobProxyExitCode::SetRLimitFailed);
                }
            }

            if (!UserJobSpec.enable_core_dump()) {
                struct rlimit rlimit = {0, 0};

                auto res = setrlimit(RLIMIT_CORE, &rlimit);
                if (res) {
                    fprintf(stderr, "Failed to disable core dumps\n%s", strerror(errno));
                    _exit(EJobProxyExitCode::SetRLimitFailed);
                }
            }

            CpuAccounting.AddCurrentProcess();
            BlockIO.AddCurrentProcess();

            if (config->UserId > 0) {
                // Set unprivileged uid and gid for user process.
                YCHECK(setuid(0) == 0);

                YCHECK(setresgid(config->UserId, config->UserId, config->UserId) == 0);
                YCHECK(setuid(config->UserId) == 0);
                
                if (UserJobSpec.enable_io_prio()) {
                    YCHECK(ioprio_set(IOPRIO_WHO_USER, config->UserId, IOPRIO_PRIO_VALUE(IOPRIO_CLASS_BE, 7)) == 0);
                }
            }

            Stroka cmd = UserJobSpec.shell_command();
            // do not search the PATH, inherit environment
            execle("/bin/sh",
                "/bin/sh",
                "-c",
                ~cmd,
                (void*)NULL,
                envp.data());

            int _errno = errno;

            fprintf(stderr, "Failed to exec job (/bin/sh -c '%s'): %s\n",
                ~cmd,
                strerror(_errno));
            _exit(EJobProxyExitCode::ExecFailed);
        } catch (const std::exception& ex) {
            fprintf(stderr, "%s", ex.what());
            _exit(EJobProxyExitCode::UncaughtException);
        }
    }

    void CheckMemoryUsage()
    {
        auto host = Host.Lock();
        if (!host)
            return;

        int uid = host->GetConfig()->UserId;
        if (uid <= 0) {
            return;
        }

        try {
            LOG_DEBUG("Started checking memory usage (UID: %d)", uid);

            auto pids = GetPidsByUid(uid);

            i64 memoryLimit = UserJobSpec.memory_limit();
            i64 rss = 0;
            FOREACH(int pid, pids) {
                try {
                    i64 processRss = GetProcessRss(pid);
                    // ProcessId itself is skipped since it's always 'sh'.
                    // This also helps to prevent taking proxy's own RSS into account
                    // when it has fork-ed but not exec-uted the child process yet.
                    bool skip = (pid == ProcessId);
                    LOG_DEBUG("PID: %d, RSS: %" PRId64 "%s",
                        pid,
                        processRss,
                        skip ? " (skipped)" : "");
                    if (!skip) {
                        rss += processRss;
                    }
                } catch (const std::exception& ex) {
                    LOG_DEBUG(ex, "Failed to get RSS for PID %d",
                        pid);
                }
            }

            LOG_DEBUG("Finished checking memory usage (UID: %d, RSS: %" PRId64 ", MemoryLimit: %" PRId64 ")",
                uid,
                rss,
                memoryLimit);

            if (rss > memoryLimit) {
                SetError(TError(EErrorCode::MemoryLimitExceeded, "Memory limit exceeded")
                    << TErrorAttribute("rss", rss)
                    << TErrorAttribute("limit", memoryLimit)
                    << TErrorAttribute("time_since_start", (TInstant::Now() - ProcessStartTime).MilliSeconds()));
                KilallByUid(uid);
                return;
            }

            if (rss > MemoryUsage) {
                i64 delta = rss - MemoryUsage;
                LOG_INFO("Memory usage increased by %" PRId64, delta);

                MemoryUsage += delta;

                auto resourceUsage = host->GetResourceUsage();
                resourceUsage.set_memory(resourceUsage.memory() + delta);
                host->SetResourceUsage(resourceUsage);
            }
        } catch (const std::exception& ex) {
            SetError(ex);
            KilallByUid(uid);
        }
    }

    virtual NJobTrackerClient::NProto::TJobStatistics GetStatistics() const override
    {
        NJobTrackerClient::NProto::TJobStatistics result;
        result.set_time(GetElapsedTime().MilliSeconds());

        ToProto(result.mutable_input(), JobIO->GetInputDataStatistics());
        ToProto(result.mutable_output(), JobIO->GetOutputDataStatistics());

        result.set_cpu_user_time(CpuAccountingStats.User.count());
        result.set_cpu_system_time(CpuAccountingStats.System.count());

        result.set_block_io_sectors(BlockIOStats.Sectors);
        result.set_block_io_bytes_read(BlockIOStats.BytesRead);
        result.set_block_io_bytes_written(BlockIOStats.BytesWritten);

        return result;
    }

    std::unique_ptr<TUserJobIO> JobIO;

    const NScheduler::NProto::TUserJobSpec& UserJobSpec;

    volatile bool InitCompleted;

    std::vector<IDataPipePtr> InputPipes;
    std::vector<IDataPipePtr> OutputPipes;

    std::vector<ISyncWriterPtr> Writers;

    TThread InputThread;
    TThread OutputThread;

    TSpinLock SpinLock;
    TError JobExitError;

    i64 MemoryUsage;

    TPeriodicExecutorPtr MemoryWatchdogExecutor;

    std::unique_ptr<TErrorOutput> ErrorOutput;
    TNullOutput NullErrorOutput;
    std::vector< std::unique_ptr<TOutputStream> > TableOutput;

    TInstant ProcessStartTime;
    int ProcessId;

    NJobAgent::TJobId JobId;

    NCGroup::TCpuAccounting CpuAccounting;
    NCGroup::TCpuAccounting::TStats CpuAccountingStats;

    NCGroup::TBlockIO BlockIO;
    NCGroup::TBlockIO::TStats BlockIOStats;
};

TJobPtr CreateUserJob(
    IJobHost* host,
    const NScheduler::NProto::TUserJobSpec& userJobSpec,
    std::unique_ptr<TUserJobIO> userJobIO,
    NJobAgent::TJobId jobId)
{
    return New<TUserJob>(
        host,
        userJobSpec,
        std::move(userJobIO),
        jobId);
}

#else

TJobPtr CreateUserJob(
    IJobHost* host,
    const NScheduler::NProto::TUserJobSpec& userJobSpec,
    std::unique_ptr<TUserJobIO> userJobIO)
{
    THROW_ERROR_EXCEPTION("Streaming jobs are supported only under Linux");
}

#endif

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
