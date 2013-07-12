#include "stdafx.h"
#include "data_statistics.h"

#include <ytlib/ytree/fluent.h>

namespace NYT {
namespace NChunkClient {

namespace NProto {

using namespace NYTree;
using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

TDataStatistics& operator += (TDataStatistics& lhs, const TDataStatistics& rhs)
{
    lhs.set_uncompressed_data_size(lhs.uncompressed_data_size() + rhs.uncompressed_data_size());
    lhs.set_compressed_data_size(lhs.compressed_data_size() + rhs.compressed_data_size());
    lhs.set_chunk_count(lhs.chunk_count() + rhs.chunk_count());
    lhs.set_row_count(lhs.row_count() + rhs.row_count());
    return lhs;
}

TDataStatistics  operator +  (const TDataStatistics& lhs, const TDataStatistics& rhs)
{
    auto result = lhs;
    result += rhs;
    return result;
}

TDataStatistics& operator -= (TDataStatistics& lhs, const TDataStatistics& rhs)
{
    lhs.set_uncompressed_data_size(lhs.uncompressed_data_size() - rhs.uncompressed_data_size());
    lhs.set_compressed_data_size(lhs.compressed_data_size() - rhs.compressed_data_size());
    lhs.set_chunk_count(lhs.chunk_count() - rhs.chunk_count());
    lhs.set_row_count(lhs.row_count() - rhs.row_count());
    return lhs;
}

TDataStatistics  operator -  (const TDataStatistics& lhs, const TDataStatistics& rhs)
{
    auto result = lhs;
    result -= rhs;
    return result;
}

TDataStatistics GetZeroDataStatistics()
{
    TDataStatistics dataStatistics;
    dataStatistics.set_chunk_count(0);
    dataStatistics.set_row_count(0);
    dataStatistics.set_compressed_data_size(0);
    dataStatistics.set_uncompressed_data_size(0);
    return dataStatistics;
}

const TDataStatistics& ZeroDataStatistics()
{
    static const TDataStatistics dataStatistics = GetZeroDataStatistics();
    return dataStatistics;
}

void Serialize(const TDataStatistics& statistics, NYson::IYsonConsumer* consumer)
{
    BuildYsonFluently(consumer)
        .BeginMap()
            .Item("chunk_count").Value(statistics.chunk_count())
            .Item("row_count").Value(statistics.row_count())
            .Item("uncompressed_data_size").Value(statistics.uncompressed_data_size())
            .Item("compressed_data_size").Value(statistics.compressed_data_size())
        .EndMap();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NProto

} // namespace NChunkClient
} // namespace NYT

