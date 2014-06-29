#include "stdafx.h"
#include "changelog.h"
#include "file_changelog.h"
#include "sync_file_changelog.h"
#include "config.h"
#include "private.h"

#include <core/misc/fs.h>
#include <core/misc/cache.h>

#include <core/concurrency/thread_affinity.h>
#include <core/concurrency/action_queue.h>
#include <core/concurrency/periodic_executor.h>

#include <core/logging/tagged_logger.h>

#include <atomic>

namespace NYT {
namespace NHydra {

using namespace NConcurrency;
using namespace NFS;

////////////////////////////////////////////////////////////////////////////////

static auto& Logger = HydraLogger;
static auto& Profiler = HydraProfiler;

static const auto FlushThreadQuantum = TDuration::MilliSeconds(10);

////////////////////////////////////////////////////////////////////////////////

class TFileChangelogDispatcher::TChangelogQueue
    : public TRefCounted
{
public:
    explicit TChangelogQueue(TSyncFileChangelogPtr changelog)
        : Changelog_(changelog)
        , UseCount_(0)
        , FlushedRecordCount_(changelog->GetRecordCount())
    { }


    void Lock()
    {
        VERIFY_THREAD_AFFINITY_ANY();

        ++UseCount_;
    }

    void Unlock()
    {
        VERIFY_THREAD_AFFINITY_ANY();

        --UseCount_;
    }


    TAsyncError Append(TSharedRef data)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        TAsyncError result;
        {
            TGuard<TSpinLock> guard(SpinLock_);
            YCHECK(!SealForced_ && !UnsealForced_);
            AppendQueue_.push_back(std::move(data));
            ByteSize_ += data.Size();
            YCHECK(FlushPromise_);
            result = FlushPromise_;
        }

        return result;
    }


    TAsyncError AsyncFlush()
    {
        VERIFY_THREAD_AFFINITY_ANY();

        TGuard<TSpinLock> guard(SpinLock_);

        if (FlushQueue_.empty() && AppendQueue_.empty()) {
            return OKFuture;
        }

        FlushForced_ = true;
        return FlushPromise_;
    }

    TAsyncError AsyncSeal(int recordCount)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        TAsyncError result;
        {
            TGuard<TSpinLock> guard(SpinLock_);
            YCHECK(!SealForced_ && !UnsealForced_);
            SealForced_ = true;
            SealRecordCount_ = recordCount;
            result = SealPromise_ = NewPromise<TError>();
        }

        return result;
    }

    TAsyncError AsyncUnseal()
    {
        VERIFY_THREAD_AFFINITY_ANY();

        TAsyncError result;
        {
            TGuard<TSpinLock> guard(SpinLock_);
            YCHECK(!SealForced_ && !UnsealForced_);
            UnsealForced_ = true;
            result = UnsealPromise_ = NewPromise<TError>();
        }

        return result;
    }


    bool HasPendingActions()
    {
        VERIFY_THREAD_AFFINITY_ANY();

        // Unguarded access seems OK.
        auto config = Changelog_->GetConfig();
        if (ByteSize_ >= config->FlushBufferSize) {
            return true;
        }

        if (Changelog_->GetLastFlushed() < TInstant::Now() - config->FlushPeriod) {
            return true;
        }

        if (FlushForced_) {
            return true;
        }

        if (SealForced_) {
            return true;
        }

        if (UnsealForced_) {
            return true;
        }

        return false;
    }

    void RunPendingActions()
    {
        VERIFY_THREAD_AFFINITY(SyncThread);

        MaybeSyncFlush();
        MaybeSyncSeal();
        MaybeSyncUnseal();
    }

    bool TrySweep()
    {
        VERIFY_THREAD_AFFINITY_ANY();

        TAsyncErrorPromise promise;
        {
            TGuard<TSpinLock> guard(SpinLock_);

            if (!AppendQueue_.empty() || !FlushQueue_.empty()) {
                return false;
            }

            if (SealForced_ && !SealPromise_.IsSet()) {
                return false;
            }

            if (UnsealForced_ && !UnsealPromise_.IsSet()) {
                return false;
            }

            if (UseCount_.load() > 0) {
                return false;
            }

            promise = FlushPromise_;
            FlushPromise_.Reset();
            FlushForced_ = false;
        }

        promise.Set(TError());

        return true;
    }
    

