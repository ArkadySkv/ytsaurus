#pragma once

#include "public.h"

#include <ytlib/actions/action_queue.h>
#include <ytlib/actions/signal.h>

#include <ytlib/logging/tagged_logger.h>

#include <ytlib/profiling/profiler.h>

#include <server/cell_node/public.h>

#include <atomic>

namespace NYT {
namespace NChunkHolder {

////////////////////////////////////////////////////////////////////////////////

DECLARE_ENUM(ELocationType,
    (Store)
    (Cache)
);

//! Describes a physical location of chunks at a chunk holder.
class TLocation
    : public TRefCounted
{
public:
    TLocation(
        ELocationType type,
        const Stroka& id,
        TLocationConfigPtr config,
        NCellNode::TBootstrap* bootstrap);

    ~TLocation();

    //! Returns the type.
    ELocationType GetType() const;

    //! Returns string id.
    const Stroka& GetId() const;

    //! Returns the cell guid. If no tag file was found and #UpdateCellGuid was not called
    //! then empty guid is returned.
    const TGuid& GetCellGuid();

    //! Sets the cell guid and overwrites the tag file.
    void SetCellGuid(const TGuid& guid);

    //! Scan the location directory removing orphaned files and returning the list of found chunks.
    //! If the scan fails, the location becomes disabled, |Disabled| signal is raised, and an empty list is returned.
    std::vector<TChunkDescriptor> Initialize();

    //! Updates #UsedSpace and #AvailalbleSpace
    void UpdateUsedSpace(i64 size);

    //! Schedules physical removal of a chunk.
    // NB: Don't try replacing TChunk with TChunkPtr since
    // this method is called from TCachedChunk::dtor.
    TFuture<void> ScheduleChunkRemoval(TChunk* chunk);

    //! Updates #AvailalbleSpace with a system call and returns the result.
    //! Never throws.
    i64 GetAvailableSpace() const;

    //! Returns the total space on the disk drive where the location resides.
    //! Never throws.
    i64 GetTotalSpace() const;

    //! Returns the bootstrap.
    NCellNode::TBootstrap* GetBootstrap() const;

    //! Returns the number of bytes used at the location.
    /*!
     *  \note
     *  This may exceed #GetQuota.
     */
    i64 GetUsedSpace() const;

    //! Returns the maximum number of bytes the chunks assigned to this location
    //! are allowed to use.
    i64 GetQuota() const;

    //! Returns the path of the location.
    Stroka GetPath() const;

    //! Returns the load factor.
    double GetLoadFactor() const;

    //! Changes the number of currently active sessions by a given delta.
    void UpdateSessionCount(int delta);

    //! Changes the number of chunks by a given delta.
    void UpdateChunkCount(int delta);

    //! Returns the number of currently active sessions.
    int GetSessionCount() const;

    //! Returns the number of chunks.
    int GetChunkCount() const;

    //! Returns a full path to a chunk file.
    Stroka GetChunkFileName(const TChunkId& chunkId) const;

    //! Checks whether the location is full.
    bool IsFull() const;

    //! Checks whether to location has enough space to contain file of size #size
    bool HasEnoughSpace(i64 size) const;

    //! Returns an invoker for reading chunk data.
    IPrioritizedInvokerPtr GetDataReadInvoker();

    //! Returns an invoker for reading chunk meta.
    IPrioritizedInvokerPtr GetMetaReadInvoker();

    //! Returns an invoker for writing chunks.
    IInvokerPtr GetWriteInvoker();

    //! Returns True iff the location is enabled.
    bool IsEnabled() const;

    //! Marks the location as disabled.
    void Disable();

    //! Raised when the location gets disabled.
    /*!
     *  Raised at most once in Control thread.
     */
    DEFINE_SIGNAL(void(), Disabled);

    //! The profiler tagged with location id.
    DEFINE_BYREF_RW_PROPERTY(NProfiling::TProfiler, Profiler);

private:
    ELocationType Type;
    Stroka Id;
    TLocationConfigPtr Config;
    NCellNode::TBootstrap* Bootstrap;

    std::atomic<bool> Enabled;

    TGuid CellGuid;

    mutable i64 AvailableSpace;
    i64 UsedSpace;
    int SessionCount;
    int ChunkCount;

    TFairShareActionQueuePtr ReadQueue;
    IPrioritizedInvokerPtr DataReadInvoker;
    IPrioritizedInvokerPtr MetaReadInvoker;

    TThreadPoolPtr WriteQueue;
    IInvokerPtr WriteInvoker;

    TDiskHealthCheckerPtr HealthChecker;

    mutable NLog::TTaggedLogger Logger;

    std::vector<TChunkDescriptor> DoInitialize();
    void OnHealthCheckFailed();
    void ScheduleDisable();
    void DoDisable();

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkHolder
} // namespace NYT

