#include "stdafx.h"
#include "executor.h"

#include "private.h"

#include "coordinator.h"
#include "evaluator.h"

namespace NYT {
namespace NQueryClient {

////////////////////////////////////////////////////////////////////////////////

#ifndef YT_USE_LLVM

static TFuture<TErrorOr<TQueryStatistics>> GetQueriesNotSupportedErrror()
{
    return MakeFuture(TErrorOr<TQueryStatistics>(TError("Query evaluation is not supported in this build")));
}

#endif

////////////////////////////////////////////////////////////////////////////////

class TEvaluatorProxy
    : public IExecutor
{
public:
    TEvaluatorProxy(IInvokerPtr invoker, IEvaluateCallbacks* callbacks)
        : Invoker_(std::move(invoker))
        , Callbacks_(callbacks)
    { }

    virtual TFuture<TErrorOr<TQueryStatistics>> Execute(
        const TPlanFragment& fragment,
        ISchemafulWriterPtr writer) override
    {
#ifdef YT_USE_LLVM
        auto this_ = MakeStrong(this);
        return BIND([this, this_, fragment, writer] () -> TErrorOr<TQueryStatistics> {
                return Evaluator_.Run(Callbacks_, fragment, std::move(writer));
            })
            .AsyncVia(Invoker_)
            .Run();
#else
        return GetQueriesNotSupportedErrror();
#endif
    }

private:
    IInvokerPtr Invoker_;
    IEvaluateCallbacks* Callbacks_;
#ifdef YT_USE_LLVM
    TEvaluator Evaluator_;
#endif

};

////////////////////////////////////////////////////////////////////////////////

class TCoordinatorProxy
    : public IExecutor
{
public:
    TCoordinatorProxy(IInvokerPtr invoker, ICoordinateCallbacks* callbacks)
        : Invoker_(std::move(invoker))
        , Callbacks_(callbacks)
    { }

    virtual TFuture<TErrorOr<TQueryStatistics>> Execute(
        const TPlanFragment& fragment,
        ISchemafulWriterPtr writer) override
    {
#ifdef YT_USE_LLVM
        auto this_ = MakeStrong(this);

        return BIND([this, this_, fragment, writer] () -> TErrorOr<TQueryStatistics> {
                TCoordinator coordinator(Callbacks_, fragment);

                auto error = coordinator.Run();
                RETURN_IF_ERROR(error);

                auto resultOrError = Evaluator_.Run(
                    &coordinator,
                    coordinator.GetCoordinatorFragment(),
                    std::move(writer));

                RETURN_IF_ERROR(resultOrError);

                TQueryStatistics result = resultOrError.Value();
                TQueryStatistics subqueryResult = coordinator.GetStatistics();

                result.RowsRead += subqueryResult.RowsRead;
                result.RowsWritten += subqueryResult.RowsWritten;
                result.Incomplete |= subqueryResult.Incomplete;

                return result;
            })
            .AsyncVia(Invoker_)
            .Run();
#else
        return GetQueriesNotSupportedErrror();
#endif
    }

private:
    IInvokerPtr Invoker_;
    ICoordinateCallbacks* Callbacks_;
#ifdef YT_USE_LLVM
    TEvaluator Evaluator_;
#endif

};

////////////////////////////////////////////////////////////////////////////////

IExecutorPtr CreateEvaluator(IInvokerPtr invoker, IEvaluateCallbacks* callbacks)
{
    return New<TEvaluatorProxy>(std::move(invoker), callbacks);
}

IExecutorPtr CreateCoordinator(IInvokerPtr invoker, ICoordinateCallbacks* callbacks)
{
    return New<TCoordinatorProxy>(std::move(invoker), callbacks);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NQueryClient
} // namespace NYT

