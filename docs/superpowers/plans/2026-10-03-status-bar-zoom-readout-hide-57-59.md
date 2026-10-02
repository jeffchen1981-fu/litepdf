# Status Bar Zoom Readout and Hide Toggle (#57, #59) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Show the current zoom percentage in the status bar, and let the reader hide the status bar from the View menu.

**Architecture:** A third child (`STATIC`) in `ui::StatusBar` shows text from a pure formatter in `StatusBarMath.hpp`. `MainWindow::refresh_zoom_readout()` pushes the active view's live percentage and is called from two places: each branch of `kick_render`, and a new `PdfCanvas` callback fired at the entry of the render-completion message arm. Hiding is `ShowWindow(SW_HIDE)` plus a flag that makes `StatusBar::height_px()` return 0, so the three places that read the status height give the strip back unchanged; `on_layout` only learns to skip positioning a hidden bar.

**Tech Stack:** C++20, Win32 (`msctls_statusbar32`, `STATIC`, `EDIT`), Direct2D canvas, Catch2 unit tests, PowerShell 5.1 GUI driver.

**Spec:** `docs/superpowers/specs/2026-10-02-status-bar-zoom-readout-hide-design.md`

## Global Constraints

- Branch: `feat/57-59-status-bar-zoom-readout-hide`. Do not commit to `main`.
- Build **Release**, never Debug (Debug fails with `LNK2038`).
- `cmake` / `ctest` on PATH are the wrong ones. Always use
  `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe` and `ctest.exe` in the same directory.
- Run `ctest` and the unit-test exe from the repo root `C:\Users\User\projects\litepdf` (fixtures resolve from there).
- **Line numbers in this plan are as of commit `8cf6cc2`**, before any task ran. Earlier tasks insert lines, so by the time you reach a file the numbers may be a few lines off. Always locate an edit by the quoted text or the named function; treat the number as a hint.
- Test count: `ctest --test-dir build -C Release` was 398/398 at `d5cba03`. Task 1 adds six test cases and no other task adds any, so every task after Task 1 expects 404/404. If the number you see differs, stop and find out why before going on.
- Catch2 `TEST_CASE` names are ASCII and start with the subsystem: `StatusBarMath ...`. Tag `[statusbar]`.
- All code, comments and commit messages in English. Commit messages end with
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Do not bump `VERSION`.
- Do not persist the status bar's visibility anywhere (no registry, no `session.json`).
- `*.ps1` must run on Windows PowerShell 5.1: no `?.`, `??`, ternary, `&&`, `||`.
- Do not run `ctest` and a GUI driver at the same time (process-name matching collides).
- Never change an OS setting (theme, display scale, High Contrast). Those steps belong to the user.

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `src/ui/detail/StatusBarMath.hpp` | modify | `format_zoom_pct`; third child rectangle |
| `tests/unit/test_status_bar_math.cpp` | modify | tests for both |
| `src/ui/StatusBar.hpp` / `.cpp` | modify | zoom label child, `set_zoom`, `set_visible`, `visible` |
| `src/ui/PdfCanvas.hpp` / `.cpp` | modify | `set_on_completion_arrived` |
| `src/ui/MainWindow.hpp` / `.cpp` | modify | `refresh_zoom_readout`, `on_toggle_status_bar`, menu arm, layout skip |
| `src/core/DocumentView.hpp` | modify | one stale comment |
| `resources/MainMenu.rc.h` | modify | `IDM_VIEW_STATUS_BAR 40064` |
| `resources/litepdf.rc.in` | modify | View menu item |
| `CHANGELOG.md`, `README.md` | modify | user-facing notes |
| `build/gui-check/status-bar.ps1` | create, untracked | GUI driver (`build/` is git-ignored) |

---

## Task 1: `format_zoom_pct`

**Files:**
- Modify: `src/ui/detail/StatusBarMath.hpp`
- Test: `tests/unit/test_status_bar_math.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `std::wstring litepdf::ui::detail::format_zoom_pct(float pct)`.

- [ ] **Step 1: Measure the baseline**

```powershell
$ctest = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
Push-Location C:\Users\User\projects\litepdf
& $ctest --test-dir build -C Release
Pop-Location
```

Expected: `100% tests passed, 0 tests failed out of 398`. If the total is not 398, stop: the Global Constraints' 404 for later tasks assumes it.

- [ ] **Step 2: Write the failing tests**

In `tests/unit/test_status_bar_math.cpp`, add to the includes at the top:

```cpp
#include <cmath>
#include <limits>
```

add to the `using` list:

```cpp
using litepdf::ui::detail::format_zoom_pct;
```

and append at the end of the file:

```cpp
TEST_CASE("StatusBarMath format_zoom_pct formats ladder values", "[statusbar]") {
    REQUIRE(format_zoom_pct(1.0f)  == L"100%");
    REQUIRE(format_zoom_pct(0.25f) == L"25%");
    REQUIRE(format_zoom_pct(8.0f)  == L"800%");
}

TEST_CASE("StatusBarMath format_zoom_pct rounds to nearest, halves away from zero",
          "[statusbar]") {
    REQUIRE(format_zoom_pct(1.374f) == L"137%");
    REQUIRE(format_zoom_pct(1.375f) == L"138%");
    // The tie rule: 12.5 is exact in float, so this separates round-half-away
    // (13) from round-half-to-even (12).
    REQUIRE(format_zoom_pct(0.125f) == L"13%");
}

TEST_CASE("StatusBarMath format_zoom_pct shows fit values above the ladder",
          "[statusbar]") {
    // fit_percentage is unbounded above; only set_zoom_pct clamps.
    REQUIRE(format_zoom_pct(38.4f) == L"3840%");
    REQUIRE(format_zoom_pct(99.0f) == L"9900%");
}

TEST_CASE("StatusBarMath format_zoom_pct is empty above four digits", "[statusbar]") {
    // The label is sized for "9999%". A value that cannot be shown whole is
    // not shown.
    REQUIRE(format_zoom_pct(100.0f).empty());
    REQUIRE(format_zoom_pct(1e30f).empty());
    REQUIRE(format_zoom_pct(std::numeric_limits<float>::max()).empty());
}

TEST_CASE("StatusBarMath format_zoom_pct is empty for zero and negatives",
          "[statusbar]") {
    // A minimized window drives the fit percentage to exactly 0.
    REQUIRE(format_zoom_pct(0.0f).empty());
    REQUIRE(format_zoom_pct(0.004f).empty());   // would round to 0
    REQUIRE(format_zoom_pct(-1.0f).empty());
    // Finite input, but pct * 100 overflows to -infinity.
    REQUIRE(format_zoom_pct(-std::numeric_limits<float>::max()).empty());
}

TEST_CASE("StatusBarMath format_zoom_pct is empty for non-finite input",
          "[statusbar]") {
    REQUIRE(format_zoom_pct(std::numeric_limits<float>::quiet_NaN()).empty());
    REQUIRE(format_zoom_pct(std::numeric_limits<float>::infinity()).empty());
    REQUIRE(format_zoom_pct(-std::numeric_limits<float>::infinity()).empty());
}
```

- [ ] **Step 3: Build to verify it fails**

```powershell
$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake --build C:\Users\User\projects\litepdf\build --target litepdf_unit_tests --config Release
```

Expected: compile error, `'format_zoom_pct': is not a member of 'litepdf::ui::detail'`.

- [ ] **Step 4: Implement**

In `src/ui/detail/StatusBarMath.hpp`, change the includes to:

```cpp
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
```

and add, just before the closing `}  // namespace litepdf::ui::detail`:

```cpp
// Text for the zoom readout: "137%" for 1.37f, empty when there is nothing
// displayable.
//
// `pct` is DocumentView::zoom_pct(), whose domain is wider than the preset
// ladder: a fit mode derives it from the viewport with no upper bound, and a
// minimized window drives it to exactly 0. The three guards run BEFORE the
// conversion, in this order, so std::lround only ever sees [0.5, 9999.5):
//   1. non-finite product  (NaN, an infinity, or a finite pct that overflows)
//   2. below 0.5           (zero, negatives, anything that would print "0%")
//   3. 9999.5 and above    (the label is sized for four digits)
inline std::wstring format_zoom_pct(float pct) {
    const float p = pct * 100.0f;
    if (!std::isfinite(p)) return {};
    if (p < 0.5f) return {};
    if (p >= 9999.5f) return {};
    return std::to_wstring(std::lround(p)) + L"%";
}
```

- [ ] **Step 5: Build and run the new tests**

```powershell
$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake --build C:\Users\User\projects\litepdf\build --target litepdf_unit_tests --config Release
Push-Location C:\Users\User\projects\litepdf
& .\build\tests\Release\litepdf_unit_tests.exe "[statusbar]"
Pop-Location
```

Expected: `All tests passed`, with six more test cases than before.

- [ ] **Step 6: Commit**

