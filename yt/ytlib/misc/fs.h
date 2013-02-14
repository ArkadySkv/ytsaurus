#pragma once

/*!
 * \file fs.h
 * \brief File system functions
 */

#include "common.h"

namespace NYT {
namespace NFS {

////////////////////////////////////////////////////////////////////////////////

//! File suffix for temporary files.
const char* const TempFileSuffix = "~";

//! Removes file.
/*!
 * \param name File name
 * \return True when succeeds
 */
bool Remove(const Stroka& name);

//! Renames file.
/*!
 * \param oldName Old name
 * \param newName New name
 * \return True when succeeds
 */
bool Rename(const Stroka& oldName, const Stroka& newName);

//! Returns name of file.
Stroka GetFileName(const Stroka& path);

//! Returns extension of file.
Stroka GetFileExtension(const Stroka& path);

//! Returns name of file without extension.
Stroka GetFileNameWithoutExtension(const Stroka& path);

//! Returns path of directory containing the file.
Stroka GetDirectoryName(const Stroka& path);

//! Combines two strings into a path.
Stroka CombinePaths(const Stroka& path1, const Stroka& path2);

//! Deletes all files with extension #TempFileSuffix in a given directory.
void CleanTempFiles(const Stroka& path);

//! Describes total, free, and available space on a disk drive.
struct TDiskSpaceStatistics
{
    i64 TotalSpace;
    i64 AvailableSpace;
};

//! Computes the space statistics for disk drive containing #path.
//! Throws an exception if something went wrong.
TDiskSpaceStatistics GetDiskSpaceStatistics(const Stroka& path);

//! Creates the #path and parent directories if they don't exists.
//! Throws an exception if something went wrong.
/*!
 *  Calls the same named function from util/folder/dirut.
 */
void ForcePath(const Stroka& path, int mode = 0777);

//! Returns size of a file.
//! Throws an exception if something went wrong.
i64 GetFileSize(const Stroka& path);

//! Converts all back slashes to forward slashes.
Stroka NormalizePathSeparators(const Stroka& path);

//! Sets 'executable' mode.
void SetExecutableMode(const Stroka& path, bool executable);

//! Makes a symbolic link on file #fileName with #linkName.
void MakeSymbolicLink(const Stroka& filePath, const Stroka& linkPath);

////////////////////////////////////////////////////////////////////////////////

} // namespace NFS
} // namespace NYT
