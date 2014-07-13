#include "stdafx.h"
#include "recovery.h"
#include "config.h"
#include "decorated_automaton.h"
#include "changelog.h"
#include "snapshot.h"
#include "changelog_download.h"

#include <core/concurrency/scheduler.h>

#include <ytlib/election/cell_manager.h>

#include <ytlib/hydra/hydra_service_proxy.h>
#include <ytlib/hydra/hydra_manager.pb.h>

namespace NYT {
namespace NHydra {

using namespace NElection;
using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

TRecovery::TRecovery(
    TDistributedHydraManagerConfigPtr config,
    TCellManagerPtr cellManager,
    TDecoratedAutomatonPtr decoratedAutomaton,
    IChangelogStorePtr changelogStore,
    ISnapshotStorePtr snapshotStore,
    TEpochContextPtr epochContext)
    : Config_(config)
    , CellManager_(cellManager)
    , DecoratedAutomaton_(decoratedAutomaton)
    , ChangelogStore_(changelogStore)
    , SnapshotStore_(snapshotStore)
    , EpochContext_(epochContext)
    , Logger(HydraLogger)
{
    YCHECK(Config_);
    YCHECK(CellManager_);
    YCHECK(DecoratedAutomaton_);
    YCHECK(ChangelogStore_);
    YCHECK(SnapshotStore_);
    YCHECK(EpochContext_);
    VERIFY_INVOKER_AFFINITY(EpochContext_->EpochSystemAutomatonInvoker, AutomatonThread);

    Logger.AddTag("CellGuid: %v", CellManager_->GetCellGuid());
}

void TRecovery::RecoverToVersion(TVersion targetVersion)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    auto snapshotIdOrError = WaitFor(SnapshotStore_->GetLatestSnapshotId(targetVersion.SegmentId));
    THROW_ERROR_EXCEPTION_IF_FAILED(snapshotIdOrError, "Error computing the latest snapshot id");

    int snapshotId = snapshotIdOrError.Value();
    YCHECK(snapshotId <= targetVersion.SegmentId);

    auto currentVersion = DecoratedAutomaton_->GetAutomatonVersion();
    YCHECK(currentVersion <= targetVersion);

    LOG_INFO("Recoverying from version %v to version %v",
        currentVersion,
        targetVersion);

    int initialChangelogId;
    if (snapshotId != NonexistingSegmentId && snapshotId > currentVersion.SegmentId) {
        // Load the snapshot.
        LOG_DEBUG("Using snapshot %v for recovery", snapshotId);

        auto readerOrError = WaitFor(SnapshotStore_->CreateReader(snapshotId));
        THROW_ERROR_EXCEPTION_IF_FAILED(readerOrError, "Error creating snapshot reader");
        auto reader = readerOrError.Value();

        auto snapshotParamsOrError = WaitFor(SnapshotStore_->GetSnapshotParams(snapshotId));
        THROW_ERROR_EXCEPTION_IF_FAILED(snapshotParamsOrError);
        const auto& snapshotParams = snapshotParamsOrError.Value();

        auto snapshotVersion = TVersion(snapshotId - 1, snapshotParams.PrevRecordCount);
        auto* input = reader->GetStream();
        DecoratedAutomaton_->LoadSnapshot(snapshotVersion, input);
        initialChangelogId = snapshotId;
    } else {
        // Recover using changelogs only.
        LOG_INFO("Not using snapshots for recovery");
        initialChangelogId = currentVersion.SegmentId;
    }

    LOG_INFO("Replaying changelogs %d-%d to reach version %v",
        initialChangelogId,
        targetVersion.SegmentId,
        targetVersion);

    for (int changelogId = initialChangelogId; changelogId <= targetVersion.SegmentId; ++changelogId) {
        bool isLastChangelog = (changelogId == targetVersion.SegmentId);
        auto changelogOrError = WaitFor(ChangelogStore_->TryOpenChangelog(changelogId));
        THROW_ERROR_EXCEPTION_IF_FAILED(changelogOrError);
        auto changelog = changelogOrError.Value();
        if (!changelog) {
            auto currentVersion = DecoratedAutomaton_->GetAutomatonVersion();

            LOG_INFO("Changelog %v is missing and will be created at version %v",
                changelogId,
                currentVersion);

            NProto::TChangelogMeta meta;
            meta.set_prev_record_count(currentVersion.RecordId);
            
            TSharedRef metaBlob;
            YCHECK(SerializeToProto(meta, &metaBlob));

            auto changelogOrError = WaitFor(ChangelogStore_->CreateChangelog(changelogId, metaBlob));
            THROW_ERROR_EXCEPTION_IF_FAILED(changelogOrError);
            changelog = changelogOrError.Value();

            TVersion newLoggedVersion(changelogId, 0);
            // NB: Equality is only possible when segmentId == 0.
            YCHECK(DecoratedAutomaton_->GetLoggedVersion() <= newLoggedVersion);
            DecoratedAutomaton_->SetLoggedVersion(newLoggedVersion);
        }

        DecoratedAutomaton_->SetChangelog(changelog);

        if (!IsLeader()) {
            SyncChangelog(changelog, changelogId);
        }

        if (!isLastChangelog && !changelog->IsSealed()) {
            WaitFor(changelog->Flush());
            if (changelog->IsSealed()) {
                LOG_WARNING("Changelog %v is already sealed",
                    changelogId);
            } else {
                WaitFor(changelog->Seal(changelog->GetRecordCount()));
            }
        }

        int targetRecordId = isLastChangelog ? targetVersion.RecordId : changelog->GetRecordCount();
        ReplayChangelog(changelog, changelogId, targetRecordId);
    }
}

void TRecovery::SyncChangelog(IChangelogPtr changelog, int changelogId)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    THydraServiceProxy proxy(CellManager_->GetPeerChannel(EpochContext_->LeaderId));
    proxy.SetDefaultTimeout(Config_->RpcTimeout);

