#pragma once

// #55: render scheduling for the continuous-scroll strip (spec 5.1, 5.2), as a
// pure planning pass plus a small state holder that applies it. No Win32, no
// Direct2D, no MuPDF: the canvas implements StripRenderState::Submitter with
// real render requests, and the unit tests implement it with a recorder.
//
// One pass, in the order the canvas applies it:
//
//   sweep       Pending pages whose cancel flag was set from outside take their
//               cancel transition (spec 5.2 step 0)
//   release     slots outside `retain` are dropped (a Pending one is cancelled
//               first); this bounds the GPU bitmap count
//   fail_reset  Failed -> None for pages outside V that failed at P0, and for
//               pages in V that failed at P1 (a failed prefetch gets one P0
//               attempt when it enters V, and is not retried while outside it)
//   cancel      Pending pages outside V u P (inside retain), and P1 pages in V
//               that are promoted, take their cancel transition
//   submit_p0   V pages, largest visible area first; the first carries on_text
//   submit_p1   P pages, nearest first, while they fit in L1 beside V and the
//               P pages already resident (Pending or Ready)
//
// Every rule after the sweep judges a swept page by its cancel-transition
// result, so a swept page in V is requested again in the same pass.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "ui/detail/StripLayout.hpp"

namespace litepdf::ui {

// The scroll direction; prefetch looks one viewport ahead in it.
enum class StripTravel { Down, Up };

enum class PageStateKind { None, Pending, Ready, Failed };

// One page's render state (spec 5.1). The bitmap itself lives with the canvas.
struct PageSlot {
    PageStateKind kind = PageStateKind::None;
    std::uint64_t seq = 0;
    // Pending: the submitted priority. Failed: the failed request's priority.
    int priority = 0;
    // Pending: as submitted. Ready: as rendered.
    float scale = 0;
    bool invert = false;
    std::shared_ptr<std::atomic<bool>> cancel_flag;
    // Pending only: the page still holds its previous bitmap, rendered at
    // stale_scale / stale_invert, and paints it stretched until the new one lands.
    bool has_stale_bitmap = false;
    float stale_scale = 0;
    bool stale_invert = false;
};

// A Ready bitmap rendered at other parameters than the view's. Exact float
// comparison is intended: any change of the render scale is a re-render.
inline bool is_stale(const PageSlot& s, float scale, bool invert) noexcept {
    return s.kind == PageStateKind::Ready && (s.scale != scale || s.invert != invert);
}

// spec 5.1: a cancelled Pending page falls back to its stale bitmap (Ready at the
// stale parameters, so still stale) or to None. Other states are unchanged.
inline PageSlot cancel_transition(const PageSlot& s) noexcept {
    if (s.kind != PageStateKind::Pending) return s;
    PageSlot out;
    if (s.has_stale_bitmap) {
        out.kind = PageStateKind::Ready;
        out.scale = s.stale_scale;
        out.invert = s.stale_invert;
    }
    return out;
}

// The pages intersecting the one viewport height beyond the viewport in the
// travel direction, minus the visible pages. Empty at either end of the strip.
inline StripRange prefetch_range(const StripLayout& l, double scroll_y, double vp_h,
                                 StripTravel t) {
    const StripRange v = l.visible(scroll_y, scroll_y + vp_h);
    StripRange p = t == StripTravel::Down
                       ? l.visible(scroll_y + vp_h, scroll_y + 2 * vp_h)
                       : l.visible(scroll_y - vp_h, scroll_y);
    if (p.empty()) return {};
    if (!v.empty()) {
        // Both ranges are contiguous and p lies on one side of v, so removing v
        // trims one end only.
        if (t == StripTravel::Down)
            p.first = std::max(p.first, v.last + 1);
        else
            p.last = std::min(p.last, v.first - 1);
    }
    if (p.empty()) return {};
    return p;
}

struct ScheduleInput {
    StripRange visible;                 // V
    std::vector<int> visible_by_area;   // V, the predicted current page first
    StripRange prefetch;                // P, one viewport ahead in the travel direction
    StripRange retain;                  // V plus one viewport each side
    float scale = 0;
    bool invert = false;
    std::size_t l1_capacity = 0;
    std::size_t l1_budget = 0;
    std::function<std::size_t(int)> bytes_of;
    int previous_first_visible = 0;
    int previous_last_visible = -1;
};

// Applied in member order; see the comment at the top of this file.
struct SchedulePlan {
    std::vector<int> sweep;
    std::vector<int> release;
    std::vector<int> fail_reset;
    std::vector<int> cancel;
    std::vector<int> submit_p0;
    std::vector<int> submit_p1;
    int text_on_p0 = -1;          // the page in submit_p0 that carries on_text
    bool visible_changed = false;
};

namespace strip_detail {

inline bool flag_set(const PageSlot& s) noexcept {
    return s.cancel_flag && s.cancel_flag->load();
}

inline bool swept(const PageSlot& s) noexcept {
    return s.kind == PageStateKind::Pending && flag_set(s);
}

inline bool same_visible(StripRange v, int prev_first, int prev_last) noexcept {
    const StripRange prev{prev_first, prev_last};
    if (v.empty() || prev.empty()) return v.empty() == prev.empty();
    return v.first == prev.first && v.last == prev.last;
}

}  // namespace strip_detail

// spec 5.2 as a pure function of the slots and the view.
inline SchedulePlan plan_strip_pass(const std::map<int, PageSlot>& slots,
                                    const ScheduleInput& in) {
    SchedulePlan plan;
    const StripRange& v = in.visible;
    const StripRange& p = in.prefetch;
    const StripRange& retain = in.retain;

    // The state each later rule judges: a swept page counts as its cancel
    // transition, an absent page as None.
    auto effective = [&](int page) -> PageSlot {
        const auto it = slots.find(page);
        if (it == slots.end()) return {};
        return strip_detail::swept(it->second) ? cancel_transition(it->second)
                                               : it->second;
    };

    for (const auto& [page, slot] : slots) {
        if (strip_detail::swept(slot)) plan.sweep.push_back(page);

        if (!retain.contains(page)) {
            // Handled once, here: release cancels a Pending page itself.
            plan.release.push_back(page);
            continue;
        }

        const PageSlot e = strip_detail::swept(slot) ? cancel_transition(slot) : slot;
        const bool in_v = v.contains(page);
        // spec 5.1 / 5.3 rule 5: no automatic retry. A P0 failure is forgotten
        // once the page is outside V; a P1 failure only when it enters V.
        if (e.kind == PageStateKind::Failed &&
            ((!in_v && e.priority == 0) || (in_v && e.priority != 0)))
            plan.fail_reset.push_back(page);
        if (e.kind == PageStateKind::Pending) {
            const bool outside = !in_v && !p.contains(page);
            const bool promoted = in_v && e.priority != 0;
            if (outside || promoted) plan.cancel.push_back(page);
        }
    }

    // P0: every V page that has nothing usable on the way.
    for (int page : in.visible_by_area) {
        const PageSlot e = effective(page);
        bool submit = false;
        switch (e.kind) {
        case PageStateKind::None: submit = true; break;
        case PageStateKind::Ready: submit = is_stale(e, in.scale, in.invert); break;
        case PageStateKind::Failed: submit = e.priority != 0; break;
        case PageStateKind::Pending: submit = e.priority != 0; break;
        }
        if (submit) plan.submit_p0.push_back(page);
    }

    // P1: only what fits in L1 alongside V and the P pages already resident,
    // nearest page first. Counting the resident ones keeps a repeated pass from
    // prefetching one page further each time.
    if (!p.empty()) {
        const std::size_t v_count = v.empty() ? 0 : static_cast<std::size_t>(v.last - v.first + 1);
        std::size_t v_bytes = 0;
        if (in.bytes_of)
            for (int page = v.first; page <= v.last; ++page) v_bytes += in.bytes_of(page);
        std::size_t entries_free = in.l1_capacity > v_count ? in.l1_capacity - v_count : 0;
        std::size_t bytes_free = in.l1_budget > v_bytes ? in.l1_budget - v_bytes : 0;

        // P sits on one side of V; above it means travelling up, nearest is P.last.
        const bool upward = !v.empty() && p.last < v.first;
        const int step = upward ? -1 : 1;
        const int start = upward ? p.last : p.first;
        const int end = upward ? p.first - 1 : p.last + 1;
        for (int page = start; page != end; page += step) {
            if (v.contains(page)) continue;
            const PageStateKind kind = effective(page).kind;
            if (kind == PageStateKind::Failed) continue;  // nothing resident
            const std::size_t need = in.bytes_of ? in.bytes_of(page) : 0;
            if (kind != PageStateKind::None) {            // Pending or Ready
                entries_free = entries_free > 0 ? entries_free - 1 : 0;
                bytes_free = bytes_free > need ? bytes_free - need : 0;
                continue;
            }
            if (entries_free == 0 || need > bytes_free) break;
            plan.submit_p1.push_back(page);
            --entries_free;
            bytes_free -= need;
        }
    }

    if (!in.visible_by_area.empty() &&
        std::find(plan.submit_p0.begin(), plan.submit_p0.end(), in.visible_by_area.front()) !=
            plan.submit_p0.end())
        plan.text_on_p0 = in.visible_by_area.front();

    plan.visible_changed =
        !strip_detail::same_visible(v, in.previous_first_visible, in.previous_last_visible);
    return plan;
}

// The canvas-side holder: owns the per-page slots and applies plans to them.
class StripRenderState {
public:
    struct Submitter {
        virtual ~Submitter() = default;
        // Submits a render of `page`; returns the request's seq.
        virtual std::uint64_t submit(int page, int priority, bool with_text,
                                     std::shared_ptr<std::atomic<bool>> flag) = 0;
        virtual void cancel_thumbnails() = 0;
        // The page's slot (and its bitmap) is gone.
        virtual void released(int page) = 0;
    };

