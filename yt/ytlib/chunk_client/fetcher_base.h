#pragma once

#include "public.h"

#include <ytlib/node_tracker_client/public.h>

#include <core/logging/tagged_logger.h>

#include <core/misc/error.h>

#include <core/rpc/public.h>

namespace NYT {
namespace NChunkClient {

////////////////////////////////////////////////////////////////////////////////

class TFetcherBase
    : public TRefCounted
{
public:
    TFetcherBase(
        TFetcherConfigPtr config,
        NNodeTrackerClient::TNodeDirectoryPtr nodeDirectory,
        IInvokerPtr invoker,
        NLog::TTaggedLogger& logger);

    virtual void AddChunk(TRefCountedChunkSpecPtr chunk);
    virtual TAsyncError Fetch();

protected:
    TFetcherConfigPtr Config_;
    NNodeTrackerClient::TNodeDirectoryPtr NodeDirectory_;
    IInvokerPtr Invoker_;

    //! All chunks for which info is to be fetched.
    std::vector<TRefCountedChunkSpecPtr> Chunks_;

    NLog::TTaggedLogger Logger;


    virtual TFuture<void> FetchFromNode(
        const NNodeTrackerClient::TNodeId& nodeId,
        std::vector<int>&& chunkIndexes) = 0;

    NRpc::IChannelPtr GetNodeChannel(const NNodeTrackerClient::TNodeId& nodeId);

    void StartFetchingRound();

    void OnChunkFailed(NNodeTrackerClient::TNodeId nodeId, int chunkIndex);
    void OnNodeFailed(NNodeTrackerClient::TNodeId nodeId, const std::vector<int>& chunkIndexes);

private:
    //! Indexes of chunks for which no info is fetched yet.
    yhash_set<int> UnfetchedChunkIndexes_;

    //! Ids of nodes that failed to reply.
    yhash_set<NNodeTrackerClient::TNodeId> DeadNodes_;

    //! |(nodeId, chunkId)| pairs for which an error was returned from the node.
    std::set< std::pair<NNodeTrackerClient::TNodeId, TChunkId> > DeadChunks_;

    TAsyncErrorPromise FetchingResult_;

    void OnFetchingRoundCompleted();

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
