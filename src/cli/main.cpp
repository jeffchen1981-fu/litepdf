// litepdf-cli — console demo + benchmark harness.
// Opens a document and prints metadata, first-page size, a text snippet,
// and the outline. Used manually during development and by Phase 11
// benchmarks. Not user-facing — the GUI is litepdf.exe.
//
// Also supports `--render N` to render page N via RenderEngine and emit
// a binary PPM (P6) image to stdout. Used for manual smoke-checking the
// render path during Phase 2+ development.
//
// `--bench-selection [--page N] [--iterations N]` times the text-selection
// work PdfCanvas does on the UI thread, page by page (#73; bench_selection.hpp).

#include "cli/bench_iteration.hpp"
#include "cli/bench_selection.hpp"
#include "cli/render_to_ppm.hpp"
#include "core/Document.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {

double vmin(const std::vector<double>& v) {
    double best = v.front();
    for (double x : v) if (x < best) best = x;
    return best;
}

double vmedian(std::vector<double> v) {  // by value: sorts a copy
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    if (n % 2 == 1) return v[n / 2];
    return 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

double vstddev(const std::vector<double>& v) {  // sample stddev (N-1)
    if (v.size() < 2) return 0.0;
    double mean = 0.0;
    for (double x : v) mean += x;
    mean /= static_cast<double>(v.size());
    double acc = 0.0;
    for (double x : v) acc += (x - mean) * (x - mean);
    return std::sqrt(acc / static_cast<double>(v.size() - 1));
}

// Runs `iterations` cold renders and reports best-of-N min of each metric.
// --json emits one compact object on stdout (the per-fixture shape
// benchmark.ps1 nests under the fixture key, §3.2). Returns 0 on success,
// 1 if any iteration fails to open/render.
int run_benchmark(const char* path, int iterations, bool json) {
    std::vector<double> open_s, init_s, render_s, total_s;
    open_s.reserve(static_cast<std::size_t>(iterations));
    init_s.reserve(static_cast<std::size_t>(iterations));
    render_s.reserve(static_cast<std::size_t>(iterations));
    total_s.reserve(static_cast<std::size_t>(iterations));

    for (int i = 0; i < iterations; ++i) {
        litepdf::cli::BenchIteration it;
        if (!litepdf::cli::run_one_iteration(path, it, std::chrono::seconds(10))) {
            std::fprintf(stderr, "Benchmark failed on iteration %d for %s\n", i, path);
            return 1;
        }
        open_s.push_back(it.open_ms);
        init_s.push_back(it.engine_init_ms);
        render_s.push_back(it.render_ms);
        total_s.push_back(it.open_render_ms);
    }

    const double open_min   = vmin(open_s);
    const double init_min   = vmin(init_s);
    const double render_min = vmin(render_s);
    const double total_min  = vmin(total_s);
    const double median     = vmedian(total_s);
    const double stddev     = vstddev(total_s);

    if (json) {
        std::printf("{");
        std::printf("\"open_ms\":%.4f,", open_min);
        std::printf("\"engine_init_ms\":%.4f,", init_min);
        std::printf("\"render_ms\":%.4f,", render_min);
        std::printf("\"open_render_ms\":%.4f,", total_min);
        std::printf("\"samples\":[");
        for (std::size_t i = 0; i < total_s.size(); ++i) {
            std::printf("%s%.4f", (i ? "," : ""), total_s[i]);
        }
        std::printf("],");
        std::printf("\"median_ms\":%.4f,", median);
        std::printf("\"stddev_ms\":%.4f", stddev);
        std::printf("}\n");
    } else {
        std::printf("Benchmark %s (best-of-%d):\n", path, iterations);
        std::printf("  open_ms        : %.3f\n", open_min);
        std::printf("  engine_init_ms : %.3f\n", init_min);
        std::printf("  render_ms      : %.3f\n", render_min);
        std::printf("  open_render_ms : %.3f  (median %.3f, stddev %.3f over %d)\n",
                    total_min, median, stddev, iterations);
    }
    return 0;
}

// --bench-selection (#73): one row per page, then each column's worst page and
// how the first-acquisition figures are spread. Human-readable only; nothing
// gates on these numbers.
int run_selection_benchmark(const char* path, int only_page, int iterations) {
    litepdf::core::Document doc;   // the shared one; see SelectionPageTiming
    if (auto err = doc.open(path)) {
        std::fprintf(stderr, "Open error: %d\n", static_cast<int>(*err));
        return 1;
    }
    const int count = static_cast<int>(doc.page_count());
    if (only_page >= count) {
        std::fprintf(stderr, "--page %d is out of range (%d pages)\n", only_page + 1, count);
        return 2;
    }
    const int begin = only_page >= 0 ? only_page : 0;
    const int end   = only_page >= 0 ? only_page + 1 : count;

    // Keep the file cache and efficiency cores out of the figures as far as the
    // harness can; see bench_selection.hpp.
    {
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        if (ec || litepdf::cli::warm_file_cache(path) != size) {
            std::fprintf(stderr, "Could not read the whole file ahead; first-read "
                                 "columns may include disk waits\n");
        }
    }
    if (!litepdf::cli::prefer_performance_cores()) {
        std::fprintf(stderr, "Could not opt out of EcoQoS; watch for E-marked rows\n");
    }

    std::printf("Selection cost %s (ms; medians of %d except scan1st/acq1st/acqseq, "
                "one sample each; move/release columns are Chars/Words/Lines; file "
                "cache warmed first, so cold-cache disk waits are excluded; E = timed partly on "
                "an efficiency core)\n",
                path, iterations);
    std::printf("%5s %6s %5s | %7s %7s %7s %7s | %7s | %-23s | %-23s | %-23s | %7s\n",
                "page", "chars", "quads", "scan1st", "acq1st", "acqseq", "acquire",
                "search", "move to page end", "move, extent at anchor",
                "release at page end", "sel-all");

    std::vector<litepdf::cli::SelectionPageTiming> rows;
    for (int p = begin; p < end; ++p) {
        litepdf::cli::SelectionPageTiming t;
        // A page MuPDF cannot load is reported and skipped: one broken page in a
        // real document should not cost the numbers for the rest.
        if (!litepdf::cli::bench_selection_page(path, doc, p, iterations, t)) continue;
        rows.push_back(t);
        std::printf("%5d %6zu %5zu | %7.3f %7.3f %7.3f %7.3f | %7.3f | %7.3f %7.3f %7.3f | "
                    "%7.3f %7.3f %7.3f | %7.3f %7.3f %7.3f | %7.3f%s\n",
                    t.page + 1, t.chars, t.quads, t.scan_first_ms, t.acquire_first_ms,
                    t.acquire_seq_ms, t.acquire_ms, t.search_ms,
                    t.move_full_ms[0], t.move_full_ms[1], t.move_full_ms[2],
                    t.move_short_ms[0], t.move_short_ms[1], t.move_short_ms[2],
                    t.release_full_ms[0], t.release_full_ms[1], t.release_full_ms[2],
                    t.select_all_ms, t.efficiency_core ? " E" : "");
    }

    if (rows.empty()) {
        std::fprintf(stderr, "No page could be measured\n");
        return 1;
    }
    using T = litepdf::cli::SelectionPageTiming;
    // Each figure's worst page, with the page (1-based) it came from.
    const auto worst = [&rows](const char* name, auto field) {
        const T* w = &rows.front();
        for (const auto& r : rows) if (field(r) > field(*w)) w = &r;
        std::printf("  %-26s %8.3f ms  (page %d, %zu chars)\n",
                    name, field(*w), w->page + 1, w->chars);
    };
    std::printf("Worst page per figure:\n");
    worst("search scan, first visit", [](const T& r) { return r.scan_first_ms; });
    worst("acquire, first in tab",    [](const T& r) { return r.acquire_first_ms; });
    worst("acquire, after prior pages", [](const T& r) { return r.acquire_seq_ms; });
    worst("acquire",                  [](const T& r) { return r.acquire_ms; });
    worst("search scan",              [](const T& r) { return r.search_ms; });
    worst("move to page end, Chars",  [](const T& r) { return r.move_full_ms[0]; });
    worst("move to page end, Words",  [](const T& r) { return r.move_full_ms[1]; });
    worst("move to page end, Lines",  [](const T& r) { return r.move_full_ms[2]; });
    worst("release, Words",           [](const T& r) { return r.release_full_ms[1]; });
    worst("select all",               [](const T& r) { return r.select_all_ms; });

    // The one-sample figures vary most from page to page, so a worst case
    // alone misleads: give their median and how many pages cross one 60 Hz
    // frame (16.7 ms) and 100 ms.
    const auto spread = [&rows](const char* name, auto field) {
        std::vector<double> v;
        v.reserve(rows.size());
        for (const auto& r : rows) v.push_back(field(r));
        std::sort(v.begin(), v.end());
        const std::size_t over_frame = static_cast<std::size_t>(
            v.end() - std::upper_bound(v.begin(), v.end(), 16.7));
        const std::size_t over_100 = static_cast<std::size_t>(
            v.end() - std::upper_bound(v.begin(), v.end(), 100.0));
        std::printf("  %-26s median %8.3f ms, %zu of %zu pages > 16.7 ms, %zu > 100 ms\n",
                    name, vmedian(v), over_frame, v.size(), over_100);
    };
    std::printf("Spread:\n");
    spread("search scan, first visit",   [](const T& r) { return r.scan_first_ms; });
    spread("acquire, first in tab",      [](const T& r) { return r.acquire_first_ms; });
    spread("acquire, after prior pages", [](const T& r) { return r.acquire_seq_ms; });

    const auto on_e = std::count_if(rows.begin(), rows.end(),
                                    [](const T& r) { return r.efficiency_core; });
    if (on_e > 0) {
        std::printf("Warning: %zu of %zu pages were timed partly on an efficiency core "
                    "(marked E), which runs this work about half as fast. To exclude it, "
                    "re-run under `start /affinity <mask of the performance cores>`.\n",
                    static_cast<std::size_t>(on_e), rows.size());
    }
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr,
            "Usage: %s <file> [--render N | --benchmark [--iterations N] [--json]\n"
            "                  | --bench-selection [--page N] [--iterations N]]\n",
            argv[0]);
        return 2;
    }

    const char* path = argv[1];
    int render_page = -1;
    bool benchmark = false;
    int iterations = 5;
    bool iterations_set = false;
    bool json = false;
    bool bench_selection = false;
    int selection_page = -1;   // 0-based; -1 = every page
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--render") == 0 && i + 1 < argc) {
            render_page = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--benchmark") == 0) {
            benchmark = true;
        } else if (std::strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
            iterations = std::atoi(argv[++i]);
            iterations_set = true;
        } else if (std::strcmp(argv[i], "--json") == 0) {
            json = true;
        } else if (std::strcmp(argv[i], "--bench-selection") == 0) {
            bench_selection = true;
        } else if (std::strcmp(argv[i], "--page") == 0) {
            // A missing value is an error, not "every page": that would turn a
            // typo into a whole-document run, minutes long on a big file.
            if (i + 1 >= argc) {
                std::fprintf(stderr, "--page needs a page number\n");
                return 2;
            }
            selection_page = std::atoi(argv[++i]) - 1;   // 1-based on the command line
            if (selection_page < 0) {
                std::fprintf(stderr, "--page must be >= 1\n");
                return 2;
            }
        }
    }

    if (bench_selection) {
        if (benchmark || render_page >= 0 || json) {
            std::fprintf(stderr,
                "--bench-selection excludes --benchmark, --render and --json\n");
            return 2;
        }
        // Most samples are one sub-millisecond call, so take more than --benchmark's 5.
        if (!iterations_set) iterations = 25;
        if (iterations < 1) {
            std::fprintf(stderr, "--iterations must be >= 1\n");
            return 2;
        }
        return run_selection_benchmark(path, selection_page, iterations);
    }
    if (selection_page >= 0) {
        std::fprintf(stderr, "--page is only valid with --bench-selection\n");
        return 2;
    }

    if (benchmark && render_page >= 0) {
        std::fprintf(stderr, "--benchmark and --render are mutually exclusive\n");
        return 2;
    }
    // --iterations / --json are only meaningful with --benchmark (spec §3.1).
    if (!benchmark && (json || iterations_set)) {
        std::fprintf(stderr, "--iterations/--json are only valid with --benchmark "
                             "(--iterations also with --bench-selection)\n");
        return 2;
    }
    if (benchmark && iterations < 1) {
        std::fprintf(stderr, "--iterations must be >= 1\n");
        return 2;
    }
    if (benchmark) {
        return run_benchmark(path, iterations, json);
    }

    litepdf::core::Document doc;
    auto err = doc.open(path);
    if (err) {
        std::fprintf(stderr, "Open error: %d\n", static_cast<int>(*err));
        return 1;
    }

    if (render_page >= 0) {
#ifdef _WIN32
        // Ensure stdout emits raw bytes (no CRLF translation) on Windows.
        std::fflush(stdout);
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        // The render + bounded-wait + teardown sequence lives in
        // render_page_to_ppm so it is unit-testable and teardown-safe by
        // construction (see render_to_ppm.{hpp,cpp} and
        // test_cli_render_teardown.cpp). Exit codes: 0 ok, 2 no pixmap,
        // 3 timeout — preserved from the original inline implementation.
        //
        // Note on rc == 3: a slow render that only finishes while
        // ~RenderEngine() drains it (inside the helper) still emits its
        // complete PPM to stdout during teardown — same as the original
        // inline path. The exit code, not the presence of stdout bytes, is
        // the authoritative success signal for this dev/smoke CLI.
        const int rc = litepdf::cli::render_page_to_ppm(
            doc, render_page, stdout, std::chrono::seconds(10));
        // Surface the failure codes on stderr. rc == 2 was historically silent
        // on this path; emitting it here matches the benchmark helper's
        // diagnostics and makes an out-of-range page visible.
        if (rc == 2) std::fprintf(stderr, "Render produced no pixmap (page %d)\n", render_page);
        else if (rc == 3) std::fprintf(stderr, "Render timed out\n");
        return rc;
    }

    const std::size_t n = doc.page_count();
    std::printf("File: %s\n", path);
    std::printf("Pages: %zu\n", n);

    if (n > 0) {
        auto size = doc.page_size(0);
        std::printf("First page: %.1f x %.1f pt\n", size.width_pt, size.height_pt);

        std::string text = doc.page_text(0);
        if (text.size() > 200) text.resize(200);
        std::printf("First-page text snippet:\n%s\n", text.c_str());
    }

    const auto outline = doc.outline();
    if (!outline.empty()) {
        std::printf("Outline (%zu entries):\n", outline.size());
        for (const auto& e : outline) {
            std::size_t page_display = (e.page_index == litepdf::core::Document::kNoPage)
                                           ? 0
                                           : e.page_index + 1;
            std::printf("  %*s- %s (page %zu)\n",
                        e.depth * 2, "", e.title.c_str(), page_display);
        }
    }

    return 0;
}
