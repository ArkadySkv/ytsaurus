﻿#pragma once

#include "public.h"
#include "config.h"
#include "chunk_replica.h"
#include "replication_writer.h"

#include <ytlib/misc/async_stream_state.h>

#include <ytlib/actions/parallel_awaiter.h>

#include <ytlib/table_client/table_reader.pb.h>

#include <ytlib/object_client/object_service_proxy.h>
#include <ytlib/object_client/master_ypath_proxy.h>

#include <ytlib/transaction_client/public.h>

#include <ytlib/logging/tagged_logger.h>

namespace NYT {
namespace NChunkClient {

////////////////////////////////////////////////////////////////////////////////

template <class TChunkWriter>
class TMultiChunkSequentialWriter
    : virtual public TRefCounted
{
public:
    typedef typename TChunkWriter::TProvider TProvider;
    typedef TIntrusivePtr<TProvider> TProviderPtr;

    typedef typename TChunkWriter::TFacade TFacade;

    TMultiChunkSequentialWriter(
        TMultiChunkWriterConfigPtr config,
        TMultiChunkWriterOptionsPtr options,
        TProviderPtr provider,
        NRpc::IChannelPtr masterChannel,
        const NTransactionClient::TTransactionId& transactionId,
        const TChunkListId& parentChunkListId);

    ~TMultiChunkSequentialWriter();

    TAsyncError AsyncOpen();
    TAsyncError AsyncClose();

    // Returns pointer to writer facade, which allows single write operation.
    // In nullptr is returned, caller should subscribe to ReadyEvent.
    TFacade* GetCurrentWriter();
    TAsyncError GetReadyEvent();

    void SetProgress(double progress);

    //! Only valid when the writer is closed.
    const std::vector<NTableClient::NProto::TInputChunk>& GetWrittenChunks() const;

    //! Provides node id to descriptor mapping for chunks returned via #GetWrittenChunks.
    NNodeTrackerClient::TNodeDirectoryPtr GetNodeDirectory() const;

    TProviderPtr GetProvider();

    //! Current row count.
    i64 GetRowCount() const;

    const TNullable<NTableClient::TKeyColumns>& GetKeyColumns() const;

protected:
    struct TSession
    {
        TIntrusivePtr<TChunkWriter> ChunkWriter;
        IAsyncWriterPtr RemoteWriter;
        std::vector<TChunkReplica> Replicas;
        TChunkId ChunkId;

        TSession()
            : ChunkWriter(NULL)
            , RemoteWriter(NULL)
        { }

        bool IsNull() const
        {
            return !ChunkWriter;
        }

        void Reset()
        {
            ChunkWriter.Reset();
            RemoteWriter.Reset();
            ChunkId = TChunkId();
        }
    };

    void CreateNextSession();
    virtual void InitCurrentSession(TSession nextSession);

    void OnChunkCreated(NObjectClient::TMasterYPathProxy::TRspCreateObjectPtr rsp);

    void FinishCurrentSession();

    void OnChunkClosed(
        int chunkIndex,
        TSession currentSession,
        TAsyncErrorPromise finishResult,
        TError error);

    void OnChunkConfirmed(
        TChunkId chunkId,
        TAsyncErrorPromise finishResult,
        NObjectClient::TObjectServiceProxy::TRspExecuteBatchPtr batchRsp);

    void OnChunkFinished(
        TChunkId chunkId,
        TError error);

    void OnRowWritten();

    void AttachChunks();
    void OnClose(NObjectClient::TObjectServiceProxy::TRspExecuteBatchPtr batchRsp);

    void SwitchSession();

    const TMultiChunkWriterConfigPtr Config;
    const TMultiChunkWriterOptionsPtr Options;
    const NRpc::IChannelPtr MasterChannel;
    const NTransactionClient::TTransactionId TransactionId;
    const TChunkListId ParentChunkListId;

    NNodeTrackerClient::TNodeDirectoryPtr NodeDirectory;

    const int UploadReplicationFactor;

    TProviderPtr Provider;

    volatile double Progress;

    //! Total compressed size of data in the completed chunks.
    i64 CompleteChunkSize;

    TAsyncStreamState State;

    TSession CurrentSession;
    TPromise<TSession> NextSession;

    TParallelAwaiterPtr CloseChunksAwaiter;

    TSpinLock WrittenChunksGuard;
    std::vector<NTableClient::NProto::TInputChunk> WrittenChunks;

    NLog::TTaggedLogger Logger;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT

#define MULTI_CHUNK_SEQUENTIAL_WRITER_INL_H_
#include "multi_chunk_sequential_writer-inl.h"
#undef MULTI_CHUNK_SEQUENTIAL_WRITER_INL_H_

