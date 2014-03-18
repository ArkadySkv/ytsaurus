#pragma once

#include "common.h"

#include <core/misc/error.h>

#ifdef _linux_
    #include <sys/resource.h>
#endif

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

std::vector<int> GetPidsByUid(int uid);

//! Gets the resident set size of a process.
/*!
   \note If |pid == -1| then self RSS is returned.
 */
i64 GetProcessRss(int pid = -1);

void DoKillallByUid(int uid);

void KillallByUid(int uid);

TError StatusToError(int status);

void DoRemoveDirAsRoot(const Stroka& path);

void RemoveDirAsRoot(const Stroka& path);

void SafeClose(int fd, bool ignoreInvalidFd = false);

void CloseAllDescriptors();

int GetErrNoFromExitCode(int exitCode);


DECLARE_ENUM(EExitStatus,
    ((ExitCodeBase)         (10000))

    ((SignalBase)           (11000))
    ((SigTerm)              (11006))
    ((SigKill)              (11009))
);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
