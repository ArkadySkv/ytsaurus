#include "stdafx.h"
#include "scheduler_commands.h"
#include "config.h"
#include "driver.h"

#include <core/concurrency/fiber.h>

#include <core/rpc/helpers.h>

#include <core/ytree/fluent.h>
#include <core/ytree/ypath_proxy.h>

#include <ytlib/scheduler/config.h>

#include <ytlib/meta_state/rpc_helpers.h>

namespace NYT {
namespace NDriver {

using namespace NScheduler;
using namespace NYTree;
using namespace NMetaState;
using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

void TSchedulerCommandBase::StartOperation(EOperationType type)
{
    auto req = SchedulerProxy->StartOperation();
    req->set_type(type);
    ToProto(req->mutable_transaction_id(), GetTransactionId(EAllowNullTransaction::Yes));
    GenerateMutationId(req);
    req->set_spec(ConvertToYsonString(Request->Spec).Data());

    auto rsp = WaitFor(req->Invoke());
    THROW_ERROR_EXCEPTION_IF_FAILED(*rsp);

    auto operationId = FromProto<TOperationId>(rsp->operation_id());
    Reply(BuildYsonStringFluently().Value(operationId));
}

////////////////////////////////////////////////////////////////////////////////

void TMapCommand::DoExecute()
{
    StartOperation(EOperationType::Map);
}

////////////////////////////////////////////////////////////////////////////////

void TMergeCommand::DoExecute()
{
    StartOperation(EOperationType::Merge);
}

////////////////////////////////////////////////////////////////////////////////

void TSortCommand::DoExecute()
{
    StartOperation(EOperationType::Sort);
}

////////////////////////////////////////////////////////////////////////////////

void TEraseCommand::DoExecute()
{
    StartOperation(EOperationType::Erase);
}

////////////////////////////////////////////////////////////////////////////////

void TReduceCommand::DoExecute()
{
    StartOperation(EOperationType::Reduce);
}

////////////////////////////////////////////////////////////////////////////////

void TMapReduceCommand::DoExecute()
{
    StartOperation(EOperationType::MapReduce);
}

////////////////////////////////////////////////////////////////////////////////

void TRemoteCopyCommand::DoExecute()
{
    StartOperation(EOperationType::RemoteCopy);
}

////////////////////////////////////////////////////////////////////////////////

void TAbortOperationCommand::DoExecute()
{
    TSchedulerServiceProxy proxy(Context->GetSchedulerChannel());
    auto req = proxy.AbortOperation();
    ToProto(req->mutable_operation_id(), Request->OperationId);

    auto rsp = WaitFor(req->Invoke());
    THROW_ERROR_EXCEPTION_IF_FAILED(*rsp);
}

////////////////////////////////////////////////////////////////////////////////

void TSuspendOperationCommand::DoExecute()
{
    TSchedulerServiceProxy proxy(Context->GetSchedulerChannel());
    auto req = proxy.SuspendOperation();
    ToProto(req->mutable_operation_id(), Request->OperationId);

    auto rsp = WaitFor(req->Invoke());
    THROW_ERROR_EXCEPTION_IF_FAILED(*rsp);
}

////////////////////////////////////////////////////////////////////////////////

void TResumeOperationCommand::DoExecute()
{
    TSchedulerServiceProxy proxy(Context->GetSchedulerChannel());
    auto req = proxy.ResumeOperation();
    ToProto(req->mutable_operation_id(), Request->OperationId);

    auto rsp = WaitFor(req->Invoke());
    THROW_ERROR_EXCEPTION_IF_FAILED(*rsp);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NDriver
} // namespace NYT
