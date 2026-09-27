#include "cli/bench_selection.hpp"

#include "core/Document.hpp"
#include "core/TextSelection.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace litepdf::cli {

namespace {

using litepdf::core::Document;
using litepdf::core::SelectMode;
using litepdf::core::SelPoint;
using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

double median(std::vector<double> v) {   // by value: sorts a copy
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return (n % 2 == 1) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

// Median wall time of `iterations` calls of `fn`, after one untimed warm-up
// call so a first-touch allocation does not land in the sample.
template <typename Fn>
double median_ms(int iterations, Fn&& fn) {
    fn();
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(iterations));
    for (int i = 0; i < iterations; ++i) {
        const auto t0 = Clock::now();
        fn();
        samples.push_back(ms_since(t0));
    }
    return median(std::move(samples));
}

std::size_t count_code_points(const std::string& utf8) {
    std::size_t n = 0;
    for (unsigned char b : utf8) {
        if ((b & 0xC0) != 0x80 && b != '\r' && b != '\n') ++n;
    }
    return n;
}

// What PdfCanvas::refresh_live_selection does on one WM_MOUSEMOVE.
std::size_t move(const Document::TextPage& text, SelPoint anchor, SelPoint extent,
                 SelectMode mode) {
    const auto s = text.snap(anchor, extent, mode);
    return text.highlight(s.a, s.b).size();
}

// What PdfCanvas::commit_live_selection does on the release.
std::size_t release(const Document::TextPage& text, SelPoint anchor, SelPoint extent,
                    SelectMode mode) {
    const auto s = text.snap(anchor, extent, mode);
    return text.highlight(s.a, s.b).size() + text.copy(s.a, s.b).size();
}

// No real query is this, so the scan walks the whole page: the longest the
// search worker holds doc_mutex for this page.
constexpr const char* kNeverMatches = "\x7Fqz\x7F";

}  // namespace

bool bench_selection_page(const Document& doc, const Document& primer, int page,
                          int iterations, SelectionPageTiming& out) {
    out = SelectionPageTiming{};
    out.page = page;
    const auto index = static_cast<std::size_t>(page);

    // The primer loads the page first and pays what the render worker has
    // already paid by the time anyone can press on the page: process-wide
    // state, chiefly SystemFonts' DirectWrite lookup for a non-embedded font.
    auto t0 = Clock::now();
    (void)primer.text_page(index);
    out.acquire_cold_ms = ms_since(t0);

    // Then `doc`, whose own context has still never loaded this page.
    t0 = Clock::now();
    Document::TextPage text = doc.text_page(index);
    out.acquire_first_ms = ms_since(t0);
    if (!text.valid()) {
        std::fprintf(stderr, "bench-selection: no text handle for page %d\n", page);
        return false;
    }

    // Each sample drops its handle inside the timed region, as a press that
    // replaces the previous drag's handle does.
    out.acquire_ms = median_ms(iterations, [&] { (void)doc.text_page(index); });

    out.search_lock_ms = median_ms(iterations, [&] {
        (void)doc.page_hits(index, kNeverMatches, Document::SearchFlags{}, nullptr);
    });

    SelPoint first, last;
    if (!text.full_range(first, last)) return true;   // no text: nothing to drag over
    out.chars = count_code_points(text.copy(first, last));
    out.quads = text.highlight(first, last).size();

    const SelectMode modes[3] = { SelectMode::Chars, SelectMode::Words, SelectMode::Lines };
    volatile std::size_t sink = 0;   // keeps the calls from being optimised away
    for (int m = 0; m < 3; ++m) {
        const SelectMode mode = modes[m];
        out.move_full_ms[m] = median_ms(iterations, [&] { sink = move(text, first, last, mode); });
        out.move_short_ms[m] =
            median_ms(iterations, [&] { sink = move(text, first, first, mode); });
        out.release_full_ms[m] =
            median_ms(iterations, [&] { sink = release(text, first, last, mode); });
    }

    // PdfCanvas::select_all, acquisition included.
    out.select_all_ms = median_ms(iterations, [&] {
        const auto t = doc.text_page(index);
        SelPoint a, b;
        if (t.full_range(a, b)) sink = t.highlight(a, b).size() + t.copy(a, b).size();
    });
    (void)sink;
    return true;
}

} // namespace litepdf::cli
