#pragma once

#include "public.h"

#include <ytlib/ypath/rich.h>

#include <ytlib/ytree/yson_serializable.h>

#include <ytlib/table_client/config.h>
#include <ytlib/file_client/config.h>

#include <ytlib/formats/format.h>

namespace NYT {
namespace NScheduler {

////////////////////////////////////////////////////////////////////////////////

struct TJobIOConfig
    : public TYsonSerializable
{
    NTableClient::TTableReaderConfigPtr TableReader;
    NTableClient::TTableWriterConfigPtr TableWriter;
    NFileClient::TFileWriterConfigPtr ErrorFileWriter;

    TJobIOConfig()
    {
        Register("table_reader", TableReader)
            .DefaultNew();
        Register("table_writer", TableWriter)
            .DefaultNew();
        Register("error_file_writer", ErrorFileWriter)
            .DefaultNew();

        // We do not provide much fault tolerance for stderr by default.
        ErrorFileWriter->ReplicationFactor = 1;
        ErrorFileWriter->UploadReplicationFactor = 1;
        ErrorFileWriter->ChunkVital = false;
    }
};

////////////////////////////////////////////////////////////////////////////////

struct TOperationSpecBase
    : public TYsonSerializable
{
    TOperationSpecBase()
    {
        SetKeepOptions(true);
    }
};

////////////////////////////////////////////////////////////////////////////////

struct TUserJobSpec
    : public TYsonSerializable
{
    Stroka Command;
    
    std::vector<NYPath::TRichYPath> FilePaths;

    TNullable<NFormats::TFormat> Format;
    TNullable<NFormats::TFormat> InputFormat;
    TNullable<NFormats::TFormat> OutputFormat;

    yhash_map<Stroka, Stroka> Environment;
    
    int CpuLimit;
    i64 MemoryLimit;

    TUserJobSpec()
    {
        Register("command", Command);
        Register("file_paths", FilePaths)
            .Default();
        Register("format", Format)
            .Default();
        Register("input_format", InputFormat)
            .Default();
        Register("output_format", OutputFormat)
            .Default();
        Register("environment", Environment)
            .Default(yhash_map<Stroka, Stroka>());
        Register("cpu_limit", CpuLimit)
            .Default(1);
        Register("memory_limit", MemoryLimit)
            .Default((i64) 1024 * 1024 * 1024);
    }
};

////////////////////////////////////////////////////////////////////////////////

struct TMapOperationSpec
    : public TOperationSpecBase
{
    TUserJobSpecPtr Mapper;
    std::vector<NYPath::TRichYPath> InputTablePaths;
    std::vector<NYPath::TRichYPath> OutputTablePaths;
    TNullable<int> JobCount;
    i64 JobSliceDataSize;
    i64 MinDataSizePerJob;
    i64 MaxDataSizePerJob;
    TDuration LocalityTimeout;
    TJobIOConfigPtr JobIO;

    TMapOperationSpec()
    {
        Register("mapper", Mapper);
        Register("input_table_paths", InputTablePaths)
            .NonEmpty();
        Register("output_table_paths", OutputTablePaths);
        Register("job_count", JobCount)
            .Default()
            .GreaterThan(0);
        Register("job_slice_data_size", JobSliceDataSize)
            .Default((i64) 256 * 1024 * 1024)
            .GreaterThan(0);
        Register("min_data_size_per_job", MinDataSizePerJob)
            .Default((i64) 128 * 1024 * 1024)
            .GreaterThan(0);
        Register("max_data_size_per_job", MaxDataSizePerJob)
            .Default((i64) 1024 * 1024 * 1024)
            .GreaterThan(0);
        Register("locality_timeout", LocalityTimeout)
            .Default(TDuration::Seconds(5));
        Register("job_io", JobIO)
            .DefaultNew();

        JobIO->TableReader->PrefetchWindow = 10;
    }
};

////////////////////////////////////////////////////////////////////////////////

struct TMergeOperationSpecBase
    : public TOperationSpecBase
{
    //! During sorted merge the scheduler tries to ensure that large connected
    //! groups of chunks are partitioned into tasks of this or smaller size.
    //! This number, however, is merely an estimate, i.e. some tasks may still
    //! be larger.
    i64 MaxDataSizePerJob;

    i64 JobSliceDataSize;

    TDuration LocalityTimeout;
    TJobIOConfigPtr JobIO;

    TMergeOperationSpecBase()
    {
        Register("max_data_size_per_job", MaxDataSizePerJob)
            .Default((i64) 1024 * 1024 * 1024)
            .GreaterThan(0);
        Register("locality_timeout", LocalityTimeout)
            .Default(TDuration::Seconds(5));
        Register("job_io", JobIO)
            .DefaultNew();
        Register("job_slice_data_size", JobSliceDataSize)
            .Default((i64) 128 * 1024 * 1024)
            .GreaterThan(0);
    }
};

////////////////////////////////////////////////////////////////////////////////

DECLARE_ENUM(EMergeMode,
    (Sorted)
    (Ordered)
    (Unordered)
);

struct TMergeOperationSpec
    : public TMergeOperationSpecBase
{
    std::vector<NYPath::TRichYPath> InputTablePaths;
    NYPath::TRichYPath OutputTablePath;
    EMergeMode Mode;
    bool CombineChunks;
    bool AllowPassthroughChunks;
    TNullable< std::vector<Stroka> > MergeBy;

    TMergeOperationSpec()
    {
        Register("input_table_paths", InputTablePaths);
        Register("output_table_path", OutputTablePath);
        Register("combine_chunks", CombineChunks)
            .Default(false);
        Register("mode", Mode)
            .Default(EMergeMode::Unordered);
        Register("allow_passthrough_chunks", AllowPassthroughChunks)
            .Default(true);
        Register("merge_by", MergeBy)
            .Default();
    }
};

struct TUnorderedMergeOperationSpec
    : public TMergeOperationSpec
{ 
    TUnorderedMergeOperationSpec()
    {
        JobIO->TableReader->PrefetchWindow = 10;
    }
};

struct TOrderedMergeOperationSpec
    : public TMergeOperationSpec
{ 

};

struct TSortedMergeOperationSpec
    : public TMergeOperationSpec
{ };

////////////////////////////////////////////////////////////////////////////////

struct TEraseOperationSpec
    : public TMergeOperationSpecBase
{
    NYPath::TRichYPath TablePath;
    bool CombineChunks;

    TEraseOperationSpec()
    {
        Register("table_path", TablePath);
        Register("combine_chunks", CombineChunks)
            .Default(false);
    }
};

////////////////////////////////////////////////////////////////////////////////

struct TReduceOperationSpec
    : public TMergeOperationSpecBase
{
    TUserJobSpecPtr Reducer;
    std::vector<NYPath::TRichYPath> InputTablePaths;
    std::vector<NYPath::TRichYPath> OutputTablePaths;
    TNullable< std::vector<Stroka> > ReduceBy;

    TReduceOperationSpec()
    {
        Register("reducer", Reducer);
        Register("input_table_paths", InputTablePaths);
        Register("output_table_paths", OutputTablePaths);
        Register("reduce_by", ReduceBy)
            .Default();
    }
};

////////////////////////////////////////////////////////////////////////////////

struct TSortOperationSpecBase
    : public TOperationSpecBase
{
    std::vector<NYPath::TRichYPath> InputTablePaths;

    TNullable<int> PartitionCount;

    TNullable<int> PartitionJobCount;
    i64 PartitionJobSliceDataSize;

    //! Only used if no partitioning is done.
    i64 SortJobSliceDataSize;

    //! Minimum amount of (uncompressed) data to be given to a single partition job.
    i64 MinDataSizePerPartitionJob;

    //! Maximum amount of (uncompressed) data to be given to a single partition job.
    i64 MaxDataSizePerPartitionJob;

    i64 MinPartitionDataSize;
    i64 MaxPartitionDataSize;

    //! Minimum amount of (uncompressed) data to be given to a single sort job.
    //! Used for adjusting #SortJobCount.
    //! Only applies to simple sort.
    i64 MinDataSizePerSortJob;

    //! Maximum amount of (uncompressed) data to be given to a single sort job
    //! or to a single unordered merge job taking care of a maniac partition.
    i64 MaxDataSizePerSortJob;

    double ShuffleStartThreshold;
    double MergeStartThreshold;

    TDuration PartitionLocalityTimeout;
    TDuration SortLocalityTimeout;
    TDuration MergeLocalityTimeout;

    int ShuffleNetworkLimit;

    TSortOperationSpecBase()
    {
        Register("input_table_paths", InputTablePaths);
        Register("partition_count", PartitionCount)
            .Default()
            .GreaterThan(0);
        Register("min_partition_data_size", MinPartitionDataSize)
            .Default((i64) 128 * 1024 * 1024)
            .GreaterThan(0);
        Register("max_partition_data_size", MaxPartitionDataSize)
            .Default((i64) 750 * 1024 * 1024)
            .GreaterThan(0);
        Register("min_data_size_per_sort_job", MinDataSizePerSortJob)
            .Default((i64) 128 * 1024 * 1024)
            .GreaterThan(0);
        Register("max_data_size_per_sort_job", MaxDataSizePerSortJob)
            .Default((i64) 1024 * 1024 * 1024)
            .GreaterThan(0);
        Register("shuffle_start_threshold", ShuffleStartThreshold)
            .Default(0.75)
            .InRange(0.0, 1.0);
        Register("merge_start_threshold", MergeStartThreshold)
            .Default(0.9)
            .InRange(0.0, 1.0);
        Register("shuffle_network_limit", ShuffleNetworkLimit)
            .Default(20);
    }

};

////////////////////////////////////////////////////////////////////////////////

struct TSortOperationSpec
    : public TSortOperationSpecBase
{
    NYPath::TRichYPath OutputTablePath;
    
    std::vector<Stroka> SortBy;

    // Desired number of samples per partition.
    int SamplesPerPartition;

    //! Only used if no partitioning is done.
    TNullable<int> SortJobCount;

    TJobIOConfigPtr PartitionJobIO;
    TJobIOConfigPtr SortJobIO;
    TJobIOConfigPtr MergeJobIO;

    TSortOperationSpec()
    {
        Register("output_table_path", OutputTablePath);
        Register("sort_by", SortBy)
            .NonEmpty();
        Register("sort_job_count", SortJobCount)
            .Default()
            .GreaterThan(0);
        Register("samples_per_partition", SamplesPerPartition)
            .Default(10)
            .GreaterThan(1);
        Register("partition_job_io", PartitionJobIO)
            .DefaultNew();
        Register("sort_job_io", SortJobIO)
            .DefaultNew();
        Register("merge_job_io", MergeJobIO)
            .DefaultNew();

        // Provide custom names for shared settings.
        Register("partition_job_count", PartitionJobCount)
            .Default()
            .GreaterThan(0);
        Register("partition_job_slice_data_size", PartitionJobSliceDataSize)
            .Default((i64) 256 * 1024 * 1024)
            .GreaterThan(0);
        Register("sort_job_slice_data_size", SortJobSliceDataSize)
            .Default((i64) 256 * 1024 * 1024)
            .GreaterThan(0);
        Register("min_data_size_per_partition_job", MinDataSizePerPartitionJob)
            .Default((i64) 128 * 1024 * 1024)
            .GreaterThan(0);
        Register("max_data_size_per_partition_job", MaxDataSizePerPartitionJob)
            .Default((i64) 1024 * 1024 * 1024)
            .GreaterThan(0);
        Register("partition_locality_timeout", PartitionLocalityTimeout)
            .Default(TDuration::Seconds(5));
        Register("sort_locality_timeout", SortLocalityTimeout)
            .Default(TDuration::Seconds(10));
        Register("merge_locality_timeout", MergeLocalityTimeout)
            .Default(TDuration::Seconds(10));

        PartitionJobIO->TableReader->PrefetchWindow = 10;
        PartitionJobIO->TableWriter->MaxBufferSize = (i64) 2 * 1024 * 1024 * 1024; // 2 GB

        SortJobIO->TableReader->PrefetchWindow = 10;
    }
};

////////////////////////////////////////////////////////////////////////////////

struct TMapReduceOperationSpec
    : public TSortOperationSpecBase
{
    std::vector<NYPath::TRichYPath> OutputTablePaths;

    std::vector<Stroka> SortBy;
    std::vector<Stroka> ReduceBy;

    TUserJobSpecPtr Mapper;
    TUserJobSpecPtr Reducer;

    TJobIOConfigPtr MapJobIO;
    TJobIOConfigPtr SortJobIO;
    TJobIOConfigPtr ReduceJobIO;

    TMapReduceOperationSpec()
    {
        Register("output_table_paths", OutputTablePaths);
        Register("sort_by", SortBy)
            .NonEmpty();
        Register("reduce_by", ReduceBy)
            .Default(std::vector<Stroka>());
        Register("mapper", Mapper)
            .Default();
        Register("reducer", Reducer);
        Register("map_job_io", MapJobIO)
            .DefaultNew();
        Register("sort_job_io", SortJobIO)
            .DefaultNew();
        Register("reduce_job_io", ReduceJobIO)
            .DefaultNew();

        // Provide custom names for shared settings.
        Register("map_job_count", PartitionJobCount)
            .Default()
            .GreaterThan(0);
        Register("map_job_slice_data_size", PartitionJobSliceDataSize)
            .Default((i64) 256 * 1024 * 1024)
            .GreaterThan(0);
        Register("min_data_size_per_map_job", MinDataSizePerPartitionJob)
            .Default((i64) 128 * 1024 * 1024)
            .GreaterThan(0);
        Register("max_data_size_per_map_job", MaxDataSizePerPartitionJob)
            .Default((i64) 1024 * 1024 * 1024)
            .GreaterThan(0);
        Register("map_locality_timeout", PartitionLocalityTimeout)
            .Default(TDuration::Seconds(5));

        // The following settings are inherited from base but make no sense for map-reduce:
        //   SortJobSliceDataSize
        //   MaxDataSizePerUnorderedMergeJob
        //   SortLocalityTimeout
        //   MergeLocalityTimeout

        MapJobIO->TableReader->PrefetchWindow = 10;
        MapJobIO->TableWriter->MaxBufferSize = (i64) 2 * 1024 * 1024 * 1024; // 2 GB

        SortJobIO->TableReader->PrefetchWindow = 10;
    }

    virtual void OnLoaded() override
    {
        if (ReduceBy.empty()) {
            ReduceBy = SortBy;
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

DECLARE_ENUM(ESchedulingMode,
    (Fifo)
    (FairShare)
);

struct TPoolConfig
    : public TYsonSerializable
{
    double Weight;
    double MinShareRatio;
    ESchedulingMode Mode;

    TPoolConfig()
    {
        Register("weight", Weight)
            .Default(1.0)
            .GreaterThanOrEqual(1.0);
        Register("min_share_ratio", MinShareRatio)
            .Default(0.0)
            .InRange(0.0, 1.0);
        Register("mode", Mode)
            .Default(ESchedulingMode::Fifo);
    }
};

////////////////////////////////////////////////////////////////////

struct TPooledOperationSpec
    : public TYsonSerializable
{
    TNullable<Stroka> Pool;
    double Weight;
    double MinShareRatio;

    TDuration MinSharePreemptionTimeout;
    TDuration FairSharePreemptionTimeout;

    TPooledOperationSpec()
    {
        Register("pool", Pool)
            .Default(TNullable<Stroka>())
            .NonEmpty();
        Register("weight", Weight)
            .Default(1.0)
            .GreaterThanOrEqual(1.0);
        Register("min_share_ratio", MinShareRatio)
            .Default(0.0)
            .InRange(0.0, 1.0);

        Register("min_share_preemption_timeout", MinSharePreemptionTimeout)
            .Default(TDuration::Seconds(15));
        Register("fair_share_preemption_timeout", FairSharePreemptionTimeout)
            .Default(TDuration::Seconds(30));
    }
};

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT
