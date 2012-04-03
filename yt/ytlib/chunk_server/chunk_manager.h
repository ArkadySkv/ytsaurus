#pragma once

#include "public.h"
#include <ytlib/chunk_server/chunk_manager.pb.h>

#include <ytlib/actions/signal.h>
#include <ytlib/meta_state/composite_meta_state.h>
#include <ytlib/meta_state/meta_change.h>
#include <ytlib/meta_state/map.h>
#include <ytlib/cell_master/public.h>

namespace NYT {
namespace NChunkServer {

////////////////////////////////////////////////////////////////////////////////

class TChunkManager
    : public TRefCounted
{
public:
    //! Creates an instance.
    TChunkManager(
        TChunkManagerConfigPtr config,
        NCellMaster::TBootstrap* bootstrap);

    NMetaState::TMetaChange< yvector<TChunkId> >::TPtr InitiateCreateChunks(
        const NProto::TMsgCreateChunks& message);

    NMetaState::TMetaChange<THolderId>::TPtr InitiateRegisterHolder(
        const NProto::TMsgRegisterHolder& message);

    NMetaState::TMetaChange<TVoid>::TPtr InitiateUnregisterHolder(
        const NProto::TMsgUnregisterHolder& message);

    NMetaState::TMetaChange<TVoid>::TPtr InitiateFullHeartbeat(
        const NProto::TMsgFullHeartbeat & message);

    NMetaState::TMetaChange<TVoid>::TPtr InitiateIncrementalHeartbeat(
        const NProto::TMsgIncrementalHeartbeat& message);

    NMetaState::TMetaChange<TVoid>::TPtr InitiateUpdateJobs(
        const NProto::TMsgUpdateJobs& message);

    DECLARE_METAMAP_ACCESSORS(Chunk, TChunk, TChunkId);
    DECLARE_METAMAP_ACCESSORS(ChunkList, TChunkList, TChunkListId);
    DECLARE_METAMAP_ACCESSORS(Holder, THolder, THolderId);
    DECLARE_METAMAP_ACCESSORS(JobList, TJobList, TChunkId);
    DECLARE_METAMAP_ACCESSORS(Job, TJob, TJobId);

    //! Fired when a holder gets registered.
    /*!
     *  \note
     *  Only fired for leaders, not fired during recovery.
     */
    DECLARE_SIGNAL(void(const THolder&), HolderRegistered);
    //! Fired when a holder gets unregistered.
    /*!
     *  \note
     *  Only fired for leaders, not fired during recovery.
     */
    DECLARE_SIGNAL(void(const THolder&), HolderUnregistered);

    const THolder* FindHolder(const Stroka& address) const;
    THolder* FindHolder(const Stroka& address);
    const TReplicationSink* FindReplicationSink(const Stroka& address);

    yvector<THolderId> AllocateUploadTargets(int replicaCount);

    TChunk& CreateChunk();
    TChunkList& CreateChunkList();

    void AttachToChunkList(TChunkList& chunkList, const yvector<TChunkTreeRef> &children);
    void DetachFromChunkList(TChunkList& chunkList, const yvector<TChunkTreeRef>& children);

    void ScheduleJobs(
        const THolder& holder,
        const yvector<NProto::TJobInfo>& runningJobs,
        yvector<NProto::TJobStartInfo>* jobsToStart,
        yvector<NProto::TJobStopInfo>* jobsToStop);

    bool IsJobSchedulerEnabled();

    //! Fills a given protobuf structure with the list of holder addresses.
    /*!
     *  Not too nice but seemingly fast.
     */
    void FillHolderAddresses(
        ::google::protobuf::RepeatedPtrField< TProtoStringType>* addresses,
        const TChunk& chunk);

    const yhash_set<TChunkId>& LostChunkIds() const;
    const yhash_set<TChunkId>& OverreplicatedChunkIds() const;
    const yhash_set<TChunkId>& UnderreplicatedChunkIds() const;

    TTotalHolderStatistics GetTotalHolderStatistics();

    bool IsHolderConfirmed(const THolder& holder);

    //! Returns the total number of all chunk replicas.
    i32 GetChunkReplicaCount();

private:
    class TImpl;
    class TChunkTypeHandler;
    class TChunkProxy;
    class TChunkListTypeHandler;
    class TChunkListProxy;
    
    TIntrusivePtr<TImpl> Impl;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkServer
} // namespace NYT
