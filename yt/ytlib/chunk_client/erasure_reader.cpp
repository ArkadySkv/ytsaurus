#include "erasure_reader.h"
#include "async_writer.h"
#include "async_reader.h"
#include "chunk_meta_extensions.h"
#include "dispatcher.h"

#include <ytlib/actions/parallel_awaiter.h>
#include <ytlib/actions/parallel_collector.h>

#include <ytlib/fibers/fiber.h>

#include <ytlib/erasure/codec.h>
#include <ytlib/erasure/helpers.h>

#include <numeric>

namespace NYT {
namespace NChunkClient {

using namespace NErasure;
using namespace NChunkClient::NProto;

///////////////////////////////////////////////////////////////////////////////

namespace {

IAsyncReader::TAsyncGetMetaResult AsyncGetPlacementMeta(IAsyncReaderPtr reader)
{
    std::vector<int> tags;
    tags.push_back(TProtoExtensionTag<TErasurePlacementExt>::Value);
    return reader->AsyncGetChunkMeta(Null, &tags);
}

} // anonymous namespace

///////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////
// Non-reparing reader

class TNonReparingReaderSession
    : public TRefCounted
{
public:
    TNonReparingReaderSession(
        const std::vector<IAsyncReaderPtr>& readers,
        const std::vector<TPartInfo>& partInfos,
        const std::vector<int>& blockIndexes)
            : Readers_(readers)
            , PartInfos_(partInfos)
            , BlockIndexes_(blockIndexes)
            , Result_(BlockIndexes_.size())
            , ResultPromise_(NewPromise<IAsyncReader::TReadResult>())
    { }


    IAsyncReader::TAsyncReadResult Run()
    {
        // For each reader we find blocks to read and their initial indices
        std::vector<
            std::pair<
                std::vector<int>, // indices of blocks in the part
                TPartIndexList   // indices of blocks in the requested blockIndexes
            > > BlockLocations_(Readers_.size());

        // Fill BlockLocations_ using information about blocks in parts
        int initialPosition = 0;
        FOREACH (int blockIndex, BlockIndexes_) {
            YCHECK(blockIndex >= 0);

            // Searching for the part of given block
            auto it = upper_bound(PartInfos_.begin(), PartInfos_.end(), blockIndex, TPartComparator());
            YCHECK(it != PartInfos_.begin());
            do {
                --it;
            } while (it != PartInfos_.begin() && (it->start() > blockIndex || it->block_sizes().size() == 0));

            YCHECK(it != PartInfos_.end());
            int readerIndex = it - PartInfos_.begin();

            YCHECK(blockIndex >= it->start());
            int blockInPartIndex = blockIndex - it->start();

            YCHECK(blockInPartIndex < it->block_sizes().size());
            BlockLocations_[readerIndex].first.push_back(blockInPartIndex);
            BlockLocations_[readerIndex].second.push_back(initialPosition++);
        }

        auto this_ = MakeStrong(this);
        auto awaiter = New<TParallelAwaiter>(TDispatcher::Get()->GetReaderInvoker());
        for (int readerIndex = 0; readerIndex < Readers_.size(); ++readerIndex) {
            auto reader = Readers_[readerIndex];
            awaiter->Await(
                reader->AsyncReadBlocks(BlockLocations_[readerIndex].first),
                BIND(
                    &TNonReparingReaderSession::OnBlocksRead,
                    this_,
                    BlockLocations_[readerIndex].second));
        }

        awaiter->Complete(BIND(&TThis::OnComplete, this_));

        return ResultPromise_;
    }

    void OnBlocksRead(const TPartIndexList& indicesInPart, IAsyncReader::TReadResult readResult)
    {
        if (readResult.IsOK()) {
            auto dataRefs = readResult.GetValue();
            for (int i = 0; i < dataRefs.size(); ++i) {
                Result_[indicesInPart[i]] = dataRefs[i];
            }
        } else {
            TGuard<TSpinLock> guard(AddReadErrorLock_);
            ReadErrors_.push_back(readResult);
        }
    }

    void OnComplete()
    {
        if (ReadErrors_.empty()) {
            ResultPromise_.Set(Result_);
        } else {
            auto error = TError("Error reading erasure chunk");
            error.InnerErrors() = ReadErrors_;
            ResultPromise_.Set(error);
        }
    }

private:
    typedef TNonReparingReaderSession TThis;

    struct TPartComparator
    {
        bool operator()(int position, const TPartInfo& info) const
        {
            return position < info.start();
        }
    };

    std::vector<IAsyncReaderPtr> Readers_;
    std::vector<TPartInfo> PartInfos_;

    std::vector<int> BlockIndexes_;

    std::vector<TSharedRef> Result_;
    IAsyncReader::TAsyncReadPromise ResultPromise_;

    TSpinLock AddReadErrorLock_;
    std::vector<TError> ReadErrors_;
};

///////////////////////////////////////////////////////////////////////////////

class TNonReparingReader
    : public IAsyncReader
{
public:
    explicit TNonReparingReader(const std::vector<IAsyncReaderPtr>& readers)
        : Readers_(readers)
    {
        YCHECK(!Readers_.empty());
    }

    virtual TAsyncReadResult AsyncReadBlocks(const std::vector<int>& blockIndexes) override
    {
        auto this_ = MakeStrong(this);
        return PreparePartInfos().Apply(
            BIND([this, this_, blockIndexes] (TError error) -> TAsyncReadResult {
                RETURN_FUTURE_IF_ERROR(error, TReadResult);
                return New<TNonReparingReaderSession>(Readers_, PartInfos_, blockIndexes)->Run();
            }));
    }

    virtual TAsyncGetMetaResult AsyncGetChunkMeta(
        const TNullable<int>& partitionTag = Null,
        const std::vector<int>* tags = nullptr) override
    {
        // TODO(ignat): check that no storage-layer extensions are being requested
        YCHECK(!partitionTag);
        return Readers_.front()->AsyncGetChunkMeta(partitionTag, tags);
    }

    virtual TChunkId GetChunkId() const override
    {
        return Readers_.front()->GetChunkId();
    }

private:
    std::vector<IAsyncReaderPtr> Readers_;
    std::vector<TPartInfo> PartInfos_;

    TAsyncError PreparePartInfos()
    {
        if (!PartInfos_.empty()) {
            return MakePromise(TError());
        }

        auto this_ = MakeStrong(this);
        return AsyncGetPlacementMeta(this).Apply(
            BIND([this, this_] (IAsyncReader::TGetMetaResult metaOrError) -> TError {
                RETURN_IF_ERROR(metaOrError);

                auto extension = GetProtoExtension<TErasurePlacementExt>(metaOrError.GetValue().extensions());
                PartInfos_ = std::vector<TPartInfo>(extension.part_infos().begin(), extension.part_infos().end());

                // Check that part infos are correct.
                YCHECK(PartInfos_.front().start() == 0);
                for (int i = 0; i + 1 < PartInfos_.size(); ++i) {
                    YCHECK(PartInfos_[i].start() + PartInfos_[i].block_sizes().size() == PartInfos_[i + 1].start());
                }

                return TError();
            })
        );
    }
};

///////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////
// Repairing readers

//! Asynchronously reads data by window of size windowSize.
//! It is guaranteed that each original block will be read only once.
class TWindowReader
    : public TRefCounted
{
public:
    typedef TErrorOr<TSharedRef> TReadResult;
    typedef TPromise<TReadResult> TReadPromise;
    typedef TFuture<TReadResult> TReadFuture;

    TWindowReader(
        IAsyncReaderPtr reader,
        int blockCount,
        IInvokerPtr controlInvoker)
            : Reader_(reader)
            , BlockCount_(blockCount)
            , ControlInvoker_(controlInvoker)
            , BlockIndex_(0)
            , BlocksDataSize_(0)
            , BuildDataSize_(0)
            , FirstBlockOffset_(0)
    { }

    TReadFuture Read(i64 windowSize)
    {
        if (BlockIndex_ < BlockCount_ &&  BlocksDataSize_ < BuildDataSize_ + windowSize) {
            // Read one more block if necessary.
            auto blocksToRead = std::vector<int>(1, BlockIndex_);
            auto this_ = MakeStrong(this);
            return Reader_->AsyncReadBlocks(blocksToRead).Apply(
                BIND(&TWindowReader::OnBlockRead, this_, windowSize)
                    .AsyncVia(ControlInvoker_));
        } else {
            // We have enough blocks to build the window.
            return MakePromise(TReadResult(BuildWindow(windowSize)));
        }
    }

private:
    TReadFuture OnBlockRead(i64 windowSize, IAsyncReader::TReadResult readResult)
    {
        RETURN_FUTURE_IF_ERROR(readResult, TReadResult);

        YCHECK(readResult.GetValue().size() == 1);
        auto block = readResult.GetValue().front();

        BlockIndex_ += 1;
        Blocks_.push_back(block);
        BlocksDataSize_ += block.Size();
        return Read(windowSize);
    }

    TSharedRef BuildWindow(i64 windowSize)
    {
        // Allocate the resulting window filling it with zeros (used as padding).
        struct TRepairWindowTag { };
        auto result = TSharedRef::Allocate<TRepairWindowTag>(windowSize, true);

        i64 resultPosition = 0;
        while (!Blocks_.empty()) {
            auto block = Blocks_.front();

            // Begin and end inside of current block

            i64 beginIndex = FirstBlockOffset_;
            i64 endIndex = std::min(beginIndex + windowSize - resultPosition, (i64)block.Size());
            i64 size = endIndex - beginIndex;

            std::copy(block.Begin() + beginIndex, block.Begin() + endIndex, result.Begin() + resultPosition);
            resultPosition += size;

            FirstBlockOffset_ += size;
            if (endIndex == block.Size()) {
                Blocks_.pop_front();
                FirstBlockOffset_ = 0;
            } else {
                break;
            }
        }
        BuildDataSize_ += windowSize;

        return result;
    }

    IAsyncReaderPtr Reader_;
    int BlockCount_;
    IInvokerPtr ControlInvoker_;

    std::deque<TSharedRef> Blocks_;

    // Current number of read blocks.
    int BlockIndex_;

    //! Total blocks data size.
    i64 BlocksDataSize_;

    //! Total size of data returned from |Read|
    i64 BuildDataSize_;

    //! Offset of used data in the first block.
    i64 FirstBlockOffset_;

};

typedef TIntrusivePtr<TWindowReader> TWindowReaderPtr;

//! Does the job reverse to that of TWindowReader.
//! Consumes windows and returns blocks of the current part that
//! can be reconstructed.
class TRepairPartReader
{
public:
    explicit TRepairPartReader(const std::vector<i64>& blockSizes)
        : BlockIndex_(0)
        , BlockSizes_(blockSizes)
    {
        if (!BlockSizes_.empty()) {
            PrepareNextBlock();
        }
    }

