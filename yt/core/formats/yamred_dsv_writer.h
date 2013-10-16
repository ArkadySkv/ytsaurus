#pragma once

#include "public.h"
#include "config.h"
#include "helpers.h"
#include "dsv_table.h"

#include <ytlib/table_client/public.h>

#include <core/misc/blob_output.h>
#include <core/misc/small_set.h>

namespace NYT {
namespace NFormats {

////////////////////////////////////////////////////////////////////////////////

//! Note: TYamrWriter only supports tabular data.
class TYamredDsvWriter
    : public TFormatsConsumerBase
{
public:
    explicit TYamredDsvWriter(
        TOutputStream* stream,
        TYamredDsvFormatConfigPtr config = New<TYamredDsvFormatConfig>());

    ~TYamredDsvWriter();

    // IYsonConsumer overrides.
    virtual void OnStringScalar(const TStringBuf& value) override;
    virtual void OnIntegerScalar(i64 value) override;
    virtual void OnDoubleScalar(double value) override;
    virtual void OnEntity() override;
    virtual void OnBeginList() override;
    virtual void OnListItem() override;
    virtual void OnEndList() override;
    virtual void OnBeginMap() override;
    virtual void OnKeyedItem(const TStringBuf& key) override;
    virtual void OnEndMap() override;
    virtual void OnBeginAttributes() override;
    virtual void OnEndAttributes() override;

private:
    DECLARE_ENUM(EState,
        (None)
        (ExpectColumnName)
        (ExpectValue)
        (ExpectAttributeName)
        (ExpectAttributeValue)
        (ExpectEndAttributes)
        (ExpectEntity)
    );

    struct TColumnValue
    {
        i64 RowIndex;
        TStringBuf Value;

        TColumnValue()
            : RowIndex(-1)
        { }
    };

    // For small data sizes, set and map are faster than hash set and hash map.
    typedef std::map<TStringBuf, TColumnValue> TDictionary;

    TOutputStream* Stream;
    TYamredDsvFormatConfigPtr Config;

    i64 RowCount;

    EState State;

    TStringBuf ColumnName;
    NTableClient::EControlAttribute ControlAttribute;

    TSmallSet<TStringBuf, 4> KeyColumnNames;
    TSmallSet<TStringBuf, 4> SubkeyColumnNames;

    TDictionary KeyFields;
    i32 KeyCount;
    ui32 KeyLength;

    TDictionary SubkeyFields;
    i32 SubkeyCount;
    ui32 SubkeyLength;

    std::vector<TStringBuf> ValueFields;
    ui32 ValueLength;

    TDsvTable Table;

    void WriteRow();
    void WriteYamrKey(
        const std::vector<Stroka>& columnNames,
        const TDictionary& fieldValues,
        i32 fieldCount);

    void WriteYamrValue();

    void EscapeAndWrite(const TStringBuf& string, bool inKey);
    ui32 CalculateLength(const TStringBuf& string, bool inKey);
    void IncreaseLength(ui32* length, ui32 delta);

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NFormats
} // namespace NYT

