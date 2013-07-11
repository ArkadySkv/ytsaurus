#include "stdafx.h"
#include "private.h"
#include "partition_chunk_reader.h"

#include <ytlib/chunk_client/config.h>
#include <ytlib/chunk_client/dispatcher.h>
#include <ytlib/chunk_client/sequential_reader.h>
#include <ytlib/chunk_client/chunk_meta_extensions.h>

#include <ytlib/table_client/chunk_meta_extensions.h>

#include <ytlib/yson/varint.h>

namespace NYT {
namespace NTableClient {

using namespace NChunkClient;
using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

TPartitionChunkReaderFacade::TPartitionChunkReaderFacade(TPartitionChunkReader* reader)
    : Reader(reader)
{ }

const char* TPartitionChunkReaderFacade::GetRowPointer() const
{
    return Reader->GetRowPointer();
}

TValue TPartitionChunkReaderFacade::ReadValue(const TStringBuf& name) const
{
    return Reader->ReadValue(name);
}

////////////////////////////////////////////////////////////////////////////////

// ToDo(psushin): eliminate copy-paste from table_chunk_reader.cpp
TPartitionChunkReader::TPartitionChunkReader(
    TPartitionChunkReaderProviderPtr provider,
    const NChunkClient::TSequentialReaderConfigPtr& sequentialReader,
    const NChunkClient::IAsyncReaderPtr& asyncReader,
    int partitionTag,
    NCompression::ECodec codecId)
    : RowPointer_(NULL)
    , RowIndex_(-1)
    , Provider(provider)
    , Facade(this)
    , SequentialConfig(sequentialReader)
    , AsyncReader(asyncReader)
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

    const auto& chunkMeta = result.GetValue();

    if (chunkMeta.type() != EChunkType::Table) {
        LOG_FATAL("Invalid chunk type %d", chunkMeta.type());
    }

    if (chunkMeta.version() != FormatVersion) {
        OnFail(TError("Invalid chunk format version: expected %d, actual %d",
            FormatVersion,
            chunkMeta.version()));
        return;
    }

    auto channelsExt = GetProtoExtension<NProto::TChannelsExt>(chunkMeta.extensions());
    YCHECK(channelsExt.items_size() == 1);

    std::vector<TSequentialReader::TBlockInfo> blockSequence;
    {
        for (int i = 0; i < channelsExt.items(0).blocks_size(); ++i) {
            const auto& blockInfo = channelsExt.items(0).blocks(i);
            YCHECK(PartitionTag == blockInfo.partition_tag());
            blockSequence.push_back(TSequentialReader::TBlockInfo(
                blockInfo.block_index(),
                blockInfo.block_size()));
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

    LOG_DEBUG("Switching to next block at row %" PRId64, RowIndex_);

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

        ++RowIndex_;
        ++Provider->RowIndex_;
        return true;
    } else {
        RowPointer_ = NULL;
        return false;
    }
}

auto TPartitionChunkReader::GetFacade() const -> const TFacade*
{
    return RowPointer_ ?  &Facade : nullptr;
}

bool TPartitionChunkReader::FetchNext()
{
    if (!NextRow() && SequentialReader->HasNext()) {
        State.StartOperation();
        SequentialReader->AsyncNextBlock().Subscribe(BIND(
            &TPartitionChunkReader::OnNextBlock,
            MakeWeak(this)));
        return false;
    }

    return true;
}

TAsyncError TPartitionChunkReader::GetReadyEvent()
{
    return State.GetOperationError();
}

TValue TPartitionChunkReader::ReadValue(const TStringBuf& name) const
{
    YASSERT(RowPointer_);

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

NChunkClient::NProto::TDataStatistics TPartitionChunkReader::GetDataStatistics() const
{

    NChunkClient::NProto::TDataStatistics result;
    result.set_chunk_count(1);

    if (SequentialReader) {
        result.set_row_count(GetRowIndex());
        result.set_uncompressed_data_size(SequentialReader->GetUncompressedDataSize());
        result.set_compressed_data_size(SequentialReader->GetCompressedDataSize());
    } else {
        result.set_row_count(0);
        result.set_uncompressed_data_size(0);
        result.set_compressed_data_size(0);
    }

    return result;
}

void TPartitionChunkReader::OnFail(const TError& error)
{
    LOG_WARNING(error);
    State.Fail(error);
}

////////////////////////////////////////////////////////////////////////////////

TPartitionChunkReaderProvider::TPartitionChunkReaderProvider(
    const NChunkClient::TSequentialReaderConfigPtr& config)
    : RowIndex_(-1)
    , Config(config)
    , DataStatistics(NChunkClient::NProto::ZeroDataStatistics())
{ }

TPartitionChunkReaderPtr TPartitionChunkReaderProvider::CreateReader(
    const NChunkClient::NProto::TChunkSpec& chunkSpec,
    const NChunkClient::IAsyncReaderPtr& chunkReader)
{
    auto miscExt = GetProtoExtension<NChunkClient::NProto::TMiscExt>(chunkSpec.extensions());

    return New<TPartitionChunkReader>(
        this,
        Config,
        chunkReader,
        chunkSpec.partition_tag(),
        NCompression::ECodec(miscExt.compression_codec()));
}

bool TPartitionChunkReaderProvider::KeepInMemory() const
{
    return true;
}

void TPartitionChunkReaderProvider::OnReaderOpened(
    TPartitionChunkReaderPtr reader,
    NChunkClient::NProto::TChunkSpec& chunkSpec)
{
    TGuard<TSpinLock> guard(SpinLock);
    YCHECK(ActiveReaders.insert(reader).second);
}

void TPartitionChunkReaderProvider::OnReaderFinished(TPartitionChunkReaderPtr reader)
{
    TGuard<TSpinLock> guard(SpinLock);
    DataStatistics += reader->GetDataStatistics();
    YCHECK(ActiveReaders.erase(reader) == 1);
}

NChunkClient::NProto::TDataStatistics TPartitionChunkReaderProvider::GetDataStatistics() const
{
    auto dataStatistics = DataStatistics;

    TGuard<TSpinLock> guard(SpinLock);
    FOREACH(const auto& reader, ActiveReaders) {
        dataStatistics += reader->GetDataStatistics();
    }
    return dataStatistics;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
