# Windows 7 color-font drawing compatibility check

This document exercises the viewer on Windows 7, where document text was
drawn through the color-font path (`D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT`)
and every `EndDraw` failed with `0x80070057` on the software rasterizer, so
the window stayed blank and looked frozen. Color glyph runs are a
Windows 8.1+ Direct2D feature; Tinta now requests them only on
DirectWrite 1.1+ hardware-accelerated render targets. A Windows 7 software
render target draws the same text through the plain glyph path.

## What the window should show

| Feature | Expected on Windows 7 |
| :--- | :--- |
| Headings | # to ### render in theme heading sizes and colours |
| Table | Columns align; the copy button appears on hover |
| Lists | Nested bullets and a numbered list keep their indents |
| Emphasis | *italic*, **bold**, ~~struck~~ and ==highlight== differ |
| Links | `[links](#what-the-window-should-show)` are tinted and clickable |
| Quotes | Keep their tint and spacing, inline markup included |
| Code | Fenced blocks scroll; inline code stays literal |
| Emoji | Segoe UI Emoji glyphs appear (monochrome on Windows 7) |

## The text that used to fail

Before the fix, the document's first text runs were drawn with the
`ENABLE_COLOR_FONT` option. The Windows 7 software rasterizer failed every
`EndDraw` with `0x80070057` from the very first frame, so neither this
paragraph, the tab strip, nor any on-screen button ever appeared. With the
fix, the text below is the first thing a Windows 7 user sees:

> Text draws on the software rasterizer through the plain glyph path, so
> `EndDraw` succeeds and the window comes up normally.

## A mixed paragraph

Markdown mixes *emphasis*, **bold**, ~~struck~~, ==highlight==, `inline
code` and a [link](#a-mixed-paragraph) in one paragraph. A table, a quote
and a fenced code block surround it, so a render regression in any of
those neighbours would show up here too.

```c
// Code blocks, like everything else, rely on the same text pipeline.
HRESULT hr = target->EndDraw();
if (FAILED(hr)) {
    /* Before the fix this returned E_INVALIDARG on Windows 7. */
}
```

## What was actually changed

1. Color glyph runs (`D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT`) apply
   only on DirectWrite 1.1+ hardware render targets
   (`include/textparams.h` decides; `src/main_d2d.cpp` and
   `src/math_render.cpp` apply it).
2. A software render target draws document and math text through the plain
   glyph path — same layout, monochrome emoji.
3. `tests/colorfont_tests.cpp` locks the decision in as a CTest.

| Step | Result |
| :--- | :--- |
| Launch | Window opens, tab strip and title render |
| Open this file | Headings, table, lists, emphasis, links, quotes and code appear |
| Narrow the window | The column re-flows; nothing goes blank |
| Switch theme | Text still draws in the new theme's fonts and colours |

The fixture document itself is ordinary Markdown — the compatibility work
is in the render setup, not the parser — so this page also serves as the
first Windows 7 render smoke test after a build.
