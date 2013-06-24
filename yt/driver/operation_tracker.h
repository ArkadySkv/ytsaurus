#pragma once

#include "executor.h"

#include <ytlib/misc/nullable.h>

#include <ytlib/driver/driver.h>

#include <ytlib/ytree/yson_string.h>

#include <server/job_proxy/config.h>


namespace NYT {
namespace NDriver {

////////////////////////////////////////////////////////////////////////////////

class TOperationTracker
{
public:
    TOperationTracker(
        TExecutorConfigPtr config,
        NDriver::IDriverPtr driver,
        const NScheduler::TOperationId& operationId);

    EExitCode Run();

private:
    TExecutorConfigPtr Config;
    NDriver::IDriverPtr Driver;
    NScheduler::TOperationId OperationId;
    NScheduler::EOperationType OperationType;
    TNullable<NYTree::TYsonString> PrevProgress;

    static void AppendPhaseProgress(Stroka* out, const Stroka& phase, const NYTree::TYsonString& progress);

    Stroka FormatProgress(const NYTree::TYsonString& progress);
    void DumpProgress();
    EExitCode DumpResult();

    NScheduler::EOperationType GetOperationType(const NScheduler::TOperationId& operationId);

    bool CheckFinished();

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NDriver
} // namespace NYT

