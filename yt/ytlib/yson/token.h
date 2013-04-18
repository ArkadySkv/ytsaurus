#pragma once

#include "public.h"

#include <ytlib/misc/property.h>

namespace NYT {
namespace NYson {

////////////////////////////////////////////////////////////////////////////////

DECLARE_ENUM(ETokenType,
    (EndOfStream) // Empty or uninitialized token

    (String)
    (Integer)
    (Double)

    // Special values:
    // YSON
    (Semicolon) // ;
    (Equals) // =
    (Hash) // #
    (LeftBracket) // [
    (RightBracket) // ]
    (LeftBrace) // {
    (RightBrace) // }
    (LeftAngle) // <
    (RightAngle) // >
    // Table ranges
    (LeftParenthesis) // (
    (RightParenthesis) // )
    (Plus) // +
    (Colon) // :
    (Comma) // ,
);

////////////////////////////////////////////////////////////////////////////////

ETokenType CharToTokenType(char ch);        // returns ETokenType::EndOfStream for non-special chars
char TokenTypeToChar(ETokenType type);      // YUNREACHABLE for non-special types
Stroka TokenTypeToString(ETokenType type);  // YUNREACHABLE for non-special types

////////////////////////////////////////////////////////////////////////////////

class TLexerImpl;

////////////////////////////////////////////////////////////////////////////////

class TToken
{
    friend class TLexerImpl;

public:
    static const TToken EndOfStream;

    TToken(ETokenType type = ETokenType::EndOfStream); // for special types
    explicit TToken(const TStringBuf& stringValue); // for strings
    explicit TToken(i64 integerValue); // for integers
    explicit TToken(double doubleValue); // for doubles

    DEFINE_BYVAL_RO_PROPERTY(ETokenType, Type);

    bool IsEmpty() const;
    const TStringBuf& GetStringValue() const;
    i64 GetIntegerValue() const;
    double GetDoubleValue() const;

    Stroka ToString() const;

    void CheckType(ETokenType expectedType) const;
    void CheckType(const std::vector<ETokenType>& expectedTypes) const;

private:
    TStringBuf StringValue;
    i64 IntegerValue;
    double DoubleValue;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYson
} // namespace NYT
