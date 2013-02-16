﻿#pragma once

#include <ytlib/misc/ref.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

DECLARE_ENUM(ECodec,
    ((None)(0))
    ((Snappy)(1))
    ((GzipNormal)(2))
    ((GzipBestCompression)(3))
    ((Lz4)(4))
    ((Lz4HighCompression)(5))
    ((QuickLz)(6))
);


////////////////////////////////////////////////////////////////////////////////

//! A generic interface for compression/decompression.
struct ICodec
{
    //! Compress a given block.
    virtual TSharedRef Compress(const TSharedRef& block) = 0;

    //! Compress a vector of blocks.
    virtual TSharedRef Compress(const std::vector<TSharedRef>& blocks) = 0;

    //! Decompress a given block.
    virtual TSharedRef Decompress(const TSharedRef& block) = 0;

    virtual ECodec GetId() const = 0;
};


//! Returns a codec for the registered id.
ICodec* GetCodec(ECodec id);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

