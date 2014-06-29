#pragma once

#include "public.h"

#include <core/misc/common.h>
#include <core/misc/ref.h>
#include <core/misc/error.h>

#include <core/actions/future.h>

#include <ytlib/chunk_client/public.h>
#include <ytlib/chunk_client/chunk_meta.pb.h>

namespace NYT {
namespace NChunkClient {

///////////////////////////////////////////////////////////////////////////////

//! A basic interface for readings chunks from a suitable source.
struct IReader
    : public virtual TRefCounted
{
    typedef TErrorOr<std::vector<TSharedRef>> TReadBlocksResult;
    typedef TFuture<TReadBlocksResult> TAsyncReadBlocksResult;

    typedef TErrorOr<NChunkClient::NProto::TChunkMeta> TGetMetaResult;
    typedef TFuture<TGetMetaResult> TAsyncGetMetaResult;

    //! Asynchronously reads a given set of blocks.
    //! Returns a collection of blocks, each corresponding to a single given index.
    virtual TAsyncReadBlocksResult ReadBlocks(const std::vector<int>& blockIndexes) = 0;

    //! Asynchronously reads a given range of blocks.
    //! The call may return less blocks than requested.
    //! If an empty list of blocks is returned then there are no blocks in the given range.
    virtual TAsyncReadBlocksResult ReadBlocks(int firstBlockIndex, int blockCount) = 0;

    //! Asynchronously obtains a meta, possibly filtered by #partitionTag and #extensionTags.
    virtual TAsyncGetMetaResult GetMeta(
        const TNullable<int>& partitionTag = Null,
        const std::vector<int>* extensionTags = nullptr) = 0;

    virtual TChunkId GetChunkId() const = 0;
};

DEFINE_REFCOUNTED_TYPE(IReader)

///////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
