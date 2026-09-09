# Completion Lifecycle, Page Anchors and Wheel Scrolling (PR-A2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make an asynchronous render completion say what it is for, replace the
unconditional pan reset with a per-submission page anchor, and give the mouse wheel
in-page scrolling that flips pages at the edges — then restore FitWidth as the
default zoom mode.

**Architecture:** Three pure headers carry all the new logic so it is unit-testable
headless: `ui/detail/CompletionMath.hpp` (`Slot`, `accept_completion`),
`ui/detail/PageAnchor.hpp` (`PageAnchor`, `AnchorSlot`) and
`ui/detail/ScrollMath.hpp` (`Flip`, `consume_notches`, `wheel_step_dip`,
`apply_wheel`). `PdfCanvas` gains a monotonic per-submission-batch `seq`; the
completion message carries `{escrow, epoch, page, slot, seq}`; `AnchorSlot` is
stamped with the seq at submit time and consumed only by the matching left/single
completion. The wheel path and every page-change path funnel into one new private
`PdfCanvas::navigate_to_page(int, PageAnchor)`.

**Tech Stack:** C++20, Win32, Direct2D, MuPDF (static), Catch2 v3.5.4, CMake +
Visual Studio 17 2022, x64.

---

## Global Constraints

- **Spec:** `docs/superpowers/specs/2026-09-07-zoom-correctness-page-nav-design.md`
  §3 (PR-A2). Section references below point into it.
- **Baseline:** `main` @ `8bf143d` (PR #42, PR-A1 merged). Branch from `main`.
- **Branch name:** `spec/zoom-a2-completion-lifecycle`.
- **The spec's line numbers are STALE.** They were taken at `4c2cefe`, before
  PR-A1's 13 commits. Every anchor in this plan was re-read against `8bf143d` on
  2026-09-09 and is current. If a task's line number does not match what you see,
  stop and re-locate by symbol — do not edit by line number alone.
- **Build config is Release, never Debug.** MuPDF's static libs are
  `MT_StaticRelease`; a Debug test build fails with a flood of
  `LNK2038: RuntimeLibrary mismatch`.
- **cmake/ctest are not on PATH.** Use
  `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`
  and the sibling `ctest.exe`.
- **Catch2 `TEST_CASE` names must be pure ASCII.** Non-ASCII names mangle under
  `catch_discover_tests` on Windows and produce "No test cases matched" in CI only.
- **`ctest -R` matches TEST_CASE NAMES, not Catch2 tags.** Every new `TEST_CASE`
  below carries a stable name prefix (`CompletionMath`, `PageAnchor`, `ScrollMath`)
  and every `-R` filters on that prefix.
- **A `-R` run is never the final check for a task.** It is the fast inner loop; the
  full `ctest --test-dir build -C Release` from the repo root is what closes a task.
- **Run tests from the repo root** so fixtures resolve (`catch_discover_tests`
  `WORKING_DIRECTORY` is `CMAKE_SOURCE_DIR`).
- **No `VERSION` bump in this PR.** Version bumps happen at phase boundaries only.
- **All project artifacts in English** — code, comments, commit messages, test names.
- **Green baseline, measured on this tree at `8bf143d` on 2026-09-09** (not quoted
  from memory):
  - `cmake --build build --config Release` → clean, exit 0.
  - `ctest --test-dir build -C Release` from the repo root → **257/257 passed, 0
    failed**, 13.01 s.

  Every task below states the expected new total. If a task's total does not match,
  something else broke — find it before moving on.

---

## Scope rulings made while writing this plan

Read these before Task 1. Each one is a decision a task would otherwise re-open.

**1. `cancel_all_below_priority` is NOT changed.** Spec §3.1 lists it as one of three
reasons a completion is unidentifiable today (`RenderEngine.cpp:417-425`,
`entry.priority > p`, so `cancel_stale_renders(0)` never cancels a P0). But §3.1's
single stated remedy is `RenderMeta` gaining `{page, slot, seq}` — with `seq`, two
in-flight P0s for the same page are distinguishable, which is what the section is
actually about. Changing `>` to `>=` would additionally turn
`cancel_stale_renders(INT_MAX)` (`MainWindow.cpp:594`) from a documented no-op into
"cancel everything on tab switch", and spec §7 records that no-op as **deliberately
not fixed here**. Two in-flight P0s cost wasted work, not correctness, once `seq`
lands. Leave the engine alone.

**2. The null-completion path is NOT changed.** Spec §3.1's third bullet (a
cancelled/failed render posts `LPARAM = 0` with no metadata) is answered by §3.2's
rule that null completions *leave the anchor pending*: the retry re-issues the
anchor at a new seq. No metadata is needed on the null path.

**3. `PageCache` L1's entry-count eviction is OUT of scope.** It is a follow-up
recorded in the PR-A1 handoff, not a spec §3 requirement.

**4. The search-hit overlay stays disabled in dual mode (R17).** Task 6 fixes the
`scroll_into_view` / `on_paint` clamp mismatch that R17 currently hides, but does not
enable the overlay.

**5. Spec §3.5 (Ctrl+wheel dual fix) is already done.** PR-A1 pulled it forward:
`PdfCanvas.cpp:481` already routes Ctrl+wheel through `resubmit_current_page()`.
**Do not re-implement it.** Task 8 touches the same `WM_MOUSEWHEEL` case only to add
the non-Ctrl branch.

**6. `SessionTab::zoom_mode`'s header default needs no change.** It is
`SessionZoom::FitWidth` (`src/core/SessionState.hpp:22`). The PR-A1 handoff flagged
it as "worth cleaning up" *because* FitWidth was temporarily not the app default.
Task 9 restores FitWidth as the app default, which makes the header default correct
rather than merely harmless. Verify, comment, do not change.

---

## File Structure

**New pure-logic headers** (no Win32, no Direct2D, no MuPDF — the same pattern as
`ViewportMath.hpp` and `SplitterMath.hpp`, so the logic is unit-testable without
the exe):

| file | responsibility |
|------|----------------|
| `src/ui/detail/CompletionMath.hpp` | `Slot` enum; `accept_completion` — is this pixmap still wanted? |
| `src/ui/detail/PageAnchor.hpp` | `PageAnchor` value type; `AnchorSlot` — install / stamp / consume lifetime |
| `src/ui/detail/ScrollMath.hpp` | `Flip` enum; `consume_notches`, `wheel_step_dip`, `apply_wheel` |

**New tests:** `tests/unit/test_completion_math.cpp`,
`tests/unit/test_page_anchor.cpp`, `tests/unit/test_scroll_math.cpp`.

**Modified:**

| file | what changes |
|------|--------------|
| `src/ui/PdfCanvas.hpp` | include the three new headers; `next_render_seq()`; `change_current_page(idx, anchor)`; `post_render_done*` gain `page`/`seq` |
| `src/ui/PdfCanvas.cpp` | `RenderMeta` grows; completion handler uses `accept_completion` + `AnchorSlot`; `set_view` clears `right_bitmap`; new `navigate_to_page` / `page_origin_y` / `apply_anchor`; wheel scrolling |
| `src/ui/MainWindow.cpp` | four `set_current_page` bypasses routed through `change_current_page`; submit sites pass `page`/`seq`; search paths pass a `Hit` anchor; FitWidth restore |
| `src/core/DocumentView.cpp` | `Impl::zm` default back to `FitWidth` |
| `src/core/SessionState.cpp` | `migrate_v1_to_v2` no longer forces FitPage |
| `tests/CMakeLists.txt` | three new test files |
| `CHANGELOG.md` | one Unreleased entry |

---

## Task 1: `CompletionMath.hpp` — completion identity predicate

**Files:**
- Create: `src/ui/detail/CompletionMath.hpp`
- Create: `tests/unit/test_completion_math.cpp`
- Modify: `tests/CMakeLists.txt:76` (append after `unit/test_cjk_extract_liveness.cpp`)

**Interfaces:**
- Consumes: `litepdf::ui::dual_page_compute_left` / `dual_page_compute_right` from
  `src/ui/PdfCanvasLayout.hpp` (existing, unchanged).
- Produces: `enum class litepdf::ui::Slot { Left, Right };` and
  `bool litepdf::ui::accept_completion(std::uint64_t meta_epoch, std::uint64_t
  cur_epoch, int meta_page, Slot meta_slot, int cur_page, bool dual, int
  page_count)`. Tasks 3–8 rely on both names.

**Why this is a pure function.** Spec §3.1 requires the predicate be extracted
"so it is unit-testable rather than buried in the WndProc". The failure it prevents
is not hypothetical: in dual mode the right-slot render is submitted for `left + 1`
while `current_page()` is already snapped to `left`, and both messages share one
`case` block (`PdfCanvas.cpp:499-500`). A predicate that compared every completion
against `current_page()` would drop **every** right-slot pixmap and leave the right
half of each spread as the grey placeholder forever.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_completion_math.cpp`:

```cpp
// PR-A2 Task 1: pure-logic tests for the render-completion accept predicate.
//
// The spread case is the load-bearing one: in dual mode the RIGHT slot is
// submitted for left+1 while current_page() is already snapped to left, so a
// predicate that compared every completion against current_page() would reject
// every right-slot pixmap and leave half of every spread grey.

#include "ui/detail/CompletionMath.hpp"

#include <catch2/catch_test_macros.hpp>

using litepdf::ui::accept_completion;
using litepdf::ui::Slot;

TEST_CASE("CompletionMath accepts a matching single-page completion",
          "[ui][completion]") {
    REQUIRE(accept_completion(7, 7, 4, Slot::Left, 4, false, 10));
}

TEST_CASE("CompletionMath rejects a completion from a superseded view epoch",
          "[ui][completion]") {
    REQUIRE_FALSE(accept_completion(6, 7, 4, Slot::Left, 4, false, 10));
    // Even a perfectly matching page loses to an epoch mismatch.
    REQUIRE_FALSE(accept_completion(0, 1, 0, Slot::Left, 0, false, 10));
}

TEST_CASE("CompletionMath rejects a left completion for another page",
          "[ui][completion]") {
    REQUIRE_FALSE(accept_completion(7, 7, 3, Slot::Left, 4, false, 10));
    REQUIRE_FALSE(accept_completion(7, 7, 5, Slot::Left, 4, false, 10));
}

TEST_CASE("CompletionMath rejects a right completion in single-page mode",
          "[ui][completion]") {
    REQUIRE_FALSE(accept_completion(7, 7, 4, Slot::Right, 4, false, 10));
    // Not even the page that WOULD be the spread partner is accepted.
    REQUIRE_FALSE(accept_completion(7, 7, 5, Slot::Right, 4, false, 10));
}

TEST_CASE("CompletionMath accepts the right completion for left plus one",
          "[ui][completion]") {
    // The spread-blanking regression. Pair (3,4), current_page snapped to 3.
    REQUIRE(accept_completion(7, 7, 3, Slot::Left,  3, true, 10));
    REQUIRE(accept_completion(7, 7, 4, Slot::Right, 3, true, 10));
    // And the left slot still rejects the partner page.
    REQUIRE_FALSE(accept_completion(7, 7, 4, Slot::Left, 3, true, 10));
}

TEST_CASE("CompletionMath re-snaps an unsnapped current page in dual mode",
          "[ui][completion]") {
    // Every submission path snaps current_page to the pair's LEFT before
    // submitting, so this is defence in depth rather than a live path. It
    // costs one call and makes the predicate independent of call ordering.
    // Page 4 belongs to pair (3,4): left 3, right 4.
    REQUIRE(accept_completion(7, 7, 3, Slot::Left,  4, true, 10));
    REQUIRE(accept_completion(7, 7, 4, Slot::Right, 4, true, 10));
}

TEST_CASE("CompletionMath rejects a right completion for a pair that has none",
          "[ui][completion]") {
    // Cover page: page 0 renders alone, dual_page_compute_right returns -1.
    REQUIRE(accept_completion(7, 7, 0, Slot::Left, 0, true, 10));
    REQUIRE_FALSE(accept_completion(7, 7, 1, Slot::Right, 0, true, 10));
    // Odd tail: 4-page document, pair (3,-). Left 3 exists, right does not.
    REQUIRE(accept_completion(7, 7, 3, Slot::Left, 3, true, 4));
    REQUIRE_FALSE(accept_completion(7, 7, 4, Slot::Right, 3, true, 4));
}

TEST_CASE("CompletionMath rejects everything for a document with no pages",
          "[ui][completion]") {
    REQUIRE_FALSE(accept_completion(7, 7, 0, Slot::Left,  0, false, 0));
    REQUIRE_FALSE(accept_completion(7, 7, 0, Slot::Right, 0, true,  0));
}
```

Register it — modify `tests/CMakeLists.txt`, appending after line 76
(`unit/test_cjk_extract_liveness.cpp  # cjk-system-font-loader Task 5`):

```cmake
    unit/test_completion_math.cpp      # PR-A2 Task 1
```

- [ ] **Step 2: Run test to verify it fails**

```bash
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --config Release
```

Expected: FAIL at compile — `Cannot open include file: 'ui/detail/CompletionMath.hpp'`.

- [ ] **Step 3: Write the header**

Create `src/ui/detail/CompletionMath.hpp`:

