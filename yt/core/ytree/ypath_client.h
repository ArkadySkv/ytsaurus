#pragma once

#include "public.h"
#include "ypath_service.h"
#include "ephemeral_attribute_owner.h"
#include "attribute_provider.h"

#include <core/misc/ref.h>
#include <core/misc/property.h>

#include <core/rpc/client.h>

#include <core/ytree/ypath.pb.h>

namespace NYT {
namespace NYTree {

////////////////////////////////////////////////////////////////////////////////

class TYPathRequest
    : public TEphemeralAttributeOwner
    , public NRpc::IClientRequest
{
public:
    explicit TYPathRequest(const NRpc::NProto::TRequestHeader& header);

    TYPathRequest(
        const Stroka& service,
        const Stroka& verb,
        const NYPath::TYPath& path,
        bool mutating);

    virtual bool IsOneWay() const override;
    virtual NRpc::TRequestId GetRequestId() const override;

    virtual const Stroka& GetVerb() const override;
    virtual const Stroka& GetService() const override;

    const Stroka& GetPath() const;
    void SetPath(const Stroka& path);

    virtual TInstant GetStartTime() const override;
    virtual void SetStartTime(TInstant value) override;
    
    virtual const NYTree::IAttributeDictionary& Attributes() const override;
    virtual NYTree::IAttributeDictionary* MutableAttributes() override;

    virtual const NRpc::NProto::TRequestHeader& Header() const override;
    virtual NRpc::NProto::TRequestHeader& Header() override;

    virtual TSharedRefArray Serialize() const override;

protected:
    NRpc::NProto::TRequestHeader Header_;
    std::vector<TSharedRef> Attachments_;

    virtual bool IsRequestHeavy() const override;
    virtual bool IsResponseHeavy() const override;

    virtual TSharedRef SerializeBody() const = 0;

};

////////////////////////////////////////////////////////////////////////////////

template <class TRequestMessage, class TResponseMessage>
class TTypedYPathRequest
    : public TYPathRequest
    , public TRequestMessage
{
public:
    typedef TTypedYPathResponse<TRequestMessage, TResponseMessage> TTypedResponse;

    explicit TTypedYPathRequest(const NRpc::NProto::TRequestHeader& header)
        : TYPathRequest(header)
    { }

    TTypedYPathRequest(
        const Stroka& service,
        const Stroka& verb,
        const NYPath::TYPath& path,
        bool mutating)
        : TYPathRequest(
            service,
            verb,
            path,
            mutating)
    { }

protected:
    virtual TSharedRef SerializeBody() const override
    {
        TSharedRef data;
        YCHECK(SerializeToProtoWithEnvelope(*this, &data));
        return data;
    }

};

////////////////////////////////////////////////////////////////////////////////

class TYPathResponse
    : public TRefCounted
    , public TEphemeralAttributeOwner
{
    DEFINE_BYVAL_RW_PROPERTY(TError, Error);
    DEFINE_BYREF_RW_PROPERTY(std::vector<TSharedRef>, Attachments);

public:
    void Deserialize(TSharedRefArray message);

    bool IsOK() const;
    operator TError() const;

protected:
    virtual void DeserializeBody(const TRef& data);

};

////////////////////////////////////////////////////////////////////////////////

template <class TRequestMessage, class TResponseMessage>
class TTypedYPathResponse
    : public TYPathResponse
    , public TResponseMessage
{
protected:
    virtual void DeserializeBody(const TRef& data) override
    {
        YCHECK(DeserializeFromProtoWithEnvelope(this, data));
    }

};

////////////////////////////////////////////////////////////////////////////////

#define DEFINE_YPATH_PROXY_METHOD_IMPL(ns, method, isMutating) \
    typedef ::NYT::NYTree::TTypedYPathRequest<ns::TReq##method, ns::TRsp##method> TReq##method; \
    typedef ::NYT::NYTree::TTypedYPathResponse<ns::TReq##method, ns::TRsp##method> TRsp##method; \
    typedef TIntrusivePtr<TReq##method> TReq##method##Ptr; \
    typedef TIntrusivePtr<TRsp##method> TRsp##method##Ptr; \
    \
    static TReq##method##Ptr method(const NYT::NYPath::TYPath& path = "") \
    { \
        return New<TReq##method>(GetServiceName(), #method, path, isMutating); \
    }

#define DEFINE_YPATH_PROXY_METHOD(ns, method) \
    DEFINE_YPATH_PROXY_METHOD_IMPL(ns, method, false)

#define DEFINE_MUTATING_YPATH_PROXY_METHOD(ns, method) \
    DEFINE_YPATH_PROXY_METHOD_IMPL(ns, method, true)

////////////////////////////////////////////////////////////////////////////////

const TYPath& GetRequestYPath(NRpc::IServiceContextPtr context);

const TYPath& GetRequestYPath(const NRpc::NProto::TRequestHeader& header);

void SetRequestYPath(NRpc::NProto::TRequestHeader* header, const TYPath& path);

TYPath ComputeResolvedYPath(
    const TYPath& wholePath,
    const TYPath& unresolvedPath);

//! Runs a sequence of IYPathService::Resolve calls aimed to discover the
//! ultimate endpoint responsible for serving a given request.
void ResolveYPath(
    IYPathServicePtr rootService,
    NRpc::IServiceContextPtr context,
    IYPathServicePtr* suffixService,
    TYPath* suffixPath);

//! Asynchronously executes an untyped request against a given service.
TFuture<TSharedRefArray>
ExecuteVerb(
    IYPathServicePtr service,
    TSharedRefArray requestMessage);

//! Asynchronously executes a request against a given service.
void ExecuteVerb(
    IYPathServicePtr service,
    NRpc::IServiceContextPtr context);

//! Asynchronously executes a typed YPath request against a given service.
template <class TTypedRequest>
TFuture< TIntrusivePtr<typename TTypedRequest::TTypedResponse> >
ExecuteVerb(
    IYPathServicePtr service,
    TIntrusivePtr<TTypedRequest> request);

//! Synchronously executes a typed YPath request against a given service.
//! Throws if an error has occurred.
template <class TTypedRequest>
TIntrusivePtr<typename TTypedRequest::TTypedResponse>
SyncExecuteVerb(
    IYPathServicePtr service,
    TIntrusivePtr<TTypedRequest> request);

//! Synchronously executes |GetKey| verb. Throws if an error has occurred.
Stroka SyncYPathGetKey(
    IYPathServicePtr service,
    const TYPath& path);

//! Asynchronously executes |Get| verb.
TFuture< TErrorOr<TYsonString> > AsyncYPathGet(
    IYPathServicePtr service,
    const TYPath& path,
    const TAttributeFilter& attributeFilter = TAttributeFilter::None,
    bool ignoreOpaque = false);

//! Synchronously executes |Get| verb. Throws if an error has occurred.
TYsonString SyncYPathGet(
    IYPathServicePtr service,
    const TYPath& path,
    const TAttributeFilter& attributeFilter = TAttributeFilter::None,
    bool ignoreOpaque = false);

//! Asynchronously executes |Exists| verb.
TFuture< TErrorOr<bool> > AsyncYPathExists(
    IYPathServicePtr service,
    const TYPath& path);

//! Synchronously executes |Exists| verb. Throws if an error has occurred.
bool SyncYPathExists(
    IYPathServicePtr service,
    const TYPath& path);

//! Synchronously executes |Set| verb. Throws if an error has occurred.
void SyncYPathSet(
    IYPathServicePtr service,
    const TYPath& path,
    const TYsonString& value);

//! Synchronously executes |Remove| verb. Throws if an error has occurred.
void SyncYPathRemove(
    IYPathServicePtr service,
    const TYPath& path,
    bool recursive = true,
    bool force = false);

//! Synchronously executes |List| verb. Throws if an error has occurred.
std::vector<Stroka> SyncYPathList(
    IYPathServicePtr service,
    const TYPath& path);

//! Overrides a part of #root tree.
/*!
 *  #overrideString must have the |path = value| format.
 *  The method updates #root by setting |value| (forcing those parts of |path| that are missing).
 */
void ApplyYPathOverride(
    INodePtr root,
    const TStringBuf& overrideString);

/*!
 *  Throws exception if the specified node does not exist.
 */
INodePtr GetNodeByYPath(INodePtr root, const TYPath& path);

void SetNodeByYPath(INodePtr root, const TYPath& path, INodePtr value);

//! Creates missing maps along #path.
/*!
 *  E.g. if #root is an empty map and #path is |/a/b/c| then
 *  nested maps |a| and |b| get created. Note that the final key (i.e. |c|)
 *  is not forced (since we have no idea of its type anyway).
 */
void ForceYPath(INodePtr root, const TYPath& path);

//! Computes a full YPath for a given #node and (optionally) returns the root.
TYPath GetNodeYPath(INodePtr node, INodePtr* root = nullptr);

//! Constructs an ephemeral deep copy of #node.
INodePtr CloneNode(INodePtr node);

//! Applies changes given by #patch to #base.
//! Returns the resulting tree.
INodePtr UpdateNode(INodePtr base, INodePtr patch);

//! Checks given nodes for deep equality.
// TODO(babenko): currently it ignores attributes
bool AreNodesEqual(INodePtr lhs, INodePtr rhs);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT

#define YPATH_CLIENT_INL_H_
#include "ypath_client-inl.h"
#undef YPATH_CLIENT_INL_H_
