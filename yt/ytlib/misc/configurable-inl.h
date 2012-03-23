#ifndef CONFIGURABLE_INL_H_
#error "Direct inclusion of this file is not allowed, include configurable.h"
#endif
#undef CONFIGURABLE_INL_H_

#include "guid.h"
#include "string.h"
#include "nullable.h"
#include "enum.h"
#include "demangle.h"

#include <ytlib/actions/action_util.h>
#include <ytlib/ytree/serialize.h>
#include <ytlib/ytree/tree_visitor.h>
#include <ytlib/ytree/yson_consumer.h>

#include <util/datetime/base.h>

// Avoid circular references.
// TODO(babenko): shoudn't this be in public.h?
namespace NYT {
namespace NYTree {
    TYPath CombineYPaths(
        const TYPath& path1,
        const TYPath& path2);
}
}

namespace NYT {
namespace NConfig {

////////////////////////////////////////////////////////////////////////////////

template <class T, class = void>
struct TLoadHelper
{
    static void Load(T& parameter, NYTree::INode* node, const NYTree::TYPath& path)
    {
        UNUSED(path);
        NYTree::Read(parameter, node);
    }
};

// TConfigurable
template <class T>
struct TLoadHelper<
    T,
    typename NMpl::TEnableIf< NMpl::TIsConvertible<T*, TConfigurable*> >::TType
>
{
    static void Load(T& parameter, NYTree::INode* node, const NYTree::TYPath& path)
    {
        if (!parameter) {
            parameter = New<T>();
        }
        parameter->Load(node, false, path);
    }
};

// TNullable
template <class T>
struct TLoadHelper<TNullable<T>, void>
{
    static void Load(TNullable<T>& parameter, NYTree::INode* node, const NYTree::TYPath& path)
    {
        T value;
        TLoadHelper<T>::Load(value, node, path);
        parameter = value;
    }
};

// yvector
template <class T>
struct TLoadHelper<yvector<T>, void>
{
    static void Load(yvector<T>& parameter, NYTree::INode* node, const NYTree::TYPath& path)
    {
        auto listNode = node->AsList();
        auto size = listNode->GetChildCount();
        parameter.resize(size);
        for (int i = 0; i < size; ++i) {
            TLoadHelper<T>::Load(parameter[i], ~listNode->GetChild(i), NYTree::CombineYPaths(path, ToString(i)));
        }
    }
};

// yhash_set
template <class T>
struct TLoadHelper<yhash_set<T>, void>
{
    static void Load(yhash_set<T>& parameter, NYTree::INode* node, const NYTree::TYPath& path)
    {
        auto listNode = node->AsList();
        auto size = listNode->GetChildCount();
        for (int i = 0; i < size; ++i) {
            T value;
            TLoadHelper<T>::Load(value, ~listNode->GetChild(i), NYTree::CombineYPaths(path, ToString(i)));
            parameter.insert(MoveRV(value));
        }
    }
};

// yhash_map
template <class T>
struct TLoadHelper<yhash_map<Stroka, T>, void>
{
    static void Load(yhash_map<Stroka, T>& parameter, NYTree::INode* node, const NYTree::TYPath& path)
    {
        auto mapNode = node->AsMap();
        FOREACH (const auto& pair, mapNode->GetChildren()) {
            auto& key = pair.first;
            T value;
            TLoadHelper<T>::Load(value, ~pair.second, NYTree::CombineYPaths(path, key));
            parameter.insert(MakePair(key, MoveRV(value)));
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

// all
inline void ValidateSubconfigs(
    const void* /* parameter */,
    const NYTree::TYPath& /* path */)
{ }

// TConfigurable
template <class T>
inline void ValidateSubconfigs(
    const TIntrusivePtr<T>* parameter,
    const NYTree::TYPath& path,
    typename NMpl::TEnableIf<NMpl::TIsConvertible< T*, TConfigurable* >, int>::TType = 0)
{
    if (*parameter) {
        (*parameter)->Validate(path);
    }
}

// yvector
template <class T>
inline void ValidateSubconfigs(
    const yvector<T>* parameter,
    const NYTree::TYPath& path)
{
    for (int i = 0; i < parameter->ysize(); ++i) {
        ValidateSubconfigs(
            &(*parameter)[i],
            NYTree::CombineYPaths(path, ToString(i)));
    }
}

// yhash_map
template <class T>
inline void ValidateSubconfigs(
    const yhash_map<Stroka, T>* parameter,
    const NYTree::TYPath& path)
{
    FOREACH (const auto& pair, *parameter) {
        ValidateSubconfigs(
            &pair.second,
            NYTree::CombineYPaths(path, pair.first));
    }
}

////////////////////////////////////////////////////////////////////////////////

// all
inline bool IsPresent(const void* /* parameter */)
{
    return true;
}

// TIntrusivePtr
template <class T>
inline bool IsPresent(TIntrusivePtr<T>* parameter)
{
    return (bool) (*parameter);
}

// TNullable
template <class T>
inline bool IsPresent(TNullable<T>* parameter)
{
    return parameter->IsInitialized();
}

////////////////////////////////////////////////////////////////////////////////

template <class T>
TParameter<T>::TParameter(T& parameter)
    : Parameter(parameter)
    , HasDefaultValue(false)
{ }

template <class T>
void TParameter<T>::Load(NYTree::INode* node, const NYTree::TYPath& path)
{
    if (node) {
        try {
            TLoadHelper<T>::Load(Parameter, node, path);
        } catch (const std::exception& ex) {
            ythrow yexception()
                << Sprintf("Could not read parameter (Path: %s)\n%s",
                    ~path,
                    ex.what());
        }
    } else if (!HasDefaultValue) {
        ythrow yexception()
            << Sprintf("Required parameter is missing (Path: %s)", ~path);
    }
}

template <class T>
void TParameter<T>::Validate(const NYTree::TYPath& path) const
{
    ValidateSubconfigs(&Parameter, path);
    FOREACH (auto validator, Validators) {
        try {
            validator->Do(Parameter);
        } catch (const std::exception& ex) {
            ythrow yexception()
                << Sprintf("Validation failed (Path: %s)\n%s",
                    ~path,
                    ex.what());
        }
    }
}

template <class T>
void TParameter<T>::Save(NYTree::IYsonConsumer* consumer) const
{
    if (IsPresent()) {
        NYTree::Write(Parameter, consumer);
    }
}

template <class T>
bool TParameter<T>::IsPresent() const
{
    return NConfig::IsPresent(&Parameter);
}


template <class T>
TParameter<T>& TParameter<T>::Default(const T& defaultValue)
{
    Parameter = defaultValue;
    HasDefaultValue = true;
    return *this;
}

template <class T>
TParameter<T>& TParameter<T>::Default(T&& defaultValue)
{
    Parameter = MoveRV(defaultValue);
    HasDefaultValue = true;
    return *this;
}

template <class T>
TParameter<T>& TParameter<T>::DefaultNew()
{
    return Default(New<typename T::TElementType>());
}

template <class T>
TParameter<T>& TParameter<T>::CheckThat(TValidatorPtr validator)
{
    Validators.push_back(validator);
    return *this;
}

////////////////////////////////////////////////////////////////////////////////
// Standard validators

#define DEFINE_VALIDATOR(method, condition, ex) \
    template <class T> \
    TParameter<T>& TParameter<T>::method \
    { \
        return CheckThat(FromFunctor([=] (const T& parameter) \
            { \
                if (!(condition)) { \
                    ythrow (ex); \
                } \
            })); \
    }

DEFINE_VALIDATOR(
    GreaterThan(T value),
    parameter > value,
    yexception()
        << "Validation failure (Expected: >"
        << value << ", Actual: " << parameter << ")")

DEFINE_VALIDATOR(
    GreaterThanOrEqual(T value),
    parameter >= value,
    yexception()
        << "Validation failure (Expected: >="
        << value << ", Actual: " << parameter << ")")

DEFINE_VALIDATOR(
    LessThan(T value),
    parameter < value,
    yexception()
        << "Validation failure (Expected: <"
        << value << ", Actual: " << parameter << ")")

DEFINE_VALIDATOR(
    LessThanOrEqual(T value),
    parameter <= value,
    yexception()
        << "Validation failure (Expected: <="
        << value << ", Actual: " << parameter << ")")

DEFINE_VALIDATOR(
    InRange(T lowerBound, T upperBound),
    lowerBound <= parameter && parameter <= upperBound,
    yexception()
        << "Validation failure (Expected: in range ["
        << lowerBound << ", " << upperBound << "], Actual: " << parameter << ")")

DEFINE_VALIDATOR(
    NonEmpty(),
    parameter.size() > 0,
    yexception()
        << "Validation failure (Expected: non-empty)")

#undef DEFINE_VALIDATOR

////////////////////////////////////////////////////////////////////////////////

} // namespace NConfig

////////////////////////////////////////////////////////////////////////////////

template <class T>
NConfig::TParameter<T>& TConfigurable::Register(const Stroka& parameterName, T& value)
{
    auto parameter = New< NConfig::TParameter<T> >(value);
    YVERIFY(Parameters.insert(
        TPair<Stroka, NConfig::IParameter::TPtr>(parameterName, parameter)).second);
    return *parameter;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
