#pragma once

#include "common.h"

#include <ytlib/misc/enum.h>
#include <ytlib/misc/foreach.h>
#include <ytlib/misc/serialize.h>
#include <ytlib/misc/thread_affinity.h>
#include <ytlib/actions/future.h>

#include <util/ysaveload.h>

namespace NYT {
namespace NMetaState {

////////////////////////////////////////////////////////////////////////////////

//! Default traits for cloning, saving and loading values.
template <class TKey, class TValue>
struct TDefaultMetaMapTraits
{
    TAutoPtr<TValue> Create(const TKey& key) const;
};

//! Snapshottable map used to store various meta-state tables.
/*!
 *  \tparam TKey Key type.
 *  \tparam TValue Value type.
 *  \tparam THash Hash function for keys.
 *  \tparam TTraits Traits for cloning, saving and loading values.
 * 
 *  \note
 *  All public methods must be called from a single thread.
 * 
 *  TODO: this is not true, write about Traits
 *  TValue type must have the following methods:
 *          TAutoPtr<TValue> Clone();
 *          void Save(TOutputStream* output);
 *          static TAutoPtr<TValue> Load(TInputStream* input);
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

    explicit TMetaStateMap(TTraits traits = TTraits());

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
    const TValue& Get(const TKey& key) const;

    //! Returns a modifiable value corresponding to the key.
    /*!
     *  In contrast to #Find this method fails if the key does not exist in the map.
     *  \param key A key.
     *  \returns A reference to the value.
     */
    TValue& Get(const TKey& key);

    //! Removes the key from the map and deletes the corresponding value.
    /*!
     *  \param A key.
     *  
     *  \note Fails if the key is not in the map.
     */
    void Remove(const TKey& key);

	//! Similar to #Remove but does not delete the object and returns the pointer to it instead.
    TValue* Release(const TKey& key);

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
    yvector<TKey> GetKeys(size_t sizeLimit = Max<size_t>()) const;

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

    //! Asynchronously saves the map to the stream.
    /*!
     *  This method saves the snapshot of the map as it is seen at the moment of
     *  the invocation. All further updates are accepted but are kept in-memory.
     *  
     *  \param invoker Invoker used to perform the heavy lifting.
     *  \param output Output stream.
     *  \return An asynchronous result indicating that the snapshot is saved.
     */
    void SaveKeys(TOutputStream* output) const;

    void SaveValues(TOutputStream* output) const;

    //! Synchronously loads the map from the stream.
    /*!
     * \param input Input stream.
     */
    void LoadKeys(TInputStream* input);

    template <class TContext>
    void LoadValues(TInputStream* input, const TContext& context);
    
private:
    //! Slot for the thread in which all the public methods are called.
    DECLARE_THREAD_AFFINITY_SLOT(UserThread);
    
    /*!
     * When no snapshot is being written this is the actual map we're working with.
     * When a snapshot is being created this map is kept read-only and
     * #PatchMap is used to store the changes.
     */
    TMap Map;

    //! Traits for cloning, saving and loading values.
    TTraits Traits;

    //! Current map size.
    int Size;
    
    typedef TPair<TKey, TValue*> TItem;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NMetaState
} // namespace NYT


namespace NYT {
namespace NForeach {

//! Provides a begin-like iterator for #FOREACH macro.
template <class TKey, class TValue, class THash>
inline auto Begin(NMetaState::TMetaStateMap<TKey, TValue, THash>& collection) -> decltype(collection.Begin())
{
    return collection.Begin();
}

//! Provides an end-like iterator for #FOREACH macro.
template <class TKey, class TValue, class THash>
inline auto End(NMetaState::TMetaStateMap<TKey, TValue, THash>& collection) -> decltype(collection.End())
{
    return collection.End();
}
 
} // namespace NForeach
} // namespace NYT

////////////////////////////////////////////////////////////////////////////////

#define DECLARE_METAMAP_ACCESSORS(entityName, entityType, idType) \
    const entityType* Find ## entityName(const idType& id) const; \
    entityType* Find ## entityName(const idType& id); \
    const entityType& Get ## entityName(const idType& id) const; \
    entityType& Get ## entityName(const idType& id); \
    yvector<idType> Get ## entityName ## Ids(size_t sizeLimit = Max<size_t>()); \
    int Get ## entityName ## Count() const;

#define DEFINE_METAMAP_ACCESSORS(declaringType, entityName, entityType, idType, map) \
    const entityType* declaringType::Find ## entityName(const idType& id) const \
    { \
        return (map).Find(id); \
    } \
    \
    entityType* declaringType::Find ## entityName(const idType& id) \
    { \
        return (map).Find(id); \
    } \
    \
    const entityType& declaringType::Get ## entityName(const idType& id) const \
    { \
        return (map).Get(id); \
    } \
    \
    entityType& declaringType::Get ## entityName(const idType& id) \
    { \
        return (map).Get(id); \
    } \
    \
    yvector<idType> declaringType::Get ## entityName ## Ids(size_t sizeLimit) \
    { \
        return (map).GetKeys(sizeLimit); \
    } \
    \
    int declaringType::Get ## entityName ## Count() const \
    { \
        return (map).GetSize(); \
    }

#define DELEGATE_METAMAP_ACCESSORS(declaringType, entityName, entityType, idType, delegateTo) \
    const entityType* declaringType::Find ## entityName(const idType& id) const \
    { \
        return (delegateTo).Find ## entityName(id); \
    } \
    \
    entityType* declaringType::Find ## entityName(const idType& id) \
    { \
        return (delegateTo).Find ## entityName(id); \
    } \
    \
    const entityType& declaringType::Get ## entityName(const idType& id) const \
    { \
        return (delegateTo).Get ## entityName(id); \
    } \
    \
    entityType& declaringType::Get ## entityName(const idType& id) \
    { \
        return (delegateTo).Get ## entityName(id); \
    } \
    \
    yvector<idType> declaringType::Get ## entityName ## Ids(size_t sizeLimit) \
    { \
        return (delegateTo).Get ## entityName ## Ids(sizeLimit); \
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
