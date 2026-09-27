#pragma once
// ui::detail ThumbnailPane palette (#90). Lives in a header, not in
// ThumbnailPane.cpp's anonymous namespace, so the unit tests can check the
// High Contrast colours without creating the pane.

#include <windows.h>

namespace litepdf::ui::detail {

struct ThumbnailPalette {
    COLORREF pane_bg;        // pane background (around tiles)
    COLORREF tile_fill;      // placeholder rectangle fill
    COLORREF tile_border;    // placeholder rectangle outline
    COLORREF text;           // page-number label color
    COLORREF current_border; // current-page highlight border; CLR_INVALID
                             // means "the DWM accent, read at paint time"
};

// Under High Contrast every colour comes from the system: the tiles sit on
// the window colour, outlined and labelled in the window text colour, and the
// current page is framed in the highlight colour. Outside it the current-page
// frame is the DWM accent, which is left to paint time (CLR_INVALID): DWM
// reports black under High Contrast, so an accent frozen in at construction
// or at a theme change can stay black for the whole session (#90, as #88
// found for the tab strip).
inline ThumbnailPalette make_thumbnail_palette(bool dark, bool high_contrast) {
    if (high_contrast) {
        return {
            /*pane_bg*/        GetSysColor(COLOR_WINDOW),
            /*tile_fill*/      GetSysColor(COLOR_WINDOW),
            /*tile_border*/    GetSysColor(COLOR_WINDOWTEXT),
            /*text*/           GetSysColor(COLOR_WINDOWTEXT),
            /*current_border*/ GetSysColor(COLOR_HIGHLIGHT),
        };
    }
    if (dark) {
        return {
            /*pane_bg*/        RGB(0x1F, 0x1F, 0x1F),
            /*tile_fill*/      RGB(0x2D, 0x2D, 0x2D),
            /*tile_border*/    RGB(0x55, 0x55, 0x55),
            /*text*/           RGB(0xE0, 0xE0, 0xE0),
            /*current_border*/ CLR_INVALID,
        };
    }
    return {
        /*pane_bg*/        RGB(0xF5, 0xF5, 0xF5),
        /*tile_fill*/      RGB(0xFF, 0xFF, 0xFF),
        /*tile_border*/    RGB(0xC8, 0xC8, 0xC8),
        /*text*/           RGB(0x30, 0x30, 0x30),
        /*current_border*/ CLR_INVALID,
    };
}

}  // namespace litepdf::ui::detail
