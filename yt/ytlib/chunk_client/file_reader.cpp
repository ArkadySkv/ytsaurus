#include "stdafx.h"
#include "file_reader.h"
#include "chunk_meta_extensions.h"

#include <core/misc/serialize.h>
#include <core/misc/protobuf_helpers.h>
#include <core/misc/fs.h>
#include <core/misc/assert.h>

namespace NYT {
namespace NChunkClient {

using namespace NChunkClient::NProto;

///////////////////////////////////////////////////////////////////////////////

TFileReader::TFileReader(const Stroka& fileName)
    : FileName_(fileName)
    , Opened_(false)
    , MetaSize_(-1)
    , DataSize_(-1)
{ }

void TFileReader::Open()
{
    YCHECK(!Opened_);

    Stroka metaFileName = FileName_ + ChunkMetaSuffix;
    TFile metaFile(
        metaFileName,
        OpenExisting | RdOnly | Seq | CloseOnExec);

    MetaSize_ = metaFile.GetLength();
    TBufferedFileInput chunkMetaInput(metaFile);

    TChunkMetaHeader metaHeader;
    ReadPod(chunkMetaInput, metaHeader);
    if (metaHeader.Signature != TChunkMetaHeader::ExpectedSignature) {
        THROW_ERROR_EXCEPTION("Incorrect header signature in chunk meta file %s: expected %" PRIx64 ", actual %" PRIx64,
            ~FileName_.Quote(),
            TChunkMetaHeader::ExpectedSignature,
            metaHeader.Signature);
    }

    auto metaBlob = chunkMetaInput.ReadAll();
    auto metaBlobRef = TRef::FromString(metaBlob);

    auto checksum = GetChecksum(metaBlobRef);
    if (checksum != metaHeader.Checksum) {
        THROW_ERROR_EXCEPTION("Incorrect checksum in chunk meta file %s: expected %" PRIx64 ", actual %" PRIx64,
            ~FileName_.Quote(),
            metaHeader.Checksum,
            checksum);
    }

    if (!DeserializeFromProtoWithEnvelope(&ChunkMeta_, metaBlobRef)) {
        THROW_ERROR_EXCEPTION("Failed to parse chunk meta file %s",
            ~FileName_.Quote());
    }

    BlocksExt_ = GetProtoExtension<TBlocksExt>(ChunkMeta_.extensions());

    DataFile_.reset(new TFile(FileName_, OpenExisting | RdOnly | CloseOnExec));
    Opened_ = true;
}

TFuture<IReader::TReadResult>
TFileReader::ReadBlocks(const std::vector<int>& blockIndexes)
{
    YCHECK(Opened_);

    std::vector<TSharedRef> blocks;
    blocks.reserve(blockIndexes.size());

    for (int index = 0; index < blockIndexes.size(); ++index) {
        int blockIndex = blockIndexes[index];
        blocks.push_back(ReadBlock(blockIndex));
    }

    return MakeFuture(TReadResult(std::move(blocks)));
}

TSharedRef TFileReader::ReadBlock(int blockIndex)
{
    YCHECK(Opened_);
    YCHECK(blockIndex >= 0 && blockIndex < BlocksExt_.blocks_size());

    const auto& blockInfo = BlocksExt_.blocks(blockIndex);
    struct TFileChunkBlockTag { };
    auto data = TSharedRef::Allocate<TFileChunkBlockTag>(blockInfo.size(), false);
    i64 offset = blockInfo.offset();
    DataFile_->Pread(data.Begin(), data.Size(), offset);

    auto checksum = GetChecksum(data);
    if (checksum != blockInfo.checksum()) {
        THROW_ERROR_EXCEPTION("Incorrect checksum of block %d in chunk data file %s: expected %" PRIx64 ", actual %" PRIx64,
            blockIndex,
            ~FileName_,
            blockInfo.checksum(),
            checksum);
    }

    return data;
}

i64 TFileReader::GetMetaSize() const
{
    YCHECK(Opened_);
    return MetaSize_;
}

i64 TFileReader::GetDataSize() const
{
    YCHECK(Opened_);
    return DataSize_;
}

i64 TFileReader::GetFullSize() const
{
    YCHECK(Opened_);
    return MetaSize_ + DataSize_;
}

TChunkMeta TFileReader::GetChunkMeta(const std::vector<int>* extensionTags) const
{
    YCHECK(Opened_);
    return extensionTags
        ? FilterChunkMetaByExtensionTags(ChunkMeta_, *extensionTags)
        : ChunkMeta_;
}

IReader::TAsyncGetMetaResult TFileReader::GetChunkMeta(
    const TNullable<int>& partitionTag,
    const std::vector<int>* extensionTags)
{
    // Partition tag filtering not implemented here
    // because there is no practical need.
    // Implement when necessary.
    YCHECK(!partitionTag.HasValue());
    return MakeFuture(TGetMetaResult(GetChunkMeta(extensionTags)));
}

TChunkId TFileReader::GetChunkId() const 
{
    YUNREACHABLE();
}

///////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
