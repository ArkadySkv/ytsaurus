﻿#include "stdafx.h"
#include "sorted_reduce_job_io.h"
#include "config.h"
#include "user_job_io.h"
#include "job.h"

#include <ytlib/chunk_client/client_block_cache.h>
#include <ytlib/chunk_client/chunk_spec.pb.h>

#include <ytlib/chunk_client/multi_chunk_sequential_reader.h>
#include <ytlib/table_client/table_chunk_reader.h>
#include <ytlib/table_client/sync_reader.h>
#include <ytlib/table_client/table_producer.h>
#include <ytlib/table_client/merging_reader.h>

namespace NYT {
namespace NJobProxy {

using namespace NScheduler;
using namespace NScheduler::NProto;
using namespace NTableClient;
using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NJobTrackerClient::NProto;

////////////////////////////////////////////////////////////////////

class TSortedReduceJobIO
    : public TUserJobIO
{
public:
    TSortedReduceJobIO(
        TJobIOConfigPtr ioConfig,
        IJobHost* host)
        : TUserJobIO(ioConfig, host)
    { }

    std::unique_ptr<TTableProducer> CreateTableInput(
        int index,
        NYson::IYsonConsumer* consumer) override
    {
        YCHECK(index >= 0 && index < GetInputCount());

        std::vector<TTableChunkSequenceReaderPtr> readers;
        auto options = New<TChunkReaderOptions>();
        options->ReadKey = true;

        const auto& jobSpec = Host->GetJobSpec();
        const auto& schedulerJobSpecExt = jobSpec.GetExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);

        for (const auto& inputSpec : schedulerJobSpecExt.input_specs()) {
            // ToDo(psushin): validate that input chunks are sorted.
            std::vector<TChunkSpec> chunks(inputSpec.chunks().begin(), inputSpec.chunks().end());

            auto provider = New<TTableChunkReaderProvider>(
                chunks,
                IOConfig->TableReader,
                options);

            auto reader = New<TTableChunkSequenceReader>(
                IOConfig->TableReader,
                Host->GetMasterChannel(),
                Host->GetBlockCache(),
                Host->GetNodeDirectory(),
                std::move(chunks),
                provider);

            readers.push_back(reader);
        }

        auto reader = CreateMergingReader(readers);

        // ToDo(psushin): init all inputs in constructor, get rid of this check.
        YCHECK(index == Inputs.size());
        Inputs.push_back(reader);

        reader->Open();

        return std::unique_ptr<TTableProducer>(new TTableProducer(reader, consumer));
    }

    virtual void PopulateResult(TJobResult* result) override
    {
        auto* resultExt = result->MutableExtension(TReduceJobResultExt::reduce_job_result_ext);
        PopulateUserJobResult(resultExt->mutable_reducer_result());
    }
};

std::unique_ptr<TUserJobIO> CreateSortedReduceJobIO(
    TJobIOConfigPtr ioConfig,
    IJobHost* host)
{
    return std::unique_ptr<TUserJobIO>(new TSortedReduceJobIO(ioConfig, host));
}

////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
