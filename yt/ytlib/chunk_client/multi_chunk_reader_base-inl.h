#ifndef MULTI_CHUNK_READER_BASE_INL_H_
#error "Direct inclusion of this file is not allowed, include multi_chunk_reader_base.h"
#endif
#undef MULTI_CHUNK_READER_BASE_INL_H_

#include "private.h"
#include "config.h"

#include "block_cache.h"
#include "async_reader.h"
#include "dispatcher.h"
#include "chunk_meta_extensions.h"
#include "replication_reader.h"
#include "erasure_reader.h"

#include <ytlib/erasure/codec.h>

#include <ytlib/node_tracker_client/node_directory.h>

#include <ytlib/rpc/channel.h>

#include <ytlib/misc/protobuf_helpers.h>

namespace NYT {
namespace NChunkClient {

////////////////////////////////////////////////////////////////////////////////

template <class TChunkReader>
TMultiChunkReaderBase<TChunkReader>::TMultiChunkReaderBase(
    TMultiChunkReaderConfigPtr config,
    NRpc::IChannelPtr masterChannel,
    NChunkClient::IBlockCachePtr blockCache,
    NNodeTrackerClient::TNodeDirectoryPtr nodeDirectory,
    std::vector<NChunkClient::NProto::TChunkSpec>&& chunkSpecs,
    const TProviderPtr& readerProvider)
    : IsFetchingComplete_(false)
    , Config(config)
    , MasterChannel(masterChannel)
    , BlockCache(blockCache)
    , NodeDirectory(nodeDirectory)
    , ChunkSpecs(chunkSpecs)
    , ReaderProvider(readerProvider)
    , LastPreparedReader(-1)
    , FetchingCompleteAwaiter(New<TParallelAwaiter>())
    , Logger(ChunkReaderLogger)
{
    std::vector<i64> chunkDataSizes;
    chunkDataSizes.reserve(ChunkSpecs.size());

    FOREACH (const auto& chunkSpec, ChunkSpecs) {
        i64 dataSize;
        NChunkClient::GetStatistics(chunkSpec, &dataSize);
        chunkDataSizes.push_back(dataSize);
    }

    if (ReaderProvider->KeepInMemory()) {
        PrefetchWindow = MaxPrefetchWindow;
    } else {
        std::sort(chunkDataSizes.begin(), chunkDataSizes.end(), std::greater<i64>());

        PrefetchWindow = 0;
        i64 bufferSize = 0;
        while (PrefetchWindow < chunkDataSizes.size()) {
            bufferSize += std::min(
                chunkDataSizes[PrefetchWindow],
                config->WindowSize + config->GroupSize) + ChunkReaderMemorySize;
            if (bufferSize > Config->MaxBufferSize) {
                break;
            } else {
                ++PrefetchWindow;
            }
        }

        PrefetchWindow = std::min(PrefetchWindow, MaxPrefetchWindow);
        PrefetchWindow = std::max(PrefetchWindow, 1);
    }
    LOG_DEBUG("Preparing reader (PrefetchWindow: %d)",
        PrefetchWindow);
}

template <class TChunkReader>
void TMultiChunkReaderBase<TChunkReader>::PrepareNextChunk()
{
    int chunkSpecsCount = static_cast<int>(ChunkSpecs.size());

    int chunkIndex = -1;

    {
        TGuard<TSpinLock> guard(NextChunkLock);
        LastPreparedReader = std::min(LastPreparedReader + 1, chunkSpecsCount);
        if (LastPreparedReader == chunkSpecsCount) {
            return;
        }
        chunkIndex = LastPreparedReader;
    }

    TSession session;
    session.ChunkIndex = chunkIndex;
    const auto& chunkSpec = ChunkSpecs[chunkIndex];

    auto chunkId = NYT::FromProto<TChunkId>(chunkSpec.chunk_id());
    auto replicas = NYT::FromProto<TChunkReplica, TChunkReplicaList>(chunkSpec.replicas());

    LOG_DEBUG("Opening chunk (ChunkIndex: %d, ChunkId: %s)",
        chunkIndex,
        ~ToString(chunkId));

    IAsyncReaderPtr asyncReader;
    if (IsErasureChunkId(chunkId)) {
        std::sort(
            replicas.begin(),
            replicas.end(),
            [] (TChunkReplica lhs, TChunkReplica rhs) {
                return lhs.GetIndex() < rhs.GetIndex();
            });

        auto erasureCodecId = NErasure::ECodec(chunkSpec.erasure_codec());
        auto* erasureCodec = NErasure::GetCodec(erasureCodecId);
        auto dataPartCount = erasureCodec->GetDataPartCount();

        std::vector<IAsyncReaderPtr> readers;
        readers.reserve(dataPartCount);

        {
            auto it = replicas.begin();
            while (it != replicas.end()) {
                auto jt = it;
                while (jt != replicas.end() && it->GetIndex() == jt->GetIndex()) {
                    ++jt;
                }

                TChunkReplicaList partReplicas(it, jt);
                auto partId = ErasurePartIdFromChunkId(chunkId, it->GetIndex());
                auto reader = CreateReplicationReader(
                    Config,
                    BlockCache,
                    MasterChannel,
                    NodeDirectory,
                    Null,
                    partId,
                    partReplicas);
                readers.push_back(reader);

                it = jt;
            }
        }
        YCHECK(readers.size() == dataPartCount);
        asyncReader = CreateNonReparingErasureReader(readers);
    } else {
        asyncReader = CreateReplicationReader(
            Config,
            BlockCache,
            MasterChannel,
            NodeDirectory,
            Null,
            chunkId,
            replicas);
    }

    session.Reader = ReaderProvider->CreateReader(chunkSpec, asyncReader);

    session.Reader->AsyncOpen()
        .Subscribe(BIND(
            &TMultiChunkReaderBase<TChunkReader>::OnReaderOpened,
            MakeWeak(this),
            session)
        .Via(NChunkClient::TDispatcher::Get()->GetReaderInvoker()));
}

template <class TChunkReader>
void TMultiChunkReaderBase<TChunkReader>::ProcessOpenedReader(const TSession& session)
{
    LOG_DEBUG("Chunk opened (ChunkIndex: %d)", session.ChunkIndex);
    ReaderProvider->OnReaderOpened(session.Reader, ChunkSpecs[session.ChunkIndex]);

    FetchingCompleteAwaiter->Await(session.Reader->GetFetchingCompleteEvent());
    if (FetchingCompleteAwaiter->GetRequestCount() == ChunkSpecs.size()) {
        auto this_ = MakeStrong(this);
        FetchingCompleteAwaiter->Complete(BIND([=]() {
            this_->IsFetchingComplete_ = true;
        }));
    }
}

template <class TChunkReader>
void TMultiChunkReaderBase<TChunkReader>::ProcessFinishedReader(const TSession& session)
{
    ReaderProvider->OnReaderFinished(session.Reader);
}

template <class TChunkReader>
void TMultiChunkReaderBase<TChunkReader>::AddFailedChunk(const TSession& session)
{
    const auto& chunkSpec = ChunkSpecs[session.ChunkIndex];
    auto chunkId = NYT::FromProto<NChunkClient::TChunkId>(chunkSpec.chunk_id());
    LOG_DEBUG("Failed chunk added (ChunkId: %s)", ~ToString(chunkId));
    TGuard<TSpinLock> guard(FailedChunksLock);
    FailedChunks.push_back(chunkId);
}

template <class TChunkReader>
std::vector<NChunkClient::TChunkId> TMultiChunkReaderBase<TChunkReader>::GetFailedChunks() const
{
    TGuard<TSpinLock> guard(FailedChunksLock);
    return FailedChunks;
}

template <class TChunkReader>
TAsyncError TMultiChunkReaderBase<TChunkReader>::GetReadyEvent()
{
    return State.GetOperationError();
}

template <class TChunkReader>
auto TMultiChunkReaderBase<TChunkReader>::GetFacade() const -> const TFacade*
{
    YASSERT(!State.HasRunningOperation());
    if (CurrentSession.Reader) {
        return CurrentSession.Reader->GetFacade();
    } else {
        return nullptr;
    }
}

template <class TChunkReader>
auto TMultiChunkReaderBase<TChunkReader>::GetProvider() -> TProviderPtr
{
    return ReaderProvider;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
