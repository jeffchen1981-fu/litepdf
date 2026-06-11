#include "core/DocumentView.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "app/SearchDispatcher.hpp"
#include "core/PageCache.hpp"
#include "core/RenderEngine.hpp"
#include "core/SearchSession.hpp"
#include "core/ThumbCache.hpp"
#include "core/ThumbnailRenderer.hpp"
#include "ui/ThumbnailPane.hpp"

// DocumentView is MuPDF-aware only to the extent of holding an
// fz_context* and dropping it. Pull in fitz.h here (not in the header)
// so the PIMPL stays clean for UI consumers.
extern "C" {
struct fz_context;
struct fz_pixmap;
void fz_drop_context(fz_context*);
void fz_drop_pixmap(fz_context*, fz_pixmap*);
}

namespace litepdf::core {

struct DocumentView::Impl {
    // NOTE: declaration order == reverse destruction order. Members
    // declared LATER destruct EARLIER. The required tear-down order is
    // (first to last):
    //   session → engine → cache → cache_ctx → ui_ctx → doc
    // so `session` must be declared LAST (destructs first) and `doc`
    // FIRST (destructs last). See DocumentView.hpp for rationale.
    Document doc;

    // ui_ctx is UI-thread-exclusive (D3). cache_ctx is a dedicated clone
    // used by PageCache for its internal keep/drop ops from worker
    // threads — MuPDF's lock table serializes refcount updates across
    // clones of the same root, so this is safe. Both are raw fz_context*
    // (dropped explicitly in ~DocumentView); they do not RAII-destruct
    // in reverse-declaration order.
    fz_context*                   ui_ctx    = nullptr;
    fz_context*                   cache_ctx = nullptr;
    std::unique_ptr<PageCache>    cache;
    std::unique_ptr<RenderEngine> engine;

    int                    current_page = 0;
    DocumentView::ZoomMode zm           = DocumentView::ZoomMode::FitWidth;
    float                  scale        = 1.0f;
    float                  vp_w         = 0.0f;
    float                  vp_h         = 0.0f;
    float                  dpi          = 96.0f;
    bool                   invert_colors = false;  // Phase 8 D7/D9
    bool                   dual_page     = false;  // Phase 8 D10

    // Preset custom zoom levels.
    static constexpr float kPresets[] = {
        0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f, 3.0f, 4.0f,
    };

    // Declared LAST so it destructs FIRST — see order contract above.
    // The SearchSession holds non-owning refs to `doc` and to the
    // externally-owned ISearchDispatcher; both must outlive the session.
    // `doc` is declared above (destructs after session) ✓.
    // Dispatcher is owned by MainWindow and outlives all DocumentViews ✓.
    std::unique_ptr<SearchSession> session;

    // Phase 7 Task 8: per-tab thumbnail trio. Lazily created on first F4
    // press for this tab via ensure_thumb_pane(). Member declaration
    // order (and therefore C++ destruction order) matters:
    //   thumb_cache    declared FIRST  → destructs LAST  (cache outlives renderer + pane)
    //   thumb_renderer declared SECOND → destructs MIDDLE (renderer outlives pane)
    //   thumb_pane     declared LAST   → destructs FIRST  (pane HWND tears down before
    //                                      renderer dtor's worker drain)
    //
    // Why: ThumbnailPane's dtor calls ThumbnailRenderer::cancel_pending(),
    // so the renderer must still exist when the pane is destroyed. The
    // renderer's own dtor (D16) then spin-drains its workers — by the
    // time it returns, no PostMessageW(WM_USER_THUMB_READY) is in flight
    // that could target the (now-freed) pane HWND. Cache outlives the
    // renderer because in-flight render callbacks blit into the cache
    // via the pane's WM_USER_THUMB_READY handler — but since the pane is
    // destroyed first, that handler can't fire after pane teardown, so
    // cache could in principle be freed earlier. Keeping cache last is
    // defense-in-depth against future paint paths that touch the cache
    // directly.
    //
    // These three are constructed together in ensure_thumb_pane(), so
    // they share a "all-or-none" lifecycle: thumb_pane != nullptr iff
    // both thumb_cache and thumb_renderer are also non-null.
    //
    // The pane subclasses MainWindow's HWND. MainWindow owns `tabs_`
    // (TabManager), which owns the Tab list, which owns each
    // DocumentView. tabs_ is declared in MainWindow.hpp BEFORE the HWND
    // tear-down sites (matches the find_bar_ pattern), so the pane's
    // RemoveWindowSubclass runs against a still-live HWND.
    std::unique_ptr<ThumbCache>             thumb_cache;
    std::unique_ptr<ThumbnailRenderer>      thumb_renderer;
    std::unique_ptr<litepdf::ui::ThumbnailPane> thumb_pane;
};

DocumentView::DocumentView(Document doc,
                           litepdf::app::ISearchDispatcher& dispatcher,
                           std::size_t num_workers,
                           std::size_t l1_capacity,
                           std::size_t l2_capacity)
    : impl_(std::make_unique<Impl>()) {
    impl_->doc = std::move(doc);

    // Clone the UI-thread ctx and a dedicated cache_ctx BEFORE
    // constructing cache/engine so that a throw here doesn't leak
    // half-built subobjects. ui_ctx stays UI-thread-exclusive (D3);
    // cache_ctx is conceptually "the cache's own ctx" and is used by
    // workers via PageCache for refcount ops.
    impl_->ui_ctx    = impl_->doc.clone_context();
    impl_->cache_ctx = impl_->doc.clone_context();
    if (!impl_->ui_ctx || !impl_->cache_ctx) {
        if (impl_->cache_ctx) { fz_drop_context(impl_->cache_ctx); impl_->cache_ctx = nullptr; }
        if (impl_->ui_ctx)    { fz_drop_context(impl_->ui_ctx);    impl_->ui_ctx    = nullptr; }
        throw std::runtime_error(
            "DocumentView: Document is not open (clone_context returned null)");
    }

    // Cache uses cache_ctx (not ui_ctx) for its internal keep/drop —
    // those ops happen on worker threads.
    impl_->cache = std::make_unique<PageCache>(l1_capacity, l2_capacity, impl_->cache_ctx);

    // Engine clones its own per-worker contexts off the Document.
    impl_->engine =
        std::make_unique<RenderEngine>(impl_->doc, num_workers, impl_->cache.get());

    // Construct SearchSession LAST — it references impl_->doc (already
    // moved into place) and the caller-owned dispatcher. If this throws
    // (shouldn't, current ctor is noexcept-ish), earlier members RAII-
    // unwind correctly via Impl's declaration order.
    impl_->session = std::make_unique<SearchSession>(impl_->doc, dispatcher);
}

DocumentView::~DocumentView() {
    if (!impl_) return;

    // Phase 7 Task 8: tear the thumb trio down BEFORE engine/session so
    // the pane's cancel_in_flight (called from ~ThumbnailPane) reaches
    // a still-alive renderer, the renderer's worker-drain (D16) reaches
    // a still-alive RenderEngine, and the cache outlives both. The
    // explicit reset() ordering mirrors Impl's declaration order so it
    // is just-in-case redundancy with the natural reverse-declaration
    // order (thumb_pane → thumb_renderer → thumb_cache).
    impl_->thumb_pane.reset();
    impl_->thumb_renderer.reset();
    impl_->thumb_cache.reset();

    // Session first — drops the strong ref to its shared_ptr<State>.
    // Any in-flight worker task keeps its own shared_ptr alive until it
    // completes; the task's captured `const Document&` stays valid
    // because impl_->doc is destroyed LAST (below, via Impl unwinding).
    impl_->session.reset();
    // Engine next — ensures no worker is mid-access to cache or contexts.
    impl_->engine.reset();
    // Cache next — drops remaining pixmaps / display lists via cache_ctx.
    impl_->cache.reset();
    // cache_ctx before ui_ctx; both before doc (doc destructs via Impl).
    if (impl_->cache_ctx) {
        fz_drop_context(impl_->cache_ctx);
        impl_->cache_ctx = nullptr;
    }
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

void DocumentView::set_zoom_scale(float scale) noexcept {
    const float lo = Impl::kPresets[0];                                                      // 0.5f
    const float hi = Impl::kPresets[sizeof(Impl::kPresets)/sizeof(Impl::kPresets[0]) - 1];  // 4.0f
    if (scale < lo) scale = lo;
    if (scale > hi) scale = hi;
    impl_->zm    = ZoomMode::Custom;
    impl_->scale = scale;
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
    req.invert      = impl_->invert_colors;  // Phase 8 D7
    (void)impl_->engine->submit(std::move(req));
}

bool DocumentView::invert_colors() const noexcept {
    return impl_->invert_colors;
}

void DocumentView::set_invert_colors(bool on) {
    if (impl_->invert_colors == on) return;
    impl_->invert_colors = on;
    // (Phase 8 D9 addendum) Drain in-flight renders submitted at the
    // OLD polarity so their on_complete callbacks do not deliver stale-
    // polarity pixmaps that briefly flash the wrong colors. Phase 7's
    // cancel checkpoints invoke on_complete(nullptr) on cancelled
    // requests, which the canvas treats as a no-paint event. The caller
    // (MainWindow toggle handler) is then responsible for kicking a
    // fresh render at the new polarity.
    impl_->engine->cancel_all_below_priority(0);
}

bool DocumentView::dual_page() const noexcept {
    return impl_->dual_page;
}

void DocumentView::set_dual_page(bool on) {
    if (impl_->dual_page == on) return;
    impl_->dual_page = on;
    // (Phase 8 D9 addendum / D10) Drain in-flight single-page renders
    // so their bitmaps don't land in the wrong slot of the new layout.
    // Caller kicks a fresh dual-page render afterwards.
    impl_->engine->cancel_all_below_priority(0);
}

void DocumentView::cancel_stale_renders(int keep_priority_threshold) {
    impl_->engine->cancel_all_below_priority(keep_priority_threshold);
}

void DocumentView::request_render_with_prefetch(int page, RenderCb cb) {
    // 1. Drop any pending P1/P2 from a prior nav so the queue stays
    //    short during rapid PgDn scrubbing.
    cancel_stale_renders(0);

    // 2. P0 — current page (user-visible). Caller's callback delivers
    //    the pixmap to the UI thread.
    {
        RenderEngine::RenderRequest r0;
        r0.page_num    = page;
        r0.priority    = 0;
        r0.scale       = impl_->scale;
        r0.on_complete = std::move(cb);
        r0.invert      = impl_->invert_colors;  // Phase 8 D7
        (void)impl_->engine->submit(std::move(r0));
    }

    // 3. P1 — prev/next prefetch. PageCache::put_pixmap fires inside
    //    the worker before on_complete, so the cache already holds its
    //    own ref by the time this callback runs; we just drop ours.
    auto drop_cb = [](fz_pixmap* p, fz_context* ctx) {
        if (p) fz_drop_pixmap(ctx, p);
    };
    const int total = page_count();
    if (page - 1 >= 0) {
        RenderEngine::RenderRequest r1;
        r1.page_num    = page - 1;
        r1.priority    = 1;
        r1.scale       = impl_->scale;
        r1.on_complete = drop_cb;
        r1.invert      = impl_->invert_colors;  // Phase 8 D7
        (void)impl_->engine->submit(std::move(r1));
    }
    if (page + 1 < total) {
        RenderEngine::RenderRequest r1;
        r1.page_num    = page + 1;
        r1.priority    = 1;
        r1.scale       = impl_->scale;
        r1.on_complete = drop_cb;
        r1.invert      = impl_->invert_colors;  // Phase 8 D7
        (void)impl_->engine->submit(std::move(r1));
    }
}

fz_context* DocumentView::ui_ctx() const noexcept {
    return impl_->ui_ctx;
}

const std::filesystem::path& DocumentView::source_path() const {
    return impl_->doc.source_path();
}

const Document& DocumentView::document() const {
    return impl_->doc;
}

SearchSession& DocumentView::search() {
    return *impl_->session;
}

const SearchSession& DocumentView::search() const {
    return *impl_->session;
}

// Phase 7 Task 8: lazy thumbnail trio construction. Saves ~50 KB per
// tab that never opens the thumbnail pane (no ThumbCache LRU map, no
// ThumbnailRenderer pending_tasks counter, no Win32 ListView HWND).
litepdf::ui::ThumbnailPane* DocumentView::ensure_thumb_pane(
    HINSTANCE hInstance, HWND parent_hwnd)
{
    if (impl_->thumb_pane) return impl_->thumb_pane.get();

    // Construction order is the inverse of destruction order: cache
    // FIRST, then renderer (which references the per-tab RenderEngine
    // for actual page rasterization at priority=3, bypass_cache=true),
    // then pane (which records non-owning pointers to both via setters
    // immediately so the first WM_DRAWITEM cycle hits the renderer).
    //
    // Capacity: 64 thumbs ~= 8x what's typically visible at once, leaves
    // generous headroom for scrolling without re-rendering recently-seen
    // pages. Tune in a follow-up if memory profiling demands it.
    constexpr std::size_t kThumbCacheCap = 64;
    impl_->thumb_cache = std::make_unique<ThumbCache>(kThumbCacheCap);
    impl_->thumb_renderer =
        std::make_unique<ThumbnailRenderer>(*impl_->engine);
    impl_->thumb_pane =
        std::make_unique<litepdf::ui::ThumbnailPane>(hInstance, parent_hwnd);

    // Wire the pane to its peers + seed with current document state. The
    // pane is born hidden (LVS-styled WS_CHILD without WS_VISIBLE per
    // ThumbnailPane.cpp); MainWindow's F4 handler calls show() AFTER
    // this returns to flip visibility on.
    impl_->thumb_pane->set_renderer(impl_->thumb_renderer.get());
    impl_->thumb_pane->set_cache(impl_->thumb_cache.get());
    impl_->thumb_pane->set_page_count(page_count());
    impl_->thumb_pane->set_current_page(impl_->current_page);

    return impl_->thumb_pane.get();
}

litepdf::ui::ThumbnailPane* DocumentView::thumb_pane() const noexcept {
    return impl_ ? impl_->thumb_pane.get() : nullptr;
}

}  // namespace litepdf::core
