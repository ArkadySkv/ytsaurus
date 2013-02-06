﻿#include "stdafx.h"
#include "private.h"
#include "job_detail.h"
#include "config.h"
#include "partition_job.h"

#include <ytlib/misc/sync.h>

#include <ytlib/meta_state/master_channel.h>

#include <ytlib/chunk_client/client_block_cache.h>
#include <server/chunk_server/public.h>

#include <ytlib/table_client/partition_chunk_sequence_writer.h>
#include <ytlib/table_client/table_chunk_reader.h>
#include <ytlib/table_client/multi_chunk_parallel_reader.h>
#include <ytlib/table_client/partitioner.h>

#include <ytlib/yson/lexer.h>

namespace NYT {
namespace NJobProxy {

using namespace NElection;
using namespace NTableClient;
using namespace NChunkClient;
using namespace NChunkServer;
using namespace NYTree;
using namespace NScheduler::NProto;

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = JobProxyLogger;
static NProfiling::TProfiler& Profiler = JobProxyProfiler;

////////////////////////////////////////////////////////////////////////////////

typedef TMultiChunkParallelReader<TTableChunkReader> TReader;

class TPartitionJob
    : public TJob
{
public:
    explicit TPartitionJob(IJobHost* host)
        : TJob(host)
    {
        const auto& jobSpec = Host->GetJobSpec();
        auto config = Host->GetConfig();

        YCHECK(jobSpec.input_specs_size() == 1);
        YCHECK(jobSpec.output_specs_size() == 1);

        auto masterChannel = CreateLeaderChannel(config->Masters);
        auto blockCache = CreateClientBlockCache(New<TClientBlockCacheConfig>());
        auto jobSpecExt = jobSpec.GetExtension(TPartitionJobSpecExt::partition_job_spec_ext);

        std::vector<NTableClient::NProto::TInputChunk> chunks(
            jobSpec.input_specs(0).chunks().begin(),
            jobSpec.input_specs(0).chunks().end());

        auto provider = New<TTableChunkReaderProvider>(config->JobIO->TableReader);
        Reader = New<TReader>(
            config->JobIO->TableReader,
            masterChannel,
            blockCache,
            std::move(chunks),
            provider);

        if (jobSpecExt.partition_keys_size() > 0) {
            YCHECK(jobSpecExt.partition_keys_size() + 1 == jobSpecExt.partition_count());
            FOREACH (const auto& key, jobSpecExt.partition_keys()) {
                PartitionKeys.push_back(TOwningKey::FromProto(key));
            }
            Partitioner = CreateOrderedPartitioner(&PartitionKeys);
        } else {
            Partitioner = CreateHashPartitioner(jobSpecExt.partition_count());
        }

        auto transactionId = TTransactionId::FromProto(jobSpec.output_transaction_id());
        const auto& outputSpec = jobSpec.output_specs(0);
        auto account = outputSpec.account();
        auto chunkListId = TChunkListId::FromProto(outputSpec.chunk_list_id());
        auto keyColumns = FromProto<Stroka>(jobSpecExt.key_columns());
        Writer = New<TPartitionChunkSequenceWriter>(
            config->JobIO->TableWriter,
            masterChannel,
            transactionId,
            account,
            chunkListId,
            keyColumns,
            ~Partitioner);
    }

    virtual NScheduler::NProto::TJobResult Run() override
    {
        PROFILE_TIMING ("/partition_time") {
            LOG_INFO("Initializing");
            {
                Sync(~Reader, &TReader::AsyncOpen);
                Sync(~Writer, &TPartitionChunkSequenceWriter::AsyncOpen);
            }
            PROFILE_TIMING_CHECKPOINT("init");

            LOG_INFO("Partitioning");
            {
                while (Reader->IsValid()) {
                    while (!Writer->TryWriteRowUnsafe(Reader->CurrentReader()->GetRow())) {
                        Sync(~Writer, &TPartitionChunkSequenceWriter::GetReadyEvent);
                    }
                    if (!Reader->FetchNextItem()) {
                        Sync(~Reader, &TReader::GetReadyEvent);
                    }
                }

                Sync(~Writer, &TPartitionChunkSequenceWriter::AsyncClose);
            }
            PROFILE_TIMING_CHECKPOINT("partition");

            LOG_INFO("Finalizing");
            {
                TJobResult result;
                ToProto(result.mutable_error(), TError());
                auto* resultExt = result.MutableExtension(TPartitionJobResultExt::partition_job_result_ext);
                ToProto(resultExt->mutable_chunks(), Writer->GetWrittenChunks());
                return result;
            }
        }
    }

    double GetProgress() const override
    {
        i64 total = Reader->GetItemCount();
        if (total == 0) {
            LOG_WARNING("GetProgress: empty total");
            return 0.0;
        } else {
            double progress = (double) Reader->GetItemIndex() / total;
            LOG_DEBUG("GetProgress: %lf", progress);
            return progress;
        }
    }

private:
    TIntrusivePtr<TReader> Reader;
    TPartitionChunkSequenceWriterPtr Writer;
    std::vector<TOwningKey> PartitionKeys;
    TAutoPtr<IPartitioner> Partitioner;

};

TJobPtr CreatePartitionJob(IJobHost* host)
{
    return New<TPartitionJob>(host);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
