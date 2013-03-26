#pragma once

#include "codec.h"
#include "jerasure.h"

namespace NYT {
namespace NErasure {

////////////////////////////////////////////////////////////////////////////////

//! Cauchy version of the standard Reed--Solomon encoding scheme.
/*!
 *  See http://en.wikipedia.org/wiki/Reed%E2%80%93Solomon_error_correction
 *  for more details.
 */
class TCauchyReedSolomon
    : public ICodec
{
public:
    TCauchyReedSolomon(int blockCount, int parityCount, int wordSize);

    virtual std::vector<TSharedRef> Encode(const std::vector<TSharedRef>& blocks) override;

    virtual std::vector<TSharedRef> Decode(
        const std::vector<TSharedRef>& blocks,
        const TBlockIndexList& erasedIndices) override;

    virtual bool CanRepair(const TBlockIndexList& erasedIndices) override;

    virtual TNullable<TBlockIndexList> GetRepairIndices(const TBlockIndexList& erasedIndices) override;

    virtual int GetDataBlockCount() override;

    virtual int GetParityBlockCount() override;

    virtual int GetWordSize() override;

private:
    int BlockCount_;
    int ParityCount_;
    int WordSize_;

    TMatrix Matrix_;
    TMatrix BitMatrix_;
    TSchedule Schedule_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NErasure
} // namespace NYT
