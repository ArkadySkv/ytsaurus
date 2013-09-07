#pragma once

#include "public.h"
#include "composite_meta_state.h"

#include <ytlib/concurrency/thread_affinity.h>

namespace NYT {
namespace NMetaState {

////////////////////////////////////////////////////////////////////////////////

//! Default traits for creating values.
template <class TKey, class TValue>
struct TDefaultMetaMapTraits
{
    std::unique_ptr<TValue> Create(const TKey& key) const;
};

////////////////////////////////////////////////////////////////////////////////

//! Map type used to store various meta-state tables.
/*!
 *  \tparam TKey Key type.
 *  \tparam TValue Value type.
 *  \tparam THash Hash function for keys.
 *  \tparam TTraits Traits for creating values.
 *
 *  \note
 *  All public methods must be called from a single thread.
 *
 *  TValue type must have the following methods:
 *  \code
 *      void Save(const TContext& context);
 *      void Load(const TContext& context);
 *  \endcode
 */
template <
    class TKey,
    class TValue,
    class TTraits = TDefaultMetaMapTraits<TKey, TValue>,
    class THash = ::THash<TKey>
>
class TMetaStateMap
{
public:
    typedef TMetaStateMap<TKey, TValue, TTraits, THash> TThis;
    typedef yhash_map<TKey, TValue*, THash> TMap;
    typedef typename TMap::iterator TIterator;
    typedef typename TMap::iterator TConstIterator;
    typedef std::pair<TKey, TValue*> TItem;

    explicit TMetaStateMap(const TTraits& traits = TTraits());

    ~TMetaStateMap();

    //! Inserts a key-value pair.
    /*!
     *  \param key A key to insert.
     *  \param value A value to insert.
     *
     *  \note The map will own the value and will call "delete" for it  when time comes.
     *  \note Fails if the key is already in map.
     */
    void Insert(const TKey& key, TValue* value);

    //! Tries to find a value by its key. The returned value is read-only.
    /*!
     *  \param key A key.
     *  \return A pointer to the value if found, NULL otherwise.
     */
    const TValue* Find(const TKey& key) const;

    //! Tries to find a value by its key.
    //! May return a modifiable copy if snapshot creation is in progress.
    /*!
     * \param key A key.
     * \return A pointer to the value if found, NULL otherwise.
     */
    TValue* Find(const TKey& key);

    //! Returns a read-only value corresponding to the key.
    /*!
     *  In contrast to #Find this method fails if the key does not exist in the map.
     *  \param key A key.
     *  \returns A reference to the value.
     */
    const TValue* Get(const TKey& key) const;

    //! Returns a modifiable value corresponding to the key.
    /*!
     *  In contrast to #Find this method fails if the key does not exist in the map.
     *  \param key A key.
     *  \returns A reference to the value.
     */
    TValue* Get(const TKey& key);

    //! Removes the key from the map and deletes the corresponding value.
    /*!
     *  \param A key.
     *
     *  \note Fails if the key is not in the map.
     */
    void Remove(const TKey& key);

    //! Similar to #Remove but may also be called for missing keys.
    //! Returns |true| if #key was found and removed.
    bool TryRemove(const TKey& key);

    //! Similar to #Remove but does not delete the object and returns the pointer to it instead.
    std::unique_ptr<TValue> Release(const TKey& key);

    //! Checks whether the key exists in the map.
    /*!
     *  \param key A key.
     *  \return True iff the key exists in the map.
     */
    bool Contains(const TKey& key) const;

    //! Clears the map.
    void Clear();

    //! Returns the size of the map.
    int GetSize() const;

    //! Returns all keys that are present in the map.
    std::vector<TKey> GetKeys(size_t sizeLimit = Max<size_t>()) const;

    //! Returns all values that are present in the map.
    std::vector<TValue*> GetValues(size_t sizeLimit = Max<size_t>()) const;

    //! (Unordered) begin()-iterator.
    /*!
     *  \note
     *  This call is potentially dangerous!
     *  The user must understand its semantics and call it at its own risk.
     *  Iteration is only possible when no snapshot is being created.
     *  A typical use-case is to iterate over the items right after reading a snapshot.
     */
    TIterator Begin();

    //! (Unordered) end()-iterator.
    /*!
     *  See the note for #Begin.
     */
    TIterator End();

    //! (Unordered) const begin()-iterator.
    /*!
     *  See the note for #Begin.
     */
    TConstIterator Begin() const;

    //! (Unordered) const end()-iterator.
    /*!
     *  See the note for #Begin.
     */
    TConstIterator End() const;

    void SaveKeys(NMetaState::TSaveContext& context) const;
        
    template <class TContext>
    void SaveValues(TContext& context) const;

    void LoadKeys(NMetaState::TLoadContext& context);

    template <class TContext>
    void LoadValues(TContext& context);

private:
    //! Slot for the thread in which all the public methods are called.
    DECLARE_THREAD_AFFINITY_SLOT(UserThread);

    TMap Map;

    //! Traits for creating values.
    TTraits Traits;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NMetaState
} // namespace NYT

// Foreach interop.

template <class TKey, class TValue, class THash>
inline auto begin(NYT::NMetaState::TMetaStateMap<TKey, TValue, THash>& collection) -> decltype(collection.Begin())
{
    return collection.Begin();
}

template <class TKey, class TValue, class THash>
inline auto end(NYT::NMetaState::TMetaStateMap<TKey, TValue, THash>& collection) -> decltype(collection.End())
{
    return collection.End();
}

////////////////////////////////////////////////////////////////////////////////

#define DECLARE_METAMAP_ACCESSORS(entityName, entityType, idType) \
    entityType* Find ## entityName(const idType& id); \
    entityType* Get ## entityName(const idType& id); \
    std::vector<entityType*> Get ## entityName ## s(size_t sizeLimit = Max<size_t>()); \
    int Get ## entityName ## Count() const;

#define DEFINE_METAMAP_ACCESSORS(declaringType, entityName, entityType, idType, map) \
    entityType* declaringType::Find ## entityName(const idType& id) \
    { \
        return (map).Find(id); \
    } \
    \
    entityType* declaringType::Get ## entityName(const idType& id) \
    { \
        return (map).Get(id); \
    } \
    \
    std::vector<entityType*> declaringType::Get ## entityName ##s(size_t sizeLimit) \
    { \
        return (map).GetValues(sizeLimit); \
    } \
    \
    int declaringType::Get ## entityName ## Count() const \
    { \
        return (map).GetSize(); \
    }

#define DELEGATE_METAMAP_ACCESSORS(declaringType, entityName, entityType, idType, delegateTo) \
    entityType* declaringType::Find ## entityName(const idType& id) \
    { \
        return (delegateTo).Find ## entityName(id); \
    } \
    \
    entityType* declaringType::Get ## entityName(const idType& id) \
    { \
        return (delegateTo).Get ## entityName(id); \
    } \
    \
    std::vector<entityType*> declaringType::Get ## entityName ## s(size_t sizeLimit) \
    { \
        return (delegateTo).Get ## entityName ## s(sizeLimit); \
    } \
    \
    int declaringType::Get ## entityName ## Count() const \
    { \
        return (delegateTo).Get ## entityName ## Count(); \
    }

////////////////////////////////////////////////////////////////////////////////

#define MAP_INL_H_
#include "map-inl.h"
#undef MAP_INL_H_
