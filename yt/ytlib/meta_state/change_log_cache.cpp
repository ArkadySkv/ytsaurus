#include "stdafx.h"
#include "change_log_cache.h"
#include "private.h"
#include "meta_state_manager.h"
#include "change_log.h"
#include "config.h"

#include <ytlib/misc/fs.h>

#include <util/folder/dirut.h>

namespace NYT {
namespace NMetaState {

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = MetaStateLogger;

static const char* LogExtension = "log";

////////////////////////////////////////////////////////////////////////////////

TCachedAsyncChangeLog::TCachedAsyncChangeLog(TChangeLogPtr changeLog)
    : TCacheValueBase<i32, TCachedAsyncChangeLog>(changeLog->GetId())
    , TAsyncChangeLog(changeLog)
{ }

////////////////////////////////////////////////////////////////////////////////

TChangeLogCache::TChangeLogCache(TChangeLogCacheConfigPtr config)
    : TSizeLimitedCache<i32, TCachedAsyncChangeLog>(config->MaxSize)
    , Config(config)
{ }

void TChangeLogCache::Start()
{
    auto path = Config->Path;

    LOG_DEBUG("Preparing changelog directory %s", ~path.Quote());

    NFS::ForcePath(path);
    NFS::CleanTempFiles(path);
}

Stroka TChangeLogCache::GetChangeLogFileName(i32 id)
{
    return NFS::CombinePaths(Config->Path, Sprintf("%09d.%s", id, LogExtension));
}

TChangeLogPtr TChangeLogCache::CreateChangeLog(i32 id)
{
    return New<TChangeLog>(GetChangeLogFileName(id), id, Config->IndexBlockSize);
}

TChangeLogCache::TGetResult TChangeLogCache::Get(i32 id)
{
    TInsertCookie cookie(id);
    if (BeginInsert(&cookie)) {
        auto fileName = GetChangeLogFileName(id);
        if (!isexist(~fileName)) {
            cookie.Cancel(TError(
                EErrorCode::NoSuchChangeLog,
                Sprintf("No such changelog (ChangeLogId: %d)", id)));
        } else {
            try {
                auto changeLog = CreateChangeLog(id);
                changeLog->Open();
                cookie.EndInsert(New<TCachedAsyncChangeLog>(~changeLog));
            } catch (const std::exception& ex) {
                LOG_FATAL(ex, "Error opening changelog (ChangeLogId: %d)", id);
            }
        }
    }
    return cookie.GetValue().Get();
}

TCachedAsyncChangeLogPtr TChangeLogCache::Create(
    i32 id,
    i32 prevRecordCount,
    const TEpochId& epoch)
{
    TInsertCookie cookie(id);
    if (!BeginInsert(&cookie)) {
        LOG_FATAL("Trying to create an already existing changelog (ChangeLogId: %d)",
            id);
    }

    auto fileName = GetChangeLogFileName(id);

    try {
        auto changeLog = New<TChangeLog>(fileName, id, Config->IndexBlockSize);
        changeLog->Create(prevRecordCount, epoch);
        cookie.EndInsert(New<TCachedAsyncChangeLog>(~changeLog));
    } catch (const std::exception& ex) {
        LOG_FATAL(ex, "Error creating changelog (ChangeLogId: %d)", id);
    }

    return cookie.GetValue().Get().Value();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NMetaState
} // namespace NYT
