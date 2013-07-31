#include "stdafx.h"
#include "serialization_context.h"

namespace NYT {
namespace NScheduler {

////////////////////////////////////////////////////////////////////

int GetCurrentSnapshotVersion()
{
    return 9;
}

bool ValidateSnapshotVersion(int version)
{
    return version == 9;
}

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

