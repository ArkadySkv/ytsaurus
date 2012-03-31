#include "stdafx.h"

#include <ytlib/ytree/yson_reader.h>
#include <ytlib/ytree/yson_consumer-mock.h>

#include <ytlib/ytree/yson_parser.h>

#include <util/stream/mem.h>

#include <contrib/testing/framework.h>

using ::testing::InSequence;
using ::testing::StrictMock;

namespace NYT {
namespace NYTree {

////////////////////////////////////////////////////////////////////////////////

class TYsonParserTest: public ::testing::Test
{
public:
    Stroka Input;
    StrictMock<TMockYsonConsumer> Mock;
    TYsonParser::EMode Mode;

    virtual void SetUp()
    {
        Mode = TYsonParser::EMode::Node;
    }

    void Run()
    {
        ParseYson(Input, &Mock, Mode);
    }
};

////////////////////////////////////////////////////////////////////////////////

TEST_F(TYsonParserTest, Int64)
{
    Input = "   100500  ";

    InSequence dummy;
    EXPECT_CALL(Mock, OnInt64Scalar(100500, false));

    Run();
}

TEST_F(TYsonParserTest, Double)
{
    Input = " 31415926e-7  ";

    InSequence dummy;
    EXPECT_CALL(Mock, OnDoubleScalar(::testing::DoubleEq(3.1415926), false));

    Run();
}

TEST_F(TYsonParserTest, StringStartingWithLetter)
{
    Input = " Hello_789_World_123   ";

    InSequence dummy;
    EXPECT_CALL(Mock, OnStringScalar("Hello_789_World_123", false));

    Run();
}

TEST_F(TYsonParserTest, StringStartingWithQuote)
{
    Input = "\" abcdeABCDE <1234567> + (10_000) - = 900   \"";

    InSequence dummy;
    EXPECT_CALL(Mock, OnStringScalar(" abcdeABCDE <1234567> + (10_000) - = 900   ", false));

    Run();
}

TEST_F(TYsonParserTest, EntityWithEmptyAttributes)
{
    Input = "< >";

    InSequence dummy;
    EXPECT_CALL(Mock, OnEntity(true));
    EXPECT_CALL(Mock, OnBeginAttributes());
    EXPECT_CALL(Mock, OnEndAttributes());

    Run();
}

TEST_F(TYsonParserTest, BinaryInt64)
{
    Input = Stroka(" \x02\x80\x80\x80\x02  ", 1 + 5 + 2); //Int64Marker + (1 << 21) as VarInt ZigZagEncoded

    InSequence dummy;
    EXPECT_CALL(Mock, OnInt64Scalar(1ull << 21, false));

    Run();
}

TEST_F(TYsonParserTest, BinaryDouble)
{
    double x = 2.71828;
    Input = Stroka("\x03", 1) + Stroka((char*) &x, sizeof(double)); // DoubleMarker

    InSequence dummy;
    EXPECT_CALL(Mock, OnDoubleScalar(::testing::DoubleEq(2.71828), false));

    Run();
}

TEST_F(TYsonParserTest, BinaryString)
{
    Input = Stroka(" \x01\x08YSON", 1 + 6); // StringMarker + length ( = 4) + String

    InSequence dummy;
    EXPECT_CALL(Mock, OnStringScalar("YSON", false));

    Run();
}

TEST_F(TYsonParserTest, EmpryBinaryString)
{
    Input = Stroka("\x01\x00", 2); // StringMarker + length ( = 0 )

    InSequence dummy;
    EXPECT_CALL(Mock, OnStringScalar("", false));

    Run();
}

TEST_F(TYsonParserTest, EmptyList)
{
    Input = "  [    ]   ";

    InSequence dummy;
    EXPECT_CALL(Mock, OnBeginList());
    EXPECT_CALL(Mock, OnEndList(false));

    Run();
}

TEST_F(TYsonParserTest, EmptyMap)
{
    Input = "  {    }   ";

    InSequence dummy;
    EXPECT_CALL(Mock, OnBeginMap());
    EXPECT_CALL(Mock, OnEndMap(false));

    Run();
}

TEST_F(TYsonParserTest, OneElementList)
{
    Input = "  [  42  ]   ";

    InSequence dummy;
    EXPECT_CALL(Mock, OnBeginList());
    EXPECT_CALL(Mock, OnListItem());
    EXPECT_CALL(Mock, OnInt64Scalar(42, false));
    EXPECT_CALL(Mock, OnEndList(false));

    Run();
}

TEST_F(TYsonParserTest, OneElementMap)
{
    Input = "  {  hello = world  }   ";

    InSequence dummy;
    EXPECT_CALL(Mock, OnBeginMap());
    EXPECT_CALL(Mock, OnMapItem("hello"));
    EXPECT_CALL(Mock, OnStringScalar("world", false));
    EXPECT_CALL(Mock, OnEndMap(false));

    Run();
}

TEST_F(TYsonParserTest, OneElementBinaryMap)
{
    Input = Stroka("{\x01\x0Ahello=\x01\x0Aworld}",1 + 7 + 1 + 7 + 1);

    InSequence dummy;
    EXPECT_CALL(Mock, OnBeginMap());
    EXPECT_CALL(Mock, OnMapItem("hello"));
    EXPECT_CALL(Mock, OnStringScalar("world", false));
    EXPECT_CALL(Mock, OnEndMap(false));

    Run();
}



TEST_F(TYsonParserTest, SeveralElementsList)
{
    Input = "  [  42    ; 1e3   ; nosy_111 ; \"nosy is the best format ever!\"; { } ; ]   ";

    InSequence dummy;
    EXPECT_CALL(Mock, OnBeginList());

    EXPECT_CALL(Mock, OnListItem());
    EXPECT_CALL(Mock, OnInt64Scalar(42, false));

    EXPECT_CALL(Mock, OnListItem());
    EXPECT_CALL(Mock, OnDoubleScalar(::testing::DoubleEq(1000), false));

    EXPECT_CALL(Mock, OnListItem());
    EXPECT_CALL(Mock, OnStringScalar("nosy_111", false));

    EXPECT_CALL(Mock, OnListItem());
    EXPECT_CALL(Mock, OnStringScalar("nosy is the best format ever!", false));

    EXPECT_CALL(Mock, OnListItem());
    EXPECT_CALL(Mock, OnBeginMap());
    EXPECT_CALL(Mock, OnEndMap(false));

    EXPECT_CALL(Mock, OnEndList(false));

    Run();
}

TEST_F(TYsonParserTest, MapWithAttributes)
{
    Input =  "{ path = \"/home/sandello\" ; mode = 0755 } \n";
    Input += "<acl = { read = [ \"*\" ]; write = [ sandello ] } ;  \n";
    Input += "  lock_scope = mytables>";

    InSequence dummy;
    EXPECT_CALL(Mock, OnBeginMap());

    EXPECT_CALL(Mock, OnMapItem("path"));
        EXPECT_CALL(Mock, OnStringScalar("/home/sandello", false));

    EXPECT_CALL(Mock, OnMapItem("mode"));
        EXPECT_CALL(Mock, OnInt64Scalar(755, false));

    EXPECT_CALL(Mock, OnEndMap(true));

    EXPECT_CALL(Mock, OnBeginAttributes());
    EXPECT_CALL(Mock, OnAttributesItem("acl"));
        EXPECT_CALL(Mock, OnBeginMap());

        EXPECT_CALL(Mock, OnMapItem("read"));
        EXPECT_CALL(Mock, OnBeginList());
        EXPECT_CALL(Mock, OnListItem());
        EXPECT_CALL(Mock, OnStringScalar("*", false));
        EXPECT_CALL(Mock, OnEndList(false));

        EXPECT_CALL(Mock, OnMapItem("write"));
        EXPECT_CALL(Mock, OnBeginList());
        EXPECT_CALL(Mock, OnListItem());
        EXPECT_CALL(Mock, OnStringScalar("sandello", false));
        EXPECT_CALL(Mock, OnEndList(false));

        EXPECT_CALL(Mock, OnEndMap(false));

    EXPECT_CALL(Mock, OnAttributesItem("lock_scope"));
        EXPECT_CALL(Mock, OnStringScalar("mytables", false));

    EXPECT_CALL(Mock, OnEndAttributes());

    Run();
}

TEST_F(TYsonParserTest, Unescaping)
{
    Input =
        "\"\\0\\1\\2\\3\\4\\5\\6\\7\\x08\\t\\n\\x0B\\x0C\\r\\x0E\\x0F"
        "\\x10\\x11\\x12\\x13\\x14\\x15\\x16\\x17\\x18\\x19\\x1A\\x1B"
        "\\x1C\\x1D\\x1E\\x1F !\\\"#$%&'()*+,-./0123456789:;<=>?@ABCD"
        "EFGHIJKLMNOPQRSTUVWXYZ[\\\\]^_`abcdefghijklmnopqrstuvwxyz{|}~"
        "\\x7F\\x80\\x81\\x82\\x83\\x84\\x85\\x86\\x87\\x88\\x89\\x8A"
        "\\x8B\\x8C\\x8D\\x8E\\x8F\\x90\\x91\\x92\\x93\\x94\\x95\\x96"
        "\\x97\\x98\\x99\\x9A\\x9B\\x9C\\x9D\\x9E\\x9F\\xA0\\xA1\\xA2"
        "\\xA3\\xA4\\xA5\\xA6\\xA7\\xA8\\xA9\\xAA\\xAB\\xAC\\xAD\\xAE"
        "\\xAF\\xB0\\xB1\\xB2\\xB3\\xB4\\xB5\\xB6\\xB7\\xB8\\xB9\\xBA"
        "\\xBB\\xBC\\xBD\\xBE\\xBF\\xC0\\xC1\\xC2\\xC3\\xC4\\xC5\\xC6"
        "\\xC7\\xC8\\xC9\\xCA\\xCB\\xCC\\xCD\\xCE\\xCF\\xD0\\xD1\\xD2"
        "\\xD3\\xD4\\xD5\\xD6\\xD7\\xD8\\xD9\\xDA\\xDB\\xDC\\xDD\\xDE"
        "\\xDF\\xE0\\xE1\\xE2\\xE3\\xE4\\xE5\\xE6\\xE7\\xE8\\xE9\\xEA"
        "\\xEB\\xEC\\xED\\xEE\\xEF\\xF0\\xF1\\xF2\\xF3\\xF4\\xF5\\xF6"
        "\\xF7\\xF8\\xF9\\xFA\\xFB\\xFC\\xFD\\xFE\\xFF\"";

    Stroka output;
    for (int i = 0; i < 256; ++i) {
        output.push_back(char(i));
    }

    InSequence dummy;
    EXPECT_CALL(Mock, OnStringScalar(output, false));

    Run();
}

TEST_F(TYsonParserTest, TrailingSlashes)
{
    Stroka slash = "\\";
    Stroka escapedSlash = slash + slash;
    Stroka quote = "\"";
    Input = quote + escapedSlash + quote;

    InSequence dummy;
    EXPECT_CALL(Mock, OnStringScalar(slash, false));

    Run();
}

TEST_F(TYsonParserTest, ListFragment)
{
    Input = "   1 ;2; 3; 4;5  ";
    Mode = TYsonParser::EMode::ListFragment;

    InSequence dummy;
    EXPECT_CALL(Mock, OnListItem());
    EXPECT_CALL(Mock, OnInt64Scalar(1, false));
    EXPECT_CALL(Mock, OnListItem());
    EXPECT_CALL(Mock, OnInt64Scalar(2, false));
    EXPECT_CALL(Mock, OnListItem());
    EXPECT_CALL(Mock, OnInt64Scalar(3, false));
    EXPECT_CALL(Mock, OnListItem());
    EXPECT_CALL(Mock, OnInt64Scalar(4, false));
    EXPECT_CALL(Mock, OnListItem());
    EXPECT_CALL(Mock, OnInt64Scalar(5, false));

    Run();
}

TEST_F(TYsonParserTest, ListFragmentWithTrailingSemicolon)
{
    Input = "{};[];<>;";
    Mode = TYsonParser::EMode::ListFragment;

    InSequence dummy;
    EXPECT_CALL(Mock, OnListItem());
    EXPECT_CALL(Mock, OnBeginMap());
    EXPECT_CALL(Mock, OnEndMap(false));
    EXPECT_CALL(Mock, OnListItem());
    EXPECT_CALL(Mock, OnBeginList());
    EXPECT_CALL(Mock, OnEndList(false));
    EXPECT_CALL(Mock, OnListItem());
    EXPECT_CALL(Mock, OnEntity(true));
    EXPECT_CALL(Mock, OnBeginAttributes());
    EXPECT_CALL(Mock, OnEndAttributes());

    Run();
}

TEST_F(TYsonParserTest, OneListFragment)
{
    Input = "   100500  ";
    Mode = TYsonParser::EMode::ListFragment;

    InSequence dummy;
    EXPECT_CALL(Mock, OnListItem());
    EXPECT_CALL(Mock, OnInt64Scalar(100500, false));

    Run();
}

TEST_F(TYsonParserTest, EmptyListFragment)
{
    Input = "  ";
    Mode = TYsonParser::EMode::ListFragment;

    InSequence dummy;
    Run();
}

TEST_F(TYsonParserTest, MapFragment)
{
    Input = "  a = 1 ;b=2; c= 3; d =4;e=5  ";
    Mode = TYsonParser::EMode::MapFragment;

    InSequence dummy;
    EXPECT_CALL(Mock, OnMapItem("a"));
    EXPECT_CALL(Mock, OnInt64Scalar(1, false));
    EXPECT_CALL(Mock, OnMapItem("b"));
    EXPECT_CALL(Mock, OnInt64Scalar(2, false));
    EXPECT_CALL(Mock, OnMapItem("c"));
    EXPECT_CALL(Mock, OnInt64Scalar(3, false));
    EXPECT_CALL(Mock, OnMapItem("d"));
    EXPECT_CALL(Mock, OnInt64Scalar(4, false));
    EXPECT_CALL(Mock, OnMapItem("e"));
    EXPECT_CALL(Mock, OnInt64Scalar(5, false));

    Run();
}

TEST_F(TYsonParserTest, MapFragmentWithTrailingSemicolon)
{
    Input = "map={};list=[];entity=<>;";
    Mode = TYsonParser::EMode::MapFragment;

    InSequence dummy;
    EXPECT_CALL(Mock, OnMapItem("map"));
    EXPECT_CALL(Mock, OnBeginMap());
    EXPECT_CALL(Mock, OnEndMap(false));
    EXPECT_CALL(Mock, OnMapItem("list"));
    EXPECT_CALL(Mock, OnBeginList());
    EXPECT_CALL(Mock, OnEndList(false));
    EXPECT_CALL(Mock, OnMapItem("entity"));
    EXPECT_CALL(Mock, OnEntity(true));
    EXPECT_CALL(Mock, OnBeginAttributes());
    EXPECT_CALL(Mock, OnEndAttributes());

    Run();
}

TEST_F(TYsonParserTest, OneMapFragment)
{
    Input = "   \"1\" = 100500  ";
    Mode = TYsonParser::EMode::MapFragment;

    InSequence dummy;
    EXPECT_CALL(Mock, OnMapItem("1"));
    EXPECT_CALL(Mock, OnInt64Scalar(100500, false));

    Run();
}

TEST_F(TYsonParserTest, EmptyMapFragment)
{
    Input = "  ";
    Mode = TYsonParser::EMode::MapFragment;

    InSequence dummy;
    Run();
}


////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT
