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
NYPath::TYPath GetLivePreviewOutputPath(const TOperationId& operationId, int tableIndex);
NYPath::TYPath GetLivePreviewIntermediatePath(const TOperationId& operationId);
NYPath::TYPath GetSnapshotPath(const TOperationId& operationId);

bool IsOperationFinished(EOperationState state);
bool IsOperationFinishing(EOperationState state);
bool IsOperationInProgress(EOperationState state);
bool IsOperationActive(EOperationState state);

////////////////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT
