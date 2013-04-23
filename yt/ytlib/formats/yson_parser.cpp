#include "stdafx.h"
#include "parser.h"
#include "yson_parser.h"

#include <ytlib/yson/parser.h>

namespace NYT {
namespace NFormats {

////////////////////////////////////////////////////////////////////////////////

//! Wrapper around yson parser that implements IParser interface.
class TYsonParser
    : public IParser
{
public:
    TYsonParser(
        NYson::IYsonConsumer* consumer,
        NYson::EYsonType type,
        bool enableLinePositionInfo)
        : Parser(consumer, type, enableLinePositionInfo)
    { }

    virtual void Read(const TStringBuf& data) override
    {
        Parser.Read(data);
    }

    virtual void Finish() override
    {
        Parser.Finish();
    }

private:
    NYson::TYsonParser Parser;

};

////////////////////////////////////////////////////////////////////////////////

TAutoPtr<IParser> CreateParserForYson(
    NYson::IYsonConsumer* consumer,
    NYson::EYsonType type,
    bool enableLinePositionInfo)
{
    return new TYsonParser(consumer, type, enableLinePositionInfo);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NFormats
} // namespace NYT