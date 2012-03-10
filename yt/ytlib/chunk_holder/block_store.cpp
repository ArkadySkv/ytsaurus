#include "stdafx.h"
#include "block_store.h"
#include "common.h"
#include "chunk.h"
#include "config.h"
#include "chunk_registry.h"
#include "reader_cache.h"
#include "location.h"

#include <ytlib/chunk_client/file_reader.h>

namespace NYT {
namespace NChunkHolder {

using namespace NChunkClient;

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = ChunkHolderLogger;

////////////////////////////////////////////////////////////////////////////////

TCachedBlock::TCachedBlock(
    const TBlockId& blockId,
    const TSharedRef& data,
    const Stroka& source)
    : TCacheValueBase<TBlockId, TCachedBlock>(blockId)
    , Data_(data)
    , Source_(source)
{ }

TCachedBlock::~TCachedBlock()
{
    LOG_DEBUG("Purged cached block (BlockId: %s)", ~GetKey().ToString());
}

////////////////////////////////////////////////////////////////////////////////

class TBlockStore::TStoreImpl 
    : public TWeightLimitedCache<TBlockId, TCachedBlock>
{
public:
    typedef TIntrusivePtr<TStoreImpl> TPtr;

    DEFINE_BYVAL_RO_PROPERTY(TAtomic, PendingReadSize);

    TStoreImpl(
        TChunkHolderConfig* config,
        TChunkRegistry* chunkRegistry,
        TReaderCache* readerCache)
        : TWeightLimitedCache<TBlockId, TCachedBlock>(config->MaxCachedBlocksSize)
        , ChunkRegistry(chunkRegistry)
        , ReaderCache(readerCache)
        , PendingReadSize_(0)
    { }

    TCachedBlock::TPtr Put(const TBlockId& blockId, const TSharedRef& data, const Stroka& source)
    {
        while (true) {
            TInsertCookie cookie(blockId);
            if (BeginInsert(&cookie)) {
                auto block = New<TCachedBlock>(blockId, data, source);
                cookie.EndInsert(block);

                LOG_DEBUG("Block is put into cache (BlockId: %s, BlockSize: %" PRISZT ")",
                    ~blockId.ToString(),
                    data.Size());

                return block;
            }

            auto result = cookie.GetAsyncResult()->Get();
            if (!result.IsOK()) {
                // Looks like a parallel Get request has completed unsuccessfully.
                continue;
            }

            // This is a cruel reality.
            // Since we never evict blocks of removed chunks from the cache
            // it is possible for a block to be put there more than once.
            // We shall reuse the cached copy but for sanity's sake let's
            // check that the content is the same.
            auto block = result.Value();

            if (!TRef::CompareContent(data, block->GetData())) {
                LOG_FATAL("Trying to cache a block for which a different cached copy already exists (BlockId: %s)",
                    ~blockId.ToString());
            }
            
            LOG_DEBUG("Block is resurrected in cache (BlockId: %s)", ~blockId.ToString());

            return block;
        }
    }

    TAsyncGetBlockResult::TPtr Get(const TBlockId& blockId)
    {
        TSharedPtr<TInsertCookie> cookie(new TInsertCookie(blockId));
        if (!BeginInsert(~cookie)) {
            LOG_DEBUG("Block cache hit (BlockId: %s)", ~blockId.ToString());
            return cookie->GetAsyncResult();
        }

        auto chunk = ChunkRegistry->FindChunk(blockId.ChunkId);
        if (!chunk) {
            cookie->Cancel(TError(
                TChunkHolderServiceProxy::EErrorCode::NoSuchChunk,
                Sprintf("No such chunk (ChunkId: %s)", ~blockId.ChunkId.ToString())));
            return cookie->GetAsyncResult();
        }
     
        LOG_DEBUG("Block cache miss (BlockId: %s)", ~blockId.ToString());

        auto invoker = chunk->GetLocation()->GetInvoker();
        invoker->Invoke(FromMethod(
            &TStoreImpl::DoReadBlock,
            TPtr(this),
            chunk,
            blockId,
            cookie));
        
        return cookie->GetAsyncResult();
    }

    TCachedBlock::TPtr Find(const TBlockId& blockId)
    {
        auto asyncResult = Lookup(blockId);
        TGetBlockResult result;
        if (asyncResult && asyncResult->TryGet(&result) && result.IsOK()) {
            LOG_DEBUG("Block cache hit (BlockId: %s)", ~blockId.ToString());
            return result.Value();
        } else {
            LOG_DEBUG("Block cache miss (BlockId: %s)", ~blockId.ToString());
            return NULL;
        }
    }

private:
    TChunkRegistry::TPtr ChunkRegistry;
    TReaderCache::TPtr ReaderCache;

    virtual i64 GetWeight(TCachedBlock* block) const
    {
        return block->GetData().Size();
    }

    void DoReadBlock(
        TChunkPtr chunk,
        const TBlockId& blockId,
        TSharedPtr<TInsertCookie> cookie)
    {
        auto readerResult = ReaderCache->GetReader(~chunk);
        if (!readerResult.IsOK()) {
            cookie->Cancel(readerResult);
            return;
        }

        auto reader = readerResult.Value();

        const auto& chunkInfo = reader->GetChunkInfo();
        const auto& blockInfo = chunkInfo.blocks(blockId.BlockIndex);
        auto blockSize = blockInfo.size();
        
        AtomicAdd(PendingReadSize_, blockSize);
        LOG_DEBUG("Pending read size increased (BlockSize: %d, PendingReadSize: %" PRISZT,
            blockSize,
            PendingReadSize_);

        auto data = reader->ReadBlock(blockId.BlockIndex);
        AtomicSub(PendingReadSize_, blockSize);
        LOG_DEBUG("Pending read size decreased (BlockSize: %d, PendingReadSize: %" PRISZT,
            blockSize,
            PendingReadSize_);

        if (!data) {
            cookie->Cancel(TError(
                TChunkHolderServiceProxy::EErrorCode::NoSuchBlock,
                Sprintf("No such block (BlockId: %s)", ~blockId.ToString())));
            return;
        }

        auto block = New<TCachedBlock>(blockId, data, Stroka());
        cookie->EndInsert(block);

        LOG_DEBUG("Finished loading block into cache (BlockId: %s)", ~blockId.ToString());
    }

    void UpdatePeer()
    {
        auto blocks = GetAll();
        FOREACH (const auto& block, blocks) {
            auto source = block->Source();
            if (!source.empty()) {
                
            }
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

class TBlockStore::TCacheImpl
    : public IBlockCache
{
public:
    TCacheImpl(TStoreImpl* storeImpl)
        : StoreImpl(storeImpl)
    { }

    void Put(const TBlockId& id, const TSharedRef& data, const Stroka& source)
    {
        StoreImpl->Put(id, data, source);
    }

    TSharedRef Find(const TBlockId& id)
    {
        auto block = StoreImpl->Find(id);
        return block ? block->GetData() : TSharedRef();
    }

private:
    TStoreImpl::TPtr StoreImpl;

};

////////////////////////////////////////////////////////////////////////////////

TBlockStore::TBlockStore(
    TChunkHolderConfig* config,
    TChunkRegistry* chunkRegistry,
    TReaderCache* readerCache)
    : StoreImpl(New<TStoreImpl>(
        config,
        chunkRegistry,
        readerCache))
    , CacheImpl(New<TCacheImpl>(~StoreImpl))
{ }

TBlockStore::TAsyncGetBlockResult::TPtr TBlockStore::GetBlock(const TBlockId& blockId)
{
    return StoreImpl->Get(blockId);
}

TCachedBlock::TPtr TBlockStore::FindBlock(const TBlockId& blockId)
{
    return StoreImpl->Find(blockId);
}

TCachedBlock::TPtr TBlockStore::PutBlock(
    const TBlockId& blockId,
    const TSharedRef& data,
    const Stroka& source)
{
    return StoreImpl->Put(blockId, data, source);
}

i64 TBlockStore::GetPendingReadSize() const
{
    return StoreImpl->GetPendingReadSize();
}

IBlockCache* TBlockStore::GetBlockCache()
{
    return ~CacheImpl;
}

yvector<TCachedBlock::TPtr> TBlockStore::GetAllBlocks() const
{
    return StoreImpl->GetAll();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkHolder
} // namespace NYT
