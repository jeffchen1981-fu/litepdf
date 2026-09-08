# Zoom Correctness and Unit Contract (PR-A1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make zoom actually change what the user sees, by separating the user-facing
magnification from the point→pixel render scale, applying DPI exactly once, drawing
bitmaps at natural size, and migrating `session.json` to the new zoom semantics.

**Architecture:** `DocumentView` gains `zoom_pct_` (magnification, 1.0 = one PDF point
per DIP) and derives `render_scale() = zoom_pct_ × dpi/96` for MuPDF. A new pure-logic
header `ui/detail/ViewportMath.hpp` owns all placement and pan-clamping math, replacing
three copies of a shrink-to-fit calculation in `PdfCanvas`. The Direct2D render target
is created at the window's DPI so canvas geometry has one unit. `session.json` goes to
version 2 with a v1-accepting migration and a fail-closed backup.

This PR **deliberately does not touch any render-completion code**: the pan reset in
`WM_USER_RENDER_DONE` (`src/ui/PdfCanvas.cpp:547-553`) is left exactly as it is. All
completion-identity and page-anchor work belongs to PR-A2.

**Tech Stack:** C++20, Win32, Direct2D, MuPDF (static), Catch2 v3.5.4, CMake +
Visual Studio 17 2022, x64.

## Global Constraints

- **Spec:** `docs/superpowers/specs/2026-09-07-zoom-correctness-page-nav-design.md`
  (committed at `dfddc7a`). Section references below point into it.
- **Branch:** work continues on `spec/zoom-correctness-page-nav`.
- **Build config is Release, never Debug.** MuPDF's static libs are `MT_StaticRelease`;
  a Debug test build fails with a flood of `LNK2038: RuntimeLibrary mismatch`.
- **cmake/ctest are not on PATH.** Use
  `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`
  and the sibling `ctest.exe`.
- **Catch2 `TEST_CASE` names must be pure ASCII.** Non-ASCII names mangle under
  `catch_discover_tests` on Windows and produce "No test cases matched" in CI only.
- **Verify through `ctest`, not just the test executable** — the same CI-only failure
  mode.
- **`ctest -R` matches TEST_CASE NAMES, not Catch2 tags.** `catch_discover_tests`
  registers one ctest test per name from `--list-test-names-only`
  (`build/_deps/catch2-src/extras/CatchAddTests.cmake:129`, `set(test_name "${test}")`),
  and no `TEST_PREFIX` is configured (`tests/CMakeLists.txt:91`). A filter written
  against a tag word silently matches nothing and `ctest` reports
  "No tests were found!!!" — which reads like a pass if you only check the exit
  banner. Every new TEST_CASE in this plan therefore carries a stable name prefix
  (`ViewportMath`, `ZoomMath`, `SessionState v2`, `SessionStore backup`) and every
  `-R` below filters on that prefix.
- **A `-R` run is never the final check for a task.** It is the fast inner loop; the
  full `ctest --test-dir build -C Release` is what closes a task, because a filter
  cannot show you what your change broke elsewhere.
- **Run tests from the repo root** so fixtures resolve (`catch_discover_tests`
  `WORKING_DIRECTORY` is `CMAKE_SOURCE_DIR`).
- **No `VERSION` bump in this PR.** Version bumps happen at phase boundaries only.
- **All project artifacts in English** — code, comments, commit messages, test names.
- **Green baseline, measured on this tree at `dfddc7a` on 2026-09-08** — not quoted
  from memory, which still carried a stale pre-Phase-12 figure:
  - `ctest --test-dir build -C Release` from the repo root → **230/230 passed, 0
    failed**, ~17 s.
  - `build\tests\Release\litepdf_unit_tests.exe` with no arguments → **899 assertions
    in 229 test cases**, all passed.

  The two counts differ by one because `catch_discover_tests` registers every name
  from `--list-test-names-only`, including the `[!shouldfail]` case, while a bare exe
  run reports it differently. Both numbers are the "before" for every task below: if a
  task's full-suite run lands on anything other than 230 + the cases that task adds,
  something outside the task changed.

---

## File Structure

| File | Responsibility |
|------|----------------|
| `src/ui/detail/ViewportMath.hpp` | **new.** Pure placement, pan-clamp and PDF-point→DIP math. No UI types. |
| `tests/unit/test_viewport_math.cpp` | **new.** Unit tests for the above. |
| `scripts/generate-spread-fixture.py` + `tests/fixtures/spread-unequal.pdf` | **new.** Two pages of different sizes, so the spread fit can be tested through `DocumentView` rather than only as pure arithmetic. |
| `src/core/detail/ZoomMath.hpp` | **new.** Pure fit-percentage and preset-ladder math. Lives under `core/` so `DocumentView` does not include a `ui/` header. |
| `tests/unit/test_zoom_math.cpp` | **new.** Unit tests for the above, including an unequal-page spread. |
| `src/core/DocumentView.hpp/.cpp` | `zoom_pct_` / `render_scale()` split, `set_viewport`, extended ladder. |
| `src/ui/PdfCanvas.cpp` | Render-target DPI, natural-size paint (single + dual), arrow-key clamping, overlay unit fix. |
| `src/ui/MainWindow.cpp` | Four `set_zoom_mode` call sites → `set_viewport`; dual-mode fit ordering. |
| `src/core/SessionState.hpp/.cpp` | Version 2, versionless-as-v1, v1→v2 migration. |
| `src/core/SessionStore.cpp` | Fail-closed `session.v1.bak` before the first v2 write. |
| `tests/unit/test_document_view.cpp` | Existing zoom tests updated for the new API and ladder. |
| `tests/unit/test_session_state.cpp` | Four v1-touching sites updated (spec §2.5 table). |
| `tests/CMakeLists.txt` | Register the new test file. |
| `CHANGELOG.md` | One-way-door note for the session downgrade. |

---

## Task 1: ViewportMath pure helpers

**Files:**
- Create: `src/ui/detail/ViewportMath.hpp`
- Create: `tests/unit/test_viewport_math.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces: `litepdf::ui::bitmap_px_to_dip(float px, float rt_dpi) -> float`;
  `litepdf::ui::clamp_pan(float pan, float content, float viewport) -> float`;
  `struct litepdf::ui::Placement { float x, y, w, h; }`;
  `litepdf::ui::place_bitmap(float src_w, float src_h, float vp_w, float vp_h, float pan_x, float pan_y) -> Placement`;
  `litepdf::ui::pdf_point_to_dip(float pt, float zoom_pct) -> float`.
  Task 4 consumes all five.

- [ ] **Step 1: Write the failing tests**

Create `tests/unit/test_viewport_math.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "ui/detail/ViewportMath.hpp"

using litepdf::ui::bitmap_px_to_dip;
using litepdf::ui::clamp_pan;
using litepdf::ui::place_bitmap;

TEST_CASE("ViewportMath bitmap px to dip divides by the render target dpi ratio",
          "[ui][viewport]") {
    REQUIRE(bitmap_px_to_dip(1000.0f, 96.0f)  == Catch::Approx(1000.0f));
    REQUIRE(bitmap_px_to_dip(1000.0f, 144.0f) == Catch::Approx(666.6667f).epsilon(0.001));
    REQUIRE(bitmap_px_to_dip(1000.0f, 192.0f) == Catch::Approx(500.0f));
    // Defensive: a zero or negative dpi must not divide by zero.
    REQUIRE(bitmap_px_to_dip(1000.0f, 0.0f)   == Catch::Approx(1000.0f));
}

TEST_CASE("ViewportMath clamp pan centers content that fits", "[ui][viewport]") {
    // Content no larger than the viewport is centered by place_bitmap, so its
    // pan is pinned to zero regardless of what the caller asks for.
    REQUIRE(clamp_pan(  0.0f, 400.0f, 800.0f) == Catch::Approx(0.0f));
    REQUIRE(clamp_pan(-250.0f, 400.0f, 800.0f) == Catch::Approx(0.0f));
    REQUIRE(clamp_pan( 250.0f, 800.0f, 800.0f) == Catch::Approx(0.0f));
}

TEST_CASE("ViewportMath clamp pan uses a top left origin when content overflows",
          "[ui][viewport]") {
    // content 1600, viewport 800 -> valid range [-800, 0].
    REQUIRE(clamp_pan(   0.0f, 1600.0f, 800.0f) == Catch::Approx(0.0f));
    REQUIRE(clamp_pan(-400.0f, 1600.0f, 800.0f) == Catch::Approx(-400.0f));
    REQUIRE(clamp_pan(-800.0f, 1600.0f, 800.0f) == Catch::Approx(-800.0f));
    REQUIRE(clamp_pan(-999.0f, 1600.0f, 800.0f) == Catch::Approx(-800.0f));
    REQUIRE(clamp_pan( 120.0f, 1600.0f, 800.0f) == Catch::Approx(0.0f));
}

TEST_CASE("ViewportMath place bitmap keeps natural size for every viewport",
          "[ui][viewport]") {
    // This is the direct regression assertion for the shipped shrink-to-fit
    // defect: the destination extent must equal the source extent whether the
    // viewport is larger, smaller, or equal.
    const auto bigger  = place_bitmap(400.0f, 500.0f, 1000.0f, 900.0f, 0.0f, 0.0f);
    REQUIRE(bigger.w  == Catch::Approx(400.0f));
    REQUIRE(bigger.h  == Catch::Approx(500.0f));

    const auto smaller = place_bitmap(2000.0f, 3000.0f, 1000.0f, 900.0f, 0.0f, 0.0f);
    REQUIRE(smaller.w == Catch::Approx(2000.0f));
    REQUIRE(smaller.h == Catch::Approx(3000.0f));

    const auto equal   = place_bitmap(1000.0f, 900.0f, 1000.0f, 900.0f, 0.0f, 0.0f);
    REQUIRE(equal.w   == Catch::Approx(1000.0f));
    REQUIRE(equal.h   == Catch::Approx(900.0f));
}

TEST_CASE("ViewportMath place bitmap centers a fitting axis and top aligns an overflowing one",
          "[ui][viewport]") {
    // Width fits (400 <= 1000) -> centered at (1000-400)/2 = 300.
    // Height overflows (3000 > 900) -> top-left origin, pan applied and clamped.
    const auto p = place_bitmap(400.0f, 3000.0f, 1000.0f, 900.0f, 55.0f, -500.0f);
    REQUIRE(p.x == Catch::Approx(300.0f));
    REQUIRE(p.y == Catch::Approx(-500.0f));

    // Pan zero on the overflowing axis means the content top sits at the
    // viewport top -- not centered, which is what the old origin would give.
    const auto top = place_bitmap(400.0f, 3000.0f, 1000.0f, 900.0f, 0.0f, 0.0f);
    REQUIRE(top.y == Catch::Approx(0.0f));

    // Bottom-aligned: pan = viewport - content.
    const auto bottom = place_bitmap(400.0f, 3000.0f, 1000.0f, 900.0f, 0.0f, -2100.0f);
    REQUIRE(bottom.y == Catch::Approx(-2100.0f));
    REQUIRE(bottom.y + bottom.h == Catch::Approx(900.0f));
}

TEST_CASE("ViewportMath pdf point to dip mapping is dpi invariant", "[ui][viewport]") {
    using litepdf::ui::pdf_point_to_dip;
    // The search-hit overlay draws in DIPs. One PDF point is exactly zoom_pct
    // DIPs -- the pixmap is page_pt * render_scale PIXELS, and converting that
    // to DIPs divides the dpi factor back out. So the mapping must not mention
    // dpi at all, and this asserts the formula stayed that way: the shipped
    // code multiplied by a render scale, and an earlier draft of the spec
    // prescribed doing so again, which would double every hit rectangle at
    // 200% scaling.
    REQUIRE(pdf_point_to_dip(100.0f, 1.5f) == Catch::Approx(150.0f));
    REQUIRE(pdf_point_to_dip(100.0f, 1.0f) == Catch::Approx(100.0f));
    // What this CANNOT test, stated plainly so nobody mistakes it for covered:
    // the regression that matters is a caller in PdfCanvas reaching for
    // render_scale() instead of zoom_pct(). This function's signature has no
    // dpi parameter, so no test of it can observe that choice. The guard for
    // the caller is the Task 7 GUI check, item 0.
    //
    // What this DOES pin: the mapping is linear in the percentage and has no
    // hidden dpi term of its own.
    REQUIRE(pdf_point_to_dip(72.0f, 2.0f)
            == Catch::Approx(2.0f * pdf_point_to_dip(72.0f, 1.0f)));
}
```

- [ ] **Step 2: Register the test file**

In `tests/CMakeLists.txt`, inside the existing `target_sources(litepdf_unit_tests PRIVATE ...)` list, add after the `unit/test_dual_page_layout.cpp` line:

```cmake
    unit/test_viewport_math.cpp        # PR-A1 Task 1
```

- [ ] **Step 3: Run the tests to verify they fail**

```bash
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --target litepdf_unit_tests --config Release
```

Expected: build FAILS with `cannot open include file: 'ui/detail/ViewportMath.hpp'`.

- [ ] **Step 4: Write the header**

Create `src/ui/detail/ViewportMath.hpp`:

```cpp
#pragma once

// PR-A1: pure placement + pan math for PdfCanvas. No Win32, no Direct2D, no
// MuPDF -- so it is unit-testable headless, the same pattern as SplitterMath.hpp
// and PdfCanvasLayout.hpp.
//
// UNITS. Everything here is in render-target DIPs. Bitmaps are created at 96 DPI
// (D2D1::BitmapProperties defaults dpiX/dpiY to 96.0f), so ID2D1Bitmap::GetSize()
// reports PIXELS, while the render target is created at the window's DPI and
// ID2D1RenderTarget::GetSize() reports DIPs. bitmap_px_to_dip is the one
// conversion point; call it on the bitmap extent before anything else here.

namespace litepdf::ui {

// Bitmap pixel extent -> render-target DIPs.
inline float bitmap_px_to_dip(float px, float rt_dpi) noexcept {
    if (!(rt_dpi > 0.0f)) return px;   // also rejects NaN
    return px * 96.0f / rt_dpi;
}

// Clamp one axis of the pan offset.
//
//   content <= viewport : the axis is centered by place_bitmap, so the pan is
//                         meaningless and pinned to 0.
//   content >  viewport : TOP-LEFT origin. pan 0 puts the content's leading
//                         edge at the viewport's leading edge; the valid range
//                         is [viewport - content, 0].
//
// NOTE this is a deliberate change from the shipped semantics, where pan was an
// offset from a CENTERED position (PdfCanvas.cpp:857-858 computed
// dx = (vp.width - dst_w) * 0.5f and added pan to it). Under that origin pan 0
// meant "centered", so on an overflowing axis the content's top sat above the
// viewport. No pan value is persisted -- SessionTab carries only path, page,
// zoom mode and zoom scale -- so the change needs no migration.
inline float clamp_pan(float pan, float content, float viewport) noexcept {
    if (!(pan == pan))          return 0.0f;   // NaN
    if (!(content > viewport))  return 0.0f;   // fits (or degenerate) -> centered
    const float lo = viewport - content;       // negative
    if (pan < lo)   return lo;
    if (pan > 0.0f) return 0.0f;
    return pan;
}

struct Placement {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

// Destination rect for a bitmap drawn at NATURAL SIZE -- w/h always equal the
// source extent, for every viewport. The shipped code instead scaled the
// destination down (and up) to fit the viewport unconditionally, which is why
// changing the render scale never changed what the user saw.
//
// An axis whose content fits is centered; an axis that overflows uses the
// top-left origin described on clamp_pan.
inline Placement place_bitmap(float src_w, float src_h,
                              float vp_w,  float vp_h,
                              float pan_x, float pan_y) noexcept {
    Placement p;
    p.w = src_w;
    p.h = src_h;
    p.x = (src_w > vp_w) ? clamp_pan(pan_x, src_w, vp_w)
                         : (vp_w - src_w) * 0.5f;
    p.y = (src_h > vp_h) ? clamp_pan(pan_y, src_h, vp_h)
                         : (vp_h - src_h) * 0.5f;
    return p;
}

// PDF points -> render-target DIPs for overlay geometry (search-hit quads).
//
// One PDF point is exactly zoom_pct DIPs. Note there is NO dpi term: the pixmap
// is page_pt * render_scale pixels, and converting that to DIPs divides the dpi
// factor straight back out. The shipped overlay multiplied by a render scale
// times the (now removed) fit ratio; using a render scale here would double
// every hit rectangle at 200% scaling.
inline float pdf_point_to_dip(float pt, float zoom_pct) noexcept {
    return pt * zoom_pct;
}

}  // namespace litepdf::ui
```

- [ ] **Step 5: Run the tests to verify they pass**

```bash
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --target litepdf_unit_tests --config Release
```

then, from the repo root:

```bash
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" --test-dir build -C Release -R ViewportMath --output-on-failure
```

Expected: 6 test cases pass.

- [ ] **Step 6: Commit**

```bash
git add src/ui/detail/ViewportMath.hpp tests/unit/test_viewport_math.cpp tests/CMakeLists.txt
git commit -m "feat(ui): ViewportMath natural-size placement and pan clamping (PR-A1 Task 1)"
```

---

## Task 2: Split zoom_pct from render_scale in DocumentView

**Files:**
- Create: `src/core/detail/ZoomMath.hpp`
- Create: `tests/unit/test_zoom_math.cpp`
- Create: `scripts/generate-spread-fixture.py`, `tests/fixtures/spread-unequal.pdf`
- Modify: `tests/CMakeLists.txt`
- Modify: `src/core/DocumentView.hpp:81-107`
- Modify: `src/core/DocumentView.cpp:47-58` (Impl fields, presets), `:185-282` (accessors, set_zoom_mode, set_zoom_scale, ladder), `:283-370` (render request scale)
- Modify: `tests/unit/test_document_view.cpp:70-165`

The fit and ladder arithmetic goes in `core/detail/ZoomMath.hpp` rather than inline
in `DocumentView.cpp` for one concrete reason: the unequal-page spread case cannot be
exercised through `DocumentView` without a fixture PDF whose two pages differ in size,
and no such fixture exists. As a free function it is testable with arbitrary page
dimensions. It lives under `core/` — not next to `ui/detail/ViewportMath.hpp` — so
`DocumentView` does not include a `ui/` header.

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces: `litepdf::core::fit_percentage(float vp_w_px, float vp_h_px, float dpi, float pw_pt, float ph_pt, bool fit_page) -> float`;
  `litepdf::core::next_preset(float pct) -> float`;
  `litepdf::core::prev_preset(float pct) -> float`;
  `litepdf::core::clamp_preset_span(float pct) -> float`;
  `DocumentView::zoom_pct() const noexcept -> float`;
  `DocumentView::render_scale() const noexcept -> float`;
  `DocumentView::set_viewport(float w_px, float h_px, float dpi, int pair_page = -1)`;
  `DocumentView::set_zoom_pct(float) noexcept`. `zoom_scale()` and `set_zoom_mode()`
  are **removed**, deliberately, so every call site becomes a compile error.
  Tasks 3, 4 and 5 consume these.

- [ ] **Step 1: Write the failing ZoomMath tests**

Create `tests/unit/test_zoom_math.cpp`:

```cpp
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/detail/ZoomMath.hpp"

using litepdf::core::clamp_preset_span;
using litepdf::core::fit_percentage;
using litepdf::core::next_preset;
using litepdf::core::prev_preset;

TEST_CASE("ZoomMath fit percentage derives dips per point from viewport pixels",
          "[core][zoom][math]") {
    // A4 is 595.276 x 841.89 pt. At 96 dpi one DIP is one device pixel, so a
    // 1190.552 px viewport is exactly 2.0x fit-width.
    REQUIRE(fit_percentage(1190.552f, 800.0f, 96.0f, 595.276f, 841.89f, false)
            == Catch::Approx(2.0f).epsilon(0.001));
}

TEST_CASE("ZoomMath fit percentage applies dpi exactly once", "[core][zoom][math]") {
    // The SAME physical viewport at 192 dpi: each DIP is two device pixels, so
    // the percentage halves while the resulting pixmap width in pixels is
    // unchanged. The shipped code applied dpi here AND at every caller, which
    // rendered pixmaps twice as wide as the canvas at 200% scaling.
    const float pct = fit_percentage(1190.552f, 800.0f, 192.0f,
                                     595.276f, 841.89f, false);
    REQUIRE(pct == Catch::Approx(1.0f).epsilon(0.001));
    const float render_scale = pct * (192.0f / 96.0f);
    REQUIRE(595.276f * render_scale == Catch::Approx(1190.552f).epsilon(0.001));
}

TEST_CASE("ZoomMath fit percentage at 144 dpi", "[core][zoom][math]") {
    // The middle rung of the spec's 96 / 144 / 192 sweep. At 150% scaling a
    // 1785.66 px viewport is 1190.44 DIP, so the same 2.0x as the 96 dpi case
    // -- and the pixmap comes out 1785.66 px, matching the canvas exactly.
    const float pct = fit_percentage(1785.66f, 1200.0f, 144.0f,
                                     595.22f, 842.0f, false);
    REQUIRE(pct == Catch::Approx(2.0f).epsilon(0.001));
    REQUIRE(595.22f * pct * (144.0f / 96.0f)
            == Catch::Approx(1785.66f).epsilon(0.001));
}

TEST_CASE("ZoomMath fit page takes the smaller of the two fits", "[core][zoom][math]") {
    // Width fits at 2.0x, height only at 1.0x -> fit-page picks 1.0x.
    REQUIRE(fit_percentage(1190.552f, 841.89f, 96.0f, 595.276f, 841.89f, true)
            == Catch::Approx(1.0f).epsilon(0.001));
    REQUIRE(fit_percentage(1190.552f, 841.89f, 96.0f, 595.276f, 841.89f, false)
            == Catch::Approx(2.0f).epsilon(0.001));
}

TEST_CASE("ZoomMath fit percentage for an unequal spread fits the larger page",
          "[core][zoom][math]") {
    // A spread of unequal pages -- legal in PDF, common in scanned books with
    // an inserted plate -- shares one render scale across both slots. Feeding
    // the pair's max extent is what keeps the larger page inside its slot;
    // deriving from the left page alone would overflow it.
    const float slot_px = 600.0f;
    const float left_w = 595.276f,  left_h = 841.89f;
    const float right_w = 1200.0f,  right_h = 2000.0f;
    const float pair_w = (left_w > right_w) ? left_w : right_w;
    const float pair_h = (left_h > right_h) ? left_h : right_h;

    const float pct = fit_percentage(slot_px, 900.0f, 96.0f, pair_w, pair_h, true);
    // Both pages fit inside the slot at that one percentage.
    REQUIRE(left_w  * pct <= Catch::Approx(slot_px));
    REQUIRE(right_w * pct <= Catch::Approx(slot_px));
    REQUIRE(left_h  * pct <= Catch::Approx(900.0f));
    REQUIRE(right_h * pct <= Catch::Approx(900.0f));
}

TEST_CASE("ZoomMath fit percentage tolerates degenerate inputs", "[core][zoom][math]") {
    REQUIRE(fit_percentage(1000.0f, 800.0f, 0.0f,  595.0f, 842.0f, false) > 0.0f);
    REQUIRE(fit_percentage(1000.0f, 800.0f, 96.0f, 0.0f,   842.0f, false)
            == Catch::Approx(1.0f));
    REQUIRE(fit_percentage(0.0f,    0.0f,   96.0f, 595.0f, 842.0f, false)
            == Catch::Approx(0.0f));
}

TEST_CASE("ZoomMath preset ladder steps to the next rung strictly above or below",
          "[core][zoom][math]") {
    REQUIRE(next_preset(1.0f)  == Catch::Approx(1.25f));
    REQUIRE(prev_preset(1.0f)  == Catch::Approx(0.75f));
    // At the ends the ladder returns the input unchanged, which is how
    // zoom_in()/zoom_out() report "no change".
    REQUIRE(next_preset(8.0f)  == Catch::Approx(8.0f));
    REQUIRE(prev_preset(0.25f) == Catch::Approx(0.25f));
    // A fit-derived percentage above the shipped 4.0 ceiling must still find a
    // larger rung. This exact state is what made zoom_in() a permanent no-op.
    REQUIRE(next_preset(5.2f)  == Catch::Approx(6.0f));
    REQUIRE(prev_preset(5.2f)  == Catch::Approx(4.0f));
    // And between rungs in the middle of the table.
    REQUIRE(next_preset(1.1f)  == Catch::Approx(1.25f));
    REQUIRE(prev_preset(1.1f)  == Catch::Approx(1.0f));
}

TEST_CASE("ZoomMath clamp preset span pins values into the table range",
          "[core][zoom][math]") {
    REQUIRE(clamp_preset_span(99.0f) == Catch::Approx(8.0f));
    REQUIRE(clamp_preset_span(0.01f) == Catch::Approx(0.25f));
    REQUIRE(clamp_preset_span(1.5f)  == Catch::Approx(1.5f));
}
```

Register it in `tests/CMakeLists.txt` next to the Task 1 entry:

```cmake
    unit/test_zoom_math.cpp            # PR-A1 Task 2
```

- [ ] **Step 1a: Generate an unequal-page spread fixture**

The `ZoomMath` spread case above computes `max(left, right)` in the test body, so it
proves the arithmetic but not the wiring: an implementation of
`DocumentView::set_viewport` that ignored its `pair_page` argument entirely would
still pass it. Closing that requires a document whose two pages differ in size, and
none of the existing fixtures qualify.

Add `scripts/generate-spread-fixture.py`, following the house pattern of
`scripts/generate-search-fixture.py` (module docstring in English explaining what the
fixture is load-bearing for, white page fills because the canvas surface is dark,
`pageCompression=0` as in `generate-cjk-fixture.py:45` and `generate-large-fixture.py:119`,
so the output is byte-stable across zlib builds — byte-identity otherwise breaks
between zlib-ng and stock zlib):

```python
#!/usr/bin/env python3
"""
Generate tests/fixtures/spread-unequal.pdf -- a two-page document whose pages
differ in size, for the PR-A1 two-page-spread fit tests.

  Page | Page   | Size (pt)
  idx  | number |
  -----+--------+------------------
    0  |   1    | 420 x 595  (A5)
    1  |   2    | 595 x 842  (A4)

The size DIFFERENCE is the whole point: one render scale is shared by both
slots of a spread, so a fit derived from the left page alone overflows the
slot holding the right one. A fixture with two equal pages cannot tell a
correct implementation from one that ignores the pair.
"""

import os

from reportlab.lib.colors import black, white
from reportlab.pdfgen import canvas

# Script-relative, like every other generator here -- generate-search-fixture.py:77,
# generate-large-fixture.py:56 and generate-bookmarks-fixture.py:44 all resolve the
# output this way so the script works from any cwd.
OUT = os.path.join(os.path.dirname(__file__), "..", "tests", "fixtures",
                   "spread-unequal.pdf")
PAGES = [(420.0, 595.0), (595.0, 842.0)]

def main():
    c = canvas.Canvas(OUT, pageCompression=0)
    for i, (w, h) in enumerate(PAGES):
        c.setPageSize((w, h))
        c.setFillColor(white)
        c.rect(0, 0, w, h, stroke=0, fill=1)
        c.setFillColor(black)
        c.setFont("Helvetica", 24)
        c.drawString(40, h - 60, f"spread page {i + 1}  {int(w)}x{int(h)}")
        c.showPage()
    c.save()

if __name__ == "__main__":
    main()
```

Run it and commit the generated PDF alongside the script, which is how the other
fixtures in `tests/fixtures/` are tracked. The output path is script-relative, so the
cwd does not matter:

```bash
python scripts/generate-spread-fixture.py
```

- [ ] **Step 1b: Update the existing DocumentView tests to the new API**

`tests/unit/test_document_view.cpp` builds views as
`InlineDispatcher disp; DocumentView view(open_simple(), disp);` — `open_simple()` is
the file-local helper at `:18-22` that opens `tests/fixtures/simple.pdf`. Keep that
shape; only the zoom calls change. Replace the three `set_zoom_mode` call sites
(`:82`, `:96`, `:109`) and the ladder-endpoint assertions (`:130`, `:140`, `:153`,
`:155`, `:157-158`, `:160`) with:

`tests/fixtures/simple.pdf` page 0 has `/MediaBox [0 0 595.22 842]` — read it out of
the file rather than assuming nominal A4 (595.276 x 841.89); the constants below are
derived from the real values so the assertions are exact rather than
epsilon-rescued.

**Every case sets its fit mode explicitly.** Do not lean on the constructor default:
Task 7 changes it from FitWidth to FitPage, and a test that leans on it silently
changes meaning at that point.

```cpp
TEST_CASE("DocumentView FitWidth derives a percentage from viewport pixels",
          "[core][view][zoom]") {
    InlineDispatcher disp;
    DocumentView view(open_simple(), disp);
    view.set_zoom_mode_fit_width();
    // 1190.44 px / 595.22 pt = exactly 2.0 at 96 dpi.
    view.set_viewport(1190.44f, 800.0f, 96.0f);
    REQUIRE(view.zoom_mode() == DocumentView::ZoomMode::FitWidth);
    REQUIRE(view.zoom_pct()     == Catch::Approx(2.0f).epsilon(0.001));
    REQUIRE(view.render_scale() == Catch::Approx(2.0f).epsilon(0.001));
}

TEST_CASE("DocumentView render scale carries the dpi factor",
          "[core][view][zoom]") {
    InlineDispatcher disp;
    DocumentView view(open_simple(), disp);
    view.set_zoom_mode_fit_width();
    // The same physical viewport at 192 dpi: each DIP is two device pixels, so
    // the percentage halves while the render scale -- and the pixmap width in
    // pixels -- is unchanged.
    view.set_viewport(1190.44f, 800.0f, 192.0f);
    REQUIRE(view.zoom_pct()     == Catch::Approx(1.0f).epsilon(0.001));
    REQUIRE(view.render_scale() == Catch::Approx(2.0f).epsilon(0.001));
}

TEST_CASE("DocumentView FitPage recomputes from the stored viewport",
          "[core][view][zoom]") {
    InlineDispatcher disp;
    DocumentView view(open_simple(), disp);
    view.set_zoom_mode_fit_width();
    view.set_viewport(1190.44f, 842.0f, 96.0f);
    REQUIRE(view.zoom_pct() == Catch::Approx(2.0f).epsilon(0.001));
    // Same viewport, fit-page: height binds at exactly 1.0.
    view.set_zoom_mode_fit_page();
    REQUIRE(view.zoom_pct() == Catch::Approx(1.0f).epsilon(0.001));
}

TEST_CASE("DocumentView zoom ladder walks the extended preset table",
          "[core][view][zoom]") {
    InlineDispatcher disp;
    DocumentView view(open_simple(), disp);
    view.set_zoom_pct(1.0f);
    int up_steps = 0;
    while (view.zoom_in()) {
        ++up_steps;
        REQUIRE(up_steps < 32);
    }
    REQUIRE(view.zoom_pct() == Catch::Approx(8.0f));
    REQUIRE(view.zoom_mode() == DocumentView::ZoomMode::Custom);
    REQUIRE_FALSE(view.zoom_in());

    int down_steps = 0;
    while (view.zoom_out()) {
        ++down_steps;
        REQUIRE(down_steps < 32);
    }
    REQUIRE(view.zoom_pct() == Catch::Approx(0.25f));
    REQUIRE_FALSE(view.zoom_out());
}

TEST_CASE("DocumentView zoom in works above the old preset ceiling",
          "[core][view][zoom]") {
    InlineDispatcher disp;
    DocumentView view(open_simple(), disp);
    view.set_zoom_mode_fit_width();
    // A wide canvas against a narrow page derives a percentage above 4.0
    // (3000 / 595.22 = 5.04) -- exactly the state in which the shipped
    // zoom_in() could never find a larger rung and silently did nothing.
    view.set_viewport(3000.0f, 900.0f, 96.0f);
    REQUIRE(view.zoom_pct() > 4.0f);
    REQUIRE(view.zoom_in());
    REQUIRE(view.zoom_pct() == Catch::Approx(6.0f));
}

TEST_CASE("DocumentView set_zoom_pct clamps to the extended span",
          "[core][view][zoom]") {
    InlineDispatcher disp;
    DocumentView view(open_simple(), disp);
    view.set_zoom_pct(99.0f);  REQUIRE(view.zoom_pct() == Catch::Approx(8.0f));
    view.set_zoom_pct(0.01f);  REQUIRE(view.zoom_pct() == Catch::Approx(0.25f));
    view.set_zoom_pct(0.25f);  REQUIRE(view.zoom_pct() == Catch::Approx(0.25f));
    view.set_zoom_pct(8.0f);   REQUIRE(view.zoom_pct() == Catch::Approx(8.0f));
}

TEST_CASE("DocumentView set_viewport fits the larger page of a spread pair",
          "[core][view][zoom]") {
    // spread-unequal.pdf: page 0 is 420x595, page 1 is 595x842. This is the
    // test that fails if set_viewport ignores pair_page -- the ZoomMath case
    // computes max() in the test body and so cannot detect that.
    Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/spread-unequal.pdf").has_value());
    InlineDispatcher disp;
    DocumentView view(std::move(doc), disp);
    view.set_zoom_mode_fit_page();

    const float slot_px = 600.0f, slot_h_px = 900.0f;

    // Without the pair: derived from page 0 (420x595) alone.
    // min(600/420, 900/595) = 1.42857. At that scale page 1 (595x842) is
    // 850 x 1203 -- overflowing its slot on BOTH axes. Compare width against
    // slot width and height against slot height; crossing them silently
    // asserts nothing.
    view.set_viewport(slot_px, slot_h_px, 96.0f);
    const float solo = view.zoom_pct();
    REQUIRE(solo == Catch::Approx(1.42857f).epsilon(0.001));
    REQUIRE(595.0f * solo > slot_px);        // page 1 too wide for its slot
    REQUIRE(842.0f * solo > slot_h_px);      // and too tall

    // With the pair: derived from max(420,595) x max(595,842).
    view.set_viewport(slot_px, slot_h_px, 96.0f, /*pair_page=*/1);
    const float paired = view.zoom_pct();
    REQUIRE(paired < solo);
    REQUIRE(420.0f * paired <= Catch::Approx(slot_px));     // page 0 width
    REQUIRE(595.0f * paired <= Catch::Approx(slot_h_px));   // page 0 height
    REQUIRE(595.0f * paired <= Catch::Approx(slot_px));     // page 1 width
    REQUIRE(842.0f * paired <= Catch::Approx(slot_h_px));   // page 1 height

    // An out-of-range pair index is ignored, not clamped into a wrong page.
    view.set_viewport(slot_px, slot_h_px, 96.0f, /*pair_page=*/99);
    REQUIRE(view.zoom_pct() == Catch::Approx(solo));
}
```

Delete the superseded cases these replace. Keep every non-zoom case untouched —
including the constructor case at `:33-35`, whose `zoom_mode() == FitWidth`
assertion Task 7 updates when it changes the default.

- [ ] **Step 2: Run the tests to verify they fail**

```bash
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --target litepdf_unit_tests --config Release
```

Expected: build FAILS — `cannot open include file: 'core/detail/ZoomMath.hpp'`, and
`zoom_pct`, `render_scale`, `set_viewport`, `set_zoom_pct` are not members of
`DocumentView`.

- [ ] **Step 2b: Write the ZoomMath header**

Create `src/core/detail/ZoomMath.hpp`:

```cpp
#pragma once

#include <algorithm>

// PR-A1: pure fit and preset-ladder arithmetic for DocumentView. No MuPDF, no
// Win32 -- so the unequal-page spread case is testable without a fixture PDF
// whose pages differ in size (none exists in tests/fixtures).
//
// UNITS. A "percentage" here is DIPs per PDF point: 1.0 means one PDF point
// maps to one DIP, the conventional 96-dpi screen ratio. The display dpi is
// applied exactly once, on the way back out in DocumentView::render_scale().

namespace litepdf::core {

// Preset zoom percentages. Extended past the shipped 4.0 ceiling because a
// fit-derived percentage legitimately exceeds it (a narrow page on a wide
// canvas), and at the old ceiling that state made zoom_in() a permanent no-op.
inline constexpr float kZoomPresets[] = {
    0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f,
};
inline constexpr int kZoomPresetCount =
    static_cast<int>(sizeof(kZoomPresets) / sizeof(kZoomPresets[0]));

// Fit percentage for a viewport given in DEVICE PIXELS.
//
// For a two-page spread, pass the pair's max width and max height: one render
// scale is shared by both slots, so deriving it from the left page alone would
// overflow the slot holding the larger page.
inline float fit_percentage(float vp_w_px, float vp_h_px, float dpi,
                            float pw_pt, float ph_pt, bool fit_page) noexcept {
    const float ratio    = (dpi > 0.0f) ? (96.0f / dpi) : 1.0f;
    const float vp_w_dip = vp_w_px * ratio;
    const float vp_h_dip = vp_h_px * ratio;
    const float fit_w    = (pw_pt > 0.0f) ? vp_w_dip / pw_pt : 1.0f;
    const float fit_h    = (ph_pt > 0.0f) ? vp_h_dip / ph_pt : 1.0f;
    return fit_page ? std::min(fit_w, fit_h) : fit_w;
}

// First rung strictly above `pct`, or `pct` itself at the top of the table.
inline float next_preset(float pct) noexcept {
    for (int i = 0; i < kZoomPresetCount; ++i) {
        if (kZoomPresets[i] > pct + 1e-4f) return kZoomPresets[i];
    }
    return pct;
}

// Last rung strictly below `pct`, or `pct` itself at the bottom of the table.
inline float prev_preset(float pct) noexcept {
    for (int i = kZoomPresetCount - 1; i >= 0; --i) {
        if (kZoomPresets[i] < pct - 1e-4f) return kZoomPresets[i];
    }
    return pct;
}

// Pin a percentage into the table's span. NaN-safe: NaN and -inf clamp low,
// +inf clamps high, keeping the [lo, hi] contract total.
inline float clamp_preset_span(float pct) noexcept {
    const float lo = kZoomPresets[0];
    const float hi = kZoomPresets[kZoomPresetCount - 1];
    if (!(pct >= lo)) return lo;
    if (pct > hi)     return hi;
    return pct;
}

}  // namespace litepdf::core
```

- [ ] **Step 2c: Run the ZoomMath tests to verify they pass**

```bash
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --target litepdf_unit_tests --config Release
```

The build still fails on `test_document_view.cpp` (the `DocumentView` members do not
exist yet), so temporarily comment out `unit/test_document_view.cpp` in
`tests/CMakeLists.txt`, build, and run:

```bash
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" --test-dir build -C Release -R ZoomMath --output-on-failure
```

Expected: **8** cases pass (the seven above plus the 144 dpi case). Restore the
commented line before Step 3.

- [ ] **Step 3: Change the header**

In `src/core/DocumentView.hpp`, replace lines 81-107 (from `ZoomMode zoom_mode()`
through `bool zoom_out();`) with:

```cpp
    ZoomMode zoom_mode() const noexcept;

    // User-facing magnification. 1.0 means ONE PDF POINT MAPS TO ONE DIP -- the
    // conventional 96-dpi screen ratio browsers also call 100%. It is not
    // physical actual size: a PDF point is 1/72 inch, so 1.0 renders at 0.75x
    // ruler size. Nothing surfaces a numeric percentage today; this is the
    // definition an eventual readout must be built on.
    float zoom_pct() const noexcept;

    // Point -> PIXEL factor handed to MuPDF (fz_scale) and used as part of the
    // PageCache L1 key. This is the ONLY place the display dpi is applied; the
    // shipped code applied it here AND at every caller, rendering 2x oversized
    // at 200% scaling.
    float render_scale() const noexcept;

    // Set the viewport and, for FitWidth/FitPage, re-derive zoom_pct_ from it.
    // Custom leaves the percentage frozen.
    //
    // The viewport is in DEVICE PIXELS -- the raw GetClientRect extent. The
    // parameter names say _px because the shipped signature named them _dip
    // while every caller passed pixels, which is how the double-dpi defect got
    // in. `dpi` is the window dpi from GetDpiForWindow.
    //
    // `pair_page` is the other page of a two-page spread, or -1 in single-page
    // mode. When set, the fit uses max(width) and max(height) of the two pages
    // so one shared render scale fits both slots; a spread of unequal pages
    // would otherwise overflow the slot whose page is larger.
    //
    // Thread-safety: UI thread only, unchanged from the shipped contract. The
    // derived percentage is read by request_render() to build the CTM; all
    // callers set the viewport before requesting a render in the same message
    // handler, so a worker never observes a torn read.
    void set_viewport(float viewport_w_px, float viewport_h_px,
                      float dpi, int pair_page = -1);

    // Switch fit mode, re-deriving the percentage from the stored viewport.
    void set_zoom_mode_fit_width();
    void set_zoom_mode_fit_page();

    // Directly set a Custom percentage (session restore, and tests). Clamps to
    // the preset span [0.25, 8.0]; non-finite inputs clamp into range. Does not
    // require viewport dims.
    void set_zoom_pct(float pct) noexcept;

    // Step through preset percentages
    // {0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0, 4.0, 6.0, 8.0}.
    // Both switch the mode to Custom and return true iff the percentage changed.
    //
    // The table is percentages, which is what the shipped code got wrong: it
    // compared these values against a render scale that was routinely above 12,
    // so zoom_in() never found a larger rung and was a permanent no-op.
    bool zoom_in();
    bool zoom_out();
```

- [ ] **Step 4: Change the implementation**

In `src/core/DocumentView.cpp`, rename the Impl field at `:48-49` and extend the
preset table at `:57-59`:

```cpp
    int                    current_page = 0;
    DocumentView::ZoomMode zm           = DocumentView::ZoomMode::FitWidth;
    float                  pct          = 1.0f;   // was: scale (a render scale)
    float                  vp_w         = 0.0f;   // device pixels
    float                  vp_h         = 0.0f;   // device pixels
    float                  dpi          = 96.0f;
```

Delete the `Impl::kPresets` table at `:56-59` entirely — `kZoomPresets` in
`core/detail/ZoomMath.hpp` replaces it — and add `#include "core/detail/ZoomMath.hpp"`
to the includes at the top of the file.

Replace `zoom_scale()` (`:200-202`) and `set_zoom_mode` (`:204-246`) with:

```cpp
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

    const auto a = impl_->doc.page_size(static_cast<std::size_t>(impl_->current_page));
    float pw = a.width_pt;
    float ph = a.height_pt;
    if (pair_page >= 0 && pair_page < page_count()) {
        // Two-page spread: one render scale is shared by both slots, so fit the
        // pair's bounding size. Deriving from the current page alone overflows
        // the slot holding the larger page once the paint path stops
        // shrink-to-fitting.
        const auto b = impl_->doc.page_size(static_cast<std::size_t>(pair_page));
        pw = std::max(pw, b.width_pt);
        ph = std::max(ph, b.height_pt);
    }

    impl_->pct = fit_percentage(viewport_w_px, viewport_h_px, dpi, pw, ph,
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
```

Rename `set_zoom_scale` to `set_zoom_pct` (`:248-257`) and delegate the clamp:

```cpp
void DocumentView::set_zoom_pct(float pct) noexcept {
    impl_->zm  = ZoomMode::Custom;
    impl_->pct = clamp_preset_span(pct);
}
```

Replace the ladder bodies (`:259-281`) with the pure steppers, which return the input
unchanged at the ends — that is how "no change" is reported:

```cpp
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
```

Everywhere else `impl_->scale` appears, it becomes `impl_->pct`. In the page-change
recompute (`:190-192`), replace the `set_zoom_mode(...)` call with:

```cpp
    // Recompute the percentage if not Custom (page size may differ per page).
    if (impl_->zm != ZoomMode::Custom) {
        set_viewport(impl_->vp_w, impl_->vp_h, impl_->dpi);
    }
```

In the four render-submission sites (`:287`, `:338`, `:355`, `:364`), the scale
handed to the engine becomes the render scale, not the percentage:

```cpp
    req.scale = render_scale();
```

(and `r0.scale` / `r1.scale` likewise). These feed `fz_scale` and the `PageCache`
L1 key, both of which are in pixels.

- [ ] **Step 5: Run the tests to verify they pass**

```bash
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --target litepdf_unit_tests --config Release
```

The `litepdf_unit_tests` target links `litepdf_core`, and `litepdf_core`
(`CMakeLists.txt:41-75`) contains **no** `PdfCanvas.cpp` and **no** `MainWindow.cpp` —
its only `src/ui` members are `ThumbnailPane.cpp`, `PasswordDialog.cpp` and
`password_retry.cpp`. So the test build succeeds at this point even though the
`litepdf` exe target does not yet compile; Tasks 3 and 4 fix that.

```bash
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" --test-dir build -C Release -R "DocumentView|ZoomMath" --output-on-failure
```

Expected: **18** cases pass. That is 8 `ZoomMath` + 7 `DocumentView` zoom cases + the
3 pre-existing `DocumentView` cases that are not about zoom (`constructs from opened
Document`, the throwing case, and `set_current_page clamps`) — `-R` matches names, and
those three start with `DocumentView` too. Count them before assuming a discrepancy
is a bug.

- [ ] **Step 6: Commit**

```bash
git add src/core/detail/ZoomMath.hpp tests/unit/test_zoom_math.cpp tests/CMakeLists.txt \n        scripts/generate-spread-fixture.py tests/fixtures/spread-unequal.pdf \n        src/core/DocumentView.hpp src/core/DocumentView.cpp tests/unit/test_document_view.cpp
git commit -m "fix(core): split zoom percentage from render scale, apply dpi once (PR-A1 Task 2)"
```

---

## Task 3: Migrate MainWindow call sites and fix dual-mode fit ordering

**Files:**
- Create: nothing.
- Modify: `src/ui/PdfCanvas.hpp` (declare `apply_viewport`), `src/ui/PdfCanvas.cpp`
  (define it; call it from the dual branch of `on_key_down` at `:712-742` and from
  `resubmit_current_page` at `:609-634`)
- Modify: `src/ui/MainWindow.cpp:245-300` (`kick_render`), `:617-630` (`on_tab_switch`), `:700-716` (`capture_session`), `:836-852` (session restore), `:1380-1396` (`IDM_ZOOM_RESET`)

**Interfaces:**
- Consumes: `DocumentView::set_viewport`, `zoom_pct`, `set_zoom_pct`,
  `set_zoom_mode_fit_width`, `set_zoom_mode_fit_page` (Task 2).
- Produces: `PdfCanvas::apply_viewport()` — pushes the canvas's current client
  extent, DPI and (in spread mode) slot width plus pair page into the active
  `DocumentView`. Task 4 does not use it; it exists so that **every** render
  submission path derives the fit the same way.

- [ ] **Step 1: Add `PdfCanvas::apply_viewport()` and route every submission path through it**

The fit must be derived after the spread's left-page snap and from *both* pages of
the pair — but `MainWindow::kick_render` is not the only submission path.
`PdfCanvas::on_key_down`'s dual branch (`:712-742`) and `resubmit_current_page`
(`:609-634`) each snap and submit on their own without touching the viewport, so a
PgDn in spread mode would leave the percentage derived from whatever the previous
`kick_render` saw. Put the derivation in one place and call it from all three.

Declare in `src/ui/PdfCanvas.hpp`, next to `render_epoch()`:

```cpp
    // Push the canvas's client extent + dpi into the active DocumentView so the
    // fit percentage is re-derived. In spread mode this passes the HALF-WIDTH
    // slot and the pair's other page, because both slots share one render scale
    // and a spread of unequal pages must fit the larger one.
    //
    // Every render-submission path calls this first: MainWindow::kick_render,
    // on_key_down's dual branch, resubmit_current_page, and the Ctrl+wheel zoom
    // handler. Deriving the fit in only some of them leaves the rest rendering
    // at a stale percentage.
    void apply_viewport();
```

Define in `src/ui/PdfCanvas.cpp`:

```cpp
void PdfCanvas::apply_viewport() {
    if (!impl_ || !impl_->view || !hwnd_) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const float dpi_f = static_cast<float>(GetDpiForWindow(hwnd_));
    const float cw_px = static_cast<float>(rc.right - rc.left);
    const float ch_px = static_cast<float>(rc.bottom - rc.top);

    if (!impl_->dual_page) {
        impl_->view->set_viewport(cw_px, ch_px, dpi_f);
        return;
    }
    // Do NOT assume the caller already snapped current_page to the pair's LEFT
    // page. DocumentView derives the fit from current_page and treats pair_page
    // as the other half, so if current_page were the RIGHT page the left page's
    // size would never enter the fit and its slot could overflow. Re-snap here,
    // the same defensive move resubmit_current_page already makes at :725-730.
    const int total = impl_->view->page_count();
    const int left  = dual_page_compute_left(impl_->view->current_page(), total);
    if (left != impl_->view->current_page()) impl_->view->set_current_page(left);
    const int right = dual_page_compute_right(left, total);
    const float gutter_px = 8.0f * dpi_f / 96.0f;   // matches the 8 DIP gutter
    const float slot_px   = std::max(0.0f, (cw_px - gutter_px) * 0.5f);
    impl_->view->set_viewport(slot_px, ch_px, dpi_f, right);
}
```

Then wire it into the other three submission sites. The exact insertion point differs
in each, so they are spelled out rather than described:

- **`resubmit_current_page`, dual branch (`:618-623`).** There is **no**
  `set_current_page` call here — it re-renders whatever page is already current.
  Insert `apply_viewport();` immediately after
  `impl_->view->cancel_stale_renders(0);` and before the first `request_render`.
- **`on_key_down`, dual branch (`:725-733`).** The re-snap is *conditional*
  (`if (left != impl_->view->current_page()) { … }`), so placing the call inside that
  `if` would skip it whenever the page is already canonical — which is the common
  case. Put `apply_viewport();` **after** the closing brace of that `if`, before
  `const int right = …`.
- **Ctrl+wheel zoom (`:436-449`).** Replace its hand-rolled single-page submission
  with a call to `resubmit_current_page()`, which already re-derives the viewport (via
  the bullet above) and submits **both** slots:

