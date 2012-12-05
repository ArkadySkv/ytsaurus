#pragma once

#include "public.h"

#include <ytlib/yson/public.h>

#include <ytlib/scheduler/scheduler_service.pb.h>

#include <ytlib/profiling/profiler.h>

#include <server/job_proxy/public.h>

namespace NYT {
namespace NScheduler {

////////////////////////////////////////////////////////////////////////////////

// NB: Types must be numbered from 0 to N - 1.
DECLARE_ENUM(EResourceType,
    (Slots)
    (Cpu)
    (Memory)
    (Network)
);

Stroka FormatResourceUtilization(const NProto::TNodeResources& utilization, const NProto::TNodeResources& limits);
Stroka FormatResources(const NProto::TNodeResources& resources);

void ProfileResources(NProfiling::TProfiler& profiler, const NProto::TNodeResources& resources);

NProto::TNodeResources  operator +  (const NProto::TNodeResources& lhs, const NProto::TNodeResources& rhs);
NProto::TNodeResources& operator += (NProto::TNodeResources& lhs, const NProto::TNodeResources& rhs);

NProto::TNodeResources  operator -  (const NProto::TNodeResources& lhs, const NProto::TNodeResources& rhs);
NProto::TNodeResources& operator -= (NProto::TNodeResources& lhs, const NProto::TNodeResources& rhs);

NProto::TNodeResources  operator *  (const NProto::TNodeResources& lhs, i64 rhs);
NProto::TNodeResources  operator *  (const NProto::TNodeResources& lhs, double rhs);
NProto::TNodeResources& operator *= (NProto::TNodeResources& lhs, i64 rhs);
NProto::TNodeResources& operator *= (NProto::TNodeResources& lhs, double rhs);

bool operator == (const NProto::TNodeResources& a, const NProto::TNodeResources& b);
bool operator != (const NProto::TNodeResources& a, const NProto::TNodeResources& b);

bool Dominates(const NProto::TNodeResources& lhs, const NProto::TNodeResources& rhs);

NProto::TNodeResources Max(const NProto::TNodeResources& a, const NProto::TNodeResources& b);
NProto::TNodeResources Min(const NProto::TNodeResources& a, const NProto::TNodeResources& b);

EResourceType GetDominantResource(
    const NProto::TNodeResources& demand,
    const NProto::TNodeResources& limits);

i64 GetResource(
    const NProto::TNodeResources& resources,
    EResourceType type);

void SetResource(
    NProto::TNodeResources& resources,
    EResourceType type,
    i64 value);

const NProto::TNodeResources& ZeroNodeResources();
const NProto::TNodeResources& InfiniteNodeResources();
const NProto::TNodeResources& LowWatermarkNodeResources();

i64 GetFootprintMemorySize();

i64 GetIOMemorySize(
    TJobIOConfigPtr ioConfig,
    int inputStreamCount,
    int outputStreamCount);

namespace NProto {

void Serialize(
    const NProto::TNodeResources& resources,
    NYson::IYsonConsumer* consumer);

} // namespace NProto

////////////////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT
