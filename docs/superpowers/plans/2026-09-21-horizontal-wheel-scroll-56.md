# Horizontal Wheel Scrolling (#56) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give horizontal scrolling a mouse route: `WM_MOUSEHWHEEL` (a tilt wheel or a sideways touchpad swipe) and Shift + the plain wheel both pan a zoomed-in page left and right.

**Architecture:**
- **Pure logic.** `ui/detail/ScrollMath.hpp` gains one pure helper, `rightward_delta`, which turns either input into one sign convention. The existing `consume_notches` and `wheel_step_dip` do the rest.
- **The canvas.** `PdfCanvas` gains `on_hwheel_scroll`, which writes `pan_x` only and never turns the page. It also gains `bitmap_is_stale()`, the vertical wheel's bitmap-identity predicate moved into a helper both wheels call.
- **The page box.** The status bar's page box forwards `WM_MOUSEHWHEEL` to the canvas, as it already does `WM_MOUSEWHEEL`, and returns the canvas's result.

**Tech Stack:** C++17, Win32, Direct2D, Catch2 v3.5.4, CMake + MSVC v143 (VS 2022 BuildTools).

**Source spec:** `docs/superpowers/specs/2026-09-21-horizontal-wheel-scroll-design.md` (decisions H1-H9, verification §4, residual risks R1-R5). It passed the spec gate on this branch: Opus, Sonnet, terra@high and luna@max.

## Global Constraints

- **Catch2 `TEST_CASE` names are ASCII and start with their subsystem** (`ScrollMath …`). `ctest -R` matches the **name**, never the tag. Confirm a filter's count with `ctest --test-dir build -C Release -N -R <filter>` before trusting a green run from it.
- **Tests build Release, never Debug.** MuPDF is `MT_StaticRelease`, so Debug fails with `LNK2038`. Verify through `ctest --test-dir build -C Release`, not only by running the test exe.
- **Use the VS 2022 BuildTools `cmake` and `ctest`**, which configured `build/`:
  - `"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"`, with `ctest.exe` beside it.
  - This plan writes `cmake` / `ctest` for brevity.
  - Do not trust a bare `cmake` on PATH: in Git Bash it resolves a MinGW build that did not configure `build/`.
- **Run tests from the repo root.** Fixtures resolve relative to it.
- **No `git worktree`.** MuPDF builds inside its submodule tree, so a fresh worktree means a full MuPDF rebuild. Work in the main checkout on branch `feat/horizontal-wheel-scroll`.
- **`VERSION` is not bumped** (stays `1.3.0`). The About-dialog literal in `MainWindow.cpp` stays untouched.
- **Binary size:** `build/Release/litepdf.exe` must stay under **19,000,000 bytes**. Record the baseline and the final size.
- **PIMPL discipline:** `PdfCanvas.hpp` stays free of `<mupdf/fitz.h>`. `ScrollMath.hpp` stays free of Win32, Direct2D and MuPDF; it is headless-tested.
- **Do not weaken the vertical wheel's two guards**, the `wheel_flip_seq` latch and the bitmap-identity test. This is decision #4 of the zoom/page-nav spec. Task 2 moves the identity predicate into a helper **unchanged**; the latch is not touched.
- **Cite symbols, not line numbers**, in code comments and commit messages. Line numbers go stale.
- **All artifacts in English.** Commit messages end with the implementing session's own `Co-Authored-By:` trailer; the trailers shown here are Opus 5's.
- **CHANGELOG** entries go under `## [Unreleased]`, with no version heading.

## Branch and PR shape

- One PR. Branch `feat/horizontal-wheel-scroll`, off `main` @ `1d6f838`.
- The branch already holds the spec (three commits) and this plan.
- Closes #56.

## Plan-time corrections to the spec's §4.2 mechanics

The spec's decisions H1-H9 are implemented as written. Its **verification mechanics** are refined here. No correction weakens a spec row. C8 and C9 add rows, and C9 moves row 3 to a document where it can fail.

**C1. The probe is applied in the main checkout and reverted with `git checkout`, not in a worktree.** A worktree would rebuild MuPDF (see Global Constraints). The feature is committed first, and then the probe is added as uncommitted edits to `src/ui/PdfCanvas.cpp`. After the run, `git checkout -- src/ui/PdfCanvas.cpp` restores the committed file byte for byte. `git status --short` must be empty afterwards.

**C2. Rows 10 and 11 get a deterministic race window.** The spec relied on a slow-rendering page, but an L1 cache hit returns almost instantly. Instead, the probe delays every render completion by 1500 ms on the **worker** thread, but only while a flag file exists. The driver creates the flag just for those two rows. The spec's VOID conditions are still checked.
- Row 10 uses `simple.pdf` as tab A and `search.pdf` as tab B; `large.pdf` is not needed.

**C3. Every wheel message is `SendMessage`d, not posted.** Posting and sending reach the same `handle_message` arm, and the modifiers come from wParam either way. Sending makes each probe line exist before the driver reads the log, and it returns the `LRESULT` that rows 9 and 9b need.

**C4. The probe computes the branch label from the observed state:**
- `stale-drop`: a horizontal message arrived while `bitmap_is_stale()` was true.
- `step`: `pan_x` changed.
- `no-move`: horizontal, not stale, and `pan_x` unchanged. This covers a fractional notch (the residual is logged, so rows can tell), an edge clamp, and no extent.
- `other`: any non-horizontal message, i.e. plain wheel or Ctrl+wheel.

**C5. `Read-Pan` is a zero-delta `WM_MOUSEHWHEEL`.** A delta of 0 consumes no notch and never moves the pan, and the probe logs the canvas state for it. That lets the driver read `pan_x`, `pan_y`, the page, the content box and the viewport at any moment. Its one side effect: while the bitmap is stale it zeroes the horizontal residual, exactly as a real notch would. No row depends on a residual across a stale window.

**C6. The pan is reset with the arrow keys.** Sixty `WM_KEYDOWN` `VK_LEFT` + `VK_UP` pairs, sent to the canvas, pin the pan at (0, 0). That needs neither the foreground nor a mouse capture.

**C7. Row 11's notch is leftward (−120) from the bottom-right corner.** A rightward notch there would be clamped and move nothing, and the row needs a `step`.

**C8. Row 12 is added:** Ctrl+Shift+wheel still zooms (H2). The spec's §4.3 asks the user to check this by hand, and it costs one message to automate.

**C9. Row 3 runs on a multi-page document, and row 3b is added** (found at the plan gate).
- On the single-page `simple.pdf`, a wrong implementation that turned the page at the horizontal edge would clamp its target to the current page and change nothing, so "the page does not change" could not fail. H6 would have had no failing row.
- Row 3 therefore runs on `search.pdf` (7 same-size pages), on a page that is not the last.
- Row 3b sends leftward notches at the left edge on a page after the first, where a backward page turn is possible.

**C10. Every "after the render lands" read waits for it** (found at the plan gate). `Wait-Settled` polls until the probe reports the bitmap is no longer stale. A zoom re-renders the same page under the same epoch, so it is never stale; there the driver waits for the content box itself to change. A fixed sleep could read the pan before `apply_anchor` ran, and row 7 would then PASS on an implementation that zeroes `pan_x` when the new page lands.

## Known limitations (recorded in the spec, not fixed here)

| | |
|---|---|
| R1 | The `WM_MOUSEHWHEEL` sign is not checked on real hardware: only a plain wheel is available. |
| R2 | Emulating drivers (Logitech SetPoint) are untested. H8's TRUE return is the mitigation. |
| R3 | A wheel notch during a selection drag moves the page under a still pointer. Inherited from the vertical wheel. |
| R4 | In spread mode a page flip can reset `pan_x`, depending on which half lands first. This is existing behaviour with the arrow keys. |
| R5 | The arrow keys and the hand tool lack the tab-switch guard that H7 adds for the wheel. A separate follow-up. |
| R6 | **No GUI row can tell which wheel setting each input reads (H5)** while `SPI_GETWHEELSCROLLCHARS` equals `SPI_GETWHEELSCROLLLINES`. Both are 3 on the development machine. Changing a system setting for the run is not done. H5's per-source choice is therefore checked by reading `on_hwheel_scroll` at the merge gate (Task 3 Step 6, item 5). |

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `src/ui/detail/ScrollMath.hpp` | modify | `HWheelSource`, `rightward_delta`; `wheel_step_dip`'s `vp_h` → `vp_extent` rename — pure |
| `tests/unit/test_scroll_math.cpp` | modify | three `ScrollMath` cases |
| `src/ui/PdfCanvas.hpp` | modify | declare `on_hwheel_scroll`, `bitmap_is_stale` |
| `src/ui/PdfCanvas.cpp` | modify | `Impl::hwheel_residual`; `bitmap_is_stale`; `on_hwheel_scroll`; the `WM_MOUSEHWHEEL` arm and the Shift branch |
| `src/ui/StatusBar.hpp` / `.cpp` | modify | `OnWheel` carries the message and returns an `LRESULT`; the page box forwards both wheel messages |
| `src/ui/MainWindow.cpp` | modify | the `set_on_wheel` callback forwards the message it was given and returns the result |
| `CHANGELOG.md`, `README.md` | modify | the feature, the Shift+wheel behaviour change, one shortcut row |

No new files. `tests/CMakeLists.txt` already compiles `test_scroll_math.cpp`.

## GUI verification

- Task 2 ends with a scripted GUI run: a scratch driver, `build/gui/hwheel-drive.ps1`, and a check script, `build/gui/hwheel-checks.ps1`.
- Both are written out in full **inside Task 2 only**, because each task's brief is extracted on its own and no other task runs them.
- Neither file is committed (`build/` is git-ignored).

---

## Task 1: Horizontal wheel math in ScrollMath

**Files:**
- Modify: `src/ui/detail/ScrollMath.hpp`
- Test: `tests/unit/test_scroll_math.cpp`

**Interfaces:**
- Consumes: nothing new.
- Produces, in namespace `litepdf::ui`:
  - `enum class HWheelSource { Tilt, Shift };`
  - `inline int rightward_delta(int raw, HWheelSource src) noexcept;`, where > 0 means "reveal the content to the right".
  - `inline float wheel_step_dip(int notches, unsigned lines_per_notch, float vp_extent) noexcept;`. Only the parameter name changes; the body is identical.

- [ ] **Step 1: Record the baseline**

```bash
git status --short
git log --oneline -1
cmake --build build --config Release
ctest --test-dir build -C Release
ctest --test-dir build -C Release -N -R ScrollMath
```

`git status --short` must print nothing, and the current branch must be `feat/horizontal-wheel-scroll`, with this plan committed and no code change yet (`git diff main --stat -- src tests` is empty). Record:
- the total passing count, **N0** (re-measure it; do not quote an old figure);
- the `ScrollMath` count (13 at plan time);
- `(Get-Item build\Release\litepdf.exe).Length` (7,300,608 at plan time).

- [ ] **Step 2: Write the failing tests**

In `tests/unit/test_scroll_math.cpp`, extend the using-declarations block:

```cpp
using litepdf::ui::apply_wheel;
using litepdf::ui::consume_notches;
using litepdf::ui::Flip;
using litepdf::ui::HWheelSource;
using litepdf::ui::rightward_delta;
using litepdf::ui::wheel_step_dip;
```

Append at the end of the file:

```cpp
// #56: horizontal wheel input. pan_x lives in [vp_w - content_w, 0] with 0 at
// the page's LEFT edge; rightward_delta > 0 reveals the content to the right,
// and the canvas subtracts the resulting step from pan_x (VK_RIGHT's direction).

TEST_CASE("ScrollMath tilt wheel keeps its sign: tilting right is rightward",
          "[ui][scroll]") {
    // WM_MOUSEHWHEEL: "A positive value indicates that the wheel was rotated
    // to the right" -- already the rightward convention.
    REQUIRE(rightward_delta(120, HWheelSource::Tilt) == 120);
    REQUIRE(rightward_delta(-120, HWheelSource::Tilt) == -120);
    REQUIRE(rightward_delta(40, HWheelSource::Tilt) == 40);
}

TEST_CASE("ScrollMath Shift wheel toward the user scrolls right",
          "[ui][scroll]") {
    // WM_MOUSEWHEEL is negative when the wheel rolls toward the user -- which
    // scrolls DOWN without Shift, and scrolls RIGHT with it.
    REQUIRE(rightward_delta(-120, HWheelSource::Shift) == 120);
    REQUIRE(rightward_delta(120, HWheelSource::Shift) == -120);
    REQUIRE(rightward_delta(-40, HWheelSource::Shift) == 40);
}

TEST_CASE("ScrollMath Shift and tilt describing one motion give the same notches",
          "[ui][scroll]") {
    // The same rightward-then-back-then-rightward motion, once from a tilt
    // wheel and once from Shift + the plain wheel (whose raw deltas carry the
    // opposite sign), must fire the same notches in the same places.
    const int tilt_raw[]  = {40, 40, 50, -30, 150};
    const int shift_raw[] = {-40, -40, -50, 30, -150};
    const int expected[]  = {0, 0, 1, 0, 1};
    int tilt_residual = 0;
    int shift_residual = 0;
    for (int i = 0; i < 5; ++i) {
        const int t = consume_notches(rightward_delta(tilt_raw[i], HWheelSource::Tilt),
                                      tilt_residual);
        const int s = consume_notches(rightward_delta(shift_raw[i], HWheelSource::Shift),
                                      shift_residual);
        REQUIRE(t == expected[i]);
        REQUIRE(s == expected[i]);
        REQUIRE(tilt_residual == shift_residual);
    }
}
```

- [ ] **Step 3: Run the build and see it fail**

```bash
cmake --build build --target litepdf_unit_tests --config Release
```

Expected: the build FAILS in `test_scroll_math.cpp`, with `'HWheelSource': is not a member of 'litepdf::ui'` / `'rightward_delta': is not a member of 'litepdf::ui'` (C2039/C2065 family). Any other error means the edit went wrong.

- [ ] **Step 4: Implement**

In `src/ui/detail/ScrollMath.hpp`, extend the header comment. Replace:

```cpp
// ViewportMath.hpp). Wheel UP is delta > 0 and moves the reader toward the top,
// so step > 0 and pan_y increases toward 0. This matches the arrow keys:
// VK_UP calls pan_by(0, +100).
```

with:

```cpp
// ViewportMath.hpp). Wheel UP is delta > 0 and moves the reader toward the top,
// so step > 0 and pan_y increases toward 0. This matches the arrow keys:
// VK_UP calls pan_by(0, +100).
//
// HORIZONTAL (#56). pan_x lives in [vp_w - content_w, 0], with 0 at the page's
// LEFT edge. rightward_delta turns either horizontal input into a delta where
// > 0 reveals the content to the RIGHT; the canvas then SUBTRACTS the step
// from pan_x. This matches the arrow keys: VK_RIGHT calls pan_by(-100, 0).
```

Replace the head of `wheel_step_dip`:

```cpp
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
```

with:

```cpp
// Scroll magnitude in DIPs for `notches` whole notches, along the axis that
// `vp_extent` measures: the viewport HEIGHT for the vertical wheel (positive =
// toward the top of the page), its WIDTH for the horizontal one (positive =
// rightward, see rightward_delta). `lines_per_notch` is the setting for the
// control that moved -- SPI_GETWHEELSCROLLLINES, or SPI_GETWHEELSCROLLCHARS
// for a tilt wheel.
inline float wheel_step_dip(int notches, unsigned lines_per_notch,
                            float vp_extent) noexcept {
    if (notches == 0) return 0.0f;
    float per_notch;
    if (lines_per_notch == kWheelPageScroll) {
        // "One screen at a time". A full viewport would land exactly on the
        // far edge -- vertically, the very next notch would then flip the page
        // -- with no overlap for the reader to reacquire their place. 90%
        // leaves a strip. Documented for the lines setting only; a chars value
        // of UINT_MAX lands here too, which no Windows UI can set.
        per_notch = vp_extent * 0.9f;
```

The rest of `wheel_step_dip` is unchanged. Directly after its closing `}`, and before `struct WheelResult`, add:

```cpp
// Which control produced a horizontal wheel message (#56).
enum class HWheelSource {
    Tilt,    // WM_MOUSEHWHEEL: a tilt wheel, or a sideways touchpad swipe
    Shift,   // WM_MOUSEWHEEL with Shift held: the plain wheel, turned sideways
};

// Normalise a horizontal wheel delta so that > 0 means "reveal the content to
// the RIGHT" (pan_x decreases, the direction VK_RIGHT pans).
//
// WM_MOUSEHWHEEL is positive when the wheel tilts right, which already is that
// direction. Shift + the plain wheel follows the browser convention: rolling
// the wheel toward the user (delta < 0, which scrolls DOWN without Shift)
// scrolls right, so its sign flips. Negating is safe: the raw value comes from
// GET_WHEEL_DELTA_WPARAM, a 16-bit short.
inline int rightward_delta(int raw, HWheelSource src) noexcept {
    return (src == HWheelSource::Shift) ? -raw : raw;
}
```

- [ ] **Step 5: Run the tests and see them pass**

```bash
cmake --build build --config Release
ctest --test-dir build -C Release -N -R ScrollMath
ctest --test-dir build -C Release
```

Expected:
- the full build is clean, with no new warnings;
- the `-N -R ScrollMath` count is **16** (13 + 3);
- the full run passes **N0 + 3**.

The full build matters: `PdfCanvas.cpp` calls `wheel_step_dip` positionally and must still compile after the rename.

- [ ] **Step 6: Commit**

```bash
git add src/ui/detail/ScrollMath.hpp tests/unit/test_scroll_math.cpp
git commit -m "feat(scroll): horizontal wheel sign convention in ScrollMath

rightward_delta turns WM_MOUSEHWHEEL and Shift + the plain wheel into one
convention, > 0 revealing the content to the right. wheel_step_dip's viewport
parameter becomes vp_extent: its body never depended on the axis.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 2: Route horizontal wheel input through the canvas and the page box, and verify it

**Files:**
- Modify: `src/ui/PdfCanvas.hpp`, `src/ui/PdfCanvas.cpp`
- Modify: `src/ui/StatusBar.hpp`, `src/ui/StatusBar.cpp`
- Modify: `src/ui/MainWindow.cpp`
- Scratch, not committed: `build/gui/hwheel-drive.ps1`, `build/gui/hwheel-checks.ps1`, plus temporary probe edits to `src/ui/PdfCanvas.cpp`

**Interfaces:**
- Consumes (Task 1): `litepdf::ui::HWheelSource`, `rightward_delta(int, HWheelSource)`, `consume_notches(int, int&)` and `wheel_step_dip(int, unsigned, float)`. `PdfCanvas.hpp` already includes `ui/detail/ScrollMath.hpp`.
- Produces:
  - `LRESULT PdfCanvas::on_hwheel_scroll(int raw_delta, litepdf::ui::HWheelSource src);` (private).
  - `bool PdfCanvas::bitmap_is_stale() const;` (private).
  - `StatusBar::OnWheel` = `std::function<LRESULT(UINT msg, WPARAM, LPARAM)>`.

**Why there is no failing-test-first step here.** `PdfCanvas` has no headless harness: it is an HWND with a Direct2D target. Its behaviour is pinned by the scripted GUI run in Steps 11-14. Each row there names the observation that makes it fail, and row 5 fails on today's code (Shift+wheel scrolls vertically).

- [ ] **Step 1: Record the starting count, then add the residual field**

Before editing anything, run `ctest --test-dir build -C Release` from the repo root and record the passing count as **N1**. This task adds no unit test, so N1 is the count Steps 8 and 14 expect. `git status --short` must print nothing.

In `src/ui/PdfCanvas.cpp`, in `struct PdfCanvas::Impl`, directly after `int wheel_residual = 0;` and its comment, add:

```cpp
    // The horizontal wheel's own leftover delta (#56), in "rightward" units
    // (see rightward_delta). Separate from wheel_residual so a diagonal
    // touchpad swipe cannot feed horizontal motion into a vertical notch, and
    // cleared only by bitmap_is_stale() -- never by the pending-flip latch,
    // since this axis never turns the page.
    int                           hwheel_residual = 0;
```

- [ ] **Step 2: Declarations**

In `src/ui/PdfCanvas.hpp`, replace:

```cpp
    // Plain (unmodified) mouse-wheel scrolling. Scrolls within the page, and
    // flips to the neighbouring page or spread once the pan is already at the
    // edge the wheel is pushing toward.
    LRESULT on_wheel_scroll(int delta);
```

with:

```cpp
    // Plain (unmodified) mouse-wheel scrolling. Scrolls within the page, and
    // flips to the neighbouring page or spread once the pan is already at the
    // edge the wheel is pushing toward.
    LRESULT on_wheel_scroll(int delta);

    // Horizontal wheel scrolling (#56): WM_MOUSEHWHEEL, or Shift + the plain
    // wheel. Writes pan_x ONLY and never turns the page -- at the edge it
    // clamps.
    LRESULT on_hwheel_scroll(int raw_delta, litepdf::ui::HWheelSource src);

    // True while current_bitmap belongs to another view or page than the one
    // showing. set_view and navigate_to_page keep painting the outgoing bitmap
    // until the incoming render lands, and both wheels drop a notch until
    // then. NOT own_bitmap(): this compares against the CANONICAL left page in
    // spread mode, and it is false when there is no bitmap at all.
    bool bitmap_is_stale() const;
```

- [ ] **Step 3: Move the vertical wheel's identity predicate into `bitmap_is_stale()`, unchanged**

In `src/ui/PdfCanvas.cpp`, in `PdfCanvas::on_wheel_scroll`, replace:

```cpp
    // bitmap_epoch/bitmap_page are what make that detectable; scroll_into_view
    // runs the same test for the same reason (Task 6). Compare against the
    // CANONICAL left, exactly as accept_completion does: bitmap_page is written
    // only by the LEFT slot, so in spread mode it holds the pair's left page.
    // Deriving that here rather than trusting current_page to be left-aligned
    // keeps the guard independent of an invariant maintained four call sites
    // away -- the same reasoning as the flip guard further down.
    const int wheel_total = impl_->view->page_count();
    const int wheel_canon =
        impl_->dual_page
            ? dual_page_compute_left(impl_->view->current_page(), wheel_total)
            : impl_->view->current_page();
    if (impl_->current_bitmap
        && (impl_->bitmap_epoch != impl_->view_epoch
            || impl_->bitmap_page != wheel_canon)) {
        impl_->wheel_residual = 0;
        return 0;
    }
```

with:

```cpp
    // bitmap_epoch/bitmap_page are what make that detectable; scroll_into_view
    // runs the same test for the same reason (Task 6). bitmap_is_stale() holds
    // the predicate, which the horizontal wheel shares (#56).
    if (bitmap_is_stale()) {
        impl_->wheel_residual = 0;
        return 0;
    }
```

Directly **above** `LRESULT PdfCanvas::on_wheel_scroll(int delta) {`, add:

```cpp
bool PdfCanvas::bitmap_is_stale() const {
    if (!impl_ || !impl_->view || !impl_->current_bitmap) return false;
    // Compare against the CANONICAL left, exactly as accept_completion does:
    // bitmap_page is written only by the LEFT slot, so in spread mode it holds
    // the pair's left page. Deriving that here rather than trusting
    // current_page to be left-aligned keeps the guard independent of an
    // invariant maintained four call sites away.
    const int total = impl_->view->page_count();
    const int canon =
        impl_->dual_page
            ? dual_page_compute_left(impl_->view->current_page(), total)
            : impl_->view->current_page();
    return impl_->bitmap_epoch != impl_->view_epoch
        || impl_->bitmap_page  != canon;
}

```

**Equivalence check** (do it, and do not skip it): the old test ran only after `on_wheel_scroll`'s own `if (!impl_ || !impl_->view) return 0;`. So the helper's extra `!impl_ || !impl_->view` returns are unreachable from that call site. The `current_bitmap &&` precondition and both comparisons are the same expressions, and `wheel_residual = 0; return 0;` is kept at the call site.

- [ ] **Step 4: `on_hwheel_scroll`**

In `src/ui/PdfCanvas.cpp`, directly **after** the closing `}` of `PdfCanvas::on_wheel_scroll` (before `float PdfCanvas::page_origin_y`), add:

```cpp
LRESULT PdfCanvas::on_hwheel_scroll(int raw_delta, HWheelSource src) {
    if (!impl_ || !impl_->view) return 0;

    // The bitmap on screen may still be the OUTGOING tab's or page's: a tab
    // switch or a page change whose render has not landed. Its extent is the
    // wrong one to clamp against. After a tab switch a notch here would clamp
    // the pan that set_pan just restored for the incoming tab to the outgoing
    // document's width -- to 0 if that one fit -- and apply_anchor only
    // re-clamps, so the reader's column would be lost. Drop the notch and the
    // residual, as on_wheel_scroll does. The wheel_flip_seq latch is
    // deliberately NOT consulted: it stops a notch from turning the page
    // twice, and this path never turns the page.
    if (bitmap_is_stale()) {
        impl_->hwheel_residual = 0;
        return 0;
    }

    const int notches =
        consume_notches(rightward_delta(raw_delta, src), impl_->hwheel_residual);
    if (notches == 0) return 0;   // a fraction of a notch: keep accumulating

    ContentBox box{};
    if (!content_extent(box)) return 0;   // nothing rendered yet
    const D2D1_SIZE_F vp = impl_->rt->GetSize();

    // Each input reads the setting for the control that moved. A tilt wheel has
    // its own "characters" setting; Shift + the wheel is the vertical wheel, so
    // it honours the lines setting. Both default to 3.
    UINT units = 3;   // the Windows default, and the value if the query fails
    SystemParametersInfoW(src == HWheelSource::Tilt ? SPI_GETWHEELSCROLLCHARS
                                                    : SPI_GETWHEELSCROLLLINES,
                          0, &units, 0);
    const float step = wheel_step_dip(notches, units, vp.width);

    // pan_x ONLY. pan_by would re-clamp pan_y as well, against whatever bitmap
    // and viewport are current -- right after a resize, before the replacement
    // render lands, that moves the reader vertically. The vertical wheel
    // likewise writes pan_y only. A rightward step reveals the content's right
    // side, so pan_x decreases: the direction VK_RIGHT pans.
    impl_->pan_x = clamp_pan(impl_->pan_x - step, box.w, vp.width);
    InvalidateRect(hwnd_, nullptr, FALSE);
    return 0;
}
```

- [ ] **Step 5: Using-declarations**

In `src/ui/PdfCanvas.cpp`'s anonymous namespace near the top, directly after `using litepdf::ui::WheelResult;`, add:

```cpp
using litepdf::ui::HWheelSource;
using litepdf::ui::rightward_delta;
```

- [ ] **Step 6: The message arms**

In `PdfCanvas::handle_message`, replace the whole `case WM_MOUSEWHEEL:` block:

```cpp
        case WM_MOUSEWHEEL: {
            if (!impl_->view) return 0;
            WORD modifiers = GET_KEYSTATE_WPARAM(w);
            if (modifiers & MK_CONTROL) {
```

Keep the `MK_CONTROL` branch exactly as it is. Then replace the arm's last line:

```cpp
            return on_wheel_scroll(GET_WHEEL_DELTA_WPARAM(w));
        }
```

with:

```cpp
            // #56: Shift turns the plain wheel sideways. Tested AFTER Ctrl, so
            // Ctrl+Shift+wheel still zooms.
            if (modifiers & MK_SHIFT) {
                return on_hwheel_scroll(GET_WHEEL_DELTA_WPARAM(w),
                                        HWheelSource::Shift);
            }
            return on_wheel_scroll(GET_WHEEL_DELTA_WPARAM(w));
        }
        case WM_MOUSEHWHEEL:
            // #56: a tilt wheel, or a sideways touchpad swipe. Modifiers are
            // ignored: there is no horizontal zoom.
            on_hwheel_scroll(GET_WHEEL_DELTA_WPARAM(w), HWheelSource::Tilt);
            // TRUE, not the 0 the current reference asks for. Real wheel input
            // is posted, and no message loop reads this value; the only readers
            // are senders -- drivers that EMULATE the message, for which
            // Microsoft's device guidance says TRUE marks it handled and
            // without it "the horizontal scroll action may be repeated".
            // Returned with no document open too, for the same reason.
            return TRUE;
```

- [ ] **Step 7: The page box forwards both wheel messages and returns the canvas's result**

In `src/ui/StatusBar.hpp`, replace:

```cpp
    // WM_MOUSEWHEEL arrived while the box had focus. WM_MOUSEWHEEL goes to the
    // FOCUSED window, so without forwarding, the wheel would be dead whenever
    // the reader had clicked into the page box.
    using OnWheel = std::function<void(WPARAM, LPARAM)>;
```

with:

```cpp
    // A wheel message (WM_MOUSEWHEEL or WM_MOUSEHWHEEL) reached the box. Wheel
    // input can be delivered to the FOCUSED window, so without forwarding, the
    // wheel would be dead whenever the reader had clicked into the page box.
    // The owner returns the canvas's own result, and the box hands that back
    // to the sender -- so a WM_MOUSEHWHEEL keeps the canvas's TRUE (#56).
    using OnWheel = std::function<LRESULT(UINT msg, WPARAM, LPARAM)>;
```

In `src/ui/StatusBar.cpp`, in `status_bar_edit_subclass`, replace:

```cpp
        case WM_MOUSEWHEEL:
            // WM_MOUSEWHEEL is delivered to the FOCUSED window. With the caret
            // in this box the canvas would never see a notch, so hand it over.
            // FindBar's and ResultsPanel's edits do not do this -- there the
            // wheel belongs to their own list.
            if (impl->on_wheel) {
                impl->on_wheel(w, l);
                return 0;
            }
            break;
```

with:

```cpp
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            // Wheel input can be delivered to the FOCUSED window. With the caret
            // in this box the canvas would never see a notch, so hand it over.
            // FindBar's and ResultsPanel's edits do not do this -- there the
            // wheel belongs to their own list. WM_MOUSEHWHEEL too (#56):
            // DefWindowProc would pass it up the PARENT chain, and the canvas is
            // a sibling. Return the canvas's result: a forwarder that answered
            // 0 itself would undo the TRUE the canvas gives emulating drivers.
            if (impl->on_wheel) {
                return impl->on_wheel(msg, w, l);
            }
            break;
```

In `src/ui/MainWindow.cpp`, replace:

```cpp
            // WM_MOUSEWHEEL goes to the FOCUSED window: with the caret in the
            // page box the canvas would never see a notch.
            status_bar_->set_on_wheel([this](WPARAM w, LPARAM l) {
                if (canvas_ && canvas_->hwnd()) {
                    SendMessageW(canvas_->hwnd(), WM_MOUSEWHEEL, w, l);
                }
            });
```

with:

```cpp
            // Wheel input can go to the FOCUSED window: with the caret in the
            // page box the canvas would never see a notch. Forward whichever
            // wheel message arrived and return the canvas's result (#56).
            status_bar_->set_on_wheel([this](UINT wheel_msg, WPARAM w, LPARAM l) -> LRESULT {
                if (canvas_ && canvas_->hwnd()) {
                    return SendMessageW(canvas_->hwnd(), wheel_msg, w, l);
                }
                return 0;
            });
```

(The parameter is `wheel_msg`, not `msg`, because the enclosing `MainWindow::handle_message` already has a `msg` parameter, and shadowing it could raise MSVC C4457 under `/W4`.)

Confirm there is no other caller: `rg -n "set_on_wheel|OnWheel" src` must list only `StatusBar.hpp`, `StatusBar.cpp` and this one call in `MainWindow.cpp`.

- [ ] **Step 8: Build and test**

```bash
cmake --build build --config Release
ctest --test-dir build -C Release
```

Expected: the build is clean with no new warnings, and **N1** tests pass (Step 1's count; this task adds no unit test).

- [ ] **Step 9: Commit the feature**

```bash
git add src/ui/PdfCanvas.hpp src/ui/PdfCanvas.cpp src/ui/StatusBar.hpp src/ui/StatusBar.cpp src/ui/MainWindow.cpp
git commit -m "feat(canvas): scroll sideways with Shift+wheel and a tilt wheel

WM_MOUSEHWHEEL and Shift + WM_MOUSEWHEEL pan pan_x by the wheel's
lines/chars setting and clamp at the edge; they never turn the page and
never write pan_y. Ctrl is tested first, so Ctrl+Shift+wheel still zooms.
The vertical wheel's bitmap-identity test moves unchanged into
bitmap_is_stale(), which the horizontal path shares so a notch during a tab
switch cannot clamp the incoming tab's pan to the outgoing page. The page
box forwards WM_MOUSEHWHEEL too and returns the canvas's result, which is
TRUE for WM_MOUSEHWHEEL for the sake of emulating drivers.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

- [ ] **Step 10: Add the temporary probe (NOT committed)**

These edits exist only for Steps 11-13. They are reverted in Step 14 with `git checkout -- src/ui/PdfCanvas.cpp` (plan-time correction C1).

(a) In `src/ui/PdfCanvas.cpp`, after the last `#include <utility>` line, add:

```cpp
#include <cstdarg>   // #56 PROBE -- temporary
#include <cstdio>    // #56 PROBE -- temporary
#include <cwchar>    // #56 PROBE -- temporary
#include <share.h>   // #56 PROBE -- temporary
```

(b) Directly **above** the line `bool post_render_done_impl(HWND target, UINT msg, litepdf::ui::Slot slot,`, which is inside an anonymous namespace, add:

```cpp
// ---- #56 PROBE -- temporary, never committed ----
void probe_log(const char* fmt, ...) {
    wchar_t path[MAX_PATH + 1];
    const DWORD n = GetTempPathW(MAX_PATH + 1, path);
    if (n == 0 || n > MAX_PATH - 32) return;
    wcscat_s(path, L"litepdf-wheel-probe.log");
    FILE* f = _wfsopen(path, L"a", _SH_DENYNO);
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}
bool probe_slow() {
    wchar_t path[MAX_PATH + 1];
    const DWORD n = GetTempPathW(MAX_PATH + 1, path);
    if (n == 0 || n > MAX_PATH - 32) return false;
    wcscat_s(path, L"litepdf-probe-slow");
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}
// ---- end #56 PROBE ----
```

(c) As the first statement inside `post_render_done_impl`, before `if (!pix) {`, add. This runs on the render worker thread, so the UI thread stays responsive (correction C2):

```cpp
    if (probe_slow()) Sleep(1500);   // #56 PROBE -- temporary
```

(d) In `PdfCanvas::handle_message`, directly before `switch (msg) {`, add:

```cpp
    // ---- #56 PROBE -- temporary, never committed ----
    static bool probe_inside = false;
    if ((msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL) && !probe_inside) {
        const WORD keys  = GET_KEYSTATE_WPARAM(w);
        const int  delta = GET_WHEEL_DELTA_WPARAM(w);
        const bool horizontal =
            msg == WM_MOUSEHWHEEL
            || ((keys & MK_SHIFT) != 0 && (keys & MK_CONTROL) == 0);
        const bool  stale = bitmap_is_stale();
        const int   page0 = impl_->view ? impl_->view->current_page() : -1;
        const float x0 = impl_->pan_x, y0 = impl_->pan_y;
        probe_inside = true;
        const LRESULT r = handle_message(hwnd, msg, w, l);
        probe_inside = false;
        ContentBox box{};
        const bool has_box = content_extent(box);
        const D2D1_SIZE_F vp = impl_->rt ? impl_->rt->GetSize() : D2D1::SizeF(0.0f, 0.0f);
        const char* branch = !horizontal ? "other"
                           : stale ? "stale-drop"
                           : (impl_->pan_x != x0) ? "step" : "no-move";
        probe_log("wheel msg=%s keys=0x%02x delta=%d branch=%s stale=%d page0=%d page1=%d "
                  "epoch=%llu x0=%.2f x1=%.2f y0=%.2f y1=%.2f hres=%d vres=%d "
                  "boxw=%.2f boxh=%.2f vpw=%.2f vph=%.2f result=%lld",
                  msg == WM_MOUSEHWHEEL ? "H" : "V", static_cast<unsigned>(keys), delta,
                  branch, stale ? 1 : 0, page0,
                  impl_->view ? impl_->view->current_page() : -1,
                  static_cast<unsigned long long>(impl_->view_epoch),
                  x0, impl_->pan_x, y0, impl_->pan_y,
                  impl_->hwheel_residual, impl_->wheel_residual,
                  has_box ? box.w : -1.0f, has_box ? box.h : -1.0f,
                  vp.width, vp.height, static_cast<long long>(r));
        return r;
    }
    // ---- end #56 PROBE ----
```

(e) In the `case WM_USER_RENDER_DONE:` / `case WM_USER_RENDER_DONE_RIGHT:` block, directly after the line `auto* meta = reinterpret_cast<RenderMeta*>(l);`, add:

```cpp
            probe_log("render-done slot=%s pix=%d page=%d epoch=%llu view_epoch=%llu",   // #56 PROBE
                      is_right ? "R" : "L", pix ? 1 : 0, meta ? meta->page : -1,
                      meta ? static_cast<unsigned long long>(meta->epoch) : 0ULL,
                      static_cast<unsigned long long>(impl_->view_epoch));
```

Write down the wall-clock time, then build the probe binary. Step 13 checks the exe against that time.

```bash
date
cmake --build build --config Release --target litepdf
```

It must compile. Warnings from the probe lines are acceptable: the project uses `/W4` without `/WX`, and the probe is reverted.

- [ ] **Step 11: Write the driver**

Create the folder first (`build/gui` does not exist yet): `New-Item -ItemType Directory -Force build\gui | Out-Null`. Then save the driver as `build/gui/hwheel-drive.ps1`. It is a scratch file, not committed. It runs in 64-bit Windows PowerShell 5.1 (`C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe`): no `?.`, no `??`, no ternary.

Function names are deliberately long. A short name like `H` or `V` would lose to a built-in alias (`h` is `Get-History`), and an alias always beats a function.

