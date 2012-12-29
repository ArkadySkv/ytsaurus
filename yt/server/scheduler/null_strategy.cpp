#include "stdafx.h"
#include "null_strategy.h"
#include "scheduler_strategy.h"
#include "operation.h"
#include "exec_node.h"
#include "job.h"
#include "operation_controller.h"

namespace NYT {
namespace NScheduler {

////////////////////////////////////////////////////////////////////

class TNullStrategy
    : public ISchedulerStrategy
{
public:
    virtual void ScheduleJobs(ISchedulingContext* context) override
    {
        // Refuse to do anything.
        UNUSED(context);
    }

    virtual void BuildOperationProgressYson(TOperationPtr operation, NYson::IYsonConsumer* consumer) override
    {
        UNUSED(operation);
        UNUSED(consumer);
    }

    virtual Stroka GetOperationLoggingProgress(TOperationPtr operation) override
    {
        UNUSED(operation);
        return "";
    }

    virtual void BuildOrchidYson(NYson::IYsonConsumer* consumer) override
    {
        UNUSED(consumer);
    }
};

TAutoPtr<ISchedulerStrategy> CreateNullStrategy(ISchedulerStrategyHost* host)
{
    UNUSED(host);
    return new TNullStrategy();
}

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