    std::vector<TSharedRef> Read(int firstRecordId, int maxRecords, i64 maxBytes)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        std::vector<TSharedRef> records;
        int currentRecordId = firstRecordId;
        int needRecords = maxRecords;
        i64 needBytes = maxBytes;
        i64 readBytes = 0;

        auto appendRecord = [&] (const TSharedRef& record) {
            records.push_back(record);
            --needRecords;
            ++currentRecordId;
            needBytes -= record.Size();
            readBytes += record.Size();
        };

        auto needMore = [&] () {
            return needRecords > 0 && needBytes > 0;
        };

        while (needMore()) {
            TGuard<TSpinLock> guard(SpinLock_);
            if (currentRecordId < FlushedRecordCount_) {
                // Read from disk, w/o spinlock.
                guard.Release();

                PROFILE_TIMING ("/changelog_read_io_time") {
                    auto diskRecords = Changelog_->Read(currentRecordId, needRecords, needBytes);
                    for (const auto& record : diskRecords) {
                        appendRecord(record);
                    }
                }
            } else {
                // Read from memory, w/ spinlock.

                auto readFromMemory = [&] (const std::vector<TSharedRef>& memoryRecords, int firstMemoryRecordId) {
                    if (!needMore())
                        return;
                    int memoryIndex = currentRecordId - firstMemoryRecordId;
                    YCHECK(memoryIndex >= 0);
                    while (memoryIndex < static_cast<int>(memoryRecords.size()) && needMore()) {
                        appendRecord(memoryRecords[memoryIndex++]);
                    }
                };

                PROFILE_TIMING ("/changelog_read_copy_time") {
                    readFromMemory(FlushQueue_, FlushedRecordCount_);
                    readFromMemory(AppendQueue_, FlushedRecordCount_ + FlushQueue_.size());
                }

                // Break since we don't except more records beyond this point.
                break;
            }
        }

        Profiler.Enqueue("/changelog_read_record_count", records.size());
        Profiler.Enqueue("/changelog_read_size", readBytes);

        return records;
    }

private:
    TSyncFileChangelogPtr Changelog_;

    TSpinLock SpinLock_;
    std::atomic<int> UseCount_;

    //! Number of records flushed to the underlying sync changelog.
    int FlushedRecordCount_ = 0;
    //! These records are currently being flushed to the underlying sync changelog and
    //! immediately follow the flushed part.
    std::vector<TSharedRef> FlushQueue_;
    //! Newly appended records go here. These records immediately follow the flush part.
    std::vector<TSharedRef> AppendQueue_;

    i64 ByteSize_ = 0;

    TAsyncErrorPromise FlushPromise_ = NewPromise<TError>();
    bool FlushForced_ = false;

    TAsyncErrorPromise SealPromise_;
    bool SealForced_ = false;
    int SealRecordCount_ = -1;

    TAsyncErrorPromise UnsealPromise_;
    bool UnsealForced_ = false;


    DECLARE_THREAD_AFFINITY_SLOT(SyncThread);


    void MaybeSyncFlush()
    {
        TAsyncErrorPromise flushPromise;
        {
            TGuard<TSpinLock> guard(SpinLock_);

            YCHECK(FlushQueue_.empty());
            FlushQueue_.swap(AppendQueue_);
            ByteSize_ = 0;

            YCHECK(FlushPromise_);
            flushPromise = FlushPromise_;
            FlushPromise_ = NewPromise<TError>();
            FlushForced_ = false;
        }

        if (!FlushQueue_.empty()) {
            PROFILE_TIMING("/changelog_flush_io_time") {
                Changelog_->Append(FlushedRecordCount_, FlushQueue_);
                Changelog_->Flush();
            }
        }

        {
            TGuard<TSpinLock> guard(SpinLock_);
            FlushedRecordCount_ += FlushQueue_.size();
            FlushQueue_.clear();
        }

        flushPromise.Set(TError());
    }

    void MaybeSyncSeal()
    {
        TAsyncErrorPromise promise;
        {
            TGuard<TSpinLock> guard(SpinLock_);
            if (!SealForced_)
                return;
            promise = SealPromise_;
            SealPromise_.Reset();
            SealForced_ = false;
        }

        while (true) {
            {
                TGuard<TSpinLock> guard(SpinLock_);
                if (AppendQueue_.empty())
                    break;
            }
            MaybeSyncFlush();
        }

        PROFILE_TIMING("/changelog_seal_io_time") {
            Changelog_->Seal(SealRecordCount_);
        }

        promise.Set(TError());
    }

    void MaybeSyncUnseal()
    {
        TAsyncErrorPromise promise;
        {
            TGuard<TSpinLock> guard(SpinLock_);
            if (!UnsealForced_)
                return;
            promise = UnsealPromise_;
            UnsealPromise_.Reset();
            UnsealForced_ = false;
        }

        PROFILE_TIMING("/changelog_unseal_io_time") {
            Changelog_->Unseal();
        }

        promise.Set(TError());
    }

};

