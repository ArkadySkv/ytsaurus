#pragma once

#include <ytlib/transaction_server/transaction_ypath.pb.h>

#include <ytlib/misc/nullable.h>
#include <ytlib/misc/configurable.h>
#include <ytlib/object_server/object_ypath_proxy.h>

namespace NYT {
namespace NTransactionServer {

////////////////////////////////////////////////////////////////////////////////

extern NYTree::TYPath RootTransactionPath;

////////////////////////////////////////////////////////////////////////////////

struct TTransactionManifest
    : public TConfigurable
{
    TNullable<TDuration> Timeout;

    TTransactionManifest()
    {
        Register("timeout", Timeout)
            .Default();
    }
};

////////////////////////////////////////////////////////////////////////////////

struct TTransactionYPathProxy
    : public NObjectServer::TObjectYPathProxy
{
    DEFINE_YPATH_PROXY_METHOD(NProto, Commit);
    DEFINE_YPATH_PROXY_METHOD(NProto, Abort);
    DEFINE_YPATH_PROXY_METHOD(NProto, RenewLease);

    DEFINE_YPATH_PROXY_METHOD(NProto, CreateObject);
    DEFINE_YPATH_PROXY_METHOD(NProto, ReleaseObject);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NTransactionServer
} // namespace NYT
