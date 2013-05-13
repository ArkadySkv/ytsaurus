#pragma once

#include "public.h"

#include <ytlib/ytree/yson_serializable.h>

#include <ytlib/table_client/config.h>

#include <ytlib/file_client/config.h>

#include <ytlib/rpc/retrying_channel.h>

#include <server/job_proxy/config.h>

namespace NYT {
namespace NScheduler {

////////////////////////////////////////////////////////////////////////////////

class TFairShareStrategyConfig
    : public TYsonSerializable
{
public:
    // The following settings can be overridden in operation spec.
    TDuration MinSharePreemptionTimeout;
    TDuration FairSharePreemptionTimeout;
    double FairShareStarvationTolerance;
    double FairSharePreemptionTolerance;

    TDuration FairShareUpdatePeriod;

    double NewOperationWeightBoostFactor;
    TDuration NewOperationWeightBoostPeriod;

    //! Any operation with usage less than this cannot be preempted.
    double MinPreemptableRatio;

    TFairShareStrategyConfig()
    {
        RegisterParameter("min_share_preemption_timeout", MinSharePreemptionTimeout)
            .Default(TDuration::Seconds(15));
        RegisterParameter("fair_share_preemption_timeout", FairSharePreemptionTimeout)
            .Default(TDuration::Seconds(30));
        RegisterParameter("fair_share_starvation_tolerance", FairShareStarvationTolerance)
            .InRange(0.0, 1.0)
            .Default(0.8);
        RegisterParameter("fair_share_preemption_tolerance", FairSharePreemptionTolerance)
            .GreaterThanOrEqual(0.0)
            .Default(1.05);

        RegisterParameter("fair_share_update_period", FairShareUpdatePeriod)
            .Default(TDuration::MilliSeconds(1000));

        RegisterParameter("new_operation_weight_boost_factor", NewOperationWeightBoostFactor)
            .GreaterThanOrEqual(1.0)
            .Default(1.0);
        RegisterParameter("new_operation_weight_boost_period", NewOperationWeightBoostPeriod)
            .Default(TDuration::Minutes(0));

        RegisterParameter("min_preemptable_ratio", MinPreemptableRatio)
            .InRange(0.0, 1.0)
            .Default(0.05);
    }
};

////////////////////////////////////////////////////////////////////////////////

class TSchedulerConfig
    : public TFairShareStrategyConfig
{
public:
    TDuration ConnectRetryPeriod;

    TDuration NodeHearbeatTimeout;

    TDuration TransactionsRefreshPeriod;

    TDuration OperationsUpdatePeriod;

    TDuration WatchersUpdatePeriod;

    TDuration ResourceDemandSanityCheckPeriod;

    TDuration LockTransactionTimeout;

    TDuration OperationTransactionTimeout;

    //! Timeout used for direct RPC requests to nodes.
    TDuration NodeRpcTimeout;

    ESchedulerStrategy Strategy;

    //! Once this limit is reached the operation fails.
    int MaxFailedJobCount;

    //! Limits the number of stderrs the operation is allowed to produce.
    int MaxStdErrCount;

    //! Number of chunk lists to be allocated when an operation starts.
    int ChunkListPreallocationCount;

    //! Maximum number of chunk lists to request via a single request.
    int MaxChunkListAllocationCount;

    //! Better keep the number of spare chunk lists above this threshold.
    int ChunkListWatermarkCount;

    //! Each time the number of spare chunk lists drops below #ChunkListWatermarkCount or
    //! the controller requests more chunk lists than we currently have,
    //! another batch is allocated. Each time we allocate #ChunkListAllocationMultiplier times
    //! more chunk lists than previously.
    double ChunkListAllocationMultiplier;

    //! Maximum number of chunk trees to attach per request.
    int MaxChildrenPerAttachRequest;

    //! Max size of data slice for different jobs.
    i64 MapJobMaxSliceDataSize;
    i64 MergeJobMaxSliceDataSize;
    i64 SortJobMaxSliceDataSize;
    i64 PartitionJobMaxSliceDataSize;

    //! Maximum number of partitions during sort, ever.
    int MaxPartitionCount;

    //! Maximum number of jobs per operation (an approximation!).
    int MaxJobCount;

    //! Maximum size of table allowed to be passed as a file to jobs.
    i64 MaxTableFileSize;

    //! Maximum number of output tables an operation can have.
    int MaxOutputTableCount;

    int MaxInputTableCount;

    //! Maximum number of jobs to start within a single heartbeat.
    TNullable<int> MaxStartedJobsPerHeartbeat;

    NYTree::INodePtr MapOperationSpec;
    NYTree::INodePtr ReduceOperationSpec;
    NYTree::INodePtr EraseOperationSpec;
    NYTree::INodePtr OrderedMergeOperationSpec;
    NYTree::INodePtr UnorderedMergeOperationSpec;
    NYTree::INodePtr SortedMergeOperationSpec;
    NYTree::INodePtr MapReduceOperationSpec;
    NYTree::INodePtr SortOperationSpec;

    // Default environment variables set for every job.
    yhash_map<Stroka, Stroka> Environment;

    TDuration SnapshotPeriod;
    TDuration SnapshotTimeout;
    Stroka SnapshotTempPath;
    NFileClient::TFileReaderConfigPtr SnapshotReader;
    NFileClient::TFileWriterConfigPtr SnapshotWriter;

    NRpc::TRetryingChannelConfigPtr NodeChannel;

    TSchedulerConfig()
    {
        RegisterParameter("connect_retry_period", ConnectRetryPeriod)
            .Default(TDuration::Seconds(15));
        RegisterParameter("node_heartbeat_timeout", NodeHearbeatTimeout)
            .Default(TDuration::Seconds(60));
        RegisterParameter("transactions_refresh_period", TransactionsRefreshPeriod)
            .Default(TDuration::Seconds(3));
        RegisterParameter("operations_update_period", OperationsUpdatePeriod)
            .Default(TDuration::Seconds(3));
        RegisterParameter("watchers_update_period", WatchersUpdatePeriod)
            .Default(TDuration::Seconds(15));
        RegisterParameter("resource_demand_sanity_check_period", ResourceDemandSanityCheckPeriod)
            .Default(TDuration::Seconds(15));
        RegisterParameter("lock_transaction_timeout", LockTransactionTimeout)
            .Default(TDuration::Seconds(15));
        RegisterParameter("operation_transaction_timeout", OperationTransactionTimeout)
            .Default(TDuration::Minutes(60));
        RegisterParameter("node_rpc_timeout", NodeRpcTimeout)
            .Default(TDuration::Seconds(15));

        RegisterParameter("strategy", Strategy)
            .Default(ESchedulerStrategy::Null);

        RegisterParameter("max_failed_job_count", MaxFailedJobCount)
            .Default(100)
            .GreaterThanOrEqual(0);
        RegisterParameter("max_stderr_count", MaxStdErrCount)
            .Default(100)
            .GreaterThanOrEqual(0);

        RegisterParameter("chunk_list_preallocation_count", ChunkListPreallocationCount)
            .Default(128)
            .GreaterThanOrEqual(0);
        RegisterParameter("max_chunk_list_allocation_count", MaxChunkListAllocationCount)
            .Default(16384)
            .GreaterThanOrEqual(0);
        RegisterParameter("chunk_list_watermark_count", ChunkListWatermarkCount)
            .Default(50)
            .GreaterThanOrEqual(0);
        RegisterParameter("chunk_list_allocation_multiplier", ChunkListAllocationMultiplier)
            .Default(2.0)
            .GreaterThan(1.0);

        RegisterParameter("max_children_per_attach_request", MaxChildrenPerAttachRequest)
            .Default(10000)
            .GreaterThan(0);

        RegisterParameter("map_job_max_slice_data_size", MapJobMaxSliceDataSize)
            .Default((i64)256 * 1024 * 1024)
            .GreaterThan(0);

        RegisterParameter("merge_job_max_slice_data_size", MergeJobMaxSliceDataSize)
            .Default((i64)256 * 1024 * 1024)
            .GreaterThan(0);

        RegisterParameter("partition_job_max_slice_data_size", PartitionJobMaxSliceDataSize)
            .Default((i64)256 * 1024 * 1024)
            .GreaterThan(0);

        RegisterParameter("sort_job_max_slice_data_size", SortJobMaxSliceDataSize)
            .Default((i64)256 * 1024 * 1024)
            .GreaterThan(0);

        RegisterParameter("max_partition_count", MaxPartitionCount)
            .Default(2000)
            .GreaterThan(0);

        RegisterParameter("table_file_size_limit", MaxTableFileSize)
            .Default((i64) 2 * 1024 * 1024 * 1024);

        RegisterParameter("max_input_table_count", MaxInputTableCount)
            .Default(1000)
            .GreaterThan(1);

        RegisterParameter("max_output_table_count", MaxOutputTableCount)
            .Default(20)
            .GreaterThan(1)
            .LessThan(1000);

        RegisterParameter("max_started_jobs_per_heartbeat", MaxStartedJobsPerHeartbeat)
            .Default()
            .GreaterThan(0);

        RegisterParameter("map_operation_spec", MapOperationSpec)
            .Default(nullptr);
        RegisterParameter("reduce_operation_spec", ReduceOperationSpec)
            .Default(nullptr);
        RegisterParameter("erase_operation_spec", EraseOperationSpec)
            .Default(nullptr);
        RegisterParameter("ordered_merge_operation_spec", OrderedMergeOperationSpec)
            .Default(nullptr);
        RegisterParameter("unordered_merge_operation_spec", UnorderedMergeOperationSpec)
            .Default(nullptr);
        RegisterParameter("sorted_merge_operation_spec", SortedMergeOperationSpec)
            .Default(nullptr);
        RegisterParameter("map_reduce_operation_spec", MapReduceOperationSpec)
            .Default(nullptr);
        RegisterParameter("sort_operation_spec", SortOperationSpec)
            .Default(nullptr);

        RegisterParameter("max_job_count", MaxJobCount)
            .Default(20000)
            .GreaterThan(0);

        RegisterParameter("environment", Environment)
            .Default(yhash_map<Stroka, Stroka>());

        RegisterParameter("node_channel", NodeChannel)
            .DefaultNew();

        RegisterParameter("snapshot_timeout", SnapshotTimeout)
            .Default(TDuration::Seconds(60));
        RegisterParameter("snapshot_period", SnapshotPeriod)
            .Default(TDuration::Seconds(300));
        RegisterParameter("snapshot_temp_path", SnapshotTempPath)
            .NonEmpty();
        RegisterParameter("snapshot_reader", SnapshotReader)
            .DefaultNew();
        RegisterParameter("snapshot_writer", SnapshotWriter)
            .DefaultNew();
    }
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT
