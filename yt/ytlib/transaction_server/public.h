#pragma once

#include <ytlib/misc/common.h>
#include <ytlib/object_server/id.h>

namespace NYT {
namespace NTransactionServer {

////////////////////////////////////////////////////////////////////////////////

class TTransactionManager;
typedef TIntrusivePtr<TTransactionManager> TTransactionManagerPtr;

class TTransaction;

using NObjectServer::TTransactionId;
using NObjectServer::NullTransactionId;

////////////////////////////////////////////////////////////////////////////////
            
} // namespace NTransactionServer
} // namespace NYT
