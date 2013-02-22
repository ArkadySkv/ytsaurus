#ifndef CHUNK_INFO_COLLECTOR_INL_H_
#error "Direct inclusion of this file is not allowed, include chunk_info_collector.h"
#endif
#undef CHUNK_INFO_COLLECTOR_INL_H_

#include <ytlib/actions/parallel_awaiter.h>

#include <ytlib/table_client/key.h>

namespace NYT {
namespace NScheduler {

////////////////////////////////////////////////////////////////////////////////

template <class TFetcher>
TChunkInfoCollector<TFetcher>::TChunkInfoCollector(
    TFetcherPtr fetcher,
    IInvokerPtr invoker)
    : Fetcher(fetcher)
    , Invoker(invoker)
    , Promise(NewPromise< TValueOrError<void> >())
{ }

template <class TFetcher>
void TChunkInfoCollector<TFetcher>::AddChunk(
    NTableClient::TRefCountedInputChunkPtr chunk)
{
    YCHECK(UnfetchedChunkIndexes.insert(static_cast<int>(Chunks.size())).second);
    Chunks.push_back(chunk);
}

template <class TFetcher>
TFuture< TValueOrError<void> > TChunkInfoCollector<TFetcher>::Run()
{
    Fetcher->Prepare(Chunks);
    SendRequests();
    return Promise;
}

template <class TFetcher>
void TChunkInfoCollector<TFetcher>::SendRequests()
{
    auto& Logger = Fetcher->GetLogger();

    // Construct address -> chunk* map.
    typedef yhash_map<Stroka, std::vector<int> > TAddressToChunkIndexes;
    TAddressToChunkIndexes addressToChunkIndexes;

    FOREACH (auto chunkIndex, UnfetchedChunkIndexes) {
        const auto& chunk = Chunks[chunkIndex];
        auto chunkId = NChunkServer::TChunkId::FromProto(chunk->chunk_id());
        bool chunkAvailable = false;
        FOREACH (const auto& address, chunk->node_addresses()) {
            if (DeadNodes.find(address) == DeadNodes.end() &&
                DeadChunkIds.find(std::make_pair(address, chunkId)) == DeadChunkIds.end())
            {
                addressToChunkIndexes[address].push_back(chunkIndex);
                chunkAvailable = true;
            }
        }
        if (!chunkAvailable) {
            Promise.Set(TError("Unable to fetch chunk info for chunk %s from any of nodes [%s]",
                ~chunkId.ToString(),
                ~JoinToString(chunk->node_addresses())));
            return;
        }
    }

    // Sort nodes by number of chunks (in decreasing order).
    std::vector<TAddressToChunkIndexes::iterator> addressIts;
    for (auto it = addressToChunkIndexes.begin(); it != addressToChunkIndexes.end(); ++it) {
        addressIts.push_back(it);
    }
    std::sort(
        addressIts.begin(),
        addressIts.end(),
        [=] (const TAddressToChunkIndexes::iterator& lhs, const TAddressToChunkIndexes::iterator& rhs) {
            return lhs->second.size() > rhs->second.size();
        });

    // Pick nodes greedily.
    auto awaiter = New<TParallelAwaiter>(Invoker);
    yhash_set<int> requestedChunkIndexes;
    FOREACH (const auto& it, addressIts) {
        auto address = it->first;

        Fetcher->CreateNewRequest(address);

        auto& addressChunkIndexes = it->second;
        std::vector<int> requestChunkIndexes;
        FOREACH (auto chunkIndex, addressChunkIndexes) {
            if (requestedChunkIndexes.find(chunkIndex) == requestedChunkIndexes.end()) {
                YCHECK(requestedChunkIndexes.insert(chunkIndex).second);

                auto& chunk = Chunks[chunkIndex];
                if (Fetcher->AddChunkToRequest(chunk)) {
                    requestChunkIndexes.push_back(chunkIndex);
                } else {
                    // We are not going to fetch info for this chunk.
                    YCHECK(UnfetchedChunkIndexes.erase(chunkIndex) == 1);
                }
            }
        }

        // Send the request, if not empty.
        if (!requestChunkIndexes.empty()) {
            LOG_DEBUG("Requesting chunk info for %d chunks from %s",
                static_cast<int>(requestChunkIndexes.size()),
                ~address);

            awaiter->Await(
                Fetcher->InvokeRequest(),
                BIND(
                    &TChunkInfoCollector<TFetcher>::OnResponse,
                    MakeStrong(this),
                    address,
                    Passed(std::move(requestChunkIndexes))));
        }
    }
    awaiter->Complete(BIND(
        &TChunkInfoCollector<TFetcher>::OnEndRound,
        MakeStrong(this)));

    LOG_INFO("Done, %d requests sent", awaiter->GetRequestCount());
}

template <class TFetcher>
void TChunkInfoCollector<TFetcher>::OnResponse(
    const Stroka& address,
    std::vector<int> chunkIndexes,
    typename TFetcher::TResponsePtr rsp)
{
    auto& Logger = Fetcher->GetLogger();

    if (rsp->IsOK()) {
        for (int index = 0; index < static_cast<int>(chunkIndexes.size()); ++index) {
            int chunkIndex = chunkIndexes[index];
            auto& chunk = Chunks[chunkIndex];
            auto chunkId = NChunkServer::TChunkId::FromProto(chunk->chunk_id());

            auto result = Fetcher->ProcessResponseItem(rsp, index, chunk);
            if (result.IsOK()) {
                YCHECK(UnfetchedChunkIndexes.erase(chunkIndex) == 1);
            } else {
                LOG_WARNING(result, "Unable to fetch info for chunk %s from %s",
                    ~chunkId.ToString(),
                    ~address);
                YCHECK(DeadChunkIds.insert(std::make_pair(address, chunkId)).second);
            }
        }
        LOG_DEBUG("Received chunk info from %s", ~address);
    } else {
        LOG_WARNING(*rsp, "Error requesting chunk info from %s",
            ~address);
        YCHECK(DeadNodes.insert(address).second);
    }
}

template <class TFetcher>
void TChunkInfoCollector<TFetcher>::OnEndRound()
{
    auto& Logger = Fetcher->GetLogger();

    if (UnfetchedChunkIndexes.empty()) {
        LOG_INFO("All info is fetched");
        Promise.Set(TError());
    } else {
        LOG_DEBUG("Chunk info for %d chunks is still unfetched",
            static_cast<int>(UnfetchedChunkIndexes.size()));
        SendRequests();
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT
