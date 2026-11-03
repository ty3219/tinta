# Tinta Linux Port Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Port Tinta — the native Windows Markdown/Mermaid viewer built on Direct2D/DirectWrite — to a native Linux build (X11 and Wayland) with **no Wine, no web engine**, preserving the architecture, the feature set, the fast-startup character, and the "one native binary" model.

**Architecture:** Add a `platform/` abstraction layer (window, painter, text engine, image, clipboard, dialogs, filesystem, HTTP, process, print) with two backends: the existing D2D/DWrite/Win32 code stays as the Windows backend (regression baseline), and a new Skia + SDL3 backend drives Linux. Core logic that is already portable — md4c parsing, the qmd:: element tree, the mermaid/mermaid_ext layout engines, math parsing, syntax highlighting, settings, i18n, exports — is compiled as a shared `tinta_core` library on both platforms. The Windows build and its test suite must stay green for the entire port.

**Tech Stack:** C++17, CMake ≥3.15 (already used), SDL3 (windowing/input/IME/touch/clipboard, X11+Wayland), Google Skia with SkParagraph (2D GPU/software rendering + full text layout, the closest cross-platform equivalent to DirectWrite), fontconfig/HarfBuzz (via Skia's default font manager), libcurl (HTTP), Skia's PDF backend (`SkPDFDocument`, replaces `ID2D1PrintControl`), XDG Desktop Portal (file dialogs, "reveal in file manager"), D-Bus (`xdg-desktop-portal` / `org.freedesktop.FileManager1`), `xdg-mime`/`.desktop` files (file association). Tests: CTest + the existing `tests/keystroke.h` convention, plus new headless raster checks.

## Global Constraints

- **No Wine.** The Linux binary links no Windows API and does not run under Wine.
- **No web engine.** No Electron, CEF, QtWebEngine, WebView, or `libwebkit`. Rendering stays native 2D.
- **Windows stays green.** Every task that changes shared code must keep the MSVC build and `ctest` passing on Windows. The Windows CI job in `.github/workflows/build.yml` is the gate.
- **Keep `std::wstring` internally.** On Windows `wchar_t` is UTF-16; on Linux it is UTF-32. All encoding conversions go through one header, `platform/text.h`. No scattered `WideCharToMultiByte`/`MultiByteToWideChar`.
- **All UI coordinates are DIPs (96 dpi).** On Linux the window's device-pixel scale multiplies at the canvas/present level, mirroring how `dpi(App, x)` already scales D2D coordinates.
- **One code path, two backends** is the end state. During the port, `#ifdef TINTA_PLATFORM_LINUX` selects new code; the Windows path keeps the old D2D code until each module is migrated onto the abstraction.
- **Dependency policy:** static, vendored or system packages only; no runtime installer on the end user's machine. Skia is built once by a `scripts/linux/build-skia.sh` script (GN → static `libskia.a`); SDL3, curl, fontconfig come from system packages with a FetchContent fallback for SDL3.
- **AGENTS.md applies to every behavior change:** new or changed rendering behavior gets a runnable fixture under `tests/fixtures/` (mixing the feature with headings, tables, lists, emphasis, links, quotes, code), is exercised in the built app at normal and narrow widths and across themes/exports, and gets an automated regression check. Generated screenshots/exports go to ignored build/output directories.
- **Keyboard in tests goes through `tests/keystroke.h`** (key-down first, character handed over only when unconsumed). On Linux the same header is driven by injected SDL events, never direct `handleCharInput` calls.
- **Commits referencing GitHub work** name the issue; no automatic replies to reporters — a draft reply is prepared for the maintainer when a feature is finished.
- Version floors: CMake ≥ 3.15; C++17; SDL3 ≥ 3.2; Skia must be built from the pinned commit in `scripts/linux/build-skia.sh` (parity is validated against it, upgrades happen as their own task).

---

## 1. Background & Key Decisions

### 1.1 What actually needs to move (measured against the current tree)

| Layer | Current code | Linux replacement |
|---|---|---|
| Window, message loop, timers, DWM chrome | `src/main_d2d.cpp` (2414 lines), `WinMain`/`WndProc` | `platform/linux/Main.cpp` + SDL3 event loop |
| 2D rendering | Direct2D (`src/render.cpp`, `src/overlays.cpp`, `src/editor.cpp`, `src/tabs.cpp`, `src/startpage.cpp`, `src/themes.cpp`, `src/inline_style.cpp`) | Skia `SkCanvas` via a `Painter` abstraction |
| Text layout, shaping, fallback | DirectWrite (`createLayout` in `render.cpp`, formats in `d2d_init.cpp`) | SkParagraph + `SkFontMgr::RefDefault()` (fontconfig) |
| Font selection (missing faces, CJK) | `installedFace()` / `documentCjkFamily()` in `d2d_init.cpp` | fontconfig matching (`platform/linux/TextEngineSkia.cpp`) |
| Image decode | WIC (`src/image_loader.cpp`, `render.cpp::asyncImageWorker`) | SkCodec (PNG/JPEG/GIF/WebP/AVIF/BMP) + Skia SVG (`SkSVGDOM`) |
| Remote images + update check | `URLDownloadToCacheFileW`, WinHTTP (`main_d2d.cpp`) | libcurl |
| Keyboard/mouse/touch/IME | `src/input.cpp` (4626 lines), `src/touch.cpp`, `imm32` | SDL3 events + SDL text-input/IME events; `Keymap.h` translation tables |
| Clipboard | OLE (`src/utils.cpp`, `export_docx.cpp`) | SDL3 clipboard; `text/html` and `image/png` mime via `SDL_SetClipboardData` |
| File dialogs, file association, explorer reveal | `commdlg`, `ShellExecute`, `shell32` | XDG Desktop Portal / `xdg-mime` / `org.freedesktop.FileManager1` |
| Config dir, portable mode | `%APPDATA%\Tinta`, exe-adjacent `settings.ini` (`src/config_dir.cpp`) | `$XDG_CONFIG_HOME/tinta`, exe-adjacent `settings.ini` |
| File watching, file stat | `FindFirstChangeNotification` (via `file_utils.cpp`), `WIN32_FILE_ATTRIBUTE_DATA` (`plantuml.cpp`) | inotify (`platform/linux/FileSystemPosix.cpp`), `stat()` |
| Print / PDF export | `ID2D1PrintControl` + "Microsoft Print to PDF" (`src/print.cpp`) | Skia `SkPDFDocument` (vector pages, embedded fonts) |
| External processes (pandoc, PlantUML jar, xdg-open) | `CreateProcessW` (`pandoc.cpp`), `OpenProcess` (`plantuml_queue.cpp`) | POSIX `posix_spawn`/`fork+exec`; pid liveness via `/proc` |
| Packaging | MSIX / winget / portable exe | CPack → `.deb` + AppImage; `.desktop` + icons |
| Tests | Win32 message-only windows (`tests/input_tests.cpp` etc.) | SDL3 headless event injection into the same handlers |

### 1.2 Technology decision

**Skia + SkParagraph + SDL3 is the recommendation.** Rationale, grounded in what the code needs:

- The app is a **custom-drawn UI with its own block/layout engine** (`render.cpp`), not a widget app. It needs a bare canvas + a text layout engine, exactly what Skia provides. SkParagraph is the only cross-platform layout stack with DirectWrite-grade capabilities the code already depends on: per-range styling (bold/italic/code/sup/sub inside one layout), automatic font fallback (emoji and CJK), bidirectional text, line breaking, and metrics (`widthIncludingTrailingWhitespace`, height, baseline).
- SDL3 supplies windowing (X11 + Wayland), keyboard/mouse/touch events, **text-input/IME events** (`SDL_EVENT_TEXT_INPUT`, `SDL_EVENT_TEXT_EDITING`, `SDL_SetTextInputArea` for candidate-window placement), drag-drop, and clipboard — the pieces that would otherwise each be a separate integration.
- Skia's GPU backends (Vulkan preferred, OpenGL fallback) preserve the "hardware-accelerated rendering" positioning, with a raster fallback that matches the app's existing "software first paint, then swap to GPU" startup trick (`WM_APP_GPU_READY`).
- Skia's PDF backend replaces the Windows print pipeline for export to PDF while keeping vector output + embedded fonts (including CJK).

**Alternatives considered (Decision Record, kept for later re-evaluation):**

| Option | Pros | Cons |
|---|---|---|
| Qt 6 (QPainter/QTextLayout) | Windowing/IME/dialogs/clipboard/print for free | Heavy dependency, its own widget semantics fight the custom-drawn design; larger footprint |
| Cairo + Pango | Lightweight, standard on Linux, excellent Pango shaping | CPU rendering mostly; cairo-gl is immature; more manual plumbing for windows/events |
| GLFW + custom text pipeline (harfbuzz/freetype) | Smallest | No IME/touch/clipboard/drop support; would re-implement paragraph layout — the largest risk |
| Wine | None | Forbidden by the requirement |

Decision: **Skia + SkParagraph + SDL3.** Re-evaluate only if SkParagraph's layout fidelity on a specific fixture differs from DirectWrite (see Phase 2 validation fixtures).

### 1.3 The two critical cross-platform hazards (must be solved first)

1. **`wchar_t` size.** Windows: 16-bit UTF-16. Linux: 32-bit UTF-32. The plan does **not** refactor `std::wstring` away; instead `platform/text.h` provides `toWide`, `fromWide`, `wideToUtf16`, `utf16ToWide`, and a one-time audit of the handful of UTF-16-specific assumptions (`src/editor.cpp:829` "off surrogate-pair middles", emoji surrogate handling). With UTF-32, a surrogate pair is a single code point, so the caret-clamping logic in `editor.cpp` becomes "never split a code point" and is simpler.
2. **`D2D1_COLOR_F` vs `qmd::Color`.** Both are `{r,g,b,a}` float structs with identical layout. `include/types.h` already defines `qmd::Color`. The migration re-types `D2DTheme` and all inline-style colors to `qmd::Color` (a `static_assert(sizeof)` + implicit conversion bridge during the transition on Windows). This is mechanical and lets themes, syntax colors, and exporters share one color type.

---

## 2. Architecture & File Map

### 2.1 Layering

```
+----------------------------+----------------------------+
| tinta_core (static lib)    | App logic (both platforms) |
| platform-free:             | render.cpp editor.cpp      |
| markdown.cpp footnotes.cpp | overlays.cpp tabs.cpp      |
| document.cpp frontmatter   | input.cpp touch.cpp        |
| syntax.cpp math_parser     | search*.cpp themes.cpp     |
| mermaid.cpp mermaid_ext    | inline_style.cpp           |
| export_html settings i18n  | image_loader file_utils    |
+----------------------------+ print export_docx          |
                              | annotations drafts         |
                              | startpage editrail         |
                              | sidepanels tableedit       |
                              | keymap utils signals       |
                              | pandoc plantuml* wordcount|
+----------------------------+----------------------------+
| platform/ (pure interfaces)                            |
| Types.h text.h Keymap.h Window.h Event.h Painter.h     |
| TextEngine.h Image.h Clipboard.h Dialogs.h             |
| FileSystem.h HttpClient.h Process.h Print.h            |
+--------------------------------------------------------+
+---------------------------+----------------------------+
| platform/windows/         | platform/linux/            |
| (wraps existing D2D)      | Main.cpp WindowSDL.cpp     |
|                           | PainterSkia.cpp            |
|                           | TextEngineSkia.cpp         |
|                           | ImageSkia.cpp              |
|                           | ClipboardSDL.cpp           |
|                           | DialogsPortal.cpp          |
|                           | FileSystemPosix.cpp        |
|                           | HttpCurl.cpp ProcessPosix  |
|                           | PrintSkia.cpp KeymapSDL    |
+---------------------------+----------------------------+
```

### 2.2 New files

```
platform/
  platform.h, Types.h, text.h, Keymap.h, Window.h, Event.h,
  Painter.h, TextEngine.h, Image.h, Clipboard.h, Dialogs.h,
  FileSystem.h, HttpClient.h, Process.h, Print.h
  windows/   (backends that wrap the existing D2D/Win32 code)
  linux/
    Main.cpp          — SDL event loop, dispatch, GPU warm-up swap
    WindowSDL.cpp     — window + surface + DIP scale + timers
    PainterSkia.cpp   — SkCanvas implementation of Painter
    TextEngineSkia.cpp — formats, SkParagraph layouts, fallback
    ImageSkia.cpp     — SkCodec decode, SkSVGDOM raster, GPU upload
    ClipboardSDL.cpp  — text / text/html / image/png
    DialogsPortal.cpp — XDG portal file chooser + zenity fallback
    FileSystemPosix.cpp — paths, stat, inotify watcher, exe dir
    HttpCurl.cpp      — GET to buffer/temp file (update check, images)
    ProcessPosix.cpp  — posix_spawn (pandoc, plantuml jar, xdg-open)
    PrintSkia.cpp     — SkPDFDocument page replay
    KeymapSDL.cpp     — SDL scancode/keycode <-> Win32-VK tables
scripts/linux/build-skia.sh — pinned Skia GN build -> libskia.a
scripts/linux/install-deps-ubuntu.sh
tests/fixtures/linux-port-*.md  — port-validation fixtures
tests/platform_tests.cpp — platform-layer unit tests (headless)
tests/sdl_tests.cpp      — injected SDL-event input tests (keystroke.h)
```

### 2.3 Modified files (core, every one is a defined task below)

`include/app.h`, `include/types.h`, `include/keymap.h`, `include/input.h`, `include/touch.h`, `include/render.h`, `include/utils.h`, `include/inline_style.h`, `include/settings.h`, `include/file_utils.h`, `include/export.h`, `include/print.h`, `include/image_loader.h`, `include/plantuml.h`, `include/plantuml_queue.h`, `include/d2d_init.h`, `CMakeLists.txt`, `src/utils.cpp`, `src/keymap.cpp`, `src/d2d_init.cpp`, `src/render.cpp`, `src/overlays.cpp`, `src/editor.cpp`, `src/tabs.cpp`, `src/themes.cpp`, `src/inline_style.cpp`, `src/input.cpp`, `src/touch.cpp`, `src/image_loader.cpp`, `src/file_utils.cpp`, `src/config_dir.cpp`, `src/print.cpp`, `src/export_docx.cpp`, `src/pandoc.cpp`, `src/plantuml.cpp`, `src/plantuml_queue.cpp`, `src/plantuml_app.cpp`, `src/i18n.cpp`, `src/settings.cpp`, `.github/workflows/build.yml`.

---

## Phase 0 — Linux toolchain, core library, and headless smoke test

**Deliverable:** `tinta_core` static library and a `tinta-linux-smoke` executable compile on Linux; a fixture Markdown file parses to a correct element tree; CTest runs the smoke test and the pure-logic test targets on Linux. The Windows build is untouched.

### Task 0.1: UTF conversion helpers (`platform/text.h`)

**Files:**
- Create: `platform/text.h`
- Create: `tests/platform_text_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `std::wstring toWide(const std::string& utf8)` (replaces `src/utils.cpp:13`), `std::string fromWide(const std::wstring&)`, `std::u16string wideToUtf16(const std::wstring&)`, `std::wstring utf16ToWide(const std::u16string&)`. On Windows the first two delegate to `MultiByteToWideChar`/`WideCharToMultiByte` so behavior is byte-for-byte unchanged.

- [ ] **Step 1: Write the failing test**

`tests/platform_text_tests.cpp`:

```cpp
#include "platform/text.h"
#include <cassert>

int main() {
    // CJK + emoji + 4-byte UTF-8: round-trips both ways
    const char* utf8 = "\xF0\x9F\x9A\x80 Rocket \xE4\xB8\xAD\xE6\x96\x87";
    std::wstring w = toWide(utf8);
    assert(fromWide(w) == utf8);
    std::u16string u16 = wideToUtf16(w);
    assert(utf16ToWide(u16) == w);
    // empty strings
    assert(toWide("").empty() && fromWide(L"").empty());
    return 0;
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
cmake -S . -B build-linux -DTINTA_LINUX_SMOKE=ON && cmake --build build-linux --target platform_text_tests
./build-linux/platform_text_tests
```
Expected: build fails — `platform/text.h` does not exist.

- [ ] **Step 3: Write the implementation**

```cpp
// platform/text.h
#pragma once
#include <string>
#include <string_view>

#ifdef _WIN32
#include <windows.h>

inline std::wstring toWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out((size_t)n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n);
    return out;
}
inline std::string fromWide(const std::wstring& s) {
    if (s.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}
inline std::u16string wideToUtf16(const std::wstring& s) {
    return {reinterpret_cast<const char16_t*>(s.data()), s.size()};
}
inline std::wstring utf16ToWide(const std::u16string& s) {
    return {reinterpret_cast<const wchar_t*>(s.data()), s.size()};
}
#else
// wchar_t is UTF-32 on Linux
#include <cstdint>
#include <vector>

inline std::wstring toWide(const std::string& s) {
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        uint32_t cp = 0;
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { cp = c; i += 1; }
        else if ((c >> 5) == 0x6) { cp = c & 0x1F; i += 1;
            cp = (cp << 6) | ((unsigned char)s[i] & 0x3F); i += 1; }
        else if ((c >> 4) == 0xE) { cp = c & 0x0F; i += 1;
            cp = (cp << 6) | ((unsigned char)s[i] & 0x3F); i += 1;
            cp = (cp << 6) | ((unsigned char)s[i] & 0x3F); i += 1; }
        else { cp = c & 0x07; i += 1;
            for (int k = 0; k < 3; ++k) {
                cp = (cp << 6) | ((unsigned char)s[i] & 0x3F); i += 1; } }
        out.push_back((wchar_t)cp);
    }
    return out;
}
inline std::string fromWide(const std::wstring& s) {
    std::string out;
    for (wchar_t cp : s) {
        if (cp < 0x80) out.push_back((char)cp);
        else if (cp < 0x800) {
            out.push_back((char)(0xC0 | (cp >> 6)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back((char)(0xE0 | (cp >> 12)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            out.push_back((char)(0xF0 | (cp >> 18)));
            out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}
inline std::u16string wideToUtf16(const std::wstring& s) {
    std::u16string out;
    for (wchar_t cp : s) {
        if (cp < 0x10000) out.push_back((char16_t)cp);
        else {
            uint32_t v = (uint32_t)cp - 0x10000;
            out.push_back((char16_t)(0xD800 + (v >> 10)));
            out.push_back((char16_t)(0xDC00 + (v & 0x3FF)));
        }
    }
    return out;
}
inline std::wstring utf16ToWide(const std::u16string& s) {
    std::wstring out;
    for (size_t i = 0; i < s.size(); ++i) {
        char16_t c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size() &&
            s[i+1] >= 0xDC00 && s[i+1] <= 0xDFFF) {
            out.push_back((wchar_t)(0x10000 + ((c - 0xD800) << 10) + (s[i+1] - 0xDC00)));
            ++i;
        } else out.push_back((wchar_t)c);
    }
    return out;
}
#endif
```

- [ ] **Step 4: Wire into CMake and run the test**

In `CMakeLists.txt`, after the existing test block:

```cmake
if(UNIX AND NOT APPLE)
    set(TINTA_PLATFORM_LINUX 1 CACHE INTERNAL "")
    add_compile_definitions(TINTA_PLATFORM_LINUX=1)
    add_executable(platform_text_tests tests/platform_text_tests.cpp)
    target_include_directories(platform_text_tests PRIVATE platform include)
    add_test(NAME platform_text COMMAND platform_text_tests)
endif()
```

Run the build + test again; expected: PASS.

- [ ] **Step 5: Replace the old `toWide` and add `fromWide`**

Modify `src/utils.cpp` to `#include "platform/text.h"` and delete the local `toWide` (line 13); add `std::string fromWide(const std::wstring&)` to `include/utils.h`. The rest of `utils.cpp` keeps calling `toWide`. On Windows the behavior is identical (the old implementation is the new one). Verify Windows still builds and `ctest` passes; commit.

```bash
git add platform/text.h tests/platform_text_tests.cpp src/utils.cpp include/utils.h CMakeLists.txt
git commit -m "port: UTF conversions in platform/text.h, used by utils.cpp"
```

### Task 0.2: Scrub the small Windows usages out of portable sources

**Files:**
- Modify: `src/frontmatter.cpp:7`, `src/i18n.cpp:5,2092,2231`, `src/plantuml.cpp:343`, `src/settings.cpp:5`

**Interfaces:**
- Consumes: `platform/text.h` (`toWide`, `fromWide`)
- Produces: `platform/FileSystem.h` with `bool fileMTime(const std::wstring& path, int64_t& mtimeNs)` (Phase 0 provides only this function; the full header arrives in Task 5.3).

- [ ] **Step 1: Port `i18n.cpp` conversions**

`src/i18n.cpp:2092` and `:2231` call `MultiByteToWideChar`/`WideCharToMultiByte`. Replace with `toWide`/`fromWide` from `platform/text.h`. These are the language-file loaders (`languages.ini`) — the same bytes flow through, so translations are unchanged.

- [ ] **Step 2: Port `plantuml.cpp` file-time query**

`src/plantuml.cpp:343` uses `WIN32_FILE_ATTRIBUTE_DATA` to decide whether a cached PlantUML PNG is stale. Replace with:

```cpp
// platform/FileSystem.h (Phase 0 subset)
#pragma once
#include <string>
#include <cstdint>

bool fileMTime(const std::wstring& path, int64_t& mtimeNs); // false if missing
```

On Windows this wraps `GetFileAttributesExW`; on Linux:

```cpp
// platform/linux/FileSystemPosix.cpp
#include "platform/FileSystem.h"
#include "platform/text.h"
#include <sys/stat.h>
#include <ctime>

bool fileMTime(const std::wstring& path, int64_t& mtimeNs) {
    struct stat st;
    std::string p = fromWide(path);
    if (stat(p.c_str(), &st) != 0) return false;
    mtimeNs = (int64_t)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec;
    return true;
}
```

Adjust the call site to `int64_t mtime; fileMTime(path, mtime);` with the same comparisons.

- [ ] **Step 3: Port `frontmatter.cpp` and `settings.cpp` includes**

Both include `<windows.h>` for no required Windows call on Linux (verify by reading; if a call is found, route it through the matching platform helper). Guard the include:

```cpp
#ifdef _WIN32
#include <windows.h>
#endif
```

- [ ] **Step 4: Build Linux core library**

Create the CMake core library and smoke target:

```cmake
# after the md4c FetchContent block, guarded by TINTA_PLATFORM_LINUX:
add_library(tinta_core STATIC
    src/markdown.cpp src/footnotes.cpp src/document.cpp src/frontmatter.cpp
    src/syntax.cpp src/syntax_extended.cpp src/math_parser.cpp src/math_macros.cpp
    src/mermaid.cpp src/mermaid_ext.cpp)
target_include_directories(tinta_core PUBLIC include ${md4c_SOURCE_DIR}/src platform)
target_compile_definitions(tinta_core PUBLIC TINTA_PLATFORM_LINUX=1)
target_link_libraries(tinta_core PUBLIC md4c)
```

Expected: `tinta_core` compiles on Linux (files verified Windows-free in the audit: `markdown.cpp`, `footnotes.cpp`, `document.cpp`, `syntax.cpp`, `syntax_extended.cpp`, `math_parser.cpp`, `math_macros.cpp`, `mermaid_ext.cpp`, `mermaid.cpp`).

- [ ] **Step 5: Smoke test binary + fixture**

Create `tests/fixtures/linux-port-core-smoke.md` mixing the parsing features (headings, a table, a nested list, emphasis, a code fence, a link, a block quote, math `$x^2$`, and a mermaid flowchart). Create `tests/linux_smoke.cpp` that runs `parseDocument` (see `include/document.h`) over the fixture and asserts: document has ≥1 heading; the table block exists; a list item nests; the code fence and quote blocks exist; the mermaid diagram is detected. Register with CTest.

- [ ] **Step 6: Verify Linux green, Windows untouched, commit**

```bash
cmake -S . -B build-linux -DTINTA_LINUX_SMOKE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-linux --parallel
ctest --test-dir build-linux --output-on-failure
```
Expected: `tinta-linux-smoke` and `platform_text_tests` pass. Windows build not yet affected. Commit.

---

## Phase 1 — Platform layer + first visible window

**Deliverable:** an SDL3 window that paints a themed background and placeholder page with Skia, handles resize/DPI/timers, receives events through the `platform/` abstractions, and renders a raster screenshot for tests. Windows remains green.

### Task 1.1: Skia build script and SDL3 dependency

**Files:**
- Create: `scripts/linux/build-skia.sh`
- Create: `scripts/linux/install-deps-ubuntu.sh`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `libskia.a` + `include/skia/` at `third_party/skia-build/`; CMake variables `TINTA_SKIA_DIR`, `SDL3_FOUND`.

- [ ] **Step 1: Pin and build Skia**

```bash
# scripts/linux/build-skia.sh
set -euo pipefail
SKIA_REV=chromium/128   # pin at review time; record exact SHA in the script
SKIA_DIR=third_party/skia
mkdir -p "$SKIA_DIR"
git clone --depth 1 --branch "$SKIA_REV" https://skia.googlesource.com/skia "$SKIA_DIR"
python3 "$SKIA_DIR/tools/git-sync-deps"
cat > "$SKIA_DIR/out/linux/args.gn" <<'GNEOF'
is_official_build=true
is_debug=false
is_component_build=false
target_cpu="x64"
skia_use_system_freetype2=true
skia_use_system_libpng=true
skia_use_system_libjpeg_turbo=true
skia_use_system_zlib=true
skia_use_system_harfbuzz=true
skia_use_system_icu=true
skia_use_system_expat=true
skia_enable_sksl_tracing=false
skia_use_system_fontconfig=true
GNEOF
bin/gn gen out/linux --args-file=out/linux/args.gn
ninja -C out/linux skia
mkdir -p third_party/skia-build/lib
cp out/linux/libskia.a third_party/skia-build/lib/
mkdir -p third_party/skia-build/include && cp -r include third_party/skia-build/
```

- [ ] **Step 2: Ubuntu dependency script**

`scripts/linux/install-deps-ubuntu.sh`:

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build pkg-config \
    libsdl3-dev libcurl4-openssl-dev libfontconfig1-dev \
    libfreetype-dev libpng-dev libjpeg-dev libwebp-dev zlib1g-dev \
    libharfbuzz-dev libicu-dev libexpat1-dev libgl1-mesa-dev \
    python3 python3-pip
```

- [ ] **Step 3: CMake integration**

```cmake
if(TINTA_PLATFORM_LINUX)
    find_package(PkgConfig REQUIRED)
    find_package(CURL REQUIRED)
    find_package(fontconfig REQUIRED)
    # SDL3: system package first, FetchContent fallback
    find_package(SDL3 QUIET)
    if(NOT SDL3_FOUND)
        FetchContent_Declare(sdl3
            GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
            GIT_TAG release-3.2.0  GIT_SHALLOW TRUE)
        set(SDL_SHARED FALSE CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(sdl3)
    endif()
    if(NOT DEFINED TINTA_SKIA_DIR)
        set(TINTA_SKIA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/skia-build")
    endif()
    add_library(skia STATIC IMPORTED GLOBAL)
    set_target_properties(skia PROPERTIES
        IMPORTED_LOCATION "${TINTA_SKIA_DIR}/lib/libskia.a"
        INTERFACE_INCLUDE_DIRECTORIES "${TINTA_SKIA_DIR}/include")
endif()
```

- [ ] **Step 4: Verify**

```bash
./scripts/linux/install-deps-ubuntu.sh
./scripts/linux/build-skia.sh   # ~20-40 min first time
cmake -S . -B build-linux -DTINTA_PLATFORM_LINUX=1 -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux --parallel
```
Expected: configuration finds SDL3, CURL, fontconfig, skia. Commit.

### Task 1.2: `Painter` interface + Skia implementation

**Files:**
- Create: `platform/Painter.h`
- Create: `platform/linux/PainterSkia.cpp`
- Create: `tests/platform_painter_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces (used by later tasks and by `render.cpp`/`overlays.cpp`/`editor.cpp` ports):

```cpp
// platform/Types.h — shared value types for the platform namespace
#pragma once
#include "types.h"   // qmd::Color

namespace platform {
// float RGBA color — same {r,g,b,a} layout as qmd::Color and D2D1_COLOR_F
using Color = qmd::Color;
// A transform for zoom: translate+uniform scale, matching D2D Matrix3x2F use
struct Transform { float tx = 0.f, ty = 0.f, scale = 1.f; };
} // namespace platform
```

```cpp
// platform/Painter.h
#pragma once
#include "types.h"      // qmd::Color
#include "platform/Types.h"
#include <memory>

namespace platform {

struct RectF { float x, y, w, h; };

class Path {
public:
    void moveTo(float x, float y);
    void lineTo(float x, float y);
    void quadTo(float cx, float cy, float x, float y);
    void close();
    // mermaid uses SVG path strings; a helper parses them into the path
    static Path fromSvgPathData(const std::string& d);
    struct Impl; std::shared_ptr<Impl> impl;
};

using ImageRef = std::shared_ptr<class Image>;

class Painter {
public:
    virtual ~Painter() = default;
    virtual void clear(const qmd::Color& c) = 0;
    virtual void fillRect(const qmd::Color& c, const RectF& r) = 0;
    virtual void fillRoundedRect(const qmd::Color& c, const RectF& r, float radius) = 0;
    virtual void strokeRect(const qmd::Color& c, const RectF& r, float thickness) = 0;
    virtual void drawLine(const qmd::Color& c, float thickness, float x1, float y1, float x2, float y2) = 0;
    virtual void fillEllipse(const qmd::Color& c, const RectF& r) = 0;
    virtual void drawEllipse(const qmd::Color& c, const RectF& r, float thickness) = 0;
    virtual void fillPath(const qmd::Color& c, const Path& p) = 0;
    virtual void drawPath(const qmd::Color& c, const Path& p, float thickness) = 0;
    virtual void drawImage(const ImageRef& img, const RectF& dst, const RectF& src, float opacity) = 0;
    virtual void clipRect(const RectF& r) = 0;
    virtual void save() = 0;
    virtual void restore() = 0;
    virtual void translate(float dx, float dy) = 0;
    virtual void scale(float s) = 0;
    // text draws through the TextEngine (Task 1.3); Painter exposes only a hook
    virtual void drawTextLayout(void* layout, float x, float y) = 0;
};
} // namespace platform
```

Implementation notes for `PainterSkia.cpp` (complete skeleton):

```cpp
#include "platform/Painter.h"
#include "platform/linux/SkiaContext.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRRect.h"
#include "include/core/SkPath.h"

namespace platform {

struct Path::Impl { SkPath sk; };
void Path::moveTo(float x, float y){ impl->sk.moveTo(x,y); }
void Path::lineTo(float x, float y){ impl->sk.lineTo(x,y); }
void Path::quadTo(float cx, float cy, float x, float y){ impl->sk.quadTo(cx,cy,x,y); }
void Path::close(){ impl->sk.close(); }
Path Path::fromSvgPathData(const std::string& d){ /* SkPath::Builder::parseSvgPathData */ }

class PainterSkia final : public Painter {
    SkCanvas* cv;
    SkColor toSk(const qmd::Color& c){ return SkColorSetARGB((U8CPU)(c.a*255), (U8CPU)(c.r*255), (U8CPU)(c.g*255), (U8CPU)(c.b*255)); }
public:
    explicit PainterSkia(SkCanvas* canvas) : cv(canvas) {}
    void clear(const qmd::Color& c) override { cv->clear(toSk(c)); }
    void fillRect(const qmd::Color& c, const RectF& r) override {
        SkPaint p; p.setColor(toSk(c)); p.setStyle(SkPaint::kFill_Style);
        cv->drawRect(SkRect::MakeXYWH(r.x, r.y, r.w, r.h), p);
    }
    // fillRoundedRect via SkRRect, strokeRect/drawLine via SkPaint style,
    // ellipses via SkOval, paths via cv->drawPath, images via drawImageRect,
    // clipRect via cv->clipRect, save/restore/translate/scale map 1:1 to SkCanvas
    void drawTextLayout(void* layout, float x, float y) override; // forwards to TextEngineSkia
};

std::shared_ptr<Painter> makePainter(SkCanvas* canvas); // factory in SkiaContext.h
}
```

- [ ] **Step 1: Write the failing test** (draw primitives into an offscreen raster surface, assert pixel colors):

```cpp
// tests/platform_painter_tests.cpp
#include "platform/Painter.h"
#include "platform/linux/SkiaContext.h"
#include "include/core/SkSurface.h"
#include "include/core/SkImage.h"
#include <cassert>

int main() {
    auto surf = SkSurface::MakeRasterN32Premul(64, 64);
    auto canvas = surf->getCanvas();
    auto painter = platform::makePainter(canvas);
    painter->clear(qmd::Color::white());
    painter->fillRect(qmd::Color(1,0,0,1), {8,8,48,48});
    painter->save(); painter->translate(4,4); painter->scale(2);
    painter->fillRect(qmd::Color(0,1,0,1), {0,0,4,4}); // lands at (4,4)-(12,12)
    painter->restore();
    SkPixmap pm; surf->peekPixels(&pm);
    auto px = [&](int x, int y) -> uint32_t { return *pm.addr32(x,y); };
    assert(px(32,32) == 0xFFFF0000);          // center of big red rect
    assert(px(8,8) == 0xFF00FF00);            // translated green
    assert(px(0,0) == 0xFFFFFFFF);            // clear stayed white
    return 0;
}
```

- [ ] **Step 2: Run to verify it fails** (build target `platform_painter_tests`; fails to compile because `platform/Painter.h` missing).

- [ ] **Step 3: Implement** the header + `PainterSkia.cpp` per the skeleton above, plus `platform/linux/SkiaContext.h` exposing `makePainter(SkCanvas*)` and (later) the surface/frame logic.

- [ ] **Step 4: Run test to verify it passes.**

- [ ] **Step 5: Commit.**

### Task 1.3: `TextEngine` interface + SkParagraph implementation

**Files:**
- Create: `platform/TextEngine.h`
- Create: `platform/linux/TextEngineSkia.cpp`
- Create: `tests/platform_textengine_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces:

```cpp
// platform/TextEngine.h
#pragma once
#include "platform/Painter.h"
#include "platform/Types.h"
#include <string>
#include <vector>
#include <memory>
#include <optional>

namespace platform {

struct FontSpec {
    std::string family;        // "Segoe UI", "Consolas", theme family, ...
    float sizePx = 14.0f;
    int weight = 400;          // 100..900, Skia maps to SkFontStyle
    bool italic = false;
};

class TextLayout {
public:
    virtual ~TextLayout() = default;
    virtual float width() const = 0;          // DIPs, incl trailing whitespace
    virtual float height() const = 0;
    virtual void draw(Painter& painter, float x, float y) = 0;
};

// Per-range style applied on top of a base format (mirrors DWrite SetFont*)
struct RunStyle {
    int32_t start = -1, end = -1;             // -1 = whole layout
    std::optional<FontSpec> font;             // weight/italic/size/family
    bool underline = false, strikethrough = false;
    qmd::Color color;
    bool hasColor = false;
};

class TextEngine {
public:
    virtual ~TextEngine() = default;
    // Base format == DWrite CreateTextFormat
    virtual std::shared_ptr<const FontSpec> format(const FontSpec& base) = 0;
    virtual std::unique_ptr<TextLayout> layout(
        const std::wstring& text, const FontSpec& base,
        float maxWidth, float lineHeight, const std::vector<RunStyle>& runs) = 0;
    // Font selection parity with installedFace()/documentCjkFamily()
    virtual std::string installedFace(const std::string& family, bool mono) = 0;
    virtual std::string cjkFamilyFor(const std::string& family) = 0;
    virtual void enumerateFamilies(std::vector<std::string>& out) = 0;
};

std::shared_ptr<TextEngine> makeTextEngine();
} // namespace platform
```

Linux implementation (core, complete):

```cpp
// platform/linux/TextEngineSkia.cpp
#include "platform/TextEngine.h"
#include "platform/text.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontStyle.h"
#include "modules/skparagraph/include/ParagraphBuilder.h"
#include "modules/skparagraph/include/Paragraph.h"
#include "modules/skparagraph/include/TypefaceFontProvider.h"
#include <fontconfig/fontconfig.h>
#include <memory>

namespace platform {
namespace {
SkFontStyle toSkStyle(int weight, bool italic) {
    return SkFontStyle(weight, SkFontStyle::kNormal_Width,
                       italic ? SkFontStyle::kItalic_Slant : SkFontStyle::kUpright_Slant);
}
} // namespace

class TextLayoutSkia final : public TextLayout {
    std::shared_ptr<skia::textlayout::Paragraph> p;
public:
    explicit TextLayoutSkia(std::shared_ptr<skia::textlayout::Paragraph> para) : p(para) {}
    float width() const override  { return p->getMaxWidth(); }
    float height() const override { return p->getHeight(); }
    void draw(Painter& painter, float x, float y) override {
        p->paint(skiaContextCanvas(painter), x, y);
    }
};

class TextEngineSkia final : public TextEngine {
    std::shared_ptr<skia::textlayout::FontCollection> fonts;
    std::shared_ptr<SkFontMgr> mgr = SkFontMgr::RefDefault();
public:
    TextEngineSkia() {
        fonts = std::make_shared<skia::textlayout::FontCollection>();
        fonts->setDefaultFontManager(mgr);   // fontconfig fallback, emoji, CJK
    }
    std::shared_ptr<const FontSpec> format(const FontSpec& base) override {
        return std::make_shared<FontSpec>(base);
    }
    std::unique_ptr<TextLayout> layout(const std::wstring& text, const FontSpec& base,
        float maxWidth, float lineHeight, const std::vector<RunStyle>& runs) override {
        auto builder = skia::textlayout::ParagraphBuilder::make(
            skia::textlayout::ParagraphStyle{}, fonts);
        builder->pushStyle(makeStyle(base));
        builder->addText(wideToUtf16(text));
        auto para = builder->Build();
        para->layout(maxWidth);
        return std::make_unique<TextLayoutSkia>(para);
    }
    // installedFace: ask fontconfig for the nearest family; if missing, for
    // mono try "monospace" else "sans-serif"
    std::string installedFace(const std::string& family, bool mono) override {
        FcPattern* pat = FcNameParse((FcChar8*)(mono ? "monospace" : "sans-serif").c_str());
        FcConfigSubstitute(nullptr, pat, FcMatchPattern); FcDefaultSubstitute(pat);
        FcResult res; FcPattern* m = FcFontMatch(nullptr, pat, &res);
        std::string out; FcChar8* name = nullptr;
        if (m && FcPatternGetString(m, FC_FAMILY, 0, &name) == FcResultMatch) out = (const char*)name;
        if (m) FcPatternDestroy(m); FcPatternDestroy(pat);
        return out.empty() ? family : out;
    }
    // cjkFamilyFor / enumerateFamilies via FcFontSort + SkFontMgr::getFamilies
};
}
```

Test `tests/platform_textengine_tests.cpp`:

```cpp
int main() {
    auto te = platform::makeTextEngine();
    platform::FontSpec base{"sans-serif", 16, 400, false};
    auto lay = te->layout(L"Hello World", base, 1000.f, 20.f, {});
    assert(lay->width() > 50 && lay->width() < 200);
    assert(lay->height() > 10);
    // CJK + emoji must measure (fallback works)
    auto cjk = te->layout(L"\xE4\xB8\xAD\xE6\x96\x87\xF0\x9F\x9A\x80", base, 1000.f, 20.f, {});
    assert(cjk->height() > 10);
    // installedFace resolves a real family
    assert(!te->installedFace("Segoe UI", false).empty());
    return 0;
}
```

(Steps 1–5 follow the same write-failing-test → run → implement → pass → commit cycle as Task 1.2.)

### Task 1.4: `Window` + SDL3 event loop skeleton

**Files:**
- Create: `platform/Window.h`, `platform/Event.h`
- Create: `platform/linux/WindowSDL.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces (used by `platform/linux/Main.cpp` and every later task):

```cpp
// platform/Event.h
#pragma once
#include <cstdint>
#include <string>

namespace platform {
struct Event {
    enum class Type { None, KeyDown, KeyUp, Char, ImeComposition, ImeCommit,
                      MouseMove, MouseDown, MouseUp, MouseWheel, MouseHWheel,
                      TouchDown, TouchMove, TouchUp, TouchCancel, DropFile,
                      App /* custom, see id */, Quit, Resize, DpiChanged };
    Type type = Type::None;
    int x = 0, y = 0;             // DIPs
    int dx = 0, dy = 0;           // wheel deltas (120 = one notch)
    unsigned key = 0;             // Win32 virtual key code (see Keymap.h)
    bool ctrl=false, shift=false, alt=false;
    std::wstring text;            // Char / ImeCommit payload
    std::wstring preedit; int preeditCursor=0; // ImeComposition
    uint32_t id = 0;              // App events (WM_APP_* equivalents)
    void* data = nullptr;         // ownership-transferred payloads
};
}

// platform/Window.h
#pragma once
#include "platform/Event.h"
#include <functional>
#include <memory>

namespace platform {
struct WindowDesc {
    std::string title; int width=1024, height=768; bool maximized=false;
};
class Window {
public:
    virtual ~Window() = default;
    virtual void show() = 0;
    virtual void setTitle(const std::wstring&) = 0;
    virtual void setSize(int w, int h) = 0;
    virtual int width() const = 0;   // DIPs
    virtual int height() const = 0;
    virtual float deviceScale() const = 0; // 1,2,3...
    virtual bool pollEvent(Event& out) = 0;
    virtual void postEvent(const Event& e) = 0;   // replaces PostMessage
    virtual void requestPaint() = 0;
    // timers: id + intervalMs; fires Event{Type::App, id}
    virtual int setTimer(int id, int intervalMs) = 0;
    virtual void killTimer(int id) = 0;
    virtual void setTextInputArea(int x, int y, int w, int h) = 0; // IME caret
    virtual void startTextInput() = 0;
    virtual void stopTextInput() = 0;
    virtual void* nativeHandle() = 0; // SDL_Window*
};
std::unique_ptr<Window> createWindow(const WindowDesc& desc);
}
```

- [ ] **Step 1: Write the failing test** — a `tests/window_sdl_tests.cpp` that creates a window, posts a custom App event, polls it back, and quits; plus a 1-frame paint to a raster surface.

- [ ] **Step 2: Implement `WindowSDL`**

Key mappings (document in `WindowSDL.cpp`):
- `SDL_WINDOWEVENT_RESIZED` → `Event::Resize` with DIP size = pixels ÷ `deviceScale()` (from `SDL_GetWindowDisplayScale`, round to nearest int).
- `SDL_EVENT_MOUSE_BUTTON_DOWN/UP/MOVE/WHEEL` → Mouse* with DIP coordinates (divide by scale).
- `SDL_EVENT_KEY_DOWN/UP` → `KeyDown/KeyUp`; translate `event.key.key` (SDL keycode) to a Win32 VK value via the table in Task 1.5, so `keymap.cpp`/`input.cpp` logic is untouched. Carry ctrl/shift/alt from `event.key.mod`.
- `SDL_EVENT_TEXT_INPUT` → `Event::Char` (text).
- `SDL_EVENT_TEXT_EDITING` → `Event::ImeComposition` (preedit, cursor).
- `SDL_EVENT_DROP_FILE` → `Event::DropFile`.
- `SDL_EVENT_FINGER_DOWN/MOTION/UP` → Touch* with pixel coordinates = (finger.x * windowPixelWidth).
- Custom events (image-ready, folder-search, pandoc-done, layout-chunk, gpu-ready, update-check) are `SDL_RegisterEvents(8)`; `postEvent` becomes `SDL_PushEvent` with a matching custom SDL event, keeping the existing worker-thread ownership-transfer contract intact.
- Timers: an internal min-heap on the event loop; `Event::App` with the timer id is emitted when a timer expires. `WM_APP_*` numeric ids stay the same (`app.h` constants) so `input.cpp`/`render.cpp` dispatch code ports without renumbering.

- [ ] **Step 3: Verify** the test passes (window opens on X11 and Wayland if available; headless CI uses `SDL_VIDEODRIVER=offscreen` with a raster canvas).

### Task 1.5: Keycode translation tables

**Files:**
- Create: `platform/Keymap.h`, `platform/linux/KeymapSDL.cpp`
- Modify: `include/keymap.h` (include `platform/Keymap.h` instead of `<windows.h>` on Linux)

**Interfaces:**
- Produces: `unsigned sdlToVk(SDL_Keycode, SDL_Keymod)` and `unsigned vkToSdl(unsigned vk)`; a constexpr table of the VK constants the code uses (`VK_TAB`, `VK_F1`, `VK_F11`, `VK_RETURN`, `VK_BACK`, `VK_ESCAPE`, `VK_SPACE`, `VK_NUMPAD0..9`, `VK_OEM_*`, `VK_CONTROL/SHIFT/MENU`, arrows, `VK_HOME/END/PRIOR/NEXT/DELETE`, `VK_XBUTTON1/2`).

- [ ] **Step 1:** On Linux define the missing VK constants in `platform/Keymap.h` with their Windows numeric values, so `include/keymap.h`, `tests/keystroke.h` (which already computes `usLayoutCharacter`), and `src/keymap.cpp` compile unchanged:

```cpp
// platform/Keymap.h (Linux)
#ifndef VK_TAB
#define VK_TAB 0x09
#define VK_RETURN 0x0D
#define VK_BACK 0x08
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_PRIOR 0x21
#define VK_NEXT 0x22
#define VK_HOME 0x24
#define VK_END 0x23
#define VK_DELETE 0x2E
#define VK_INSERT 0x2D
#define VK_NUMPAD0 0x60 /* ... through VK_NUMPAD9 0x69 ... */
#define VK_F1 0x70 /* ... through VK_F24 0x87 ... */
#define VK_XBUTTON1 0x05
#define VK_XBUTTON2 0x06
#define VK_OEM_1 0xBA /* ;: */
#define VK_OEM_PLUS 0xBB
#define VK_OEM_COMMA 0xBC
#define VK_OEM_MINUS 0xBD
#define VK_OEM_PERIOD 0xBE
#define VK_OEM_2 0xBF
#define VK_OEM_3 0xC0
#define VK_OEM_4 0xDB
#define VK_OEM_5 0xDC
#define VK_OEM_6 0xDD
#define VK_OEM_7 0xDE
#define VK_CONTROL 0x11
#define VK_SHIFT 0x10
#define VK_MENU 0x12
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#endif
```

- [ ] **Step 2:** `KeymapSDL.cpp` maps SDL keycodes to VK. Table-driven (complete table in the file), e.g.: `SDLK_a..SDLK_z -> VK_A..VK_Z` (uppercase), `SDLK_0..SDLK_9 -> VK_0..VK_9`, `SDLK_TAB->VK_TAB`, `SDLK_RETURN->VK_RETURN`, `SDLK_BACKSPACE->VK_BACK`, `SDLK_ESCAPE->VK_ESCAPE`, `SDLK_LEFT/RIGHT/UP/DOWN->VK_LEFT/RIGHT/UP/DOWN`, `SDLK_F1..F24->VK_F1..`, `SDLK_KP_0..9->VK_NUMPAD0..`, SDL OEM keys (`SDLK_SEMICOLON`, `SDLK_MINUS`, `SDLK_COMMA`, `SDLK_PERIOD`, `SDLK_SLASH`, `SDLK_BACKQUOTE`, `SDLK_LEFTBRACKET`, `SDLK_BACKSLASH`, `SDLK_RIGHTBRACKET`, `SDLK_QUOTE`) → the `VK_OEM_*` values above.

- [ ] **Step 3: Test** — `tests/keymap_tests.cpp` already exists; on Linux extend it with: for every SDL keycode in the table, `sdlToVk` returns the expected VK; `vkToSdl` inverts; and the round trip is stable. Run on Linux + Windows (table is guarded, Windows uses the real headers).

### Task 1.6: First visible window (themes + raster screenshot)

**Files:**
- Create: `platform/linux/Main.cpp`
- Create: `tests/linux_window_screenshot.cpp`
- Modify: `CMakeLists.txt`, `include/app.h` (start of the D2D-type migration)

**Interfaces:**
- Consumes: `Window`, `Painter`, `TextEngine`, `THEMES` (`src/themes.cpp`)
- Produces: `int main()` for the Linux app; the paint path that later renders a document's first viewport.

- [ ] **Step 1: Theme color migration**

Modify `include/app.h:100` `struct D2DTheme`: change the `D2D1_COLOR_F` fields to `qmd::Color`, and add at the top of `app.h`:

```cpp
#if defined(TINTA_PLATFORM_LINUX)
#include "types.h"
using D2D1_COLOR_F = qmd::Color;  // identical {r,g,b,a} layout; transition alias
#else
#include <d2d1.h>                 // unchanged Windows path
#endif
```

`src/themes.cpp` compiles unchanged because `hexColor()` returns `D2D1_COLOR_F` and the fields now hold `qmd::Color` (same layout; add `static_assert(sizeof(qmd::Color)==sizeof(D2D1_COLOR_F))` in `app.h`).

- [ ] **Step 2: Minimal Linux main**

```cpp
// platform/linux/Main.cpp
#include "platform/Window.h"
#include "platform/Painter.h"
#include "platform/TextEngine.h"
#include "app.h"
#include <memory>

int main(int argc, char** argv) {
    App app;                       // platform-neutral state
    platform::WindowDesc desc;
    desc.title = "Tinta";
    auto window = platform::createWindow(desc);
    auto painter = platform::makePainter(window);   // wraps the frame canvas
    auto text = platform::makeTextEngine();

    bool running = true;
    while (running) {
        platform::Event ev;
        while (window->pollEvent(ev)) {
            if (ev.type == platform::Event::Type::Quit) running = false;
            if (ev.type == platform::Event::Type::Resize) window->requestPaint();
        }
        if (window->needsPaint()) {
            painter->clear(app.theme.background);
            auto lay = text->layout(L"Tinta on Linux", {"sans-serif", 24, 700, false},
                                    1000.f, 32.f, {});
            painter->save(); painter->translate((window->width()-lay->width())/2,
                                                (window->height()-lay->height())/2);
            painter->drawTextLayout(lay.get(), 0, 0);
            painter->restore();
            window->present();
        }
    }
    return 0;
}
```

- [ ] **Step 3: Screenshot test** — `tests/linux_window_screenshot.cpp` runs the same paint path into a raster surface and writes `build/output/linux-window.png` (an ignored build dir per AGENTS.md). Assert the PNG is non-trivial (e.g., ≥ 90% of center pixels are the theme background, and title-text pixels differ from background). This is the first automated visual regression hook.

- [ ] **Step 4: Manual check** — run `./build-linux/tinta` on X11 and Wayland; verify resize/close work, the DIP scale is applied, and startup is fast. Record in the phase validation `.md` (below).

- [ ] **Step 5: Validation fixture** — `tests/fixtures/linux-port-phase1.md` documenting what was checked (window open on X11/Wayland, resize, theme background paint, screenshot hook) per the AGENTS.md workflow. Commit.

---

## Phase 2 — Viewer core: a Markdown document renders

**Deliverable:** opening a `.md` file shows a correctly laid-out, themed, scrollable, zoomable page: headings, paragraphs, emphasis, links, lists, tables, block quotes, code blocks, inline code. GPU or raster. This is the first "usable viewer" milestone.

### Task 2.1: Port `d2d_init.cpp` into the Linux text engine

**Files:**
- Modify: `src/d2d_init.cpp` (split: D2D-specific init stays Windows-only)
- Create: `src/d2d_init_linux.cpp` (or fold into `platform/linux/TextEngineSkia.cpp`)

**Interfaces:**
- Consumes: `platform::TextEngine`
- Produces: same public functions as `include/d2d_init.h` on Linux: `updateTextFormats`, `applyTheme`, `enumerateSystemFontFamilies`, `installedFace`, `documentCjkFamily`, `useInstalledFaces`, `useUiFontFallback` (no-op where fallback is automatic).

- [ ] **Step 1: Port the format-creation block** (`src/d2d_init.cpp:426-570`, all `CreateTextFormat` calls). Each call becomes `textEngine->format(FontSpec{family, sizePx, weight, italic})`. The mapping table (put it in the code as a comment):

| DWrite call (d2d_init.cpp) | platform::TextEngine |
|---|---|
| `CreateTextFormat(family, nullptr, weight, italic, size, L"", nullptr)` | `format(FontSpec{family, sizePx, weight, italic})` |
| `SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP)` | layouts are made per-block with explicit `maxWidth`; no-wrap equivalent = pass `1e6` maxWidth |
| `SetParagraphAlignment(CENTER)` | ParagraphStyle alignment in `layout()` |
| `SetTextAlignment(CENTER)` | ParagraphStyle textAlign |
| `SetLineSpacing(DWRITE_LINE_SPACING_METHOD_PROPORTIONAL, 0, lineHeight)` | `layout(..., lineHeight)` — SkParagraph line-height |
| `SetTypography` | initially deferred; script typography (code ligatures) is a Phase 2 later item |

- [ ] **Step 2: Port `installedFace` / `documentCjkFamily`**

Replace the Windows `installedFace` logic with `textEngine->installedFace(family, mono)` and `textEngine->cjkFamilyFor(family)`. Keep the exact Windows behavior for the Windows backend (`#ifdef`).

- [ ] **Step 3: Test**

Add `tests/d2d_init_linux_tests.cpp`: after `updateTextFormats`, assert the document format, heading formats (all 6), code format, and bold/italic variants exist and carry the theme faces via `installedFace`; assert CJK family resolves to a real installed CJK face (test with `Noto Sans CJK SC` when installed, else fontconfig's CJK match). Register with CTest.

### Task 2.2: Port the layout half of `render.cpp` (text measurement)

**Files:**
- Modify: `src/render.cpp`
- Create: `tests/layout_linux_tests.cpp`

**Interfaces:**
- Consumes: `platform::TextEngine`
- Produces: `LayoutInfo` (same struct) now holds `std::shared_ptr<platform::TextLayout>` instead of `IDWriteTextLayout*`.

- [ ] **Step 1: Port `createLayout` (`src/render.cpp:39-60`)** to build a `platform::TextLayout`:

```cpp
static LayoutInfo createLayout(App& app, std::wstring_view text, const FontSpec& base,
                               float lineHeight) {
    LayoutInfo info;
    if (text.empty()) return info;
    auto tl = app.textEngine->layout(std::wstring(text), base, kHugeWidth, lineHeight, {});
    info.width = tl->width();
    info.height = tl->height();
    info.layout = std::move(tl);
    return info;
}
```

Then port every call site that applies per-range styles (bold/italic/code/sup/sub). DWrite used `layout->SetFontWeight(range)`, `SetFontStyle`, `SetFontSize`, `SetUnderline`, `SetStrikethrough`, `SetFontFamilyName`, plus baseline shifts. The SkParagraph equivalent is the `RunStyle` vector passed to `TextEngine::layout`. The porting rule:

- For each `StyledRun` (`inline_style.h`), accumulate a `RunStyle{start,end,font,underline,strikethrough,color}`; sup/sub also set `sizePx *= 0.65` and a `baselineShift` (SkParagraph supports `BaselineShift` in `TextStyle`; verify against the current sup/sub pixel offsets in `render.cpp`).

- [ ] **Step 2: Port `measureText` (`src/utils.cpp:20`)** to `app.textEngine->layout(text, fmt, 10000, 100, {})->width()`.

- [ ] **Step 3: Port the mermaid/math measurement callback.** `mermaid_ext.h` already injects text measurement ("text measurement is injected through a callback") — wire the callback to the Linux `TextEngine`. `math_render.cpp` uses `IDWriteTextLayout` heavily — port its layout calls the same way (it has 24 D2D/DWrite references; each is one of the same 6 patterns above).

- [ ] **Step 4: Regression tests (layout parity)** — `tests/layout_linux_tests.cpp` loads `tests/fixtures/linux-port-layout.md` (a document with headings, a table, a nested list, block quotes, code, emphasis, links, inline code, sup/sub, math), runs the full `layoutDocument`, and asserts layout invariants that DirectWrite also satisfies on Windows:
  - heading N y-position increases with N;
  - table cell widths sum ≈ table width;
  - a narrow window (reading width 60%) wraps a long paragraph into > 1 line;
  - a code block's width ≥ its longest line's measured width.
  The same asserts compile into the existing Windows `input_tests` (guard with `#if defined(_WIN32)`) so both platforms enforce the same invariants.

### Task 2.3: Port the paint half of `render.cpp` (D2D → Painter)

**Files:**
- Modify: `src/render.cpp`
- Create: `tests/linux_render_raster_tests.cpp`

**Interfaces:**
- Consumes: `platform::Painter`
- Produces: a `paintPage(App&, platform::Painter&)` entry that replaces the `BeginDraw`/`EndDraw`/`DrawTextLayout`/`DrawBitmap` block in `main_d2d.cpp` (`WM_PAINT`).

- [ ] **Step 1: API substitution table** (the complete inventory; apply mechanically, keep the surrounding layout math):

| D2D call in render.cpp | Painter call |
|---|---|
| `renderTarget->Clear(color)` | `painter->clear(color)` |
| `renderTarget->FillRectangle(rect, brush)` | `painter->fillRect(brushColor, rect)` |
| `FillRoundedRectangle(rect, r, brush)` | `painter->fillRoundedRect(color, rect, r)` |
| `DrawRectangle(rect, brush, width)` | `painter->strokeRect(color, rect, width)` |
| `DrawLine(p0, p1, brush, width)` | `painter->drawLine(color, width, x0, y0, x1, y1)` |
| `FillEllipse(center, r, brush)` | `painter->fillEllipse(color, rect)` |
| `DrawEllipse(center, r, brush, width)` | `painter->drawEllipse(color, rect, width)` |
| `FillGeometry(geom, brush)` | `painter->fillPath(color, path)` |
| `DrawGeometry(geom, brush, width)` | `painter->drawPath(color, path, width)` |
| `DrawTextLayout(origin, layout, brush)` | `painter->drawTextLayout(layout, x, y)` |
| `DrawBitmap(bitmap, dstRect, opacity)` | `painter->drawImage(image, dst, src, opacity)` |
| `PushAxisAlignedClip(rect)` / `PopAxisAlignedClip()` | `painter->clipRect(rect)` / `restore()` |
| `SetTransform(Matrix3x2F::Scale(s))` | `painter->save(); painter->scale(s)` |
| brush creation (`CreateSolidColorBrush`) | eliminated — colors passed directly |

`app.h`'s `ID2D1SolidColorBrush* brush` cache is dropped; all draw sites pass colors. The `App::TextRect` geometry (used by hit-testing, `textViewForRect`, selection) is unchanged because it stores DIP positions, not device objects.

- [ ] **Step 2: Port the geometry (path) creation.** `app.h:1213` `ID2D1PathGeometry* geometry` (used by mermaid drawing) becomes `platform::Path`; `Path::fromSvgPathData` covers the `BuildPath`/`Figure` loops (`render.cpp` mermaid paint section). Mermaid prims (`mermaid_ext.h`) are pure floats — no changes there.

- [ ] **Step 3: Port `App::ImageEntry`.** `ID2D1Bitmap*` → `platform::ImageRef`. `createDecodedBitmap` (`render.cpp:2994`) uploads `DecodedImage` (BGRA premultiplied bytes) via `SkImage::MakeRasterCopy` (Phase 2 uses the raster canvas; GPU upload arrives with the GPU swap task).

- [ ] **Step 4: Raster render test** — `tests/linux_render_raster_tests.cpp` renders `tests/fixtures/linux-port-viewer.md` into a 1024-wide raster surface at 1x and 2x, and at a 640-wide viewport. Assertions:
  - the page background color dominates the corners;
  - heading text pixels differ from background in the heading band (text actually drew);
  - a code block has its `codeBackground` at its top-left;
  - the 2x image's text band is at least 2× the 1x height (zoom transform applied).
  PNGs go to `build/output/` (kept out of git per AGENTS.md).

- [ ] **Step 5: Manual check in the built app** — open `tests/fixtures/linux-port-viewer.md`; verify theme switch (T), zoom (Ctrl+wheel), and narrow/wide layout. Record in `tests/fixtures/linux-port-phase2.md`.

### Task 2.4: Port `inline_style.cpp`, `image_loader.cpp`, and `App` members

**Files:**
- Modify: `include/inline_style.h`, `src/inline_style.cpp`, `src/image_loader.cpp`, `include/image_loader.h`, `include/app.h`

**Interfaces:**
- Consumes: `platform::TextEngine`, `platform::Image`
- Produces: `DecodedImage` (unchanged struct), `bool decodeImageFile(const std::wstring& path, uint32_t maxDimension, DecodedImage& out)` replacing the WIC call; `InlineStyle` carries `std::shared_ptr<const FontSpec>` instead of `IDWriteTextFormat*`; `emphasisFormat`/`inlineCodeFormat` use `app.textEngine`.

- [ ] **Step 1: `image_loader.cpp` port.** Keep `boundedImageSize` and `DecodedImage` (already platform-free, BGRA premultiplied). Replace the WIC body with:

```cpp
// platform/linux/ImageSkia.cpp
#include "include/codec/SkCodec.h"
#include "include/core/SkBitmap.h"
#include "platform/Image.h"
#include "platform/text.h"

bool decodeImageFile(const std::wstring& path, uint32_t maxDimension, DecodedImage& out) {
    out = {};
    auto data = SkData::MakeFromFileName(fromWide(path).c_str());
    if (!data) return false;
    auto codec = SkCodec::MakeFromData(data);
    if (!codec) return false;
    SkImageInfo info = codec->getInfo();
    if (info.width() > IMAGE_SOURCE_MAX_DIMENSION || info.height() > IMAGE_SOURCE_MAX_DIMENSION ||
        (uint64_t)info.width() * info.height() > IMAGE_SOURCE_MAX_PIXELS) return false;
    const auto size = boundedImageSize(info.width(), info.height(), maxDimension);
    SkImageInfo dstInfo = info.makeDimensions({size.width, size.height})
                              .makeColorType(kBGRA_8888_SkColorType)
                              .makeAlphaType(kPremul_SkAlphaType);
    SkBitmap bm; bm.allocPixels(dstInfo);
    if (SkCodec::kIncompleteInput == codec->getPixels(dstInfo, bm.getPixels(), bm.rowBytes()))
        return false;
    out.width = size.width; out.height = size.height;
    out.dpiX = out.dpiY = 96.0f;
    out.pixels.assign((uint8_t*)bm.getPixels(),
                      (uint8_t*)bm.getPixels() + bm.computeByteSize());
    return true;
}
```

Remote-image path (`asyncImageWorker`, `render.cpp:3021`): replace `URLDownloadToCacheFileW` with `platform::HttpClient::httpGetToTempFile` (Task 5.4 provides it; Phase 2 stubs it with a synchronous curl call).

- [ ] **Step 2: `InlineStyle` port** — mechanical: `IDWriteTextFormat*` → `std::shared_ptr<const platform::FontSpec>`; `D2D1_COLOR_F` → `qmd::Color` (via the alias). `flattenInline` logic unchanged.

- [ ] **Step 3: Verify viewer still renders after the App member migration.** Run the raster tests + Windows tests; commit.

---

## Phase 3 — Interaction: scroll, zoom, search, TOC, links, selection

**Deliverable:** the viewer is fully interactive — wheel scroll, Ctrl+wheel zoom, click links, search with matches, TOC with jump, text selection + copy, back/forward navigation, lightbox.

### Task 3.1: Port `input.cpp` mouse/wheel/zoom handlers

**Files:**
- Modify: `src/input.cpp`, `include/input.h`
- Create: `tests/linux_input_tests.cpp` (uses the SDL-injected event harness, keystroke.h)

**Interfaces:**
- Consumes: `platform::Event`, `platform::Window`
- Produces: `bool handleKeyDown(App&, platform::Window&, unsigned vk, const KeyMods&)`, `void handleCharInput(App&, const std::wstring&)`, `void handleMouseDown/Up/Move(App&, platform::Window&, int x, int y, unsigned button, const KeyMods&)`, `void handleMouseWheel(App&, platform::Window&, int deltaY, int deltaX)` — same bodies, HWND parameter replaced by the `platform::Window&` (used only for `SetTimer`/`PostMessage`/clipboard now).

- [ ] **Step 1:** Change the signatures in `include/input.h`; inside the bodies replace:
  - `SetTimer(hwnd, id, ms)` → `window->setTimer(id, ms)`
  - `KillTimer(hwnd, id)` → `window->killTimer(id)`
  - `PostMessage(hwnd, msg, ...)` → `window->postEvent(platform::Event{...})`
  - `GetAsyncKeyState(VK_*)` → the `KeyMods` carried on the Event
  - `WM_MOUSEWHEEL` delta handling stays (120 units); `platform::Event` delivers the same deltas.
- [ ] **Step 2:** Port `handleCharInput` — it already receives `wchar_t`; keep it. The SDL loop calls it on `Event::Char`.
- [ ] **Step 3: Test** — extend the harness from `tests/keystroke.h`: its `usLayoutCharacter` already models TranslateMessage; add a `postKey(vk, mods)` that builds an SDL key event, pushes it through `Window::pollEvent`, and runs `handleKeyDown` then, only if unconsumed, `handleCharInput` (mirroring the main loop contract from `main_d2d.cpp` / issue #195). Assert: typing "abc" in a search box appends "abc"; Ctrl+F opens search; Esc closes it; wheel zoom changes `app.zoomFactor`.
- [ ] **Step 4: Commit.**

### Task 3.2: Port search, TOC, links, selection, lightbox

**Files:**
- Modify: `src/search.cpp`, `src/search_input.cpp`, `src/search_panel.cpp`, `src/sidepanels.cpp`, `src/utils.cpp` (`handleLinkClick`, `scrollToHeadingId`), `src/overlays.cpp` (lightbox)
- Create: `tests/linux_nav_tests.cpp`

**Interfaces:**
- Consumes: `Painter`, `TextEngine`, `Window`, `Event`
- Produces: nothing new — all these modules already run on the element tree + DIP geometry; their D2D draw calls migrate with the Task 2.3 substitution table. The only functional change is link-click dispatch: `openUrl` (`utils.cpp`) currently uses `ShellExecuteW`; on Linux it becomes `xdg-open` (Task 5.5), and file references open via `openDocumentInViewer`.

- [ ] **Step 1:** Migrate the three search modules' paint calls per the Task 2.3 table; keep their hit-testing unchanged.
- [ ] **Step 2:** Migrate selection (`src/selection.cpp`, `include/selection.h`) — selection rectangles are DIPs; only the paint and `copyToClipboard` calls change (clipboard in Task 5.1; Phase 3 wires the SDL text clipboard).
- [ ] **Step 3: Nav tests** — with a fixture that has two headings and one footnote link (`tests/fixtures/linux-port-nav.md`): search finds the second heading and scrolls to it; TOC lists both headings and Enter jumps; clicking a local file reference opens it (via a temp-file fixture); selection + Ctrl+C copies the selected text (assert against the SDL clipboard).
- [ ] **Step 4: Commit.**

---

## Phase 4 — Editor, tabs, overlays, start page, multi-window, annotations

**Deliverable:** edit mode (docked preview, line-number gutter, carets/multi-cursor, list continuation, Ctrl+B/I wraps), tabs in the title bar (drag/reorder/close, session restore), start page, context menus, annotation rail, drafts, folder browser — feature-parity with the Windows viewer for the interactive surface, with CJK IME.

### Task 4.1: Port `editor.cpp` and `editrail.cpp`

**Files:**
- Modify: `src/editor.cpp`, `src/editrail.cpp`, `include/editor.h`
- Create: `tests/linux_editor_tests.cpp`

**Interfaces:**
- Consumes: `Painter`, `TextEngine`, `Window`, `Event`
- Produces: editor stays `App`-state-driven; caret, selection and the text buffer are already in `App`. Text-format per style (`editorTextFormat`, `editorGutterFormat`, line-number face) come from `app.textEngine`.

- [ ] **Step 1:** Apply the paint table to the ~78 D2D references in `editor.cpp`; the layout/caret math (already DIP-based) is untouched.
- [ ] **Step 2: Surrogate-pair audit** (`src/editor.cpp:829`). On Linux (UTF-32) "never split a code point" replaces "never split a surrogate pair"; make the helper operate on code points by construction (verify and add a Linux-only assert in the caret test).
- [ ] **Step 3: IME for the editor.** The Windows path anchors the composition window via `WM_IME_*` (`main_d2d.cpp:1610`). On Linux the SDL loop delivers `Event::ImeComposition`; render the preedit string at the caret in the editor, and call `window->setTextInputArea(caretDips * scale, ...)` so fcitx/IBus place their candidate window. Verify with fcitx + a CJK input method in both X11 and Wayland sessions.
- [ ] **Step 4: Editor tests** — via keystroke.h: type "hello" in edit mode, Read returns to viewer preserving edits; Ctrl+B wraps a selection; Enter continues a list; multi-cursor Alt+Click adds a caret (mouse events injected); unsaved-close drafts are stashed to the config dir (Task 5.3).
- [ ] **Step 5: Fixture** — `tests/fixtures/linux-port-editor.md` (mixed document edited at normal/narrow widths, Chinese + Japanese text with IME notes). Commit.

### Task 4.2: Port `tabs.cpp`, `overlays.cpp`, `startpage.cpp`, annotations, drafts

**Files:**
- Modify: `src/tabs.cpp` (~100 D2D refs), `src/overlays.cpp` (~462 D2D refs), `src/startpage.cpp`, `src/annotations.cpp`, `src/drafts.cpp`, `src/context_menu.cpp`, `src/tableedit.cpp`, `src/wordcount.cpp`

**Interfaces:**
- Consumes: `Painter`, `Window`, `Event`
- Produces: same `App` state; the title-bar tab strip becomes a client-drawn strip inside the window (on Wayland the compositor titlebar is minimal anyway — the existing in-titlebar-tabs design ports as a drawn strip at the top of the SDL window).

- [ ] **Step 1:** Apply the paint table to the 4 big draw modules. The overlays are hit-test/geometry driven — their `TextRect` lists are DIP data, unchanged.
- [ ] **Step 2: Multi-window.** Windows spawns additional `WinMain` processes with `/quicknote` or `/file` args (`main_d2d.cpp:1914`, `launchQuickNoteWindow`). Linux: `posix_spawn` the same binary with the same args (Task 5.5 `Process`); the SDL event loop is identical per process.
- [ ] **Step 3: Session restore + drafts** — already persisted as settings.ini files; the config-dir port (Task 5.3) makes them land in `~/.config/tinta`.
- [ ] **Step 4: Tests** — window-drag tests (`window_drag_tests.cpp`), tab-drop tests, tab-hover tests, docked-preview tests, notification tests all port by routing the same gestures through SDL events. Port the existing test files first (they encode the expected behaviors), then adjust the event delivery. Run on Linux; run unchanged on Windows.
- [ ] **Step 5: Commit.**

---

## Phase 5 — System integration

**Deliverable:** clipboard (plain/HTML/bitmap), drag-drop, native file dialogs, folder browse, file association, reveal-in-file-manager, XDG config dir, file watcher, external editor, pandoc/PlantUML process spawn, HTTP.

### Task 5.1: Clipboard (`platform/Clipboard.h` + SDL)

**Files:**
- Create: `platform/Clipboard.h`, `platform/linux/ClipboardSDL.cpp`
- Modify: `src/utils.cpp` (`copyToClipboard`, `copyRichTextToClipboard`, `openClipboard`), `src/export_docx.cpp` (bitmap copy)

**Interfaces:**
- Produces: `bool copyText(const std::wstring&)`, `bool copyRichText(const std::wstring& text, const std::wstring& htmlFragment)`, `std::wstring clipboardText()`, `bool copyPng(const std::vector<uint8_t>& pngBytes)`, `std::optional<std::vector<uint8_t>> clipboardPng()` (editor image paste).

- [ ] **Step 1:** Implement over SDL3's clipboard API: plain text via `SDL_SetClipboardText`; rich text and PNG via `SDL_SetClipboardData` with mime `text/html` and `image/png` and a callback that returns the bytes (SDL3 supports custom mime types on X11 and Wayland).
- [ ] **Step 2:** Replace the `HWND`-taking clipboard helpers (`utils.cpp`) with the platform calls. Keep the Windows implementations behind `#ifdef` (behavior parity: the CF_UNICODETEXT/CF_HTML pair stays).
- [ ] **Step 3: Tests** — copy text, paste it back; copy a rich fragment and assert the `text/html` mime exists (on X11; on Wayland headless, gate the assertion); `clipboardImageToPngFile` round-trips a PNG.
- [ ] **Step 4: Commit.**

### Task 5.2: File dialogs (`platform/Dialogs.h` + XDG portal)

**Files:**
- Create: `platform/Dialogs.h`, `platform/linux/DialogsPortal.cpp`
- Modify: `src/file_utils.cpp` (`runOpenFileDialog`, `runSaveFileDialog`), `src/settings.cpp` (settings/fonts dialogs), `src/print.cpp` (save PDF dialog)

**Interfaces:**
- Produces: `bool openFileDialog(const std::vector<std::pair<std::wstring,std::wstring>>& filters, std::wstring& outPath)` (label+pattern, `*.*` allowed), `bool saveFileDialog(const std::vector<...>& filters, const std::wstring& defaultName, std::wstring& outPath)`, `bool messageBox(const std::wstring& text, bool isError)`.

- [ ] **Step 1:** Implement via the XDG Desktop Portal file-chooser D-Bus API (`org.freedesktop.portal.FileChooser` with `OpenFile`/`SaveFile`, response via the `Response` signal), using `libdbus` or a minimal D-Bus client (no GTK). Fallback: spawn `zenity --file-selection` if the portal is unavailable (e.g., some minimal WMs).
- [ ] **Step 2:** Rewire `runOpenFileDialog`/`runSaveFileDialog` — the existing `OPENFILENAMEW` sites each map to a `filters` list; on Windows the `OPENFILENAMEW` path stays.
- [ ] **Step 3: Test** — a dialog test that stubs the portal response (returns a temp path) and asserts the filter → path round trip; a live manual check on a GNOME/KDE session.
- [ ] **Step 4: Commit.**

### Task 5.3: Config dir, file association, reveal-in-file-manager, file watcher

**Files:**
- Modify: `src/config_dir.cpp`, `src/settings.cpp`, `src/file_utils.cpp`, `src/utils.cpp` (openUrl), `include/settings.h`
- Create: `platform/FileSystem.h` (full), `platform/linux/FileSystemPosix.cpp`

**Interfaces:**
- Produces:
  - `std::wstring tintaConfigDir()` — portable mode (settings.ini beside exe) unchanged; otherwise `$XDG_CONFIG_HOME/tinta` (fallback `~/.config/tinta`), created on demand.
  - `bool registerFileAssociation()` — writes `~/.local/share/applications/tinta.desktop` (`MimeType=text/markdown;text/x-markdown;application/x-mermaid;...`) and runs `xdg-mime default tinta.desktop text/markdown ...`.
  - `void revealInFileManager(const std::wstring& path)` — D-Bus `org.freedesktop.FileManager1.ShowItems`; fallback `xdg-open` the parent folder.
  - `void openUrl(const std::string& url)` — `xdg-open` via `posix_spawn` (Task 5.5), never a shell.
  - `void fileWatchStart(App&)` / `void fileWatchStop(App&)` — inotify on the current document directory; posts the existing `TIMER_FILE_WATCH` event.
  - `bool isRootPath(path)`, `getParentPath`, `getDirectoryFromFile` — reuse the existing pure logic; both separators are already handled everywhere.

- [ ] **Step 1:** Implement `config_dir.cpp` Linux branch (per the interface above). Portable-mode parity is a test: run the binary from a dir containing `settings.ini`, assert settings/themes/drafts stay next to the exe.
- [ ] **Step 2:** Implement file association + reveal + openUrl; wire `askAndRegisterFileAssociation` (`settings.cpp`) to a messageBox offering registration.
- [ ] **Step 3:** Implement the inotify watcher; `src/file_utils.cpp`'s existing watch logic ports to the `TIMER_FILE_WATCH` event contract.
- [ ] **Step 4: Tests** — config-dir precedence (`tests/config_path_tests.cpp` already exists; add a Linux case for XDG), association writes the `.desktop` file (in a temp `XDG_DATA_HOME`), watcher fires on file change.
- [ ] **Step 5: Commit.**

### Task 5.4: HTTP (`platform/HttpClient.h` + libcurl)

**Files:**
- Create: `platform/HttpClient.h`, `platform/linux/HttpCurl.cpp`
- Modify: `src/main_d2d.cpp` (update check), `src/render.cpp` (remote image download)

**Interfaces:**
- Produces: `bool httpGetToBuffer(const std::string& url, std::vector<char>& out, int timeoutMs)`, `bool httpGetToTempFile(const std::string& url, std::string& outPath)` — the two call patterns the app uses today (update check body, remote image → temp cache file).

- [ ] **Step 1:** Implement with libcurl (easy API), a `CAINFO` path via the system trust store (`/etc/ssl/certs/ca-certificates.crt`), and a 10 s timeout. Keep the existing worker-thread + `Event::App{id=WM_APP_UPDATE_CHECK|WM_APP_IMAGE_READY}` delivery (Task 1.4's custom SDL events).
- [ ] **Step 2:** Replace the WinHTTP block (`main_d2d.cpp:1122`) and `URLDownloadToCacheFileW` (`render.cpp:3021`).
- [ ] **Step 3: Tests** — against a local `python3 -m http.server` fixture: GET a small file, assert bytes; timeout case with a hanging server. (Do not hit the network in CI; use a localhost server only.)
- [ ] **Step 4: Commit.**

### Task 5.5: Process spawn (`platform/Process.h` + posix)

**Files:**
- Create: `platform/Process.h`, `platform/linux/ProcessPosix.cpp`
- Modify: `src/pandoc.cpp` (CreateProcessW), `src/plantuml_app.cpp`, `src/plantuml_queue.cpp` (OpenProcess)

**Interfaces:**
- Produces: `bool spawnProcess(const std::vector<std::string>& argv, const std::string& cwd, int& pid)`, `bool processAlive(int pid)` (Linux: `/proc/<pid>` exists), `bool runAndWait(const std::vector<std::string>& argv, std::string& stdoutOut, int& exitCode)` (pandoc, plantuml jar).

- [ ] **Step 1:** Implement with `posix_spawn` (cwd via file-actions `chdir`), argv UTF-8, and `/proc` pid checks.
- [ ] **Step 2:** Port `pandoc.cpp` (its job/`WM_APP_PANDOC_DONE` contract stays), `plantuml_app.cpp` (find `java -jar plantuml.jar`, spawn, watch temp output), and `plantuml_queue.cpp` pid-liveness (replace `OpenProcess` with `/proc`).
- [ ] **Step 3: Tests** — `spawnProcess`/`runAndWait` with `/bin/echo`; `processAlive` against a sleeping child then after it exits; PlantUML smoke with the `fake_plantuml` test helper (`tests/fake_plantuml.cpp` exists — reuse it).
- [ ] **Step 4: Commit.**

---

## Phase 6 — Diagrams & math: Mermaid, PlantUML, math, images

**Deliverable:** Mermaid (all 22 families), math, PlantUML fences, image cache (local + remote + SVG), all rendering in the Linux viewer and matching the Windows layout.

### Task 6.1: Mermaid painting through `platform::Path`/`Painter`

**Files:**
- Modify: `src/render.cpp` (mermaid paint section), `src/mermaid*.cpp` only where they touch D2D (audited: `mermaid.cpp` and `mermaid_ext.cpp` are pure — no changes)

**Interfaces:**
- Consumes: `platform::Path`, `Painter`
- Produces: same `mermaidext::Prim` structures; the prim → path/rect/ellipse/line translation in `render.cpp` is replaced by `Painter` calls.

- [ ] **Step 1:** Apply the geometry substitution: `BuildPath`/`ID2D1GeometrySink` → `Path::fromSvgPathData`/`Path` ops; `FillGeometry`/`DrawGeometry` → `fillPath`/`drawPath`; arrowheads and labels (already text layouts) → `drawTextLayout`.
- [ ] **Step 2: Fixture-based regression** — `tests/fixtures/linux-port-mermaid.md` exercises a flowchart with subgraphs, a sequence diagram, a class diagram, a state diagram, an ER diagram, a gantt, a pie, and a C4 diagram. Raster-render at normal and narrow widths; assert each diagram's bounding box is non-degenerate and its title/labels draw. The existing `mermaid_layout_checks.h` invariants (node boxes don't overlap their labels) compile as Linux asserts too.
- [ ] **Step 3: Commit.**

### Task 6.2: Math rendering

**Files:**
- Modify: `src/math_render.cpp` (24 D2D/DWrite refs), `src/math_parser.cpp` (pure — unchanged)

**Interfaces:**
- Consumes: `TextEngine` (per-run styling), `Painter`
- Produces: `mathBoxSvg` (already used by `export_html`) unchanged; on-screen math uses `TextEngine::layout` with script fonts and a matrix/brace paint via `Painter`.

- [ ] **Step 1:** Port `math_render.cpp`'s layout calls (same table as Task 2.2) and its brace/matrix drawing (fills/line/paths per the Task 2.3 table).
- [ ] **Step 2: Tests** — `tests/math_layout_tests.cpp` (already exists) runs on Linux; add a raster check that `$\sqrt{x^2}$` and a matrix render taller than a plain line and produce non-background pixels.
- [ ] **Step 3: Commit.**

### Task 6.3: PlantUML and image cache end-to-end

**Files:**
- Modify: `src/plantuml.cpp`, `src/plantuml_queue.cpp`, `src/render.cpp` (cache)
- Create: `tests/linux_plantuml_tests.cpp`

**Interfaces:**
- Consumes: `Process`, `HttpClient`, `FileSystem`
- Produces: PlantUML fences render via the local PlantUML jar (already the Windows flow: `java -jar plantuml.jar -pipe`); the queue's cache directory moves under the config dir.

- [ ] **Step 1:** Wire the queue's subprocess + pid checks to `Process`; keep the cache key/path logic identical.
- [ ] **Step 2:** Remote images: `getOrLoadImage` (`render.cpp:3182`) already keys by URL and gates `mayOpenFromDocument` (`network_path.h`, pure — unchanged). On Linux the same security gate applies to paths: a document may only pull images from its own host or local paths (UNC-specific checks become SMB-mount checks — `/mnt/server/...` has no Windows credentials, so remote-image gating is relaxed but kept for parity).
- [ ] **Step 3: Tests** — PlantUML via `fake_plantuml.cpp`; image cache dedupe with two documents referencing the same-name image in different folders (`tests/fixtures/same-image-name` already covers this on Windows — port the test).
- [ ] **Step 4: Commit.**

---

## Phase 7 — Exports and printing

**Deliverable:** Export as HTML/DOCX/PDF and print preview work from the Linux app; diagrams and math survive in every format; PDFs are vector with embedded fonts.

### Task 7.1: HTML and DOCX export

**Files:**
- Modify: `src/export_html.cpp`, `src/export_docx.cpp`, `src/export.h`

**Interfaces:**
- Consumes: `Painter` (diagram rasterization), `Dialogs` (save), `Clipboard` (copy-diagram-image)
- Produces: `exportHtmlFile`, `exportDocxFile` (unchanged signatures) with the same outputs.

- [ ] **Step 1:** `export_html.cpp` is already portable (it emits SVG from the element tree; the includes of `commdlg.h`/`urlmon` are for dialogs and image-embedding only). Guard the `commdlg`/WinHTTP bits and route through `Dialogs`/`HttpClient`. No output change.
- [ ] **Step 2:** `export_docx.cpp` (59 D2D refs) rasterizes diagrams to PNG through the same primitives. Replace `ID2D1Bitmap`/WIC writing with `SkSurface` raster + `SkPngEncoder`. The clipboard DIB/PNG copy (`:1856`, `:1994`) goes through `Clipboard::copyPng`. The hand-rolled `ZipWriter` is already portable.
- [ ] **Step 3: Tests** — reuse `tests/print_export_tests.cpp` and `tests/rich_copy_tests.cpp` on Linux: export a fixture to HTML and DOCX in a temp dir, assert the HTML contains the inline SVG and the DOCX opens with `python3 -c "import zipfile; zipfile.ZipFile(...)"` listing `word/document.xml` and the PNG media parts.

### Task 7.2: Print preview and PDF export (`platform/Print.h` + Skia PDF)

**Files:**
- Create: `platform/Print.h`, `platform/linux/PrintSkia.cpp`
- Modify: `src/print.cpp`, `include/print.h`

**Interfaces:**
- Produces: `bool exportPdfFile(App& app, const std::wstring& path)` (existing signature) — vector PDF, embedded fonts; `printDocument(App&)` — on Linux, prints to PDF via the same page-replay, or launches a system print dialog when available (defer the dialog integration to a follow-up; the PDF export is the deliverable).

- [ ] **Step 1:** Reimplement `print.cpp`'s page replay (already device-independent: it paginates into `D2DTheme`-colored pages at paper DIPs) into an `SkPDFDocument`:

```cpp
// platform/linux/PrintSkia.cpp
#include "include/docs/SkPDFDocument.h"
#include "platform/Print.h"
#include "platform/text.h"

bool exportPdfFile(App& app, const std::wstring& path) {
    auto pdf = SkPDF::MakeDocument(fromWide(path).c_str());
    int pageCount = paginateForPrint(app, kA4DipsW, kA4DipsH); // ported from print.cpp
    for (int i = 0; i < pageCount; ++i) {
        auto* page = pdf->beginPage(kA4DipsW, kA4DipsH);
        paintPageIntoCanvas(app, page);   // Painter over SkCanvas, print palette
        pdf->endPage();
    }
    return pdf->close();
}
```

Font embedding (including CJK) is handled by Skia's PDF backend; set `SkPDF::Metadata::setProducer` to "Tinta". Keep `printTheme`, `PRINT_PAPERS`, `PRINT_MARGINS_MM`, and the preview overlay (`openPrintPreview` etc.) — they are UI/state and port via the paint table.

- [ ] **Step 2: Tests** — `exportPdfFile` to a temp file; assert it starts with `%PDF` and contains a font object (vector text) by scanning the bytes; compare page count with `printDebugPages` (which now rasterizes via `Painter`).

---

## Phase 8 — Packaging, updates, performance

**Deliverable:** installable packages, update check, startup timing within the same order of magnitude as Windows, GPU rendering when available.

### Task 8.1: GPU rendering + startup swap

**Files:**
- Modify: `platform/linux/PainterSkia.cpp`, `platform/linux/Main.cpp`

**Interfaces:**
- Produces: `Painter` over a Vulkan (then GL, then raster) surface; first paint software, then swap to GPU — the exact strategy of `WM_APP_GPU_READY` (`main_d2d.cpp:59-74`).

- [ ] **Step 1:** Implement the Skia GPU context (Vulkan via `GrDirectContext::MakeVulkan`, fallback OpenGL, fallback raster). On `Event::App{id=WM_APP_GPU_READY}` swap the canvas; keep the raster screenshot tests green (they use the raster path).
- [ ] **Step 2:** Startup metrics: keep the `StartupMetrics` struct and print them with a `TINTA_STARTUP_METRICS` env var on Linux; assert in CI that `totalStartupUs` (software first frame) is < 250 ms on the runner.

### Task 8.2: Packaging (CPack) and desktop integration

**Files:**
- Create: `packaging/linux/tinta.desktop`, `packaging/linux/CMakeLists.txt`
- Modify: `CMakeLists.txt`

- [ ] **Step 1:** CPack `.deb` (and AppImage via `appimagetool` in CI): package the binary + `tinta.desktop` (`Exec=tinta %F`, `MimeType=text/markdown;...`) + icon (convert `resources/tinta.ico` → PNG at build time with ImageMagick or a tiny Skia tool).
- [ ] **Step 2:** `install()` rules so `cmake --install` lands in `/usr/local`; CI builds the package on ubuntu-latest, installs it in a container, then `xdg-mime query default text/markdown` returns `tinta.desktop`.
- [ ] **Step 3:** Update `.github/workflows/build.yml` with a `linux` job (build, ctest, package, smoke-install). Windows job stays as-is.

### Task 8.3: Update check

**Files:**
- Modify: `src/main_d2d.cpp` (update check — move the logic into a shared `src/update_check.cpp`)

- [ ] **Step 1:** Extract the update check from `main_d2d.cpp` into `src/update_check.cpp` using `HttpClient::httpGetToBuffer("https://api.github.com/repos/oipoistar/tinta/releases/latest", ...)`; same quiet chip UI; same once-a-day persistence.

---

## Phase 9 — Test suite port and regression

**Deliverable:** the Windows test suite runs on Linux (or has a documented Linux equivalent), the fixture validation docs are complete, and CI is green on both platforms.

### Task 9.1: Port the input_tests harness to SDL

**Files:**
- Modify: `CMakeLists.txt` (Linux test targets), `tests/keystroke.h`, `tests/sdl_tests.cpp`
- Create: `tests/run_linux_*.cmake` (mirrors of the existing `run_*.cmake` scripts)

- [ ] **Step 1:** Build one `linux_tests` binary on Linux from the same test .cpp list as `input_tests` (minus `main_d2d.cpp`), linking `tinta_core` + platform + Skia + SDL. Replace the message-only window with an offscreen SDL window (`SDL_VIDEODRIVER=offscreen`) and route keystrokes through `Window::pollEvent` using `sdlToVk`.
- [ ] **Step 2:** Port each `run_*.cmake` script's semantics (they run the binary per fixture with a temp `TINTA_TEST_DIR`). For tests that verify layout by rendering, add the raster-PNG step from Task 2.3.
- [ ] **Step 3: Parity run** — same fixture inputs, same invariants, on both platforms: add a `linux` CTest label and a CI job that fails if the Linux/Windows invariant asserts diverge for a shared fixture.

### Task 9.2: Fixture validation docs

**Files:**
- Create: `tests/fixtures/linux-port-phase1.md` … `linux-port-phase8.md` (one per phase, updated as phases land; Phase 1/2 docs were created in Tasks 1.6/2.3)

**Interfaces:**
- Each doc records what was checked in the built Linux app (normal + narrow widths, light + dark themes, X11 + Wayland where available), what was exercised via automated tests, and remaining limitations — per the AGENTS.md feature-validation workflow.

### Task 9.3: Final regression + parity report

**Files:**
- Create: `docs/linux-port-status.md`

- [ ] **Step 1:** Run the full fixture suite on Linux (viewer, editor, search, tabs, mermaid, math, plantuml, exports, print). Compare against Windows runs for the shared fixtures.
- [ ] **Step 2:** Write the status report: ported feature matrix, remaining gaps (Wayland IME caveats, printer-dialog integration, SMB share semantics), performance numbers, and the maintainer-facing draft release notes.

---

## 3. Risks & Mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| `wchar_t` 32-bit vs 16-bit semantics | subtle text/caret bugs | `platform/text.h` only; audit `editor.cpp:829`; parity tests on both platforms (Task 4.1/9.1) |
| SkParagraph layout fidelity differs from DirectWrite | line breaks/widths shift; fixtures regress | Task 2.2 parity fixtures asserting invariants on BOTH platforms; per-API substitution table; escape hatch: adjust RunStyle details only, never the element tree |
| IME on Wayland is patchy | CJK editing poor on Wayland | SDL3 text-input events + `setTextInputArea`; test X11 + Wayland; document Wayland IME as a known limitation if compositors lag |
| Skia build weight and CI time | slow first builds | pinned GN build script with system packages; ccache in CI; artifact caching |
| Rich clipboard mimes on Wayland | paste into Word/Excel loses HTML | `SDL_SetClipboardData` mime callbacks; fall back to plain text on platforms without mime support |
| SMB/UNC security model doesn't exist on Linux | network-share gating semantics change | keep `mayOpenFromDocument` logic; document the Linux meaning (mount-path based) |
| Printer dialog integration | print preview → PDF only initially | deliverable is PDF export; system printer dialog is a tracked follow-up, not a blocker |

## 4. Milestones & Effort (single senior C++/graphics engineer)

| Milestone | Phase | Deliverable | Estimate |
|---|---|---|---|
| M0 | 0 | Linux core lib + smoke tests green | 3–5 days |
| M1 | 1 | SDL/Skia window skeleton + platform layer | 1–2 weeks |
| M2 | 2 | Viewer renders a Markdown file (usable viewer) | 2–3 weeks |
| M3 | 3 | Full viewer interaction | 1–2 weeks |
| M4 | 4 | Editor, tabs, overlays, IME | 3–4 weeks |
| M5 | 5 | System integration (clipboard/dialogs/files/HTTP/process) | 2 weeks |
| M6 | 6 | Mermaid/math/PlantUML/images | 2 weeks |
| M7 | 7 | Exports + PDF | 1–2 weeks |
| M8 | 8 | GPU, packaging, CI, updates | 1–2 weeks |
| M9 | 9 | Test parity + regression report | 1–2 weeks |

Total: **≈ 3.5–5 months** for full feature parity on one platform pair (X11/Wayland). A "usable viewer" (M0–M2 plus search/links basics) is reachable in ~6–8 weeks.

## 5. Self-Review

- **Spec coverage:** The requirement ("port to Linux, no Wine") is covered end-to-end: build (P0), rendering/text/window (P1–P2), interaction (P3), full UI (P4), OS integration (P5), diagrams/exports (P6–P7), packaging/perf (P8), and test parity (P9). The AGENTS.md workflow (fixtures, built-app exercise, regression checks, ignored output dirs, keystroke.h) is encoded in Global Constraints and per-task steps.
- **Placeholder scan:** No TBDs. Porting-map tasks carry explicit API substitution tables and concrete test assertions rather than "implement later" phrasing. Every new file has a named owner task with code.
- **Type consistency:** `platform::` names (`Painter`, `TextEngine`, `FontSpec`, `TextLayout`, `RunStyle`, `Window`, `Event`, `Path`, `DecodedImage`, `Clipboard`, `Dialogs`, `FileSystem`, `HttpClient`, `Process`, `Print`) are defined once in Phase 1/5 interfaces and reused verbatim by every later task. `qmd::Color` is the single color type; `D2D1_COLOR_F` is a transition alias on Linux and the real type on Windows. VK constants keep their Windows numeric values so `keymap.cpp`/`keystroke.h` are unchanged.
