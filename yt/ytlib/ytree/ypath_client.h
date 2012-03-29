#pragma once

#include "public.h"
#include "ypath_service.h"
#include "attribute_provider_detail.h"

#include <ytlib/misc/ref.h>
#include <ytlib/misc/property.h>
#include <ytlib/bus/message.h>
#include <ytlib/rpc/client.h>
#include <ytlib/actions/action_util.h>

namespace NYT {
namespace NYTree {

////////////////////////////////////////////////////////////////////////////////

class TYPathRequest
    : public TRefCounted
    , public TEphemeralAttributeProvider
{
    DEFINE_BYVAL_RO_PROPERTY(Stroka, Verb);
    DEFINE_BYVAL_RW_PROPERTY(TYPath, Path);
    DEFINE_BYREF_RW_PROPERTY(yvector<TSharedRef>, Attachments);

public:
    typedef TIntrusivePtr<TYPathRequest> TPtr;
    
    TYPathRequest(const Stroka& verb);

    NBus::IMessage::TPtr Serialize();

protected:
    virtual TBlob SerializeBody() const = 0;

};

////////////////////////////////////////////////////////////////////////////////

template <class TRequestMessage, class TResponseMessage>
class TTypedYPathRequest
    : public TYPathRequest
    , public TRequestMessage
{
public:
    typedef TTypedYPathResponse<TRequestMessage, TResponseMessage> TTypedResponse;
    typedef TIntrusivePtr< TTypedYPathRequest<TRequestMessage, TResponseMessage> > TPtr;

    TTypedYPathRequest(const Stroka& verb)
        : TYPathRequest(verb)
    { }

protected:
    virtual TBlob SerializeBody() const
    {
        TBlob blob;
        YVERIFY(SerializeToProtobuf(this, &blob));
        return blob;
    }
};

////////////////////////////////////////////////////////////////////////////////

class TYPathResponse
    : public TRefCounted
    , public TEphemeralAttributeProvider
{
    DEFINE_BYVAL_RW_PROPERTY(TError, Error);
    DEFINE_BYREF_RW_PROPERTY(yvector<TSharedRef>, Attachments);

public:
    typedef TIntrusivePtr<TYPathResponse> TPtr;

    void Deserialize(NBus::IMessage* message);

    int GetErrorCode() const;
    bool IsOK() const;

    void ThrowIfError() const;

protected:
    virtual void DeserializeBody(const TRef& data);

};

////////////////////////////////////////////////////////////////////////////////

template <class TRequestMessage, class TResponseMessage>
class TTypedYPathResponse
    : public TYPathResponse
    , public TResponseMessage
{
public:
    typedef TIntrusivePtr< TTypedYPathResponse<TRequestMessage, TResponseMessage> > TPtr;

protected:
    virtual void DeserializeBody(const TRef& data)
    {
        YVERIFY(DeserializeFromProtobuf(this, data));
    }

};

////////////////////////////////////////////////////////////////////////////////

#define DEFINE_YPATH_PROXY_METHOD(ns, method) \
    typedef ::NYT::NYTree::TTypedYPathRequest<ns::TReq##method, ns::TRsp##method> TReq##method; \
    typedef ::NYT::NYTree::TTypedYPathResponse<ns::TReq##method, ns::TRsp##method> TRsp##method; \
    \
    static TReq##method::TPtr method(const NYT::NYTree::TYPath& path  = "") \
    { \
        auto req = New<TReq##method>(#method); \
        req->SetPath(path); \
        return req; \
    }

////////////////////////////////////////////////////////////////////////////////

TYPath ComputeResolvedYPath(
    const TYPath& wholePath,
    const TYPath& unresolvedPath);

TYPath CombineYPaths(
    const TYPath& path1,
    const TYPath& path2);

TYPath CombineYPaths(
    const TYPath& path1,
    const TYPath& path2,
    const TYPath& path3);

TYPath CombineYPaths(
    const TYPath& path1,
    const TYPath& path2,
    const TYPath& path3,
    const TYPath& path4);

TYPath EscapeYPath(const Stroka& value);
TYPath EscapeYPath(i64 value);

void ResolveYPath(
    IYPathService* rootService,
    const TYPath& path,
    const Stroka& verb,
    IYPathServicePtr* suffixService,
    TYPath* suffixPath);

//! Asynchronously executes an untyped YPath verb against the given service.
TFuture<NBus::IMessage::TPtr>::TPtr
ExecuteVerb(IYPathService* service, NBus::IMessage* requestMessage);

//! Asynchronously executes a request against the given service.
void ExecuteVerb(IYPathService* service, NRpc::IServiceContext* context);

//! Asynchronously executes a typed YPath requested against a given service.
template <class TTypedRequest>
TIntrusivePtr< TFuture< TIntrusivePtr<typename TTypedRequest::TTypedResponse> > >
ExecuteVerb(IYPathService* service, TTypedRequest* request);

//! Synchronously executes a typed YPath requested against a given service.
//! Throws if an error has occurred.
template <class TTypedRequest>
TIntrusivePtr<typename TTypedRequest::TTypedResponse>
SyncExecuteVerb(IYPathService* service, TTypedRequest* request);

//! Asynchronously executes "Get" verb. 
TFuture< TValueOrError<TYson> >::TPtr AsyncYPathGet(IYPathService* service, const TYPath& path);

//! Synchronously executes "Get" verb. Throws if an error has occurred.
TYson SyncYPathGet(IYPathService* service, const TYPath& path);

//! Synchronously executes "GetNode" verb. Throws if an error has occurred.
INodePtr SyncYPathGetNode(IYPathService* service, const TYPath& path);

//! Synchronously executes "Set" verb. Throws if an error has occurred.
void SyncYPathSet(IYPathService* service, const TYPath& path, const TYson& value);

//! Synchronously executes "SetNode" verb. Throws if an error has occurred.
void SyncYPathSetNode(IYPathService* service, const TYPath& path, INode* value);

//! Synchronously executes "Remove" verb. Throws if an error has occurred.
void SyncYPathRemove(IYPathService* service, const TYPath& path);

//! Synchronously executes "List" verb. Throws if an error has occurred.
yvector<Stroka> SyncYPathList(IYPathService* service, const TYPath& path);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT

#define YPATH_CLIENT_INL_H_
#include "ypath_client-inl.h"
#undef YPATH_CLIENT_INL_H_
