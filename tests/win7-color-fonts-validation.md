# Windows 7 color-font fix (the second blank-window report)

## Goal

The first text-params fix (see `win7-text-params-validation.md`) was built
and shipped to the Windows 7 machine. The next diagnostic log — `tinta.log`
from Windows 6.1 build 7601 — showed the app running the fixed build:

    render: keeping default text rendering params (dwrite11=1 hardware=0)
    render: EndDraw failed hr=0x80070057 at stage=end
    render: recreating render target after EndDraw failure (attempt 1 of 3)

`dwrite11=1` means DirectWrite 1.1 (the Platform Update KB2670838) is
present, and `hardware=0` means the D3D11 hardware device creation failed
(`hr=0x887A0004`, `DXGI_ERROR_UNSUPPORTED`), so the app drew on the D2D
software rasterizer. Even with the custom rendering params skipped, every
`EndDraw` still failed with `E_INVALIDARG` from the very first frame, and
the window stayed blank. The text-params theory (DirectWrite 1.0 mode
renumbering) does not apply to this machine — the real culprit is the
color-font drawing option.

## Root cause

Document text runs are drawn with `D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT`
through the `ID2D1DeviceContext`:

```cpp
if (app.deviceContext) {
    app.deviceContext->DrawTextLayout(drawPos, run.layout, app.brush,
        D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
} else {
    app.renderTarget->DrawTextLayout(drawPos, run.layout, app.brush);
}
```

`ID2D1DeviceContext` is only non-null when Direct2D 1.1 is present, which is
exactly this machine (KB2670838). Color glyph runs are a Windows 8.1+
Direct2D feature (`D2D1_DRAW_TEXT_OPTIONS` documentation: "In Windows 8.1
and later, text is rendered using color versions of glyphs"). On the Windows
7 Direct2D **software** rasterizer the flag is accepted but every `EndDraw`
fails with `0x80070057` (`E_INVALIDARG`), so the whole frame is discarded —
blank window, buttons look dead. Every paint in the log draws the document's
first text runs, which is why every paint failed.

## Change

- `include/textparams.h` — `colorfont::enabled(dwrite11, hardwareTarget)`:
  color glyph runs are only requested on DirectWrite 1.1+ hardware render
  targets; everything else draws text through the plain glyph path.
- `include/app.h` — `App::dwrite11` caches the DirectWrite 1.1 detection.
- `src/d2d_init.cpp` — `createRenderTarget` stores the detection and logs
  `render: color fonts off (dwrite11=.. hardware=..)` when the flag is
  skipped, so the next real-machine log confirms the path.
- `src/main_d2d.cpp` — document text runs use `ENABLE_COLOR_FONT` only when
  `app.deviceContext` is present AND `colorfont::enabled(...)` says so;
  otherwise the plain `DrawTextLayout`.
- `src/math_render.cpp` / `include/math_render.h` — `mathBoxDrawTo` takes the
  draw-text options; `mathBoxDraw` chooses them with `colorfont::enabled`.
  `mathToPng` (in `src/export_docx.cpp`) and the `math_layout_tests` harness
  pass the plain options on software targets.
- `tests/colorfont_tests.cpp` + CTest — pure-C++ regression locking in the
  four branches of the decision.
- `tests/fixtures/win7-color-fonts.md` — mixed document (headings, table,
  nested lists, emphasis, links, quotes, fenced code, emoji) that is the
  first content a Windows 7 user should see after this fix.

## What was checked (static, this workspace)

- `colorfont` CTest passes on Linux (pure C++, no Windows headers): color
  fonts only for DWrite 1.1 + hardware; plain glyph path for DWrite 1.0 or
  software targets.
- `textparams` and `win7_compat` CTest still pass: the new code introduces
  no Windows 8/10-only API names and no `<filesystem>` in `src/`/`include/`.
- The Linux-buildable CTest suite (`colorfont`, `textparams`, `win7_compat`,
  `log`, `math_parser`) builds and passes.
- The Windows-only changed files (`src/main_d2d.cpp`, `src/d2d_init.cpp`,
  `src/math_render.cpp`, `src/export_docx.cpp`, `include/math_render.h`,
  `tests/math_layout_tests.cpp`) compile in the Windows CI build; they
  cannot be compiled in this Linux workspace.

## Still to verify on real Windows 7 (maintainer)

1. Start the new build on the Windows 7 SP1 machine and open
   `tests/fixtures/win7-color-fonts.md`. The window must show the tab strip
   and the document; `tinta.log` must contain
   `render: color fonts off (dwrite11=1 hardware=0)` and must not contain
   `EndDraw failed hr=0x80070057` for the normal frame path. Paint lines
   should show `rects=/runs=` growing as the layout streams in.
2. Windows 10/11 behaviour must be unchanged: the hardware render target
   still receives `ENABLE_COLOR_FONT` (color emoji renders in colour), and
   the brief software-rendered startup frames using the plain glyph path
   are not visibly different.

## Remaining limitations

- The exact D2D/DWrite failure mechanism cannot be reproduced in this Linux
  workspace; the fix removes the only Windows-7-specific text drawing state
  in the failing path and adds a diagnostic line, but the real-machine
  verification above is still the confirmation step.
- Color emoji renders monochrome on Windows 7 software targets (no color
  glyph runs there). This matches the documented Windows 8.1+ availability
  of the feature.
- A Windows 7 machine with a working hardware target still requests colour
  fonts (`dwrite11=1 hardware=1`); if a report ever shows EndDraw failing
  there, gate `colorfont::enabled` on the OS version (>= 8.1) as well.
