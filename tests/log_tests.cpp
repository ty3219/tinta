// Regression checks for the diagnostic log module (include/log.h,
// src/log.cpp). The log writes tinta.log in the launch directory; these
// tests redirect it to a temp file so the assertions never touch a real
// launch folder, then verify the line format, append behavior, reset and
// thread safety. Pure C++ so it builds and runs on any host.
#include "log.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << "\n";
        ++g_failures;
    }
}

std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    return content;
}

fs::path tempLogPath(int suffix) {
    return fs::temp_directory_path() /
           ("tinta-log-test-" + std::to_string(suffix) + ".log");
}

// The written line is "[t=123 tid=456] message\n"; the message part (the
// caller's text) must follow the prefix and a newline.
bool lineHas(const std::string& line, const std::string& message) {
    return line.rfind("[t=", 0) == 0 && line.find("] ") != std::string::npos &&
           line.find(message) != std::string::npos &&
           line.back() == '\n';
}

void testBasicAppend() {
    fs::path first = tempLogPath(1);
    fs::remove(first);
    tintaLogReset();
    tintaLogSetPath(first.u8string());

    tintaLog("startup: version %d.%d", 3, 9);
    tintaLog("file opened: %s (%zu bytes)", "notes.md", 42);
    tintaLog("parse failed: hr=0x%08X", 0x80070002);

    check(fs::exists(first), "first log file created");
    std::string content = readFile(first);
    std::vector<std::string> lines;
    std::string::size_type start = 0;
    while (start < content.size()) {
        std::string::size_type end = content.find('\n', start);
        if (end == std::string::npos) break;
        lines.push_back(content.substr(start, end + 1 - start));
        start = end + 1;
    }
    check(lines.size() == 3, "three log lines written");
    if (lines.size() == 3) {
        check(lineHas(lines[0], "startup: version 3.9"),
              "first line formatted with args");
        check(lineHas(lines[1], "file opened: notes.md (42 bytes)"),
              "second line formatted with string/size args");
        check(lineHas(lines[2], "parse failed: hr=0x80070002"),
              "third line formatted with hex arg");
    }

    // A later line appends instead of truncating the file.
    tintaLog("later: appended");
    std::string after = readFile(first);
    check(after.size() > content.size(), "log appends across calls");
    check(after.find("later: appended") != std::string::npos,
          "appended line present");
    tintaLogReset();
    fs::remove(first);
}

void testThreadSafety() {
    fs::path path = tempLogPath(2);
    fs::remove(path);
    tintaLogReset();
    tintaLogSetPath(path.u8string());

    constexpr int kThreads = 8;
    constexpr int kLinesEach = 25;
    std::atomic<int> started{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([t, &started, kLinesEach] {
            ++started;
            for (int i = 0; i < kLinesEach; ++i) {
                tintaLog("thread %d line %d", t, i);
            }
        });
    }
    for (auto& worker : workers) worker.join();

    check(started == kThreads, "all worker threads ran");
    std::string content = readFile(path);
    for (int t = 0; t < kThreads; ++t) {
        for (int i = 0; i < kLinesEach; ++i) {
            std::string needle = "thread " + std::to_string(t) + " line " +
                                 std::to_string(i) + "\n";
            check(content.find(needle) != std::string::npos,
                  "threaded line present and complete");
        }
    }
    tintaLogReset();
    fs::remove(path);
}

void testResetRepoints() {
    fs::path first = tempLogPath(3);
    fs::path second = tempLogPath(4);
    fs::remove(first);
    fs::remove(second);

    tintaLogReset();
    tintaLogSetPath(first.u8string());
    tintaLog("before reset");
    tintaLogReset();
    tintaLogSetPath(second.u8string());
    tintaLog("after reset");

    check(fs::exists(first), "first path written before reset");
    check(fs::exists(second), "second path written after reset");
    std::string firstContent = readFile(first);
    std::string secondContent = readFile(second);
    check(firstContent.find("before reset") != std::string::npos,
          "first path keeps its lines");
    check(firstContent.find("after reset") == std::string::npos,
          "post-reset lines go to the new path only");
    check(secondContent.find("after reset") != std::string::npos,
          "second path receives post-reset lines");

    tintaLogReset();
    fs::remove(first);
    fs::remove(second);
}

}  // namespace

int main() {
    testBasicAppend();
    testThreadSafety();
    testResetRepoints();

    if (g_failures > 0) {
        std::cerr << "log_tests: " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "log_tests: diagnostic log module OK\n";
    return 0;
}
