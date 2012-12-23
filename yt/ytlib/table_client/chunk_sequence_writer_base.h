﻿#pragma once

#include "public.h"
#include "config.h"

#include <ytlib/misc/thread_affinity.h>
#include <ytlib/misc/async_stream_state.h>

#include <ytlib/actions/parallel_awaiter.h>

#include <ytlib/chunk_client/public.h>
#include <ytlib/chunk_client/remote_writer.h>
#include <ytlib/chunk_client/chunk_meta_extensions.h>

#include <ytlib/table_client/table_reader.pb.h>

#include <ytlib/object_client/object_service_proxy.h>

#include <ytlib/transaction_client/transaction.h>

#include <ytlib/transaction_client/transaction_ypath_proxy.h>

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

template <class TChunkWriter>
class TChunkSequenceWriterBase
    : virtual public TRefCounted
{
public:
    TChunkSequenceWriterBase(
        TTableWriterConfigPtr config,
        NRpc::IChannelPtr masterChannel,
        const NTransactionClient::TTransactionId& transactionId,
        const TNullable<Stroka>& account,
        const NChunkClient::TChunkListId& parentChunkListId,
        const TNullable<TKeyColumns>& keyColumns);

    ~TChunkSequenceWriterBase();

    TAsyncError AsyncOpen();
    virtual TAsyncError AsyncClose();

    TAsyncError GetReadyEvent();

    virtual bool TryWriteRow(const TRow& row);
    virtual bool TryWriteRowUnsafe(const TRow& row);

    void SetProgress(double progress);

    /*!
     *  To get consistent data, should be called only when the writer is closed.
     */
    const std::vector<NProto::TInputChunk>& GetWrittenChunks() const;

    //! Current row count.
    i64 GetRowCount() const;

    const TNullable<TKeyColumns>& GetKeyColumns() const;

protected:
    struct TSession
    {
        TIntrusivePtr<TChunkWriter> ChunkWriter;
        NChunkClient::TRemoteWriterPtr RemoteWriter;

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
        }
    };

    void CreateNextSession();
    virtual void InitCurrentSession(TSession nextSession);

    void OnChunkCreated(NTransactionClient::TTransactionYPathProxy::TRspCreateObjectPtr rsp);
    virtual void PrepareChunkWriter(TSession* newSession) = 0;

    void FinishCurrentSession();

    void OnChunkClosed(
        int chunkIndex,
        TSession currentSession,
        TAsyncErrorPromise finishResult,
        TError error);

    void OnChunkRegistered(
        NChunkClient::TChunkId chunkId,
        TAsyncErrorPromise finishResult,
        NObjectClient::TObjectServiceProxy::TRspExecuteBatchPtr batchRsp);

    void OnChunkFinished(
        NChunkClient::TChunkId chunkId,
        TError error);

    void OnRowWritten();

    void AttachChunks();
    void OnClose(NObjectClient::TObjectServiceProxy::TRspExecuteBatchPtr batchRsp);

    void SwitchSession();

    const TTableWriterConfigPtr Config;
    const int ReplicationFactor;
    const int UploadReplicationFactor;
    const NRpc::IChannelPtr MasterChannel;
    const NObjectClient::TTransactionId TransactionId;
    const TNullable<Stroka> Account;
    const NChunkClient::TChunkListId ParentChunkListId;
    const TNullable<TKeyColumns> KeyColumns;

    i64 RowCount;

    volatile double Progress;

    //! Total compressed size of data in the completed chunks.
    i64 CompleteChunkSize;

    TAsyncStreamState State;

    TSession CurrentSession;
    TPromise<TSession> NextSession;

    TParallelAwaiterPtr CloseChunksAwaiter;

    TSpinLock WrittenChunksGuard;
    std::vector<NProto::TInputChunk> WrittenChunks;

    NLog::TLogger& Logger;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT

#define CHUNK_SEQUENCE_WRITER_BASE_INL_H_
#include "chunk_sequence_writer_base-inl.h"
#undef CHUNK_SEQUENCE_WRITER_BASE_INL_H_

