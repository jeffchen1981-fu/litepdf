#pragma once

#include <algorithm>

// PR-A1: pure fit and preset-ladder arithmetic for DocumentView. No MuPDF, no
// Win32 -- so the unequal-page spread case is testable without a fixture PDF
// whose pages differ in size (none exists in tests/fixtures).
//
// UNITS. A "percentage" here is DIPs per PDF point: 1.0 means one PDF point
// maps to one DIP, the conventional 96-dpi screen ratio. The display dpi is
// applied exactly once, on the way back out in DocumentView::render_scale().

namespace litepdf::core {

// Preset zoom percentages. Extended past the shipped 4.0 ceiling because a
// fit-derived percentage legitimately exceeds it (a narrow page on a wide
// canvas), and at the old ceiling that state made zoom_in() a permanent no-op.
inline constexpr float kZoomPresets[] = {
    0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f,
};
inline constexpr int kZoomPresetCount =
    static_cast<int>(sizeof(kZoomPresets) / sizeof(kZoomPresets[0]));

// Fit percentage for a viewport given in DEVICE PIXELS.
//
// For a two-page spread, pass the pair's max width and max height: one render
// scale is shared by both slots, so deriving it from the left page alone would
// overflow the slot holding the larger page.
inline float fit_percentage(float vp_w_px, float vp_h_px, float dpi,
                            float pw_pt, float ph_pt, bool fit_page) noexcept {
    const float ratio    = (dpi > 0.0f) ? (96.0f / dpi) : 1.0f;
    const float vp_w_dip = vp_w_px * ratio;
    const float vp_h_dip = vp_h_px * ratio;
    const float fit_w    = (pw_pt > 0.0f) ? vp_w_dip / pw_pt : 1.0f;
    const float fit_h    = (ph_pt > 0.0f) ? vp_h_dip / ph_pt : 1.0f;
    return fit_page ? std::min(fit_w, fit_h) : fit_w;
}

// First rung strictly above `pct`, or `pct` itself at the top of the table.
inline float next_preset(float pct) noexcept {
    for (int i = 0; i < kZoomPresetCount; ++i) {
        if (kZoomPresets[i] > pct + 1e-4f) return kZoomPresets[i];
    }
    return pct;
}

// Last rung strictly below `pct`, or `pct` itself at the bottom of the table.
inline float prev_preset(float pct) noexcept {
    for (int i = kZoomPresetCount - 1; i >= 0; --i) {
        if (kZoomPresets[i] < pct - 1e-4f) return kZoomPresets[i];
    }
    return pct;
}

// Pin a percentage into the table's span. NaN-safe: NaN and -inf clamp low,
// +inf clamps high, keeping the [lo, hi] contract total.
inline float clamp_preset_span(float pct) noexcept {
    const float lo = kZoomPresets[0];
    const float hi = kZoomPresets[kZoomPresetCount - 1];
    if (!(pct >= lo)) return lo;
    if (pct > hi)     return hi;
    return pct;
}

}  // namespace litepdf::core
