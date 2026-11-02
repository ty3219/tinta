# Windows 7 compatibility (#port)

## Goal

The portable `tinta.exe` must start and run on Windows 7 SP1. Tinta links
Direct2D/DirectWrite and other system DLLs that ship with Windows 7, but the
code directly called six APIs added in Windows 8 or Windows 10. Those became
static imports, so the loader refused to start the process on Windows 7 with
"procedure entry point not found".

## Change

`include/win7_compat.h` resolves those six APIs at runtime through
`GetProcAddress` and falls back to a legacy equivalent when the symbol is
missing. On Windows 10/11 the symbols exist, so behavior is unchanged there.

| API | Added in | Windows 7 fallback |
| --- | --- | --- |
| `GetDpiForWindow` | Win10 1607 | `GetDeviceCaps(LOGPIXELSX)` of the window DC |
| `GetSystemMetricsForDpi` | Win10 1607 | `GetSystemMetrics` (already DPI-scaled) |
| `SetProcessDpiAwarenessContext` | Win10 1703 | `SetProcessDPIAware` (system-DPI aware) |
| `GetCurrentPackageFullName` | Win8 | `false` (no packaged apps on Win7) |
| `GetPointerType` / `GetPointerInfo` | Win8 | `false` (no `WM_POINTER*` on Win7) |

Call sites converted: `src/main_d2d.cpp`, `src/input.cpp`, `src/tabs.cpp`,
`src/touch.cpp`, `src/settings.cpp`, and the `input_tests` harness
`tests/window_drag_tests.cpp`. `include/win7_compat.h` is the only production
file allowed to name those APIs; `tests/win7_compat_tests.cpp` scans `src/`,
`include/` and `tests/` on every `ctest` run and fails if a direct call
returns.

### Seventh API: `CreateFile2` (and `CopyFile2`) via `std::filesystem`

The first port's source scan missed an import that is not in Tinta's source
at all: the MSVC STL's `<filesystem>`. Since the STL's "Win8 baseline"
(VS 2022 17.14, April 2025) `filesystem.cpp` calls `CreateFile2` and
`CopyFile2` unconditionally, so any `std::filesystem` use in the statically
linked exe puts those Windows 8+ names in its kernel32 import table and the
loader refuses to start on Windows 7 with "procedure entry point CreateFile2
could not be located in KERNEL32.dll".

The fix removes `<filesystem>` from the exe entirely. `include/path_utils.h` /
`src/path_utils.cpp` provide the handful of calls the code actually made,
backed only by Windows 7 APIs (GetFileAttributesW, GetFileAttributesExW,
FindFirstFileW, CreateDirectoryW, DeleteFileW, RemoveDirectoryW), plus
`pathNormalize` (lexically_normal) and `pathParentDir` (parent_path)
semantics for the path-joining call sites:

- `src/search.cpp`: folder search scans `FindFirstFileW`, file size via
  `GetFileSizeEx`, reads via `CreateFileW`/`ReadFile`.
- `src/input.cpp`: wiki-link directory scan, file-name extraction, create-ref
  parent directories (`CreateDirectoryW`), link-peek resolution.
- `src/inline_style.cpp`: relative image/file reference resolution and
  `lexically_normal` normalization.
- `src/editor.cpp`: Save-As name/parent and clipboard-image candidate paths.
- `src/render.cpp`: local image `parent_path` + join.
- `src/export_html.cpp`, `src/plantuml.cpp`, `src/plantuml_queue.cpp`:
  recursive temp work-dir create/delete and artifact scans.

`tests/win7_compat_tests.cpp` now also fails if `<filesystem>` /
`std::filesystem` appears in `src/` or `include/` (tests keep it: they run on
Windows 10/11 CI), and the CI workflow runs `tests/check_win7_imports.ps1`,
which inspects the built exe's import table and fails on any Windows 8/10-only
import: it runs `dumpbin /imports` when a Visual Studio toolchain is available
(PATH, the standard install tree, or vswhere), and otherwise falls back to a
byte scan of the PE import-name strings (which only the library-smuggled names
trigger, never the win7_compat.h GetProcAddress literals). That check catches
static-library imports, not just direct source calls.

