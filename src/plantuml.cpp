// PlantUML bridge implementation. App-free and headless: standard C++ plus
// the Win32 process and file APIs (CreateProcessW / WaitForSingleObject /
// TerminateProcess, file attributes, SearchPathW). No UI, no graphics, no
// theme or App types, and no remote access of any kind.

#include "plantuml.h"
#include "path_utils.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <fstream>
#include <string>
#include <vector>

#include <windows.h>

namespace plantuml {
namespace {

constexpr uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

void fnvByte(uint64_t& hash, unsigned char value) {
    hash ^= static_cast<uint64_t>(value);
    hash *= kFnvPrime;
}

void fnvBlock(uint64_t& hash, const void* data, size_t size) {
    const unsigned char* bytes = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) fnvByte(hash, bytes[i]);
}

// Length prefixing keeps the field stream unambiguous.
void fnvU64(uint64_t& hash, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        fnvByte(hash, static_cast<unsigned char>(value >> (8 * i)));
    }
}

void fnvText(uint64_t& hash, const std::string& text) {
    fnvU64(hash, static_cast<uint64_t>(text.size()));
    fnvBlock(hash, text.data(), text.size());
}

void fnvText(uint64_t& hash, const std::wstring& text) {
    fnvU64(hash, static_cast<uint64_t>(text.size()));
    for (wchar_t ch : text) {
        fnvByte(hash, static_cast<unsigned char>(ch & 0xFF));
        fnvByte(hash, static_cast<unsigned char>((ch >> 8) & 0xFF));
    }
}

bool fileExists(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool hasJarSuffix(const std::wstring& path) {
    if (path.size() < 4) return false;
    return _wcsicmp(path.c_str() + path.size() - 4, L".jar") == 0;
}

// Trims spaces, tabs and a CR terminator from a line range and matches the
// PlantUML anchor case-insensitively: the exact token `@startuml`, or the
// token followed by a block-name separator - whitespace (`@startuml Flow`)
// or an opening parenthesis (`@startuml(Flow)`). A lookalike such as
// `@startumlx`, and every non-UML start tag (`@startjson`, `@startyaml`,
// ...), never matches. A UTF-8 byte order mark ahead of the line (a .puml
// saved "UTF-8 with BOM") is skipped.
bool isAnchorLine(const std::string& source, size_t begin, size_t end) {
    if (end - begin >= 3 && static_cast<unsigned char>(source[begin]) == 0xEF &&
        static_cast<unsigned char>(source[begin + 1]) == 0xBB &&
        static_cast<unsigned char>(source[begin + 2]) == 0xBF) {
        begin += 3;
    }
    while (begin < end && (source[begin] == ' ' || source[begin] == '\t')) ++begin;
    while (end > begin && (source[end - 1] == ' ' || source[end - 1] == '\t' ||
                           source[end - 1] == '\r')) {
        --end;
    }
    static const char kAnchor[] = "@startuml";
    const size_t anchorLen = 9;
    if (end - begin < anchorLen) return false;
    for (size_t i = 0; i < anchorLen; ++i) {
        char c = source[begin + i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != kAnchor[i]) return false;
    }
    if (end - begin == anchorLen) return true;
    const char after = source[begin + anchorLen];
    return after == ' ' || after == '\t' || after == '(';
}

// The line [begin, end), leading blanks skipped, starts with `tag`
// case-insensitively. PlantUML starts a block on any line beginning
// `@start` (`@startmindmap`, `@startjson`, even `@startumlz`) and ends it
// on any line beginning `@end`.
bool lineStartsWith(const std::string& source, size_t begin, size_t end, const char* tag) {
    while (begin < end && (source[begin] == ' ' || source[begin] == '\t')) ++begin;
    for (size_t i = 0; tag[i] != '\0'; ++i, ++begin) {
        if (begin >= end) return false;
        char c = source[begin];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != tag[i]) return false;
    }
    return true;
}

// Locates the image the tool produced in `workDir`: the canonical
// `input.<ext>` (toolInput hands the tool one unnamed block), else the only
// remaining `*.<ext>` file (lexicographically first when there are
// several). Every candidate must be a non-empty regular file. Returns an
// empty path when nothing usable exists.
std::wstring resolveArtifact(const std::wstring& workDir, int format) {
    const std::wstring suffix = format == 1 ? L".svg" : L".png";
    auto usable = [](const std::wstring& candidate) {
        return pathIsRegularFile(candidate) && pathFileSize(candidate) > 0;
    };

    const std::wstring canonical = pathJoin(workDir, L"input" + suffix);
    if (usable(canonical)) return canonical;

    std::vector<std::wstring> candidates;
    std::vector<std::wstring> entries;
    listDirectory(workDir, entries);
    for (const auto& entry : entries) {
        if (!pathIsRegularFile(entry)) continue;
        std::wstring extension = pathExtension(entry);
        for (wchar_t& c : extension) {
            c = static_cast<wchar_t>(std::towlower(c));
        }
        if (extension != suffix) continue;
        if (!usable(entry)) continue;
        candidates.push_back(entry);
    }
    if (candidates.empty()) return L"";
    std::sort(candidates.begin(), candidates.end(),
              [](const std::wstring& left, const std::wstring& right) {
                  return left < right;
              });
    return candidates.front();
}

// Removes every image artifact and any staged source from the private
// workDir, so a failed render can never be mistaken for a success and no
// temp source survives a failure.
void scrubWorkDir(const std::wstring& workDir) {
    std::vector<std::wstring> entries;
    listDirectory(workDir, entries);
    for (const auto& entry : entries) {
        if (!pathIsRegularFile(entry)) continue;
        std::wstring ext = pathExtension(entry);
        for (wchar_t& c : ext) c = static_cast<wchar_t>(std::towlower(c));
        if (ext == L".png" || ext == L".svg" || ext == L".puml") {
            removeAll(entry);
        }
    }
}

}  // namespace

bool isFenceLanguage(const std::string& lowercasedLanguage) {
    return lowercasedLanguage == "plantuml" || lowercasedLanguage == "puml" ||
           lowercasedLanguage == "pu";
}

std::wstring Tool::describe() const {
    if (!available) return std::wstring();
    if (isJar) return program + L" -jar " + jar;
    return program;
}

Tool resolveTool(const std::wstring& userPath) {
    Tool tool;
    if (userPath.empty() || !fileExists(userPath)) return tool;

    if (hasJarSuffix(userPath)) {
        std::wstring java(MAX_PATH, L'\0');
        DWORD length = SearchPathW(nullptr, L"java.exe", nullptr,
                                   static_cast<DWORD>(java.size()), java.data(),
                                   nullptr);
        if (length == 0) return tool;
        if (length >= java.size()) {
            java.resize(length);
            length = SearchPathW(nullptr, L"java.exe", nullptr,
                                 static_cast<DWORD>(java.size()), java.data(),
                                 nullptr);
            if (length == 0 || length >= java.size()) return tool;
        }
        java.resize(length);
        tool.available = true;
        tool.isJar = true;
        tool.program = java;
        tool.jar = userPath;
        return tool;
    }

    tool.available = true;
    tool.isJar = false;
    tool.program = userPath;
    return tool;
}

Tool resolveToolWithPathSearch(const std::wstring& userPath) {
    if (!userPath.empty()) return resolveTool(userPath);
    // SearchPathW runs its default search order, not PATH alone: the
    // application directory and the current directory are searched before
    // the system directories.

    std::wstring found(MAX_PATH, L'\0');
    DWORD length = SearchPathW(nullptr, L"plantuml.exe", nullptr,
                               static_cast<DWORD>(found.size()),
                               found.data(), nullptr);
    if (length == 0) return Tool();
    if (length >= found.size()) {
        found.resize(length);
        length = SearchPathW(nullptr, L"plantuml.exe", nullptr,
                             static_cast<DWORD>(found.size()),
                             found.data(), nullptr);
        if (length == 0 || length >= found.size()) return Tool();
    }
    found.resize(length);
    return resolveTool(found);
}

