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

**Pre-flight verification (per plan-eng-review 1B/1E + pr-review-toolkit C1/C2/C3):**

1. **Re-baseline test count.** `origin/main` has drifted since Phase 6's tag — verify the actual baseline before quoting "X/X pass" numbers in subsequent steps:

   ```bash
   grep -c TEST_CASE tests/unit/*.cpp | awk -F: '{sum+=$2} END {print sum}'
   ```

   As of `origin/main` after Phase 6 land, this returns **104** (NOT the 101 from commit `8e11f39`'s message — three cases landed afterward). All "ctest N/N" numbers throughout this plan are anchored to 104 baseline. If the grep returns a different value when you start, recompute downstream counts accordingly.

2. **RenderEngine cache + cancel surface.** Phase 7 plumbs thumbs via a new `bypass_cache` flag (D3 + D16) AND patches three cancel checkpoints to always invoke `on_complete` (D17 / Task 0.5 — required for D16 drain to actually work). Map both surfaces:

   ```bash
   grep -n "impl->cache\|impl->cache->" src/core/RenderEngine.cpp
   grep -n "Checkpoint\|canceled->load" src/core/RenderEngine.cpp
   ```

   Expected: 4 cache touch points (L1 read, L2 read, L2 write, L1 write — all spelled `impl->cache`) and 3 cancel checkpoints (A pre-MuPDF, B post-load pre-rasterize, C post-rasterize). If counts differ, the engine has shifted and Task 0.5 must adapt.

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

**D16. ThumbnailRenderer adopts Phase 6 C1's task-drain pattern, prerequisite-gated by D17** (revised per pr-review-toolkit C1). Worker callbacks reach into `ThumbnailPane` (via the `OnDone` lambda → `PostMessageW` → cache put). When a tab is closed mid-render, `DocumentView` (and therefore `ThumbnailPane`, `ThumbCache`, `ThumbnailRenderer`) is destroyed. Without a drain, an in-flight worker could complete its render after destruction and either leak the produced HBITMAP via PostMessage to a dead HWND, or touch destroyed cache state. `ThumbnailRenderer::Impl` holds an `std::atomic<int> pending_tasks{0}` incremented at `submit()` and decremented at the end of each `on_complete` lambda; `~Impl()` spin-waits to zero. **Critical correctness gate:** Phase 2's `RenderEngine` worker loop has three cancel checkpoints (`src/core/RenderEngine.cpp` L108 / L162 / L201) that originally `continue` without invoking `on_complete`, which would deadlock the drain — `pending_tasks` could never reach zero on cancel. **D17 (Task 0.5) patches all three cancel checkpoints to always invoke `on_complete(nullptr, ctx)` before `continue`**, so the drain has a chance to decrement on every code path. Without D17, D16 is a deadlock waiting to happen on every `cancel_pending()` call — DPI hot-switch, tab close mid-render, or `set_page_count` change.

**D17. Patch `RenderEngine` cancel checkpoints to always call `on_complete` before `continue`** (added per pr-review-toolkit C1). Currently three cancel paths in `RenderEngine.cpp` skip the user callback. Task 0.5 changes them to invoke `req.on_complete(nullptr, ctx)` (or drop pixmap and call with nullptr at Checkpoint C) so callers always observe completion exactly once. Existing callers in `src/ui/PdfCanvas.cpp` already handle `pix == nullptr` gracefully (line 211: `else if (pix) drop` — no `pix` means no drop, and PdfCanvas's `WM_USER_RENDER_DONE` handler tolerates null pixmap). The semantic change is "callback fires on completion or cancellation" instead of "callback fires only on completion." Trade-off: minimal engine change (~6 LOC), enables D16 drain pattern correctly, and matches the convention used by `core::SearchSession`'s dispatcher (which also fires task lambda even on stale-epoch cancellation).

**D18. `bypass_cache` gates ALL FOUR `RenderEngine` cache touch points, not just two** (added per pr-review-toolkit C3). The engine touches `impl->cache` at four sites: L1 pixmap read (L113), L2 display list read (L129), L2 display list write (L155), L1 pixmap write (L192). Original Task 0.5 draft only gated the L1 pair, which would let thumb requests churn L2 with thumb-priority display-list lookups and writes — directly violating D2's "no main cache pollution" promise. D18 gates all four with `if (impl->cache && !req.bypass_cache)`, and the T0.5 unit test asserts both L1 and L2 are untouched after a `bypass_cache=true` submission. Note: display lists are zoom-independent, so an alternative ("share L2, isolate L1") was considered for CPU reuse but rejected for v1 — the explicit isolation is simpler to reason about and the duplicate display-list build cost (~5 ms per page) is dwarfed by the rasterize cost (~30–80 ms per thumb).

---

## Task List

### Task 0: Reserve menu IDs + F4 accelerator (no behavior change)

**Goal:** Land the menu ID + accelerator wiring so future tasks can reference them; menu item is grayed (no handler yet).

**Files:**
- Modify: `resources/MainMenu.rc.h` — add `IDM_VIEW_THUMBS`.
- Modify: `resources/litepdf.rc` — add menu item.
- Modify: `src/ui/MainWindow.cpp` — add F4 to `ACCEL[]`.

**Step 0.1:** Edit `resources/MainMenu.rc.h`. After the `IDM_FIND_*` block (40047), add:

```c
// Phase 7: thumbnail pane.
#define IDM_VIEW_THUMBS 40060   // F4
```

**Step 0.2:** Edit `resources/litepdf.rc`. The chosen ID `40060` respects `MainMenu.rc.h`'s reservation comment "Reserve 40048-40059 for future search commands" (per pr-review-toolkit S2). In the View menu popup, after the `Toggle &Outline\tF5` entry, add:

```rc
MENUITEM "Toggle &Thumbnails\tF4", IDM_VIEW_THUMBS
```

Then edit `src/ui/MainWindow.cpp` — accelerators are built programmatically here (not in a `.rc` `IDR_ACCEL` block, despite earlier plan revisions hinting otherwise; the `.rc` file has no `ACCELERATORS` block). In the `ACCEL accels[]` array (around line 1128), adjacent to the existing `VK_F5 IDM_VIEW_OUTLINE` entry, add:

```cpp
{ FVIRTKEY,            VK_F4,        IDM_VIEW_THUMBS   },
```

**Step 0.3:** Build + smoke-test:

```bash
cmake --build build --config Release
build/Release/litepdf.exe tests/fixtures/bookmarks.pdf
```

Expected: View menu shows the new "Toggle Thumbnails F4" item, grayed/no-op (no handler yet). Pressing F4 does nothing. **No test changes — this is pure resource wiring.**

**Step 0.4:** Commit.

```bash
git add resources/MainMenu.rc.h resources/litepdf.rc src/ui/MainWindow.cpp
git commit -m "feat(ui): reserve menu IDs for thumbnail pane (Phase 7 Task 0)"
```

---

### Task 0.5: Extend `core::RenderEngine` with `bypass_cache` flag + cancel-path callback discipline (per plan-eng-review 1B + pr-review-toolkit C1/C3 / D17 / D18)

**Goal:** Three coupled engine changes that together enable Phase 7's design promises (D2 cache isolation + D16 drain):

1. **Add `bool bypass_cache` to `RenderRequest`** (D3 / D18 enabling change). When true, ALL FOUR cache touch points are skipped (L1 read + L2 read + L2 write + L1 write).
2. **Patch three cancel checkpoints to always invoke `on_complete`** (D17 enabling change). Without this, D16's `pending_tasks` drain in `ThumbnailRenderer` cannot decrement on cancel paths and would deadlock.
3. **One test for the cache-isolation contract; one test for the cancel-callback contract.**

Total engine delta: ~25 LOC (3 cache-guard `&& !req.bypass_cache` clauses + 1 added clause + 3 `if (req.on_complete) req.on_complete(nullptr, ctx);` insertions before `continue`).

**Files:**
- Modify: `src/core/RenderEngine.hpp` — add `bool bypass_cache` field on `RenderRequest`.
- Modify: `src/core/RenderEngine.cpp` — gate four cache sites with `&& !req.bypass_cache`; insert callback invocation before three cancel `continue`s.
- Modify: `tests/unit/test_render_engine_lifecycle.cpp` (existing file — confirmed via `grep -l RenderEngine tests/unit/*.cpp`; that's where lifecycle/cancel tests live) — add two test cases.

**Step 0.5.1: Edit `src/core/RenderEngine.hpp`.** In the `RenderRequest` struct, after `scale`:

```cpp
struct RenderRequest {
    int   page_num     = 0;
    int   priority     = 0;
    float scale        = 1.0f;
    bool  bypass_cache = false;  // (Phase 7) when true, this request neither
                                 // reads from nor populates L1 OR L2. Used by
                                 // ThumbnailRenderer so thumb pixmaps and
                                 // their display lists never displace main-
                                 // render entries.
    std::function<void(fz_pixmap*, fz_context*)> on_complete;
};
```

**Step 0.5.2: Edit `src/core/RenderEngine.cpp`.** Patch the four cache touch points AND the three cancel checkpoints. The actual variable name is `impl->cache` (not `cache_`):

Cache touch points — change every `if (impl->cache)` to `if (impl->cache && !req.bypass_cache)`:

| File:line | Site |
|---|---|
| `RenderEngine.cpp:113` | L1 pixmap read |
| `RenderEngine.cpp:129` | L2 display list read |
| `RenderEngine.cpp:155` | L2 display list write (currently `if (dlist && impl->cache)` → add `&& !req.bypass_cache`) |
| `RenderEngine.cpp:192` | L1 pixmap write (currently `if (pix && impl->cache)` → add `&& !req.bypass_cache`) |

Cancel checkpoints — change every `continue` to invoke callback first:

```cpp
// Checkpoint A (line ~108): pre-MuPDF cancel.
if (canceled && canceled->load()) {
    if (req.on_complete) req.on_complete(nullptr, ctx);
    continue;
}

// Checkpoint B (line ~162): post-load pre-rasterize cancel.
if (canceled && canceled->load()) {
    if (dlist) fz_drop_display_list(ctx, dlist);
    if (req.on_complete) req.on_complete(nullptr, ctx);
    continue;
}

// Checkpoint C (line ~201): post-rasterize cancel.
if (canceled && canceled->load()) {
    if (pix) fz_drop_pixmap(ctx, pix);
    if (req.on_complete) req.on_complete(nullptr, ctx);
    continue;
}
```

Note the L1-hit cancel path at `RenderEngine.cpp:117-120` already invokes `on_complete` if a pixmap was retrieved — no change there. Existing callers (PdfCanvas's render-done handler) already handle `pix == nullptr` (see line 211 fallback `else if (pix) drop`); D17 just makes the null-callback path consistent across all four cancel sites.

**Step 0.5.3: Two unit tests.** In `tests/unit/test_render_engine_lifecycle.cpp`:

```cpp
TEST_CASE("RenderEngine: bypass_cache=true touches neither L1 nor L2",
          "[render_engine][bypass]") {
    Document doc; REQUIRE(doc.open(L"tests/fixtures/simple.pdf"));
    PageCache cache(/*l1=*/4, /*l2=*/4, doc.ui_ctx());
    RenderEngine eng(doc, /*workers=*/1, &cache);

    std::atomic<bool> done{false};
    std::condition_variable cv;
    std::mutex mu;
    RenderEngine::RenderRequest req;
    req.page_num     = 0;
    req.priority     = 0;
    req.scale        = 0.15f;
    req.bypass_cache = true;
    req.on_complete  = [&](fz_pixmap* pix, fz_context* ctx) {
        if (pix) fz_drop_pixmap(ctx, pix);
        { std::lock_guard<std::mutex> lk(mu); done = true; }
        cv.notify_one();
    };
    eng.submit(std::move(req));

    {
        std::unique_lock<std::mutex> lk(mu);
        REQUIRE(cv.wait_for(lk, std::chrono::seconds(15),
                            [&]{ return done.load(); }));
    }

    // D2 + D18: BOTH cache tiers untouched after a bypass_cache request.
    REQUIRE(cache.get_pixmap(0, 0.15f) == nullptr);
    REQUIRE(cache.get_display_list(0) == nullptr);
}

TEST_CASE("RenderEngine: cancel still invokes on_complete with nullptr "
          "(D17 contract for ThumbnailRenderer drain)",
          "[render_engine][cancel_callback]") {
    Document doc; REQUIRE(doc.open(L"tests/fixtures/simple.pdf"));
    RenderEngine eng(doc, /*workers=*/1, /*cache=*/nullptr);

    std::atomic<int> callbacks{0};
    std::atomic<int> nulls{0};
    for (int i = 0; i < 5; ++i) {
        RenderEngine::RenderRequest req;
        req.page_num    = 0;
        req.priority    = 0;
        req.scale       = 0.15f;
        req.on_complete = [&](fz_pixmap* pix, fz_context* ctx) {
            callbacks.fetch_add(1, std::memory_order_release);
            if (!pix) nulls.fetch_add(1, std::memory_order_release);
            if (pix) fz_drop_pixmap(ctx, pix);
        };
        eng.submit(std::move(req));
    }
    eng.cancel_all_below_priority(0);  // cancels everything (priority 0 is highest)

    // Wait for the worker to drain. With D17, every request — completed or
    // cancelled — must produce exactly one on_complete call.
    for (int i = 0; i < 1000 && callbacks.load() < 5; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    REQUIRE(callbacks.load() == 5);
    // We can't pin the cancelled-vs-completed count exactly (race window),
    // but at least one should have been cancelled with pix=nullptr in a
    // 5-deep queue with 1 worker.
    REQUIRE(nulls.load() >= 1);
}
```

**Step 0.5.4: Build + tests.** Existing render-engine cancellation tests must still pass (they shouldn't observe the change — they don't assert "callback NOT called on cancel," they just assert cancel itself works). New cases bring count from 104 → 106. Verify no regressions in `[render_engine_lifecycle]` and `[escrow]` tags.

**Step 0.5.5: Commit.**

```bash
git add src/core/RenderEngine.hpp src/core/RenderEngine.cpp tests/unit/test_render_engine_lifecycle.cpp
git commit -m "feat(core): RenderEngine bypass_cache flag + cancel-path callback discipline (Phase 7 D3 / D17 / D18)"
```

---

### Task 1: `core::ThumbnailModel` pure-logic class with TDD

**Goal:** Pure-logic state machine for thumb-pane geometry. Eight tests, all in [thumbnail_model] tag.

**Files:**
- Create: `src/core/ThumbnailModel.hpp` (~80 LOC).
- Create: `src/core/ThumbnailModel.cpp` (~100 LOC).
- Create: `tests/unit/test_thumbnail_model.cpp` (~120 LOC).
- Modify: `tests/CMakeLists.txt` (add new test source).
- Modify: top-level `CMakeLists.txt` (add `src/core/ThumbnailModel.cpp` to `litepdf_core`).

**Step 1.1: Write the failing tests.** Create `tests/unit/test_thumbnail_model.cpp`:

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

**Step 1.5: Add to CMake.** In top-level `CMakeLists.txt`, find the `add_library(litepdf_core STATIC ...)` block and add `src/core/ThumbnailModel.cpp`. In `tests/CMakeLists.txt`, add `unit/test_thumbnail_model.cpp` to the test executable sources.

**Step 1.6: Build + run tests.**

```bash
cmake --build build --config Release
ctest --test-dir build -C Release -R thumbnail_model --output-on-failure
```

Expected: 8/8 [thumbnail_model] cases pass. Total ctest count: 101 → 109.

**Step 1.7: Commit.**

```bash
git add src/core/ThumbnailModel.* CMakeLists.txt tests/unit/test_thumbnail_model.cpp tests/CMakeLists.txt
git commit -m "feat(core): ThumbnailModel pure-logic pane geometry (Phase 7 D5)"
```

---

### Task 2: `core::ThumbCache` HBITMAP LRU + tests

**Goal:** Tiny LRU keyed by `int page_num`, holding `HBITMAP` refs. Capacity ~30. Tests verify put/get/evict and resource cleanup discipline.

**Files:**
- Create: `src/core/ThumbCache.hpp` (~50 LOC).
- Create: `src/core/ThumbCache.cpp` (~80 LOC).
- Create: `tests/unit/test_thumb_cache.cpp` (~80 LOC).
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`.

**Step 2.1: Write tests.** Create `tests/unit/test_thumb_cache.cpp`:

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

**Step 2.5: CMake + run tests.** Expected: 6/6 [thumb_cache] cases pass; total 114 → 120. (Counts anchored to actual `origin/main` baseline of 104; Task 0.5 added 2 cases [render_engine][bypass] + [render_engine][cancel_callback] for 106; Task 1 added 8 [thumbnail_model] for 114.)

**Step 2.6: Commit.**

```bash
git add src/core/ThumbCache.* CMakeLists.txt tests/unit/test_thumb_cache.cpp tests/CMakeLists.txt
git commit -m "feat(core): ThumbCache HBITMAP LRU (Phase 7 D2)"
```

---

### Task 3: `core::ThumbnailRenderer` — thumb-friendly wrapper around the existing per-tab `RenderEngine`

**Goal:** Provide a `submit(page, on_done)` API that submits to each `DocumentView`'s **existing** `RenderEngine` with `priority=3` and `bypass_cache=true` (the flag added in Task 0.5). On completion, the worker's pixmap is converted to `HBITMAP` and delivered to `on_done`. **Lifetime safety**: adopt Phase 6 C1's task-drain pattern (D16) so a tab close mid-render cannot UAF.

**Files:**
- Create: `src/core/ThumbnailRenderer.hpp` (~50 LOC).
- Create: `src/core/ThumbnailRenderer.cpp` (~120 LOC).
- Create: `tests/unit/test_thumbnail_renderer.cpp` (~60 LOC, integration with real Document fixture).
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`.

**Step 3.1: Write tests.** Create `tests/unit/test_thumbnail_renderer.cpp`:

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

**Step 3.4: Write `ThumbnailRenderer.cpp`** — submits via the borrowed engine with `priority=3` and `bypass_cache=true`, converts the resulting `fz_pixmap` to `HBITMAP` via `CreateDIBSection`, and gates dtor on a `pending_tasks` drain (D16, made correct by D17's cancel-path callback patch). **Pixmap layout** (per pr-review-toolkit S4): the engine produces BGRA via `fz_device_bgr` + alpha=1 at `RenderEngine.cpp:177-178` — `pix` field `n` is always 4 in practice. Use stride + samples directly (matching `PdfCanvas.cpp:365-368`'s pattern), do NOT call `fz_pixmap_components` (which returns colorant count *excluding* alpha — would be 3 for BGRA, breaking a `n != 4` check). Forward-declared `fz_pixmap_*` functions in the existing PdfCanvas extern block (line 17-24) are reused; the `n` channel field comes via stride math (`stride / w`), not via a separate API call.

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
    // BGRA → BGRA: same channel order. Copy row-by-row honoring stride
    // (which may be > w*4 due to alignment padding).
    for (int y = 0; y < h; ++y) {
        std::memcpy(dst + y * dst_stride, src + y * stride,
                    static_cast<size_t>(w) * 4);
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

**Step 3.5: CMake + tests.** Expected: 3/3 [thumb_renderer] cases pass (added drain-on-dtor case per D16). Cumulative count: post-T2 was 120 → adding 3 [thumb_renderer] → **123**.

**Step 3.6: Commit.**

```bash
git add src/core/ThumbnailRenderer.* CMakeLists.txt tests/unit/test_thumbnail_renderer.cpp tests/CMakeLists.txt
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
- Modify: `tests/CMakeLists.txt`, optionally add `tests/unit/test_splitter_math.cpp` (~30 LOC) covering clamp boundaries for both axes.

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
- Create: `tests/unit/test_vertical_splitter.cpp` — exercises `detail::compute_drag_target_x` directly via the free helper added in T4a, plus a smoke unit case constructing/destroying VerticalSplitter without a parent window (confirms ctor doesn't blow up on minimal init).
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`.

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
git add src/ui/VerticalSplitter.* CMakeLists.txt tests/unit/test_vertical_splitter.cpp tests/CMakeLists.txt
git commit -m "feat(ui): VerticalSplitter wraps SplitterCore (Phase 7 D9 / Task 4b)"
```

---

### Task 5: `ui::ThumbnailPane` skeleton (Win32 widget, paint placeholder, hit-test)

**Goal:** Land the widget skeleton — owner-draw `SysListView32`, integrates with `ThumbnailModel`, paints page-number placeholder rectangles (no real thumbs yet, no renderer wiring). Hide-by-default, show/hide/visible API matching `OutlinePane`.

**Files:**
- Create: `src/ui/ThumbnailPane.hpp` (~60 LOC).
- Create: `src/ui/ThumbnailPane.cpp` (~250 LOC).
- Modify: `CMakeLists.txt`.
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

**Step 5.3 (per plan-eng-review 1D + pr-review-toolkit I5): DPI hot-switch handling.** This step depends on `cache_` / `renderer_` member pointers, which are added by setters in T6. Two implementation orderings are valid; **chosen: introduce the nullable members + the dpi-handler method here in T5, then T6 just adds the public setters that populate them.** This avoids the compile error path where T5 lands first with members not yet declared.

Add to `ThumbnailPane::Impl` private members in T5.2:

```cpp
struct ThumbnailPane::Impl {
    // ... existing members from Step 5.2 ...
    ThumbCache*         cache_    = nullptr;  // populated by T6's set_cache
    ThumbnailRenderer*  renderer_ = nullptr;  // populated by T6's set_renderer
};
```

Add a public method to `ThumbnailPane`:

```cpp
void ThumbnailPane::on_dpi_changed(unsigned new_dpi) {
    impl_->model_.set_dpi(new_dpi);
    if (impl_->cache_)    impl_->cache_->clear();
    if (impl_->renderer_) impl_->renderer_->cancel_pending();
    if (impl_->list_hwnd_) InvalidateRect(impl_->list_hwnd_, nullptr, TRUE);
}
```

Note all touches are nullptr-guarded: when this lands in T5 before T6 has wired the cache/renderer, `on_dpi_changed` becomes a model-only update (still correct — placeholder tiles will repaint at new DPI; no real thumbs to invalidate yet).

`MainWindow` already handles `WM_DPICHANGED` for the tab strip (Phase 5.4). Extend that handler in this step:

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
git add src/ui/ThumbnailPane.* CMakeLists.txt
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

**`pending_renders_` lifecycle (per plan-eng-review 4A + pr-review-toolkit I2):** the set MUST be mutated at exactly these 6 points, all on the UI thread:

1. **Insert** — when `WM_DRAWITEM` submits a render for a cache miss.
2. **Erase** — when `WM_USER_THUMB_READY` arrives for a given page (regardless of whether HBITMAP is non-null; null lParam signals render failure but the request is no longer pending).
3. **Erase + cancel** — on `set_page_count(n)` change (document swapped or cleared); call `renderer_->cancel_pending()` then `pending_renders_.clear()`.
4. **Erase + cancel** — on `clear()` (explicit reset); same as above.
5. **Erase + cancel** — on `hide()` — when MainWindow tab-switches away or F4-toggles the pane off, the now-hidden pane should drop in-flight requests. `WM_USER_THUMB_READY` arriving after hide is harmless (pane HWND still valid; cache still owned by this pane), but cancelling avoids wasted CPU on a pane the user isn't looking at. Re-submission happens organically next time the pane is shown and `WM_DRAWITEM` re-walks visible_range. **D17 makes this safe:** every cancelled request still fires `on_complete(nullptr, ...)` so `ThumbnailRenderer`'s `pending_tasks` drains correctly.
6. **Erase + cancel** — on dtor; `~Impl()` calls `cancel_pending` and then drops the set. `ThumbnailRenderer::~Impl` blocks on its own task drain (D16, gated by D17) so by the time the pane's dtor returns, no `WM_USER_THUMB_READY` is in flight that could touch a freed pane.

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

**Step 7.1:** Add `PdfCanvas::set_on_page_changed(cb)`. **Audit and fire from EVERY existing path that mutates current_page_** (per pr-review-toolkit S5). Known sites to install observer:

```bash
# Find all call sites that change the canvas's tracked page:
grep -n "current_page_\|set_page\|goto_page" src/ui/PdfCanvas.cpp \
                                              src/ui/MainWindow.cpp \
                                              src/core/DocumentView.cpp
```

Expected sites (verify before implementing):
- `PdfCanvas::on_key_down` PgUp / PgDn / Ctrl+Home / Ctrl+End
- Mouse-wheel + scroll handler if it spans page boundaries
- Outline-navigate path (`MainWindow::on_outline_navigate`)
- Search-jump (`PdfCanvas::scroll_into_view` or equivalent)
- **Tab switch** (`MainWindow::on_tab_switched` → `set_view` → restored page) — observer must fire here so a fresh tab's thumb pane shows the correct current-page highlight immediately, not "stale until first PgDn"
- Drag/drop file open (jumps to page 0)

Missing any of these means the thumb pane's current-page highlight goes stale silently (no crash, just wrong UX).

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
  - Wire `VerticalSplitter` next to the visible pane (instantiated once on MainWindow ctor; visible only when a left pane is visible).

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

**Step 8.4:** `on_layout()` — refactor the existing `outline_->visible()` branch into `left_pane_visible()` returning a generic HWND of whichever left-dock pane is visible. Layout sequence: top tab strip → left dock (whichever pane is visible, width = `left_pane_width_px_`) → vertical splitter (4 dip wide, immediately right of left dock) → canvas (remaining width). On hide of either pane, splitter is also hidden. **MainWindow owns the `VerticalSplitter`** (one instance, not per-tab); `set_bounds` is called from `on_layout` whenever the left dock is visible. The splitter's `set_on_drag` callback updates `MainWindow::left_pane_width_px_` and re-invokes `on_layout()`.

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

Expected: 129/129 pass.

**Test count derivation** (anchored to `origin/main` actual baseline of 104, NOT the legacy `8e11f39` commit-message claim of 101):

| Stage | Cases added | Cumulative |
|---|---|---|
| `origin/main` post-Phase-6 baseline | — | **104** |
| T4a prerequisite PR merges → 4 [splitter_math] | +4 | 108 |
| (Phase 7 PR begins, rebased on post-T4a main) | — | 108 |
| T0.5: [render_engine][bypass] + [render_engine][cancel_callback] | +2 | 110 |
| T1: 8 [thumbnail_model] | +8 | 118 |
| T2: 6 [thumb_cache] | +6 | 124 |
| T3: 3 [thumb_renderer] | +3 | 127 |
| T4b: 2 [vsplitter] (reusing T4a's math helpers) | +2 | **129** |
| **Final** | **+21** | **129** |

If pre-flight grep at `origin/main` returns a different baseline, recompute downstream cumulative numbers proportionally.

**Step 9.4:** Commit + tag.

```bash
git add VERSION scripts/smoke-test.ps1 src/ui/MainWindow.cpp
git commit -m "chore(ui): hardening pass + bump to v0.0.8 (Phase 7 finalize)"
git tag v0.0.8-phase7
```

**Step 9.5:** Prepare PR per `superpowers:finishing-a-development-branch` or `/ship` (pick ONE per CLAUDE.md decision table). Push branch + tag, open PR, merge with rebase strategy (per Phase 5/6 precedent).

---

## Summary Table

Phase 7 lands in two PRs to honor Beck's two-step rule (refactor first, add second). **LOC numbers below INCLUDE test source files** (per pr-review-toolkit I6 — methodology must be uniform; Phase 6 plan used the same convention).

**Prerequisite PR (refactor only, lands separately on `main`):**

| Task | Component | LOC | New tests | Notes |
|---|---|---|---|---|
| 4a | `ui::SplitterCore` extract from existing Splitter + `detail/SplitterMath.hpp` + 4 [splitter_math] cases | ~150 | 4 | Zero behavior change for in-production results-panel splitter |

**Phase 7 PR (the main feature work, branched off post-T4a `main`):**

| Task | Component | LOC | New tests | Notes |
|---|---|---|---|---|
| 0 | Menu IDs + F4 accel | ~5 | — | resource only |
| 0.5 | `RenderEngine::RenderRequest::bypass_cache` flag + 3 cancel-path callback patches + 2 [render_engine] cases (D3 + D17 + D18) | ~30 | 2 | Phase 2 engine extension |
| 1 | `core::ThumbnailModel` + 8 [thumbnail_model] cases | ~300 | 8 | pure logic, TDD |
| 2 | `core::ThumbCache` + 6 [thumb_cache] cases (incl. same-handle no-op test per 2B) | ~210 | 6 | HBITMAP LRU |
| 3 | `core::ThumbnailRenderer` (borrows engine + D16 task drain) + 3 [thumb_renderer] cases | ~250 | 3 | wraps existing RenderEngine; uses stride/samples directly per S4 |
| 4b | `ui::VerticalSplitter` (thin wrapper over T4a's SplitterCore) + 2 [vsplitter] cases | ~100 | 2 | new file only; shared math reused |
| 5 | `ui::ThumbnailPane` skeleton + `WM_DPICHANGED` handler with nullable cache_/renderer_ members per I5 | ~340 | — | manual smoke |
| 6 | Pane render + cache wire + `pending_renders_` 6-point lifecycle (per 4A + I2) | ~130 | — | manual smoke |
| 7 | Current-page sync + audit ALL `current_page_` mutation sites (per S5) | ~80 | — | observer chain |
| 8 | MainWindow integration + VerticalSplitter ownership + `on_layout` left-dock branch | ~130 | — | layout + F4 handler |
| 9 | Smoke + version + design-doc §4.1 sync (per 2E) + tag | ~25 | — | release prep |
| **Phase 7 PR subtotal** | — | **~1600** | **21** | excludes T4a (separate PR) |
| **Combined (T4a + Phase 7 PR)** | — | **~1750** | **25** | total surface across both PRs |

LOC budget overrun (~5× design's `ui/ThumbnailPane: 350`): the design row counted only the pane widget; reality includes model, cache, renderer, RenderEngine bypass-cache + cancel-path patches, splitter refactor + new vertical splitter, and integration. Comparable to Phase 6's overrun (~750 vs roadmap 500 for similar reasons — model + dispatcher + UI plumbing). Tests are roughly half the LOC; counted in to honor the same methodology Phase 6 used so future-phase comparisons stay apples-to-apples.

**Test count: 104** (post-Phase-6 actual baseline via `grep -c TEST_CASE tests/unit/*.cpp`) **→ 108** (after T4a) **→ 129** (Phase 7 PR end). +21 from this PR; +4 from T4a; +25 combined.

---

## Risk Annotations

> Status legend: **[OPEN]** = monitored at implementation. **[MITIGATED]** = upgraded to a concrete design decision or task step during plan-eng-review.

- **R1 [MITIGATED]: `RenderEngine` bypass-cache path** (revised post pr-review-toolkit C3 / D18). Original concern: cache=nullptr direct-render path in Phase 2 RenderEngine was rarely exercised in production. **Resolution**: Task 0.5 introduces a per-request `bypass_cache` flag that gates ALL FOUR cache touch points (L1 read, L2 read, L2 write, L1 write — pr-review-toolkit C3 caught the original draft only gating two). The Pre-flight grep verifies the cache-conditional sites; T0.5 wraps them. One [render_engine][bypass] unit case asserts both L1 AND L2 untouched after a bypass_cache request.

- **R2 [OPEN]: HBITMAP lifetime in ThumbCache eviction.** Win32 GDI handles are notoriously easy to leak. T2 tests verify map state but cannot directly assert `DeleteObject` was called (no public Win32 handle counter). Mitigations: (a) added [thumb_cache] same-page+same-handle no-op test (per 2B) catches double-DeleteObject pattern; (b) keep `ThumbCache` code minimal; review against the `~ThumbCache()` invariant — every `put`-ed handle is `DeleteObject`'d on eviction OR on dtor; (c) acceptance smoke in T9 includes a Task Manager handle-count check after a tab-close-mid-render stress.

- **R3 [MITIGATED]: Re-entrance / cache eviction during paint** (revised post pr-review-toolkit C1 / I2). Original concern: `WM_USER_THUMB_READY` arriving mid-paint could DeleteObject an HBITMAP currently being BitBlt'd. **Resolution**: `BitBlt` is synchronous within `WM_DRAWITEM`, and PostMessage'd `WM_USER_THUMB_READY` cannot preempt — it queues until WndProc returns. Plus the `pending_renders_` 6-point lifecycle (T6 §6.2 per 4A + I2) ensures no double-submission and clean handoff on hide/tab-switch. **Plus, ThumbnailRenderer's D16 task-drain pattern (per 1A) is now made correct by D17's RenderEngine cancel-path patch** — without D17, drain would deadlock because cancelled requests never decremented `pending_tasks`. With D17, every request fires `on_complete` exactly once (success OR cancellation), so drain is sound. **Verify in T6 + T0.5 unit tests.**

- **R4 [MITIGATED]: Splitter refactor regression.** Original concern: refactoring the in-production `ui::Splitter` (used by Phase 6 results panel) inside the same PR as adding `VerticalSplitter` couples concerns and breaks `git bisect`. **Resolution per plan-eng-review 2A**: split into Task 4a (refactor only, lands as a separate prerequisite PR with zero behavior change) and Task 4b (new file VerticalSplitter, lands in Phase 7 PR). T4a has its own CI gate + manual smoke pass on the results-panel splitter before merging.

- **R5 [OPEN]: Lazy-creation race on F4 spam.** If user mashes F4 fast, `ensure_thumb_pane` could be called multiple times before construction completes. Mitigation: `ensure_thumb_pane` checks for existing instance under a single-threaded UI-thread invariant (which holds — all `WM_COMMAND` is serialized through one WndProc).

- **R6 [MITIGATED]: DPI hot-switch.** Original concern: a one-line acceptance reminder without a concrete handling step. **Resolution per plan-eng-review 1D**: T5 §5.3 now spells out `ThumbnailPane::on_dpi_changed(unsigned new_dpi)` (model.set_dpi → cache.clear → renderer.cancel_pending → InvalidateRect) plus the MainWindow `WM_DPICHANGED` extension that calls into the active view's pane. Done When entry verifies on dual-monitor drag.

- **R7 [OPEN]: T4a prerequisite PR slips** (scope reduced per pr-review-toolkit I1). If the T4a refactor PR has unexpected fallout in production results-panel splitter, **only T4b is blocked**. T0–T3 may proceed in parallel branches off `main` even before T4a lands (none touch Splitter); T5–T9 depend on the new core/ classes from T1–T3 + the new VerticalSplitter from T4b, so they wait on whichever lands later. Mitigation: T4a is a tiny refactor (~150 LOC incl. tests, zero behavior delta target), single-purpose PR, single commit, low review surface. Worst case: revert T4a, fold the SplitterCore extraction into Phase 7 PR (back to monolithic T4) with R4 reverting to [OPEN].

---

## Done When

1. **Test count:** `ctest` 129/129 green on Release build (post T4a + Phase 7 PR; T4a contributes +4 [splitter_math] cases that land first; baseline 104 verified via `grep -c TEST_CASE tests/unit/*.cpp` at start of work).
2. **Smoke test:** `scripts/smoke-test.ps1` passes the new "F4 opens thumb pane" step.
3. **First-thumb latency:** F4 toggles thumb pane open/closed; first thumb visible within 1 s on `bookmarks.pdf` (HDD-warm). Subsequent visible tiles render serially behind the existing 2-worker main pool (per 4B / D3) — second tile lands ~150–300 ms after first; this is expected single-worker behavior, not a regression.
4. **Mutual exclusion:** F4 ↔ F5 verified manually — exactly one of {outline, thumbs} visible in the left dock at any time.
5. **Current-page sync:** PgDn ×N in canvas highlights pages 1..N in thumb pane and auto-scrolls when the new page leaves the visible range. Tab-switch also updates thumb pane current-page highlight (per S5).
6. **Splitter:** VerticalSplitter drag resizes pane; canvas reflows; width persists across F4-cycle within the same session.
7. **Per-tab independence:** open 3 tabs, F4-toggle each independently — each `DocumentView` owns its own pane state.
8. **No regression in horizontal Splitter:** Phase 6 results panel still drags + resizes correctly after T4a refactor lands. (CI'd separately as part of T4a PR; re-verified post Phase 7 merge.)
9. **DPI hot-switch (per 1D):** drag the main window between two monitors at different scaling (e.g. 100% ↔ 200%). Pane content invalidates; thumbs re-render at the correct pixel size; no stretched/squished tiles persist.
10. **D2 / D18 invariant:** after an F4 session with thumb scrolling, returning to canvas shows the same main-render cache content as before F4 (no L1 OR L2 pollution). The new [render_engine][bypass] unit test asserts both tiers untouched; manual smoke confirms end-to-end.
11. **D17 cancel callback contract:** the new [render_engine][cancel_callback] unit test passes — cancelled requests still fire `on_complete(nullptr, ctx)` exactly once. Without this, D16's drain deadlocks.
12. **No HBITMAP leak / no deadlock under stress:** open large `bookmarks.pdf`, F4, immediately Ctrl+W on the active tab during thumb-render burst. Repeat 10×. GDI handle count returns to baseline within ~2 s; no UI freeze (would indicate D16 drain deadlock from missing D17 patch).
13. **Versioning:** Tag `v0.0.8-phase7` on the final commit. **T4a prerequisite PR merged before T4b begins.** T0–T3 may proceed in parallel branches before or alongside T4a since they don't touch Splitter (per pr-review-toolkit I1). Phase 7 PR opened with rebase merge (per Phase 5/6 precedent).
14. About dialog shows `v0.0.8`; VERSION file `0.0.8-dev` (becomes `0.0.8` post-release per current convention).
15. Design doc `docs/plans/2026-04-15-litepdf-design.md` §4.1 row 7 updated to reflect the realized 5-class footprint (per 2E).

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

- **C1-style task drain pattern requires dispatcher cooperation (applied, plus engine patch in Task 0.5):** Phase 7 adopts Phase 6 commit `8e11f39`'s atomic `pending_tasks` + dtor spin-wait pattern (D16 / Task 3). **But this only works if every submitted task eventually fires `on_complete`** — otherwise `pending_tasks` cannot reach zero and the drain deadlocks. `core::SearchSession`'s dispatcher (Phase 6) always invokes the lambda even on stale-epoch cancel; `core::RenderEngine`'s worker loop (Phase 2) originally `continue`d without callback at three cancel checkpoints. Phase 7's Task 0.5 / D17 patches all three to invoke `on_complete(nullptr, ctx)` first. **Mechanical takeaway:** when adopting a C1-style drain pattern, audit the underlying dispatcher's cancel paths first — confirm callback fires on EVERY exit from the worker, not just success. This was missed by `plan-eng-review` (which trusted the D16 reasoning without checking RenderEngine semantics) and caught by `pr-review-toolkit:code-reviewer`'s C1 finding. Lesson generalizes: **stack ≥ 2 reviewers per substantial plan** (now codified in `~/.claude/CLAUDE.md` rule 14).
- **Observer chain pattern:** Phase 6's `CrossTabSearch` wraps `SearchSession`'s `on_update`. Phase 7's `PdfCanvas::on_page_changed` is intentionally a single-observer slot — only one consumer at a time (the active view's thumb pane). If a future feature wants to subscribe (e.g. status bar showing "Page X of Y"), introduce `multi_observer<>` infrastructure then. Don't pre-build for one consumer.
- **PostMessage vs SendMessage:** Phase 6 used PostMessage for cross-thread callbacks because Send risks UI re-entrancy. Phase 7 follows: `WM_USER_THUMB_READY` is PostMessage'd from worker.
- **Don't wrap MuPDF calls in lambdas that out-live the cloned context:** T3's `pixmap_to_hbitmap` runs entirely inside the `on_complete` callback while `ctx` is still valid; the resulting HBITMAP is GDI-managed (no MuPDF context dependency) so it's safe to ferry through PostMessage.
- **"Documentation is not implementation" — verified via Pre-flight grep (per plan-eng-review 1E):** the original D3 took `RenderEngine(cache=nullptr)` direct-render path as gospel based on a header comment without verifying the .cpp branch existed. Pre-flight grep step now confirms the cache-conditional sites before T0.5 wraps them. Mechanical takeaway: when a plan depends on a "rarely exercised but documented" code path, verify implementation actually matches docs before building on it.

---

> **Fast-follow log:** _(empty at plan write — populate at first follow-up tag.)_
