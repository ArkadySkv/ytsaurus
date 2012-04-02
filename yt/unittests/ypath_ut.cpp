#include "stdafx.h"

#include <ytlib/ytree/ypath_service.h>
#include <ytlib/ytree/ypath_client.h>

#include <ytlib/ytree/tree_builder.h>
#include <ytlib/ytree/tree_visitor.h>

#include <ytlib/ytree/yson_reader.h>
#include <ytlib/ytree/yson_writer.h>
#include <ytlib/ytree/ephemeral.h>

#include <contrib/testing/framework.h>

namespace NYT {
namespace NYTree {

////////////////////////////////////////////////////////////////////////////////

class TYPathTest: public ::testing::Test
{
public:
    IYPathServicePtr RootService;

    virtual void SetUp()
    {
        RootService = GetEphemeralNodeFactory()->CreateMap();
    }

    static TYson TextifyYson(const TYson& data)
    {
        TStringStream outputStream;
        TYsonWriter writer(&outputStream, EYsonFormat::Text);
        TStringInput input(data);
        TYsonReader reader(&writer, &input);
        reader.Read();
        return outputStream.Str();
    }

    void Set(const TYPath& path, const TYson& value)
    {
        SyncYPathSet(~RootService, path, value);
    }

    void Remove(const TYPath& path)
    {
        SyncYPathRemove(~RootService, path);
    }

    TYson Get(const TYPath& path)
    {
        return TextifyYson(SyncYPathGet(~RootService, path));
    }

    yvector<Stroka> List(const TYPath& path)
    {
        return SyncYPathList(~RootService, path);
    }

    void Check(const TYPath& path, TYson expected)
    {
        TYson output = Get(path);
        EXPECT_EQ(expected, output);
    }

    void CheckList(const TYPath& path, TYson expected)
    {
        VectorStrok result;
        SplitStroku(&result, expected, ";");

        for (int index = 0; index < result.ysize(); ++index) {
            Check(path + "/" + ToString(index), result[index]);
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

TEST_F(TYPathTest, MapModification)
{
    Set("/map", "{\"hello\"=\"world\"; \"list\"=[0;\"a\";{}]; \"n\"=1}");

    Set("/map/hello", "not_world");
    Check("", "{\"map\"={\"hello\"=\"not_world\";\"list\"=[0;\"a\";{}];\"n\"=1}}");

    Set("/map/list/2/some", "value");
    Check("", "{\"map\"={\"hello\"=\"not_world\";\"list\"=[0;\"a\";{\"some\"=\"value\"}];\"n\"=1}}");

    Remove("/map/n");
    Check("", "{\"map\"={\"hello\"=\"not_world\";\"list\"=[0;\"a\";{\"some\"=\"value\"}]}}");

    Set("/map/list", "[]");
    Check("", "{\"map\"={\"hello\"=\"not_world\";\"list\"=[]}}");

    Remove("/map/hello");
    Check("", "{\"map\"={\"list\"=[]}}");

    Remove("/map");
    Check("", "{}");
}

TEST_F(TYPathTest, ListModification)
{
    Set("/list", "[1;2;3]");
    Check("", "{\"list\"=[1;2;3]}");
    Check("/list", "[1;2;3]");
    Check("/list/0", "1");
    Check("/list/1", "2");
    Check("/list/2", "3");
    Check("/list/-1", "3");
    Check("/list/-2", "2");
    Check("/list/-3", "1");

    Set("/list/+", "4");
    Check("/list", "[1;2;3;4]");

    Set("/list/+", "5");
    Check("/list", "[1;2;3;4;5]");

    Set("/list/2", "100");
    Check("/list", "[1;2;100;4;5]");

    Set("/list/-2", "3");
    Check("/list", "[1;2;100;3;5]");

    Remove("/list/4");
    Check("/list", "[1;2;100;3]");

    Remove("/list/2");
    Check("/list", "[1;2;3]");

    Remove("/list/-1");
    Check("/list", "[1;2]");

    Set("/list/^0", "0");
    Check("/list", "[0;1;2]");

    Set("/list/1^", "3");
    Check("/list", "[0;1;3;2]");

    Set("/list/-1^", "4");
    Check("/list", "[0;1;3;2;4]");

    Set("/list/^-1", "5");
    Check("/list", "[0;1;3;2;5;4]");
}

TEST_F(TYPathTest, ListReassignment)
{
    Set("/list", "[a;b;c]");
    Set("/list", "[1;2;3]");

    Check("", "{\"list\"=[1;2;3]}");
}

TEST_F(TYPathTest, Ls)
{
    Set("", "{a={x1={y1=1}};b={x2={y2=2}};c={x3={y3=3}};d={x4={y4=4}}}");

    Remove("/b");
    Set("/e", "5");

    auto result = List("");
    std::sort(result.begin(), result.end());

    yvector<Stroka> expected;
    expected.push_back("a");
    expected.push_back("c");
    expected.push_back("d");
    expected.push_back("e");

    EXPECT_EQ(expected, result);
}

TEST_F(TYPathTest, Attributes)
{
    Set("/root", "{nodes=[\"1\"; \"2\"]} <attr=100;mode=\"rw\">");
    Check("/root@", "{\"attr\"=100;\"mode\"=\"rw\"}");
    Check("/root@attr", "100");

    Set("/root/value", "500<>");
    Check("/root/value", "500");

    Remove("/root@");
    Check("/root@", "{}");

    Remove("/root/nodes");
    Remove("/root/value");
    Check("", "{\"root\"={}}");

    Set("/root/\"2\"", "<author=\"ignat\">");
    Check("", "{\"root\"={\"2\"=<>}}");
    Check("/root/\"2\"@", "{\"author\"=\"ignat\"}");
    Check("/root/\"2\"@author", "\"ignat\"");

    // note: empty attributes are shown when nested
    Set("/root/\"3\"", "<dir=<file=-100<>>>");
    Check("/root/\"3\"@", "{\"dir\"=<\"file\"=-100<>>}");
    Check("/root/\"3\"@dir@", "{\"file\"=-100<>}");
    Check("/root/\"3\"@dir@file", "-100<>");
    Check("/root/\"3\"@dir@file@", "{}");
}

TEST_F(TYPathTest, InvalidCases)
{
    Set("/root", "{}");

    EXPECT_ANY_THROW(Set("a", "{}")); // must start with '/'
    EXPECT_ANY_THROW(Set("/root/", "{}")); // cannot end with '/'
    EXPECT_ANY_THROW(Set("", "[]")); // change the type of root
    EXPECT_ANY_THROW(Remove("")); // remove the root
    EXPECT_ANY_THROW(Get("/b")); // get non-existent path

    // get non-existent attribute from non-existent node
    EXPECT_ANY_THROW(Get("/b@some"));

    // get non-existent attribute from existent node
    EXPECT_ANY_THROW({
       Set("/c", "{}");
       Get("/c@some");
   });

    // remove non-existing child
    EXPECT_ANY_THROW(Remove("/a"));

//    EXPECT_ANY_THROW(Set("/@/some", "{}"));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT
