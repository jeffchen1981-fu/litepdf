#include "core/DocumentView.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "core/PageCache.hpp"
#include "core/RenderEngine.hpp"

// DocumentView is MuPDF-aware only to the extent of holding an
// fz_context* and dropping it. Pull in fitz.h here (not in the header)
// so the PIMPL stays clean for UI consumers.
extern "C" {
struct fz_context;
void fz_drop_context(fz_context*);
}

namespace litepdf::core {

struct DocumentView::Impl {
    Document doc;

    // Order matters for destruction (see ~DocumentView):
    //   engine (joins workers) → cache (drops pixmaps via ui_ctx) → ui_ctx.
    fz_context*                   ui_ctx = nullptr;
    std::unique_ptr<PageCache>    cache;
    std::unique_ptr<RenderEngine> engine;

    int                    current_page = 0;
    DocumentView::ZoomMode zm           = DocumentView::ZoomMode::FitWidth;
    float                  scale        = 1.0f;
    float                  vp_w         = 0.0f;
    float                  vp_h         = 0.0f;
    float                  dpi          = 96.0f;

    // Preset custom zoom levels.
    static constexpr float kPresets[] = {
        0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f, 3.0f, 4.0f,
    };
};

DocumentView::DocumentView(Document doc,
                           std::size_t num_workers,
                           std::size_t l1_capacity,
                           std::size_t l2_capacity)
    : impl_(std::make_unique<Impl>()) {
    impl_->doc = std::move(doc);

    // Clone the UI-thread ctx BEFORE constructing cache/engine so that
    // a throw here doesn't leak half-built subobjects.
    impl_->ui_ctx = impl_->doc.clone_context();
    if (!impl_->ui_ctx) {
        throw std::runtime_error(
            "DocumentView: Document is not open (clone_context returned null)");
    }

    // Cache uses ui_ctx for its internal keep/drop.
    impl_->cache = std::make_unique<PageCache>(l1_capacity, l2_capacity, impl_->ui_ctx);

    // Engine clones its own per-worker contexts off the Document.
    impl_->engine =
        std::make_unique<RenderEngine>(impl_->doc, num_workers, impl_->cache.get());
}

DocumentView::~DocumentView() {
    if (!impl_) return;

    // Engine first — ensures no worker is mid-access to cache or ui_ctx.
    impl_->engine.reset();
    // Cache next — drops remaining pixmaps / display lists via ui_ctx.
    impl_->cache.reset();
    // UI ctx last.
    if (impl_->ui_ctx) {
        fz_drop_context(impl_->ui_ctx);
        impl_->ui_ctx = nullptr;
    }
    // Document cleans up automatically via its own Impl.
}

int DocumentView::page_count() const {
    return static_cast<int>(impl_->doc.page_count());
}

int DocumentView::current_page() const noexcept {
    return impl_->current_page;
}

bool DocumentView::set_current_page(int idx) {
    const int pc = page_count();
    if (pc <= 0) return false;
    const int max_idx = pc - 1;
    const int clamped = std::clamp(idx, 0, max_idx);
    if (clamped == impl_->current_page) return false;
    impl_->current_page = clamped;
    // Recompute scale if not Custom (page size may differ per page).
    if (impl_->zm != ZoomMode::Custom) {
        set_zoom_mode(impl_->zm, impl_->vp_w, impl_->vp_h, impl_->dpi);
    }
    return true;
}

DocumentView::ZoomMode DocumentView::zoom_mode() const noexcept {
    return impl_->zm;
}

float DocumentView::zoom_scale() const noexcept {
    return impl_->scale;
}

void DocumentView::set_zoom_mode(ZoomMode mode,
                                 float viewport_w_dip,
                                 float viewport_h_dip,
                                 float dpi) {
    impl_->zm  = mode;
    impl_->vp_w = viewport_w_dip;
    impl_->vp_h = viewport_h_dip;
    impl_->dpi  = dpi;

    if (page_count() <= 0) {
        impl_->scale = 1.0f;
        return;
    }

    const auto size = impl_->doc.page_size(static_cast<std::size_t>(impl_->current_page));
    const float pw = size.width_pt;
    const float ph = size.height_pt;

    // page_size is in points (1/72 inch). MuPDF's fz_scale applied during
    // rasterize produces a pixmap whose pixel extent is page_pt * scale.
    // We want that pixmap to cover vp_dip physical pixels, where the
    // monitor has `dpi` DPI. At 96 DPI, 1 DIP == 1 device px; at higher
    // DPI, 1 DIP == (dpi/96) device px.
    //
    //   FitWidth: page_pt_w * scale == vp_w_dip * (dpi/96)
    //   ⇒ scale_fit_w = vp_w_dip * (dpi/96) / page_pt_w
    const float phys_px_w = viewport_w_dip * (dpi / 96.0f);
    const float phys_px_h = viewport_h_dip * (dpi / 96.0f);
    const float fit_w     = (pw > 0.0f) ? (phys_px_w / pw) : 1.0f;
    const float fit_h     = (ph > 0.0f) ? (phys_px_h / ph) : 1.0f;

    switch (mode) {
        case ZoomMode::FitWidth:
            impl_->scale = fit_w;
            break;
        case ZoomMode::FitPage:
            impl_->scale = std::min(fit_w, fit_h);
            break;
        case ZoomMode::Custom:
            // Don't change scale — caller drives via zoom_in/zoom_out.
            break;
    }
}

bool DocumentView::zoom_in() {
    const float cur = impl_->scale;
    for (float lvl : Impl::kPresets) {
        if (lvl > cur + 1e-4f) {
            impl_->scale = lvl;
            impl_->zm    = ZoomMode::Custom;
            return true;
        }
    }
    return false;
}

bool DocumentView::zoom_out() {
    const float cur = impl_->scale;
    float best = -1.0f;
    for (float lvl : Impl::kPresets) {
        if (lvl < cur - 1e-4f && lvl > best) best = lvl;
    }
    if (best < 0.0f) return false;
    impl_->scale = best;
    impl_->zm    = ZoomMode::Custom;
    return true;
}

void DocumentView::request_render(int page, RenderCb on_complete) {
    RenderEngine::RenderRequest req;
    req.page_num    = page;
    req.priority    = 0;
    req.scale       = impl_->scale;
    req.on_complete = std::move(on_complete);
    (void)impl_->engine->submit(std::move(req));
}

void DocumentView::cancel_stale_renders(int keep_priority_threshold) {
    impl_->engine->cancel_all_below_priority(keep_priority_threshold);
}

fz_context* DocumentView::ui_ctx() const noexcept {
    return impl_->ui_ctx;
}

const std::filesystem::path& DocumentView::source_path() const {
    return impl_->doc.source_path();
}

}  // namespace litepdf::core
