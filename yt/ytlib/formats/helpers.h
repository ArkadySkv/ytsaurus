#pragma once

#include "public.h"

#include <ytlib/yson/yson_consumer.h>
#include <ytlib/yson/lexer.h>

namespace NYT {
namespace NFormats {

////////////////////////////////////////////////////////////////////////////////

class TFormatsConsumerBase
    : public virtual NYson::TYsonConsumerBase
{
public:
    virtual void OnStringScalar(const TStringBuf& value) = 0;
    virtual void OnIntegerScalar(i64 value) = 0;
    virtual void OnDoubleScalar(double value) = 0;
    virtual void OnEntity() = 0;
    virtual void OnBeginList() = 0;
    virtual void OnListItem() = 0;
    virtual void OnEndList() = 0;
    virtual void OnBeginMap() = 0;
    virtual void OnKeyedItem(const TStringBuf& key) = 0;
    virtual void OnEndMap() = 0;
    virtual void OnBeginAttributes() = 0;
    virtual void OnEndAttributes() = 0;

    // This method has standard implementation for yamr, dsv and yamred dsv formats.
    virtual void OnRaw(const TStringBuf& yson, NYson::EYsonType type) override;

private:
    NYson::TLexer Lexer;
};

////////////////////////////////////////////////////////////////////////////////
            
} // namespace NFormats
} // namespace NYT
