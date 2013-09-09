#include "stdafx.h"

#include <core/ytree/fluent.h>
#include <core/ytree/yson_consumer-mock.h>

#include <contrib/testing/framework.h>

using ::testing::Types;
using ::testing::InSequence;
using ::testing::StrictMock;

// TODO(sandello): Fix this test under clang.

namespace NYT {
namespace NYTree {
#ifndef __clang__
// String-like Scalars {{{
////////////////////////////////////////////////////////////////////////////////

template <class T>
class TYTreeFluentStringScalarTest
    : public ::testing::Test
{
};

TYPED_TEST_CASE_P(TYTreeFluentStringScalarTest);
TYPED_TEST_P(TYTreeFluentStringScalarTest, Ok)
{
    StrictMock<TMockYsonConsumer> mock;
    InSequence dummy;

    EXPECT_CALL(mock, OnStringScalar("Hello World"));

    TypeParam passedScalar = "Hello World";
    BuildYsonFluently(&mock)
        .Value(passedScalar);
}

typedef Types<const char*, Stroka> TYTreeFluentStringScalarTestTypes;

REGISTER_TYPED_TEST_CASE_P(TYTreeFluentStringScalarTest, Ok);
INSTANTIATE_TYPED_TEST_CASE_P(
    TypeParametrized,
    TYTreeFluentStringScalarTest,
    TYTreeFluentStringScalarTestTypes
);

////////////////////////////////////////////////////////////////////////////////
// }}}

// Integer-like Scalars {{{
////////////////////////////////////////////////////////////////////////////////

template <class T>
class TYTreeFluentIntegerScalarTest
    : public ::testing::Test
{
};

TYPED_TEST_CASE_P(TYTreeFluentIntegerScalarTest);
TYPED_TEST_P(TYTreeFluentIntegerScalarTest, Ok)
{
    StrictMock<TMockYsonConsumer> mock;
    InSequence dummy;

    EXPECT_CALL(mock, OnIntegerScalar(42));

    TypeParam passedScalar = 42;
    BuildYsonFluently(&mock)
        .Value(passedScalar);
}

typedef Types<i8, ui8, i16, ui16, i32, ui32, i64>
    TYTreeFluentIntegerScalarTestTypes;
REGISTER_TYPED_TEST_CASE_P(TYTreeFluentIntegerScalarTest, Ok);
INSTANTIATE_TYPED_TEST_CASE_P(TypeParametrized, TYTreeFluentIntegerScalarTest,
    TYTreeFluentIntegerScalarTestTypes
);

////////////////////////////////////////////////////////////////////////////////
// }}}

// Float-like Scalars {{{
////////////////////////////////////////////////////////////////////////////////

template <class T>
class TYTreeFluentFloatScalarTest
    : public ::testing::Test
{
};

TYPED_TEST_CASE_P(TYTreeFluentFloatScalarTest);
TYPED_TEST_P(TYTreeFluentFloatScalarTest, Ok)
{
    StrictMock<TMockYsonConsumer> mock;
    InSequence dummy;

    EXPECT_CALL(mock, OnDoubleScalar(::testing::DoubleEq(3.14f)));

    TypeParam passedScalar = 3.14f;
    BuildYsonFluently(&mock)
        .Value(passedScalar);
}

typedef Types<float, double>
    TYTreeFluentFloatScalarTestTypes;
REGISTER_TYPED_TEST_CASE_P(TYTreeFluentFloatScalarTest, Ok);
INSTANTIATE_TYPED_TEST_CASE_P(TypeParametrized, TYTreeFluentFloatScalarTest,
    TYTreeFluentFloatScalarTestTypes
);

////////////////////////////////////////////////////////////////////////////////
// }}}

// Map {{{
////////////////////////////////////////////////////////////////////////////////

TEST(TYTreeFluentMapTest, Empty)
{
    StrictMock<TMockYsonConsumer> mock;
    InSequence dummy;

    EXPECT_CALL(mock, OnBeginMap());
    EXPECT_CALL(mock, OnEndMap());

    BuildYsonFluently(&mock)
        .BeginMap()
        .EndMap();
}

TEST(TYTreeFluentMapTest, Simple)
{
    StrictMock<TMockYsonConsumer> mock;
    InSequence dummy;

    EXPECT_CALL(mock, OnBeginMap());
    EXPECT_CALL(mock, OnKeyedItem("foo"));
    EXPECT_CALL(mock, OnIntegerScalar(10));
    EXPECT_CALL(mock, OnKeyedItem("bar"));
    EXPECT_CALL(mock, OnIntegerScalar(20));
    EXPECT_CALL(mock, OnEndMap());

    BuildYsonFluently(&mock)
        .BeginMap()
            .Item("foo")
            .Value(10)

            .Item("bar")
            .Value(20)
        .EndMap();
}

TEST(TYTreeFluentMapTest, Items)
{
    StrictMock<TMockYsonConsumer> mock;
    InSequence dummy;

    auto node = ConvertToNode(TYsonString("{bar = 10}"));

    EXPECT_CALL(mock, OnBeginMap());
    EXPECT_CALL(mock, OnKeyedItem("bar"));
    EXPECT_CALL(mock, OnIntegerScalar(10));
    EXPECT_CALL(mock, OnEndMap());

    BuildYsonFluently(&mock)
        .BeginMap()
            .Items(node->AsMap())
        .EndMap();
}


TEST(TYTreeFluentMapTest, Nested)
{
    StrictMock<TMockYsonConsumer> mock;
    InSequence dummy;

    EXPECT_CALL(mock, OnBeginMap());
    EXPECT_CALL(mock, OnKeyedItem("foo"));
    EXPECT_CALL(mock, OnBeginMap());
    EXPECT_CALL(mock, OnKeyedItem("xxx"));
    EXPECT_CALL(mock, OnIntegerScalar(17));
    EXPECT_CALL(mock, OnEndMap());
    EXPECT_CALL(mock, OnKeyedItem("bar"));
    EXPECT_CALL(mock, OnIntegerScalar(42));
    EXPECT_CALL(mock, OnEndMap());

    BuildYsonFluently(&mock)
        .BeginMap()
            .Item("foo")
            .BeginMap()
                .Item("xxx")
                .Value(17)
            .EndMap()

            .Item("bar")
            .Value(42)
        .EndMap();
}

////////////////////////////////////////////////////////////////////////////////
// }}}

// List {{{
////////////////////////////////////////////////////////////////////////////////

TEST(TYTreeFluentListTest, Empty)
{
    StrictMock<TMockYsonConsumer> mock;
    InSequence dummy;

    EXPECT_CALL(mock, OnBeginList());
    EXPECT_CALL(mock, OnEndList());

    BuildYsonFluently(&mock)
        .BeginList()
        .EndList();
}

TEST(TYTreeFluentListTest, Simple)
{
    StrictMock<TMockYsonConsumer> mock;
    InSequence dummy;

    EXPECT_CALL(mock, OnBeginList());
    EXPECT_CALL(mock, OnListItem());
    EXPECT_CALL(mock, OnStringScalar("foo"));
    EXPECT_CALL(mock, OnListItem());
    EXPECT_CALL(mock, OnStringScalar("bar"));
    EXPECT_CALL(mock, OnEndList());

    BuildYsonFluently(&mock)
        .BeginList()
            .Item()
            .Value("foo")

            .Item()
            .Value("bar")
        .EndList();
}

TEST(TYTreeFluentListTest, Items)
{
    StrictMock<TMockYsonConsumer> mock;
    InSequence dummy;

    auto node = ConvertToNode(TYsonString("[10; 20; 30]"));

    EXPECT_CALL(mock, OnBeginList());
    EXPECT_CALL(mock, OnListItem());
    EXPECT_CALL(mock, OnIntegerScalar(10));
    EXPECT_CALL(mock, OnListItem());
    EXPECT_CALL(mock, OnIntegerScalar(20));
    EXPECT_CALL(mock, OnListItem());
    EXPECT_CALL(mock, OnIntegerScalar(30));
    EXPECT_CALL(mock, OnEndList());

    BuildYsonFluently(&mock)
        .BeginList()
            .Items(node->AsList())
        .EndList();
}

TEST(TYTreeFluentListTest, Nested)
{
    StrictMock<TMockYsonConsumer> mock;
    InSequence dummy;

    EXPECT_CALL(mock, OnBeginList());
    EXPECT_CALL(mock, OnListItem());
    EXPECT_CALL(mock, OnBeginList());
    EXPECT_CALL(mock, OnListItem());
    EXPECT_CALL(mock, OnStringScalar("foo"));
    EXPECT_CALL(mock, OnEndList());
    EXPECT_CALL(mock, OnListItem());
    EXPECT_CALL(mock, OnStringScalar("bar"));
    EXPECT_CALL(mock, OnEndList());

    BuildYsonFluently(&mock)
        .BeginList()
            .Item()
            .BeginList()
                .Item()
                .Value("foo")
            .EndList()

            .Item()
            .Value("bar")
        .EndList();
}

////////////////////////////////////////////////////////////////////////////////
// }}}

TEST(TYTreeFluentTest, Complex)
{
    StrictMock<TMockYsonConsumer> mock;
    InSequence dummy;

    EXPECT_CALL(mock, OnBeginList());

    EXPECT_CALL(mock, OnListItem());
    EXPECT_CALL(mock, OnBeginAttributes());
    EXPECT_CALL(mock, OnKeyedItem("attr1"));
    EXPECT_CALL(mock, OnIntegerScalar(-1));
    EXPECT_CALL(mock, OnKeyedItem("attr2"));
    EXPECT_CALL(mock, OnIntegerScalar(-2));
    EXPECT_CALL(mock, OnEndAttributes());
    EXPECT_CALL(mock, OnIntegerScalar(42));

    EXPECT_CALL(mock, OnListItem());
    EXPECT_CALL(mock, OnIntegerScalar(17));

    EXPECT_CALL(mock, OnListItem());
    EXPECT_CALL(mock, OnBeginList());
    EXPECT_CALL(mock, OnEndList());

    EXPECT_CALL(mock, OnListItem());
    EXPECT_CALL(mock, OnBeginAttributes());
    EXPECT_CALL(mock, OnKeyedItem("hot"));
    EXPECT_CALL(mock, OnStringScalar("chocolate"));
    EXPECT_CALL(mock, OnEndAttributes());
    EXPECT_CALL(mock, OnBeginList());
    EXPECT_CALL(mock, OnListItem());
    EXPECT_CALL(mock, OnStringScalar("hello"));
    EXPECT_CALL(mock, OnListItem());
    EXPECT_CALL(mock, OnStringScalar("world"));
    EXPECT_CALL(mock, OnEndList());

    EXPECT_CALL(mock, OnListItem());
    EXPECT_CALL(mock, OnBeginMap());
    EXPECT_CALL(mock, OnKeyedItem("aaa"));
    EXPECT_CALL(mock, OnIntegerScalar(1));
    EXPECT_CALL(mock, OnKeyedItem("bbb"));
    EXPECT_CALL(mock, OnIntegerScalar(2));
    EXPECT_CALL(mock, OnEndMap());

    EXPECT_CALL(mock, OnListItem());
    EXPECT_CALL(mock, OnBeginAttributes());
    EXPECT_CALL(mock, OnKeyedItem("type"));
    EXPECT_CALL(mock, OnStringScalar("extra"));
    EXPECT_CALL(mock, OnEndAttributes());
    EXPECT_CALL(mock, OnEntity());

    EXPECT_CALL(mock, OnEndList());

    BuildYsonFluently(&mock)
        .BeginList()
            // 0
            .Item()
            .BeginAttributes()
                .Item("attr1").Value(-1)
                .Item("attr2").Value(-2)
            .EndAttributes()
            .Value(42)

            // 1
            .Item()
            .Value(17)

            // 2
            .Item()
            .BeginList()
            .EndList()

            // 3
            .Item()
            .BeginAttributes()
                .Item("hot").Value("chocolate")
            .EndAttributes()
            .BeginList()
                .Item().Value("hello")
                .Item().Value("world")
            .EndList()

            // 4
            .Item()
            .BeginMap()
                .Item("aaa").Value(1)
                .Item("bbb").Value(2)
            .EndMap()

            // 5
            .Item()
            .BeginAttributes()
                .Item("type").Value("extra")
            .EndAttributes()
            .Entity()
        .EndList();
}

////////////////////////////////////////////////////////////////////////////////
#endif
} // namespace NYTree
} // namespace NYT
