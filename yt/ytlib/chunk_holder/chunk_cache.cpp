#include "stdafx.h"
#include "chunk_cache.h"
#include "common.h"
#include "reader_cache.h"
#include "location.h"
#include "chunk.h"
#include "block_store.h"

#include <ytlib/misc/thread_affinity.h>
#include <ytlib/misc/serialize.h>
#include <ytlib/misc/string.h>
#include <ytlib/misc/fs.h>
#include <ytlib/logging/tagged_logger.h>
#include <ytlib/chunk_client/file_writer.h>
#include <ytlib/chunk_client/remote_reader.h>
#include <ytlib/chunk_client/sequential_reader.h>
#include <ytlib/chunk_server/chunk_service_proxy.h>
#include <ytlib/election/leader_channel.h>
#include <ytlib/chunk_holder/bootstrap.h>

namespace NYT {
namespace NChunkHolder {

using namespace NChunkClient;
using namespace NChunkServer;
using namespace NElection;
using namespace NRpc;
using namespace NProto;

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger Logger("ChunkHolder");

////////////////////////////////////////////////////////////////////////////////

class TChunkCache::TImpl
    : public TWeightLimitedCache<TChunkId, TCachedChunk>
{
public:
    typedef TWeightLimitedCache<TChunkId, TCachedChunk> TBase;
    typedef TIntrusivePtr<TImpl> TPtr;

    TImpl(TChunkHolderConfig* config, TBootstrap* bootstrap)
        : TBase(config->ChunkCacheLocation->Quota == 0 ? Max<i64>() : config->ChunkCacheLocation->Quota)
        , Config(config)
        , Bootstrap(bootstrap)
    { }

    void Start()
    {
        LOG_INFO("Chunk cache scan started");

        Location = New<TLocation>(
            ELocationType::Cache,
            ~Config->ChunkCacheLocation,
            ~Bootstrap->GetReaderCache(),
            "ChunkCache");

        try {
            FOREACH (const auto& descriptor, Location->Scan()) {
                auto chunk = New<TCachedChunk>(
                    ~Location,
                    descriptor,
                    ~Bootstrap->GetChunkCache());
                Put(~chunk);
            }
        } catch (const std::exception& ex) {
            LOG_FATAL("Failed to initialize storage locations\n%s", ex.what());
        }

        LOG_INFO("Chunk cache scan completed, %d chunks found", GetSize());
    }

    void Register(TCachedChunk* chunk)
    {
        chunk->GetLocation()->UpdateUsedSpace(chunk->GetSize());
    }

    void Unregister(TCachedChunk* chunk)
    {
        chunk->GetLocation()->UpdateUsedSpace(-chunk->GetSize());
    }

    void Put(TCachedChunk* chunk)
    {
        TInsertCookie cookie(chunk->GetId());
        YVERIFY(BeginInsert(&cookie));
        cookie.EndInsert(chunk);
        Register(chunk);
    }

    TAsyncDownloadResult::TPtr Download(
        const TChunkId& chunkId,
        const yvector<Stroka>& seedAddresses)
    {
        LOG_INFO("Getting chunk from cache (ChunkId: %s, SeedAddresses: [%s])",
            ~chunkId.ToString(),
            ~JoinToString(seedAddresses));

        TSharedPtr<TInsertCookie> cookie(new TInsertCookie(chunkId));
        if (BeginInsert(~cookie)) {
            LOG_INFO("Loading chunk into cache (ChunkId: %s)", ~chunkId.ToString());
            auto session = New<TDownloadSession>(this, chunkId, seedAddresses, cookie);
            session->Start();
        } else {
            LOG_INFO("Chunk is already cached (ChunkId: %s)", ~chunkId.ToString());
        }

        return cookie->GetAsyncResult();
    }

private:
    TChunkHolderConfigPtr Config;
    TBootstrap* Bootstrap;
    TLocation::TPtr Location;

    DEFINE_SIGNAL(void(TChunk*), ChunkAdded);
    DEFINE_SIGNAL(void(TChunk*), ChunkRemoved);

    virtual i64 GetWeight(TCachedChunk* chunk) const
    {
        return chunk->GetSize();
    }

    virtual void OnAdded(TCachedChunk* value)
    {
        TBase::OnAdded(value);
        ChunkAdded_.Fire(value);
    }
    
    virtual void OnRemoved(TCachedChunk* value)
    {
        TBase::OnRemoved(value);
        ChunkRemoved_.Fire(value);
    }

    class TDownloadSession
        : public TRefCounted
    {
    public:
        typedef TDownloadSession TThis;
        typedef TIntrusivePtr<TThis> TPtr;

        TDownloadSession(
            TImpl* owner,
            const TChunkId& chunkId,
            const yvector<Stroka>& seedAddresses,
            TSharedPtr<TInsertCookie> cookie)
            : Owner(owner)
            , ChunkId(chunkId)
            , SeedAddresses(seedAddresses)
            , Cookie(cookie)
            , Invoker(Owner->Location->GetInvoker())
            , Logger(ChunkHolderLogger)
        {
            Logger.AddTag(Sprintf("ChunkId: %s", ~ChunkId.ToString()));
        }

        void Start()
        {
            Stroka fileName = Owner->Location->GetChunkFileName(ChunkId);
            try {
                NFS::ForcePath(NFS::GetDirectoryName(fileName));
                FileWriter = New<TChunkFileWriter>(ChunkId, fileName);
                FileWriter->Open();
            } catch (const std::exception& ex) {
                LOG_FATAL("Error opening cached chunk for writing\n%s", ex.what());
            }

            RemoteReader = CreateRemoteReader(
                ~Owner->Config->CacheRemoteReader,
                Owner->Bootstrap->GetBlockStore()->GetBlockCache(),
                ~Owner->Bootstrap->GetMasterConnector()->GetLeaderChannel(),
                ChunkId,
                SeedAddresses);

            LOG_INFO("Getting chunk info from holders");
            RemoteReader->AsyncGetChunkInfo()->Subscribe(
                FromMethod(&TThis::OnGotChunkInfo, MakeStrong(this))
                ->Via(Invoker));
        }

    private:
        TImpl::TPtr Owner;
        TChunkId ChunkId;
        yvector<Stroka> SeedAddresses;
        TSharedPtr<TInsertCookie> Cookie;
        IInvoker::TPtr Invoker;

        TChunkFileWriter::TPtr FileWriter;
        IAsyncReader::TPtr RemoteReader;
        TSequentialReader::TPtr SequentialReader;
        TChunkInfo ChunkInfo;
        int BlockCount;
        int BlockIndex;

        NLog::TTaggedLogger Logger;

        void OnGotChunkInfo(IAsyncReader::TGetInfoResult result)
        {
            if (!result.IsOK()) {
                OnError(result);
                return;
            }

            LOG_INFO("Chunk info received from holders");
            ChunkInfo = result.Value();

            // Download all blocks.
            BlockCount = static_cast<int>(ChunkInfo.blocks_size());
            yvector<int> blockIndexes;
            blockIndexes.reserve(BlockCount);
            for (int index = 0; index < BlockCount; ++index) {
                blockIndexes.push_back(index);
            }

            SequentialReader = New<TSequentialReader>(
                ~Owner->Config->CacheSequentialReader,
                blockIndexes,
                ~RemoteReader);

            BlockIndex = 0;
            FetchNextBlock();
        }

        void FetchNextBlock()
        {
            if (BlockIndex >= BlockCount) {
                CloseChunk();
                return;
            }

            LOG_INFO("Asking for another block (BlockIndex: %d)",
                BlockIndex);

            SequentialReader->AsyncNextBlock()->Subscribe(
                FromMethod(&TThis::OnNextBlock, MakeStrong(this))
                ->Via(Invoker));
        }

        void OnNextBlock(TError error)
        {
            if (!error.IsOK()) {
                OnError(error);
                return;
            }

            LOG_INFO("Writing block (BlockIndex: %d)", BlockIndex);
            // NB: This is always done synchronously.
            auto writeResult = FileWriter->AsyncWriteBlock(SequentialReader->GetBlock())->Get();
            if (!writeResult.IsOK()) {
                OnError(writeResult);
                return;
            }
            LOG_INFO("Block written");

            ++BlockIndex;
            FetchNextBlock();
        }

        void CloseChunk()
        {
            LOG_INFO("Closing chunk");
            // NB: This is always done synchronously.
            auto closeResult = FileWriter->AsyncClose(ChunkInfo.attributes())->Get();
            if (!closeResult.IsOK()) {
                OnError(closeResult);
                return;
            }
            LOG_INFO("Chunk is closed");

            OnSuccess();
        }

        void OnSuccess()
        {
            LOG_INFO("Chunk is downloaded into cache");
            auto chunk = New<TCachedChunk>(
                ~Owner->Location,
                ChunkInfo,
                ~Owner->Bootstrap->GetChunkCache());
            Cookie->EndInsert(chunk);
            Owner->Register(~chunk);
            Cleanup();
        }

        void OnError(const TError& error)
        {
            YASSERT(!error.IsOK());
            TError wrappedError(
                error.GetCode(),
                Sprintf("Error downloading chunk into cache (ChunkId: %s)\n%s",
                    ~ChunkId.ToString(),
                    ~error.ToString()));
            Cookie->Cancel(wrappedError);
            LOG_WARNING("%s", ~wrappedError.ToString());
            Cleanup();
        }

        void Cleanup()
        {
            Owner.Reset();
            if (FileWriter) {
                FileWriter.Reset();
            }
            RemoteReader.Reset();
            SequentialReader.Reset();
        }
    };
};

////////////////////////////////////////////////////////////////////////////////

TChunkCache::TChunkCache(TChunkHolderConfig* config, TBootstrap* bootstrap)
    : Impl(New<TImpl>(config, bootstrap))
{ }

void TChunkCache::Start()
{
    Impl->Start();
}

TCachedChunkPtr TChunkCache::FindChunk(const TChunkId& chunkId)
{
    VERIFY_THREAD_AFFINITY_ANY();
    return Impl->Find(chunkId);
}

TChunkCache::TChunks TChunkCache::GetChunks()
{
    VERIFY_THREAD_AFFINITY_ANY();
    return Impl->GetAll();
}

int TChunkCache::GetChunkCount()
{
    VERIFY_THREAD_AFFINITY_ANY();
    return Impl->GetSize();
}

TChunkCache::TAsyncDownloadResult::TPtr TChunkCache::DownloadChunk(
    const TChunkId& chunkId,
    const yvector<Stroka>& seedAddresses)
{
    VERIFY_THREAD_AFFINITY_ANY();
    return Impl->Download(chunkId, seedAddresses);
}

DELEGATE_SIGNAL(TChunkCache, void(TChunk*), ChunkAdded, *Impl);
DELEGATE_SIGNAL(TChunkCache, void(TChunk*), ChunkRemoved, *Impl);

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkHolder
} // namespace NYT
