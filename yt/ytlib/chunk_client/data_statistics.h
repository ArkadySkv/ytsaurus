#pragma once

#include <ytlib/chunk_client/data_statistics.pb.h>
#include <ytlib/yson/public.h>

namespace NYT {
namespace NChunkClient {

////////////////////////////////////////////////////////////////////////////////

namespace NProto {

TDataStatistics& operator += (TDataStatistics& lhs, const TDataStatistics& rhs);
TDataStatistics  operator +  (const TDataStatistics& lhs, const TDataStatistics& rhs);

TDataStatistics& operator -= (TDataStatistics& lhs, const TDataStatistics& rhs);
TDataStatistics  operator -  (const TDataStatistics& lhs, const TDataStatistics& rhs);

void Serialize(const TDataStatistics& statistics, NYson::IYsonConsumer* consumer);

const TDataStatistics& ZeroDataStatistics();

}

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT

