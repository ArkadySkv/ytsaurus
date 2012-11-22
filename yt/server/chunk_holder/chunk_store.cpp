#include "stdafx.h"
#include "private.h"
#include "config.h"
#include "location.h"
#include "chunk.h"
#include "chunk_store.h"
#include "bootstrap.h"

#include <ytlib/chunk_client/data_node_service_proxy.h>

#include <util/random/random.h>

#include <utility>
#include <limits>

namespace NYT {
namespace NChunkHolder {

using namespace NChunkClient;
using namespace NRpc;

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = DataNodeLogger;

////////////////////////////////////////////////////////////////////////////////

TChunkStore::TChunkStore(TDataNodeConfigPtr config, TBootstrap* bootstrap)
    : Config(config)
    , Bootstrap(bootstrap)
{ }

void TChunkStore::Start()
{
    LOG_INFO("Chunk store scan started");

    try {
        for (int i = 0; i < Config->StoreLocations.size(); ++i) {
            auto locationConfig = Config->StoreLocations[i];

            auto location = New<TLocation>(
                ELocationType::Store,
                "store" + ToString(i),
                locationConfig,
                Bootstrap);
            Locations_.push_back(location);

            auto descriptors = location->Initialize();
            FOREACH (const auto& descriptor, descriptors) {
                auto chunk = New<TStoredChunk>(
                    location, 
                    descriptor,
                    Bootstrap->GetMemoryUsageTracker());
                RegisterChunk(chunk);
            }
        }

        FOREACH (const auto& location, Locations_) {
            const auto& cellGuid = location->GetCellGuid();
            if (cellGuid.IsEmpty())
                continue;

            if (CellGuid.IsEmpty()) {
                CellGuid = cellGuid;
            } else if (CellGuid != cellGuid) {
                LOG_FATAL("Inconsistent cell guid across chunk store locations: %s vs %s", 
                    ~CellGuid.ToString(),
                    ~cellGuid.ToString());
            }
        }

        if (!CellGuid.IsEmpty()) {
            DoSetCellGuid();
        }

    } catch (const std::exception& ex) {
        LOG_FATAL(ex, "Failed to initialize storage locations");
    }

    LOG_INFO("Chunk store scan completed, %d chunks found", ChunkMap.ysize());
}

void TChunkStore::RegisterChunk(TStoredChunkPtr chunk)
{
    auto res = ChunkMap.insert(MakePair(chunk->GetId(), chunk));
    if (!res.second) {
        auto oldChunk = res.first->second;
        LOG_FATAL("Duplicate chunk (Current chunk: %s; previous chunk: %s)",
            ~chunk->GetLocation()->GetChunkFileName(chunk->GetId()),
            ~oldChunk->GetLocation()->GetChunkFileName(oldChunk->GetId()));
    }

    auto location = chunk->GetLocation();
    location->UpdateChunkCount(+1);
    location->UpdateUsedSpace(+chunk->GetInfo().size());

    LOG_DEBUG("Chunk registered (ChunkId: %s, Size: %" PRId64 ")",
        ~chunk->GetId().ToString(),
        chunk->GetInfo().size());

    ChunkAdded_.Fire(chunk);
}

TStoredChunkPtr TChunkStore::FindChunk(const TChunkId& chunkId) const
{
    auto it = ChunkMap.find(chunkId);
    return it == ChunkMap.end() ? NULL : it->second;
}

TFuture<void> TChunkStore::RemoveChunk(TStoredChunkPtr chunk)
{
    auto promise = NewPromise<void>();
    chunk->ScheduleRemoval().Subscribe(
        BIND([=] () mutable {
            auto chunkId = chunk->GetId();
            YCHECK(ChunkMap.erase(chunkId) == 1);

            auto location = chunk->GetLocation();
            location->UpdateChunkCount(-1);
            location->UpdateUsedSpace(-chunk->GetInfo().size());

            ChunkRemoved_.Fire(chunk);
            promise.Set();
        })
        .Via(Bootstrap->GetControlInvoker()));
    return promise;
}

TLocationPtr TChunkStore::GetNewChunkLocation()
{
    YASSERT(!Locations_.empty());

    std::vector<TLocationPtr> candidates;
    candidates.reserve(Locations_.size());

    int minCount = Max<int>();
    FOREACH (const auto& location, Locations_) {
        if (location->IsFull() || !location->IsEnabled()) {
            continue;
        }
        int count = location->GetSessionCount();
        if (count < minCount) {
            candidates.clear();
            minCount = count;
        }
        if (count == minCount) {
            candidates.push_back(location);
        }
    }

    if (candidates.empty()) {
        THROW_ERROR_EXCEPTION(
            TDataNodeServiceProxy::EErrorCode::OutOfSpace,
            "All locations are either disabled or full");
    }

    return candidates[RandomNumber(candidates.size())];
}

TChunkStore::TChunks TChunkStore::GetChunks() const
{
    TChunks result;
    result.reserve(ChunkMap.ysize());
    FOREACH (const auto& pair, ChunkMap) {
        result.push_back(pair.second);
    }
    return result;
}

int TChunkStore::GetChunkCount() const
{
    return ChunkMap.ysize();
}

void TChunkStore::SetCellGuid(const TGuid& cellGuid)
{
    CellGuid = cellGuid;
    DoSetCellGuid();
}

void TChunkStore::DoSetCellGuid()
{
    FOREACH (const auto& location, Locations_) {
        location->SetCellGuid(CellGuid);
    }
}

const TGuid& TChunkStore::GetCellGuid() const
{
    return CellGuid;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkHolder
} // namespace NYT
