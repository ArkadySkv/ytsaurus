#include "stdafx.h"

#include <ytlib/ytree/yson_serializable.h>

namespace NYT {
namespace NYTree {

////////////////////////////////////////////////////////////////////////////////

NYson::EYsonType GetYsonType(const TYsonString& yson)
{
    return yson.GetType();
}

NYson::EYsonType GetYsonType(const TYsonInput& input)
{
    return input.GetType();
}

NYson::EYsonType GetYsonType(const TYsonProducer& producer)
{
    return producer.GetType();
}

////////////////////////////////////////////////////////////////////////////////

void Serialize(short value, NYson::IYsonConsumer* consumer)
{
    consumer->OnIntegerScalar(CheckedStaticCast<i64>(value));
}

void Serialize(unsigned short value, NYson::IYsonConsumer* consumer)
{
    consumer->OnIntegerScalar(CheckedStaticCast<i64>(value));
}

void Serialize(int value, NYson::IYsonConsumer* consumer)
{
    consumer->OnIntegerScalar(CheckedStaticCast<i64>(value));
}

void Serialize(unsigned int value, NYson::IYsonConsumer* consumer)
{
    consumer->OnIntegerScalar(CheckedStaticCast<i64>(value));
}

void Serialize(long value, NYson::IYsonConsumer* consumer)
{
    consumer->OnIntegerScalar(CheckedStaticCast<i64>(value));
}

void Serialize(unsigned long value, NYson::IYsonConsumer* consumer)
{
    consumer->OnIntegerScalar(CheckedStaticCast<i64>(value));
}

void Serialize(long long value, NYson::IYsonConsumer* consumer)
{
    consumer->OnIntegerScalar(CheckedStaticCast<i64>(value));
}

void Serialize(unsigned long long value, NYson::IYsonConsumer* consumer)
{
    consumer->OnIntegerScalar(CheckedStaticCast<i64>(value));
}

// double
void Serialize(double value, NYson::IYsonConsumer* consumer)
{
    consumer->OnDoubleScalar(value);
}

// Stroka
void Serialize(const Stroka& value, NYson::IYsonConsumer* consumer)
{
    consumer->OnStringScalar(value);
}

// TStringBuf
void Serialize(const TStringBuf& value, NYson::IYsonConsumer* consumer)
{
    consumer->OnStringScalar(value);
}

// const char*
void Serialize(const char* value, NYson::IYsonConsumer* consumer)
{
    consumer->OnStringScalar(TStringBuf(value));
}

// bool
void Serialize(bool value, NYson::IYsonConsumer* consumer)
{
    consumer->OnStringScalar(FormatBool(value));
}

// char
void Serialize(char value, NYson::IYsonConsumer* consumer)
{
    consumer->OnStringScalar(Stroka(value));
}

// TDuration
void Serialize(TDuration value, NYson::IYsonConsumer* consumer)
{
    consumer->OnIntegerScalar(value.MilliSeconds());
}

// TInstant
void Serialize(TInstant value, NYson::IYsonConsumer* consumer)
{
    consumer->OnStringScalar(value.ToString());
}

// TGuid
void Serialize(const TGuid& value, NYson::IYsonConsumer* consumer)
{
    consumer->OnStringScalar(ToString(value));
}

// TInputStream
void Serialize(TInputStream& input, NYson::IYsonConsumer* consumer)
{
    Serialize(TYsonInput(&input), consumer);
}

////////////////////////////////////////////////////////////////////////////////

// i64
void Deserialize(i64& value, INodePtr node)
{
    value = CheckedStaticCast<i64>(node->AsInteger()->GetValue());
}

// ui64
void Deserialize(ui64& value, INodePtr node)
{
    value = CheckedStaticCast<ui64>(node->AsInteger()->GetValue());
}

// i32
void Deserialize(i32& value, INodePtr node)
{
    value = CheckedStaticCast<i32>(node->AsInteger()->GetValue());
}

// ui32
void Deserialize(ui32& value, INodePtr node)
{
    value = CheckedStaticCast<ui32>(node->AsInteger()->GetValue());
}

// i16
void Deserialize(i16& value, INodePtr node)
{
    value = CheckedStaticCast<i16>(node->AsInteger()->GetValue());
}

// ui16
void Deserialize(ui16& value, INodePtr node)
{
    value = CheckedStaticCast<ui16>(node->AsInteger()->GetValue());
}

// double
void Deserialize(double& value, INodePtr node)
{
    value = node->AsDouble()->GetValue();
}

// Stroka
void Deserialize(Stroka& value, INodePtr node)
{
    value = node->AsString()->GetValue();
}

// bool
void Deserialize(bool& value, INodePtr node)
{
    Stroka stringValue = node->AsString()->GetValue();
    value = ParseBool(stringValue);
}

// char
void Deserialize(char& value, INodePtr node)
{
    Stroka stringValue = node->AsString()->GetValue();
    if (stringValue.size() != 1) {
        THROW_ERROR_EXCEPTION("Expected string of length 1 but found of length %" PRISZT, stringValue.size());
    }
    value = stringValue[0];
}

// TDuration
void Deserialize(TDuration& value, INodePtr node)
{
    value = TDuration::MilliSeconds(node->AsInteger()->GetValue());
}

// TInstant
void Deserialize(TInstant& value, INodePtr node)
{
    if (node->GetType() == ENodeType::Integer) {
        value = TInstant::MilliSeconds(node->AsInteger()->GetValue());
    } else {
        value = TInstant::ParseIso8601(node->AsString()->GetValue());
    }
}

// TGuid
void Deserialize(TGuid& value, INodePtr node)
{
    value = TGuid::FromString(node->AsString()->GetValue());
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT
