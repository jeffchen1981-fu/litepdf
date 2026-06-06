#include "cli/bench_iteration.hpp"

#include "core/Document.hpp"
#include "core/RenderEngine.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>

// Keep this TU MuPDF-header-free, matching render_to_ppm.cpp and the PIMPL
// discipline of core's public headers: forward-declare the one fz_* C-ABI entry
// point the callback needs. It resolves at link time against libmupdf.lib via
// the litepdf::mupdf dependency that litepdf_core propagates to consumers (the
// litepdf-cli executable and the unit-test binary both pull it in without
// naming it directly).
extern "C" {
    struct fz_pixmap;
    struct fz_context;
    void fz_drop_pixmap(fz_context*, fz_pixmap*);
}

namespace litepdf::cli {

namespace {

using clock = std::chrono::steady_clock;

double to_ms(clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

// Wait/result state shared between run_one_iteration's stack frame and the
// worker callback. Heap-allocated and co-owned via std::shared_ptr so whichever
// side outlives the other keeps it alive. This is what makes the function
// teardown-safe: if the bounded wait times out, ~RenderEngine() drains the
// still-in-flight render and fires the callback during teardown — and because
// the callback holds its own shared_ptr copy, writing render_end / got_pixmap /
// done is well-defined regardless of stack-destruction order. (Same
// construction as render_to_ppm.cpp's WaitState; this variant also carries the
// render-end timestamp the benchmark's render_ms needs.)
struct WaitState {
    std::mutex              m;
    std::condition_variable cv;
    bool                    done = false;
    bool                    got_pixmap = false;
    clock::time_point       render_end;
};

} // namespace

bool run_one_iteration(const char* path, BenchIteration& out,
                       std::chrono::milliseconds timeout) {
    auto t0 = clock::now();
    litepdf::core::Document doc;
    auto err = doc.open(path);
    auto t1 = clock::now();
    if (err) {
        std::fprintf(stderr, "Open error: %d\n", static_cast<int>(*err));
        return false;
    }
    out.open_ms = to_ms(t1 - t0);

    auto state = std::make_shared<WaitState>();

    // `engine` is declared after `state`, so on return it is destroyed first:
    // ~RenderEngine() joins the worker and drains any still-in-flight render,
    // firing the callback below. The callback's by-value shared_ptr copy keeps
    // WaitState alive through that join regardless of stack order, so this
    // ordering is belt-and-suspenders, not load-bearing.
    auto t2 = clock::now();
    litepdf::core::RenderEngine engine(doc, 1);
    auto t3 = clock::now();
    out.engine_init_ms = to_ms(t3 - t2);

    auto render_start = clock::now();
    engine.submit({
        0,        // page_num
        0,        // priority (0 = highest)
        1.0f,     // scale (1.0 = 72 dpi)
        // Capture `state` BY VALUE (shared_ptr copy) so the callback owns the
        // wait/result state it writes. No stack locals are captured by
        // reference, so the engine destructor firing this callback during
        // teardown can never touch freed memory.
        [state](fz_pixmap* pix, fz_context* ctx) {
            auto end = clock::now();
            std::lock_guard<std::mutex> g(state->m);
            state->render_end = end;
            state->got_pixmap = (pix != nullptr);
            if (pix) fz_drop_pixmap(ctx, pix);
            state->done = true;
            state->cv.notify_all();
        }
    });

    {
        std::unique_lock<std::mutex> lk(state->m);
        if (!state->cv.wait_for(lk, timeout, [&] { return state->done; })) {
            std::fprintf(stderr, "Render timed out\n");
            return false;
        }
    }
    if (!state->got_pixmap) {
        std::fprintf(stderr, "Render produced no pixmap\n");
        return false;
    }
    // render_start is a stack local read only here, on the calling thread, and
    // only on the success path. state->render_end was written by the callback
    // under state->m; we observed `done` under the same lock, so that write
    // happens-before this read.
    out.render_ms = to_ms(state->render_end - render_start);
    out.open_render_ms = out.open_ms + out.engine_init_ms + out.render_ms;
    return true;
}

} // namespace litepdf::cli
