#pragma once

// PR-B: pure logic behind ui::StatusBar -- input parsing, child geometry and
// the box-overwrite rule. Deliberately free of <windows.h> so the whole
// decision surface of the status bar is unit-testable without a window, the
// same split ScrollMath/CompletionMath/SplitterMath use.

#include <cmath>
#include <optional>
#include <string>
#include <string_view>

namespace litepdf::ui::detail {

// Parse the go-to-page box into a ZERO-BASED page index.
//
// UI is 1-based, internals are 0-based, and this is the single place the
// conversion happens.
//
// Rejects: empty or whitespace-only text, any non-digit character, 0, anything
// above page_count, and any digit string long enough to overflow. The range
// check is NOT belt-and-braces: DocumentView::set_current_page CLAMPS its
// argument, so an accepted out-of-range page would silently jump to the first
// or last page instead of being refused. ES_NUMBER stops non-digits from the
// keyboard but not from a paste, so the character check is equally real.
inline std::optional<int> parse_page_input(std::wstring_view text,
                                           int page_count) noexcept {
    if (page_count <= 0) return std::nullopt;

    // Trim ASCII spaces and tabs; a paste can carry them on either end.
    while (!text.empty() && (text.front() == L' ' || text.front() == L'\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == L' ' || text.back() == L'\t')) {
        text.remove_suffix(1);
    }
    if (text.empty()) return std::nullopt;

    long long value = 0;
    for (const wchar_t c : text) {
        if (c < L'0' || c > L'9') return std::nullopt;
        value = value * 10 + static_cast<long long>(c - L'0');
        // Bail the moment the accumulator passes the document, which doubles as
        // the overflow guard: no input can drive `value` past page_count and
        // then wrap back into range, because the loop stops at the crossing.
        if (value > page_count) return std::nullopt;
    }
    if (value < 1) return std::nullopt;
    return static_cast<int>(value) - 1;
}

// Pixel geometry of the three children inside the bar's client rect.
struct StatusBarChildRects {
    int edit_x = 0, edit_y = 0, edit_w = 0, edit_h = 0;
    int label_x = 0, label_y = 0, label_w = 0, label_h = 0;
    int zoom_x = 0, zoom_y = 0, zoom_w = 0, zoom_h = 0;
};

// Lay the page box, the "/ N" label and the zoom readout out left to right
// with a uniform padding, all vertically centred in a bar `bar_h` pixels tall.
// Callers pass pixel values already scaled for DPI, so this stays pure
// arithmetic.
//
// The two labels take the padded strip, bar_h - 2 * pad_px; they are
// SS_CENTERIMAGE, so their text centres itself in whatever height they get.
// The page box cannot do that (#113): a single-line EDIT draws its line from
// the top of its client area and clips whatever does not fit, and the padded
// strip is SHORTER than one line of the bar's own font -- at 96 DPI a 10 px
// client area under a 15 px line, so the bottom third of every digit was cut.
// So the box takes `edit_h_px`, the window height its font needs (line height
// plus frame, measured by the caller), centred on the same axis as the labels.
// It is clamped to the bar so it can never overhang it; `edit_h_px <= 0`
// (the font could not be measured) falls back to the padded strip.
//
// The readout starts after the label's RECTANGLE, not after its text: the
// label is a fixed width, so a short "/ 12" leaves a gap. Closing it would
// mean measuring text, which this fixed-offset layout deliberately avoids.
inline StatusBarChildRects status_bar_child_rects(int bar_h, int pad_px,
                                                  int edit_w_px,
                                                  int edit_h_px,
                                                  int label_w_px,
                                                  int zoom_w_px) noexcept {
    const int ctrl_h = (bar_h > 2 * pad_px) ? (bar_h - 2 * pad_px) : bar_h;
    const int y      = (bar_h - ctrl_h) / 2;

    int box_h = (edit_h_px > 0) ? edit_h_px : ctrl_h;
    if (box_h > bar_h) box_h = bar_h;
    const int box_y = (bar_h - box_h) / 2;

    StatusBarChildRects r;
    r.edit_x  = pad_px;
    r.edit_y  = (box_y > 0) ? box_y : 0;
    r.edit_w  = edit_w_px;
    r.edit_h  = (box_h > 0) ? box_h : 0;
    r.label_x = pad_px + edit_w_px + pad_px;
    r.label_y = (y > 0) ? y : 0;
    r.label_w = label_w_px;
    r.label_h = (ctrl_h > 0) ? ctrl_h : 0;
    r.zoom_x  = r.label_x + label_w_px + pad_px;
    r.zoom_y  = r.label_y;
    r.zoom_w  = zoom_w_px;
    r.zoom_h  = r.label_h;
    return r;
}

// May an incoming page change rewrite the text in the box?
//
// The indicator tracks every page transition, including ones the reader causes
// with the wheel WHILE the box has focus (the box forwards WM_MOUSEWHEEL to the
// canvas, so this is reachable, not theoretical). Rewriting the box then would
// eat digits mid-keystroke. So: always safe when the box is not focused; safe
// when focused only if the text is still exactly what the bar itself last wrote,
// i.e. the reader has not started editing.
inline bool should_overwrite_page_box(bool box_has_focus,
                                      std::wstring_view current_text,
                                      std::wstring_view last_written) noexcept {
    if (!box_has_focus) return true;
    return current_text == last_written;
}

// Text for the zoom readout: "137%" for 1.37f, empty when there is nothing
// displayable.
//
// `pct` is DocumentView::zoom_pct(), whose domain is wider than the preset
// ladder: a fit mode derives it from the viewport with no upper bound, and a
// minimized window drives it to exactly 0. The three guards run BEFORE the
// conversion, in this order, so std::lround only ever sees [0.5, 9999.5):
//   1. non-finite product  (NaN, an infinity, or a finite pct that overflows)
//   2. below 0.5           (zero, negatives, anything that would print "0%")
//   3. 9999.5 and above    (the label is sized for four digits)
inline std::wstring format_zoom_pct(float pct) {
    const float p = pct * 100.0f;
    if (!std::isfinite(p)) return {};
    if (p < 0.5f) return {};
    if (p >= 9999.5f) return {};
    return std::to_wstring(std::lround(p)) + L"%";
}

}  // namespace litepdf::ui::detail
