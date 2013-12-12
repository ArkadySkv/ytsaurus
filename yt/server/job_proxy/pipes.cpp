﻿#include "stdafx.h"
#include "pipes.h"

#include <core/yson/parser.h>
#include <core/yson/consumer.h>

#include <ytlib/table_client/table_producer.h>
#include <ytlib/table_client/sync_reader.h>

#include <ytlib/pipes/async_reader.h>
#include <ytlib/pipes/async_writer.h>
#include <ytlib/pipes/io_dispatcher.h>

#include <core/misc/proc.h>
#include <core/concurrency/fiber.h>

#include <util/system/file.h>

#include <errno.h>

#if defined(_linux_) || defined(_darwin_)
    #include <unistd.h>
    #include <fcntl.h>
    #include <sys/stat.h>
#endif

#if defined(_win_)
    #include <io.h>
#endif

namespace NYT {
namespace NJobProxy {

using namespace NTableClient;
using NConcurrency::WaitFor;

////////////////////////////////////////////////////////////////////

static auto& Logger = JobProxyLogger;

static const i64 InputBufferSize  = (i64) 1 * 1024 * 1024;
static const i64 OutputBufferSize = (i64) 1 * 1024 * 1024;

////////////////////////////////////////////////////////////////////

#if defined(_linux_) || defined(_darwin_)

int SafeDup(int oldFd)
{
    while (true) {
        auto fd = dup(oldFd);

        if (fd == -1) {
            switch (errno) {
            case EINTR:
            case EBUSY:
                break;

            default:
                THROW_ERROR_EXCEPTION("dup failed")
                    << TError::FromSystem();
            }
        } else {
            return fd;
        }
    }
}

void SafeDup2(int oldFd, int newFd)
{
    while (true) {
        auto res = dup2(oldFd, newFd);

        if (res == -1) {
            switch (errno) {
            case EINTR:
            case EBUSY:
                break;

            default:
                THROW_ERROR_EXCEPTION("dup2 failed (OldFd: %d, NewFd: %d)",
                    oldFd,
                    newFd)
                    << TError::FromSystem();
            }
        } else {
            return;
        }
    }
}

int SafePipe(int fd[2])
{
    auto res = pipe(fd);
    if (res == -1) {
        THROW_ERROR_EXCEPTION("pipe failed")
            << TError::FromSystem();
    }
    return res;
}

void SafeMakeNonblocking(int fd)
{
    auto res = fcntl(fd, F_GETFL);

    if (res == -1) {
        THROW_ERROR_EXCEPTION("fcntl failed to get descriptor flags")
            << TError::FromSystem();
    }

    res = fcntl(fd, F_SETFL, res | O_NONBLOCK);

    if (res == -1) {
        THROW_ERROR_EXCEPTION("fcntl failed to set descriptor flags")
            << TError::FromSystem();
    }
}

void CheckJobDescriptor(int fd)
{
    auto res = fcntl(fd, F_GETFD);
    if (res == -1) {
        THROW_ERROR_EXCEPTION("Job descriptor is not valid (Fd: %d)", fd)
            << TError::FromSystem();
    }

    if (res & FD_CLOEXEC) {
        THROW_ERROR_EXCEPTION("CLOEXEC flag is set for job descriptor (Fd: %d)", fd);
    }
}

void ChmodJobDescriptor(int fd)
{
    const int permissions = S_IRUSR | S_IRGRP | S_IROTH | S_IWUSR | S_IWGRP | S_IWOTH;
    auto procPath = Sprintf("/proc/self/fd/%d", fd);
    auto res = chmod(~procPath, permissions);

    if (res == -1) {
        THROW_ERROR_EXCEPTION("Failed to chmod job descriptor (Fd: %d, Permissions: %d)",
            fd,
            permissions)
            << TError::FromSystem();
    }
}


#else

// Streaming jobs are not supposed to work on windows for now.

int SafeDup(int oldFd)
{
    YUNIMPLEMENTED();
}

void SafeDup2(int oldFd, int newFd)
{
    YUNIMPLEMENTED();
}

int SafePipe(int fd[2])
{
    YUNIMPLEMENTED();
}

void SafeMakeNonblocking(int fd)
{
    YUNIMPLEMENTED();
}

void CheckJobDescriptor(int fd)
{
    YUNIMPLEMENTED();
}

void ChmodJobDescriptor(int fd)
{
    YUNIMPLEMENTED();
}

#endif

////////////////////////////////////////////////////////////////////

TOutputPipe::TOutputPipe(
    int fd[2],
    TOutputStream* output,
    int jobDescriptor)
    : OutputStream(output)
    , JobDescriptor(jobDescriptor)
    , Pipe(fd)
    , IsFinished(false)
    , IsClosed(false)
    , Buffer(OutputBufferSize)
    , Reader(New<NPipes::TAsyncReader>(Pipe.ReadFd))
{
    YCHECK(JobDescriptor);
}

void TOutputPipe::PrepareJobDescriptors()
{
    YASSERT(!IsFinished);

    SafeClose(Pipe.ReadFd);

    // Always try to close target descriptor before calling dup2.
    SafeClose(JobDescriptor, true);

    SafeDup2(Pipe.WriteFd, JobDescriptor);
    SafeClose(Pipe.WriteFd);

    ChmodJobDescriptor(JobDescriptor);

    CheckJobDescriptor(JobDescriptor);
}

void TOutputPipe::PrepareProxyDescriptors()
{
    YASSERT(!IsFinished);

    SafeClose(Pipe.WriteFd);
    SafeMakeNonblocking(Pipe.ReadFd);
}

TError TOutputPipe::DoAll()
{
    return ReadAll();
}

TError TOutputPipe::ReadAll()
{
    bool isClosed = false;

    TBlob buffer;
    while (!isClosed)
    {
        TBlob data;
        std::tie(data, isClosed) = Reader->Read(std::move(buffer));

        try {
            OutputStream->Write(data.Begin(), data.Size());
        } catch (const std::exception& ex) {
            return TError("Failed to write into output (Fd: %d)",
                JobDescriptor) << TError(ex);
        }

        if ((!isClosed) && (data.Size() == 0)) {
            auto error = WaitFor(Reader->GetReadyEvent());
            RETURN_IF_ERROR(error);
        }
        buffer = std::move(data);
    }
    return TError();
}

TError TOutputPipe::Close()
{
    return Reader->Close();
}

void TOutputPipe::Finish()
{
    OutputStream->Finish();
}

////////////////////////////////////////////////////////////////////

TInputPipe::TInputPipe(
    int fd[2],
    std::unique_ptr<NTableClient::TTableProducer> tableProducer,
    std::unique_ptr<TBlobOutput> buffer,
    std::unique_ptr<NYson::IYsonConsumer> consumer,
    int jobDescriptor)
    : Pipe(fd)
    , JobDescriptor(jobDescriptor)
    , TableProducer(std::move(tableProducer))
    , Buffer(std::move(buffer))
    , Consumer(std::move(consumer))
    , Position(0)
    , HasData(true)
    , IsFinished(false)
    , Writer(New<NPipes::TAsyncWriter>(Pipe.WriteFd))
{
    YCHECK(~TableProducer);
    YCHECK(~Buffer);
    YCHECK(~Consumer);
}

void TInputPipe::PrepareJobDescriptors()
{
    YASSERT(!IsFinished);

    SafeClose(Pipe.WriteFd);

    // Always try to close target descriptor before calling dup2.
    SafeClose(JobDescriptor, true);

    SafeDup2(Pipe.ReadFd, JobDescriptor);
    SafeClose(Pipe.ReadFd);

    ChmodJobDescriptor(JobDescriptor);

    CheckJobDescriptor(JobDescriptor);
}

void TInputPipe::PrepareProxyDescriptors()
{
    YASSERT(!IsFinished);

    SafeMakeNonblocking(Pipe.WriteFd);
}

TError TInputPipe::DoAll()
{
    return WriteAll();
}

TError TInputPipe::WriteAll()
{
    while (HasData) {
        HasData = TableProducer->ProduceRow();
        bool enough = Writer->Write(Buffer->Begin(), Buffer->Size());
        Buffer->Clear();

        if (enough) {
            auto error = WaitFor(Writer->GetReadyEvent());
            RETURN_IF_ERROR(error);
        }
    }
    {
        auto error = WaitFor(Writer->AsyncClose());
        return error;
    }
}

TError TInputPipe::Close()
{
    return WaitFor(Writer->AsyncClose());
}

void TInputPipe::Finish()
{
    bool dataConsumed = !HasData;
    if (dataConsumed) {
        char buffer;
        // Try to read some data from the pipe.
        ssize_t res = read(Pipe.ReadFd, &buffer, 1);
        dataConsumed = res <= 0;
    }

    SafeClose(Pipe.ReadFd);

    if (!dataConsumed) {
        THROW_ERROR_EXCEPTION("Input stream was not fully consumed by user process (Fd: %d, JobDescriptor: %d)",
            Pipe.WriteFd,
            JobDescriptor);
    }
}

////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
