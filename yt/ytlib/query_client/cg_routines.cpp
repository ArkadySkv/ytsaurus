#include "stdafx.h"
#include "cg_routines.h"
#include "cg_routine_registry.h"

#include "helpers.h"
#include "callbacks.h"

#include <ytlib/new_table_client/unversioned_row.h>
#include <ytlib/new_table_client/schemaful_reader.h>
#include <ytlib/new_table_client/schemaful_writer.h>
#include <ytlib/new_table_client/schemaful_merging_reader.h>
#include <ytlib/new_table_client/row_buffer.h>

#include <ytlib/chunk_client/chunk_spec.h>

#include <core/concurrency/scheduler.h>

#include <core/profiling/scoped_timer.h>

#include <mutex>

namespace NYT {
namespace NQueryClient {
namespace NRoutines {

using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

static const size_t InitialGroupOpHashtableCapacity = 1024;

#ifdef DEBUG
#define CHECK_STACK() \
    { \
        int dummy; \
        size_t currentStackSize = executionContext->StackSizeGuardHelper - reinterpret_cast<intptr_t>(&dummy); \
        YCHECK(currentStackSize < 10000); \
    }
#else
#define CHECK_STACK() (void) 0;
#endif

////////////////////////////////////////////////////////////////////////////////

void WriteRow(TRow row, TExecutionContext* executionContext)
{
    CHECK_STACK()

    --executionContext->OutputRowLimit;
    ++executionContext->Statistics->RowsWritten;

    auto* batch = executionContext->Batch;
    auto* writer = executionContext->Writer;
    auto* rowBuffer = executionContext->RowBuffer;

    YASSERT(batch->size() < batch->capacity());

    batch->push_back(rowBuffer->Capture(row));

    if (batch->size() == batch->capacity()) {
        if (!writer->Write(*batch)) {
            NProfiling::TAggregatingTimingGuard timingGuard(&executionContext->Statistics->AsyncTime);
            auto error = WaitFor(writer->GetReadyEvent());
            THROW_ERROR_EXCEPTION_IF_FAILED(error);
        }
        batch->clear();
        rowBuffer->Clear();
    }
}

void ScanOpHelper(
    TExecutionContext* executionContext,
    int dataSplitsIndex,
    void** consumeRowsClosure,
    void (*consumeRows)(void** closure, TRow* rows, int size))
{
    auto* callbacks = executionContext->Callbacks;
    auto* context = executionContext->Context;
    auto dataSplits = (*executionContext->DataSplitsArray)[dataSplitsIndex];

    std::vector<ISchemafulReaderPtr> splitReaders;
    TTableSchema schema;
    for (const auto& dataSplit : dataSplits) {
        if (splitReaders.empty()) {
            // All schemas are expected to be same; take the first one. 
            schema = GetTableSchemaFromDataSplit(dataSplit);
        }
        splitReaders.push_back(callbacks->GetReader(dataSplit, context));
    }

    auto mergingReader = CreateSchemafulMergingReader(splitReaders);

    {
        auto error = WaitFor(mergingReader->Open(schema));
        THROW_ERROR_EXCEPTION_IF_FAILED(error);
    }

    std::vector<TRow> rows;
    rows.reserve(MaxRowsPerRead);

    while (true) {
        executionContext->ScratchSpace->Clear();

        bool hasMoreData = mergingReader->Read(&rows);
        bool shouldWait = rows.empty();

        if (executionContext->InputRowLimit < rows.size()) {
            rows.resize(executionContext->InputRowLimit);
            executionContext->Statistics->IncompleteInput = true;
        }
        executionContext->InputRowLimit -= rows.size();
        executionContext->Statistics->RowsRead += rows.size();        

        i64 rowsLeft = rows.size();
        auto* currentRow = rows.data();

        size_t consumeSize;
        while ((consumeSize = std::min(executionContext->OutputRowLimit, rowsLeft)) > 0) {
            consumeRows(consumeRowsClosure, currentRow, consumeSize);
            currentRow += consumeSize;
            rowsLeft -= consumeSize;
        }

        if (!(executionContext->OutputRowLimit >= 0 && rowsLeft == 0)) {
            executionContext->Statistics->IncompleteOutput = true;
        }

        rows.clear();

        if (!hasMoreData
            || executionContext->InputRowLimit <= 0
            || executionContext->OutputRowLimit <= 0) {
            break;
        }

        if (shouldWait) {
            NProfiling::TAggregatingTimingGuard timingGuard(&executionContext->Statistics->AsyncTime);
            auto error = WaitFor(mergingReader->GetReadyEvent());
            THROW_ERROR_EXCEPTION_IF_FAILED(error);
        }
    }
}

void GroupOpHelper(
    int keySize,
    int aggregateItemCount,
    void** consumeRowsClosure,
    void (*consumeRows)(
        void** closure,
        std::vector<TRow>* groupedRows,
        TLookupRows* rows))
{
    std::vector<TRow> groupedRows;
    TLookupRows lookupRows(
        InitialGroupOpHashtableCapacity,
        NDetail::TGroupHasher(keySize),
        NDetail::TGroupComparer(keySize));

    consumeRows(consumeRowsClosure, &groupedRows, &lookupRows);
}

const TRow* FindRow(TExecutionContext* executionContext, TLookupRows* rows, TRow row)
{
    CHECK_STACK()

    auto it = rows->find(row);
    return it != rows->end()? &*it : nullptr;
}

void AddRow(
    TExecutionContext* executionContext,
    TLookupRows* lookupRows,
    std::vector<TRow>* groupedRows,
    TRow* newRow,
    int valueCount)
{
    CHECK_STACK()

    --executionContext->OutputRowLimit;

    groupedRows->push_back(executionContext->RowBuffer->Capture(*newRow));
    lookupRows->insert(groupedRows->back());
    *newRow = TRow::Allocate(executionContext->ScratchSpace, valueCount);
}

void AllocateRow(TExecutionContext* executionContext, int valueCount, TRow* row)
{
    CHECK_STACK()

    *row = TRow::Allocate(executionContext->ScratchSpace, valueCount);
}

TRow* GetRowsData(std::vector<TRow>* groupedRows)
{
    return groupedRows->data();
}

int GetRowsSize(std::vector<TRow>* groupedRows)
{
    return groupedRows->size();
}

////////////////////////////////////////////////////////////////////////////////

i64 IsPrefix(
    const char* lhsData,
    ui32 lhsLength,
    const char* rhsData,
    ui32 rhsLength)
{
    return lhsLength <= rhsLength &&
        std::mismatch(lhsData, lhsData + lhsLength, rhsData).first == lhsData + lhsLength;
}

i64 Equal(
    const char* lhsData,
    ui32 lhsLength,
    const char* rhsData,
    ui32 rhsLength)
{
    return lhsLength == rhsLength && std::equal(lhsData, lhsData + lhsLength, rhsData);
}

i64 NotEqual(
    const char* lhsData,
    ui32 lhsLength,
    const char* rhsData,
    ui32 rhsLength)
{
    return !Equal(lhsData, lhsLength, rhsData, rhsLength);
}

i64 LexicographicalCompare(
    const char* lhsData,
    ui32 lhsLength,
    const char* rhsData,
    ui32 rhsLength)
{
    return std::lexicographical_compare(lhsData, lhsData + lhsLength, rhsData, rhsData + rhsLength);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NRoutines

////////////////////////////////////////////////////////////////////////////////

void RegisterCGRoutinesImpl()
{
#define REGISTER_ROUTINE(routine) \
    TRoutineRegistry::RegisterRoutine(#routine, NRoutines::routine)
    REGISTER_ROUTINE(WriteRow);
    REGISTER_ROUTINE(ScanOpHelper);
    REGISTER_ROUTINE(GroupOpHelper);
    REGISTER_ROUTINE(FindRow);
    REGISTER_ROUTINE(AddRow);
    REGISTER_ROUTINE(AllocateRow);
    REGISTER_ROUTINE(GetRowsData);
    REGISTER_ROUTINE(GetRowsSize);
    REGISTER_ROUTINE(IsPrefix);
    REGISTER_ROUTINE(Equal);
    REGISTER_ROUTINE(NotEqual);
    REGISTER_ROUTINE(LexicographicalCompare);
#undef REGISTER_ROUTINE
}

void RegisterCGRoutines()
{
    static std::once_flag onceFlag;
    std::call_once(onceFlag, &RegisterCGRoutinesImpl);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NQueryClient
} // namespace NYT

