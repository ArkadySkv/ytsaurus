#include "stdafx.h"
#include "channel_reader.h"
#include "value.h"

#include <ytlib/misc/serialize.h>

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

TChannelReader::TChannelReader(const TChannel& channel)
    : Channel(channel)
    , ColumnBuffers(channel.GetColumns().size() + 1)
    , CurrentColumnIndex(-1)
{ }

void TChannelReader::SetBlock(const TSharedRef& block)
{
    YASSERT(CurrentColumnIndex == -1);

    CurrentBlock = block;

    TMemoryInput input(CurrentBlock.Begin(), CurrentBlock.Size());
    std::vector<size_t> columnSizes;
    columnSizes.reserve(Channel.GetColumns().size());
    for (int columnIndex = 0; columnIndex < Channel.GetColumns().size(); ++columnIndex) {
        ui64 size;
        ReadVarUInt64(&input, &size);
        YASSERT(size <= static_cast<ui64>(Max<size_t>()));
        columnSizes.push_back(static_cast<size_t>(size));
    }

    const char* currentPos = input.Buf();
    for (int columnIndex = 0; columnIndex < columnSizes.size(); ++columnIndex) {
        size_t size = columnSizes[columnIndex];
        ColumnBuffers[columnIndex].Reset(currentPos, size);
        currentPos += size;
    }

    ColumnBuffers.back().Reset(currentPos, CurrentBlock.End() - currentPos);
}

bool TChannelReader::NextRow()
{
    if (!CurrentBlock) {
        return false;
    }

    while (NextColumn())
    { }

    CurrentColumn = TStringBuf();
    CurrentValue = TStringBuf();
    CurrentColumnIndex = -1;

    if (ColumnBuffers.front().Avail() == 0) {
        return false;
    }

    return true;
}

bool TChannelReader::NextColumn()
{
    int ColumnBuffersSize = static_cast<int>(ColumnBuffers.size());
    while (true) {
        YASSERT(CurrentColumnIndex <= ColumnBuffersSize);

        if (CurrentColumnIndex == ColumnBuffersSize) {
            return false;
        } else if (CurrentColumnIndex == ColumnBuffersSize - 1) {
            YASSERT(ColumnBuffers.back().Avail() > 0);
            // Processing range column.
            auto& rangeBuffer = ColumnBuffers[CurrentColumnIndex];
            auto value = TValue::Load(&rangeBuffer);
            if (value.IsNull()) {
                ++CurrentColumnIndex;
                return false;
            }
            CurrentValue = value.ToStringBuf();
            i32 nameSize;
            ReadVarInt32(&rangeBuffer, &nameSize);

            if (nameSize < 0) {
                // global key column index, not implemented yet.
                YUNREACHABLE();
            } else {
                CurrentColumn = TStringBuf(rangeBuffer.Buf(), nameSize);
                rangeBuffer.Skip(nameSize);
            }

            return true;
        } 

        YASSERT(ColumnBuffers.back().Avail() > 0);
        ++CurrentColumnIndex;

        if (CurrentColumnIndex < ColumnBuffersSize - 1) {
            // Processing fixed column.
            auto& rangeBuffer = ColumnBuffers[CurrentColumnIndex];
            auto value = TValue::Load(&rangeBuffer);
            if (!value.IsNull()) {
                CurrentValue = value.ToStringBuf();
                return true;
            }
        }
    }
}

TStringBuf TChannelReader::GetColumn() const
{
    YASSERT(CurrentColumnIndex >= 0);

    int ColumnBuffersSize = static_cast<int>(ColumnBuffers.size());
    YASSERT(CurrentColumnIndex < ColumnBuffersSize);

    if (CurrentColumnIndex < ColumnBuffersSize - 1) {
        return Channel.GetColumns()[CurrentColumnIndex];
    } else {
        return CurrentColumn;
    }
}

const TStringBuf& TChannelReader::GetValue() const
{
    YASSERT(CurrentColumnIndex >= 0);
    YASSERT(CurrentColumnIndex <= static_cast<int>(ColumnBuffers.size()));

    return CurrentValue;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