```bash
git add src/ui/detail/StatusBarMath.hpp tests/unit/test_status_bar_math.cpp
git commit -m "feat: format the zoom percentage for the status bar (#57)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

## Task 2: Third child rectangle

**Files:**
- Modify: `src/ui/detail/StatusBarMath.hpp:50-75`
- Modify: `src/ui/StatusBar.cpp:107-109`, `:220-226`
- Test: `tests/unit/test_status_bar_math.cpp:61-92`

**Interfaces:**
- Consumes: nothing.
- Produces: `StatusBarChildRects` gains `int zoom_x, zoom_y, zoom_w, zoom_h`;
  `status_bar_child_rects(int bar_h, int pad_px, int edit_w_px, int label_w_px, int zoom_w_px)` — five required parameters, no default.
  `StatusBar.cpp` gains `constexpr int kZoomWDip = 45;`.

- [ ] **Step 1: Update the two existing tests so they fail to compile**

In `tests/unit/test_status_bar_math.cpp`, replace the whole existing test case named `StatusBarMath status_bar_child_rects centers children and lays them left to right` with:

```cpp
TEST_CASE("StatusBarMath status_bar_child_rects centers children and lays them left to right",
          "[statusbar]") {
    const auto r = status_bar_child_rects(/*bar_h=*/24, /*pad_px=*/4,
                                          /*edit_w_px=*/48, /*label_w_px=*/72,
                                          /*zoom_w_px=*/40);
    REQUIRE(r.edit_h  == 16);
    REQUIRE(r.edit_y  == 4);
    REQUIRE(r.edit_x  == 4);
    REQUIRE(r.edit_w  == 48);
    REQUIRE(r.label_x == 4 + 48 + 4);
    REQUIRE(r.label_y == r.edit_y);
    REQUIRE(r.label_h == r.edit_h);
    REQUIRE(r.label_w == 72);
    // The zoom readout: one padding step right of the "/ N" label's rectangle.
    REQUIRE(r.zoom_x  == 4 + 48 + 4 + 72 + 4);
    REQUIRE(r.zoom_y  == r.edit_y);
    REQUIRE(r.zoom_h  == r.edit_h);
    REQUIRE(r.zoom_w  == 40);
}
```

and the whole existing test case named `StatusBarMath status_bar_child_rects degrades safely on a tiny bar` with:

```cpp
TEST_CASE("StatusBarMath status_bar_child_rects degrades safely on a tiny bar",
          "[statusbar]") {
    // A bar shorter than twice the padding (bar_h=4 <= 2*pad_px=8) falls back
    // to the `else` branch of both clamps: ctrl_h = bar_h (not bar_h - 2*pad)
    // and y = 0 (not a negative offset). Pin every field so a rewrite that
    // silently returns e.g. edit_h == 0 for an ordinary 24 px bar -- or drops
    // the fallback entirely -- cannot still pass this test.
    const auto r = status_bar_child_rects(/*bar_h=*/4, /*pad_px=*/4,
                                          /*edit_w_px=*/48, /*label_w_px=*/72,
                                          /*zoom_w_px=*/40);
    REQUIRE(r.edit_x   == 4);
    REQUIRE(r.edit_y   == 0);
    REQUIRE(r.edit_w   == 48);
    REQUIRE(r.edit_h   == 4);
    REQUIRE(r.label_x  == 56);
    REQUIRE(r.label_y  == 0);
    REQUIRE(r.label_w  == 72);
    REQUIRE(r.label_h  == 4);
    REQUIRE(r.zoom_x   == 132);
    REQUIRE(r.zoom_y   == 0);
    REQUIRE(r.zoom_w   == 40);
    REQUIRE(r.zoom_h   == 4);
}
```

- [ ] **Step 2: Build to verify it fails**

```powershell
$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake --build C:\Users\User\projects\litepdf\build --target litepdf_unit_tests --config Release
```

Expected: compile error, `'status_bar_child_rects': function does not take 5 arguments`.

- [ ] **Step 3: Implement the geometry**

In `src/ui/detail/StatusBarMath.hpp`, replace everything from the comment line
`// Pixel geometry of the two children inside the bar's client rect.` through the closing `}` of `status_bar_child_rects` (the line after `return r;`) — the struct, both comments and the function — with:

```cpp
// Pixel geometry of the three children inside the bar's client rect.
struct StatusBarChildRects {
    int edit_x = 0, edit_y = 0, edit_w = 0, edit_h = 0;
    int label_x = 0, label_y = 0, label_w = 0, label_h = 0;
    int zoom_x = 0, zoom_y = 0, zoom_w = 0, zoom_h = 0;
};

// Lay the page box, the "/ N" label and the zoom readout out left to right
// with a uniform padding, all vertically centred in a bar `bar_h` pixels tall.
// Callers pass pixel values already scaled for DPI, so this stays pure
// arithmetic.
//
// The readout starts after the label's RECTANGLE, not after its text: the
// label is a fixed width, so a short "/ 12" leaves a gap. Closing it would
// mean measuring text, which this fixed-offset layout deliberately avoids.
inline StatusBarChildRects status_bar_child_rects(int bar_h, int pad_px,
                                                  int edit_w_px,
                                                  int label_w_px,
                                                  int zoom_w_px) noexcept {
    const int ctrl_h = (bar_h > 2 * pad_px) ? (bar_h - 2 * pad_px) : bar_h;
    const int y      = (bar_h - ctrl_h) / 2;

    StatusBarChildRects r;
    r.edit_x  = pad_px;
    r.edit_y  = (y > 0) ? y : 0;
    r.edit_w  = edit_w_px;
    r.edit_h  = (ctrl_h > 0) ? ctrl_h : 0;
    r.label_x = pad_px + edit_w_px + pad_px;
    r.label_y = r.edit_y;
    r.label_w = label_w_px;
    r.label_h = r.edit_h;
    r.zoom_x  = r.label_x + label_w_px + pad_px;
    r.zoom_y  = r.edit_y;
    r.zoom_w  = zoom_w_px;
    r.zoom_h  = r.edit_h;
    return r;
}
```

- [ ] **Step 4: Update the one production call site**

In `src/ui/StatusBar.cpp`, after `constexpr int kLabelWDip = 96;` (line 109) add:

```cpp
// "9999%" -- the longest string format_zoom_pct returns -- measures 41 px in
// 9 pt Segoe UI at 96 DPI (TextRenderer.MeasureText, NoPadding). Plus one
// padding step.
constexpr int kZoomWDip  = 45;
```

and in `Impl::relayout()` change the call (lines 224-226) to:

```cpp
        const auto r = detail::status_bar_child_rects(
            rc.bottom - rc.top, dp(kPadDip, dpi),
            dp(kEditWDip, dpi), dp(kLabelWDip, dpi), dp(kZoomWDip, dpi));
```

The zoom rectangle is not used yet; Task 3 positions the control with it.

- [ ] **Step 5: Build both targets and run the tests**

```powershell
$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake --build C:\Users\User\projects\litepdf\build --target litepdf_unit_tests --config Release
& $cmake --build C:\Users\User\projects\litepdf\build --target litepdf --config Release
Push-Location C:\Users\User\projects\litepdf
& .\build\tests\Release\litepdf_unit_tests.exe "[statusbar]"
Pop-Location
```

Expected: both builds succeed; `All tests passed`.

- [ ] **Step 6: Commit**

```bash
git add src/ui/detail/StatusBarMath.hpp src/ui/StatusBar.cpp tests/unit/test_status_bar_math.cpp
git commit -m "feat: lay out a third status bar child for the zoom readout (#57)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

## Task 3: The zoom label and its update path

**Files:**
- Modify: `src/ui/StatusBar.hpp`, `src/ui/StatusBar.cpp`
- Modify: `src/ui/PdfCanvas.hpp:202-207`, `src/ui/PdfCanvas.cpp:321-323`, `:425-428`, `:1127-1131`
- Modify: `src/ui/MainWindow.hpp:69`, `src/ui/MainWindow.cpp:331-384`, `:1294-1296`
- Modify: `src/core/DocumentView.hpp:85-90`

**Interfaces:**
- Consumes: `detail::format_zoom_pct(float)` (Task 1); `StatusBarChildRects::zoom_*` and `kZoomWDip` (Task 2).
- Produces:
  - `void StatusBar::set_zoom(float pct);`
  - `using PdfCanvas::CompletionArrivedCb = std::function<void()>; void PdfCanvas::set_on_completion_arrived(CompletionArrivedCb cb);`
  - `void MainWindow::refresh_zoom_readout();`

There is no unit test for this task: every line needs a live HWND. It ends with a scripted smoke that reads the label's text from the running exe; the full GUI checks are Task 6.

- [ ] **Step 1: Declare `set_zoom`**

In `src/ui/StatusBar.hpp`, change the first line of the header comment (line 3-4) to:

```cpp
// ui::StatusBar -- PR-B: a msctls_statusbar32 docked at the bottom of
// MainWindow, hosting the page indicator, the go-to-page input and the zoom
// readout (#57).
```

and after the `set_page` declaration (line 74) add:

```cpp
    // Show the magnification (#57). `pct` is DocumentView::zoom_pct(): 1.0 is
    // "100%". Skips the write and the repaint when the formatted text has not
    // changed, so callers may call it on every render completion.
    void set_zoom(float pct);
```

Change the `set_empty` comment (line 76) to:

```cpp
    // No document (last tab closed): clear all three children and disable the box.
```

- [ ] **Step 2: Add the child to `Impl`**

In `src/ui/StatusBar.cpp`:

After `constexpr UINT_PTR kIdLabel        = 2;` (line 112) add:

```cpp
constexpr UINT_PTR kIdZoom         = 3;
```

In `struct StatusBar::Impl`, change the HWND members (lines 129-131) to:

```cpp
    HWND hwnd  = nullptr;
    HWND edit  = nullptr;
    HWND label = nullptr;
    HWND zoom  = nullptr;
```

and after `std::wstring last_written;` (line 143) add:

```cpp

    // The text the zoom readout currently shows. set_zoom compares against it
    // so an unchanged percentage costs neither a SetWindowText nor a repaint.
    std::wstring zoom_text;
```

In `Impl::relayout()`, after the `if (label) { ... }` block add:

```cpp
        if (zoom) {
            SetWindowPos(zoom, nullptr, r.zoom_x, r.zoom_y,
                         r.zoom_w, r.zoom_h,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
```

In `Impl::repaint()`, replace the body (lines 257-258) with:

```cpp
        if (hwnd)  InvalidateRect(hwnd, nullptr, TRUE);
        if (label) InvalidateRect(label, nullptr, FALSE);
        if (zoom)  InvalidateRect(zoom, nullptr, FALSE);
```

and in the comment above it, change "BOTH windows must be invalidated" to "The bar AND each transparent label must be invalidated", and "The label erases nothing" to "A label erases nothing".

- [ ] **Step 3: Theme arm and section comment**

In `status_bar_subclass`, in the `WM_SYSCOLORCHANGE` / `WM_SETTINGCHANGE` arm, after
`if (impl->label) InvalidateRect(impl->label, nullptr, FALSE);` (line 408) add:

```cpp
                    if (impl->zoom)  InvalidateRect(impl->zoom, nullptr, FALSE);
```

Change the section comment at line 295 from
`// Status bar subclass -- background brushes for the two children.` to:

```cpp
// Status bar subclass -- background brushes for the children.
```

and in the same comment block change `The "/ N" STATIC wants to be transparent` to `The "/ N" and zoom STATICs want to be transparent`. No colour code changes: the final `WM_CTLCOLORSTATIC` branch already serves every child that is not the page box.

- [ ] **Step 4: Create the control**

In the `StatusBar::StatusBar` constructor, after the label's `WM_SETFONT` call and before `impl_->measure();` (line 567) add:

```cpp

    // #57: the zoom readout. Read-only, so a plain STATIC like the label.
    impl_->zoom = CreateWindowExW(
        0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
        0, 0, 0, 0,
        impl_->hwnd, reinterpret_cast<HMENU>(kIdZoom), hInstance, nullptr);
    SendMessageW(impl_->zoom, WM_SETFONT,
                 reinterpret_cast<WPARAM>(impl_->font.get()),
                 MAKELPARAM(TRUE, 0));
```

- [ ] **Step 5: Font on DPI change**

In `StatusBar::update_dpi`, after the label's `WM_SETFONT` line (line 604) add:

```cpp
    if (impl_->zoom)  SendMessageW(impl_->zoom,  WM_SETFONT, f, MAKELPARAM(TRUE, 0));
```

and in the comment above (lines 594-598) change "all three windows" to "all four windows". This line is load-bearing: without it the readout keeps the old `HFONT` after `old_font` is deleted at the end of the function.

- [ ] **Step 6: `set_zoom` and the empty state**

In `StatusBar::set_empty`, after `if (impl_->label) SetWindowTextW(impl_->label, L"");` (line 653) add:

```cpp
    if (impl_->zoom)  SetWindowTextW(impl_->zoom, L"");
    impl_->zoom_text.clear();
```

After the closing brace of `StatusBar::set_empty` add:

```cpp

void StatusBar::set_zoom(float pct) {
    if (!impl_ || !impl_->zoom) return;
    std::wstring text = detail::format_zoom_pct(pct);
    if (text == impl_->zoom_text) return;
    SetWindowTextW(impl_->zoom, text.c_str());
    impl_->zoom_text = std::move(text);
    impl_->repaint();
}
```

- [ ] **Step 7: The canvas callback**

In `src/ui/PdfCanvas.hpp`, after `void set_on_zoom_changed(ZoomChangedCb cb);` (line 207) add:

```cpp

    // #57: fires for EVERY render-completion message the canvas receives --
    // accepted, stale, superseded and failed alike -- before the canvas looks
    // at the message. The owner uses it to refresh anything derived from the
    // view's live state rather than from the pixmap (the zoom readout). The
    // callback must not submit renders or change the view. Pass nullptr to
    // clear.
    using CompletionArrivedCb = std::function<void()>;
    void set_on_completion_arrived(CompletionArrivedCb cb);
```

In `src/ui/PdfCanvas.cpp`, in `struct PdfCanvas::Impl` after
`PdfCanvas::ZoomChangedCb      on_zoom_changed;` (line 323) add:

```cpp
    // Fired at the entry of the render-completion arm (#57).
    PdfCanvas::CompletionArrivedCb on_completion_arrived;
```

After the closing brace of `PdfCanvas::set_on_zoom_changed` (line 428) add:

```cpp

void PdfCanvas::set_on_completion_arrived(CompletionArrivedCb cb) {
    if (!impl_) return;
    impl_->on_completion_arrived = std::move(cb);
}
```

In the window procedure, change the start of the completion arm (lines 1127-1131) to:

```cpp
        case WM_USER_RENDER_DONE:
        case WM_USER_RENDER_DONE_RIGHT: {
            // #57: BEFORE the null check and before accept_completion, on
            // purpose. The owner refreshes the zoom readout from the view's
            // live percentage, not from this pixmap, so a failed or stale
            // completion is as good a moment as an accepted one -- and a
            // fit-mode page turn has already changed the percentage by the
            // time its render fails. Moving this below either early return
            // would leave the readout disagreeing with the page box.
            if (impl_->on_completion_arrived) impl_->on_completion_arrived();

            const bool is_right = (msg == WM_USER_RENDER_DONE_RIGHT);
            auto* pix  = reinterpret_cast<fz_pixmap*>(w);
            auto* meta = reinterpret_cast<RenderMeta*>(l);
```

- [ ] **Step 8: `MainWindow::refresh_zoom_readout` and its two call sites**

In `src/ui/MainWindow.hpp`, after `void kick_render(int page);` (line 69) add:

```cpp
    // #57: push the active view's live zoom percentage to the status bar.
    // No-op without a status bar or an active view (the empty state is
    // StatusBar::set_empty's job). Writes text only -- it must never submit a
    // render or change the view, because the canvas calls it from inside its
    // render-completion arm.
    void refresh_zoom_readout();
```

In `src/ui/MainWindow.cpp`, in `kick_render`, change the spread branch's `apply_viewport` line (line 361) to:

```cpp
        canvas_->apply_viewport();
        // #57: this branch returns early, so it carries its own refresh.
        refresh_zoom_readout();
```

and the single-page branch's (line 378) to:

```cpp
    canvas_->apply_viewport();
    // #57: the fit is re-derived by now, so the readout changes in the same
    // message as the command that caused it.
    refresh_zoom_readout();
```

After the closing brace of `kick_render` (line 384) add:

```cpp

void MainWindow::refresh_zoom_readout() {
    if (!status_bar_) return;
    if (auto* v = active_view()) status_bar_->set_zoom(v->zoom_pct());
}
```

In `WM_CREATE`, after `canvas_->set_on_zoom_changed([this] { schedule_session_save(); });` (line 1296) add:

```cpp
            // #57: kick_render covers the commands that go through it. This
            // covers the two paths that do not: Ctrl+wheel and the canvas's
            // own page turns.
            canvas_->set_on_completion_arrived([this] { refresh_zoom_readout(); });
```

- [ ] **Step 9: The stale comment in `DocumentView.hpp`**

In `src/core/DocumentView.hpp`, change lines 85-89 to:

```cpp
    // User-facing magnification. 1.0 means ONE PDF POINT MAPS TO ONE DIP -- the
    // conventional 96-dpi screen ratio browsers also call 100%. It is not
    // physical actual size: a PDF point is 1/72 inch, so 1.0 renders at 0.75x
    // ruler size. The status bar shows it as a percentage (#57).
```

- [ ] **Step 10: Build and run the unit tests**

```powershell
$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ctest = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
& $cmake --build C:\Users\User\projects\litepdf\build --target litepdf --config Release
& $cmake --build C:\Users\User\projects\litepdf\build --target litepdf_unit_tests --config Release
Push-Location C:\Users\User\projects\litepdf
& $ctest --test-dir build -C Release
Pop-Location
```

Expected: both builds succeed; `100% tests passed`.

- [ ] **Step 11: Smoke — the label shows a percentage**

Close any running LitePDF first (the app is single-instance). Create the folder with `New-Item -ItemType Directory -Force C:\Users\User\projects\litepdf\build\gui-check`, save the script as `build/gui-check/smoke-readout.ps1` and run it with `powershell -ExecutionPolicy Bypass -File build\gui-check\smoke-readout.ps1`:

```powershell
#Requires -Version 5.1
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
$repo = 'C:\Users\User\projects\litepdf'
$exe  = Join-Path $repo 'build\Release\litepdf.exe'
$pdf  = Join-Path $repo 'tests\fixtures\simple.pdf'
if (Get-Process | Where-Object { $_.Name -eq 'litepdf' -or $_.Name -like '*-probe' }) {
    throw 'A LitePDF instance is already running. Close it first.'
}
Add-Type @'
using System; using System.Collections.Generic; using System.Runtime.InteropServices; using System.Text;
public static class SmokeW {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")] static extern IntPtr SendText(IntPtr h, uint m, IntPtr w, StringBuilder l);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  public static List<string> StaticTexts(IntPtr main) {
    var r = new List<string>();
    EnumChildWindows(main, (h, l) => {
      var c = new StringBuilder(64); GetClassNameW(h, c, 64);
      if (c.ToString() == "Static") { var t = new StringBuilder(256); SendText(h, 0x000D, (IntPtr)256, t); r.Add(t.ToString()); }
      return true; }, IntPtr.Zero);
    return r;
  }
}
'@
$env:LITEPDF_NO_RESTORE = '1'
$session = Join-Path $env:LOCALAPPDATA 'LitePDF\session.json'
$backup  = Join-Path $env:TEMP 'litepdf-session-smoke.bak'
$had = Test-Path $session
if ($had) { Copy-Item $session $backup -Force }
$p = Start-Process $exe -ArgumentList ('"{0}"' -f $pdf) -PassThru
try {
    $main = [IntPtr]::Zero
    for ($i = 0; $i -lt 50 -and [int64]$main -eq 0; $i++) { Start-Sleep -Milliseconds 200; $p.Refresh(); $main = $p.MainWindowHandle }
    if ([int64]$main -eq 0) { throw 'no main window' }
    Start-Sleep -Milliseconds 1500
    $texts = [SmokeW]::StaticTexts($main)
    'static texts: ' + ($texts -join ' | ')
    $hit = @($texts | Where-Object { $_ -match '^\d+%$' })
    if ($hit.Count -ne 1) { throw "expected exactly one percentage label, got $($hit.Count)" }
    'PASS readout = ' + $hit[0]
} finally {
    # The app writes session.json as it exits, so it must be gone before the
    # backup is copied back.
    if (-not $p.HasExited) {
        [void][SmokeW]::PostMessageW($p.MainWindowHandle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
        if (-not $p.WaitForExit(5000)) { $p.Kill(); [void]$p.WaitForExit(5000) }
    }
    if ($had) { Copy-Item $backup $session -Force } elseif (Test-Path $session) { Remove-Item $session -Force }
}
```

Expected: a line `static texts: / 1 | NNN%` (the page count may differ) and `PASS readout = NNN%`.

- [ ] **Step 12: Commit**

```bash
git add src/ui/StatusBar.hpp src/ui/StatusBar.cpp src/ui/PdfCanvas.hpp src/ui/PdfCanvas.cpp src/ui/MainWindow.hpp src/ui/MainWindow.cpp src/core/DocumentView.hpp
git commit -m "feat: show the zoom percentage in the status bar (#57)

A read-only label next to the page count. MainWindow refreshes it from the
active view's live percentage in two places: each branch of kick_render,
and a canvas callback at the entry of the render-completion arm, which
covers Ctrl+wheel and the canvas's own page turns.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

## Task 4: `StatusBar::set_visible`

**Files:**
- Modify: `src/ui/StatusBar.hpp`, `src/ui/StatusBar.cpp`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: `void StatusBar::set_visible(bool visible);` and `bool StatusBar::visible() const;`.
  `height_px()` returns 0 while hidden; `page_box_has_focus()` returns false while hidden.

No unit test: HWND-only. Nothing calls `set_visible` until Task 5, so this task is verified by the build and by the unchanged unit suite; behaviour is checked in Task 6.

- [ ] **Step 1: Declare**

In `src/ui/StatusBar.hpp`, replace the `height_px` declaration and its comment (lines 61-63) with:

```cpp
    // Natural height at the current DPI, in pixels -- or 0 while the bar is
    // hidden, which is what makes MainWindow's layout give the strip back.
    // Measured from the control itself at construction and re-measured by
    // update_dpi().
    int height_px() const;

    // #59: show or hide the whole bar. Hiding hands the keyboard back first if
    // the page box holds it. The measured height is kept while hidden, so a
    // DPI change in between cannot bring back a reserved strip. The owner
    // re-runs its layout afterwards.
    void set_visible(bool visible);
    bool visible() const;
```

and in the `page_box_has_focus` comment, change the first line (line 79) to:

```cpp
    // True while the page box holds the keyboard focus AND the bar is visible.
```

- [ ] **Step 2: Implement**

In `src/ui/StatusBar.cpp`, in `struct StatusBar::Impl` after `int  height_px = 0;` (line 133) add:

```cpp
    // #59. height_px above is the MEASURED height and is never zeroed:
    // update_dpi() re-measures a hidden bar too, and StatusBar::height_px()
    // gates on this flag instead.
    bool visible = true;
```

Replace `StatusBar::height_px` (line 580) with:

```cpp
int StatusBar::height_px() const {
    return (impl_ && impl_->visible) ? impl_->height_px : 0;
}

void StatusBar::set_visible(bool visible) {
    if (!impl_ || !impl_->hwnd) return;
    if (visible == impl_->visible) return;
    if (!visible) {
        // Hand the keyboard back BEFORE hiding. ShowWindow(SW_HIDE) does not
        // move the focus off a child, so the caret would stay in a box nobody
        // can see and digits typed next would go into it. Same order as
        // set_empty(), which does it before EnableWindow(FALSE).
        if (GetFocus() == impl_->edit && impl_->on_focus_out) {
            impl_->on_focus_out();
        }
        ShowWindow(impl_->hwnd, SW_HIDE);
    } else {
        ShowWindow(impl_->hwnd, SW_SHOWNA);
    }
    impl_->visible = visible;
}

bool StatusBar::visible() const { return impl_ && impl_->visible; }
```

Replace `StatusBar::page_box_has_focus` (lines 657-659) with:

```cpp
bool StatusBar::page_box_has_focus() const {
    // visible first, for the reason ResultsPanel::has_focus() gives: hiding a
    // window does not move the focus off its children, so a hidden box must
    // not claim ESC.
    return impl_ && impl_->visible && impl_->edit && GetFocus() == impl_->edit;
}
```

- [ ] **Step 3: Build and run the unit tests**

```powershell
$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ctest = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
& $cmake --build C:\Users\User\projects\litepdf\build --target litepdf --config Release
& $cmake --build C:\Users\User\projects\litepdf\build --target litepdf_unit_tests --config Release
Push-Location C:\Users\User\projects\litepdf
& $ctest --test-dir build -C Release
Pop-Location
```

Expected: both builds succeed; `100% tests passed`.

- [ ] **Step 4: Commit**

```bash
git add src/ui/StatusBar.hpp src/ui/StatusBar.cpp
git commit -m "feat: let the status bar be hidden (#59)

set_visible hides the control and makes height_px() report 0. The
measured height is kept, so a DPI change while hidden cannot bring back
a reserved strip.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

## Task 5: The View-menu toggle

**Files:**
- Modify: `resources/MainMenu.rc.h:66-73`
- Modify: `resources/litepdf.rc.in:74-75`
- Modify: `src/ui/MainWindow.hpp:82-83`
- Modify: `src/ui/MainWindow.cpp:444-449`, `:621-627`, `:1497-1512`, `:1614-1620`

**Interfaces:**
- Consumes: `StatusBar::set_visible(bool)`, `StatusBar::visible()` (Task 4); `MainWindow::kick_render(int)`, which since Task 3 refreshes the readout.
- Produces: `#define IDM_VIEW_STATUS_BAR 40064`; `void MainWindow::on_toggle_status_bar();`.

- [ ] **Step 1: The command id**

In `resources/MainMenu.rc.h`, replace lines 69-71 with:

```cpp
// #59: status bar toggle. Takes the first id of the old 40064-40070 reservation.
#define IDM_VIEW_STATUS_BAR  40064   // View > Status Bar (no accelerator)

// Next free ID: 40065. Reserve 40065-40070 for future Phase 8.x cleanups.

// #52: text selection. A fresh block, leaving the 40065-40070 reservation alone.
```

- [ ] **Step 2: The menu item**

In `resources/litepdf.rc.in`, after the Two-Page Spread item (line 74) and before `MENUITEM SEPARATOR` add:

```
        MENUITEM "Status &Bar", IDM_VIEW_STATUS_BAR
```

- [ ] **Step 3: Declare the handler**

In `src/ui/MainWindow.hpp`, after `void toggle_thumbs();` (line 83) add:

```cpp
    void on_toggle_status_bar();         // IDM_VIEW_STATUS_BAR (#59)
```

- [ ] **Step 4: Implement the handler**

In `src/ui/MainWindow.cpp`, after the closing brace of `MainWindow::toggle_outline` (line 627) add:

```cpp

void MainWindow::on_toggle_status_bar() {
    // #59: window-level state, so no active_view() gate -- the toggle works
    // with no document open.
    if (!status_bar_) return;
    status_bar_->set_visible(!status_bar_->visible());
    on_layout();
    // The canvas just changed height. Its own WM_SIZE resubmits only when the
    // render target has to be recreated, so without this a fit mode would keep
    // the fit it derived for the old height.
    if (auto* view = active_view()) kick_render(view->current_page());
}
```

- [ ] **Step 5: Skip `set_bounds` while hidden**

In `MainWindow::on_layout`, change the condition at line 446 from
`if (status_bar_ && status_bar_->hwnd()) {` to:

```cpp
    if (status_bar_ && status_bar_->hwnd() && status_bar_->visible()) {
```

`status_h` two lines above needs no change: `height_px()` already reports 0 while hidden, and so do the other two readers (the results-panel clamp and the splitter drag clamp).

- [ ] **Step 6: The command arm**

In the `WM_COMMAND` switch, after the `IDM_VIEW_THUMBS` case (lines 1618-1620) add:

```cpp
                case IDM_VIEW_STATUS_BAR:
                    on_toggle_status_bar();
                    return 0;
```

- [ ] **Step 7: The checkmark**

In the `WM_INITMENUPOPUP` View arm, inside the `if (popup_owns(popup, IDM_VIEW_INVERT)) {` block, after the `CheckMenuItem(popup, IDM_VIEW_DUAL_PAGE, ...)` call and **before** `return 0;` (line 1511) add:

```cpp
                // #59: window-level, so not read off active_view(). It must
                // sit inside this block: the block returns, so an arm after it
                // would never run.
                const bool bar_on = status_bar_ && status_bar_->visible();
                CheckMenuItem(popup, IDM_VIEW_STATUS_BAR,
                              MF_BYCOMMAND
                              | (bar_on ? MF_CHECKED : MF_UNCHECKED));
```

- [ ] **Step 8: Build and run the unit tests**

```powershell
$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ctest = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
& $cmake --build C:\Users\User\projects\litepdf\build --target litepdf --config Release
& $cmake --build C:\Users\User\projects\litepdf\build --target litepdf_unit_tests --config Release
Push-Location C:\Users\User\projects\litepdf
& $ctest --test-dir build -C Release
Pop-Location
```

Expected: both builds succeed (the resource compiler picks up the new id); `100% tests passed`.

- [ ] **Step 9: Run the repo smoke test**

```powershell
powershell -ExecutionPolicy Bypass -File C:\Users\User\projects\litepdf\scripts\smoke-test.ps1 -ExpectDev
```

Expected: exits 0. It also asserts the exe is under 12 MB.

- [ ] **Step 10: Commit**

```bash
git add resources/MainMenu.rc.h resources/litepdf.rc.in src/ui/MainWindow.hpp src/ui/MainWindow.cpp
git commit -m "feat: View > Status Bar hides and shows the status bar (#59)

Window-level, not persisted, no accelerator. The toggle re-runs the
layout and re-renders, so a fit mode follows the canvas's new height.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

## Task 6: Scripted GUI checks

**Files:**
- Create (untracked): `build/gui-check/status-bar.ps1`
- Create (untracked): `build/gui-check/normal-probe.exe`, `build/gui-check/site1off-probe.exe`
- Temporarily modify, then revert: `src/ui/MainWindow.cpp`

**Interfaces:**
- Consumes: the finished feature from Tasks 1-5. Command ids: `IDM_ZOOM_IN 40010`, `IDM_ZOOM_OUT 40011`, `IDM_ZOOM_RESET 40012`, `IDM_VIEW_OUTLINE 40013`, `IDM_TAB_CLOSE 40030`, `IDM_TAB_GOTO_1 40033`, `IDM_TAB_GOTO_2 40034`, `IDM_CROSS_TAB_FIND 40045`, `IDM_TOGGLE_RESULTS 40046`, `IDM_VIEW_INVERT 40061`, `IDM_VIEW_DUAL_PAGE 40062`, `IDM_VIEW_STATUS_BAR 40064`. Window classes: `LitePDFPdfCanvas`, `msctls_statusbar32`.
- Produces: a PASS/FAIL table for each of two exes. Nothing is committed.

Two exes are driven. `normal-probe.exe` is the real build. `site1off-probe.exe` is a mutant with the canvas callback left unwired; it proves that `kick_render`'s two calls are sufficient on their own for the paths they claim, and that the callback is what covers Ctrl+wheel and PgDn. The mutant is the negative control for the normal run.

Preconditions: close every running LitePDF, including an installed copy. Do not run `ctest` at the same time. Leave the machine alone while the driver runs; the monitor must be on.

- [ ] **Step 1: Build the mutant exe, then restore the source**

Run from the repo root in Git Bash:

```bash
cd /c/Users/User/projects/litepdf
test -z "$(git status --short)" || { echo "working tree not clean"; exit 1; }
mkdir -p build/gui-check
rm -f build/gui-check/site1off-probe.exe build/gui-check/normal-probe.exe
python - <<'EOF'
import io
p = "src/ui/MainWindow.cpp"
s = io.open(p, encoding="utf-8", newline="").read()
old = "canvas_->set_on_completion_arrived([this] { refresh_zoom_readout(); });"
assert s.count(old) == 1, s.count(old)
io.open(p, "w", encoding="utf-8", newline="").write(s.replace(old, "/* site1off probe */"))
EOF
"/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build --target litepdf --config Release \
  && cp build/Release/litepdf.exe build/gui-check/site1off-probe.exe \
  || echo "MUTANT BUILD FAILED - no site1off-probe.exe was made"
git checkout -- src/ui/MainWindow.cpp
test -z "$(git status --short)" && echo "source restored"
"/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build --target litepdf --config Release
cp build/Release/litepdf.exe build/gui-check/normal-probe.exe
```

Expected: `source restored`, two builds succeed, no `MUTANT BUILD FAILED` line, two exes in `build/gui-check/`. The source is restored whether or not the mutant build succeeded. If `python` is not found, use `py -3` in its place.

- [ ] **Step 2: Write the driver**

Save as `build/gui-check/status-bar.ps1`:

```powershell
#Requires -Version 5.1
# GUI checks for #57 (zoom readout) and #59 (hide the status bar).
#   -Mode normal    : the real build.
#   -Mode site1off  : mutant with the canvas completion callback unwired.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][ValidateSet('normal', 'site1off')][string]$Mode
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
Add-Type -AssemblyName System.Drawing

Add-Type @'
using System; using System.Collections.Generic; using System.Runtime.InteropServices; using System.Text;
public static class SbW {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [StructLayout(LayoutKind.Sequential)] public struct GUITHREADINFO {
    public int cbSize; public int flags; public IntPtr hwndActive, hwndFocus, hwndCapture, hwndMenuOwner, hwndMoveSize, hwndCaret; public RECT rcCaret; }
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")] static extern IntPtr SendText(IntPtr h, uint m, IntPtr w, StringBuilder l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ScreenToClient(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] static extern bool GetGUIThreadInfo(uint tid, ref GUITHREADINFO g);
  [DllImport("user32.dll")] public static extern IntPtr GetMenu(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetSubMenu(IntPtr m, int pos);
  [DllImport("user32.dll")] public static extern uint GetMenuState(IntPtr m, uint id, uint flags);
  [DllImport("user32.dll")] public static extern IntPtr GetDC(IntPtr h);
  [DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr h, IntPtr dc);
  [DllImport("gdi32.dll")] public static extern bool BitBlt(IntPtr d, int x, int y, int w, int h, IntPtr s, int sx, int sy, uint rop);
  [DllImport("user32.dll")] public static extern bool RedrawWindow(IntPtr h, IntPtr r, IntPtr rgn, uint f);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  public static List<IntPtr> Kids(IntPtr p) {
    var r = new List<IntPtr>();
    EnumChildWindows(p, (h, l) => { r.Add(h); return true; }, IntPtr.Zero);
    return r;
  }
  public static string Cls(IntPtr h) { var s = new StringBuilder(128); GetClassNameW(h, s, 128); return s.ToString(); }
  public static string Text(IntPtr h) { var s = new StringBuilder(256); SendText(h, 0x000D, (IntPtr)256, s); return s.ToString(); }
  public static string Title(IntPtr h) { var s = new StringBuilder(512); GetWindowTextW(h, s, 512); return s.ToString(); }
  public static IntPtr Focus(IntPtr anyWindowOfThread) {
    uint pid; uint tid = GetWindowThreadProcessId(anyWindowOfThread, out pid);
    var g = new GUITHREADINFO(); g.cbSize = Marshal.SizeOf(typeof(GUITHREADINFO));
    return GetGUIThreadInfo(tid, ref g) ? g.hwndFocus : IntPtr.Zero;
  }
}
'@
[void][SbW]::SetProcessDPIAware()

$repo   = 'C:\Users\User\projects\litepdf'
$simple = Join-Path $repo 'tests\fixtures\simple.pdf'
$spread = Join-Path $repo 'tests\fixtures\spread-unequal.pdf'
foreach ($f in $Exe, $simple, $spread) { if (-not (Test-Path $f)) { throw "missing: $f" } }
if (Get-Process | Where-Object { $_.Name -eq 'litepdf' -or $_.Name -like '*-probe' }) {
    throw 'A LitePDF instance is already running. Close it first.'
}

$script:Results = New-Object System.Collections.ArrayList
function Add-Check([string]$Name, [bool]$Ok, [string]$Detail) {
    $tag = 'FAIL'; if ($Ok) { $tag = 'PASS' }
    [void]$script:Results.Add("$tag  $Name  [$Detail]")
    Write-Host "$tag  $Name  [$Detail]"
}
function Wait-Until([scriptblock]$Cond, [int]$Ms = 4000) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $Ms) { if (& $Cond) { return $true }; Start-Sleep -Milliseconds 50 }
    return [bool](& $Cond)
}
function Send-Cmd([int]$Id) { [void][SbW]::PostMessageW($script:Main, 0x0111, [IntPtr]$Id, [IntPtr]::Zero) }
function Get-Zoom { return [SbW]::Text($script:ZoomLabel) }
# Post a command and wait for the readout to differ from $Old. Returns the text.
function Send-AndRead([int]$Id, [string]$Old) {
    Send-Cmd $Id
    [void](Wait-Until { (Get-Zoom) -ne $Old } 3000)
    Start-Sleep -Milliseconds 150
    return (Get-Zoom)
}
function Get-RectIn([IntPtr]$Child) {
    $r = New-Object SbW+RECT; [void][SbW]::GetWindowRect($Child, [ref]$r)
    $a = New-Object SbW+POINT; $a.X = $r.Left;  $a.Y = $r.Top
    $b = New-Object SbW+POINT; $b.X = $r.Right; $b.Y = $r.Bottom
    [void][SbW]::ScreenToClient($script:Main, [ref]$a); [void][SbW]::ScreenToClient($script:Main, [ref]$b)
    return @{ Left = $a.X; Top = $a.Y; Right = $b.X; Bottom = $b.Y }
}
function Get-ClientH { $r = New-Object SbW+RECT; [void][SbW]::GetClientRect($script:Main, [ref]$r); return $r.Bottom }
# Lowest edge of any visible direct child of the main window other than the bar.
function Get-ContentBottom {
    $max = 0
    foreach ($h in [SbW]::Kids($script:Main)) {
        if ([SbW]::GetParent($h) -ne $script:Main) { continue }
        if ($h -eq $script:Bar) { continue }
        if (-not [SbW]::IsWindowVisible($h)) { continue }
        $b = (Get-RectIn $h).Bottom
        if ($b -gt $max) { $max = $b }
    }
    return $max
}
function Test-Checked([int]$Id) {
    $view = [SbW]::GetSubMenu([SbW]::GetMenu($script:Main), 2)   # File, Edit, View
    [void][SbW]::SendMessageW($script:Main, 0x0117, $view, [IntPtr]2)   # WM_INITMENUPOPUP
    return (([SbW]::GetMenuState($view, [uint32]$Id, 0) -band 0x8) -ne 0)   # MF_CHECKED
}
function Get-Pixels([IntPtr]$H) {
    $r = New-Object SbW+RECT; [void][SbW]::GetClientRect($H, [ref]$r)
    if ($r.Right -le 0 -or $r.Bottom -le 0) { return '' }
    $bmp = New-Object System.Drawing.Bitmap($r.Right, $r.Bottom)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $dst = $g.GetHdc(); $src = [SbW]::GetDC($H)
    [void][SbW]::BitBlt($dst, 0, 0, $r.Right, $r.Bottom, $src, 0, 0, 0x00CC0020)
    [void][SbW]::ReleaseDC($H, $src); $g.ReleaseHdc($dst); $g.Dispose()
    $sb = New-Object System.Text.StringBuilder
    for ($y = 0; $y -lt $r.Bottom; $y++) { for ($x = 0; $x -lt $r.Right; $x++) { [void]$sb.Append($bmp.GetPixel($x, $y).ToArgb().ToString('X8')) } }
    $bmp.Dispose()
    return $sb.ToString()
}
function Find-Parts {
    $script:Bar = [IntPtr]::Zero; $script:Canvas = [IntPtr]::Zero; $script:Edit = [IntPtr]::Zero
    $statics = @()
    foreach ($h in [SbW]::Kids($script:Main)) {
        $c = [SbW]::Cls($h)
        if ($c -eq 'msctls_statusbar32') { $script:Bar = $h }
        if ($c -eq 'LitePDFPdfCanvas')   { $script:Canvas = $h }
    }
    if ([int64]$script:Bar -eq 0 -or [int64]$script:Canvas -eq 0) { throw 'status bar or canvas not found' }
    foreach ($h in [SbW]::Kids($script:Bar)) {
        if ([SbW]::GetParent($h) -ne $script:Bar) { continue }
        $c = [SbW]::Cls($h)
        if ($c -eq 'Edit')   { $script:Edit = $h }
        if ($c -eq 'Static') { $statics += ,@($h, (Get-RectIn $h).Left) }
    }
    if ($statics.Count -ne 2) { throw "expected 2 STATIC children in the bar, found $($statics.Count)" }
    $sorted = @($statics | Sort-Object { $_[1] })
    $script:ZoomLabel = $sorted[1][0]   # the right-hand one
}

$env:LITEPDF_NO_RESTORE = '1'
$session = Join-Path $env:LOCALAPPDATA 'LitePDF\session.json'
$backup  = Join-Path $env:TEMP 'litepdf-session-guicheck.bak'
$hadSession = Test-Path $session
if ($hadSession) { Copy-Item $session $backup -Force }

$proc = Start-Process $Exe -ArgumentList ('"{0}"' -f $simple) -PassThru
try {
    $script:Main = [IntPtr]::Zero
    for ($i = 0; $i -lt 50 -and [int64]$script:Main -eq 0; $i++) { Start-Sleep -Milliseconds 200; $proc.Refresh(); $script:Main = $proc.MainWindowHandle }
    if ([int64]$script:Main -eq 0) { throw 'no main window' }
    [void][SbW]::ShowWindow($script:Main, 1)   # SW_SHOWNORMAL: a known, non-maximized placement
    Find-Parts
    $pct = '^\d+%$'

    # --- 1. initial readout -------------------------------------------------
    [void](Wait-Until { (Get-Zoom) -match $pct } 5000)
    $fit = Get-Zoom
    Add-Check '01 initial readout is a percentage' ($fit -match $pct) $fit

    # --- 2. zoom commands (kick_render, single-page branch) -----------------
    $z = Send-AndRead 40010 $fit
    Add-Check '02 Zoom In changes the readout' (($z -match $pct) -and ($z -ne $fit)) "$fit -> $z"
    for ($i = 0; $i -lt 12; $i++) { Send-Cmd 40010; Start-Sleep -Milliseconds 120 }
    [void](Wait-Until { (Get-Zoom) -eq '800%' } 3000)
    Add-Check '03 top of the ladder reads 800%' ((Get-Zoom) -eq '800%') (Get-Zoom)
    Send-Cmd 40010; Start-Sleep -Milliseconds 600
    Add-Check '04 Zoom In at the top leaves 800%' ((Get-Zoom) -eq '800%') (Get-Zoom)
    $r = Send-AndRead 40012 '800%'
    Add-Check '05 Reset Zoom returns to the fit value' ($r -eq $fit) "$r vs $fit"

    # --- 3. Ctrl+wheel (canvas-only path: call site 1) ----------------------
    $cr = New-Object SbW+RECT; [void][SbW]::GetWindowRect($script:Canvas, [ref]$cr)
    $cx = [int](($cr.Left + $cr.Right) / 2); $cy = [int](($cr.Top + $cr.Bottom) / 2)
    $lp = [IntPtr](($cy -shl 16) -bor ($cx -band 0xFFFF))
    $wp = [IntPtr]((120 -shl 16) -bor 0x0008)   # one notch up, MK_CONTROL
    [void][SbW]::PostMessageW($script:Canvas, 0x020A, $wp, $lp)
    [void](Wait-Until { (Get-Zoom) -ne $fit } 1500)
    $w = Get-Zoom
    if ($Mode -eq 'normal') {
        Add-Check '06 Ctrl+wheel changes the readout' (($w -match $pct) -and ($w -ne $fit)) "$fit -> $w"
    } else {
        Add-Check '06 MUTANT Ctrl+wheel leaves the readout stale' ($w -eq $fit) "$fit -> $w"
    }
    Send-Cmd 40010; Start-Sleep -Milliseconds 400   # move off the fit so Reset is a real change
    $r = Send-AndRead 40012 (Get-Zoom)
    Add-Check '07 Reset after the wheel returns to the fit value' ($r -eq $fit) "$r vs $fit"

    # --- 4. window resize in FitWidth ---------------------------------------
    $wr = New-Object SbW+RECT; [void][SbW]::GetWindowRect($script:Main, [ref]$wr)
    $ww = $wr.Right - $wr.Left; $wh = $wr.Bottom - $wr.Top
    [void][SbW]::SetWindowPos($script:Main, [IntPtr]::Zero, 0, 0, ($ww - 240), $wh, 0x0006)
    [void](Wait-Until { (Get-Zoom) -ne $fit } 3000)
    $n = Get-Zoom
    Add-Check '08 a narrower window changes the readout' (($n -match $pct) -and ($n -ne $fit)) "$fit -> $n"
    [void][SbW]::SetWindowPos($script:Main, [IntPtr]::Zero, 0, 0, $ww, $wh, 0x0006)
    [void](Wait-Until { (Get-Zoom) -eq $fit } 3000)
    Add-Check '09 the original width restores the fit value' ((Get-Zoom) -eq $fit) (Get-Zoom)

    # --- 5. outline pane toggle (F5) ----------------------------------------
    $o = Send-AndRead 40013 $fit
    Add-Check '10 opening the outline pane changes the readout' (($o -match $pct) -and ($o -ne $fit)) "$fit -> $o"
    $o2 = Send-AndRead 40013 $o
    Add-Check '11 closing the outline pane restores the fit value' ($o2 -eq $fit) "$o2 vs $fit"
    $th = Send-AndRead 40060 $fit
    Add-Check '11a opening the thumbnail pane changes the readout' (($th -match $pct) -and ($th -ne $fit)) "$fit -> $th"
    $th2 = Send-AndRead 40060 $th
    Add-Check '11b closing the thumbnail pane restores the fit value' ($th2 -eq $fit) "$th2 vs $fit"

    # --- 6. second tab: page turn, tab switch, two-page mode -----------------
    Start-Process $Exe -ArgumentList ('"{0}"' -f $spread) | Out-Null
    [void](Wait-Until { [SbW]::Title($script:Main) -like '*spread-unequal*' } 6000)
    Start-Sleep -Milliseconds 800
    $t2 = Get-Zoom
    # Page 1 here is 420 pt wide against simple.pdf's 595 pt, so the fits differ.
    Add-Check '12 the new tab shows its own percentage' (($t2 -match $pct) -and ($t2 -ne $fit)) "$fit -> $t2"
    [void][SbW]::PostMessageW($script:Canvas, 0x0100, [IntPtr]0x22, [IntPtr]::Zero)   # PgDn
    [void](Wait-Until { (Get-Zoom) -ne $t2 } 1500)
    $pg = Get-Zoom
    if ($Mode -eq 'normal') {
        Add-Check '13 a fit-mode page turn changes the readout' (($pg -match $pct) -and ($pg -ne $t2)) "$t2 -> $pg"
    } else {
        Add-Check '13 MUTANT a page turn leaves the readout stale' ($pg -eq $t2) "$t2 -> $pg"
    }
    [void][SbW]::PostMessageW($script:Canvas, 0x0100, [IntPtr]0x21, [IntPtr]::Zero)   # PgUp
    Start-Sleep -Milliseconds 600
    $t2z = Send-AndRead 40010 (Get-Zoom)
    Send-Cmd 40033
    [void](Wait-Until { (Get-Zoom) -eq $fit } 3000)
    Add-Check '14 switching to tab 1 shows tab 1''s zoom' ((Get-Zoom) -eq $fit) "$(Get-Zoom) vs $fit"
    Send-Cmd 40034
    [void](Wait-Until { (Get-Zoom) -eq $t2z } 3000)
    Add-Check '15 switching to tab 2 shows tab 2''s zoom' ((Get-Zoom) -eq $t2z) "$(Get-Zoom) vs $t2z"
    $single = Send-AndRead 40012 $t2z
    $dual = Send-AndRead 40062 $single
    Add-Check '16 Two-Page Spread changes the readout' (($dual -match $pct) -and ($dual -ne $single)) "$single -> $dual"
    Add-Check '16a Two-Page Spread is checked, Status Bar still checked' ((Test-Checked 40062) -and (Test-Checked 40064)) ''
    $dz = Send-AndRead 40010 $dual
    Add-Check '17 Zoom In in two-page mode changes the readout' (($dz -match $pct) -and ($dz -ne $dual)) "$dual -> $dz"
    $dr = Send-AndRead 40012 $dz
    Add-Check '18 Reset in two-page mode returns to the spread fit' ($dr -eq $dual) "$dr vs $dual"
    # Resize and tab switch again, this time through kick_render's spread branch.
    [void][SbW]::SetWindowPos($script:Main, [IntPtr]::Zero, 0, 0, ($ww - 240), $wh, 0x0006)
    [void](Wait-Until { (Get-Zoom) -ne $dual } 3000)
    $dn = Get-Zoom
    Add-Check '18a two-page mode: a narrower window changes the readout' (($dn -match $pct) -and ($dn -ne $dual)) "$dual -> $dn"
    [void][SbW]::SetWindowPos($script:Main, [IntPtr]::Zero, 0, 0, $ww, $wh, 0x0006)
    [void](Wait-Until { (Get-Zoom) -eq $dual } 3000)
    Add-Check '18b two-page mode: the original width restores the spread fit' ((Get-Zoom) -eq $dual) (Get-Zoom)
    Send-Cmd 40033
    [void](Wait-Until { (Get-Zoom) -eq $fit } 3000)
    Add-Check '18c leaving a two-page tab shows tab 1''s zoom' ((Get-Zoom) -eq $fit) "$(Get-Zoom) vs $fit"
    Send-Cmd 40034
    [void](Wait-Until { (Get-Zoom) -eq $dual } 3000)
    Add-Check '18d returning to the two-page tab shows the spread fit' ((Get-Zoom) -eq $dual) "$(Get-Zoom) vs $dual"
    $back = Send-AndRead 40062 $dual
    Add-Check '19 leaving two-page mode restores the single-page fit' ($back -eq $single) "$back vs $single"
    Add-Check '19a Two-Page Spread is unchecked again' (-not (Test-Checked 40062)) ''

    # --- 7. minimize / restore -----------------------------------------------
    [void][SbW]::ShowWindow($script:Main, 6); Start-Sleep -Milliseconds 700
    # A minimized window has a 0x0 client, the fit is exactly 0, and the
    # formatter returns the empty string for it -- not "0%".
    Add-Check '20 a minimized window shows an empty readout, not 0%' ((Get-Zoom) -eq '') ("'" + (Get-Zoom) + "'")
    [void][SbW]::ShowWindow($script:Main, 9)
    [void](Wait-Until { (Get-Zoom) -eq $single } 3000)
    Add-Check '21 restoring the window restores the readout' ((Get-Zoom) -eq $single) "$(Get-Zoom) vs $single"

    # --- 8. stale glyphs as the text shrinks (paint, not caption) -----------
    $wide = Get-Pixels $script:ZoomLabel
    $paintOk = $true; $steps = 0
    for ($i = 0; $i -lt 12 -and (Get-Zoom) -ne '25%'; $i++) {
        $before = Get-Zoom
        Send-Cmd 40011
        [void](Wait-Until { (Get-Zoom) -ne $before } 2000)
        Start-Sleep -Milliseconds 250
        $hot = Get-Pixels $script:ZoomLabel
        [void][SbW]::RedrawWindow($script:Bar, [IntPtr]::Zero, [IntPtr]::Zero, 0x0185)   # INVALIDATE|ERASE|ALLCHILDREN|UPDATENOW
        Start-Sleep -Milliseconds 250
        $clean = Get-Pixels $script:ZoomLabel
        if ($hot -ne $clean) { $paintOk = $false; Write-Host "  stale pixels after $before -> $(Get-Zoom)" }
        $steps++
    }
    Add-Check '22 reached 25% on the way down' ((Get-Zoom) -eq '25%') (Get-Zoom)
    Add-Check '23 no stale glyphs at any step down' $paintOk "$steps steps"
    Add-Check '24 CONTROL the capture sees the label change' ((Get-Pixels $script:ZoomLabel) -ne $wide) 'wide vs 25%'
    [void](Send-AndRead 40012 '25%')

    # --- 9. hide / show: layout and checkmarks ------------------------------
    $br = New-Object SbW+RECT; [void][SbW]::GetWindowRect($script:Bar, [ref]$br)
    $barH = $br.Bottom - $br.Top; $ch = Get-ClientH
    $preHide = Get-Zoom
    Add-Check '25 CONTROL shown bar: content ends above it' ((Get-ContentBottom) -eq ($ch - $barH)) "bottom=$(Get-ContentBottom) client=$ch bar=$barH"
    Add-Check '26 Status Bar is checked while shown' (Test-Checked 40064) ''
    Send-Cmd 40064; [void](Wait-Until { -not [SbW]::IsWindowVisible($script:Bar) } 2000); Start-Sleep -Milliseconds 300
    Add-Check '27 the toggle hides the bar' (-not [SbW]::IsWindowVisible($script:Bar)) ''
    Add-Check '28 hidden bar: content reaches the window bottom' ((Get-ContentBottom) -eq $ch) "bottom=$(Get-ContentBottom) client=$ch"
    Add-Check '29 Status Bar is unchecked while hidden' (-not (Test-Checked 40064)) ''
    Send-Cmd 40061; Start-Sleep -Milliseconds 400
    Add-Check '30 Invert Colors checkmark still works' ((Test-Checked 40061) -and (-not (Test-Checked 40064))) ''
    Send-Cmd 40061; Start-Sleep -Milliseconds 400
    Send-Cmd 40064; [void](Wait-Until { [SbW]::IsWindowVisible($script:Bar) } 2000); Start-Sleep -Milliseconds 300
    Add-Check '31 the toggle shows the bar again' ([SbW]::IsWindowVisible($script:Bar)) ''
    Add-Check '32 shown again: content ends above the bar' ((Get-ContentBottom) -eq ($ch - $barH)) "bottom=$(Get-ContentBottom)"
    Add-Check '33 the readout is unchanged by the round trip' ((Get-Zoom) -eq $preHide) "$(Get-Zoom) vs $preHide"
    # The readout keeps being written while the bar is hidden.
    Send-Cmd 40064; [void](Wait-Until { -not [SbW]::IsWindowVisible($script:Bar) } 2000)
    Send-Cmd 40010; Start-Sleep -Milliseconds 600
    Send-Cmd 40064; [void](Wait-Until { [SbW]::IsWindowVisible($script:Bar) } 2000); Start-Sleep -Milliseconds 300
    Add-Check '33a a zoom made while hidden shows when the bar returns' (((Get-Zoom) -match $pct) -and ((Get-Zoom) -ne $preHide)) "$preHide -> $(Get-Zoom)"
    [void](Send-AndRead 40012 (Get-Zoom))

    # --- 10. results panel + hidden bar --------------------------------------
    Send-Cmd 40045; Start-Sleep -Milliseconds 600
    $panel = [IntPtr]::Zero
    foreach ($h in [SbW]::Kids($script:Main)) { if ([SbW]::Cls($h) -eq 'LitePDFResultsPanel') { $panel = $h } }
    $panelUp = ([int64]$panel -ne 0) -and [SbW]::IsWindowVisible($panel)
    Add-Check '34 CONTROL the results panel is open and ends above the shown bar' ($panelUp -and ((Get-RectIn $panel).Bottom -eq ($ch - $barH))) "open=$panelUp bottom=$(Get-ContentBottom)"
    Send-Cmd 40064; Start-Sleep -Milliseconds 500
    Add-Check '35 results panel reaches the bottom when the bar is hidden' ((Get-ContentBottom) -eq $ch) "bottom=$(Get-ContentBottom)"
    Send-Cmd 40064; Start-Sleep -Milliseconds 500
    Send-Cmd 40046; Start-Sleep -Milliseconds 500   # F6: hide the results panel

    # --- 11. focus handback when hiding --------------------------------------
    $pt = [IntPtr]((5 -shl 16) -bor 5)
    [void][SbW]::PostMessageW($script:Edit, 0x0201, [IntPtr]1, $pt)
    [void][SbW]::PostMessageW($script:Edit, 0x0202, [IntPtr]0, $pt)
    [void](Wait-Until { [SbW]::Focus($script:Main) -eq $script:Edit } 2000)
    Add-Check '36 CONTROL the page box takes the focus' ([SbW]::Focus($script:Main) -eq $script:Edit) ''
    Send-Cmd 40064; [void](Wait-Until { -not [SbW]::IsWindowVisible($script:Bar) } 2000); Start-Sleep -Milliseconds 300
    Add-Check '37 hiding the bar hands the focus to the canvas' ([SbW]::Focus($script:Main) -eq $script:Canvas) ('focus=0x{0:X}' -f [int64][SbW]::Focus($script:Main))
    Send-Cmd 40064; Start-Sleep -Milliseconds 500

    # --- 12. no document ------------------------------------------------------
    # Tab 2 is active. Closing it leaves tab 1, whose readout is the fit value;
    # that is deliberately the last text shown before the bar goes empty.
    Send-Cmd 40030
    [void](Wait-Until { (Get-Zoom) -eq $fit } 3000)
    Add-Check '37a closing tab 2 shows tab 1''s zoom' ((Get-Zoom) -eq $fit) "$(Get-Zoom) vs $fit"
    Send-Cmd 40030; Start-Sleep -Milliseconds 700
    Add-Check '38 closing the last tab clears the readout' ((Get-Zoom) -eq '') ("'" + (Get-Zoom) + "'")
    Send-Cmd 40064; [void](Wait-Until { -not [SbW]::IsWindowVisible($script:Bar) } 2000)
    Add-Check '39 the toggle works with no document' ((-not [SbW]::IsWindowVisible($script:Bar)) -and (-not (Test-Checked 40064))) ''
    Add-Check '39a no document: Invert and Two-Page are unchecked' ((-not (Test-Checked 40061)) -and (-not (Test-Checked 40062))) ''
    Send-Cmd 40064; [void](Wait-Until { [SbW]::IsWindowVisible($script:Bar) } 2000)
    Add-Check '40 and shows the bar again with no document' ([SbW]::IsWindowVisible($script:Bar) -and (Test-Checked 40064)) ''
    # Reopen the same document at the same window size: the readout is the same
    # text it showed last. If set_empty() forgot to clear the remembered text,
    # set_zoom() would see "unchanged" and leave the label blank.
    Start-Process $Exe -ArgumentList ('"{0}"' -f $simple) | Out-Null
    [void](Wait-Until { (Get-Zoom) -eq $fit } 6000)
    Add-Check '41 reopening a document after the empty state shows its zoom' ((Get-Zoom) -eq $fit) "'$(Get-Zoom)' vs $fit"
} finally {
    if (-not $proc.HasExited) {
        [void][SbW]::PostMessageW($script:Main, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
        if (-not $proc.WaitForExit(5000)) { $proc.Kill() }
    }
    if ($hadSession) { Copy-Item $backup $session -Force } elseif (Test-Path $session) { Remove-Item $session -Force }
}

$failed = @($script:Results | Where-Object { $_ -like 'FAIL*' })
Write-Host ''
Write-Host ("{0}: {1} checks, {2} failed" -f $Mode, $script:Results.Count, $failed.Count)
if ($failed.Count -gt 0) { exit 1 }
```

- [ ] **Step 3: Run the normal exe**

```powershell
powershell -ExecutionPolicy Bypass -File C:\Users\User\projects\litepdf\build\gui-check\status-bar.ps1 -Exe C:\Users\User\projects\litepdf\build\gui-check\normal-probe.exe -Mode normal
```

Expected: the last line reads `normal: 52 checks, 0 failed`.

Two things that look like product failures and are not. Checks named "changes the readout" compare text: if a fit happens to round to the ladder rung the next step lands on (a fit of 124.6% shows `125%`, and Zoom In then goes to 125%), the text does not change. Resize the window by a few pixels in the driver's `SetWindowPos` width and re-run. And check 03 renders `simple.pdf` at 800%, which on a 200% display is a very large bitmap (#49); if the app is slow to answer there, raise the waits rather than dropping the check.

If a `CONTROL` check fails, the run is invalid, not a product failure: fix the driver or the environment (monitor asleep, another window on top, a leftover instance) and re-run. If any other check fails, stop and use superpowers:systematic-debugging; do not edit the check to make it pass.

- [ ] **Step 4: Run the mutant exe**

```powershell
powershell -ExecutionPolicy Bypass -File C:\Users\User\projects\litepdf\build\gui-check\status-bar.ps1 -Exe C:\Users\User\projects\litepdf\build\gui-check\site1off-probe.exe -Mode site1off
```

Expected: `site1off: 52 checks, 0 failed`, with checks 06 and 13 reading `MUTANT ... stale`.

What this proves: with the canvas callback gone, every `kick_render` path (02-05, 07-11b, 14-21) still updates the readout, in single-page and two-page mode, so call site 2 is present on both branches. Checks 06 and 13 going stale show that those two paths depend on call site 1 — which the normal run's 06 and 13 show is working.

If mutant check 06 or 13 reports the readout **changed**, the mutant was not built correctly (the callback is still wired) or another path refreshes the readout that the spec does not know about. Investigate before continuing.

- [ ] **Step 5: The toggle re-fits the page (FitPage, through session restore)**

The driver above never leaves FitWidth, and FitWidth ignores the canvas height, so nothing in it fails if `on_toggle_status_bar` forgets its `kick_render`. FitPage does depend on the height, and the only way into FitPage is a restored session. This script writes a FitPage session, lets the app restore it, and toggles the bar.

Save as `build/gui-check/fitpage.ps1`:

```powershell
#Requires -Version 5.1
# #59: hiding the status bar must re-fit a FitPage document.
[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$Exe)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
Add-Type @'
using System; using System.Collections.Generic; using System.Runtime.InteropServices; using System.Text;
public static class FpW {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")] static extern IntPtr SendText(IntPtr h, uint m, IntPtr w, StringBuilder l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  public static uint Pid(IntPtr h) { uint pid; GetWindowThreadProcessId(h, out pid); return pid; }
  public static string Readout(IntPtr main) {
    string found = "";
    EnumChildWindows(main, (h, l) => {
      var c = new StringBuilder(64); GetClassNameW(h, c, 64);
      if (c.ToString() == "Static") {
        var t = new StringBuilder(256); SendText(h, 0x000D, (IntPtr)256, t);
        if (t.ToString().EndsWith("%")) found = t.ToString();
      }
      return true; }, IntPtr.Zero);
    return found;
  }
}
'@
$repo   = 'C:\Users\User\projects\litepdf'
$simple = Join-Path $repo 'tests\fixtures\simple.pdf'
if (Get-Process | Where-Object { $_.Name -eq 'litepdf' -or $_.Name -like '*-probe' }) { throw 'A LitePDF instance is already running. Close it first.' }
$dir     = Join-Path $env:LOCALAPPDATA 'LitePDF'
$session = Join-Path $dir 'session.json'
$marker  = Join-Path $dir 'running.lock'
$bakS = Join-Path $env:TEMP 'litepdf-session-fitpage.bak'
$bakM = Join-Path $env:TEMP 'litepdf-marker-fitpage.bak'
$hadS = Test-Path $session; $hadM = Test-Path $marker
if ($hadS) { Copy-Item $session $bakS -Force }
if ($hadM) { Copy-Item $marker $bakM -Force }
New-Item -ItemType Directory -Force $dir | Out-Null
$jsonPath = $simple.Replace('\', '\\')
$json = '{"version":2,"window":{"flags":0,"show":1,"x":0,"y":0,"w":0,"h":0},"active":0,"tabs":[{"path":"' + $jsonPath + '","page":0,"zoom_mode":"fit_page","zoom_scale":1}]}'
[System.IO.File]::WriteAllText($session, $json, (New-Object System.Text.UTF8Encoding($false)))
if (-not $hadM) { [System.IO.File]::WriteAllText($marker, '') }   # the abnormal-exit marker makes the app offer a restore
Remove-Item Env:\LITEPDF_NO_RESTORE -ErrorAction SilentlyContinue
function Wait-For([scriptblock]$Cond, [int]$Ms) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $Ms) { if (& $Cond) { return $true }; Start-Sleep -Milliseconds 100 }
    return $false
}
$fail = 0
function Say([string]$Name, [bool]$Ok, [string]$Detail) {
    $tag = 'FAIL'; if ($Ok) { $tag = 'PASS' } else { $script:fail++ }
    Write-Host "$tag  $Name  [$Detail]"
}
$proc = Start-Process $Exe -PassThru
try {
    # The restore prompt is a MessageBox titled "LitePDF" owned by this process.
    $dlg = [IntPtr]::Zero
    $gotDlg = Wait-For { $script:dlg = [FpW]::FindWindowW('#32770', 'LitePDF'); ([int64]$script:dlg -ne 0) -and ([FpW]::Pid($script:dlg) -eq [uint32]$proc.Id) } 8000
    Say 'F1 CONTROL the restore prompt appeared' $gotDlg ''
    if (-not $gotDlg) { throw 'no restore prompt: the marker or session.json was not accepted - invalid run' }
    [void][FpW]::PostMessageW($dlg, 0x0111, [IntPtr]6, [IntPtr]::Zero)   # IDYES
    $main = [IntPtr]::Zero
    [void](Wait-For { $proc.Refresh(); $script:main = $proc.MainWindowHandle; ([int64]$script:main -ne 0) -and ([FpW]::Readout($script:main) -match '^\d+%$') } 8000)
    $shown = [FpW]::Readout($main)
    Say 'F2 CONTROL the restored document shows a percentage' ($shown -match '^\d+%$') $shown
    [void][FpW]::PostMessageW($main, 0x0111, [IntPtr]40064, [IntPtr]::Zero)
    [void](Wait-For { [FpW]::Readout($script:main) -ne $shown } 3000)
    $hidden = [FpW]::Readout($main)
    # A portrait page in the default window is limited by height in FitPage, so
    # a taller canvas means a larger fit. The label is still written while the
    # bar is hidden.
    Say 'F3 hiding the bar re-fits the page (FitPage percentage grows)' (($hidden -match '^\d+%$') -and ([int]$hidden.TrimEnd('%') -gt [int]$shown.TrimEnd('%'))) "$shown -> $hidden"
    [void][FpW]::PostMessageW($main, 0x0111, [IntPtr]40064, [IntPtr]::Zero)
    [void](Wait-For { [FpW]::Readout($script:main) -eq $shown } 3000)
    Say 'F4 showing the bar restores the original fit' ([FpW]::Readout($main) -eq $shown) "$([FpW]::Readout($main)) vs $shown"
} finally {
    if (-not $proc.HasExited) {
        $proc.Refresh()
        if ([int64]$proc.MainWindowHandle -ne 0) { [void][FpW]::PostMessageW($proc.MainWindowHandle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) }
        if (-not $proc.WaitForExit(5000)) { $proc.Kill(); [void]$proc.WaitForExit(5000) }
    }
    if ($hadS) { Copy-Item $bakS $session -Force } elseif (Test-Path $session) { Remove-Item $session -Force }
    if ($hadM) { Copy-Item $bakM $marker -Force } elseif (Test-Path $marker) { Remove-Item $marker -Force }
}
Write-Host "fitpage: $fail failed"
if ($fail -gt 0) { exit 1 }
```

Run it:

```powershell
powershell -ExecutionPolicy Bypass -File C:\Users\User\projects\litepdf\build\gui-check\fitpage.ps1 -Exe C:\Users\User\projects\litepdf\build\gui-check\normal-probe.exe
```

Expected: four `PASS` lines and `fitpage: 0 failed`.

If F1 fails, the run is invalid, not a product failure: read `maybe_offer_restore` in `src/ui/MainWindow.cpp` and the marker path in `src/app/AppPaths.cpp` to see what the app needs before it offers a restore, fix the script, and re-run. If F3 reports the percentage unchanged, `on_toggle_status_bar` is not re-rendering.

- [ ] **Step 6: Confirm the tree is clean**

```bash
cd /c/Users/User/projects/litepdf && git status --short
```

Expected: no output. `build/` is ignored; nothing from this task is committed.

- [ ] **Step 7: Record the result**

Write the three summary lines (`normal: ...`, `site1off: ...`, `fitpage: ...`) and any deviation from the expected output to `build/gui-check/results.txt`, and include them in your report. Not covered by these scripts, and why:

- Session restore of a Custom zoom updating the readout: restore goes through `kick_render`, the path checks 02-21 exercise. It is on the user's list in Task 7.
- `page_box_has_focus()` returning false while hidden, the skipped `set_bounds`, and `set_zoom` skipping an unchanged text: none is observable from outside once check 37 passes. Pinned by review.
- Drag-resize flicker, the font after a DPI change, the repaint after a theme switch, a DPI change while hidden: these need a person or an OS setting change. They are Task 7's checklist.
- The results splitter's drag clamp with the bar hidden: it reads `height_px()`, which is unchanged code fed a value check 28 already proves is 0.
- The callback's position ahead of the null check: no fixture produces a failed render (`corrupt.pdf` fails at open). Pinned by the comment at the call site and by review.

---

## Task 7: Docs, user-assisted checks, final verification

**Files:**
- Modify: `CHANGELOG.md` (the `## [Unreleased]` → `### Added` list)
- Modify: `README.md:45`

**Interfaces:**
- Consumes: the finished feature. Task 6's results are in `build/gui-check/results.txt` (untracked); this task does not need them.
- Produces: the branch ready for the PR gate.

- [ ] **Step 1: CHANGELOG**

In `CHANGELOG.md`, under `## [Unreleased]` → `### Added`, add at the top of the list:

```markdown
- Zoom readout (#57). The status bar shows the current zoom as a percentage,
  next to the page count. It follows every zoom change: the View menu, Ctrl+wheel,
  resizing the window, opening a side pane, and switching tabs.
- View → Status Bar hides or shows the status bar (#59). The setting applies to
  the window and is not remembered: the bar is shown again the next time
  LitePDF starts. While it is hidden there is no page box to type a page into.
```

- [ ] **Step 2: README**

In `README.md`, change the line that starts `- **Page indicator**` (line 45) to:

```markdown
- **Page indicator** — status bar showing the current page and page count, with a box you can type a page into (v1.3.0). It also shows the current zoom percentage, and View → Status Bar hides it (unreleased)
```

The `(unreleased)` suffix is the convention the neighbouring lines use for features after v1.3.0.

- [ ] **Step 3: Full verification**

```powershell
$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ctest = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
& $cmake --build C:\Users\User\projects\litepdf\build --config Release
Push-Location C:\Users\User\projects\litepdf
& $ctest --test-dir build -C Release
Pop-Location
powershell -ExecutionPolicy Bypass -File C:\Users\User\projects\litepdf\scripts\smoke-test.ps1 -ExpectDev
```

Expected: the build succeeds; `100% tests passed, 0 tests failed out of 404`; the smoke test exits 0.

- [ ] **Step 4: Commit**

```bash
git add CHANGELOG.md README.md
git commit -m "docs: zoom readout and the status bar toggle (#57, #59)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

- [ ] **Step 5: Hand the user the checks only a person can do**

These change OS settings or need eyes on the screen. Ask the user to run them on `build\Release\litepdf.exe` with any PDF open, and record each answer. Do not perform the OS changes yourself.

1. **Drag-resize.** In the default FitWidth mode, drag the window's right edge back and forth for a few seconds. The readout should change continuously; the status bar should not flicker visibly.
2. **DPI change with the bar shown.** Change the display scale (Settings → System → Display → Scale), or drag the window to a monitor at a different scale. The zoom readout should use the same font and size as the `/ N` label next to it.
3. **DPI change with the bar hidden.** View → Status Bar to hide it, change the display scale, and check there is no blank strip under the page. Then View → Status Bar again: the bar returns at the right height with all three items readable.
4. **Theme switch.** Switch Windows between light and dark app mode (Settings → Personalization → Colors). The zoom readout should repaint in the new colours along with the rest of the bar.
5. **Session restore.** With a document zoomed to a value other than the fit, end LitePDF from Task Manager, start it again and accept the restore prompt. The readout should show the restored zoom.

A failure in any of them goes back through superpowers:systematic-debugging before the PR gate.
