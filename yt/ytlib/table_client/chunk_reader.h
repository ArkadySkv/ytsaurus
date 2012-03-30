﻿#pragma once

#include "common.h"
#include "value.h"
#include "schema.h"
#include "async_reader.h"
#include "channel_reader.h"
#include <ytlib/table_client/table_chunk_meta.pb.h>
#include <ytlib/table_client/table_reader.pb.h>

#include <ytlib/chunk_client/async_reader.h>
#include <ytlib/chunk_client/sequential_reader.h>
#include <ytlib/misc/thread_affinity.h>
#include <ytlib/misc/async_stream_state.h>
#include <ytlib/misc/codec.h>

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

struct IValidator;

//! Reads single table chunk row-after-row using given #NChunkClient::IAsyncReader.
class TChunkReader
    : public IAsyncReader
{
public:
    typedef TIntrusivePtr<TChunkReader> TPtr;

    /*! 
     *  \param EndRow - if given value exceeds row count of the chunk,
     *  chunk is processed to the end without error. To guarantee reading
     *  chunk to the end, set it to numerical_limits<int>::max().
     */
    TChunkReader(
        NChunkClient::TSequentialReader::TConfig* config,
        const TChannel& channel,
        NChunkClient::IAsyncReader* chunkReader,
        const NProto::TReadLimit& startLimit,
        const NProto::TReadLimit& endLimit);

    TAsyncError::TPtr AsyncOpen();

    //! Asynchronously switches the reader to the next row.
    /*!
     *  This call cannot block.
     */
    TAsyncError::TPtr AsyncNextRow();

    bool IsValid() const;
    const TRow& GetCurrentRow() const;

private:
    TAsyncError::TPtr DoNextRow();

    TAsyncError::TPtr ContinueNextRow(
        TError error, 
        int channelIndex, 
        TAsyncError::TPtr result);

    void MakeCurrentRow();

    class TInitializer;
    TIntrusivePtr<TInitializer> Initializer;

    ICodec* Codec;
    NChunkClient::TSequentialReader::TPtr SequentialReader;

    TAsyncStreamState State;
    TChannel Channel;

    TRow CurrentRow;

    // ToDo(psushin): may be use vector TValue's here.
    TKey CurrentKey;

    struct TColumnInfo
    {
        int KeyIndex;
        bool InChannel;
        bool Used;

        TColumnInfo()
        : KeyIndex(-1)
        , InChannel(false)
        , Used(false)
        { }
    };

    yhash_map<TColumn, TColumnInfo> FixedColumns;
    yhash_set<TColumn> UsedRangeColumns;

    i64 CurrentRowIndex;
    i64 EndRowIndex;

    THolder<IValidator> EndValidator;

    std::vector<TChannelReader> ChannelReaders;

    DECLARE_THREAD_AFFINITY_SLOT(ClientThread);
    DECLARE_THREAD_AFFINITY_SLOT(ReaderThread);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