////////////////////////////////////////////////////////////////////////////////

class TFileChangelogDispatcher::TImpl
    : public TRefCounted
{
public:
    explicit TImpl(const Stroka& threadName)
        : ProcessQueuesCallback_(BIND(&TImpl::ProcessQueues, MakeWeak(this)))
        , ActionQueue_(New<TActionQueue>(threadName))
        , PeriodicExecutor_(New<TPeriodicExecutor>(
            ActionQueue_->GetInvoker(),
            ProcessQueuesCallback_,
            FlushThreadQuantum))
        , RecordCounter_("/record_rate")
        , SizeCounter_("/record_throughput")
    {
        ProcessQueuesCallbackPending_ = false;
        PeriodicExecutor_->Start();
    }

    ~TImpl()
    {
        Shutdown();
    }

    void Shutdown()
    {
        PeriodicExecutor_->Stop();
        ActionQueue_->Shutdown();
    }

    IInvokerPtr GetInvoker()
    {
        return ActionQueue_->GetInvoker();
    }

    TAsyncError Append(
        TSyncFileChangelogPtr changelog,
        const TSharedRef& record)
    {
        auto queue = GetQueueAndLock(changelog);
        auto result = queue->Append(record);
        queue->Unlock();
        Wakeup();

        Profiler.Increment(RecordCounter_);
        Profiler.Increment(SizeCounter_, record.Size());

        return result;
    }

    std::vector<TSharedRef> Read(
        TSyncFileChangelogPtr changelog,
        int recordId,
        int maxRecords,
        i64 maxBytes)
    {
        YCHECK(recordId >= 0);
        YCHECK(maxRecords >= 0);

        if (maxRecords == 0) {
            return std::vector<TSharedRef>();
        }

        auto queue = FindQueueAndLock(changelog);
        if (queue) {
            auto records = queue->Read(recordId, maxRecords, maxBytes);
            queue->Unlock();
            return std::move(records);
        } else {
            PROFILE_TIMING ("/changelog_read_io_time") {
                return changelog->Read(recordId, maxRecords, maxBytes);
            }
        }
    }

    TAsyncError Flush(TSyncFileChangelogPtr changelog)
    {
        auto queue = FindQueue(changelog);
        return queue ? queue->AsyncFlush() : OKFuture;
    }

    void Close(TSyncFileChangelogPtr changelog)
    {
        RemoveQueue(changelog);
        changelog->Close();
    }

    TAsyncError Seal(TSyncFileChangelogPtr changelog, int recordCount)
    {
        auto queue = GetQueueAndLock(changelog);
        auto result = queue->AsyncSeal(recordCount);
        queue->Unlock();
        Wakeup();
        return result;
    }

    TAsyncError Unseal(TSyncFileChangelogPtr changelog)
    {
        auto queue = GetQueueAndLock(changelog);
        auto result = queue->AsyncUnseal();
        queue->Unlock();
        Wakeup();
        return result;
    }

    void Remove(TSyncFileChangelogPtr changelog)
    {
        RemoveQueue(changelog);

        auto path = changelog->GetFileName();

        changelog->Close();

        NFS::Remove(path);
        NFS::Remove(path + "." + ChangelogIndexExtension);
    }

private:
    TClosure ProcessQueuesCallback_;
    std::atomic<bool> ProcessQueuesCallbackPending_;

    TActionQueuePtr ActionQueue_;
    TPeriodicExecutorPtr PeriodicExecutor_;

    TSpinLock SpinLock_;
    yhash_map<TSyncFileChangelogPtr, TChangelogQueuePtr> QueueMap_;

    NProfiling::TRateCounter RecordCounter_;
    NProfiling::TRateCounter SizeCounter_;


    TChangelogQueuePtr FindQueue(TSyncFileChangelogPtr changelog) const
    {
        TGuard<TSpinLock> guard(SpinLock_);
        auto it = QueueMap_.find(changelog);
        return it == QueueMap_.end() ? nullptr : it->second;
    }

    TChangelogQueuePtr FindQueueAndLock(TSyncFileChangelogPtr changelog) const
    {
        TGuard<TSpinLock> guard(SpinLock_);
        auto it = QueueMap_.find(changelog);
        if (it == QueueMap_.end()) {
            return nullptr;
        }

        auto queue = it->second;
        queue->Lock();
        return queue;
    }

    TChangelogQueuePtr GetQueueAndLock(TSyncFileChangelogPtr changelog)
    {
        TGuard<TSpinLock> guard(SpinLock_);
        TChangelogQueuePtr queue;

        auto it = QueueMap_.find(changelog);
        if (it != QueueMap_.end()) {
            queue = it->second;
        } else {
            queue = New<TChangelogQueue>(changelog);
            YCHECK(QueueMap_.insert(std::make_pair(changelog, queue)).second);
        }

        queue->Lock();
        return queue;
    }

    void RemoveQueue(TSyncFileChangelogPtr changelog)
    {
        TGuard<TSpinLock> guard(SpinLock_);
        QueueMap_.erase(changelog);
    }

    void FlushQueues()
    {
        // Take a snapshot.
        std::vector<TChangelogQueuePtr> queues;
        {
            TGuard<TSpinLock> guard(SpinLock_);
            for (const auto& pair : QueueMap_) {
                const auto& queue = pair.second;
                if (queue->HasPendingActions()) {
                    queues.push_back(queue);
                }
            }
        }

        // Flush and seal the changelogs.
        for (auto queue : queues) {
            queue->RunPendingActions();
        }
    }

    void SweepQueues()
    {
        TGuard<TSpinLock> guard(SpinLock_);
        auto it = QueueMap_.begin();
        while (it != QueueMap_.end()) {
            auto jt = it++;
            auto queue = jt->second;
            if (queue->TrySweep()) {
                QueueMap_.erase(jt);
            }
        }
    }


    void Wakeup()
    {
        if (!ProcessQueuesCallbackPending_.load(std::memory_order_relaxed)) {
            bool expected = false;
            if (ProcessQueuesCallbackPending_.compare_exchange_strong(expected, true)) {
                ActionQueue_->GetInvoker()->Invoke(ProcessQueuesCallback_);
            }
        }
    }

    void ProcessQueues()
    {
        ProcessQueuesCallbackPending_ = false;
        FlushQueues();
        SweepQueues();
    }

};

