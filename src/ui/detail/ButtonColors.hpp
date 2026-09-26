#pragma once
// ui::detail::button_colors -- the background/foreground pair for one of the
// owner-drawn buttons in FindBar and ResultsPanel (the latch toggles and the
// close button).
//
// Each state carries its own foreground. The light and dark palettes use one
// text colour on every background, but a High Contrast palette cannot: a
// latched button sits on COLOR_HIGHLIGHT and needs COLOR_HIGHLIGHTTEXT, which
// is unreadable on the COLOR_BTNFACE of an idle one (#83).

#include <windows.h>

namespace litepdf::ui::detail {

struct ButtonColors {
    COLORREF bg;
    COLORREF fg;
};

// `Palette` is FindBar's or ResultsPanel's file-local palette; both carry the
// btn_* and close_hover_* fields named here. Precedence is unchanged from the
// single-foreground version: a hovered close button first, then pressed or
// latched, then hovered, then idle.
template <class Palette>
constexpr ButtonColors button_colors(const Palette& pal, bool is_close,
                                     bool hover, bool pressed, bool latched) {
    if (is_close && hover) return {pal.close_hover_bg, pal.close_hover_fg};
    if (pressed || latched) return {pal.btn_pressed, pal.btn_pressed_fg};
    if (hover) return {pal.btn_hover, pal.btn_hover_fg};
    return {pal.btn_normal, pal.btn_fg};
}

}  // namespace litepdf::ui::detail
