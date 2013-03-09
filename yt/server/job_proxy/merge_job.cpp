﻿#include "stdafx.h"
#include "merge_job.h"
#include "private.h"
#include "job_detail.h"
#include "config.h"

#include <ytlib/meta_state/master_channel.h>

#include <ytlib/table_client/table_chunk_reader.h>
#include <ytlib/table_client/sync_reader.h>
#include <ytlib/table_client/table_chunk_sequence_writer.h>
#include <ytlib/table_client/multi_chunk_sequential_reader.h>
#include <ytlib/table_client/multi_chunk_parallel_reader.h>
#include <ytlib/table_client/sync_writer.h>

#include <ytlib/chunk_client/remote_reader.h>
#include <ytlib/chunk_client/client_block_cache.h>

#include <server/chunk_server/public.h>

#include <ytlib/ytree/yson_string.h>
#include <ytlib/yson/lexer.h>

namespace NYT {
namespace NJobProxy {

using namespace NYTree;
using namespace NTableClient;
using namespace NChunkClient;
using namespace NChunkServer;
using namespace NScheduler::NProto;
using namespace NTableClient::NProto;

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = JobProxyLogger;
static NProfiling::TProfiler& Profiler = JobProxyProfiler;

////////////////////////////////////////////////////////////////////////////////

template <template <typename> class TMultiChunkReader>
class TMergeJob
    : public TJob
{
public:
    typedef TMultiChunkReader<TTableChunkReader> TTableMultiChunkReader;

    explicit TMergeJob(IJobHost* host)
        : TJob(host)
    {
        const auto& jobSpec = Host->GetJobSpec();
        auto config = Host->GetConfig();

        YCHECK(jobSpec.output_specs_size() == 1);

        std::vector<TInputChunk> inputChunks;
        FOREACH (const auto& inputSpec, jobSpec.input_specs()) {
            FOREACH (const auto& inputChunk, inputSpec.chunks()) {
                inputChunks.push_back(inputChunk);
            }
        }

        auto provider = New<TTableChunkReaderProvider>(config->JobIO->TableReader);
        Reader = CreateSyncReader(New<TTableMultiChunkReader>(
            config->JobIO->TableReader,
            Host->GetMasterChannel(),
            Host->GetBlockCache(),
            Host->GetNodeDirectory(),
            std::move(inputChunks),
            provider));

        if (jobSpec.HasExtension(TMergeJobSpecExt::merge_job_spec_ext)) {
            const auto& mergeSpec = jobSpec.GetExtension(TMergeJobSpecExt::merge_job_spec_ext);
            KeyColumns.Assign(FromProto<Stroka>(mergeSpec.key_columns()));

            LOG_INFO("Ordered merge produces sorted output");
        }

        // ToDo(psushin): estimate row count for writer.
        auto transactionId = FromProto<TTransactionId>(jobSpec.output_transaction_id());
        const auto& outputSpec = jobSpec.output_specs(0);

        auto chunkListId = FromProto<TChunkListId>(outputSpec.chunk_list_id());
        auto options = ConvertTo<TTableWriterOptionsPtr>(TYsonString(outputSpec.table_writer_options()));
        options->KeyColumns = KeyColumns;
        Writer = New<TTableChunkSequenceWriter>(
            config->JobIO->TableWriter,
            options,
            Host->GetMasterChannel(),
            transactionId,
            chunkListId);
    }

    virtual NScheduler::NProto::TJobResult Run() override
    {
        PROFILE_TIMING ("/merge_time") {
            LOG_INFO("Initializing");

            yhash_map<TStringBuf, int> keyColumnToIndex;

            auto writer = CreateSyncWriter(Writer);
            {
                if (KeyColumns) {
                    for (int i = 0; i < KeyColumns->size(); ++i) {
                        TStringBuf name(~KeyColumns->at(i), KeyColumns->at(i).size());
                        keyColumnToIndex[name] = i;
                    }
                }

                Reader->Open();
                writer->Open();
            }
            PROFILE_TIMING_CHECKPOINT("init");

            LOG_INFO("Merging");
            {
                NYson::TLexer lexer;
                // Unsorted write - use dummy key.
                TNonOwningKey key;
                if (KeyColumns)
                    key.ClearAndResize(KeyColumns->size());

                while (const TRow* row = Reader->GetRow()) {
                    if (KeyColumns) {
                        key.Clear();

                        FOREACH (const auto& pair, *row) {
                            auto it = keyColumnToIndex.find(pair.first);
                            if (it != keyColumnToIndex.end()) {
                                key.SetKeyPart(it->second, pair.second, lexer);
                            }
                        }
                        writer->WriteRowUnsafe(*row, key);
                    } else {
                        writer->WriteRowUnsafe(*row);
                    }
                }
            }
            PROFILE_TIMING_CHECKPOINT("merge");

            LOG_INFO("Finalizing");
            {
                writer->Close();

                TJobResult result;
                ToProto(result.mutable_error(), TError());
                return result;
            }
        }
    }

    double GetProgress() const override
    {
        i64 total = Reader->GetRowCount();
        if (total == 0) {
            LOG_WARNING("GetProgress: empty total");
            return 0;
        } else {
            double progress = (double) Reader->GetRowIndex() / total;
            LOG_DEBUG("GetProgress: %lf", progress);
            return progress;
        }
    }

    std::vector<NChunkClient::TChunkId> GetFailedChunks() const override
    {
        return Reader->GetFailedChunks();
    }

private:
    ISyncReaderPtr Reader;
    TTableChunkSequenceWriterPtr Writer;

    TNullable<TKeyColumns> KeyColumns;

};

TJobPtr CreateOrderedMergeJob(IJobHost* host)
{
    return New< TMergeJob<TMultiChunkSequentialReader> >(host);
}

TJobPtr CreateUnorderedMergeJob(IJobHost* host)
{
    return New< TMergeJob<TMultiChunkParallelReader> >(host);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
