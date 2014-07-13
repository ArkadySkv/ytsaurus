#include "stdafx.h"
#include "local_changelog_store.h"
#include "changelog.h"
#include "file_changelog_dispatcher.h"
#include "config.h"
#include "private.h"

#include <core/misc/fs.h>
#include <core/misc/cache.h>

#include <core/logging/tagged_logger.h>

namespace NYT {
namespace NHydra {

using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

class TCachedLocalChangelog
    : public TCacheValueBase<int, TCachedLocalChangelog>
    , public IChangelog
{
public:
    explicit TCachedLocalChangelog(
        int id,
        IChangelogPtr underlyingChangelog)
        : TCacheValueBase(id)
        , UnderlyingChangelog_(underlyingChangelog)
    { }

    virtual TSharedRef GetMeta() const override
    {
        return UnderlyingChangelog_->GetMeta();
    }

    virtual int GetRecordCount() const override
    {
        return UnderlyingChangelog_->GetRecordCount();
    }

    virtual i64 GetDataSize() const override
    {
        return UnderlyingChangelog_->GetDataSize();
    }

    virtual bool IsSealed() const override
    {
        return UnderlyingChangelog_->IsSealed();
    }

    virtual TAsyncError Append(const TSharedRef& data) override
    {
        return UnderlyingChangelog_->Append(data);
    }

    virtual TAsyncError Flush() override
    {
        return UnderlyingChangelog_->Flush();
    }

    virtual std::vector<TSharedRef> Read(
        int firstRecordId,
        int maxRecords,
        i64 maxBytes) const override
    {
        return UnderlyingChangelog_->Read(firstRecordId, maxRecords, maxBytes);
    }

    virtual TAsyncError Seal(int recordCount) override
    {
        return UnderlyingChangelog_->Seal(recordCount);
    }

    virtual TAsyncError Unseal() override
    {
        return UnderlyingChangelog_->Unseal();
    }

private:
    IChangelogPtr UnderlyingChangelog_;

};

class TFileChangelogStore
    : public TSizeLimitedCache<int, TCachedLocalChangelog>
    , public IChangelogStore
{
public:
    TFileChangelogStore(
        const Stroka& threadName,
        TFileChangelogStoreConfigPtr config)
        : TSizeLimitedCache(config->MaxCachedChangelogs)
        , Dispatcher_(New<TFileChangelogDispatcher>(threadName))
        , Config_(config)
        , Logger(HydraLogger)
    {
        Logger.AddTag("Path: %v", Config_->Path);
    }

    void Start()
    {
        LOG_DEBUG("Preparing changelog store");

        NFS::ForcePath(Config_->Path);
        NFS::CleanTempFiles(Config_->Path);
    }

    virtual TFuture<TErrorOr<IChangelogPtr>> CreateChangelog(int id, const TSharedRef& meta) override
    {
        return BIND(&TFileChangelogStore::DoCreateChangelog, MakeStrong(this))
            .Guarded()
            .AsyncVia(GetHydraIOInvoker())
            .Run(id, meta);
    }

    virtual TFuture<TErrorOr<IChangelogPtr>> OpenChangelog(int id) override
    {
        return BIND(&TFileChangelogStore::DoOpenChangelog, MakeStrong(this))
            .Guarded()
            .AsyncVia(GetHydraIOInvoker())
            .Run(id);
    }

    virtual TFuture<TErrorOr<int>> GetLatestChangelogId(int initialId) override
    {
        return BIND(&TFileChangelogStore::DoGetLatestChangelogId, MakeStrong(this))
            .Guarded()
            .AsyncVia(GetHydraIOInvoker())
            .Run(initialId);
    }

private:
    TFileChangelogDispatcherPtr Dispatcher_;
    TFileChangelogStoreConfigPtr Config_;

    NLog::TTaggedLogger Logger;


    Stroka GetChangelogPath(int id)
    {
        return NFS::CombinePaths(
            Config_->Path,
            Format("%09d.%v", id, ChangelogExtension));
    }


    IChangelogPtr DoCreateChangelog(int id, const TSharedRef& meta)
    {
        TInsertCookie cookie(id);
        if (!BeginInsert(&cookie)) {
            THROW_ERROR_EXCEPTION("Trying to create an already existing changelog %d",
                id);
        }

        auto path = GetChangelogPath(id);

        try {
            auto underlyingChangelog = Dispatcher_->CreateChangelog(path, meta, Config_);
            auto cachedChangelog = New<TCachedLocalChangelog>(id, underlyingChangelog);
            cookie.EndInsert(cachedChangelog);
        } catch (const std::exception& ex) {
            LOG_FATAL(ex, "Error creating changelog %d",
                id);
        }

        auto result = WaitFor(cookie.GetValue());
        THROW_ERROR_EXCEPTION_IF_FAILED(result);
        return result.Value();
    }

    IChangelogPtr DoOpenChangelog(int id)
    {
        TInsertCookie cookie(id);
        if (BeginInsert(&cookie)) {
            auto path = GetChangelogPath(id);
            if (!NFS::Exists(path)) {
                cookie.Cancel(TError(
                    NHydra::EErrorCode::NoSuchChangelog,
                    "No such changelog %d",
                    id));
            } else {
                try {
                    auto underlyingChangelog = Dispatcher_->OpenChangelog(path, Config_);
                    auto cachedChangelog = New<TCachedLocalChangelog>(id, underlyingChangelog);
                    cookie.EndInsert(cachedChangelog);
                } catch (const std::exception& ex) {
                    LOG_FATAL(ex, "Error opening changelog %d",
                        id);
                }
            }
        }

        auto changelogOrError = WaitFor(cookie.GetValue());
        THROW_ERROR_EXCEPTION_IF_FAILED(changelogOrError);
        return changelogOrError.Value();
    }

    int DoGetLatestChangelogId(int initialId)
    {
        int latestId = NonexistingSegmentId;
        yhash_set<int> ids;

        auto fileNames = NFS::EnumerateFiles(Config_->Path);
        for (const auto& fileName : fileNames) {
            auto extension = NFS::GetFileExtension(fileName);
            if (extension != ChangelogExtension)
                continue;
            auto name = NFS::GetFileNameWithoutExtension(fileName);
            try {
                int id = FromString<int>(name);
                YCHECK(ids.insert(id).second);
                if (id >= initialId && (id > latestId || latestId == NonexistingSegmentId)) {
                    latestId = id;
                }
            } catch (const std::exception&) {
                LOG_WARNING("Found unrecognized file %s", ~fileName.Quote());
            }
        }

        if (latestId != NonexistingSegmentId) {
            for (int id = initialId; id <= latestId; ++id) {
                if (ids.find(id) == ids.end()) {
                    LOG_FATAL("Interim changelog %d is missing",
                        id);                    
                }
            }
        }

        return latestId;
    }

};

IChangelogStorePtr CreateLocalChangelogStore(
    const Stroka& threadName,
    TFileChangelogStoreConfigPtr config)
{
    auto store = New<TFileChangelogStore>(
        threadName,
        config);
    store->Start();
    return store;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NHydra
} // namespace NYT