```cpp
#pragma once

// PR-A2: the render-completion accept predicate, as pure logic.
//
// A completion message carries {epoch, page, slot, seq}. `seq` decides which
// PENDING ANCHOR a completion may consume (see ui/detail/PageAnchor.hpp); this
// file decides the prior question -- whether the pixmap should be painted at
// all.
//
// Three things can make a completion unwanted:
//   epoch  the view was swapped (tab switch) after the render was submitted;
//   page   the user paged away after the render was submitted;
//   slot   a RIGHT-slot pixmap arrived while the layout is single-page.
//
// SLOT IS LOAD-BEARING. In spread mode the right-slot render is submitted for
// left+1 while current_page() has already been snapped to left, and both
// messages share one WndProc case. Comparing every completion against
// current_page() would reject EVERY right-slot pixmap, leaving the right half
// of each spread as the grey placeholder permanently -- each resubmit takes the
// same path, so it would never recover.

#include <cstdint>

#include "ui/PdfCanvasLayout.hpp"

namespace litepdf::ui {

// Which half of the two-page spread a completion belongs to. A single-page
// render is Left.
enum class Slot { Left, Right };

// True iff the pixmap described by (meta_epoch, meta_page, meta_slot) is still
// wanted by a canvas at (cur_epoch, cur_page, dual, page_count).
//
// `cur_page` is re-snapped to the pair's LEFT page in dual mode. Every
// submission path already snaps before submitting (MainWindow::kick_render,
// PdfCanvas::resubmit_current_page, PdfCanvas::apply_viewport,
// PdfCanvas::on_key_down), so the snap here is defence in depth -- it makes the
// predicate correct regardless of the order a future caller does things in.
inline bool accept_completion(std::uint64_t meta_epoch, std::uint64_t cur_epoch,
                              int meta_page, Slot meta_slot,
                              int cur_page, bool dual, int page_count) noexcept {
    if (meta_epoch != cur_epoch) return false;
    if (page_count <= 0)         return false;

    if (!dual) {
        // No right slot exists, so a right-slot pixmap has nowhere to land.
        if (meta_slot == Slot::Right) return false;
        return meta_page == cur_page;
    }

    const int left = dual_page_compute_left(cur_page, page_count);
    if (meta_slot == Slot::Left) return meta_page == left;

    const int right = dual_page_compute_right(left, page_count);
    if (right < 0) return false;   // cover page, or odd tail: no right half
    return meta_page == right;
}

}  // namespace litepdf::ui
```

- [ ] **Step 4: Run tests to verify they pass**

```bash
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --config Release
```

then

```bash
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" --test-dir build -C Release -R CompletionMath
```

Expected: 8 tests, 8 passed. Then the full suite:

```bash
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" --test-dir build -C Release
```

Expected: **265/265 passed** (257 + 8).

- [ ] **Step 5: Commit**

```bash
git add src/ui/detail/CompletionMath.hpp tests/unit/test_completion_math.cpp tests/CMakeLists.txt
git commit -m "feat(canvas): extract the render-completion accept predicate as pure logic"
```

---

## Task 2: `PageAnchor.hpp` — anchor value type and lifetime slot

**Files:**
- Create: `src/ui/detail/PageAnchor.hpp`
- Create: `tests/unit/test_page_anchor.cpp`
- Modify: `tests/CMakeLists.txt` (append after the Task 1 line)

**Interfaces:**
- Consumes: `litepdf::core::SearchSession::Hit` from `src/core/SearchSession.hpp`
  (existing; `{ std::size_t page; Document::PageHit geom; }`).
- Produces: `struct litepdf::ui::PageAnchor` with `enum class Kind { None, Top,
  Bottom, Hit }`, a `Kind kind` field, a `core::SearchSession::Hit target` field
  and static factories `none()`, `top()`, `bottom()`, `hit(const Hit&)`; and
  `struct litepdf::ui::AnchorSlot` with `install(PageAnchor)`, `stamp(uint64_t)`,
  `consume(uint64_t)`, `clear()`, `pending()`. Tasks 4–8 rely on all of them.

