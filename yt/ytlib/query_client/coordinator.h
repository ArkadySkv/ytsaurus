#pragma once

#include "public.h"
#include "callbacks.h"
#include "plan_fragment.h"

#include <core/logging/tagged_logger.h>

namespace NYT {
namespace NQueryClient {

////////////////////////////////////////////////////////////////////////////////

class TCoordinator
    : public IEvaluateCallbacks
{
public:
    TCoordinator(
        ICoordinateCallbacks* callbacks,
        const TPlanFragment& fragment);

    ~TCoordinator();

    int GetPeerIndex(const TDataSplit& dataSplit);

    virtual ISchemafulReaderPtr GetReader(
        const TDataSplit& split,
        TPlanContextPtr context) override;

    //! Actually evaluates query.
    //! NB: Does not throw.
    TError Run();

    //! Returns a plan fragment to be evaluated by the coordinator.
    TPlanFragment GetCoordinatorFragment() const;

    //! Returns plan fragments to be evaluated by peers.
    std::vector<TPlanFragment> GetPeerFragments() const;

private:
    struct TDataSplitExplanation
    {
        bool IsInternal;
        bool IsEmpty;
        int PeerIndex;
    };

    struct TPeer
    {
        TPeer(
            TPlanFragment fragment,
            const TDataSplit& collocatedSplit,
            ISchemafulReaderPtr reader)
            : Fragment(std::move(fragment))
            , CollocatedSplit(collocatedSplit)
            , Reader(std::move(reader))
        { }

        TPeer(const TPeer&) = delete;
        TPeer(TPeer&&) = default;

        TPlanFragment Fragment;
        const TDataSplit& CollocatedSplit;
        ISchemafulReaderPtr Reader;
    };

private:
    std::vector<const TOperator*> Scatter(const TOperator* op);
    const TOperator* Gather(const std::vector<const TOperator*>& ops);

    const TOperator* Simplify(const TOperator*);

    TGroupedDataSplits SplitAndRegroup(
        const TDataSplits& splits,
        const TTableSchema& tableSchema,
        const TKeyColumns& keyColumns);

    TDataSplitExplanation Explain(const TDataSplit& split);

    void DelegateToPeers();

private:
    ICoordinateCallbacks* Callbacks_;
    TPlanFragment Fragment_;

    std::vector<TPeer> Peers_;

    NLog::TTaggedLogger Logger;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NQueryClient
} // namespace NYT

