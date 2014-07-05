#pragma once

#include "public.h"

#include <util/generic/typetraits.h>

namespace NYT {
namespace NHydra {

////////////////////////////////////////////////////////////////////////////////

struct TVersion
{
    i32 SegmentId;
    i32 RecordId;

    TVersion();
    TVersion(i32 segmentId, i32 recordId);

    bool operator < (TVersion other) const;
    bool operator == (TVersion other) const;
    bool operator != (TVersion other) const;
    bool operator > (TVersion other) const;
    bool operator <= (TVersion other) const;
    bool operator >= (TVersion other) const;

    i64 ToRevision() const;
    static TVersion FromRevision(i64 revision);

    bool IsValid() const;

};

extern const TVersion InvalidVersion;

Stroka ToString(TVersion version);

////////////////////////////////////////////////////////////////////////////////

} // namespace NHydra
} // namespace NYT

DECLARE_PODTYPE(NYT::NHydra::TVersion);
