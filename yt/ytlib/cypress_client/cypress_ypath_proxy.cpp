#include "stdafx.h"
#include "cypress_ypath_proxy.h"

#include <ytlib/ytree/attribute_helpers.h>

#include <ytlib/rpc/client.h>
#include <ytlib/rpc/service.h>

namespace NYT {
namespace NCypressClient {

using namespace NYTree;
using namespace NRpc;
using namespace NRpc::NProto;
using namespace NCypressClient::NProto;

////////////////////////////////////////////////////////////////////////////////

TStringBuf ObjectIdPathPrefix("#");

TYPath FromObjectId(const TObjectId& id)
{
    return Stroka(ObjectIdPathPrefix) + ToString(id);
}

TTransactionId GetTransactionId(IServiceContextPtr context)
{
    return GetTransactionId(context->RequestHeader());
}

TTransactionId GetTransactionId(const TRequestHeader& header)
{
    return header.HasExtension(TTransactionalExt::transaction_id)
           ? FromProto<TTransactionId>(header.GetExtension(TTransactionalExt::transaction_id))
           : NullTransactionId;
}

void SetTransactionId(IClientRequestPtr request, const TTransactionId& transactionId)
{
    SetTransactionId(&request->Header(), transactionId);
}

void SetTransactionId(NRpc::NProto::TRequestHeader* header, const TTransactionId& transactionId)
{
    ToProto(header->MutableExtension(TTransactionalExt::transaction_id), transactionId);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NCypressClient
} // namespace NYT

