﻿#pragma once

#include "public.h"

#include <core/misc/blob_output.h>
#include <core/misc/blob_range.h>
#include <core/misc/nullable.h>
#include <core/misc/property.h>

#include <core/yson/consumer.h>
#include <core/yson/writer.h>

#include <ytlib/new_table_client/writer.h>
#include <ytlib/new_table_client/unversioned_row.h>

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

// TODO(babenko): deprecate
/*!
 *  For performance reasons we don't use TForwardingYsonConsumer.
 */
class TTableConsumer
    : public NYson::IYsonConsumer
{
public:
    template <class TWriter>
    explicit TTableConsumer(TWriter writer)
        : ControlState(EControlState::None)
        , CurrentTableIndex(0)
        , Writer(writer)
        , Depth(0)
        , ValueWriter(&RowBuffer)
    {
        Writers.push_back(writer);
    }

    template <class TWriter>
    TTableConsumer(const std::vector<TWriter>& writers, int tableIndex)
        : ControlState(EControlState::None)
        , CurrentTableIndex(tableIndex)
        , Writers(writers.begin(), writers.end())
        , Writer(Writers[CurrentTableIndex])
        , Depth(0)
        , ValueWriter(&RowBuffer)
    { }

    virtual void OnKeyedItem(const TStringBuf& name) override;
    virtual void OnStringScalar(const TStringBuf& value) override;
    virtual void OnIntegerScalar(i64 value) override;
    virtual void OnDoubleScalar(double value) override;
    virtual void OnEntity() override;
    virtual void OnBeginList() override;
    virtual void OnListItem() override;
    virtual void OnEndList() override;
    virtual void OnBeginMap() override;
    virtual void OnEndMap() override;
    virtual void OnBeginAttributes() override;
    virtual void OnEndAttributes() override;
    virtual void OnRaw(const TStringBuf& yson, NYson::EYsonType type) override;

private:
    void ThrowError(const Stroka& message) const;
    void ThrowMapExpected() const;
    void ThrowEntityExpected() const;
    void ThrowInvalidControlAttribute(const Stroka& whatsWrong) const;

    DECLARE_ENUM(EControlState,
        (None)
        (ExpectControlAttributeName)
        (ExpectControlAttributeValue)
        (ExpectEndControlAttributes)
        (ExpectEntity)
    );

    EControlState ControlState;
    EControlAttribute ControlAttribute;

    int CurrentTableIndex;
    std::vector<IWriterBasePtr> Writers;
    IWriterBasePtr Writer;

    int Depth;

    //! Keeps the current row data.
    TBlobOutput RowBuffer;

    //! |(endColumn, endValue)| offsets in #RowBuffer.
    std::vector<size_t> Offsets;

    NYson::TYsonWriter ValueWriter;

};

////////////////////////////////////////////////////////////////////////////////

class TTableConsumerBase
    : public NYson::IYsonConsumer
{
public:
    NVersionedTableClient::TNameTablePtr GetNameTable() const;

protected:
    TTableConsumerBase(
        const NVersionedTableClient::TTableSchema& schema,
        const NVersionedTableClient::TKeyColumns& keyColumns);

    virtual TError AttachLocationAttributes(TError error);

    virtual void OnControlIntegerScalar(i64 value);
    virtual void OnControlStringScalar(const TStringBuf& value);

    virtual void OnBeginRow() = 0;
    virtual void OnValue(const NVersionedTableClient::TUnversionedValue& value) = 0;
    virtual void OnEndRow() = 0;

    virtual void OnStringScalar(const TStringBuf& value) override;
    virtual void OnIntegerScalar(i64 value) override;
    virtual void OnDoubleScalar(double value) override;
    virtual void OnEntity() override;
    virtual void OnBeginList() override;
    virtual void OnListItem() override;
    virtual void OnBeginMap() override;
    virtual void OnKeyedItem(const TStringBuf& name) override;
    virtual void OnEndMap() override;

    virtual void OnBeginAttributes() override;

    void ThrowControlAttributesNotSupported();
    void ThrowMapExpected();
    void ThrowCompositesNotSupported();
    void ThrowInvalidSchemaColumnType(int columnId, NVersionedTableClient::EValueType actualType);
    void ThrowInvalidControlAttribute(const Stroka& whatsWrong);

    virtual void OnEndList() override;
    virtual void OnEndAttributes() override;
    virtual void OnRaw(const TStringBuf& yson, NYson::EYsonType type) override;

    void WriteValue(const NVersionedTableClient::TUnversionedValue& value);

    DECLARE_ENUM(EControlState,
        (None)
        (ExpectName)
        (ExpectValue)
        (ExpectEndAttributes)
        (ExpectEntity)
    );


    bool TreatMissingAsNull_;
    int KeyColumnCount_;
    NVersionedTableClient::TNameTablePtr NameTable_;

    EControlState ControlState_;
    EControlAttribute ControlAttribute_;

    int Depth_;
    int ColumnIndex_;

    struct TColumnDescriptor
    {
        TColumnDescriptor()
            : Written(false)
        { }

        bool Written;
        NVersionedTableClient::EValueType Type;
    };

    std::vector<TColumnDescriptor> SchemaColumnDescriptors_;

};

////////////////////////////////////////////////////////////////////////////////

class TWritingTableConsumer
    : public TTableConsumerBase
{
public:
    TWritingTableConsumer(
        const NVersionedTableClient::TTableSchema& schema,
        const NVersionedTableClient::TKeyColumns& keyColumns,
        NVersionedTableClient::IWriterPtr writer);

    TWritingTableConsumer(
        const NVersionedTableClient::TTableSchema& schema,
        const NVersionedTableClient::TKeyColumns& keyColumns,
        std::vector<NVersionedTableClient::IWriterPtr> writers,
        int tableIndex);

private:
    virtual TError AttachLocationAttributes(TError error) override;

    virtual void OnControlIntegerScalar(i64 value) override;
    virtual void OnControlStringScalar(const TStringBuf& value) override;

    virtual void OnBeginRow() override;
    virtual void OnValue(const NVersionedTableClient::TUnversionedValue& value) override;
    virtual void OnEndRow() override;


    int CurrentTableIndex_;
    std::vector<NVersionedTableClient::IWriterPtr> Writers_;
    NVersionedTableClient::IWriterPtr CurrentWriter_;

};

////////////////////////////////////////////////////////////////////////////////

class TBuildingTableConsumer
    : public TTableConsumerBase
{
public:
    TBuildingTableConsumer(
        const NVersionedTableClient::TTableSchema& schema,
        const NVersionedTableClient::TKeyColumns& keyColumns);

    const std::vector<NVersionedTableClient::TUnversionedOwningRow>& Rows() const;

    bool GetTreatMissingAsNull() const;
    void SetTreatMissingAsNull(bool value);

private:
    virtual TError AttachLocationAttributes(TError error) override;

    virtual void OnBeginRow() override;
    virtual void OnValue(const NVersionedTableClient::TUnversionedValue& value) override;
    virtual void OnEndRow() override;

    int RowIndex_;
    NVersionedTableClient::TUnversionedOwningRowBuilder Builder_;
    std::vector<NVersionedTableClient::TUnversionedOwningRow> Rows_;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
