#include "stdafx.h"
#include "gc.h"
#include "private.h"
#include "config.h"
#include "object_manager.h"

#include <server/cell_master/bootstrap.h>
#include <server/cell_master/meta_state_facade.h>
#include <server/cell_master/serialization_context.h>

#include <server/object_server/object_manager.pb.h>

namespace NYT {
namespace NObjectServer {

using namespace NCellMaster;
using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

static auto& Logger = ObjectServerLogger;
static auto& Profiler = ObjectServerProfiler;

////////////////////////////////////////////////////////////////////////////////

TGarbageCollector::TGarbageCollector(
    TObjectManagerConfigPtr config,
    NCellMaster::TBootstrap* bootstrap)
    : Config(config)
    , Bootstrap(bootstrap)
{
    YCHECK(Config);
    YCHECK(Bootstrap);
}

void TGarbageCollector::StartSweep()
{
    YCHECK(!SweepExecutor);
    SweepExecutor = New<TPeriodicExecutor>(
        Bootstrap->GetMetaStateFacade()->GetEpochInvoker(),
        BIND(&TGarbageCollector::OnSweep, MakeWeak(this)),
        Config->GCSweepPeriod,
        EPeriodicExecutorMode::Manual);
    SweepExecutor->Start();
}

void TGarbageCollector::StopSweep()
{
    if (SweepExecutor) {
        SweepExecutor->Stop();
        SweepExecutor.Reset();
    }
}

void TGarbageCollector::Save(NCellMaster::TSaveContext& context) const
{
    std::vector<TObjectBase*> allZombies;
    allZombies.reserve(Zombies.size() + LockedZombies.size());
    for (auto* object : Zombies) {
        allZombies.push_back(object);
    }
    for (auto* object : LockedZombies) {
        allZombies.push_back(object);
    }
    // NB: allZombies is vector, not hashset; manual sort needed.
    std::sort(allZombies.begin(), allZombies.end(), CompareObjectsForSerialization);
    SaveObjectRefs(context, allZombies);
}

void TGarbageCollector::Load(NCellMaster::TLoadContext& context)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    LoadObjectRefs(context, Zombies);
    LockedZombies.clear();

    CollectPromise = NewPromise();
    if (Zombies.empty()) {
        CollectPromise.Set();
    }
}

void TGarbageCollector::Clear()
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    Zombies.clear();
    LockedZombies.clear();

    CollectPromise = NewPromise();
    CollectPromise.Set();
}

TFuture<void> TGarbageCollector::Collect()
{
    VERIFY_THREAD_AFFINITY_ANY();

    return CollectPromise;
}

bool TGarbageCollector::IsEnqueued(TObjectBase* object) const
{
    return Zombies.find(object) != Zombies.end() ||
           LockedZombies.find(object) != LockedZombies.end();
}

void TGarbageCollector::Enqueue(TObjectBase* object)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);
    YASSERT(!object->IsAlive());

    if (Zombies.empty() && LockedZombies.empty() && CollectPromise.IsSet()) {
        CollectPromise = NewPromise();
    }

    if (object->IsLocked()) {
        YCHECK(LockedZombies.insert(object).second);
        LOG_DEBUG("Object is put into locked zombie queue (ObjectId: %s)",
            ~ToString(object->GetId()));
    } else {
        YCHECK(Zombies.insert(object).second);
        LOG_TRACE("Object is put into zombie queue (ObjectId: %s)",
            ~ToString(object->GetId()));
    }
}

void TGarbageCollector::Unlock(TObjectBase* object)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);
    YASSERT(!object->IsAlive());
    YASSERT(!object->IsLocked());

    YCHECK(LockedZombies.erase(object) == 1);
    YCHECK(Zombies.insert(object).second);
    
    LOG_DEBUG("Object is unlocked and moved to zombie queue (ObjectId: %s)",
        ~ToString(object->GetId()));
}

void TGarbageCollector::UnlockAll()
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    for (auto* object : LockedZombies) {
        YASSERT(object->IsLocked());
        YCHECK(Zombies.insert(object).second);
    }
    LockedZombies.clear();
}

void TGarbageCollector::Dequeue(TObjectBase* object)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    YCHECK(Zombies.erase(object) == 1);
}

void TGarbageCollector::CheckEmpty()
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    if (Zombies.empty() && LockedZombies.empty()) {
        auto hydraManager = Bootstrap->GetMetaStateFacade()->GetManager();
        LOG_DEBUG_UNLESS(hydraManager->IsRecovery(), "GC queue is empty");
        CollectPromise.Set();
    }
}

void TGarbageCollector::OnSweep()
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    // Shrink zombies hashtable, if needed.
    if (Zombies.bucket_count() > 4 * Zombies.size() && Zombies.bucket_count() > 16) {
        yhash_set<TObjectBase*> newZombies(Zombies.begin(), Zombies.end());
        LOG_DEBUG("Shrinking zombie set (BucketCount: %" PRISZT "->%" PRISZT ", ZombieCount: %" PRISZT ")",
            Zombies.bucket_count(),
            newZombies.bucket_count(),
            Zombies.size());
        Zombies.swap(newZombies);
    }

    auto metaStateFacade = Bootstrap->GetMetaStateFacade();
    auto hydraManager = metaStateFacade->GetManager();
    if (Zombies.empty() || !hydraManager->IsActiveLeader()) {
        SweepExecutor->ScheduleNext();
        return;
    }

    // Extract up to MaxObjectsPerGCSweep objects and post a mutation.
    NProto::TReqDestroyObjects request;
    for (auto it = Zombies.begin();
         it != Zombies.end() && request.object_ids_size() < Config->MaxObjectsPerGCSweep;
         ++it)
    {
        auto* object = *it;
        ToProto(request.add_object_ids(), object->GetId());
    }

    LOG_DEBUG("Starting GC sweep for %d objects",
        request.object_ids_size());

    auto invoker = metaStateFacade->GetEpochInvoker();
    Bootstrap
        ->GetObjectManager()
        ->CreateDestroyObjectsMutation(request)
        ->OnSuccess(BIND(&TGarbageCollector::OnCommitSucceeded, MakeWeak(this)).Via(invoker))
        ->OnError(BIND(&TGarbageCollector::OnCommitFailed, MakeWeak(this)).Via(invoker))
        ->PostCommit();
}

void TGarbageCollector::OnCommitSucceeded()
{
    LOG_DEBUG("GC sweep commit succeeded");

    SweepExecutor->ScheduleOutOfBand();
    SweepExecutor->ScheduleNext();
}

void TGarbageCollector::OnCommitFailed(const TError& error)
{
    LOG_ERROR(error, "GC sweep commit failed");

    SweepExecutor->ScheduleNext();
}

int TGarbageCollector::GetGCQueueSize() const
{
    return static_cast<int>(Zombies.size());
}

int TGarbageCollector::GetLockedGCQueueSize() const
{
    return static_cast<int>(LockedZombies.size());
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NObjectServer
} // namespace NYT
