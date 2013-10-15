#include "stdafx.h"
#include "changelog_download.h"
#include "config.h"
#include "changelog.h"
#include "changelog_discovery.h"
#include "private.h"

#include <core/concurrency/fiber.h>

#include <core/misc/serialize.h>

#include <core/logging/tagged_logger.h>

#include <ytlib/election/cell_manager.h>

#include <ytlib/hydra/hydra_service_proxy.h>

namespace NYT {
namespace NHydra {

using namespace NElection;
using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

class TChangelogDownloader
    : public TRefCounted
{
public:
    TChangelogDownloader(
        TDistributedHydraManagerConfigPtr config,
        TCellManagerPtr cellManager,
        IChangelogStorePtr changelogStore)
        : Config(config)
        , CellManager(cellManager)
        , ChangelogStore(changelogStore)
        , Logger(HydraLogger)
    {
        YCHECK(Config);
        YCHECK(CellManager);
        YCHECK(ChangelogStore);

        Logger.AddTag(Sprintf("CellGuid: %s",
            ~ToString(CellManager->GetCellGuid())));
    }

    TAsyncError Run(int changelogId, int recordCount)
    {
        return BIND(&TChangelogDownloader::DoRun, MakeStrong(this))
            .AsyncVia(HydraIOQueue->GetInvoker())
            .Run(changelogId, recordCount);
    }

private:
    TDistributedHydraManagerConfigPtr Config;
    TCellManagerPtr CellManager;
    IChangelogStorePtr ChangelogStore;

    NLog::TTaggedLogger Logger;


    TError DoRun(int changelogId, int recordCount)
    {
        try {
            LOG_INFO("Requested %d records in changelog %d",
                recordCount,
                changelogId);

            auto changelog = ChangelogStore->OpenChangelogOrThrow(changelogId);
            if (changelog->GetRecordCount() >= recordCount) {
                LOG_INFO("Local changelog already contains %d records, no download needed",
                    changelog->GetRecordCount());
                return TError();
            }

            auto asyncChangelogInfo = DiscoverChangelog(Config, CellManager, changelogId, recordCount);
            auto changelogInfo = WaitFor(asyncChangelogInfo);
            if (changelogInfo.ChangelogId == NonexistingSegmentId) {
                THROW_ERROR_EXCEPTION("Unable to find a download source for changelog %d with %d records",
                    changelogId,
                    recordCount);
            }

            int downloadedRecordCount = changelog->GetRecordCount();
            
            LOG_INFO("Downloading records %d-%d from peer %d",
                changelog->GetRecordCount(),
                recordCount,
                changelogInfo.PeerId);
            
            THydraServiceProxy proxy(CellManager->GetPeerChannel(changelogInfo.PeerId));
            proxy.SetDefaultTimeout(Config->ChangelogDownloader->RpcTimeout);
            
            while (downloadedRecordCount < recordCount) {
                int desiredChunkSize = std::min(
                    Config->ChangelogDownloader->RecordsPerRequest,
                    recordCount - downloadedRecordCount);
            
                LOG_DEBUG("Requesting records %d-%d",
                    downloadedRecordCount,
                    downloadedRecordCount + desiredChunkSize - 1);
            
                auto req = proxy.ReadChangeLog();
                req->set_changelog_id(changelogId);
                req->set_start_record_id(downloadedRecordCount);
                req->set_record_count(desiredChunkSize);
            
                auto rsp = WaitFor(req->Invoke());
                THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error downloading changelog");
            
                const auto& attachments = rsp->Attachments();
                YCHECK(attachments.size() == 1);
            
                std::vector<TSharedRef> recordsData;
                UnpackRefs(rsp->Attachments()[0], &recordsData);
            
                if (recordsData.empty()) {
                    THROW_ERROR_EXCEPTION("Peer %d does not have %d records of changelog %d anymore",
                        changelogInfo.PeerId,
                        recordCount,
                        changelogId);
                }
            
                int actualChunkSize = static_cast<int>(recordsData.size());
                if (actualChunkSize != desiredChunkSize) {
                    LOG_DEBUG("Received records %d-%d while %d records were requested",
                        downloadedRecordCount,
                        downloadedRecordCount + actualChunkSize - 1,
                        desiredChunkSize);
                    // Continue anyway.
                } else {
                    LOG_DEBUG("Received records %d-%d",
                        downloadedRecordCount,
                        downloadedRecordCount + actualChunkSize - 1);
                }
            
                FOREACH (const auto& data, recordsData) {
                    changelog->Append(data);
                    ++downloadedRecordCount;
                }
            }
            
            LOG_INFO("Changelog downloaded successfully");           

            return TError();
        } catch (const std::exception& ex) {
            return ex;
        }
    }

};

TAsyncError DownloadChangelog(
    TDistributedHydraManagerConfigPtr config,
    NElection::TCellManagerPtr cellManager,
    IChangelogStorePtr changelogStore,
    int changelogId,
    int recordCount)
{
    auto downloader = New<TChangelogDownloader>(config, cellManager, changelogStore);
    return downloader->Run(changelogId, recordCount);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NHydra
} // namespace NYT
