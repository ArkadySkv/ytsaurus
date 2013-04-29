#pragma once

#include "command.h"

namespace NYT {
namespace NDriver {

////////////////////////////////////////////////////////////////////////////////

struct TStartTransactionRequest
    : public TTransactionalRequest
    , public TMutatingRequest
{
    TNullable<TDuration> Timeout;
    NYTree::INodePtr Attributes;

    TStartTransactionRequest()
    {
        RegisterParameter("timeout", Timeout)
            .Default(Null);
        RegisterParameter("attributes", Attributes)
            .Default(nullptr);
    }
};

typedef TIntrusivePtr<TStartTransactionRequest> TStartRequestPtr;

class TStartTransactionCommand
    : public TTypedCommand<TStartTransactionRequest>
{
private:
    virtual void DoExecute();

};

////////////////////////////////////////////////////////////////////////////////

struct TPingTransactionRequest
    : public TTransactionalRequest
{ };

typedef TIntrusivePtr<TPingTransactionRequest> TRenewRequestPtr;

class TPingTransactionCommand
    : public TTypedCommand<TPingTransactionRequest>
{
private:
    virtual void DoExecute();

};

////////////////////////////////////////////////////////////////////////////////

struct TCommitTransactionRequest
    : public TTransactionalRequest
    , public TMutatingRequest
{ };

typedef TIntrusivePtr<TCommitTransactionRequest> TCommitRequestPtr;

class TCommitTransactionCommand
    : public TTypedCommand<TCommitTransactionRequest>
{
private:
    virtual void DoExecute();

};

////////////////////////////////////////////////////////////////////////////////

struct TAbortTransactionRequest
    : public TTransactionalRequest
    , public TMutatingRequest
{ };

typedef TIntrusivePtr<TAbortTransactionRequest> TAbortTransactionRequestPtr;

class TAbortTransactionCommand
    : public TTypedCommand<TAbortTransactionRequest>
{
private:
    virtual void DoExecute();

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NDriver
} // namespace NYT

