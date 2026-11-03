# On-disk diagnostic log (#port)

## Goal

A Windows 7 user reported that `tinta.exe` starts and shows the main
interface, but opening a Markdown file changes nothing - no content appears
and the on-screen buttons do not react. The workspace is Linux, so the Win7
runtime cannot be reproduced here; instead Tinta now writes a diagnostic log
that captures the exact trail on the user's machine, so the maintainer (or a
user) can read what actually happened instead of guessing.

## Change

New module `include/log.h` / `src/log.cpp`:

- `tintaLog(fmt, ...)` appends one timestamped line to `tinta.log` in the
  directory the app was started in ("current directory" at launch). The
  path is resolved on the first call, before `parkWorkingDirectory()` moves
  the process into the system folder, so the file lands next to the exe or
  in the user's working folder - not in `C:\Windows\System32`.
- The file is truncated per launch (`CREATE_ALWAYS`), flushed after every
  line, serialized by a mutex (worker threads log safely), and lines also go
  to `OutputDebugStringA`. If the launch folder is read-only the module
  falls back to the system temp folder; if that fails too it degrades to
  debugger output only.
- `tintaLogPath()` / `tintaLogSetPath()` / `tintaLogReset()` are the test
  hooks used by `tests/log_tests.cpp`.

Instrumented call sites:

| Area | What is logged |
| --- | --- |
| `src/main_d2d.cpp` startup | Version, pid, real OS version (RtlGetVersion), DPI awareness, log path, settings summary, command line, window class/window, DPI, D2D init, render target, document load result, tab count, show, message-loop start/exit/error, crash filter entry, D3D11 warm-up failure |
| `src/main_d2d.cpp` WndProc | WM_PAINT (first 40 + every 100th, with render-target/root/layout/rect/run counts), WM_LBUTTONDOWN/UP coordinates and overlay state, WM_COPYDATA, WM_DROPFILES, WM_APP_GPU_READY, WM_APP_LAYOUT_CHUNK completion, WM_DESTROY |
| `src/main_d2d.cpp` render() | Early return without a render target; `EndDraw` failures, now naming the drawing stage (`stage=document`, `stage=tab-strip`, ...) and a bounded render-target recreation after a failed `EndDraw` |
| `src/d2d_init.cpp` | `D2D1CreateFactory` / `DWriteCreateFactory` / WIC / `CreateHwndRenderTarget` / brush `HRESULT` failures |
| `src/input.cpp` | `openDocumentInViewer` open/read/parse failures, `openDocumentSourceInViewer` success (path, bytes, blocks, parse time), drop count, app-menu and tab-strip hit decisions |
| `src/tabs.cpp` | `tabOpenPath` open/cannot-read, `tabActivate` read failure and activation |
| `src/editor.cpp` | File > Open dialog invoked / cancelled / picked path |

## What was checked (static, this workspace)

- `tests/log_tests.cpp` (new, pure C++, runs on Linux and Windows CI):
  verifies the `[t=...]` line format with format arguments, that later lines
  append instead of truncating, that `tintaLogReset` + `tintaLogSetPath`
  repoint to a second file, and that 8 threads logging 25 lines each lose no
  line. Runs as CTest `diagnostic_log`.
- `tests/win7_compat_tests.cpp` still passes: `src/log.cpp` uses only
  Windows 7 APIs (`GetCurrentDirectoryW`, `GetTempPathW`, `CreateFileW`
  via `std::ofstream`, `MultiByteToWideChar`, ...), no `std::filesystem`,
  and no Windows 8/10-only import names.
- `tests/fixtures/diagnostic-log.md` was added: a mixed document (headings,
  table, ordered/unordered nested lists, emphasis, link, quote, fenced code,
  PowerShell) that documents the feature and confirms the log trail belongs
  to a document that still renders normally.

## Still to verify on real Windows 7 / Windows 10 (maintainer)

1. Launch from a folder, open `tests/fixtures/diagnostic-log.md`, then close
   Tinta. A `tinta.log` must appear in the launch folder (or the temp folder
   when that one is read-only) whose first lines name the Windows version and
   the log path, and whose file-open lines show the fixture loaded and
   parsed.
2. Reproduce the reported failure: the log then shows which stage stopped -
   no `message loop started` (startup hang), no `WM_LBUTTONDOWN`
   (mouse events never arrive), `paint #...: rt=0` (render target missing),
   `EndDraw failed` (D2D draw error), or a missing `document parsed` /
   `viewer: opened` line (the open never reached the viewer).
3. A normal session must not visibly slow down: the log writes one small
   line per event, paints are throttled, and there is no per-frame logging.
4. Confirm the fixture renders (the existing render harnesses on Windows CI
   already cover the Markdown mix; run the built app once with `--stats` on
   Windows 10 to see the document counts still match).

## Remaining limitations

- The log is best-effort: a hung or crashed process may not flush the last
  lines, and the temp-folder fallback path is only as discoverable as the
  user's temp folder.
- `GetMessage` failure and unhandled-exception lines are only written when
  the crash handler can still run the log mutex (not during a hard stack
  overflow).
- Per-frame draw-call counts are not logged (only layout counts at throttled
  paints); a render that succeeds but draws nothing is narrowed down by the
  paint log's `rects=/runs=` numbers plus the `document parsed` block count.
