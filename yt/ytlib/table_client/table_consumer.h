﻿#pragma once

#include "public.h"

#include <ytlib/chunk_client/chunk.pb.h>
#include <ytlib/ytree/forwarding_yson_consumer.h>
#include <ytlib/yson/writer.h>
#include <ytlib/yson/lexer.h>
#include <ytlib/misc/blob_output.h>
#include <ytlib/misc/blob_range.h>
#include <ytlib/misc/nullable.h>

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

/*!
 *  For performance reasons we don't use ForwardingConsumer.
 */
class TTableConsumer
    : public NYson::IYsonConsumer
{
public:
    TTableConsumer(const IWriterBasePtr& writer);

private:
    void OnStringScalar(const TStringBuf& value);
    void OnIntegerScalar(i64 value);
    void OnDoubleScalar(double value);
    void OnEntity();
    void OnBeginList();
    void OnListItem();
    void OnBeginMap();
    void OnKeyedItem(const TStringBuf& name);
    void OnEndMap();

    void OnBeginAttributes();

    void ThrowMapExpected();

    void OnEndList();
    void OnEndAttributes();
    void OnRaw(const TStringBuf& yson, NYson::EYsonType type);

    IWriterBasePtr Writer;

    int Depth;

    //! Keeps the current row data.
    TBlobOutput RowBuffer;

    //! |(endColumn, endValue)| offsets in #RowBuffer.
    std::vector<size_t> Offsets;

    NYson::TYsonWriter ValueWriter;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
