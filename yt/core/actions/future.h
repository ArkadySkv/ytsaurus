#pragma once

#include "common.h"
#include "callback_forward.h"

#include <core/misc/nullable.h>

#include <util/system/event.h>

namespace NYT {

//! Helpers
////////////////////////////////////////////////////////////////////////////////

#define RETURN_IF_ERROR(valueOrError) \
    if (!(valueOrError).IsOK()) { \
        return TError(valueOrError); \
    }

#define RETURN_FUTURE_IF_ERROR(valueOrError, type) \
    if (!(valueOrError).IsOK()) { \
        return MakeFuture<type>(TError(valueOrError)); \
    }

////////////////////////////////////////////////////////////////////////////////

namespace NDetail {

//! Internal state holding the value.
template <class T>
class TPromiseState;

} // namespace NDetail

////////////////////////////////////////////////////////////////////////////////

template <class T>
class TFuture;

template <class T>
class TPromise;

//! Creates an empty (unset) promise.
template <class T>
TPromise<T> NewPromise();

//! Creates an empty (unset) void promise.
TPromise<void> NewPromise();

//! Constructs a pre-set future.
template <class T>
TFuture<typename NMpl::TDecay<T>::TType> MakeFuture(T&& value);

//! Constructs a pre-set void future.
TFuture<void> MakeFuture();

//! Constructs a pre-set promise.
template <class T>
TPromise<typename NMpl::TDecay<T>::TType> MakePromise(T&& value);

//! Constructs a pre-set void promise.
TPromise<void> MakePromise();

//! Constructs a future that gets set when a given #delay elapses.
TFuture<void> MakeDelayed(TDuration delay);

////////////////////////////////////////////////////////////////////////////////

//! Represents a read-only view of an asynchronous computation.
/*
 *  Futures and Promises come in pairs and provide means for one party
 *  to wait for the result of the computation performed by the other party.
 *
 *  TPromise encapsulates the value-returning mechanism while
 *  TFuture enables the clients to wait for this value.
 *
 *  TPromise is implicitly convertible to TFuture while the reverse conversion
 *  is not allowed. This prevents a "malicious" client from setting the value
 *  by itself.
 *
 *  Futures and Promises are thread-safe.
 */
template <class T>
class TFuture
{
public:
    typedef T TValueType;

    //! Empty constructor.
    TFuture();

    //! Empty constructor.
    TFuture(TNull);

    //! Copy constructor.
    TFuture(const TFuture& other);

    //! Move constructor.
    TFuture(TFuture&& other);

    typedef TIntrusivePtr<NYT::NDetail::TPromiseState<T>> TFuture::* TUnspecifiedBoolType;
    //! Checks if the future is associated with a state.
    operator TUnspecifiedBoolType() const;

    //! Drops underlying associated state.
    void Reset();

    //! Swaps underlying associated state.
    void Swap(TFuture& other);

    //! Copy assignment.
    TFuture<T>& operator=(const TFuture<T>& other);

    //! Move assignment.
    TFuture<T>& operator=(TFuture<T>&& other);

    //! Checks if the value is set.
    bool IsSet() const;

    //! Checks if the future is canceled.
    bool IsCanceled() const;

    //! Gets the value.
    /*!
     *  This call will block until the value is set.
     */
    const T& Get() const;

    //! Gets the value if set.
    /*!
     *  This call will not block until the value is set.
     */
    TNullable<T> TryGet() const;

    //! Attaches a result listener.
    /*!
     *  \param onResult A callback to call when the value gets set
     *  (passing the value as a parameter).
     *
     *  \note
     *  If the value is set before the call to #Subscribe, then
     *  #callback gets called synchronously.
     */
    void Subscribe(TCallback<void(T)> onResult);

    //! Attaches a result listener.
    /*!
     *  \param timeout Asynchronously wait for the specified time before
     *  dropping the subscription.
     *  \param onResult A callback to call when the value gets set
     *  (passing the value as a parameter).
     *  \param onTimeout A callback to call when the timeout exceeded.
     *
     *  \note
     *  If the value is set before the call to #Subscribe, then
     *  #callback gets called synchronously.
     */
    void Subscribe(
        TDuration timeout,
        TCallback<void(T)> onResult,
        TClosure onTimeout);

    //! Does exactly same thing as its TPromise counterpart.
    //! Gives the consumder a chance to handle cancelation.
    void OnCanceled(TClosure onCancel);

