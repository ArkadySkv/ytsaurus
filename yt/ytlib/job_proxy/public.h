﻿#pragma once

#include <ytlib/scheduler/public.h>
#include <ytlib/misc/intrusive_ptr.h>

namespace NYT {
namespace NJobProxy {

////////////////////////////////////////////////////////////////////////////////

typedef NScheduler::TJobId TJobId;

struct TJobIOConfig;
typedef TIntrusivePtr<TJobIOConfig> TJobIOConfigPtr;

struct TJobProxyConfig;
typedef TIntrusivePtr<TJobProxyConfig> TJobProxyConfigPtr;

struct IUserJobIO;
struct IJob;

struct IDataPipe;
typedef TIntrusivePtr<IDataPipe> TDataPipePtr;

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
