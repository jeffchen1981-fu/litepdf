#pragma once
// ui::detail::SplitterCore — shared implementation for Splitter (horizontal)
// and the future VerticalSplitter (vertical). Phase 7 D9 prerequisite (Task 4a).
//
// Beck's two-step rationale: the horizontal Splitter (in production, behavior
// frozen) and the upcoming VerticalSplitter (Phase 7 T4b, separate TU) share
// identical hover/capture/theme bookkeeping and only differ on a small set of
// orientation-dependent decisions (cursor, drag axis). By declaring SplitterCore
// + Orientation here, both .cpp files can include this header and compose the
// shared core without the vertical class having to re-derive any of the
// WndProc dispatch logic.
//
// Implementation of `handle_message` lives in `src/ui/Splitter.cpp` (file
// scope, namespace-qualified definition). VerticalSplitter.cpp will include
// this header but reuse the same out-of-line definition via the linker —
// there is exactly one symbol for the dispatcher.
//
// Theme helpers (make_palette / detect_dark_mode) are declared inline here so
// they are visible from the cross-TU dispatcher. Pre-refactor they lived in
// Splitter.cpp's anonymous namespace; that worked when the dispatcher was
// also file-local, but the shared dispatcher needs them visible from both
// Splitter.cpp and the future VerticalSplitter.cpp. TODO phase-6.x:
// consolidate with FindBar/TabManager Palettes into ui/Theme.hpp.

#include <windows.h>
#include <dwmapi.h>

#include <functional>

namespace litepdf::ui::detail {

enum class Orientation { Horizontal, Vertical };

struct Palette {
    COLORREF bar_bg;
    COLORREF bar_hover;
};

inline Palette make_palette(bool dark) {
    if (dark) {
        return {
            /*bar_bg*/    RGB(0x3A, 0x3A, 0x3A),
            /*bar_hover*/ RGB(0x50, 0x50, 0x50),
        };
    }
    return {
        /*bar_bg*/    RGB(0xD8, 0xD8, 0xD8),
        /*bar_hover*/ RGB(0xB8, 0xB8, 0xB8),
    };
}

inline bool detect_dark_mode(HWND hwnd) {
    BOOL dark = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd,
            DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark)))) {
        if (dark) return true;
    }
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_READ, &hk) == ERROR_SUCCESS) {
        DWORD val = 1, cb = sizeof(val);
        LONG r = RegQueryValueExW(hk, L"AppsUseLightTheme", nullptr, nullptr,
                                  reinterpret_cast<LPBYTE>(&val), &cb);
        RegCloseKey(hk);
        if (r == ERROR_SUCCESS) return val == 0;
    }
    return false;
}

// Drag callback signature. The horizontal Splitter posts new height-from-
// bottom (px); the vertical splitter will post new width-from-left (px).
// Both pass an int, so a single std::function<void(int)> serves both.
using OnDrag = std::function<void(int new_extent_px)>;

struct SplitterCore {
    HWND hwnd = nullptr;

    // Drag gesture state. `dragging` mirrors GetCapture()==hwnd for clarity
    // and so we don't repeatedly call the WinAPI during WM_MOUSEMOVE.
    bool dragging = false;
    bool hover    = false;

    OnDrag on_drag;

    bool        dark_mode = false;
    Palette     palette   = make_palette(false);
    Orientation orient    = Orientation::Horizontal;

    // Dispatches WndProc messages. Returns the LRESULT for handled messages
    // (or whatever DefWindowProcW returns for unhandled ones). The static
    // WndProc in each splitter .cpp retrieves a SplitterCore* from
    // GWLP_USERDATA and forwards here.
    LRESULT handle_message(HWND hwnd_in, UINT msg, WPARAM w, LPARAM l);
};

}  // namespace litepdf::ui::detail