    //! Notifies the producer that the promised value is no longer needed.
    //! Returns |true| if succeeded, |false| is the promise was already set or canceled.
    bool Cancel();

    //! Chains the asynchronous computation with another synchronous function.
    TFuture<void> Apply(TCallback<void(T)> mutator);

    //! Chains the asynchronous computation with another asynchronous function.
    TFuture<void> Apply(TCallback<TFuture<void>(T)> mutator);

    //! Chains the asynchronous computation with another synchronous function.
    template <class R>
    TFuture<R> Apply(TCallback<R(T)> mutator);

    //! Chains the asynchronous computation with another asynchronous function.
    template <class R>
    TFuture<R> Apply(TCallback<TFuture<R>(T)> mutator);

    //! Converts into a void future by effectively discarding the value.
    TFuture<void> IgnoreResult();

private:
    explicit TFuture(const TIntrusivePtr<NYT::NDetail::TPromiseState<T>>& state);
    explicit TFuture(TIntrusivePtr<NYT::NDetail::TPromiseState<T>>&& state);

    TIntrusivePtr<NYT::NDetail::TPromiseState<T>> Impl_;

private:
    friend class TPromise<T>;

    template <class U>
    friend TFuture<typename NMpl::TDecay<U>::TType> MakeFuture(U&& value);

    template <class U>
    friend bool operator==(const TFuture<U>& lhs, const TFuture<U>& rhs);
    template <class U>
    friend bool operator!=(const TFuture<U>& lhs, const TFuture<U>& rhs);

};

////////////////////////////////////////////////////////////////////////////////

//! #TFuture<> specialized for |void| type.
template <>
class TFuture<void>
{
public:
    typedef void TValueType;

    //! Empty constructor.
    TFuture();

    //! Empty constructor.
    TFuture(TNull);

    //! Copy constructor.
    TFuture(const TFuture& other);

    //! Move constructor.
    TFuture(TFuture&& other);

    typedef TIntrusivePtr<NYT::NDetail::TPromiseState<void>> TFuture::* TUnspecifiedBoolType;
    //! Checks if the future is associated with a state.
    operator TUnspecifiedBoolType() const;

    //! Drops underlying associated state.
    void Reset();

    //! Swaps underlying associated state.
    void Swap(TFuture& other);

    //! Copy assignment.
    TFuture<void>& operator=(const TFuture<void>& other);

    //! Move assignment.
    TFuture<void>& operator=(TFuture<void>&& other);

    //! Checks if the value is set.
    bool IsSet() const;

    //! Checks if the future is canceled.
    bool IsCanceled() const;

    //! Synchronously waits until #Set is called.
    void Get() const;

    //! Attaches a result listener.
    /*!
     *  \param onResult A callback to call when the value gets set
     *  (passing the value as a parameter).
     *
     *  \note
     *  If the value is set before the call to #Subscribe, then
     *  #callback gets called synchronously.
     */
    void Subscribe(TClosure onResult);

    //! Attaches a result listener.
    /*!
     *  \param timeout Asynchronously wait for the specified time before
     *  dropping the subscription.
     *  \param onResult A callback to call when the value gets set
     *  (passing the value as a parameter).
     *  \param onTimeout A callback to call when the timeout exceeded.
     *
     *  \note
     *  If the value is set before the call to #Subscribe, then
     *  #callback gets called synchronously.
     */
    void Subscribe(
        TDuration timeout,
        TClosure onResult,
        TClosure onTimeout);

    //! Does exactly same thing as its TPromise counterpart.
    //! Gives the consumer a chance to handle cancelation.
    void OnCanceled(TClosure onCancel);

    //! Notifies the producer that the promised value is no longer needed.
    //! Returns |true| if succeeded, |false| is the promise was already set or canceled.
    bool Cancel();

    //! Chains the asynchronous computation with another synchronous function.
    TFuture<void> Apply(TCallback<void()> mutator);

    //! Chains the asynchronous computation with another asynchronous function.
    TFuture<void> Apply(TCallback<TFuture<void>()> mutator);

    //! Chains the asynchronous computation with another synchronous function.
    template <class R>
    TFuture<R> Apply(TCallback<R()> mutator);

    //! Chains the asynchronous computation with another asynchronous function.
    template <class R>
    TFuture<R> Apply(TCallback<TFuture<R>()> mutator);

private:
    explicit TFuture(const TIntrusivePtr<NYT::NDetail::TPromiseState<void>>& state);
    explicit TFuture(TIntrusivePtr<NYT::NDetail::TPromiseState<void>>&& state);

