#ifndef SERIALIZE_INL_H_
#error "Direct inclusion of this file is not allowed, include serialize.h"
#endif
#undef SERIALIZE_INL_H_

#include "node.h"
#include "yson_stream.h"
#include "yson_string.h"
#include "yson_serializable.h"

#include <ytlib/misc/nullable.h>
#include <ytlib/misc/serialize.h>
#include <ytlib/misc/string.h>
#include <ytlib/misc/error.h>

namespace NYT {
namespace NYTree {

////////////////////////////////////////////////////////////////////////////////

template <class T>
NYson::EYsonType GetYsonType(const T&)
{
    return NYson::EYsonType::Node;
}

////////////////////////////////////////////////////////////////////////////////

template <class T>
void WriteYson(
    TOutputStream* output,
    const T& value,
    NYson::EYsonType type,
    NYson::EYsonFormat format)
{
    NYson::TYsonWriter writer(output, format, type);
    Consume(value, &writer);
}

template <class T>
void WriteYson(
    TOutputStream* output,
    const T& value,
    NYson::EYsonFormat format)
{
    WriteYson(output, value, GetYsonType(value), format);
}

template <class T>
void WriteYson(
    const TYsonOutput& output,
    const T& value,
    NYson::EYsonFormat format)
{
    WriteYson(output.GetStream(), value, output.GetType(), format);
}

////////////////////////////////////////////////////////////////////////////////

template <class T>
void Serialize(T* value, NYson::IYsonConsumer* consumer)
{
    YASSERT(value);
    Serialize(*value, consumer);
}

template <class T>
void Serialize(const TIntrusivePtr<T>& value, NYson::IYsonConsumer* consumer)
{
    Serialize(~value, consumer);
}

// TEnumBase
template <class T>
void Serialize(
    T value,
    NYson::IYsonConsumer* consumer,
    typename NMpl::TEnableIf<NMpl::TIsConvertible<T&, TEnumBase<T>&>, int>::TType)
{
    consumer->OnStringScalar(FormatEnum(value));
}

// TNullable
template <class T>
void Serialize(const TNullable<T>& value, NYson::IYsonConsumer* consumer)
{
    YASSERT(value);
    Serialize(*value, consumer);
}

// TSmallVector
template <class T, unsigned N>
void Serialize(const TSmallVector<T, N>& value, NYson::IYsonConsumer* consumer)
{
    consumer->OnBeginList();
    FOREACH (const auto& value, value) {
        consumer->OnListItem();
        Serialize(value, consumer);
    }
    consumer->OnEndList();
}

// std::vector
template <class T>
void Serialize(const std::vector<T>& value, NYson::IYsonConsumer* consumer)
{
    consumer->OnBeginList();
    FOREACH (const auto& value, value) {
        consumer->OnListItem();
        Serialize(value, consumer);
    }
    consumer->OnEndList();
}

// yhash_set
template <class T>
void Serialize(const yhash_set<T>& value, NYson::IYsonConsumer* consumer)
{
    consumer->OnBeginList();
    auto sortedItems = GetSortedIterators(value);
    FOREACH (const auto& value, sortedItems) {
        consumer->OnListItem();
        Serialize(*value, consumer);
    }
    consumer->OnEndList();
}

// yhash_map
template <class T>
void Serialize(const yhash_map<Stroka, T>& value, NYson::IYsonConsumer* consumer)
{
    consumer->OnBeginMap();
    auto sortedItems = GetSortedIterators(value);
    FOREACH (const auto& pair, sortedItems) {
        consumer->OnKeyedItem(pair->first);
        Serialize(pair->second, consumer);
    }
    consumer->OnEndMap();
}

////////////////////////////////////////////////////////////////////////////////

template <class T>
void Deserialize(TIntrusivePtr<T>& value, INodePtr node)
{
    if (!value) {
        value = New<T>();
    }
    Deserialize(*value, node);
}

template <class T>
void Deserialize(std::unique_ptr<T>& value, INodePtr node)
{
    if (!value) {
        value.Reset(new T());
    }
    Deserialize(*value, node);
}

template <class T>
T CheckedStaticCast(i64 value)
{
    if (value < Min<T>() || value > Max<T>()) {
        THROW_ERROR_EXCEPTION("Argument is out of integral range: %" PRId64 ")", value);
    }
    return static_cast<T>(value);
}

// TEnumBase
template <class T>
void Deserialize(
    T& value,
    INodePtr node,
    typename NMpl::TEnableIf<NMpl::TIsConvertible<T&, TEnumBase<T>&>, int>::TType)
{
    auto stringValue = node->AsString()->GetValue();
    value = ParseEnum<T>(stringValue);
}

// TNullable
template <class T>
void Deserialize(TNullable<T>& value, INodePtr node)
{
    if (!value) {
        value = T();
    }
    Deserialize(*value, node);
}

// TSmallVector
template <class T, unsigned N>
void Deserialize(TSmallVector<T, N>& value, INodePtr node)
{
    auto listNode = node->AsList();
    auto size = listNode->GetChildCount();
    value.resize(size);
    for (int i = 0; i < size; ++i) {
        Deserialize(value[i], listNode->GetChild(i));
    }
}

// std::vector
template <class T>
void Deserialize(std::vector<T>& value, INodePtr node)
{
    auto listNode = node->AsList();
    auto size = listNode->GetChildCount();
    value.resize(size);
    for (int i = 0; i < size; ++i) {
        Deserialize(value[i], listNode->GetChild(i));
    }
}

// yhash_set
template <class T>
void Deserialize(yhash_set<T>& value, INodePtr node)
{
    auto listNode = node->AsList();
    auto size = listNode->GetChildCount();
    for (int i = 0; i < size; ++i) {
        T value;
        Deserialize(value, listNode->GetChild(i));
        value.insert(std::move(value));
    }
}

// yhash_map
template <class T>
void Deserialize(yhash_map<Stroka, T>& value, INodePtr node)
{
    auto mapNode = node->AsMap();
    FOREACH (const auto& pair, mapNode->GetChildren()) {
        auto& key = pair.first;
        T value;
        Deserialize(value, pair.second);
        value.insert(std::make_pair(key, std::move(value)));
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT
