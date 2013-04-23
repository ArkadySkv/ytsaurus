#include "stdafx.h"

#include <ytlib/ytree/yson_serializable.h>
#include <ytlib/ytree/tree_builder.h>
#include <ytlib/ytree/ephemeral_node_factory.h>
#include <ytlib/ytree/fluent.h>
#include <ytlib/yson/writer.h>
#include <ytlib/ytree/tree_visitor.h>
#include <ytlib/ytree/ypath_client.h>

#include <contrib/testing/framework.h>

namespace NYT {

using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

DECLARE_ENUM(ETestEnum,
    (Value0)
    (Value1)
    (Value2)
);

struct TTestSubconfig
    : public TYsonSerializable
{
    typedef TIntrusivePtr<TTestSubconfig> TPtr;

    int MyInt;
    bool MyBool;
    std::vector<Stroka> MyStringList;
    ETestEnum MyEnum;

    TTestSubconfig()
    {
        RegisterParameter("my_int", MyInt).Default(100).InRange(95, 205);
        RegisterParameter("my_bool", MyBool).Default(false);
        RegisterParameter("my_string_list", MyStringList).Default();
        RegisterParameter("my_enum", MyEnum).Default(ETestEnum::Value1);
    }
};

class TTestConfig
    : public TYsonSerializable
{
public:
    typedef TIntrusivePtr<TTestConfig> TPtr;

    Stroka MyString;
    TTestSubconfig::TPtr Subconfig;
    std::vector<TTestSubconfig::TPtr> SubconfigList;
    yhash_map<Stroka, TTestSubconfig::TPtr> SubconfigMap;

    TTestConfig()
    {
        RegisterParameter("my_string", MyString).NonEmpty();
        RegisterParameter("sub", Subconfig).DefaultNew();
        RegisterParameter("sub_list", SubconfigList).Default();
        RegisterParameter("sub_map", SubconfigMap).Default();

        RegisterInitializer([&] () {
            MyString = "x";
            Subconfig->MyInt = 200;
        });
    }
};

void TestCompleteSubconfig(TTestSubconfig* subconfig)
{
    EXPECT_EQ(99, subconfig->MyInt);
    EXPECT_TRUE(subconfig->MyBool);
    EXPECT_EQ(3, subconfig->MyStringList.size());
    EXPECT_EQ("ListItem0", subconfig->MyStringList[0]);
    EXPECT_EQ("ListItem1", subconfig->MyStringList[1]);
    EXPECT_EQ("ListItem2", subconfig->MyStringList[2]);
    EXPECT_EQ(ETestEnum::Value2, subconfig->MyEnum);
}

TEST(TConfigTest, Complete)
{
    auto configNode = BuildYsonNodeFluently()
        .BeginMap()
            .Item("my_string").Value("TestString")
            .Item("sub").BeginMap()
                .Item("my_int").Value(99)
                .Item("my_bool").Value(true)
                .Item("my_enum").Value("Value2")
                .Item("my_string_list").BeginList()
                    .Item().Value("ListItem0")
                    .Item().Value("ListItem1")
                    .Item().Value("ListItem2")
                .EndList()
            .EndMap()
            .Item("sub_list").BeginList()
                .Item().BeginMap()
                    .Item("my_int").Value(99)
                    .Item("my_bool").Value(true)
                    .Item("my_enum").Value("Value2")
                    .Item("my_string_list").BeginList()
                        .Item().Value("ListItem0")
                        .Item().Value("ListItem1")
                        .Item().Value("ListItem2")
                    .EndList()
                .EndMap()
                .Item().BeginMap()
                    .Item("my_int").Value(99)
                    .Item("my_bool").Value(true)
                    .Item("my_enum").Value("Value2")
                    .Item("my_string_list").BeginList()
                        .Item().Value("ListItem0")
                        .Item().Value("ListItem1")
                        .Item().Value("ListItem2")
                    .EndList()
                .EndMap()
            .EndList()
            .Item("sub_map").BeginMap()
                .Item("sub1").BeginMap()
                    .Item("my_int").Value(99)
                    .Item("my_bool").Value(true)
                    .Item("my_enum").Value("Value2")
                    .Item("my_string_list").BeginList()
                        .Item().Value("ListItem0")
                        .Item().Value("ListItem1")
                        .Item().Value("ListItem2")
                    .EndList()
                .EndMap()
                .Item("sub2").BeginMap()
                    .Item("my_int").Value(99)
                    .Item("my_bool").Value(true)
                    .Item("my_enum").Value("Value2")
                    .Item("my_string_list").BeginList()
                        .Item().Value("ListItem0")
                        .Item().Value("ListItem1")
                        .Item().Value("ListItem2")
                    .EndList()
                .EndMap()
            .EndMap()
        .EndMap();

    auto config = New<TTestConfig>();
    config->Load(configNode->AsMap());

    EXPECT_EQ("TestString", config->MyString);
    TestCompleteSubconfig(~config->Subconfig);
    EXPECT_EQ(2, config->SubconfigList.size());
    TestCompleteSubconfig(~config->SubconfigList[0]);
    TestCompleteSubconfig(~config->SubconfigList[1]);
    EXPECT_EQ(2, config->SubconfigMap.size());
    auto it1 = config->SubconfigMap.find("sub1");
    EXPECT_FALSE(it1 == config->SubconfigMap.end());
    TestCompleteSubconfig(~it1->second);
    auto it2 = config->SubconfigMap.find("sub2");
    EXPECT_FALSE(it2 == config->SubconfigMap.end());
    TestCompleteSubconfig(~it2->second);
}

TEST(TConfigTest, MissingParameter)
{
    auto configNode = BuildYsonNodeFluently()
        .BeginMap()
            .Item("my_string").Value("TestString")
            .Item("sub").BeginMap()
                .Item("my_bool").Value(true)
            .EndMap()
        .EndMap();

    auto config = New<TTestConfig>();
    config->Load(configNode->AsMap());

    EXPECT_EQ("TestString", config->MyString);
    EXPECT_EQ(200, config->Subconfig->MyInt);
    EXPECT_TRUE(config->Subconfig->MyBool);
    EXPECT_EQ(0, config->Subconfig->MyStringList.size());
    EXPECT_EQ(ETestEnum::Value1, config->Subconfig->MyEnum);
    EXPECT_EQ(0, config->SubconfigList.size());
    EXPECT_EQ(0, config->SubconfigMap.size());
}

TEST(TConfigTest, MissingSubconfig)
{
    auto configNode = BuildYsonNodeFluently()
        .BeginMap()
            .Item("my_string").Value("TestString")
        .EndMap();

    auto config = New<TTestConfig>();
    config->Load(configNode->AsMap());

    EXPECT_EQ("TestString", config->MyString);
    EXPECT_EQ(200, config->Subconfig->MyInt);
    EXPECT_FALSE(config->Subconfig->MyBool);
    EXPECT_EQ(0, config->Subconfig->MyStringList.size());
    EXPECT_EQ(ETestEnum::Value1, config->Subconfig->MyEnum);
    EXPECT_EQ(0, config->SubconfigList.size());
    EXPECT_EQ(0, config->SubconfigMap.ysize());
}

TEST(TConfigTest, Options)
{
    auto configNode = BuildYsonNodeFluently()
        .BeginMap()
            .Item("my_string").Value("TestString")
            .Item("option").Value(1)
        .EndMap();

    auto config = New<TTestConfig>();
    config->SetKeepOptions(true);
    config->Load(configNode->AsMap());

    auto optionsNode = config->GetOptions();
    EXPECT_EQ(1, optionsNode->GetChildCount());
    FOREACH (const auto& pair, optionsNode->GetChildren()) {
        const auto& name = pair.first;
        auto child = pair.second;
        EXPECT_EQ("option", name);
        EXPECT_EQ(1, child->AsInteger()->GetValue());
    }
}

TEST(TConfigTest, MissingRequiredParameter)
{
    auto configNode = BuildYsonNodeFluently()
        .BeginMap()
            .Item("sub").BeginMap()
                .Item("my_int").Value(99)
                .Item("my_bool").Value(true)
            .EndMap()
        .EndMap();

    auto config = New<TTestConfig>();
    EXPECT_THROW(config->Load(configNode->AsMap()), std::exception);
}

TEST(TConfigTest, IncorrectNodeType)
{
    auto builder = CreateBuilderFromFactory(GetEphemeralNodeFactory());
    builder->BeginTree();
    BuildYsonFluently(~builder)
        .BeginMap()
            .Item("my_string").Value(1) // incorrect type
        .EndMap();
    auto configNode = builder->EndTree();

    auto config = New<TTestConfig>();
    EXPECT_THROW(config->Load(configNode->AsMap()), std::exception);
}

TEST(TConfigTest, ArithmeticOverflow)
{
    auto builder = CreateBuilderFromFactory(GetEphemeralNodeFactory());
    builder->BeginTree();
    BuildYsonFluently(~builder)
        .BeginMap()
            .Item("my_string").Value("TestString")
            .Item("sub").BeginMap()
                .Item("my_int").Value(Max<i64>())
                .Item("my_bool").Value(true)
                .Item("my_enum").Value("Value2")
                .Item("my_string_list").BeginList()
                    .Item().Value("ListItem0")
                    .Item().Value("ListItem1")
                    .Item().Value("ListItem2")
                .EndList()
            .EndMap()
        .EndMap();
    auto configNode = builder->EndTree();

    auto config = New<TTestConfig>();
    EXPECT_THROW(config->Load(configNode->AsMap()), std::exception);
}

TEST(TConfigTest, Validate)
{
    auto builder = CreateBuilderFromFactory(GetEphemeralNodeFactory());
    builder->BeginTree();
    BuildYsonFluently(~builder)
        .BeginMap()
            .Item("my_string").Value("") // empty!
        .EndMap();
    auto configNode = builder->EndTree();

    auto config = New<TTestConfig>();
    config->Load(configNode, false);
    EXPECT_THROW(config->Validate(), std::exception);
}

TEST(TConfigTest, ValidateSubconfig)
{
    auto builder = CreateBuilderFromFactory(GetEphemeralNodeFactory());
    builder->BeginTree();
    BuildYsonFluently(~builder)
        .BeginMap()
            .Item("my_string").Value("TestString")
            .Item("sub").BeginMap()
                .Item("my_int").Value(210) // out of range
            .EndMap()
        .EndMap();
    auto configNode = builder->EndTree();

    auto config = New<TTestConfig>();
    config->Load(configNode->AsMap(), false);
    EXPECT_THROW(config->Validate(), std::exception);
}

TEST(TConfigTest, ValidateSubconfigList)
{
    auto builder = CreateBuilderFromFactory(GetEphemeralNodeFactory());
    builder->BeginTree();
    BuildYsonFluently(~builder)
        .BeginMap()
            .Item("my_string").Value("TestString")
            .Item("sub_list").BeginList()
                .Item().BeginMap()
                    .Item("my_int").Value(210) // out of range
                .EndMap()
            .EndList()
        .EndMap();
    auto configNode = builder->EndTree();

    auto config = New<TTestConfig>();
    config->Load(configNode->AsMap(), false);
    EXPECT_THROW(config->Validate(), std::exception);
}

TEST(TConfigTest, ValidateSubconfigMap)
{
    auto builder = CreateBuilderFromFactory(GetEphemeralNodeFactory());
    builder->BeginTree();
    BuildYsonFluently(~builder)
        .BeginMap()
            .Item("my_string").Value("TestString")
            .Item("sub_map").BeginMap()
                .Item("sub").BeginMap()
                    .Item("my_int").Value(210) // out of range
                .EndMap()
            .EndMap()
        .EndMap();
    auto configNode = builder->EndTree();

    auto config = New<TTestConfig>();
    config->Load(configNode->AsMap(), false);
    EXPECT_THROW(config->Validate(), std::exception);
}

TEST(TConfigTest, Save)
{
    auto config = New<TTestConfig>();

    // add non-default fields;
    config->MyString = "hello!";
    config->SubconfigList.push_back(New<TTestSubconfig>());
    config->SubconfigMap["item"] = New<TTestSubconfig>();

    auto output = ConvertToYsonString(config, NYson::EYsonFormat::Text);

    Stroka subconfigYson =
        "{\"my_bool\"=\"false\";"
        "\"my_enum\"=\"value1\";"
        "\"my_int\"=200;"
        "\"my_string_list\"=[]}";

    Stroka subconfigYsonOrigin =
        "{\"my_bool\"=\"false\";"
        "\"my_enum\"=\"value1\";"
        "\"my_int\"=100;"
        "\"my_string_list\"=[]}";

    Stroka expectedYson;
    expectedYson += "{\"my_string\"=\"hello!\";";
    expectedYson += "\"sub\"=" + subconfigYson + ";";
    expectedYson += "\"sub_list\"=[" + subconfigYsonOrigin + "];";
    expectedYson += "\"sub_map\"={\"item\"=" + subconfigYsonOrigin + "}}";

    EXPECT_TRUE(AreNodesEqual(
        ConvertToNode(TYsonString(expectedYson)),
        ConvertToNode(TYsonString(output.Data()))));
}

TEST(TConfigTest, TestConfigUpdate)
{
    auto config = New<TTestConfig>();
    {
        auto newConfig = UpdateYsonSerializable(config, 0);
        EXPECT_EQ(newConfig->Subconfig->MyInt, 200);
    }

    {
        auto newConfig = UpdateYsonSerializable(config, ConvertToNode(TYsonString("{\"sub\"={\"my_int\"=150}}")));
        EXPECT_EQ(newConfig->Subconfig->MyInt, 150);
    }

    {
        auto newConfig = UpdateYsonSerializable(config, ConvertToNode(TYsonString("{\"sub\"={\"my_int_\"=150}}")));
        EXPECT_EQ(newConfig->Subconfig->MyInt, 200);
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
