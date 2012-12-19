#pragma once

#include "public.h"

#include <ytlib/misc/property.h>
#include <ytlib/misc/error.h>

#include <ytlib/scheduler/job.pb.h>

namespace NYT {
namespace NScheduler {

////////////////////////////////////////////////////////////////////////////////

typedef TCallback<TVoid(NProto::TJobSpec* jobSpec)> TJobSpecBuilder;

class TJob
    : public TRefCounted
{
    DEFINE_BYVAL_RO_PROPERTY(TJobId, Id);

    DEFINE_BYVAL_RO_PROPERTY(EJobType, Type);

    //! The operation the job belongs to.
    DEFINE_BYVAL_RO_PROPERTY(TOperation*, Operation);
    
    //! Exec node where the job is running.
    DEFINE_BYVAL_RO_PROPERTY(TExecNodePtr, Node);

    //! The time when the job was started.
    DEFINE_BYVAL_RO_PROPERTY(TInstant, StartTime);

    //! The time when the job was finished.
    DEFINE_BYVAL_RW_PROPERTY(TNullable<TInstant>, FinishTime);

    //! Job result returned by node.
    DEFINE_BYREF_RW_PROPERTY(NProto::TJobResult, Result);

    //! Some rough approximation that is updated with every heartbeat.
    DEFINE_BYVAL_RW_PROPERTY(EJobState, State);

    //! Current resources usage limits.
    /*!
     *  Initially captures the limits suggested by the scheduler.
     *  May change afterwards on heartbeats.
     */
    DEFINE_BYREF_RW_PROPERTY(NProto::TNodeResources, ResourceUsage);

    //! Asynchronous spec builder callback.
    DEFINE_BYVAL_RW_PROPERTY(TJobSpecBuilder, SpecBuilder);

public:
    TJob(
        const TJobId& id,
        EJobType type,
        TOperationPtr operation,
        TExecNodePtr node,
        TInstant startTime,
        const NProto::TNodeResources& resourceUsage,
        TJobSpecBuilder specBuilder);

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT
