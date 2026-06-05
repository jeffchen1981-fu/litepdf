// litepdf-cli — console demo + benchmark harness.
// Opens a document and prints metadata, first-page size, a text snippet,
// and the outline. Used manually during development and by Phase 11
// benchmarks. Not user-facing — the GUI is litepdf.exe.
//
// Also supports `--render N` to render page N via RenderEngine and emit
// a binary PPM (P6) image to stdout. Used for manual smoke-checking the
// render path during Phase 2+ development.

#include "cli/render_to_ppm.hpp"
#include "core/Document.hpp"
#include "core/RenderEngine.hpp"

#include <mupdf/fitz.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {

struct BenchIteration {
    double open_ms = 0.0;
    double engine_init_ms = 0.0;
    double render_ms = 0.0;
    double open_render_ms = 0.0;
};

// Render page 0 once on a FRESH Document + RenderEngine (no PageCache, one
// worker) and fill `it`. Mirrors the --render await/drop pattern below: the
// callback fires on the worker thread; we block on a condition_variable with a
// 10 s timeout and fz_drop_pixmap inside the callback. Returns false (and
// prints to stderr) if open or render fails — no silent zero metric.
//
// engine_init_ms is NOT zero: the RenderEngine constructor opens the
// per-worker fz_document, so a regression in worker-side open lands here, not
// in render_ms. Gating the total open_render_ms makes the boundary irrelevant.
bool run_one_iteration(const char* path, BenchIteration& it) {
    using clock = std::chrono::steady_clock;
    auto to_ms = [](clock::duration d) {
        return std::chrono::duration<double, std::milli>(d).count();
    };

    auto t0 = clock::now();
    litepdf::core::Document doc;
    auto err = doc.open(path);
    auto t1 = clock::now();
    if (err) {
        std::fprintf(stderr, "Open error: %d\n", static_cast<int>(*err));
        return false;
    }
    it.open_ms = to_ms(t1 - t0);

    // Declare the sync primitives BEFORE `engine` so they outlive it. Locals
    // destroy in reverse declaration order, so `engine` (last) is destroyed
    // first: ~RenderEngine() joins/drains any in-flight job — which may fire
    // the callback below on a worker thread — while m/cv/render_end are still
    // alive. Reversing this order would let the timeout path (returns false
    // with a job still queued) lock a destroyed mutex / write a destroyed
    // time_point in the drained callback = UB.
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    bool ok = false;
    clock::time_point render_end;

    auto t2 = clock::now();
    litepdf::core::RenderEngine engine(doc, 1, nullptr);
    auto t3 = clock::now();
    it.engine_init_ms = to_ms(t3 - t2);

    auto render_start = clock::now();
    engine.submit({
        0,        // page_num
        0,        // priority (0 = highest)
        1.0f,     // scale (1.0 = 72 dpi)
        [&](fz_pixmap* pix, fz_context* ctx) {
            auto end = clock::now();
            std::lock_guard<std::mutex> g(m);
            render_end = end;
            ok = (pix != nullptr);
            if (pix) fz_drop_pixmap(ctx, pix);
            done = true;
            cv.notify_all();
        }
    });

    {
        std::unique_lock<std::mutex> lk(m);
        if (!cv.wait_for(lk, std::chrono::seconds(10), [&] { return done; })) {
            std::fprintf(stderr, "Render timed out\n");
            return false;
        }
    }
    if (!ok) {
        std::fprintf(stderr, "Render produced no pixmap\n");
        return false;
    }
    it.render_ms = to_ms(render_end - render_start);
    it.open_render_ms = it.open_ms + it.engine_init_ms + it.render_ms;
    return true;
}

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
        BenchIteration it;
        if (!run_one_iteration(path, it)) {
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

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr,
            "Usage: %s <file> [--render N | --benchmark [--iterations N] [--json]]\n",
            argv[0]);
        return 2;
    }

    const char* path = argv[1];
    int render_page = -1;
    bool benchmark = false;
    int iterations = 5;
    bool iterations_set = false;
    bool json = false;
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
        }
    }

    if (benchmark && render_page >= 0) {
        std::fprintf(stderr, "--benchmark and --render are mutually exclusive\n");
        return 2;
    }
    // --iterations / --json are only meaningful with --benchmark (spec §3.1).
    if (!benchmark && (json || iterations_set)) {
        std::fprintf(stderr, "--iterations/--json are only valid with --benchmark\n");
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
        if (rc == 3) std::fprintf(stderr, "Render timed out\n");
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