```powershell
# hwheel-drive.ps1 -- scratch driver for the #56 GUI checks. Not committed.
# Dot-source from the repo root in Windows PowerShell 5.1:  . .\build\gui\hwheel-drive.ps1
if (-not ('HwU' -as [type])) {
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class HwU {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll", CharSet=CharSet.Unicode)]
  public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, IntPtr title);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [DllImport("user32.dll")] public static extern bool SystemParametersInfoW(uint action, uint param, out uint value, uint winini);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
}
[void][HwU]::SetProcessDPIAware()

$WM_COMMAND = 0x0111; $WM_KEYDOWN = 0x0100; $WM_CLOSE = 0x0010
$WM_MOUSEWHEEL = 0x020A; $WM_MOUSEHWHEEL = 0x020E
$MK_SHIFT = 0x0004; $MK_CONTROL = 0x0008
$VK_LEFT = 0x25; $VK_UP = 0x26; $VK_RIGHT = 0x27; $VK_DOWN = 0x28
$IDM_ZOOM_IN = 40010; $IDM_TAB_GOTO_1 = 40033; $IDM_TAB_GOTO_2 = 40034
$SPI_GETWHEELSCROLLLINES = 0x0068; $SPI_GETWHEELSCROLLCHARS = 0x006C
$SWP_NOMOVE = 0x0002; $SWP_NOZORDER = 0x0004; $SW_RESTORE = 9
# GetTempPath, the same call the probe makes, so both sides agree on the folder.
$ProbeLog = Join-Path ([System.IO.Path]::GetTempPath()) 'litepdf-wheel-probe.log'
$SlowFlag = Join-Path ([System.IO.Path]::GetTempPath()) 'litepdf-probe-slow'
$script:Fails = 0; $script:Voids = 0; $script:Proc = $null
$BuildExe = (Resolve-Path .\build\Release\litepdf.exe).Path

function Get-Spi([uint32]$action) {
  $v = [uint32]0
  if (-not [HwU]::SystemParametersInfoW($action, 0, [ref]$v, 0)) { throw "SystemParametersInfoW($action) failed" }
  return $v
}

function Start-LitePdf([string]$file) {
  # litepdf is single-instance: with ANY copy already running (the user's
  # installed one included), this launch would forward its file there and exit.
  $running = @(Get-Process litepdf -ErrorAction SilentlyContinue)
  if ($running.Count -gt 0) {
    throw "litepdf is already running (pid $($running[0].Id), $($running[0].Path)). Close it first: Close-AnyLitePdf closes the build copy; ask the user to close any other."
  }
  $env:LITEPDF_NO_RESTORE = '1'
  Remove-Item $ProbeLog -ErrorAction SilentlyContinue
  Remove-Item $SlowFlag -ErrorAction SilentlyContinue
  $p = Start-Process -PassThru $BuildExe -ArgumentList ('"' + (Resolve-Path $file).Path + '"')
  for ($i = 0; $i -lt 50 -and [int64]$p.MainWindowHandle -eq 0; $i++) { Start-Sleep -Milliseconds 100; $p.Refresh() }
  $script:Proc   = $p
  $script:Main   = [IntPtr]$p.MainWindowHandle
  $script:Canvas = [HwU]::FindWindowExW($script:Main, [IntPtr]::Zero, 'LitePDFPdfCanvas', [IntPtr]::Zero)
  if ([int64]$script:Canvas -eq 0) { throw 'canvas HWND not found' }
  $sb = [HwU]::FindWindowExW($script:Main, [IntPtr]::Zero, 'msctls_statusbar32', [IntPtr]::Zero)
  $script:PageBox = [HwU]::FindWindowExW($sb, [IntPtr]::Zero, 'Edit', [IntPtr]::Zero)
  if ([int64]$script:PageBox -eq 0) { throw 'page-box EDIT not found' }
  [void][HwU]::ShowWindow($script:Main, $SW_RESTORE)
  Start-Sleep -Milliseconds 1500
}

# A second file goes to the running instance (single-instance forwarding) and
# opens as a new, active tab.
function Open-SecondTab([string]$file) {
  Start-Process $BuildExe -ArgumentList ('"' + (Resolve-Path $file).Path + '"')
  Start-Sleep -Milliseconds 2500
}

function Send-Command([int]$id) {
  [void][HwU]::SendMessageW($script:Main, $WM_COMMAND, [IntPtr]$id, [IntPtr]::Zero)
  Start-Sleep -Milliseconds 1200   # let the render land
}

function Window-Height { $r = New-Object HwU+RECT; [void][HwU]::GetWindowRect($script:Main, [ref]$r); return ($r.Bottom - $r.Top) }
function Window-Width  { $r = New-Object HwU+RECT; [void][HwU]::GetWindowRect($script:Main, [ref]$r); return ($r.Right - $r.Left) }
function Set-WindowHeight([int]$h) {
  [void][HwU]::SetWindowPos($script:Main, [IntPtr]::Zero, 0, 0, (Window-Width), $h, ($SWP_NOMOVE -bor $SWP_NOZORDER))
}

# Send one wheel message; returns the LRESULT. wParam = delta in the HIWORD,
# key state in the LOWORD. Built in int64 so a negative delta cannot overflow.
function Send-Wheel([IntPtr]$hwnd, [int]$msg, [int]$delta, [int]$keys = 0) {
  $wp = ((([int64]$delta) -band 0xFFFF) -shl 16) -bor $keys
  return [int64][HwU]::SendMessageW($hwnd, $msg, [IntPtr]$wp, [IntPtr]::Zero)
}

function Parse-Probe([string]$line) {
  $o = @{ raw = $line }
  foreach ($m in [regex]::Matches($line, '(\w+)=(\S+)')) { $o[$m.Groups[1].Value] = $m.Groups[2].Value }
  return [pscustomobject]$o
}
function Last-Wheel {
  $line = @(Get-Content $ProbeLog | Where-Object { $_ -like 'wheel *' }) | Select-Object -Last 1
  if (-not $line) { throw 'no wheel line in the probe log -- is the PROBE build running?' }
  return (Parse-Probe $line)
}
function Send-TiltWheel([int]$delta)  { [void](Send-Wheel $script:Canvas $WM_MOUSEHWHEEL $delta); return Last-Wheel }
function Send-ShiftWheel([int]$delta) { [void](Send-Wheel $script:Canvas $WM_MOUSEWHEEL $delta $MK_SHIFT); return Last-Wheel }
function Send-PlainWheel([int]$delta) { [void](Send-Wheel $script:Canvas $WM_MOUSEWHEEL $delta); return Last-Wheel }
# A zero-delta WM_MOUSEHWHEEL consumes no notch; its probe line is a state read.
function Read-Pan { return (Send-TiltWheel 0) }

function Mark-Log([string]$name) {
  for ($i = 0; $i -lt 5; $i++) {
    try { Add-Content -Path $ProbeLog -Value "mark $name" -ErrorAction Stop; return } catch { Start-Sleep -Milliseconds 50 }
  }
  throw "could not append mark $name"
}
function Lines-After([string]$name) {
  $all = @(Get-Content $ProbeLog)
  $i = [array]::LastIndexOf($all, "mark $name")
  if ($i -lt 0) { throw "mark $name not found" }
  return @($all | Select-Object -Skip ($i + 1))
}
# True when a render that could have LANDED -- a pixmap (pix=1) for the view
# that was current when it arrived (epoch == view_epoch) -- is logged after the
# mark and before the first wheel line: the race window the row needs was never
# open. Cancellations (pix=0) and stale completions for another view land
# nothing, so they do not void the row.
function Render-Landed-First([string]$name) {
  foreach ($l in (Lines-After $name)) {
    if ($l -like 'render-done *') {
      $d = Parse-Probe $l
      if ($d.pix -eq '1' -and $d.epoch -eq $d.view_epoch) { return $true }
    }
    if ($l -like 'wheel *') { return $false }
  }
  return $false
}

function Near([double]$a, [double]$b) { return [math]::Abs($a - $b) -le 0.01 }
function Check([string]$name, [bool]$ok, [string]$detail) {
  if ($ok) { Write-Host "PASS $name  $detail" } else { $script:Fails++; Write-Host "FAIL $name  $detail" }
}
function Void-Row([string]$name, [string]$why) { $script:Voids++; Write-Host "VOID $name  $why" }

function Send-Key([int]$vk, [int]$times) {
  for ($i = 0; $i -lt $times; $i++) { [void][HwU]::SendMessageW($script:Canvas, $WM_KEYDOWN, [IntPtr]$vk, [IntPtr]::Zero) }
}
# Pin the pan at (0, 0) with the arrow keys (correction C6).
function Reset-Pan {
  Send-Key $VK_LEFT 60; Send-Key $VK_UP 60
  $p = Read-Pan
  if (-not ((Near ([double]$p.x1) 0) -and (Near ([double]$p.y1) 0))) { throw "Reset-Pan left the pan at ($($p.x1), $($p.y1))" }
}
function Pan-To-BottomRight {
  Send-Key $VK_RIGHT 60; Send-Key $VK_DOWN 60
  $p = Read-Pan
  $ex = [double]$p.vpw - [double]$p.boxw; $ey = [double]$p.vph - [double]$p.boxh
  if (-not ((Near ([double]$p.x1) $ex) -and (Near ([double]$p.y1) $ey))) { throw "Pan-To-BottomRight: ($($p.x1), $($p.y1)), expected ($ex, $ey)" }
  return $p
}
# Zoom in until the page overflows the viewport by at least $minX / $minY DIPs,
# so every row has room for the steps it takes. A zoom re-renders the same page
# under the same epoch, so the bitmap is never "stale" meanwhile: wait for the
# content box itself to grow.
function Zoom-Until-Overflow([double]$minX, [double]$minY) {
  $prev = [double](Read-Pan).boxw
  for ($i = 1; $i -le 6; $i++) {
    Send-Command $IDM_ZOOM_IN
    $p = Read-Pan
    for ($k = 0; $k -lt 30 -and [double]$p.boxw -le $prev + 1; $k++) { Start-Sleep -Milliseconds 100; $p = Read-Pan }
    $prev = [double]$p.boxw
    if (([double]$p.boxw - [double]$p.vpw -ge $minX) -and ([double]$p.boxh - [double]$p.vph -ge $minY)) {
      Write-Host "zoomed $i rung(s): box $($p.boxw) x $($p.boxh), viewport $($p.vpw) x $($p.vph)"
      return
    }
  }
  throw "the page does not overflow by $minX x $minY DIP after 6 zoom-ins"
}

# Poll until the bitmap on screen belongs to the current view and page, i.e. the
# pending render has landed. Returns that state line.
function Wait-Settled {
  for ($i = 0; $i -lt 60; $i++) {
    $p = Read-Pan
    if ($p.stale -eq '0' -and [double]$p.boxw -ge 0) { return $p }
    Start-Sleep -Milliseconds 100
  }
  throw 'the canvas never settled: the bitmap stayed stale for 6 s'
}

# WM_CLOSE, then WAIT: the app saves session.json on its way out.
function Close-LitePdf {
  [void][HwU]::PostMessageW($script:Main, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
  if (-not $script:Proc.WaitForExit(10000)) { throw 'litepdf did not exit within 10 s' }
}

# Recovery for a run that died with the BUILD copy still open. Closes only
# processes started from build\Release -- never the user's installed copy.
function Close-AnyLitePdf {
  foreach ($p in @(Get-Process litepdf -ErrorAction SilentlyContinue)) {
    if ($p.Path -ne $BuildExe) { continue }
    if ([int64]$p.MainWindowHandle -ne 0) {
      [void][HwU]::PostMessageW([IntPtr]$p.MainWindowHandle, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
    }
    if (-not $p.WaitForExit(10000)) { throw "litepdf pid $($p.Id) did not exit within 10 s" }
  }
}
```

- [ ] **Step 12: Write the check script**

Save as `build/gui/hwheel-checks.ps1` (scratch). Run the whole sequence as **one** script: a PowerShell tool call does not keep variables into the next one. `Set-StrictMode` right after the dot-source makes a misspelt constant throw, instead of silently sending message 0.

Numbers come from the machine's settings (spec §4.2). The tilt step is `SPI_GETWHEELSCROLLCHARS × 16` DIP and the Shift step is `SPI_GETWHEELSCROLLLINES × 16` DIP. Both are 48 at plan time.

```powershell
. .\build\gui\hwheel-drive.ps1
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$C = Get-Spi $SPI_GETWHEELSCROLLCHARS; $L = Get-Spi $SPI_GETWHEELSCROLLLINES
if ($C -lt 1 -or $C -gt 100 -or $L -lt 1 -or $L -gt 100) { throw "wheel settings chars=${C} lines=${L} -- set both to 1-100 for this run" }
$HStep = [double]($C * 16); $SStep = [double]($L * 16)
$Room  = 3 * [math]::Max($HStep, $SStep)   # every row takes at most 2 steps
Write-Host "settings: chars=${C} lines=${L} -> tilt step $HStep DIP, Shift step $SStep DIP"

try {
  Start-LitePdf 'tests\fixtures\simple.pdf'
  $H0 = Window-Height

  # Row 5 -- FitWidth, the negative control. Nothing may move. FAILS on today's
  # code: Shift+wheel scrolls vertically there.
  $p = Read-Pan
  if ([double]$p.boxw -lt 0) { throw 'no content extent -- the canvas has not painted; is the window visible?' }
  if ([double]$p.boxw -gt [double]$p.vpw + 0.5) { throw "row 5 precondition: the FitWidth page overflows ($($p.boxw) > $($p.vpw))" }
  $a = Send-TiltWheel 120; $b = Send-ShiftWheel -120
  Check 'row 5' ((Near ([double]$a.x1) 0) -and (Near ([double]$a.y1) 0) -and (Near ([double]$b.x1) 0) -and (Near ([double]$b.y1) 0)) "tilt x1=$($a.x1) y1=$($a.y1); shift x1=$($b.x1) y1=$($b.y1)"

  Zoom-Until-Overflow $Room (2 * $SStep)

  # Row 1 -- a tilt notch right.
  Reset-Pan
  $r = Send-TiltWheel 120
  Check 'row 1' (($r.branch -eq 'step') -and (Near ([double]$r.x1) (-$HStep)) -and (Near ([double]$r.y1) 0)) $r.raw

  # Row 2 -- Shift + the wheel toward the user.
  Reset-Pan
  $r = Send-ShiftWheel -120
  Check 'row 2' (($r.branch -eq 'step') -and (Near ([double]$r.x1) (-$SStep)) -and (Near ([double]$r.y1) 0)) $r.raw

  # Row 4 -- fractions accumulate into one notch.
  Reset-Pan
  if ((Read-Pan).hres -ne '0') { throw 'row 4 precondition: the horizontal residual is not 0' }
  $r1 = Send-TiltWheel 40; $r2 = Send-TiltWheel 40; $r3 = Send-TiltWheel 40
  Check 'row 4' (($r1.branch -eq 'no-move') -and ($r1.hres -eq '40') -and ($r2.branch -eq 'no-move') -and ($r2.hres -eq '80') -and ($r3.branch -eq 'step') -and (Near ([double]$r3.x1) (-$HStep))) "$($r1.branch)/$($r1.hres) $($r2.branch)/$($r2.hres) $($r3.branch) x1=$($r3.x1)"

  # Row 6 -- positive control: the plain wheel still scrolls vertically only.
  Reset-Pan
  $r = Send-PlainWheel -120
  Check 'row 6' (($r.branch -eq 'other') -and (Near ([double]$r.y1) (-$SStep)) -and (Near ([double]$r.x1) 0)) $r.raw

  # Rows 8 and 9b -- WM_MOUSEHWHEEL sent to the page box reaches the canvas, and
  # the box returns the canvas's TRUE.
  Reset-Pan
  Mark-Log 'row8'
  $res = Send-Wheel $script:PageBox $WM_MOUSEHWHEEL 120
  $w8 = @(Lines-After 'row8' | Where-Object { $_ -like 'wheel *' })
  if ($w8.Count -ne 1) {
    Check 'row 8' $false "expected exactly 1 forwarded wheel line, got $($w8.Count)"
  } else {
    $r = Parse-Probe $w8[0]
    Check 'row 8' (($r.branch -eq 'step') -and (Near ([double]$r.x1) (-$HStep))) $r.raw
  }
  Check 'row 9b' ($res -eq 1) "page box returned $res"

  # Row 9 -- the canvas itself returns TRUE.
  $res = Send-Wheel $script:Canvas $WM_MOUSEHWHEEL 120
  Check 'row 9' ($res -eq 1) "canvas returned $res"

  # Row 10 -- a notch during a tab switch must not clamp the incoming tab's pan
  # to the outgoing page. Tab A = simple.pdf (zoomed), tab B = search.pdf (FitWidth).
  Reset-Pan
  [void](Send-TiltWheel 120); $a = Send-TiltWheel 120
  $saved = [double]$a.x1
  if (-not ($saved -lt -1)) { throw "row 10 precondition: tab A pan_x is $saved, not < 0" }
  Open-SecondTab 'tests\fixtures\search.pdf'
  $b = Wait-Settled
  if ([double]$b.boxw -gt [double]$b.vpw + 0.5) { throw 'row 10 precondition: tab B overflows horizontally, so a clamp against it would not reach 0' }
  # Leave a 40 residual. Only the stale guard zeroes it. An implementation with
  # no guard would fold 40 + 120 into one notch and keep 40, even if it then
  # returned early because the outgoing page fits -- a path on which pan_x
  # alone would not change.
  $pre = Send-TiltWheel 40
  if ($pre.hres -ne '40') { throw "row 10 precondition: residual $($pre.hres), expected 40" }
  Set-Content -Path $SlowFlag -Value ''
  Mark-Log 'row10'
  [void][HwU]::SendMessageW($script:Main, $WM_COMMAND, [IntPtr]$IDM_TAB_GOTO_1, [IntPtr]::Zero)
  $r = Send-TiltWheel 120
  $landedFirst = Render-Landed-First 'row10'
  Remove-Item $SlowFlag
  $after = Wait-Settled
  if ($landedFirst) {
    Void-Row 'row 10' 'a render landed between the tab switch and the notch'
  } else {
    Check 'row 10' (($r.branch -eq 'stale-drop') -and ($r.hres -eq '0') -and (Near ([double]$r.x1) $saved) -and (Near ([double]$after.x1) $saved)) "notch=$($r.branch) hres=$($r.hres) x0=$($r.x0) x1=$($r.x1) after=$($after.x1) saved=$saved"
  }

  # Row 11 -- a notch right after a resize, before the new render, writes pan_x
  # only. Tab A is active and zoomed.
  Set-WindowHeight ($H0 - 300); Start-Sleep -Milliseconds 1500
  $before = Pan-To-BottomRight
  Set-Content -Path $SlowFlag -Value ''
  Mark-Log 'row11'
  Set-WindowHeight $H0
  $r = Send-TiltWheel -120
  $landedFirst = Render-Landed-First 'row11'
  Remove-Item $SlowFlag
  Start-Sleep -Milliseconds 2000
  if ($landedFirst) {
    Void-Row 'row 11' 'a render landed between the resize and the notch'
  } elseif (-not ([double]$r.vph -gt [double]$before.vph + 1)) {
    Void-Row 'row 11' "the canvas did not grow ($($before.vph) -> $($r.vph))"
  } else {
    Check 'row 11' (($r.branch -eq 'step') -and (Near ([double]$r.y1) ([double]$r.y0)) -and (Near ([double]$r.x1) ([double]$r.x0 + $HStep))) $r.raw
  }

  # Tab B (search.pdf, 7 same-size pages) for every row that needs a page to
  # turn -- or needs to prove one did NOT turn.
  Send-Command $IDM_TAB_GOTO_2
  [void](Wait-Settled)
  Zoom-Until-Overflow $Room (2 * $SStep)

  # Row 7 -- a vertical page turn keeps pan_x (single-page, same-size pages).
  Reset-Pan
  $s = Send-TiltWheel 120
  $page = [int]$s.page1
  for ($i = 0; $i -lt 300; $i++) { $r = Send-PlainWheel -120; if ([int]$r.page1 -ne $page) { break } }
  if ([int]$r.page1 -ne $page + 1) { throw "row 7: the page did not turn forward ($page -> $($r.page1))" }
  $after = Wait-Settled   # the page-turn render has LANDED: apply_anchor has run
  Check 'row 7' (([int]$after.page1 -eq $page + 1) -and (Near ([double]$after.x1) (-$HStep)) -and (Near ([double]$after.boxw) ([double]$s.boxw))) "page $page -> $($after.page1), x1=$($after.x1), boxw $($s.boxw) -> $($after.boxw)"

  # Row 3 -- repeated notches stop at the RIGHT edge and never turn the page.
  # A multi-page document, not on its last page, so a page turn is possible.
  Reset-Pan
  $page = [int](Read-Pan).page1
  $n = 0; $pageMoved = $false
  do {
    $r = Send-TiltWheel 120; $n++
    if ([int]$r.page0 -ne $page -or [int]$r.page1 -ne $page) { $pageMoved = $true }
  } while ($r.branch -eq 'step' -and $n -lt 200)
  $edge = [double]$r.vpw - [double]$r.boxw
  Check 'row 3' (($r.branch -eq 'no-move') -and ($n -ge 2) -and (Near ([double]$r.x1) $edge) -and (-not $pageMoved)) "page $page, notches=$n x1=$($r.x1) edge=$edge pageMoved=$pageMoved"

  # Row 3b -- at the LEFT edge, on a page after the first, a leftward notch
  # neither moves nor turns the page back.
  Reset-Pan
  $page = [int](Read-Pan).page1
  if ($page -lt 1) { throw "row 3b precondition: on page $page, so a backward page turn is impossible" }
  $r1 = Send-TiltWheel -120; $r2 = Send-TiltWheel -120
  Check 'row 3b' (($r1.branch -eq 'no-move') -and ($r2.branch -eq 'no-move') -and ([int]$r2.page1 -eq $page) -and (Near ([double]$r2.x1) 0)) "page $page -> $($r2.page1), branches $($r1.branch)/$($r2.branch)"

  # Row 12 -- Ctrl+Shift+wheel still zooms (H2; correction C8). Same page, same
  # epoch, so wait for the content box itself to grow.
  # The message itself must not pan: Shift must not ALSO take the horizontal path.
  $z0 = Read-Pan
  [void](Send-Wheel $script:Canvas $WM_MOUSEWHEEL 120 ($MK_CONTROL -bor $MK_SHIFT))
  $zm = Last-Wheel
  $z1 = Read-Pan
  for ($k = 0; $k -lt 30 -and [double]$z1.boxw -le [double]$z0.boxw + 1; $k++) { Start-Sleep -Milliseconds 100; $z1 = Read-Pan }
  Check 'row 12' (([double]$z1.boxw -gt [double]$z0.boxw + 1) -and (Near ([double]$zm.x1) ([double]$zm.x0)) -and (Near ([double]$zm.y1) ([double]$zm.y0))) "boxw $($z0.boxw) -> $($z1.boxw); the zoom message moved x $($zm.x0) -> $($zm.x1), y $($zm.y0) -> $($zm.y1)"
}
finally {
  Remove-Item $SlowFlag -ErrorAction SilentlyContinue
  if ($script:Proc -ne $null -and -not $script:Proc.HasExited) { Close-LitePdf }
}
Write-Host "DONE: $script:Fails FAIL, $script:Voids VOID"
# Exit code: 0 = all rows passed, 1 = a FAIL, 2 = no FAIL but a VOID. An
# uncaught throw also exits 1.
if ($script:Fails -gt 0) { exit 1 }
if ($script:Voids -gt 0) { exit 2 }
exit 0
```

