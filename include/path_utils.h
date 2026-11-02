#ifndef TINTA_PATH_UTILS_H
#define TINTA_PATH_UTILS_H

// Windows 7-compatible path and file helpers.
//
// Tinta's portable exe must start on Windows 7 SP1. Since the MSVC STL's
// "Win8 baseline" (VS 2022 17.14, April 2025), the <filesystem>
// implementation statically imports CreateFile2 and CopyFile2 from
// kernel32.dll, so any std::filesystem call in the exe makes the loader
// refuse to start on Windows 7 ("procedure entry point CreateFile2 not
// located in KERNEL32.dll"). These helpers cover the handful of
// std::filesystem calls the exe used to make, using only Windows 7 APIs
// (GetFileAttributesW, FindFirstFileW, CreateDirectoryW, DeleteFileW,
// RemoveDirectoryW).
//
// tests/win7_compat_tests.cpp forbids <filesystem> / std::filesystem in
// src/ and include/, so new code must use these helpers instead. The
// module is deliberately app-free (no app.h), so the headless PlantUML
// bridge can use it too.

#include <cstdint>
#include <string>
#include <vector>

// The final component of a path: "dir\\file.md" -> "file.md", "C:\\" -> "".
std::wstring pathFileName(const std::wstring& path);
// The file name without its extension: "dir\\file.md" -> "file"; a name
// whose only dot is its first character has no extension to strip
// (".gitignore" -> ".gitignore").
std::wstring pathStem(const std::wstring& path);
// The extension including the dot, empty when there is none:
// "file.md" -> ".md", "dir\\a.tar.gz" -> ".gz", ".gitignore" -> "".
std::wstring pathExtension(const std::wstring& path);
// The directory holding the path, matching std::filesystem::parent_path():
// "C:\\dir\\file.md" -> "C:\\dir", "C:\\file.md" -> "C:\\",
// "dir\\file.md" -> "dir", "file.md" -> "".
std::wstring pathParentDir(const std::wstring& path);
// dir + "\\" + name, with no double separator when dir already ends in one.
// An absolute name (drive, UNC, or root-relative) replaces the directory,
// like the standard library's operator/.
std::wstring pathJoin(const std::wstring& dir, const std::wstring& name);
// True when the path has no root: relative paths like "images\\a.png" and
// "..\\a.png", drive-relative "C:a.png"; false for "C:\\...", "\\...",
// "\\\\server\\share\\...".
bool pathIsRelative(const std::wstring& path);
// Matches std::filesystem::path::lexically_normal(): collapses "." and ".."
// textually, keeps a root prefix (".." above a root is kept, as in
// lexically_normal) and returns "." for an empty result.
std::wstring pathNormalize(const std::wstring& path);

// GetFileAttributesW-based tests (Windows 7 APIs).
bool pathExists(const std::wstring& path);
bool pathIsDirectory(const std::wstring& path);
bool pathIsRegularFile(const std::wstring& path);
// The size in bytes of a regular file; 0 when it cannot be queried.
uint64_t pathFileSize(const std::wstring& path);

// Recursively creates the directory tree (like create_directories); true
// when it exists afterwards. The empty path is a no-op.
bool createDirectories(const std::wstring& path);
// Recursively deletes a file or directory tree (like remove_all).
bool removeAll(const std::wstring& path);
// Every entry in dir as a full path ("." and ".." excluded). Returns true
// when the directory could be opened (even if it had no matching entries).
bool listDirectory(const std::wstring& dir, std::vector<std::wstring>& paths);

#endif  // TINTA_PATH_UTILS_H
