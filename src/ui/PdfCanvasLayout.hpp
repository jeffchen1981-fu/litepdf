#pragma once

// Phase 8 Task 4: pure-logic helpers for the two-page spread layout.
//
// dual_page_compute_left  → the LEFT page of the page-pair containing
//                           `page`, applying the cover-page rule (page 0
//                           always renders alone; subsequent pages pair
//                           as (1,2) (3,4) …). Acrobat / Foxit default.
// dual_page_compute_right → the RIGHT page of the pair, or -1 if the
//                           pair has no right (cover alone, or odd-tail
//                           where the last page is unpaired).
//
// Both are unit-tested without any UI dependency (test_dual_page_layout.cpp).

namespace litepdf::ui {

inline int dual_page_compute_left(int page, int page_count) noexcept {
    if (page <= 0 || page_count <= 0) return 0;
    // Cover page (0) is its own pair-left; pairs from page 1 onward are
    // (1,2) (3,4) (5,6) …, so for page >= 1 round to the nearest odd
    // page <= page. Bit trick: ((page - 1) & ~1) + 1 gives:
    //   1 → 1, 2 → 1, 3 → 3, 4 → 3, 5 → 5, 6 → 5, …
    return ((page - 1) & ~1) + 1;
}

inline int dual_page_compute_right(int left_page, int page_count) noexcept {
    if (left_page == 0)              return -1;  // cover alone
    if (left_page < 0)               return -1;  // defensive
    if (left_page + 1 >= page_count) return -1;  // odd-tail
    return left_page + 1;
}

}  // namespace litepdf::ui
