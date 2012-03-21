﻿#pragma once

#include "common.h"
#include "sync_reader.h"

#include <ytlib/ytree/yson_writer.h>
#include <util/stream/output.h>

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////

class TYsonTableInput
{
public:
    TYsonTableInput(
        ISyncReader* reader,
        NYTree::EYsonFormat format,
        TOutputStream* outputStream);

    bool ReadRow();

private:

    TIntrusivePtr<ISyncReader> Reader;
    NYTree::TYsonWriter YsonWriter;
};

////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
