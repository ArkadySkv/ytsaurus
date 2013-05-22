#pragma once

#include "public.h"
#include <ytlib/chunk_client/schema.h>

#include <ytlib/chunk_client/public.h>
#include <ytlib/chunk_client/config.h>
#include <ytlib/ytree/yson_serializable.h>

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

class TChunkWriterConfig
    : public NChunkClient::TEncodingWriterConfig
{
public:
    i64 BlockSize;

    //! Fraction of rows data size samples are allowed to occupy.
    double SampleRate;

    //! Fraction of rows data size chunk index allowed to occupy.
    double IndexRate;

    double EstimatedCompressionRatio;

    bool AllowDuplicateColumnNames;

    i64 MaxBufferSize;

    TChunkWriterConfig()
    {
        // Block less than 1M is nonsense.
        RegisterParameter("block_size", BlockSize)
            .GreaterThanOrEqual((i64) 1024 * 1024)
            .Default((i64) 32 * 1024 * 1024);
        RegisterParameter("sample_rate", SampleRate)
            .GreaterThan(0)
            .LessThanOrEqual(0.001)
            .Default(0.0001);
        RegisterParameter("index_rate", IndexRate)
            .GreaterThan(0)
            .LessThanOrEqual(0.001)
            .Default(0.0001);
        RegisterParameter("estimated_compression_ratio", EstimatedCompressionRatio)
            .GreaterThan(0)
            .LessThan(1)
            .Default(0.2);
        RegisterParameter("allow_duplicate_column_names", AllowDuplicateColumnNames)
            .Default(true);
        RegisterParameter("max_buffer_size", MaxBufferSize)
            .GreaterThanOrEqual(1024 * 1024)
            .Default(32 * 1024 * 1024);
    }
};

////////////////////////////////////////////////////////////////////////////////

class TTableWriterConfig
    : public TChunkWriterConfig
    , public NChunkClient::TMultiChunkWriterConfig
{ };

////////////////////////////////////////////////////////////////////////////////

class TChunkWriterOptions
    : public virtual NChunkClient::TEncodingWriterOptions
{
public:
    TNullable<TKeyColumns> KeyColumns;
    NChunkClient::TChannels Channels;

    TChunkWriterOptions()
    {
        RegisterParameter("key_columns", KeyColumns)
            .Default(Null);
        RegisterParameter("channels", Channels)
            .Default(NChunkClient::TChannels());
    }
};

////////////////////////////////////////////////////////////////////////////////

class TTableWriterOptions
    : public NChunkClient::TMultiChunkWriterOptions
    , public TChunkWriterOptions
{
public:
    TTableWriterOptions()
    {
        CompressionCodec = NCompression::ECodec::Lz4;
    }
};

////////////////////////////////////////////////////////////////////////////////

class TChunkReaderOptions
    : public virtual TYsonSerializable
{
public:
    bool ReadKey;

    // If set, reader keeps all memory buffers valid until destruction.
    bool KeepBlocks;

    TChunkReaderOptions()
    {
        RegisterParameter("read_key", ReadKey)
            .Default(false);
        RegisterParameter("keep_blocks", KeepBlocks)
            .Default(false);
    }
};

////////////////////////////////////////////////////////////////////////////////

class TTableReaderConfig
    : public NChunkClient::TMultiChunkReaderConfig
{ };

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
