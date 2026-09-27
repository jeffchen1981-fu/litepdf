#include "cli/bench_selection.hpp"

#include "core/Document.hpp"
#include "core/TextSelection.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>

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

// One entry per core and processor group: the core's processors there, and
// whether its efficiency class is below the machine's highest.
struct CoreClass {
    WORD      group;
    KAFFINITY mask;
    bool      efficiency;
};

std::vector<CoreClass> core_classes() {
    std::vector<CoreClass> out;
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    std::vector<unsigned char> buf(len);
    auto* first = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data());
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, first, &len)) return out;
    BYTE top = 0;
    std::vector<std::pair<const PROCESSOR_RELATIONSHIP*, BYTE>> cores;
    for (DWORD off = 0; off < len;) {
        const auto* info =
            reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
        cores.emplace_back(&info->Processor, info->Processor.EfficiencyClass);
        top = (std::max)(top, info->Processor.EfficiencyClass);
        off += info->Size;
    }
    for (const auto& [core, cls] : cores) {
        for (WORD g = 0; g < core->GroupCount; ++g) {
            out.push_back({ core->GroupMask[g].Group, core->GroupMask[g].Mask, cls < top });
        }
    }
    return out;
}

}  // namespace

std::uintmax_t warm_file_cache(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return 0;
    std::vector<char> chunk(1 << 20);
    std::uintmax_t total = 0;
    while (in.read(chunk.data(), static_cast<std::streamsize>(chunk.size())) || in.gcount() > 0) {
        total += static_cast<std::uintmax_t>(in.gcount());
    }
    return total;
}

bool on_efficiency_core() noexcept {
    static const std::vector<CoreClass> classes = [] {
        try {
            return core_classes();
        } catch (...) {
            return std::vector<CoreClass>{};
        }
    }();
    PROCESSOR_NUMBER here{};
    GetCurrentProcessorNumberEx(&here);
    for (const auto& c : classes) {
        if (c.group == here.Group && (c.mask & (KAFFINITY{ 1 } << here.Number)) != 0) {
            return c.efficiency;
        }
    }
    return false;
}

bool prefer_performance_cores() noexcept {
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version     = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    state.StateMask   = 0;   // controlled, and off: High QoS (still not a pin)
    return SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state,
                                 sizeof(state)) != FALSE;
}

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
    // Called at every timed section's boundaries; see efficiency_core.
    const auto note_core = [&out] {
        if (on_efficiency_core()) out.efficiency_core = true;
    };
    note_core();
    auto t0 = Clock::now();
    (void)scan(scanner, index);
    out.scan_first_ms = ms_since(t0);
    note_core();

    t0 = Clock::now();
    const Document::TextPage first = fresh.text_page(index);
    out.acquire_first_ms = ms_since(t0);
    note_core();
    if (!first.valid()) {
        std::fprintf(stderr, "bench-selection: no text handle for page %d\n", page + 1);
        return false;
    }

    t0 = Clock::now();
    Document::TextPage text = shared.text_page(index);
    out.acquire_seq_ms = ms_since(t0);
    note_core();
    if (!text.valid()) {
        std::fprintf(stderr, "bench-selection: no text handle for page %d\n", page + 1);
        return false;
    }

    out.acquire_ms = median_ms(iterations, [&] { return shared.text_page(index); });
    note_core();
    out.search_ms  = median_ms(iterations, [&] { return scan(shared, index); });
    note_core();

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
        note_core();
    }

    // PdfCanvas::select_all, acquisition included.
    out.select_all_ms = median_ms(iterations, [&] {
        const auto t = shared.text_page(index);
        SelPoint a, b;
        return t.full_range(a, b) ? t.highlight(a, b).size() + t.copy(a, b).size()
                                  : std::size_t{0};
    });
    note_core();
    return true;
}

} // namespace litepdf::cli
