#pragma once

#include "public.h"

#include <ytlib/node_tracker_client/node.pb.h>

#include <ytlib/profiling/profiler.h>

#include <ytlib/yson/public.h>

namespace NYT {
namespace NNodeTrackerClient {

////////////////////////////////////////////////////////////////////////////////

// NB: Types must be numbered from 0 to N - 1.
DECLARE_ENUM(EResourceType,
    (UserSlots)
    (Cpu)
    (Memory)
    (Network)
    (ReplicationSlots)
    (RemovalSlots)
    (RepairSlots)
);

Stroka FormatResourceUsage(const NProto::TNodeResources& usage, const NProto::TNodeResources& limits);
Stroka FormatResources(const NProto::TNodeResources& resources);

void ProfileResources(NProfiling::TProfiler& profiler, const NProto::TNodeResources& resources);

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

double GetMinResourceRatio(
    const NProto::TNodeResources& nominator,
    const NProto::TNodeResources& denominator);

const NProto::TNodeResources& ZeroNodeResources();
const NProto::TNodeResources& InfiniteNodeResources();

namespace NProto {

TNodeResources  operator +  (const TNodeResources& lhs, const TNodeResources& rhs);
TNodeResources& operator += (TNodeResources& lhs, const TNodeResources& rhs);

TNodeResources  operator -  (const TNodeResources& lhs, const TNodeResources& rhs);
TNodeResources& operator -= (TNodeResources& lhs, const TNodeResources& rhs);

TNodeResources  operator *  (const TNodeResources& lhs, i64 rhs);
TNodeResources  operator *  (const TNodeResources& lhs, double rhs);
TNodeResources& operator *= (TNodeResources& lhs, i64 rhs);
TNodeResources& operator *= (TNodeResources& lhs, double rhs);

TNodeResources  operator -  (const TNodeResources& resources);

bool operator == (const TNodeResources& a, const TNodeResources& b);
bool operator != (const TNodeResources& a, const TNodeResources& b);

bool Dominates(const NProto::TNodeResources& lhs, const TNodeResources& rhs);

TNodeResources Max(const TNodeResources& a, const TNodeResources& b);
TNodeResources Min(const TNodeResources& a, const TNodeResources& b);

void Serialize(
    const NProto::TNodeResources& resources,
    NYson::IYsonConsumer* consumer);

} // namespace NProto

////////////////////////////////////////////////////////////////////////////////

} // namespace NNodeTrackerClient
} // namespace NYT
