#pragma once

// #55: the continuous-scroll strip's viewport model. Everything about the strip
// that is not Win32 or Direct2D: the layout, the scroll offset, the shared
// horizontal pan, the current page and its navigation pin, keys, hit scrolling,
// position carry, per-tab state and the scrollbar's integer units. The canvas
// owns one and forwards input to it; the unit tests drive it headless.
//
// UNITS. Everything is `double` DIPs, like StripLayout.hpp. The viewport size is
// set by the caller (client size in DIPs).
//
// INPUT vs CURRENT PAGE. scroll_by, scroll_to, key (but Home and End), pan_x_by
// and scroll_to_pos only move the view; none of them derives the current page.
// Vertical input (scroll_by, scroll_to, scroll_to_pos, the LineUp/LineDown/
// PageUp/PageDown keys) ends the navigation pin (spec 4.6); horizontal input
// (pan_x_by, the Left/Right keys) keeps it, since it cannot change which page is
// on screen. The caller calls recompute_current() after input, so it can tell
// whether the page changed.
//
// Callers must also call recompute_current() after restore, relayout and
// apply_carry, which move the view without deriving the current page (a pin
// that apply_carry sets keeps its page through the recompute).

#include <algorithm>
#include <utility>
#include <vector>

#include "ui/detail/StripCarry.hpp"
#include "ui/detail/StripLayout.hpp"
#include "ui/detail/StripSchedule.hpp"
#include "ui/detail/StripScroll.hpp"
#include "ui/detail/StripScrollbar.hpp"

namespace litepdf::ui {

// The margin a search hit keeps from the viewport's top and bottom edge to count
// as fully visible: the one paged mode's scroll_into_view uses.
inline constexpr double kStripHitMarginDip = 24.0;

// A page's rectangle relative to the viewport's top-left corner, in DIPs.
struct StripPageRect {
    double x = 0, y = 0, w = 0, h = 0;
};

enum class StripKey { LineUp, LineDown, PageUp, PageDown, Home, End, Left, Right };

class StripViewport {
public:
    // No pages, scroll 0, desired pan 0, tracker reset, travel Down. The viewport
    // size is kept.
    void reset() noexcept {
        sizes_.clear();
        layout_ = StripLayout();
        scroll_y_ = 0;
        desired_pan_x_ = 0;
        tracker_.reset();
        travel_ = StripTravel::Down;
    }

    // A fresh layout: scroll 0, no pan, unpinned.
    void build(std::vector<StripPageSize> sizes, double zoom_pct) {
        sizes_ = std::move(sizes);
        layout_ = StripLayout(sizes_, zoom_pct);
        scroll_y_ = 0;
        desired_pan_x_ = 0;
        tracker_.reset();
        travel_ = StripTravel::Down;
    }

    bool has_layout() const noexcept { return layout_.page_count() > 0; }
    const StripLayout& layout() const noexcept { return layout_; }

    // spec 4.5: the viewport-top point stays put across a zoom change, and the
    // desired pan scales with the pages. False, and nothing touched, when the
    // zoom is unchanged or there are no pages.
    bool relayout(double zoom_pct) {
        if (!has_layout() || zoom_pct == layout_.zoom_pct()) return false;
        const StripPos top = layout_.position_at(scroll_y_);
        const double ratio = layout_.zoom_pct() > 0 ? zoom_pct / layout_.zoom_pct() : 1.0;
        layout_ = StripLayout(sizes_, zoom_pct);
        scroll_y_ = relayout_scroll(top, layout_, vp_h_);
        desired_pan_x_ *= ratio;
        return true;
    }

    // Keeps the top point; only the clamp can move it.
    void set_viewport(double w, double h) {
        vp_w_ = w;
        vp_h_ = h;
        scroll_y_ = clamp_strip_scroll(scroll_y_, layout_.total_height(), vp_h_);
    }

    // Scroll input: ends the pin and sets the travel from the direction of the
    // clamped target (unchanged when it does not move). True when scroll_y moved.
    bool scroll_by(double dy) { return scroll_to(scroll_y_ + dy); }
    bool scroll_to(double y) {
        tracker_.scroll_input();
        return move_to(y);
    }

