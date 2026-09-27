#pragma once

// bench_selection -- litepdf-cli's `--bench-selection` measurement (#73).
//
// Times, on one page, the text-selection work PdfCanvas asks MuPDF for on the
// UI thread, through the same Document::TextPage calls it makes (painting the
// resulting quads is not included):
//
//   acquire     Document::text_page -- every left press on a page, and Select
//               All. Builds the page's structured text under doc_mutex. What
//               it costs depends on what the tab's Document has loaded before,
//               so it is reported three ways (see SelectionPageTiming).
//   move        snap() + highlight() -- PdfCanvas::refresh_live_selection, run
//               on every WM_MOUSEMOVE of a drag. Timed with the extent at the
//               page's last character (the largest highlight) and with the
//               extent on the anchor at the first character (the smallest: the
//               O(page) floor every move pays whatever its size).
//   release     snap() + highlight() + copy() -- commit_live_selection.
//   select all  text_page() + full_range() + highlight() + copy().
//   search      one Document::page_hits call for a needle that never matches,
//               so the scan walks the whole page: how long a search scan holds
//               doc_mutex on this page, i.e. how long a press can wait behind
//               it (spec R4). Reported cold and warm.
//
// Not a gate: timings are machine-dependent and nothing asserts on their size.
// The point is to put a number on the cost before deciding whether anything
// needs caching or moving off the UI thread.

#include <cstddef>
#include <filesystem>

namespace litepdf::core { class Document; }

namespace litepdf::cli {

// All timings in milliseconds. Per-call medians unless named otherwise.
struct SelectionPageTiming {
    int         page  = -1;
    std::size_t chars = 0;   // code points on the page, line breaks excluded
    std::size_t quads = 0;   // highlight quads for the whole page's text

    // One sample each, on Documents freshly opened for this page. scan_first:
    // a search scan's first visit to the page -- the lock hold a press can
    // wait behind; it also pays any process-wide first use (SystemFonts'
    // DirectWrite lookup for a non-embedded font), as the render worker does
    // in the GUI before anyone can press. acquire_first: then a second fresh
    // Document's first acquisition -- the first press in a tab that has not
    // built any page yet, i.e. every resource the page uses parsed cold.
    // Both are single samples. In one run scan_first spiked to several times
    // acquire_first on dozens of pages; every spike that was re-run -- in a
    // whole-document run, or alone with --page -- came back within ~30% of
    // acquire_first. Re-run an outlier before quoting it.
    double scan_first_ms    = 0.0;
    double acquire_first_ms = 0.0;
    // One sample, on the Document shared across the run: the page's first
    // acquisition after every earlier page of the run was acquired on it --
    // a first press on this page after presses on all the preceding ones, with
    // the fonts they share already parsed. Equals acquire_first in --page mode.
    double acquire_seq_ms   = 0.0;
    // Medians on the shared Document once it has built the page.
    double acquire_ms       = 0.0;
    double search_ms        = 0.0;

    // Indexed by core::SelectMode: Chars, Words, Lines.
    double move_full_ms[3]  = {};
    double move_short_ms[3] = {};
    double release_full_ms[3] = {};

    double select_all_ms = 0.0;
};

// Measures page `page` (0-based) of the document at `path`, `iterations`
// samples per median. `shared` is a Document open on the same file that has not
// built this page yet; the function opens two more of its own. Returns false --
// with a one-line diagnostic on stderr -- when a Document cannot be opened or
// the page yields no text handle. A page with a handle but no text returns true
// with chars == 0 and the move / release / select-all figures left at zero:
// there is nothing a drag could select.
bool bench_selection_page(const std::filesystem::path& path,
                          const litepdf::core::Document& shared, int page,
                          int iterations, SelectionPageTiming& out);

} // namespace litepdf::cli
