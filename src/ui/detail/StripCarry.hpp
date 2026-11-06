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

// Into paged spread: pan_y = -(the carried page's top in the union + offset),
// clamped to the union.
inline float spread_pan_y_for(const CarriedPosition& c, double page_top_in_union,
                              double union_h, double vp_h) noexcept {
    return clamp_pan(static_cast<float>(-(page_top_in_union + c.offset_dip)),
                     static_cast<float>(union_h), static_cast<float>(vp_h));
}

// The vertical geometry of a paged spread's painted union (PdfCanvas::content_extent
// and on_paint's dual branch, on one axis). Each page sits in a slot `slot_h` high
// at place_bitmap's pan-0 y: centred when it fits, 0 when it overflows. The union
// spans both pages; when it overflows the slot, on_paint shifts it by -union_top
// so pan 0 shows the union's top edge. `right_h < 0` means there is no right page.
struct SpreadVertical {
    // The left page's top at pan 0, in the painted frame: below the union's top
    // edge when the union overflows the slot (on_paint's base_y shift), else the
    // page's slot-centred y.
    double left_top_in_union = 0;
    // The same for the right page; 0 when there is none.
    double right_top_in_union = 0;
    double union_h = 0;
};

inline SpreadVertical spread_vertical(double left_h, double right_h,
                                      double slot_h) noexcept {
    auto page_y = [slot_h](double h) { return h <= slot_h ? (slot_h - h) * 0.5 : 0.0; };
    const double left_y = page_y(left_h);
    double top = left_y;
    double bottom = left_y + left_h;
    double right_y = 0.0;
    if (right_h >= 0.0) {
        right_y = page_y(right_h);
        top = std::min(top, right_y);
        bottom = std::max(bottom, right_y + right_h);
    }
    SpreadVertical out;
    out.union_h = std::max(0.0, bottom - top);
    const double base_y = out.union_h > slot_h ? -top : 0.0;
    out.left_top_in_union = left_y + base_y;
    if (right_h >= 0.0) out.right_top_in_union = right_y + base_y;
    return out;
}

// The top of `page` in the painted frame, for a spread whose left page is
// `left_page`. A carry from the strip can name either page of the pair.
inline double spread_page_top(const SpreadVertical& sv, int page, int left_page) noexcept {
    return page == left_page ? sv.left_top_in_union : sv.right_top_in_union;
}

// Paged spread: the LEFT page, the spread's current page, until the viewport top
// has left it. Past its bottom only the right page can hold the position, and it
// is measured from that page's own top. `right_page < 0` means there is no right
// page.
inline CarriedPosition carry_from_spread(int left_page, int right_page, double pan_y,
                                         const SpreadVertical& sv, double left_h,
                                         double right_h) noexcept {
    const double viewport_top = -pan_y;
    if (right_page >= 0 && viewport_top >= sv.left_top_in_union + left_h) {
        return {right_page, detail::carry_offset(viewport_top - sv.right_top_in_union, right_h)};
    }
    return {left_page, detail::carry_offset(viewport_top - sv.left_top_in_union, left_h)};
}

// Paged spread, when the page is already known: the one a carry from the strip came
// in on (spec 7.3). `page_h` is that page's height. The top goes through float, as
// the pan did, so a page carried in at its top reads exactly 0: strip_scroll_for
// lands that on the page's anchor, and anything else 8 DIP lower.
inline CarriedPosition carry_from_spread_on(int page, int left_page, double pan_y,
                                            const SpreadVertical& sv, double page_h) noexcept {
    const double top = static_cast<float>(spread_page_top(sv, page, left_page));
    return {page, detail::carry_offset(-pan_y - top, page_h)};
}

// Paged spread, with the page the strip carried the reader in on, or the page of a
// search hit that was off screen (spec 7.3, #211, #214):
// that page until the viewport top passes its bottom, wherever the reader has
// scrolled inside the pair (above a short page's top, at offset 0, with all of it
// on screen). Past its bottom only the other page of the pair can hold the
// position, so carry_from_spread answers. `held` is the held page, `held_h` its
// height. Half a DIP of slack: a carry in at the held page's bottom comes back
// through a float pan.
inline CarriedPosition carry_from_spread_held(int held, int left_page, int right_page,
                                              double pan_y, const SpreadVertical& sv,
                                              double left_h, double right_h) noexcept {
    const double held_h = held == left_page ? left_h : right_h;
    const double top = static_cast<float>(spread_page_top(sv, held, left_page));
    if (-pan_y <= top + held_h + 0.5) {
        return carry_from_spread_on(held, left_page, pan_y, sv, held_h);
    }
    return carry_from_spread(left_page, right_page, pan_y, sv, left_h, right_h);
}

}  // namespace litepdf::ui