  ```cpp
              if (changed) {
                  // Route through resubmit_current_page rather than submitting a
                  // single render here: in spread mode this handler refreshed only
                  // the left slot, which was invisible while zoom could not change
                  // displayed size -- and becomes a spread at two different
                  // magnifications the moment Task 4 lands.
                  resubmit_current_page();
                  if (impl_->on_zoom_changed) impl_->on_zoom_changed();
              }
              return 0;
  ```

  **This is a deliberate deviation from the spec**, which assigns the Ctrl+wheel dual
  fix to PR-A2 (§3.5) and tells A1 to leave the handler alone. That instruction was
  written on the assumption that A1 could not make the defect visible. It can: A1 is
  what makes a changed render scale change displayed size. Leaving it for A2 would
  ship a spread with mismatched halves in the release that introduced it. The A2 spec
  section becomes a no-op; note that when A2 is planned.

- [ ] **Step 2: Rewrite `kick_render` so the dual snap precedes the fit**

`src/ui/MainWindow.cpp:245-256` currently sets the zoom mode and only afterwards, at
`:263-272`, snaps the current page to the spread's left page — so the fit is derived
for whatever page was current *before* the pair was canonicalized. Reorder, and
delegate the derivation:

```cpp
void MainWindow::kick_render(int page) {
    auto* view = active_view();
    if (!view || !canvas_) return;

    if (view->dual_page()) {
        // Canonicalize the pair FIRST: the fit is derived from current_page, so
        // deriving it before the snap fits the wrong page.
        const int total = view->page_count();
        const int left  = litepdf::ui::dual_page_compute_left(page, total);
        const int right = litepdf::ui::dual_page_compute_right(left, total);
        view->cancel_stale_renders(0);
        view->set_current_page(left);
        canvas_->apply_viewport();

        HWND target = canvas_->hwnd();
        const std::uint64_t epoch = canvas_->render_epoch();
        view->request_render(left,
            [target, epoch](fz_pixmap* p, fz_context* worker_ctx) {
                PdfCanvas::post_render_done(target, p, worker_ctx, epoch);
            });
        if (right >= 0) {
            view->request_render(right,
                [target, epoch](fz_pixmap* p, fz_context* worker_ctx) {
                    PdfCanvas::post_render_done_right(target, p, worker_ctx, epoch);
                });
        }
        InvalidateRect(canvas_->hwnd(), nullptr, FALSE);
        return;
    }

    canvas_->apply_viewport();
    // ... existing single-page submission path, unchanged ...
}
```

Keep the existing single-page body below the `dual_page()` block exactly as it is,
including the render-epoch comment at `:258-262`. The `InvalidateRect` above
preserves the one the shipped dual branch already performs at `:283`.

- [ ] **Step 3: Re-point the remaining three call sites**

`:622-628` (`on_tab_switch`) and `:1385-1391` (`IDM_ZOOM_RESET`):

```cpp
    RECT rc; GetClientRect(canvas_->hwnd(), &rc);
    const UINT dpi = GetDpiForWindow(hwnd_);
    view->set_viewport(static_cast<float>(rc.right - rc.left),
                       static_cast<float>(rc.bottom - rc.top),
                       static_cast<float>(dpi));
```

`IDM_ZOOM_RESET` additionally selects the mode; since PR-A1 ships FitPage as the
default (Task 7), Reset returns to FitPage:

```cpp
                case IDM_ZOOM_RESET: {
                    if (auto* view = active_view(); view && canvas_) {
                        view->set_zoom_mode_fit_page();
                        RECT rc; GetClientRect(canvas_->hwnd(), &rc);
                        UINT dpi = GetDpiForWindow(hwnd);
                        view->set_viewport(static_cast<float>(rc.right - rc.left),
                                           static_cast<float>(rc.bottom - rc.top),
                                           static_cast<float>(dpi));
                        kick_render(view->current_page());
                        schedule_session_save();
                    }
                    return 0;
```

`:714` (`capture_session`) persists the percentage:

```cpp
            st.zoom_scale = v->zoom_pct();
```

The `SessionTab` field keeps the name `zoom_scale` — the JSON key is part of the
on-disk format and Task 5 handles its changed meaning via the version bump.

`:836-851` (session restore):

```cpp
        v->set_current_page(st.page);
        switch (st.zoom_mode) {
            case litepdf::core::SessionZoom::Custom:
                v->set_zoom_pct(st.zoom_scale);   // no viewport needed
                break;
            case litepdf::core::SessionZoom::FitWidth:
            case litepdf::core::SessionZoom::FitPage: {
                if (st.zoom_mode == litepdf::core::SessionZoom::FitWidth)
                    v->set_zoom_mode_fit_width();
                else
                    v->set_zoom_mode_fit_page();
                RECT rc; GetClientRect(canvas_->hwnd(), &rc);
                const UINT dpi = GetDpiForWindow(hwnd_);
                v->set_viewport(static_cast<float>(rc.right - rc.left),
                                static_cast<float>(rc.bottom - rc.top),
                                static_cast<float>(dpi));
                break;
            }
        }
        kick_render(st.page);
```

- [ ] **Step 4: Build**

```bash
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --target litepdf --config Release
```

Expected: `MainWindow.cpp` compiles; `PdfCanvas.cpp` still fails on `zoom_scale()`
at `:292` and `:882` — Task 4 fixes those. If any `set_zoom_mode` call site remains
anywhere, the compiler names it here; that is the rename doing its job.

- [ ] **Step 5: Commit**

```bash
git add src/ui/PdfCanvas.hpp src/ui/PdfCanvas.cpp src/ui/MainWindow.cpp
git commit -m "fix(ui): derive the fit once via apply_viewport, snap spread before fit (PR-A1 Task 3)"
```

---

## Task 4: Natural-size paint path and canvas unit contract

**Files:**
- Modify: `src/ui/PdfCanvas.cpp:276-315` (`scroll_into_view`), `:565-582` (`create_render_target`), `:686-706` (arrow keys), `:786-833` (dual paint), `:848-900` (single paint + hit overlay)

**Interfaces:**
- Consumes: `bitmap_px_to_dip`, `clamp_pan`, `place_bitmap`, `Placement` (Task 1);
  `DocumentView::zoom_pct` (Task 2).
- Produces: nothing new.

- [ ] **Step 1: Create the render target at the window's DPI**

`src/ui/PdfCanvas.cpp:573-576` uses `D2D1::RenderTargetProperties()`, whose default
`dpiX/dpiY` of 0.0 makes D2D adopt the **desktop** DPI. Replace with:

```cpp
    // Pin the render target to THIS WINDOW's dpi, not the desktop's. The helper
    // defaults dpiX/dpiY to 0.0, which means "desktop dpi" -- wrong on a
    // secondary monitor with a different scale factor, and the reason the
    // WM_DPICHANGED teardown below could not actually rebuild at the new dpi.
    // Bitmaps stay at the D2D default of 96 (so their GetSize() is in pixels);
    // ViewportMath::bitmap_px_to_dip is the single conversion point.
    const float rt_dpi = static_cast<float>(GetDpiForWindow(hwnd_));
    HRESULT hr = impl_->factory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(),
            rt_dpi, rt_dpi),
        D2D1::HwndRenderTargetProperties(hwnd_, sz),
        &impl_->rt);
```

`HwndRenderTargetProperties` and `rt->Resize` both take pixels and are unaffected.

- [ ] **Step 2: Draw the single-page path at natural size**

Replace `PdfCanvas.cpp:848-865` (from `D2D1_SIZE_F src = ...` through the
`DrawBitmap` call) with:

```cpp
    if (impl_->current_bitmap) {
        const D2D1_SIZE_F src_px = impl_->current_bitmap->GetSize();  // PIXELS
        const D2D1_SIZE_F vp     = impl_->rt->GetSize();              // DIPs
        const float rt_dpi = static_cast<float>(GetDpiForWindow(hwnd_));
        const float src_w  = bitmap_px_to_dip(src_px.width,  rt_dpi);
        const float src_h  = bitmap_px_to_dip(src_px.height, rt_dpi);

        // Natural size: no shrink-to-fit. The shipped code scaled the
        // destination to fit the viewport unconditionally, which is why a
        // changed render scale never changed the displayed size.
        const Placement pl = place_bitmap(src_w, src_h, vp.width, vp.height,
                                          impl_->pan_x, impl_->pan_y);
        const D2D1_RECT_F dst = D2D1::RectF(pl.x, pl.y, pl.x + pl.w, pl.y + pl.h);
        impl_->rt->DrawBitmap(impl_->current_bitmap.Get(), dst, 1.0f,
                              D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
```

- [ ] **Step 3: Fix the hit-overlay unit**

Immediately below, `PdfCanvas.cpp:882-885` computes the PDF-point → screen factor as
`zoom_scale() * scale`, where `scale` was the (now gone) fit ratio. Replace **`:882-885`**
— the range must include `:884-885`, where `ox`/`oy` are declared, because the block
below declares them too:

```cpp
            // One PDF point is exactly zoom_pct DIPs: the pixmap is
            // page_pt * render_scale pixels wide, and dividing that by
            // rt_dpi/96 to reach DIPs cancels the dpi factor back out. Using
            // render_scale() here instead would double every hit rectangle at
            // 200% scaling.
            const float pct = impl_->view->zoom_pct();
            const float ox  = dst.left;
            const float oy  = dst.top;
```

and every `pt * s` in the quad loop below becomes `pdf_point_to_dip(pt, pct)`, so the
one place the mapping is written is the tested helper rather than a bare
multiplication that can silently pick up a dpi term again.

**`scroll_into_view` needs the same treatment, and it is not a one-line swap.**
Replace **`:285-313`** — note the range starts at `:285`, not `:287`: `src` and `vp`
are declared there, the replacement block below declares `vp` itself, and leaving the
originals in place is a `C2374` redefinition plus an unused `src`. The current body
carries a second copy of the fit math *and* the old centered origin:

```cpp
    float fit_scale = vp.width / src.width;
    if (src.height * fit_scale > vp.height) fit_scale = vp.height / src.height;
    const float dst_h_unpanned = src.height * fit_scale;
    const float dy = (vp.height - dst_h_unpanned) * 0.5f;
    ...
    const float q_top_dip = dy + impl_->pan_y + q_min_y_pt * s;
    ...
    impl_->pan_y = vp.height * 0.5f - dy - q_center_pt * s;
```

Substituting only the multiplier would leave `dy` as an always-centered offset while
`on_paint` now places content from a top-left origin — the two would disagree about
what a given `pan_y` means on any overflowing axis, which is reachable in A1 through
Custom zoom. Replace the whole block so it uses the same helper as paint:

```cpp
    const D2D1_SIZE_F src_px = impl_->current_bitmap->GetSize();
    const D2D1_SIZE_F vp     = impl_->rt->GetSize();
    const float rt_dpi = static_cast<float>(GetDpiForWindow(hwnd_));
    const float src_w  = bitmap_px_to_dip(src_px.width,  rt_dpi);
    const float src_h  = bitmap_px_to_dip(src_px.height, rt_dpi);
    const float pct    = impl_->view->zoom_pct();

    // Quad extent in PDF points. These four lines are the ONLY part of the old
    // block that survives verbatim -- keep them, because everything below reads
    // them.
    const float q_min_y_pt = std::min({ h.geom.ul_y, h.geom.ur_y,
                                        h.geom.ll_y, h.geom.lr_y });
    const float q_max_y_pt = std::max({ h.geom.ul_y, h.geom.ur_y,
                                        h.geom.ll_y, h.geom.lr_y });
    const float q_center_pt = (q_min_y_pt + q_max_y_pt) * 0.5f;

    // Where the page currently sits, using the SAME placement rule as on_paint.
    const Placement cur = place_bitmap(src_w, src_h, vp.width, vp.height,
                                       impl_->pan_x, impl_->pan_y);
    const float q_top_dip = cur.y + pdf_point_to_dip(q_min_y_pt, pct);
    const float q_bot_dip = cur.y + pdf_point_to_dip(q_max_y_pt, pct);
    const float margin    = 24.0f;
    if (q_top_dip >= margin && q_bot_dip <= vp.height - margin) {
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;   // already visible; no scroll
    }

    // Center the quad vertically. place_bitmap's origin is the content's top on
    // an overflowing axis, so the target pan is a direct offset from it; the
    // clamp keeps it inside the page.
    const float q_center = pdf_point_to_dip(q_center_pt, pct);
    impl_->pan_y = clamp_pan(vp.height * 0.5f - q_center, src_h, vp.height);
    InvalidateRect(hwnd_, nullptr, FALSE);
```

The three `q_*_pt` declarations above are lifted from the old block's `:296-298`
and `:312`. They are shown here in full because the replacement deletes their
original site — omitting them would produce three undeclared identifiers and a
build failure, which is not something the engineer should have to discover.

Keep the surrounding page-switch logic at `:260-274` and the early-out at `:280-283`
exactly as they are.

- [ ] **Step 4: Draw the dual path at natural size**

The slot stays a fixed half-width band, but each bitmap is drawn at natural size
within it — and the pan is clamped **once, against the painted union of the two
slots**, not per slot. Clamping each slot separately against `slot_w` would let a
narrow page's clamp fight a wide page's, and with unequal pages neither bound
describes what is actually on screen.

**Replace `PdfCanvas.cpp:799-817`** — the `draw_slot` lambda through its two call
sites. Note the range: `:793-797` are the `gutter` / `slot_w` / `slot_h` / `left_x0` /
`right_x0` declarations, which stay; `:799` opens the lambda; `:815` closes it; and
`:816-817` are the two old two-argument calls, which must go with it or they will not
match the new three-argument signature.

```cpp
        const float rt_dpi = static_cast<float>(GetDpiForWindow(hwnd_));

        // Unpanned placement of each slot; the pan is clamped once against the
        // painted union of both (content_extent, Step 5), never per slot --
        // with unequal pages a per-slot clamp describes neither what is on
        // screen nor what the arrow keys move.
        auto slot_placement = [&](ID2D1Bitmap* bm) -> Placement {
            if (!bm) return Placement{};
            const D2D1_SIZE_F src_px = bm->GetSize();
            return place_bitmap(bitmap_px_to_dip(src_px.width,  rt_dpi),
                                bitmap_px_to_dip(src_px.height, rt_dpi),
                                slot_w, slot_h, 0.0f, 0.0f);
        };
        const Placement le = slot_placement(impl_->current_bitmap.Get());
        const Placement re = slot_placement(impl_->right_bitmap.Get());

        ContentBox box{};
        content_extent(box);
        const float pan_x = clamp_pan(impl_->pan_x, box.w, vp.width);
        const float pan_y = clamp_pan(impl_->pan_y, box.h, slot_h);

        // When the union overflows, pan is measured from ITS left/top edge, not
        // from the canvas origin -- otherwise both clamp endpoints are offset by
        // box.l and neither reaches a painted edge (pan 0 leaves a blank strip,
        // pan at the limit stops short of the viewport edge). When it fits, the
        // slots keep their natural centered positions and the base is zero.
        const float base_x = (box.w > vp.width) ? -box.l : 0.0f;
        const float base_y = (box.h > slot_h)   ? -box.t : 0.0f;

        auto draw_slot = [&](ID2D1Bitmap* bm, float x0, const Placement& pl) {
            if (!bm) return;
            const D2D1_RECT_F dst = D2D1::RectF(x0 + pl.x + base_x + pan_x,
                                                pl.y + base_y + pan_y,
                                                x0 + pl.x + pl.w + base_x + pan_x,
                                                pl.y + pl.h + base_y + pan_y);
            impl_->rt->DrawBitmap(bm, dst, 1.0f,
                                  D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        };
        draw_slot(impl_->current_bitmap.Get(), left_x0,  le);
        draw_slot(impl_->right_bitmap.Get(),   right_x0, re);
```

Keep the existing placeholder-rectangle branch for an absent right slot
(`:823-833`) unchanged, and keep the existing `gutter` / `slot_w` / `slot_h` /
`left_x0` / `right_x0` declarations at `:792-797` — they still define the bands.

- [ ] **Step 5: Clamp arrow-key panning**

`PdfCanvas.cpp:686-706` adjusts `pan_x`/`pan_y` by 100 DIP with no bounds — the
comment there still says "No clamping in Phase 3 — user can pan off-canvas".
Replace the four cases with a helper that clamps against the current content:

```cpp
        // Arrow keys pan by 100 DIP, now clamped to the content. Panning is
        // only meaningful once content can overflow the viewport, which is
        // what the natural-size paint path above introduces.
        case VK_LEFT:  return pan_by( 100.0f,    0.0f);
        case VK_RIGHT: return pan_by(-100.0f,    0.0f);
        case VK_UP:    return pan_by(   0.0f,  100.0f);
        case VK_DOWN:  return pan_by(   0.0f, -100.0f);
```

with two new private members of `PdfCanvas`. `content_extent` is the shared
definition of "how big is the thing on screen" — the dual paint path above calls it
too, so panning and painting cannot disagree:

```cpp
// Painted content box in canvas DIPs -- what clamp_pan must measure against.
// Single page: the bitmap's DIP size at the origin. Spread: the UNION of the
// two slot placements, INCLUDING its left/top edge, because in an unequal
// spread that edge is not zero and the paint path has to subtract it.
//
// Measuring a spread against one bitmap would be wrong in the direction that
// hides the bug: each page fits inside its own half-width slot, so clamp_pan's
// "content fits -> pin to 0" branch would fire on every arrow key and
// horizontal panning would silently do nothing.
//
// Returns false when there is nothing rendered yet.
bool PdfCanvas::content_extent(ContentBox& out) const {
    if (!impl_ || !impl_->rt || !impl_->current_bitmap) return false;
    const float rt_dpi = static_cast<float>(GetDpiForWindow(hwnd_));
    const D2D1_SIZE_F vp = impl_->rt->GetSize();

    auto dip_size = [&](ID2D1Bitmap* bm, float& w, float& h) {
        const D2D1_SIZE_F px = bm->GetSize();
        w = bitmap_px_to_dip(px.width,  rt_dpi);
        h = bitmap_px_to_dip(px.height, rt_dpi);
    };

    float lw = 0.0f, lh = 0.0f;
    dip_size(impl_->current_bitmap.Get(), lw, lh);
    if (!impl_->dual_page) {
        out = ContentBox{0.0f, 0.0f, lw, lh};
        return true;
    }

    // Same band geometry as on_paint's dual branch.
    const float gutter   = 8.0f;
    const float slot_w   = std::max(0.0f, (vp.width - gutter) * 0.5f);
    const float slot_h   = vp.height;
    const float left_x0  = 0.0f;
    const float right_x0 = slot_w + gutter;

    const Placement le = place_bitmap(lw, lh, slot_w, slot_h, 0.0f, 0.0f);
    float l = left_x0 + le.x;
    float r = left_x0 + le.x + le.w;
    float t = le.y;
    float b = le.y + le.h;

    if (impl_->right_bitmap) {
        float rw = 0.0f, rh = 0.0f;
        dip_size(impl_->right_bitmap.Get(), rw, rh);
        const Placement re = place_bitmap(rw, rh, slot_w, slot_h, 0.0f, 0.0f);
        l = std::min(l, right_x0 + re.x);
        r = std::max(r, right_x0 + re.x + re.w);
        t = std::min(t, re.y);
        b = std::max(b, re.y + re.h);
    }
    out = ContentBox{l, t, std::max(0.0f, r - l), std::max(0.0f, b - t)};
    return true;
}

LRESULT PdfCanvas::pan_by(float dx, float dy) {
    ContentBox box{};
    if (!content_extent(box)) return 0;
    const D2D1_SIZE_F vp = impl_->rt->GetSize();
    impl_->pan_x = clamp_pan(impl_->pan_x + dx, box.w, vp.width);
    impl_->pan_y = clamp_pan(impl_->pan_y + dy, box.h, vp.height);
    InvalidateRect(hwnd_, nullptr, FALSE);
    return 0;
}
```

Declare the struct and both members in `src/ui/PdfCanvas.hpp`, next to `on_key_down`:

```cpp
    // Painted extent plus its origin, in canvas DIPs. `l`/`t` are zero for a
    // single page and non-zero for an unequal spread, where the union of the
    // two slots does not start at the canvas origin.
    struct ContentBox { float l, t, w, h; };

    LRESULT pan_by(float dx, float dy);
    bool    content_extent(ContentBox& out) const;
```

While in that header, fix the stale comment at `:62` — `pan()` / `set_pan()` no longer
describe "DIPs from the centered/fit origin". Same for `PdfCanvas.cpp:114-115`
("from the centered/fit position … No clamping in Phase 3 — user can pan off-canvas"),
which is now wrong in both halves. State the new rule: an axis that fits is centered
and its pan is 0; an axis that overflows uses a top-left origin with pan clamped to
`[viewport - content, 0]`.

Add `#include "ui/detail/ViewportMath.hpp"` plus these using-declarations in the
`PdfCanvas.cpp` anonymous-namespace preamble:

```cpp
using litepdf::ui::bitmap_px_to_dip;
using litepdf::ui::clamp_pan;
using litepdf::ui::pdf_point_to_dip;
using litepdf::ui::place_bitmap;
using litepdf::ui::Placement;
```

**Do not touch** `WM_USER_RENDER_DONE`'s pan reset at `:547-553`. It stays
unconditional in this PR; PR-A2 replaces it with the anchor machinery.

- [ ] **Step 6: Build the full exe**

```bash
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --config Release --parallel
```

Expected: `litepdf.exe` and `litepdf_unit_tests.exe` both link.

- [ ] **Step 7: Run the whole suite**

From the repo root:

```bash
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" --test-dir build -C Release --output-on-failure
```

Expected: all cases pass except the one `[!shouldfail]` that is expected to fail.

- [ ] **Step 8: Commit**

```bash
git add src/ui/PdfCanvas.cpp src/ui/PdfCanvas.hpp
git commit -m "fix(ui): draw bitmaps at natural size, pin render target dpi, clamp pan (PR-A1 Task 4)"
```

---

## Task 5: Session version 2 with a v1-accepting migration

> **Deviation (2026-09-09):** the migration target below is written as FitWidth,
> matching the original design. What shipped resets Custom to **FitPage**
> instead -- a late-branch change (see Task 7): with the paint-path fix, a
> FitWidth A4 page is unreadable with no wheel scrolling to reach the rest of
> it. The code blocks in this task are updated below to match what shipped;
> see `migrate_v1_to_v2` in `src/core/SessionState.cpp` for the final wording.

**Files:**
- Modify: `src/core/SessionState.hpp:12` (version constant), `:28` (struct), plus the new `peek_version` declaration
- Modify: `src/core/SessionState.cpp:259-276` (`parse_object`), `:279-281` (`validate`), `:336-345` (`from_json`)
- Modify: `tests/unit/test_session_state.cpp:40-59`, `:90-183`, `:120`, `:182-185`

**Interfaces:**
- Consumes: nothing.
- Produces: `kSessionVersion == 2`; `from_json` returns a value whose `version` is
  always 2. Task 6 consumes the fact that a raw v1 document is detectable.

- [ ] **Step 1: Write the failing tests**

Add to `tests/unit/test_session_state.cpp`:

```cpp
TEST_CASE("SessionState v2 migrates a version 1 document",
          "[core][session][json][migration]") {
    const std::string v1 =
        "{\"version\":1,\"window\":{\"flags\":0,\"show\":1,"
        "\"x\":10,\"y\":20,\"w\":1280,\"h\":800},\"active\":0,"
        "\"tabs\":[{\"path\":\"C:\\\\a\\\\one.pdf\",\"page\":3,"
        "\"zoom_mode\":\"custom\",\"zoom_scale\":4.0}]}";
    auto r = from_json(v1);
    REQUIRE(r.has_value());
    REQUIRE(r->version == 2);
    // Window placement and tabs survive the migration intact.
    REQUIRE(r->window.w == 1280);
    REQUIRE(r->tabs.size() == 1);
    REQUIRE(r->tabs[0].page == 3);
    // A v1 Custom zoom held a render scale, not a percentage. It is reset
    // rather than reinterpreted -- to FitPage, not FitWidth: see the
    // deviation note at the top of this task.
    REQUIRE(r->tabs[0].zoom_mode == SessionZoom::FitPage);
}

TEST_CASE("SessionState v2 treats a missing version key as version 1",
          "[core][session][json][migration]") {
    // The parser writes out.version only when the key is present, and
    // SessionState default-initialises it to kSessionVersion. Once that
    // constant became 2, a versionless document would silently claim to be v2
    // and skip the migration, restoring an old render scale as a percentage.
    const std::string versionless =
        "{\"window\":{\"flags\":0,\"show\":1,\"x\":0,\"y\":0,\"w\":800,\"h\":600},"
        "\"active\":0,\"tabs\":[{\"path\":\"C:\\\\a\\\\one.pdf\",\"page\":0,"
        "\"zoom_mode\":\"custom\",\"zoom_scale\":3.0}]}";
    auto r = from_json(versionless);
    REQUIRE(r.has_value());
    REQUIRE(r->version == 2);
    REQUIRE(r->tabs[0].zoom_mode == SessionZoom::FitPage);
}

TEST_CASE("SessionState v2 rejects a version above the current one",
          "[core][session][json]") {
    REQUIRE_FALSE(from_json("{\"version\":3,\"tabs\":[]}").has_value());
}
```

Then fix the four existing v1-touching sites:

- `:42` — change `s.version = 1;` to `s.version = 2;` so the round-trip case
  exercises a current-version document and its `:57` Custom assertion stays valid.
- `:90-183` — change each `\"version\":1` fixture literal to `\"version\":2`.
- `:120` — `REQUIRE_FALSE(from_json("{\"version\":2,…}"))` now describes a *valid*
  document; delete that line (the new "rejects a version above the current one" case
  above replaces its intent).
- `:182-185` — the "rejects a Custom tab with zoom_scale 0" case must use a
  `"version":2` document, or migration resets the Custom mode before `validate`
  ever sees the bad scale and the case silently stops testing anything.

- [ ] **Step 2: Run the tests to verify they fail**

```bash
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" --test-dir build -C Release -R "SessionState v2" --output-on-failure
```

Build first — a `-R` run cannot execute a test that has not been compiled and
re-discovered, and `ctest` on its own would report "No tests were found!!!", which
reads like a pass if you only glance at the banner:

```bash
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --target litepdf_unit_tests --config Release
```

Then run the ctest command above.

Expected: **two** of the three cases FAIL — the two migration cases, because
`r->version` is still 1 and `zoom_mode` is still `Custom`. The third,
"rejects a version above the current one", **already passes**: `kSessionVersion` is
still 1, so a `version:3` document is rejected by the existing check. That is correct
and the test is not redundant — after Step 3 it is the only thing standing between a
v3 file written by some future release and a v2 binary that would otherwise try to
migrate it. Do not contrive a way to make it fail first.

Then run the full suite once to see the four pre-existing v1 sites from the table
above go red as well; that red is expected and Step 3 fixes it.

- [ ] **Step 3: Bump the version constant**

`src/core/SessionState.hpp:12`:

```cpp
// v2 (PR-A1): SessionTab::zoom_scale changed meaning from a point->pixel render
// scale to a user-facing magnification percentage. from_json migrates v1 by
// resetting Custom zooms to FitPage, not FitWidth: this release ships no wheel
// scrolling, so a FitWidth A4 page would be unreadable and unnavigable;
// PR-A2 revisits this once ScrollMath lands. See SessionState.cpp.
inline constexpr int kSessionVersion = 2;
```

- [ ] **Step 4: Track version presence and migrate**

In `src/core/SessionState.cpp`, give the parser a "saw the key" flag. `Json` is a
**class** whose only data members (`s_`, `i_`) sit under its single `private:` label
(`:87-89`), and `from_json` is a free function that must read the flag — so put the
flag in the `public:` section, immediately after `parse_root_object_into`. Dropping it
next to `s_`/`i_` compiles the parser fine and then fails at the read site with C2248.

```cpp
public:
    // Set by parse_object when the "version" key is present. from_json needs to
    // distinguish "absent" from "explicitly 1", because SessionState::version
    // default-initialises to the CURRENT version -- so an absent key would
    // otherwise claim to be v2 and skip the migration.
    bool saw_version = false;
```

```cpp
            if (key=="version") { out.version = (int)parse_int(); saw_version = true; }
```

Loosen `validate` (`:280-281`) so it accepts anything the migration can normalize,
and let `from_json` own the version policy:

```cpp
bool validate(const SessionState& s) {
    // from_json has already migrated, so by this point the version must be
    // exactly current. Older versions are handled before we get here; newer
    // ones are rejected, since we cannot know what they mean.
    if (s.version != kSessionVersion) return false;
```

(unchanged text — it stays correct because migration runs first)

Rewrite `from_json` (`:337-346`):

```cpp
namespace {
// v1 -> v2: the stored scale changed from a render scale to a percentage. The
// only Custom values v1 could persist were preset-table numbers (the removed
// direct-scale setter clamped on write), so they are valid percentages only by
// coincidence and never corresponded to what the user saw. Reset them rather
// than reinterpret.
//
// Deliberately avoids naming the removed API here: the Definition of Done greps
// src/ for it, and a mention in a comment would make that check unpassable.
void migrate_v1_to_v2(SessionState& s) {
    for (auto& t : s.tabs) {
        if (t.zoom_mode == SessionZoom::Custom) {
            // Reset to FitPage, NOT FitWidth -- see the deviation note at the
            // top of this task. This release's paint path draws at natural
            // size and ships no wheel scrolling, so a FitWidth A4 page would
            // be unreadable with no way to reach the rest of it.
            t.zoom_mode  = SessionZoom::FitPage;
            t.zoom_scale = 1.0f;
        }
    }
    s.version = 2;
}
}  // namespace

std::optional<SessionState> from_json(std::string_view json) {
    try {
        SessionState s;
        Json j(json);
        j.parse_root_object_into(s);
        // A document with no "version" key predates the field's use as a
        // migration gate; treat it as v1 rather than letting the struct's
        // default (always the CURRENT version) wave it through unmigrated.
        const int doc_version = j.saw_version ? s.version : 1;
        if (doc_version > kSessionVersion) return std::nullopt;
        if (doc_version == 1) migrate_v1_to_v2(s);
        if (!validate(s)) return std::nullopt;
        return s;
    } catch (const ParseError&) {
        return std::nullopt;
    }
}
```

If `parse_root_object_into` does not expose the `Json` instance, hoist it to a local
as shown; that is the only structural change to the function.

- [ ] **Step 5: Run the tests to verify they pass**

```bash
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" --test-dir build -C Release -R "SessionState v2" --output-on-failure
```

Expected: the three new cases pass. Then run the full suite —
`ctest --test-dir build -C Release` — and confirm the 19 pre-existing `to_json` /
`from_json` cases are green too; a filtered run cannot show you what the version bump
broke elsewhere in that file.

- [ ] **Step 6: Commit**

```bash
git add src/core/SessionState.hpp src/core/SessionState.cpp tests/unit/test_session_state.cpp
git commit -m "feat(core): session v2 with a v1-accepting migration (PR-A1 Task 5)"
```

---

## Task 6: Fail-closed session backup before the first v2 write

**Files:**
- Modify: `src/core/SessionStore.cpp:9-27` (`save_session`)
- Modify: `tests/unit/test_session_store.cpp`

**Interfaces:**
- Consumes: nothing from Task 5 at the call level. The v1 detection deliberately
  scans the raw bytes rather than calling `from_json`, because `from_json` migrates
  and would report version 2 for a file that is still v1 on disk.
- Produces: nothing new.

Downgrading is a one-way door: once a v2 file exists, the shipped v1.2.0 binary
rejects it outright and the user loses window placement, tabs and pages. The backup
is the only recourse, so a failure to write it must abort the save rather than
destroy the last readable copy.

- [ ] **Step 1: Write the failing tests**

Add to `tests/unit/test_session_store.cpp`. The new cases use `std::ostringstream`,
and that file currently includes only `<fstream>` — add `#include <sstream>` at the
top rather than relying on a transitive include.

```cpp
TEST_CASE("SessionStore backup keeps a v1 copy on first v2 save",
          "[core][session][store][migration]") {
    auto file = temp_session();
    const std::string v1 =
        "{\"version\":1,\"window\":{\"flags\":0,\"show\":1,"
        "\"x\":0,\"y\":0,\"w\":800,\"h\":600},\"active\":0,\"tabs\":[]}";
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(v1.data(), (std::streamsize)v1.size());
    }

    SessionState s;   // defaults to the current version
    REQUIRE(save_session(file, s));

    auto bak = file;
    bak.replace_extension(L".v1.bak");
    REQUIRE(std::filesystem::exists(bak));

    std::ifstream in(bak, std::ios::binary);
    std::ostringstream ss; ss << in.rdbuf();
    REQUIRE(ss.str() == v1);   // byte-identical to the original
}

TEST_CASE("SessionStore backup is not rewritten once the file is v2",
          "[core][session][store][migration]") {
    auto file = temp_session();
    SessionState s;
    REQUIRE(save_session(file, s));       // creates a v2 file, no backup needed
    auto bak = file;
    bak.replace_extension(L".v1.bak");
    REQUIRE_FALSE(std::filesystem::exists(bak));
    REQUIRE(save_session(file, s));       // and still none on a second save
    REQUIRE_FALSE(std::filesystem::exists(bak));
}

TEST_CASE("SessionStore backup failure aborts the save",
          "[core][session][store][migration]") {
    auto file = temp_session();
    const std::string v1 =
        "{\"version\":1,\"window\":{\"flags\":0,\"show\":1,"
        "\"x\":0,\"y\":0,\"w\":800,\"h\":600},\"active\":0,\"tabs\":[]}";
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(v1.data(), (std::streamsize)v1.size());
    }
    // Occupy the backup path with a DIRECTORY so the copy cannot succeed.
    auto bak = file;
    bak.replace_extension(L".v1.bak");
    std::filesystem::create_directory(bak);

    SessionState s;
    REQUIRE_FALSE(save_session(file, s));

    std::ifstream in(file, std::ios::binary);
    std::ostringstream ss; ss << in.rdbuf();
    REQUIRE(ss.str() == v1);   // untouched
}
```

- [ ] **Step 2: Run the tests to verify they fail**

```bash
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" --test-dir build -C Release -R "SessionStore backup" --output-on-failure
```

Build first, for the same reason as Task 5 Step 2 — an un-rebuilt filter run reports
"No tests were found!!!" rather than the red proof this step is for:

```bash
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --target litepdf_unit_tests --config Release
```

Then re-run the filter above.

Expected: all three `SessionStore backup` cases FAIL — no backup is written, and the
third save succeeds when it should abort.

- [ ] **Step 3: Implement the backup**

In `src/core/SessionStore.cpp`, add at the top of `save_session`, before the temp
file is written:

**Use the parser, not a byte scan.** An earlier draft of this task searched the raw
text for `"version"`. That is unsafe in two independent ways, both of which silently
skip the backup or write a mislabelled one:

- `parse_object` (`SessionState.cpp:260-270`) has no duplicate-key guard, so in
  `{"version":2,"version":1,…}` the **last** key wins — while a scan finds the first.
- keys go through `parse_string`, which decodes `\u` escapes (see the existing case
  "from_json decodes a uXXXX BMP escape to UTF-8"), so `"version"` is the
  `version` key to the parser and invisible to a scan.

Add to `src/core/SessionState.hpp`, next to `from_json`:

```cpp
// Report the version a document DECLARES, without migrating or validating it.
// Returns nullopt if the document does not parse; treats an absent "version"
// key as 1, matching from_json.
//
// SessionStore uses this to decide whether the file on disk is still v1. A raw
// text scan cannot be trusted for that decision: the parser decodes \u escapes
// in keys and lets a later duplicate key win, so a scan and the parser can
// disagree about the one field the backup turns on.
std::optional<int> peek_version(std::string_view json);
```

and in `src/core/SessionState.cpp`, beside `from_json`:

```cpp
std::optional<int> peek_version(std::string_view json) {
    try {
        SessionState s;
        Json j(json);
        j.parse_root_object_into(s);
        return j.saw_version ? s.version : 1;
    } catch (const ParseError&) {
        return std::nullopt;
    }
}
```

Then in `src/core/SessionStore.cpp`:

```cpp
namespace {
// True iff `file` currently holds a parseable v1 session document. Unparseable
// or absent files return false: there is nothing worth preserving, and the save
// must not be blocked by a corrupt predecessor.
bool existing_file_is_v1(const std::filesystem::path& file) {
    std::error_code ec;
    const auto sz = std::filesystem::file_size(file, ec);
    if (ec || sz > kMaxSessionBytes) return false;
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss; ss << in.rdbuf();
    const auto v = peek_version(ss.str());
    return v.has_value() && *v == 1;
}
}  // namespace

bool save_session(const std::filesystem::path& file, const SessionState& s) {
    // One-way door: once a v2 file exists, a rolled-back v1.2.0 binary rejects
    // it and the user loses their whole session. Keep the last v1 copy, and
    // FAIL CLOSED -- a save that destroys the only recoverable copy is exactly
    // the failure this guards against.
    if (existing_file_is_v1(file)) {
        std::filesystem::path bak = file;
        bak.replace_extension(L".v1.bak");
        std::error_code ec;
        std::filesystem::copy_file(
            file, bak, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) return false;
    }

    std::filesystem::path tmp = file;
    // ... existing body unchanged ...
```

No new includes are needed: `peek_version` replaced the byte scan that wanted
`<cctype>`, and `SessionStore.cpp` already has `<fstream>`, `<sstream>` and
`<system_error>`.

- [ ] **Step 4: Run the tests to verify they pass**

```bash
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" --test-dir build -C Release -R "SessionStore backup" --output-on-failure
```

Expected: the three new cases pass. Then run the full suite and confirm the four
pre-existing `SessionStore` cases — including "save leaves the prior file intact when
it cannot replace" — are still green, since the new early-return sits in front of
them.

- [ ] **Step 5: Commit**

```bash
git add src/core/SessionStore.cpp tests/unit/test_session_store.cpp
git commit -m "feat(core): fail-closed v1 session backup before the first v2 write (PR-A1 Task 6)"
```

---

## Task 7: Default to FitPage, verify on screen, note the one-way door

**Files:**
- Modify: `src/core/DocumentView.cpp:48` (default zoom mode)
- Modify: `CHANGELOG.md`

**Interfaces:**
- Consumes: everything above.
- Produces: the shipped default view for this PR. PR-A2 restores FitWidth once the
  wheel exists.

With Defect 2 fixed, FitWidth would mean page width equals canvas width — an A4 page
on a maximized 16:9 window renders roughly 2.8x taller than the viewport. PR-A1 ships
no wheel scrolling and PgDn jumps to the next page, so a reader would silently never
see the lower two thirds of any page. FitPage keeps the observable default identical
to today's while all the machinery underneath changes.

- [ ] **Step 1: Change the default and the test that pins it**

`tests/unit/test_document_view.cpp:35` asserts the constructor's default:

```cpp
    REQUIRE(view.zoom_mode() == DocumentView::ZoomMode::FitWidth);
```

Change it to `FitPage` and add a comment naming the reason, so the next person to
flip it back (PR-A2) finds the trail:

```cpp
    // PR-A1 ships FitPage; PR-A2 restores FitWidth once wheel scrolling exists.
    REQUIRE(view.zoom_mode() == DocumentView::ZoomMode::FitPage);
```

Then `src/core/DocumentView.cpp:48`:

```cpp
    // PR-A1 ships FitPage so the default view still shows a whole page: with
    // the paint path now drawing at natural size, FitWidth overflows the
    // viewport vertically and this PR has no wheel scrolling to navigate it.
    // PR-A2 restores FitWidth when ScrollMath lands.
    DocumentView::ZoomMode zm           = DocumentView::ZoomMode::FitPage;
```

- [ ] **Step 2: Build and run the full suite**

```bash
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --config Release --parallel
```

From the repo root:

```bash
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" --test-dir build -C Release --output-on-failure
```

Expected: green, one `[!shouldfail]` aside.

- [ ] **Step 3: Verify on screen that zoom now does something**

This is the whole point of the PR and no unit test can see it. Drive the GUI with the
topmost-window screenshot method — a foreground-based capture loses the race against
other applications on this machine, so the window must be pinned with
`SetWindowPos(hwnd, HWND_TOPMOST, 0,0,0,0, SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE)`
before each `CopyFromScreen`.

Script it as: launch `build\Release\litepdf.exe tests\fixtures\large.pdf` with
`LITEPDF_NO_RESTORE=1` set (suppresses the crash-restore prompt), maximize, pin
topmost, screenshot; then `PostMessageW(hwnd, WM_COMMAND, 40010, 0)` three times with
a ~900 ms pause each (`IDM_ZOOM_IN`), screenshot again; finish with
`PostMessageW(hwnd, WM_CLOSE, 0, 0)` so `running.lock` clears.

Confirm all five:

0. **The rendered pixmap width equals the canvas width at 200 %** — the check for
   Defect 3, and the only verification it gets: no unit test can see it, and it is
   invisible to the eye because the paint path scales the bitmap either way.

