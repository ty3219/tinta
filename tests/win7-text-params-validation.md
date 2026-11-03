# Windows 7 text-params fix (the blank-window report)

## Goal

A Windows 7 user reported that `tinta.exe` starts but the window stays
blank and the on-screen buttons do not react. The on-disk diagnostic log
added for that report now pinpoints the failing stage: every `WM_PAINT` —
including the first, which draws only the tab strip — ended with

    render: EndDraw failed hr=0x80070057

`0x80070057` is `E_INVALIDARG`. Direct2D surfaces drawing errors at
`EndDraw`, and the only text-state the failing frames set that differs
between Windows 7 and Windows 10/11 is the custom DirectWrite rendering
params in `createRenderTarget`:

```cpp
app.dwriteFactory->CreateCustomRenderingParams(
    ..., DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, &customParams);
app.renderTarget->SetTextRenderingParams(customParams);
```

DirectWrite renumbered `DWRITE_RENDERING_MODE` when 1.1 shipped (Windows 8
and the Windows 7 Platform Update KB2670838): the old (1.0) enum had
`CLEARTYPE_NATURAL_SYMMETRIC` at the numeric value that 1.1+ calls
`NATURAL_SYMMETRIC`. The app compiles against the Windows 10 SDK, so the
value it passes to DirectWrite 1.0 (plain Windows 7 SP1) is a different
mode than intended. Applied on the D2D 1.0 software rasterizer (the
machine's D3D11 hardware device failed with `0x887A0004`, so the software
render target is the one used), those params make the first text draw
fail, `EndDraw` reports the error, and every subsequent frame fails the
same way — the blank window.

## Change

- `include/textparams.h` — a pure decision helper: custom params only on
  DirectWrite 1.1+ hardware-accelerated render targets.
- `src/d2d_init.cpp` — `createRenderTarget` detects DirectWrite 1.1 via an
  `IDWriteFactory1` `QueryInterface` and applies the custom params only
  when `textparams::choose(dwrite11, app.useHardwareRT)` says so;
  otherwise it logs `render: keeping default text rendering params`.
- `src/main_d2d.cpp` — `render()` tracks the section being drawn
  (`document`, `tab-strip`, `chrome`, ...) and the EndDraw-failure log now
  names it. A failed `EndDraw` also recreates the render target up to
  three times (bounded device-reset) instead of leaving a permanently
  blank window, and the failure counter resets on a successful frame.
- `tests/textparams_tests.cpp` — pure-C++ CTest locking in the four
  branches of the decision.
- `tests/fixtures/win7-text-params.md` — mixed document (headings, table,
  nested lists, emphasis, links, quotes, fenced code) that is the first
  content a Windows 7 user should see after this fix.

## What was checked (static, this workspace)

- `textparams` CTest passes on Linux (pure C++, no Windows headers): custom
  params only for DWrite 1.1 + hardware; default params for DWrite 1.0 or
  software targets.
- `win7_compat` scan still passes: `src/d2d_init.cpp` gains an
  `IDWriteFactory1` interface query, not a static import of any Windows
  8/10-only API; no `<filesystem>` anywhere in `src/` or `include/`.
- `diagnostic_log` CTest passes (the log module is unchanged).
- The full CTest suite builds and runs on Linux.

## Still to verify on real Windows 7 (maintainer)

1. Start the new build on the Windows 7 SP1 machine and open
   `tests/fixtures/win7-text-params.md`. The window must show the tab
   strip and the document; `tinta.log` must not contain
   `EndDraw failed hr=0x80070057` for the normal frame path, and the paint
   lines should show `rects=/runs=` growing as the layout streams in.
2. If an `EndDraw` failure still occurs, its log line now names the stage
   (`... at stage=tab-strip`) and the render target is recreated up to
   three times — send that log back and the offending section is known.
3. Windows 10/11 behaviour must be unchanged: the hardware render target
   still receives the custom params (DWrite 1.1 is always present there),
   and the brief software-rendered startup frames using default params are
   not visibly different.

## Remaining limitations

- The exact D2D/DWrite failure mechanism cannot be reproduced in this
  Linux workspace; the fix removes the only Windows-7-specific text state
  in the failing path and adds diagnostics, but the real-machine
  verification above is still the confirmation step.
- The bounded render-target recreation is a safety net, not a fix for a
  persistent invalid draw command; if one remains, the stage log names it
  for the next iteration.