    std::vector<TSharedRef> Add(const TSharedRef& window)
    {
        std::vector<TSharedRef> result;

        i64 offset = 0;
        while (offset < window.Size() && BlockIndex_ < BlockSizes_.size()) {
            i64 size = std::min(window.Size() - offset, CurrentBlock_.Size() - CompletedOffset_);
            std::copy(
                window.Begin() + offset,
                window.Begin() + offset + size,
                CurrentBlock_.Begin() + CompletedOffset_);

            offset += size;
            CompletedOffset_ += size;
            if (CompletedOffset_ == CurrentBlock_.Size()) {
                result.push_back(CurrentBlock_);
                BlockIndex_ += 1;
                if (BlockIndex_ < BlockSizes_.size()) {
                    PrepareNextBlock();
                }
            }
        }

        return result;
    }

private:
    void PrepareNextBlock()
    {
        CompletedOffset_ = 0;

        struct TRepairBlockTag { };
        CurrentBlock_ = TSharedRef::Allocate<TRepairBlockTag>(BlockSizes_[BlockIndex_]);
    }

    int BlockIndex_;
    std::vector<i64> BlockSizes_;

    TSharedRef CurrentBlock_;
    i64 CompletedOffset_;

};

///////////////////////////////////////////////////////////////////////////////

// This reader asynchronously repairs blocks of given parts.
// It is designed to minimize memory consumption.
//
// We store repaired blocks queue. When RepairNextBlock() is called,
// we first check the queue, if it isn't empty then we extract the block. Otherwise
// we read window from each part, repair windows of erased parts and add it
// to blocks and add it to RepairPartReaders. All blocks that can be
// reconstructed we add to queue.
class TRepairReader
    : public TRefCounted
{
public:
    struct TBlock
    {
        TBlock()
            : Index(-1)
        { }

        TBlock(TSharedRef data, int index)
            : Data(data)
            , Index(index)
        { }

        TSharedRef Data;
        int Index;
    };

    typedef TErrorOr<TBlock> TReadResult;
    typedef TPromise< TErrorOr<TBlock> > TReadPromise;
    typedef TFuture< TErrorOr<TBlock> > TReadFuture;

    TRepairReader(
        NErasure::ICodec* codec,
        const std::vector<IAsyncReaderPtr>& readers,
        const TPartIndexList& erasedIndices,
        const TPartIndexList& repairIndices,
        IInvokerPtr controlInvoker)
        : Codec_(codec)
        , Readers_(readers)
        , ErasedIndices_(erasedIndices)
        , RepairIndices_(repairIndices)
        , Prepared_(false)
        , WindowIndex_(0)
        , ErasedDataSize_(0)
        , ErasedBlockCount_(0)
        , RepairedBlockCount_(0)
        , ControlInvoker_(controlInvoker)
    {
        YCHECK(Codec_->GetRepairIndices(ErasedIndices_));
        YCHECK(Codec_->GetRepairIndices(ErasedIndices_)->size() == Readers_.size());
        YCHECK(ControlInvoker_);
    }

    TAsyncError Prepare();

    bool HasNextBlock() const
    {
        YCHECK(Prepared_);
        return RepairedBlockCount_ < ErasedBlockCount_;
    }

    TReadFuture RepairNextBlock();

    i64 GetErasedDataSize() const;

private:
    NErasure::ICodec* Codec_;
    std::vector<IAsyncReaderPtr> Readers_;

    TPartIndexList ErasedIndices_;
    TPartIndexList RepairIndices_;

    std::vector<TWindowReaderPtr> WindowReaders_;
    std::vector<TRepairPartReader> RepairBlockReaders_;

    std::deque<TBlock> RepairedBlocksQueue_;

    bool Prepared_;

    int WindowIndex_;
    int WindowCount_;
    i64 WindowSize_;
    i64 LastWindowSize_;

    i64 ErasedDataSize_;

    int ErasedBlockCount_;
    int RepairedBlockCount_;

    IInvokerPtr ControlInvoker_;

    TAsyncError RepairIfNeeded();
    TAsyncError OnBlocksCollected(TErrorOr<std::vector<TSharedRef>> result);
    TAsyncError Repair(const std::vector<TSharedRef>& aliveWindows);
    TError OnGotMeta(IAsyncReader::TGetMetaResult metaOrError);

};

typedef TIntrusivePtr<TRepairReader> TRepairReaderPtr;

///////////////////////////////////////////////////////////////////////////////

TRepairReader::TReadFuture TRepairReader::RepairNextBlock()
{
    YCHECK(Prepared_);
    YCHECK(HasNextBlock());

    auto this_ = MakeStrong(this);
    return RepairIfNeeded()
        .Apply(BIND([this, this_] (TError error) -> TReadFuture {
            RETURN_FUTURE_IF_ERROR(error, TReadResult);

            YCHECK(!RepairedBlocksQueue_.empty());
            auto result = TRepairReader::TReadResult(RepairedBlocksQueue_.front());
            RepairedBlocksQueue_.pop_front();
            RepairedBlockCount_ += 1;
            return MakePromise(result);
        }).AsyncVia(ControlInvoker_));
}

TAsyncError TRepairReader::Repair(const std::vector<TSharedRef>& aliveWindows)
{
    auto repairedWindows = Codec_->Decode(aliveWindows, ErasedIndices_);
    YCHECK(repairedWindows.size() == ErasedIndices_.size());
    for (int i = 0; i < repairedWindows.size(); ++i) {
        auto repairedWindow = repairedWindows[i];
        FOREACH (auto block, RepairBlockReaders_[i].Add(repairedWindow)) {
            RepairedBlocksQueue_.push_back(TBlock(block, ErasedIndices_[i]));
        }
    }

    if (RepairedBlocksQueue_.empty()) {
        return RepairIfNeeded();
    } else {
        return MakePromise(TError());
    }
}

TAsyncError TRepairReader::OnBlocksCollected(TErrorOr<std::vector<TSharedRef>> result)
{
    RETURN_FUTURE_IF_ERROR(result, TError);

    return BIND(&TRepairReader::Repair, MakeStrong(this), result.GetValue())
        .AsyncVia(TDispatcher::Get()->GetErasureInvoker()).Run();
}

TAsyncError TRepairReader::RepairIfNeeded()
{
    YCHECK(HasNextBlock());

    if (!RepairedBlocksQueue_.empty()) {
        return MakeFuture(TError());
    }

    WindowIndex_ += 1;
    i64 windowSize = (WindowIndex_ == WindowCount_) ? LastWindowSize_ : WindowSize_;

    auto collector = New<TParallelCollector<TSharedRef>>();
    FOREACH (auto windowReader, WindowReaders_) {
        collector->Collect(windowReader->Read(windowSize));
    }

    return collector->Complete().Apply(
            BIND(&TRepairReader::OnBlocksCollected, MakeStrong(this))
                .AsyncVia(ControlInvoker_));
}

TError TRepairReader::OnGotMeta(IAsyncReader::TGetMetaResult metaOrError)
{
    RETURN_IF_ERROR(metaOrError);
    auto placementExt = GetProtoExtension<TErasurePlacementExt>(
        metaOrError.GetValue().extensions());

    WindowCount_ = placementExt.parity_block_count();
    WindowSize_ = placementExt.parity_block_size();
    LastWindowSize_ = placementExt.parity_last_block_size();
    
    auto recoveryIndices = Codec_->GetRepairIndices(ErasedIndices_);
    YCHECK(recoveryIndices);
    YCHECK(recoveryIndices->size() == Readers_.size());

    for (int i = 0; i < Readers_.size(); ++i) {
        int recoveryIndex = (*recoveryIndices)[i];
        int blockCount =
            recoveryIndex < Codec_->GetDataPartCount()
            ? placementExt.part_infos().Get(recoveryIndex).block_sizes().size()
            : placementExt.parity_block_count();

        WindowReaders_.push_back(New<TWindowReader>(
            Readers_[i],
            blockCount,
            ControlInvoker_));
    }

    FOREACH (int erasedIndex, ErasedIndices_) {
        std::vector<i64> blockSizes;
        if (erasedIndex < Codec_->GetDataPartCount()) {
            blockSizes = std::vector<i64>(
                placementExt.part_infos().Get(erasedIndex).block_sizes().begin(),
                placementExt.part_infos().Get(erasedIndex).block_sizes().end());
        } else {
            blockSizes = std::vector<i64>(
                placementExt.parity_block_count(),
                placementExt.parity_block_size());
            blockSizes.back() = placementExt.parity_last_block_size();
        }
        ErasedBlockCount_ += blockSizes.size();
        ErasedDataSize_ += std::accumulate(blockSizes.begin(), blockSizes.end(), 0);
        RepairBlockReaders_.push_back(TRepairPartReader(blockSizes));
    }

    Prepared_ = true;
    return TError();
}

TAsyncError TRepairReader::Prepare()
{
    YCHECK(!Prepared_);
    YCHECK(!Readers_.empty());

    auto reader = Readers_.front();
    return AsyncGetPlacementMeta(reader).Apply(
        BIND(&TRepairReader::OnGotMeta, MakeStrong(this))
            .AsyncVia(ControlInvoker_));
}

i64 TRepairReader::GetErasedDataSize() const
{
    YCHECK(Prepared_);
    return ErasedDataSize_;
}

///////////////////////////////////////////////////////////////////////////////
// Repair reader of all parts

class TRepairAllPartsSession
    : public TRefCounted
{
public:
    TRepairAllPartsSession(
        NErasure::ICodec* codec,
        const TPartIndexList& erasedIndices,
        const std::vector<IAsyncReaderPtr>& readers,
        const std::vector<IAsyncWriterPtr>& writers,
        TCallback<void(double)> onProgress,
        IInvokerPtr controlInvoker)
        : Reader_(New<TRepairReader>(
            codec,
            readers,
            erasedIndices,
            erasedIndices,
            controlInvoker))
        , Readers_(readers)
        , Writers_(writers)
        , OnProgress_(onProgress)
        , RepairedDataSize_(0)
        , ControlInvoker_(controlInvoker)
    {
        YCHECK(erasedIndices.size() == writers.size());

        for (int i = 0; i < erasedIndices.size(); ++i) {
            IndexToWriter_[erasedIndices[i]] = writers[i];
        }
    }

    TError Run()
    {
        try {
            // Prepare reader.
            {
                auto result = WaitFor(Reader_->Prepare());
                THROW_ERROR_EXCEPTION_IF_FAILED(result);
            }
            
            // Open writers.
            FOREACH (auto writer, Writers_) {
                writer->Open();
            }

            // Repair all blocks with the help of TRepairReader and push them to the
            // corresponding writers.
            while (Reader_->HasNextBlock()) {
                auto blockOrError = WaitFor(Reader_->RepairNextBlock());
                THROW_ERROR_EXCEPTION_IF_FAILED(blockOrError);

                const auto& block = blockOrError.GetValue();
                RepairedDataSize_ += block.Data.Size();

                if (!OnProgress_.IsNull()) {
                    double progress = static_cast<double>(RepairedDataSize_) / Reader_->GetErasedDataSize();
                    OnProgress_.Run(progress);
                }

                auto writer = GetWriterForIndex(block.Index);
                if (!writer->WriteBlock(block.Data)) {
                    auto result = WaitFor(writer->GetReadyEvent());
                    THROW_ERROR_EXCEPTION_IF_FAILED(result);
                }

            }

            // Fetch chunk meta.
            TChunkMeta meta;
            {
                auto reader = Readers_.front(); // an arbitrary one will do
                auto metaOrError = WaitFor(reader->AsyncGetChunkMeta());
                THROW_ERROR_EXCEPTION_IF_FAILED(metaOrError);
                meta = metaOrError.GetValue();
            }

            // Close all writers.
            {
                auto collector = New<TParallelCollector<void>>();
                FOREACH (auto writer, Writers_) {
                    collector->Collect(writer->AsyncClose(meta));
                }
                auto result = WaitFor(collector->Complete());
                THROW_ERROR_EXCEPTION_IF_FAILED(result);
            }

            return TError();
        } catch (const std::exception& ex) {
            return ex;
        }
    }

private:
    IAsyncWriterPtr GetWriterForIndex(int index)
    {
        auto it = IndexToWriter_.find(index);
        YCHECK(it != IndexToWriter_.end());
        return it->second;
    }


    TRepairReaderPtr Reader_;
    std::vector<IAsyncReaderPtr> Readers_;
    std::vector<IAsyncWriterPtr> Writers_;
    yhash_map<int, IAsyncWriterPtr> IndexToWriter_;

    TCallback<void(double)> OnProgress_;
    i64 RepairedDataSize_;

    IInvokerPtr ControlInvoker_;

};

///////////////////////////////////////////////////////////////////////////////

IAsyncReaderPtr CreateNonReparingErasureReader(
    const std::vector<IAsyncReaderPtr>& dataBlockReaders)
{
    return New<TNonReparingReader>(dataBlockReaders);
}

TAsyncError RepairErasedBlocks(
    NErasure::ICodec* codec,
    const TPartIndexList& erasedIndices,
    const std::vector<IAsyncReaderPtr>& readers,
    const std::vector<IAsyncWriterPtr>& writers,
    TCancelableContextPtr cancelableContext,
    TCallback<void(double)> onProgress)
{
    if (erasedIndices.empty()) {
        YCHECK(readers.empty());
        YCHECK(writers.empty());
        return MakeFuture(TError());
    }

    auto invoker = TDispatcher::Get()->GetReaderInvoker();
    if (cancelableContext) {
        invoker = cancelableContext->CreateInvoker(invoker);
    }
    
    auto session = New<TRepairAllPartsSession>(
        codec,
        erasedIndices,
        readers,
        writers,
        onProgress,
        invoker);
    return BIND(&TRepairAllPartsSession::Run, session)
        .AsyncVia(invoker)
        .Run();
}

///////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT

