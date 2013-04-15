﻿#include "stdafx.h"
#include "private.h"
#include "partition_chunk_reader.h"

#include <ytlib/chunk_client/dispatcher.h>
#include <ytlib/chunk_client/sequential_reader.h>
#include <ytlib/chunk_client/config.h>
#include <ytlib/chunk_client/chunk_meta_extensions.h>

#include <ytlib/table_client/table_chunk_meta.pb.h>
#include <ytlib/table_client/chunk_meta_extensions.h>

#include <ytlib/yson/varint.h>

namespace NYT {
namespace NTableClient {

using namespace NChunkClient;
using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

// ToDo(psushin): eliminate copy-paste from table_chunk_reader.cpp

TPartitionChunkReader::TPartitionChunkReader(
    const NChunkClient::TSequentialReaderConfigPtr& sequentialReader,
    const NChunkClient::IAsyncReaderPtr& asyncReader,
    int partitionTag,
    NCompression::ECodec codecId)
    : RowPointer_(NULL)
    , RowCount_(0)
    , SequentialConfig(sequentialReader)
    , AsyncReader(asyncReader)
    , CurrentRowIndex(0)
    , PartitionTag(partitionTag)
    , CodecId(codecId)
    , Logger(TableReaderLogger)
{ }

TAsyncError TPartitionChunkReader::AsyncOpen()
{
    State.StartOperation();

    Logger.AddTag(Sprintf("ChunkId: %s", ~ToString(AsyncReader->GetChunkId())));

    std::vector<int> tags;
    tags.push_back(TProtoExtensionTag<NProto::TChannelsExt>::Value);

    LOG_INFO("Requesting chunk meta");
    AsyncReader->AsyncGetChunkMeta(PartitionTag, &tags).Subscribe(
        BIND(&TPartitionChunkReader::OnGotMeta, MakeWeak(this))
            .Via(NChunkClient::TDispatcher::Get()->GetReaderInvoker()));

    return State.GetOperationError();
}

void TPartitionChunkReader::OnGotMeta(NChunkClient::IAsyncReader::TGetMetaResult result)
{
    if (!result.IsOK()) {
        OnFail(result);
        return;
    }

    LOG_INFO("Chunk meta received");

    if (result.Value().type() != EChunkType::Table) {
        LOG_FATAL("Invalid chunk type %d", result.Value().type());
    }

    if (result.Value().version() != FormatVersion) {
        OnFail(TError("Invalid chunk format version: expected %d, actual %d",
            FormatVersion,
            result.Value().version()));
        return;
    }

    auto channelsExt = GetProtoExtension<NProto::TChannelsExt>(result.Value().extensions());
    YCHECK(channelsExt.items_size() == 1);

    std::vector<TSequentialReader::TBlockInfo> blockSequence;
    {
        for (int i = 0; i < channelsExt.items(0).blocks_size(); ++i) {
            const auto& blockInfo = channelsExt.items(0).blocks(i);
            YCHECK(PartitionTag == blockInfo.partition_tag());
            blockSequence.push_back(TSequentialReader::TBlockInfo(
                blockInfo.block_index(),
                blockInfo.block_size()));
            RowCount_ += blockInfo.row_count();
        }
    }

    SequentialReader = New<TSequentialReader>(
        SequentialConfig,
        std::move(blockSequence),
        AsyncReader,
        CodecId);

    LOG_INFO("Reading %d blocks for partition %d",
        static_cast<int>(blockSequence.size()),
        PartitionTag);

    Blocks.reserve(blockSequence.size());

    if (SequentialReader->HasNext()) {
        SequentialReader->AsyncNextBlock().Subscribe(BIND(
            &TPartitionChunkReader::OnNextBlock,
            MakeWeak(this)));
    } else {
        State.FinishOperation();
    }
}

void TPartitionChunkReader::OnNextBlock(TError error)
{
    if (!error.IsOK()) {
        State.Fail(error);
        return;
    }

    LOG_DEBUG("Switching to next block at row %" PRId64, CurrentRowIndex);

    Blocks.push_back(SequentialReader->GetBlock());
    YCHECK(Blocks.back().Size() > 0);

    TMemoryInput input(Blocks.back().Begin(), Blocks.back().Size());

    ui64 dataSize;
    ReadVarUInt64(&input, &dataSize);
    YCHECK(dataSize > 0);

    RowPointer_ = input.Buf();
    SizeToNextRow = 0;

    const char* dataEnd = RowPointer_ + dataSize;
    SizeBuffer.Reset(dataEnd, Blocks.back().End() - dataEnd);

    ++CurrentRowIndex;
    YCHECK(NextRow());
    State.FinishOperation();
}

bool TPartitionChunkReader::NextRow()
{
    if (SizeBuffer.Avail() > 0) {
        RowPointer_ = RowPointer_ + SizeToNextRow;
        ReadVarUInt64(&SizeBuffer, &SizeToNextRow);

        DataBuffer.Reset(RowPointer_, SizeToNextRow);

        CurrentRow.clear();
        while (true) {
            auto value = TValue::Load(&DataBuffer);
            if (value.IsNull()) {
                break;
            }

            i32 columnNameLength;
            ReadVarInt32(&DataBuffer, &columnNameLength);
            YASSERT(columnNameLength > 0);
            CurrentRow.insert(std::make_pair(TStringBuf(DataBuffer.Buf(), columnNameLength), value));
            DataBuffer.Skip(columnNameLength);
        }

        return true;
    } else {
        RowPointer_ = NULL;
        return false;
    }
}

bool TPartitionChunkReader::IsValid() const
{
    return RowPointer_ != NULL;
}

bool TPartitionChunkReader::FetchNextItem()
{
    YASSERT(IsValid());

    if (!NextRow() && SequentialReader->HasNext()) {
        State.StartOperation();
        SequentialReader->AsyncNextBlock().Subscribe(BIND(
            &TPartitionChunkReader::OnNextBlock,
            MakeWeak(this)));
        return false;
    }

    ++CurrentRowIndex;
    return true;
}

TAsyncError TPartitionChunkReader::GetReadyEvent()
{
    return State.GetOperationError();
}

TValue TPartitionChunkReader::ReadValue(const TStringBuf& name)
{
    YASSERT(IsValid());

    auto it = CurrentRow.find(name);
    if (it == CurrentRow.end()) {
        // Null value.
        return TValue();
    } else {
        return it->second;
    }
}

TFuture<void> TPartitionChunkReader::GetFetchingCompleteEvent()
{
    return SequentialReader->GetFetchingCompleteEvent();
}

i64 TPartitionChunkReader::GetRowIndex() const
{
    return CurrentRowIndex;
}

void TPartitionChunkReader::OnFail(const TError& error)
{
    LOG_WARNING(error);
    State.Fail(error);
}

const NYTree::TYsonString& TPartitionChunkReader::GetRowAttributes() const
{
    // When reading from partition chunk no row attributes are preserved.
    YUNREACHABLE();
}

////////////////////////////////////////////////////////////////////////////////

TPartitionChunkReaderProvider::TPartitionChunkReaderProvider(
    const NChunkClient::TSequentialReaderConfigPtr& config)
    : Config(config)
{ }

TPartitionChunkReaderPtr TPartitionChunkReaderProvider::CreateNewReader(
    const NChunkClient::NProto::TInputChunk& inputChunk,
    const NChunkClient::IAsyncReaderPtr& chunkReader)
{
    auto miscExt = GetProtoExtension<NChunkClient::NProto::TMiscExt>(inputChunk.extensions());

    return New<TPartitionChunkReader>(
        Config,
        chunkReader,
        inputChunk.partition_tag(),
        NCompression::ECodec(miscExt.compression_codec()));
}

bool TPartitionChunkReaderProvider::KeepInMemory() const
{
    return true;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
