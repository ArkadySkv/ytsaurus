#pragma once

#include "common.h"

#include <ytlib/actions/callback.h>

#include <util/system/spinlock.h>

namespace NYT
{

////////////////////////////////////////////////////////////////////////////////

template <class T>
const TCallback<TIntrusivePtr<T>()>& DefaultRefCountedFactory()
{
    static auto result = BIND([] () -> TIntrusivePtr<T> {
        return New<T>();
    });
    return result;
}

//! Intrusive ptr with lazy creation and double-checked locking.
template <class T, class TLock = TSpinLock>
class TLazyIntrusivePtr
    : public TPointerCommon<TLazyIntrusivePtr<T, TLock>, T>
{
public:
    typedef TCallback<TIntrusivePtr<T>()> TFactory;

    explicit TLazyIntrusivePtr(TFactory factory = DefaultRefCountedFactory<T>())
        : Factory(std::move(factory))
    { }

    T* Get() const throw()
    {
        static_assert(NMpl::TIsConvertible<T*, TRefCountedBase*>::Value, "T must be ref-counted.");
        if (!Value) {
            TGuard<TLock> guard(Lock);
            if (!Value) {
                Value = Factory.Run();
            }
        }
        return ~Value;
    }

    bool HasValue() const throw()
    {
        return Value;
    }

private:
    TLock Lock;
    TFactory Factory;
    mutable TIntrusivePtr<T> Value;

};

////////////////////////////////////////////////////////////////////////////////

template <class T>
T* operator ~ (const TLazyIntrusivePtr<T>& ptr)
{
    return ptr.Get();
}

////////////////////////////////////////////////////////////////////////////////

template <class T>
const TCallback<T*()>& DefaultNonRefCountedFactory()
{
    static auto result = BIND([] () -> T* {
        return new T();
    });
    return result;
}

//! Non-intrusive ptr with lazy creation and double-checked locking.
template <class T, class TLock = TSpinLock>
class TLazyUniquePtr
    : public TPointerCommon<TLazyUniquePtr<T, TLock>, T>
{
public:
    typedef TCallback<T*()> TFactory;

    explicit TLazyUniquePtr(TFactory factory = DefaultNonRefCountedFactory<T>())
        : Factory(std::move(factory))
    { }

    T* Get() const throw()
    {
        static_assert(!NMpl::TIsConvertible<T*, TRefCountedBase*>::Value, "T must not be ref-counted.");
        if (!Value) {
            TGuard<TLock> guard(Lock);
            if (!Value) {
                Value.reset(Factory.Run());
            }
        }
        return ~Value;
    }

    bool HasValue() const throw()
    {
        return Value;
    }

private:
    TLock Lock;
    TFactory Factory;
    mutable std::unique_ptr<T> Value;

};

////////////////////////////////////////////////////////////////////////////////

template <class T>
T* operator ~ (const TLazyUniquePtr<T>& ptr)
{
    return ptr.Get();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