## What was checked (static, this workspace)

The workspace is Linux, so no Windows 7 runtime was available. Static checks
performed here:

- Source audit: no remaining direct calls of the six APIs outside the shim
  (`grep` over `src/`, `include/`, `tests/`).
- No `std::filesystem` / `<filesystem>` remains in `src/` or `include/`; the
  extended `win7_compat_tests` scan fails otherwise.
- `win7_compat` CTest test: the extended scan (six APIs + `CreateFile2` /
  `CopyFile2` + the `src/`/`include/` filesystem ban) builds and passes on
  Linux; the same target runs in the Windows CI `ctest`.
- Path helper semantics were checked against `std::filesystem` behavior
  (parent_path, stem, extension, lexically_normal) with a standalone harness
  before wiring the Win32 implementations. Two operator/ semantics surfaced
  during the CI run and were corrected there: `pathJoin` must let an absolute
  name (drive, UNC, or root-relative) replace the directory instead of gluing
  the folder in front of it, and `pathNormalize` must keep forward-slash UNC
  hosts (`//server/share`) intact so `pathHost` still sees the server. The
  network-path and PlantUML-in-app test suites cover both.
- Broader audit: no other Windows 8/10-only API calls in the source
  (`GetDpiForMonitor`, `GetSystemTimePreciseAsFileTime`, `SetWindowCompositionAttribute`,
  DPI-context family, appmodel family, `PathCch*`).
- DLL audit from `CMakeLists.txt` `target_link_libraries`: every linked import
  library (`d2d1`, `dwrite`, `d3d11`, `dwmapi`, `windowscodecs`, `urlmon`,
  `winhttp`, `imm32`, `shell32`, `dbghelp`, `ole32`, `comdlg32`, `winspool`,
  `shlwapi`) corresponds to a DLL that ships with Windows 7 SP1.
- `win7_compat` CTest test: `cmake -S . -B cmake-build-linux` +
  `cmake --build cmake-build-linux --target win7_compat_tests` +
  `./cmake-build-linux/win7_compat_tests` passed (pure C++, so it builds and
  runs on Linux; the same target builds in the Windows CI `ctest`).

## Still to verify on real Windows 7 (maintainer)

1. **Startup** on a Windows 7 SP1 x64 VM or machine: run `tinta.exe` and open
   `tests/fixtures/windows7-compat.md`. There must be no "procedure entry
   point" dialog; the window opens and renders.
2. **DPI**: at 96 DPI and 120 DPI system scaling, text and controls scale and
   stay crisp (system-DPI aware).
3. **Core viewer**: headings, tables, lists, emphasis, links, quotes, code,
   math and the Mermaid flowchart in the fixture render in Paper and Midnight
   themes, at normal and narrow widths.
4. **Path handling** (the `CreateFile2` fix): open the updated
   `tests/fixtures/windows7-compat.md` and check the "Path handling" section —
   the relative image, `[[windows7-compat]]` wiki link, file reference and
   `..\` reference resolve as on Windows 10; the PlantUML fence renders when a
   tool is installed and its per-render work directory is removed afterwards.
5. **Platform Update split** (optional): on plain SP1, color emoji and SVG
   images degrade (alt text), and Print/PDF export shows no dialog or a
   graceful failure; with KB2670838 installed, Direct2D 1.1 features return.
6. **Touch** is unavailable on Windows 7 by design (`WM_POINTER*`); mouse and
   keyboard must work.

## Remaining limitations

- Windows 7 has no per-monitor DPI: dragging between mixed-DPI monitors keeps
  the system DPI (same as today's behavior when per-monitor V2 is unavailable).
- Direct2D 1.1 features (print/PDF, SVG, color emoji, custom CJK font fallback)
  need the Platform Update KB2670838; plain SP1 starts with graceful loss.
- MSIX/Store packaging is meaningless on Windows 7; only the portable exe
  route applies. The exe is built as before with the Windows 10 SDK; the
  runtime has no Windows 8/10-only imports.
