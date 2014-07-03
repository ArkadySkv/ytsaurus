#pragma once

#include <core/misc/common.h>
#include <core/misc/enum.h>

#include <ytlib/hive/public.h>

namespace NYT {
namespace NHive {

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(THiveManager)

struct TMessage;
class TMailbox;

DECLARE_REFCOUNTED_STRUCT(ITransactionManager)

DECLARE_REFCOUNTED_CLASS(TTransactionSupervisor)

DECLARE_REFCOUNTED_CLASS(THiveManagerConfig)
DECLARE_REFCOUNTED_CLASS(TTransactionSupervisorConfig)

////////////////////////////////////////////////////////////////////////////////

DECLARE_ENUM(ETransactionState,
    ((Active)                     (0))
    ((TransientCommitPrepared)    (1))
    ((PersistentCommitPrepared)   (2))
    ((Committed)                  (3))
    ((TransientAbortPrepared)     (4))
    ((Aborted)                    (5))
);

////////////////////////////////////////////////////////////////////////////////

} // namespace NHive
} // namespace NYT
