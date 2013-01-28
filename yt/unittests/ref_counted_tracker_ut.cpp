#include "stdafx.h"

#define ENABLE_REF_COUNTED_TRACKING

#include <ytlib/misc/common.h>
#include <ytlib/misc/ref_counted_tracker.h>
#include <ytlib/misc/ref_counted.h>
#include <ytlib/misc/new.h>

#include <contrib/testing/framework.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

namespace {

class TSimpleObject
    : public TRefCounted
{
    ui32 Foo;
    ui32 Bar;

public:
    typedef TIntrusivePtr<TSimpleObject> TPtr;

    static i64 GetAliveCount()
    {
        return TRefCountedTracker::Get()->GetObjectsAlive(&typeid(TSimpleObject));
    }

    static i64 GetAllocatedCount()
    {
        return TRefCountedTracker::Get()->GetObjectsAllocated(&typeid(TSimpleObject));
    }
};

} // namespace

////////////////////////////////////////////////////////////////////////////////

TEST(TRefCountedTrackerTest, Simple)
{
    std::vector<TSimpleObject::TPtr> container;
    container.reserve(2000);

    EXPECT_EQ(   0, TSimpleObject::GetAliveCount());
    EXPECT_EQ(   0, TSimpleObject::GetAllocatedCount());

    for (size_t i = 0; i < 1000; ++i) {
        container.push_back(New<TSimpleObject>());
    }

    EXPECT_EQ(1000, TSimpleObject::GetAliveCount());
    EXPECT_EQ(1000, TSimpleObject::GetAllocatedCount());

    for (size_t i = 0; i < 1000; ++i) {
        container.push_back(New<TSimpleObject>());
    }

    EXPECT_EQ(2000, TSimpleObject::GetAliveCount());
    EXPECT_EQ(2000, TSimpleObject::GetAllocatedCount());

    container.resize(1000);

    EXPECT_EQ(1000, TSimpleObject::GetAliveCount());
    EXPECT_EQ(2000, TSimpleObject::GetAllocatedCount());

    container.resize(0);

    EXPECT_EQ(   0, TSimpleObject::GetAliveCount());
    EXPECT_EQ(2000, TSimpleObject::GetAllocatedCount());
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

