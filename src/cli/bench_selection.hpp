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
//
// Two things about the machine can move the numbers without the code changing,
// both measured while chasing spikes in the first runs:
//   - A cold OS file cache. The first reader of a page's bytes waits on the
//     disk and every later reader does not, so scan_first -- the first reader
//     -- spiked on up to a fifth of the pages of a freshly copied file (an
//     off-CPU median of 1.5 ms against 0.1 ms, single waits up to ~200 ms)
//     while acquire_first, right behind it, did not. Run the file through
//     warm_file_cache first and every column measures CPU work alone. That is
//     the GUI's case for a press (the render worker has read the page), but
//     NOT always for a search scan, which can reach pages nothing has read.
//   - A hybrid CPU's efficiency cores run this work about half as fast (an
//     i7-12700 pinned to them: 51-63 ms against 26-36 ms on its performance
//     cores, same pages). Runs started from a background shell sometimes spent
//     nearly their whole length there (736 of 753 pages in one).
//     prefer_performance_cores asks Windows not to, and each page records
//     whether a timed section began or ended on one anyway.

#include <cstddef>
#include <cstdint>
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
    // Single samples. scan_first reads the page's bytes first, so a cold file
    // cache lands on it alone -- see the header comment.
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

    // A timed section began or ended on an efficiency core: this page's
    // figures may be up to about twice a performance core's. Sampled at the
    // sections' boundaries, so a section that visits one only in its middle
    // goes unflagged.
    bool efficiency_core = false;
};

// Reads the file at `path` once, end to end, so its bytes sit in the OS file
// cache. Returns the bytes read: 0 when the file cannot be opened.
std::uintmax_t warm_file_cache(const std::filesystem::path& path);

// True when the calling thread is on a core of a lower efficiency class than
// the machine's highest -- an E-core of a hybrid CPU. Always false on a CPU
// with one core class.
bool on_efficiency_core() noexcept;

// Opts this process out of Windows' EcoQoS execution-speed throttling, the
// state in which Windows prefers efficiency cores -- as it never does for the
// GUI's foreground UI thread, the one these figures stand for. A preference,
// not a pin: efficiency_core still reports where the work actually ran.
// Returns whether Windows accepted the request.
bool prefer_performance_cores() noexcept;

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