////////////////////////////////////////////////////////////////////////////////

class TFileChangelog
    : public IChangelog
{
public:
    TFileChangelog(
        TFileChangelogDispatcherPtr dispatcher,
        TFileChangelogConfigPtr config,
        TSyncFileChangelogPtr changelog)
        : DispatcherImpl_(dispatcher->Impl_)
        , Config_(config)
        , SyncChangelog_(changelog)
        , RecordCount_(changelog->GetRecordCount())
        , DataSize_(changelog->GetDataSize())
    { }

    virtual int GetRecordCount() const override
    {
        return RecordCount_;
    }

    virtual i64 GetDataSize() const override
    {
        return DataSize_;
    }

    virtual TSharedRef GetMeta() const override
    {
        return SyncChangelog_->GetMeta();
    }

    virtual bool IsSealed() const override
    {
        return SyncChangelog_->IsSealed();
    }

    virtual TAsyncError Append(const TSharedRef& data) override
    {
        RecordCount_ += 1;
        DataSize_ += data.Size();
        return DispatcherImpl_->Append(SyncChangelog_, data);
    }

    virtual TAsyncError Flush() override
    {
        return DispatcherImpl_->Flush(SyncChangelog_);
    }

    virtual void Close() override
    {
        return DispatcherImpl_->Close(SyncChangelog_);
    }

    virtual std::vector<TSharedRef> Read(
        int firstRecordId,
        int maxRecords,
        i64 maxBytes) const override
    {
        return DispatcherImpl_->Read(
            SyncChangelog_,
            firstRecordId,
            maxRecords,
            maxBytes);
    }

    virtual TAsyncError Seal(int recordCount) override
    {
        YCHECK(recordCount <= RecordCount_);
        RecordCount_.store(recordCount);

        return DispatcherImpl_->Seal(SyncChangelog_, recordCount);
    }

    virtual TAsyncError Unseal() override
    {
        return DispatcherImpl_->Unseal(SyncChangelog_);
    }

    void Remove()
    {
        DispatcherImpl_->Remove(SyncChangelog_);
    }

private:
    TFileChangelogDispatcher::TImplPtr DispatcherImpl_;
    TFileChangelogConfigPtr Config_;
    TSyncFileChangelogPtr SyncChangelog_;

    std::atomic<int> RecordCount_;
    std::atomic<i64> DataSize_;

};

