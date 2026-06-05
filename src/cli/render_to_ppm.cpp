#include "cli/render_to_ppm.hpp"

#include "core/Document.hpp"
#include "core/RenderEngine.hpp"

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>

// Keep this TU MuPDF-header-free, matching the PIMPL discipline of core's
// public headers and the render unit tests: forward-declare the handful of
// fz_* C-ABI entry points the PPM writer needs. They resolve at link time
// against libmupdf.lib: litepdf_core links litepdf::mupdf PRIVATE, and CMake
// propagates a static library's PRIVATE link dependencies to each consumer's
// final link, so both the litepdf-cli executable and the unit-test binary pull
// in libmupdf.lib without naming it directly.
extern "C" {
    struct fz_pixmap;
    struct fz_context;
    int            fz_pixmap_width(fz_context*, fz_pixmap*);
    int            fz_pixmap_height(fz_context*, fz_pixmap*);
    int            fz_pixmap_stride(fz_context*, fz_pixmap*);
    unsigned char* fz_pixmap_samples(fz_context*, fz_pixmap*);
    void           fz_drop_pixmap(fz_context*, fz_pixmap*);
}

namespace litepdf::cli {

namespace {

// Wait/result state shared between render_page_to_ppm's stack frame and the
// worker callback. Heap-allocated and co-owned via std::shared_ptr so that
// whichever side outlives the other keeps it alive. This is what makes the
// function teardown-safe: if the bounded wait times out, ~RenderEngine()
// drains the still-in-flight render and fires the callback during teardown —
// and because the callback holds its own shared_ptr, writing `done` /
// `got_pixmap` is well-defined regardless of stack-destruction order.
struct WaitState {
    std::mutex              m;
    std::condition_variable cv;
    bool                    done = false;
    bool                    got_pixmap = false;
};

} // namespace

int render_page_to_ppm(litepdf::core::Document& doc, int page, std::FILE* out,
                       std::chrono::milliseconds timeout) {
    auto state = std::make_shared<WaitState>();

    // `engine` is declared after `state`, so on return it is destroyed first:
    // ~RenderEngine() joins the worker and drains any still-in-flight render,
    // firing the callback below. The callback's by-value shared_ptr copy keeps
    // WaitState alive through that join regardless of stack order, so this
    // ordering is belt-and-suspenders, not load-bearing.
    litepdf::core::RenderEngine engine(doc, 1);
    engine.submit({
        page,
        0,        // priority (0 = highest)
        1.0f,     // scale (1.0 = 72 dpi)
        // Capture `state` BY VALUE (shared_ptr copy) and `out` by value so the
        // callback owns everything it touches. No stack locals are captured by
        // reference, so the engine destructor firing this callback during
        // teardown can never touch freed memory.
        [state, out](fz_pixmap* pix, fz_context* ctx) {
            if (pix) {
                const int            w      = fz_pixmap_width(ctx, pix);
                const int            h      = fz_pixmap_height(ctx, pix);
                const int            stride = fz_pixmap_stride(ctx, pix);
                const unsigned char* samples = fz_pixmap_samples(ctx, pix);

                // RenderEngine produces BGRA (fz_device_bgr + alpha=1). PPM's
                // P6 format is RGB with no alpha, so swap BGRA -> RGB at write
                // time. BGRA layout: byte[0]=B, byte[1]=G, byte[2]=R, byte[3]=A.
                std::fprintf(out, "P6\n%d %d\n255\n", w, h);
                for (int y = 0; y < h; ++y) {
                    const unsigned char* row =
                        samples + static_cast<std::size_t>(y) * stride;
                    for (int x = 0; x < w; ++x) {
                        const unsigned char* px =
                            row + static_cast<std::size_t>(x) * 4;
                        const unsigned char rgb[3] = {px[2], px[1], px[0]};
                        std::fwrite(rgb, 1, 3, out);
                    }
                }
                std::fflush(out);
                fz_drop_pixmap(ctx, pix);
            }

            std::lock_guard<std::mutex> g(state->m);
            state->got_pixmap = (pix != nullptr);
            state->done = true;
            state->cv.notify_all();
        }
    });

    {
        std::unique_lock<std::mutex> lk(state->m);
        if (!state->cv.wait_for(lk, timeout, [&] { return state->done; })) {
            return 3;  // timed out
        }
        // Read got_pixmap under the same lock that observed `done` so the
        // result is unambiguously synchronized with the callback's write.
        return state->got_pixmap ? 0 : 2;
    }
}

} // namespace litepdf::cli
