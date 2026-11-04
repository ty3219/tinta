#pragma once
// Text rendering compatibility between DirectWrite 1.0 and 1.1 on Windows 7.
//
// Tinta's custom text rendering params request DWRITE_RENDERING_MODE_
// NATURAL_SYMMETRIC, a mode that exists in DirectWrite 1.1+ (Windows 8
// and later, and the Windows 7 Platform Update KB2670838). DirectWrite 1.0
// renumbered the DWRITE_RENDERING_MODE values, so the NATURAL_SYMMETRIC
// constant compiled against the Windows 10 SDK means a different mode on
// DirectWrite 1.0. Applied on the Windows 7 software rasterizer, those
// params made every text draw fail: EndDraw reported E_INVALIDARG on each
// paint and the window stayed blank (the Windows 7 blank-window report).
// The custom params are therefore only applied when DirectWrite 1.1+ is
// present AND the render target is hardware-accelerated; everything else
// keeps the render target's default text rendering params.
namespace textparams {
enum Choice { kDefaultParams = 0, kCustomNaturalSymmetric = 1 };
inline Choice choose(bool dwrite11, bool hardwareTarget) {
    return (dwrite11 && hardwareTarget) ? kCustomNaturalSymmetric
                                        : kDefaultParams;
}
}  // namespace textparams

namespace colorfont {
// Color glyph runs (D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT) are a
// Windows 8.1+ Direct2D feature. On Windows 7 the Direct2D software
// rasterizer - the fallback when no D3D11 device is available - accepts
// the flag but fails every EndDraw with E_INVALIDARG, which is the second
// Windows 7 blank-window report (EndDraw hr=0x80070057 on every paint,
// even with the custom rendering params fix already in place). Color
// fonts are therefore only requested when DirectWrite 1.1+ is present AND
// the render target is hardware-accelerated; everywhere else text draws
// with the plain glyph path (monochrome emoji, same layout).
inline bool enabled(bool dwrite11, bool hardwareTarget) {
    return dwrite11 && hardwareTarget;
}
}  // namespace colorfont
