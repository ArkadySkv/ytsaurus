#pragma once

#include "blob.h"
#include "ref.h"

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

template<class TChunkType>
TChunkType GetChunkMask(int bitIndex, bool value) 
{
     return static_cast<TChunkType>(value) << (bitIndex % sizeof(TChunkType));
}

////////////////////////////////////////////////////////////////////////////////

template<class TChunkType>
class TAppendOnlyBitMap
{
public:
    explicit TAppendOnlyBitMap(int bitCapacity = 0)
        : BitSize_(0)
    {
        YCHECK(bitCapacity >= 0);
        if (bitCapacity) {
            Data_.reserve((bitCapacity - 1) / sizeof(TChunkType) + 1);
        }
    }

    void Append(bool value)
    {
        if (Data_.size() * sizeof(TChunkType) == BitSize_) {
            Data_.push_back(TChunkType());
        }

        Data_.back() |= GetChunkMask<TChunkType>(BitSize_, value);
        ++BitSize_;
    }

    TSharedRef Flush()
    {
        TBlob blob(Data_.data(), Size());
        return TSharedRef::FromBlob(std::move(blob));
    }

    int Size() const
    {
        return Data_.size() * sizeof(TChunkType);
    }

private:
    int BitSize_;
    std::vector<TChunkType> Data_;

};

////////////////////////////////////////////////////////////////////////////////

template<class TChunkType>
class TReadOnlyBitMap
{
public:
    TReadOnlyBitMap()
        : Data_(nullptr)
        , BitSize_(0)
    { }

    TReadOnlyBitMap(const TChunkType* data, int bitSize)
    {
        Reset(data, bitSize);
    }

    void Reset(const TChunkType* data, int bitSize)
    {
        YCHECK(data);
        YCHECK(bitSize >= 0);
        Data_ = data;
        BitSize_ = bitSize;
    }

    bool operator[] (int index) const
    {
        YCHECK(index < BitSize_);
        int dataIndex = index / sizeof(TChunkType);
        return static_cast<bool>(Data_[dataIndex] & GetChunkMask<TChunkType>(index, true));
    }

    int GetByteSize() const
    {
        return BitSize_ / 8 + (BitSize_ % 8 ? 1 : 0) ;
    }

private:
    TChunkType* Data_;
    int BitSize_;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