    TIntrusivePtr<NYT::NDetail::TPromiseState<void>> Impl_;

private:
    friend class TPromise<void>;

    friend TFuture<void> MakeFuture();

    template <class U>
    friend bool operator==(const TFuture<U>& lhs, const TFuture<U>& rhs);
    template <class U>
    friend bool operator!=(const TFuture<U>& lhs, const TFuture<U>& rhs);

};

////////////////////////////////////////////////////////////////////////////////

//! #TFuture<> equality operator.
template <class T>
bool operator==(const TFuture<T>& lhs, const TFuture<T>& rhs);

//! #TFuture<> inequality operator.
template <class T>
bool operator!=(const TFuture<T>& lhs, const TFuture<T>& rhs);

////////////////////////////////////////////////////////////////////////////////

//! Encapsulates the value-returning mechanism.
template <class T>
class TPromise
{
public:
    typedef T TValueType;

    //! Empty constructor.
    TPromise();

    //! Empty constructor.
    TPromise(TNull);

    //! Copy constructor.
    TPromise(const TPromise& other);

    //! Move constructor.
    TPromise(TPromise&& other);

    typedef TIntrusivePtr<NYT::NDetail::TPromiseState<T>> TPromise::*TUnspecifiedBoolType;
    //! Checks if the promise is associated with a state.
    operator TUnspecifiedBoolType() const;

    //! Drops underlying associated state.
    void Reset();

    //! Swaps underlying associated state.
    void Swap(TPromise& other);

    //! Copy assignment.
    TPromise<T>& operator=(const TPromise<T>& other);

    //! Move assignment.
    TPromise<T>& operator=(TPromise<T>&& other);

    //! Checks if the value is set.
    bool IsSet() const;

    //! Sets the value.
    /*!
     *  Calling this method also invokes all the subscribers.
     */
    void Set(const T& value);
    void Set(T&& value);

    //! Atomically invokes |Set|, if not already set or canceled.
    //! Returns |true| if succeeded, |false| is the promise was already set or canceled.
    bool TrySet(const T& value);
    bool TrySet(T&& value);

    //! Gets the value.
    /*!
     *  This call will block until the value is set.
     */
    const T& Get() const;

    //! Gets the value if set.
    /*!
     *  This call will not block until the value is set.
     */
    TNullable<T> TryGet() const;

    //! Attaches a result listener.
    /*!
     *  \param onResult A callback to call when the value gets set
     *  (passing the value as a parameter).
     *
     *  \note
     *  If the value is set before the call to #Subscribe, then
     *  #callback gets called synchronously.
     */
    void Subscribe(TCallback<void(T)> onResult);

    //! Attaches a result listener.
    /*!
     *  \param timeout Asynchronously wait for the specified time before
     *  dropping the subscription.
     *  \param onResult A callback to call when the value gets set
     *  (passing the value as a parameter).
     *  \param onTimeout A callback to call when the timeout exceeded.
     *
     *  \note
     *  If the value is set before the call to #Subscribe, then
     *  #callback gets called synchronously.
     */
    void Subscribe(
        TDuration timeout,
        TCallback<void(T)> onResult,
        TClosure onTimeout);

    //! Attaches a cancellation listener.
    /*!
     *  \param onCancel A callback to call when #TFuture<T>::Cancel is triggered
     *  by the client.
     *
     *  \note
     *  If the value is set before the call to #OnCanceled, then
     *  #onCancel is discarded.
     */
    void OnCanceled(TClosure onCancel);

    TFuture<T> ToFuture() const;
    operator TFuture<T>() const;

private:
    explicit TPromise(const TIntrusivePtr<NYT::NDetail::TPromiseState<T>>& state);
    explicit TPromise(TIntrusivePtr<NYT::NDetail::TPromiseState<T>>&& state);

    TIntrusivePtr<NYT::NDetail::TPromiseState<T>> Impl_;

private:
    friend class TFuture<T>;

    template <class U>
    friend TPromise<U> NewPromise();
    friend TPromise<void> NewPromise();
    template <class U>
    friend TPromise<typename NMpl::TDecay<U>::TType> MakePromise(U&& value);

    template <class U>
    friend bool operator==(const TPromise<U>& lhs, const TPromise<U>& rhs);
    template <class U>
    friend bool operator!=(const TPromise<U>& lhs, const TPromise<U>& rhs);

};

////////////////////////////////////////////////////////////////////////////////

//! #TPromise<> specialized for |void| type.

//! Encapsulates the value-returning mechanism.
template <>
class TPromise<void>
{
public:
    typedef void TValueType;

