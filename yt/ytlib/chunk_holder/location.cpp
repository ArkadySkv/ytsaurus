#include "stdafx.h"
#include "location.h"
#include "common.h"
#include "chunk.h"
#include "reader_cache.h"
#include "config.h"

#include <ytlib/misc/fs.h>
#include <ytlib/chunk_client/format.h>

namespace NYT {
namespace NChunkHolder {

using namespace NChunkClient;

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = ChunkHolderLogger;

////////////////////////////////////////////////////////////////////////////////

TLocation::TLocation(
    ELocationType type,
    TLocationConfigPtr config,
    TReaderCachePtr readerCache,
    const Stroka& threadName)
    : Type(type)
    , Config(config)
    , ReaderCache(readerCache)
    , AvailableSpace(0)
    , UsedSpace(0)
    , ActionQueue(New<TActionQueue>(threadName))
    , SessionCount(0)
    , Logger(ChunkHolderLogger)
{
    Logger.AddTag(Sprintf("Path: %s", ~Config->Path.Quote()));
}

TLocation::~TLocation()
{ }

ELocationType TLocation::GetType() const
{
    return Type;
}

void TLocation::UpdateUsedSpace(i64 size)
{
    UsedSpace += size;
    AvailableSpace -= size;
}

i64 TLocation::GetAvailableSpace() const
{
    auto path = GetPath();
    
    try {
        AvailableSpace = NFS::GetAvailableSpace(path);
    } catch (const std::exception& ex) {
        LOG_FATAL("Failed to compute available space\n%s",
            ex.what());
    }

    i64 remainingQuota = Max(static_cast<i64>(0), GetQuota() - GetUsedSpace());

    AvailableSpace = Min(AvailableSpace, remainingQuota);

    return AvailableSpace;
}

IInvoker::TPtr TLocation::GetInvoker() const
{
    return ActionQueue->GetInvoker();
}

TIntrusivePtr<TReaderCache> TLocation::GetReaderCache() const
{
    return ReaderCache;
}

i64 TLocation::GetUsedSpace() const
{
    return UsedSpace;
}

i64 TLocation::GetQuota() const
{
   return Config->Quota == 0 ? Max<i64>() : Config->Quota;
}

double TLocation::GetLoadFactor() const
{
    i64 used = GetUsedSpace();
    i64 quota = GetQuota();
    if (used >= quota) {
        return 1.0;
    } else {
        return (double) used / quota;
    }
}

Stroka TLocation::GetPath() const
{
    return Config->Path;
}

void TLocation::UpdateSessionCount(int delta)
{
    SessionCount += delta;
    LOG_DEBUG("Location session count updated (SessionCount: %d)",
        SessionCount);
}

int TLocation::GetSessionCount() const
{
    return SessionCount;
}

Stroka TLocation::GetChunkFileName(const TChunkId& chunkId) const
{
    ui8 firstHashByte = static_cast<ui8>(chunkId.Parts[0] & 0xff);
    return NFS::CombinePaths(
        GetPath(),
        Sprintf("%x%s%s", firstHashByte, LOCSLASH_S, ~chunkId.ToString()));
}

bool TLocation::IsFull() const
{
    return GetAvailableSpace() < Config->LowWatermark;
}

bool TLocation::HasEnoughSpace(i64 size) const
{
    return GetAvailableSpace() - size >= Config->HighWatermark;
}

namespace {

void RemoveFile(const Stroka& fileName)
{
    if (!NFS::Remove(fileName)) {
        LOG_FATAL("Error deleting file %s", ~fileName.Quote());
    }
}

} // namespace <anonymous>

yvector<TChunkDescriptor> TLocation::Scan()
{
    auto path = GetPath();

    LOG_INFO("Scanning storage location");

    NFS::ForcePath(path);
    NFS::CleanTempFiles(path);

    yhash_set<Stroka> fileNames;
    yhash_set<TChunkId> chunkIds;

    TFileList fileList;
    fileList.Fill(path, TStringBuf(), TStringBuf(), Max<int>());
    i32 size = fileList.Size();
    for (i32 i = 0; i < size; ++i) {
        Stroka fileName = fileList.Next();
        fileNames.insert(NFS::NormalizePathSeparators(NFS::CombinePaths(path, fileName)));
        TChunkId chunkId;
        if (TChunkId::FromString(NFS::GetFileNameWithoutExtension(fileName), &chunkId)) {
            chunkIds.insert(chunkId);
        } else {
            LOG_ERROR("Invalid chunk filename %s", ~fileName.Quote());
        }
    }

    yvector<TChunkDescriptor> result;
    result.reserve(chunkIds.size());

    FOREACH (const auto& chunkId, chunkIds) {
        auto chunkDataFileName = GetChunkFileName(chunkId);
        auto chunkMetaFileName = chunkDataFileName + ChunkMetaSuffix;

        bool hasMeta = fileNames.find(NFS::NormalizePathSeparators(chunkMetaFileName)) != fileNames.end();
        bool hasData = fileNames.find(NFS::NormalizePathSeparators(chunkDataFileName)) != fileNames.end();

        YASSERT(hasMeta || hasData);

        if (hasMeta && hasData) {
            i64 chunkDataSize = NFS::GetFileSize(chunkDataFileName);
            i64 chunkMetaSize = NFS::GetFileSize(chunkMetaFileName);
            if (chunkMetaSize == 0) {
                LOG_FATAL("Chunk %s has empty meta file", ~chunkMetaFileName);
            }
            TChunkDescriptor descriptor;
            descriptor.Id = chunkId;
            descriptor.Size = chunkDataSize + chunkMetaSize;
            result.push_back(descriptor);
        } else if (!hasMeta) {
            LOG_WARNING("Missing meta file for %s, removing data file", ~chunkDataFileName.Quote());
            RemoveFile(chunkDataFileName);
        } else if (!hasData) {
            LOG_WARNING("Missing data file for %s, removing meta file", ~chunkMetaFileName.Quote());
            RemoveFile(chunkDataFileName);
        }
    }

    LOG_INFO("Done, %d chunks found", result.ysize());

    return result;
}

void TLocation::RemoveChunk(TChunkPtr chunk)
{
    auto id = chunk->GetId();
    Stroka fileName = chunk->GetFileName();
    GetInvoker()->Invoke(FromFunctor([=] ()
        {
            // TODO: retry on failure
            LOG_DEBUG("Started removing chunk files (ChunkId: %s)", ~id.ToString());
            RemoveFile(fileName);
            RemoveFile(fileName + ChunkMetaSuffix);
            LOG_DEBUG("Finished removing chunk files (ChunkId: %s)", ~id.ToString());
        }));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkHolder
} // namespace NYT
