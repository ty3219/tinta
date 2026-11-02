#include "path_utils.h"

#include <windows.h>
#include <cwchar>

std::wstring pathFileName(const std::wstring& path) {
    const size_t sep = path.find_last_of(L"\\/");
    return sep == std::wstring::npos ? path : path.substr(sep + 1);
}

std::wstring pathStem(const std::wstring& path) {
    const std::wstring name = pathFileName(path);
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0) return name;
    return name.substr(0, dot);
}

std::wstring pathExtension(const std::wstring& path) {
    const std::wstring name = pathFileName(path);
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0) return L"";
    return name.substr(dot);
}

// True when the prefix is a root: "\", "C:\", or a UNC share "\\server\share\".
static bool isPathRoot(const std::wstring& p) {
    if (p.size() == 1 && (p[0] == L'\\' || p[0] == L'/')) return true;
    if (p.size() == 3 && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/')) return true;
    if (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\') {
        // UNC: everything up to and including the separator that closes the
        // share name ("\\server\share\") is a root.
        const size_t sep1 = p.find_first_of(L"\\/", 2);
        if (sep1 == std::wstring::npos) return true;
        const size_t sep2 = p.find_first_of(L"\\/", sep1 + 1);
        if (sep2 == std::wstring::npos) return true;
        return sep2 + 1 == p.size();
    }
    return false;
}

std::wstring pathParentDir(const std::wstring& path) {
    if (path.empty()) return path;
    std::wstring p = path;
    // Trailing separators do not change the parent, but never strip a bare
    // root such as "C:\" down to "C:".
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    const size_t sep = p.find_last_of(L"\\/");
    if (sep == std::wstring::npos) return L"";
    if (isPathRoot(p.substr(0, sep + 1))) return p.substr(0, sep + 1);
    return p.substr(0, sep);
}

bool pathIsRelative(const std::wstring& path) {
    if (path.size() >= 3 && path[1] == L':' &&
        (path[2] == L'\\' || path[2] == L'/')) {
        return false;  // drive root: "C:\..."
    }
    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') {
        return false;  // UNC: "\\server\share\..."
    }
    if (!path.empty() && (path[0] == L'\\' || path[0] == L'/')) {
        return false;  // root-relative: "\..."
    }
    return true;
}

std::wstring pathJoin(const std::wstring& dir, const std::wstring& name) {
    // An absolute name replaces the directory, exactly as the standard
    // library's operator/ does: a document's folder must never be glued in
    // front of a UNC share path or an absolute rendered-artifact path.
    if (name.empty()) return dir;
    if (!pathIsRelative(name)) return name;
    if (dir.empty()) return name;
    if (dir.back() == L'\\' || dir.back() == L'/') return dir + name;
    return dir + L"\\" + name;
}

std::wstring pathNormalize(const std::wstring& path) {
    const auto isSep = [](wchar_t c) { return c == L'\\' || c == L'/'; };
    // Split off the root prefix, which ".." can never climb above.
    std::wstring root;
    std::wstring rest = path;
    if (rest.size() >= 2 && rest[1] == L':' &&
        (rest.size() == 2 || isSep(rest[2]))) {
        root = rest.substr(0, 2);  // drive name "C:"
        rest = rest.substr(2);
        if (!rest.empty() && isSep(rest[0])) {
            root += L"\\";
            rest = rest.substr(1);
        }
    } else if (rest.size() >= 2 && isSep(rest[0]) && isSep(rest[1])) {
        // UNC in either separator style: "\\server\share\..." or
        // "//server/share/...". The host must survive, or pathHost()
        // (network_path.h) no longer sees the server the path reaches.
        const size_t sep1 = rest.find_first_of(L"\\/", 2);
        if (sep1 != std::wstring::npos) {
            const size_t sep2 = rest.find_first_of(L"\\/", sep1 + 1);
            if (sep2 != std::wstring::npos) {
                root = rest.substr(0, sep2 + 1);  // "\\server\share\" (any slashes)
                rest = rest.substr(sep2 + 1);
            } else {
                root = rest;  // "\\server\share" with no trailing separator
                rest.clear();
            }
        } else {
            root = rest;  // "\\server"
            rest.clear();
        }
    } else if (!rest.empty() && isSep(rest[0])) {
        root = L"\\";
        rest = rest.substr(1);
    }

    // Collapse the remaining elements like lexically_normal(): "." drops,
    // ".." pops the previous element, and ".." at the start or above the
    // root stays.
    std::vector<std::wstring> parts;
    size_t pos = 0;
    while (pos <= rest.size()) {
        const size_t sep = rest.find_first_of(L"\\/", pos);
        const std::wstring component = sep == std::wstring::npos
                                           ? rest.substr(pos)
                                           : rest.substr(pos, sep - pos);
        if (component == L"." || component.empty()) {
            // dropped
        } else if (component == L"..") {
            if (parts.empty() || parts.back() == L"..") parts.push_back(component);
            else parts.pop_back();
        } else {
            parts.push_back(component);
        }
        if (sep == std::wstring::npos) break;
        pos = sep + 1;
    }

    std::wstring normalized = root;
    for (const std::wstring& part : parts) {
        if (normalized.empty()) normalized = part;
        else if (isSep(normalized.back())) normalized += part;
        else normalized += L"\\" + part;
    }
    if (normalized.empty()) normalized = L".";
    return normalized;
}

bool pathExists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool pathIsDirectory(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool pathIsRegularFile(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

uint64_t pathFileSize(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        return 0;
    }
    return (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

bool createDirectories(const std::wstring& path) {
    if (path.empty()) return true;
    std::wstring p = path;
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    if (p.empty()) return false;
    if (pathIsDirectory(p)) return true;
    const size_t sep = p.find_last_of(L"\\/");
    if (sep != std::wstring::npos && sep > 0) {
        createDirectories(p.substr(0, sep));  // ensure the parent first
    }
    return CreateDirectoryW(p.c_str(), nullptr) != FALSE ||
           GetLastError() == ERROR_ALREADY_EXISTS;
}

bool removeAll(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;
    // Like the standard library's remove_all: recurse into real directories
    // only; a reparse point (junction/symlink) is a link to delete, never a
    // tree to walk into.
    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
        (attrs & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
        std::wstring search = path;
        if (search.back() != L'\\' && search.back() != L'/') search += L'\\';
        search += L"*";
        WIN32_FIND_DATAW findData;
        HANDLE hFind = FindFirstFileW(search.c_str(), &findData);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(findData.cFileName, L".") != 0 &&
                    wcscmp(findData.cFileName, L"..") != 0) {
                    removeAll(pathJoin(path, findData.cFileName));
                }
            } while (FindNextFileW(hFind, &findData));
            FindClose(hFind);
        }
        return RemoveDirectoryW(path.c_str()) != FALSE;
    }
    return DeleteFileW(path.c_str()) != FALSE;
}

bool listDirectory(const std::wstring& dir, std::vector<std::wstring>& paths) {
    paths.clear();
    if (dir.empty()) return false;
    std::wstring search = dir;
    if (search.back() != L'\\' && search.back() != L'/') search += L'\\';
    search += L"*";
    WIN32_FIND_DATAW findData;
    HANDLE hFind = FindFirstFileW(search.c_str(), &findData);
    if (hFind == INVALID_HANDLE_VALUE) return false;
    do {
        if (wcscmp(findData.cFileName, L".") != 0 &&
            wcscmp(findData.cFileName, L"..") != 0) {
            paths.push_back(pathJoin(dir, findData.cFileName));
        }
    } while (FindNextFileW(hFind, &findData));
    FindClose(hFind);
    return true;
}
