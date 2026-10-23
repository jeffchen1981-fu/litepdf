#pragma once

// #55: scroll arithmetic for the continuous-scroll strip, as pure functions.
// No Win32, no MuPDF. Everything is `double` DIPs (see StripLayout.hpp).

#include <algorithm>
#include <cmath>

#include "ui/detail/StripLayout.hpp"

namespace litepdf::ui {

// One arrow-key / line step, and the page step as a fraction of the viewport.
inline constexpr double kStripArrowStepDip = 100.0;
inline constexpr double kStripPageStepFraction = 0.9;

// Keeps scroll_y in [0, max(0, total - vp_h)]. NaN -> 0.
inline double clamp_strip_scroll(double y, double total, double vp_h) noexcept {
    if (!(y == y)) return 0.0;
    const double max_y = std::max(0.0, total - vp_h);
    if (y < 0.0) return 0.0;
    if (y > max_y) return max_y;
    return y;
}

// The scroll offset that keeps the viewport-top point in place after a
// re-layout (zoom or page-size change): `top_point` was captured with
// position_at() on the old layout (spec 4.5).
inline double relayout_scroll(const StripPos& top_point, const StripLayout& next,
                              double vp_h) {
    return clamp_strip_scroll(next.y_of(top_point), next.total_height(), vp_h);
}

// Visible heights closer than this are a tie: at a fractional zoom, equal pages'
// heights differ by rounding noise (a few 1e-12 DIP at 30 %).
inline constexpr double kStripTieDip = 1e-6;

// spec 4.6, in order: at max scroll -> the last page; at 0 -> page 0; else the page
// with the largest visible height, ties to the lower index. Rules 1-2 apply only
// when the strip is at least 1 DIP taller than the viewport; otherwise max
// scroll is about 0 and both would fire at once. No pages -> -1.
inline int derive_current_page(const StripLayout& l, double scroll_y, double vp_h) {
    const int n = l.page_count();
    if (n <= 0) return -1;

    const double total = l.total_height();
    const bool tall = total - vp_h >= 1.0;
    if (tall && scroll_y >= (total - vp_h) - 0.5) return n - 1;
    if (tall && scroll_y <= 0.5) return 0;

    const double y0 = scroll_y;
    const double y1 = scroll_y + vp_h;
    const StripRange r = l.visible(y0, y1);
    if (r.empty()) {
        // Only gaps are on screen: the nearest page above, else page 0.
        return std::clamp(l.position_at(y0).page, 0, n - 1);
    }
    int best = r.first;
    double best_h = -1.0;
    for (int p = r.first; p <= r.last; ++p) {
        const double t = l.top(p);
        const double vis = std::min(t + l.height(p), y1) - std::max(t, y0);
        if (vis > best_h + kStripTieDip) {  // ties keep the lower index
            best_h = vis;
            best = p;
        }
    }
    return best;
}

// Holds the strip's current page and the navigation pin of spec 4.6: an explicit
// navigation sets the page and pins it; the next scroll input ends the pin;
// while pinned, recompute() keeps the pinned page.
class CurrentPageTracker {
public:
    void navigate(int page) noexcept {
        page_ = page;
        pinned_ = true;
    }
    void scroll_input() noexcept { pinned_ = false; }

    int recompute(const StripLayout& l, double scroll_y, double vp_h) {
        if (!pinned_) page_ = derive_current_page(l, scroll_y, vp_h);
        return page_;
    }

    int current() const noexcept { return page_; }
    bool pinned() const noexcept { return pinned_; }

    void reset() noexcept {
        page_ = 0;
        pinned_ = false;
    }

private:
    int page_ = 0;
    bool pinned_ = false;
};

// clamp_pan (ViewportMath.hpp) in double: a widest page that fits the viewport
// -> 0, else clamp to [vp_w - widest, 0]. NaN -> 0.
inline double effective_pan_x(double desired, double widest, double vp_w) noexcept {
    if (!(desired == desired)) return 0.0;
    if (!(widest > vp_w)) return 0.0;
    const double lo = vp_w - widest;
    if (desired < lo) return lo;
    if (desired > 0.0) return 0.0;
    return desired;
}

// Applies a pan delta to the effective (clamped) pan, then clamps again; the
// caller stores the result as the new desired pan.
inline double apply_pan_x_delta(double desired, double delta, double widest,
                                double vp_w) noexcept {
    return effective_pan_x(effective_pan_x(desired, widest, vp_w) + delta, widest,
                           vp_w);
}

// spec 4.2: a page no wider than the viewport is centred; a wider one sits at the
// shared pan, centred inside the widest page's span.
inline double page_x(double page_w, double widest, double pan_x_eff,
                     double vp_w) noexcept {
    if (page_w <= vp_w) return (vp_w - page_w) / 2.0;
    return pan_x_eff + (widest - page_w) / 2.0;
}

}  // namespace litepdf::ui
