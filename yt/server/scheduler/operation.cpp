#include "stdafx.h"
#include "operation.h"
#include "job.h"
#include "exec_node.h"
#include "operation_controller.h"

#include <ytlib/scheduler/helpers.h>

namespace NYT {
namespace NScheduler {

using namespace NTransactionClient;

////////////////////////////////////////////////////////////////////

TOperation::TOperation(
    const TOperationId& operationId,
    EOperationType type,
    ITransactionPtr userTransaction,
    NYTree::IMapNodePtr spec,
    const Stroka& authenticatedUser,
    TInstant startTime,
    EOperationState state)
    : OperationId_(operationId)
    , Type_(type)
    , State_(state)
    , UserTransaction_(userTransaction)
    , Spec_(spec)
    , AuthenticatedUser_(authenticatedUser)
    , StartTime_(startTime)
    , StdErrCount_(0)
    , MaxStdErrCount_(0)
    , FinishedPromise(NewPromise<void>())
{ }

TFuture<void> TOperation::GetFinished()
{
    return FinishedPromise;
}

void TOperation::SetFinished()
{
    FinishedPromise.Set();
}

bool TOperation::IsFinishedState() const
{
    return IsOperationFinished(State_);
}

bool TOperation::IsFinishingState() const
{
    return IsOperationFinishing(State_);
}

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

