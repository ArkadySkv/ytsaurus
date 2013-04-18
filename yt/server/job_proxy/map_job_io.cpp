#include "stdafx.h"
#include "map_job_io.h"
#include "config.h"
#include "user_job_io.h"
#include "job.h"

#include <ytlib/table_client/multi_chunk_parallel_reader.h>

#include <ytlib/scheduler/config.h>

namespace NYT {
namespace NJobProxy {

using namespace NScheduler;
using namespace NScheduler::NProto;
using namespace NJobTrackerClient::NProto;

////////////////////////////////////////////////////////////////////

class TMapJobIO
    : public TUserJobIO
{
public:
    TMapJobIO(
        TJobIOConfigPtr config,
        IJobHost* host)
        : TUserJobIO(config, host)
    { }

    virtual void PopulateResult(TJobResult* result) override
    {
        auto* resultExt = result->MutableExtension(TMapJobResultExt::map_job_result_ext);
        PopulateUserJobResult(resultExt->mutable_mapper_result());
    }

};

TAutoPtr<TUserJobIO> CreateMapJobIO(
    TJobIOConfigPtr ioConfig,
    IJobHost* host)
{
    return new TMapJobIO(ioConfig, host);
}

////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
