﻿#include "stdafx.h"
#include "merging_reader.h"
#include "table_chunk_reader.h"
#include "multi_chunk_sequential_reader.h"
#include "key.h"

#include <ytlib/misc/sync.h>
#include <ytlib/misc/heap.h>

#include <ytlib/actions/parallel_awaiter.h>

#include <ytlib/ytree/yson_string.h>

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

namespace {

inline bool CompareReaders(
    const TTableChunkSequenceReader* lhs,
    const TTableChunkSequenceReader* rhs)
{
    return CompareKeys(lhs->CurrentReader()->GetKey(), rhs->CurrentReader()->GetKey()) < 0;
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

class TMergingReader
    : public ISyncReader
{
public:
    explicit TMergingReader(const std::vector<TTableChunkSequenceReaderPtr>& readers)
        : Readers(readers)
        , IsStarted_(false)
    { }

    virtual void Open() override
    {
        // Open all readers in parallel and wait until of them are opened.
        auto awaiter = New<TParallelAwaiter>(
            NChunkClient::TDispatcher::Get()->GetReaderInvoker());
        std::vector<TError> errors;

        FOREACH (auto reader, Readers) {
            awaiter->Await(
                reader->AsyncOpen(),
                BIND([&] (TError error) {
                    if (!error.IsOK()) {
                        errors.push_back(error);
                    }
            }));
        }

        TPromise<void> completed(NewPromise<void>());
        awaiter->Complete(BIND([=] () mutable {
            completed.Set();
        }));
        completed.Get();

        if (!errors.empty()) {
            TError error("Error opening merging reader");
            FOREACH (const auto& innerError, errors) {
                error.InnerErrors().push_back(innerError);
            }
            THROW_ERROR error;
        }

        // Push all non-empty readers to the heap.
        FOREACH (auto reader, Readers) {
            if (reader->IsValid()) {
                ReaderHeap.push_back(~reader);
            }
        }

        // Prepare the heap.
        if (!ReaderHeap.empty()) {
            MakeHeap(ReaderHeap.begin(), ReaderHeap.end(), CompareReaders);
        }
    }

    virtual const TRow* GetRow() override
    {
        if (IsStarted_) {
            auto* currentReader = ReaderHeap.front();
            if (!currentReader->FetchNextItem()) {
                Sync(currentReader, &TTableChunkSequenceReader::GetReadyEvent);
            }
            if (currentReader->IsValid()) {
                AdjustHeap(ReaderHeap.begin(), ReaderHeap.end(), CompareReaders);
            } else {
                ExtractHeap(ReaderHeap.begin(), ReaderHeap.end(), CompareReaders);
                ReaderHeap.pop_back();
            }
        }
        IsStarted_ = true;

        if (ReaderHeap.empty()) {
            return NULL;
        }
        else {
            return &(ReaderHeap.front()->CurrentReader()->GetRow());
        }
    }

    virtual const TNonOwningKey& GetKey() const override
    {
        return ReaderHeap.front()->CurrentReader()->GetKey();
    }

    virtual i64 GetRowCount() const override
    {
        i64 total = 0;
        FOREACH(const auto& reader, Readers) {
            total += reader->GetItemCount();
        }
        return total;
    }

    virtual i64 GetRowIndex() const override
    {
        i64 total = 0;
        FOREACH(const auto& reader, Readers) {
            total += reader->GetItemIndex();
        }
        return total;
    }

    virtual std::vector<NChunkClient::TChunkId> GetFailedChunks() const override
    {
        std::vector<NChunkClient::TChunkId> result;
        FOREACH(auto reader, Readers) {
            auto part = reader->GetFailedChunks();
            result.insert(result.end(), part.begin(), part.end());
        }
        return result;
    }

private:
    std::vector<TTableChunkSequenceReaderPtr> Readers;
    std::vector<TTableChunkSequenceReader*> ReaderHeap;

    bool IsStarted_;
};

ISyncReaderPtr CreateMergingReader(const std::vector<TTableChunkSequenceReaderPtr>& readers)
{
    return New<TMergingReader>(readers);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
