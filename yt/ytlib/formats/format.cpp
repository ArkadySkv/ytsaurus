#include "stdafx.h"
#include "format.h"

#include "json_parser.h"
#include "json_writer.h"

#include "dsv_parser.h"
#include "dsv_writer.h"

#include "yamr_parser.h"
#include "yamr_writer.h"

#include "yamred_dsv_parser.h"
#include "yamred_dsv_writer.h"

#include "yson_parser.h"

#include <ytlib/misc/error.h>

#include <ytlib/yson/writer.h>
#include <ytlib/ytree/fluent.h>
#include <ytlib/ytree/forwarding_yson_consumer.h>

namespace NYT {
namespace NFormats {

using namespace NYTree;
using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

TFormat::TFormat()
    : Type_(EFormatType::Null)
{ }

TFormat::TFormat(EFormatType type, const IAttributeDictionary* attributes)
    : Type_(type)
    , Attributes_(attributes ? attributes->Clone() : CreateEphemeralAttributes())
{ }

TFormat::TFormat(const TFormat& other)
    : Type_(other.Type_)
    , Attributes_(other.Attributes_->Clone())
{ }

TFormat& TFormat::operator=(const TFormat& other)
{
    if (this != &other) {
        Type_ = other.Type_;
        Attributes_ = ~other.Attributes_ ? other.Attributes_->Clone() : NULL;
    }
    return *this;
}

const IAttributeDictionary& TFormat::Attributes() const
{
    return *Attributes_;
}

void Serialize(const TFormat& value, IYsonConsumer* consumer)
{
    BuildYsonFluently(consumer)
        .BeginAttributes()
            .Items(value.Attributes())
        .EndAttributes()
        .Value(value.GetType());
}

void Deserialize(TFormat& value, INodePtr node)
{
    if (node->GetType() != ENodeType::String) {
        THROW_ERROR_EXCEPTION("Format can only be parsed from String");
    }

    auto typeStr = node->GetValue<Stroka>();
    EFormatType type;
    try {
        type = ParseEnum<EFormatType>(typeStr);
    } catch (const std::exception& ex) {
        THROW_ERROR_EXCEPTION("Invalid format type: %s",
            ~typeStr);
    }

    value = TFormat(type, &node->Attributes());
}

///////////////////////////////////////////////////////////////////////////////

EYsonType DataTypeToYsonType(EDataType dataType)
{
    switch (dataType) {
        case EDataType::Structured:
            return EYsonType::Node;
        case EDataType::Tabular:
            return EYsonType::ListFragment;
        default:
            THROW_ERROR_EXCEPTION("Data type is not supported by YSON: %s",
                ~FormatEnum(dataType));
    }
}

TAutoPtr<IYsonConsumer> CreateConsumerForYson(
    EDataType dataType,
    const IAttributeDictionary& attributes,
    TOutputStream* output)
{
    class TNewlineAppendingConsumer
        : public TForwardingYsonConsumer
    {
    public:
        explicit TNewlineAppendingConsumer(
            TOutputStream* output,
            TAutoPtr<IYsonConsumer> underlyingConsumer,
            EYsonType ysonType)
            : Output(output)
            , UnderlyingConsumer(underlyingConsumer)
        {
            Forward(
                ~UnderlyingConsumer,
                BIND(&TNewlineAppendingConsumer::OnFinished, this),
                ysonType);
        }

    private:
        TOutputStream* Output;
        TAutoPtr<IYsonConsumer> UnderlyingConsumer;

        void OnFinished()
        {
            Output->Write('\n');
        }
    };

    try {
        auto ysonFormat = attributes.Find<EYsonFormat>("format");
        auto ysonType = DataTypeToYsonType(dataType);
        auto enableRaw = attributes.Find<bool>("enable_raw");
        if (!ysonFormat) {
            ysonFormat = EYsonFormat::Binary;
            enableRaw = true;
        } else {
            if (!enableRaw) {
                // In case of textual format we would like to force textual output.
                enableRaw = (*ysonFormat == EYsonFormat::Binary);
            }
        }

        TAutoPtr<IYsonConsumer> writer(new TYsonWriter(output, *ysonFormat, ysonType, *enableRaw));
        return *ysonFormat == EYsonFormat::Binary
            ? writer
            : new TNewlineAppendingConsumer(output, writer, ysonType);
    } catch (const std::exception& ex) {
        THROW_ERROR_EXCEPTION("Error parsing YSON output format")
            << ex;;
    }
}

TAutoPtr<IYsonConsumer> CreateConsumerForJson(
    EDataType dataType,
    const IAttributeDictionary& attributes,
    TOutputStream* output)
{
    auto config = New<TJsonFormatConfig>();
    config->Load(ConvertToNode(&attributes)->AsMap());
    return new TJsonWriter(output, DataTypeToYsonType(dataType), config);
}

TAutoPtr<IYsonConsumer> CreateConsumerForDsv(
    EDataType dataType,
    const IAttributeDictionary& attributes,
    TOutputStream* output)
{
    auto config = New<TDsvFormatConfig>();
    config->Load(ConvertToNode(&attributes)->AsMap());
    return new TDsvWriter(output, DataTypeToYsonType(dataType), config);
}

TAutoPtr<IYsonConsumer> CreateConsumerForYamr(
    EDataType dataType,
    const IAttributeDictionary& attributes,
    TOutputStream* output)
{
    if (dataType != EDataType::Tabular) {
        THROW_ERROR_EXCEPTION("YAMR is supported only for tabular data");
    }
    auto config = New<TYamrFormatConfig>();
    config->Load(ConvertToNode(&attributes)->AsMap());
    return new TYamrWriter(output, config);
}

TAutoPtr<IYsonConsumer> CreateConsumerForYamredDsv(
    EDataType dataType,
    const IAttributeDictionary& attributes,
    TOutputStream* output)
{
    if (dataType != EDataType::Tabular) {
        ythrow yexception() << Sprintf("Yamred DSV is supported only for tabular data");
    }
    auto config = New<TYamredDsvFormatConfig>();
    config->Load(ConvertToNode(&attributes)->AsMap());
    return new TYamredDsvWriter(output, config);
}


TAutoPtr<IYsonConsumer> CreateConsumerForFormat(const TFormat& format, EDataType dataType, TOutputStream* output)
{
    switch (format.GetType()) {
        case EFormatType::Yson:
            return CreateConsumerForYson(dataType, format.Attributes(), output);
        case EFormatType::Json:
            return CreateConsumerForJson(dataType, format.Attributes(), output);
        case EFormatType::Dsv:
            return CreateConsumerForDsv(dataType, format.Attributes(), output);
        case EFormatType::Yamr:
            return CreateConsumerForYamr(dataType, format.Attributes(), output);
        case EFormatType::YamredDsv:
            return CreateConsumerForYamredDsv(dataType, format.Attributes(), output);
        default:
            THROW_ERROR_EXCEPTION("Unsupported output format: %s",
                ~FormatEnum(format.GetType()));
    }
}

////////////////////////////////////////////////////////////////////////////////

TYsonProducer CreateProducerForDsv(
    EDataType dataType,
    const IAttributeDictionary& attributes,
    TInputStream* input)
{
    if (dataType != EDataType::Tabular) {
        THROW_ERROR_EXCEPTION("DSV is only supported only for tabular data");
    }
    auto config = New<TDsvFormatConfig>();
    config->Load(ConvertToNode(&attributes)->AsMap());
    return BIND([=] (IYsonConsumer* consumer) {
        ParseDsv(input, consumer, config);
    });
}

TYsonProducer CreateProducerForYamr(
    EDataType dataType,
    const IAttributeDictionary& attributes,
    TInputStream* input)
{
    if (dataType != EDataType::Tabular) {
        THROW_ERROR_EXCEPTION("YAMR is only supported only for tabular data");
    }
    auto config = New<TYamrFormatConfig>();
    config->Load(ConvertToNode(&attributes)->AsMap());
    return BIND([=] (IYsonConsumer* consumer) {
        ParseYamr(input, consumer, config);
    });
}

TYsonProducer CreateProducerForYamredDsv(
    EDataType dataType,
    const IAttributeDictionary& attributes,
    TInputStream* input)
{
    if (dataType != EDataType::Tabular) {
        ythrow yexception() << Sprintf("Yamred Dsv is only supported only for tabular data");
    }
    auto config = New<TYamredDsvFormatConfig>();
    config->Load(ConvertToNode(&attributes)->AsMap());
    return BIND([=] (IYsonConsumer* consumer) {
        ParseYamredDsv(input, consumer, config);
    });
}

TYsonProducer CreateProducerForJson(
    EDataType dataType,
    const IAttributeDictionary& attributes,
    TInputStream* input)
{
    if (dataType != EDataType::Structured) {
        THROW_ERROR_EXCEPTION("JSON is only supported only for structured data");
    }
    auto config = New<TJsonFormatConfig>();
    config->Load(ConvertToNode(&attributes)->AsMap());
    return BIND([=] (IYsonConsumer* consumer) {
        ParseJson(input, consumer, config);
    });
}

TYsonProducer CreateProducerForYson(EDataType dataType, TInputStream* input)
{
    auto ysonType = DataTypeToYsonType(dataType);
    return ConvertToProducer(TYsonInput(input, ysonType));
}

TYsonProducer CreateProducerForFormat(const TFormat& format, EDataType dataType, TInputStream* input)
{
    switch (format.GetType()) {
        case EFormatType::Yson:
            return CreateProducerForYson(dataType, input);
        case EFormatType::Json:
            return CreateProducerForJson(dataType, format.Attributes(), input);
        case EFormatType::Dsv:
            return CreateProducerForDsv(dataType, format.Attributes(), input);
        case EFormatType::Yamr:
            return CreateProducerForYamr(dataType, format.Attributes(), input);
        case EFormatType::YamredDsv:
            return CreateProducerForYamredDsv(dataType, format.Attributes(), input);
        default:
            THROW_ERROR_EXCEPTION("Unsupported input format: %s",
                ~FormatEnum(format.GetType()));
    }
}

////////////////////////////////////////////////////////////////////////////////

TAutoPtr<IParser> CreateParserForFormat(const TFormat& format, EDataType dataType, IYsonConsumer* consumer)
{
    switch (format.GetType()) {
        case EFormatType::Yson:
            return CreateParserForYson(consumer, DataTypeToYsonType(dataType));

        case EFormatType::Json: {
            auto config = New<TJsonFormatConfig>();
            config->Load(ConvertToNode(&format.Attributes())->AsMap());
            return new TJsonParser(consumer);
        }
        case EFormatType::Dsv: {
            auto config = New<TDsvFormatConfig>();
            config->Load(ConvertToNode(&format.Attributes())->AsMap());
            return CreateParserForDsv(consumer, config);
        }
        case EFormatType::Yamr: {
            auto config = New<TYamrFormatConfig>();
            config->Load(ConvertToNode(&format.Attributes())->AsMap());
            return CreateParserForYamr(consumer, config);
        }
        case EFormatType::YamredDsv: {
            auto config = New<TYamredDsvFormatConfig>();
            config->Load(ConvertToNode(&format.Attributes())->AsMap());
            return CreateParserForYamredDsv(consumer, config);
        }
        default:
            THROW_ERROR_EXCEPTION("Unsupported input format: %s",
                ~FormatEnum(format.GetType()));
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NFormats
} // namespace NYT
