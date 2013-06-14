#pragma once

#include "public.h"

#include <ytlib/yson/public.h>

#include <ytlib/misc/mpl.h>
#include <ytlib/misc/property.h>
#include <ytlib/misc/nullable.h>
#include <ytlib/misc/error.h>
#include <ytlib/misc/serialize.h>

#include <ytlib/actions/bind.h>
#include <ytlib/actions/callback.h>

namespace NYT {
namespace NConfig {

// Introduces Serialize function family into current scope.
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

struct IParameter
    : public TRefCounted
{
    // node can be NULL
    virtual void Load(NYTree::INodePtr node, const NYPath::TYPath& path) = 0;
    virtual void Validate(const NYPath::TYPath& path) const = 0;
    virtual void SetDefaults() = 0;
    virtual void Save(NYson::IYsonConsumer* consumer) const = 0;
    virtual bool IsPresent() const = 0;
};

typedef TIntrusivePtr<IParameter> IParameterPtr;

////////////////////////////////////////////////////////////////////////////////

template <class T>
class TParameter
    : public IParameter
{
public:
    /*!
     * \note Must throw exception for incorrect data
     */
    typedef TCallback<void(const T&)> TValidator;
    typedef typename TNullableTraits<T>::TValueType TValueType;

    explicit TParameter(T& parameter);

    virtual void Load(NYTree::INodePtr node, const NYPath::TYPath& path) override;
    virtual void Validate(const NYPath::TYPath& path) const override;
    virtual void SetDefaults() override;
    virtual void Save(NYson::IYsonConsumer* consumer) const override;
    virtual bool IsPresent() const override;

public:
    TParameter& Describe(const char* description);
    TParameter& Default(const T& defaultValue = T());
    TParameter& DefaultNew();
    TParameter& CheckThat(TValidator validator);
    TParameter& GreaterThan(TValueType value);
    TParameter& GreaterThanOrEqual(TValueType value);
    TParameter& LessThan(TValueType value);
    TParameter& LessThanOrEqual(TValueType value);
    TParameter& InRange(TValueType lowerBound, TValueType upperBound);
    TParameter& NonEmpty();

private:
    T& Parameter;
    const char* Description;
    TNullable<T> DefaultValue;
    std::vector<TValidator> Validators;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NConfig

////////////////////////////////////////////////////////////////////////////////

class TYsonSerializableLite
{
public:
    TYsonSerializableLite();

    void Load(
        NYTree::INodePtr node,
        bool validate = true,
        bool setDefaults = true,
        const NYPath::TYPath& path = "");

    void Validate(const NYPath::TYPath& path = "") const;

    void SetDefaults();

    void Save(NYson::IYsonConsumer* consumer) const;

    DEFINE_BYVAL_RW_PROPERTY(bool, KeepOptions);
    NYTree::IMapNodePtr GetOptions() const;

    std::vector<Stroka> GetRegisteredKeys() const;

protected:
    virtual void OnLoaded();

    template <class T>
    NConfig::TParameter<T>& RegisterParameter(
        const Stroka& parameterName,
        T& value);

    template <class F>
    void RegisterInitializer(const F& func);

    template <class F>
    void RegisterValidator(const F& func);

private:
    template <class T>
    friend class TParameter;

    typedef yhash_map<Stroka, NConfig::IParameterPtr> TParameterMap;

    TParameterMap Parameters;
    NYTree::IMapNodePtr Options;

    std::vector<TClosure> Initializers;
    std::vector<TClosure> Validators;
};

////////////////////////////////////////////////////////////////////////////////

class TYsonSerializable
    : public TRefCounted
    , public TYsonSerializableLite
{ };

////////////////////////////////////////////////////////////////////////////////

struct TBinaryYsonSerializer
{
    static void Save(TStreamSaveContext& context, const TYsonSerializableLite& obj);
    static void Load(TStreamLoadContext& context, TYsonSerializableLite& obj);
};

template <class T, class C>
struct TSerializerTraits<
    T,
    C,
    typename NMpl::TEnableIf<NMpl::TIsConvertible<T&, TYsonSerializableLite&>>::TType>
{
    typedef TBinaryYsonSerializer TSerializer;
};

////////////////////////////////////////////////////////////////////////////////

template <class T>
TIntrusivePtr<T> CloneYsonSerializable(TIntrusivePtr<T> obj);

void Serialize(const TYsonSerializableLite& value, NYson::IYsonConsumer* consumer);
void Deserialize(TYsonSerializableLite& value, NYTree::INodePtr node);

template <class T>
TIntrusivePtr<T> UpdateYsonSerializable(
    TIntrusivePtr<T> obj,
    NYTree::INodePtr patch);

template <class T>
bool ReconfigureYsonSerializable(
    TIntrusivePtr<T> config,
    const NYTree::TYsonString& newConfigYson);

template <class T>
bool ReconfigureYsonSerializable(
    TIntrusivePtr<T> config,
    NYTree::INodePtr newConfigNode);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

#define YSON_SERIALIZABLE_INL_H_
#include "yson_serializable-inl.h"
#undef YSON_SERIALIZABLE_INL_H_
