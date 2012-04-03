#include "stdafx.h"
#include "common.h"
#include "snapshot_store.h"
#include "snapshot.h"

#include <ytlib/misc/fs.h>
#include <ytlib/misc/thread_affinity.h>

#include <util/folder/filelist.h>
#include <util/folder/dirut.h>

namespace NYT {
namespace NMetaState {

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = MetaStateLogger;
static const char* const SnapshotExtension = "snapshot";

////////////////////////////////////////////////////////////////////////////////

TSnapshotStore::TSnapshotStore(const Stroka& path)
    : Path(path)
    , Started(false)
{ }

void TSnapshotStore::Start()
{
    YASSERT(!Started);

    LOG_INFO("Preparing snapshot directory %s", ~Path.Quote());

    NFS::ForcePath(Path);
    NFS::CleanTempFiles(Path);

    LOG_INFO("Looking for snapshots in %s", ~Path.Quote());

    TFileList fileList;
    fileList.Fill(Path);

    i32 maxSnapshotId = NonexistingSnapshotId;
    Stroka fileName;
    while ((fileName = fileList.Next()) != NULL) {
        auto extension = NFS::GetFileExtension(fileName);
        if (extension == SnapshotExtension) {
            auto name = NFS::GetFileNameWithoutExtension(fileName);
            try {
                i32 snapshotId = FromString<i32>(name);
                SnapshotIds.insert(snapshotId);
                LOG_INFO("Found snapshot %d", snapshotId);
            } catch (const yexception&) {
                LOG_WARNING("Found unrecognized file %s", ~fileName.Quote());
            }
        }
    }

    LOG_INFO("Snapshot scan complete");
    Started = true;
}

Stroka TSnapshotStore::GetSnapshotFileName(i32 snapshotId) const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return NFS::CombinePaths(Path, Sprintf("%09d.%s", snapshotId, SnapshotExtension));
}

TSnapshotStore::TGetReaderResult TSnapshotStore::GetReader(i32 snapshotId) const
{
    VERIFY_THREAD_AFFINITY_ANY();
    YASSERT(Started);
    YASSERT(snapshotId > 0);

    auto fileName = GetSnapshotFileName(snapshotId);
    if (!isexist(~fileName)) {
        return TError(
            EErrorCode::NoSuchSnapshot,
            Sprintf("No such snapshot %d", snapshotId));
    }
    return New<TSnapshotReader>(fileName, snapshotId);
}

TSnapshotWriterPtr TSnapshotStore::GetWriter(i32 snapshotId) const
{
    VERIFY_THREAD_AFFINITY_ANY();
    YASSERT(Started);
    YASSERT(snapshotId > 0);

    auto fileName = GetSnapshotFileName(snapshotId);
    return New<TSnapshotWriter>(fileName, snapshotId);
}

i32 TSnapshotStore::LookupLatestSnapshot(i32 maxSnapshotId)
{
    VERIFY_THREAD_AFFINITY_ANY();
    YASSERT(Started);

    while (true) {
        i32 snapshotId;

        // Fetch the most appropriate id from the set.
        {
            TGuard<TSpinLock> guard(SpinLock);
            auto it = SnapshotIds.upper_bound(maxSnapshotId);
            if (it == SnapshotIds.begin()) {
                return NonexistingSnapshotId;
            }
            snapshotId = *(--it);
            YASSERT(snapshotId <= maxSnapshotId);
        }

        // Check that the file really exists.
        auto fileName = GetSnapshotFileName(snapshotId);
        if (isexist(~fileName)) {
            return snapshotId;
        }

        // Remove the orphaned id from the set and retry.
        {
            TGuard<TSpinLock> guard(SpinLock);
            SnapshotIds.erase(snapshotId);
        }
    }
}

void TSnapshotStore::OnSnapshotAdded(i32 snapshotId)
{
    VERIFY_THREAD_AFFINITY_ANY();
    YASSERT(Started);

    TGuard<TSpinLock> guard(SpinLock);
    SnapshotIds.insert(snapshotId);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NMetaState
} // namespace NYT
