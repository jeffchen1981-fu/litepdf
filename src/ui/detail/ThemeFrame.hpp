#pragma once
// ui::detail helper that repaints a bordered control's frame once the control
// has picked up a new visual style (#89).
//
// A WS_EX_CLIENTEDGE border (EDIT, ListView, TreeView) is non-client and is
// drawn by the visual style. When High Contrast is turned off, such a control
// repaints that frame after WM_SYSCOLORCHANGE while it still holds the
// contrast theme, which gives a flat, square edge in the wrong colours, and
// only afterwards receives WM_THEMECHANGED and reopens the normal theme.
// Nothing repaints the frame after that, so the flat edge stays until
// something else happens to redraw it. Measured on controls created while
// High Contrast was on.
//
// The components' own theme arms cannot fix this: they run on
// WM_SETTINGCHANGE, which arrives before WM_THEMECHANGED, so an RDW_FRAME
// there repaints with the old theme (tried and reverted in PR #88). This
// subclass repaints after the control's own WM_THEMECHANGED handling, the
// first point at which the new theme is in place.

#include <windows.h>
#include <commctrl.h>

namespace litepdf::ui::detail {

inline constexpr UINT_PTR kThemeFrameSubclassId = 0x8900;

inline LRESULT CALLBACK theme_frame_subclass(HWND hwnd, UINT msg, WPARAM w,
                                             LPARAM l, UINT_PTR id,
                                             DWORD_PTR /*ref_data*/) {
    switch (msg) {
        case WM_THEMECHANGED: {
            const LRESULT result = DefSubclassProc(hwnd, msg, w, l);
            RedrawWindow(hwnd, nullptr, nullptr,
                         RDW_FRAME | RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
            return result;
        }
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, theme_frame_subclass, id);
            break;
    }
    return DefSubclassProc(hwnd, msg, w, l);
}

// Install on every control created with WS_EX_CLIENTEDGE. It coexists with
// the control's other subclasses: each one is keyed by its own procedure.
inline void repaint_frame_on_theme_change(HWND control) {
    if (control) {
        SetWindowSubclass(control, theme_frame_subclass,
                          kThemeFrameSubclassId, 0);
    }
}

}  // namespace litepdf::ui::detail
