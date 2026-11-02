// Windows 7 compatibility regression scan.
//
// The portable exe must not statically import Windows 8/10-only APIs, or it
// fails to load on Windows 7 ("procedure entry point not found"). Every such
// call has to go through the dynamic-loading shims in include/win7_compat.h,
// which resolve the symbols at runtime and fall back to legacy equivalents.
//
// This test scans the source tree and fails if any of those API names appear
// anywhere except the shim header and this test itself. It is pure standard
// C++ (no windows.h) so it builds and runs on any host.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

const char* kForbidden[] = {
    "GetDpiForWindow",            // Windows 10 1607+
    "GetSystemMetricsForDpi",     // Windows 10 1607+
    "SetProcessDpiAwarenessContext",  // Windows 10 1703+
    "GetCurrentPackageFullName",  // Windows 8+
    "GetPointerType",             // Windows 8+
    "GetPointerInfo",             // Windows 8+
    "CreateFile2",                // Windows 8+ (MSVC <filesystem> imports it)
    "CopyFile2",                  // Windows 8+ (MSVC <filesystem> imports it)
    "GetSystemTimePreciseAsFileTime",  // Windows 8+ (MSVC <chrono> system_clock)
};

// <filesystem> in the production tree. Since the MSVC STL's Win8 baseline
// (VS 2022 17.14, April 2025), std::filesystem statically imports
// CreateFile2/CopyFile2 from kernel32.dll, so any use in the exe makes the
// loader refuse to start on Windows 7. The tests keep std::filesystem (they
// run on Windows 10/11 CI), so this ban covers src/ and include/ only.
const char* kForbiddenFilesystem[] = {
    "<filesystem>",
    "<experimental/filesystem>",
    "std::filesystem",
};

const char* kAllowedFiles[] = {
    "win7_compat.h",   // the shim header (GetProcAddress string literals)
    "win7_compat_tests.cpp",  // this file (the list above)
    "path_utils.h",    // documents why <filesystem> is banned
};

// The token counts only when it is a whole identifier, so a hypothetical
// MyGetDpiForWindow in another project's code does not trip the scan.
bool wholeIdentifier(const std::string& line, size_t pos, size_t len) {
    auto isIdent = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '_';
    };
    if (pos > 0 && isIdent(line[pos - 1])) return false;
    if (pos + len < line.size() && isIdent(line[pos + len])) return false;
    return true;
}

int scanFile(const std::filesystem::path& path, int& failures,
             bool scanFilesystem) {
    std::ifstream in(path);
    if (!in) {
        std::cerr << "FAIL: cannot read " << path << "\n";
        ++failures;
        return 0;
    }
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        for (const char* api : kForbidden) {
            std::string token(api);
            size_t pos = 0;
            while ((pos = line.find(token, pos)) != std::string::npos) {
                if (wholeIdentifier(line, pos, token.size())) {
                    std::cerr << "FAIL: " << path << ":" << lineNo
                              << " calls " << token
                              << " directly; route through win7_compat.h\n";
                    ++failures;
                }
                pos += token.size();
            }
        }
        if (scanFilesystem) {
            for (const char* api : kForbiddenFilesystem) {
                std::string token(api);
                size_t pos = 0;
                while ((pos = line.find(token, pos)) != std::string::npos) {
                    std::cerr << "FAIL: " << path << ":" << lineNo
                              << " uses " << token
                              << "; route through path_utils.h instead\n";
                    ++failures;
                    pos += token.size();
                }
            }
        }
    }
    return 0;
}

}  // namespace

int main() {
#ifndef TINTA_SOURCE_DIR
    std::cerr << "TINTA_SOURCE_DIR must be defined (CMake does this)\n";
    return 1;
#endif
    const std::filesystem::path root(TINTA_SOURCE_DIR);
    int failures = 0;
    for (const char* dirName : { "src", "include", "tests" }) {
        std::filesystem::path dir = root / dirName;
        std::error_code ec;
        if (!std::filesystem::exists(dir, ec)) continue;
        // std::filesystem is fine in tests (they run on Windows 10/11 CI);
        // the exe's own source may not use it (see kForbiddenFilesystem).
        const bool production = std::string(dirName) != "tests";
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file()) continue;
            std::string name = entry.path().filename().string();
            std::string ext = entry.path().extension().string();
            if (ext != ".cpp" && ext != ".h") continue;
            bool allowed = false;
            for (const char* allowedFile : kAllowedFiles) {
                if (name == allowedFile) { allowed = true; break; }
            }
            if (!allowed) scanFile(entry.path(), failures, production);
        }
    }
    if (failures > 0) {
        std::cerr << "win7_compat: " << failures
                  << " direct call(s) of Windows 8/10-only APIs\n";
        return 1;
    }
    std::cout << "win7_compat: no Windows 8/10-only API calls and no "
                 "std::filesystem outside win7_compat.h / path_utils.h\n";
    return 0;
}
