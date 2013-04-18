#pragma once

#include "multi_chunk_reader_base.h"

namespace NYT {
namespace NChunkClient {

////////////////////////////////////////////////////////////////////////////////

template <class TChunkReader>
class TMultiChunkParallelReader
    : public TMultiChunkReaderBase<TChunkReader>
{
public:
    typedef TMultiChunkReaderBase<TChunkReader> TBase;

    TMultiChunkParallelReader(
        TMultiChunkReaderConfigPtr config,
        NRpc::IChannelPtr masterChannel,
        NChunkClient::IBlockCachePtr blockCache,
        NNodeTrackerClient::TNodeDirectoryPtr nodeDirectory,
        std::vector<NChunkClient::NProto::TInputChunk>&& inputChunks,
        const typename TBase::TProviderPtr& readerProvider);

    virtual TAsyncError AsyncOpen() override;
    virtual bool FetchNext() override;

private:
    using TBase::State;
    using TBase::Logger;
    using TBase::InputChunks;
    using TBase::PrefetchWindow;
    using TBase::CurrentSession;
    using TBase::ReaderProvider;

    // Protects CompleteReaderCount, ReadySessions, CurrentSession.
    TSpinLock SpinLock;
    std::vector<typename TBase::TSession> ReadySessions;
    std::vector<typename TBase::TSession> CompleteSessions;

    volatile int CompleteReaderCount;

    virtual void OnReaderOpened(
        const typename TBase::TSession& session,
        TError error) override;

    void OnReaderReady(const typename TBase::TSession& session, TError error);

    void ProcessReadyReader(typename TBase::TSession session);
    void FinishReader(const typename TBase::TSession& session);

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT

#define MULTI_CHUNK_PARALLEL_READER_INL_H_
#include "multi_chunk_parallel_reader-inl.h"
#undef MULTI_CHUNK_PARALLEL_READER_INL_H_
