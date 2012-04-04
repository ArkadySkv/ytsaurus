#pragma once

#include "yson_consumer.h"

namespace NYT {
namespace NYTree {

////////////////////////////////////////////////////////////////////////////////

//! The data format.
DECLARE_ENUM(EYsonFormat,
    // Binary.
    // Most compact but not human-readable.
    (Binary)
    // Text.
    // Not so compact but human-readable.
    // Does not use indentation.
    // Uses escaping for non-text characters.
    (Text)
    // Text with indentation.
    // Extremely verbose but human-readable.
    // Uses escaping for non-text characters.
    (Pretty)
);

//! Creates a YSON data stream from a sequence of YSON events.
class TYsonWriter
    : public IYsonConsumer
    , private TNonCopyable
{
public:
    //! Initializes an instance.
    /*!
     *  \param stream A stream for outputting the YSON data.
     *  \param format A format used for encoding the data.
     */
    TYsonWriter(TOutputStream* stream, EYsonFormat format = EYsonFormat::Binary);

    // IYsonConsumer overrides.
    virtual void OnStringScalar(const Stroka& value, bool hasAttributes = false);
    virtual void OnIntegerScalar(i64 value, bool hasAttributes = false);
    virtual void OnDoubleScalar(double value, bool hasAttributes = false);
    virtual void OnEntity(bool hasAttributes = false);

    virtual void OnBeginList();
    virtual void OnListItem();
    virtual void OnEndList(bool hasAttributes = false);

    virtual void OnBeginMap();
    virtual void OnMapItem(const Stroka& name);
    virtual void OnEndMap(bool hasAttributes = false);

    virtual void OnBeginAttributes();
    virtual void OnAttributesItem(const Stroka& name);
    virtual void OnEndAttributes();

    //! Inserts a portion of raw YSON into the stream.
    void OnRaw(const TYson& yson);

private:
    TOutputStream* Stream;
    bool IsFirstItem;
    bool IsEmptyEntity;
    int Indent;
    EYsonFormat Format;

    static const int IndentSize = 4;

    void WriteIndent();
    void WriteStringScalar(const Stroka& value);
    void WriteMapItem(const Stroka& name);

    void BeginCollection(char openBracket);
    void CollectionItem(char separator);
    void EndCollection(char closeBracket);

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT

