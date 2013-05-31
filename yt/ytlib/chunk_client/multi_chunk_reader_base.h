#pragma once

#include "public.h"
#include "chunk_spec.h"

#include <ytlib/node_tracker_client/public.h>
#include <ytlib/misc/async_stream_state.h>
#include <ytlib/rpc/public.h>
#include <ytlib/actions/parallel_awaiter.h>

#include <ytlib/logging/log.h>

namespace NYT {
namespace NChunkClient {

////////////////////////////////////////////////////////////////////////////////

template <class TChunkReader>
class TMultiChunkReaderBase
    : public virtual TRefCounted
{
    DEFINE_BYVAL_RO_PROPERTY(volatile bool, IsFetchingComplete);

public:
    typedef TIntrusivePtr<typename TChunkReader::TProvider> TProviderPtr;

    // Facade provides item-level fine grained api, which differs from one chunk reader
    // to another and therefore cannot be exposed by multi chunk reader
    // (e.g compare facades for TableChunkReader and PartitionChunkReader).
    typedef typename TChunkReader::TFacade TFacade;

    TMultiChunkReaderBase(
        TMultiChunkReaderConfigPtr config,
        NRpc::IChannelPtr masterChannel,
        NChunkClient::IBlockCachePtr blockCache,
        NNodeTrackerClient::TNodeDirectoryPtr nodeDirectory,
        std::vector<NChunkClient::NProto::TChunkSpec>&& chunkSpecs,
        const TProviderPtr& readerProvider);

    virtual TAsyncError AsyncOpen() = 0;

    virtual bool FetchNext() = 0;

    // If nullptr is returned - reader is finished.
    const TFacade* GetFacade() const;
    TAsyncError GetReadyEvent();

    std::vector<NChunkClient::TChunkId> GetFailedChunks() const;

    TProviderPtr GetProvider();

protected:
    typedef TIntrusivePtr<TChunkReader> TReaderPtr;

    struct TSession
    {
        TReaderPtr Reader;
        int ChunkIndex;

        TSession()
            : ChunkIndex(-1)
        { }

        TSession(TReaderPtr reader, int chunkIndex)
            : Reader(reader)
            , ChunkIndex(chunkIndex)
        { }
    };

    TMultiChunkReaderConfigPtr Config;
    int PrefetchWindow;

    NRpc::IChannelPtr MasterChannel;
    NChunkClient::IBlockCachePtr BlockCache;
    NNodeTrackerClient::TNodeDirectoryPtr NodeDirectory;

    std::vector<NChunkClient::NProto::TChunkSpec> ChunkSpecs;

    TProviderPtr ReaderProvider;

    TSession CurrentSession;

    TAsyncStreamState State;

    // Protects LastPreparedReader;
    TSpinLock NextChunkLock;
    volatile int LastPreparedReader;

    TParallelAwaiterPtr FetchingCompleteAwaiter;

    TSpinLock FailedChunksLock;
    std::vector<NChunkClient::TChunkId> FailedChunks;

    NLog::TLogger& Logger;
    DECLARE_THREAD_AFFINITY_SLOT(ReaderThread);

    virtual void OnReaderOpened(const TSession& session, TError error) = 0;

    void PrepareNextChunk();
    void ProcessOpenedReader(const TSession& session);
    void ProcessFinishedReader(const  TSession& reader);
    void AddFailedChunk(const TSession& session);

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT

#define MULTI_CHUNK_READER_BASE_INL_H_
#include "multi_chunk_reader_base-inl.h"
#undef MULTI_CHUNK_READER_BASE_INL_H_
