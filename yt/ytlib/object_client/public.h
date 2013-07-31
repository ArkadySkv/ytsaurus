#pragma once

#include <ytlib/misc/guid.h>

namespace NYT {
namespace NObjectClient {

////////////////////////////////////////////////////////////////////////////////

DECLARE_ENUM(EErrorCode,
    ((PrerequisiteCheckFailed)     (1000))
);
    
////////////////////////////////////////////////////////////////////////////////

//! Provides a globally unique identifier for an object.
/*!
 *  TGuid consists of four 32-bit parts.
 *  For TObjectId, these parts have the following meaning:
 *
 *  Part 0: some hash
 *  Part 1: bits 0..15:  object type
 *          bits 16..31: cell id
 *  Part 2: the lower  part of 64-bit sequential counter
 *  Part 3: the higher part of 64-bit sequential counter
 *
 */
typedef TGuid TObjectId;

//! The all-zero id used to denote a non-existing object.
extern TObjectId NullObjectId;

//! Identifies a particular installation of YT.
//! Must be unique to prevent object ids from colliding.
typedef ui16 TCellId;

//! Describes the runtime type of an object.
DECLARE_ENUM(EObjectType,
    // Does not represent any actual type.
    ((Null)                       (0))

    // The following are non-versioned objects.
    // These must be created by sending TTransactionYPathProxy::CreateObject to a transaction.
    // Except for EObjectType::Transaction, the latter transaction cannot be null.

    // Transaction Manager stuff
    ((Transaction)                (1))

    // Chunk Manager stuff
    ((Chunk)                      (100))
    ((ChunkList)                  (101))
    ((ErasureChunk)               (102)) // erasure chunk as a whole
    ((ErasureChunkPart_0)         (103)) // erasure chunk parts, mnemonic names are for debugging convenience only
    ((ErasureChunkPart_1)         (104))
    ((ErasureChunkPart_2)         (105))
    ((ErasureChunkPart_3)         (106))
    ((ErasureChunkPart_4)         (107))
    ((ErasureChunkPart_5)         (108))
    ((ErasureChunkPart_6)         (109))
    ((ErasureChunkPart_7)         (110))
    ((ErasureChunkPart_8)         (111))
    ((ErasureChunkPart_9)         (112))
    ((ErasureChunkPart_10)        (113))
    ((ErasureChunkPart_11)        (114))
    ((ErasureChunkPart_12)        (115))
    ((ErasureChunkPart_13)        (116))
    ((ErasureChunkPart_14)        (117))
    ((ErasureChunkPart_15)        (118))

    // The following are versioned objects AKA Cypress nodes.
    // These must be created by calling TCypressYPathProxy::Create.
    // NB: When adding a new type, don't forget to update IsVersioned.

    // Static nodes
    ((StringNode)                 (300))
    ((IntegerNode)                (301))
    ((DoubleNode)                 (302))
    ((MapNode)                    (303))
    ((ListNode)                   (304))

    // Dynamic nodes
    ((File)                       (400))
    ((Table)                      (401))
    ((ChunkMap)                   (402))
    ((LostChunkMap)               (403))
    ((OverreplicatedChunkMap)     (404))
    ((UnderreplicatedChunkMap)    (405))
    ((DataMissingChunkMap)        (419))
    ((ParityMissingChunkMap)      (420))
    ((ChunkListMap)               (406))
    ((TransactionMap)             (407))
    ((TopmostTransactionMap)      (418))
    ((CellNodeMap)                (408))
    ((CellNode)                   (410))
    ((Orchid)                     (412))
    ((LostVitalChunkMap)          (413))
    ((AccountMap)                 (414))
    ((UserMap)                    (415))
    ((GroupMap)                   (416))
    ((Link)                       (417))
    ((Document)                   (421))

    // Security
    ((Account)                    (500))
    ((User)                       (501))
    ((Group)                      (502))

    // A mysterious creature representing master as a whole.
    ((Master)                     (600))
);

//! Types (both regular and schematic) are supposed to be in range [0, MaxObjectType].
const int MaxObjectType = 65535;

//! Checks if the given type is versioned, i.e. represents a Cypress node.
bool IsVersionedType(EObjectType type);

//! Checks if the given type is user, i.e. regular users are allowed to create its instances.
bool IsUserType(EObjectType type);

//! Extracts the type component from an id.
EObjectType TypeFromId(const TObjectId& id);

//! Returns |true| iff a given regular type has an associated schema type.
bool HasSchema(EObjectType type);

//! Returns the schema type for a given regular type.
EObjectType SchemaTypeFromType(EObjectType type);

//! Returns the regular type for a given schema type.
EObjectType TypeFromSchemaType(EObjectType type);

//! Constructs the id from its parts.
TObjectId MakeId(
    EObjectType type,
    TCellId cellId,
    ui64 counter,
    ui32 hash);

//! Constructs a id corresponding to well-known (usually singleton) entities.
TObjectId MakeWellKnownId(
    EObjectType type,
    TCellId cellId,
    ui64 counter = 0xffffffffffffffff);

//! Returns the id of the schema object for a given regular type.
TObjectId MakeSchemaObjectId(
    EObjectType type,
    TCellId cellId);

TObjectId ReplaceTypeInId(
    const TObjectId& id,
    EObjectType type);

////////////////////////////////////////////////////////////////////////////////

typedef TObjectId TTransactionId;
extern TTransactionId NullTransactionId;

////////////////////////////////////////////////////////////////////////////////

//! Identifies a node possibly branched by a transaction.
struct TVersionedObjectId
{
    //! Id of the node itself.
    TObjectId ObjectId;

    //! Id of the transaction that had branched the node.
    //! #NullTransactionId if the node is not branched.
    TTransactionId TransactionId;

    //! Initializes a null instance.
    /*!
     *  #NodeId id #NullObjectId, #TransactionId is #NullTransactionId.
     */
    TVersionedObjectId();

    //! Initializes an instance by given node. Sets #TransactionId to #NullTransactionId.
    explicit TVersionedObjectId(const TObjectId& objectId);

    //! Initializes an instance by given node and transaction ids.
    TVersionedObjectId(const TObjectId& objectId, const TTransactionId& transactionId);

    //! Checks that the id is branched, i.e. #TransactionId is not #NullTransactionId.
    bool IsBranched() const;


    static TVersionedObjectId FromString(const TStringBuf& str);
};

//! Converts id into a string (for debugging and logging purposes mainly).
Stroka ToString(const TVersionedObjectId& id);

//! Compares TVersionedNodeId s for equality.
bool operator == (const TVersionedObjectId& lhs, const TVersionedObjectId& rhs);

//! Compares TVersionedNodeId s for inequality.
bool operator != (const TVersionedObjectId& lhs, const TVersionedObjectId& rhs);

//! Compares TVersionedNodeId s for "less than" (used to sort nodes in meta-map).
bool operator <  (const TVersionedObjectId& lhs, const TVersionedObjectId& rhs);

} // namespace NObjectClient
} // namespace NYT

////////////////////////////////////////////////////////////////////////////////

DECLARE_PODTYPE(NYT::NObjectClient::TVersionedObjectId);

//! A hasher for TVersionedNodeId.
template <>
struct hash<NYT::NObjectClient::TVersionedObjectId>
{
    size_t operator() (const NYT::NObjectClient::TVersionedObjectId& id) const
    {
        return THash<NYT::TGuid>()(id.TransactionId) * 497 +
               THash<NYT::TGuid>()(id.ObjectId);
    }
};

////////////////////////////////////////////////////////////////////////////////