DEFINE_REFCOUNTED_TYPE(TFileChangelog)

////////////////////////////////////////////////////////////////////////////////

TFileChangelogDispatcher::TFileChangelogDispatcher(const Stroka& threadName)
    : Impl_(New<TImpl>(threadName))
{ }

TFileChangelogDispatcher::~TFileChangelogDispatcher()
{ }

void TFileChangelogDispatcher::Shutdown()
{
    Impl_->Shutdown();
}

IInvokerPtr TFileChangelogDispatcher::GetInvoker()
{
    return Impl_->GetInvoker();
}

IChangelogPtr TFileChangelogDispatcher::CreateChangelog(
    const Stroka& path,
    const TSharedRef& meta,
    TFileChangelogConfigPtr config)
{
    auto syncChangelog = New<TSyncFileChangelog>(path, config);
    syncChangelog->Create(meta);

    return New<TFileChangelog>(this, config, syncChangelog);
}

IChangelogPtr TFileChangelogDispatcher::OpenChangelog(
    const Stroka& path,
    TFileChangelogConfigPtr config)
{
    auto syncChangelog = New<TSyncFileChangelog>(path, config);
    syncChangelog->Open();

    return New<TFileChangelog>(this, config, syncChangelog);
}

void TFileChangelogDispatcher::RemoveChangelog(IChangelogPtr changelog)
{
    auto* fileChangelog = dynamic_cast<TFileChangelog*>(changelog.Get());
    YCHECK(fileChangelog);
    fileChangelog->Remove();
}

////////////////////////////////////////////////////////////////////////////////

class TCachedFileChangelog
    : public TCacheValueBase<int, TCachedFileChangelog>
    , public TFileChangelog
{
public:
    explicit TCachedFileChangelog(
        TFileChangelogDispatcherPtr dispather,
        TFileChangelogConfigPtr config,
        TSyncFileChangelogPtr changelog,
        int id)
        : TCacheValueBase(id)
        , TFileChangelog(
            dispather,
            config,
            changelog)
    { }

};

