#pragma once

// PR-A2: mouse-wheel scrolling as pure logic. No Win32, no Direct2D -- the
// canvas supplies the raw delta, the SPI_GETWHEELSCROLLLINES setting and the
// viewport, and gets back a new pan plus a page-flip verdict.
//
// UNITS AND SIGN. pan_y is in canvas DIPs and lives in [vp_h - content_h, 0],
// with 0 at the page's TOP (the top-left origin PR-A1 introduced; see
// ViewportMath.hpp). Wheel UP is delta > 0 and moves the reader toward the top,
// so step > 0 and pan_y increases toward 0. This matches the arrow keys:
// VK_UP calls pan_by(0, +100).

#include "ui/detail/ViewportMath.hpp"

namespace litepdf::ui {

// WHEEL_DELTA, spelled out so this header stays Win32-free.
inline constexpr int kWheelDelta = 120;

// One "line" of wheel scroll in DIPs. Chosen so the Windows default of 3 lines
// gives 48 DIP per notch -- a little under half the 100 DIP arrow-key step, so
// the wheel feels finer than the keyboard rather than coarser.
inline constexpr float kWheelLineDip = 16.0f;

// SPI_GETWHEELSCROLLLINES returns this sentinel when the user has chosen
// "One screen at a time". Spelled out for the same reason as kWheelDelta.
inline constexpr unsigned kWheelPageScroll = 0xFFFFFFFFu;

enum class Flip { None, Next, Prev };

// Fold `delta` into `residual` and return the number of WHOLE notches now
// available, leaving the remainder in `residual`.
//
// High-resolution wheels and precision touchpads deliver |delta| < WHEEL_DELTA,
// often 10-40 at a time. Without accumulation each of those would either be
// rounded to a full notch (absurdly fast) or dropped (dead wheel). Integer
// division truncates toward zero, which is exactly right here: the residual
// keeps the sign of the motion, so a reversal cancels rather than compounds.
inline int consume_notches(int delta, int& residual) noexcept {
    residual += delta;
    const int notches = residual / kWheelDelta;
    residual -= notches * kWheelDelta;
    return notches;
}

// Scroll magnitude in DIPs for `notches` whole notches. Positive = toward the
// top of the page.
inline float wheel_step_dip(int notches, unsigned lines_per_notch,
                            float vp_h) noexcept {
    if (notches == 0) return 0.0f;
    float per_notch;
    if (lines_per_notch == kWheelPageScroll) {
        // "One screen at a time". A full viewport height would land exactly on
        // the far edge, so the very next notch would flip the page with no
        // overlap for the reader to reacquire their place. 90% leaves a strip.
        per_notch = vp_h * 0.9f;
    } else if (lines_per_notch == 0) {
        return 0.0f;                     // the user turned wheel scrolling off
    } else {
        per_notch = static_cast<float>(lines_per_notch) * kWheelLineDip;
    }
    return static_cast<float>(notches) * per_notch;
}

struct WheelResult {
    float pan_y;
    Flip  flip;
};

// Apply one accumulated step. Returns the new pan, or a Flip when the page is
// already at the edge the step pushes toward.
//
// A step that merely REACHES an edge scrolls and does not flip; the flip is the
// following notch. That gives the reader a natural stop at each page boundary
// instead of skating past it.
inline WheelResult apply_wheel(float pan_y, float content_h, float vp_h,
                               float step) noexcept {
    WheelResult r{pan_y, Flip::None};
    if (step == 0.0f) return r;

    if (!(content_h > vp_h)) {
        // The page fits: there is nothing to scroll, so one notch is one page.
        r.flip = (step < 0.0f) ? Flip::Next : Flip::Prev;
        return r;
    }

    const float lo = vp_h - content_h;      // negative; the bottom of the range
    const bool at_bottom = !(pan_y > lo);
    const bool at_top    = !(pan_y < 0.0f);
    if (step < 0.0f && at_bottom) { r.flip = Flip::Next; return r; }
    if (step > 0.0f && at_top)    { r.flip = Flip::Prev; return r; }

    r.pan_y = clamp_pan(pan_y + step, content_h, vp_h);
    return r;
}

}  // namespace litepdf::ui
