#pragma once

// PR-B: pure logic behind ui::StatusBar -- input parsing, child geometry and
// the box-overwrite rule. Deliberately free of <windows.h> so the whole
// decision surface of the status bar is unit-testable without a window, the
// same split ScrollMath/CompletionMath/SplitterMath use.

#include <optional>
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

// Pixel geometry of the two children inside the bar's client rect.
struct StatusBarChildRects {
    int edit_x = 0, edit_y = 0, edit_w = 0, edit_h = 0;
    int label_x = 0, label_y = 0, label_w = 0, label_h = 0;
};

// Lay the page box and the "/ N" label out left to right with a uniform
// padding, both vertically centred in a bar `bar_h` pixels tall. Callers pass
// pixel values already scaled for DPI, so this stays pure arithmetic.
inline StatusBarChildRects status_bar_child_rects(int bar_h, int pad_px,
                                                  int edit_w_px,
                                                  int label_w_px) noexcept {
    const int ctrl_h = (bar_h > 2 * pad_px) ? (bar_h - 2 * pad_px) : bar_h;
    const int y      = (bar_h - ctrl_h) / 2;

    StatusBarChildRects r;
    r.edit_x  = pad_px;
    r.edit_y  = (y > 0) ? y : 0;
    r.edit_w  = edit_w_px;
    r.edit_h  = (ctrl_h > 0) ? ctrl_h : 0;
    r.label_x = pad_px + edit_w_px + pad_px;
    r.label_y = r.edit_y;
    r.label_w = label_w_px;
    r.label_h = r.edit_h;
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

}  // namespace litepdf::ui::detail
