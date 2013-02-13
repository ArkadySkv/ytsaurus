#include "stdafx.h"

#include <ytlib/formats/yamr_writer.h>

#include <contrib/testing/framework.h>

namespace NYT {
namespace NFormats {

////////////////////////////////////////////////////////////////////////////////

TEST(TYamrWriterTest, Simple)
{
    TStringStream outputStream;
    TYamrWriter writer(&outputStream);

    writer.OnListItem();
    writer.OnBeginMap();
        writer.OnKeyedItem("key");
        writer.OnStringScalar("key1");
        writer.OnKeyedItem("value");
        writer.OnStringScalar("value1");
    writer.OnEndMap();

    writer.OnListItem();
    writer.OnBeginMap();
        writer.OnKeyedItem("key");
        writer.OnStringScalar("key2");
        writer.OnKeyedItem("value");
        writer.OnStringScalar("value2");
    writer.OnEndMap();

    Stroka output =
        "key1\tvalue1\n"
        "key2\tvalue2\n";
    EXPECT_EQ(output, outputStream.Str());
}

TEST(TYamrWriterTest, SimpleWithSubkey)
{
    TStringStream outputStream;
    auto config = New<TYamrFormatConfig>();
    config->HasSubkey = true;
    TYamrWriter writer(&outputStream, config);

    writer.OnListItem();
    writer.OnBeginMap();
        writer.OnKeyedItem("key");
        writer.OnStringScalar("key1");
        writer.OnKeyedItem("subkey");
        writer.OnStringScalar("subkey1");
        writer.OnKeyedItem("value");
        writer.OnStringScalar("value1");
    writer.OnEndMap();

    writer.OnListItem();
    writer.OnBeginMap();
        writer.OnKeyedItem("key");
        writer.OnStringScalar("key2");
        writer.OnKeyedItem("subkey");
        writer.OnStringScalar("subkey2");
        writer.OnKeyedItem("value");
        writer.OnStringScalar("value2");
    writer.OnEndMap();

    Stroka output =
        "key1\tsubkey1\tvalue1\n"
        "key2\tsubkey2\tvalue2\n";
    EXPECT_EQ(output, outputStream.Str());
}

TEST(TYamrWriterTest, WritingWithoutSubkey)
{
    TStringStream outputStream;
    TYamrWriter writer(&outputStream);

    writer.OnListItem();
    writer.OnBeginMap();
        writer.OnKeyedItem("key");
        writer.OnStringScalar("key1");
        writer.OnKeyedItem("subkey");
        writer.OnStringScalar("subkey1");
        writer.OnKeyedItem("value");
        writer.OnStringScalar("value1");
    writer.OnEndMap();

    writer.OnListItem();
    writer.OnBeginMap();
        writer.OnKeyedItem("key");
        writer.OnStringScalar("key2");
        writer.OnKeyedItem("subkey");
        writer.OnStringScalar("subkey2");
        writer.OnKeyedItem("value");
        writer.OnStringScalar("value2");
    writer.OnEndMap();

    Stroka output =
        "key1\tvalue1\n"
        "key2\tvalue2\n";
    EXPECT_EQ(output, outputStream.Str());
}

TEST(TYamrWriterTest, NonStringValues)
{
    TStringStream outputStream;
    auto config = New<TYamrFormatConfig>();
    config->HasSubkey = true;
    TYamrWriter writer(&outputStream, config);

    writer.OnListItem();
    writer.OnBeginMap();
        writer.OnKeyedItem("subkey");
        writer.OnDoubleScalar(0.1);
        writer.OnKeyedItem("key");
        writer.OnStringScalar("integer");
        writer.OnKeyedItem("value");
        writer.OnIntegerScalar(42);
    writer.OnEndMap();

    writer.OnListItem();
    writer.OnBeginMap();
        writer.OnKeyedItem("value");
        writer.OnDoubleScalar(10);
        writer.OnKeyedItem("subkey");
        writer.OnStringScalar("double");
        writer.OnKeyedItem("key");
        writer.OnStringScalar("");
    writer.OnEndMap();

    Stroka output =
        "integer\t0.1\t42\n"
        "\tdouble\t10.\n";

    EXPECT_EQ(output, outputStream.Str());
}

TEST(TYamrWriterTest, SkippedKey)
{
    TStringStream outputStream;
    TYamrWriter writer(&outputStream);

    auto DoWrite = [&]() {
        writer.OnListItem();
        writer.OnBeginMap();
            writer.OnKeyedItem("key");
            writer.OnStringScalar("foo");
        writer.OnEndMap();
    };

    EXPECT_THROW(DoWrite(), std::exception);
}

TEST(TYamrWriterTest, SkippedValue)
{
    TStringStream outputStream;
    TYamrWriter writer(&outputStream);

    auto DoWrite = [&]() {
        writer.OnListItem();
        writer.OnBeginMap();
            writer.OnKeyedItem("value");
            writer.OnStringScalar("bar");
        writer.OnEndMap();
    };

    EXPECT_THROW(DoWrite(), std::exception);
}

TEST(TYamrWriterTest, SubkeyCouldBeSkipped)
{
    TStringStream outputStream;
    auto config = New<TYamrFormatConfig>();
    config->HasSubkey = true;
    TYamrWriter writer(&outputStream, config);

    writer.OnListItem();
    writer.OnBeginMap();
        writer.OnKeyedItem("value");
        writer.OnStringScalar("bar");
        writer.OnKeyedItem("key");
        writer.OnStringScalar("foo");
    writer.OnEndMap();

    Stroka output = "foo\t\tbar\n";
    EXPECT_EQ(output, outputStream.Str());
}

TEST(TYamrWriterTest, SimpleWithTableIndex)
{
    TStringStream outputStream;
    auto config = New<TYamrFormatConfig>();
    config->EnableTableIndex = true;
    TYamrWriter writer(&outputStream, config);

    writer.OnListItem();
    writer.OnBeginAttributes();
        writer.OnKeyedItem("table_index");
        writer.OnIntegerScalar(1);
    writer.OnEndAttributes();
    writer.OnBeginMap();
        writer.OnKeyedItem("key");
        writer.OnStringScalar("key1");
        writer.OnKeyedItem("value");
        writer.OnStringScalar("value1");
    writer.OnEndMap();

    Stroka output = Stroka(
        "\x01\x00" "key1" "\t" "value1" "\n"
        , 2 + 4 + 1 + 6 + 1
    );

    EXPECT_EQ(output, outputStream.Str());
}

TEST(TYamrWriterTest, Lenval)
{
    TStringStream outputStream;
    auto config = New<TYamrFormatConfig>();
    config->HasSubkey = true;
    config->Lenval = true;
    TYamrWriter writer(&outputStream, config);


    writer.OnListItem();
    writer.OnBeginMap();
        writer.OnKeyedItem("key");
        writer.OnStringScalar("key1");
        writer.OnKeyedItem("subkey");
        writer.OnStringScalar("subkey1");
        writer.OnKeyedItem("value");
        writer.OnStringScalar("value1");
    writer.OnEndMap();

    writer.OnListItem();
    writer.OnBeginMap();
        writer.OnKeyedItem("key");
        writer.OnStringScalar("key2");
        writer.OnKeyedItem("subkey");
        writer.OnStringScalar("subkey2");
        writer.OnKeyedItem("value");
        writer.OnStringScalar("value2");
    writer.OnEndMap();

    Stroka output = Stroka(
        "\x04\x00\x00\x00" "key1"
        "\x07\x00\x00\x00" "subkey1"
        "\x06\x00\x00\x00" "value1"

        "\x04\x00\x00\x00" "key2"
        "\x07\x00\x00\x00" "subkey2"
        "\x06\x00\x00\x00" "value2"
        , 2 * (3 * 4 + 4 + 6 + 7) // all i32 + lengths of keys
    );

    EXPECT_EQ(output, outputStream.Str());
}

TEST(TYamrWriterTest, LenvalWithoutFields)
{
    TStringStream outputStream;
    auto config = New<TYamrFormatConfig>();
    config->HasSubkey = true;
    config->Lenval = true;
    TYamrWriter writer(&outputStream, config);

    // Note: order is unusual (value, key)
    writer.OnListItem();
    writer.OnBeginMap();
        writer.OnKeyedItem("value");
        writer.OnStringScalar("value1");
        writer.OnKeyedItem("subkey");
        writer.OnStringScalar("");
        writer.OnKeyedItem("key");
        writer.OnStringScalar("key1");
    writer.OnEndMap();

    writer.OnListItem();
    writer.OnBeginMap();
        writer.OnKeyedItem("subkey");
        writer.OnStringScalar("subkey2");
        writer.OnKeyedItem("value");
        writer.OnStringScalar("");
        writer.OnKeyedItem("key");
        writer.OnStringScalar("key2");
    writer.OnEndMap();

    writer.OnListItem();
    writer.OnBeginMap();
        writer.OnKeyedItem("value");
        writer.OnStringScalar("value3");
        writer.OnKeyedItem("key");
        writer.OnStringScalar("");
        writer.OnKeyedItem("subkey");
        writer.OnStringScalar("subkey3");
    writer.OnEndMap();

    Stroka output = Stroka(
        "\x04\x00\x00\x00" "key1"
        "\x00\x00\x00\x00" ""
        "\x06\x00\x00\x00" "value1"

        "\x04\x00\x00\x00" "key2"
        "\x07\x00\x00\x00" "subkey2"
        "\x00\x00\x00\x00" ""

        "\x00\x00\x00\x00" ""
        "\x07\x00\x00\x00" "subkey3"
        "\x06\x00\x00\x00" "value3"

        , 9 * 4 + (4 + 6) + (4 + 7) + (7 + 6) // all i32 + lengths of keys
    );

    EXPECT_EQ(output, outputStream.Str());
}

TEST(TYamrWriterTest, LenvalWithTableIndex)
{
    TStringStream outputStream;
    auto config = New<TYamrFormatConfig>();
    config->Lenval = true;
    config->EnableTableIndex = true;
    TYamrWriter writer(&outputStream, config);

    writer.OnListItem();
    writer.OnBeginAttributes();
        writer.OnKeyedItem("table_index");
        writer.OnIntegerScalar(0);
    writer.OnEndAttributes();
    writer.OnBeginMap();
        writer.OnKeyedItem("key");
        writer.OnStringScalar("key1");
        writer.OnKeyedItem("value");
        writer.OnStringScalar("value1");
    writer.OnEndMap();

    Stroka output = Stroka(
        "\x00\x00"
        "\x04\x00\x00\x00" "key1"
        "\x06\x00\x00\x00" "value1"
        , 2 + 2 * 4 + 4 + 6
    );

    EXPECT_EQ(output, outputStream.Str());
}


////////////////////////////////////////////////////////////////////////////////

} // namespace NFormats
} // namespace NYT
