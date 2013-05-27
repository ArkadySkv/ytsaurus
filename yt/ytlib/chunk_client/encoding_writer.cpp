﻿#include "stdafx.h"
#include "encoding_writer.h"
#include "config.h"
#include "private.h"
#include "dispatcher.h"
#include "async_writer.h"

#include <ytlib/compression/codec.h>

namespace NYT {
namespace NChunkClient {

///////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = ChunkWriterLogger;

///////////////////////////////////////////////////////////////////////////////

TEncodingWriter::TEncodingWriter(
    TEncodingWriterConfigPtr config,
    TEncodingWriterOptionsPtr options,
    IAsyncWriterPtr asyncWriter)
    : UncompressedSize_(0)
    , CompressedSize_(0)
    , CompressionRatio_(config->DefaultCompressionRatio)
    , Config(config)
    , AsyncWriter(asyncWriter)
    , CompressionInvoker(CreateSerializedInvoker(TDispatcher::Get()->GetCompressionInvoker()))
    , Semaphore(Config->EncodeWindowSize)
    , Codec(NCompression::GetCodec(options->CompressionCodec))
    , WritePending(
        BIND(&TEncodingWriter::WritePendingBlocks, MakeWeak(this))
            .Via(CompressionInvoker))
{ }

void TEncodingWriter::WriteBlock(const TSharedRef& block)
{
    AtomicAdd(UncompressedSize_, block.Size());
    Semaphore.Acquire(block.Size());
    CompressionInvoker->Invoke(BIND(
        &TEncodingWriter::DoCompressBlock,
        MakeStrong(this),
        block));
}

void TEncodingWriter::WriteBlock(std::vector<TSharedRef>&& vectorizedBlock)
{
    FOREACH (const auto& part, vectorizedBlock) {
        Semaphore.Acquire(part.Size());
        AtomicAdd(UncompressedSize_, part.Size());
    }
    CompressionInvoker->Invoke(BIND(
        &TEncodingWriter::DoCompressVector,
        MakeWeak(this),
        std::move(vectorizedBlock)));
}

// Serialized compression invoker affinity (don't use thread affinity because of thread pool).
void TEncodingWriter::DoCompressBlock(const TSharedRef& block)
{
    LOG_DEBUG("Compressing block");

    auto compressedBlock = Codec->Compress(block);
    CompressedSize_ += compressedBlock.Size();

    int sizeToRelease = -compressedBlock.Size();

    if (!Config->VerifyCompression) {
        // We immediately release original data.
        sizeToRelease += block.Size();
    }

    ProcessCompressedBlock(compressedBlock, sizeToRelease);

    if (Config->VerifyCompression) {
        TDispatcher::Get()->GetCompressionInvoker()->Invoke(BIND(
            &TEncodingWriter::VerifyBlock,
            MakeWeak(this),
            block,
            compressedBlock));
    }
}

// Serialized compression invoker affinity (don't use thread affinity because of thread pool).
void TEncodingWriter::DoCompressVector(const std::vector<TSharedRef>& vectorizedBlock)
{
    LOG_DEBUG("Compressing block");

    auto compressedBlock = Codec->Compress(vectorizedBlock);
    AtomicAdd(CompressedSize_, compressedBlock.Size());

    i64 sizeToRelease = -static_cast<i64>(compressedBlock.Size());

    if (!Config->VerifyCompression) {
        // We immediately release original data.
        FOREACH (const auto& part, vectorizedBlock) {
            sizeToRelease += part.Size();
        }
    }

    ProcessCompressedBlock(compressedBlock, sizeToRelease);

    if (Config->VerifyCompression) {
        TDispatcher::Get()->GetCompressionInvoker()->Invoke(BIND(
            &TEncodingWriter::VerifyVector,
            MakeWeak(this),
            vectorizedBlock,
            compressedBlock));
    }
}

// Verification is run in thread pool without serialized invoker guard.
void TEncodingWriter::VerifyVector(
    const std::vector<TSharedRef>& origin,
    const TSharedRef& compressedBlock)
{
    auto decompressedBlock = Codec->Decompress(compressedBlock);

    char* begin = decompressedBlock.Begin();
    FOREACH (const auto& block, origin) {
        LOG_FATAL_IF(
            !TRef::AreBitwiseEqual(TRef(begin, block.Size()), block),
            "Compression verification failed");
        begin += block.Size();
        Semaphore.Release(block.Size());
    }
}

// Verification is run in compression thread pool without serialized invoker guard.
void TEncodingWriter::VerifyBlock(
    const TSharedRef& origin,
    const TSharedRef& compressedBlock)
{
    auto decompressedBlock = Codec->Decompress(compressedBlock);
    LOG_FATAL_IF(
        !TRef::AreBitwiseEqual(decompressedBlock, origin),
        "Compression verification failed");
    Semaphore.Release(origin.Size());
}

// Serialized compression invoker affinity (don't use thread affinity because of thread pool).
void TEncodingWriter::ProcessCompressedBlock(const TSharedRef& block, i64 sizeToRelease)
{
    SetCompressionRatio(
        double(AtomicGet(CompressedSize_)) /
        AtomicGet(UncompressedSize_));

    if (sizeToRelease > 0) {
        Semaphore.Release(sizeToRelease);
    } else {
        Semaphore.Acquire(-sizeToRelease);
    }

    PendingBlocks.push_back(block);
    LOG_DEBUG("Pending block added");

    if (PendingBlocks.size() == 1) {
        WritePending.Run(TError());
    }
}

// Serialized compression invoker affinity (don't use thread affinity because of thread pool).
void TEncodingWriter::WritePendingBlocks(TError error)
{
    if (!error.IsOK()) {
        State.Fail(error);
        return;
    }

    while (!PendingBlocks.empty()) {
        LOG_DEBUG("Writing pending block");
        auto& front = PendingBlocks.front();
        auto result = AsyncWriter->WriteBlock(front);
        Semaphore.Release(front.Size());
        PendingBlocks.pop_front();

        if (!result && !PendingBlocks.empty()) {
            AsyncWriter->GetReadyEvent().Subscribe(WritePending);
            return;
        }
    }
}

bool TEncodingWriter::IsReady() const
{
    return Semaphore.IsReady() && State.IsActive();
}

TAsyncError TEncodingWriter::GetReadyEvent()
{
    if (!Semaphore.IsReady()) {
        State.StartOperation();

        auto this_ = MakeStrong(this);
        Semaphore.GetReadyEvent().Subscribe(BIND([=] () {
            this_->State.FinishOperation();
        }));
    }

    return State.GetOperationError();
}

TAsyncError TEncodingWriter::AsyncFlush()
{
    State.StartOperation();

    auto this_ = MakeStrong(this);
    Semaphore.GetFreeEvent().Subscribe(BIND([=] () {
        this_->State.FinishOperation();
    }));

    return State.GetOperationError();
}

TEncodingWriter::~TEncodingWriter()
{ }

i64 TEncodingWriter::GetUncompressedSize() const
{
    return AtomicGet(UncompressedSize_);
}

i64 TEncodingWriter::GetCompressedSize() const
{
    // NB: #CompressedSize_ may have not been updated yet (updated in compression invoker).
    return static_cast<i64>(GetUncompressedSize() * GetCompressionRatio());
}

void TEncodingWriter::SetCompressionRatio(double value)
{
    TGuard<TSpinLock> guard(SpinLock);
    CompressionRatio_ = value;
}

double TEncodingWriter::GetCompressionRatio() const
{
    TGuard<TSpinLock> guard(SpinLock);
    return CompressionRatio_;
}


///////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
