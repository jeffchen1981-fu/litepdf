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
// call so a first-touch allocation does not land in the sample. `fn` returns
// its result so that destroying it -- dropping a TextPage handle, which no
// press pays for -- happens after the clock stops.
template <typename Fn>
double median_ms(int iterations, Fn&& fn) {
    (void)fn();
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(iterations));
    for (int i = 0; i < iterations; ++i) {
        const auto t0 = Clock::now();
        const auto result = fn();
        samples.push_back(ms_since(t0));
        (void)result;
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

// No real query is this, so the scan walks the whole page.
constexpr const char* kNeverMatches = "\x7Fqz\x7F";

std::size_t scan(const Document& doc, std::size_t index) {
    return doc.page_hits(index, kNeverMatches, Document::SearchFlags{}, nullptr).size();
}

}  // namespace

bool bench_selection_page(const std::filesystem::path& path, const Document& shared,
                          int page, int iterations, SelectionPageTiming& out) {
    out = SelectionPageTiming{};
    out.page = page;
    const auto index = static_cast<std::size_t>(page);

    // Two Documents that have never built any page: the first stands in for a
    // search scan's first visit, the second for a tab's first press.
    Document scanner, fresh;
    if (scanner.open(path) || fresh.open(path)) {
        std::fprintf(stderr, "bench-selection: cannot open %s\n", path.string().c_str());
        return false;
    }
    auto t0 = Clock::now();
    (void)scan(scanner, index);
    out.scan_first_ms = ms_since(t0);

    t0 = Clock::now();
    const Document::TextPage first = fresh.text_page(index);
    out.acquire_first_ms = ms_since(t0);
    if (!first.valid()) {
        std::fprintf(stderr, "bench-selection: no text handle for page %d\n", page + 1);
        return false;
    }

    t0 = Clock::now();
    Document::TextPage text = shared.text_page(index);
    out.acquire_seq_ms = ms_since(t0);
    if (!text.valid()) {
        std::fprintf(stderr, "bench-selection: no text handle for page %d\n", page + 1);
        return false;
    }

    out.acquire_ms = median_ms(iterations, [&] { return shared.text_page(index); });
    out.search_ms  = median_ms(iterations, [&] { return scan(shared, index); });

    SelPoint first_pt, last_pt;
    if (!text.full_range(first_pt, last_pt)) return true;   // no text: nothing to drag over
    out.chars = count_code_points(text.copy(first_pt, last_pt));
    out.quads = text.highlight(first_pt, last_pt).size();

    const SelectMode modes[3] = { SelectMode::Chars, SelectMode::Words, SelectMode::Lines };
    for (int m = 0; m < 3; ++m) {
        const SelectMode mode = modes[m];
        out.move_full_ms[m] =
            median_ms(iterations, [&] { return move(text, first_pt, last_pt, mode); });
        out.move_short_ms[m] =
            median_ms(iterations, [&] { return move(text, first_pt, first_pt, mode); });
        out.release_full_ms[m] =
            median_ms(iterations, [&] { return release(text, first_pt, last_pt, mode); });
    }

    // PdfCanvas::select_all, acquisition included.
    out.select_all_ms = median_ms(iterations, [&] {
        const auto t = shared.text_page(index);
        SelPoint a, b;
        return t.full_range(a, b) ? t.highlight(a, b).size() + t.copy(a, b).size()
                                  : std::size_t{0};
    });
    return true;
}

} // namespace litepdf::cli
