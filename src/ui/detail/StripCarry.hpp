#pragma once

// #55: the position carried across a layout switch (spec 7.3), as pure
// functions. No Win32, no MuPDF. Everything is `double` DIPs (see
// StripLayout.hpp) except the paged pans, which are `float` like clamp_pan.
//
// A position is (page, offset): `offset_dip` is how far the viewport top is
// below the TOP of `page`, in [0, page height]. It is measured from the page's
// top, not from the strip anchor (which sits one gap above the top), so that a
// page that fits (offset 0) lands where navigating to it would, and a scrolled
// page keeps exactly the part of it that was on screen.

#include <algorithm>
#include <cmath>

#include "ui/detail/StripLayout.hpp"
#include "ui/detail/StripScroll.hpp"
#include "ui/detail/ViewportMath.hpp"

namespace litepdf::ui {

struct CarriedPosition {
    int page = 0;
    double offset_dip = 0;
};

namespace detail {

// std::clamp with NaN -> 0 and a non-positive hi collapsing to 0.
inline double carry_offset(double raw, double hi) noexcept {
    if (!(raw == raw)) return 0.0;
    if (!(hi > 0.0)) return 0.0;
    return std::clamp(raw, 0.0, hi);
}

}  // namespace detail

// Paged single: pan_y is the page's top edge relative to the viewport top when
// the page overflows (pan_y <= 0), and page_origin_y is where the page sits in
// the viewport when pan is 0 (a page that fits is centred: its origin > 0).
// The offset is how far the viewport top is below the page's top.
inline CarriedPosition carry_from_paged_single(int page, double pan_y,
                                               double page_origin_y,
                                               double page_h) noexcept {
    return {page, detail::carry_offset(-pan_y - page_origin_y, page_h)};
}

// Paged spread: measured on the LEFT page alone, whose top sits
// `left_top_in_union` below the top of the union placement.
inline CarriedPosition carry_from_spread(int left_page, double pan_y,
                                         double left_top_in_union,
                                         double left_h) noexcept {
    return {left_page, detail::carry_offset(-pan_y - left_top_in_union, left_h)};
}

// Strip: page is the derived current page (or the pinned one).
inline CarriedPosition carry_from_strip(const StripLayout& l, int page,
                                        double scroll_y) {
    return {page, detail::carry_offset(scroll_y - l.top(page), l.height(page))};
}

// Into the strip: offset 0 -> anchor(page), exactly where navigation puts it;
// otherwise top(page) + offset (spec deviation 4: the offset is from the top).
inline double strip_scroll_for(const StripLayout& l, const CarriedPosition& c,
                               double vp_h) {
    const double y = c.offset_dip == 0 ? l.anchor(c.page) : l.top(c.page) + c.offset_dip;
    return clamp_strip_scroll(y, l.total_height(), vp_h);
}

// Into paged single: pan_y = -offset, clamped to the page.
inline float paged_pan_y_for(const CarriedPosition& c, double page_h,
                             double vp_h) noexcept {
    return clamp_pan(static_cast<float>(-c.offset_dip), static_cast<float>(page_h),
                     static_cast<float>(vp_h));
}

// Into paged spread: pan_y = -(the left page's top in the union + offset),
// clamped to the union.
inline float spread_pan_y_for(const CarriedPosition& c, double left_top_in_union,
                              double union_h, double vp_h) noexcept {
    return clamp_pan(static_cast<float>(-(left_top_in_union + c.offset_dip)),
                     static_cast<float>(union_h), static_cast<float>(vp_h));
}

}  // namespace litepdf::ui
