﻿#pragma once

#include "sync_writer.h"

#include <ytlib/ytree/yson_consumer.h>

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

class TRowConsumer
    : public NYTree::IYsonConsumer
{
public:
    TRowConsumer(ISyncTableWriter* writer);

private:
    virtual void OnStringScalar(const Stroka& value, bool hasAttributes);
    virtual void OnInt64Scalar(i64 value, bool hasAttributes);
    virtual void OnDoubleScalar(double value, bool hasAttributes);
    virtual void OnEntity(bool hasAttributes);
    virtual void OnBeginList();
    virtual void OnListItem();
    virtual void OnEndList(bool hasAttributes);
    virtual void OnBeginMap();
    virtual void OnMapItem(const Stroka& name);
    virtual void OnEndMap(bool hasAttributes);
    virtual void OnBeginAttributes();
    virtual void OnAttributesItem(const Stroka& name);
    virtual void OnEndAttributes();
    void CheckNoAttributes(bool hasAttributes);
    void CheckInsideRow();

    ISyncTableWriter* Writer;
    int RowIndex;
    bool InsideRow;
    TColumn Column;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
