#pragma once

#include <core/misc/common.h>

#include <ytlib/hydra/public.h>

#include <ytlib/transaction_client/public.h>

namespace NYT {
namespace NHive {

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TCellDirectory)

DECLARE_REFCOUNTED_CLASS(TCellDirectoryConfig)

////////////////////////////////////////////////////////////////////////////////

using NHydra::TCellGuid;
using NHydra::NullCellGuid;

using NTransactionClient::TTransactionId;
using NTransactionClient::NullTransactionId;
using NTransactionClient::TTimestamp;
using NTransactionClient::NullTimestamp;

////////////////////////////////////////////////////////////////////////////////

} // namespace NHive
} // namespace NYT