bool injectPreamble(std::string& source, const std::string& preambleLines) {
    const size_t size = source.size();
    size_t insertAt = std::string::npos;
    bool anchorAtEof = false;
    size_t pos = 0;
    while (pos <= size) {
        const size_t newline = source.find('\n', pos);
        const size_t lineEnd = newline == std::string::npos ? size : newline;
        if (isAnchorLine(source, pos, lineEnd)) {
            anchorAtEof = newline == std::string::npos;
            insertAt = anchorAtEof ? size : newline + 1;
            break;
        }
        if (newline == std::string::npos) break;
        pos = newline + 1;
    }
    if (insertAt == std::string::npos) return false;
    if (preambleLines.empty()) return true;

    std::string block = preambleLines;
    if (block.back() != '\n') block += '\n';
    if (anchorAtEof) {
        source += '\n';
        insertAt = source.size();
    }
    source.insert(insertAt, block);
    return true;
}

std::string preamble(const std::string& fontFamily, float fontBaseSize,
                     const std::string& textColorHex, bool darkPalette,
                     const std::string& fillColorHex,
                     const std::string& strokeColorHex) {
    char sizeText[64] = {};
    if (fontBaseSize >= 0.0f && fontBaseSize <= 4096.0f &&
        static_cast<float>(static_cast<int>(fontBaseSize)) == fontBaseSize) {
        std::snprintf(sizeText, sizeof(sizeText), "%d",
                      static_cast<int>(fontBaseSize));
    } else {
        std::snprintf(sizeText, sizeof(sizeText), "%.4g",
                      static_cast<double>(fontBaseSize));
    }

    std::string out;
    out += "skinparam backgroundColor transparent\n";
    out += "skinparam shadowing false\n";
    out += "skinparam defaultFontName " + fontFamily + "\n";
    out += "skinparam defaultFontSize " + std::string(sizeText) + "\n";
    out += "skinparam defaultFontColor " + textColorHex + "\n";
    if (!darkPalette) return out;

    // Dark palettes only: PlantUML's built-in light fills and dark strokes
    // would hide the themed text (light on light) or vanish against the page
    // (dark ink on a dark background). Theme every fill and stroke from the
    // supplied colors so the diagram reads on the dark page: fills use the
    // palette surface text is designed to sit on, strokes/borders/lifelines
    // use the accent. PlantUML ignores unknown skinparam names, so the set
    // below is deliberately generous across diagram families.
    out += "skinparam ArrowColor " + strokeColorHex + "\n";
    out += "skinparam ArrowFontColor " + textColorHex + "\n";
    out += "skinparam sequenceArrowColor " + strokeColorHex + "\n";
    out += "skinparam sequenceLifeLineBorderColor " + strokeColorHex + "\n";
    out += "skinparam sequenceGroupBorderColor " + strokeColorHex + "\n";
    out += "skinparam participantBackgroundColor " + fillColorHex + "\n";
    out += "skinparam participantBorderColor " + strokeColorHex + "\n";
    out += "skinparam actorBackgroundColor " + fillColorHex + "\n";
    out += "skinparam actorBorderColor " + strokeColorHex + "\n";
    out += "skinparam classBackgroundColor " + fillColorHex + "\n";
    out += "skinparam classBorderColor " + strokeColorHex + "\n";
    out += "skinparam usecaseBackgroundColor " + fillColorHex + "\n";
    out += "skinparam usecaseBorderColor " + strokeColorHex + "\n";
    out += "skinparam activityBackgroundColor " + fillColorHex + "\n";
    out += "skinparam activityBorderColor " + strokeColorHex + "\n";
    out += "skinparam activityDiamondBackgroundColor " + fillColorHex + "\n";
    out += "skinparam activityDiamondBorderColor " + strokeColorHex + "\n";
    out += "skinparam stateBackgroundColor " + fillColorHex + "\n";
    out += "skinparam stateBorderColor " + strokeColorHex + "\n";
    out += "skinparam objectBackgroundColor " + fillColorHex + "\n";
    out += "skinparam componentBackgroundColor " + fillColorHex + "\n";
    out += "skinparam noteBackgroundColor " + fillColorHex + "\n";
    out += "skinparam noteBorderColor " + strokeColorHex + "\n";
    return out;
}

uint64_t cacheKey(const std::string& source, const std::string& preambleLines,
                  const std::wstring& toolPath, uint64_t toolStamp, int format) {
    uint64_t hash = kFnvOffset;
    fnvText(hash, source);
    fnvText(hash, preambleLines);
    fnvText(hash, toolPath);
    fnvU64(hash, toolStamp);
    fnvU64(hash, static_cast<uint64_t>(static_cast<uint32_t>(format)));
    return hash;
}

uint64_t toolStampFor(const std::wstring& path) {
    if (path.empty()) return 0;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return 0;
    const uint64_t mtime =
        (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
        static_cast<uint64_t>(data.ftLastWriteTime.dwLowDateTime);
    const uint64_t size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) |
                          static_cast<uint64_t>(data.nFileSizeLow);
    uint64_t hash = kFnvOffset;
    fnvU64(hash, mtime);
    fnvU64(hash, size);
    return hash;
}

std::wstring buildCommandLine(const Tool& tool, int format,
                              const std::wstring& outDir,
                              const std::wstring& inputFile) {
    const wchar_t* formatFlag = format == 1 ? L"-tsvg" : L"-tpng";
    std::wstring cmd;
    cmd += L'"';
    cmd += tool.program;
    cmd += L'"';
    if (tool.isJar) {
        // The JVM property as well as the environment (sandboxEnvironment):
        // either one puts PlantUML in its sandbox
        cmd += L" -DPLANTUML_SECURITY_PROFILE=SANDBOX -jar \"";
        cmd += tool.jar;
        cmd += L'"';
    }
    cmd += L' ';
    cmd += formatFlag;
    cmd += L" -charset UTF-8 -failfast2 -o \"";
    cmd += outDir;
    cmd += L"\" \"";
    cmd += inputFile;
    cmd += L'"';
    return cmd;
}

std::wstring sandboxEnvironment(const wchar_t* inherited) {
    static const wchar_t kProfile[] = L"PLANTUML_SECURITY_PROFILE";
    // Variable name: up to the first '=' after position 0, since the
    // per-drive current directories ("=C:=C:\...") start with one
    auto nameOf = [](const std::wstring& entry) {
        const size_t eq = entry.find(L'=', 1);
        return eq == std::wstring::npos ? entry : entry.substr(0, eq);
    };
    std::vector<std::wstring> vars;
    for (const wchar_t* p = inherited; p != nullptr && *p != L'\0'; p += wcslen(p) + 1) {
        std::wstring entry(p);
        // Whatever profile the user's environment names, Tinta's renders
        // run in the sandbox: a document must not reach files or the net
        if (_wcsicmp(nameOf(entry).c_str(), kProfile) == 0) continue;
        vars.push_back(std::move(entry));
    }
    vars.push_back(std::wstring(kProfile) + L"=SANDBOX");
    // CreateProcessW expects the block sorted by name, ignoring case
    std::stable_sort(vars.begin(), vars.end(),
                     [&](const std::wstring& a, const std::wstring& b) {
                         return _wcsicmp(nameOf(a).c_str(), nameOf(b).c_str()) < 0;
                     });
    std::wstring block;
    for (const std::wstring& entry : vars) {
        block += entry;
        block += L'\0';
    }
    block += L'\0';
    return block;
}

std::string toolInput(const std::string& source) {
    const size_t size = source.size();
    size_t pos = 0;
    while (pos < size) {
        const size_t newline = source.find('\n', pos);
        const size_t lineEnd = newline == std::string::npos ? size : newline;
        if (isAnchorLine(source, pos, lineEnd)) {
            std::string out = "@startuml\n";
            size_t line = newline == std::string::npos ? size : newline + 1;
            while (line < size) {
                const size_t next = source.find('\n', line);
                const size_t end = next == std::string::npos ? size : next;
                // Any start tag opens another, nameable block: the copy ends
                if (lineStartsWith(source, line, end, "@start")) break;
                if (lineStartsWith(source, line, end, "@end")) {
                    out += "@enduml\n";  // bare, whatever followed the tag
                    break;
                }
                const size_t stop = next == std::string::npos ? size : next + 1;
                out.append(source, line, stop - line);
                line = stop;
            }
            return out;
        }
        if (newline == std::string::npos) break;
        pos = newline + 1;
    }
    return source;
}

bool renderSync(const Tool& tool, const std::string& sourceWithPreamble,
                int format, const std::wstring& workDir, std::wstring& outFile,
                DWORD timeoutMs, std::wstring& error, int* exitCodeOut,
                HANDLE cancelEvent) {
    outFile.clear();
    error.clear();
    if (exitCodeOut != nullptr) *exitCodeOut = -1;
    const int fmt = format == 1 ? 1 : 0;

    if (!tool.available || tool.program.empty() ||
        (tool.isJar && tool.jar.empty())) {
        error = L"PlantUML tool is not available";
        return false;
    }
    if (timeoutMs == INFINITE) {
        error = L"PlantUML render requires a bounded timeout";
        return false;
    }
    if (workDir.empty()) {
        error = L"PlantUML work directory is empty";
        return false;
    }

    if (!createDirectories(workDir) || !pathIsDirectory(workDir)) {
        error = L"Cannot prepare the PlantUML work directory";
        return false;
    }

    const std::wstring inputPath = pathJoin(workDir, L"input.puml");
    const std::string input = toolInput(sourceWithPreamble);
    {
        std::ofstream out(inputPath.c_str(), std::ios::binary | std::ios::trunc);
        if (!out) {
            error = L"Cannot write the PlantUML input file";
            return false;
        }
        out.write(input.data(), static_cast<std::streamsize>(input.size()));
        out.flush();
        if (!out) {
            error = L"Cannot write the PlantUML input file";
            return false;
        }
    }

    std::wstring command =
        buildCommandLine(tool, fmt, workDir, inputPath);
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    std::wstring environment;
    if (wchar_t* inherited = GetEnvironmentStringsW()) {
        environment = sandboxEnvironment(inherited);
        FreeEnvironmentStringsW(inherited);
    } else {
        environment = sandboxEnvironment(nullptr);
    }

    // The tool runs in a job that dies with it: a timeout or a cancel ends
    // everything it started (Graphviz under java.exe, java.exe under a
    // launcher), not just the direct child
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                     sizeof(limits))) {
            CloseHandle(job);
            job = nullptr;
        }
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
                        environment.data(), workDir.c_str(), &si, &pi)) {
        const DWORD lastError = GetLastError();
        if (job != nullptr) CloseHandle(job);
        scrubWorkDir(workDir);
        error = L"Cannot start the PlantUML tool (error " +
                std::to_wstring(lastError) + L")";
        return false;
    }
    if (job != nullptr && !AssignProcessToJobObject(job, pi.hProcess)) {
        CloseHandle(job);  // runs anyway, just without the tree kill
        job = nullptr;
    }
    ResumeThread(pi.hThread);

    const HANDLE waits[2] = {pi.hProcess, cancelEvent};
    const DWORD waitResult =
        WaitForMultipleObjects(cancelEvent != nullptr ? 2 : 1, waits, FALSE, timeoutMs);
    bool timedOut = false;
    bool cancelled = false;
    DWORD exitCode = 1;
    if (waitResult == WAIT_OBJECT_0) {
        GetExitCodeProcess(pi.hProcess, &exitCode);
    } else {
        cancelled = waitResult == WAIT_OBJECT_0 + 1;
        timedOut = !cancelled;
        if (job != nullptr) TerminateJobObject(job, 1);
        else TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);  // reap the killed process
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (job != nullptr) CloseHandle(job);  // ends whatever the tool left running

    if (cancelled) {
        scrubWorkDir(workDir);
        error = L"PlantUML render cancelled";
        return false;
    }
    if (timedOut) {
        scrubWorkDir(workDir);
        error = L"PlantUML timed out after " + std::to_wstring(timeoutMs) + L" ms";
        return false;
    }
    if (exitCode != 0) {
        if (exitCodeOut != nullptr) *exitCodeOut = static_cast<int>(exitCode);
        scrubWorkDir(workDir);
        error = L"PlantUML exited with code " + std::to_wstring(exitCode);
        return false;
    }

    const std::wstring artifact = resolveArtifact(workDir, fmt);
    if (artifact.empty()) {
        if (exitCodeOut != nullptr) *exitCodeOut = static_cast<int>(exitCode);
        scrubWorkDir(workDir);
        error = L"PlantUML produced no usable image";
        return false;
    }
    if (exitCodeOut != nullptr) *exitCodeOut = 0;
    outFile = artifact;
    return true;
}

}  // namespace plantuml
