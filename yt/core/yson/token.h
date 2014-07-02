#pragma once

#include "public.h"

#include <core/misc/property.h>

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
public:
    static const TToken EndOfStream;

    TToken();
    TToken(ETokenType type); // for special types
    explicit TToken(const TStringBuf& stringValue); // for strings
    explicit TToken(i64 integerValue); // for integers
    explicit TToken(double doubleValue); // for doubles

    DEFINE_BYVAL_RO_PROPERTY(ETokenType, Type);

    bool IsEmpty() const;
    const TStringBuf& GetStringValue() const;
    i64 GetIntegerValue() const;
    double GetDoubleValue() const;

    void CheckType(ETokenType expectedType) const;
    void CheckType(const std::vector<ETokenType>& expectedTypes) const;
    void Reset();

private:
    friend class TLexerImpl;

    TStringBuf StringValue;
    i64 IntegerValue;
    double DoubleValue;

};

Stroka ToString(const TToken& token);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYson
} // namespace NYT