    bool pan_x_by(double dx) {
        const double before = pan_x_eff();
        desired_pan_x_ = apply_pan_x_delta(desired_pan_x_, dx, layout_.widest(visible()), vp_w_);
        return pan_x_eff() != before;
    }

    // spec 6.1. Returns whether anything moved or a pin was set.
    bool key(StripKey k) {
        if (!has_layout()) return false;
        switch (k) {
        case StripKey::LineUp: return scroll_by(-kStripArrowStepDip);
        case StripKey::LineDown: return scroll_by(kStripArrowStepDip);
        case StripKey::PageUp: return scroll_by(-kStripPageStepFraction * vp_h_);
        case StripKey::PageDown: return scroll_by(kStripPageStepFraction * vp_h_);
        case StripKey::Left: return pan_x_by(kStripArrowStepDip);
        case StripKey::Right: return pan_x_by(-kStripArrowStepDip);
        case StripKey::Home: return pin_at(0.0, 0);
        case StripKey::End:
            return pin_at(layout_.total_height(), layout_.page_count() - 1);
        }
        return false;
    }

    // spec 6.3: the page's anchor, clamped; the page becomes current and pinned.
    void navigate(int page) {
        if (!has_layout()) return;
        page = std::clamp(page, 0, layout_.page_count() - 1);
        move_to(layout_.anchor(page));
        tracker_.navigate(page);
    }

    // spec 6.3 (Find, cross-tab result) with the paged 24 DIP margin: a quad that
    // lies inside the viewport by that margin stays put; any other is centred
    // vertically. The quad is given in the page's points; it is mapped to document
    // y with the layout's zoom. The hit's page becomes current and pinned either
    // way. Returns whether the view scrolled.
    bool scroll_hit_into_view(int page, double q_top_pt, double q_bottom_pt) {
        if (!has_layout() || page < 0 || page >= layout_.page_count()) return false;
        const double zoom = layout_.zoom_pct();
        const double q_top = layout_.top(page) + q_top_pt * zoom;
        const double q_bottom = layout_.top(page) + q_bottom_pt * zoom;
        bool scrolled = false;
        const bool fully_visible = q_top - scroll_y_ >= kStripHitMarginDip &&
                                   q_bottom - scroll_y_ <= vp_h_ - kStripHitMarginDip;
        if (!fully_visible) scrolled = move_to((q_top + q_bottom) / 2.0 - vp_h_ / 2.0);
        tracker_.navigate(page);
        return scrolled;
    }

    // The tracker's page after (re)deriving it; a pin keeps its page.
    int recompute_current() { return tracker_.recompute(layout_, scroll_y_, vp_h_); }

    int current_page() const noexcept { return tracker_.current(); }
    bool pinned() const noexcept { return tracker_.pinned(); }
    double scroll_y() const noexcept { return scroll_y_; }
    double viewport_w() const noexcept { return vp_w_; }
    double viewport_h() const noexcept { return vp_h_; }

    // V: the pages on screen.
    StripRange visible() const { return layout_.visible(scroll_y_, scroll_y_ + vp_h_); }

    // The search start page (spec 6.3): the first page on screen, else the
    // current page when only gaps are showing.
    int top_visible_page() const {
        const StripRange v = visible();
        return v.empty() ? current_page() : v.first;
    }

    // P: one viewport ahead in the travel direction, minus V.
    StripRange prefetch() const { return prefetch_range(layout_, scroll_y_, vp_h_, travel_); }

    // One viewport each side of V's span, so P is always inside it.
    StripRange retain() const {
        return layout_.visible(scroll_y_ - vp_h_, scroll_y_ + 2 * vp_h_);
    }

    // V by visible height, largest first, ties to the lower index; the current
    // page (the tracker's value, never a fresh derivation) goes first when it is
    // in V. That is the order PR 1's scheduler takes.
    std::vector<int> visible_by_area() const {
        std::vector<int> out;
        const StripRange v = visible();
        if (v.empty()) return out;
        const double y0 = scroll_y_;
        const double y1 = scroll_y_ + vp_h_;
        std::vector<std::pair<int, double>> pages;
        for (int p = v.first; p <= v.last; ++p) {
            const double t = layout_.top(p);
            pages.emplace_back(p, std::min(t + layout_.height(p), y1) - std::max(t, y0));
        }
        std::stable_sort(pages.begin(), pages.end(), [](const auto& a, const auto& b) {
            return a.second > b.second + kStripTieDip;
        });
        out.reserve(pages.size());
        for (const auto& pr : pages) out.push_back(pr.first);
        const auto it = std::find(out.begin(), out.end(), current_page());
        if (it != out.end()) std::rotate(out.begin(), it, it + 1);
        return out;
    }

