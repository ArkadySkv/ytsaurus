#include "stdafx.h"
#include "file_reader_base.h"
#include "private.h"
#include "config.h"
#include "chunk_meta_extensions.h"

#include <ytlib/misc/string.h>
#include <ytlib/misc/sync.h>

#include <ytlib/file_client/file_ypath_proxy.h>

#include <ytlib/cypress_client/cypress_ypath_proxy.h>

#include <ytlib/transaction_client/transaction.h>

#include <ytlib/chunk_client/chunk_meta_extensions.h>

namespace NYT {
namespace NFileClient {

using namespace NCypressClient;
using namespace NYTree;
using namespace NTransactionClient;
using namespace NFileClient;
using namespace NChunkClient;
using namespace NTransactionClient;

////////////////////////////////////////////////////////////////////////////////

TFileReaderBase::TFileReaderBase(
    TFileReaderConfigPtr config,
    NRpc::IChannelPtr masterChannel,
    IBlockCachePtr blockCache)
    : Config(config)
    , MasterChannel(masterChannel)
    , BlockCache(blockCache)
    , IsOpen(false)
    , BlockCount(0)
    , BlockIndex(0)
    , Proxy(masterChannel)
    , Logger(FileReaderLogger)
{
    YCHECK(config);
    YCHECK(masterChannel);
    YCHECK(blockCache);
}

void TFileReaderBase::Open(
    const TChunkId& chunkId,
    const std::vector<Stroka>& nodeAddresses)
{
    VERIFY_THREAD_AFFINITY(Client);
    YCHECK(!IsOpen);

    auto remoteReader = CreateRemoteReader(
        Config,
        BlockCache,
        MasterChannel,
        chunkId,
        nodeAddresses);

    LOG_INFO("Requesting chunk info");

    auto getMetaResult = remoteReader->AsyncGetChunkMeta().Get();
    if (!getMetaResult.IsOK()) {
        THROW_ERROR_EXCEPTION("Error getting chunk meta")
            << getMetaResult;
    }

    auto& chunkMeta = getMetaResult.Value();
    YCHECK(chunkMeta.type() == EChunkType::File);

    if (chunkMeta.version() != FormatVersion) {
        THROW_ERROR_EXCEPTION("Chunk format version mismatch: expected %d, actual %d",
            FormatVersion,
            chunkMeta.version());
    }

    auto miscExt = GetProtoExtension<NChunkClient::NProto::TMiscExt>(chunkMeta.extensions());
    Size = miscExt.uncompressed_data_size();

    std::vector<TSequentialReader::TBlockInfo> blockSequence;

    // COMPAT: new file chunk meta!
    auto fileBlocksExt = FindProtoExtension<NFileClient::NProto::TBlocksExt>(chunkMeta.extensions());

    if (fileBlocksExt) {
        // New chunk.
        BlockCount = fileBlocksExt->blocks_size();
        blockSequence.reserve(BlockCount);
        for (int index = 0; index < BlockCount; ++index) {
            blockSequence.push_back(TSequentialReader::TBlockInfo(
                index,
                fileBlocksExt->blocks(index).size()));
        }
    } else {
        // Old chunk.
        auto blocksExt = GetProtoExtension<NChunkClient::NProto::TBlocksExt>(chunkMeta.extensions());
        BlockCount = blocksExt.blocks_size();

        blockSequence.reserve(BlockCount);
        for (int index = 0; index < BlockCount; ++index) {
            blockSequence.push_back(TSequentialReader::TBlockInfo(
                index,
                blocksExt.blocks(index).size()));
        }
    }

    LOG_INFO("Chunk info received (BlockCount: %d, Size: %" PRId64 ")",
        BlockCount,
        Size);

    SequentialReader = New<TSequentialReader>(
        Config,
        std::move(blockSequence),
        remoteReader,
        ECodec(miscExt.codec()));

    LOG_INFO("File reader opened");

    IsOpen = true;
}

TSharedRef TFileReaderBase::Read()
{
    VERIFY_THREAD_AFFINITY(Client);
    YCHECK(IsOpen);

    CheckAborted();

    if (!SequentialReader->HasNext()) {
        return TSharedRef();
    }

    LOG_INFO("Reading block (BlockIndex: %d)", BlockIndex);
    Sync(~SequentialReader, &TSequentialReader::AsyncNextBlock);
    auto block = SequentialReader->GetBlock();
    ++BlockIndex;
    LOG_INFO("Block read (BlockIndex: %d)", BlockIndex);

    return block;
}

i64 TFileReaderBase::GetSize() const
{
    VERIFY_THREAD_AFFINITY(Client);
    YCHECK(IsOpen);

    return Size;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NFileClient
} // namespace NYT
