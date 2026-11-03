# Diagnostic log check

This document exercises the viewer while the on-disk diagnostic log is
active. Tinta writes `tinta.log` in the directory it was started in, so a
user who sees a problem can paste that file instead of describing it. This
document mixes the usual Markdown features so a maintainer can confirm that
the log trail (startup, file open, paints, clicks) lines up with a document
that renders as expected — and that the log itself does not disturb
rendering.

## Where the log goes

| Fact | Value |
| :--- | :--- |
| File name | `tinta.log` |
| Location | The directory Tinta was launched from ("current directory") |
| Freshness | Truncated on every launch — one session per file |
| Fallback | System temp folder when the launch folder is read-only |

## What it records

1. Startup: version, Windows build, DPI, settings, window, D2D/render target.
2. Document opens: path, byte count, parsed block count and parse time.
3. Paints: a few leading frames plus every 100th, with layout counts.
4. Input: mouse-down/up coordinates and the hit-test outcome.
5. Errors: `HRESULT` failures and any unhandled exception.

> **Hint** — a log line looks like
> `[t=1234 tid=5678] file loaded: notes.md (42 bytes)`. The `t=` prefix is
> milliseconds since process start, so the trail reads top to bottom in
> time order even when worker threads log in between.

## The document still renders

Every element below must appear exactly as on Windows 10: *italic*,
**bold**, ~~struck~~, ==highlight==, `inline code`, a [link](#what-it-records),
and a code block:

```rust
fn log_line(fmt: &str) {
    // The log is plain UTF-8 text, so CJK and emoji appear verbatim.
    eprintln!("tinta.log ← {}", fmt);
}
```

- Nested lists, first level.
  - Second level keeps its indent.
    1. Ordered inside an unordered list.
    2. Still numbered correctly.

| Feature | Expected |
| :--- | :--- |
| Headings | # to ### sizes and theme colours apply |
| Table | Columns align and the copy button appears |
| Emphasis | Bold, italic, struck, highlight differ |
| Quote | Keeps its tint, spacing and inline markup |
| Code | Fenced block scrolls; inline code stays literal |

## Launching for diagnosis

```powershell
# From the folder of the file you are about to open:
tinta.exe .\diagnostic-log.md
# Then reproduce the problem, close Tinta, and send tinta.log along.
```

The first lines should name the Windows version and the log path itself:

    [t=0 tid=1234] tinta v3.9.6 started (pid=5678), Windows 6.1 (build 7601), dpiAware=1
    [t=0 tid=1234] log file: C:\Users\me\Documents\tinta.log
    [t=12 tid=1234] window created: hwnd=0x00200BC 1024x768 at 100,100
    [t=20 tid=1234] file loaded: C:\Users\me\Documents\diagnostic-log.md (1234 bytes)
    [t=21 tid=1234] document parsed: bytes=1234 blocks=7 parseTimeUs=1500
