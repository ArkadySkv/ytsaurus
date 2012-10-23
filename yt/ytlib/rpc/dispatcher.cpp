#include "stdafx.h"
#include "dispatcher.h"

#include <util/generic/singleton.h>

namespace NYT {
namespace NRpc {

////////////////////////////////////////////////////////////////////////////////

static const int ThreadPoolSize = 8;

////////////////////////////////////////////////////////////////////////////////

TDispatcher::TDispatcher()
    : ThreadPool(New<TThreadPool>(ThreadPoolSize, "Rpc"))
{ }

TDispatcher* TDispatcher::Get()
{
    return Singleton<TDispatcher>();
}

IInvokerPtr TDispatcher::GetPoolInvoker()
{
    return ThreadPool->GetInvoker();
}

void TDispatcher::Shutdown()
{
    ThreadPool->Shutdown();
}

////////////////////////////////////////////////////////////////////////////////           

} // namespace NRpc
} // namespace NYT
