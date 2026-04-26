#include "core/ThumbnailRenderer.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

extern "C" {
#include <mupdf/fitz.h>
}

namespace litepdf::core {

namespace {
// Convert the engine's BGRA pixmap to a Win32 32-bit top-down DIBSection.
// The engine produces BGRA via fz_device_bgr + alpha=1 (RenderEngine.cpp:177),
// so input is always 4-channel BGRA in row-major order. Stride math is
// reused from PdfCanvas's existing pattern; we deliberately avoid calling
// fz_pixmap_components (which returns colorant count *excluding* alpha and
// would mislead a switch-on-n).
HBITMAP pixmap_to_hbitmap(fz_pixmap* pix, fz_context* ctx) {
    if (!pix) return nullptr;
    const int w      = fz_pixmap_width(ctx, pix);
    const int h      = fz_pixmap_height(ctx, pix);
    const int stride = fz_pixmap_stride(ctx, pix);
    if (w <= 0 || h <= 0 || stride < w * 4) return nullptr;  // assume BGRA

    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;  // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bm = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bm || !bits) {
        if (bm) DeleteObject(bm);
        return nullptr;
    }

    const unsigned char* src = fz_pixmap_samples(ctx, pix);
    auto* dst = static_cast<unsigned char*>(bits);
    const int dst_stride = w * 4;
    // BGRA -> BGRA: same channel order. Copy row-by-row honoring stride
    // (which may be > w*4 due to alignment padding).
    for (int y = 0; y < h; ++y) {
        std::memcpy(dst + y * dst_stride, src + y * stride,
                    static_cast<size_t>(w) * 4);
    }
    return bm;
}
}  // namespace

struct ThumbnailRenderer::Impl {
    RenderEngine&    engine;
    float            scale;
    std::atomic<int> pending_tasks{0};

    Impl(RenderEngine& e, float s) : engine(e), scale(s) {}

    // D16: spin-wait until every in-flight on_complete has decremented
    // pending_tasks. Mirrors Phase 6 C1's pattern in core::SearchSession.
    ~Impl() {
        while (pending_tasks.load(std::memory_order_acquire) > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
};

ThumbnailRenderer::ThumbnailRenderer(RenderEngine& engine, float scale)
  : impl_(std::make_unique<Impl>(engine, scale)) {}

ThumbnailRenderer::~ThumbnailRenderer() = default;

void ThumbnailRenderer::submit(int page, OnDone on_done) {
    // D16: increment BEFORE submit so a synchronous worker dispatch can
    // never observe pending_tasks=0 while work is actually in flight.
    impl_->pending_tasks.fetch_add(1, std::memory_order_relaxed);
    RenderEngine::RenderRequest req;
    req.page_num     = page;
    req.priority     = 3;
    req.scale        = impl_->scale;
    req.bypass_cache = true;  // Task 0.5: thumb pixmaps don't touch L1/L2.
    req.on_complete = [impl = impl_.get(), cb = std::move(on_done)]
                      (fz_pixmap* pix, fz_context* ctx) {
        HBITMAP bm = pixmap_to_hbitmap(pix, ctx);
        if (pix) fz_drop_pixmap(ctx, pix);
        cb(bm);
        // RAII-decrement at the end so the dtor's spin-wait observes
        // completion only after the user callback has fully run.
        impl->pending_tasks.fetch_sub(1, std::memory_order_release);
    };
    impl_->engine.submit(std::move(req));
}

void ThumbnailRenderer::cancel_pending() {
    // Cancel anything currently below priority 3 in the engine's queue.
    // In-flight workers cooperatively check the cancel flag at safe
    // points; their on_complete still fires (with pix possibly null),
    // so pending_tasks still decrements correctly.
    impl_->engine.cancel_all_below_priority(3);
}

}  // namespace litepdf::core
