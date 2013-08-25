#include "stdafx.h"
#include "cypress_commands.h"

#include <ytlib/fibers/fiber.h>

#include <ytlib/object_client/object_service_proxy.h>
#include <ytlib/object_client/master_ypath_proxy.h>

#include <ytlib/cypress_client/cypress_ypath_proxy.h>
#include <ytlib/cypress_client/lock_ypath_proxy.h>

#include <ytlib/ytree/fluent.h>
#include <ytlib/ytree/ypath_proxy.h>
#include <ytlib/ytree/attribute_helpers.h>

#include <ytlib/meta_state/rpc_helpers.h>

namespace NYT {
namespace NDriver {

using namespace NYTree;
using namespace NCypressClient;
using namespace NObjectClient;
using namespace NTransactionClient;

////////////////////////////////////////////////////////////////////////////////

void TGetCommand::DoExecute()
{
    auto req = TYPathProxy::Get(Request->Path.GetPath());
    SetTransactionId(req, EAllowNullTransaction::Yes);
    SetSuppressAccessTracking(req);

    TAttributeFilter attributeFilter(EAttributeFilterMode::MatchingOnly, Request->Attributes);
    ToProto(req->mutable_attribute_filter(), attributeFilter);
    if (Request->MaxSize) {
        req->set_max_size(*Request->MaxSize);
    }

    auto rsp = WaitFor(ObjectProxy->Execute(req));
    THROW_ERROR_EXCEPTION_IF_FAILED(*rsp);

    ReplySuccess(TYsonString(rsp->value()));
}

////////////////////////////////////////////////////////////////////////////////

void TSetCommand::DoExecute()
{
    auto req = TYPathProxy::Set(Request->Path.GetPath());
    SetTransactionId(req, EAllowNullTransaction::Yes);
    GenerateMutationId(req);
    auto producer = Context->CreateInputProducer();
    auto value = ConvertToYsonString(producer);
    req->set_value(value.Data());

    auto rsp = WaitFor(ObjectProxy->Execute(req));
    THROW_ERROR_EXCEPTION_IF_FAILED(*rsp);
}

////////////////////////////////////////////////////////////////////////////////

void TRemoveCommand::DoExecute()
{
    auto req = TYPathProxy::Remove(Request->Path.GetPath());
    SetTransactionId(req, EAllowNullTransaction::Yes);
    GenerateMutationId(req);
    req->set_recursive(Request->Recursive);
    req->set_force(Request->Force);
    req->MutableAttributes()->MergeFrom(Request->GetOptions());

    auto rsp = WaitFor(ObjectProxy->Execute(req));
    THROW_ERROR_EXCEPTION_IF_FAILED(*rsp);
}

//////////////////////////////////////////////////////////////////////////////////

void TListCommand::DoExecute()
{
    auto req = TYPathProxy::List(Request->Path.GetPath());
    SetTransactionId(req, EAllowNullTransaction::Yes);
    SetSuppressAccessTracking(req);

    TAttributeFilter attributeFilter(EAttributeFilterMode::MatchingOnly, Request->Attributes);
    ToProto(req->mutable_attribute_filter(), attributeFilter);
    if (Request->MaxSize) {
        req->set_max_size(*Request->MaxSize);
    }
    
    auto rsp = WaitFor(ObjectProxy->Execute(req));
    THROW_ERROR_EXCEPTION_IF_FAILED(*rsp);

    ReplySuccess(TYsonString(rsp->keys()));
}

//////////////////////////////////////////////////////////////////////////////////

void TCreateCommand::DoExecute()
{
    if (IsVersionedType(Request->Type)) {
        if (!Request->Path) {
            THROW_ERROR_EXCEPTION("Object type is versioned, Cypress path required");
        }

        auto req = TCypressYPathProxy::Create(Request->Path.Get().GetPath());
        SetTransactionId(req, EAllowNullTransaction::Yes);
        GenerateMutationId(req);
        req->set_type(Request->Type);
        req->set_recursive(Request->Recursive);
        req->set_ignore_existing(Request->IgnoreExisting);

        if (Request->Attributes) {
            auto attributes = ConvertToAttributes(Request->Attributes);
            ToProto(req->mutable_node_attributes(), *attributes);
        }

        auto rsp = WaitFor(ObjectProxy->Execute(req));
        THROW_ERROR_EXCEPTION_IF_FAILED(*rsp);

        auto nodeId = FromProto<TNodeId>(rsp->node_id());
        ReplySuccess(BuildYsonStringFluently().Value(nodeId));
    } else {
        if (Request->Path) {
            THROW_ERROR_EXCEPTION("Object type is nonversioned, Cypress path is not required");
        }

        auto transactionId = GetTransactionId(EAllowNullTransaction::Yes);
        auto req = TMasterYPathProxy::CreateObject();
        GenerateMutationId(req);
        if (transactionId != NullTransactionId) {
            ToProto(req->mutable_transaction_id(), transactionId);
        }
        req->set_type(Request->Type);
        if (Request->Attributes) {
            auto attributes = ConvertToAttributes(Request->Attributes);
            ToProto(req->mutable_object_attributes(), *attributes);
        }

        auto rsp = WaitFor(ObjectProxy->Execute(req));
        THROW_ERROR_EXCEPTION_IF_FAILED(*rsp);

        auto objectId = FromProto<TObjectId>(rsp->object_id());
        ReplySuccess(BuildYsonStringFluently().Value(objectId));
    }
}

//////////////////////////////////////////////////////////////////////////////////

void TLockCommand::DoExecute()
{
    auto lockReq = TCypressYPathProxy::Lock(Request->Path.GetPath());
    SetTransactionId(lockReq, EAllowNullTransaction::No);
    GenerateMutationId(lockReq);
    lockReq->set_mode(Request->Mode);
    lockReq->set_waitable(Request->Waitable);

    auto lockRsp = WaitFor(ObjectProxy->Execute(lockReq));
    THROW_ERROR_EXCEPTION_IF_FAILED(*lockRsp);

    auto lockId = FromProto<TLockId>(lockRsp->lock_id());
    ReplySuccess(BuildYsonStringFluently().Value(lockId));
}

////////////////////////////////////////////////////////////////////////////////

void TCopyCommand::DoExecute()
{
    auto req = TCypressYPathProxy::Copy(Request->DestinationPath.GetPath());
    SetTransactionId(req, EAllowNullTransaction::Yes);
    GenerateMutationId(req);
    req->set_source_path(Request->SourcePath.GetPath());

    auto rsp = WaitFor(ObjectProxy->Execute(req));
    THROW_ERROR_EXCEPTION_IF_FAILED(*rsp);

    auto nodeId = FromProto<TNodeId>(rsp->object_id());
    ReplySuccess(BuildYsonStringFluently().Value(nodeId));
}

////////////////////////////////////////////////////////////////////////////////

void TMoveCommand::DoExecute()
{
    {
        auto req = TCypressYPathProxy::Copy(Request->DestinationPath.GetPath());
        SetTransactionId(req, EAllowNullTransaction::Yes);
        GenerateMutationId(req);
        req->set_source_path(Request->SourcePath.GetPath());

        auto rsp = WaitFor(ObjectProxy->Execute(req));
        THROW_ERROR_EXCEPTION_IF_FAILED(*rsp);
    }

    {
        auto req = TYPathProxy::Remove(Request->SourcePath.GetPath());
        req->set_recursive(true);
        this->SetTransactionId(req, EAllowNullTransaction::Yes);
        GenerateMutationId(req);

        auto rsp = WaitFor(ObjectProxy->Execute(req));
        THROW_ERROR_EXCEPTION_IF_FAILED(*rsp);
    }
}

////////////////////////////////////////////////////////////////////////////////

void TExistsCommand::DoExecute()
{
    auto req = TYPathProxy::Exists(Request->Path.GetPath());
    SetTransactionId(req, EAllowNullTransaction::Yes);

    auto rsp = WaitFor(ObjectProxy->Execute(req));
    THROW_ERROR_EXCEPTION_IF_FAILED(*rsp);

    ReplySuccess(BuildYsonStringFluently().Value(rsp->value()));
}

////////////////////////////////////////////////////////////////////////////////

void TLinkCommand::DoExecute()
{
    auto req = TCypressYPathProxy::Create(Request->LinkPath.GetPath());
    req->set_type(EObjectType::Link);
    req->set_recursive(Request->Recursive);
    req->set_ignore_existing(Request->IgnoreExisting);
    this->SetTransactionId(req, EAllowNullTransaction::Yes);
    this->GenerateMutationId(req);

    auto attributes = Request->Attributes ? ConvertToAttributes(Request->Attributes) : CreateEphemeralAttributes();
    attributes->Set("target_path", Request->TargetPath);
    ToProto(req->mutable_node_attributes(), *attributes);

    auto rsp = WaitFor(ObjectProxy->Execute(req));
    THROW_ERROR_EXCEPTION_IF_FAILED(*rsp);

    auto linkId = FromProto<TNodeId>(rsp->node_id());
    ReplySuccess(BuildYsonStringFluently().Value(linkId));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NDriver
} // namespace NYT
