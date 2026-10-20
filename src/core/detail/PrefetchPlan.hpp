#pragma once

#include <cmath>
#include <cstddef>

// #49: pure prefetch policy for DocumentView under PageCache's L1 byte budget.
// No MuPDF, no Win32 -- so the policy is testable with arbitrary page sizes,
// budgets and travel directions, no fixture PDF and no render required.
//
// While the current page and both neighbours fit in the budget, both
// neighbours are prefetched, as before #49. Past that, only one is: the
// neighbour in the travel direction, which is the page the next turn shows, or
// the other one on the first or last page, where the travel-direction one does
// not exist (Home reads as a move backward, then the reader pages forward).
// Prefetching both would have them evict each other (and the current page),
// rendering a top-rung pixmap per turn that is never shown. The current page
// itself lives on as the canvas's own bitmap.
//
// While two pages fit, a reader paging on in one direction turns every page
// from L1. Once a single page is over half the budget only one pixmap fits:
// after a miss the current page and the prefetched one render at the same time
// on the two workers, and whichever lands last evicts the other.

namespace litepdf::core {

enum class TravelDirection { Forward, Backward };

// Direction of the move from `last_page` to `page`. -1 for `last_page` means
// no previous page, and a resubmit of the same page (zoom step, resize, device
// loss) is no move: both keep `last`.
inline TravelDirection travel_direction(int page, int last_page,
                                        TravelDirection last) noexcept {
    if (last_page < 0 || page == last_page) return last;
    return page > last_page ? TravelDirection::Forward
                            : TravelDirection::Backward;
}

// Bytes of the BGRA pixmap RenderEngine allocates for a page of `w_pt` x
// `h_pt` points at `scale`. MuPDF's bbox rounding (fz_round_rect) can add a
// pixel per edge, so each side gets one pixel of margin. 0 means unknown: a
// non-positive or non-finite input.
inline std::size_t estimated_pixmap_bytes(float w_pt, float h_pt,
                                          float scale) noexcept {
    const double w = static_cast<double>(w_pt) * scale;
    const double h = static_cast<double>(h_pt) * scale;
    if (!(w > 0.0) || !(h > 0.0) || !std::isfinite(w) || !std::isfinite(h)) {
        return 0;
    }
    return static_cast<std::size_t>(std::ceil(w) + 1.0) *
           static_cast<std::size_t>(std::ceil(h) + 1.0) * 4u;
}

struct PrefetchPlan {
    bool prev;
    bool next;
};

// Which neighbours to prefetch for a page of `page_bytes` (0 = unknown, which
// keeps the pre-#49 behaviour of prefetching both). `has_prev` / `has_next`
// say whether the page has that neighbour; the plan never names one it lacks.
inline PrefetchPlan prefetch_plan(std::size_t page_bytes,
                                  std::size_t budget_bytes,
                                  TravelDirection dir,
                                  bool has_prev, bool has_next) noexcept {
    if (page_bytes == 0 || page_bytes <= budget_bytes / 3) {
        return {has_prev, has_next};
    }
    if (dir == TravelDirection::Forward) {
        return has_next ? PrefetchPlan{false, true} : PrefetchPlan{has_prev, false};
    }
    return has_prev ? PrefetchPlan{true, false} : PrefetchPlan{false, has_next};
}

}  // namespace litepdf::core
