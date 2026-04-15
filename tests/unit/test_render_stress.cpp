// Task 11 — Phase 2 exit criterion stress test.
//
// Submits 1000 render requests cycling through pages of simple.pdf with
// a PageCache (L1=5, L2=10) and 2 render workers. Asserts that the
// PeakWorkingSetSize growth, measured from a post-warmup baseline, stays
// under 25 MB.
//
// Warmup: 4 pre-submits drain first-time allocation costs (MuPDF internal
// stores, per-worker fz_document open cost, thread stack reserve) so we
// isolate steady-state memory behavior for the 1000-render loop.

#include <catch2/catch_test_macros.hpp>
#include "core/Document.hpp"
#include "core/PageCache.hpp"
#include "core/RenderEngine.hpp"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>

// Windows PSAPI for peak working set measurement. Only compiled on Windows.
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#endif

extern "C" {
    struct fz_context;
    struct fz_pixmap;
    void fz_drop_pixmap(fz_context*, fz_pixmap*);
    void fz_drop_context(fz_context*);
}

TEST_CASE("Stress: 1000 renders stay under 25 MB RSS growth", "[core][render][stress]") {
#if !defined(_WIN32)
    SUCCEED("Stress test requires Windows psapi.h; skipped on non-Windows");
#else
    litepdf::core::Document doc;
    REQUIRE(!doc.open("tests/fixtures/simple.pdf").has_value());
    int pages = doc.page_count();
    REQUIRE(pages > 0);

    fz_context* cache_ctx = doc.clone_context();
    REQUIRE(cache_ctx);
    {
        litepdf::core::PageCache cache(5, 10, cache_ctx);
        litepdf::core::RenderEngine engine(doc, 2, &cache);

        // Warm-up: let the pool + worker_docs finish open cost before measuring.
        std::atomic<int> warmup_done{0};
        for (int i = 0; i < 4; ++i) {
            engine.submit({i % pages, 0, 1.0f, [&](fz_pixmap* p, fz_context* ctx){
                if (p) fz_drop_pixmap(ctx, p);
                ++warmup_done;
            }});
        }
        while (warmup_done.load() < 4) std::this_thread::sleep_for(std::chrono::milliseconds(10));

        // Baseline RSS AFTER warm-up (so we measure the steady-state).
        PROCESS_MEMORY_COUNTERS_EX before{};
        before.cb = sizeof(before);
        REQUIRE(GetProcessMemoryInfo(GetCurrentProcess(),
                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&before), sizeof(before)));

        std::atomic<int> done{0};
        for (int i = 0; i < 1000; ++i) {
            engine.submit({i % pages, 0, 1.0f, [&](fz_pixmap* p, fz_context* ctx){
                // D2: caller owns the pixmap ref; drop it here.
                if (p) fz_drop_pixmap(ctx, p);
                ++done;
            }});
        }
        // Drain
        while (done.load() < 1000) std::this_thread::sleep_for(std::chrono::milliseconds(5));

        PROCESS_MEMORY_COUNTERS_EX after{};
        after.cb = sizeof(after);
        REQUIRE(GetProcessMemoryInfo(GetCurrentProcess(),
                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&after), sizeof(after)));

        SIZE_T peak_growth = 0;
        if (after.PeakWorkingSetSize > before.PeakWorkingSetSize) {
            peak_growth = after.PeakWorkingSetSize - before.PeakWorkingSetSize;
        }
        INFO("Baseline peak WS: " << (before.PeakWorkingSetSize / (1024 * 1024)) << " MB");
        INFO("Final peak WS:    " << (after.PeakWorkingSetSize / (1024 * 1024)) << " MB");
        INFO("Peak WS growth:   " << (peak_growth / (1024 * 1024)) << " MB");
        REQUIRE(peak_growth < 25 * 1024 * 1024);
    }
    fz_drop_context(cache_ctx);
#endif
}
