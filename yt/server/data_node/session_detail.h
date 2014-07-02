#pragma once

#include "public.h"
#include "session.h"

#include <core/concurrency/thread_affinity.h>

#include <core/logging/tagged_logger.h>

#include <core/profiling/profiler.h>

#include <server/cell_node/public.h>

namespace NYT {
namespace NDataNode {

////////////////////////////////////////////////////////////////////////////////

class TSessionBase
    : public ISession
{
public:
    TSessionBase(
        TDataNodeConfigPtr config,
        NCellNode::TBootstrap* bootstrap,
        const TChunkId& chunkId,
        const TSessionOptions& options,
        TLocationPtr location);

    ~TSessionBase();

    virtual const TChunkId& GetChunkId() const override;
    virtual EWriteSessionType GetType() const override;
    TLocationPtr GetLocation() const override;

    virtual void Start(TLeaseManager::TLease lease) override;

    virtual void Ping() override;

    virtual void Cancel(const TError& error) override;

    virtual TFuture<TErrorOr<IChunkPtr>> Finish(
        const NChunkClient::NProto::TChunkMeta& chunkMeta) override;

    virtual TAsyncError PutBlocks(
        int startBlockIndex,
        const std::vector<TSharedRef>& blocks,
        bool enableCaching) override;

    virtual TAsyncError SendBlocks(
        int startBlockIndex,
        int blockCount,
        const NNodeTrackerClient::TNodeDescriptor& target) override;

    virtual TAsyncError FlushBlocks(int blockIndex) override;

    DEFINE_SIGNAL(void(const TError& error), Finished);

protected:
    TDataNodeConfigPtr Config_;
    NCellNode::TBootstrap* Bootstrap_;
    TChunkId ChunkId_;
    TSessionOptions Options_;
    TLocationPtr Location_;

    IInvokerPtr WriteInvoker_;

    bool Active_ = false;
    TLeaseManager::TLease Lease_;

    NLog::TTaggedLogger Logger;
    NProfiling::TProfiler Profiler;


    virtual void DoStart() = 0;
    virtual void DoCancel() = 0;
    virtual TFuture<TErrorOr<IChunkPtr>> DoFinish(
        const NChunkClient::NProto::TChunkMeta& chunkMeta) = 0;
    virtual TAsyncError DoPutBlocks(
        int startBlockIndex,
        const std::vector<TSharedRef>& blocks,
        bool enableCaching) = 0;
    virtual TAsyncError DoSendBlocks(
        int startBlockIndex,
        int blockCount,
        const NNodeTrackerClient::TNodeDescriptor& target) = 0;
    virtual TAsyncError DoFlushBlocks(int blockIndex) = 0;

    void ValidateActive();

    DECLARE_THREAD_AFFINITY_SLOT(ControlThread);
    DECLARE_THREAD_AFFINITY_SLOT(WriterThread);

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NDataNode
} // namespace NYT

