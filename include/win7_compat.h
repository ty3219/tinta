#pragma once
// Windows 7 compatibility shims.
//
// Tinta's portable exe must start on Windows 7 SP1. The APIs below were added
// after Windows 7, so a static import of any of them makes the loader refuse
// to start the process ("procedure entry point ... not located in ..."). Each
// shim resolves the symbol at runtime and falls back to a legacy equivalent;
// on Windows 10/11 the modern symbol exists, so behavior is unchanged there.
//
// The regression scan tests/win7_compat_tests.cpp forbids naming these APIs
// anywhere else in the source tree, so new code must go through these shims.
#include <windows.h>

// Values for target SDKs that predate the declarations (stable public ABI).
#ifndef APPMODEL_ERROR_NO_PACKAGE
#define APPMODEL_ERROR_NO_PACKAGE 0x1E2L
#endif
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((HANDLE)(INT_PTR)-4)
#endif

namespace win7compat {
namespace detail {
// The system DLLs below are always loaded in a GUI process; GetModuleHandleW
// is enough and avoids pinning them.
inline FARPROC load(const wchar_t* module, const char* symbol) {
    HMODULE h = GetModuleHandleW(module);
    return h ? GetProcAddress(h, symbol) : nullptr;
}
}  // namespace detail

// GetDpiForWindow (Windows 10 1607+). Fallback: the DPI of the window's
// device context, which is the system DPI a system-DPI-aware process sees
// on Windows 7.
inline UINT windowDpi(HWND hwnd) {
    static FARPROC sFn =
        detail::load(L"user32.dll", "GetDpiForWindow");
    if (sFn) {
        using Fn = UINT(WINAPI*)(HWND);
        UINT dpi = reinterpret_cast<Fn>(sFn)(hwnd);
        if (dpi) return dpi;
    }
    UINT dpi = 96;
    if (HDC dc = GetDC(hwnd)) {
        int v = GetDeviceCaps(dc, LOGPIXELSX);
        if (v > 0) dpi = (UINT)v;
        ReleaseDC(hwnd, dc);
    }
    return dpi;
}

// GetSystemMetricsForDpi (Windows 10 1607+). Fallback: GetSystemMetrics,
// which already returns DPI-scaled values for a DPI-aware process.
inline int systemMetricsForDpi(int nIndex, UINT dpi) {
    static FARPROC sFn =
        detail::load(L"user32.dll", "GetSystemMetricsForDpi");
    if (sFn) {
        using Fn = int(WINAPI*)(int, UINT);
        return reinterpret_cast<Fn>(sFn)(nIndex, dpi);
    }
    return GetSystemMetrics(nIndex);
}

// SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)
// is Windows 10 1703+. Fallback: system-DPI awareness via SetProcessDPIAware
// (Vista+), which is all Windows 7 offers.
inline void setPerMonitorV2DpiAwareness() {
    static FARPROC sFn =
        detail::load(L"user32.dll", "SetProcessDpiAwarenessContext");
    if (sFn) {
        using Fn = BOOL(WINAPI*)(HANDLE);
        if (reinterpret_cast<Fn>(sFn)(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
            return;
        }
    }
    SetProcessDPIAware();
}

// GetCurrentPackageFullName is Windows 8+. Windows 7 has no packaged apps,
// so the fallback reports "not packaged".
inline bool runningAsPackagedApp() {
    static FARPROC sFn =
        detail::load(L"kernel32.dll", "GetCurrentPackageFullName");
    if (sFn) {
        using Fn = LONG(WINAPI*)(UINT32*, PWSTR);
        UINT32 len = 0;
        return reinterpret_cast<Fn>(sFn)(&len, nullptr) !=
               APPMODEL_ERROR_NO_PACKAGE;
    }
    return false;
}

// GetPointerType / GetPointerInfo are Windows 8+ and only ever arrive with
// WM_POINTER* messages (also Windows 8+), so the fallback is "not a pointer
// message": on Windows 7 touch input never reaches these paths.
inline bool pointerType(UINT32 pointerId, POINTER_INPUT_TYPE* type) {
    static FARPROC sFn =
        detail::load(L"user32.dll", "GetPointerType");
    if (!sFn) return false;
    using Fn = BOOL(WINAPI*)(UINT32, POINTER_INPUT_TYPE*);
    return reinterpret_cast<Fn>(sFn)(pointerId, type) != 0;
}

inline bool pointerInfo(UINT32 pointerId, POINTER_INFO* info) {
    static FARPROC sFn =
        detail::load(L"user32.dll", "GetPointerInfo");
    if (!sFn) return false;
    using Fn = BOOL(WINAPI*)(UINT32, POINTER_INFO*);
    return reinterpret_cast<Fn>(sFn)(pointerId, info) != 0;
}

}  // namespace win7compat
