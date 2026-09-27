#pragma once

// bench_selection -- litepdf-cli's `--bench-selection` measurement (#73).
//
// Times, on one page, every piece of text-selection work PdfCanvas does on the
// UI thread, through the same Document::TextPage calls it makes:
//
//   acquire     Document::text_page -- every left press on a page, and Select
//               All. Builds the page's structured text under doc_mutex.
//   move        snap() + highlight() -- PdfCanvas::refresh_live_selection, run
//               on every WM_MOUSEMOVE of a drag. Timed with the extent at the
//               page's last character (the largest highlight) and with the
//               extent on the anchor at the first character (the smallest: the
//               O(page) floor every move pays whatever its size).
//   release     snap() + highlight() + copy() -- commit_live_selection.
//   select all  text_page() + full_range() + highlight() + copy().
//   search lock one Document::page_hits call for a needle that never matches:
//               how long a running search scan holds doc_mutex per page, i.e.
//               the longest an acquire can wait behind it (spec R4).
//
// Not a gate: timings are machine-dependent and nothing asserts on them. The
// point is to put a number on the cost before deciding whether anything needs
// caching or moving off the UI thread.

#include <cstddef>

namespace litepdf::core { class Document; }

namespace litepdf::cli {

// All timings in milliseconds. Per-call medians unless named otherwise.
struct SelectionPageTiming {
    int         page  = -1;
    std::size_t chars = 0;   // code points on the page, line breaks excluded
    std::size_t quads = 0;   // highlight quads for the whole page's text

    // A page's first acquisition, three ways. cold: the primer's, which also
    // pays process-wide first-use costs. first: `doc`'s, after the primer --
    // what a first press on the page costs in the GUI, where the render worker
    // has loaded the page on its own fz_document by then. acquire: the median
    // of `doc`'s later ones.
    double acquire_cold_ms  = 0.0;
    double acquire_first_ms = 0.0;
    double acquire_ms       = 0.0;
    double search_lock_ms   = 0.0;

    // Indexed by core::SelectMode: Chars, Words, Lines.
    double move_full_ms[3]  = {};
    double move_short_ms[3] = {};
    double release_full_ms[3] = {};

    double select_all_ms = 0.0;
};

// Measures page `page` of an open `doc`, `iterations` samples per figure
// (acquire_cold_ms and acquire_first_ms are one sample each). `primer` is a
// second Document open on the same file; neither may have loaded the page yet.
// Returns false -- with a one-line diagnostic on stderr -- when the page has no
// text handle. A page with a handle but no text returns true with chars == 0
// and the move / release / select-all figures left at zero: there is nothing a
// drag could select.
bool bench_selection_page(const litepdf::core::Document& doc,
                          const litepdf::core::Document& primer, int page,
                          int iterations, SelectionPageTiming& out);

} // namespace litepdf::cli
