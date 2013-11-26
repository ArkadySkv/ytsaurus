#pragma once

#include <core/misc/common.h>
#include <core/misc/guid.h>
#include <core/misc/string.h>
#include <core/misc/nullable.h>

#include <core/ytree/permission.h>

#include <core/yson/writer.h>

#include <ytlib/object_client/public.h>

#include <ytlib/cypress_client/public.h>

#include <tclap/CmdLine.h>

namespace TCLAP {

template <>
struct ArgTraits<Stroka>
{
    typedef StringLike ValueCategory;
};

template <>
struct ArgTraits< ::NYT::TGuid >
{
    typedef ValueLike ValueCategory;
};

template <>
struct ArgTraits< ::NYT::NCypressClient::ELockMode >
{
    typedef ValueLike ValueCategory;
};

template <>
struct ArgTraits< ::NYT::NObjectClient::EObjectType >
{
    typedef ValueLike ValueCategory;
};

template <>
struct ArgTraits< ::NYT::NYson::EYsonFormat >
{
    typedef ValueLike ValueCategory;
};

template <>
struct ArgTraits< ::NYT::NYTree::EPermission >
{
    typedef ValueLike ValueCategory;
};

template <class T>
struct ArgTraits< NYT::TNullable<T> >
{
    typedef ValueLike ValueCategory;
};

template <>
struct ArgTraits< long long >
{
    typedef ValueLike ValueCategory;
};

}

////////////////////////////////////////////////////////////////////////////////

namespace NYT {

Stroka ReadAll(std::istringstream& input);

std::istringstream& operator >> (std::istringstream& input, TGuid& guid);

template <class T>
std::istringstream& operator >> (std::istringstream& input, TEnumBase<T>& mode);

template <class T>
std::istringstream& operator >> (std::istringstream& input, TNullable<T>& nullable);

// TODO(babenko): move to inl

template <class T>
inline std::istringstream& operator >> (std::istringstream& input, TEnumBase<T>& mode)
{
    auto str = ReadAll(input);
    static_cast<T&>(mode) = NYT::ParseEnum<T>(str);
    return input;
}

template <class T>
inline std::istringstream& operator >> (std::istringstream& input, TNullable<T>& nullable)
{
    auto str = ReadAll(input);
    if (str.empty()) {
        nullable = NYT::TNullable<T>();
    } else {
        std::istringstream strStream(str);
        T value;
        strStream >> value;
        nullable = NYT::TNullable<T>(value);
    }
    return input;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT


