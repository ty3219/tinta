# Tinta Windows 7 Compatibility Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Make the portable `tinta.exe` build start and run on Windows 7 SP1 (64-bit) by removing every static import of Windows 8/10-only APIs, with graceful fallbacks, without changing behavior on Windows 10/11.

**Architecture:** A single header `include/win7_compat.h` provides inline shims that resolve the Windows 8/10-only APIs (`GetDpiForWindow`, `GetSystemMetricsForDpi`, `SetProcessDpiAwarenessContext`, `GetCurrentPackageFullName`, `GetPointerType`, `GetPointerInfo`) via `GetProcAddress`, falling back to legacy equivalents. All call sites are converted to the shims, so the linker records no imports for those symbols and the PE imports nothing that Windows 7 lacks. A pure-C++ regression test scans the source tree to guarantee no direct calls return.

**Tech Stack:** C++17, CMake, Win32/Direct2D (unchanged), Windows 10 SDK (unchanged — dynamic loading means the built binary starts on Win7; `_WIN32_WINNT` stays at the SDK default).

## Global Constraints

- Windows 10/11 behavior must be byte-for-byte unchanged (the modern APIs exist there, so the shims take the modern path).
- No new Windows-only DLLs; every linked DLL (`d2d1`, `dwrite`, `d3d11`, `dwmapi`, `windowscodecs`, `urlmon`, `winhttp`, `imm32`, `shell32`, `dbghelp`, `ole32`, `comdlg32`, `winspool`, `shlwapi`) already ships with Windows 7 SP1.
- Windows 7 SP1 (with the Platform Update KB2670838 for Direct2D/DirectWrite 1.1 features) is the floor; plain SP1 still starts with graceful feature loss (print/PDF, color emoji, SVG, CJK custom font fallback degrade).
- The MSVC build and `ctest` must stay green on Windows (CI: `windows-latest`).
- AGENTS.md applies: add a runnable fixture under `tests/fixtures/`, add an automated regression check, and record verification + remaining limitations.
- This workspace has no Windows toolchain; runtime verification on a real Windows 7 machine is the maintainer's step, documented in the validation doc.

---
## File Map

- Create: `include/win7_compat.h` — the dynamic-loading shims (only file allowed to name the forbidden APIs).
- Create: `tests/win7_compat_tests.cpp` — pure-C++ source scan; fails if a forbidden API token appears outside the shim.
- Modify: `src/main_d2d.cpp:1096,1917,2165`, `src/input.cpp:1113-1115`, `src/tabs.cpp:2108`, `src/touch.cpp:417,419`, `src/settings.cpp:478`, `tests/window_drag_tests.cpp:38-40` — route calls through the shims.
- Modify: `CMakeLists.txt` — register `win7_compat_tests` in CTest.
- Create: `tests/fixtures/windows7-compat.md`, `tests/windows7-compat-validation.md` — AGENTS.md fixture + verification record.
- Modify: `README.md` — Windows 7 compatibility note.

---

### Task 1: Write the failing regression scan

**Files:**
- Create: `tests/win7_compat_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `win7_compat_tests` executable + CTest test `win7_compat`. Pure standard C++ (`<filesystem>`, `<fstream>`, `<string>`), no `windows.h`, so it can build on any host. Compiled with `TINTA_SOURCE_DIR` pointing at the repo root.

- [x] **Step 1:** Write `tests/win7_compat_tests.cpp`:
  - `kForbidden[] = { "GetDpiForWindow", "GetSystemMetricsForDpi", "SetProcessDpiAwarenessContext", "GetCurrentPackageFullName", "GetPointerType", "GetPointerInfo" }`.
  - Walk `src/*.cpp`, `include/*.h`, `tests/*.cpp`, `tests/*.h` under `TINTA_SOURCE_DIR`.
  - Skip the file itself and `include/win7_compat.h`; any other file containing any forbidden token (as an identifier, not part of a longer identifier) fails the test with the file/line.
- [x] **Step 2:** Register in CMakeLists with `add_executable(win7_compat_tests ...)`, `target_compile_definitions(... TINTA_SOURCE_DIR="...")`, `add_test(NAME win7_compat ...)`.
- [x] **Step 3 (Linux, this workspace):** configure + build + run to watch it FAIL (current src/ still calls the APIs directly).

### Task 2: Implement `include/win7_compat.h`

**Files:**
- Create: `include/win7_compat.h`

**Interfaces:**
- Produces (each caches its `GetProcAddress` result in a function-local static):
  - `namespace win7compat { UINT windowDpi(HWND); }` — `GetDpiForWindow` (Win10 1607+); fallback `GetDeviceCaps(GetDC(hwnd), LOGPIXELSX)` then `ReleaseDC`.
  - `int systemMetricsForDpi(int nIndex, UINT dpi);` — `GetSystemMetricsForDpi` (Win10 1607+); fallback `GetSystemMetrics(nIndex)`.
  - `void setPerMonitorV2DpiAwareness();` — `SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)` (Win10 1703+); fallback `SetProcessDPIAware()` (Vista+). Define the `-4` handle value locally if the SDK macro is absent.
  - `bool runningAsPackagedApp();` — `GetCurrentPackageFullName` (Win8+); fallback `false` (Windows 7 has no packaged apps). Uses the `APPMODEL_ERROR_NO_PACKAGE` constant, locally defined if absent.
  - `bool pointerType(UINT32 id, POINTER_INPUT_TYPE* type);` and `bool pointerInfo(UINT32 id, POINTER_INFO* info);` — `GetPointerType`/`GetPointerInfo` (Win8+); fallback `false`.

- [x] **Step 1:** Write the header (guarded, includes `<windows.h>`, includes `<appmodel.h>` only if needed for the constant, caches `HMODULE`/`FARPROC` in statics).
- [x] **Step 2 (Linux):** run the scan test — still red (call sites not converted).

### Task 3: Convert call sites

**Files:**
- Modify: `src/main_d2d.cpp:1096,1917,2165`, `src/input.cpp:1113-1115`, `src/tabs.cpp:2108`, `src/touch.cpp:417,419`, `src/settings.cpp:478`, `tests/window_drag_tests.cpp:38-40`
- Modify: `include/app.h`, `src/input.h`, `src/tabs.h`, `src/touch.cpp`, `src/settings.cpp`, `tests/window_drag_tests.cpp` — add `#include "win7_compat.h"` where the shims are used.

- [x] **Step 1:** Replace each direct call with its `win7compat::` shim.
- [x] **Step 2 (Linux):** run the scan test — GREEN.

### Task 4: Fixture + validation record (AGENTS.md)

**Files:**
- Create: `tests/fixtures/windows7-compat.md`, `tests/windows7-compat-validation.md`
- Modify: `README.md`

- [x] **Step 1:** `tests/fixtures/windows7-compat.md` — a mixed document (headings, tables, lists, emphasis, links, quotes, code, math, a mermaid diagram) demonstrating the core viewer on Windows 7.
- [x] **Step 2:** `tests/windows7-compat-validation.md` — record the static checks done here (import scan, symbol audit), the required Windows 7 verification steps (startup on SP1 and SP1+KB2670838, DPI, rendering, print fallback), and remaining limitations.
- [x] **Step 3:** README compatibility note.

### Task 5: Final audit + commit

- [x] **Step 1:** Re-run the full grep audit: `grep -rn "GetDpiForWindow|..." src include tests` → matches only in `include/win7_compat.h` and `tests/win7_compat_tests.cpp`.
- [x] **Step 2:** Re-run the scan test (green) and review the diff.
- [x] **Step 3:** Commit on branch `win7-compat` with a clear message; report what still needs real-Windows-7 verification.

## Self-Review

- Spec coverage: every identified Win7-incompatible static import is wrapped; the regression scan locks it in; fixtures + validation record satisfy AGENTS.md; Windows 10/11 behavior preserved because the shims resolve the same functions.
- Placeholder scan: none.
- TDD: Task 1 writes the failing scan before any production code; Task 2/3 make it pass.
- Honesty: runtime startup on real Windows 7 is NOT verifiable in this Linux workspace; the validation doc and final report say so explicitly.
