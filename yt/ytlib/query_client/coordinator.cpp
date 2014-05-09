#include "stdafx.h"
#include "coordinator.h"

#include "private.h"
#include "helpers.h"

#include "plan_node.h"
#include "plan_visitor.h"
#include "plan_helpers.h"

#include "graphviz.h"

#include <core/concurrency/scheduler.h>

#include <core/profiling/scoped_timer.h>

#include <core/misc/protobuf_helpers.h>

#include <core/tracing/trace_context.h>

#include <ytlib/chunk_client/chunk_replica.h>

#include <ytlib/new_table_client/schemaful_reader.h>
#include <ytlib/new_table_client/writer.h>
#include <ytlib/new_table_client/schema.h>
#include <ytlib/new_table_client/unversioned_row.h>

#include <ytlib/object_client/helpers.h>

namespace NYT {
namespace NQueryClient {

using namespace NConcurrency;
using namespace NObjectClient;
using namespace NVersionedTableClient;

////////////////////////////////////////////////////////////////////////////////

static const int MaxCounter = std::numeric_limits<int>::max();

class TEmptySchemafulReader
    : public ISchemafulReader
{
    virtual TAsyncError Open(const TTableSchema& /*schema*/) override
    {
        return MakeFuture(TError());
    }

    virtual bool Read(std::vector<TUnversionedRow>* /*rows*/) override
    {
        return false;
    }

    virtual TAsyncError GetReadyEvent() override
    {
        return MakeFuture(TError());
    }
};

////////////////////////////////////////////////////////////////////////////////

TCoordinator::TCoordinator(
    ICoordinateCallbacks* callbacks,
    const TPlanFragment& fragment)
    : Callbacks_(callbacks)
    , Fragment_(fragment)
    , Logger(QueryClientLogger)
{
    Logger.AddTag(Sprintf(
        "FragmendId: %s",
        ~ToString(Fragment_.Id())));
}

TCoordinator::~TCoordinator()
{ }

TError TCoordinator::Run()
{
    TRACE_CHILD("QueryClient", "Coordinate") {
        TRACE_ANNOTATION("fragment_id", Fragment_.Id());
        QueryStat = TQueryStatistics();
        TDuration wallTime;

        try {
            LOG_DEBUG("Coordinating plan fragment");
            NProfiling::TAggregatingTimingGuard timingGuard(&wallTime);

            // Infer key range and push it down.
            auto keyRange = Fragment_.GetHead()->GetKeyRange();
            auto keyRangeFormatter = [] (const TKeyRange& range) -> Stroka {
                return Sprintf("[%s .. %s]",
                    ~ToString(range.first),
                    ~ToString(range.second));
            };
            Fragment_.Rewrite([&] (TPlanContext* context, const TOperator* op) -> const TOperator* {
                if (auto* scanOp = op->As<TScanOperator>()) {
                    auto* clonedScanOp = scanOp->Clone(context)->As<TScanOperator>();
                    for (auto& split : clonedScanOp->DataSplits()) {
                        auto originalRange = GetBothBoundsFromDataSplit(split);
                        auto intersectedRange = Intersect(originalRange, keyRange);
                        LOG_DEBUG("Narrowing split %s key range from %s to %s",
                                ~ToString(GetObjectIdFromDataSplit(split)),
                                ~keyRangeFormatter(originalRange),
                                ~keyRangeFormatter(intersectedRange));
                        SetBothBounds(&split, intersectedRange);
                    }
                    return clonedScanOp;
                }
                return op;
            });

            // Now build and distribute fragments.
            Fragment_ = TPlanFragment(
                Fragment_.GetContext(),
                Simplify(Gather(Scatter(Fragment_.GetHead()))));

            DelegateToPeers();

            QueryStat.SyncTime = wallTime - QueryStat.AsyncTime;
        } catch (const std::exception& ex) {
            auto error = TError("Failed to coordinate query fragment") << ex;
            LOG_ERROR(error);
            return error;
        }

        return TError();
    }
}

TPlanFragment TCoordinator::GetCoordinatorFragment() const
{
    return Fragment_;
}

std::vector<TPlanFragment> TCoordinator::GetPeerFragments() const
{
    std::vector<TPlanFragment> result;
    result.reserve(Peers_.size());
    for (const auto& peer : Peers_) {
        result.emplace_back(peer.Fragment);
    }
    return result;
}

TQueryStatistics TCoordinator::GetStatistics() const
{
    TQueryStatistics result;

    for (const auto& peer : Peers_) {
        TQueryStatistics subResult = peer.QueryResult.Get().Value();

        result.RowsRead += subResult.RowsRead;
        result.RowsWritten += subResult.RowsWritten;
        result.SyncTime += subResult.SyncTime;
        result.AsyncTime += subResult.AsyncTime;
        result.Incomplete |= subResult.Incomplete;
    }

    result.SyncTime += QueryStat.SyncTime;
    result.AsyncTime += QueryStat.AsyncTime;    

    return result;
}

std::vector<const TOperator*> TCoordinator::Scatter(const TOperator* op)
{
    auto* context = Fragment_.GetContext().Get();
    std::vector<const TOperator*> resultOps;

    switch (op->GetKind()) {

        case EOperatorKind::Scan: {
            auto* scanOp = op->As<TScanOperator>();
            auto groupedSplits = SplitAndRegroup(
                scanOp->DataSplits(),
                scanOp->GetTableSchema(),
                scanOp->GetKeyColumns());

            for (const auto& splits : groupedSplits) {
                auto* newScanOp = scanOp->Clone(context)->As<TScanOperator>();
                newScanOp->DataSplits() = splits;
                resultOps.push_back(newScanOp);
            }

            break;
        }

        case EOperatorKind::Filter: {
            auto* filterOp = op->As<TFilterOperator>();

            resultOps = Scatter(filterOp->GetSource());
            for (auto& resultOp : resultOps) {
                auto* newFilterOp = filterOp->Clone(context)->As<TFilterOperator>();
                newFilterOp->SetSource(resultOp);
                resultOp = newFilterOp;
            }

            break;
        }

        case EOperatorKind::Group: {
            auto* groupOp = op->As<TGroupOperator>();

            resultOps = Scatter(groupOp->GetSource());
            for (auto& resultOp : resultOps) {
                auto* newGroupOp = groupOp->Clone(context)->As<TGroupOperator>();
                newGroupOp->SetSource(resultOp);
                resultOp = newGroupOp;
            }

            if (resultOps.size() <= 1) {
                break;
            }

            auto* finalGroupOp = context->TrackedNew<TGroupOperator>(Gather(resultOps));

            auto& finalGroupItems = finalGroupOp->GroupItems();
            for (const auto& groupItem : groupOp->GroupItems()) {
                auto referenceExpr = context->TrackedNew<TReferenceExpression>(
                    NullSourceLocation,
                    groupItem.Name);
                finalGroupItems.push_back(TNamedExpression(
                    referenceExpr,
                    groupItem.Name));
            }

            auto& finalAggregateItems = finalGroupOp->AggregateItems();
            for (const auto& aggregateItem : groupOp->AggregateItems()) {
                auto referenceExpr = context->TrackedNew<TReferenceExpression>(
                    NullSourceLocation,
                    aggregateItem.Name);
                finalAggregateItems.push_back(TAggregateItem(
                    referenceExpr,
                    aggregateItem.AggregateFunction,
                    aggregateItem.Name));
            }

            resultOps.clear();
            resultOps.push_back(finalGroupOp);

            break;
        }

        case EOperatorKind::Project: {
            auto* projectOp = op->As<TProjectOperator>();

            resultOps = Scatter(projectOp->GetSource());

            for (auto& resultOp : resultOps) {
                auto* newProjectOp = projectOp->Clone(context)->As<TProjectOperator>();
                newProjectOp->SetSource(resultOp);
                resultOp = newProjectOp;
            }

            break;
        }

    }

    return resultOps;
}

const TOperator* TCoordinator::Gather(const std::vector<const TOperator*>& ops)
{
    YASSERT(!ops.empty());

    auto* context = Fragment_.GetContext().Get();

    auto* resultOp = context->TrackedNew<TScanOperator>();
    auto& resultSplits = resultOp->DataSplits();

    std::function<const TDataSplit&(const TOperator*)> collocatedSplit =
        [&collocatedSplit] (const TOperator* op) -> const TDataSplit& {
            switch (op->GetKind()) {
                case EOperatorKind::Scan:
                    return op->As<TScanOperator>()->DataSplits().front();
                case EOperatorKind::Filter:
                    return collocatedSplit(op->As<TFilterOperator>()->GetSource());
                case EOperatorKind::Group:
                    return collocatedSplit(op->As<TGroupOperator>()->GetSource());
                case EOperatorKind::Project:
                    return collocatedSplit(op->As<TProjectOperator>()->GetSource());
            }
            YUNREACHABLE();
        };

    for (const auto& op : ops) {
        auto fragment = TPlanFragment(context, op);
        LOG_DEBUG("Created subfragment (SubfragmentId: %s)",
            ~ToString(fragment.Id()));

        int index = Peers_.size();
        Peers_.emplace_back(fragment, collocatedSplit(op), nullptr, Null);

        TDataSplit facadeSplit;

        SetObjectId(
            &facadeSplit,
            MakeId(EObjectType::PlanFragment, 0xbabe, index, 0xc0ffee));
        SetTableSchema(&facadeSplit, op->GetTableSchema());
        SetKeyColumns(&facadeSplit, op->GetKeyColumns());
        SetBothBounds(&facadeSplit, op->GetKeyRange());

        resultSplits.push_back(facadeSplit);
    }

    return resultOp;
}

const TOperator* TCoordinator::Simplify(const TOperator* op)
{
    // If we have delegated a segment locally, then we can omit extra data copy.
    // Basically, we would like to reduce
    //   (peers) -> (first local query) -> (second local query)
    // to
    //   (peers) -> (first + second local query)
    return Apply(
        Fragment_.GetContext().Get(),
        op,
        [this] (const TPlanContext* context, const TOperator* op) -> const TOperator* {
            auto* scanOp = op->As<TScanOperator>();
            if (!scanOp || scanOp->DataSplits().size() != 1) {
                return op;
            }

            const auto& outerSplit = scanOp->DataSplits().front();
            auto outerExplanation = Explain(outerSplit);
            if (!outerExplanation.IsInternal || outerExplanation.IsEmpty) {
                return op;
            }

            YCHECK(outerExplanation.PeerIndex < Peers_.size());
            const auto& peer = Peers_[outerExplanation.PeerIndex];

            const auto& innerSplit = peer.CollocatedSplit;
            auto innerExplanation = Explain(innerSplit);
            if (!innerExplanation.IsInternal) {
                return op;
            }

            LOG_DEBUG("Keeping subfragment local (SubfragmentId: %s)",
                ~ToString(peer.Fragment.Id()));

            return peer.Fragment.GetHead();
        });
}

TGroupedDataSplits TCoordinator::SplitAndRegroup(
    const TDataSplits& splits,
    const TTableSchema& tableSchema,
    const TKeyColumns& keyColumns)
{
    TGroupedDataSplits result;
    TDataSplits allSplits;

    for (const auto& split : splits) {
        auto objectId = GetObjectIdFromDataSplit(split);

        if (Callbacks_->CanSplit(split)) {
            LOG_DEBUG("Splitting input %s", ~ToString(objectId));
        } else {
            allSplits.push_back(split);
            continue;
        }

        TDataSplits newSplits;

        {
            NProfiling::TAggregatingTimingGuard timingGuard(&QueryStat.AsyncTime);
            auto newSplitsOrError = WaitFor(Callbacks_->SplitFurther(split, Fragment_.GetContext()));
            newSplits = newSplitsOrError.ValueOrThrow();
        }

        LOG_DEBUG(
            "Got %" PRISZT " splits for input %s",
            newSplits.size(),
            ~ToString(objectId));

        allSplits.insert(allSplits.end(), newSplits.begin(), newSplits.end());
    }

    if (allSplits.empty()) {
        LOG_DEBUG("Adding an empty split");

        allSplits.emplace_back();
        auto& split = allSplits.back();

        SetObjectId(
            &split,
            MakeId(EObjectType::EmptyPlanFragment, 0xdead, MaxCounter, 0xc0ffee));
        SetTableSchema(&split, tableSchema);
        SetKeyColumns(&split, keyColumns);

        result.emplace_back(allSplits);
    } else {
        LOG_DEBUG("Regrouping %" PRISZT " splits", allSplits.size());

        result = Callbacks_->Regroup(allSplits, Fragment_.GetContext());
    }

    return result;
}

TCoordinator::TDataSplitExplanation TCoordinator::Explain(const TDataSplit& split)
{
    TDataSplitExplanation explanation;

    auto objectId = GetObjectIdFromDataSplit(split);
    auto type = TypeFromId(objectId);
    auto counter = static_cast<int>(CounterFromId(objectId));

    explanation.IsInternal = true;
    explanation.IsEmpty = false;
    explanation.PeerIndex = MaxCounter;

    switch (type) {
        case EObjectType::PlanFragment:
            explanation.PeerIndex = counter;
            break;
        case EObjectType::EmptyPlanFragment:
            explanation.IsEmpty = true;
            break;
        default:
            explanation.IsInternal = false;
            break;
    }

    return explanation;
}

void TCoordinator::DelegateToPeers()
{
    for (auto& peer : Peers_) {
        auto explanation = Explain(peer.CollocatedSplit);
        if (!explanation.IsInternal) {
            LOG_DEBUG("Delegating subfragment (SubfragmentId: %s)",
                ~ToString(peer.Fragment.Id()));
            std::tie(peer.Reader, peer.QueryResult) = Callbacks_->Delegate(
                peer.Fragment,
                peer.CollocatedSplit);
        } else {
            peer.QueryResult = MakePromise<TErrorOr<TQueryStatistics>>(TQueryStatistics()).ToFuture();
        }
    }
}

ISchemafulReaderPtr TCoordinator::GetReader(
    const TDataSplit& split,
    TPlanContextPtr context)
{
    auto objectId = GetObjectIdFromDataSplit(split);
    LOG_DEBUG("Creating reader for %s", ~ToString(objectId));

    auto explanation = Explain(split);

    if (explanation.IsEmpty) {
        return New<TEmptySchemafulReader>();
    }

    if (explanation.IsInternal) {
        YCHECK(explanation.PeerIndex < Peers_.size());
        return Peers_[explanation.PeerIndex].Reader;
    }

    return Callbacks_->GetReader(split, context);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NQueryClient
} // namespace NYT