- [ ] **Step 13: Run the checks**

**Hygiene, before the run:**
- Back up the live session: `Copy-Item "$env:LOCALAPPDATA\LitePDF\session.json" "$env:TEMP\litepdf-session-56.bak"`. The app auto-saves over it within ~1.5 s of launch.
- Confirm the binary under test is the probe build: `(Get-Item .\build\Release\litepdf.exe).LastWriteTime` must be later than the time Step 10 wrote down.
- **No litepdf may be running, including the user's installed copy.** litepdf is single-instance, so the run's files would be forwarded to that copy. `Start-LitePdf` refuses in that case. If the refusal names a path outside `build\Release`, stop and report it; the controller asks the user to close that copy.
- Do not touch the mouse or keyboard during the run.

Run from the repo root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build\gui\hwheel-checks.ps1
```

Expected: `DONE: 0 FAIL, 0 VOID`, with 14 PASS lines (rows 1-9, 3b, 9b, 10-12), and exit code 0 (`$LASTEXITCODE`). Exit code 1 means a FAIL or an uncaught throw; 2 means a VOID.
- **A VOID is not a pass.** Re-run once. If it repeats, report it to the controller with the log excerpt after the row's `mark` line.
- **A FAIL** goes to `superpowers:systematic-debugging`, never a guessed fix.

Put every PASS/FAIL/VOID line, the settings line and the `zoomed` line in the task report.

After the run:
- The check script closes litepdf in its `finally`, on every exit path. If a build copy is still running anyway, close it with `powershell -NoProfile -Command ". .\build\gui\hwheel-drive.ps1; Close-AnyLitePdf"`. Never use `Stop-Process`: a force-kill leaves `running.lock`, and the next launch offers a restore. `Close-AnyLitePdf` touches only processes started from `build\Release`.
- Restore the session: `Copy-Item "$env:TEMP\litepdf-session-56.bak" "$env:LOCALAPPDATA\LitePDF\session.json" -Force`.

- [ ] **Step 14: Revert the probe and rebuild clean**

```bash
git checkout -- src/ui/PdfCanvas.cpp
git status --short
cmake --build build --config Release
ctest --test-dir build -C Release
```

`git status --short` must print nothing, and `git diff HEAD --stat` must be empty. The rebuilt `litepdf.exe` is now the shipping binary. Expected: **N1** passing (Step 1's count). Delete `%TEMP%\litepdf-wheel-probe.log` and `%TEMP%\litepdf-probe-slow` if either still exists.

If Step 13 found a defect: fix it in the source (probe reverted), commit the fix separately with its own message, then repeat Steps 10-14.

---

## Task 3: Docs, final verification, merge gate

**Files:**
- Modify: `CHANGELOG.md`
- Modify: `README.md`

**Interfaces:**
- Consumes: the behaviour shipped by Task 2.
- Produces: the PR.

- [ ] **Step 1: Record the starting count, then the CHANGELOG**

Before editing, run `ctest --test-dir build -C Release` from the repo root and record the passing count as **N2**. This task changes only docs, so Step 4 expects N2. `git status --short` must print nothing.

Under `## [Unreleased]` → `### Added`, after the "Hand-tool panning." bullet, add:

```markdown
- Sideways scrolling with the mouse. On a page zoomed wider than the window, a
  tilt wheel or a sideways touchpad swipe scrolls left and right, and so does
  the ordinary wheel with Shift held (toward you scrolls right). It stops at
  the page's edge instead of turning the page. Shift+wheel used to scroll up
  and down as if Shift were not held; it now scrolls sideways, and does
  nothing when the page already fits the window's width (#56).
```

- [ ] **Step 2: README**

In the features list, replace:

```markdown
- **Mouse-wheel scrolling** — scrolls within a page and turns the page at the edge, honouring your system's lines-per-notch setting; Ctrl+wheel zooms (v1.3.0)
```

with:

```markdown
- **Mouse-wheel scrolling** — scrolls within a page and turns the page at the edge, honouring your system's lines-per-notch setting; Ctrl+wheel zooms (v1.3.0). Shift+wheel, a tilt wheel or a sideways touchpad swipe scrolls a zoomed-in page left and right (unreleased)
```

In the `## Keyboard shortcuts` table, after the `| Space + drag       | … |` row, add:

```markdown
| Shift + wheel      | Scroll a zoomed-in page sideways (a tilt wheel or touchpad also works) |
```

- [ ] **Step 3: The user's real-hardware check (spec §4.3)**

The controller asks the user to close any running litepdf, run `build\Release\litepdf.exe tests\fixtures\search.pdf` (7 pages, so a wrong page turn would show), zoom in twice (Ctrl+=), and check on their real mouse:
- Shift + wheel toward you scrolls right, and away scrolls left;
- held at either edge, it stops there without turning the page;
- the plain wheel still scrolls up and down;
- Ctrl+Shift+wheel still zooms;
- at FitWidth (Ctrl+0), Shift+wheel does nothing.

Record the user's answer in the PR description. This covers the Shift branch only. The `WM_MOUSEHWHEEL` branch has no real-device check (R1).

Also read each new CHANGELOG and README sentence next to the running window. A CHANGELOG bullet once shipped false (PR #43); this step is why.

- [ ] **Step 4: Full verification**

```bash
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Expected: **N2** passing (Step 1's count). Record the count and `(Get-Item build\Release\litepdf.exe).Length`; the size must be under 19,000,000 bytes. `git diff main -- VERSION` must be empty.

- [ ] **Step 5: Commit**

```bash
git add CHANGELOG.md README.md
git commit -m "docs: record sideways wheel scrolling

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

- [ ] **Step 6: Merge gate, then PR (controller)**

The controller runs this step; it needs the whole plan, not this brief. Invoke the `risk-tiered-review` skill: Full tier, and never drop the Codex lens. Name these least-certain claims for the adversarial lens:

1. **`bitmap_is_stale()` is a pure extraction.** Is the vertical wheel's behaviour byte-for-byte what it was? Check the same predicate, the same `current_bitmap &&` precondition, the canonical-left comparison, and `wheel_residual = 0` kept at the call site. Also check that `wheel_flip_seq` is untouched (decision #4).
2. **`pan_x`-only writes.** `on_hwheel_scroll` bypasses `pan_by`. Is there a path where `pan_x` is left outside its clamp range for a paint? For example, `content_extent` succeeding against a bitmap that `bitmap_is_stale()` did not flag, or spread mode with only the left slot.
3. **The `WM_MOUSEHWHEEL` TRUE return (H8)**, including through the page box. Both Codex lenses objected at the spec gate, and the objection was recorded and rejected. Re-check for a concrete failure TRUE causes; a norm citation alone is not one.
4. **The `OnWheel` signature change.** Is there any caller or copy of the old `void(WPARAM, LPARAM)` shape left, and does the EDIT arm still return 0 for `WM_MOUSEWHEEL`?
5. **H5 by reading, because no GUI row can check it (R6).** In `on_hwheel_scroll`, does `HWheelSource::Tilt` read `SPI_GETWHEELSCROLLCHARS` and `HWheelSource::Shift` read `SPI_GETWHEELSCROLLLINES`, and not the other way round?

After the gate, and with the user's go-ahead, push and open the PR titled `feat: sideways wheel scrolling`. The body carries:
- the test count and the exe size;
- every GUI check line from Task 2 Step 13;
- the user's hardware answer;
- plan-time corrections C1-C10, one line each (this plan's "Plan-time corrections" section, above Task 1);
- limitations R1-R6 (this plan's "Known limitations" table; R1-R5 are also in spec §5);
- `Closes #56`.
