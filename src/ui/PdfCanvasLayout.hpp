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

// Forward / backward navigation step in spread mode. Pure logic; both
// PdfCanvas::on_key_down (PgUp/PgDn) and any future programmatic-jump
// caller share the same snap rule via these helpers.
//
// The cover page (0) is its own 1-page "pair", while subsequent pages
// form 2-page pairs (1,2)(3,4)…. A naive `cur_left + 2` stride works
// inside the 2-page region but overshoots the (0→1) bootstrap — that
// was the d2b583c-era bug surfaced by the T4 reviewer pass: cover→PgDn
// would land on page 2, skipping spread (1,2). The helper below treats
// the 0→1 step explicitly so callers don't have to know about the
// asymmetry.
//
// Clamping rule (matches plan §"Step 4.8" smoke expectations): if the
// next pair would start past the last page, clamp to the last LEFT-
// aligned candidate. In a 3-page doc that's page 2 alone; in a 10-page
// doc it's page 9 alone.
inline int dual_page_step_next_left(int cur_left, int page_count) noexcept {
    if (page_count <= 0) return 0;
    const int last = page_count - 1;
    int next_left;
    if (cur_left <= 0) {
        next_left = 1;            // cover → first spread (1,2)
    } else {
        next_left = cur_left + 2; // 1→3, 3→5, …
    }
    return next_left > last ? last : next_left;
}

inline int dual_page_step_prev_left(int cur_left, int /*page_count*/) noexcept {
    if (cur_left <= 1) return 0;     // first spread or cover → cover
    int prev = cur_left - 2;
    return prev < 0 ? 0 : prev;
}

}  // namespace litepdf::ui
