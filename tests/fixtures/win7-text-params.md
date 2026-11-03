# Windows 7 text rendering compatibility check

This document exercises the viewer on Windows 7, where the first text draw
used to fail at `EndDraw` (HRESULT `0x80070057`) and left the window blank.
The failure came from Tinta's custom DirectWrite rendering params: the
`DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC` value compiled against the
Windows 10 SDK means a different mode on DirectWrite 1.0 (Windows 7
without the Platform Update KB2670838). Those params are now only applied
to hardware-accelerated render targets on DirectWrite 1.1+; a Windows 7
software render target keeps the default ClearType params and draws text
normally.

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

## The text that used to fail

The very first frame draws the title-bar tab strip and this document's
first block. Before the fix, that frame's `EndDraw` failed with
`0x80070057` on every paint, so nothing ever appeared and the on-screen
buttons looked dead. With the fix, the text below is the first thing a
Windows 7 user sees:

> Text draws on the software rasterizer without the custom
> `NATURAL_SYMMETRIC` params — default ClearType params instead.

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

1. Custom text rendering params apply only on DirectWrite 1.1+ hardware
   render targets (`include/textparams.h` decides; `src/d2d_init.cpp`
   applies).
2. A paint that fails `EndDraw` logs the *stage* it was drawing (for
   example `tab-strip` or `document`) and recreates the render target a
   bounded number of times instead of staying blank.
3. `tests/textparams_tests.cpp` locks the decision in as a CTest.

| Step | Result |
| :--- | :--- |
| Launch | Window opens, tab strip and title render |
| Open this file | Headings, table, lists, emphasis, links, quotes and code appear |
| Narrow the window | The column re-flows; nothing goes blank |
| Switch theme | Text still draws in the new theme's fonts and colours |

The fixture document itself is ordinary Markdown — the compatibility work
is in the render setup, not the parser — so this page also serves as the
first Windows 7 render smoke test after a build.
