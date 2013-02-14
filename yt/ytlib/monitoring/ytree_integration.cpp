#include "stdafx.h"
#include "ytree_integration.h"

#include <ytlib/actions/bind.h>
#include <ytlib/ytree/node.h>
#include <ytlib/ytree/virtual.h>

namespace NYT {
namespace NMonitoring {

using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

TYPathServiceProducer CreateMonitoringProducer(
    TMonitoringManager::TPtr monitoringManager)
{
    return BIND([=] () -> IYPathServicePtr {
        return monitoringManager->GetRoot();
    });
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NMonitoring
} // namespace NYT
