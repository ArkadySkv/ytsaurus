#pragma once

#include "common.h"
#include "property.h"

#include <core/misc/preprocessor.h>
#include <core/misc/error.pb.h>

#include <core/actions/callback.h>

#include <core/ytree/public.h>
#include <core/ytree/yson_string.h>
#include <core/ytree/attributes.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

// Forward declarations
template <class T>
class TErrorOr;

typedef TErrorOr<void> TError;

////////////////////////////////////////////////////////////////////////////////

template <>
class TErrorOr<void>
{
public:
    TErrorOr();
    TErrorOr(const TError& other);
    TErrorOr(TError&& other);

    TErrorOr(const std::exception& ex);

    explicit TErrorOr(const Stroka& message);
    explicit TErrorOr(const char* format, ...);

    TErrorOr(int code, const Stroka& message);
    TErrorOr(int code, const char* format, ...);

    static TError FromSystem();
    static TError FromSystem(int error);

    TError& operator = (const TError& other);
    TError& operator = (TError&& other);

    int GetCode() const;
    TError& SetCode(int code);

    const Stroka& GetMessage() const;
    TError& SetMessage(const Stroka& message);

    const NYTree::IAttributeDictionary& Attributes() const;
    NYTree::IAttributeDictionary& Attributes();

    const std::vector<TError>& InnerErrors() const;
    std::vector<TError>& InnerErrors();

    bool IsOK() const;

    TNullable<TError> FindMatching(int code) const;

    enum
    {
        OK = 0,
        GenericFailure = 1
    };

private:
    int Code_;
    Stroka Message_;
    std::unique_ptr<NYTree::IAttributeDictionary> Attributes_;
    std::vector<TError> InnerErrors_;

    void CaptureOriginAttributes();

};

Stroka ToString(const TError& error);

void ToProto(NProto::TError* protoError, const TError& error);
void FromProto(TError* error, const NProto::TError& protoError);

void Serialize(const TError& error, NYson::IYsonConsumer* consumer);
void Deserialize(TError& error, NYTree::INodePtr node);

////////////////////////////////////////////////////////////////////////////////

namespace NYTree {

// Avoid dependency on convert.h

template <class T>
TYsonString ConvertToYsonString(const T& value);

TYsonString ConvertToYsonString(const char* value);

} // namespace NYTree

struct TErrorAttribute
{
    template <class T>
    TErrorAttribute(const Stroka& key, const T& value)
        : Key(key)
        , Value(NYTree::ConvertToYsonString(value))
    { }

    TErrorAttribute(const Stroka& key, const NYTree::TYsonString& value)
        : Key(key)
        , Value(value)
    { }

    Stroka Key;
    NYTree::TYsonString Value;
};

TError operator << (TError error, const TErrorAttribute& attribute);
TError operator << (TError error, const TError& innerError);

TError operator >>= (const TErrorAttribute& attribute, TError error);

////////////////////////////////////////////////////////////////////////////////

class TErrorException
    : public std::exception
{
    DEFINE_BYREF_RW_PROPERTY(TError, Error);

public:
    TErrorException();
    TErrorException(TErrorException&& other);
    TErrorException(const TErrorException& other);

    ~TErrorException() throw();

    virtual const char* what() const throw() override;

private:
    mutable Stroka CachedWhat;

};

// Make it template to avoid type erasure during throw.
template <class TException>
TException&& operator <<= (TException&& ex, const TError& error)
{
    ex.Error() = error;
    return std::move(ex);
}

////////////////////////////////////////////////////////////////////////////////

#define ERROR_SOURCE_LOCATION() \
    ::NYT::TErrorAttribute("file", ::NYT::NYTree::ConvertToYsonString(__FILE__)) >>= \
    ::NYT::TErrorAttribute("line", ::NYT::NYTree::ConvertToYsonString(__LINE__))

#define THROW_ERROR \
    throw \
        ::NYT::TErrorException() <<= \
        ERROR_SOURCE_LOCATION() >>= \

#define THROW_ERROR_EXCEPTION(...) \
    THROW_ERROR ::NYT::TError(__VA_ARGS__)

#define THROW_ERROR_EXCEPTION_IF_FAILED(error, ...) \
    if ((error).IsOK()) {\
    } else { \
        auto PP_CONCAT(wrapperError_, __LINE__) = ::NYT::TError(__VA_ARGS__); \
        if (PP_CONCAT(wrapperError_, __LINE__).IsOK()) { \
            THROW_ERROR (error); \
        } else { \
            THROW_ERROR PP_CONCAT(wrapperError_, __LINE__) << (error); \
        } \
    }\

////////////////////////////////////////////////////////////////////////////////

typedef TFuture<TError>  TAsyncError;
typedef TPromise<TError> TAsyncErrorPromise;

////////////////////////////////////////////////////////////////////////////////

template <class T>
class TErrorOr
    : public TError
{
public:
    TErrorOr()
        : Value_()
    { }

    TErrorOr(const T& value)
        : Value_(value)
    { }

    TErrorOr(T&& value)
        : Value_(std::move(value))
    { }

    TErrorOr(const TErrorOr<T>& other)
        : TError(other)
        , Value_(other.Value_)
    { }

    TErrorOr(const TError& other)
        : TError(other)
        , Value_()
    { }

    TErrorOr(const std::exception& ex)
        : TError(ex)
    { }

    template <class TOther>
    TErrorOr(const TErrorOr<TOther>& other)
        : TError(other)
        , Value_(other.Value())
    { }

    const T& Value() const
    {
        YCHECK(IsOK());
        return Value_;
    }

    T& Value()
    {
        YCHECK(IsOK());
        return Value_;
    }

    const T& ValueOrThrow() const
    {
        if (!IsOK()) {
            THROW_ERROR *this;
        }
        return Value_;
    }

    T& ValueOrThrow()
    {
        if (!IsOK()) {
            THROW_ERROR *this;
        }
        return Value_;
    }

private:
    T Value_;

};

template <class T>
Stroka ToString(const TErrorOr<T>& valueOrError)
{
    return ToString(TError(valueOrError));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT


#define ERROR_INL_H_
#include "error-inl.h"
#undef ERROR_INL_H_
