#include "stdafx.h"
#include "framework.h"

#include <core/misc/protobuf_helpers.h>

#include <ytlib/new_table_client/unversioned_row.h>
#include <core/ytree/convert.h>

namespace NYT {
namespace NVersionedTableClient {
namespace {

using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

void CheckSerialize(TUnversionedRow original)
{
    auto serialized = NYT::ToProto<Stroka>(original);
    auto deserialized =  NYT::FromProto<TUnversionedOwningRow>(serialized);

    ASSERT_EQ(original, deserialized.Get());
}

void CheckSerialize(const TUnversionedOwningRow& original)
{
    CheckSerialize(original.Get());
}

TEST(TUnversionedRowTest, Serialize1)
{
    TUnversionedOwningRowBuilder builder;
    auto row = builder.Finish();
    CheckSerialize(row);
}

TEST(TUnversionedRowTest, Serialize2)
{
    TUnversionedOwningRowBuilder builder;
    builder.AddValue(MakeSentinelValue<TUnversionedValue>(EValueType::Null, 0));
    builder.AddValue(MakeIntegerValue<TUnversionedValue>(42, 1));
    builder.AddValue(MakeDoubleValue<TUnversionedValue>(0.25, 2));
    CheckSerialize(builder.Finish());
}

TEST(TUnversionedRowTest, Serialize3)
{
    // TODO(babenko): cannot test Any type at the moment since CompareRowValues does not work
    // for it.
    TUnversionedOwningRowBuilder builder;
    builder.AddValue(MakeStringValue<TUnversionedValue>("string1", 10));
    builder.AddValue(MakeIntegerValue<TUnversionedValue>(1234, 20));
    builder.AddValue(MakeStringValue<TUnversionedValue>("string2", 30));
    builder.AddValue(MakeDoubleValue<TUnversionedValue>(4321.0, 1000));
    builder.AddValue(MakeStringValue<TUnversionedValue>("", 10000));
    CheckSerialize(builder.Finish());
}

TEST(TUnversionedRowTest, Serialize4)
{
    // TODO(babenko): cannot test Any type at the moment since CompareRowValues does not work
    // for it.
    TUnversionedRowBuilder builder;
    builder.AddValue(MakeUnversionedStringValue("string1"));
    builder.AddValue(MakeStringValue<TUnversionedValue>("string2"));
    //builder.AddValue(MakeDoubleValue<TUnversionedValue>(4321.0, 1000));
    //builder.AddValue(MakeStringValue<TUnversionedValue>("", 10000));
    CheckSerialize(builder.GetRow());
}

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NVersionedTableClient
} // namespace NYT
