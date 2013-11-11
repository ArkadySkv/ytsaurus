﻿#include "stdafx.h"
#include "table_producer.h"
#include "sync_reader.h"

#include <core/yson/consumer.h>

#include <core/ytree/yson_string.h>

namespace NYT {
namespace NTableClient {

using namespace NYson;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

TTableProducer::TTableProducer(
    ISyncReaderPtr reader,
    IYsonConsumer* consumer,
    int tableIndex)
    : Reader(reader)
    , Consumer(consumer)
    , TableIndex(tableIndex)
{ }

bool TTableProducer::ProduceRow()
{
    auto row = Reader->GetRow();
    if (!row) {
        return false;
    }

    int tableIndex = Reader->GetTableIndex();

    if (tableIndex != TableIndex) {
        TableIndex = tableIndex;
        NTableClient::ProduceTableSwitch(Consumer, TableIndex);
    }

    NTableClient::ProduceRow(Consumer, *row);

    return true;
}

////////////////////////////////////////////////////////////////////////////////

void ProduceRow(IYsonConsumer* consumer, const TRow& row)
{
    consumer->OnListItem();

    consumer->OnBeginMap();
    for (const auto& pair : row) {
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
