#include "stdafx.h"
#include "file_chunk_reader.h"
#include "private.h"
#include "config.h"
#include "chunk_meta_extensions.h"

#include <ytlib/chunk_client/sequential_reader.h>
#include <ytlib/chunk_client/replication_reader.h>

#include <ytlib/chunk_client/chunk_meta_extensions.h>
#include <ytlib/chunk_client/dispatcher.h>

namespace NYT {
namespace NFileClient {

using namespace NChunkClient;

static NLog::TLogger& Logger = FileReaderLogger;

////////////////////////////////////////////////////////////////////////////////

TFileChunkReader::TFileChunkReader(
    const NChunkClient::TSequentialReaderConfigPtr& sequentialConfig,
    const NChunkClient::IAsyncReaderPtr& asyncReader,
    NCompression::ECodec codecId,
    i64 startOffset,
    i64 endOffset)
    : SequentialConfig(sequentialConfig)
    , AsyncReader(asyncReader)
    , CodecId(codecId)
    , StartOffset(startOffset)
    , EndOffset(endOffset)
    , Facade(this)
    , Logger(FileReaderLogger)
{ }

TAsyncError TFileChunkReader::AsyncOpen()
{
    State.StartOperation();

    Logger.AddTag(Sprintf("ChunkId: %s", ~ToString(AsyncReader->GetChunkId())));

    LOG_INFO("Requesting chunk meta");
    AsyncReader->AsyncGetChunkMeta().Subscribe(
        BIND(&TFileChunkReader::OnGotMeta, MakeWeak(this))
            .Via(NChunkClient::TDispatcher::Get()->GetReaderInvoker()));

    return State.GetOperationError();
}

void TFileChunkReader::OnGotMeta(NChunkClient::IAsyncReader::TGetMetaResult result)
{
    if (!result.IsOK()) {
        auto error = TError("Failed to get file chunk meta") << result;
        LOG_WARNING(error);
        State.Fail(error);
        return;
    }

    LOG_INFO("Chunk meta received");

    if (result.Value().type() != EChunkType::File) {
        auto error = TError("Invalid chunk type (Expected: %s, Actual: %s)",
            ~FormatEnum(EChunkType(EChunkType::File)),
            ~FormatEnum(EChunkType(result.Value().type())));
        LOG_WARNING(error);
        State.Fail(error);
        return;
    }

    if (result.Value().version() != FormatVersion) {
        auto error = TError("Invalid file chunk format version (Expected: %d, Actual: %d)",
            FormatVersion,
            result.Value().version());
        LOG_WARNING(error);
        State.Fail(error);
        return;
    }

    auto& chunkMeta = result.Value();

    std::vector<TSequentialReader::TBlockInfo> blockSequence;

    // COMPAT(psushin): new file chunk meta!
    auto fileBlocksExt = FindProtoExtension<NFileClient::NProto::TBlocksExt>(chunkMeta.extensions());

    i64 selectedSize = 0;
    int blockIndex = 0;
    auto addBlock = [&] (int index, i64 size) -> bool {
        if (StartOffset >= size) {
            StartOffset -= size;
            EndOffset -= size;
            ++blockIndex;
            return true;
        } else if (selectedSize < EndOffset) {
            selectedSize += size;
            blockSequence.push_back(TSequentialReader::TBlockInfo(index, size));
            return true;
        }
        return false;
    };

    int blockCount = 0;
    if (fileBlocksExt) {
        // New chunk.
        blockCount = fileBlocksExt->blocks_size();
        blockSequence.reserve(blockCount);

        for (int index = 0; index < blockCount; ++index) {
            if (!addBlock(index, fileBlocksExt->blocks(index).size())) {
                break;
            }
        }
    } else {
        // Old chunk.
        auto blocksExt = GetProtoExtension<NChunkClient::NProto::TBlocksExt>(chunkMeta.extensions());
        blockCount = blocksExt.blocks_size();

        blockSequence.reserve(blockCount);
        for (int index = 0; index < blockCount; ++index) {
            if (!addBlock(index, blocksExt.blocks(index).size())) {
                break;
            }
        }
    }
    YCHECK(blockCount >= 0);

    LOG_INFO("Reading %d blocks out of %d starting from %d (SelectedSize: %" PRId64 ")",
        static_cast<int>(blockSequence.size()),
        blockCount,
        blockIndex,
        selectedSize);

    SequentialReader = New<TSequentialReader>(
        SequentialConfig,
        std::move(blockSequence),
        AsyncReader,
        NCompression::ECodec(CodecId));

    LOG_INFO("File reader opened");

    if (SequentialReader->HasNext()) {
        SequentialReader->AsyncNextBlock().Subscribe(BIND(
            &TFileChunkReader::OnNextBlock,
            MakeWeak(this)));
    } else {
        State.Close();
    }

}

void TFileChunkReader::OnNextBlock(TError error)
{
    if (!error.IsOK()) {
        auto wrappedError = TError("Failed to fetch file block") << error;
        LOG_WARNING(wrappedError);
        State.Fail(wrappedError);
        return;
    }

    State.FinishOperation();
}

bool TFileChunkReader::FetchNext()
{
    YCHECK(!State.HasRunningOperation());
    auto block = SequentialReader->GetBlock();
    StartOffset = std::max(StartOffset - static_cast<i64>(block.Size()), (i64)0);
    EndOffset = std::max(EndOffset - static_cast<i64>(block.Size()), (i64)0);

    if (SequentialReader->HasNext()) {
        State.StartOperation();
        SequentialReader->AsyncNextBlock().Subscribe(BIND(
            &TFileChunkReader::OnNextBlock,
            MakeWeak(this)));
        return false;
    } else {
        State.Close();
        return true;
    }
}

TAsyncError TFileChunkReader::GetReadyEvent()
{
    return State.GetOperationError();
}

auto TFileChunkReader::GetFacade() const -> const TFacade*
{
    YCHECK(!State.HasRunningOperation());
    return State.IsClosed() ? nullptr : &Facade;
}

TSharedRef TFileChunkReader::GetBlock() const
{
    auto block = SequentialReader->GetBlock();

    auto* begin = block.Begin();
    auto* end = block.End();

    YCHECK(EndOffset > 0);

    if (EndOffset < block.Size()) {
        end = block.Begin() + EndOffset;
    }

    if (StartOffset > 0) {
        begin = block.Begin() + StartOffset;
    }

    return block.Slice(TRef(begin, end));
}

TFuture<void> TFileChunkReader::GetFetchingCompleteEvent()
{
    return SequentialReader->GetFetchingCompleteEvent();
}

////////////////////////////////////////////////////////////////////////////////

TFileChunkReaderProvider::TFileChunkReaderProvider(
    const NChunkClient::TSequentialReaderConfigPtr& config)
    : Config(config)
{ }

TFileChunkReaderPtr TFileChunkReaderProvider::CreateReader(
    const NChunkClient::NProto::TChunkSpec& chunkSpec,
    const NChunkClient::IAsyncReaderPtr& chunkReader)
{
    auto miscExt = GetProtoExtension<NChunkClient::NProto::TMiscExt>(chunkSpec.extensions());

    i64 startOffset = 0;
    if (chunkSpec.has_start_limit() && chunkSpec.start_limit().has_offset()) {
        startOffset = chunkSpec.start_limit().offset();
    }

    i64 endOffset = std::numeric_limits<i64>::max();
    if (chunkSpec.has_end_limit() && chunkSpec.end_limit().has_offset()) {
        endOffset = chunkSpec.end_limit().offset();
    }

    LOG_INFO(
        "Creating file chunk reader (StartOffset: %" PRId64 ", EndOffset: %" PRId64 ")",
        startOffset,
        endOffset);

    return New<TFileChunkReader>(
        Config,
        chunkReader,
        NCompression::ECodec(miscExt.compression_codec()),
        startOffset,
        endOffset);
}

void TFileChunkReaderProvider::OnReaderOpened(
    TFileChunkReaderPtr reader,
    NChunkClient::NProto::TChunkSpec& chunkSpec)
{
    UNUSED(reader);
    UNUSED(chunkSpec);
}

void TFileChunkReaderProvider::OnReaderFinished(TFileChunkReaderPtr reader)
{
    UNUSED(reader);
}

bool TFileChunkReaderProvider::KeepInMemory() const
{
    return false;
}

////////////////////////////////////////////////////////////////////////////////

TFileChunkReaderFacade::TFileChunkReaderFacade(TFileChunkReader* reader)
    : Reader(reader)
{ }

TSharedRef TFileChunkReaderFacade::GetBlock() const
{
    return Reader->GetBlock();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NFileClient
} // namespace NYT