    auto req = proxy.LookupChangelog();
    req->set_changelog_id(changelogId);

    auto rsp = WaitFor(req->Invoke());
    THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error getting changelog %v info from leader",
        changelogId);

    int remoteRecordCount = rsp->record_count();
    int localRecordCount = changelog->GetRecordCount();
    // NB: Don't download records past the sync point since they are expected to be postponed.
    int syncRecordCount =
        changelogId == SyncVersion_.SegmentId
        ? SyncVersion_.RecordId
        : remoteRecordCount;

    LOG_INFO("Syncing changelog %v: local %v, remote %v, sync %v",
        changelogId,
        localRecordCount,
        remoteRecordCount,
        syncRecordCount);

    if (localRecordCount > remoteRecordCount) {
        YCHECK(syncRecordCount == remoteRecordCount);
        if (changelog->IsSealed()) {
            LOG_FATAL("Cannot truncate a sealed changelog %v",
                changelogId);
        } else {
            WaitFor(changelog->Seal(remoteRecordCount));

            TVersion sealedVersion(changelogId, remoteRecordCount);
            if (DecoratedAutomaton_->GetLoggedVersion().SegmentId == sealedVersion.SegmentId) {
                DecoratedAutomaton_->SetLoggedVersion(sealedVersion);
            }
        }
    } else if (localRecordCount < syncRecordCount) {
        auto asyncResult = DownloadChangelog(
            Config_,
            CellManager_,
            ChangelogStore_,
            changelogId,
            syncRecordCount);
        auto result = WaitFor(asyncResult);

        TVersion downloadedVersion(changelogId, changelog->GetRecordCount());
        DecoratedAutomaton_->SetLoggedVersion(std::max(DecoratedAutomaton_->GetLoggedVersion(), downloadedVersion));

        THROW_ERROR_EXCEPTION_IF_FAILED(result, "Error downloading changelog records");
    }
}

void TRecovery::ReplayChangelog(IChangelogPtr changelog, int changelogId, int targetRecordId)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    auto currentVersion = DecoratedAutomaton_->GetAutomatonVersion();
    LOG_INFO("Replaying changelog %v from version %v to version %v",
        changelogId,
        currentVersion,
        TVersion(changelogId, targetRecordId));

    if (currentVersion.SegmentId != changelogId) {
        YCHECK(currentVersion.SegmentId == changelogId - 1);

        NProto::TChangelogMeta meta;
        YCHECK(DeserializeFromProto(&meta, changelog->GetMeta()));

        YCHECK(meta.prev_record_count() == currentVersion.RecordId);

        // Prepare to apply mutations at the rotated version.
        DecoratedAutomaton_->RotateAutomatonVersion(changelogId);
    }

    if (changelog->GetRecordCount() < targetRecordId) {
        LOG_FATAL("Not enough records in changelog %v: needed %v, actual %v",
            changelogId,
            targetRecordId,
            changelog->GetRecordCount());
    }

    while (true) {
        int startRecordId = DecoratedAutomaton_->GetAutomatonVersion().RecordId;
        int recordsNeeded = targetRecordId - startRecordId;
        YCHECK(recordsNeeded >= 0);
        if (recordsNeeded == 0)
            break;
    
        LOG_INFO("Trying to read records %v-%v from changelog %v",
            startRecordId,
            targetRecordId - 1,
            changelogId);

        auto recordsData = changelog->Read(
            startRecordId,
            recordsNeeded,
            Config_->MaxChangelogReadSize);
        int recordsRead = static_cast<int>(recordsData.size());

        LOG_INFO("Finished reading records %v-%v from changelog %v",
            startRecordId,
            startRecordId + recordsRead - 1,
            changelogId);

        LOG_INFO("Applying records %v-%v from changelog %v",
            startRecordId,
            startRecordId + recordsRead - 1,
            changelogId);

        for (const auto& data : recordsData)  {
            DecoratedAutomaton_->ApplyMutationDuringRecovery(data);
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

TLeaderRecovery::TLeaderRecovery(
    TDistributedHydraManagerConfigPtr config,
    TCellManagerPtr cellManager,
    TDecoratedAutomatonPtr decoratedAutomaton,
    IChangelogStorePtr changelogStore,
    ISnapshotStorePtr snapshotStore,
    TEpochContextPtr epochContext)
    : TRecovery(
        config,
        cellManager,
        decoratedAutomaton,
        changelogStore,
        snapshotStore,
        epochContext)
{ }

TAsyncError TLeaderRecovery::Run(TVersion targetVersion)
{
    VERIFY_THREAD_AFFINITY_ANY();
    
    SyncVersion_ = targetVersion;
    return BIND(&TLeaderRecovery::DoRun, MakeStrong(this))
        .Guarded()
        .AsyncVia(EpochContext_->EpochSystemAutomatonInvoker)
        .Run(targetVersion);
}

void TLeaderRecovery::DoRun(TVersion targetVersion)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    RecoverToVersion(targetVersion);
}

bool TLeaderRecovery::IsLeader() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return true;
}

