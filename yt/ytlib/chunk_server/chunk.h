#pragma once

#include "common.h"
#include "id.h"
#include "chunk.pb.h"

#include <ytlib/misc/property.h>
#include <ytlib/object_server/object_detail.h>

namespace NYT {
namespace NChunkServer {

////////////////////////////////////////////////////////////////////////////////

class TChunk
    : public NObjectServer::TObjectWithIdBase
{
    DEFINE_BYVAL_RW_PROPERTY(i64, Size);
    DEFINE_BYVAL_RW_PROPERTY(TSharedRef, Attributes);
    // Usually small, e.g. 3 replicas.
    DEFINE_BYREF_RO_PROPERTY(yvector<THolderId>, StoredLocations);
    // Usually empty.
    DEFINE_BYREF_RO_PROPERTY(::THolder< yhash_set<THolderId> >, CachedLocations);

public:
    static const i64 UnknownSize = -1;
    
    TChunk(const TChunkId& id);

    void Save(TOutputStream* output) const;
    void Load(TInputStream* input, TVoid /* context */);

    void AddLocation(THolderId holderId, bool cached);
    void RemoveLocation(THolderId holderId, bool cached);
    yvector<THolderId> GetLocations() const;

    bool IsConfirmed() const;

    NChunkHolder::NProto::TChunkAttributes DeserializeAttributes() const;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkServer
} // namespace NYT
