// Color-font drawing compatibility regression test.
//
// Locks in the rule that D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT is only
// used when DirectWrite 1.1+ is present and the render target is
// hardware-accelerated. Color glyph runs are a Windows 8.1+ Direct2D
// feature; the Windows 7 software rasterizer (the fallback when no D3D11
// device is available) accepts the flag but fails every EndDraw with
// E_INVALIDARG, blanking the window. This is the second Windows 7
// blank-window report: the diagnostic log showed dwrite11=1 hardware=0 and
// EndDraw hr=0x80070057 on every paint even after the custom rendering
// params were limited to hardware targets. Pure C++, runs on any host.
#include "textparams.h"

#include <cassert>

int main() {
    // DirectWrite 1.1 + hardware render target (Windows 10/11): color
    // fonts stay enabled, exactly as before the Windows 7 report.
    assert(colorfont::enabled(true, true));

    // DirectWrite 1.0 (Windows 7 without KB2670838): no color glyph runs,
    // regardless of the render target type.
    assert(!colorfont::enabled(false, true));
    assert(!colorfont::enabled(false, false));

    // Software render target (the Windows 7 fallback when the D3D11 device
    // is unavailable, as the failing log's hardware=0): plain glyph path,
    // even on DirectWrite 1.1. This is the branch that fixes the blank
    // window.
    assert(!colorfont::enabled(true, false));

    return 0;
}