class TFileChangelogStore
    : public TSizeLimitedCache<int, TCachedFileChangelog>
    , public IChangelogStore
{
public:
    TFileChangelogStore(
        const Stroka& threadName,
        const TCellGuid& cellGuid,
        TFileChangelogStoreConfigPtr config)
        : TSizeLimitedCache(config->MaxCachedChangelogs)
        , Dispatcher_(New<TFileChangelogDispatcher>(threadName))
        , CellGuid_(cellGuid)
        , Config_(config)
        , Logger(HydraLogger)
    {
        Logger.AddTag(Sprintf("Path: %s", ~Config_->Path));
    }

    void Start()
    {
        LOG_DEBUG("Preparing changelog store");

        NFS::ForcePath(Config_->Path);
        NFS::CleanTempFiles(Config_->Path);
    }

    virtual const TCellGuid& GetCellGuid() const override
    {
        return CellGuid_;
    }

    virtual TFuture<TErrorOr<IChangelogPtr>> CreateChangelog(int id, const TSharedRef& meta) override
    {
        return BIND(&TFileChangelogStore::DoCreateChangelog, MakeStrong(this))
            .Guarded()
            .AsyncVia(GetHydraIOInvoker())
            .Run(id, meta);
    }

    virtual TFuture<TErrorOr<IChangelogPtr>> OpenChangelog(int id) override
    {
        return BIND(&TFileChangelogStore::DoOpenChangelog, MakeStrong(this))
            .Guarded()
            .AsyncVia(GetHydraIOInvoker())
            .Run(id);
    }

    virtual TFuture<TErrorOr<int>> GetLatestChangelogId(int initialId) override
    {
        return BIND(&TFileChangelogStore::DoGetLatestChangelogId, MakeStrong(this))
            .Guarded()
            .AsyncVia(GetHydraIOInvoker())
            .Run(initialId);
    }

private:
    TFileChangelogDispatcherPtr Dispatcher_;

    TCellGuid CellGuid_;
    TFileChangelogStoreConfigPtr Config_;

    NLog::TTaggedLogger Logger;


    Stroka GetChangelogPath(int id)
    {
        return NFS::CombinePaths(
            Config_->Path,
            Sprintf("%09d.%s", id, ~ChangelogExtension));
    }


    IChangelogPtr DoCreateChangelog(int id, const TSharedRef& meta)
    {
        TInsertCookie cookie(id);
        if (!BeginInsert(&cookie)) {
            THROW_ERROR_EXCEPTION("Trying to create an already existing changelog %d",
                id);
        }

        auto path = GetChangelogPath(id);

        try {
            auto changelog = New<TSyncFileChangelog>(
                path,
                Config_);
            changelog->Create(meta);
            cookie.EndInsert(New<TCachedFileChangelog>(
                Dispatcher_,
                Config_,
                changelog,
                id));
        } catch (const std::exception& ex) {
            LOG_FATAL(ex, "Error creating changelog %d",
                id);
        }

        auto result = WaitFor(cookie.GetValue());
        THROW_ERROR_EXCEPTION_IF_FAILED(result);
        return result.Value();
    }

    IChangelogPtr DoOpenChangelog(int id)
    {
        TInsertCookie cookie(id);
        if (BeginInsert(&cookie)) {
            auto path = GetChangelogPath(id);
            if (!NFS::Exists(path)) {
                cookie.Cancel(TError(
                    NHydra::EErrorCode::NoSuchChangelog,
                    "No such changelog %d",
                    id));
            } else {
                try {
                    auto changelog = New<TSyncFileChangelog>(
                        path,
                        Config_);
                    changelog->Open();
                    cookie.EndInsert(New<TCachedFileChangelog>(
                        Dispatcher_,
                        Config_,
                        changelog,
                        id));
                } catch (const std::exception& ex) {
                    LOG_FATAL(ex, "Error opening changelog %d",
                        id);
                }
            }
        }

        auto changelogOrError = cookie.GetValue().Get();
        THROW_ERROR_EXCEPTION_IF_FAILED(changelogOrError);
        return changelogOrError.Value();
    }

    int DoGetLatestChangelogId(int initialId)
    {
        int latestId = NonexistingSegmentId;
        yhash_set<int> ids;

        auto fileNames = EnumerateFiles(Config_->Path);
        for (const auto& fileName : fileNames) {
            auto extension = NFS::GetFileExtension(fileName);
            if (extension != ChangelogExtension)
                continue;
            auto name = NFS::GetFileNameWithoutExtension(fileName);
            try {
                int id = FromString<int>(name);
                YCHECK(ids.insert(id).second);
                if (id >= initialId && (id > latestId || latestId == NonexistingSegmentId)) {
                    latestId = id;
                }
            } catch (const std::exception&) {
                LOG_WARNING("Found unrecognized file %s", ~fileName.Quote());
            }
        }

        if (latestId != NonexistingSegmentId) {
            for (int id = initialId; id <= latestId; ++id) {
                if (ids.find(id) == ids.end()) {
                    LOG_FATAL("Interim changelog %d is missing",
                        id);                    
                }
            }
        }

        return latestId;
    }

};

IChangelogStorePtr CreateFileChangelogStore(
    const Stroka& threadName,
    const TCellGuid& cellGuid,
    TFileChangelogStoreConfigPtr config)
{
    auto store = New<TFileChangelogStore>(
        threadName,
        cellGuid,
        config);
    store->Start();
    return store;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NHydra
} // namespace NYT

