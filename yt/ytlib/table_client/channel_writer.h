#pragma once

#include "common.h"
#include "value.h"
#include "schema.h"

#include <ytlib/misc/blob_output.h>

namespace NYT {
namespace NTableClient {

///////////////////////////////////////////////////////////////////////////////

class TChannelWriter
    : public TRefCounted
{
public:
    typedef TIntrusivePtr<TChannelWriter> TPtr;

    TChannelWriter(
        const TChannel& channel,
        const yhash_map<TColumn, int>& columnIndexes);

    void Write(
        int chunkColumnIndex,
        const TColumn& column,
        TValue value);

    void EndRow();

    size_t GetCurrentSize() const;
    int GetCurrentRowCount() const;
    bool HasUnflushedData() const;

    TSharedRef FlushBlock();

    static const int UnknownIndex;
    static const int RangeIndex;

private:
    //! Size reserved for column offsets
    size_t GetEmptySize() const;

    TChannel Channel;

    /*! 
     *  Mapping from chunk column indexes to channel column indexes.
     *  UnknownIndex - channel doesn't contain column.
     *  RangeIndex - channel contains column in ranges.
     */
    std::vector<int> ColumnIndexMapping;

    //! Current buffers for fixed columns.
    yvector<TBlobOutput> FixedColumns;

    //! Current buffer for range columns.
    TBlobOutput RangeColumns;

    //! Is fixed column with corresponding index already set in the current row.
    yvector<bool> IsColumnUsed;

    //! Overall size of current buffers.
    size_t CurrentSize;

    //! Number of rows in the current unflushed buffer.
    int CurrentRowCount;
};

///////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
