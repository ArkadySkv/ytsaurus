#include "stdafx.h"

#include <ytlib/misc/configurable.h>
#include <ytlib/ytree/tree_builder.h>
#include <ytlib/ytree/ephemeral.h>
#include <ytlib/ytree/fluent.h>
#include <ytlib/ytree/yson_writer.h>
#include <ytlib/ytree/tree_visitor.h>
#include <ytlib/ytree/serialize.h>

#include <contrib/testing/framework.h>

namespace NYT {

using namespace NYTree;

DECLARE_ENUM(ETestEnum,
    (Value0)
    (Value1)
    (Value2)
);

struct TTestSubconfig
    : public TConfigurable
{
    typedef TIntrusivePtr<TTestSubconfig> TPtr;

    int MyInt;
    bool MyBool;
    yvector<Stroka> MyStringList;
    ETestEnum MyEnum;

    TTestSubconfig()
    {
        Register("my_int", MyInt).Default(100).InRange(95, 105);
        Register("my_bool", MyBool).Default(false);
        Register("my_string_list", MyStringList).Default();
        Register("my_enum", MyEnum).Default(ETestEnum::Value1);
    }
};

struct TTestConfig
    : public TConfigurable
{
    typedef TIntrusivePtr<TTestConfig> TPtr;
    
    Stroka MyString;
    TTestSubconfig::TPtr Subconfig;
    yvector<TTestSubconfig::TPtr> SubconfigList;
    yhash_map<Stroka, TTestSubconfig::TPtr> SubconfigMap;

    TTestConfig()
    {
        Register("my_string", MyString).NonEmpty();
        Register("sub", Subconfig).DefaultNew();
        Register("sub_list", SubconfigList).Default();
        Register("sub_map", SubconfigMap).Default();
    }
};

void TestCompleteSubconfig(TTestSubconfig* subconfig)
{
    EXPECT_EQ(99, subconfig->MyInt);
    EXPECT_IS_TRUE(subconfig->MyBool);
    EXPECT_EQ(3, subconfig->MyStringList.ysize());
    EXPECT_EQ("ListItem0", subconfig->MyStringList[0]);
    EXPECT_EQ("ListItem1", subconfig->MyStringList[1]);
    EXPECT_EQ("ListItem2", subconfig->MyStringList[2]);
    EXPECT_EQ(ETestEnum::Value2, subconfig->MyEnum);
}

TEST(TConfigTest, Complete)
{
    auto builder = CreateBuilderFromFactory(GetEphemeralNodeFactory());
    builder->BeginTree();
    BuildYsonFluently(~builder)
        .BeginMap()
            .Item("my_string").Scalar("TestString")
            .Item("sub").BeginMap()
                .Item("my_int").Scalar(99)
                .Item("my_bool").Scalar(true)
                .Item("my_enum").Scalar("Value2")
                .Item("my_string_list").BeginList()
                    .Item().Scalar("ListItem0")
                    .Item().Scalar("ListItem1")
                    .Item().Scalar("ListItem2")
                .EndList()
            .EndMap()
            .Item("sub_list").BeginList()
                .Item().BeginMap()
                    .Item("my_int").Scalar(99)
                    .Item("my_bool").Scalar(true)
                    .Item("my_enum").Scalar("Value2")
                    .Item("my_string_list").BeginList()
                        .Item().Scalar("ListItem0")
                        .Item().Scalar("ListItem1")
                        .Item().Scalar("ListItem2")
                    .EndList()
                .EndMap()
                .Item().BeginMap()
                    .Item("my_int").Scalar(99)
                    .Item("my_bool").Scalar(true)
                    .Item("my_enum").Scalar("Value2")
                    .Item("my_string_list").BeginList()
                        .Item().Scalar("ListItem0")
                        .Item().Scalar("ListItem1")
                        .Item().Scalar("ListItem2")
                    .EndList()
                .EndMap()            
            .EndList()
            .Item("sub_map").BeginMap()
                .Item("sub1").BeginMap()
                    .Item("my_int").Scalar(99)
                    .Item("my_bool").Scalar(true)
                    .Item("my_enum").Scalar("Value2")
                    .Item("my_string_list").BeginList()
                        .Item().Scalar("ListItem0")
                        .Item().Scalar("ListItem1")
                        .Item().Scalar("ListItem2")
                    .EndList()
                .EndMap()
                .Item("sub2").BeginMap()
                    .Item("my_int").Scalar(99)
                    .Item("my_bool").Scalar(true)
                    .Item("my_enum").Scalar("Value2")
                    .Item("my_string_list").BeginList()
                        .Item().Scalar("ListItem0")
                        .Item().Scalar("ListItem1")
                        .Item().Scalar("ListItem2")
                    .EndList()
                .EndMap()
            .EndMap()
        .EndMap();
    auto configNode = builder->EndTree();

    auto config = New<TTestConfig>();
    config->Load(~configNode->AsMap());
    
    EXPECT_EQ("TestString", config->MyString);
    TestCompleteSubconfig(~config->Subconfig);
    EXPECT_EQ(2, config->SubconfigList.ysize());
    TestCompleteSubconfig(~config->SubconfigList[0]);
    TestCompleteSubconfig(~config->SubconfigList[1]);
    EXPECT_EQ(2, config->SubconfigMap.ysize());
    auto it1 = config->SubconfigMap.find("sub1");
    EXPECT_IS_FALSE(it1 == config->SubconfigMap.end());
    TestCompleteSubconfig(~it1->Second());
    auto it2 = config->SubconfigMap.find("sub2");
    EXPECT_IS_FALSE(it2 == config->SubconfigMap.end());
    TestCompleteSubconfig(~it2->Second());
}

TEST(TConfigTest, MissingParameter)
{
    auto builder = CreateBuilderFromFactory(GetEphemeralNodeFactory());
    builder->BeginTree();
    BuildYsonFluently(~builder)
        .BeginMap()
            .Item("my_string").Scalar("TestString")
            .Item("sub").BeginMap()
                .Item("my_bool").Scalar(true)
            .EndMap()
        .EndMap();
    auto configNode = builder->EndTree();

    auto config = New<TTestConfig>();
    config->Load(~configNode->AsMap());

    EXPECT_EQ("TestString", config->MyString);
    EXPECT_EQ(100, config->Subconfig->MyInt);
    EXPECT_IS_TRUE(config->Subconfig->MyBool);
    EXPECT_EQ(0, config->Subconfig->MyStringList.ysize());
    EXPECT_EQ(ETestEnum::Value1, config->Subconfig->MyEnum);
    EXPECT_EQ(0, config->SubconfigList.ysize());
    EXPECT_EQ(0, config->SubconfigMap.ysize());
}

TEST(TConfigTest, MissingSubconfig)
{
    auto builder = CreateBuilderFromFactory(GetEphemeralNodeFactory());
    builder->BeginTree();
    BuildYsonFluently(~builder)
        .BeginMap()
            .Item("my_string").Scalar("TestString")
        .EndMap();
    auto configNode = builder->EndTree();

    auto config = New<TTestConfig>();
    config->Load(~configNode->AsMap());

    EXPECT_EQ("TestString", config->MyString);
    EXPECT_EQ(100, config->Subconfig->MyInt);
    EXPECT_IS_FALSE(config->Subconfig->MyBool);
    EXPECT_EQ(0, config->Subconfig->MyStringList.ysize());
    EXPECT_EQ(ETestEnum::Value1, config->Subconfig->MyEnum);
    EXPECT_EQ(0, config->SubconfigList.ysize());
    EXPECT_EQ(0, config->SubconfigMap.ysize());
}

TEST(TConfigTest, Options)
{
    auto builder = CreateBuilderFromFactory(GetEphemeralNodeFactory());
    builder->BeginTree();
    BuildYsonFluently(~builder)
        .BeginMap()
            .Item("my_string").Scalar("TestString")
            .Item("option").Scalar(1)
        .EndMap();
    auto configNode = builder->EndTree();

    auto config = New<TTestConfig>();
    config->SetKeepOptions(true);
    config->Load(~configNode->AsMap());

    auto optionsNode = config->GetOptions();
    EXPECT_EQ(1, optionsNode->GetChildCount());
    FOREACH (const auto& pair, optionsNode->GetChildren()) {
        const auto& name = pair.First();
        auto child = pair.Second();
        EXPECT_EQ("option", name);
        EXPECT_EQ(1, child->AsInteger()->GetValue());
    }
}

TEST(TConfigTest, MissingRequiredParameter)
{
    auto builder = CreateBuilderFromFactory(GetEphemeralNodeFactory());
    builder->BeginTree();
    BuildYsonFluently(~builder)
        .BeginMap()
            .Item("sub").BeginMap()
                .Item("my_int").Scalar(99)
                .Item("my_bool").Scalar(true)
            .EndMap()
        .EndMap();
    auto configNode = builder->EndTree();

    auto config = New<TTestConfig>();
    EXPECT_THROW(config->Load(~configNode->AsMap()), yexception);
}

TEST(TConfigTest, IncorrectNodeType)
{
    auto builder = CreateBuilderFromFactory(GetEphemeralNodeFactory());
    builder->BeginTree();
    BuildYsonFluently(~builder)
        .BeginMap()
            .Item("my_string").Scalar(1) // incorrect type
        .EndMap();
    auto configNode = builder->EndTree();

    auto config = New<TTestConfig>();
    EXPECT_THROW(config->Load(~configNode->AsMap()), yexception);
}

TEST(TConfigTest, ArithmeticOverflow)
{
    auto builder = CreateBuilderFromFactory(GetEphemeralNodeFactory());
    builder->BeginTree();
    BuildYsonFluently(~builder)
        .BeginMap()
            .Item("my_string").Scalar("TestString")
            .Item("sub").BeginMap()
                .Item("my_int").Scalar(Max<i64>())
                .Item("my_bool").Scalar(true)
                .Item("my_enum").Scalar("Value2")
                .Item("my_string_list").BeginList()
                    .Item().Scalar("ListItem0")
                    .Item().Scalar("ListItem1")
                    .Item().Scalar("ListItem2")
                .EndList()
            .EndMap()
        .EndMap();
    auto configNode = builder->EndTree();

    auto config = New<TTestConfig>();
    EXPECT_THROW(config->Load(~configNode->AsMap()), yexception);
}

TEST(TConfigTest, Validate)
{
    auto builder = CreateBuilderFromFactory(GetEphemeralNodeFactory());
    builder->BeginTree();
    BuildYsonFluently(~builder)
        .BeginMap()
            .Item("my_string").Scalar("") // empty!
        .EndMap();
    auto configNode = builder->EndTree();

    auto config = New<TTestConfig>();
    config->Load(~configNode, false);
    EXPECT_THROW(config->Validate(), yexception);
}

TEST(TConfigTest, ValidateSubconfig)
{
    auto builder = CreateBuilderFromFactory(GetEphemeralNodeFactory());
    builder->BeginTree();
    BuildYsonFluently(~builder)
        .BeginMap()
            .Item("my_string").Scalar("TestString")
            .Item("sub").BeginMap()
                .Item("my_int").Scalar(110) // out of range
            .EndMap()
        .EndMap();
    auto configNode = builder->EndTree();

    auto config = New<TTestConfig>();
    config->Load(~configNode->AsMap(), false);
    EXPECT_THROW(config->Validate(), yexception);
}

TEST(TConfigTest, ValidateSubconfigList)
{
    auto builder = CreateBuilderFromFactory(GetEphemeralNodeFactory());
    builder->BeginTree();
    BuildYsonFluently(~builder)
        .BeginMap()
            .Item("my_string").Scalar("TestString")
            .Item("sub_list").BeginList()
                .Item().BeginMap()
                    .Item("my_int").Scalar(110) // out of range
                .EndMap()
            .EndList()
        .EndMap();
    auto configNode = builder->EndTree();

    auto config = New<TTestConfig>();
    config->Load(~configNode->AsMap(), false);
    EXPECT_THROW(config->Validate(), yexception);
}

TEST(TConfigTest, ValidateSubconfigMap)
{
    auto builder = CreateBuilderFromFactory(GetEphemeralNodeFactory());
    builder->BeginTree();
    BuildYsonFluently(~builder)
        .BeginMap()
            .Item("my_string").Scalar("TestString")
            .Item("sub_map").BeginMap()
                .Item("sub").BeginMap()
                    .Item("my_int").Scalar(110) // out of range
                .EndMap()
            .EndMap()
        .EndMap();
    auto configNode = builder->EndTree();

    auto config = New<TTestConfig>();
    config->Load(~configNode->AsMap(), false);
    EXPECT_THROW(config->Validate(), yexception);
}

TEST(TConfigTest, Save)
{
    auto config = New<TTestConfig>();

    // add non-default fields;
    config->MyString = "hello!";
    config->SubconfigList.push_back(New<TTestSubconfig>());
    config->SubconfigMap["item"] = New<TTestSubconfig>();

    auto output = SerializeToYson(~config, EYsonFormat::Text);

    Stroka subconfigYson;
    subconfigYson += "{\"my_bool\"=\"false\";";
    subconfigYson += "\"my_enum\"=\"Value1\";";
    subconfigYson += "\"my_int\"=100;";
    subconfigYson += "\"my_string_list\"=[]}";

    Stroka expected;
    expected += "{\"my_string\"=\"hello!\";";
    expected += "\"sub\"=" + subconfigYson + ";";
    expected += "\"sub_list\"=[" + subconfigYson + "];";
    expected += "\"sub_map\"={\"item\"=" + subconfigYson + "}}";

    EXPECT_EQ(expected, output);
}

} // namespace NYT