////////////////////////////////////////////////////////////////////////////////

TFollowerRecovery::TFollowerRecovery(
    TDistributedHydraManagerConfigPtr config,
    TCellManagerPtr cellManager,
    TDecoratedAutomatonPtr decoratedAutomaton,
    IChangelogStorePtr changelogStore,
    ISnapshotStorePtr snapshotStore,
    TEpochContextPtr epochContext,
    TVersion syncVersion)
    : TRecovery(
        config,
        cellManager,
        decoratedAutomaton,
        changelogStore,
        snapshotStore,
        epochContext)
{
    SyncVersion_ = PostponedVersion_ = syncVersion;
}

TAsyncError TFollowerRecovery::Run()
{
    VERIFY_THREAD_AFFINITY_ANY();

    return BIND(&TFollowerRecovery::DoRun, MakeStrong(this))
        .Guarded()
        .AsyncVia(EpochContext_->EpochSystemAutomatonInvoker)
        .Run();
}

void TFollowerRecovery::DoRun()
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    RecoverToVersion(SyncVersion_);

    LOG_INFO("Checkpoint reached");

    while (true) {
        TPostponedMutations mutations;
        {
            TGuard<TSpinLock> guard(SpinLock_);
            if (PostponedMutations_.empty()) {
                break;
            }
            mutations.swap(PostponedMutations_);
        }

        LOG_INFO("Logging %v postponed mutations",
            mutations.size());

        for (const auto& mutation : mutations) {
            switch (mutation.Type) {
                case TPostponedMutation::EType::Mutation:
                    DecoratedAutomaton_->LogFollowerMutation(mutation.RecordData, nullptr);
                    break;

                case TPostponedMutation::EType::ChangelogRotation: {
                    auto result = WaitFor(DecoratedAutomaton_->RotateChangelog(EpochContext_));
                    THROW_ERROR_EXCEPTION_IF_FAILED(result);
                    break;
                }

                default:
                    YUNREACHABLE();
            }
        }
    }

    LOG_INFO("Finished logging postponed mutations");
}

TError TFollowerRecovery::PostponeChangelogRotation(
    TVersion version)
{
    VERIFY_THREAD_AFFINITY_ANY();

    TGuard<TSpinLock> guard(SpinLock_);

    if (PostponedVersion_ > version) {
        LOG_DEBUG("Late changelog rotation received during recovery, ignored: expected %v, received %v",
            PostponedVersion_,
            version);
        return TError();
    }

    if (PostponedVersion_ < version) {
        return TError("Out-of-order changelog rotation received during recovery: expected %v, received %v",
            PostponedVersion_,
            version);
    }

    PostponedMutations_.push_back(TPostponedMutation::CreateChangelogRotation());

    LOG_DEBUG("Postponing changelog rotation at version %v",
        PostponedVersion_);

    PostponedVersion_.Rotate();

    return TError();
}

TError TFollowerRecovery::PostponeMutations(
    TVersion version,
    const std::vector<TSharedRef>& recordsData)
{
    VERIFY_THREAD_AFFINITY_ANY();

    TGuard<TSpinLock> guard(SpinLock_);

    if (PostponedVersion_ > version) {
        LOG_WARNING("Late mutations received during recovery, ignored: expected %v, received %v",
            PostponedVersion_,
            version);
        return TError();
    }

    if (PostponedVersion_ != version) {
        return TError("Out-of-order mutations received during recovery: expected %v, received %v",
            PostponedVersion_,
            version);
    }

    LOG_DEBUG("Postponing %v mutations at version %v",
        recordsData.size(),
        PostponedVersion_);

    for (const auto& data : recordsData) {
        PostponedMutations_.push_back(TPostponedMutation::CreateMutation(data));
    }

    PostponedVersion_.Advance(recordsData.size());

    return TError();
}

bool TFollowerRecovery::IsLeader() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return false;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NHydra
} // namespace NYT
