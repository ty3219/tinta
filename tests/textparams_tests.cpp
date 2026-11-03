// Text rendering-parameter compatibility regression test.
//
// Locks in the rule that Tinta's custom NATURAL_SYMMETRIC text rendering
// params are only applied when DirectWrite 1.1+ is present and the render
// target is hardware-accelerated. DirectWrite 1.0 (Windows 7 without the
// Platform Update KB2670838) renumbered DWRITE_RENDERING_MODE, so the
// compiled NATURAL_SYMMETRIC value means a ClearType mode there and makes
// the first text draw fail at EndDraw (the Windows 7 blank-window report,
// EndDraw hr=0x80070057 on every paint). Pure C++, runs on any host.
#include "textparams.h"

#include <cassert>

int main() {
    // DirectWrite 1.1 + hardware render target: apply the custom params,
    // exactly as the app did before the Windows 7 report.
    assert(textparams::choose(true, true) ==
           textparams::kCustomNaturalSymmetric);

    // DirectWrite 1.0 (Windows 7 without KB2670838): keep default params
    // regardless of the render target type.
    assert(textparams::choose(false, true) == textparams::kDefaultParams);
    assert(textparams::choose(false, false) == textparams::kDefaultParams);

    // Software render target (the Windows 7 fallback when the D3D11 device
    // is unavailable): keep default params even on DirectWrite 1.1.
    assert(textparams::choose(true, false) == textparams::kDefaultParams);

    return 0;
}
