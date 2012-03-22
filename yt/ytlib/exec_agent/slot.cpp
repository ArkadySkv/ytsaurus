﻿#include "stdafx.h"
#include "slot.h"
#include "private.h"

#include <ytlib/misc/fs.h>
#include <util/folder/dirut.h>

namespace NYT {
namespace NExecAgent {

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = ExecAgentLogger;

////////////////////////////////////////////////////////////////////////////////

// ToDo(psushin): think about more complex logic of handling fs errors.

TSlot::TSlot(const Stroka& path, const Stroka& name)
    : IsFree_(true)
    , IsClean(true)
    , Path(path)
    , SlotThread(New<TActionQueue>(name))
{
    try {
        NFS::ForcePath(Path);
        SandboxPath = NFS::CombinePaths(Path, "sandbox");
    } catch (const std::exception& ex) {
        LOG_FATAL("Failed to create slot directory (Path: %s, Error: %s).", ~Path, ex.what());
    }
}

void TSlot::Acquire()
{
    IsFree_ = false;
}

bool TSlot::IsFree() const 
{
    return IsFree_;
}

void TSlot::Clean()
{
    try {
        RemoveDirWithContents(SandboxPath);
        IsClean = true;
    } catch (const std::exception& ex) {
        LOG_FATAL("Failed to clean sandbox (SandboxPath: %s, Error: %s).", ~SandboxPath, ex.what());
    }
}

void TSlot::Release()
{
    YASSERT(IsClean);
    IsFree_ = true;
}

void TSlot::InitSandbox()
{
    YASSERT(!IsFree_);
    try {
        NFS::ForcePath(SandboxPath);
    } catch (const std::exception& ex) {
        LOG_FATAL("Failed to create sandbox (SandboxPath: %s, Error: %s).", ~SandboxPath, ex.what());
    }
    IsClean = false;
    LOG_TRACE("Slot created sandbox path: %s", ~SandboxPath);
}

void TSlot::MakeLink(
    const Stroka& linkName, 
    const Stroka& targetPath, 
    bool isExecutable)
{
    auto linkPath = NFS::CombinePaths(SandboxPath, linkName);
    NFS::MakeSymbolicLink(targetPath, linkPath);
    // ToDo: fix set executable.
    //NFS::SetExecutableMode(linkPath, isExecutable);
}

const Stroka& TSlot::GetWorkingDirectory() const
{
    return Path;
}

IInvoker::TPtr TSlot::GetInvoker()
{
    return SlotThread->GetInvoker();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NExecAgent
} // namespace NYT
