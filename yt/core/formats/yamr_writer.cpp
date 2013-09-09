#include "stdafx.h"
#include "yamr_writer.h"

#include <core/misc/error.h>

#include <core/yson/format.h>

namespace NYT {
namespace NFormats {

using namespace NYTree;
using namespace NTableClient;

////////////////////////////////////////////////////////////////////////////////

TYamrWriter::TYamrWriter(TOutputStream* stream, TYamrFormatConfigPtr config)
    : Stream(stream)
    , Config(config)
    , Table(
        Config->FieldSeparator,
        Config->RecordSeparator,
        Config->EnableEscaping, // Enable key escaping
        Config->EnableEscaping, // Enable value escaping
        Config->EscapingSymbol,
        true)
    , State(EState::None)
{
    YCHECK(Config);
    YCHECK(Stream);
}

TYamrWriter::~TYamrWriter()
{ }

void TYamrWriter::OnIntegerScalar(i64 value)
{
    if (State == EState::ExpectValue) {
        THROW_ERROR_EXCEPTION("Integer values are not supported by YAMR");
    }
    YASSERT(State == EState::ExpectAttributeValue);

    switch (ControlAttribute) {
    case EControlAttribute::TableIndex:
        if (!Config->EnableTableIndex) {
            // Silently ignore table switches.
            break;
        }

        if (Config->Lenval) {
            WritePod(*Stream, static_cast<ui32>(-1));
            WritePod(*Stream, static_cast<ui32>(value));
        } else {
            Stream->Write(ToString(value));
            Stream->Write(Config->RecordSeparator);
        }
        break;

    default:
        YUNREACHABLE();
    }

    State = EState::ExpectEndAttributes;
}

void TYamrWriter::OnDoubleScalar(double value)
{
    YASSERT(State == EState::ExpectValue || State == EState::ExpectAttributeValue);
    THROW_ERROR_EXCEPTION("Double values are not supported by YAMR");
}

void TYamrWriter::OnStringScalar(const TStringBuf& value)
{
    YCHECK(State != EState::ExpectAttributeValue);
    YASSERT(State == EState::ExpectValue);

    switch (ValueType) {
        case EValueType::ExpectKey:
            Key = value;
            break;

        case EValueType::ExpectSubkey:
            Subkey = value;
            break;

        case EValueType::ExpectValue:
            Value = value;
            break;

        case EValueType::ExpectUnknown:
            //Ignore unknows columns.
            break;

        default:
            YUNREACHABLE();
    }

    State = EState::ExpectColumnName;
}

void TYamrWriter::OnEntity()
{
    if (State == EState::ExpectValue) {
        THROW_ERROR_EXCEPTION("Entities are not supported by YAMR");
    }

    YASSERT(State == EState::ExpectEntity);
    State = EState::None;
}

void TYamrWriter::OnBeginList()
{
    YASSERT(State == EState::ExpectValue);
    THROW_ERROR_EXCEPTION("Lists are not supported by YAMR");
}

void TYamrWriter::OnListItem()
{
    YASSERT(State == EState::None);
}

void TYamrWriter::OnEndList()
{
    YUNREACHABLE();
}

void TYamrWriter::OnBeginMap()
{
    if (State == EState::ExpectValue) {
        THROW_ERROR_EXCEPTION("Embedded maps are not supported by YAMR");
    }
    YASSERT(State == EState::None);
    State = EState::ExpectColumnName;

    Key = Null;
    Subkey = Null;
    Value = Null;
}

void TYamrWriter::OnKeyedItem(const TStringBuf& key)
{
    switch (State) {
    case EState::ExpectColumnName:
        if (key == Config->Key) {
            ValueType = EValueType::ExpectKey;
        } else if (key == Config->Subkey) {
            ValueType = EValueType::ExpectSubkey;
        } else if (key == Config->Value) {
            ValueType = EValueType::ExpectValue;
        } else {
            ValueType = EValueType::ExpectUnknown;
        }

        State = EState::ExpectValue;
        break;

    case EState::ExpectAttributeName:
        ControlAttribute = ParseEnum<EControlAttribute>(ToString(key));
        State = EState::ExpectAttributeValue;
        break;

    case EState::None:
    case EState::ExpectValue:
    case EState::ExpectAttributeValue:
    case EState::ExpectEntity:
    case EState::ExpectEndAttributes:
    default:
        YUNREACHABLE();
    }
}

void TYamrWriter::OnEndMap()
{
    YASSERT(State == EState::ExpectColumnName);
    State = EState::None;

    WriteRow();
}

void TYamrWriter::OnBeginAttributes()
{
    if (State == EState::ExpectValue) {
        THROW_ERROR_EXCEPTION("Attributes are not supported by YAMR");
    }

    YASSERT(State == EState::None);
    State = EState::ExpectAttributeName;
}

void TYamrWriter::OnEndAttributes()
{
    YASSERT(State == EState::ExpectEndAttributes);
    State = EState::ExpectEntity;
}

void TYamrWriter::WriteRow()
{
    if (!Key) {
        THROW_ERROR_EXCEPTION("Missing column %s in YAMR record",
            ~Config->Key.Quote());
    }

    if (!Value) {
        THROW_ERROR_EXCEPTION("Missing column %s in YAMR record",
            ~Config->Value.Quote());
    }

    TStringBuf key = *Key;
    TStringBuf subkey = Subkey ? *Subkey : "";
    TStringBuf value = *Value;

    if (!Config->Lenval) {
        EscapeAndWrite(key, true);
        Stream->Write(Config->FieldSeparator);
        if (Config->HasSubkey) {
            EscapeAndWrite(subkey, true);
            Stream->Write(Config->FieldSeparator);
        }
        EscapeAndWrite(value, false);
        Stream->Write(Config->RecordSeparator);
    } else {
        WriteInLenvalMode(key);
        if (Config->HasSubkey) {
            WriteInLenvalMode(subkey);
        }
        WriteInLenvalMode(value);
    }
}

void TYamrWriter::WriteInLenvalMode(const TStringBuf& value)
{
    WritePod(*Stream, static_cast<ui32>(value.size()));
    Stream->Write(value);
}

void TYamrWriter::EscapeAndWrite(const TStringBuf& value, bool inKey)
{
    if (Config->EnableEscaping) {
        WriteEscaped(
            Stream,
            value,
            inKey ? Table.KeyStops : Table.ValueStops,
            Table.Escapes,
            Config->EscapingSymbol);
    } else {
        Stream->Write(value);
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NFormats
} // namespace NYT
