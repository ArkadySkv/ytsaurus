#include "stdafx.h"
#include "fs.h"

#include <core/misc/error.h>

#include <core/logging/log.h>

#include <util/folder/dirut.h>
#include <util/folder/filelist.h>

// For GetAvaibaleSpace().
#if defined(_linux_)
    #include <sys/vfs.h>
    #include <sys/stat.h>
#elif defined(_freebsd_) || defined(_darwin_)
    #include <sys/param.h>
    #include <sys/mount.h>
#elif defined (_win_)
    #include <comutil.h>
    #include <shlobj.h>
#endif

// For JoinPaths
#ifdef _win_
    static const char PATH_DELIM = '\\';
    static const char PATH_DELIM2 = '/';
#else
    static const char PATH_DELIM = '/';
    static const char PATH_DELIM2 = 0;
#endif

namespace NYT {
namespace NFS {

//////////////////////////////////////////////////////////////////////////////

static NLog::TLogger Logger("FS");

//////////////////////////////////////////////////////////////////////////////

void Remove(const Stroka& path)
{
    bool ok;
#ifdef _win_
    ok = DeleteFileA(~path);
#else
    struct stat sb;
    ok = lstat(~path, &sb) == 0;
    if (ok) {
        if (S_ISDIR(sb.st_mode)) {
            ok = ::rmdir(~path) == 0;
        } else {
            ok = ::remove(~path) == 0;
        }
    }
#endif
    if (!ok) {
        THROW_ERROR_EXCEPTION("Cannot remove %s",
            ~path.Quote())
            << TError::FromSystem();
    }   
}

void Rename(const Stroka& oldPath, const Stroka& newPath)
{
    bool ok;
#if defined(_win_)
    ok = MoveFileEx(~oldPath, ~newPath, MOVEFILE_REPLACE_EXISTING) != 0;
#else
    ok = ::rename(~oldPath, ~newPath) == 0;
#endif
    if (!ok) {
        THROW_ERROR_EXCEPTION("Cannot rename %s into %s",
            ~oldPath.Quote(),
            ~newPath.Quote())
            << TError::FromSystem();
    }
}

Stroka GetFileName(const Stroka& path)
{
    size_t delimPos = path.rfind('/');
#ifdef _win32_
    if (delimPos == Stroka::npos) {
        // There's a possibility of Windows-style path
        delimPos = path.rfind('\\');
    }
#endif
    return (delimPos == Stroka::npos) ? path : path.substr(delimPos+1);
}

Stroka GetDirectoryName(const Stroka& path)
{
    auto absPath = CombinePaths(GetCwd(), path);
#ifdef _win_
    // May be mixed style of filename ('/' and '\')
    correctpath(absPath);
#endif
    return absPath.substr(0, absPath.find_last_of(LOCSLASH_C));
}

Stroka GetFileExtension(const Stroka& path)
{
    i32 dotPosition = path.find_last_of('.');
    if (dotPosition < 0) {
        return "";
    }
    return path.substr(dotPosition + 1, path.size() - dotPosition - 1);
}

Stroka GetFileNameWithoutExtension(const Stroka& path)
{
    Stroka fileName = GetFileName(path);
    i32 dotPosition = fileName.find_last_of('.');
    if (dotPosition < 0) {
        return fileName;
    }
    return fileName.substr(0, dotPosition);
}

void CleanTempFiles(const Stroka& path)
{
    LOG_INFO("Cleaning temp files in %s", ~path.Quote());

    auto entries = EnumerateFiles(path, std::numeric_limits<int>::max());
    for (const auto& entry : entries) {
        if (entry.has_suffix(TempFileSuffix)) {
            auto fileName = NFS::CombinePaths(path, entry);
            LOG_INFO("Removing file %s", ~fileName.Quote());
            NFS::Remove(fileName);
        }
    }
}

std::vector<Stroka> EnumerateFiles(const Stroka& path, int depth)
{
    std::vector<Stroka> result;
    if (isexist(~path)) {
        TFileList list;
        list.Fill(path, TStringBuf(), TStringBuf(), depth);
        int size = list.Size();
        for (int i = 0; i < size; ++i) {
            result.push_back(list.Next());
        }
    }
    return result;
}

std::vector<Stroka> EnumerateDirectories(const Stroka& path, int depth)
{
    std::vector<Stroka> result;
    if (isexist(~path)) {
        TDirsList list;
        list.Fill(path, TStringBuf(), TStringBuf(), depth);
        int size = list.Size();
        for (int i = 0; i < size; ++i) {
            result.push_back(list.Next());
        }
    }
    return result;
}

TDiskSpaceStatistics GetDiskSpaceStatistics(const Stroka& path)
{
    TDiskSpaceStatistics result;

#ifdef _win_
    bool ok = GetDiskFreeSpaceEx(
        ~path,
        (PULARGE_INTEGER) &result.AvailableSpace,
        (PULARGE_INTEGER) &result.TotalSpace,
        (PULARGE_INTEGER) NULL) != 0;
#else
    struct statfs fsData;
    bool ok = statfs(~path, &fsData) == 0;
    result.TotalSpace = (i64) fsData.f_blocks * fsData.f_bsize;
    result.AvailableSpace = (i64) fsData.f_bavail * fsData.f_bsize;
#endif

    if (!ok) {
        THROW_ERROR_EXCEPTION("Failed to get disk space statistics for %s",
            ~path.Quote())
            << TError::FromSystem();
    }

    return result;
}

void ForcePath(const Stroka& path, int mode)
{
    MakePathIfNotExist(~path, mode);
}

i64 GetFileSize(const Stroka& path)
{
#if !defined(_win_)
    struct stat fileStat;
    int result = stat(~path, &fileStat);
#else
    WIN32_FIND_DATA findData;
    HANDLE handle = FindFirstFileA(~path, &findData);
#endif

#if !defined(_win_)
    if (result == -1) {
#else
    if (handle == INVALID_HANDLE_VALUE) {
#endif
        THROW_ERROR_EXCEPTION("Failed to get the size of %s",
            ~path.Quote())
            << TError::FromSystem();
    }

#if !defined(_win_)
    i64 fileSize = static_cast<i64>(fileStat.st_size);
#else
    FindClose(handle);
    i64 fileSize =
        (static_cast<i64>(findData.nFileSizeHigh) << 32) +
        static_cast<i64>(findData.nFileSizeLow);
#endif

    return fileSize;
}

static bool IsAbsolutePath(const Stroka& path)
{
    if (path.empty())
        return false;
    if (path[0] == PATH_DELIM)
        return true;
#ifdef _win_
    if (path[0] == PATH_DELIM2)
        return true;
    if (path[0] > 0 && isalpha(path[0]) && path[1] == ':')
        return true;
#endif // _win_
    return false;
}

static Stroka JoinPaths(const Stroka& path1, const Stroka& path2)
{
    if (path1.empty())
        return path2;
    if (path2.empty())
        return path1;

    Stroka path = path1;
    int delim = 0;
    if (path1.back() == PATH_DELIM || path1.back() == PATH_DELIM2)
        ++delim;
    if (path2[0] == PATH_DELIM || path2[0] == PATH_DELIM2)
        ++delim;
    if (delim == 0)
        path.append(1, PATH_DELIM);
    path.append(path2, delim == 2 ? 1 : 0, Stroka::npos);
    return path;
}

Stroka CombinePaths(const Stroka& path1, const Stroka& path2)
{
    if (IsAbsolutePath(path2))
        return path2;
    return JoinPaths(path1, path2);
}

Stroka NormalizePathSeparators(const Stroka& path)
{
    Stroka result;
    result.reserve(path.length());
    for (int i = 0; i < path.length(); ++i) {
        if (path[i] == '\\') {
            result.append('/');
        } else {
            result.append(path[i]);
        }
    }
    return result;
}

void SetExecutableMode(const Stroka& path, bool executable)
{
#ifdef _win_
    UNUSED(path);
    UNUSED(executable);
#else
    int mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
    if (executable) {
        mode |= S_IXOTH;
        mode |= S_IXGRP;
        mode |= S_IXUSR;
    }
    bool ok = chmod(~path, mode) == 0;
    if (!ok) {
        THROW_ERROR_EXCEPTION(
            "Failed to set mode %d for %s",
            mode,
            ~path.Quote())
            << TError::FromSystem();
    }
#endif
}

void MakeSymbolicLink(const Stroka& filePath, const Stroka& linkPath)
{
#ifdef _win_
    // From MSDN: If the function succeeds, the return value is nonzero.
    // If the function fails, the return value is zero. To get extended error information, call GetLastError.
    bool ok = CreateSymbolicLink(~linkPath, ~filePath, 0) != 0;
#else
    bool ok = symlink(~filePath, ~linkPath) == 0;
#endif

    if (!ok) {
        THROW_ERROR_EXCEPTION(
            "Failed to link %s to %s",
            ~filePath.Quote(),
            ~linkPath.Quote())
            << TError::FromSystem();
    }
}

bool AreInodesIdentical(const Stroka& lhsPath, const Stroka& rhsPath)
{
#ifdef _linux_
    auto wrappedStat = [] (const Stroka& path, struct stat* buffer) {
        auto result = stat(~path, buffer);
        if (result) {
            THROW_ERROR_EXCEPTION(
                "Failed to check for identical inodes: stat failed for %s",
                ~path.Quote())
                << TError::FromSystem();
        }
    };

    struct stat lhsBuffer, rhsBuffer;
    wrappedStat(lhsPath, &lhsBuffer);
    wrappedStat(rhsPath, &rhsBuffer);

    return (lhsBuffer.st_dev == rhsBuffer.st_dev) && (lhsBuffer.st_ino == rhsBuffer.st_ino);
#else
    return false;
#endif
}

Stroka GetHomePath()
{
#ifdef _win_
    std::array<char, 1024> buffer;
    SHGetSpecialFolderPath(0, buffer, CSIDL_PROFILE, 0);
    return Stroka(buffer);
#else
    return std::getenv("HOME");
#endif
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NFS
} // namespace NYT
