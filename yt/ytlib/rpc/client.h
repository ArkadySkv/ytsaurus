#pragma once

#include "channel.h"

#include <ytlib/misc/property.h>
#include <ytlib/misc/delayed_invoker.h>
#include <ytlib/misc/metric.h>
#include <ytlib/misc/serialize.h>
#include <ytlib/bus/client.h>
#include <ytlib/actions/future.h>
#include <ytlib/ytree/attributes.h>

namespace NYT {
namespace NRpc {

////////////////////////////////////////////////////////////////////////////////

class TClientRequest;

template <class TRequestMessage, class TResponse>
class TTypedClientRequest;

class TClientResponse;

template <class TResponseMessage>
class TTypedClientResponse;

class TOneWayClientResponse;

////////////////////////////////////////////////////////////////////////////////

class TProxyBase
{
protected:
    //! Service error type.
    /*!
     * Defines a basic type of error code for all proxies.
     * A derived proxy type may hide this definition by introducing
     * an appropriate descendant of NRpc::EErrorCode.
     */
    typedef NRpc::EErrorCode EErrorCode;

    TProxyBase(IChannel::TPtr channel, const Stroka& serviceName);

    DEFINE_BYVAL_RW_PROPERTY(TNullable<TDuration>, DefaultTimeout);

