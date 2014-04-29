#include "stdafx.h"
#include "schema.h"

#include <core/ytree/serialize.h>
#include <core/ytree/convert.h>

#include <core/misc/protobuf_helpers.h>

#include <ytlib/new_table_client/chunk_meta.pb.h>
#include <ytlib/table_client/table_chunk_meta.pb.h> // TODO(babenko): remove after migration
#include "row_base.h"

namespace NYT {
namespace NVersionedTableClient {

using namespace NYTree;
using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

TColumnSchema::TColumnSchema()
    : Type(EValueType::Null)
{ }

TColumnSchema::TColumnSchema(const Stroka& name, EValueType type)
    : Name(name)
    , Type(type)
{ }

struct TSerializableColumnSchema
    : public TYsonSerializableLite
    , public TColumnSchema
{
    TSerializableColumnSchema()
    {
        RegisterAll();
    }

    explicit TSerializableColumnSchema(const TColumnSchema& other)
        : TColumnSchema(other)
    {
        RegisterAll();
    }

    void RegisterAll()
    {
        RegisterParameter("name", Name)
            .NonEmpty();
        RegisterParameter("type", Type);
    }
};

void Serialize(const TColumnSchema& schema, IYsonConsumer* consumer)
{
    TSerializableColumnSchema wrapper(schema);
    Serialize(static_cast<const TYsonSerializableLite&>(wrapper), consumer);
}

void Deserialize(TColumnSchema& schema, INodePtr node)
{
    TSerializableColumnSchema wrapper;
    Deserialize(static_cast<TYsonSerializableLite&>(wrapper), node);
    // TODO(babenko): we shouldn't be concerned with manual validation here
    wrapper.Validate();
    schema = static_cast<TColumnSchema&>(wrapper);
}

void ToProto(NProto::TColumnSchema* protoSchema, const TColumnSchema& schema)
{
    protoSchema->set_name(schema.Name);
    protoSchema->set_type(schema.Type);
}

void FromProto(TColumnSchema* schema, const NProto::TColumnSchema& protoSchema)
{
    schema->Name = protoSchema.name();
    schema->Type = EValueType(protoSchema.type());
}

////////////////////////////////////////////////////////////////////////////////

TColumnSchema* TTableSchema::FindColumn(const TStringBuf& name)
{
    for (auto& column : Columns_) {
        if (column.Name == name) {
            return &column;
        }
    }
    return nullptr;
}

const TColumnSchema* TTableSchema::FindColumn(const TStringBuf& name) const
{
    for (auto& column : Columns_) {
        if (column.Name == name) {
            return &column;
        }
    }
    return nullptr;
}

TColumnSchema& TTableSchema::GetColumnOrThrow(const TStringBuf& name)
{
    auto* column = FindColumn(name);
    if (!column) {
        THROW_ERROR_EXCEPTION("Missing schema column %s",
            ~Stroka(name).Quote());
    }
    return *column;
}

const TColumnSchema& TTableSchema::GetColumnOrThrow(const TStringBuf& name) const
{
    auto* column = FindColumn(name);
    if (!column) {
        THROW_ERROR_EXCEPTION("Missing schema column %s",
            ~Stroka(name).Quote());
    }
    return *column;
}

int TTableSchema::GetColumnIndex(const TColumnSchema& column) const
{
    return &column - Columns().data();
}

int TTableSchema::GetColumnIndexOrThrow(const TStringBuf& name) const
{
    return GetColumnIndex(GetColumnOrThrow(name));
}

TError TTableSchema::CheckKeyColumns(const TKeyColumns& keyColumns) const
{
    // ToDo(psushin): provide ToString for TTableSchema and make better error messages.
    if (Columns_.size() < keyColumns.size()) {
        return TError("Schema doesn't contain all key columns");
    }

    for (int index = 0; index < static_cast<int>(keyColumns.size()); ++index) {
        if (Columns_[index].Name != keyColumns[index]) {
            return TError("Key columns must form a prefix of schema");;
        }
    }

    return TError();
}

void TTableSchema::Save(TStreamSaveContext& context) const
{
    NYT::Save(context, NYT::ToProto<NVersionedTableClient::NProto::TTableSchemaExt>(*this));
}

void TTableSchema::Load(TStreamLoadContext& context)
{
    NVersionedTableClient::NProto::TTableSchemaExt protoSchema;
    NYT::Load(context, protoSchema);
    *this = NYT::FromProto<TTableSchema>(protoSchema);
}

////////////////////////////////////////////////////////////////////////////////


void Serialize(const TTableSchema& schema, IYsonConsumer* consumer)
{
    NYTree::Serialize(schema.Columns(), consumer);
}

void Deserialize(TTableSchema& schema, INodePtr node)
{
    NYTree::Deserialize(schema.Columns(), node);
    
    // Check for duplicate names.
    yhash_set<Stroka> names;
    for (const auto& column : schema.Columns()) {
        try {
            if (column.Name.empty()) {
                THROW_ERROR_EXCEPTION("Column name cannot be empty");
            }
            ValidateSchemaValueType(column.Type);
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error validating column %s in table schema",
                ~column.Name.Quote())
                << ex;
        }
        if (!names.insert(column.Name).second) {
            THROW_ERROR_EXCEPTION("Duplicate column %s in table schema",
                ~column.Name.Quote());
        }
    }
}

void ToProto(NProto::TTableSchemaExt* protoSchema, const TTableSchema& schema)
{
    NYT::ToProto(protoSchema->mutable_columns(), schema.Columns());
}

void FromProto(TTableSchema* schema, const NProto::TTableSchemaExt& protoSchema)
{
    schema->Columns() = NYT::FromProto<TColumnSchema>(protoSchema.columns());
}

////////////////////////////////////////////////////////////////////////////////

bool operator == (const TColumnSchema& lhs, const TColumnSchema& rhs)
{
    return lhs.Name == rhs.Name && lhs.Type == rhs.Type;
}

bool operator != (const TColumnSchema& lhs, const TColumnSchema& rhs)
{
    return !(lhs == rhs);
}

bool operator == (const TTableSchema& lhs, const TTableSchema& rhs)
{
    return lhs.Columns() == rhs.Columns();
}

bool operator != (const TTableSchema& lhs, const TTableSchema& rhs)
{
    return !(lhs == rhs);
}

////////////////////////////////////////////////////////////////////////////////

void ValidateKeyColumns(const TKeyColumns& keyColumns)
{
    if (keyColumns.size() > MaxKeyColumnCount) {
        THROW_ERROR_EXCEPTION("Too many key columns: %d > %d",
            static_cast<int>(keyColumns.size()),
            MaxKeyColumnCount);
    }

    yhash_set<Stroka> names;
    for (const auto& name : keyColumns) {
        if (!names.insert(name).second) {
            THROW_ERROR_EXCEPTION("Duplicate key column name %s",
                ~name.Quote());
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NVersionedTableClient
} // namespace NYT


namespace NYT {
namespace NTableClient {
namespace NProto {

void ToProto(TKeyColumnsExt* protoKeyColumns, const TKeyColumns& keyColumns)
{
    NYT::ToProto(protoKeyColumns->mutable_names(), keyColumns);
}

void FromProto(TKeyColumns* keyColumns, const TKeyColumnsExt& protoKeyColumns)
{
    *keyColumns = NYT::FromProto<Stroka>(protoKeyColumns.names());
}

}
}
}