   There is no existing log line that carries the pixmap size. `--log-timings`
   (`src/main.cpp:28` — a command-line flag, **not** an environment variable) emits
   only `T0->T1 … T0->T4` milliseconds plus four sub-marks
   (`src/ui/ColdStartTimer.cpp:67-73`), and `litepdf-cli` renders at a hardcoded
   `scale = 1.0f`, so neither answers this.

   Add a temporary probe for the duration of this check: in the
   `WM_USER_RENDER_DONE` handler, right after `w_px`/`h_px` are read
   (`src/ui/PdfCanvas.cpp:508-510`), emit

   ```cpp
   { wchar_t b[128]; RECT rc; GetClientRect(hwnd_, &rc);
     swprintf(b, 128, L"[A1] pixmap=%dx%d canvas=%ldx%ld dpi=%u\n",
              w_px, h_px, rc.right - rc.left, rc.bottom - rc.top,
              GetDpiForWindow(hwnd_));
     OutputDebugStringW(b); }
   ```

   Read it with DebugView (or the VS output window), then **remove the probe before
   committing** — it is scaffolding, not part of the deliverable. On a 200 % display
   the ratio `pixmap / canvas` was 2.0 before this PR; it must now be 1.0.
1. The default view shows the whole page — unchanged from v1.2.0.
2. After three Zoom In commands the page is **visibly larger**. Before this PR the
   two screenshots were pixel-identical; that is the regression being fixed.
3. `%LOCALAPPDATA%\LitePDF\session.json` shows `"version":2` and a `zoom_scale`
   in the preset range, not a double-digit render scale.
4. Arrow keys pan the enlarged page and stop at its edges rather than pushing it off
   the canvas.

- [ ] **Step 4: Note the one-way door in the changelog**

Add under the unreleased heading in `CHANGELOG.md`:

```markdown
### Changed

- Zoom now changes what you see. Zoom In / Zoom Out were previously inert: the
  zoom level was compared against a table in different units, and the paint path
  re-fitted every page to the window regardless. Pages also rendered at twice the
  needed resolution on 200% displays.
- The default view is Fit Page for this release; Fit Width returns with mouse-wheel
  scrolling.

### Note

- `session.json` is upgraded to version 2 on first save. Your previous file is kept
  as `session.v1.bak` in the same folder. Version 2 cannot be read by LitePDF 1.2.0
  or earlier, so if you roll back, restore that backup to keep your tabs.
```

- [ ] **Step 5: Commit**

```bash
git add src/core/DocumentView.cpp tests/unit/test_document_view.cpp CHANGELOG.md
git commit -m "feat(core): default to Fit Page until wheel scrolling lands (PR-A1 Task 7)"
```

---

## Definition of Done

- `ctest --test-dir build -C Release` is green from the repo root.
- The **five** on-screen checks in Task 7 Step 3 pass (items 0 through 4), with before/after screenshots kept. Item 0 is the only verification Defect 3 gets — no unit test can observe it — so it is not optional.
- The old zoom API is fully gone: `rg -n "set_zoom_mode\(|zoom_scale\(\)|set_zoom_scale" src/` returns nothing. (Do not grep bare `set_zoom_mode` — it is a prefix of the new `set_zoom_mode_fit_width` / `set_zoom_mode_fit_page` and always matches.)
- `WM_USER_RENDER_DONE` is untouched: in `git diff main -- src/ui/PdfCanvas.cpp`, no hunk falls inside the `case WM_USER_RENDER_DONE:` / `case WM_USER_RENDER_DONE_RIGHT:` block (from those labels through the `return 0;` that closes them). Do not state this as a fixed line range — Task 4 edits `scroll_into_view` above it and shifts every line number below.
- No `VERSION` bump.