    double pan_x_eff() const {
        return effective_pan_x(desired_pan_x_, layout_.widest(visible()), vp_w_);
    }

    // spec 4.2, relative to the viewport.
    StripPageRect page_rect(int page) const {
        StripPageRect r;
        r.w = layout_.width(page);
        r.h = layout_.height(page);
        r.x = page_x(r.w, layout_.widest(visible()), pan_x_eff(), vp_w_);
        r.y = layout_.top(page) - scroll_y_;
        return r;
    }

    // spec 6.1: something to pan, vertically or horizontally. A page within
    // kStripFitTolDip of the viewport width fits (StripScroll.hpp).
    bool can_pan() const {
        return layout_.total_height() > vp_h_ ||
               layout_.widest(visible()) > vp_w_ + kStripFitTolDip;
    }

    // spec 7.3.
    CarriedPosition carry() const {
        if (!has_layout()) return {};
        return carry_from_strip(layout_, std::clamp(current_page(), 0, layout_.page_count() - 1),
                                scroll_y_);
    }
    void apply_carry(const CarriedPosition& c) {
        if (!has_layout()) return;
        CarriedPosition at = c;
        at.page = std::clamp(c.page, 0, layout_.page_count() - 1);
        scroll_y_ = strip_scroll_for(layout_, at, vp_h_);
        tracker_.navigate(at.page);
    }

    // spec 7.3: the horizontal pan a carry into the strip arrives with (the
    // outgoing layout's pan, scaled by the zoom ratio). Stored as the desired
    // pan, so only pan_x_eff clamps it. Not an input: the pin is kept.
    void set_desired_pan_x(double x) noexcept { desired_pan_x_ = x; }

    // spec 7.4.
    StripTabState save() const {
        StripTabState s;
        s.top = layout_.position_at(scroll_y_);
        s.desired_pan_x = desired_pan_x_;
        s.zoom_pct = layout_.zoom_pct();
        return s;
    }
    void restore(const StripTabState& s) {
        if (!has_layout()) return;
        scroll_y_ = relayout_scroll(s.top, layout_, vp_h_);
        desired_pan_x_ = s.zoom_pct > 0
                             ? s.desired_pan_x * layout_.zoom_pct() / s.zoom_pct
                             : s.desired_pan_x;
        tracker_.reset();
    }

    StripScrollInfo scroll_info() const {
        return strip_scroll_info(layout_.total_height(), vp_h_, scroll_y_);
    }
    bool scroll_to_pos(int pos) {
        return scroll_to(strip_scroll_from_pos(pos, layout_.total_height(), vp_h_));
    }

private:
    // Clamps and applies a target scroll, setting the travel from its direction.
    // True when scroll_y moved.
    bool move_to(double y) {
        const double target = clamp_strip_scroll(y, layout_.total_height(), vp_h_);
        if (target > scroll_y_)
            travel_ = StripTravel::Down;
        else if (target < scroll_y_)
            travel_ = StripTravel::Up;
        const bool moved = target != scroll_y_;
        scroll_y_ = target;
        return moved;
    }

    // Home and End: scroll to y and pin `page`. True when the scroll moved or the
    // pin or page changed.
    bool pin_at(double y, int page) {
        const bool was_pinned = tracker_.pinned();
        const int was_page = tracker_.current();
        const bool moved = move_to(y);
        tracker_.navigate(page);
        return moved || !was_pinned || was_page != page;
    }

    std::vector<StripPageSize> sizes_;
    StripLayout layout_;
    double vp_w_ = 0;
    double vp_h_ = 0;
    double scroll_y_ = 0;
    double desired_pan_x_ = 0;
    CurrentPageTracker tracker_;
    StripTravel travel_ = StripTravel::Down;
};

}  // namespace litepdf::ui