    SchedulePlan run_pass(const ScheduleInput& in, Submitter& out) {
        SchedulePlan plan = plan_strip_pass(slots_, in);

        for (int page : plan.sweep) {
            PageSlot& s = slots_[page];
            s = cancel_transition(s);
        }
        for (int page : plan.release) {
            const auto it = slots_.find(page);
            if (it == slots_.end()) continue;
            cancel_pending(it->second);
            slots_.erase(it);
            out.released(page);
        }
        for (int page : plan.fail_reset) slots_[page] = PageSlot{};
        for (int page : plan.cancel) cancel_pending(slots_[page]);
        for (int page : plan.submit_p0) submit(page, 0, page == plan.text_on_p0, in, out);
        for (int page : plan.submit_p1) submit(page, 1, false, in, out);

        if (plan.visible_changed) out.cancel_thumbnails();
        return plan;
    }

    // A render scale or polarity change (spec 5.1, 5.2). Every Pending request is
    // cancelled and every Failed page forgotten; Ready bitmaps stay and are
    // stale by is_stale() from the next pass on, which carries the new values
    // in its ScheduleInput -- so the parameters are not stored here.
    void change_render_params(float /*scale*/, bool /*invert*/) {
        for (auto& [page, s] : slots_) {
            if (s.kind == PageStateKind::Pending)
                cancel_pending(s);
            else if (s.kind == PageStateKind::Failed)
                s = PageSlot{};
        }
    }

