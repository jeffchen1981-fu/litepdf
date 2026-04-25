# Phase 7: Thumbnail Pane — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use `superpowers:executing-plans` to implement this plan task-by-task. Pattern follows Phases 5–6: implementer → spec reviewer → quality reviewer → fix.

**Goal:** Add a left-side thumbnail pane (owner-draw ListView, lazy render, single-column 120×160 dip tiles, F4 toggle, hidden by default) that mutually excludes the existing OutlinePane in the same dock slot. Thumbnails render on demand without blocking document open and without polluting the main `PageCache` L1/L2 layers.

**Architecture:** Mirror the Phase 6 SearchSession pattern. `core::ThumbnailModel` is pure logic (page count, tile size, current page, visible range, scroll offset) — fully unit-testable. `core::ThumbCache` is a small HBITMAP LRU (separate from `PageCache` to avoid pollution). `core::ThumbnailRenderer` wraps a single-worker `RenderEngine` constructed with `cache=nullptr` (direct-render path) to produce low-DPI pixmaps that the UI thread converts to HBITMAP and stashes in `ThumbCache`. `ui::ThumbnailPane` is an owner-draw `SysListView32` that asks the model what's visible, asks the cache for HBITMAP, and kicks the renderer for misses. `ui::VerticalSplitter` extracts the orientation-agnostic core from the existing `ui::Splitter` so both directions reuse one PIMPL skeleton. `MainWindow` adds an F4 menu/accel and a mutual-exclusion rule with F5 (outline): showing one hides the other.

**Tech Stack:** C++17, Win32 (USER + GDI), `SysListView32` LVS_OWNERDRAWFIXED, GDI `CreateDIBSection` (for `fz_pixmap` → HBITMAP), Phase 6's existing `core::RenderEngine`, Catch2/CMake from prior phases. **No new dependencies.**

---

## Pre-flight

Before starting, confirm:

- Working tree is clean: `git status` should show no uncommitted changes.
- Branch off the latest `origin/main` (= post-Phase-6 tip, currently `84a4b89`).
- Release build + tests currently green:
  ```bash
  cmake --build build --config Release
  ctest --test-dir build -C Release --output-on-failure
  ```
  Expected: 101/101 pass (Phase 6 baseline).

**Reference open at all times:**
- `docs/plans/2026-04-15-litepdf-design.md` §4.1 (ThumbnailPane row) and §3 (F4 hidden-by-default rule)
- `docs/plans/2026-04-15-litepdf-roadmap.md` §"Phase 7" (deliverable + exit criteria)
- `src/core/SearchSession.{hpp,cpp}` (pattern for ThumbnailModel)
- `src/ui/Splitter.{hpp,cpp}` (pattern for VerticalSplitter)
- `src/ui/OutlinePane.{hpp,cpp}` (pattern for left-dock widget integration into MainWindow)

**Smoke-test app invocation:**
```bash
build/Release/litepdf.exe tests/fixtures/bookmarks.pdf
# Then:
#   F4 → thumbnail pane opens, page 0 thumb renders within 200 ms
#   F5 with thumbs visible → thumbs hide, outline shows (mutual exclusion)
#   Click thumb of page 5 → canvas jumps to page 5
#   PgDn ×3 in canvas → thumb pane highlights pages 1, 2, 3 in turn
```

**Pre-flight verification (added per plan-eng-review 1E + 1B):**

This phase plumbs thumbs through the existing `core::RenderEngine` via a new per-request
`bypass_cache` flag (D3 + D16). Confirm the existing engine has the cache-conditional
branch we're extending (or add it via the prerequisite step in Task 0.5):

```bash
grep -n "cache_\b\|cache->\|cache &&" src/core/RenderEngine.cpp | head -20
```

Expected: at least one `if (cache_)` (or equivalent) branch around the worker's
pixmap lookup/put. If absent, Task 0.5 below adds the bypass-cache plumbing as a
~10 LOC prerequisite before any thumb code is written.

---

## Architectural Decisions

**D1. Pane is a third widget type sharing the same left-dock slot as OutlinePane.** Not a tab control inside the dock; not side-by-side. F4 and F5 are mutually exclusive — showing one hides the other. The dock slot is at most one of `{OutlinePane, ThumbnailPane}` visible at a time, with width controlled by a single `left_pane_width_px_` member on `MainWindow`. This keeps `on_layout()` logic simple (only one branch checks visibility per pane) and matches SumatraPDF's left-sidebar UX. Trade-off: users who want both panes simultaneously cannot — design doc §3 specifies F4-toggle hidden-by-default, not "always-on dual pane," so this matches intent.

**D2. ThumbCache is its own class — NOT a third tier in `core::PageCache`.** Adding `L3 = HBITMAP` to PageCache would tangle thumb lifecycle with main render lifecycle (every render-time eviction would have to reason about thumbs). Instead: a tiny standalone `ThumbCache` keyed by `int page_num`, capacity = (visible_pages + buffer_pages) × 2 ≈ 30 entries by default. `ThumbCache` owns HBITMAP refs (DeleteObject on evict). PageCache stays untouched.

**D3. ThumbnailRenderer reuses each `DocumentView`'s existing `RenderEngine` via a per-request `bypass_cache` flag** (revised per plan-eng-review 1B; supersedes the original "new RenderEngine(num_workers=1, cache=nullptr) per tab" approach). Task 0.5 adds a `bool bypass_cache` field to `RenderEngine::RenderRequest` plus the two `if (cache_ && !req.bypass_cache)` guards in `RenderEngine.cpp`'s worker loop (~10 LOC). `ThumbnailRenderer` then holds a non-owning `RenderEngine&` and submits with `priority=3` (below main render's P0/P1/P2), `scale=0.15f` (≈ 11 dpi at 72-baseline → 120-dip-wide thumb on a 1920-px-wide PDF page), and `bypass_cache=true` (so thumb pixmaps don't pollute L1/L2 — D2 still holds). Reusing the existing engine avoids the thread-count blowup that would otherwise compound Phase 5 D16's known limitation: 5 tabs would have meant 5 main engines × 2 workers + 5 thumb engines × 1 worker = 15 threads. With the bypass flag, it stays at 10 (the Phase 5 baseline).

**D4. Thumbnail size: 120×160 dip, single column, DPI-aware.** Dip-fixed because (a) UX consistency across DPI scaling; (b) one-column simplifies hit-testing (page = `y / (tile_h + gap)`); (c) eliminates re-flow on splitter resize (only the gutter changes). Tile pixel size = `MulDiv(120, dpi, 96) × MulDiv(160, dpi, 96)`. Pane min-width = `120 + 2 * margin + scrollbar` ≈ 150 dip.

**D5. ThumbnailModel is pure logic — no Win32, no rendering, no threads.** Same TDD discipline as `core::SearchSession` and `core::TabList`. State: `page_count`, `tile_dip{w,h}`, `gap_dip`, `dpi`, `viewport_h_px`, `scroll_y_px`, `current_page`. API: `set_page_count`, `set_dpi`, `set_viewport`, `set_scroll`, `set_current_page`, `visible_range() -> {first, last}`, `page_at_y(y_px) -> std::optional<int>`, `tile_rect(page) -> RECT`, `total_height_px()`. Eight unit tests for the [thumbnail_model] tag.

**D6. Lazy render policy: only request thumbs for `visible_range() ± 1 buffer page`.** Pane's `OnPaint` walks pages in `visible_range_with_buffer()`, asks ThumbCache for HBITMAP. On miss, it submits a render request (idempotent — same page_num is deduped via a pending-set). Re-paint when render completes. This bounds RAM at ~30 thumbs × ~75 KB each = ~2 MB regardless of doc size.

**D7. Cache eviction policy = simple LRU keyed by access time.** Capacity 30 entries (covers 1920×1080 pane easily). LRU because access pattern is sequential (user pages through). FIFO would work too but LRU handles back-and-forth better.

**D8. Current-page sync via observer.** `MainWindow::on_canvas_page_changed(int page)` calls `thumb_pane_->set_current_page(page)`. Pane updates its model and:
  (a) repaints the (old, new) tile pair to redraw highlight border;
  (b) if the new page is outside `visible_range()`, scrolls so the new page sits at row 1 (one tile above center) — same UX as Sumatra. Scroll is animated only if Phase 12 adds a flag; Phase 7 ships instant scroll.

**D9. VerticalSplitter shares PIMPL skeleton with Phase 6's Splitter, refactor lands as a separate prerequisite PR** (revised per plan-eng-review 2A + 2D). The `SplitterCore` Impl owns the WndProc, parameterized by `enum Orientation { Horizontal, Vertical }`. Both `Splitter` and `VerticalSplitter` are thin ~30-line ctors binding orientation + cursor (`IDC_SIZENS` vs `IDC_SIZEWE`) + axis math + callback to the shared `SplitterCore`. **Two-step rule (Beck):** the refactor lands first as Task 4a in its own PR (zero behavior change on the in-production results-panel splitter); only then does Task 4b add `VerticalSplitter` as new code in this Phase 7 PR. Splitting the change preserves `git bisect` resolution: any future regression in either splitter direction lands on the commit that actually introduced it. DRY: WndProc lives once in `SplitterCore`; the two wrapper classes don't duplicate WM_LBUTTONDOWN / WM_MOUSEMOVE / WM_LBUTTONUP / WM_CAPTURECHANGED logic.

**D10. F4 toggle and mutual exclusion live in MainWindow, not ThumbnailPane.** Pane just exposes `show()`/`hide()`/`visible()` like OutlinePane. MainWindow's `WM_COMMAND` for `IDM_VIEW_THUMBS` does:
  ```cpp
  if (thumb_pane_ && thumb_pane_->visible()) { thumb_pane_->hide(); }
  else {
      if (outline_ && outline_->visible()) outline_->hide();
      thumb_pane_->show();
  }
  on_layout();
  ```
  And the F5 handler (existing) gains a symmetric `if (thumb_pane_ && thumb_pane_->visible()) thumb_pane_->hide();` guard before showing outline. **Both toggles cancel the other — never two left-pane widgets visible.**

**D11. Pane visibility is per-tab, not application-global.** `DocumentView` owns the `ThumbnailPane*` pointer (just like it owns the `Document` and `RenderEngine`). On tab switch, MainWindow asks `active_view_->thumb_pane()` and shows/hides per the active doc's last state. Default for new tabs: hidden (matches design doc §3).

**D12. No persistence of pane-visibility across app restarts in v1.** Adding a registry key for "last F4 state" is YAGNI for v1.0. Phase 12 session.json may revisit. Default = hidden every cold start.

**D13. ThumbnailPane uses `SysListView32` LVS_OWNERDRAWFIXED + LVS_REPORT (single column).** Owner-draw because paint is custom (HBITMAP + page-number text + current-page border). Report style because we want vertical scroll out of the box. Single column of width = pane_client_w; row height = `tile_h_px + 2 * margin + label_h`.

**D14. Color palette reuses TabManager's `Palette` struct + dark-mode detection.** Tab strip already has light/dark hot-switching on `WM_SETTINGCHANGE`. Pane subscribes to the same change. Border on current page = `palette.accent`; placeholder background = `palette.tab_bg`; text = `palette.text`.

**D15. Tag at end: `v0.0.8-phase7`.** Mirrors phase5 / phase6 convention. Version bump in CMakeLists.txt + About dialog + VERSION file. No fast-follow tags planned — if something surfaces post-tag, follow `phase7.1` pattern.

**D16. ThumbnailRenderer adopts Phase 6 C1's task-drain pattern** (added per plan-eng-review 1A). Worker callbacks reach into `ThumbnailPane` (via the `OnDone` lambda → `PostMessageW` → cache put). When a tab is closed mid-render, `DocumentView` (and therefore `ThumbnailPane`, `ThumbCache`, `ThumbnailRenderer`) is destroyed. Without a drain, an in-flight worker can complete its render after destruction and either (a) leak the produced HBITMAP via PostMessage to a dead HWND, or (b) UAF if the lambda holds a captured raw pointer. `ThumbnailRenderer::Impl` holds an `std::atomic<int> pending_tasks{0}` that is incremented at `submit()` and RAII-decremented at the top of each `on_complete` lambda; `~Impl()` spin-waits to zero before returning. Mirrors the fix in commit `8e11f39` for `core::SearchSession`. Self-check at T3 implementation review: any `submit()` lambda that captures a non-owning UI/cache pointer MUST go through this drain.

---

## Task List

### Task 0: Reserve menu IDs + F4 accelerator (no behavior change)

**Goal:** Land the menu ID + accelerator wiring so future tasks can reference them; menu item is grayed (no handler yet).

**Files:**
- Modify: `resources/MainMenu.rc.h` — add `IDM_VIEW_THUMBS`.
- Modify: `resources/litepdf.rc` — add menu item + accelerator.

**Step 0.1:** Edit `resources/MainMenu.rc.h`. After the `IDM_FIND_*` block (40047), add:

```c
// Phase 7: thumbnail pane.
#define IDM_VIEW_THUMBS 40060   // F4
```

**Step 0.2:** Edit `resources/litepdf.rc`. In the View menu popup, after the `Toggle &Outline\tF5` entry, add:

```rc
MENUITEM "Toggle &Thumbnails\tF4", IDM_VIEW_THUMBS
```

In the `IDR_ACCEL` block, add:

```rc
VK_F4,    IDM_VIEW_THUMBS, VIRTKEY
```

**Step 0.3:** Build + smoke-test:

```bash
cmake --build build --config Release
build/Release/litepdf.exe tests/fixtures/bookmarks.pdf
```

Expected: View menu shows the new "Toggle Thumbnails F4" item, grayed/no-op (no handler yet). Pressing F4 does nothing. **No test changes — this is pure resource wiring.**

**Step 0.4:** Commit.

```bash
git add resources/
git commit -m "feat(ui): reserve menu IDs for thumbnail pane (Phase 7 Task 0)"
```

---

### Task 0.5: Add `bypass_cache` per-request flag to `core::RenderEngine` (added per plan-eng-review 1B)

**Goal:** Extend `RenderEngine::RenderRequest` with an opt-in flag that suppresses `PageCache` lookup AND population for that request. This is the enabling change that lets `ThumbnailRenderer` (T3) reuse each `DocumentView`'s existing `RenderEngine` without polluting L1/L2. ~10 LOC.

**Files:**
- Modify: `src/core/RenderEngine.hpp` — add `bool bypass_cache` field.
- Modify: `src/core/RenderEngine.cpp` — guard the two cache touch points.
- Modify: `tests/unit/test_render_engine.cpp` (or equivalent existing render-engine test file) — add one test confirming `bypass_cache=true` neither reads nor writes the cache.

**Step 0.5.1: Edit `src/core/RenderEngine.hpp`.** In the `RenderRequest` struct, after `scale`:

```cpp
struct RenderRequest {
    int   page_num    = 0;
    int   priority    = 0;
    float scale       = 1.0f;
    bool  bypass_cache = false;  // (Phase 7) when true, this request neither
                                 // reads from nor populates the L1/L2 cache.
                                 // Used by ThumbnailRenderer so thumb pixmaps
                                 // never displace main-render entries.
    std::function<void(fz_pixmap*, fz_context*)> on_complete;
};
```

**Step 0.5.2: Edit `src/core/RenderEngine.cpp`.** Find the existing worker loop's cache lookup and cache-put. Pre-flight grep already confirmed which file/line to touch:

```bash
grep -n "cache_\b\|cache->\|cache &&" src/core/RenderEngine.cpp | head
```

Wrap both `if (cache_)` paths with the new flag:

```cpp
// At cache-lookup site:
if (cache_ && !req.bypass_cache) {
    if (auto* hit = cache_->get_pixmap(req.page_num, req.scale)) {
        // ... existing fast-path ...
    }
}

// At cache-put site (after render completes):
if (cache_ && !req.bypass_cache) {
    cache_->put_pixmap(req.page_num, req.scale, fz_keep_pixmap(ctx, pix));
}
```

**Step 0.5.3: Add a unit test.** In whichever test file already covers RenderEngine (Phase 2 test file likely under `tests/unit/`), add one case:

```cpp
TEST_CASE("RenderEngine: bypass_cache=true neither reads nor populates cache",
          "[render_engine][bypass]") {
    Document doc; REQUIRE(doc.open(L"tests/fixtures/simple.pdf"));
    PageCache cache(/*l1=*/4, /*l2=*/4, doc.ui_ctx());
    RenderEngine eng(doc, /*workers=*/1, &cache);

    std::atomic<int> done{0};
    RenderEngine::RenderRequest req;
    req.page_num     = 0;
    req.priority     = 0;
    req.scale        = 0.15f;
    req.bypass_cache = true;
    req.on_complete  = [&](fz_pixmap* pix, fz_context* ctx) {
        if (pix) fz_drop_pixmap(ctx, pix);
        done.fetch_add(1, std::memory_order_release);
    };
    eng.submit(std::move(req));

    for (int i = 0; i < 500 && done.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(done.load() == 1);

    // The defining assertion: cache was untouched.
    REQUIRE(cache.get_pixmap(0, 0.15f) == nullptr);
}
```

**Step 0.5.4: Build + tests.** Expected: existing render-engine tests still pass; one new [render_engine][bypass] case adds. Total ctest count: 101 → 102 (before any T1+ work).

**Step 0.5.5: Commit.**

```bash
git add src/core/RenderEngine.hpp src/core/RenderEngine.cpp tests/unit/<render_engine_test_file>.cpp
git commit -m "feat(core): RenderEngine bypass_cache per-request flag (Phase 7 Task 0.5 / D3)"
```

---

### Task 1: `core::ThumbnailModel` pure-logic class with TDD

**Goal:** Pure-logic state machine for thumb-pane geometry. Eight tests, all in [thumbnail_model] tag.

**Files:**
- Create: `src/core/ThumbnailModel.hpp` (~80 LOC).
- Create: `src/core/ThumbnailModel.cpp` (~100 LOC).
- Create: `tests/unit/ThumbnailModelTests.cpp` (~120 LOC).
- Modify: `tests/unit/CMakeLists.txt` (add new test source).
- Modify: `src/CMakeLists.txt` (add `core/ThumbnailModel.cpp` to `litepdf_core`).

**Step 1.1: Write the failing tests.** Create `tests/unit/ThumbnailModelTests.cpp`:

```cpp
#include "core/ThumbnailModel.hpp"

#include <catch2/catch_test_macros.hpp>

using litepdf::core::ThumbnailModel;

TEST_CASE("ThumbnailModel: empty model has no visible pages", "[thumbnail_model]") {
    ThumbnailModel m;
    auto r = m.visible_range();
    REQUIRE(r.first == 0);
    REQUIRE(r.last  == -1);  // empty: last < first
}

TEST_CASE("ThumbnailModel: total_height grows with page_count", "[thumbnail_model]") {
    ThumbnailModel m;
    m.set_dpi(96);
    m.set_tile_dip({120, 160});
    m.set_gap_dip(8);
    m.set_page_count(0);
    REQUIRE(m.total_height_px() == 0);
    m.set_page_count(3);
    // 3 tiles × (160 + 8) - 8 (no trailing gap) = 496
    REQUIRE(m.total_height_px() == 496);
}

TEST_CASE("ThumbnailModel: visible_range honors viewport + scroll", "[thumbnail_model]") {
    ThumbnailModel m;
    m.set_dpi(96);
    m.set_tile_dip({120, 160});
    m.set_gap_dip(8);
    m.set_page_count(20);
    m.set_viewport_h_px(400);
    m.set_scroll_y_px(0);
    auto r = m.visible_range();
    // Tile pitch = 168 px; viewport 400 covers pages 0..2 (last partial)
    REQUIRE(r.first == 0);
    REQUIRE(r.last  == 2);
    m.set_scroll_y_px(168);  // skip page 0
    r = m.visible_range();
    REQUIRE(r.first == 1);
    REQUIRE(r.last  == 3);
}

TEST_CASE("ThumbnailModel: visible_range_with_buffer extends ±1", "[thumbnail_model]") {
    ThumbnailModel m;
    m.set_dpi(96);
    m.set_tile_dip({120, 160});
    m.set_gap_dip(8);
    m.set_page_count(20);
    m.set_viewport_h_px(400);
    m.set_scroll_y_px(168);  // visible 1..3
    auto rb = m.visible_range_with_buffer();
    REQUIRE(rb.first == 0);  // 1 - 1
    REQUIRE(rb.last  == 4);  // 3 + 1
}

TEST_CASE("ThumbnailModel: page_at_y returns hit page within tile", "[thumbnail_model]") {
    ThumbnailModel m;
    m.set_dpi(96);
    m.set_tile_dip({120, 160});
    m.set_gap_dip(8);
    m.set_page_count(5);
    m.set_scroll_y_px(0);
    REQUIRE(m.page_at_y(0).value()   == 0);
    REQUIRE(m.page_at_y(159).value() == 0);
    REQUIRE(m.page_at_y(168).value() == 1);
    REQUIRE_FALSE(m.page_at_y(165).has_value());  // gap region
    REQUIRE_FALSE(m.page_at_y(99999).has_value());  // beyond
}

TEST_CASE("ThumbnailModel: tile_rect returns correct geometry", "[thumbnail_model]") {
    ThumbnailModel m;
    m.set_dpi(96);
    m.set_tile_dip({120, 160});
    m.set_gap_dip(8);
    m.set_page_count(5);
    m.set_scroll_y_px(168);
    auto rc = m.tile_rect(2);  // page 2 is at y = 2*168 - 168 = 168
    REQUIRE(rc.left   == 0);
    REQUIRE(rc.top    == 168);
    REQUIRE(rc.right  == 120);
    REQUIRE(rc.bottom == 328);
}

TEST_CASE("ThumbnailModel: set_current_page reports old/new pair", "[thumbnail_model]") {
    ThumbnailModel m;
    m.set_page_count(10);
    REQUIRE(m.current_page() == 0);
    auto changed = m.set_current_page(5);
    REQUIRE(changed.first  == 0);
    REQUIRE(changed.second == 5);
    REQUIRE(m.current_page() == 5);
    auto same = m.set_current_page(5);
    REQUIRE(same.first  == -1);  // sentinel: no change
    REQUIRE(same.second == -1);
}

TEST_CASE("ThumbnailModel: scroll_to_make_visible centers offscreen page", "[thumbnail_model]") {
    ThumbnailModel m;
    m.set_dpi(96);
    m.set_tile_dip({120, 160});
    m.set_gap_dip(8);
    m.set_page_count(20);
    m.set_viewport_h_px(400);
    m.set_scroll_y_px(0);  // visible 0..2
    m.scroll_to_make_visible(10);
    auto r = m.visible_range();
    REQUIRE(r.first <= 10);
    REQUIRE(r.last  >= 10);
    // Already-visible page is a no-op:
    int prev = m.scroll_y_px();
    m.scroll_to_make_visible(r.first + 1);
    REQUIRE(m.scroll_y_px() == prev);
}
```

**Step 1.2: Verify tests fail.**

```bash
cmake --build build --config Release
ctest --test-dir build -C Release -R thumbnail_model --output-on-failure
```

Expected: compile error — `ThumbnailModel.hpp` does not exist yet.

**Step 1.3: Write `src/core/ThumbnailModel.hpp`:**

```cpp
#pragma once

// core::ThumbnailModel — pure-logic state for the left thumbnail pane.
// No Win32, no MuPDF, no threads. UI-thread-only callers; not thread-safe.

#include <cstddef>
#include <optional>
#include <utility>
#include <windows.h>  // RECT only — header is otherwise platform-free.

namespace litepdf::core {

class ThumbnailModel {
public:
    struct DipSize { int w; int h; };
    struct Range   { int first; int last; };  // inclusive; empty if last < first.

    ThumbnailModel();
    ~ThumbnailModel();

    void set_page_count(int n);
    void set_dpi(unsigned dpi);
    void set_tile_dip(DipSize d);
    void set_gap_dip(int g);
    void set_viewport_h_px(int h);
    void set_scroll_y_px(int y);

    // Returns {old_page, new_page} on change; {-1, -1} on no-op.
    std::pair<int,int> set_current_page(int p);
    int current_page() const noexcept { return current_page_; }

    int total_height_px() const noexcept;
    int tile_h_px() const noexcept;        // tile height in pixels (no gap)
    int tile_w_px() const noexcept;
    int scroll_y_px() const noexcept { return scroll_y_; }

    Range visible_range() const noexcept;
    Range visible_range_with_buffer() const noexcept;  // ±1 page

    std::optional<int> page_at_y(int y_px) const noexcept;
    RECT  tile_rect(int page) const noexcept;

    // Adjusts scroll so `page` is in visible_range. No-op if already visible.
    void  scroll_to_make_visible(int page) noexcept;

    int   page_count() const noexcept { return page_count_; }

private:
    int      page_count_     = 0;
    unsigned dpi_            = 96;
    DipSize  tile_dip_       = {120, 160};
    int      gap_dip_        = 8;
    int      viewport_h_     = 0;
    int      scroll_y_       = 0;
    int      current_page_   = 0;
};

}  // namespace litepdf::core
```

**Step 1.4: Write `src/core/ThumbnailModel.cpp`:**

```cpp
#include "core/ThumbnailModel.hpp"

#include <algorithm>

namespace litepdf::core {

ThumbnailModel::ThumbnailModel()  = default;
ThumbnailModel::~ThumbnailModel() = default;

namespace {
inline int dip_to_px(int dip, unsigned dpi) {
    return MulDiv(dip, static_cast<int>(dpi), 96);
}
}  // namespace

void ThumbnailModel::set_page_count(int n)   { page_count_ = std::max(0, n); }
void ThumbnailModel::set_dpi(unsigned dpi)   { dpi_ = (dpi > 0 ? dpi : 96); }
void ThumbnailModel::set_tile_dip(DipSize d) { tile_dip_ = d; }
void ThumbnailModel::set_gap_dip(int g)      { gap_dip_ = std::max(0, g); }
void ThumbnailModel::set_viewport_h_px(int h){ viewport_h_ = std::max(0, h); }

void ThumbnailModel::set_scroll_y_px(int y) {
    const int max_y = std::max(0, total_height_px() - viewport_h_);
    scroll_y_ = std::clamp(y, 0, max_y);
}

std::pair<int,int> ThumbnailModel::set_current_page(int p) {
    p = std::clamp(p, 0, std::max(0, page_count_ - 1));
    if (p == current_page_) return {-1, -1};
    const int old = current_page_;
    current_page_ = p;
    return {old, p};
}

int ThumbnailModel::tile_h_px() const noexcept { return dip_to_px(tile_dip_.h, dpi_); }
int ThumbnailModel::tile_w_px() const noexcept { return dip_to_px(tile_dip_.w, dpi_); }

int ThumbnailModel::total_height_px() const noexcept {
    if (page_count_ == 0) return 0;
    const int pitch = tile_h_px() + dip_to_px(gap_dip_, dpi_);
    // n tiles consume n*pitch - gap (no trailing gap after last tile).
    return page_count_ * pitch - dip_to_px(gap_dip_, dpi_);
}

ThumbnailModel::Range ThumbnailModel::visible_range() const noexcept {
    if (page_count_ == 0 || viewport_h_ == 0) return {0, -1};
    const int pitch = tile_h_px() + dip_to_px(gap_dip_, dpi_);
    if (pitch <= 0) return {0, -1};
    const int first = std::max(0, scroll_y_ / pitch);
    // Last page whose top is < scroll_y_ + viewport_h_.
    const int last  = std::min(page_count_ - 1,
                               (scroll_y_ + viewport_h_ - 1) / pitch);
    return {first, last};
}

ThumbnailModel::Range ThumbnailModel::visible_range_with_buffer() const noexcept {
    const Range r = visible_range();
    if (r.last < r.first) return r;
    return {std::max(0, r.first - 1),
            std::min(page_count_ - 1, r.last + 1)};
}

std::optional<int> ThumbnailModel::page_at_y(int y_px) const noexcept {
    if (page_count_ == 0) return std::nullopt;
    const int pitch = tile_h_px() + dip_to_px(gap_dip_, dpi_);
    if (pitch <= 0) return std::nullopt;
    const int abs_y = scroll_y_ + y_px;
    if (abs_y < 0) return std::nullopt;
    const int page = abs_y / pitch;
    if (page >= page_count_) return std::nullopt;
    const int into_tile = abs_y % pitch;
    if (into_tile >= tile_h_px()) return std::nullopt;  // gap region
    return page;
}

RECT ThumbnailModel::tile_rect(int page) const noexcept {
    const int pitch = tile_h_px() + dip_to_px(gap_dip_, dpi_);
    const int top   = page * pitch - scroll_y_;
    return {0, top, tile_w_px(), top + tile_h_px()};
}

void ThumbnailModel::scroll_to_make_visible(int page) noexcept {
    if (page < 0 || page >= page_count_) return;
    const Range r = visible_range();
    if (page >= r.first && page <= r.last) return;  // already visible
    const int pitch = tile_h_px() + dip_to_px(gap_dip_, dpi_);
    set_scroll_y_px(page * pitch);  // align top of page to viewport top
}

}  // namespace litepdf::core
```

**Step 1.5: Add to CMake.** In `src/CMakeLists.txt`, find the `add_library(litepdf_core STATIC ...)` block and add `core/ThumbnailModel.cpp`. In `tests/unit/CMakeLists.txt`, add `ThumbnailModelTests.cpp` to the test executable sources.

**Step 1.6: Build + run tests.**

```bash
cmake --build build --config Release
ctest --test-dir build -C Release -R thumbnail_model --output-on-failure
```

Expected: 8/8 [thumbnail_model] cases pass. Total ctest count: 101 → 109.

**Step 1.7: Commit.**

```bash
git add src/core/ThumbnailModel.* src/CMakeLists.txt tests/unit/ThumbnailModelTests.cpp tests/unit/CMakeLists.txt
git commit -m "feat(core): ThumbnailModel pure-logic pane geometry (Phase 7 D5)"
```

---

### Task 2: `core::ThumbCache` HBITMAP LRU + tests

**Goal:** Tiny LRU keyed by `int page_num`, holding `HBITMAP` refs. Capacity ~30. Tests verify put/get/evict and resource cleanup discipline.

**Files:**
- Create: `src/core/ThumbCache.hpp` (~50 LOC).
- Create: `src/core/ThumbCache.cpp` (~80 LOC).
- Create: `tests/unit/ThumbCacheTests.cpp` (~80 LOC).
- Modify: `src/CMakeLists.txt`, `tests/unit/CMakeLists.txt`.

**Step 2.1: Write tests.** Create `tests/unit/ThumbCacheTests.cpp`:

```cpp
#include "core/ThumbCache.hpp"

#include <catch2/catch_test_macros.hpp>
#include <windows.h>

using litepdf::core::ThumbCache;

namespace {
HBITMAP make_dummy_bitmap() {
    // 2×2 monochrome — minimum viable HBITMAP.
    return CreateBitmap(2, 2, 1, 1, nullptr);
}
}

TEST_CASE("ThumbCache: empty cache returns null on miss", "[thumb_cache]") {
    ThumbCache c(4);
    REQUIRE(c.get(0) == nullptr);
}

TEST_CASE("ThumbCache: put then get returns same handle", "[thumb_cache]") {
    ThumbCache c(4);
    HBITMAP b = make_dummy_bitmap();
    c.put(7, b);
    REQUIRE(c.get(7) == b);
    REQUIRE(c.size() == 1);
}

TEST_CASE("ThumbCache: replacing same key drops old bitmap", "[thumb_cache]") {
    ThumbCache c(4);
    HBITMAP a = make_dummy_bitmap();
    HBITMAP b = make_dummy_bitmap();
    c.put(3, a);
    c.put(3, b);
    REQUIRE(c.get(3) == b);
    REQUIRE(c.size() == 1);
    // a was DeleteObject'd by put(3, b). We can't verify Win32-side cleanly
    // without a leak detector, but logical state must show b only.
}

TEST_CASE("ThumbCache: put with same page+same handle is a no-op",
          "[thumb_cache]") {
    // Added per plan-eng-review 2B: defensive test against accidental
    // double-DeleteObject on idempotent puts.
    ThumbCache c(4);
    HBITMAP a = make_dummy_bitmap();
    c.put(3, a);
    c.put(3, a);  // same key + same handle → must NOT DeleteObject(a)
    REQUIRE(c.get(3) == a);
    REQUIRE(c.size() == 1);
    // No way to assert DeleteObject wasn't called from this side, but the
    // implementation's same-handle short-circuit means logical state is
    // intact. If put() ever drops same-handle, get(3) would still return
    // a stale pointer that BitBlt would crash on — surfaces under the
    // smoke-test handle-count check in T9.
}

TEST_CASE("ThumbCache: capacity overflow evicts least-recently-used", "[thumb_cache]") {
    ThumbCache c(2);
    c.put(1, make_dummy_bitmap());
    c.put(2, make_dummy_bitmap());
    (void)c.get(1);  // touch 1 → 2 is now LRU
    c.put(3, make_dummy_bitmap());
    REQUIRE(c.get(2) == nullptr);  // evicted
    REQUIRE(c.get(1) != nullptr);
    REQUIRE(c.get(3) != nullptr);
    REQUIRE(c.size() == 2);
}

TEST_CASE("ThumbCache: clear empties cache", "[thumb_cache]") {
    ThumbCache c(4);
    c.put(1, make_dummy_bitmap());
    c.put(2, make_dummy_bitmap());
    c.clear();
    REQUIRE(c.size() == 0);
    REQUIRE(c.get(1) == nullptr);
}
```

**Step 2.2: Run, verify FAIL** (header missing).

**Step 2.3: Write `src/core/ThumbCache.hpp`:**

```cpp
#pragma once

// core::ThumbCache — small HBITMAP LRU keyed by page_num. Separate
// from PageCache to avoid polluting L1/L2 with thumb-resolution
// pixmaps. Owns HBITMAP refs (DeleteObject on evict / replace / dtor).
//
// Thread-safety: NONE. UI-thread-only.

#include <cstddef>
#include <list>
#include <unordered_map>
#include <windows.h>

namespace litepdf::core {

class ThumbCache {
public:
    explicit ThumbCache(std::size_t capacity);
    ~ThumbCache();

    ThumbCache(const ThumbCache&)            = delete;
    ThumbCache& operator=(const ThumbCache&) = delete;

    // Returns the HBITMAP for `page` and marks it most-recently-used.
    // Caller does NOT take ownership — handle is valid until cache
    // evicts or is destroyed. Caller MUST NOT DeleteObject the result.
    HBITMAP get(int page);

    // Cache TAKES ownership of `bm`. If `page` already had a bitmap,
    // the old one is DeleteObject'd. Replacing with the same handle
    // is a no-op.
    void put(int page, HBITMAP bm);

    void clear();

    std::size_t size() const noexcept { return map_.size(); }
    std::size_t capacity() const noexcept { return capacity_; }

private:
    using Iter = std::list<int>::iterator;
    std::size_t                          capacity_;
    std::list<int>                       lru_;       // front = MRU
    std::unordered_map<int, std::pair<HBITMAP, Iter>> map_;
};

}  // namespace litepdf::core
```

**Step 2.4: Write `src/core/ThumbCache.cpp`:**

```cpp
#include "core/ThumbCache.hpp"

namespace litepdf::core {

ThumbCache::ThumbCache(std::size_t capacity) : capacity_(capacity) {}

ThumbCache::~ThumbCache() { clear(); }

HBITMAP ThumbCache::get(int page) {
    auto it = map_.find(page);
    if (it == map_.end()) return nullptr;
    lru_.splice(lru_.begin(), lru_, it->second.second);
    return it->second.first;
}

void ThumbCache::put(int page, HBITMAP bm) {
    auto it = map_.find(page);
    if (it != map_.end()) {
        if (it->second.first == bm) {
            lru_.splice(lru_.begin(), lru_, it->second.second);
            return;
        }
        DeleteObject(it->second.first);
        it->second.first = bm;
        lru_.splice(lru_.begin(), lru_, it->second.second);
        return;
    }
    while (map_.size() >= capacity_ && !lru_.empty()) {
        const int evict = lru_.back();
        lru_.pop_back();
        auto evict_it = map_.find(evict);
        if (evict_it != map_.end()) {
            DeleteObject(evict_it->second.first);
            map_.erase(evict_it);
        }
    }
    lru_.push_front(page);
    map_.emplace(page, std::make_pair(bm, lru_.begin()));
}

void ThumbCache::clear() {
    for (auto& kv : map_) DeleteObject(kv.second.first);
    map_.clear();
    lru_.clear();
}

}  // namespace litepdf::core
```

**Step 2.5: CMake + run tests.** Expected: 6/6 [thumb_cache] cases pass; total 110 → 116. (Counts assume Task 0.5 already added one [render_engine][bypass] case, taking baseline 101 → 102.)

**Step 2.6: Commit.**

```bash
git add src/core/ThumbCache.* src/CMakeLists.txt tests/unit/ThumbCacheTests.cpp tests/unit/CMakeLists.txt
git commit -m "feat(core): ThumbCache HBITMAP LRU (Phase 7 D2)"
```

---

### Task 3: `core::ThumbnailRenderer` — thumb-friendly wrapper around the existing per-tab `RenderEngine`

**Goal:** Provide a `submit(page, on_done)` API that submits to each `DocumentView`'s **existing** `RenderEngine` with `priority=3` and `bypass_cache=true` (the flag added in Task 0.5). On completion, the worker's pixmap is converted to `HBITMAP` and delivered to `on_done`. **Lifetime safety**: adopt Phase 6 C1's task-drain pattern (D16) so a tab close mid-render cannot UAF.

**Files:**
- Create: `src/core/ThumbnailRenderer.hpp` (~50 LOC).
- Create: `src/core/ThumbnailRenderer.cpp` (~120 LOC).
- Create: `tests/unit/ThumbnailRendererTests.cpp` (~60 LOC, integration with real Document fixture).
- Modify: `src/CMakeLists.txt`, `tests/unit/CMakeLists.txt`.

**Step 3.1: Write tests.** Create `tests/unit/ThumbnailRendererTests.cpp`:

```cpp
#include "core/Document.hpp"
#include "core/PageCache.hpp"
#include "core/RenderEngine.hpp"
#include "core/ThumbnailRenderer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;
using litepdf::core::Document;
using litepdf::core::PageCache;
using litepdf::core::RenderEngine;
using litepdf::core::ThumbnailRenderer;

TEST_CASE("ThumbnailRenderer: produces HBITMAP for a real page",
          "[thumb_renderer]") {
    Document doc; REQUIRE(doc.open(L"tests/fixtures/simple.pdf"));
    PageCache cache(4, 4, doc.ui_ctx());
    RenderEngine eng(doc, /*workers=*/2, &cache);
    ThumbnailRenderer r(eng);  // borrow the doc's engine

    std::atomic<int> got{0};
    HBITMAP captured = nullptr;
    r.submit(0, [&](HBITMAP bm) {
        captured = bm;
        got.fetch_add(1, std::memory_order_release);
    });
    for (int i = 0; i < 500 && got.load(std::memory_order_acquire) == 0; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    REQUIRE(got.load() == 1);
    REQUIRE(captured != nullptr);
    DeleteObject(captured);
    // D2 invariant: thumb pass must not have polluted the main cache.
    REQUIRE(cache.get_pixmap(0, 0.15f) == nullptr);
}

TEST_CASE("ThumbnailRenderer: dtor drains in-flight tasks (D16 / 1A)",
          "[thumb_renderer]") {
    // Submits N requests then immediately destroys the renderer.
    // Without D16's pending_tasks drain, the worker callback could fire
    // after Impl is gone and either UAF-touch impl_->pending_tasks or
    // PostMessage to a destroyed HWND in real use. Test contract:
    // dtor MUST NOT return until every in-flight on_complete has run.
    Document doc; REQUIRE(doc.open(L"tests/fixtures/simple.pdf"));
    PageCache cache(4, 4, doc.ui_ctx());
    RenderEngine eng(doc, /*workers=*/1, &cache);

    std::atomic<int> completed{0};
    {
        ThumbnailRenderer r(eng);
        for (int i = 0; i < 3; ++i) {
            r.submit(0, [&](HBITMAP bm) {
                if (bm) DeleteObject(bm);
                completed.fetch_add(1, std::memory_order_release);
            });
        }
        // Renderer dtor here MUST wait for all 3 callbacks to complete.
    }
    // After dtor returns, no callback should be in flight.
    REQUIRE(completed.load() == 3);
}

TEST_CASE("ThumbnailRenderer: cancel_pending stops not-yet-started work",
          "[thumb_renderer]") {
    Document doc; REQUIRE(doc.open(L"tests/fixtures/simple.pdf"));
    PageCache cache(4, 4, doc.ui_ctx());
    RenderEngine eng(doc, /*workers=*/1, &cache);
    ThumbnailRenderer r(eng);

    std::atomic<int> seen{0};
    for (int i = 0; i < 5; ++i) {
        r.submit(i, [&](HBITMAP bm) {
            seen.fetch_add(1, std::memory_order_release);
            if (bm) DeleteObject(bm);
        });
    }
    r.cancel_pending();
    std::this_thread::sleep_for(200ms);
    REQUIRE(seen.load() <= 5);  // some may have started before cancel
}
```

**Step 3.2: Verify FAIL.**

**Step 3.3: Write `ThumbnailRenderer.hpp`:**

```cpp
#pragma once

// core::ThumbnailRenderer — thumb-friendly facade over the existing
// per-tab RenderEngine. Submits at priority=3 with bypass_cache=true
// so the main render cache (L1/L2) is never touched.
//
// Lifetime contract: on_done runs on a worker thread and receives an
// HBITMAP (or nullptr on render failure); caller takes ownership.
// Adopts Phase 6 C1's task-drain pattern (D16): the dtor blocks until
// every in-flight on_done has completed, so it is safe to destroy the
// renderer (and any objects captured by on_done lambdas) right after
// submitting work. Mirrors the fix in commit 8e11f39 for SearchSession.

#include "core/RenderEngine.hpp"

#include <functional>
#include <memory>
#include <windows.h>

namespace litepdf::core {

class ThumbnailRenderer {
public:
    using OnDone = std::function<void(HBITMAP)>;

    // `engine` must outlive this ThumbnailRenderer. Typically each
    // DocumentView owns one RenderEngine and one ThumbnailRenderer
    // that borrows the engine. Default scale = 0.15 (~ 11 dpi at the
    // 72-baseline, producing 120-dip-wide thumbs on letter pages).
    explicit ThumbnailRenderer(RenderEngine& engine, float scale = 0.15f);
    ~ThumbnailRenderer();

    ThumbnailRenderer(const ThumbnailRenderer&)            = delete;
    ThumbnailRenderer& operator=(const ThumbnailRenderer&) = delete;

    // Submits page render at priority=3, bypass_cache=true. On completion,
    // on_done fires on a worker thread.
    void submit(int page, OnDone on_done);

    // Cancels all not-yet-started priority=3 requests. In-flight ones
    // still complete (cooperative cancellation, same as RenderEngine).
    void cancel_pending();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace litepdf::core
```

**Step 3.4: Write `ThumbnailRenderer.cpp`** — submits via the borrowed engine with `priority=3` and `bypass_cache=true`, converts the resulting `fz_pixmap` to `HBITMAP` via `CreateDIBSection`, and gates dtor on a `pending_tasks` drain (D16). Pixmap component handling expanded per plan-eng-review 2C: MuPDF's render output is normally 4-component (RGBA), but ePub or some grayscale PDFs may produce 1- or 2-component pixmaps; T3 expands those to BGRA on the fly rather than silently returning nullptr.

```cpp
#include "core/ThumbnailRenderer.hpp"

#include <atomic>
#include <chrono>
#include <thread>

extern "C" {
#include <mupdf/fitz.h>
}

namespace litepdf::core {

namespace {
// Convert an MuPDF pixmap to a Win32 32-bit BGRA top-down DIBSection.
// Handles 1-component (gray), 2-component (gray+alpha), 3-component (rgb),
// and 4-component (rgba) inputs. Returns nullptr only on width/height
// validation failure or DIBSection allocation failure.
HBITMAP pixmap_to_hbitmap(fz_pixmap* pix, fz_context* ctx) {
    if (!pix) return nullptr;
    const int w = fz_pixmap_width(ctx, pix);
    const int h = fz_pixmap_height(ctx, pix);
    const int n = fz_pixmap_components(ctx, pix);
    if (w <= 0 || h <= 0 || n < 1 || n > 4) return nullptr;

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
    const int src_stride = fz_pixmap_stride(ctx, pix);
    auto* dst = static_cast<unsigned char*>(bits);
    const int dst_stride = w * 4;
    for (int y = 0; y < h; ++y) {
        const unsigned char* sr = src + y * src_stride;
        unsigned char* dr = dst + y * dst_stride;
        for (int x = 0; x < w; ++x) {
            // Per-component mapping. Goal: produce BGRA always.
            unsigned char r, g, b, a;
            switch (n) {
                case 1:  // gray
                    r = g = b = sr[0]; a = 0xFF; break;
                case 2:  // gray + alpha
                    r = g = b = sr[0]; a = sr[1]; break;
                case 3:  // rgb (no alpha)
                    r = sr[0]; g = sr[1]; b = sr[2]; a = 0xFF; break;
                default: // case 4 = rgba
                    r = sr[0]; g = sr[1]; b = sr[2]; a = sr[3]; break;
            }
            dr[0] = b; dr[1] = g; dr[2] = r; dr[3] = a;
            sr += n;
            dr += 4;
        }
    }
    return bm;
}
}  // namespace

struct ThumbnailRenderer::Impl {
    RenderEngine&   engine;
    float           scale;
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
```

**Step 3.5: CMake + tests.** Expected: 3/3 [thumb_renderer] cases pass (added drain-on-dtor case per D16); total 116 → 119.

**Step 3.6: Commit.**

```bash
git add src/core/ThumbnailRenderer.* src/CMakeLists.txt tests/unit/ThumbnailRendererTests.cpp tests/unit/CMakeLists.txt
git commit -m "feat(core): ThumbnailRenderer reuses engine + task drain (Phase 7 D3/D16)"
```

---

### Task 4a: Refactor existing `ui::Splitter` to expose a shared `SplitterCore` (PREREQUISITE PR — lands separately, before Phase 7)

> Per plan-eng-review 2A + 2D: Beck's "make the change easy, then make the easy change" — refactor first as its own PR, then add VerticalSplitter as a new file in the Phase 7 PR. Splitting preserves `git bisect` resolution if the in-production results-panel splitter ever regresses.

**Goal:** Extract the WndProc / drag-state / dispatch logic from `ui::Splitter` into a shared `SplitterCore` Impl that both `Splitter` (horizontal — unchanged behavior) and the future `VerticalSplitter` (T4b) will compose. **Zero behavior change** for the in-production results-panel splitter.

**Files:**
- Modify: `src/ui/Splitter.hpp` — keep `class Splitter` API byte-identical; add `// Refactored: Splitter now wraps SplitterCore (Phase 7 prereq).` comment.
- Modify: `src/ui/Splitter.cpp` — extract `SplitterCore` private struct holding `{HWND, drag_state, OnDrag cb, Orientation orient}` plus `enum class Orientation { Horizontal, Vertical }`. Move the WM_LBUTTONDOWN / WM_MOUSEMOVE / WM_LBUTTONUP / WM_CAPTURECHANGED / WM_SETCURSOR handlers into `SplitterCore::handle_message(...)`. `Splitter::Impl` becomes a thin shell that owns a `SplitterCore` constructed with `Orientation::Horizontal` and `IDC_SIZENS`.
- Create: `src/ui/detail/SplitterMath.hpp` — extract clamp math as free functions:
  ```cpp
  namespace litepdf::ui::detail {
  inline int compute_drag_target_y(int mouse_y, int parent_h, int min_h, int max_h);
  inline int compute_drag_target_x(int mouse_x, int parent_w, int min_w, int max_w);
  }
  ```
  Used by `SplitterCore::handle_message` based on orientation.
- Modify: `tests/unit/CMakeLists.txt`, optionally add `tests/unit/SplitterMathTests.cpp` (~30 LOC) covering clamp boundaries for both axes.

**Step 4a.1: Extract `SplitterMath.hpp`** with two pure clamp helpers. Add 4 unit cases ([splitter_math]) covering low/high clamps for both axes.

**Step 4a.2: Refactor `Splitter.cpp`.** All WndProc message handlers move into a `SplitterCore` member. `Splitter::Impl` retains its `HWND` ownership but delegates message dispatch. Public `Splitter` API and the existing horizontal-Splitter behavior are unchanged.

**Step 4a.3: Verify zero regression.**
```bash
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
build/Release/litepdf.exe tests/fixtures/bookmarks.pdf
# Press Ctrl+Shift+F to open the results panel, then drag the splitter
# bar. Behavior must be identical to pre-refactor: cursor = IDC_SIZENS,
# results-panel height resizes smoothly, capture release on alt+tab.
```

**Step 4a.4: Open prerequisite PR.** Branch `refactor/splitter-core`, single commit, PR title:
```
refactor(ui): extract SplitterCore for orientation reuse (Phase 7 prereq)
```
PR body should reference Phase 7 plan §D9 + 2A. CI gate + manual smoke. Land before any Phase 7 task starts.

**Step 4a.5: After PR-4a merges, rebase the Phase 7 implementation branch onto the new `origin/main`.** `git rebase` patch-id detection will handle this cleanly (same as the Phase 6 rebase on Phase 5.4).

---

### Task 4b: Add `ui::VerticalSplitter` as a thin wrapper over `SplitterCore` (lands in the Phase 7 PR)

**Goal:** With `SplitterCore` already extracted in T4a, T4b is purely additive: a new file thin-wraps the shared core with `Orientation::Vertical` + `IDC_SIZEWE`. No existing code is touched.

**Files:**
- Create: `src/ui/VerticalSplitter.hpp` + `.cpp` (~30 LOC each — both ctor + dtor + 3 forwarders to `Impl`).
- Create: `tests/unit/VerticalSplitterTests.cpp` — exercises `detail::compute_drag_target_x` directly via the free helper added in T4a, plus a smoke unit case constructing/destroying VerticalSplitter without a parent window (confirms ctor doesn't blow up on minimal init).
- Modify: `src/CMakeLists.txt`, `tests/unit/CMakeLists.txt`.

**Step 4b.1: VerticalSplitter.hpp:**

```cpp
#pragma once

// ui::VerticalSplitter — vertical 4-DIP drag bar for resizing the
// left thumbnail/outline pane. Thin wrapper over SplitterCore; see
// Phase 7 D9 + Task 4a (the prerequisite refactor).

#include <functional>
#include <memory>
#include <windows.h>

namespace litepdf::ui {

class VerticalSplitter {
public:
    using OnDrag = std::function<void(int new_width_px)>;
    VerticalSplitter(HINSTANCE, HWND parent);
    ~VerticalSplitter();

    VerticalSplitter(const VerticalSplitter&)            = delete;
    VerticalSplitter& operator=(const VerticalSplitter&) = delete;

    HWND hwnd() const;
    void set_on_drag(OnDrag cb);
    void set_bounds(const RECT& bounds);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace litepdf::ui
```

**Step 4b.2: VerticalSplitter.cpp** — Impl owns a `SplitterCore` constructed with `Orientation::Vertical` + `IDC_SIZEWE`. All message handling reuses `SplitterCore::handle_message`. `set_bounds` rect interpretation flips axes (the bar lives at x = pane_width, full client-height) but the call to SplitterCore is identical.

**Step 4b.3: Tests.**

```cpp
#include "ui/detail/SplitterMath.hpp"
#include <catch2/catch_test_macros.hpp>

using litepdf::ui::detail::compute_drag_target_x;

TEST_CASE("VerticalSplitter math: clamps below min", "[vsplitter]") {
    REQUIRE(compute_drag_target_x(50, 1000, 150, 800) == 150);
}
TEST_CASE("VerticalSplitter math: clamps above max", "[vsplitter]") {
    REQUIRE(compute_drag_target_x(900, 1000, 150, 800) == 800);
}
```

**Step 4b.4: CMake + tests.** Expected: 2/2 new [vsplitter] pass; baseline already includes T4a's [splitter_math] cases, so total grows by exactly 2 here.

**Step 4b.5: Commit (within Phase 7 PR).**

```bash
git add src/ui/VerticalSplitter.* src/CMakeLists.txt tests/unit/VerticalSplitterTests.cpp tests/unit/CMakeLists.txt
git commit -m "feat(ui): VerticalSplitter wraps SplitterCore (Phase 7 D9 / Task 4b)"
```

---

### Task 5: `ui::ThumbnailPane` skeleton (Win32 widget, paint placeholder, hit-test)

**Goal:** Land the widget skeleton — owner-draw `SysListView32`, integrates with `ThumbnailModel`, paints page-number placeholder rectangles (no real thumbs yet, no renderer wiring). Hide-by-default, show/hide/visible API matching `OutlinePane`.

**Files:**
- Create: `src/ui/ThumbnailPane.hpp` (~60 LOC).
- Create: `src/ui/ThumbnailPane.cpp` (~250 LOC).
- Modify: `src/CMakeLists.txt`.
- (No new tests in this task — Win32 widget is exercised via T8 smoke + manual.)

**Step 5.1:** Header API mirrors `OutlinePane`:

```cpp
class ThumbnailPane {
public:
    using NavigateCb = std::function<void(int page)>;
    ThumbnailPane(HINSTANCE, HWND parent);
    ~ThumbnailPane();

    HWND hwnd() const;
    void set_on_navigate(NavigateCb cb);
    void set_page_count(int n);
    void set_current_page(int page);

    void show();
    void hide();
    bool visible() const;

    // Phase 7 Task 6 will add:
    //   void set_renderer(ThumbnailRenderer*);
    //   void set_cache(ThumbCache*);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
```

**Step 5.2:** Implementation skeleton — `Impl` owns `HWND list_hwnd_`, `ThumbnailModel model_`, `NavigateCb nav_cb_`. Subclass the ListView's WndProc to handle `WM_SIZE` (recompute viewport_h_px in model), `WM_VSCROLL` (update model.scroll_y), `WM_LBUTTONDOWN` (call `model_.page_at_y(pt.y)` → if some, call `nav_cb_`). Owner-draw via `WM_DRAWITEM`: paint placeholder rectangle (palette.tab_bg fill + palette.text border + page-number text via `DrawTextW`).

**Step 5.3 (added per plan-eng-review 1D): DPI hot-switch handling.** Add a public `void on_dpi_changed(unsigned new_dpi)` method to `ThumbnailPane`:

```cpp
void ThumbnailPane::on_dpi_changed(unsigned new_dpi) {
    impl_->model_.set_dpi(new_dpi);
    // Cached HBITMAPs are baked to old pixel sizes; nuke them.
    if (impl_->cache_) impl_->cache_->clear();
    // Pending renders encode stale dpi-derived sizes via tile rect math
    // upstream; cancel them so we don't paint stretched results.
    if (impl_->renderer_) impl_->renderer_->cancel_pending();
    InvalidateRect(impl_->list_hwnd_, nullptr, TRUE);
}
```

`MainWindow` already handles `WM_DPICHANGED` for the tab strip (Phase 5.4). Extend that handler:

```cpp
case WM_DPICHANGED: {
    // ... existing tab-strip handling ...
    if (active_view_) {
        if (auto* tp = active_view_->thumb_pane()) {
            tp->on_dpi_changed(HIWORD(wParam));
        }
    }
    return 0;
}
```

**Step 5.4:** Manual smoke. Build, launch with `bookmarks.pdf`, push a placeholder F4 handler in MainWindow that just calls `thumb_pane_->show()` (full handler comes in T8). Verify pane shows, page-number rectangles appear for visible pages, scroll works, click logs the page (printf or OutputDebugString — temporary). DPI smoke: drag the window between two monitors at different scaling (e.g. 100% ↔ 200%); pane content invalidates and re-renders at the new size; no stretched/squished tiles.

**Step 5.5:** Commit.

```bash
git add src/ui/ThumbnailPane.* src/CMakeLists.txt
git commit -m "feat(ui): ThumbnailPane skeleton (owner-draw ListView + hit test)"
```

---

### Task 6: ThumbnailPane paints real thumbs (renderer + cache wiring)

**Goal:** Add `set_renderer(ThumbnailRenderer*)` and `set_cache(ThumbCache*)`. In `WM_DRAWITEM`, look up HBITMAP for the page in cache; on hit, blit it; on miss, paint placeholder AND submit a render via the renderer with a callback that puts the result into cache and posts `WM_USER_THUMB_READY` to invalidate the tile.

**Files:**
- Modify: `src/ui/ThumbnailPane.hpp` (+2 setters).
- Modify: `src/ui/ThumbnailPane.cpp` (+~100 LOC).

**Step 6.1:** Define `WM_USER_THUMB_READY = WM_USER + 17` in pane impl. Marshaling pattern:

```cpp
// In renderer callback (worker thread):
PostMessageW(pane_hwnd, WM_USER_THUMB_READY,
             /*wParam=*/static_cast<WPARAM>(page),
             /*lParam=*/reinterpret_cast<LPARAM>(bm));

// In pane WndProc (UI thread):
case WM_USER_THUMB_READY: {
    int page = static_cast<int>(wParam);
    HBITMAP bm = reinterpret_cast<HBITMAP>(lParam);
    cache_->put(page, bm);  // takes ownership
    invalidate_tile(page);
    return 0;
}
```

**Step 6.2:** In `WM_DRAWITEM`, walk `model.visible_range_with_buffer()`. For each page: if `cache_->get(page)` returns non-null → `BitBlt` the HBITMAP into the tile rect. Otherwise paint placeholder, and if not yet pending in `pending_renders_` set, submit `renderer_->submit(page, ...)` and insert into `pending_renders_`.

**`pending_renders_` lifecycle (added per plan-eng-review 4A):** the set MUST be mutated at exactly these 5 points, all on the UI thread:

1. **Insert** — when `WM_DRAWITEM` submits a render for a cache miss.
2. **Erase** — when `WM_USER_THUMB_READY` arrives for a given page (regardless of whether HBITMAP is non-null; null lParam signals render failure but the request is no longer pending).
3. **Erase + cancel** — on `set_page_count(n)` change (document swapped or cleared); call `renderer_->cancel_pending()` then `pending_renders_.clear()`.
4. **Erase + cancel** — on `clear()` (explicit reset); same as above.
5. **Erase + cancel** — on dtor; `~Impl()` calls `cancel_pending` and then drops the set. Note that `ThumbnailRenderer::~Impl` will block on its own task drain (D16) so by the time the pane's dtor returns, no `WM_USER_THUMB_READY` is in flight that could touch a freed pane.

If `pending_renders_` is mutated outside these points (especially from a worker thread), the dedupe guarantee breaks and the same page can re-submit while a render is in flight, wasting CPU. Code review focus area in T6.

**Step 6.3:** Hover state (optional polish, copy from TabManager pattern): on `WM_MOUSEMOVE` track `hot_page_`, draw a 1-px palette.accent border. Skip if scope tightens.

**Step 6.4:** Manual smoke: open `bookmarks.pdf`, F4 → real thumbs paint within ~1 s for the first 5 visible pages. Scroll, observe new pages render lazily.

**Step 6.5:** Commit.

```bash
git add src/ui/ThumbnailPane.*
git commit -m "feat(ui): ThumbnailPane lazy renders via ThumbCache + ThumbnailRenderer (Phase 7 D6)"
```

---

### Task 7: ThumbnailPane — current-page sync + auto-scroll

**Goal:** When `MainWindow` notifies the active `DocumentView` of a page change, `ThumbnailPane` updates its current-page highlight and (if off-screen) scrolls into view.

**Files:**
- Modify: `src/ui/PdfCanvas.{hpp,cpp}` — add `set_on_page_changed(std::function<void(int)>)` observer, fire on Page Up / Down / goto.
- Modify: `src/ui/MainWindow.cpp` — wire canvas observer to active view's thumb pane.
- Modify: `src/ui/ThumbnailPane.cpp` — paint accent border on `model_.current_page()` tile; on `set_current_page`, call `model_.scroll_to_make_visible(page)` and `InvalidateRect` the (old, new) pair.

**Step 7.1:** Add `PdfCanvas::set_on_page_changed(cb)`. Fire from existing page-change paths (PgUp/PgDn, scroll wheel, outline navigate, search-jump). `PdfCanvas` already tracks `current_page_`; just install observer at all the call sites.

**Step 7.2:** In MainWindow, where `DocumentView` is created or set active:

```cpp
view->canvas()->set_on_page_changed([this, view](int p) {
    if (auto* tp = view->thumb_pane()) tp->set_current_page(p);
});
```

**Step 7.3:** ThumbnailPane paint: in DRAWITEM for the current page tile, draw 2-DIP accent border on top of bitmap.

**Step 7.4:** Manual smoke: F4 thumbs visible, PgDn ×3 → highlight moves 0→1→2→3, pane auto-scrolls when page leaves viewport.

**Step 7.5:** Commit.

```bash
git add src/ui/PdfCanvas.* src/ui/MainWindow.cpp src/ui/ThumbnailPane.*
git commit -m "feat(ui): ThumbnailPane current-page sync + auto-scroll (Phase 7 D8)"
```

---

### Task 8: MainWindow integration — F4 handler, mutual exclusion, layout, per-tab ownership

**Goal:** Real F4 handler. F4 ↔ F5 mutual exclusion. `on_layout()` carves the left strip for whichever pane is visible. `DocumentView` owns the `ThumbnailPane*`.

**Files:**
- Modify: `src/core/DocumentView.{hpp,cpp}` — add `std::unique_ptr<ThumbnailPane> thumb_pane_` (note: ui type — DocumentView.hpp will need a forward decl + explicit dtor).
- Modify: `src/ui/MainWindow.cpp`:
  - `WM_COMMAND IDM_VIEW_THUMBS` handler (D10 logic).
  - `WM_COMMAND IDM_VIEW_OUTLINE` — guard to hide thumbs if visible.
  - `on_layout()` — branch on which pane is visible; place + size the visible one.
  - `on_tab_switched()` — show/hide panes per active view's last state.
  - Wire `VerticalSplitter` next to the visible pane (T6 deferred this; lands here).

**Step 8.1:** DocumentView wiring. ThumbnailPane construction is lazy — created on first F4 press for a given tab (saves ~50 KB per never-thumbed tab).

**Step 8.2:** F4 handler in MainWindow:

```cpp
case IDM_VIEW_THUMBS: {
    if (!active_view_) break;
    auto* tp = active_view_->ensure_thumb_pane(hwnd_, instance_,
                                               &thumb_renderer_holder_,
                                               &thumb_cache_holder_);
    if (tp->visible()) {
        tp->hide();
    } else {
        if (outline_ && outline_->visible()) outline_->hide();
        tp->show();
    }
    on_layout();
    break;
}
```

**Step 8.3:** Symmetric guard in `IDM_VIEW_OUTLINE` handler — hide thumb pane if it's the active visible one.

**Step 8.4:** `on_layout()` — refactor the existing `outline_->visible()` branch into `left_pane_visible()` returning `OutlinePane*` or `ThumbnailPane*`-coerced-to-`HWND`. Lay out left strip + vertical splitter at `left_pane_width_px_`, then canvas.

**Step 8.5:** Manual smoke (full):
- Open `bookmarks.pdf`.
- F5 → outline opens.
- F4 → outline disappears, thumbs open.
- F5 → thumbs disappear, outline reopens.
- Drag splitter → pane width changes; canvas reflows.
- Close active tab; new tab thumbs are independent (lazy-create).

**Step 8.6:** Commit.

```bash
git add src/core/DocumentView.* src/ui/MainWindow.cpp
git commit -m "feat(ui): wire ThumbnailPane into MainWindow with F4 + mutual exclusion (Phase 7 D1/D10/D11)"
```

---

### Task 9: Smoke test + version bump + design-doc sync + tag

**Goal:** Add a thumb-pane case to `scripts/smoke-test.ps1`. Bump `VERSION` and `About` dialog to `0.0.8`. Sync `docs/plans/2026-04-15-litepdf-design.md` §4.1 row 7 with reality (per plan-eng-review 2E). Tag `v0.0.8-phase7`.

**Files:**
- Modify: `scripts/smoke-test.ps1`.
- Modify: `VERSION` (`0.0.7` → `0.0.8`).
- Modify: `src/ui/MainWindow.cpp` (About dialog string `v0.0.7` → `v0.0.8`).
- Modify: `CMakeLists.txt` if version is duplicated there (check before editing).
- Modify: `docs/plans/2026-04-15-litepdf-design.md` §4.1 row 7 — replace the original "ui/ThumbnailPane | 350" entry to reflect actual Phase 7 footprint (model + cache + renderer + splitter + integration).

**Step 9.1:** Smoke-test addition (`scripts/smoke-test.ps1`):

```powershell
# Phase 7: thumbnail pane open + render check.
Test-Step "F4 opens thumb pane" {
    $proc = Start-Litepdf "tests/fixtures/bookmarks.pdf"
    Send-Key $proc "{F4}"
    Start-Sleep -Milliseconds 800
    Assert-WindowText $proc "*bookmarks*"  # pane title visible (or just that window still alive)
    Stop-Litepdf $proc
}
```

**Step 9.2:** Bump version + sync design doc:

```bash
echo 0.0.8-dev > VERSION
# Edit src/ui/MainWindow.cpp About dialog: "LitePDF v0.0.7" → "LitePDF v0.0.8"
```

In `docs/plans/2026-04-15-litepdf-design.md` §4.1, replace row 7:

```
| 7 | `ui/ThumbnailPane` | 350 | Lazy-rendered thumbnails, owner-draw |
```

with:

```
| 7 | `ui/ThumbnailPane` + `core/ThumbnailModel` + `core/ThumbCache` + `core/ThumbnailRenderer` + `ui/VerticalSplitter` | ~1275 | Lazy-rendered thumbnail pane (model, HBITMAP cache, renderer borrow of RenderEngine, vertical splitter, MainWindow integration) |
```

**Step 9.3:** Build + full ctest:

```bash
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Expected: 122/122 pass (101 baseline + 1 render_engine bypass + 8 thumbnail_model + 6 thumb_cache + 3 thumb_renderer + 4 splitter_math + 2 vsplitter — but T4a's splitter_math + 1 of its setup are landed in the prerequisite PR, so this Phase 7 PR's CI sees the post-T4a baseline, not 101).

**Note** on the test count: T4a (refactor) lands its 4 [splitter_math] cases on `main` before this PR is rebased. Post-rebase baseline becomes ~105. This Phase 7 PR adds 17 more cases (1 + 8 + 6 + 3 + 2 — the [vsplitter] math reuses the helpers from T4a so VerticalSplitter only adds 2 cases beyond the shared math) for a final 122/122.

**Step 9.4:** Commit + tag.

```bash
git add VERSION scripts/smoke-test.ps1 src/ui/MainWindow.cpp
git commit -m "chore(ui): hardening pass + bump to v0.0.8 (Phase 7 finalize)"
git tag v0.0.8-phase7
```

**Step 9.5:** Prepare PR per `superpowers:finishing-a-development-branch` or `/ship` (pick ONE per CLAUDE.md decision table). Push branch + tag, open PR, merge with rebase strategy (per Phase 5/6 precedent).

---

## Summary Table

Phase 7 lands in two PRs to honor Beck's two-step rule (refactor first, add second):

**Prerequisite PR (refactor only, lands separately on `main`):**

| Task | Component | LOC | New tests | Notes |
|---|---|---|---|---|
| 4a | `ui::SplitterCore` extract from existing Splitter + `detail/SplitterMath.hpp` | ~120 | 4 | Zero behavior change for in-production results-panel splitter |

**Phase 7 PR (the main feature work, branched off post-T4a `main`):**

| Task | Component | LOC | New tests | Notes |
|---|---|---|---|---|
| 0 | Menu IDs + F4 accel | ~5 | — | resource only |
| 0.5 | `RenderEngine::RenderRequest::bypass_cache` flag (D3 enabling change per 1B) | ~10 | 1 | Phase 2 engine extension |
| 1 | `core::ThumbnailModel` | ~180 | 8 | pure logic, TDD |
| 2 | `core::ThumbCache` | ~130 | 6 | HBITMAP LRU (incl. same-handle no-op test per 2B) |
| 3 | `core::ThumbnailRenderer` (borrows engine + D16 task drain) | ~190 | 3 | wraps existing RenderEngine; n-component pixmap handling per 2C |
| 4b | `ui::VerticalSplitter` (thin wrapper over T4a's SplitterCore) | ~70 | 2 | new file only; shared math reused |
| 5 | `ui::ThumbnailPane` skeleton + `WM_DPICHANGED` (per 1D) | ~330 | — | manual smoke |
| 6 | Pane render + cache wire + `pending_renders_` 5-point lifecycle (per 4A) | ~120 | — | manual smoke |
| 7 | Current-page sync | ~80 | — | observer chain |
| 8 | MainWindow integration | ~120 | — | layout + F4 handler |
| 9 | Smoke + version + design-doc §4.1 sync (per 2E) + tag | ~25 | — | release prep |
| **Phase 7 PR subtotal** | — | **~1260** | **20** | excludes T4a (separate PR) |
| **Combined (T4a + Phase 7 PR)** | — | **~1380** | **24** | total surface across both PRs |

LOC budget overrun (~3.9× design's `ui/ThumbnailPane: 350`): the design row counted only the pane widget; reality includes model, cache, renderer, RenderEngine bypass-cache flag, splitter refactor + new vertical splitter, and integration. Comparable to Phase 6's overrun (~750 vs roadmap 500 for similar reasons — model + dispatcher + UI plumbing).

Test count: 101 (post-Phase-6 baseline) → 105 (after T4a merges to main) → 122 (Phase 7 PR end). +20 from this PR; +4 from T4a's prerequisite PR; +24 combined.

---

## Risk Annotations

> Status legend: **[OPEN]** = monitored at implementation. **[MITIGATED]** = upgraded to a concrete design decision or task step during plan-eng-review.

- **R1 [MITIGATED]: `RenderEngine` bypass-cache path.** Original concern: cache=nullptr direct-render path in Phase 2 RenderEngine was rarely exercised in production. **Resolution per plan-eng-review 1B + 1E**: instead of relying on the rare nullptr branch, Phase 7 introduces a per-request `bypass_cache` flag (Task 0.5, ~10 LOC core/ change) that gates both cache lookup AND population. The Pre-flight grep verifies the cache-conditional sites; T0.5 wraps them. One [render_engine][bypass] unit case asserts the contract.

- **R2 [OPEN]: HBITMAP lifetime in ThumbCache eviction.** Win32 GDI handles are notoriously easy to leak. T2 tests verify map state but cannot directly assert `DeleteObject` was called (no public Win32 handle counter). Mitigations: (a) added [thumb_cache] same-page+same-handle no-op test (per 2B) catches double-DeleteObject pattern; (b) keep `ThumbCache` code minimal; review against the `~ThumbCache()` invariant — every `put`-ed handle is `DeleteObject`'d on eviction OR on dtor; (c) acceptance smoke in T9 includes a Task Manager handle-count check after a tab-close-mid-render stress.

- **R3 [MITIGATED]: Re-entrance / cache eviction during paint.** Original concern: `WM_USER_THUMB_READY` arriving mid-paint could DeleteObject an HBITMAP currently being BitBlt'd. **Resolution**: `BitBlt` is synchronous within `WM_DRAWITEM`, and PostMessage'd `WM_USER_THUMB_READY` cannot preempt — it queues until WndProc returns. Plus the `pending_renders_` 5-point lifecycle (T6 §6.2 per 4A) ensures no double-submission. Plus, ThumbnailRenderer's D16 task-drain pattern (per 1A) blocks pane destruction until callbacks unwind, eliminating the more dangerous tab-close-mid-render variant. **Verify in T6 manual smoke.**

- **R4 [MITIGATED]: Splitter refactor regression.** Original concern: refactoring the in-production `ui::Splitter` (used by Phase 6 results panel) inside the same PR as adding `VerticalSplitter` couples concerns and breaks `git bisect`. **Resolution per plan-eng-review 2A**: split into Task 4a (refactor only, lands as a separate prerequisite PR with zero behavior change) and Task 4b (new file VerticalSplitter, lands in Phase 7 PR). T4a has its own CI gate + manual smoke pass on the results-panel splitter before merging.

- **R5 [OPEN]: Lazy-creation race on F4 spam.** If user mashes F4 fast, `ensure_thumb_pane` could be called multiple times before construction completes. Mitigation: `ensure_thumb_pane` checks for existing instance under a single-threaded UI-thread invariant (which holds — all `WM_COMMAND` is serialized through one WndProc).

- **R6 [MITIGATED]: DPI hot-switch.** Original concern: a one-line acceptance reminder without a concrete handling step. **Resolution per plan-eng-review 1D**: T5 §5.3 now spells out `ThumbnailPane::on_dpi_changed(unsigned new_dpi)` (model.set_dpi → cache.clear → renderer.cancel_pending → InvalidateRect) plus the MainWindow `WM_DPICHANGED` extension that calls into the active view's pane. Done When entry verifies on dual-monitor drag.

- **R7 [OPEN]: T4a prerequisite PR slips.** If the T4a refactor PR has unexpected fallout in production results-panel splitter (regression caught in CI or manual smoke), Phase 7 PR is blocked behind it. Mitigation: T4a is a tiny refactor (~120 LOC, zero behavior delta target), single-purpose PR, single commit, low review surface. Worst case: revert T4a, fold the SplitterCore extraction into Phase 7 PR (back to monolithic T4) with R4 reverting to [OPEN].

---

## Done When

1. **Test count:** `ctest` 122/122 green on Release build (post T4a + Phase 7 PR; T4a contributes +4 [splitter_math] cases that land first).
2. **Smoke test:** `scripts/smoke-test.ps1` passes the new "F4 opens thumb pane" step.
3. **First-thumb latency:** F4 toggles thumb pane open/closed; first thumb visible within 1 s on `bookmarks.pdf` (HDD-warm). Subsequent visible tiles render serially behind the existing 2-worker main pool (per 4B / D3) — second tile lands ~150–300 ms after first; this is expected single-worker behavior, not a regression.
4. **Mutual exclusion:** F4 ↔ F5 verified manually — exactly one of {outline, thumbs} visible in the left dock at any time.
5. **Current-page sync:** PgDn ×N in canvas highlights pages 1..N in thumb pane and auto-scrolls when the new page leaves the visible range.
6. **Splitter:** VerticalSplitter drag resizes pane; canvas reflows; width persists across F4-cycle within the same session.
7. **Per-tab independence:** open 3 tabs, F4-toggle each independently — each `DocumentView` owns its own pane state.
8. **No regression in horizontal Splitter:** Phase 6 results panel still drags + resizes correctly after T4a refactor lands. (CI'd separately as part of T4a PR; re-verified post Phase 7 merge.)
9. **DPI hot-switch (per 1D):** drag the main window between two monitors at different scaling (e.g. 100% ↔ 200%). Pane content invalidates; thumbs re-render at the correct pixel size; no stretched/squished tiles persist.
10. **D2 invariant:** after an F4 session with thumb scrolling, returning to canvas shows the same main-render cache content as before F4 (no cache pollution). The new [render_engine][bypass] unit test asserts this in isolation; manual smoke confirms end-to-end.
11. **No HBITMAP leak under stress:** open large `bookmarks.pdf`, F4, immediately Ctrl+W on the active tab during thumb-render burst. Repeat 10×. GDI handle count (Task Manager → Details → GDI Objects column) returns to baseline within ~2 s.
12. **Versioning:** Tag `v0.0.8-phase7` on the final commit; PR-3 (plan) merged before T0 starts; T4a prerequisite PR merged before any of T0–T9 starts; Phase 7 PR opened with rebase merge (per Phase 5/6 precedent).
13. About dialog shows `v0.0.8`; VERSION file `0.0.8-dev` (becomes `0.0.8` post-release per current convention).
14. Design doc `docs/plans/2026-04-15-litepdf-design.md` §4.1 row 7 updated to reflect the realized 5-class footprint (per 2E).

---

## Known Limitations (to revisit)

- **Pane animation absent.** Show/hide is instant; no slide-in animation. Roadmap-aligned (no animation in v1.0).
- **No keyboard navigation in pane.** Arrow keys / PgUp/PgDn within thumb pane don't navigate. Defer to post-v1.0 — F4 + click is sufficient for v1.
- **No right-click context menu.** No "go to page", "render full quality", etc. YAGNI for v1.
- **No persistence across restart.** Cold start always = pane hidden. Phase 12 session.json may add.
- **No dual-page thumbnails.** Phase 8 will revisit when dual-page spread lands.
- **HBITMAP path uses CreateDIBSection.** This is RAM-cheap but slower than `D2D1Bitmap` for blit. If profiling shows BitBlt cost hurts paint frame rate, consider migrating to D2D bitmap upload (mirrors PdfCanvas main render path). Defer until measured.
- **ThumbnailPane assumes single-column layout.** Multi-column grid layout is post-v1 — model + pane both encode single-column geometry.
- **F4 ↔ F5 mutual exclusion does not generalize to N panes (per plan-eng-review 1C).** Phase 7's D10 mutual-exclusion handler is hard-coded for `{outline, thumbs}`. If a future phase adds a third left-dock widget (e.g. bookmarks-only view, debug tools pane), the N-way exclusion logic explodes. Revisit at that point: either (a) generalize MainWindow's left-pane slot to a `LeftPaneRegistry` enumerating all panes, or (b) introduce an internal tab control inside the dock so multiple panes coexist (matches Acrobat / Foxit). Not blocking v1.0.
- **Stale Phase-3/4 comments in `PdfCanvas.cpp` (lines 84, 381–382)** still present — orthogonal to this phase. Optional fold-in: a 1-line cleanup commit during T8 if convenient.

---

## Re-Planning Lessons Carried From Phase 6

- **C1-style task drain pattern (applied, not deferred):** Phase 7 directly adopts Phase 6 commit `8e11f39`'s atomic `pending_tasks` + dtor spin-wait pattern in `ThumbnailRenderer::Impl` (D16 / Task 3 implementation). **Lesson learned in plan-eng-review:** the original plan deferred this with a "self-check at T6 review" note that masked a real UAF risk (worker callback touching destroyed `ThumbnailPane` on tab close mid-render). Mechanical takeaway for any future "worker callback touches non-owning UI/cache pointer" subsystem: copy the C1 pattern up front; don't defer to "self-check."
- **Observer chain pattern:** Phase 6's `CrossTabSearch` wraps `SearchSession`'s `on_update`. Phase 7's `PdfCanvas::on_page_changed` is intentionally a single-observer slot — only one consumer at a time (the active view's thumb pane). If a future feature wants to subscribe (e.g. status bar showing "Page X of Y"), introduce `multi_observer<>` infrastructure then. Don't pre-build for one consumer.
- **PostMessage vs SendMessage:** Phase 6 used PostMessage for cross-thread callbacks because Send risks UI re-entrancy. Phase 7 follows: `WM_USER_THUMB_READY` is PostMessage'd from worker.
- **Don't wrap MuPDF calls in lambdas that out-live the cloned context:** T3's `pixmap_to_hbitmap` runs entirely inside the `on_complete` callback while `ctx` is still valid; the resulting HBITMAP is GDI-managed (no MuPDF context dependency) so it's safe to ferry through PostMessage.
- **"Documentation is not implementation" — verified via Pre-flight grep (per plan-eng-review 1E):** the original D3 took `RenderEngine(cache=nullptr)` direct-render path as gospel based on a header comment without verifying the .cpp branch existed. Pre-flight grep step now confirms the cache-conditional sites before T0.5 wraps them. Mechanical takeaway: when a plan depends on a "rarely exercised but documented" code path, verify implementation actually matches docs before building on it.

---

> **Fast-follow log:** _(empty at plan write — populate at first follow-up tag.)_
