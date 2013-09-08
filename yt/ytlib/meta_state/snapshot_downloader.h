#pragma once

#include "public.h"
#include "meta_state_manager_proxy.h"

#include <ytlib/concurrency/parallel_awaiter.h>

namespace NYT {
namespace NMetaState {

////////////////////////////////////////////////////////////////////////////////

class TSnapshotDownloader
    : private TNonCopyable
{
public:
    TSnapshotDownloader(
        TSnapshotDownloaderConfigPtr config,
        NElection::TCellManagerPtr cellManager);

    TError DownloadSnapshot(i32 snapshotId, const Stroka& fileName);

private:
    struct TSnapshotInfo
    {
        TPeerId SourceId;
        i64 Length;

        TSnapshotInfo()
        { }

        TSnapshotInfo(TPeerId owner, i64 length)
            : SourceId(owner)
            , Length(length)
        { }
    };

    typedef TMetaStateManagerProxy TProxy;

    TSnapshotDownloaderConfigPtr Config;
    NElection::TCellManagerPtr CellManager;

    TSnapshotInfo GetSnapshotInfo(i32 snapshotId); // also finds snapshot source
    static void OnSnapshotInfoResponse(
        NConcurrency::TParallelAwaiterPtr awaiter,
        TPromise<TSnapshotInfo> promise,
        TPeerId peerId,
        TProxy::TRspGetSnapshotInfoPtr response);
    static void OnSnapshotInfoComplete(
        i32 snapshotId,
        TPromise<TSnapshotInfo> promise);

    TError DownloadSnapshot(
        const Stroka& fileName,
        i32 snapshotId,
        const TSnapshotInfo& snapshotInfo);
    TError WriteSnapshot(
        i32 snapshotId,
        i64 snapshotLength,
        i32 sourceId,
        TOutputStream* output);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NMetaState
} // namespace NYT
