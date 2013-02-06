#include "stdafx.h"
#include "chunk_list_pool.h"
#include "private.h"

#include <ytlib/cypress_client/cypress_ypath_proxy.h>

#include <ytlib/transaction_client/transaction_ypath_proxy.h>

#include <server/chunk_server/chunk_list.h>

namespace NYT {
namespace NScheduler {

using namespace NCypressClient;
using namespace NObjectClient;
using namespace NTransactionClient;
using namespace NChunkClient;

////////////////////////////////////////////////////////////////////////////////

TChunkListPool::TChunkListPool(
    TSchedulerConfigPtr config,
    NRpc::IChannelPtr masterChannel,
    IInvokerPtr controlInvoker,
    const TOperationId& operationId,
    const NTransactionClient::TTransactionId& transactionId)
    : Config(config)
    , MasterChannel(masterChannel)
    , ControlInvoker(controlInvoker)
    , OperationId(operationId)
    , TransactionId(transactionId)
    , Logger(OperationLogger)
    , RequestInProgress(false)
    , LastSuccessCount(-1)
{
    YCHECK(config);
    YCHECK(masterChannel);
    YCHECK(controlInvoker);

    Logger.AddTag(Sprintf("OperationId: %s", ~ToString(operationId)));
}

bool TChunkListPool::HasEnough(int requestedCount)
{
    int currentSize = static_cast<int>(Ids.size());
    if (currentSize >= requestedCount + Config->ChunkListWatermarkCount) {
        // Enough chunk lists. Above the watermark even after extraction.
        return true;
    } else {
        // Additional chunk lists are definitely needed but still could be a success.
        AllocateMore();
        return currentSize >= requestedCount;
    }
}

TChunkListId TChunkListPool::Extract()
{
    YCHECK(!Ids.empty());
    auto id = Ids.back();
    Ids.pop_back();

    LOG_DEBUG("Extracted chunk list %s from the pool, %d remaining",
        ~id.ToString(),
        static_cast<int>(Ids.size()));

    return id;
}

void TChunkListPool::Release(const std::vector<TChunkListId>& ids)
{
    TObjectServiceProxy objectProxy(MasterChannel);
    auto batchReq = objectProxy.ExecuteBatch();
    FOREACH (const auto& id, ids) {
        auto req = TTransactionYPathProxy::UnstageObject(FromObjectId(TransactionId));
        *req->mutable_object_id() = id.ToProto();
        req->set_recursive(true);
        batchReq->AddRequest(req);
    }

    // Fire-and-forget.
    // The subscriber is only needed to log the outcome.
    batchReq->Invoke().Subscribe(
        BIND(&TChunkListPool::OnChunkListsReleased, MakeStrong(this)));
}

void TChunkListPool::AllocateMore()
{
    int count =
        LastSuccessCount < 0
        ? Config->ChunkListPreallocationCount
        : static_cast<int>(LastSuccessCount * Config->ChunkListAllocationMultiplier);
    
    count = std::min(count, Config->MaxChunkListAllocationCount);

    if (RequestInProgress) {
        LOG_DEBUG("Cannot allocate more chunk lists, another request is in progress");
        return;
    }

    LOG_INFO("Allocating %d chunk lists for pool", count);

    TObjectServiceProxy objectProxy(MasterChannel);
    auto batchReq = objectProxy.ExecuteBatch();

    for (int index = 0; index < count; ++index) {
        auto req = TTransactionYPathProxy::CreateObject(FromObjectId(TransactionId));
        req->set_type(EObjectType::ChunkList);
        batchReq->AddRequest(req);
    }

    batchReq->Invoke().Subscribe(
        BIND(&TChunkListPool::OnChunkListsCreated, MakeWeak(this), count)
            .Via(ControlInvoker));

    RequestInProgress = true;
}

void TChunkListPool::OnChunkListsCreated(
    int count,
    TObjectServiceProxy::TRspExecuteBatchPtr batchRsp)
{
    YCHECK(RequestInProgress);
    RequestInProgress = false;

    if (!batchRsp->IsOK()) {
        LOG_ERROR(*batchRsp, "Error allocating chunk lists");
        return;
    }

    LOG_INFO("Chunk lists allocated");

    auto rsps = batchRsp->GetResponses<TTransactionYPathProxy::TRspCreateObject>();
    FOREACH (auto rsp, rsps) {
        if (rsp->IsOK()) {
            Ids.push_back(TChunkListId::FromProto(rsp->object_id()));
        } else {
            LOG_ERROR(*rsp, "Error allocating chunk list");
        }
    }

    LastSuccessCount = count;
}

void TChunkListPool::OnChunkListsReleased(TObjectServiceProxy::TRspExecuteBatchPtr batchRsp)
{
    auto error = batchRsp->GetCumulativeError();
    if (!error.IsOK()) {
        LOG_WARNING(error, "Error releasing chunk lists");
    }
}

////////////////////////////////////////////////////////////////////////////////
            
} // namespace NScheduler
} // namespace NYT
