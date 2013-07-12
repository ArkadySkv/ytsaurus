#include "stdafx.h"
#include "profiling_manager.h"
#include "resource_tracker.h"
#include "timing.h"

#include <ytlib/misc/id_generator.h>
#include <ytlib/misc/periodic_invoker.h>

#include <ytlib/actions/action_queue_detail.h>

#include <ytlib/ytree/node.h>
#include <ytlib/ytree/ephemeral_node_factory.h>
#include <ytlib/ytree/ypath_detail.h>
#include <ytlib/ytree/virtual.h>
#include <ytlib/ytree/ypath_client.h>
#include <ytlib/ytree/fluent.h>

#include <ytlib/logging/log.h>

namespace NYT {
namespace NProfiling  {

using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger Logger("Profiling");
static TProfiler ProfilingProfiler("/profiling", EmptyTagIds, true);
// TODO(babenko): make configurable
static const TDuration MaxKeepInterval = TDuration::Minutes(5);

////////////////////////////////////////////////////////////////////////////////

class TProfilingManager::TImpl
    : public TExecutorThread
{
public:
    TImpl()
        : TExecutorThread("Profiling", EmptyTagIds, true, false)
        , Queue(New<TInvokerQueue>(this, nullptr, EmptyTagIds, true, false))
        , Root(GetEphemeralNodeFactory()->CreateMap())
        , EnqueueCounter("/enqueue_rate")
        , DequeueCounter("/dequeue_rate")
    {
#if !defined(_win_) && !defined(_darwin_)
        ResourceTracker = New<TResourceTracker>(GetInvoker());
#endif
    }

    ~TImpl()
    {
        Queue->Shutdown();
        Shutdown();
    }

    void Start()
    {
        TExecutorThread::Start();
#if !defined(_win_) && !defined(_darwin_)
        ResourceTracker->Start();
#endif
    }

    void Shutdown()
    {
        TExecutorThread::Shutdown();
    }


    void Enqueue(const TQueuedSample& sample, bool selfProfiling)
    {
        if (!IsRunning())
            return;

        if (!selfProfiling) {
            ProfilingProfiler.Increment(EnqueueCounter);
        }

        SampleQueue.Enqueue(sample);
        Signal();
    }


    IInvokerPtr GetInvoker() const
    {
        return Queue;
    }

    IMapNodePtr GetRoot() const
    {
        return Root;
    }


    TTagId RegisterTag(const TTag& tag)
    {
        TGuard<TSpinLock> guard(TagSpinLock);
        auto pair = std::make_pair(tag.Key, tag.Value);
        auto it = TagToId.find(pair);
        if (it != TagToId.end()) {
            return it->second;
        }
        
        auto id = static_cast<TTagId>(IdToTag.size());
        IdToTag.push_back(tag);
        YCHECK(TagToId.insert(std::make_pair(pair, id)).second);

        TagKeyToValues[tag.Key].push_back(tag.Value);

        auto tags = BuildYsonStringFluently()
            .DoMapFor(TagKeyToValues, [] (TFluentMap fluent, const TTagKeyToValues::value_type& pair) {
                fluent
                    .Item(pair.first)
                    .DoListFor(pair.second, [] (TFluentList fluent, const TYsonString& value) {
                        fluent
                            .Item().Value(value);
                    });
            });
        SyncYPathSet(Root, "/@tags", tags);

        return id;
    }

    TSpinLock& GetTagSpinLock()
    {
        return TagSpinLock;
    }

    const TTag& GetTag(TTagId id)
    {
        return IdToTag[id];
    }

private:
    struct TStoredSample
    {
        i64 Id;
        TInstant Time;
        TValue Value;
        TTagIdList TagIds;
    };

    class TBucket
        : public TYPathServiceBase
        , public TSupportsGet
    {
    public:
        typedef std::deque<TStoredSample> TSamples;
        typedef TSamples::iterator TSamplesIterator;
        typedef std::pair<TSamplesIterator, TSamplesIterator> TSamplesRange;

        TBucket()
        {
            Logger = NProfiling::Logger;
        }

        //! Adds a new sample to the bucket inserting in at an appropriate position.
        void AddSample(const TStoredSample& sample)
        {
            // Samples are ordered by time.
            // Search for an appropriate insertion point starting from the the back,
            // this should usually be fast.
            int index = static_cast<int>(Samples.size());
            while (index > 0 && Samples[index - 1].Time > sample.Time) {
                --index;
            }
            Samples.insert(Samples.begin() + index, sample);
        }

        //! Removes the oldest samples keeping [minTime,maxTime] interval no larger than #maxKeepInterval.
        void TrimSamples(TDuration maxKeepInterval)
        {
            if (Samples.size() <= 1)
                return;

            auto deadline = Samples.back().Time - maxKeepInterval;
            while (Samples.front().Time < deadline) {
                Samples.pop_front();
            }
        }

        //! Gets samples with timestamp larger than #lastTime.
        //! If #lastTime is #Null then all samples are returned.
        TSamplesRange GetSamples(TNullable<TInstant> lastTime = Null)
        {
            if (!lastTime) {
                return make_pair(Samples.begin(), Samples.end());
            }

            // Run binary search to find the proper position.
            TStoredSample lastSample;
            lastSample.Time = lastTime.Get();
            auto it = std::upper_bound(
                Samples.begin(),
                Samples.end(),
                lastSample,
                [=] (const TStoredSample& lhs, const TStoredSample& rhs) { return lhs.Time < rhs.Time; });

            return std::make_pair(it, Samples.end());
        }

    private:
        std::deque<TStoredSample> Samples;

        virtual bool DoInvoke(NRpc::IServiceContextPtr context) override
        {
            DISPATCH_YPATH_SERVICE_METHOD(Get);
            return TYPathServiceBase::DoInvoke(context);
        }

        static TNullable<TInstant> ParseInstant(TNullable<i64> value)
        {
            return value ? MakeNullable(TInstant::MicroSeconds(value.Get())) : Null;
        }

        virtual void GetSelf(TReqGet* request, TRspGet* response, TCtxGetPtr context)
        {
            auto profilingManager = TProfilingManager::Get()->Impl;
            TGuard<TSpinLock> tagGuard(profilingManager->GetTagSpinLock());

            context->SetRequestInfo("");
            auto fromTime = ParseInstant(request->Attributes().Find<i64>("from_time"));
            auto range = GetSamples(fromTime);
            response->set_value(BuildYsonStringFluently()
                .DoListFor(range.first, range.second, [&] (TFluentList fluent, const TSamplesIterator& it) {
                    const auto& sample = *it;
                    fluent
                        .Item().BeginMap()
                            .Item("id").Value(sample.Id)
                            .Item("time").Value(static_cast<i64>(sample.Time.MicroSeconds()))
                            .Item("value").Value(sample.Value)
                            .Item("tags").DoMapFor(sample.TagIds, [&] (TFluentMap fluent, TTagId id) {
                                const auto& tag = profilingManager->GetTag(id);
                                fluent
                                    .Item(tag.Key).Value(tag.Value);
                            })
                        .EndMap();
                }).Data());
            context->Reply();
        }

    };

    typedef TIntrusivePtr<TBucket> TBucketPtr;


    TInvokerQueuePtr Queue;
    IMapNodePtr Root;
    TRateCounter EnqueueCounter;
    TRateCounter DequeueCounter;

    TLockFreeQueue<TQueuedSample> SampleQueue;
    yhash_map<TYPath, TBucketPtr> PathToBucket;
    TIdGenerator SampleIdGenerator;

    TSpinLock TagSpinLock;
    std::vector<TTag> IdToTag;
    yhash_map<std::pair<Stroka, TYsonString>, int> TagToId;
    typedef yhash_map<Stroka, std::vector<TYsonString>> TTagKeyToValues;
    TTagKeyToValues TagKeyToValues;

#if !defined(_win_) && !defined(_darwin_)
    TIntrusivePtr<TResourceTracker> ResourceTracker;
#endif

    virtual EBeginExecuteResult BeginExecute() override
    {
        // Handle pending callbacks first.
        auto result = Queue->BeginExecute();
        if (result != EBeginExecuteResult::QueueEmpty) {
            return result;
        }

        // Process all pending samples in a row.
        int samplesProcessed = 0;
        TQueuedSample sample;
        while (SampleQueue.Dequeue(&sample)) {
            ProcessSample(sample);
            samplesProcessed = true;
        }

        ProfilingProfiler.Increment(DequeueCounter, samplesProcessed);

        return samplesProcessed > 0 ? EBeginExecuteResult::Success : EBeginExecuteResult::QueueEmpty;
    }

    virtual void EndExecute() override
    {
        Queue->EndExecute();
    }


    TBucketPtr LookupBucket(const TYPath& path)
    {
        auto it = PathToBucket.find(path);
        if (it != PathToBucket.end()) {
            return it->second;
        }

        LOG_DEBUG("Creating bucket %s", ~path);
        auto bucket = New<TBucket>();
        YCHECK(PathToBucket.insert(std::make_pair(path, bucket)).second);

        auto node = CreateVirtualNode(bucket);
        ForceYPath(Root, path);
        SetNodeByYPath(Root, path, node);

        return bucket;
    }

    void ProcessSample(TQueuedSample& queuedSample)
    {
        auto bucket = LookupBucket(queuedSample.Path);

        TStoredSample storedSample;
        storedSample.Id = SampleIdGenerator.Next();
        storedSample.Time = CpuInstantToInstant(queuedSample.Time);
        storedSample.Value = queuedSample.Value;
        storedSample.TagIds = queuedSample.TagIds;

        bucket->AddSample(storedSample);
        bucket->TrimSamples(MaxKeepInterval);
    }
};

////////////////////////////////////////////////////////////////////////////////

TProfilingManager::TProfilingManager()
    : Impl(New<TImpl>())
{ }

TProfilingManager* TProfilingManager::Get()
{
    return Singleton<TProfilingManager>();
}

void TProfilingManager::Start()
{
    Impl->Start();
}

void TProfilingManager::Shutdown()
{
    Impl->Shutdown();
}

void TProfilingManager::Enqueue(const TQueuedSample& sample, bool selfProfiling)
{
    Impl->Enqueue(sample, selfProfiling);
}

IInvokerPtr TProfilingManager::GetInvoker() const
{
    return Impl->GetInvoker();
}

NYTree::IMapNodePtr TProfilingManager::GetRoot() const
{
    return Impl->GetRoot();
}

TTagId TProfilingManager::RegisterTag(const TTag& tag)
{
    return Impl->RegisterTag(tag);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NProfiling
} // namespace NYT
