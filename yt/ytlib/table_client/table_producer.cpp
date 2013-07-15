﻿#include "stdafx.h"
#include "table_producer.h"
#include "sync_reader.h"

#include <ytlib/yson/consumer.h>

#include <ytlib/ytree/yson_string.h>

namespace NYT {
namespace NTableClient {

using namespace NYson;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

TTableProducer::TTableProducer(
    ISyncReaderPtr reader,
    IYsonConsumer* consumer)
    : Reader(reader)
    , Consumer(consumer)
    , TableIndex(Null)
{ }

bool TTableProducer::ProduceRow()
{
    auto row = Reader->GetRow();
    if (!row) {
        return false;
    }

    const auto& tableIndex = Reader->GetTableIndex();

    if (tableIndex != TableIndex) {
        TableIndex = tableIndex;
        YCHECK(tableIndex);
        NTableClient::ProduceTableSwitch(Consumer, *TableIndex);
    }

    NTableClient::ProduceRow(Consumer, *row);

    return true;
}

////////////////////////////////////////////////////////////////////////////////

void ProduceRow(IYsonConsumer* consumer, const TRow& row)
{
    consumer->OnListItem();

    consumer->OnBeginMap();
    FOREACH (const auto& pair, row) {
        consumer->OnKeyedItem(pair.first);
        consumer->OnRaw(pair.second, EYsonType::Node);
    }
    consumer->OnEndMap();
}

void ProduceTableSwitch(IYsonConsumer* consumer, int tableIndex)
{
    static Stroka tableIndexKey = FormatEnum(EControlAttribute(EControlAttribute::TableIndex));

    consumer->OnListItem();
    consumer->OnBeginAttributes();
    consumer->OnKeyedItem(tableIndexKey);
    consumer->OnIntegerScalar(tableIndex);
    consumer->OnEndAttributes();
    consumer->OnEntity();
}

void ProduceYson(ISyncReaderPtr reader, NYson::IYsonConsumer* consumer)
{
    TTableProducer producer(reader, consumer);
    while (producer.ProduceRow());
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
