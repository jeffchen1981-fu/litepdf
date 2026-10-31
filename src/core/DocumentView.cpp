#include "core/DocumentView.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

#include "app/SearchDispatcher.hpp"
#include "core/PageCache.hpp"
#include "core/RenderEngine.hpp"
#include "core/SearchSession.hpp"
#include "core/ThumbCache.hpp"
#include "core/ThumbnailRenderer.hpp"
#include "core/detail/PageSizeFill.hpp"
#include "core/detail/PrefetchPlan.hpp"
#include "core/detail/ZoomMath.hpp"
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

namespace {
struct DropContext {
    void operator()(fz_context* ctx) const noexcept { fz_drop_context(ctx); }
};
// An fz_context clone owned by DocumentView::Impl.
//
// ui_ctx and cache_ctx used to be raw fz_context*, dropped by ~DocumentView
// (and by the constructor's own null-clone branch). ~DocumentView never runs
// for a half-built object, so a throw
// from the PageCache, RenderEngine or SearchSession constructor leaked both
// clones -- and, since #62/#63, the family's root context with them. ~Impl DOES
// run on that path, and it drops these (#74).
//
// Declaration position inside Impl still carries the teardown order: declared
// after `doc` and before `cache`, they are destroyed AFTER `cache` and BEFORE
// `doc` -- exactly what the contract at the top of Impl asks for.
using ClonedContext = std::unique_ptr<fz_context, DropContext>;
}  // namespace

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
    // clones of the same root, so this is safe. Both own their clone and
    // destruct in reverse-declaration order like every other member, which is
    // what makes the constructor's failure path leak-free (#74).
    ClonedContext                 ui_ctx;
    ClonedContext                 cache_ctx;
    std::unique_ptr<PageCache>    cache;
    std::unique_ptr<RenderEngine> engine;

    int                    current_page = 0;
    // FitWidth: page width fills the canvas, and the overflow below the fold is
    // reached with the wheel (PR-A2) or the arrow keys. PR-A1 shipped FitPage
    // as an interim default precisely because it had no wheel scrolling, which
    // left the lower two thirds of an A4 page unreachable; PR-A2's ScrollMath
    // is what makes FitWidth navigable again.
    DocumentView::ZoomMode zm           = DocumentView::ZoomMode::FitWidth;
    float                  pct          = 1.0f;   // was: scale (a render scale)
    float                  vp_w         = 0.0f;   // device pixels
    float                  vp_h         = 0.0f;   // device pixels
    float                  dpi          = 96.0f;
    bool                   invert_colors = false;  // Phase 8 D7/D9
    bool                   dual_page     = false;  // Phase 8 D10
    // (#49) The L1 byte budget, and what request_render_with_prefetch needs
    // to tell which way the reader is going: the page it last ran for (-1
    // before the first) and the direction of the last real move.
    std::size_t            l1_budget     = kDefaultL1BudgetBytes;
    // (#55) The L1 entry count the cache was built with, for l1_capacity().
    std::size_t            l1_capacity   = 5;
    int                    last_prefetch_page = -1;
    TravelDirection        travel        = TravelDirection::Forward;
    // (#49) Page sizes request_render_with_prefetch and set_viewport have looked
    // up, so a page turn loads only the page it has not sized yet; a page's size
    // does not change for the life of the Document. UI thread only.
    std::vector<std::optional<PageSize>> page_sizes;
    // (#55) DocumentView::page_sizes(): every page's size, fallbacks
    // included, built once. Separate from `page_sizes` above so a fallback
    // never lands there: estimated_bytes must keep seeing an unreadable page
    // as unknown (#49). UI thread only.
    std::vector<PageSize>  strip_sizes;
    bool                   strip_sized   = false;
    bool                   continuous    = false;  // #55, spec §4.3

    // `page`'s size, or nothing when it has none to use: out of range, not
    // usable, or Document::page_size threw, which it does on a page it cannot
    // size: a PDF page object it cannot find, or another format's page that
    // does not load. A throw is not cached, so a later call tries again.
    std::optional<PageSize> known_size(int page) noexcept {
        try {
            if (page < 0) return std::nullopt;
            const auto i = static_cast<std::size_t>(page);
            if (page_sizes.size() <= i) page_sizes.resize(doc.page_count());
            if (i >= page_sizes.size()) return std::nullopt;
            if (!page_sizes[i]) page_sizes[i] = doc.page_size(i);
            if (!detail::page_size_usable(*page_sizes[i])) return std::nullopt;
            return page_sizes[i];
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }

    // Estimated L1 bytes of `page` at `scale`; 0 when its size is unknown.
    std::size_t estimated_bytes(int page, float scale) noexcept {
        const auto size = known_size(page);
        return size ? estimated_pixmap_bytes(size->width_pt, size->height_pt, scale)
                    : 0;
    }
    std::optional<TextSelection> selection;       // #52; pure data, order-free

    // (#98) At most the current page's text. Its position among the members
    // does not matter: the handle's escrow keeps the MuPDF root alive past
    // `doc`.
    Document::TextPage text_slot;
    // (#98) The page whose text is still wanted, -1 for none. Allocated once
    // in the constructor and never reassigned: queued P0 requests hold this
    // same object. Every write is a relaxed store on the UI thread; the
    // worker reads it with a relaxed load (spec §3.5).
    std::shared_ptr<std::atomic<int>> text_wanted;

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
    // time it returns, no PostMessageW(WM_USER_THUMB_READY or _CANCELED) is in flight
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
                           std::optional<std::string_view> password,
                           std::size_t num_workers,
                           std::size_t l1_capacity,
                           std::size_t l2_capacity,
                           std::size_t l1_budget_bytes)
    : impl_(std::make_unique<Impl>()) {
    impl_->doc = std::move(doc);
    impl_->l1_budget = l1_budget_bytes;
    impl_->l1_capacity = l1_capacity;
    impl_->text_wanted = std::make_shared<std::atomic<int>>(-1);   // #98

    // Clone the UI-thread ctx and a dedicated cache_ctx BEFORE
    // constructing cache/engine. ui_ctx stays UI-thread-exclusive (D3);
    // cache_ctx is conceptually "the cache's own ctx" and is used by
    // workers via PageCache for refcount ops. Both are adopted by a
    // ClonedContext as soon as they exist, so every throw below this point --
    // including one out of this function's own `throw` -- drops them through
    // ~Impl (#74).
    impl_->ui_ctx.reset(impl_->doc.clone_context());
    impl_->cache_ctx.reset(impl_->doc.clone_context());
    if (!impl_->ui_ctx || !impl_->cache_ctx) {
        throw std::runtime_error(
            "DocumentView: Document is not open (clone_context returned null)");
    }

    // Cache uses cache_ctx (not ui_ctx) for its internal keep/drop —
    // those ops happen on worker threads. The cache does not own it.
    impl_->cache = std::make_unique<PageCache>(l1_capacity, l2_capacity,
                                               impl_->cache_ctx.get(), l1_budget_bytes);

    // Engine clones its own per-worker contexts off the Document.
    impl_->engine = std::make_unique<RenderEngine>(
        impl_->doc, num_workers, impl_->cache.get(), password);

    // Construct SearchSession LAST — it references impl_->doc (already
    // moved into place) and the caller-owned dispatcher, and opens its own
    // document on a cloned context, authenticated with the same password
    // as the render workers (#130). If it throws (clone, open or password
    // failure), earlier members RAII-unwind via Impl's declaration order.
    impl_->session = std::make_unique<SearchSession>(impl_->doc, dispatcher, password);
}

DocumentView::~DocumentView() {
    if (!impl_) return;

    // (#98) No queued text build starts from here on (skip rule 2); the
    // engine's own `stopping` check backs this up once engine.reset() runs.
    impl_->text_wanted->store(-1, std::memory_order_relaxed);

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
    // ~SearchSession drains every in-flight task first; the tasks use only
    // the session's own context and document, not impl_->doc. impl_->doc
    // must still be destroyed AFTER the session (below, via Impl
    // unwinding): the session holds a `const Document&` for page_count()
    // and query_compiles() on the UI thread.
    impl_->session.reset();
    // Engine next — ensures no worker is mid-access to cache or contexts.
    impl_->engine.reset();
    // Cache next — drops remaining pixmaps / display lists via cache_ctx.
    impl_->cache.reset();
    // cache_ctx before ui_ctx; both before doc (doc destructs via Impl).
    // Redundant with Impl's declaration order, which drops them in exactly
    // this sequence -- kept explicit so the contracted order reads here too.
    impl_->cache_ctx.reset();
    impl_->ui_ctx.reset();
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
    // (#98) The slot holds only the current page's text.
    impl_->text_slot = Document::TextPage{};
    impl_->text_wanted->store(-1, std::memory_order_relaxed);
    // Recompute the percentage if not Custom (page size may differ per page).
    // (#55) Not in the strip: its one zoom is refit only at fit events
    // (spec §4.3), or a page of another size would re-layout the strip.
    if (!impl_->continuous && impl_->zm != ZoomMode::Custom) {
        set_viewport(impl_->vp_w, impl_->vp_h, impl_->dpi);
    }
    return true;
}

void DocumentView::set_continuous(bool on) noexcept {
    impl_->continuous = on;
}

bool DocumentView::continuous() const noexcept {
    return impl_->continuous;
}

const std::vector<PageSize>& DocumentView::page_sizes() {
    if (impl_->strip_sized) return impl_->strip_sizes;
    // Every page is about to be sized: a CBZ reads its pages' headers ahead
    // at once instead of one page at a time (#176).
    (void)impl_->doc.read_ahead_page_sizes();
    const std::size_t n = impl_->doc.page_count();
    auto& cache = impl_->page_sizes;
    if (cache.size() < n) cache.resize(n);
    // Reads through the #49 size cache and fills it with every size that
    // reads; a page that throws is left unknown there, and fill_page_sizes
    // gives it its stand-in in strip_sizes only.
    impl_->strip_sizes = detail::fill_page_sizes(n, [&](std::size_t i) {
        if (!cache[i]) cache[i] = impl_->doc.page_size(i);
        return *cache[i];
    });
    impl_->strip_sized = true;
    return impl_->strip_sizes;
}

DocumentView::ZoomMode DocumentView::zoom_mode() const noexcept {
    return impl_->zm;
}

float DocumentView::zoom_pct() const noexcept {
    return impl_->pct;
}

float DocumentView::render_scale() const noexcept {
    return impl_->pct * (impl_->dpi / 96.0f);
}

void DocumentView::set_viewport(float viewport_w_px, float viewport_h_px,
                                float dpi, int pair_page) {
    impl_->vp_w = viewport_w_px;
    impl_->vp_h = viewport_h_px;
    impl_->dpi  = dpi;

    if (page_count() <= 0) { impl_->pct = 1.0f; return; }
    if (impl_->zm == ZoomMode::Custom) return;   // frozen; viewport only stored

    // Two-page spread: one render scale is shared by both slots, so fit the
    // pair's bounding size. Deriving from the current page alone overflows
    // the slot holding the larger page once the paint path stops
    // shrink-to-fitting.
    //
    // This runs inside the window procedure, where nothing would catch
    // Document::page_size, so only a page with a size enters the fit. With
    // neither, it is the stand-in page_sizes() lays such a page out with, so that
    // a fit does not depend on the zoom the view arrived with.
    const auto current = impl_->known_size(impl_->current_page);
    const auto pair    = impl_->known_size(pair_page);
    const PageSize one = current ? *current
                       : pair    ? *pair
                                 : impl_->known_size(0).value_or(detail::kFallbackPageSize);
    const PageSize two = pair.value_or(one);

    impl_->pct = fit_percentage(viewport_w_px, viewport_h_px, dpi,
                                std::max(one.width_pt, two.width_pt),
                                std::max(one.height_pt, two.height_pt),
                                impl_->zm == ZoomMode::FitPage);
}

void DocumentView::set_zoom_mode_fit_width() {
    impl_->zm = ZoomMode::FitWidth;
    set_viewport(impl_->vp_w, impl_->vp_h, impl_->dpi);
}

void DocumentView::set_zoom_mode_fit_page() {
    impl_->zm = ZoomMode::FitPage;
    set_viewport(impl_->vp_w, impl_->vp_h, impl_->dpi);
}

void DocumentView::set_zoom_pct(float pct) noexcept {
    impl_->zm  = ZoomMode::Custom;
    impl_->pct = clamp_preset_span(pct);
}

bool DocumentView::zoom_in() {
    const float next = next_preset(impl_->pct);
    if (next == impl_->pct) return false;
    impl_->pct = next;
    impl_->zm  = ZoomMode::Custom;
    return true;
}

bool DocumentView::zoom_out() {
    const float prev = prev_preset(impl_->pct);
    if (prev == impl_->pct) return false;
    impl_->pct = prev;
    impl_->zm  = ZoomMode::Custom;
    return true;
}

void DocumentView::request_render(int page, RenderCb on_complete) {
    impl_->cache->release_l1_pin();  // (#159) the tab is shown again
    RenderEngine::RenderRequest req;
    req.page_num    = page;
    req.priority    = 0;
    req.scale       = render_scale();
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
    // (#98) Spread mode never prefetches text: drop any build still wanted.
    if (on) impl_->text_wanted->store(-1, std::memory_order_relaxed);
    if (impl_->dual_page == on) return;
    impl_->dual_page = on;
    // (Phase 8 D9 addendum / D10) Drain in-flight single-page renders
    // so their bitmaps don't land in the wrong slot of the new layout.
    // Caller kicks a fresh dual-page render afterwards.
    impl_->engine->cancel_all_below_priority(0);
}

const std::optional<TextSelection>& DocumentView::selection() const noexcept {
    return impl_->selection;
}

void DocumentView::set_selection(TextSelection selection) {
    impl_->selection = std::move(selection);
}

void DocumentView::clear_selection() noexcept {
    impl_->selection.reset();
}

void DocumentView::cancel_stale_renders(int keep_priority_threshold) {
    impl_->engine->cancel_all_below_priority(keep_priority_threshold);
}

void DocumentView::cancel_background_renders() {
    impl_->engine->cancel_background();
}

void DocumentView::request_render_with_prefetch(int page, RenderCb cb,
                                                TextCb on_text) {
    // 1. Drop any queued P1 prefetch (and P3 thumbnails) from a prior nav
    //    so the queue stays short during rapid PgDn scrubbing.
    cancel_stale_renders(0);
    impl_->cache->release_l1_pin();  // (#159) the tab is shown again

    // 2. P0 — current page (user-visible). Caller's callback delivers
    //    the pixmap to the UI thread. (#98) It alone may also carry the
    //    text request; the P1 neighbours below never do.
    {
        RenderEngine::RenderRequest r0;
        r0.page_num    = page;
        r0.priority    = 0;
        r0.scale       = render_scale();
        r0.on_complete = std::move(cb);
        r0.invert      = impl_->invert_colors;  // Phase 8 D7
        if (on_text && wants_text_prefetch(page)) {
            impl_->text_wanted->store(page, std::memory_order_relaxed);
            r0.on_text     = std::move(on_text);
            r0.text_wanted = impl_->text_wanted;
        }
        (void)impl_->engine->submit(std::move(r0));
    }

    // 3. P1 — prev/next prefetch. PageCache::put_pixmap fires inside
    //    the worker before on_complete, so the cache already holds its
    //    own ref by the time this callback runs; we just drop ours.
    //    (#49) Both neighbours only while the three pages fit in L1's byte
    //    budget together; past that, only one (core/detail/PrefetchPlan.hpp).
    //    This path never threw before #49, and still does not: a page whose
    //    size cannot be read counts as unknown, which falls back to both
    //    neighbours, and the render reports the failure as it always has.
    impl_->travel = travel_direction(page, impl_->last_prefetch_page, impl_->travel);
    impl_->last_prefetch_page = page;
    const int   total    = page_count();
    const bool  has_prev = page - 1 >= 0;
    const bool  has_next = page + 1 < total;
    const float scale    = render_scale();
    const PrefetchPlan plan = prefetch_plan(
        impl_->estimated_bytes(page, scale),
        has_prev ? impl_->estimated_bytes(page - 1, scale) : 0,
        has_next ? impl_->estimated_bytes(page + 1, scale) : 0,
        impl_->l1_budget, impl_->travel, has_prev, has_next);

    auto drop_cb = [](fz_pixmap* p, fz_context* ctx) {
        if (p) fz_drop_pixmap(ctx, p);
    };
    if (plan.prev) {
        RenderEngine::RenderRequest r1;
        r1.page_num    = page - 1;
        r1.priority    = 1;
        r1.scale       = render_scale();
        r1.on_complete = drop_cb;
        r1.invert      = impl_->invert_colors;  // Phase 8 D7
        (void)impl_->engine->submit(std::move(r1));
    }
    if (plan.next) {
        RenderEngine::RenderRequest r1;
        r1.page_num    = page + 1;
        r1.priority    = 1;
        r1.scale       = render_scale();
        r1.on_complete = drop_cb;
        r1.invert      = impl_->invert_colors;  // Phase 8 D7
        (void)impl_->engine->submit(std::move(r1));
    }
}

RenderEngine::RenderToken DocumentView::request_page_render(
        int page, int priority, RenderCb on_complete,
        std::shared_ptr<std::atomic<bool>> cancel_flag, TextCb on_text) {
    impl_->cache->release_l1_pin();  // (#159) the tab is shown again
    RenderEngine::RenderRequest req;
    req.page_num    = page;
    req.priority    = priority;
    req.scale       = render_scale();
    req.on_complete = std::move(on_complete);
    req.invert      = impl_->invert_colors;  // Phase 8 D7
    req.cancel_flag = std::move(cancel_flag);  // #117
    if (on_text && wants_text_prefetch(page)) {  // #98
        impl_->text_wanted->store(page, std::memory_order_relaxed);
        req.on_text     = std::move(on_text);
        req.text_wanted = impl_->text_wanted;
    }
    return impl_->engine->submit(std::move(req));
}

void DocumentView::cancel_thumbnail_renders() {
    // Flags queued requests above P2: the thumbnails' P3 only.
    impl_->engine->cancel_all_below_priority(2);
}

std::size_t DocumentView::queued_render_count() const noexcept {
    return impl_->engine->pending_count();
}

std::size_t DocumentView::renders_started() const noexcept {
    return impl_->engine->renders_started();
}

void DocumentView::trim_l1_for_background(int page, int other_page) {
    impl_->cache->retain_l1_pages(render_scale(), impl_->invert_colors, page,
                                  other_page);
}

void DocumentView::trim_l1_for_background(const std::vector<int>& pages) {
    impl_->cache->retain_l1_pages(render_scale(), impl_->invert_colors, pages);
}

std::size_t DocumentView::l1_entry_count() const {
    return impl_->cache->l1_size();
}

bool DocumentView::l1_holds(int page) const {
    fz_pixmap* p = impl_->cache->get_pixmap(page, render_scale(),
                                            impl_->invert_colors);
    if (!p) return false;
    fz_drop_pixmap(impl_->ui_ctx.get(), p);
    return true;
}

bool DocumentView::l1_contains(int page) const {
    return impl_->cache->l1_contains(page, render_scale(), impl_->invert_colors);
}

std::size_t DocumentView::l1_capacity() const noexcept {
    return impl_->l1_capacity;
}

std::size_t DocumentView::l1_budget_bytes() const noexcept {
    return impl_->l1_budget;
}

bool DocumentView::has_text_for(int page) const noexcept {
    return impl_->text_slot.page() == page;
}

bool DocumentView::wants_text_prefetch(int page) const noexcept {
    // No password condition: since #126 every render worker authenticates its
    // document with the password the tab was opened with (spec §3.6 addendum).
    return !impl_->dual_page && !has_text_for(page);
}

void DocumentView::offer_text(Document::TextPage t) {
    const int current = impl_->current_page;
    // Never replaces a handle already in the slot, so a sync-built one is not
    // churned by a late worker one. A dropped `t` goes through its own escrow.
    if (!t.valid() || t.page() != current || has_text_for(current)) return;
    impl_->text_slot = std::move(t);
    impl_->text_wanted->store(-1, std::memory_order_relaxed);
}

Document::TextPage DocumentView::text_for(int page) {
    // Shares, never moves out: the slot keeps its handle for the next press.
    if (has_text_for(page)) return impl_->text_slot.share();
    Document::TextPage built = impl_->doc.text_page(static_cast<std::size_t>(page));
    // The slot holds only the current page's, and it is empty here: a full
    // slot always holds the current page, which has_text_for just ruled out.
    if (built.valid() && page == impl_->current_page) {
        impl_->text_slot = std::move(built);
        impl_->text_wanted->store(-1, std::memory_order_relaxed);
        return impl_->text_slot.share();
    }
    return built;
}

int DocumentView::pending_text_page() const noexcept {
    return impl_->text_wanted->load(std::memory_order_relaxed);
}

fz_context* DocumentView::ui_ctx() const noexcept {
    return impl_->ui_ctx.get();
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