    //! Empty constructor.
    TPromise();

    //! Empty constructor.
    TPromise(TNull);

    //! Copy constructor.
    TPromise(const TPromise& other);

    //! Move constructor.
    TPromise(TPromise&& other);

    typedef TIntrusivePtr<NYT::NDetail::TPromiseState<void>> TPromise::* TUnspecifiedBoolType;
    //! Checks if the promise is associated with a state.
    operator TUnspecifiedBoolType() const;

    //! Drops underlying associated state.
    void Reset();

    //! Swaps underlying associated state.
    void Swap(TPromise& other);

    //! Copy assignment.
    TPromise<void>& operator=(const TPromise<void>& other);

    //! Move assignment.
    TPromise<void>& operator=(TPromise<void>&& other);

    //! Checks if the value is set.
    bool IsSet() const;

    //! Sets the value.
    /*!
     *  Calling this method also invokes all the subscribers.
     */
    void Set();

    //! Atomically sets the promise, if not already set or canceled.
    //! Returns |true| if succeeded, |false| is the promise was already set or canceled.
    /*!
     *  Calling this method also invokes all the subscribers.
     */
    bool TrySet();

    //! Gets the value.
    /*!
     *  This call will block until the value is set.
     */
    void Get() const;

    //! Attaches a result listener.
    /*!
     *  \param onResult A callback to call when the value gets set
     *  (passing the value as a parameter).
     *
     *  \note
     *  If the value is set before the call to #Subscribe, then
     *  #onResult gets called synchronously.
     */
    void Subscribe(TClosure onResult);

    //! Attaches a result listener.
    /*!
     *  \param timeout Asynchronously wait for the specified time before
     *  dropping the subscription.
     *  \param onResult A callback to call when the value gets set
     *  (passing the value as a parameter).
     *  \param onTimeout A callback to call when the timeout exceeded.
     *
     *  \note
     *  If the value is set before the call to #Subscribe, then
     *  #onResult gets called synchronously.
     */
    void Subscribe(
        TDuration timeout,
        TClosure onResult,
        TClosure onTimeout);

    //! Attaches a cancellation listener.
    /*!
     *  \param onCancel A callback to call when #TFuture<void>::Cancel is triggered
     *  by the client.
     *
     *  \note
     *  If the value is set before the call to #OnCanceled, then
     *  #onCancel is discarded.
     */
    void OnCanceled(TClosure onCancel);

    TFuture<void> ToFuture() const;
    operator TFuture<void>() const;

private:
    explicit TPromise(const TIntrusivePtr<NYT::NDetail::TPromiseState<void>>& state);
    explicit TPromise(TIntrusivePtr<NYT::NDetail::TPromiseState<void>>&& state);

    TIntrusivePtr<NYT::NDetail::TPromiseState<void>> Impl_;

private:
    friend class TFuture<void>;

    template <class U>
    friend TPromise<U> NewPromise();
    friend TPromise<void> NewPromise();
    friend TPromise<void> MakePromise();

    template <class U>
    friend bool operator==(const TPromise<U>& lhs, const TPromise<U>& rhs);
    template <class U>
    friend bool operator!=(const TPromise<U>& lhs, const TPromise<U>& rhs);

};

////////////////////////////////////////////////////////////////////////////////

//! #TPromise<> equality operator.
template <class T>
bool operator==(const TPromise<T>& lhs, const TPromise<T>& rhs);

//! #TPromise<> inequality operator.
template <class T>
bool operator!=(const TPromise<T>& lhs, const TPromise<T>& rhs);

////////////////////////////////////////////////////////////////////////////////

template <class T>
struct TPromiseSetter
{
    static void Do(TPromise<T> promise, T value)
    {
        promise.Set(std::move(value));
    }
};

template <>
struct TPromiseSetter<void>
{
    static void Do(TPromise<void> promise)
    {
        promise.Set();
    }
};

////////////////////////////////////////////////////////////////////////////////

//! Cancels a given future at the end of the scope.
/*!
 *  \note
 *  Cancelation has no effect if the future is already set.
 */
template <class T>
class TFutureCancelationGuard
{
public:
    explicit TFutureCancelationGuard(TFuture<T> future);
    ~TFutureCancelationGuard();

private:
    TFuture<T> Future_;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

#define FUTURE_INL_H_
#include "future-inl.h"
#undef FUTURE_INL_H_
