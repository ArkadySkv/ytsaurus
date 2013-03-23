#ifndef MAP_INL_H_
#error "Direct inclusion of this file is not allowed, include map.h"
#endif
#undef MAP_INL_H_

namespace NYT {
namespace NMetaState {

////////////////////////////////////////////////////////////////////////////////

template <class TKey, class TValue>
TAutoPtr<TValue> TDefaultMetaMapTraits<TKey, TValue>::Create(const TKey& key) const
{
    return new TValue(key);
}

////////////////////////////////////////////////////////////////////////////////

template <class TKey, class TValue, class TTraits, class THash>
TMetaStateMap<TKey, TValue, TTraits, THash>::TMetaStateMap(const TTraits& traits)
    : Traits(traits)
{ }

template <class TKey, class TValue, class TTraits, class THash>
TMetaStateMap<TKey, TValue, TTraits, THash>::~TMetaStateMap()
{
    FOREACH (const auto& pair, Map) {
        delete pair.second;
    }
    Map.clear();
}

template <class TKey, class TValue, class TTraits, class THash>
void TMetaStateMap<TKey, TValue, TTraits, THash>::Insert(const TKey& key, TValue* value)
{
    VERIFY_THREAD_AFFINITY(UserThread);

    YASSERT(value);
    YCHECK(Map.insert(std::make_pair(key, value)).second);
}

template <class TKey, class TValue, class TTraits, class THash>
const TValue* TMetaStateMap<TKey, TValue, TTraits, THash>::Find(const TKey& key) const
{
    VERIFY_THREAD_AFFINITY(UserThread);

    auto it = Map.find(key);
    return it == Map.end() ? NULL : it->second;
}

template <class TKey, class TValue, class TTraits, class THash>
TValue* TMetaStateMap<TKey, TValue, TTraits, THash>::Find(const TKey& key)
{
    VERIFY_THREAD_AFFINITY(UserThread);

    auto it = Map.find(key);
    return it == Map.end() ? NULL : it->second;
}

template <class TKey, class TValue, class TTraits, class THash>
const TValue* TMetaStateMap<TKey, TValue, TTraits, THash>::Get(const TKey& key) const
{
    VERIFY_THREAD_AFFINITY(UserThread);

    auto* value = Find(key);
    YCHECK(value);
    return value;
}

template <class TKey, class TValue, class TTraits, class THash>
TValue* TMetaStateMap<TKey, TValue, TTraits, THash>::Get(const TKey& key)
{
    VERIFY_THREAD_AFFINITY(UserThread);

    auto* value = Find(key);
    YCHECK(value);
    return value;
}

template <class TKey, class TValue, class TTraits, class THash>
void TMetaStateMap<TKey, TValue, TTraits, THash>::Remove(const TKey& key)
{
    VERIFY_THREAD_AFFINITY(UserThread);

    YCHECK(TryRemove(key));
}

template <class TKey, class TValue, class TTraits, class THash>
bool TMetaStateMap<TKey, TValue, TTraits, THash>::TryRemove(const TKey& key)
{
    VERIFY_THREAD_AFFINITY(UserThread);

    auto it = Map.find(key);
    if (it == Map.end()) {
        return false;
    }
    delete it->second;
    Map.erase(it);
    return true;
}

template <class TKey, class TValue, class TTraits, class THash>
TAutoPtr<TValue> TMetaStateMap<TKey, TValue, TTraits, THash>::Release(const TKey& key)
{
    VERIFY_THREAD_AFFINITY(UserThread);

    auto it = Map.find(key);
    YASSERT(it != Map.end());
    auto* value = it->second;
    Map.erase(it);
    return value;
}

template <class TKey, class TValue, class TTraits, class THash>
bool TMetaStateMap<TKey, TValue, TTraits, THash>::Contains(const TKey& key) const
{
    VERIFY_THREAD_AFFINITY(UserThread);

    return Find(key);
}

template <class TKey, class TValue, class TTraits, class THash>
void TMetaStateMap<TKey, TValue, TTraits, THash>::Clear()
{
    VERIFY_THREAD_AFFINITY(UserThread);

    FOREACH (const auto& pair, Map) {
        delete pair.second;
    }
    Map.clear();
}

template <class TKey, class TValue, class TTraits, class THash>
int TMetaStateMap<TKey, TValue, TTraits, THash>::GetSize() const
{
    VERIFY_THREAD_AFFINITY(UserThread);

    return static_cast<int>(Map.size());
}

template <class TKey, class TValue, class TTraits, class THash>
std::vector<TKey> TMetaStateMap<TKey, TValue, TTraits, THash>::GetKeys(size_t sizeLimit) const
{
    VERIFY_THREAD_AFFINITY(UserThread);

    std::vector<TKey> keys;
    keys.reserve(std::min(Map.size(), sizeLimit));

    FOREACH (const auto& pair, Map) {
        if (keys.size() == sizeLimit) {
            break;
        }
        keys.push_back(pair.first);
    }

    YCHECK(keys.size() == std::min(Map.size(), sizeLimit));

    return keys;
}

template <class TKey, class TValue, class TTraits, class THash>
std::vector<TValue*> TMetaStateMap<TKey, TValue, TTraits, THash>::GetValues(size_t sizeLimit) const
{
    VERIFY_THREAD_AFFINITY(UserThread);

    std::vector<TValue*> values;
    values.reserve(std::min(Map.size(), sizeLimit));

    FOREACH (auto& pair, Map) {
        values.push_back(pair.second);
        if (values.size() == sizeLimit) {
            break;
        }
    }

    YCHECK(values.size() == std::min(Map.size(), sizeLimit));

    return values;
}

template <class TKey, class TValue, class TTraits, class THash>
typename TMetaStateMap<TKey, TValue, TTraits, THash>::TIterator
TMetaStateMap<TKey, TValue, TTraits, THash>::Begin()
{
    VERIFY_THREAD_AFFINITY(UserThread);

    return Map.begin();
}

template <class TKey, class TValue, class TTraits, class THash>
typename TMetaStateMap<TKey, TValue, TTraits, THash>::TIterator
TMetaStateMap<TKey, TValue, TTraits, THash>::End()
{
    VERIFY_THREAD_AFFINITY(UserThread);

    return Map.end();
}

template <class TKey, class TValue, class TTraits, class THash>
typename TMetaStateMap<TKey, TValue, TTraits, THash>::TConstIterator
TMetaStateMap<TKey, TValue, TTraits, THash>::Begin() const
{
    VERIFY_THREAD_AFFINITY(UserThread);

    return Map.begin();
}

template <class TKey, class TValue, class TTraits, class THash>
typename TMetaStateMap<TKey, TValue, TTraits, THash>::TConstIterator
TMetaStateMap<TKey, TValue, TTraits, THash>::End() const
{
    VERIFY_THREAD_AFFINITY(UserThread);

    return Map.end();
}

template <class TKey, class TValue, class TTraits, class THash>
void TMetaStateMap<TKey, TValue, TTraits, THash>::LoadKeys(const TLoadContext& context)
{
    VERIFY_THREAD_AFFINITY(UserThread);

    auto* input = context.GetInput();

    Map.clear();
    size_t size = ::LoadSize(input);

    TKey previousKey;
    for (size_t index = 0; index < size; ++index) {
        TKey key;
        ::Load(input, key);

        YCHECK(index == 0 || previousKey < key);

        previousKey = key;

        auto value = Traits.Create(key);
        YCHECK(Map.insert(std::make_pair(key, value.Release())).second);
    }
}

template <class TKey, class TValue, class TTraits, class THash>
template <class TContext>
void TMetaStateMap<TKey, TValue, TTraits, THash>::LoadValues(const TContext& context)
{
    VERIFY_THREAD_AFFINITY(UserThread);

    std::vector<TKey> keys;
    keys.reserve(Map.size());
    FOREACH (const auto& pair, Map) {
        keys.push_back(pair.first);
    }
    std::sort(keys.begin(), keys.end());

    FOREACH (const auto& key, keys) {
        auto it = Map.find(key);
        YCHECK(it != Map.end());
        it->second->Load(context);
    }
}

template <class TKey, class TValue, class TTraits, class THash>
void TMetaStateMap<TKey, TValue, TTraits, THash>::SaveKeys(const TSaveContext& context) const
{
    auto* output = context.GetOutput();

    ::SaveSize(output, Map.size());

    std::vector<TKey> keys;
    keys.reserve(Map.size());
    FOREACH (const auto& pair, Map) {
        keys.push_back(pair.first);
    }
    std::sort(keys.begin(), keys.end());

    FOREACH (const auto& key, keys) {
        ::Save(output, key);
    }
}

template <class TKey, class TValue, class TTraits, class THash>
template <class TContext>
void TMetaStateMap<TKey, TValue, TTraits, THash>::SaveValues(const TContext& context) const
{
    std::vector<TItem> items(Map.begin(), Map.end());
    std::sort(
        items.begin(),
        items.end(),
        [] (const TItem& lhs, const TItem& rhs) {
            return lhs.first < rhs.first;
        });

    FOREACH (const auto& item, items) {
        item.second->Save(context);
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NMetaState
} // namespace NYT