**Why an `AnchorSlot` and not two fields in `PdfCanvas::Impl`.** `PdfCanvas` is
exe-only — it pulls in Win32 and Direct2D, so nothing in it can be reached from the
headless test binary. Spec §5 requires anchor-lifetime tests ("an anchor issued at
seq N is not consumed by a completion at seq N−1"). Putting the lifetime rule in a
value type is what makes those tests possible, and it is the pattern this project
already uses five times (`ThumbnailPane`, `PasswordDialog`, `password_retry`,
`ViewportMath`, `ZoomMath`).

**The lifetime rule, stated once.** `(epoch, page, slot)` does **not** identify a
request: a same-page zoom or resize produces a second P0 with an identical triple,
so an older completion could consume an anchor meant for a newer render. The anchor
therefore records the `seq` of the submission batch that carried it, and only a
completion with that exact `seq` may consume it.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_page_anchor.cpp`:

```cpp
// PR-A2 Task 2: pure-logic tests for the page-anchor lifetime.
//
// The anchor answers "where should the page land when the pixmap arrives?".
// It is keyed by the submission sequence number because (epoch, page, slot)
// does not identify a request -- a same-page zoom or resize produces a second
// P0 with an identical triple, and the older completion must not steal the
// newer render's anchor.

#include "ui/detail/PageAnchor.hpp"

#include <catch2/catch_test_macros.hpp>

using litepdf::ui::AnchorSlot;
using litepdf::ui::PageAnchor;

namespace {
litepdf::core::SearchSession::Hit make_hit(std::size_t page, float top) {
    litepdf::core::SearchSession::Hit h{};
    h.page = page;
    h.geom.ul_y = top;
    h.geom.ur_y = top;
    h.geom.ll_y = top + 12.0f;
    h.geom.lr_y = top + 12.0f;
    return h;
}
}  // namespace

TEST_CASE("PageAnchor factories carry their kind", "[ui][anchor]") {
    REQUIRE(PageAnchor::none().kind   == PageAnchor::Kind::None);
    REQUIRE(PageAnchor::top().kind    == PageAnchor::Kind::Top);
    REQUIRE(PageAnchor::bottom().kind == PageAnchor::Kind::Bottom);

    const PageAnchor a = PageAnchor::hit(make_hit(4, 300.0f));
    REQUIRE(a.kind == PageAnchor::Kind::Hit);
    REQUIRE(a.target.page == 4u);
    REQUIRE(a.target.geom.ul_y == 300.0f);
}

TEST_CASE("PageAnchor slot starts empty", "[ui][anchor]") {
    AnchorSlot slot;
    REQUIRE_FALSE(slot.pending());
    // An empty slot never claims a completion, whatever seq it carries.
    REQUIRE(slot.consume(0).kind == PageAnchor::Kind::None);
    REQUIRE(slot.consume(1).kind == PageAnchor::Kind::None);
}

TEST_CASE("PageAnchor slot is consumed only by its own seq", "[ui][anchor]") {
    AnchorSlot slot;
    slot.install(PageAnchor::top());
    slot.stamp(5);
    REQUIRE(slot.pending());

    // An older completion must not consume it, and must not clear it either.
    REQUIRE(slot.consume(4).kind == PageAnchor::Kind::None);
    REQUIRE(slot.pending());

    // A newer completion (should be impossible, but must not steal it either).
    REQUIRE(slot.consume(6).kind == PageAnchor::Kind::None);
    REQUIRE(slot.pending());

    REQUIRE(slot.consume(5).kind == PageAnchor::Kind::Top);
    REQUIRE_FALSE(slot.pending());
}

TEST_CASE("PageAnchor slot is consumed exactly once", "[ui][anchor]") {
    AnchorSlot slot;
    slot.install(PageAnchor::bottom());
    slot.stamp(9);
    REQUIRE(slot.consume(9).kind == PageAnchor::Kind::Bottom);
    // The right-slot completion of the same spread carries the same seq; it
    // must not re-apply the anchor after the left slot already used it.
    REQUIRE(slot.consume(9).kind == PageAnchor::Kind::None);
}

TEST_CASE("PageAnchor stamp carries a pending intent to a new seq",
          "[ui][anchor]") {
    // A resubmit with no new anchor re-stamps whatever is pending, so the
    // intent survives a superseding render instead of being stranded on a seq
    // that will never complete.
    AnchorSlot slot;
    slot.install(PageAnchor::top());
    slot.stamp(2);
    slot.stamp(3);
    REQUIRE(slot.consume(2).kind == PageAnchor::Kind::None);
    REQUIRE(slot.consume(3).kind == PageAnchor::Kind::Top);
}

TEST_CASE("PageAnchor install replaces a pending anchor", "[ui][anchor]") {
    AnchorSlot slot;
    slot.install(PageAnchor::top());
    slot.stamp(2);
    slot.install(PageAnchor::hit(make_hit(7, 90.0f)));
    slot.stamp(3);

    const PageAnchor got = slot.consume(3);
    REQUIRE(got.kind == PageAnchor::Kind::Hit);
    REQUIRE(got.target.page == 7u);
    REQUIRE(got.target.geom.ul_y == 90.0f);
}

TEST_CASE("PageAnchor stamp on an empty slot installs nothing",
          "[ui][anchor]") {
    // Same-page re-renders (resize, DPI change, zoom, pane toggle, invert)
    // submit with no anchor. The completion must then KEEP the current pan and
    // merely re-clamp it -- which is the Kind::None branch at the call site.
    AnchorSlot slot;
    slot.stamp(11);
    REQUIRE_FALSE(slot.pending());
    REQUIRE(slot.consume(11).kind == PageAnchor::Kind::None);
}

TEST_CASE("PageAnchor clear drops a pending anchor", "[ui][anchor]") {
    // set_view clears the slot: a new view means a new epoch, and the old
    // view's anchor describes a document that is no longer on screen.
    AnchorSlot slot;
    slot.install(PageAnchor::top());
    slot.stamp(4);
    slot.clear();
    REQUIRE_FALSE(slot.pending());
    REQUIRE(slot.consume(4).kind == PageAnchor::Kind::None);
}
```

Register it in `tests/CMakeLists.txt`, after the Task 1 line:

```cmake
    unit/test_page_anchor.cpp          # PR-A2 Task 2
```

- [ ] **Step 2: Run test to verify it fails**

```bash
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --config Release
```

Expected: FAIL at compile — `Cannot open include file: 'ui/detail/PageAnchor.hpp'`.

- [ ] **Step 3: Write the header**

Create `src/ui/detail/PageAnchor.hpp`:

```cpp
#pragma once

// PR-A2: where a page should land when its pixmap arrives.
//
// Before this PR the completion handler zeroed the pan unconditionally
// (PdfCanvas.cpp, WM_USER_RENDER_DONE). That is right for a page turn and
// wrong for everything else: pan to the bottom of a page and press Zoom In and
// the view snaps back to the top; switch tabs and MainWindow's restored
// per-tab pan (MainWindow.cpp:602) is destroyed by the very next completion.
//
// An anchor names the intent instead:
//
//   Top     pan_y = 0                     PgDn / Home / End / outline click /
//                                         thumbnail click / wheel flip forward
//   Bottom  pan_y = viewport - content    wheel flip backward
//   Hit     centre the quad, 24 DIP margin  search navigation
//   None    keep the current pan, re-clamp  same-page re-render (resize, DPI,
//                                           zoom, pane toggle, invert, tab
//                                           switch)
//
// Both pan formulas depend on PR-A1 having moved the origin: under the old
// centred origin pan_y = 0 meant "centred", not "top".
//
// LIFETIME. (epoch, page, slot) does not identify a request -- a same-page zoom
// or resize produces a second P0 with an identical triple -- so the anchor
// records the SEQ of the submission batch that carried it and is consumed only
// by a completion whose seq matches. It follows that a newer page change or
// resubmit REPLACES the anchor, carrying the intent forward to the new seq
// (which is also what makes a failed render recover: the retry re-issues it);
// that set_view clears it; and that epoch-mismatch drops and null completions
// leave it alone. Leaving it alone is safe precisely because of the seq match:
// a stale anchor can never be consumed by the wrong completion.

#include <cstdint>

#include "core/SearchSession.hpp"

namespace litepdf::ui {

struct PageAnchor {
    enum class Kind { None, Top, Bottom, Hit };

    Kind kind = Kind::None;
    // Meaningful only when kind == Kind::Hit. Carried by value so the anchor
    // outlives the SearchSession::next()/prev() result that produced it.
    litepdf::core::SearchSession::Hit target{};

    static PageAnchor none() noexcept { return PageAnchor{}; }

    static PageAnchor top() noexcept {
        PageAnchor a;
        a.kind = Kind::Top;
        return a;
    }

    static PageAnchor bottom() noexcept {
        PageAnchor a;
        a.kind = Kind::Bottom;
        return a;
    }

    static PageAnchor hit(const litepdf::core::SearchSession::Hit& h) {
        PageAnchor a;
        a.kind   = Kind::Hit;
        a.target = h;
        return a;
    }
};

// The canvas's single pending-anchor slot. Nothing outside PdfCanvas owns one.
class AnchorSlot {
public:
    // Record an intent. Replaces whatever was pending -- the newer navigation
    // is the one the user asked for. The seq is not known yet; stamp() sets it
    // when the submission batch is issued.
    void install(PageAnchor a) { anchor_ = std::move(a); }

    // Bind the pending intent to a submission batch. Called once per batch,
    // from PdfCanvas::next_render_seq(). Stamping an EMPTY slot installs
    // nothing: a same-page re-render legitimately carries no anchor.
    void stamp(std::uint64_t seq) noexcept { seq_ = seq; }

    // Take the anchor iff `completion_seq` is the batch it was stamped with.
    // Returns Kind::None (and leaves the slot untouched) otherwise, so an
    // out-of-order completion neither applies nor destroys a live intent.
    // A match clears the slot, so the RIGHT-slot completion of the same spread
    // -- which carries the same seq -- cannot re-apply it.
    PageAnchor consume(std::uint64_t completion_seq) {
        if (anchor_.kind == PageAnchor::Kind::None) return PageAnchor::none();
        if (completion_seq != seq_)                 return PageAnchor::none();
        PageAnchor out = std::move(anchor_);
        anchor_ = PageAnchor::none();
        return out;
    }

    void clear() { anchor_ = PageAnchor::none(); }

    bool pending() const noexcept {
        return anchor_.kind != PageAnchor::Kind::None;
    }

    // Read without consuming. Used by the dual-mode page snap, which must pass
    // an existing Hit through rather than overwrite it with Top.
    const PageAnchor& peek() const noexcept { return anchor_; }

private:
    PageAnchor    anchor_{};
    std::uint64_t seq_ = 0;
};

}  // namespace litepdf::ui
```

- [ ] **Step 4: Run tests to verify they pass**

```bash
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --config Release
```

then

```bash
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" --test-dir build -C Release -R PageAnchor
```

Expected: 8 tests, 8 passed. Then the full suite — expected **273/273 passed**.

- [ ] **Step 5: Commit**

```bash
git add src/ui/detail/PageAnchor.hpp tests/unit/test_page_anchor.cpp tests/CMakeLists.txt
git commit -m "feat(canvas): add PageAnchor and its seq-keyed lifetime slot"
```

---

## Task 3: wire completion identity into `PdfCanvas`

**Files:**
- Modify: `src/ui/PdfCanvas.hpp:104-114` (the two `post_render_done*` declarations),
  plus a new `next_render_seq()` declaration
- Modify: `src/ui/PdfCanvas.cpp:51-112` (`RenderMeta` + `post_render_done_impl`),
  `:114-164` (`Impl`), `:166-195` (`set_view`), `:499-594` (the completion case),
  `:655-687` (`resubmit_current_page`), `:810-855` (`on_key_down`'s submit block)
- Modify: `src/ui/MainWindow.cpp:291-337` (`kick_render`)

**Interfaces:**
- Consumes: `Slot`, `accept_completion` (Task 1).
- Produces:
  - `std::uint64_t PdfCanvas::next_render_seq();` — bumps the counter, returns the
    new value. **Called exactly once per submission batch**, before the
    `request_render*` calls.
  - `static bool PdfCanvas::post_render_done(HWND target, fz_pixmap* pix,
    fz_context* worker_ctx, std::uint64_t epoch, int page, std::uint64_t seq);`
    and `post_render_done_right(...)` with the identical signature.
  Tasks 4–8 rely on both.

**One seq per submission BATCH, not per request.** A spread submits two renders;
both carry the same seq. That is what lets a single anchor — a vertical concept
that belongs to the spread, not to one half of it — be consumed by the left
completion while the right completion still passes the identity check. Every
existing submission site already issues left and right together, so there is no
path where only one half is re-rendered.

This task changes **no pan behaviour**. The unconditional pan reset stays exactly
where it is; Task 4 replaces it. Keeping the two apart means a reviewer can reject
the identity change without also rejecting the anchor design.

- [ ] **Step 1: Extend the header**

Modify `src/ui/PdfCanvas.hpp`. Add the include after the existing
`#include "core/SearchSession.hpp"` (line 9):

```cpp
#include "ui/detail/CompletionMath.hpp"
```

Replace the `post_render_done` declaration block (lines 89-114) with:

```cpp
    // Post WM_USER_RENDER_DONE to `target` for the (pixmap, ctx) pair,
    // where `ctx` is a clone-escrow made from `worker_ctx` so the UI
    // thread can drop the pixmap with the correct MuPDF root — even if
    // the producing DocumentView is torn down before the message lands.
    //
    // Called from the worker thread inside the render callback. Takes
    // an extra ref on the pixmap via fz_keep_pixmap, clones
    // worker_ctx, and on any failure (clone OOM, post FALSE) cleans up
    // both the kept pixmap and the escrow ctx. Returns true iff the
    // message was successfully posted.
    //
    // Callers: MainWindow::kick_render, resubmit_current_page,
    // on_key_down's page-change path, the WM_MOUSEWHEEL zoom path.
    //
    // IDENTITY (PR-A2). `epoch` is render_epoch() read at submit time — it
    // says which VIEW the render belongs to. `page` says which page, and the
    // choice of function says which slot; together they let the handler drop a
    // pixmap the user has already paged away from. `seq` is next_render_seq()
    // read once for the whole submission batch — it says which SUBMISSION,
    // which is what decides whether this completion may consume the pending
    // page anchor. (epoch, page, slot) alone cannot: a same-page zoom or
    // resize produces a second P0 with an identical triple.
    static bool post_render_done(HWND target,
                                 fz_pixmap* pix,
                                 fz_context* worker_ctx,
                                 std::uint64_t epoch,
                                 int page,
                                 std::uint64_t seq);

    // (Phase 8 D10) Variant that posts to the RIGHT slot of the dual-
    // page layout. Same refcount discipline as post_render_done, and both
    // slots of one spread carry the SAME seq.
    static bool post_render_done_right(HWND target,
                                       fz_pixmap* pix,
                                       fz_context* worker_ctx,
                                       std::uint64_t epoch,
                                       int page,
                                       std::uint64_t seq);

    // Open a new submission batch: bump the monotonic submission counter and
    // return its new value, which every request in this batch must carry.
    //
    // Call this ONCE per batch, before the request_render* calls — a spread's
    // two renders share one seq. It also stamps whatever page anchor is
    // pending (PR-A2 Task 4), which is what carries a navigation intent
    // forward when a newer submission supersedes an older one.
    std::uint64_t next_render_seq();
```

- [ ] **Step 2: Run the build to verify it fails**

```bash
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --config Release
```

Expected: FAIL — `post_render_done`: no overload takes 4 arguments, at every call
site, plus an unresolved external for `next_render_seq`.

- [ ] **Step 3: Extend `RenderMeta` and the post helper**

In `src/ui/PdfCanvas.cpp`, replace the `RenderMeta` struct and
`post_render_done_impl` (lines 51-112) with:

```cpp
// Heap payload riding the completion message's LPARAM: the escrow ctx
// (clone of the worker ctx) plus the identity of the render that produced
// it. Allocated on the worker thread, freed on the UI thread in the
// WM_USER_RENDER_DONE[_RIGHT] handler. wParam still carries the fz_pixmap*
// directly.
//
// epoch  which VIEW (bumped by set_view)          -> is this the current tab?
// page   which PAGE was rendered                  -> has the user paged away?
// slot   which HALF of a spread                   -> see CompletionMath.hpp
// seq    which SUBMISSION BATCH (both halves of a spread share one) -> may
//        this completion consume the pending page anchor?
namespace {
struct RenderMeta {
    fz_context*   escrow;
    std::uint64_t epoch;
    int           page;
    litepdf::ui::Slot slot;
    std::uint64_t seq;
};

// Internal helper: shared escrow + Post logic for both the LEFT/single
// slot (msg = WM_USER_RENDER_DONE) and the RIGHT slot (msg =
// WM_USER_RENDER_DONE_RIGHT). Refcount discipline is identical for both;
// only the message ID and the recorded slot change.
bool post_render_done_impl(HWND target, UINT msg, litepdf::ui::Slot slot,
                           fz_pixmap* pix, fz_context* worker_ctx,
                           std::uint64_t epoch, int page, std::uint64_t seq) {
    if (!pix) {
        PostMessageW(target, msg,
                     reinterpret_cast<WPARAM>(nullptr),
                     static_cast<LPARAM>(0));
        return true;
    }
    fz_context* escrow = fz_clone_context(worker_ctx);
    if (!escrow) {
        fz_drop_pixmap(worker_ctx, pix);
        return false;
    }
    auto* meta = new (std::nothrow) RenderMeta{escrow, epoch, page, slot, seq};
    if (!meta) {
        fz_drop_pixmap(escrow, pix);
        fz_drop_context(escrow);
        return false;
    }
    if (!PostMessageW(target, msg,
                      reinterpret_cast<WPARAM>(pix),
                      reinterpret_cast<LPARAM>(meta))) {
        fz_drop_pixmap(escrow, pix);
        fz_drop_context(escrow);
        delete meta;
        return false;
    }
    return true;
}
}  // namespace

bool PdfCanvas::post_render_done(HWND target,
                                 fz_pixmap* pix,
                                 fz_context* worker_ctx,
                                 std::uint64_t epoch,
                                 int page,
                                 std::uint64_t seq) {
    return post_render_done_impl(target, WM_USER_RENDER_DONE, Slot::Left,
                                 pix, worker_ctx, epoch, page, seq);
}

bool PdfCanvas::post_render_done_right(HWND target,
                                       fz_pixmap* pix,
                                       fz_context* worker_ctx,
                                       std::uint64_t epoch,
                                       int page,
                                       std::uint64_t seq) {
    return post_render_done_impl(target, WM_USER_RENDER_DONE_RIGHT, Slot::Right,
                                 pix, worker_ctx, epoch, page, seq);
}
```

- [ ] **Step 4: Add the seq counter to `Impl` and implement `next_render_seq`**

In `struct PdfCanvas::Impl`, immediately after the `view_epoch` member and its
comment (`src/ui/PdfCanvas.cpp:120-123`), add:

```cpp
    // Monotonic submission counter, bumped once per submission BATCH by
    // next_render_seq(). Both halves of a spread carry the same value. Unlike
    // view_epoch it does not survive being compared across views -- it exists
    // only to tell two submissions of the SAME page apart, which is what a
    // same-page zoom or resize produces and what (epoch, page, slot) cannot
    // distinguish.
    std::uint64_t                 next_seq = 0;
```

Add the definition immediately after `PdfCanvas::render_epoch()`
(`src/ui/PdfCanvas.cpp:197-199`):

```cpp
std::uint64_t PdfCanvas::next_render_seq() {
    if (!impl_) return 0;
    return ++impl_->next_seq;
}
```

- [ ] **Step 5: Make `set_view` clear the right bitmap**

Spec §3.1: `set_view` today clears only `current_bitmap`, and only on the null-view
branch (`src/ui/PdfCanvas.cpp:177-184`), while `set_dual_page` returns early when
the flag already matches (`:277`). Switching between two tabs that are both in dual
mode therefore paints the previous document's right page until a new right
completion lands.

Replace the body of `set_view` from the `impl_->view = view;` line
(`src/ui/PdfCanvas.cpp:172`) down to the closing brace of the null-view branch
(`:184`) with:

```cpp
    impl_->view = view;
    // Bump the render epoch on every view swap so any render still in
    // flight for the previous view is recognised as stale at completion
    // and dropped instead of painted over the new view (issue #35).
    ++impl_->view_epoch;
    // The RIGHT slot must be dropped on EVERY swap, not only the null one.
    // set_dual_page returns early when the flag already matches, so switching
    // between two tabs that are both in spread mode never cleared it and the
    // outgoing document's right page stayed on screen until a fresh right
    // completion landed.
    impl_->right_bitmap.Reset();
    if (!view) {
        // No active view — whatever bitmap is on screen is tied to a
        // ctx that will soon be gone. Discard so the next paint shows
        // the cleared background.
        impl_->current_bitmap.Reset();
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
```

- [ ] **Step 6: Use `accept_completion` in the message handler**

In the `WM_USER_RENDER_DONE` / `WM_USER_RENDER_DONE_RIGHT` case
(`src/ui/PdfCanvas.cpp:499-543`), replace everything from
`fz_context* escrow = meta->escrow;` (line 516) down to the closing brace of the
`is_right && !impl_->dual_page` guard (line 543) with:

```cpp
            fz_context* escrow = meta->escrow;
            const std::uint64_t epoch = meta->epoch;
            const int           page  = meta->page;
            const Slot          slot  = meta->slot;
            const std::uint64_t seq   = meta->seq;
            delete meta;
            if (!escrow) {
                // Defensive: meta without escrow — nothing safe to drop.
                return 0;
            }

            // Is this pixmap still wanted? Three ways it may not be: the view
            // was swapped (issue #35 — a render for the previous tab landing
            // after the switch would otherwise paint over the now-active one),
            // the user paged away, or a RIGHT-slot pixmap arrived while the
            // layout is single-page. accept_completion answers all three; see
            // ui/detail/CompletionMath.hpp for why the slot is load-bearing.
            // Drop the pixmap + escrow and bail — do NOT adopt, and do NOT
            // touch the pending anchor: it belongs to the CURRENT view, and a
            // stale completion can never consume it anyway (seq match).
            const int cur_page = impl_->view ? impl_->view->current_page() : 0;
            const int total    = impl_->view ? impl_->view->page_count()   : 0;
            if (!accept_completion(epoch, impl_->view_epoch, page, slot,
                                   cur_page, impl_->dual_page, total)) {
                fz_drop_pixmap(escrow, pix);
                fz_drop_context(escrow);
                return 0;
            }
            (void)seq;   // Task 4 consumes the anchor with it.
```

Add the using-declaration alongside the existing ones in the anonymous namespace
at `src/ui/PdfCanvas.cpp:41-46`:

```cpp
using litepdf::ui::accept_completion;
using litepdf::ui::Slot;
```

- [ ] **Step 7: Update the three `PdfCanvas` submission sites**

In `resubmit_current_page` (`src/ui/PdfCanvas.cpp:655-687`), replace the whole
function body with:

```cpp
    if (!impl_->view) return;
    HWND target = hwnd_;
    const std::uint64_t epoch = impl_->view_epoch;
    const std::uint64_t seq   = next_render_seq();
    if (impl_->dual_page) {
        // (Phase 8 D10) Spread mode: D2DERR_RECREATE_TARGET recovery
        // also has to cover the right slot or the right page stays
        // blank until the user pages forward. Same submission shape as
        // on_key_down's dual branch, and one seq for both halves.
        const int cur   = impl_->view->current_page();
        const int total = impl_->view->page_count();
        const int left  = dual_page_compute_left(cur, total);
        const int right = dual_page_compute_right(left, total);
        impl_->view->cancel_stale_renders(0);
        apply_viewport();
        impl_->view->request_render(left,
            [target, epoch, left, seq](fz_pixmap* p, fz_context* worker_ctx) {
                PdfCanvas::post_render_done(target, p, worker_ctx, epoch, left, seq);
            });
        if (right >= 0) {
            impl_->view->request_render(right,
                [target, epoch, right, seq](fz_pixmap* p, fz_context* worker_ctx) {
                    PdfCanvas::post_render_done_right(target, p, worker_ctx,
                                                      epoch, right, seq);
                });
        }
        return;
    }
    const int page = impl_->view->current_page();
    impl_->view->request_render_with_prefetch(page,
        [target, epoch, page, seq](fz_pixmap* p, fz_context* worker_ctx) {
            PdfCanvas::post_render_done(target, p, worker_ctx, epoch, page, seq);
        });
```

In `on_key_down`'s `if (changed)` block (`src/ui/PdfCanvas.cpp:810-855`), replace
the two `const std::uint64_t epoch = impl_->view_epoch;` line and the four lambdas
so the block reads:

```cpp
    if (changed) {
        HWND target = hwnd_;
        const std::uint64_t epoch = impl_->view_epoch;
        const std::uint64_t seq   = next_render_seq();
        if (impl_->dual_page) {
            // (Phase 8 D10) Spread mode: pair changed, so both bitmaps
            // are stale. Clear them so on_paint shows the chrome
            // background (and the empty-right placeholder when the new
            // pair has no right page) until the new renders land.
            impl_->current_bitmap.Reset();
            impl_->right_bitmap.Reset();
            impl_->view->cancel_stale_renders(0);
            // Defensive re-snap: D15 (programmatic page-jump) and any
            // future entry point that leaves current_page on a non-LEFT-
            // aligned page must not silently mis-pair the spread. Snap
            // the LEFT here (and write it back so observers see the
            // canonical state) instead of trusting current_page raw.
            const int total    = impl_->view->page_count();
            const int left     = dual_page_compute_left(
                                     impl_->view->current_page(), total);
            if (left != impl_->view->current_page()) {
                impl_->view->set_current_page(left);
            }
            apply_viewport();
            const int right = dual_page_compute_right(left, total);
            impl_->view->request_render(left,
                [target, epoch, left, seq](fz_pixmap* p, fz_context* worker_ctx) {
                    PdfCanvas::post_render_done(target, p, worker_ctx, epoch,
                                                left, seq);
                });
            if (right >= 0) {
                impl_->view->request_render(right,
                    [target, epoch, right, seq](fz_pixmap* p, fz_context* worker_ctx) {
                        PdfCanvas::post_render_done_right(target, p, worker_ctx,
                                                          epoch, right, seq);
                    });
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else {
            // Cancel stale renders from rapid paging, submit P0 for current
            // page, and prefetch prev/next at P1 (Task 11). Cache fills
            // happen at the engine level so the next PgUp/PgDn is instant.
            const int page = impl_->view->current_page();
            impl_->view->request_render_with_prefetch(page,
                [target, epoch, page, seq](fz_pixmap* p, fz_context* worker_ctx) {
                    PdfCanvas::post_render_done(target, p, worker_ctx, epoch,
                                                page, seq);
                });
        }
    }
    return 0;
}
```

- [ ] **Step 8: Update `MainWindow::kick_render`**

Replace `src/ui/MainWindow.cpp:291-337` with:

```cpp
void MainWindow::kick_render(int page) {
    auto* view = active_view();
    if (!view || !canvas_) return;

    HWND target = canvas_->hwnd();
    // Stamp every render with the canvas epoch at submit time so a result
    // that lands after a tab switch (e.g. the last-restored tab's render
    // arriving after restore_finish re-activates the saved tab) is dropped
    // by the canvas instead of painted over the now-active tab (issue #35).
    const std::uint64_t epoch = canvas_->render_epoch();
    // One submission batch: both halves of a spread carry this seq, and it is
    // what binds the pending page anchor to this submission (PR-A2 §3.2).
    const std::uint64_t seq = canvas_->next_render_seq();

    if (view->dual_page()) {
        // (Phase 8 D10) Spread layout: snap to the LEFT page of the
        // pair containing `page` (cover-rule + odd-tail handled by the
        // helper) and submit two render requests.
        //
        // Canonicalize the pair FIRST: the fit is derived from current_page, so
        // deriving it before the snap fits the wrong page.
        const int total = view->page_count();
        const int left  = litepdf::ui::dual_page_compute_left(page, total);
        const int right = litepdf::ui::dual_page_compute_right(left, total);
        view->cancel_stale_renders(0);
        view->set_current_page(left);
        canvas_->apply_viewport();

        view->request_render(left,
            [target, epoch, left, seq](fz_pixmap* p, fz_context* worker_ctx) {
                PdfCanvas::post_render_done(target, p, worker_ctx, epoch, left, seq);
            });
        if (right >= 0) {
            view->request_render(right,
                [target, epoch, right, seq](fz_pixmap* p, fz_context* worker_ctx) {
                    PdfCanvas::post_render_done_right(target, p, worker_ctx,
                                                      epoch, right, seq);
                });
        }
        InvalidateRect(canvas_->hwnd(), nullptr, FALSE);
        return;
    }

    canvas_->apply_viewport();

    view->request_render_with_prefetch(page,
        [target, epoch, page, seq](fz_pixmap* p, fz_context* worker_ctx) {
            PdfCanvas::post_render_done(target, p, worker_ctx, epoch, page, seq);
        });
}
```

Note the `set_current_page(left)` at the top of the dual branch is unchanged here —
Task 5 routes it through `change_current_page`.

- [ ] **Step 9: Build and run the full suite**

```bash
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --config Release
```

Expected: clean. Then

```bash
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" --test-dir build -C Release
```

Expected: **273/273 passed** — this task adds no tests; it must break none either.

- [ ] **Step 10: Commit**

```bash
git add src/ui/PdfCanvas.hpp src/ui/PdfCanvas.cpp src/ui/MainWindow.cpp
git commit -m "feat(canvas): give every render completion a page, slot and submission seq"
```

---

## Task 4: page anchors replace the unconditional pan reset

**Files:**
- Modify: `src/ui/PdfCanvas.hpp` — include `PageAnchor.hpp`; new default parameter
  on `change_current_page`; three new private members
- Modify: `src/ui/PdfCanvas.cpp` — `AnchorSlot` in `Impl`; `next_render_seq` stamps
  it; `change_current_page(idx, anchor)`; new `page_origin_y`, `apply_anchor`,
  `navigate_to_page`; the completion handler consumes the anchor instead of zeroing
  the pan; `on_key_down` delegates to `navigate_to_page`

**Interfaces:**
- Consumes: `PageAnchor`, `AnchorSlot` (Task 2); `next_render_seq` (Task 3).
- Produces:
  - `bool PdfCanvas::change_current_page(int idx, PageAnchor anchor =
    PageAnchor::top());`
  - private `void PdfCanvas::navigate_to_page(int target, PageAnchor anchor);` —
    change page, install the anchor, clear stale bitmaps, submit. Task 8's wheel
    flips call it.
  - private `float PdfCanvas::page_origin_y(float src_h, float vp_h) const;` —
    the unpanned vertical origin of the left/single page. Task 6 uses it.

**What this fixes, concretely.** `src/ui/PdfCanvas.cpp:588-589` zeroes `pan_x` and
`pan_y` on every left/single completion. Two shipped consequences:

1. Pan to the bottom of a page, press Zoom In: the view snaps back to the top.
   Confirmed twice by instrument during PR-A1.
2. `MainWindow::on_tab_switch` restores the outgoing tab's pan with
   `canvas_->set_pan(incoming->pan_x, incoming->pan_y)`
   (`src/ui/MainWindow.cpp:602`), and the very next completion destroys it. The
   per-tab scroll snapshot has never worked.

Both become correct when "keep the pan and re-clamp it" is the default and a reset
happens only where a navigation asked for one.

**`pan_x` is never reset by an anchor.** The anchor table in spec §3.2 gives
`pan_y` formulas only — it is a vertical concept. `pan_x` is re-clamped on every
completion (the content width may have changed) but otherwise carried, so a page
turn at a horizontal offset keeps that offset, as every other PDF viewer does.

- [ ] **Step 1: Extend the header**

In `src/ui/PdfCanvas.hpp`, add after the `CompletionMath.hpp` include from Task 3:

```cpp
#include "ui/detail/PageAnchor.hpp"
```

Replace the `change_current_page` declaration (`src/ui/PdfCanvas.hpp:170-176`):

```cpp
    // Page-change entry point for external callers (MainWindow's
    // outline-navigate and cross-tab search-results paths). Wraps
    // `view->set_current_page(idx)` and fires the page-change observer
    // when the page actually moves, so all mutation sites flow through
    // one observer fire-point. Returns true iff the page changed.
    // Safe to call before set_view (returns false).
    //
    // `anchor` says where the new page should land once its pixmap arrives.
    // The default is Top, which is what every page-turn caller wants; the
    // search paths pass PageAnchor::hit(...) and the wheel's backward flip
    // passes PageAnchor::bottom(). The anchor is installed even when the page
    // does not move, because a caller that asks to land on a hit means it
    // whether or not the hit happens to be on the page already showing.
    bool change_current_page(int idx, PageAnchor anchor = PageAnchor::top());
```

Add to the private section, after `LRESULT on_key_down(WPARAM key);`
(`src/ui/PdfCanvas.hpp:196`):

```cpp
    // Change page, install `anchor`, drop stale bitmaps and submit the render
    // batch. The single funnel for every in-canvas navigation: PgUp / PgDn /
    // Home / End and the wheel's edge flips. No-op when the page does not move
    // AND the anchor is Top (nothing to re-render, nothing to re-anchor).
    void navigate_to_page(int target, PageAnchor anchor);

    // Unpanned vertical origin of the LEFT/single page in canvas DIPs, i.e.
    // what on_paint would use with pan_y == 0. Single mode: place_bitmap
    // centres a fitting page and pins an overflowing one to 0. Dual mode: the
    // same, because the slot band is the full canvas height and on_paint's
    // union base_y is provably 0 there (a union taller than the band always
    // has t == 0, since an overflowing slot placement has y == 0).
    float page_origin_y(float src_h, float vp_h) const;

    // Turn a consumed anchor into a pan. Called from the left/single
    // completion handler once the new bitmap is installed, so the page's real
    // height is known. Kind::None keeps the current pan and only re-clamps it.
    void apply_anchor(const PageAnchor& anchor);
```

- [ ] **Step 2: Run the build to verify it fails**

```bash
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --config Release
```

Expected: FAIL — unresolved externals for `navigate_to_page`, `page_origin_y` and
`apply_anchor`.

- [ ] **Step 3: Add the slot to `Impl` and stamp it in `next_render_seq`**

In `struct PdfCanvas::Impl`, after the `next_seq` member added in Task 3, add:

```cpp
    // Where the next left/single completion should put the page. Empty for a
    // same-page re-render, which must KEEP the current pan. See
    // ui/detail/PageAnchor.hpp for the lifetime rule.
    AnchorSlot                    anchor;
```

Replace `PdfCanvas::next_render_seq()` (added in Task 3) with:

```cpp
std::uint64_t PdfCanvas::next_render_seq() {
    if (!impl_) return 0;
    ++impl_->next_seq;
    // Bind whatever intent is pending to THIS batch. A resubmit that installs
    // no anchor of its own re-stamps an older pending one, which is what
    // carries a navigation forward when a superseding render replaces the one
    // that was going to consume it -- and what makes a failed render recover,
    // since the retry re-issues the batch.
    impl_->anchor.stamp(impl_->next_seq);
    return impl_->next_seq;
}
```

- [ ] **Step 4: Take the anchor in `change_current_page`, clear it in `set_view`**

Replace `PdfCanvas::change_current_page` (`src/ui/PdfCanvas.cpp:237-244`):

```cpp
bool PdfCanvas::change_current_page(int idx, PageAnchor anchor) {
    if (!impl_ || !impl_->view) return false;
    // Install the anchor even when the page does not move: a search hit on the
    // page already showing still has to be scrolled to. next_render_seq()
    // binds it to the batch the caller submits next.
    impl_->anchor.install(std::move(anchor));
    const bool changed = impl_->view->set_current_page(idx);
    if (changed && impl_->on_page_changed) {
        impl_->on_page_changed(impl_->view->current_page());
    }
    return changed;
}
```

In `set_view`, immediately after the `impl_->right_bitmap.Reset();` line added in
Task 3, add:

```cpp
    // New view, new epoch: an anchor installed for the outgoing document
    // describes a page that is no longer on screen.
    impl_->anchor.clear();
```

- [ ] **Step 5: Implement `page_origin_y`, `apply_anchor` and `navigate_to_page`**

Insert these three definitions immediately after `PdfCanvas::pan_by`
(`src/ui/PdfCanvas.cpp:752`):

```cpp
float PdfCanvas::page_origin_y(float src_h, float vp_h) const {
    // place_bitmap with pan 0 gives exactly what on_paint uses as the page's
    // unpanned top: the centred position when the page fits, and 0 when it
    // overflows. Width does not affect the vertical result, so any positive
    // width will do here.
    return place_bitmap(src_h, src_h, src_h, vp_h, 0.0f, 0.0f).y;
}

void PdfCanvas::apply_anchor(const PageAnchor& anchor) {
    ContentBox box{};
    if (!content_extent(box)) return;
    const D2D1_SIZE_F vp = impl_->rt->GetSize();

    switch (anchor.kind) {
        case PageAnchor::Kind::Top:
            impl_->pan_y = 0.0f;
            break;
        case PageAnchor::Kind::Bottom:
            // The far end of the pan range. clamp_pan below turns this into 0
            // when the content fits, which is the right answer -- a page that
            // fits has no distinct bottom to land on.
            impl_->pan_y = vp.height - box.h;
            break;
        case PageAnchor::Kind::Hit:
            impl_->pan_y = pan_y_for_hit(anchor.target);
            break;
        case PageAnchor::Kind::None:
            // Same-page re-render: keep the pan exactly where the user left
            // it. Only the clamp below applies, because the content may have
            // changed size (zoom, DPI, pane toggle, window resize).
            break;
    }
    impl_->pan_x = clamp_pan(impl_->pan_x, box.w, vp.width);
    impl_->pan_y = clamp_pan(impl_->pan_y, box.h, vp.height);
}

void PdfCanvas::navigate_to_page(int target, PageAnchor anchor) {
    if (!impl_ || !impl_->view) return;
    const bool wants_move   = target != impl_->view->current_page();
    const bool wants_anchor = anchor.kind != PageAnchor::Kind::Top;
    if (!wants_move && !wants_anchor) return;

    change_current_page(target, std::move(anchor));

    HWND target_hwnd = hwnd_;
    const std::uint64_t epoch = impl_->view_epoch;
    const std::uint64_t seq   = next_render_seq();
    if (impl_->dual_page) {
        // (Phase 8 D10) Spread mode: the pair changed, so both bitmaps are
        // stale. Clear them so on_paint shows the chrome background (and the
        // empty-right placeholder when the new pair has no right page) until
        // the new renders land.
        impl_->current_bitmap.Reset();
        impl_->right_bitmap.Reset();
        impl_->view->cancel_stale_renders(0);
        // Defensive re-snap: any entry point that leaves current_page on a
        // non-LEFT-aligned page must not silently mis-pair the spread. Snap
        // the LEFT here (and write it back so observers see the canonical
        // state) instead of trusting current_page raw.
        const int total = impl_->view->page_count();
        const int left  = dual_page_compute_left(impl_->view->current_page(),
                                                 total);
        if (left != impl_->view->current_page()) {
            // Carry the pending anchor through: routing the snap with a fresh
            // Top would overwrite a Hit installed moments earlier when a
            // search lands on the RIGHT page of a spread.
            change_current_page(left, impl_->anchor.peek());
        }
        apply_viewport();
        const int right = dual_page_compute_right(left, total);
        impl_->view->request_render(left,
            [target_hwnd, epoch, left, seq](fz_pixmap* p, fz_context* worker_ctx) {
                PdfCanvas::post_render_done(target_hwnd, p, worker_ctx, epoch,
                                            left, seq);
            });
        if (right >= 0) {
            impl_->view->request_render(right,
                [target_hwnd, epoch, right, seq](fz_pixmap* p, fz_context* worker_ctx) {
                    PdfCanvas::post_render_done_right(target_hwnd, p, worker_ctx,
                                                      epoch, right, seq);
                });
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    // Cancel stale renders from rapid paging, submit P0 for the current page,
    // and prefetch prev/next at P1 so the next PgUp/PgDn is instant.
    const int page = impl_->view->current_page();
    impl_->view->request_render_with_prefetch(page,
        [target_hwnd, epoch, page, seq](fz_pixmap* p, fz_context* worker_ctx) {
            PdfCanvas::post_render_done(target_hwnd, p, worker_ctx, epoch,
                                        page, seq);
        });
}
```

`pan_y_for_hit` does not exist yet — Task 6 extracts it from `scroll_into_view`.
For this task, declare it in the private section of `src/ui/PdfCanvas.hpp` right
after `page_origin_y`:

```cpp
    // Pan that centres `h`'s quad vertically, using the same geometry as
    // on_paint. Extracted in Task 6; scroll_into_view and the Hit anchor share
    // it so the pre-render estimate and the post-render placement cannot drift.
    float pan_y_for_hit(const litepdf::core::SearchSession::Hit& h) const;
```

and add a provisional definition just above `apply_anchor` that Task 6 replaces:

```cpp
float PdfCanvas::pan_y_for_hit(const litepdf::core::SearchSession::Hit& h) const {
    // Task 4 placeholder shape, replaced wholesale in Task 6 with the geometry
    // extracted from scroll_into_view. Keeping the signature here lets
    // apply_anchor be written and reviewed as one piece.
    if (!impl_ || !impl_->current_bitmap || !impl_->rt || !impl_->view) {
        return impl_ ? impl_->pan_y : 0.0f;
    }
    const D2D1_SIZE_F src_px = impl_->current_bitmap->GetSize();
    const D2D1_SIZE_F vp     = impl_->rt->GetSize();
    const float rt_dpi = static_cast<float>(GetDpiForWindow(hwnd_));
    const float src_h  = bitmap_px_to_dip(src_px.height, rt_dpi);
    const float pct    = impl_->view->zoom_pct();
    const float q_min_y_pt = std::min({ h.geom.ul_y, h.geom.ur_y,
                                        h.geom.ll_y, h.geom.lr_y });
    const float q_max_y_pt = std::max({ h.geom.ul_y, h.geom.ur_y,
                                        h.geom.ll_y, h.geom.lr_y });
    const float q_center = pdf_point_to_dip((q_min_y_pt + q_max_y_pt) * 0.5f, pct);
    return clamp_pan(vp.height * 0.5f - q_center - page_origin_y(src_h, vp.height),
                     src_h, vp.height);
}
```

- [ ] **Step 6: Consume the anchor in the completion handler**

In the completion case, replace the `(void)seq;` line added in Task 3 Step 6 with
nothing, and replace the left/single branch (`src/ui/PdfCanvas.cpp:580-591` in the
pre-Task-3 numbering — locate it by the `if (is_right) {` line near the end of the
case) with:

```cpp
            if (is_right) {
                impl_->right_bitmap = std::move(bmp);
            } else {
                impl_->current_bitmap = std::move(bmp);
                ColdStartTimer::mark(3);  // first pixmap -> D2D bitmap
            }
            // Place the page. The LEFT slot is the anchor point, but the pan
            // is clamped against the UNION of both slots, so a right-slot
            // delivery has to re-run the placement too -- it can change the
            // union's height. AnchorSlot::consume clears on the first match,
            // so the right slot re-clamps without re-applying the anchor.
            apply_anchor(impl_->anchor.consume(seq));
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
```

- [ ] **Step 7: Route `on_key_down` through `navigate_to_page`**

Replace `on_key_down`'s body from `int cur = impl_->view->current_page();`
(`src/ui/PdfCanvas.cpp:757`) through the closing brace of the `if (changed)` block
with:

```cpp
    const int cur     = impl_->view->current_page();
    const int max_idx = impl_->view->page_count() - 1;

    switch (key) {
        case VK_NEXT: {  // PgDn
            int next;
            if (impl_->dual_page) {
                // (Phase 8 T4) Snap from the current spread's LEFT page,
                // letting dual_page_step_next_left handle the cover->1
                // bootstrap explicitly — a plain `cur_left + 2` stride
                // overshoots from cover (0+2=2) and skips spread (1,2).
                const int total    = impl_->view->page_count();
                const int cur_left = dual_page_compute_left(cur, total);
                next = dual_page_step_next_left(cur_left, total);
            } else {
                next = std::min(cur + 1, max_idx);
            }
            navigate_to_page(next, PageAnchor::top());
            return 0;
        }
        case VK_PRIOR: {  // PgUp
            int prev;
            if (impl_->dual_page) {
                // Symmetric step-back via the helper. `cur_left == 1`
                // (first spread) snaps to 0 (cover); `cur_left >= 3`
                // walks back by 2.
                const int total    = impl_->view->page_count();
                const int cur_left = dual_page_compute_left(cur, total);
                prev = dual_page_step_prev_left(cur_left, total);
            } else {
                prev = std::max(cur - 1, 0);
            }
            // PgUp lands at the TOP of the previous page, unlike the wheel's
            // backward flip which lands at its bottom: a key press is a
            // discrete jump, while wheel scrolling is continuous motion whose
            // content must not skip.
            navigate_to_page(prev, PageAnchor::top());
            return 0;
        }
        case VK_HOME:
            navigate_to_page(0, PageAnchor::top());
            return 0;
        case VK_END:
            navigate_to_page(max_idx, PageAnchor::top());
            return 0;
        // Arrow keys pan by 100 DIP, clamped to the content.
        case VK_LEFT:  return pan_by( 100.0f,    0.0f);
        case VK_RIGHT: return pan_by(-100.0f,    0.0f);
        case VK_UP:    return pan_by(   0.0f,  100.0f);
        case VK_DOWN:  return pan_by(   0.0f, -100.0f);
        default:
            return 0;
    }
}
```

- [ ] **Step 8: Build and run the full suite**

```bash
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --config Release
```

Expected: clean. Then

```bash
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" --test-dir build -C Release
```

Expected: **273/273 passed**.

- [ ] **Step 9: Commit**

```bash
git add src/ui/PdfCanvas.hpp src/ui/PdfCanvas.cpp
git commit -m "feat(canvas): anchor a page on render completion instead of zeroing the pan"
```

---

## Task 5: route every page-change path through `change_current_page`

**Files:**
- Modify: `src/ui/MainWindow.cpp:306` (`kick_render`'s dual snap), `:1375`
  (`IDM_VIEW_DUAL_PAGE`), `:903` (`restore_on_tab_ready`)
- Modify: `src/ui/PdfCanvas.cpp` — `apply_viewport`'s defensive re-snap (`:220`)

**Interfaces:**
- Consumes: `change_current_page(idx, anchor)` (Task 4).
- Produces: nothing new. This task closes four observer gaps.

**The four bypasses and what each costs today** (spec §3.3, re-verified at
`8bf143d`):

| site | symptom |
|------|---------|
| `MainWindow.cpp:306` — `kick_render`'s dual snap | thumbnail highlight wrong in spread mode |
| `MainWindow.cpp:1375` — `IDM_VIEW_DUAL_PAGE` | same, on toggling spread |
| `MainWindow.cpp:903` — `restore_on_tab_ready` | restored page not broadcast |
| `PdfCanvas.cpp:220` — `apply_viewport`'s defensive re-snap | dual-mode End reports the pre-snap page |

`PdfCanvas.cpp:829-831` (`on_key_down`'s re-snap) was the fifth; Task 4 already
folded it into `navigate_to_page`.

**The dual snap must not clobber a pending `Hit`.** Routing a snap through
`change_current_page` with a fresh `Top` would overwrite a `Hit` installed moments
earlier — a search landing on the RIGHT page of a spread snaps to the left page on
the way to rendering. Every snap below therefore passes
`canvas_->pending_anchor()` (a new const accessor) when one is set and `Top` only
when none is.

- [ ] **Step 1: Expose the pending anchor**

Add to the public section of `src/ui/PdfCanvas.hpp`, after `change_current_page`:

```cpp
    // The anchor waiting for the next completion, or PageAnchor::none().
    //
    // Exists for one caller shape: a defensive page SNAP (dual-mode
    // canonicalisation) is not a navigation and must not replace a navigation
    // intent that is already pending. Such callers pass this value straight
    // back into change_current_page when it is set, and Top when it is not.
    PageAnchor pending_anchor() const;
```

and the definition in `src/ui/PdfCanvas.cpp`, after `change_current_page`:

```cpp
PageAnchor PdfCanvas::pending_anchor() const {
    if (!impl_) return PageAnchor::none();
    return impl_->anchor.peek();
}
```

- [ ] **Step 2: Add a snap helper to `MainWindow` and use it in `kick_render`**

Declare in the private section of `src/ui/MainWindow.hpp`, next to `kick_render`:

```cpp
    // Canonicalise the current page to `page` through the canvas so the
    // page-change observer (thumbnail highlight, and PR-B's page indicator)
    // sees it. Preserves a pending navigation anchor: a snap is bookkeeping,
    // not a navigation, and must not overwrite a search Hit already installed.
    void snap_current_page(int page);
```

Define it in `src/ui/MainWindow.cpp`, immediately above `kick_render`:

```cpp
void MainWindow::snap_current_page(int page) {
    if (!canvas_) return;
    const auto pending = canvas_->pending_anchor();
    canvas_->change_current_page(
        page,
        pending.kind == litepdf::ui::PageAnchor::Kind::None
            ? litepdf::ui::PageAnchor::top()
            : pending);
}
```

In `kick_render`'s dual branch, replace `view->set_current_page(left);` with:

```cpp
        snap_current_page(left);
```

- [ ] **Step 3: Route `IDM_VIEW_DUAL_PAGE`**

In `src/ui/MainWindow.cpp:1372-1376`, replace:

```cpp
                    if (v->dual_page()) {
                        p = litepdf::ui::dual_page_compute_left(
                                p, v->page_count());
                        v->set_current_page(p);
                    }
```

with:

```cpp
                    if (v->dual_page()) {
                        p = litepdf::ui::dual_page_compute_left(
                                p, v->page_count());
                        snap_current_page(p);
                    }
```

- [ ] **Step 4: Route `restore_on_tab_ready`**

In `src/ui/MainWindow.cpp:903`, replace:

```cpp
        v->set_current_page(st.page);
```

with:

```cpp
        // Route through the canvas so the page-change observer fires: after a
        // session restore the model and the thumbnail highlight otherwise
        // disagree until the user's first navigation. Top is right here --
        // a restored tab has no pan to preserve (SessionTab carries page and
        // zoom, not scroll offset).
        canvas_->change_current_page(st.page, litepdf::ui::PageAnchor::top());
```

`canvas_` is already dereferenced unconditionally two lines below
(`GetClientRect(canvas_->hwnd(), &rc)`), so no extra null guard is warranted; the
enclosing `if (auto* v = active_view())` runs only when a tab exists.

- [ ] **Step 5: Route `apply_viewport`'s defensive re-snap**

In `src/ui/PdfCanvas.cpp:220`, replace:

```cpp
    if (left != impl_->view->current_page()) impl_->view->set_current_page(left);
```

with:

```cpp
    if (left != impl_->view->current_page()) {
        // Route through change_current_page so the observer fires: dual-mode
        // End otherwise reports the pre-snap page. Carry any pending anchor
        // through -- a snap is bookkeeping and must not replace a navigation.
        change_current_page(left, impl_->anchor.peek());
    }
```

- [ ] **Step 6: Build and run the full suite**

```bash
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --config Release
```

then

```bash
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" --test-dir build -C Release
```

Expected: **273/273 passed**.

- [ ] **Step 7: Verify no bypass remains**

```bash
grep -n "set_current_page" src/ui/MainWindow.cpp src/ui/PdfCanvas.cpp
```

Expected: every `view->set_current_page` / `impl_->view->set_current_page` hit is
gone from both files. The remaining matches must be only
`DocumentView::set_current_page`'s own definition (not in these files),
`ThumbnailPane::set_current_page` (`MainWindow.cpp:660`, `:1108` — a different
class, the pane's own highlight setter) and `PdfCanvas::change_current_page`'s
internals. If any other hit remains, route it before committing.

- [ ] **Step 8: Commit**

```bash
git add src/ui/PdfCanvas.hpp src/ui/PdfCanvas.cpp src/ui/MainWindow.hpp src/ui/MainWindow.cpp
git commit -m "fix(nav): route every page-change path through the observer fire-point"
```

---

## Task 6: search navigation carries a `Hit` anchor

**Files:**
- Modify: `src/ui/PdfCanvas.cpp:295-354` (`scroll_into_view`) — extract
  `pan_y_for_hit`, fix the dual-mode clamp
- Modify: `src/ui/MainWindow.cpp:1790-1812` (`on_find_next` / `on_find_prev`),
  `:1975` (`on_results_row_click`)

**Interfaces:**
- Consumes: `PageAnchor::hit`, `page_origin_y`, `apply_anchor` (Tasks 2 and 4).
- Produces: the final `pan_y_for_hit`, replacing Task 4's provisional version.

**Two defects, one fix.**

*Defect A (spec §3.4).* `scroll_into_view` computes `pan_y` from the **previous**
page's bitmap and is then followed by `kick_render`, whose completion used to reset
the pan (`MainWindow.cpp:1796-1799`, `:1809-1810`, `:1982-1983`). Task 4 changed
the reset into "keep and re-clamp", which makes the stale estimate *persist*
instead of being discarded — strictly worse than before unless the `Hit` anchor
lands in the same PR. It does, here.

*Defect B (inherited from PR-A1, escalated item E2).* `scroll_into_view` clamps
against the LEFT bitmap's height (`src/ui/PdfCanvas.cpp:352`, `src_h`), while
`on_paint` clamps against the UNION of both slots (`:920`, `box.h`). In a spread
with a taller right page the two disagree, and the hit lands off-centre. Invisible
today only because the overlay is disabled in dual mode (R17); this task removes
the disagreement without enabling the overlay.

The vertical geometry is otherwise identical in both modes: `on_paint`'s dual slot
band is `slot_h = vp.height`, and its union `base_y` is provably 0 (a union taller
than the band always has `t == 0`, because an overflowing slot placement has
`y == 0`). So clamping against `content_extent().h` in both modes is the whole fix.

- [ ] **Step 1: Replace `pan_y_for_hit` with the real implementation**

Replace Task 4's provisional `PdfCanvas::pan_y_for_hit` with:

```cpp
float PdfCanvas::pan_y_for_hit(const litepdf::core::SearchSession::Hit& h) const {
    if (!impl_ || !impl_->current_bitmap || !impl_->rt || !impl_->view) {
        return impl_ ? impl_->pan_y : 0.0f;
    }
    ContentBox box{};
    if (!content_extent(box)) return impl_->pan_y;

    const D2D1_SIZE_F src_px = impl_->current_bitmap->GetSize();
    const D2D1_SIZE_F vp     = impl_->rt->GetSize();
    const float rt_dpi = static_cast<float>(GetDpiForWindow(hwnd_));
    const float src_h  = bitmap_px_to_dip(src_px.height, rt_dpi);
    const float pct    = impl_->view->zoom_pct();

    // Quad centre in PDF points -> DIPs, measured from the page's own top.
    const float q_min_y_pt = std::min({ h.geom.ul_y, h.geom.ur_y,
                                        h.geom.ll_y, h.geom.lr_y });
    const float q_max_y_pt = std::max({ h.geom.ul_y, h.geom.ur_y,
                                        h.geom.ll_y, h.geom.lr_y });
    const float q_center = pdf_point_to_dip((q_min_y_pt + q_max_y_pt) * 0.5f, pct);

    // Centre the quad in the viewport. page_origin_y is the page's unpanned
    // top, so the pan needed to put the quad at vp/2 is the difference.
    //
    // The clamp measures against box.h, the PAINTED union, not against src_h.
    // on_paint clamps the same way; using the left bitmap's own height here
    // disagreed with the paint path in a spread whose right page is taller
    // (PR-A1 escalated item E2), landing the hit off-centre. In single-page
    // mode box.h IS src_h, so this is the same number the old code produced.
    return clamp_pan(vp.height * 0.5f - q_center - page_origin_y(src_h, vp.height),
                     box.h, vp.height);
}
```

`content_extent` is already `const` (`PdfCanvas.hpp:204`, `PdfCanvas.cpp:700`), so
`pan_y_for_hit` can be `const` with no cast. Checked at `8bf143d`.

- [ ] **Step 2: Rewrite `scroll_into_view` on top of it**

Replace `PdfCanvas::scroll_into_view` (`src/ui/PdfCanvas.cpp:295-354`) with:

```cpp
void PdfCanvas::scroll_into_view(const litepdf::core::SearchSession::Hit& h) {
    if (!impl_ || !impl_->view || !hwnd_) return;

    // Install the Hit anchor unconditionally, page change or not. Two reasons:
    // the caller's kick_render will land a fresh bitmap whose real height is
    // what the centring actually needs (the estimate below is computed against
    // the OUTGOING page), and a hit on the page already showing still has to be
    // scrolled to.
    const int target_pg = static_cast<int>(h.page);
    change_current_page(target_pg, PageAnchor::hit(h));

    // Best-effort immediate scroll so the view moves before the render lands.
    // Refined by apply_anchor when the completion arrives.
    if (!impl_->current_bitmap || !impl_->rt) {
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    const D2D1_SIZE_F src_px = impl_->current_bitmap->GetSize();
    const D2D1_SIZE_F vp     = impl_->rt->GetSize();
    const float rt_dpi = static_cast<float>(GetDpiForWindow(hwnd_));
    const float src_h  = bitmap_px_to_dip(src_px.height, rt_dpi);
    const float pct    = impl_->view->zoom_pct();

    const float q_min_y_pt = std::min({ h.geom.ul_y, h.geom.ur_y,
                                        h.geom.ll_y, h.geom.lr_y });
    const float q_max_y_pt = std::max({ h.geom.ul_y, h.geom.ur_y,
                                        h.geom.ll_y, h.geom.lr_y });

    // Already visible? Measure against the SAME origin the paint path uses.
    const float origin_y  = page_origin_y(src_h, vp.height) + impl_->pan_y;
    const float q_top_dip = origin_y + pdf_point_to_dip(q_min_y_pt, pct);
    const float q_bot_dip = origin_y + pdf_point_to_dip(q_max_y_pt, pct);
    const float margin    = 24.0f;
    if (q_top_dip >= margin && q_bot_dip <= vp.height - margin) {
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;   // already visible; no scroll
    }

    impl_->pan_y = pan_y_for_hit(h);
    InvalidateRect(hwnd_, nullptr, FALSE);
}
```

Note the removed `impl_->pan_x = 0.0f; impl_->pan_y = 0.0f;` reset on page change:
the anchor now owns the landing position, and zeroing here would fight it.

- [ ] **Step 3: Make `on_results_row_click` pass the Hit explicitly**

Spec §3.4: `on_results_row_click` calls `change_current_page(h.page)` itself at
`src/ui/MainWindow.cpp:1975` — taking the default `Top` — and only then calls
`scroll_into_view`, which finds the page already correct and (before Step 2) never
installed the Hit. Step 2 makes `scroll_into_view` install it unconditionally, so
the bug is already closed; this step removes the redundant `Top` install that would
otherwise sit between them for one statement.

Replace `src/ui/MainWindow.cpp:1970-1982` with:

```cpp
    // Recompose a Hit for the canvas overlay + scroll. SearchSession::Hit
    // and CrossTabSearch::Hit share the (page, geom) pair; we copy into
    // the canvas-native shape.
    litepdf::core::SearchSession::Hit sh{h.page, h.geom};
    // scroll_into_view routes through PdfCanvas::change_current_page with a
    // Hit anchor, so the T7 page-change observer fires for cross-tab search
    // jumps too and the completion lands the page on the hit rather than at
    // its top. set_active above already triggered on_tab_switch ->
    // canvas_->set_view, which fired the observer with the incoming tab's
    // stored page; scroll_into_view's fire reflects the search-jump target.
    canvas_->set_current_hit(sh);
    canvas_->scroll_into_view(sh);
    kick_render(v->current_page());
}
```

`on_find_next` and `on_find_prev` (`src/ui/MainWindow.cpp:1790-1812`) need no
change: they already call `set_current_hit` then `scroll_into_view` then
`kick_render`, which is now the correct sequence. Re-read them and confirm rather
than editing.

- [ ] **Step 4: Build and run the full suite**

```bash
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --config Release
```

then

```bash
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" --test-dir build -C Release
```

Expected: **273/273 passed**. `test_search_session.cpp` and
`test_cross_tab_search.cpp` exercise the model, not the canvas, so they are
unaffected — if either moves, the change reached further than intended.

- [ ] **Step 5: Commit**

```bash
git add src/ui/PdfCanvas.cpp src/ui/MainWindow.cpp
git commit -m "fix(search): carry the hit geometry to the completion that can place it"
```

---

## Task 7: `ScrollMath.hpp` — wheel stepping, residuals and edge flips

**Files:**
- Create: `src/ui/detail/ScrollMath.hpp`
- Create: `tests/unit/test_scroll_math.cpp`
- Modify: `tests/CMakeLists.txt` (append after the Task 2 line)

**Interfaces:**
- Consumes: `litepdf::ui::clamp_pan` from `ui/detail/ViewportMath.hpp`.
- Produces:
  - `enum class litepdf::ui::Flip { None, Next, Prev };`
  - `int litepdf::ui::consume_notches(int delta, int& residual) noexcept;`
  - `float litepdf::ui::wheel_step_dip(int notches, unsigned lines_per_notch,
    float vp_h) noexcept;`
  - `struct litepdf::ui::WheelResult { float pan_y; Flip flip; };` and
    `WheelResult litepdf::ui::apply_wheel(float pan_y, float content_h, float
    vp_h, float step) noexcept;`
  Task 8 uses all of them.

**Sign convention, stated once.** `pan_y` lives in `[vp_h - content_h, 0]` with
`0` at the top of the page (PR-A1's top-left origin). Wheel **up** means
`delta > 0` and moves the reader toward the top, so `step > 0` and
`pan_y` increases toward 0. This matches `VK_UP -> pan_by(0, +100)`
(`src/ui/PdfCanvas.cpp:804`).

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_scroll_math.cpp`:

```cpp
// PR-A2 Task 7: pure-logic tests for wheel scrolling.
//
// pan_y lives in [vp_h - content_h, 0] with 0 at the page top (the top-left
// origin PR-A1 introduced). Wheel UP is delta > 0, step > 0, and moves pan_y
// toward 0.

#include "ui/detail/ScrollMath.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using litepdf::ui::apply_wheel;
using litepdf::ui::consume_notches;
using litepdf::ui::Flip;
using litepdf::ui::wheel_step_dip;

TEST_CASE("ScrollMath consumes one whole notch and keeps no residual",
          "[ui][scroll]") {
    int residual = 0;
    REQUIRE(consume_notches(120, residual) == 1);
    REQUIRE(residual == 0);
    REQUIRE(consume_notches(-120, residual) == -1);
    REQUIRE(residual == 0);
}

TEST_CASE("ScrollMath accumulates high-resolution wheel deltas",
          "[ui][scroll]") {
    // A precision wheel delivers |delta| < WHEEL_DELTA. No notch fires until
    // the accumulated total crosses 120, and the remainder carries forward.
    int residual = 0;
    REQUIRE(consume_notches(40, residual) == 0);
    REQUIRE(residual == 40);
    REQUIRE(consume_notches(40, residual) == 0);
    REQUIRE(residual == 80);
    REQUIRE(consume_notches(50, residual) == 1);
    REQUIRE(residual == 10);
}

TEST_CASE("ScrollMath accumulates negative deltas symmetrically",
          "[ui][scroll]") {
    int residual = 0;
    REQUIRE(consume_notches(-50, residual) == 0);
    REQUIRE(residual == -50);
    REQUIRE(consume_notches(-100, residual) == -1);
    REQUIRE(residual == -30);
}

TEST_CASE("ScrollMath consumes several notches from one fat delta",
          "[ui][scroll]") {
    int residual = 0;
    REQUIRE(consume_notches(360, residual) == 3);
    REQUIRE(residual == 0);
}

TEST_CASE("ScrollMath step is lines times the line height", "[ui][scroll]") {
    REQUIRE(wheel_step_dip(1, 3, 800.0f)  == Catch::Approx(48.0f));
    REQUIRE(wheel_step_dip(-1, 3, 800.0f) == Catch::Approx(-48.0f));
    REQUIRE(wheel_step_dip(2, 3, 800.0f)  == Catch::Approx(96.0f));
}

TEST_CASE("ScrollMath page-scroll sentinel degrades to most of the viewport",
          "[ui][scroll]") {
    // WHEEL_PAGESCROLL is UINT_MAX. A full viewport height would land exactly
    // on the next edge and flip on the following notch; 90% leaves an overlap.
    REQUIRE(wheel_step_dip(1, 0xFFFFFFFFu, 800.0f) == Catch::Approx(720.0f));
}

TEST_CASE("ScrollMath honours a zero-lines setting as no scrolling",
          "[ui][scroll]") {
    REQUIRE(wheel_step_dip(1, 0, 800.0f) == Catch::Approx(0.0f));
    REQUIRE(wheel_step_dip(0, 3, 800.0f) == Catch::Approx(0.0f));
}

TEST_CASE("ScrollMath scrolls within an overflowing page", "[ui][scroll]") {
    // content 2000, viewport 800 -> pan range [-1200, 0].
    const auto down = apply_wheel(-100.0f, 2000.0f, 800.0f, -48.0f);
    REQUIRE(down.flip == Flip::None);
    REQUIRE(down.pan_y == Catch::Approx(-148.0f));

    const auto up = apply_wheel(-100.0f, 2000.0f, 800.0f, 48.0f);
    REQUIRE(up.flip == Flip::None);
    REQUIRE(up.pan_y == Catch::Approx(-52.0f));
}

TEST_CASE("ScrollMath clamps at an edge before it flips", "[ui][scroll]") {
    // A step that merely REACHES the edge scrolls; the flip is the next notch.
    const auto reach = apply_wheel(-1180.0f, 2000.0f, 800.0f, -48.0f);
    REQUIRE(reach.flip == Flip::None);
    REQUIRE(reach.pan_y == Catch::Approx(-1200.0f));

    const auto flip = apply_wheel(-1200.0f, 2000.0f, 800.0f, -48.0f);
    REQUIRE(flip.flip == Flip::Next);
    REQUIRE(flip.pan_y == Catch::Approx(-1200.0f));   // unchanged
}

TEST_CASE("ScrollMath flips backward at the top edge", "[ui][scroll]") {
    const auto reach = apply_wheel(-20.0f, 2000.0f, 800.0f, 48.0f);
    REQUIRE(reach.flip == Flip::None);
    REQUIRE(reach.pan_y == Catch::Approx(0.0f));

    const auto flip = apply_wheel(0.0f, 2000.0f, 800.0f, 48.0f);
    REQUIRE(flip.flip == Flip::Prev);
    REQUIRE(flip.pan_y == Catch::Approx(0.0f));
}

TEST_CASE("ScrollMath flips immediately when the page fits", "[ui][scroll]") {
    // Nothing to scroll: one notch is one page.
    const auto next = apply_wheel(0.0f, 600.0f, 800.0f, -48.0f);
    REQUIRE(next.flip == Flip::Next);
    const auto prev = apply_wheel(0.0f, 600.0f, 800.0f, 48.0f);
    REQUIRE(prev.flip == Flip::Prev);
}

TEST_CASE("ScrollMath does nothing for a zero step", "[ui][scroll]") {
    const auto r = apply_wheel(-100.0f, 2000.0f, 800.0f, 0.0f);
    REQUIRE(r.flip == Flip::None);
    REQUIRE(r.pan_y == Catch::Approx(-100.0f));
}
```

Register it in `tests/CMakeLists.txt`, after the Task 2 line:

```cmake
    unit/test_scroll_math.cpp          # PR-A2 Task 7
```

- [ ] **Step 2: Run test to verify it fails**

```bash
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --config Release
```

Expected: FAIL — `Cannot open include file: 'ui/detail/ScrollMath.hpp'`.

- [ ] **Step 3: Write the header**

Create `src/ui/detail/ScrollMath.hpp`:

```cpp
#pragma once

// PR-A2: mouse-wheel scrolling as pure logic. No Win32, no Direct2D -- the
// canvas supplies the raw delta, the SPI_GETWHEELSCROLLLINES setting and the
// viewport, and gets back a new pan plus a page-flip verdict.
//
// UNITS AND SIGN. pan_y is in canvas DIPs and lives in [vp_h - content_h, 0],
// with 0 at the page's TOP (the top-left origin PR-A1 introduced; see
// ViewportMath.hpp). Wheel UP is delta > 0 and moves the reader toward the top,
// so step > 0 and pan_y increases toward 0. This matches the arrow keys:
// VK_UP calls pan_by(0, +100).

#include "ui/detail/ViewportMath.hpp"

namespace litepdf::ui {

// WHEEL_DELTA, spelled out so this header stays Win32-free.
inline constexpr int kWheelDelta = 120;

// One "line" of wheel scroll in DIPs. Chosen so the Windows default of 3 lines
// gives 48 DIP per notch -- a little under half the 100 DIP arrow-key step, so
// the wheel feels finer than the keyboard rather than coarser.
inline constexpr float kWheelLineDip = 16.0f;

// SPI_GETWHEELSCROLLLINES returns this sentinel when the user has chosen
// "One screen at a time". Spelled out for the same reason as kWheelDelta.
inline constexpr unsigned kWheelPageScroll = 0xFFFFFFFFu;

enum class Flip { None, Next, Prev };

// Fold `delta` into `residual` and return the number of WHOLE notches now
// available, leaving the remainder in `residual`.
//
// High-resolution wheels and precision touchpads deliver |delta| < WHEEL_DELTA,
// often 10-40 at a time. Without accumulation each of those would either be
// rounded to a full notch (absurdly fast) or dropped (dead wheel). Integer
// division truncates toward zero, which is exactly right here: the residual
// keeps the sign of the motion, so a reversal cancels rather than compounds.
inline int consume_notches(int delta, int& residual) noexcept {
    residual += delta;
    const int notches = residual / kWheelDelta;
    residual -= notches * kWheelDelta;
    return notches;
}

// Scroll magnitude in DIPs for `notches` whole notches. Positive = toward the
// top of the page.
inline float wheel_step_dip(int notches, unsigned lines_per_notch,
                            float vp_h) noexcept {
    if (notches == 0) return 0.0f;
    float per_notch;
    if (lines_per_notch == kWheelPageScroll) {
        // "One screen at a time". A full viewport height would land exactly on
        // the far edge, so the very next notch would flip the page with no
        // overlap for the reader to reacquire their place. 90% leaves a strip.
        per_notch = vp_h * 0.9f;
    } else if (lines_per_notch == 0) {
        return 0.0f;                     // the user turned wheel scrolling off
    } else {
        per_notch = static_cast<float>(lines_per_notch) * kWheelLineDip;
    }
    return static_cast<float>(notches) * per_notch;
}

struct WheelResult {
    float pan_y;
    Flip  flip;
};

// Apply one accumulated step. Returns the new pan, or a Flip when the page is
// already at the edge the step pushes toward.
//
// A step that merely REACHES an edge scrolls and does not flip; the flip is the
// following notch. That gives the reader a natural stop at each page boundary
// instead of skating past it.
inline WheelResult apply_wheel(float pan_y, float content_h, float vp_h,
                               float step) noexcept {
    WheelResult r{pan_y, Flip::None};
    if (step == 0.0f) return r;

    if (!(content_h > vp_h)) {
        // The page fits: there is nothing to scroll, so one notch is one page.
        r.flip = (step < 0.0f) ? Flip::Next : Flip::Prev;
        return r;
    }

    const float lo = vp_h - content_h;      // negative; the bottom of the range
    const bool at_bottom = !(pan_y > lo);
    const bool at_top    = !(pan_y < 0.0f);
    if (step < 0.0f && at_bottom) { r.flip = Flip::Next; return r; }
    if (step > 0.0f && at_top)    { r.flip = Flip::Prev; return r; }

    r.pan_y = clamp_pan(pan_y + step, content_h, vp_h);
    return r;
}

}  // namespace litepdf::ui
```

- [ ] **Step 4: Run tests to verify they pass**

```bash
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --config Release
```

then

```bash
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" --test-dir build -C Release -R ScrollMath
```

Expected: 12 tests, 12 passed. Then the full suite — expected **285/285 passed**
(273 + 12).

- [ ] **Step 5: Commit**

```bash
git add src/ui/detail/ScrollMath.hpp tests/unit/test_scroll_math.cpp tests/CMakeLists.txt
git commit -m "feat(canvas): add pure wheel-scroll stepping, residuals and edge detection"
```

---

## Task 8: wire the wheel into the canvas

**Files:**
- Modify: `src/ui/PdfCanvas.hpp` — include `ScrollMath.hpp`; declare `on_wheel_scroll`
- Modify: `src/ui/PdfCanvas.cpp` — `wheel_residual` in `Impl`; the non-Ctrl branch
  of `WM_MOUSEWHEEL` (`:468-488`)

**Interfaces:**
- Consumes: `consume_notches`, `wheel_step_dip`, `apply_wheel`, `Flip` (Task 7);
  `navigate_to_page`, `PageAnchor` (Task 4); `dual_page_step_next_left` /
  `dual_page_step_prev_left` (existing, `src/ui/PdfCanvasLayout.hpp:49-65`).
- Produces: nothing other tasks depend on.

**The Ctrl branch is not touched.** `src/ui/PdfCanvas.cpp:471-486` already routes
Ctrl+wheel through `resubmit_current_page()` — PR-A1 pulled spec §3.5 forward. Only
the `DefWindowProcW` fallthrough at `:487` is replaced.

**Landing positions.** Forward flip lands at the new page's **top**; backward flip
lands at the previous page's **bottom**, so scrolling back and forth shows
continuous content rather than skipping the part you just read. In spread mode the
flip steps by spread, using the same helpers PgDn/PgUp use.

- [ ] **Step 1: Declare the handler**

In `src/ui/PdfCanvas.hpp`, add after the `PageAnchor.hpp` include:

```cpp
#include "ui/detail/ScrollMath.hpp"
```

and in the private section, after `LRESULT pan_by(float dx, float dy);`:

```cpp
    // Plain (unmodified) mouse-wheel scrolling. Scrolls within the page, and
    // flips to the neighbouring page or spread once the pan is already at the
    // edge the wheel is pushing toward.
    LRESULT on_wheel_scroll(int delta);
```

- [ ] **Step 2: Run the build to verify it fails**

```bash
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --config Release
```

Expected: FAIL — unresolved external `PdfCanvas::on_wheel_scroll`.

- [ ] **Step 3: Add the residual accumulator to `Impl`**

In `struct PdfCanvas::Impl`, after the `anchor` member added in Task 4:

```cpp
    // Leftover wheel delta below one full notch. High-resolution wheels and
    // precision touchpads deliver |delta| < WHEEL_DELTA; without this the
    // canvas would either round every fragment up to a full notch or drop it.
    // See ui/detail/ScrollMath.hpp.
    int                           wheel_residual = 0;
```

- [ ] **Step 4: Implement `on_wheel_scroll`**

Insert after `PdfCanvas::pan_by` (`src/ui/PdfCanvas.cpp:752`):

```cpp
LRESULT PdfCanvas::on_wheel_scroll(int delta) {
    if (!impl_ || !impl_->view) return 0;

    const int notches = consume_notches(delta, impl_->wheel_residual);
    if (notches == 0) return 0;   // a fractional notch is never a page flip

    ContentBox box{};
    if (!content_extent(box)) return 0;   // nothing rendered yet
    const D2D1_SIZE_F vp = impl_->rt->GetSize();

    UINT lines = 3;   // the Windows default, and the value if the query fails
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    const float step = wheel_step_dip(notches, lines, vp.height);

    const WheelResult r = apply_wheel(impl_->pan_y, box.h, vp.height, step);
    if (r.flip == Flip::None) {
        impl_->pan_y = r.pan_y;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }

    // At the edge: turn the page. Forward lands at the new page's top;
    // backward lands at the previous page's BOTTOM, so scrolling back shows
    // the content the reader just scrolled past instead of skipping it.
    const int cur     = impl_->view->current_page();
    const int total   = impl_->view->page_count();
    const int max_idx = total - 1;
    int target;
    if (impl_->dual_page) {
        const int cur_left = dual_page_compute_left(cur, total);
        target = (r.flip == Flip::Next)
                     ? dual_page_step_next_left(cur_left, total)
                     : dual_page_step_prev_left(cur_left, total);
    } else {
        target = (r.flip == Flip::Next) ? std::min(cur + 1, max_idx)
                                        : std::max(cur - 1, 0);
    }
    // At the first or last page the step clamps to where we already are, so
    // navigate_to_page would still re-render for a Bottom anchor. Bail instead
    // -- the document has no more pages and the pan is already at the edge.
    if (target == cur) return 0;

    navigate_to_page(target, (r.flip == Flip::Next) ? PageAnchor::top()
                                                    : PageAnchor::bottom());
    return 0;
}
```

Add the using-declarations alongside the existing ones in the anonymous namespace
at `src/ui/PdfCanvas.cpp:41-46`:

```cpp
using litepdf::ui::apply_wheel;
using litepdf::ui::consume_notches;
using litepdf::ui::Flip;
using litepdf::ui::wheel_step_dip;
using litepdf::ui::WheelResult;
```

- [ ] **Step 5: Replace the `DefWindowProcW` fallthrough**

In the `WM_MOUSEWHEEL` case, replace `src/ui/PdfCanvas.cpp:487`:

```cpp
            return DefWindowProcW(hwnd_, WM_MOUSEWHEEL, w, l);
```

with:

```cpp
            return on_wheel_scroll(GET_WHEEL_DELTA_WPARAM(w));
```

- [ ] **Step 6: Build and run the full suite**

```bash
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --config Release
```

then

```bash
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" --test-dir build -C Release
```

Expected: **285/285 passed**.

- [ ] **Step 7: Commit**

```bash
git add src/ui/PdfCanvas.hpp src/ui/PdfCanvas.cpp
git commit -m "feat(canvas): scroll with the mouse wheel and flip pages at the edges"
```

---

## Task 9: restore FitWidth, update the CHANGELOG, verify on screen

**Files:**
- Modify: `src/core/DocumentView.cpp:48-53` (`Impl::zm`)
- Modify: `src/core/SessionState.cpp:355-373` (`migrate_v1_to_v2`)
- Modify: `src/ui/MainWindow.cpp:904-929` (`restore_on_tab_ready`'s zoom switch),
  `:1458-1470` (`IDM_ZOOM_RESET`)
- Modify: `CHANGELOG.md`

**Interfaces:**
- Consumes: `DocumentView::set_zoom_mode_fit_width()`
  (`src/core/DocumentView.cpp:234-237`) — this task is what revives it. It has had
  zero call sites in `src/` since PR-A1, which dead-code scanners flag.
- Produces: nothing.

**Why this comes last.** PR-A1 set the default to FitPage because with the paint
path drawing at natural size and no wheel scrolling, a FitWidth A4 page stands
about 2.8× taller than a maximized 16:9 viewport and the lower two thirds are
simply unreachable. Task 8 makes it reachable. Flipping the default before Task 8
lands would ship an unnavigable build at every intermediate commit.

- [ ] **Step 1: Restore the default zoom mode**

In `src/core/DocumentView.cpp:48-53`, replace:

```cpp
    int                    current_page = 0;
    // PR-A1 ships FitPage so the default view still shows a whole page: with
    // the paint path now drawing at natural size, FitWidth overflows the
    // viewport vertically and this PR has no wheel scrolling to navigate it.
    // PR-A2 restores FitWidth when ScrollMath lands.
    DocumentView::ZoomMode zm           = DocumentView::ZoomMode::FitPage;
```

with:

```cpp
    int                    current_page = 0;
    // FitWidth: page width fills the canvas, and the overflow below the fold is
    // reached with the wheel (PR-A2) or the arrow keys. PR-A1 shipped FitPage
    // as an interim default precisely because it had no wheel scrolling, which
    // left the lower two thirds of an A4 page unreachable; PR-A2's ScrollMath
    // is what makes FitWidth navigable again.
    DocumentView::ZoomMode zm           = DocumentView::ZoomMode::FitWidth;
```

- [ ] **Step 2: Stop forcing migrated sessions into FitPage**

In `src/core/SessionState.cpp:355-373`, replace the whole `migrate_v1_to_v2`
function with:

```cpp
void migrate_v1_to_v2(SessionState& s) {
    // v1 and v2 differ in what zoom_scale MEANS, not in which modes exist: v1
    // stored a render scale (PDF point -> pixel, DPI folded in), v2 stores a
    // magnification (1.0 = one point per DIP). A v1 Custom value is therefore
    // uninterpretable and its tab is reset to a fit mode; the fit modes carry
    // over untouched because they are re-derived from the viewport on restore.
    //
    // PR-A1 reset Custom to FitPage because that release had no wheel
    // scrolling and a FitWidth A4 page was unnavigable below the fold. PR-A2
    // ships the wheel, so the reset target returns to FitWidth -- the mode
    // v1.2.0 actually persisted, and this build's default.
    for (auto& t : s.tabs) {
        if (t.zoom_mode == SessionZoom::Custom) {
            t.zoom_mode  = SessionZoom::FitWidth;
            t.zoom_scale = 1.0f;
        }
    }
    s.version = 2;
}
```

- [ ] **Step 3: Honour the persisted fit mode on restore**

In `src/ui/MainWindow.cpp`, replace the `FitWidth`/`FitPage` case of
`restore_on_tab_ready`'s switch (`:908-928`) with:

```cpp
            case litepdf::core::SessionZoom::FitWidth:
            case litepdf::core::SessionZoom::FitPage: {
                // Honour what the file says. PR-A1 collapsed both onto FitPage
                // because that release could not navigate a FitWidth page
                // below the fold; PR-A2's wheel scrolling removes the reason.
                if (st.zoom_mode == litepdf::core::SessionZoom::FitWidth) {
                    v->set_zoom_mode_fit_width();
                } else {
                    v->set_zoom_mode_fit_page();
                }
                RECT rc; GetClientRect(canvas_->hwnd(), &rc);
                const UINT dpi = GetDpiForWindow(hwnd_);
                v->set_viewport(static_cast<float>(rc.right - rc.left),
                                static_cast<float>(rc.bottom - rc.top),
                                static_cast<float>(dpi));
                break;
            }
```

- [ ] **Step 4: Point Reset Zoom back at FitWidth**

In `src/ui/MainWindow.cpp:1460`, replace:

```cpp
                        view->set_zoom_mode_fit_page();
```

with:

```cpp
                        // Ctrl+0 returns to the app default, which is FitWidth
                        // again now that the wheel can reach the overflow.
                        // The View menu ships no Fit Width / Fit Page items, so
                        // this is the only mode control the user has.
                        view->set_zoom_mode_fit_width();
```

- [ ] **Step 5: Confirm the session header default is now correct**

```bash
grep -n "zoom_mode = SessionZoom" src/core/SessionState.hpp
```

Expected: `SessionZoom zoom_mode = SessionZoom::FitWidth;` at line 22. This
matches the app default again, so it needs no edit — the PR-A1 handoff flagged it
only because FitWidth was temporarily not the default. Do not change it.

- [ ] **Step 6: Update the CHANGELOG**

`CHANGELOG.md` already has an `## [Unreleased]` section (line 9) holding PR-A1's
entries, and one of them is a claim **this PR invalidates**:

```markdown
- Every view is Fit Page in this release — including restored and migrated sessions;
  Fit Width returns with mouse-wheel scrolling.
```

That is lines 17-18. **Delete those two lines** — Fit Width has now returned, so
leaving them would ship a release note that contradicts the build. Then merge the
entries below into the existing section: add an `### Added` block above the
existing `### Changed`, append the two `### Changed` bullets to the existing
`### Changed` list, and add a `### Fixed` block after it. Leave the `### Note`
block about `session.json` version 2 exactly as it is — that migration is still
what ships.

```markdown
### Added
- Mouse-wheel scrolling. The wheel scrolls within a page and turns the page when
  it is already at the edge — forward lands at the top of the next page, backward
  at the bottom of the previous one, so scrolling back and forth shows continuous
  content. Honours the system "lines per notch" setting, including "one screen at
  a time", and accumulates sub-notch deltas from high-resolution wheels and
  precision touchpads. In two-page spread mode the flip steps by spread.

### Changed
- The default zoom mode is Fit Width again, and Reset Zoom (Ctrl+0) returns to it.
  v1.2.0 shipped Fit Page as an interim default because that release drew pages at
  natural size but had no way to scroll below the fold. Restored sessions now
  honour the fit mode they recorded instead of collapsing onto Fit Page.
- A render result now carries the page, slot and submission it belongs to, so a
  pixmap for a page the reader has already left is dropped instead of painted.

### Fixed
- Scroll position survives a re-render. Panning to the middle of a page and then
  zooming, resizing the window, toggling a side pane or toggling Invert Colors no
  longer snaps the view back to the top of the page.
- Per-tab scroll position is restored on tab switch. The position was captured and
  handed back correctly, then immediately discarded by the next render completion.
- Search navigation centres the hit against the page it actually landed on. The
  scroll was previously computed from the outgoing page's height, which put the
  hit off-screen on a tall page.
- Switching between two tabs that are both in two-page spread mode no longer shows
  the previous document's right-hand page until a new render arrives.
- The thumbnail highlight tracks the current page in spread mode, after a session
  restore, and after End in spread mode. Four navigation paths bypassed the
  page-change notification.
```

- [ ] **Step 7: Build, run the full suite, and check the size gate**

```bash
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --config Release
```

then

```bash
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" --test-dir build -C Release
```

Expected: **285/285 passed**.

The CI benchmark job hard-gates the exe at 19,000,000 bytes absolute. Three
header-only additions and a handful of methods must not move it meaningfully, but
check rather than assume:

```bash
ls -l build/src/Release/litepdf.exe
```

Expected: comfortably under 19,000,000. Record the number in the PR description.

- [ ] **Step 8: GUI verification**

Spec §5's A2 GUI checks. Screen capture **cannot see this app's Direct2D canvas**
(established during PR-A1 by seven cross-checks) — so drive the app and read state
back with in-process probes or `session.json`, and use the user's eyes for the
purely visual items. Launch the Release exe with
`tests/fixtures/large.pdf`, maximize, then:

1. **Pan survives a same-page re-render.** Scroll to mid-page, then in turn:
   resize the window, toggle F4 (outline pane), toggle Ctrl+Shift+I (invert), press
   Ctrl+`=`. After each, the page must still be where it was — not snapped to the
   top. This is the check the whole anchor design exists for.
2. **Per-tab scroll survives a tab switch.** Open two documents, scroll each to a
   different position, switch back and forth. Each tab returns to its own position.
3. **Spread renders both halves.** Ctrl+Shift+D, page to spread (3,4). Both halves
   must appear — if `accept_completion` were wrong about slots, the right half
   would be the grey placeholder permanently.
4. **Ctrl+wheel in spread mode leaves both halves at the same magnification.**
5. **Switching between two spread tabs never shows the other document's right
   page.**
6. **Wheel scrolls and flips at both edges with the correct landing.** Scroll to
   the bottom of a page, one more notch flips forward and lands at the next page's
   top; scroll back up, one more notch flips backward and lands at the previous
   page's **bottom**. On a page that fits entirely, one notch is one page.
7. **The default view is Fit Width again** — the page width fills the canvas and
   the page overflows below the fold, reachable with the wheel.

`tests/fixtures/large.pdf` page 1 is 0.105 % ink, which the PR-A1 handoff flagged
as a weak visual fixture for exactly this QA. Use a text-dense document for the
wheel checks — any of `tests/fixtures/` with real body text, or a document the user
supplies.

- [ ] **Step 9: Commit**

```bash
git add src/core/DocumentView.cpp src/core/SessionState.cpp src/ui/MainWindow.cpp CHANGELOG.md
git commit -m "feat(zoom): restore Fit Width as the default now that the wheel can scroll"
```

---

## Definition of done

- [ ] `ctest --test-dir build -C Release` from the repo root: **285/285 passed**.
- [ ] `grep -n "set_current_page" src/ui/MainWindow.cpp src/ui/PdfCanvas.cpp`
      shows no `view->set_current_page` bypass (Task 5 Step 7).
- [ ] `grep -rn "set_zoom_mode_fit_width" src/` shows at least one call site — the
      function is no longer dead (Task 9).
- [ ] `grep -n "pan_x = 0.0f" src/ui/PdfCanvas.cpp` shows the reset only in
      `set_dual_page`, never in the completion handler.
- [ ] `grep -n "Every view is Fit Page" CHANGELOG.md` returns nothing — PR-A1's
      now-false release note is gone (Task 9 Step 6).
- [ ] `VERSION` is unchanged at `1.2.0`.
- [ ] `build/src/Release/litepdf.exe` is under 19,000,000 bytes.
- [ ] The Task 9 Step 8 GUI checks are done, with item 1 (pan survives a re-render)
      and item 6 (wheel flip landings) confirmed by a human looking at the running
      exe — no instrument on this machine can see the composited canvas.
- [ ] `git log --oneline main..HEAD` shows nine commits, one per task.
- [ ] The PR runs the **Full-tier** risk-tiered review stack before merge (spec §6.4:
      all three PRs modify shipped behaviour). Invoke the `risk-tiered-review`
      skill; do not classify the tier by hand.

## Known residuals this PR does NOT close

State these in the PR description so a reviewer does not report them as misses.

1. **`cancel_all_below_priority` still cancels only `priority > p`**, so
   `cancel_stale_renders(0)` does not cancel an in-flight P0 and
   `cancel_stale_renders(INT_MAX)` (`MainWindow.cpp:594`) remains the documented
   no-op of spec §7. Wasted work, not incorrectness — `seq` makes the duplicate
   completions distinguishable. See "Scope rulings", ruling 1.
2. **The search-hit overlay stays disabled in two-page spread mode** (R17). Task 6
   removes the clamp mismatch that R17 was hiding, but enabling the overlay is its
   own change.
3. **`PageCache` L1 evicts by entry count (5), not bytes.** A top-rung A4 pixmap on
   a 200 % display is ~513 MB. Not a regression, and not a spec §3 requirement.
4. **The unequal-spread clip corner case** documented at `PdfCanvas.cpp:951-955`:
   when the left page fits its slot but the right does not, the per-slot clip hides
   a strip of the right page. Unreachable at every pan value because the pan is
   clamped once against the union. Inherent to the fixed-band layout; per-slot
   scrolling would be its own design.
5. **`navigate_to_page`, `apply_anchor` and `on_wheel_scroll` have no headless
   test** — they live in `PdfCanvas`, which is exe-only. The three pure headers
   they are built from are covered by 28 new unit tests; what is untested is the
   wiring, which is what the Task 9 Step 8 GUI checks exercise.

## Self-review notes

Checked against spec §3 with fresh eyes after writing:

- §3.1 completion identity → Tasks 1 and 3. The `cancel_all_below_priority` and
  null-completion bullets are answered by scope rulings 1 and 2 rather than by code,
  with the reasoning stated.
- §3.2 page anchors → Tasks 2 and 4. All four anchor kinds, the seq-keyed lifetime,
  the `set_view` clear, and the "epoch-mismatch and null completions leave it
  alone" rule are implemented and tested.
- §3.3 the four bypass sites → Task 5, all four, plus the pending-`Hit` protection.
- §3.4 search navigation → Task 6, including `on_results_row_click`.
- §3.5 Ctrl+wheel → already shipped in PR-A1; scope ruling 5.
- §3.6 wheel scrolling → Tasks 7 and 8, including the residual accumulator, the
  `WHEEL_PAGESCROLL` degradation, both flip directions with their landing anchors,
  the fits-entirely case, and the spread stepping. FitWidth restored in Task 9.
- §5 A2 unit tests → `accept_completion` (Task 1, 8 cases), anchor lifetime
  (Task 2, 8 cases), `ScrollMath` (Task 7, 12 cases).
- §5 A2 GUI checks → Task 9 Step 8, all six spec items plus the FitWidth default.

Type consistency: `PageAnchor::Kind` is spelled the same in every task;
`next_render_seq()` is declared in Task 3 and redefined once in Task 4 (stated
explicitly at that step); `pan_y_for_hit` is declared in Task 4 with a provisional
body and replaced wholesale in Task 6 (also stated). `content_extent`'s constness
is flagged as a verify-at-implementation point in Task 6 Step 1 rather than assumed.
