#pragma once

#include <ytlib/ytree/ypath_proxy.h>

#include <ytlib/security_client/group_ypath.pb.h>

namespace NYT {
namespace NSecurityClient {

////////////////////////////////////////////////////////////////////////////////

struct TGroupYPathProxy
    : public NYTree::TYPathProxy
{
    DEFINE_YPATH_PROXY_METHOD(NProto, AddMember);
    DEFINE_YPATH_PROXY_METHOD(NProto, RemoveMember);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NSecurityClient
} // namespace NYT
