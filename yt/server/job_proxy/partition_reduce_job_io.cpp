﻿#include "stdafx.h"
#include "partition_reduce_job_io.h"
#include "config.h"
#include "sorting_reader.h"
#include "user_job_io.h"
#include "job.h"

#include <ytlib/misc/protobuf_helpers.h>

#include <ytlib/chunk_client/client_block_cache.h>
#include <ytlib/chunk_client/multi_chunk_sequential_writer.h>

#include <ytlib/table_client/sync_reader.h>
#include <ytlib/table_client/table_producer.h>
#include <ytlib/table_client/table_chunk_writer.h>

#include <ytlib/node_tracker_client/node_directory.h>

namespace NYT {
namespace NJobProxy {

using namespace NScheduler;
using namespace NScheduler::NProto;
using namespace NTableClient;
using namespace NChunkClient;
using namespace NJobTrackerClient::NProto;

////////////////////////////////////////////////////////////////////

class TPartitionReduceJobIO
    : public TUserJobIO
{
public:
    TPartitionReduceJobIO(
        TJobIOConfigPtr ioConfig,
        IJobHost* host)
        : TUserJobIO(ioConfig, host)
    { }

    std::unique_ptr<NTableClient::TTableProducer> CreateTableInput(
        int index,
        NYson::IYsonConsumer* consumer) override
    {
        YCHECK(index == 0);

        const auto& jobSpec = Host->GetJobSpec();
        const auto& schedulerJobSpecExt = jobSpec.GetExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);

        YCHECK(schedulerJobSpecExt.input_specs_size() == 1);

        const auto& inputSpec = schedulerJobSpecExt.input_specs(0);
        std::vector<NChunkClient::NProto::TChunkSpec> chunks(
            inputSpec.chunks().begin(),
            inputSpec.chunks().end());

        auto jobSpecExt = jobSpec.GetExtension(TReduceJobSpecExt::reduce_job_spec_ext);
        auto keyColumns = FromProto<Stroka>(jobSpecExt.key_columns());

        auto reader = CreateSortingReader(
            IOConfig->TableReader,
            Host->GetMasterChannel(),
            Host->GetBlockCache(),
            Host->GetNodeDirectory(),
            keyColumns,
            BIND(&IJobHost::ReleaseNetwork, Host),
            std::move(chunks),
            schedulerJobSpecExt.input_row_count(),
            schedulerJobSpecExt.is_approximate());

        YCHECK(index == Inputs.size());

        // NB: put reader here before opening, for proper failed chunk generation.
        Inputs.push_back(reader);

        reader->Open();

        return std::unique_ptr<NTableClient::TTableProducer>(new TTableProducer(reader, consumer));
    }

    virtual void PopulateResult(TJobResult* result) override
    {
        auto* resultExt = result->MutableExtension(TReduceJobResultExt::reduce_job_result_ext);
        PopulateUserJobResult(resultExt->mutable_reducer_result());

        // This code is required for proper handling of intermediate chunks, when
        // PartitionReduce job is run as ReduceCombiner in MapReduce operation.
        auto* schedulerResultExt = result->MutableExtension(TSchedulerJobResultExt::scheduler_job_result_ext);
        Outputs[0]->GetNodeDirectory()->DumpTo(schedulerResultExt->mutable_node_directory());
        ToProto(schedulerResultExt->mutable_chunks(), Outputs[0]->GetWrittenChunks());
    }

};

std::unique_ptr<TUserJobIO> CreatePartitionReduceJobIO(
    TJobIOConfigPtr ioConfig,
    IJobHost* host)
{
    return std::unique_ptr<TUserJobIO>(new TPartitionReduceJobIO(ioConfig, host));
}

////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
