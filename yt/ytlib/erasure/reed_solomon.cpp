#include "reed_solomon.h"
#include "helpers.h"
#include "jerasure.h"

#include <ytlib/misc/foreach.h>

#include <contrib/libs/jerasure/cauchy.h>
#include <contrib/libs/jerasure/jerasure.h>

#include <algorithm>

namespace NYT {
namespace NErasure {

////////////////////////////////////////////////////////////////////////////////

TCauchyReedSolomon::TCauchyReedSolomon(int blockCount, int parityCount, int wordSize)
    : DataPartCount_(blockCount)
    , ParityPartCount_(parityCount)
    , WordSize_(wordSize)
    , Matrix_(cauchy_good_general_coding_matrix(blockCount, parityCount, wordSize))
    , BitMatrix_(jerasure_matrix_to_bitmatrix(blockCount, parityCount, wordSize, Matrix_.Get()))
    , Schedule_(jerasure_smart_bitmatrix_to_schedule(blockCount, parityCount, wordSize, BitMatrix_.Get()))
{ }

std::vector<TSharedRef> TCauchyReedSolomon::Encode(const std::vector<TSharedRef>& blocks)
{
    return ScheduleEncode(DataPartCount_, ParityPartCount_, WordSize_, Schedule_, blocks);
}

std::vector<TSharedRef> TCauchyReedSolomon::Decode(
    const std::vector<TSharedRef>& blocks,
    const TPartIndexList& erasedIndices)
{
    if (erasedIndices.empty()) {
        return std::vector<TSharedRef>();
    }

    return BitMatrixDecode(DataPartCount_, ParityPartCount_, WordSize_, BitMatrix_, blocks, erasedIndices);
}

TNullable<TPartIndexList> TCauchyReedSolomon::GetRepairIndices(const TPartIndexList& erasedIndices)
{
    if (erasedIndices.empty()) {
        return Null;
    }

    TPartIndexList indices = erasedIndices;
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());

    if (indices.size() > ParityPartCount_) {
        return Null;
    }

    return Difference(0, DataPartCount_ + ParityPartCount_, indices);
}

bool TCauchyReedSolomon::CanRepair(const TPartIndexList& erasedIndices)
{
    return erasedIndices.size() <= ParityPartCount_;
}

bool TCauchyReedSolomon::CanRepair(const TPartIndexSet& erasedIndices)
{
    return erasedIndices.count() <= ParityPartCount_;
}

int TCauchyReedSolomon::GetDataPartCount()
{
    return DataPartCount_;
}

int TCauchyReedSolomon::GetParityPartCount()
{
    return ParityPartCount_;
}

int TCauchyReedSolomon::GetWordSize()
{
    return WordSize_ * 8;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NErasure
} // namespace NYT
