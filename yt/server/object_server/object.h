#pragma once

#include "public.h"

#include <ytlib/misc/foreach.h>

#include <server/cell_master/public.h>

namespace NYT {
namespace NObjectServer {

////////////////////////////////////////////////////////////////////////////////

//! Provides a base for all objects in YT server.
class TObjectBase
    : private TNonCopyable
{
public:
    explicit TObjectBase(const TObjectId& id);

    //! Returns the object id.
    const TObjectId& GetId() const;

    //! Returns the object type.
    EObjectType GetType() const;

    //! Increments the object's reference counter.
    /*!
     *  \returns the incremented counter.
     */
    int RefObject();

    //! Decrements the object's reference counter.
    /*!
     *  \note
     *  Objects do not self-destruct, it's callers responsibility to check
     *  if the counter reaches zero.
     *
     *  \returns the decremented counter.
     */
    int UnrefObject();

    //! Increments the object's lock counter.
    /*!
     *  \returns the incremented counter.
     */
    int LockObject();

    //! Decrements the object's lock counter.
    /*!
     *  \returns the decremented counter.
     */
    int UnlockObject();

    //! Sets lock counter to zero.
    void ResetObjectLocks();

    //! Returns the current reference counter.
    int GetObjectRefCounter() const;

    //! Returns the current lock counter.
    int GetObjectLockCounter() const;

    //! Returns True iff the reference counter is non-zero.
    bool IsAlive() const;

    //! Returns True iff the lock counter is non-zero.
    bool IsLocked() const;

    //! Returns True iff the object is either non-versioned or versioned but does not belong to a transaction.
    bool IsTrunk() const;

protected:
    void Save(NCellMaster::TSaveContext& context) const;
    void Load(NCellMaster::TLoadContext& context);

    TObjectId Id;
    int RefCounter;
    int LockCounter;

};

TObjectId GetObjectId(const TObjectBase* object);
bool IsObjectAlive(const TObjectBase* object);
bool CompareObjectsForSerialization(const TObjectBase* lhs, const TObjectBase* rhs);

////////////////////////////////////////////////////////////////////////////////

template <class T>
std::vector<TObjectId> ToObjectIds(
    const T& objects,
    size_t sizeLimit = std::numeric_limits<size_t>::max())
{
    std::vector<TObjectId> result;
    result.reserve(std::min(objects.size(), sizeLimit));
    FOREACH (auto* object, objects) {
        if (result.size() == sizeLimit)
            break;
        result.push_back(object->GetId());
    }
    return result;
}

////////////////////////////////////////////////////////////////////////////////

class TNonversionedObjectBase
    : public TObjectBase
{
public:
    explicit TNonversionedObjectBase(const TObjectId& id);

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NObjectServer
} // namespace NYT

