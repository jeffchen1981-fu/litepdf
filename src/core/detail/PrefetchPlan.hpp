#pragma once

#include <cmath>
#include <cstddef>

// #49: pure prefetch policy for DocumentView under PageCache's L1 byte budget.
// No MuPDF, no Win32 -- so the policy is testable with arbitrary page sizes,
// budgets and travel directions, no fixture PDF and no render required.
//
// While the current page and both neighbours fit in the budget, both
// neighbours are prefetched, as before #49. Past that, only the neighbour in
// the travel direction is: it is the page the next turn shows, so a forward
// reader still turns pages from L1. Prefetching both would have them evict
// each other (and the current page), rendering a top-rung pixmap per turn that
// is never shown. The current page itself lives on as the canvas's own bitmap.

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
// keeps the pre-#49 behaviour of prefetching both).
inline PrefetchPlan prefetch_plan(std::size_t page_bytes,
                                  std::size_t budget_bytes,
                                  TravelDirection dir) noexcept {
    if (page_bytes == 0 || page_bytes <= budget_bytes / 3) return {true, true};
    return dir == TravelDirection::Forward ? PrefetchPlan{false, true}
                                           : PrefetchPlan{true, false};
}

}  // namespace litepdf::core
