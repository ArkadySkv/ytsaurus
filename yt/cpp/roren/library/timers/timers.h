#pragma once

#include <util/generic/fwd.h>
#include <util/generic/set.h>
#include <util/generic/vector.h>
#include <util/system/mutex.h>
#include <yt/yt/client/api/public.h>
#include <yt/yt/core/ypath/public.h>

#include "timer/timer.h"

namespace NRoren::NPrivate
{

////////////////////////////////////////////////////////////////////////////////

class TTimersContainer
{
public:
    bool IsValidForExecute(const TTimer& timer, const bool isTimerChanged);
    TVector<TTimer> GetReadyTimers(size_t limit);

protected:
    using TLock = TMutex;
    using TGuard = TGuard<TMutex>;

    TGuard GetLock();

    void ResetInFly(const TGuard& lock, const TTimer& timer);
    bool IsValidForExecute(const TGuard& lock, const TTimer& timer, const bool isTimerChanged);
    TVector<TTimer> GetReadyTimers(const TGuard& lock, size_t limit);
    void Clear(const TGuard& lock);
    void Insert(const TGuard& lock, TTimer timer);
    bool InsertTop(const TGuard& lock, TTimer timer); // Insert only if timer timestamp less than max known timer, return true if inserted
    void Delete(const TGuard& lock, const TTimer& timer);
    void Cleanup(const TGuard& lock, size_t limit);
    size_t GetIndexSize(const TGuard& lock) const;
    std::optional<TTimer> GetLastTimer(const TGuard& lock) const;

    void ResetDeletedTimers(const TGuard& lock);
    bool IsDeleted(const TGuard& lock, const TTimer& timer) const;

private:
    struct TLessValByIt
    {
        bool operator () (const std::set<TTimer>::iterator l, const std::set<TTimer>::iterator r) const
        {
            return *l < *r;
        }
    };

    TLock Lock_;
    std::set<TTimer> TimersIndex_;
    std::set<std::set<TTimer>::iterator, TLessValByIt> TimersNotInFly_;
    THashSet<TTimer, TTimer::THasher> TimersInFly_;
    THashSet<TTimer, TTimer::THasher> DeletedTimers_;
}; // class TTimersContainer

class TTimers:
    public TTimersContainer
{
public:
    using TShardProvider = std::function<TTimer::TShardId(const TTimer::TRawKey&)>;
    using TTimersHashMap = THashMap<TTimer::TKey, std::tuple<TTimer, TTimer::EMergePolicy>, TTimer::TKeyHasher, TTimer::TKeyEqual>;

    void ReInit();
    void Commit(const NYT::NApi::ITransactionPtr tx, const TTimers::TTimersHashMap& updates);
    void OnCommit();

    TTimers(const NYT::NApi::IClientPtr ytClient, NYT::NYPath::TYPath ytPath, TTimer::TShardId shardId, TShardProvider shardProvider);

protected:
    static TTimer MergeTimers(const std::optional<TTimer>& oldTimer, const TTimer& newTimer, const TTimer::EMergePolicy policy);
    void Migrate(const TTimer& timer, const TTimer::TShardId shardId);
    void PopulateIndex();

    TVector<TTimer> YtSelectIndex(const size_t offset, const std::optional<TTimer>& lastKey);
    TVector<TTimer> YtSelectMigrate();
    TVector<TTimer> YtLookupTimers(const NYT::NApi::IClientBasePtr tx, const TVector<TTimer::TKey>& keys);
    void YtInsertMigrate(const NYT::NApi::ITransactionPtr tx, const TTimer& timer, const TTimer::TShardId shardId);
    void YtInsertTimer(const NYT::NApi::ITransactionPtr tx, const TTimer& timer);
    void YtInsertIndex(const NYT::NApi::ITransactionPtr tx, const TTimer& timer);
    void YtDeleteTimer(const NYT::NApi::ITransactionPtr tx, const TTimer::TKey& key);
    void YtDeleteIndex(const NYT::NApi::ITransactionPtr tx, const TTimer& timer);

    std::atomic<bool> PopulateInProgress_ = false;
    TInstant SkipPopulateUntil_ = TInstant::Seconds(0);
    uint64_t IndexLimit_ = 16384;
    uint64_t IndexSelectBatch_ = 1024;
    NYT::NApi::IClientPtr YtClient_;
    NYT::NYPath::TYPath YTimersPath_;
    NYT::NYPath::TYPath YTimersIndexPath_;
    NYT::NYPath::TYPath YTimersMigratePath_;
    TTimer::TShardId ShardId_ = 0;
    TShardProvider GetShardId_;
};

////////////////////////////////////////////////////////////////////////////////

}  // namespace NRoren::NPrivate

