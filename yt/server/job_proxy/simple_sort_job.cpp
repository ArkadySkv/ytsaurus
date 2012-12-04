﻿#include "stdafx.h"
#include "private.h"
#include "job_detail.h"
#include "config.h"
#include "simple_sort_job.h"
#include "small_key.h"

#include <ytlib/misc/sync.h>

#include <ytlib/meta_state/master_channel.h>

#include <ytlib/chunk_client/client_block_cache.h>

#include <ytlib/table_client/table_chunk_sequence_writer.h>
#include <ytlib/table_client/table_chunk_reader.h>
#include <ytlib/table_client/multi_chunk_parallel_reader.h>
#include <ytlib/table_client/sync_writer.h>

#include <ytlib/ytree/lexer.h>

namespace NYT {
namespace NJobProxy {

using namespace NElection;
using namespace NTableClient;
using namespace NChunkClient;
using namespace NObjectClient;
using namespace NYTree;
using namespace NScheduler::NProto;

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = JobProxyLogger;
static NProfiling::TProfiler& Profiler = JobProxyProfiler;

typedef TMultiChunkParallelReader<TTableChunkReader> TReader;

////////////////////////////////////////////////////////////////////////////////

// ToDo(psushin): get rid of value count.

class TSimpleSortJob
    : public TJob
{
public:
    explicit TSimpleSortJob(IJobHost* host)
        : TJob(host)
    {
        const auto& jobSpec = Host->GetJobSpec();
        auto config = Host->GetConfig();

        YCHECK(jobSpec.input_specs_size() == 1);
        YCHECK(jobSpec.output_specs_size() == 1);

        auto masterChannel = CreateLeaderChannel(config->Masters);
        auto blockCache = CreateClientBlockCache(New<TClientBlockCacheConfig>());
        auto jobSpecExt = jobSpec.GetExtension(TSortJobSpecExt::sort_job_spec_ext);

        KeyColumns = FromProto<Stroka>(jobSpecExt.key_columns());

        TReaderOptions options;
        options.KeepBlocks = true;

        std::vector<NTableClient::NProto::TInputChunk> chunks(
            jobSpec.input_specs(0).chunks().begin(),
            jobSpec.input_specs(0).chunks().end());

        srand(time(NULL));
        std::random_shuffle(chunks.begin(), chunks.end());

        auto provider = New<TTableChunkReaderProvider>(config->JobIO->TableReader, options);

        Reader = New<TReader>(
            config->JobIO->TableReader, 
            masterChannel, 
            blockCache, 
            MoveRV(chunks),
            provider);

        Writer = New<TTableChunkSequenceWriter>(
            config->JobIO->TableWriter,
            masterChannel,
            TTransactionId::FromProto(jobSpec.output_transaction_id()),
            TChunkListId::FromProto(jobSpec.output_specs(0).chunk_list_id()),
            ConvertTo<TChannels>(TYsonString(jobSpec.output_specs(0).channels())),
            KeyColumns);
    }

    virtual NScheduler::NProto::TJobResult Run() override
    {
        PROFILE_TIMING ("/sort_time") {
            size_t keyColumnCount = KeyColumns.size();

            yhash_map<TStringBuf, int> keyColumnToIndex;

            std::vector< std::pair<TStringBuf, TStringBuf> > valueBuffer;
            std::vector<TSmallKeyPart> keyBuffer;
            std::vector<ui32> valueIndexBuffer;
            std::vector<ui32> rowIndexBuffer;

            auto estimatedRowCount = Host->GetJobSpec().input_row_count();

            LOG_INFO("Initializing");
            {
                for (int i = 0; i < KeyColumns.size(); ++i) {
                    TStringBuf name(~KeyColumns[i], KeyColumns[i].size());
                    keyColumnToIndex[name] = i;
                }

                Sync(~Reader, &TReader::AsyncOpen);

                valueBuffer.reserve(1000000);
                keyBuffer.reserve(estimatedRowCount * keyColumnCount);
                valueIndexBuffer.reserve(estimatedRowCount + 1);
                rowIndexBuffer.reserve(estimatedRowCount);

                // Add fake row.
                valueIndexBuffer.push_back(0);
            }

            PROFILE_TIMING_CHECKPOINT("init");

            LOG_INFO("Reading");
            {
                TLexer lexer;
                while (Reader->IsValid()) {
                    rowIndexBuffer.push_back(rowIndexBuffer.size());
                    YASSERT(rowIndexBuffer.back() <= std::numeric_limits<ui32>::max());

                    keyBuffer.resize(keyBuffer.size() + keyColumnCount);

                    FOREACH (const auto& pair, Reader->CurrentReader()->GetRow()) {
                        auto it = keyColumnToIndex.find(pair.first);
                        if (it != keyColumnToIndex.end()) {
                            auto& keyPart = keyBuffer[rowIndexBuffer.back() * keyColumnCount + it->second];
                            SetSmallKeyPart(keyPart, pair.second, lexer);
                        }
                        valueBuffer.push_back(pair);
                    }

                    valueIndexBuffer.push_back(valueBuffer.size());

                    if (!Reader->FetchNextItem()) {
                        Sync(~Reader, &TReader::GetReadyEvent);
                    }
                }
            }
            PROFILE_TIMING_CHECKPOINT("read");

            LOG_INFO("Sorting");

            std::sort(
                rowIndexBuffer.begin(), 
                rowIndexBuffer.end(),
                [&] (ui32 lhs, ui32 rhs) -> bool {
                    for (int i = 0; i < keyColumnCount; ++i) {
                        auto res = CompareSmallKeyParts(
                            keyBuffer[lhs * keyColumnCount + i], 
                            keyBuffer[rhs * keyColumnCount + i]);

                        if (res < 0)
                            return true;
                        if (res > 0)
                            return false;
                    }

                    return false;
            }
            );

            PROFILE_TIMING_CHECKPOINT("sort");

            LOG_INFO("Writing");
            {
                auto writer = CreateSyncWriter(Writer);
                writer->Open();

                TRow row;
                TNonOwningKey key(keyColumnCount);
                for (size_t progressIndex = 0; progressIndex < rowIndexBuffer.size(); ++progressIndex) {
                    row.clear();
                    key.Clear();

                    auto rowIndex = rowIndexBuffer[progressIndex];
                    for (auto valueIndex = valueIndexBuffer[rowIndex];
                        valueIndex < valueIndexBuffer[rowIndex + 1]; 
                        ++valueIndex)
                    {
                        row.push_back(valueBuffer[valueIndex]);
                    }

                    for (int keyIndex = 0; keyIndex < keyColumnCount; ++keyIndex) {
                        auto& keyPart = keyBuffer[rowIndex * keyColumnCount + keyIndex];
                        SetKeyPart(&key, keyPart, keyIndex);
                    }

                    writer->WriteRowUnsafe(row, key);

                    if (progressIndex % 1000 == 0) {
                        Writer->SetProgress(double(progressIndex) / rowIndexBuffer.size());
                    }
                }

                writer->Close();
            }

            PROFILE_TIMING_CHECKPOINT("write");

            LOG_INFO("Finalizing");
            {
                TJobResult result;
                ToProto(result.mutable_error(), TError());

                auto* resultExt = result.MutableExtension(TSortJobResultExt::sort_job_result_ext);
                ToProto(resultExt->mutable_chunks(), Writer->GetWrittenChunks());

                return result;
            }
        }
    }

    double GetProgress() const override
    {
        i64 total = Host->GetJobSpec().input_row_count();
        if (total == 0) {
            LOG_WARNING("GetProgress: empty total");
            return 0;
        } else {
            // Split progress evenly between reading and writing.
            double progress =
                0.5 * Reader->GetItemIndex() / total +
                0.5 * Writer->GetRowCount() / total;
            LOG_DEBUG("GetProgress: %lf", progress);
            return progress;
        }
    }

private:
    TKeyColumns KeyColumns;
    TIntrusivePtr<TReader> Reader;
    TTableChunkSequenceWriterPtr Writer;

};

TJobPtr CreateSimpleSortJob(IJobHost* host)
{
    return New<TSimpleSortJob>(host);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