    IChannel::TPtr Channel;
    Stroka ServiceName;
};          

////////////////////////////////////////////////////////////////////////////////

struct IClientRequest
    : public virtual TRefCounted
{
    typedef TIntrusivePtr<IClientRequest> TPtr;

    virtual NBus::IMessage::TPtr Serialize() const = 0;

    virtual const TRequestId& GetRequestId() const = 0;
    virtual const Stroka& GetPath() const = 0;
    virtual const Stroka& GetVerb() const = 0;

    virtual NYTree::IAttributeDictionary& Attributes() = 0;
    virtual const NYTree::IAttributeDictionary& Attributes() const = 0;
};

////////////////////////////////////////////////////////////////////////////////

class TClientRequest
    : public IClientRequest
{
    DEFINE_BYREF_RW_PROPERTY(yvector<TSharedRef>, Attachments);
    DEFINE_BYVAL_RO_PROPERTY(bool, OneWay);
    DEFINE_BYVAL_RW_PROPERTY(TNullable<TDuration>, Timeout);

public:
    typedef TIntrusivePtr<TClientRequest> TPtr;

    virtual NBus::IMessage::TPtr Serialize() const;

    virtual const TRequestId& GetRequestId() const;
    virtual const Stroka& GetPath() const;
    virtual const Stroka& GetVerb() const;

    virtual NYTree::IAttributeDictionary& Attributes();
    virtual const NYTree::IAttributeDictionary& Attributes() const;

protected:
    IChannel::TPtr Channel;
    Stroka Path;
    Stroka Verb;
    TRequestId RequestId;
    TAutoPtr<NYTree::IAttributeDictionary> Attributes_;

    TClientRequest(
        IChannel::TPtr channel,
        const Stroka& path,
        const Stroka& verb,
        bool oneWay);

    virtual TBlob SerializeBody() const = 0;

    void DoInvoke(
        IClientResponseHandler* responseHandler,
        TNullable<TDuration> timeout);
};

////////////////////////////////////////////////////////////////////////////////

template <class TRequestMessage, class TResponse>
class TTypedClientRequest
    : public TClientRequest
    , public TRequestMessage
{
public:
    typedef TIntrusivePtr<TTypedClientRequest> TPtr;

    TTypedClientRequest(
        IChannel::TPtr channel,
        const Stroka& path,
        const Stroka& verb,
        bool oneWay)
        : TClientRequest(channel, path, verb, oneWay)
    { }

    typename TFuture< TIntrusivePtr<TResponse> >::TPtr Invoke()
    {
        auto response = NYT::New<TResponse>(GetRequestId());
        auto asyncResult = response->GetAsyncResult();
        DoInvoke(~response, Timeout_);
        return asyncResult;
    }

    // Override base method for fluent use.
    TIntrusivePtr<TTypedClientRequest> SetTimeout(TNullable<TDuration> timeout)
    {
        TClientRequest::SetTimeout(timeout);
        return this;
    }

private:
    virtual TBlob SerializeBody() const
    {
        NLog::TLogger& Logger = RpcLogger;
        TBlob blob;
        YVERIFY(SerializeToProtobuf(this, &blob));
        return blob;
    }

};

////////////////////////////////////////////////////////////////////////////////

//! Handles response for an RPC request.
struct IClientResponseHandler
    : public virtual TRefCounted
{
    typedef TIntrusivePtr<IClientResponseHandler> TPtr;

    //! Request delivery has been acknowledged.
    virtual void OnAcknowledgement() = 0;
    //! The request has been replied with #EErrorCode::OK.
    /*!
     *  \param message A message containing the response.
     */
    virtual void OnResponse(NBus::IMessage* message) = 0;
    //! The request has failed.
    /*!
     *  \param error An error that has occurred.
     */
    virtual void OnError(const TError& error) = 0;
};

////////////////////////////////////////////////////////////////////////////////

//! Provides a common base for both one-way and two-way responses.
class TClientResponseBase
    : public IClientResponseHandler
{
    DEFINE_BYVAL_RO_PROPERTY(TRequestId, RequestId);
    DEFINE_BYVAL_RO_PROPERTY(TError, Error);
    DEFINE_BYVAL_RO_PROPERTY(TInstant, StartTime);

public:
    typedef TIntrusivePtr<TClientResponseBase> TPtr;

    int GetErrorCode() const;
    bool IsOK() const;

protected:
    TClientResponseBase(const TRequestId& requestId);

    virtual void FireCompleted() = 0;

    DECLARE_ENUM(EState,
        (Sent)
        (Ack)
        (Done)
    );

    // Protects state.
    TSpinLock SpinLock;
    EState State;

    // IClientResponseHandler implementation.
    virtual void OnError(const TError& error);

};

////////////////////////////////////////////////////////////////////////////////

//! Describes a two-way response.
class TClientResponse
    : public TClientResponseBase
{
    DEFINE_BYREF_RW_PROPERTY(yvector<TSharedRef>, Attachments);

public:
    typedef TIntrusivePtr<TClientResponse> TPtr;

    NBus::IMessage::TPtr GetResponseMessage() const;

    NYTree::IAttributeDictionary& Attributes();
    const NYTree::IAttributeDictionary& Attributes() const;

protected:
    TClientResponse(const TRequestId& requestId);

    virtual void DeserializeBody(const TRef& data) = 0;

private:
    // Protected by #SpinLock.
    NBus::IMessage::TPtr ResponseMessage;
    TAutoPtr<NYTree::IAttributeDictionary> Attributes_;

    // IClientResponseHandler implementation.
    virtual void OnAcknowledgement();
    virtual void OnResponse(NBus::IMessage* message);

    void Deserialize(NBus::IMessage::TPtr responseMessage);

};

////////////////////////////////////////////////////////////////////////////////

template <class TResponseMessage>
class TTypedClientResponse
    : public TClientResponse
    , public TResponseMessage
{
public:
    typedef TIntrusivePtr<TTypedClientResponse> TPtr;

    TTypedClientResponse(const TRequestId& requestId)
        : TClientResponse(requestId)
        , AsyncResult(NYT::New< TFuture<TPtr> >())
    { }

    typename TFuture<TPtr>::TPtr GetAsyncResult()
    {
        return AsyncResult;
    }

private:
    typename TFuture<TPtr>::TPtr AsyncResult;

    virtual void FireCompleted()
    {
        AsyncResult->Set(this);
        AsyncResult.Reset();
    }

    virtual void DeserializeBody(const TRef& data)
    {
        NLog::TLogger& Logger = RpcLogger;
        YVERIFY(DeserializeFromProtobuf(this, data));
    }
};

////////////////////////////////////////////////////////////////////////////////

//! Describes a one-way response.
class TOneWayClientResponse
    : public TClientResponseBase
{
public:
    typedef TIntrusivePtr<TOneWayClientResponse> TPtr;

    TOneWayClientResponse(const TRequestId& requestId);

    TFuture<TPtr>::TPtr GetAsyncResult();

private:
    TFuture<TPtr>::TPtr AsyncResult;

    // IClientResponseHandler implementation.
    virtual void OnAcknowledgement();
    virtual void OnResponse(NBus::IMessage* message);

    virtual void FireCompleted();

};

////////////////////////////////////////////////////////////////////////////////

#define DEFINE_RPC_PROXY_METHOD(ns, method) \
    typedef ::NYT::NRpc::TTypedClientResponse<ns::TRsp##method> TRsp##method; \
    typedef ::NYT::NRpc::TTypedClientRequest<ns::TReq##method, TRsp##method> TReq##method; \
    typedef ::NYT::TFuture<TRsp##method::TPtr> TInv##method; \
    \
    TReq##method::TPtr method() \
    { \
        return \
            New<TReq##method>(~Channel, ServiceName, #method, false) \
            ->SetTimeout(DefaultTimeout_); \
    }

////////////////////////////////////////////////////////////////////////////////

#define DEFINE_ONE_WAY_RPC_PROXY_METHOD(ns, method) \
    typedef ::NYT::NRpc::TOneWayClientResponse TRsp##method; \
    typedef ::NYT::NRpc::TTypedClientRequest<ns::TReq##method, TRsp##method> TReq##method; \
    typedef ::NYT::TFuture<TRsp##method::TPtr> TInv##method; \
    \
    TReq##method::TPtr method() \
    { \
        return \
            New<TReq##method>(~Channel, ServiceName, #method, true) \
            ->SetTimeout(DefaultTimeout_); \
    }

////////////////////////////////////////////////////////////////////////////////
} // namespace NRpc
} // namespace NYT
