#pragma once

#include "public.h"
#include "async_writer.h"
#include "format.h"

#include <ytlib/chunk_client/chunk.pb.h>
#include <ytlib/misc/checksum.h>

#include <util/system/file.h>

namespace NYT {
namespace NChunkClient {

///////////////////////////////////////////////////////////////////////////////

//! Provides a local and synchronous implementation of #IAsyncWriter.
class TFileWriter
    : public IAsyncWriter
{
public:
    explicit TFileWriter(const Stroka& fileName);

    virtual void Open();

    virtual bool TryWriteBlock(const TSharedRef& block);
    virtual TAsyncError GetReadyEvent();

    virtual TAsyncError AsyncClose(const NChunkClient::NProto::TChunkMeta& chunkMeta);

    void Abort();

    //! Returns chunk info. The writer must be already closed.
    const NChunkClient::NProto::TChunkInfo& GetChunkInfo() const;

    //! Returns chunk meta. The writer must be already closed.
    const NChunkClient::NProto::TChunkMeta& GetChunkMeta() const;

    i64 GetDataSize() const;

private:
    Stroka FileName;
    bool IsOpen;
    bool IsClosed;
    i64 DataSize;
    THolder<TFile> DataFile;
    NChunkClient::NProto::TChunkInfo ChunkInfo;
    NChunkClient::NProto::TBlocksExt BlocksExt;
    NChunkClient::NProto::TChunkMeta ChunkMeta;

    TChecksumOutput ChecksumOutput;

    TAsyncError Result;

    bool EnsureOpen();

};

///////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
