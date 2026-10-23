#pragma once

// #55: the strip's vertical scrollbar in integer units (spec 6.2), as pure
// functions. No Win32, no MuPDF. Win32 scroll bars hold `int`, so the strip's
// `double` DIPs are rounded to whole DIPs here and nowhere else; nMin is always 0.
//
//   nMax  = round(total) - 1          (the last addressable DIP)
//   nPage = round(viewport height)
//   the largest thumb position is nMax - nPage + 1
//
// The largest position maps to the exact maximum scroll (total - vp_h), not to a
// rounded one, so dragging the thumb to the end reaches the strip's real end.

#include <algorithm>
#include <climits>
#include <cmath>

namespace litepdf::ui {

struct StripScrollInfo {
    int max = 0;
    int page = 0;
    int pos = 0;
};

namespace detail {

// Round to a non-negative int; NaN and negatives -> 0, overflow saturates.
inline int scrollbar_round(double v) noexcept {
    if (!(v > 0.0)) return 0;
    if (v >= static_cast<double>(INT_MAX)) return INT_MAX;
    return static_cast<int>(std::llround(v));
}

// nMax: the last addressable DIP, never negative.
inline int scrollbar_max(double total) noexcept {
    const int t = scrollbar_round(total);
    return t > 0 ? t - 1 : 0;
}

// nMax - nPage + 1, never negative.
inline int scrollbar_largest(int max, int page) noexcept {
    const long long l = static_cast<long long>(max) - page + 1;
    return l < 0 ? 0 : static_cast<int>(l);
}

}  // namespace detail

inline StripScrollInfo strip_scroll_info(double total, double vp_h, double scroll_y) noexcept {
    StripScrollInfo i;
    i.max = detail::scrollbar_max(total);
    i.page = detail::scrollbar_round(vp_h);
    const int largest = detail::scrollbar_largest(i.max, i.page);
    if (total - vp_h >= 1.0 && scroll_y >= (total - vp_h) - 0.5)
        i.pos = largest;
    else
        i.pos = std::clamp(detail::scrollbar_round(scroll_y), 0, largest);
    return i;
}

// The thumb position the user dragged to -> the scroll offset in DIPs.
inline double strip_scroll_from_pos(int pos, double total, double vp_h) noexcept {
    const double max_scroll = std::max(0.0, total - vp_h);
    const int largest = detail::scrollbar_largest(detail::scrollbar_max(total),
                                                  detail::scrollbar_round(vp_h));
    if (pos >= largest) return max_scroll;
    if (pos <= 0) return 0.0;
    return std::min(static_cast<double>(pos), max_scroll);
}

}  // namespace litepdf::ui
