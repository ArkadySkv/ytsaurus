#include "node.h"
#include "error.h"
#include "stream_stack.h"

#include <core/ytree/node.h>
#include <core/ytree/tree_builder.h>
#include <core/ytree/yson_string.h>
#include <core/ytree/ephemeral_node_factory.h>
#include <core/ytree/convert.h>
#include <core/ytree/ypath_client.h>

#include <core/yson/consumer.h>
#include <core/yson/writer.h>

#include <ytlib/formats/format.h>

#include <util/stream/zlib.h>
#include <util/stream/lz.h>

#include <util/string/base64.h>

namespace NYT {
namespace NNodeJS {

////////////////////////////////////////////////////////////////////////////////

COMMON_V8_USES

using namespace NYTree;
using namespace NYson;
using namespace NFormats;

////////////////////////////////////////////////////////////////////////////////

namespace {

static Persistent<String> SpecialValueKey;
static Persistent<String> SpecialAttributesKey;

static const char SpecialBase64Marker = '&';

// Declare.
void ConsumeV8Array(Handle<Array> array, ITreeBuilder* builder);
void ConsumeV8Object(Handle<Object> object, ITreeBuilder* builder);
void ConsumeV8ObjectProperties(Handle<Object> object, ITreeBuilder* builder);
void ConsumeV8Value(Handle<Value> value, ITreeBuilder* builder);

Handle<Value> ProduceV8(const INodePtr& node);

// Define.
void ConsumeV8Array(Handle<Array> array, ITreeBuilder* builder)
{
    THREAD_AFFINITY_IS_V8();

    builder->OnBeginList();

    for (ui32 i = 0; i < array->Length(); ++i) {
        builder->OnListItem();
        ConsumeV8Value(array->Get(i), builder);
    }

    builder->OnEndList();
}

void ConsumeV8Object(Handle<Object> object, ITreeBuilder* builder)
{
    THREAD_AFFINITY_IS_V8();

    if (object->Has(SpecialValueKey)) {
        auto value = object->Get(SpecialValueKey);
        if (object->Has(SpecialAttributesKey)) {
            auto attributes = object->Get(SpecialAttributesKey);
            if (!attributes->IsObject()) {
                THROW_ERROR_EXCEPTION("Attributes have to be a V8 object");
                return;
            }

            builder->OnBeginAttributes();
            ConsumeV8ObjectProperties(attributes->ToObject(), builder);
            builder->OnEndAttributes();
        }

        ConsumeV8Value(value, builder);
    } else {
        builder->OnBeginMap();
        ConsumeV8ObjectProperties(object, builder);
        builder->OnEndMap();
    }
}

void ConsumeV8ObjectProperties(Handle<Object> object, ITreeBuilder* builder)
{
    THREAD_AFFINITY_IS_V8();

    Local<Array> properties = object->GetOwnPropertyNames();
    for (ui32 i = 0; i < properties->Length(); ++i) {
        Local<String> key = properties->Get(i)->ToString();
        String::AsciiValue keyValue(key);

        builder->OnKeyedItem(TStringBuf(*keyValue, keyValue.length()));
        ConsumeV8Value(object->Get(key), builder);
    }
}

void ConsumeV8Value(Handle<Value> value, ITreeBuilder* builder)
{
    THREAD_AFFINITY_IS_V8();

    /****/ if (value->IsString()) {
        String::AsciiValue asciiValue(value->ToString());
        TStringBuf string(*asciiValue, asciiValue.length());

        if (string.length() >= 1 && string[0] == SpecialBase64Marker) {
            builder->OnStringScalar(Base64Decode(string.Tail(1)));
        } else {
            builder->OnStringScalar(string);
        }
    } else if (value->IsNumber()) {
        if (value->IsInt32() || value->IsUint32()) {
            builder->OnIntegerScalar(value->IntegerValue());
        } else {
            builder->OnDoubleScalar(value->NumberValue());
        }
    } else if (value->IsObject()) {
        if (TNodeWrap::HasInstance(value)) {
            builder->OnNode(CloneNode(TNodeWrap::UnwrapNode(value)));
            return;
        }
        if (value->IsArray()) {
            ConsumeV8Array(
                Local<Array>::Cast(Local<Value>::New(value)),
                builder);
        } else {
            ConsumeV8Object(
                Local<Object>::Cast(Local<Value>::New(value)),
                builder);
        }
    } else if (value->IsBoolean()) {
        builder->OnStringScalar(value->BooleanValue() ? "true" : "false");
    } else {
        String::AsciiValue asciiValue(value);
        THROW_ERROR_EXCEPTION(
            "Unsupported JS value type within V8-to-YSON conversion: %s",
            *asciiValue);
    }
}

Handle<Value> ProduceV8(const INodePtr& node)
{
    THREAD_AFFINITY_IS_V8();

    if (!node) {
        return v8::Undefined();
    }

    switch (node->GetType()) {
        case ENodeType::String: {
            auto value = node->GetValue<Stroka>();
            return String::New(value.c_str(), value.length());
        }
        case ENodeType::Integer: {
            return Integer::New(node->GetValue<i64>());
        }
        case ENodeType::Double: {
            return Number::New(node->GetValue<double>());
        }
        case ENodeType::Map: {
            auto children = node->AsMap()->GetChildren();
            auto result = Object::New();
            FOREACH (const auto& pair, children) {
                const auto& key = pair.first;
                const auto& value = pair.second;
                result->Set(
                    String::New(key.c_str(), key.length()),
                    ProduceV8(value));
            }
            return result;
        }
        case ENodeType::List: {
            auto children = node->AsList()->GetChildren();
            auto result = Array::New(children.size());
            for (size_t i = 0; i < children.size(); ++i) {
                result->Set(
                    Integer::New(i),
                    ProduceV8(children[i]));
            }
            return result;
        }
        default: {
            return v8::Null();
        }
    }
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

INodePtr ConvertV8ValueToNode(Handle<Value> value)
{
    THREAD_AFFINITY_IS_V8();
    HandleScope scope;

    auto builder = CreateBuilderFromFactory(GetEphemeralNodeFactory());
    builder->BeginTree();
    ConsumeV8Value(value, ~builder);
    return builder->EndTree();
}

INodePtr ConvertV8BytesToNode(const char* buffer, size_t length, ECompression compression, INodePtr format)
{
    TMemoryInput baseStream(buffer, length);
    TGrowingStreamStack<TInputStream, 2> streamStack(&baseStream);

    switch (compression) {
        case ECompression::None:
            break;
        case ECompression::Gzip:
        case ECompression::Deflate:
            streamStack.Add<TZLibDecompress>();
            break;
        case ECompression::LZO:
            streamStack.Add<TLzoDecompress>();
            break;
        case ECompression::LZF:
            streamStack.Add<TLzfDecompress>();
            break;
        case ECompression::Snappy:
            streamStack.Add<TSnappyDecompress>();
            break;
        default:
            YUNREACHABLE();
    }

    return ConvertToNode(CreateProducerForFormat(
        ConvertTo<TFormat>(std::move(format)),
        EDataType::Structured,
        streamStack.Top()));
}

Handle<Value> ConvertNodeToV8Value(const INodePtr& node)
{
    THREAD_AFFINITY_IS_V8();
    HandleScope scope;

    return scope.Close(ProduceV8(node));
}

////////////////////////////////////////////////////////////////////////////////

Persistent<FunctionTemplate> TNodeWrap::ConstructorTemplate;

TNodeWrap::TNodeWrap(INodePtr node)
    : node::ObjectWrap()
    , Node_(std::move(node))
{
    THREAD_AFFINITY_IS_V8();
}

TNodeWrap::~TNodeWrap() throw()
{
    THREAD_AFFINITY_IS_V8();
}

////////////////////////////////////////////////////////////////////////////////

void TNodeWrap::Initialize(Handle<Object> target)
{
    THREAD_AFFINITY_IS_V8();
    HandleScope scope;

    SpecialValueKey = NODE_PSYMBOL("$value");
    SpecialAttributesKey = NODE_PSYMBOL("$attributes");

    ConstructorTemplate = Persistent<FunctionTemplate>::New(
        FunctionTemplate::New(TNodeWrap::New));

    ConstructorTemplate->InstanceTemplate()->SetInternalFieldCount(1);
    ConstructorTemplate->SetClassName(String::NewSymbol("TNodeWrap"));

    NODE_SET_PROTOTYPE_METHOD(ConstructorTemplate, "Print", TNodeWrap::Print);
    NODE_SET_PROTOTYPE_METHOD(ConstructorTemplate, "Traverse", TNodeWrap::Traverse);
    NODE_SET_PROTOTYPE_METHOD(ConstructorTemplate, "Get", TNodeWrap::Get);

    target->Set(
        String::NewSymbol("TNodeWrap"),
        ConstructorTemplate->GetFunction());

    target->Set(
        String::NewSymbol("CreateMergedNode"),
        FunctionTemplate::New(TNodeWrap::CreateMerged)->GetFunction());

    target->Set(
        String::NewSymbol("CreateV8Node"),
        FunctionTemplate::New(TNodeWrap::CreateV8)->GetFunction());
}

bool TNodeWrap::HasInstance(Handle<Value> value)
{
    THREAD_AFFINITY_IS_V8();
    HandleScope scope;

    return
        value->IsObject() &&
        ConstructorTemplate->HasInstance(value->ToObject());
}

INodePtr TNodeWrap::UnwrapNode(Handle<Value> value)
{
    THREAD_AFFINITY_IS_V8();
    HandleScope scope;

    return ObjectWrap::Unwrap<TNodeWrap>(value->ToObject())->GetNode();
}

Handle<Value> TNodeWrap::New(const Arguments& args)
{
    THREAD_AFFINITY_IS_V8();
    HandleScope scope;

    try {
        INodePtr node;

        /****/ if (args.Length() == 0) {
            node = NULL;
        } else if (args.Length() == 1) {
            auto arg = args[0];
            if (arg->IsObject()) {
                node = ConvertV8ValueToNode(arg);
            } else if (arg->IsString()) {
                String::AsciiValue argValue(arg->ToString());
                node = ConvertToNode(TYsonString(Stroka(*argValue, argValue.length())));
            } else if (arg->IsNull() || arg->IsUndefined()) {
                node = NULL;
            } else {
                THROW_ERROR_EXCEPTION(
                    "1-ary constructor of TNodeWrap can consume either Object or String or Null or Undefined");
            }
        } else if (args.Length() == 3) {
            EXPECT_THAT_IS(args[1], Uint32);
            EXPECT_THAT_HAS_INSTANCE(args[2], TNodeWrap);

            ECompression compression = (ECompression)args[1]->Uint32Value();
            INodePtr format = TNodeWrap::UnwrapNode(args[2]);

            auto arg = args[0];
            if (node::Buffer::HasInstance(arg)) {
                node = ConvertV8BytesToNode(
                    node::Buffer::Data(arg->ToObject()),
                    node::Buffer::Length(arg->ToObject()),
                    compression,
                    format);
            } else if (arg->IsString()) {
                String::AsciiValue argValue(arg->ToString());
                node = ConvertV8BytesToNode(
                    *argValue,
                    argValue.length(),
                    compression,
                    format);
            } else {
                THROW_ERROR_EXCEPTION(
                    "3-ary constructor of TNodeWrap can consume either String or Buffer with compression (Uint32) and format (TNodeWrap)");
            }
        } else {
            THROW_ERROR_EXCEPTION(
                "There are only 0-ary, 1-ary and 3-ary constructors of TNodeWrap");
        }

        std::unique_ptr<TNodeWrap> wrappedNode(new TNodeWrap(node));
        wrappedNode.release()->Wrap(args.This());

        return args.This();
    } catch (const std::exception& ex) {
        return ThrowException(ConvertErrorToV8(ex));
    }
}

////////////////////////////////////////////////////////////////////////////////

Handle<Value> TNodeWrap::CreateMerged(const Arguments& args)
{
    THREAD_AFFINITY_IS_V8();
    HandleScope scope;

    INodePtr delta = NULL;
    INodePtr result = NULL;

    try {
        for (int i = 0; i < args.Length(); ++i) {
            const auto& arg = args[i];
            if (arg->IsNull() || arg->IsUndefined()) {
                continue;
            } else {
                EXPECT_THAT_HAS_INSTANCE(arg, TNodeWrap);
            }

            delta = TNodeWrap::UnwrapNode(args[i]);
            result = result ? UpdateNode(std::move(result), std::move(delta)) : std::move(delta);
        }
    } catch (const std::exception& ex) {
        return ThrowException(ConvertErrorToV8(ex));
    }

    Local<Object> handle = ConstructorTemplate->GetFunction()->NewInstance();
    ObjectWrap::Unwrap<TNodeWrap>(handle)->SetNode(std::move(result));

    return scope.Close(std::move(handle));
}

////////////////////////////////////////////////////////////////////////////////

Handle<Value> TNodeWrap::CreateV8(const Arguments& args)
{
    THREAD_AFFINITY_IS_V8();
    HandleScope scope;

    YASSERT(args.Length() == 1);

    INodePtr node = NULL;

    try {
        node = ConvertV8ValueToNode(args[0]);
    } catch (const std::exception& ex) {
        return ThrowException(ConvertErrorToV8(ex));
    }

    Local<Object> handle = ConstructorTemplate->GetFunction()->NewInstance();
    ObjectWrap::Unwrap<TNodeWrap>(handle)->SetNode(std::move(node));

    return scope.Close(std::move(handle));
}

////////////////////////////////////////////////////////////////////////////////

Handle<Value> TNodeWrap::Print(const Arguments& args)
{
    THREAD_AFFINITY_IS_V8();
    HandleScope scope;

    YASSERT(args.Length() == 0);

    INodePtr node = TNodeWrap::UnwrapNode(args.This());

    auto string = ConvertToYsonString(node, EYsonFormat::Text);
    auto handle = String::New(string.Data().c_str(), string.Data().length());

    return scope.Close(std::move(handle));
}

////////////////////////////////////////////////////////////////////////////////

Handle<Value> TNodeWrap::Traverse(const Arguments& args)
{
    THREAD_AFFINITY_IS_V8();
    HandleScope scope;

    YASSERT(args.Length() == 1);

    EXPECT_THAT_IS(args[0], String);

    INodePtr node = TNodeWrap::UnwrapNode(args.This());
    String::AsciiValue pathValue(args[0]->ToString());
    TStringBuf path(*pathValue, pathValue.length());

    try {
        node = GetNodeByYPath(std::move(node), Stroka(path));
    } catch (const std::exception& ex) {
        return ThrowException(ConvertErrorToV8(ex));
    }

    Local<Object> handle = ConstructorTemplate->GetFunction()->NewInstance();
    ObjectWrap::Unwrap<TNodeWrap>(handle)->SetNode(std::move(node));

    return scope.Close(std::move(handle));
}

////////////////////////////////////////////////////////////////////////////////

Handle<Value> TNodeWrap::Get(const Arguments& args)
{
    THREAD_AFFINITY_IS_V8();
    HandleScope scope;

    YASSERT(args.Length() == 0);

    INodePtr node = TNodeWrap::UnwrapNode(args.This());
    return scope.Close(ProduceV8(node));
}

////////////////////////////////////////////////////////////////////////////////

INodePtr TNodeWrap::GetNode()
{
    return Node_;
}

const INodePtr TNodeWrap::GetNode() const
{
    return Node_;
}

void TNodeWrap::SetNode(INodePtr node)
{
    Node_ = std::move(node);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NNodeJS
} // namespace NYT
