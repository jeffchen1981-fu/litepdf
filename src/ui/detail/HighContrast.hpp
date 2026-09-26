#pragma once
// ui::detail High Contrast helpers shared by every custom-painted component
// (TabManager, StatusBar, FindBar, ResultsPanel, SplitterCore) -- #83.
//
// Under a High Contrast theme each component swaps its hard-coded palette for
// one built from GetSysColor. That palette is a SNAPSHOT: the system colours
// change again when the user moves from one contrast theme to another without
// ever leaving High Contrast, which is why theme_needs_rebuild() answers true
// for every theme message while High Contrast is on.

#include <windows.h>

namespace litepdf::ui::detail {

// True while the OS is running under a High Contrast theme. Checked
// independently of dark/light: High Contrast wins over both.
inline bool is_high_contrast_active() {
    HIGHCONTRASTW hc = {};
    hc.cbSize = sizeof(hc);
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0)) {
        return (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
    }
    return false;
}

// Whether a theme message (WM_SETTINGCHANGE / WM_SYSCOLORCHANGE) must rebuild
// a component's palette and repaint it. A dark/light flip or a High Contrast
// toggle always does. While High Contrast stays on, so does every message:
// the palette holds system colours, and a switch between two contrast themes
// changes them without changing either flag.
constexpr bool theme_needs_rebuild(bool old_dark, bool old_hc,
                                   bool new_dark, bool new_hc) {
    return new_dark != old_dark || new_hc != old_hc || new_hc;
}

}  // namespace litepdf::ui::detail
