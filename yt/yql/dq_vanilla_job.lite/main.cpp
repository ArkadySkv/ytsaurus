#include <contrib/ydb/library/yql/providers/dq/runtime/task_command_executor.h>

#include <yt/yql/providers/yt/comp_nodes/dq/dq_yt_factory.h>
#include <yt/yql/providers/yt/mkql_dq/yql_yt_dq_transform.h>
#include <yql/essentials/providers/common/comp_nodes/yql_factory.h>

#include <yql/essentials/utils/backtrace/backtrace.h>

#include <yql/essentials/minikql/computation/mkql_computation_node.h>
#include <yql/essentials/minikql/comp_nodes/mkql_factories.h>
#include <yql/essentials/minikql/mkql_stats_registry.h>

#include <yql/essentials/core/dq_integration/transform/yql_dq_task_transform.h>
#include <contrib/ydb/library/yql/dq/comp_nodes/yql_common_dq_factory.h>
#include <contrib/ydb/library/yql/dq/transform/yql_common_dq_transform.h>

#include <library/cpp/yt/mlock/mlock.h>

#include <util/system/mlock.h>
#include <util/stream/output.h>

using namespace NYql;

int main() {
    NBacktrace::RegisterKikimrFatalActions();
    if (!NYT::MlockFileMappings()) {
        Cerr << "mlockall failed, but that's fine" << Endl;
    }

    NKikimr::NMiniKQL::IStatsRegistryPtr statsRegistry = NKikimr::NMiniKQL::CreateDefaultStatsRegistry();

    auto dqCompFactory = NKikimr::NMiniKQL::GetCompositeWithBuiltinFactory({
        GetCommonDqFactory(),
        GetDqYtFactory(statsRegistry.Get()),
        NKikimr::NMiniKQL::GetYqlFactory(),
    });

    auto dqTaskTransformFactory = NYql::CreateCompositeTaskTransformFactory({
        CreateCommonDqTaskTransformFactory(),
        CreateYtDqTaskTransformFactory(),
    });

    return NTaskRunnerProxy::CreateTaskCommandExecutor(dqCompFactory, dqTaskTransformFactory, statsRegistry.Get(), true);
}
