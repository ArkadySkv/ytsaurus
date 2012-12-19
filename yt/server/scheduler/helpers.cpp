#include "stdafx.h"
#include "helpers.h"
#include "operation.h"
#include "job.h"
#include "exec_node.h"
#include "operation_controller.h"
#include "job_resources.h"

#include <ytlib/ytree/fluent.h>

namespace NYT {
namespace NScheduler {

using namespace NYTree;
using namespace NTransactionClient;

////////////////////////////////////////////////////////////////////

void BuildOperationAttributes(TOperationPtr operation, NYson::IYsonConsumer* consumer)
{
    BuildYsonMapFluently(consumer)
        .Item("operation_type").Scalar(operation->GetType())
        .Item("user_transaction_id").Scalar(operation->GetUserTransaction() ? operation->GetUserTransaction()->GetId() : NullTransactionId)
        .Item("scheduler_transaction_id").Scalar(operation->GetUserTransaction() ? operation->GetSchedulerTransaction()->GetId() : NullTransactionId)
        .Item("state").Scalar(FormatEnum(operation->GetState()))
        .Item("start_time").Scalar(operation->GetStartTime())
        .Item("spec").Node(operation->GetSpec());
}

void BuildJobAttributes(TJobPtr job, NYson::IYsonConsumer* consumer)
{
    auto state = job->GetState();
    BuildYsonMapFluently(consumer)
        .Item("job_type").Scalar(FormatEnum(job->GetType()))
        .Item("state").Scalar(FormatEnum(state))
        .Item("address").Scalar(job->GetNode()->GetAddress())
        .DoIf(state == EJobState::Failed, [=] (TFluentMap fluent) {
            auto error = FromProto(job->Result().error());
            fluent.Item("error").Scalar(error);
        });
}

void BuildExecNodeAttributes(TExecNodePtr node, NYson::IYsonConsumer* consumer)
{
    BuildYsonMapFluently(consumer)
        .Item("resource_usage").Scalar(node->ResourceUsage())
        .Item("resource_limits").Scalar(node->ResourceLimits());
}

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

