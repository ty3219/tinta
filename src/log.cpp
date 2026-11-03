#include "log.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

struct Log {
    std::mutex mutex;
    std::string path;       // resolved file path (UTF-8), empty until opened
    std::ofstream file;
    bool attempted = false; // one open attempt per resolved path
};

Log& state() {
    static Log log;
    return log;
}

#ifdef _WIN32
std::wstring toWide(const std::string& text) {
    if (text.empty()) return {};
    int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide(static_cast<size_t>(length - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &wide[0], length);
    return wide;
}

std::string toUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0,
                                     nullptr, nullptr);
    if (length <= 0) return {};
    std::string utf8(static_cast<size_t>(length - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, &utf8[0], length,
                        nullptr, nullptr);
    return utf8;
}

std::string currentDirectoryUtf8() {
    wchar_t cwd[MAX_PATH];
    DWORD length = GetCurrentDirectoryW(MAX_PATH, cwd);
    if (length == 0 || length >= MAX_PATH) return {};
    return toUtf8(std::wstring(cwd, length));
}
#endif

// The launch folder: what the user calls the "current directory". The app
// parks its working directory into the system folder a moment after startup
// (#253), so the path is captured here on the first log line — before
// WinMain's parkWorkingDirectory() runs.
std::string defaultLogPath() {
#ifdef _WIN32
    std::string cwd = currentDirectoryUtf8();
    if (cwd.empty()) return "tinta.log";
    if (cwd.back() != '\\' && cwd.back() != '/') cwd += '\\';
    return cwd + "tinta.log";
#else
    return "tinta.log";
#endif
}

void open(Log& log) {
    if (log.file.is_open() || log.attempted) return;
    log.attempted = true;
    std::string path = log.path.empty() ? defaultLogPath() : log.path;
#ifdef _WIN32
    log.file.open(toWide(path),
                  std::ios::out | std::ios::binary | std::ios::trunc);
    if (!log.file.is_open()) {
        // Launch folder not writable (Program Files, a read-only share, a
        // portable exe inside a zip): fall back to the temp folder so the
        // trail still lands somewhere the user can reach.
        wchar_t temp[MAX_PATH];
        DWORD length = GetTempPathW(MAX_PATH, temp);
        if (length > 0 && length < MAX_PATH) {
            std::string tempDir = toUtf8(std::wstring(temp, length));
            if (!tempDir.empty() && tempDir.back() != '\\' &&
                tempDir.back() != '/') {
                tempDir += '\\';
            }
            path = tempDir + "tinta.log";
            log.file.open(toWide(path),
                          std::ios::out | std::ios::binary | std::ios::trunc);
        }
    }
#else
    log.file.open(path, std::ios::out | std::ios::binary | std::ios::trunc);
#endif
    if (log.file.is_open()) log.path = path;
}

unsigned long long nowMs() {
    static const auto started = std::chrono::steady_clock::now();
    return static_cast<unsigned long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started)
            .count());
}

}  // namespace

void tintaLog(const char* fmt, ...) {
    char message[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    char line[2304];
#ifdef _WIN32
    int written = snprintf(line, sizeof(line), "[t=%llu tid=%lu] %s\n",
                           nowMs(), (unsigned long)GetCurrentThreadId(),
                           message);
#else
    int written = snprintf(line, sizeof(line), "[t=%llu] %s\n", nowMs(),
                           message);
#endif
    if (written <= 0) return;

    Log& log = state();
    std::lock_guard<std::mutex> lock(log.mutex);
    open(log);
    if (log.file.is_open()) {
        size_t count = static_cast<size_t>(written);
        if (count > sizeof(line)) count = sizeof(line);
        log.file.write(line, static_cast<std::streamsize>(count));
        log.file.flush();
    }
#ifdef _WIN32
    OutputDebugStringA(line);
#endif
}

std::string tintaLogPath() {
    Log& log = state();
    std::lock_guard<std::mutex> lock(log.mutex);
    open(log);
    return log.path;
}

void tintaLogSetPath(const std::string& path) {
    Log& log = state();
    std::lock_guard<std::mutex> lock(log.mutex);
    if (log.file.is_open()) log.file.close();
    log.path = path;
    log.attempted = false;
}

void tintaLogReset() {
    Log& log = state();
    std::lock_guard<std::mutex> lock(log.mutex);
    if (log.file.is_open()) log.file.close();
    log.path.clear();
    log.attempted = false;
}
