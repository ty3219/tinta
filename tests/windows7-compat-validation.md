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

## What was checked (static, this workspace)

The workspace is Linux, so no Windows 7 runtime was available. Static checks
performed here:

- Source audit: no remaining direct calls of the six APIs outside the shim
  (`grep` over `src/`, `include/`, `tests/`).
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
4. **Platform Update split** (optional): on plain SP1, color emoji and SVG
   images degrade (alt text), and Print/PDF export shows no dialog or a
   graceful failure; with KB2670838 installed, Direct2D 1.1 features return.
5. **Touch** is unavailable on Windows 7 by design (`WM_POINTER*`); mouse and
   keyboard must work.

## Remaining limitations

- Windows 7 has no per-monitor DPI: dragging between mixed-DPI monitors keeps
  the system DPI (same as today's behavior when per-monitor V2 is unavailable).
- Direct2D 1.1 features (print/PDF, SVG, color emoji, custom CJK font fallback)
  need the Platform Update KB2670838; plain SP1 starts with graceful loss.
- MSIX/Store packaging is meaningless on Windows 7; only the portable exe
  route applies. The exe is built as before with the Windows 10 SDK; the
  runtime has no Windows 8/10-only imports.
