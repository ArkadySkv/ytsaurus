#pragma once

#include "public.h"

#include <ytlib/misc/property.h>

#include <server/cell_master/public.h>

#include <server/chunk_server/chunk_service.pb.h>

namespace NYT {
namespace NChunkServer {

////////////////////////////////////////////////////////////////////////////////

DECLARE_ENUM(ENodeState,
    // Not registered.
    (Offline)
    // Registered but did not report the full heartbeat yet.
    (Registered)
    // Registered and reported the full heartbeat.
    (Online)
);

class TDataNode
{
    DEFINE_BYVAL_RO_PROPERTY(TNodeId, Id);
    DEFINE_BYVAL_RO_PROPERTY(Stroka, Address);
    DEFINE_BYVAL_RO_PROPERTY(TIncarnationId, IncarnationId);
    DEFINE_BYVAL_RW_PROPERTY(ENodeState, State);
    DEFINE_BYREF_RW_PROPERTY(NProto::TNodeStatistics, Statistics);
    DEFINE_BYREF_RW_PROPERTY(yhash_set<TChunk*>, StoredChunks);
    DEFINE_BYREF_RW_PROPERTY(yhash_set<TChunk*>, CachedChunks);
    DEFINE_BYREF_RW_PROPERTY(yhash_set<TChunk*>, UnapprovedChunks);
    DEFINE_BYREF_RO_PROPERTY(std::vector<TJob*>, Jobs);
    DEFINE_BYVAL_RW_PROPERTY(int, HintedSessionCount);

    //! Indexed by priority.
    typedef std::vector< yhash_set<TChunkId> > TChunksToReplicate;
    DEFINE_BYREF_RW_PROPERTY(TChunksToReplicate, ChunksToReplicate);

    //! NB: Ids are used instead of raw pointers since these chunks are typically already dead.
    typedef yhash_set<TChunkId> TChunksToRemove;
    DEFINE_BYREF_RW_PROPERTY(TChunksToRemove, ChunksToRemove);

public:
    TDataNode(
        TNodeId id,
        const Stroka& address,
        const TIncarnationId& incarnationId);

    explicit TDataNode(TNodeId id);

    void Save(const NCellMaster::TSaveContext& context) const;
    void Load(const NCellMaster::TLoadContext& context);

    void AddChunk(TChunk* chunk, bool cached);
    void RemoveChunk(TChunk* chunk, bool cached);
    bool HasChunk(TChunk* chunk, bool cached) const;

    void MarkChunkUnapproved(TChunk* chunk);
    bool HasUnapprovedChunk(TChunk* chunk) const;
    void ApproveChunk(TChunk* chunk);

    void AddJob(TJob* job);
    void RemoveJob(TJob* id);

    int GetTotalSessionCount() const;

private:
    void Init();
    
};

////////////////////////////////////////////////////////////////////////////////

class TReplicationSink
{
    DEFINE_BYVAL_RO_PROPERTY(Stroka, Address);
    DEFINE_BYREF_RW_PROPERTY(yhash_set<TJob*>, Jobs);

public:
    explicit TReplicationSink(const Stroka &address)
        : Address_(address)
    { }

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkServer
} // namespace NYT
