#pragma once

#include "public.h"

#include <ytlib/ytree/public.h>

namespace NYT {
namespace NScheduler {

////////////////////////////////////////////////////////////////////////////////

NYPath::TYPath GetOperationPath(const TOperationId& operationId);
NYPath::TYPath GetJobsPath(const TOperationId& operationId);
NYPath::TYPath GetJobPath(const TOperationId& operationId, const TJobId& jobId);
NYPath::TYPath GetStdErrPath(const TOperationId& operationId, const TJobId& jobId);

bool IsOperationFinished(EOperationState state);
bool IsOperationFinishing(EOperationState state);
bool IsOperationInProgress(EOperationState state);

////////////////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT
