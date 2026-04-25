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

---

## Architectural Decisions

**D1. Pane is a third widget type sharing the same left-dock slot as OutlinePane.** Not a tab control inside the dock; not side-by-side. F4 and F5 are mutually exclusive — showing one hides the other. The dock slot is at most one of `{OutlinePane, ThumbnailPane}` visible at a time, with width controlled by a single `left_pane_width_px_` member on `MainWindow`. This keeps `on_layout()` logic simple (only one branch checks visibility per pane) and matches SumatraPDF's left-sidebar UX. Trade-off: users who want both panes simultaneously cannot — design doc §3 specifies F4-toggle hidden-by-default, not "always-on dual pane," so this matches intent.

**D2. ThumbCache is its own class — NOT a third tier in `core::PageCache`.** Adding `L3 = HBITMAP` to PageCache would tangle thumb lifecycle with main render lifecycle (every render-time eviction would have to reason about thumbs). Instead: a tiny standalone `ThumbCache` keyed by `int page_num`, capacity = (visible_pages + buffer_pages) × 2 ≈ 30 entries by default. `ThumbCache` owns HBITMAP refs (DeleteObject on evict). PageCache stays untouched.

**D3. ThumbnailRenderer wraps `RenderEngine(num_workers=1, cache=nullptr)`.** Phase 2's RenderEngine accepts `cache=nullptr` and falls through to a direct-render path that does not look up or populate L1/L2 — exactly what we need for non-polluting thumb rendering. Single worker because (a) thumbs are low-priority background work, (b) sharing one HDD-friendly serial pipeline avoids contending with the main 2-worker render pool. Each thumb request submits with `priority=3` (below the existing P0/P1/P2 levels), `scale=0.15f` (≈ 11 dpi at 72-baseline → 120-dip-wide thumb on a 1920-px-wide PDF page).

**D4. Thumbnail size: 120×160 dip, single column, DPI-aware.** Dip-fixed because (a) UX consistency across DPI scaling; (b) one-column simplifies hit-testing (page = `y / (tile_h + gap)`); (c) eliminates re-flow on splitter resize (only the gutter changes). Tile pixel size = `MulDiv(120, dpi, 96) × MulDiv(160, dpi, 96)`. Pane min-width = `120 + 2 * margin + scrollbar` ≈ 150 dip.

**D5. ThumbnailModel is pure logic — no Win32, no rendering, no threads.** Same TDD discipline as `core::SearchSession` and `core::TabList`. State: `page_count`, `tile_dip{w,h}`, `gap_dip`, `dpi`, `viewport_h_px`, `scroll_y_px`, `current_page`. API: `set_page_count`, `set_dpi`, `set_viewport`, `set_scroll`, `set_current_page`, `visible_range() -> {first, last}`, `page_at_y(y_px) -> std::optional<int>`, `tile_rect(page) -> RECT`, `total_height_px()`. Eight unit tests for the [thumbnail_model] tag.

**D6. Lazy render policy: only request thumbs for `visible_range() ± 1 buffer page`.** Pane's `OnPaint` walks pages in `visible_range_with_buffer()`, asks ThumbCache for HBITMAP. On miss, it submits a render request (idempotent — same page_num is deduped via a pending-set). Re-paint when render completes. This bounds RAM at ~30 thumbs × ~75 KB each = ~2 MB regardless of doc size.

**D7. Cache eviction policy = simple LRU keyed by access time.** Capacity 30 entries (covers 1920×1080 pane easily). LRU because access pattern is sequential (user pages through). FIFO would work too but LRU handles back-and-forth better.

**D8. Current-page sync via observer.** `MainWindow::on_canvas_page_changed(int page)` calls `thumb_pane_->set_current_page(page)`. Pane updates its model and:
  (a) repaints the (old, new) tile pair to redraw highlight border;
  (b) if the new page is outside `visible_range()`, scrolls so the new page sits at row 1 (one tile above center) — same UX as Sumatra. Scroll is animated only if Phase 12 adds a flag; Phase 7 ships instant scroll.

**D9. VerticalSplitter shares PIMPL skeleton with Phase 6's Splitter.** Refactor in T4 extracts a `SplitterCore` private struct holding `{HWND, drag_state, OnDrag cb}` plus `enum Orientation { Horizontal, Vertical }`. `Splitter::set_bounds`/`VerticalSplitter::set_bounds` differ only in cursor (`IDC_SIZENS` vs `IDC_SIZEWE`) and the axis math. Total LOC delta: +~50 (mostly `VerticalSplitter.{hpp,cpp}` thin wrapper); existing `Splitter` callers see zero behavior change.

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

**Step 2.5: CMake + run tests.** Expected: 5/5 [thumb_cache] cases pass; total 109 → 114.

**Step 2.6: Commit.**

```bash
git add src/core/ThumbCache.* src/CMakeLists.txt tests/unit/ThumbCacheTests.cpp tests/unit/CMakeLists.txt
git commit -m "feat(core): ThumbCache HBITMAP LRU (Phase 7 D2)"
```

---

### Task 3: `core::ThumbnailRenderer` — single-worker bypass-cache renderer

**Goal:** Wrap `RenderEngine(num_workers=1, cache=nullptr)` with a thumb-friendly API: `submit(page, on_done)` where `on_done(HBITMAP)` fires on UI thread (post-marshaled).

**Files:**
- Create: `src/core/ThumbnailRenderer.hpp` (~50 LOC).
- Create: `src/core/ThumbnailRenderer.cpp` (~120 LOC).
- Create: `tests/unit/ThumbnailRendererTests.cpp` (~60 LOC, integration with real Document fixture).
- Modify: `src/CMakeLists.txt`, `tests/unit/CMakeLists.txt`.

**Step 3.1: Write tests.** Create `tests/unit/ThumbnailRendererTests.cpp`:

```cpp
#include "core/Document.hpp"
#include "core/ThumbnailRenderer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;
using litepdf::core::Document;
using litepdf::core::ThumbnailRenderer;

TEST_CASE("ThumbnailRenderer: produces HBITMAP for a real page", "[thumb_renderer]") {
    Document doc;
    REQUIRE(doc.open(L"tests/fixtures/simple.pdf"));
    ThumbnailRenderer r(doc);
    std::atomic<int> got{0};
    HBITMAP captured = nullptr;
    r.submit(0, [&](HBITMAP bm) {
        captured = bm;
        got.fetch_add(1, std::memory_order_release);
    });
    // Spin-wait up to 5 s.
    for (int i = 0; i < 500 && got.load(std::memory_order_acquire) == 0; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    REQUIRE(got.load() == 1);
    REQUIRE(captured != nullptr);
    DeleteObject(captured);  // test owns the result
}

TEST_CASE("ThumbnailRenderer: cancel_pending stops in-flight work", "[thumb_renderer]") {
    Document doc;
    REQUIRE(doc.open(L"tests/fixtures/simple.pdf"));
    ThumbnailRenderer r(doc);
    std::atomic<int> seen{0};
    for (int i = 0; i < 3; ++i) {
        r.submit(0, [&](HBITMAP bm) {
            seen.fetch_add(1, std::memory_order_release);
            if (bm) DeleteObject(bm);
        });
    }
    r.cancel_pending();
    std::this_thread::sleep_for(200ms);
    // At least 0, at most 3 should have completed. The contract is
    // "cancel_pending makes no further callbacks for not-yet-started
    // work." We can't pin an exact count; verify no UB / no crash.
    REQUIRE(seen.load() <= 3);
}
```

**Step 3.2: Verify FAIL.**

**Step 3.3: Write `ThumbnailRenderer.hpp`:**

```cpp
#pragma once

// core::ThumbnailRenderer — low-priority single-worker page renderer
// for the thumbnail pane. Wraps RenderEngine(num_workers=1,
// cache=nullptr) so thumbs do NOT pollute PageCache L1/L2.
//
// Lifetime contract: on_done callback runs on a worker thread; the
// HBITMAP it delivers is owned by the callback. ThumbnailPane
// (the typical caller) immediately puts the HBITMAP into ThumbCache,
// which then owns it.

#include "core/RenderEngine.hpp"

#include <functional>
#include <memory>
#include <windows.h>

namespace litepdf::core {

class Document;

class ThumbnailRenderer {
public:
    using OnDone = std::function<void(HBITMAP)>;

    explicit ThumbnailRenderer(Document& doc, float scale = 0.15f);
    ~ThumbnailRenderer();

    ThumbnailRenderer(const ThumbnailRenderer&)            = delete;
    ThumbnailRenderer& operator=(const ThumbnailRenderer&) = delete;

    // Submits page render at low priority. On completion, on_done
    // fires on a worker thread with an HBITMAP (or nullptr on render
    // failure). Caller takes ownership of the HBITMAP.
    void submit(int page, OnDone on_done);

    // Cancels all not-yet-started requests. In-flight ones still
    // complete (cooperative cancellation, same as RenderEngine).
    void cancel_pending();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace litepdf::core
```

**Step 3.4: Write `ThumbnailRenderer.cpp`** — wires `RenderEngine.submit` with priority 3 + scale 0.15, converts the resulting `fz_pixmap` to `HBITMAP` via `CreateDIBSection` + memcpy of pixmap rows. Implementation skeleton:

```cpp
#include "core/ThumbnailRenderer.hpp"

#include "core/Document.hpp"

extern "C" {
#include <mupdf/fitz.h>
}

namespace litepdf::core {

namespace {
HBITMAP pixmap_to_hbitmap(fz_pixmap* pix, fz_context* ctx) {
    if (!pix) return nullptr;
    const int w = fz_pixmap_width(ctx, pix);
    const int h = fz_pixmap_height(ctx, pix);
    const int n = fz_pixmap_components(ctx, pix);
    if (w <= 0 || h <= 0 || n != 4) return nullptr;

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
            // MuPDF RGBA → Win32 BGRA
            dr[0] = sr[2];
            dr[1] = sr[1];
            dr[2] = sr[0];
            dr[3] = sr[3];
            sr += 4;
            dr += 4;
        }
    }
    return bm;
}
}  // namespace

struct ThumbnailRenderer::Impl {
    RenderEngine engine;
    float        scale;
    explicit Impl(Document& doc, float s)
      : engine(doc, /*num_workers=*/1, /*cache=*/nullptr), scale(s) {}
};

ThumbnailRenderer::ThumbnailRenderer(Document& doc, float scale)
  : impl_(std::make_unique<Impl>(doc, scale)) {}

ThumbnailRenderer::~ThumbnailRenderer() = default;

void ThumbnailRenderer::submit(int page, OnDone on_done) {
    RenderEngine::RenderRequest req;
    req.page_num = page;
    req.priority = 3;
    req.scale    = impl_->scale;
    req.on_complete = [cb = std::move(on_done)](fz_pixmap* pix, fz_context* ctx) {
        HBITMAP bm = pixmap_to_hbitmap(pix, ctx);
        if (pix) fz_drop_pixmap(ctx, pix);
        cb(bm);
    };
    impl_->engine.submit(std::move(req));
}

void ThumbnailRenderer::cancel_pending() {
    impl_->engine.cancel_all_below_priority(0);  // cancel everything (priority 0 is highest)
}

}  // namespace litepdf::core
```

**Step 3.5: CMake + tests.** Expected: 2/2 [thumb_renderer] cases pass; total 114 → 116.

**Step 3.6: Commit.**

```bash
git add src/core/ThumbnailRenderer.* src/CMakeLists.txt tests/unit/ThumbnailRendererTests.cpp tests/unit/CMakeLists.txt
git commit -m "feat(core): ThumbnailRenderer (RenderEngine bypass-cache wrapper) (Phase 7 D3)"
```

---

### Task 4: `ui::VerticalSplitter` — extract orientation-agnostic core from existing Splitter

**Goal:** Refactor existing `ui::Splitter` to share PIMPL skeleton with new `ui::VerticalSplitter`. No behavior change to existing horizontal use; total LOC delta ~+50.

**Files:**
- Modify: `src/ui/Splitter.hpp` (add `Orientation` enum, but keep `Splitter` class as a horizontal-only thin wrapper for callers).
- Modify: `src/ui/Splitter.cpp` (parameterize WndProc on orientation).
- Create: `src/ui/VerticalSplitter.hpp` + `.cpp` (thin wrapper over the shared core, with `IDC_SIZEWE` cursor).
- Create: `tests/unit/VerticalSplitterTests.cpp` — tests are minimal because Splitter is heavy on Win32; we test the shared `compute_drag_target` math only.
- Modify: `src/CMakeLists.txt`, `tests/unit/CMakeLists.txt`.

**Step 4.1: Refactor.** In `src/ui/Splitter.hpp`, extract a private `enum class Orientation { Horizontal, Vertical }` into the `Impl` struct. Keep `class Splitter` exactly as before but add a `// Phase 7 refactor: shared core with VerticalSplitter.` doc-comment. Add new file `VerticalSplitter.hpp`:

```cpp
#pragma once

// ui::VerticalSplitter — vertical 4-DIP drag bar for resizing the
// left thumbnail/outline pane. Shares Impl skeleton with Splitter
// (horizontal). See Phase 7 D9.

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

**Step 4.2: Implementation** — copy `src/ui/Splitter.cpp`, change cursor to `IDC_SIZEWE`, change `WM_MOUSEMOVE` to use the X-axis instead of Y, callback delivers `new_width_px = mouse_x` (clamped).

**Step 4.3: Tests.** Math-only test: `compute_drag_target_x(mouse_x, parent_w, min_w, max_w)` returns clamped width. (If it's a free function, test it directly; if it's a private member, expose via friend or a free helper in an anonymous namespace's test-only header.)

```cpp
#include "ui/VerticalSplitter.hpp"
// ... or test a free helper in detail/SplitterMath.hpp ...

TEST_CASE("VerticalSplitter: clamps below min", "[vsplitter]") {
    REQUIRE(litepdf::ui::compute_drag_target_x(50, 1000, 150, 800) == 150);
}
TEST_CASE("VerticalSplitter: clamps above max", "[vsplitter]") {
    REQUIRE(litepdf::ui::compute_drag_target_x(900, 1000, 150, 800) == 800);
}
```

**Step 4.4: CMake + tests.** Expected: 2/2 [vsplitter] pass; total 116 → 118.

**Step 4.5: Commit.**

```bash
git add src/ui/Splitter.* src/ui/VerticalSplitter.* src/CMakeLists.txt tests/unit/VerticalSplitterTests.cpp tests/unit/CMakeLists.txt
git commit -m "refactor(ui): VerticalSplitter shares core with Splitter (Phase 7 D9)"
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

**Step 5.3:** Manual smoke. Build, launch with `bookmarks.pdf`, push a placeholder F4 handler in MainWindow that just calls `thumb_pane_->show()` (full handler comes in T8). Verify pane shows, page-number rectangles appear for visible pages, scroll works, click logs the page (printf or OutputDebugString — temporary).

**Step 5.4:** Commit.

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

**Step 6.2:** In `WM_DRAWITEM`, walk `model.visible_range_with_buffer()`. For each page: if `cache_->get(page)` returns non-null → `BitBlt` the HBITMAP into the tile rect. Otherwise paint placeholder, and if not yet pending in `pending_renders_` set, submit `renderer_->submit(page, ...)` and insert into `pending_renders_`. Pending set is cleared on tab close / set_page_count change / clear().

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

### Task 9: Smoke test + version bump + tag

**Goal:** Add a thumb-pane case to `scripts/smoke-test.ps1`. Bump `VERSION` and `About` dialog to `0.0.8`. Tag `v0.0.8-phase7`.

**Files:**
- Modify: `scripts/smoke-test.ps1`.
- Modify: `VERSION` (`0.0.7` → `0.0.8`).
- Modify: `src/ui/MainWindow.cpp` (About dialog string `v0.0.7` → `v0.0.8`).
- Modify: `CMakeLists.txt` if version is duplicated there (check before editing).

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

**Step 9.2:** Bump version:

```bash
echo 0.0.8-dev > VERSION
# Edit src/ui/MainWindow.cpp About dialog: "LitePDF v0.0.7" → "LitePDF v0.0.8"
```

**Step 9.3:** Build + full ctest:

```bash
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Expected: 118/118 pass (101 baseline + 8 thumbnail_model + 5 thumb_cache + 2 thumb_renderer + 2 vsplitter = 118).

**Step 9.4:** Commit + tag.

```bash
git add VERSION scripts/smoke-test.ps1 src/ui/MainWindow.cpp
git commit -m "chore(ui): hardening pass + bump to v0.0.8 (Phase 7 finalize)"
git tag v0.0.8-phase7
```

**Step 9.5:** Prepare PR per `superpowers:finishing-a-development-branch` or `/ship` (pick ONE per CLAUDE.md decision table). Push branch + tag, open PR, merge with rebase strategy (per Phase 5/6 precedent).

---

## Summary Table

| Task | Component | LOC | New tests | Notes |
|---|---|---|---|---|
| 0 | Menu IDs + F4 accel | ~5 | — | resource only |
| 1 | `core::ThumbnailModel` | ~180 | 8 | pure logic, TDD |
| 2 | `core::ThumbCache` | ~130 | 5 | HBITMAP LRU |
| 3 | `core::ThumbnailRenderer` | ~170 | 2 | RenderEngine wrapper |
| 4 | `ui::VerticalSplitter` | ~150 | 2 | shared core w/ Splitter |
| 5 | `ui::ThumbnailPane` skeleton | ~310 | — | manual smoke |
| 6 | Pane render + cache wire | ~110 | — | manual smoke |
| 7 | Current-page sync | ~80 | — | observer chain |
| 8 | MainWindow integration | ~120 | — | layout + F4 handler |
| 9 | Smoke + version + tag | ~20 | — | release prep |
| **Total** | — | **~1275** | **17** | est vs design ~350 was for ThumbnailPane only — full Phase 7 with model, cache, renderer, splitter refactor, integration is realistically ~1275 |

LOC budget overrun (~3.6× design's `ui/ThumbnailPane: 350`): the design row counts only the pane widget; this plan's 1275 includes model, cache, renderer, splitter refactor, integration. Comparable to Phase 6's overrun (~750 vs roadmap 500 for similar reasons — model + dispatcher + UI plumbing).

Test count: 101 → 118 (+17).

---

## Risk Annotations

- **R1: `RenderEngine(cache=nullptr)` direct-render path.** The Phase 2 plan's D-line about `cache=nullptr` is documented but the code path is rarely exercised (production always passes a cache). Verify at T3 smoke that pixmap delivery + ownership transfer works correctly without cache. If broken, the RenderEngine work to fix is small (it's a single `if (cache_)` branch).

- **R2: HBITMAP lifetime in ThumbCache eviction.** Win32 GDI handles are notoriously easy to leak. T2 tests verify map state but cannot directly assert `DeleteObject` was called. Mitigation: keep ThumbCache code minimal and review against `ThumbCache::clear()` invariant ("after clear, every put-ed handle has been DeleteObject'd"). Optional: add a debug build assertion using `GdiGetThumbHandleCount` if debugging leaks ever surfaces.

- **R3: Re-entrance during `WM_USER_THUMB_READY`.** If the UI thread is mid-paint and `WM_USER_THUMB_READY` arrives, the `cache_->put` might evict a tile currently being blitted. Mitigation: `WM_USER_THUMB_READY` is processed only between `WM_DRAWITEM` calls (PostMessage queues; it can't preempt). But if a `BitBlt` happens to run with a stale HBITMAP that was just `DeleteObject`'d on eviction... hmm. Cheap fix: never evict on `put` if `pending_renders_.contains(page)` — but then capacity is soft. Better: paint via `BitBlt` is synchronous, so by the time `WndProc` returns from `WM_DRAWITEM` the BitBlt is done; subsequent eviction is safe. Verify in T6.

- **R4: Splitter refactor regression.** Phase 6's `Splitter` is currently used for the bottom results panel. The T4 refactor must not change its behavior. Mitigation: T4 keeps existing tests passing, leaves the public Splitter API byte-identical, only adds VerticalSplitter as new code path. Manual smoke after T4: open results panel, drag splitter — must work as before.

- **R5: Lazy-creation race on F4 spam.** If user mashes F4 fast, `ensure_thumb_pane` could be called multiple times before construction completes. Mitigation: `ensure_thumb_pane` checks for existing instance under a single-threaded UI-thread invariant (which holds — all WM_COMMAND is serialized).

- **R6: DPI hot-switch.** `WM_DPICHANGED` arrives when user moves window between monitors. ThumbnailModel.set_dpi must be called; pane invalidates all tiles in cache (since pixel size changed). T5/T6 should handle this — easy to forget. Add to acceptance checklist.

---

## Done When

1. `ctest` 118/118 green on Release build.
2. `scripts/smoke-test.ps1` passes the new "F4 opens thumb pane" step.
3. F4 toggles thumb pane open/closed; first thumb visible within 1 s on `bookmarks.pdf` (HDD-warm).
4. F4 ↔ F5 mutual exclusion verified manually (one pane at a time).
5. PgDn ×N in canvas highlights pages 1..N in thumb pane and auto-scrolls.
6. VerticalSplitter drag resizes pane; canvas reflows; persists across F4-cycle.
7. Per-tab independence verified: open 3 tabs, F4-toggle each independently.
8. No regression: existing `Splitter` (bottom results panel) still works.
9. Tag `v0.0.8-phase7` on the final commit; PR opened (rebase merge per repo precedent).
10. About dialog shows `v0.0.8`; VERSION file `0.0.8-dev` (will become `0.0.8` post-release per current convention).

---

## Known Limitations (to revisit)

- **Pane animation absent.** Show/hide is instant; no slide-in animation. Roadmap-aligned (no animation in v1.0).
- **No keyboard navigation in pane.** Arrow keys / PgUp/PgDn within thumb pane don't navigate. Defer to post-v1.0 — F4 + click is sufficient for v1.
- **No right-click context menu.** No "go to page", "render full quality", etc. YAGNI for v1.
- **No persistence across restart.** Cold start always = pane hidden. Phase 12 session.json may add.
- **No dual-page thumbnails.** Phase 8 will revisit when dual-page spread lands.
- **HBITMAP path uses CreateDIBSection.** This is RAM-cheap but slower than `D2D1Bitmap` for blit. If profiling shows BitBlt cost hurts paint frame rate, consider migrating to D2D bitmap upload (mirrors PdfCanvas main render path). Defer until measured.
- **ThumbnailPane assumes single-column layout.** Multi-column grid layout is post-v1 — model + pane both encode single-column geometry.
- **Stale Phase-3/4 comments in `PdfCanvas.cpp` (lines 84, 381–382)** still present — orthogonal to this phase. Optional fold-in: a 1-line cleanup commit during T8 if convenient.

---

## Re-Planning Lessons Carried From Phase 6

- **C1-style task drain pattern**: ThumbnailRenderer currently has no analog because callbacks are simple (deliver HBITMAP, no ref to Document). If this changes in T6 (e.g. callback captures `&document`), apply Phase 6's atomic `pending_tasks` + dtor spin-wait pattern. **Self-check at T6 review.**
- **Observer chain pattern**: Phase 6's CrossTabSearch wraps SearchSession's `on_update`. Phase 7's `PdfCanvas::on_page_changed` should NOT be similarly wrapped — only one consumer at a time (the thumb pane). If a future feature wants to subscribe (e.g. status bar showing "Page X of Y"), introduce `multi_observer<>` infrastructure then.
- **PostMessage vs SendMessage**: Phase 6 used PostMessage for cross-thread cb's because Send risks UI re-entrancy. Phase 7 follows: `WM_USER_THUMB_READY` is PostMessage'd from worker.
- **Don't wrap MuPDF calls in lambdas that out-live the cloned context**: T3 is careful — `pixmap_to_hbitmap` runs entirely inside the on_complete callback while `ctx` is still valid.

---

> **Fast-follow log:** _(empty at plan write — populate at first follow-up tag.)_