    // discard_render_target: every bitmap is gone. Pending requests stay.
    void drop_bitmaps() {
        for (auto& [page, s] : slots_) {
            if (s.kind == PageStateKind::Ready) {
                s = PageSlot{};
            } else if (s.kind == PageStateKind::Pending) {
                s.has_stale_bitmap = false;
                s.stale_scale = 0;
                s.stale_invert = false;
            }
        }
    }

    // spec 5.2 text prefetch, for a current-page change or the current page's P0
    // completing. The text-only request is tracked apart from the page's slot and
    // never changes it; a P0 carrying text is never cancelled here.
    bool request_text(int page, bool slot_has_text, bool text_pending, bool l1_contains,
                      Submitter& out) {
        if (slot_has_text || text_pending || !l1_contains) return false;
        if (text_flag_) text_flag_->store(true);
        text_flag_ = std::make_shared<std::atomic<bool>>(false);
        out.submit(page, 1, true, text_flag_);
        return true;
    }

    const PageSlot* slot(int page) const {
        const auto it = slots_.find(page);
        return it == slots_.end() ? nullptr : &it->second;
    }
    std::map<int, PageSlot>& slots() { return slots_; }

    // The view goes away: cancel every request, release every slot.
    void clear(Submitter& out) {
        for (auto& [page, s] : slots_) {
            if (s.kind == PageStateKind::Pending && s.cancel_flag) s.cancel_flag->store(true);
            out.released(page);
        }
        slots_.clear();
        if (text_flag_) text_flag_->store(true);
        text_flag_.reset();
    }

private:
    // Stores true to a Pending slot's flag and takes the cancel transition.
    static void cancel_pending(PageSlot& s) {
        if (s.kind != PageStateKind::Pending) return;
        if (s.cancel_flag) s.cancel_flag->store(true);
        s = cancel_transition(s);
    }

    void submit(int page, int priority, bool with_text, const ScheduleInput& in,
                Submitter& out) {
        auto flag = std::make_shared<std::atomic<bool>>(false);
        const std::uint64_t seq = out.submit(page, priority, with_text, flag);
        PageSlot& s = slots_[page];
        PageSlot next;
        next.kind = PageStateKind::Pending;
        next.seq = seq;
        next.priority = priority;
        next.scale = in.scale;
        next.invert = in.invert;
        next.cancel_flag = std::move(flag);
        if (s.kind == PageStateKind::Ready) {
            // The old bitmap stays on screen until the new one lands.
            next.has_stale_bitmap = true;
            next.stale_scale = s.scale;
            next.stale_invert = s.invert;
        }
        s = std::move(next);
    }

    std::map<int, PageSlot> slots_;
    std::shared_ptr<std::atomic<bool>> text_flag_;  // the latest text-only request
};

}  // namespace litepdf::ui
