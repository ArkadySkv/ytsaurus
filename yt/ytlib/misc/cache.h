#pragma once

#include "common.h"

#include "../actions/future.h"

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

template<class TKey, class TValue, class THash>
class TCacheBase;

template<class TKey, class TValue, class THash = hash<TKey> >
class TCacheValueBase
    : public virtual TRefCountedBase
{
public:
    typedef TIntrusivePtr<TValue> TPtr;

    virtual ~TCacheValueBase();

    TKey GetKey() const;

protected:
    TCacheValueBase(TKey key);

private:
    typedef TCacheBase<TKey, TValue, THash> TCache;
    friend class TCacheBase<TKey, TValue, THash>;

    TIntrusivePtr<TCache> Cache;
    TKey Key;
};

////////////////////////////////////////////////////////////////////////////////

template<class TKey, class TValue, class THash = hash<TKey> >
class TCacheBase
    : public virtual TRefCountedBase
{
public:
    void Clear();
    i32 GetSize() const;

protected:
    typedef TIntrusivePtr<TValue> TValuePtr;
    typedef TIntrusivePtr< TCacheBase<TKey, TValue, THash> > TPtr;
    typedef typename TFuture<TValuePtr>::TPtr TFuturePtr;

    class TInsertCookie
    {
    public:
        TInsertCookie(const TKey& key);
        ~TInsertCookie();

        TFuturePtr GetAsyncResult() const;
        TKey GetKey() const;
        bool IsActive() const;
        void Cancel();
        void EndInsert(TValuePtr value);

    private:
        friend class TCacheBase;

        TKey Key;
        TPtr Cache;
        TFuturePtr AsyncResult;
        bool Active;

    };

    TCacheBase();

    TFuturePtr Lookup(const TKey& key);
    bool BeginInsert(TInsertCookie* cookie);
    void Touch(const TKey& key);
    bool Remove(const TKey& key);

    // Called under SpinLock.
    virtual bool NeedTrim() const = 0;

    // Called without SpinLock.
    virtual void OnTrim(TValuePtr value);

private:
    friend class TCacheValueBase<TKey, TValue, THash>;

    struct TItem
        : public TIntrusiveListItem<TItem>
    {
        TItem()
            : AsyncResult(New< TFuture<TValuePtr> >())
        { }

        explicit TItem(const TValuePtr& value)
            : AsyncResult(New< TFuture<TValuePtr> >(value))
        { }

        TFuturePtr AsyncResult;
    };

    TSpinLock SpinLock;

    typedef yhash_map<TKey, TValue*, THash> TValueMap;
    typedef yhash_map<TKey, TItem*, THash> TItemMap;
    typedef TIntrusiveListWithAutoDelete<TItem, TDelete> TItemList;

    TValueMap ValueMap;
    TItemMap ItemMap;
    TItemList LruList;
    i32 LruListSize;

    void EndInsert(TValuePtr value, TInsertCookie* cookie);
    void CancelInsert(const TKey& key);
    void Touch(TItem* item); // thread-unsafe
    void Unregister(const TKey& key);
    void Trim(); // thread-unsafe
};

////////////////////////////////////////////////////////////////////////////////

template<class TKey, class TValue, class THash = hash<TKey> >
class TCapacityLimitedCache
    : public TCacheBase<TKey, TValue, THash>
{
protected:
    TCapacityLimitedCache(i32 capacity);
    virtual bool NeedTrim() const;

private:
    i32 Capacity;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

#define CACHE_INL_H_
#include "cache-inl.h"
#undef CACHE_INL_H_
