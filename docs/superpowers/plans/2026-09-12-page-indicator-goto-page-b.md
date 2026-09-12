# PR-B — Page Indicator and Go-To-Page Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a status bar at the bottom of the main window showing the current page and total page count, with an editable box that jumps to a typed page.

**Architecture:** A new `ui::StatusBar` component (PIMPL, same shape as `ui::Splitter` / `ui::FindBar`) wraps a `msctls_statusbar32` hosting one `EDIT` and one `STATIC`. All decision logic — input parsing, child geometry, and the "may I overwrite the box?" rule — lives in a header-only pure-function file `ui/detail/StatusBarMath.hpp` so it is unit-testable without a window. `MainWindow` owns the bar, reserves its height in `on_layout`, feeds it from the existing `set_on_page_changed` observer, and routes a committed page through the already-hardened `MainWindow::navigate_click` path.

**Tech Stack:** C++20, Win32 / Common Controls v6, Direct2D (existing canvas), Catch2 v3.5.4, CMake + MSVC v143.

**Source spec:** `docs/superpowers/specs/2026-09-07-zoom-correctness-page-nav-design.md` §4 (with §5 "B unit tests", §6 risks, §7 out of scope).

---

## Spec deviations — read before writing any code

The spec was written on 2026-09-07 against `main` @ `4c2cefe`. PR-A1 (`8bf143d`) and PR-A2 (`cdd9312`) have landed since. Every claim in §4 was re-verified against `main` @ `cdd9312` while writing this plan. Four corrections:

**D1 — `change_current_page` takes ONE argument.** Spec §4.3 prescribes `change_current_page(idx, PageAnchor::top())`. The real declaration is `bool PdfCanvas::change_current_page(int idx)` (`src/ui/PdfCanvas.hpp`), and the two-argument form was **deliberately rejected** during PR-A2 after it produced three defects. `set_pending_anchor()` is the only anchor installer. A reviewer who cites spec §4.3 against this is citing something two gates have already refuted.

Further: this plan does not call `change_current_page` at all. It calls **`MainWindow::navigate_click(page)`**, which is the same entry point outline clicks and thumbnail clicks use and which already does the four things a go-to-page needs, in the order PR-A2 proved correct: canonicalise to the spread's left page, compute `view_moves` *before* the page changes, install a `Top` anchor only when the view actually moves, and `kick_render`. Reimplementing that sequence inside the status bar would reintroduce a stranded-anchor bug PR-A2 already paid for.

**D2 — every `file:line` in spec §4 is stale.** `on_layout` is not at `:306`, the `INITCOMMONCONTROLSEX` block is not at `:1957-1959`, `set_on_page_changed` is not at `:1024`. This plan cites **symbols, not line numbers** — that is the lesson PR #43 paid for twice (two of its gate findings were stale citations). Do not "fix" this plan by adding line numbers.

**D3 — there is no deliberate wheel-blocking code in FindBar/ResultsPanel.** Spec §4.3 says their edits "deliberately do not" forward `WM_MOUSEWHEEL`. In fact neither file handles `WM_MOUSEWHEEL` at all — the behaviour is an absence, not a mechanism. Nothing there needs changing; do not go looking for a flag to flip.

**D4 — `set_current_page` clamps silently.** `DocumentView::set_current_page` does `std::clamp(idx, 0, page_count-1)`. So an out-of-range page would *jump to the first or last page* rather than being refused. This makes `parse_page_input`'s range rejection **load-bearing, not decorative** — it is the only thing standing between a pasted `9999` and a silent jump to the last page.

**Verified true and unchanged from the spec:** `PdfCanvas::set_view(nullptr)` returns from its null branch *before* firing `on_page_changed`, so the empty state is genuinely not on the observer path; `on_tab_switch` really does have an `else` branch that calls `canvas_->set_pan(0.0f, 0.0f)`, and that is where the empty state gets cleared; the results-panel rect really does use the full client height as its bottom; the splitter drag clamp really is a separate third site.

---

## Plan-gate findings, folded in

Full tier (dim ④ native/build chain — this PR edits both `CMakeLists.txt` files), not high-stakes. Four lenses ran on 2026-09-12: Opus + Sonnet in round 1, then Codex `terra@high` and `luna@max` sequentially against the post-fix baseline. Sonnet returned zero. Everything below is already resolved in the text; it is recorded because two of the findings changed what this PR touches.

**Round 1 (Opus)** — one Critical, three Important:

- **C1 — ESC never reaches the page box.** ESC is a bare accelerator (`{ FVIRTKEY, VK_ESCAPE, IDM_FIND_CLOSE }`) and the pump calls `TranslateAcceleratorW` before dispatch, so the draft's `case VK_ESCAPE:` in the edit subclass was dead code and the `WM_GETDLGCODE`/`DLGC_WANTALLKEYS` line copied from FindBar does nothing about it (`DLGC_*` only matters to `IsDialogMessage`, which this pump never calls). **Consequence for scope: PR-B now modifies the shipped `IDM_FIND_CLOSE` arm** — Task 5 Step 2. Note that FindBar's own `VK_ESCAPE` branch is dead for the same reason today; the draft inherited a dead branch by copying a pattern without checking whether it fires.
- **I1 — a disabled EDIT's background query arrives as `WM_CTLCOLORSTATIC`,** not `WM_CTLCOLOREDIT`. The draft returned `NULL_BRUSH` from that message for the label's sake, which would also have left the page box unpainted — and the box is disabled from the constructor onward until a document opens. The arm now discriminates by HWND.
- **I2 — nothing reverted uncommitted text when focus left by any route other than Enter,** and the draft's public `revert()` had zero callers. `WM_KILLFOCUS` is now the single revert point and `revert()` is gone from the interface.
- **I3 — spec §5's GUI checks for B name session restore and dual-mode End**, and the draft's acceptance list had neither. Both are now in Task 4 Step 3.

Two of round 1's UNVERIFIED QUESTIONS were cheap enough to close by construction rather than by experiment: `EnableWindow(FALSE)` on a focused box (focus is handed back first, in `set_empty`) and a `GetDpiForWindow` of 0 (guarded, matching FindBar).

**Lens 3 (Codex)** — the third question, the transparent label, came back as a finding:

- **`ctest -R statusbar` selects zero tests** (`luna@max`). `catch_discover_tests` registers each test under its `TEST_CASE` **name**; the Catch tag becomes a CTest *label*, which `-R` does not search. Every `TEST_CASE` is now prefixed `StatusBarMath`, matching the `PageAnchor …` / `ScrollMath …` convention already in the suite, and Task 1 Step 4 verifies the filter selects 13. A verification step that verifies nothing is the defect this project's ctest memo exists to prevent, and the first draft walked straight into it.
- **The shrinking label** (`terra@high`, and round 1's open question). The claim as filed — "the plan neither invalidates nor repaints that rectangle through the status-bar parent" — was **false**: both `set_page` and `set_empty` already invalidated the bar with `fErase`. But the uncertainty underneath is real, so `Impl::repaint()` now invalidates **both** windows, documents why neither call may be deleted, names the failure mode, and names the one-line fallback; Task 4 Step 3 checks the `/ 128` → `/ 2` → blank transition explicitly.
- **ESC does not close the results panel** (`luna@max`) — real, and **pre-existing**, not introduced here. Recorded under "Out of scope"; not fixed, because it is a shipped-behaviour change to a panel this PR does not otherwise touch.
- One finding was **refuted**: `terra@high` claimed a committed page reverts the box to the old page, having traced `set_page`'s deliberate refusal to overwrite a focused box mid-typing but stopped one line short — `commit()` calls `force_revert()` *after* `on_goto()`, by which point `cur_page` is the new page. Its falsifier pointed back at its own anchor, which is the low-discrimination shape the report contract warns about.

**What the ESC finding is really about, and it is not ESC.** The draft copied FindBar's edit-subclass pattern including its `case VK_ESCAPE`, without checking whether that branch fires in the original. It does not — it has been dead in FindBar since it was written. **Copying a pattern imports its dead code along with its live code, and "the neighbouring file does it this way" is evidence about style, never about behaviour.** Three of the six review passes over this artifact missed it.

---

## Global Constraints

- **Catch2 `TEST_CASE` names must be ASCII.** Non-ASCII names mangle under `catch_discover_tests` on Windows and fail CI only.
- **Verify through `ctest --test-dir build -C Release`**, not only by running the test executable directly.
- **Tests build Release, never Debug.** MuPDF's static libs are `MT_StaticRelease`; Debug fails with `LNK2038`.
- **`VERSION` is NOT bumped by this PR.** Bumps happen at phase boundaries only. `VERSION` stays `1.2.0`; the About-dialog literal in `MainWindow.cpp` stays in sync by not moving.
- **Binary size gate:** `build/Release/litepdf.exe` must stay under **19,000,000 bytes** (absolute CI gate) and under 25 MB (smoke gate). Baseline on `main` @ `cdd9312` is **7,265,792 bytes**.
- **All artifacts in English** — code, comments, commit messages, PR title/body, CHANGELOG.
- **UI is 1-based, internals are 0-based.** The conversion happens in exactly one place: `parse_page_input` on the way in, `set_page` on the way out.
- **CHANGELOG** gets a `### Added` bullet under `## [Unreleased]`; no new section, no version heading.
- Commit messages end with `Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>`.

---

## File Structure

| File | Status | Responsibility |
|---|---|---|
| `src/ui/detail/StatusBarMath.hpp` | create | Pure functions: page-input parsing, child geometry, box-overwrite rule. No `<windows.h>` dependency. |
| `tests/unit/test_status_bar_math.cpp` | create | Catch2 coverage for all three pure functions. |
| `tests/CMakeLists.txt` | modify | Register the new test source. |
| `src/ui/StatusBar.hpp` | create | Public interface of the component (PIMPL, callbacks). |
| `src/ui/StatusBar.cpp` | create | Win32: control creation, fonts, DPI, edit subclass, painting glue. |
| `CMakeLists.txt` | modify | Add `src/ui/StatusBar.cpp` to the `litepdf` executable sources. |
| `src/ui/MainWindow.hpp` | modify | `std::unique_ptr<StatusBar> status_bar_;` member. |
| `src/ui/MainWindow.cpp` | modify | `ICC_BAR_CLASSES`; construction; three layout subtractions; observer hook; empty-state clear; go-to-page wiring; the `IDM_FIND_CLOSE` ESC arm. |
| `CHANGELOG.md` | modify | One `### Added` bullet under `[Unreleased]`. |

---

## Task 1: Pure status-bar logic

**Files:**
- Create: `src/ui/detail/StatusBarMath.hpp`
- Create: `tests/unit/test_status_bar_math.cpp`
- Modify: `tests/CMakeLists.txt` (add the new source to the `target_sources(litepdf_unit_tests PRIVATE ...)` list, after the `unit/test_scroll_math.cpp   # PR-A2 Task 7` line)

**Interfaces:**
- Consumes: nothing.
- Produces, all in namespace `litepdf::ui::detail`:
  - `std::optional<int> parse_page_input(std::wstring_view text, int page_count) noexcept` — returns a **0-based** page index, or `nullopt` when the text is not a valid 1-based page for a document of `page_count` pages.
  - `struct StatusBarChildRects { int edit_x, edit_y, edit_w, edit_h; int label_x, label_y, label_w, label_h; };`
  - `StatusBarChildRects status_bar_child_rects(int bar_h, int pad_px, int edit_w_px, int label_w_px) noexcept`
  - `bool should_overwrite_page_box(bool box_has_focus, std::wstring_view current_text, std::wstring_view last_written) noexcept`

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_status_bar_math.cpp`:

```cpp
// PR-B Task 1: pure status-bar logic -- page-input parsing, child geometry,
// and the rule that decides whether a page change may overwrite the box.
#include <catch2/catch_test_macros.hpp>

#include "ui/detail/StatusBarMath.hpp"

using litepdf::ui::detail::parse_page_input;
using litepdf::ui::detail::should_overwrite_page_box;
using litepdf::ui::detail::status_bar_child_rects;

TEST_CASE("StatusBarMath parse_page_input accepts an in-range 1-based page", "[statusbar]") {
    REQUIRE(parse_page_input(L"1", 128)   == 0);
    REQUIRE(parse_page_input(L"12", 128)  == 11);
    REQUIRE(parse_page_input(L"128", 128) == 127);
}

TEST_CASE("StatusBarMath parse_page_input rejects out of range", "[statusbar]") {
    // DocumentView::set_current_page CLAMPS, so an accepted 9999 would jump
    // silently to the last page. Rejection here is the only guard.
    REQUIRE_FALSE(parse_page_input(L"129", 128).has_value());
    REQUIRE_FALSE(parse_page_input(L"0", 128).has_value());
}

TEST_CASE("StatusBarMath parse_page_input rejects empty and whitespace only", "[statusbar]") {
    REQUIRE_FALSE(parse_page_input(L"", 128).has_value());
    REQUIRE_FALSE(parse_page_input(L"   ", 128).has_value());
    REQUIRE_FALSE(parse_page_input(L"\t", 128).has_value());
}

TEST_CASE("StatusBarMath parse_page_input rejects non numeric", "[statusbar]") {
    // ES_NUMBER blocks non-digits from the KEYBOARD but not from a paste.
    REQUIRE_FALSE(parse_page_input(L"abc", 128).has_value());
    REQUIRE_FALSE(parse_page_input(L"1a", 128).has_value());
    REQUIRE_FALSE(parse_page_input(L"-5", 128).has_value());
    REQUIRE_FALSE(parse_page_input(L"1.5", 128).has_value());
    REQUIRE_FALSE(parse_page_input(L"1 2", 128).has_value());
}

TEST_CASE("StatusBarMath parse_page_input tolerates surrounding whitespace", "[statusbar]") {
    REQUIRE(parse_page_input(L"  7  ", 128) == 6);
    REQUIRE(parse_page_input(L"\t7", 128)   == 6);
}

TEST_CASE("StatusBarMath parse_page_input tolerates leading zeros", "[statusbar]") {
    REQUIRE(parse_page_input(L"007", 128) == 6);
}

TEST_CASE("StatusBarMath parse_page_input does not overflow on a long digit string",
          "[statusbar]") {
    // Must be REJECTED as out of range, not wrapped into range.
    REQUIRE_FALSE(parse_page_input(L"99999999999999999999", 128).has_value());
    REQUIRE_FALSE(parse_page_input(L"4294967297", 128).has_value());
}

TEST_CASE("StatusBarMath parse_page_input rejects everything when there is no document",
          "[statusbar]") {
    REQUIRE_FALSE(parse_page_input(L"1", 0).has_value());
    REQUIRE_FALSE(parse_page_input(L"1", -1).has_value());
}

TEST_CASE("StatusBarMath status_bar_child_rects centers children and lays them left to right",
          "[statusbar]") {
    const auto r = status_bar_child_rects(/*bar_h=*/24, /*pad_px=*/4,
                                          /*edit_w_px=*/48, /*label_w_px=*/72);
    REQUIRE(r.edit_h  == 16);
    REQUIRE(r.edit_y  == 4);
    REQUIRE(r.edit_x  == 4);
    REQUIRE(r.edit_w  == 48);
    REQUIRE(r.label_x == 4 + 48 + 4);
    REQUIRE(r.label_y == r.edit_y);
    REQUIRE(r.label_h == r.edit_h);
    REQUIRE(r.label_w == 72);
}

TEST_CASE("StatusBarMath status_bar_child_rects degrades safely on a tiny bar",
          "[statusbar]") {
    // A bar shorter than twice the padding must not produce a negative height.
    const auto r = status_bar_child_rects(/*bar_h=*/4, /*pad_px=*/4,
                                          /*edit_w_px=*/48, /*label_w_px=*/72);
    REQUIRE(r.edit_h >= 0);
    REQUIRE(r.edit_y >= 0);
}

TEST_CASE("StatusBarMath should_overwrite_page_box allows overwrite when unfocused",
          "[statusbar]") {
    REQUIRE(should_overwrite_page_box(false, L"anything", L"7"));
}

TEST_CASE("StatusBarMath should_overwrite_page_box allows overwrite of untouched text",
          "[statusbar]") {
    REQUIRE(should_overwrite_page_box(true, L"7", L"7"));
}

TEST_CASE("StatusBarMath should_overwrite_page_box refuses to clobber typing",
          "[statusbar]") {
    // A wheel flip while the reader is mid-keystroke must not eat the digits.
    REQUIRE_FALSE(should_overwrite_page_box(true, L"12", L"7"));
    REQUIRE_FALSE(should_overwrite_page_box(true, L"", L"7"));
}
```

Register it in `tests/CMakeLists.txt`:

```cmake
    unit/test_scroll_math.cpp          # PR-A2 Task 7
    unit/test_status_bar_math.cpp      # PR-B Task 1
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build build --config Release --target litepdf_unit_tests
```

Expected: FAIL at compile time — `Cannot open include file: 'ui/detail/StatusBarMath.hpp'`.

- [ ] **Step 3: Write the implementation**

Create `src/ui/detail/StatusBarMath.hpp`:

```cpp
#pragma once

// PR-B: pure logic behind ui::StatusBar -- input parsing, child geometry and
// the box-overwrite rule. Deliberately free of <windows.h> so the whole
// decision surface of the status bar is unit-testable without a window, the
// same split ScrollMath/CompletionMath/SplitterMath use.

#include <optional>
#include <string_view>

namespace litepdf::ui::detail {

// Parse the go-to-page box into a ZERO-BASED page index.
//
// UI is 1-based, internals are 0-based, and this is the single place the
// conversion happens.
//
// Rejects: empty or whitespace-only text, any non-digit character, 0, anything
// above page_count, and any digit string long enough to overflow. The range
// check is NOT belt-and-braces: DocumentView::set_current_page CLAMPS its
// argument, so an accepted out-of-range page would silently jump to the first
// or last page instead of being refused. ES_NUMBER stops non-digits from the
// keyboard but not from a paste, so the character check is equally real.
inline std::optional<int> parse_page_input(std::wstring_view text,
                                           int page_count) noexcept {
    if (page_count <= 0) return std::nullopt;

    // Trim ASCII spaces and tabs; a paste can carry them on either end.
    while (!text.empty() && (text.front() == L' ' || text.front() == L'\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == L' ' || text.back() == L'\t')) {
        text.remove_suffix(1);
    }
    if (text.empty()) return std::nullopt;

    long long value = 0;
    for (const wchar_t c : text) {
        if (c < L'0' || c > L'9') return std::nullopt;
        value = value * 10 + static_cast<long long>(c - L'0');
        // Bail the moment the accumulator passes the document, which doubles as
        // the overflow guard: no input can drive `value` past page_count and
        // then wrap back into range, because the loop stops at the crossing.
        if (value > page_count) return std::nullopt;
    }
    if (value < 1) return std::nullopt;
    return static_cast<int>(value) - 1;
}

// Pixel geometry of the two children inside the bar's client rect.
struct StatusBarChildRects {
    int edit_x = 0, edit_y = 0, edit_w = 0, edit_h = 0;
    int label_x = 0, label_y = 0, label_w = 0, label_h = 0;
};

// Lay the page box and the "/ N" label out left to right with a uniform
// padding, both vertically centred in a bar `bar_h` pixels tall. Callers pass
// pixel values already scaled for DPI, so this stays pure arithmetic.
inline StatusBarChildRects status_bar_child_rects(int bar_h, int pad_px,
                                                  int edit_w_px,
                                                  int label_w_px) noexcept {
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
    return r;
}

// May an incoming page change rewrite the text in the box?
//
// The indicator tracks every page transition, including ones the reader causes
// with the wheel WHILE the box has focus (the box forwards WM_MOUSEWHEEL to the
// canvas, so this is reachable, not theoretical). Rewriting the box then would
// eat digits mid-keystroke. So: always safe when the box is not focused; safe
// when focused only if the text is still exactly what the bar itself last wrote,
// i.e. the reader has not started editing.
inline bool should_overwrite_page_box(bool box_has_focus,
                                      std::wstring_view current_text,
                                      std::wstring_view last_written) noexcept {
    if (!box_has_focus) return true;
    return current_text == last_written;
}

}  // namespace litepdf::ui::detail
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build build --config Release --target litepdf_unit_tests && ctest --test-dir build -C Release --output-on-failure -R StatusBarMath
```

Expected: all 13 cases PASS **and `ctest` lists them by name** — proving the discovery path works, not just the exe.

**Why the names are prefixed `StatusBarMath`, and why the filter is not `-R statusbar`.** `catch_discover_tests` registers each CTest test under the **`TEST_CASE` name string**; the Catch tag becomes a CTest *label*, which `-R` does not search. So `-R statusbar` would match zero tests — and `ctest` exits non-zero on an empty selection, so the step would fail rather than silently pass, but either way it would verify nothing. Existing suites in this repo already carry the prefix for exactly this reason (`PageAnchor …`, `ScrollMath …`, `ThumbnailModel: …`). Confirm with:

```bash
ctest --test-dir build -C Release -N -R StatusBarMath
```

Expected: 13 tests listed. Zero means the names drifted from the filter.

- [ ] **Step 5: Commit**

```bash
git add src/ui/detail/StatusBarMath.hpp tests/unit/test_status_bar_math.cpp tests/CMakeLists.txt
git commit -m "feat(ui): add pure status-bar logic - page parsing, geometry, overwrite rule

Header-only pure functions behind the PR-B status bar so the whole decision
surface is unit-testable without a window. The range check in
parse_page_input is load-bearing: DocumentView::set_current_page clamps, so
an accepted out-of-range page would jump silently to the last page.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 2: The StatusBar component

**Files:**
- Create: `src/ui/StatusBar.hpp`
- Create: `src/ui/StatusBar.cpp`
- Modify: `CMakeLists.txt` — add `src/ui/StatusBar.cpp   # PR-B Task 2` to the `add_executable(litepdf WIN32 ...)` source list, next to `src/ui/Splitter.cpp`

**Interfaces:**
- Consumes: `litepdf::ui::detail::parse_page_input`, `status_bar_child_rects`, `should_overwrite_page_box` from Task 1.
- Produces: class `litepdf::ui::StatusBar` with `hwnd()`, `height_px()`, `set_bounds(const RECT&)`, `update_dpi(UINT)`, `set_page(int page_index, int page_count)`, `set_empty()`, `page_box_has_focus()`, `set_on_goto(OnGoto)`, `set_on_focus_out(OnFocusOut)`, `set_on_wheel(OnWheel)`.

Nothing wires this up yet — Task 2 ends with a component that compiles and links but is not constructed. That is deliberate: a reviewer can reject the component's interface without rejecting the layout surgery in Task 3.

- [ ] **Step 1: Write the header**

Create `src/ui/StatusBar.hpp`:

```cpp
#pragma once

// ui::StatusBar -- PR-B: a msctls_statusbar32 docked at the bottom of
// MainWindow, hosting the page indicator and the go-to-page input.
//
// Single part, children at fixed offsets -- no SB_SETPARTS. MainWindow owns
// positioning: it asks for height_px() in on_layout, reserves that much at the
// bottom of the client area, and calls set_bounds().
//
// The control is sent WM_SIZE in exactly two places -- construction and
// update_dpi -- and only to make it compute its own themed natural height for
// the current font, which is then read back and handed to MainWindow. Every
// other position change goes through set_bounds(), so the control never
// self-docks behind the layout's back.

#include <functional>
#include <memory>

#include <windows.h>

namespace litepdf::ui {

class StatusBar {
public:
    // A page was committed in the box. The argument is a ZERO-BASED index --
    // the 1-based UI value is converted inside the bar (detail::parse_page_input).
    using OnGoto = std::function<void(int page_index)>;
    // The box is done with the keyboard: Esc, or any commit (valid or not).
    // The owner returns focus to the canvas.
    using OnFocusOut = std::function<void()>;
    // WM_MOUSEWHEEL arrived while the box had focus. WM_MOUSEWHEEL goes to the
    // FOCUSED window, so without forwarding, the wheel would be dead whenever
    // the reader had clicked into the page box.
    using OnWheel = std::function<void(WPARAM, LPARAM)>;

    StatusBar(HINSTANCE hInstance, HWND parent);
    ~StatusBar();

    StatusBar(const StatusBar&)            = delete;
    StatusBar& operator=(const StatusBar&) = delete;

    // Forward-declared PUBLICLY, not privately: the two subclass procedures in
    // StatusBar.cpp are free functions and receive an Impl* as their ref_data,
    // so they have to be able to name the type. Same reason FindBar.hpp
    // declares FindBar::Impl in its public section. Impl stays opaque here.
    struct Impl;

    HWND hwnd() const;

    // Natural height at the current DPI, in pixels. Measured from the control
    // itself at construction and re-measured by update_dpi().
    int height_px() const;

    // Position the bar in parent-client coordinates and re-lay the children.
    void set_bounds(const RECT& bounds);

    // Rebuild the font and re-measure after a DPI change.
    void update_dpi(UINT dpi);

    // Show a live document: `page_index` is 0-based, the box shows page_index+1.
    // Honours detail::should_overwrite_page_box, so it will not clobber digits
    // the reader is in the middle of typing.
    void set_page(int page_index, int page_count);

    // No document (last tab closed): clear both children and disable the box.
    void set_empty();

    // True while the page box holds the keyboard focus.
    //
    // MainWindow needs this because ESC is a BARE ACCELERATOR in this app
    // (`{ FVIRTKEY, VK_ESCAPE, IDM_FIND_CLOSE }`), and TranslateAcceleratorW
    // runs before the message is dispatched to any child -- so ESC never
    // reaches this control's WM_KEYDOWN and the status bar cannot claim it on
    // its own. The IDM_FIND_CLOSE arm in Task 5 asks this question instead.
    bool page_box_has_focus() const;

    void set_on_goto(OnGoto cb);
    void set_on_focus_out(OnFocusOut cb);
    void set_on_wheel(OnWheel cb);

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace litepdf::ui
```

- [ ] **Step 2: Write the implementation**

Create `src/ui/StatusBar.cpp`:

```cpp
// LitePDF -- ui::StatusBar: bottom status bar with page indicator + go-to-page.
#include "ui/StatusBar.hpp"

#include "ui/detail/StatusBarMath.hpp"

#include <commctrl.h>

#include <string>
#include <type_traits>
#include <utility>

#pragma comment(lib, "comctl32.lib")

namespace litepdf::ui {

namespace {

using unique_hfont = std::unique_ptr<std::remove_pointer_t<HFONT>,
                                     decltype(&DeleteObject)>;

unique_hfont make_unique_hfont(HFONT h) {
    return unique_hfont(h, &DeleteObject);
}

HFONT create_status_font(UINT dpi, int pt_size = 9) {
    LOGFONTW lf = {};
    lf.lfHeight  = -MulDiv(pt_size, static_cast<int>(dpi), 72);
    lf.lfWeight  = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    return CreateFontIndirectW(&lf);
}

int dp(int dip, UINT dpi) {
    return MulDiv(dip, static_cast<int>(dpi), 96);
}

constexpr int kPadDip    = 4;
constexpr int kEditWDip  = 52;
constexpr int kLabelWDip = 96;

constexpr UINT_PTR kIdEdit         = 1;
constexpr UINT_PTR kIdLabel        = 2;
constexpr UINT_PTR kEditSubclassId = 1;
constexpr UINT_PTR kBarSubclassId  = 2;

// The box holds at most a sloppy paste; parse_page_input rejects anything
// longer on range grounds anyway.
constexpr int kEditTextMax = 15;

}  // namespace

LRESULT CALLBACK status_bar_edit_subclass(HWND hwnd, UINT msg, WPARAM w,
                                          LPARAM l, UINT_PTR id,
                                          DWORD_PTR ref_data);
LRESULT CALLBACK status_bar_subclass(HWND hwnd, UINT msg, WPARAM w,
                                     LPARAM l, UINT_PTR id, DWORD_PTR ref_data);

struct StatusBar::Impl {
    HWND hwnd  = nullptr;
    HWND edit  = nullptr;
    HWND label = nullptr;
    UINT dpi   = 96;
    int  height_px = 0;

    // 0-based current page and the document's page count. -1 / 0 means "no
    // document"; set_empty() restores that state.
    int  cur_page   = -1;
    int  page_count = 0;

    // The exact text the bar last wrote into the box. Compared against the live
    // text to tell "the reader is typing" from "nobody has touched it" --
    // see detail::should_overwrite_page_box.
    std::wstring last_written;

    unique_hfont font { nullptr, &DeleteObject };

    StatusBar::OnGoto     on_goto;
    StatusBar::OnFocusOut on_focus_out;
    StatusBar::OnWheel    on_wheel;

    std::wstring edit_text() const {
        if (!edit) return std::wstring();
        wchar_t buf[kEditTextMax + 1] = {};
        const int n = GetWindowTextW(edit, buf, kEditTextMax + 1);
        return std::wstring(buf, (n > 0) ? static_cast<std::size_t>(n) : 0u);
    }

    void write_box(const std::wstring& text) {
        if (!edit) return;
        SetWindowTextW(edit, text.c_str());
        last_written = text;
    }

    // Rewrite the box from cur_page, unconditionally.
    void force_revert() {
        if (cur_page < 0 || page_count <= 0) {
            write_box(L"");
            return;
        }
        write_box(std::to_wstring(cur_page + 1));
    }

    void relayout() {
        if (!hwnd) return;
        RECT rc;
        GetClientRect(hwnd, &rc);
        const auto r = detail::status_bar_child_rects(
            rc.bottom - rc.top, dp(kPadDip, dpi),
            dp(kEditWDip, dpi), dp(kLabelWDip, dpi));
        if (edit) {
            SetWindowPos(edit, nullptr, r.edit_x, r.edit_y, r.edit_w, r.edit_h,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        if (label) {
            SetWindowPos(label, nullptr, r.label_x, r.label_y,
                         r.label_w, r.label_h,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    // Repaint after any text change. BOTH windows must be invalidated, and
    // that is the whole reason this is a function rather than one line.
    //
    // The label erases nothing (NULL_BRUSH + TRANSPARENT bkmode), so the only
    // thing that can clear its old pixels is the BAR painting its background
    // across that strip -- which it can do only because the bar has no
    // WS_CLIPCHILDREN. Invalidating the bar alone is not obviously enough
    // either, since a parent's invalid region does not propagate to children.
    // Invalidate both and rely on the documented order: within one update
    // cycle a parent paints before its children.
    //
    // The case that fails if this is wrong is a SHRINKING label -- "/ 128"
    // becoming "/ 2", or going empty when the last tab closes, leaving "28" or
    // the whole old string behind. Task 4 Step 3 checks exactly that
    // transition; if stale glyphs survive it, the fallback is to give the
    // label an opaque background brush instead of transparency (one line in
    // status_bar_subclass), accepting a possible slight mismatch against a
    // themed bar. Do not "fix" it by deleting either Invalidate call.
    void repaint() {
        if (hwnd)  InvalidateRect(hwnd, nullptr, TRUE);
        if (label) InvalidateRect(label, nullptr, FALSE);
    }

    // Re-measure the control's natural height for the current font.
    void measure() {
        if (!hwnd) return;
        // A status bar computes its own height only when it PROCESSES WM_SIZE.
        // The window is created 0x0, so without this the GetWindowRect below
        // would read 0 every time and the fallback would be the only answer
        // this function ever gave. Sending it here also repositions the control
        // to the parent's bottom edge, which is harmless: on_layout calls
        // set_bounds() immediately afterwards and owns the position from then on.
        SendMessageW(hwnd, WM_SIZE, 0, 0);
        RECT wr;
        GetWindowRect(hwnd, &wr);
        const int measured = wr.bottom - wr.top;
        // Guard the degenerate case so on_layout can never reserve a negative
        // strip -- 22 DIP is the classic status bar height at 100%.
        height_px = (measured > 0) ? measured : dp(22, dpi);
    }

    // Commit whatever is in the box.
    void commit() {
        const auto parsed = detail::parse_page_input(edit_text(), page_count);
        if (parsed.has_value() && on_goto) {
            on_goto(*parsed);
        }
        // Normalise unconditionally: a successful goto has already updated
        // cur_page through the owner's page-change observer, and a goto to the
        // page we are already on changes nothing -- both end with the box
        // showing the canonical 1-based page rather than "007" or "129".
        force_revert();
        if (on_focus_out) on_focus_out();
    }
};

// -----------------------------------------------------------------------------
// Status bar subclass -- background brushes for the two children.
//
// The "/ N" STATIC wants to be transparent so it paints over the themed bar
// instead of a grey rectangle: NULL_BRUSH plus TRANSPARENT bkmode means it
// draws text and erases nothing, so the bar underneath must repaint first.
// set_page() invalidates the whole bar with fErase for that reason, and the bar
// is deliberately NOT created with WS_CLIPCHILDREN so the erase reaches under
// the label.
//
// The page box must NOT get that treatment, and this is the trap: Windows
// routes a DISABLED (or read-only) EDIT's background query to
// WM_CTLCOLORSTATIC, not to WM_CTLCOLOREDIT. The box is disabled from
// construction until a document opens -- set_empty() runs at the end of the
// constructor -- so a blanket NULL_BRUSH here would leave the box's client area
// unfilled on the very first frame, and again after Ctrl+W on the last tab.
// The two are told apart by HWND. FindBar handles the same pair with two real
// brushes (FindBar.cpp, WM_CTLCOLOREDIT / WM_CTLCOLORSTATIC) and returns a
// NULL_BRUSH from neither.
// -----------------------------------------------------------------------------
LRESULT CALLBACK status_bar_subclass(HWND hwnd, UINT msg, WPARAM w,
                                     LPARAM l, UINT_PTR /*id*/,
                                     DWORD_PTR ref_data) {
    auto* impl = reinterpret_cast<StatusBar::Impl*>(ref_data);

    switch (msg) {
        case WM_CTLCOLORSTATIC: {
            auto hdc = reinterpret_cast<HDC>(w);
            auto ctl = reinterpret_cast<HWND>(l);
            if (impl && ctl == impl->edit) {
                // Disabled page box: the standard disabled-field look.
                SetBkColor(hdc, GetSysColor(COLOR_3DFACE));
                return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_3DFACE));
            }
            SetBkMode(hdc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
        }
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, status_bar_subclass, kBarSubclassId);
            break;
    }
    return DefSubclassProc(hwnd, msg, w, l);
}

// -----------------------------------------------------------------------------
// Edit subclass -- Enter commits, Esc reverts, and the wheel is handed back to
// the owner. Everything else passes through so ordinary typing, selection and
// paste behave exactly like a plain EDIT.
//
// Pattern mirrors find_bar_edit_subclass in FindBar.cpp.
// -----------------------------------------------------------------------------
LRESULT CALLBACK status_bar_edit_subclass(HWND hwnd, UINT msg, WPARAM w,
                                          LPARAM l, UINT_PTR /*id*/,
                                          DWORD_PTR ref_data) {
    auto* impl = reinterpret_cast<StatusBar::Impl*>(ref_data);
    if (!impl) return DefSubclassProc(hwnd, msg, w, l);

    switch (msg) {
        case WM_GETDLGCODE:
            // Route Enter/Escape/characters here rather than to the dialog
            // manager, so WM_KEYDOWN below actually sees them.
            return DLGC_WANTALLKEYS | DLGC_WANTCHARS
                 | DefSubclassProc(hwnd, msg, w, l);

        case WM_KEYDOWN:
            // ESC is deliberately absent from this switch. It is a BARE
            // ACCELERATOR in this app, so TranslateAcceleratorW converts it to
            // WM_COMMAND(IDM_FIND_CLOSE) before the message is ever dispatched
            // to this control -- a `case VK_ESCAPE` here would be dead code
            // that reads as live. MainWindow's IDM_FIND_CLOSE arm owns ESC and
            // hands focus back to the canvas; WM_KILLFOCUS below then reverts.
            // (VK_RETURN is NOT in the accelerator table, so it does arrive.)
            if (w == VK_RETURN) {
                impl->commit();
                return 0;  // consumed
            }
            break;

        case WM_CHAR:
            // Swallow the character form so the EDIT does not MessageBeep about
            // a key it cannot handle. Only VK_RETURN can actually arrive here
            // today: the pump skips TranslateMessage entirely for a translated
            // accelerator, so ESC never becomes a WM_CHAR either. The ESC
            // disjunct is kept as a one-token guard against the accelerator
            // table changing, not as a claim that it fires.
            if (w == VK_RETURN || w == VK_ESCAPE) return 0;
            break;

        case WM_KILLFOCUS:
            // THE single place uncommitted text is discarded. Leaving the box
            // by any route -- clicking the canvas, Alt+Tab, ESC, a pane toggle,
            // the commit's own focus handback -- lands here. Without it the box
            // keeps showing a page the document is not on, and worse,
            // should_overwrite_page_box then refuses every later update forever,
            // because the live text no longer matches what the bar last wrote.
            // Idempotent: it rewrites from cur_page, which a completed commit
            // has already updated through the owner's page-change observer.
            impl->force_revert();
            break;  // fall through to the EDIT's own caret teardown

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

        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, status_bar_edit_subclass,
                                 kEditSubclassId);
            break;
    }
    return DefSubclassProc(hwnd, msg, w, l);
}

StatusBar::StatusBar(HINSTANCE hInstance, HWND parent)
    : impl_(std::make_unique<Impl>()) {
    impl_->dpi = GetDpiForWindow(parent);
    // Same guard FindBar carries in the identical situation: a 0 here would
    // propagate into dp() and make even measure()'s fallback height 0, which
    // would lay the bar out zero-tall.
    if (impl_->dpi == 0) impl_->dpi = 96;

    // No WS_CLIPCHILDREN: the bar must be able to erase the strip under the
    // label, which paints transparently (see status_bar_subclass).
    impl_->hwnd = CreateWindowExW(
        0, STATUSCLASSNAMEW, L"",
        WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0,
        parent, nullptr, hInstance, nullptr);
    if (!impl_->hwnd) return;

    // ref_data carries Impl* so the WM_CTLCOLORSTATIC arm can tell the page box
    // from the label by HWND. Registered before the children exist, which is
    // fine: impl_->edit is null until then and no colour query can name it.
    SetWindowSubclass(impl_->hwnd, status_bar_subclass, kBarSubclassId,
                      reinterpret_cast<DWORD_PTR>(impl_.get()));

    impl_->font = make_unique_hfont(create_status_font(impl_->dpi));
    SendMessageW(impl_->hwnd, WM_SETFONT,
                 reinterpret_cast<WPARAM>(impl_->font.get()),
                 MAKELPARAM(TRUE, 0));

    impl_->edit = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_RIGHT,
        0, 0, 0, 0,
        impl_->hwnd, reinterpret_cast<HMENU>(kIdEdit), hInstance, nullptr);
    SendMessageW(impl_->edit, WM_SETFONT,
                 reinterpret_cast<WPARAM>(impl_->font.get()),
                 MAKELPARAM(TRUE, 0));
    SendMessageW(impl_->edit, EM_SETLIMITTEXT,
                 static_cast<WPARAM>(kEditTextMax), 0);
    SetWindowSubclass(impl_->edit, status_bar_edit_subclass, kEditSubclassId,
                      reinterpret_cast<DWORD_PTR>(impl_.get()));

    impl_->label = CreateWindowExW(
        0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
        0, 0, 0, 0,
        impl_->hwnd, reinterpret_cast<HMENU>(kIdLabel), hInstance, nullptr);
    SendMessageW(impl_->label, WM_SETFONT,
                 reinterpret_cast<WPARAM>(impl_->font.get()),
                 MAKELPARAM(TRUE, 0));

    impl_->measure();
    set_empty();
}

StatusBar::~StatusBar() {
    if (impl_ && impl_->hwnd) DestroyWindow(impl_->hwnd);
}

HWND StatusBar::hwnd() const { return impl_ ? impl_->hwnd : nullptr; }

int StatusBar::height_px() const { return impl_ ? impl_->height_px : 0; }

void StatusBar::set_bounds(const RECT& bounds) {
    if (!impl_ || !impl_->hwnd) return;
    SetWindowPos(impl_->hwnd, nullptr,
                 bounds.left, bounds.top,
                 bounds.right - bounds.left, bounds.bottom - bounds.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    impl_->relayout();
}

void StatusBar::update_dpi(UINT dpi) {
    if (!impl_ || !impl_->hwnd) return;
    impl_->dpi  = dpi;
    impl_->font = make_unique_hfont(create_status_font(dpi));
    const WPARAM f = reinterpret_cast<WPARAM>(impl_->font.get());
    SendMessageW(impl_->hwnd, WM_SETFONT, f, MAKELPARAM(TRUE, 0));
    if (impl_->edit)  SendMessageW(impl_->edit,  WM_SETFONT, f, MAKELPARAM(TRUE, 0));
    if (impl_->label) SendMessageW(impl_->label, WM_SETFONT, f, MAKELPARAM(TRUE, 0));
    impl_->measure();
    impl_->relayout();
}

void StatusBar::set_page(int page_index, int page_count) {
    if (!impl_) return;
    impl_->cur_page   = page_index;
    impl_->page_count = page_count;

    if (impl_->edit) {
        EnableWindow(impl_->edit, page_count > 0);
        const bool focused = (GetFocus() == impl_->edit);
        if (detail::should_overwrite_page_box(focused, impl_->edit_text(),
                                              impl_->last_written)) {
            impl_->force_revert();
        }
    }
    if (impl_->label) {
        const std::wstring text = (page_count > 0)
            ? (L"/ " + std::to_wstring(page_count))
            : std::wstring();
        SetWindowTextW(impl_->label, text.c_str());
    }
    impl_->repaint();
}

void StatusBar::set_empty() {
    if (!impl_) return;
    impl_->cur_page   = -1;
    impl_->page_count = 0;
    if (impl_->edit) {
        // Hand focus back BEFORE disabling. Disabling a window that holds the
        // focus leaves the focus NULL, and MainWindow has no WM_KEYDOWN handler
        // of its own -- the keyboard would be dead until the reader clicked the
        // canvas. Reachable: caret in the box, then Ctrl+W (an accelerator, so
        // it fires regardless of focus) closing the last tab.
        if (GetFocus() == impl_->edit && impl_->on_focus_out) {
            impl_->on_focus_out();
        }
        impl_->write_box(L"");
        EnableWindow(impl_->edit, FALSE);
    }
    if (impl_->label) SetWindowTextW(impl_->label, L"");
    impl_->repaint();
}

bool StatusBar::page_box_has_focus() const {
    return impl_ && impl_->edit && GetFocus() == impl_->edit;
}

void StatusBar::set_on_goto(OnGoto cb) {
    if (impl_) impl_->on_goto = std::move(cb);
}

void StatusBar::set_on_focus_out(OnFocusOut cb) {
    if (impl_) impl_->on_focus_out = std::move(cb);
}

void StatusBar::set_on_wheel(OnWheel cb) {
    if (impl_) impl_->on_wheel = std::move(cb);
}

}  // namespace litepdf::ui
```

- [ ] **Step 3: Add to the build**

In `CMakeLists.txt`, inside `add_executable(litepdf WIN32 ...)`, after the `src/ui/Splitter.cpp` line:

```cmake
    src/ui/Splitter.cpp          # Phase 6 Task 12
    src/ui/StatusBar.cpp         # PR-B Task 2
```

- [ ] **Step 4: Build and verify it compiles and links**

```bash
cmake --build build --config Release
```

Expected: SUCCESS. `StatusBar.cpp` compiles; the linker does not complain about the unreferenced class (nothing constructs it yet).

- [ ] **Step 5: Commit**

```bash
git add src/ui/StatusBar.hpp src/ui/StatusBar.cpp CMakeLists.txt
git commit -m "feat(ui): add StatusBar component - page box, page-count label, subclasses

The bar is constructed by nobody yet; MainWindow wiring lands next. Enter
commits, Esc reverts, and the page box forwards WM_MOUSEWHEEL to its owner
because WM_MOUSEWHEEL is delivered to the FOCUSED window - without that the
wheel would be dead whenever the caret sat in the box.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 3: Reserve the bar in MainWindow's layout

**Files:**
- Modify: `src/ui/MainWindow.hpp` — new member + include
- Modify: `src/ui/MainWindow.cpp` — `ICC_BAR_CLASSES`, construction in `WM_CREATE`, three layout subtractions, DPI hook

**Interfaces:**
- Consumes: `litepdf::ui::StatusBar` from Task 2.
- Produces: `MainWindow::status_bar_` (a live, visible, empty bar) and a client area that no longer overlaps it.

**This is the task the spec singles out as the one that gets botched.** Three separate places subtract the bar's height, not one. Missing the second puts the results panel's last row under the bar; missing the third lets a splitter drag push the panel under it.

- [ ] **Step 1: Declare the member**

In `src/ui/MainWindow.hpp`, next to `std::unique_ptr<Splitter> splitter_;`:

```cpp
    std::unique_ptr<Splitter>     splitter_;
    // PR-B: bottom status bar (page indicator + go-to-page). Owns its own
    // height; on_layout reserves status_bar_->height_px() at the bottom.
    std::unique_ptr<StatusBar>    status_bar_;
```

And add the include to the alphabetical `ui/` block at the top of `MainWindow.hpp`, between `Splitter.hpp` and `TabManager.hpp`. It must be a full include, not a forward declaration — `std::unique_ptr<StatusBar>` needs the complete type where `MainWindow`'s destructor is instantiated, which is how every other pane in this header is already handled:

```cpp
#include "ui/Splitter.hpp"
#include "ui/StatusBar.hpp"
#include "ui/TabManager.hpp"
```

- [ ] **Step 2: Register the common-control class**

In `MainWindow.cpp`, find the `INITCOMMONCONTROLSEX icc = { sizeof(icc), ... }` initialiser and add `ICC_BAR_CLASSES`:

```cpp
    INITCOMMONCONTROLSEX icc = { sizeof(icc),
        ICC_STANDARD_CLASSES | ICC_TAB_CLASSES |
        ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES |
        ICC_BAR_CLASSES };   // PR-B: msctls_statusbar32
```

- [ ] **Step 3: Construct the bar in WM_CREATE**

In the `WM_CREATE` handler, immediately after the block that creates `splitter_`, add:

```cpp
            // PR-B: bottom status bar. Created before the first on_layout so
            // height_px() is already measured when the layout reserves for it.
            status_bar_ = std::make_unique<StatusBar>(cs->hInstance, hwnd);
```

- [ ] **Step 4: Subtract the height in all three places**

**(a)** In `MainWindow::on_layout()` — and only there: the line `const UINT dpi = GetDpiForWindow(hwnd_);` occurs **four times** in this file, so anchor on the function, not the line. The site is the one immediately preceded by `const int h = rc.bottom - rc.top;`. Insert after it:

```cpp
    // PR-B: the status bar owns the bottom strip. Everything laid out below
    // measures against `layout_h`, not the raw client height. THREE call sites
    // depend on this, not one -- see (b) and the splitter drag clamp.
    const int status_h = status_bar_ ? status_bar_->height_px() : 0;
    const int layout_h = std::max(0, h - status_h);
    if (status_bar_ && status_bar_->hwnd()) {
        RECT sb = { 0, layout_h, w, h };
        status_bar_->set_bounds(sb);
    }
```

**(b)** Still in `on_layout()`, change the two places that use the raw client height for vertical extent:

```cpp
    const int canvas_bottom = layout_h - panel_h - splitter_h;
```

and

```cpp
    if (results_panel_ && panel_h > 0) {
        RECT pr = { 0, canvas_bottom + splitter_h, w, layout_h };
        results_panel_->set_bounds(pr);
    }
```

**(c)** In the `splitter_->set_on_drag` lambda in `WM_CREATE`, the clamp must leave room for the bar as well as for the canvas:

```cpp
            splitter_->set_on_drag([this](int new_h) {
                // Clamp the dragged height so the splitter can't swallow
                // the canvas entirely — leave at least ~100 px of canvas
                // visible and refuse a panel shorter than ~80 px (below
                // which the ListView has no room for even a single row).
                // PR-B: the status bar's strip is not draggable space either,
                // so it comes off the budget before the 100 px canvas floor.
                RECT client; GetClientRect(hwnd_, &client);
                const int status_h = status_bar_ ? status_bar_->height_px() : 0;
                const int max_h = std::max(80,
                    static_cast<int>(client.bottom) - status_h - 100);
                results_panel_height_px_ = std::clamp(new_h, 80, max_h);
                on_layout();
            });
```

- [ ] **Step 5: Follow the DPI change**

In the `WM_DPICHANGED` handler, alongside the existing per-tab thumb-pane DPI loop, add:

```cpp
            // PR-B: the status bar's font and natural height are DPI-derived.
            if (status_bar_) status_bar_->update_dpi(HIWORD(w));
```

- [ ] **Step 6: Build and verify by eye**

```bash
cmake --build build --config Release
```

Then run it and check each of the following:

```bash
./build/Release/litepdf.exe tests/fixtures/large.pdf
```

Expected: an empty bar is visible along the bottom edge; the page canvas stops above it rather than running underneath; press F6 to show the results panel and confirm its last row is fully visible; drag the results splitter all the way down and confirm the panel never slides under the bar.

- [ ] **Step 7: Commit**

```bash
git add src/ui/MainWindow.hpp src/ui/MainWindow.cpp
git commit -m "feat(ui): reserve the status bar strip in MainWindow's layout

Three sites subtract the bar's height, not one: the canvas bottom, the
results-panel rect (whose bottom was the full client height), and the
results splitter's drag clamp. Missing either of the latter two puts the
panel's last row under the bar.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 4: Drive the indicator from the page-change observer

**Files:**
- Modify: `src/ui/MainWindow.cpp` — the `canvas_->set_on_page_changed` lambda, and `on_tab_switch`'s empty branch

**Interfaces:**
- Consumes: `StatusBar::set_page(int, int)` and `StatusBar::set_empty()` from Task 2; `MainWindow::active_view()`.
- Produces: an indicator that tracks every page transition and clears when the last tab closes.

**Why two sites, not one.** `PdfCanvas::set_view(nullptr)` returns from its null branch *before* firing `on_page_changed` — verified on `main` @ `cdd9312`. Closing the last tab is exactly a `set_view(nullptr)`, so an indicator driven only by the observer would show the last document's page forever after Ctrl+W on the final tab.

- [ ] **Step 1: Extend the observer**

In the `canvas_->set_on_page_changed([this](int page) { ... })` lambda in `WM_CREATE`, add before `schedule_session_save();`:

```cpp
                // PR-B: the page indicator. This observer covers every page
                // transition after PR-A2 -- PgDn/PgUp/Home/End, outline and
                // thumbnail clicks, search navigation, wheel flips, dual-page
                // snaps, session restore, and tab switches (set_view fires it
                // for a non-null view). The one case it does NOT cover is the
                // null view, handled in on_tab_switch's empty branch.
                if (status_bar_) {
                    if (auto* v = active_view()) {
                        status_bar_->set_page(page, v->page_count());
                    }
                }
```

- [ ] **Step 2: Clear the empty state**

In `MainWindow::on_tab_switch`, the branch that already handles "no incoming tab":

```cpp
    auto* incoming = tabs_->active_tab();
    if (canvas_) {
        canvas_->set_view(incoming ? incoming->view.get() : nullptr);
        if (incoming) canvas_->set_pan(incoming->pan_x, incoming->pan_y);
        else          canvas_->set_pan(0.0f, 0.0f);
```

becomes:

```cpp
    auto* incoming = tabs_->active_tab();
    if (canvas_) {
        canvas_->set_view(incoming ? incoming->view.get() : nullptr);
        if (incoming) canvas_->set_pan(incoming->pan_x, incoming->pan_y);
        else          canvas_->set_pan(0.0f, 0.0f);
        // PR-B: set_view's null branch returns before firing the page-change
        // observer, so the indicator would otherwise keep showing the closed
        // document's page. This is the only path to the empty state.
        if (!incoming && status_bar_) status_bar_->set_empty();
```

- [ ] **Step 3: Build and verify by eye**

```bash
cmake --build build --config Release
```

```bash
./build/Release/litepdf.exe tests/fixtures/large.pdf
```

Expected, each checked in turn: the box shows `1` and the label `/ N` on open; PgDn advances the number; Home and End jump it; an outline entry click (F5) moves it; a thumbnail click (F4) moves it; Ctrl+Shift+D spread mode shows the left page of the pair; wheel-flipping at a page edge moves it; opening a second tab and switching back and forth shows each tab's own page; Ctrl+W on the last tab blanks both the box and the label.

Two more, named explicitly by spec §5's GUI checks for B because they reach the observer by their own routes and would otherwise go unchecked:

- **Session restore.** Open a document, navigate to page 40, close the app, reopen it with no command-line argument so the session is restored. The box must show `40`, not `1`. (Restore path: `change_current_page(st.page)` in the tab-ready handler.)
- **Dual-mode End.** In spread mode (`Ctrl+Shift+D`), press End. The box must show the left page of the final spread, matching what is on screen — not the document's last page index when that page is the right half. (Path: `kick_render`'s spread branch canonicalises with `dual_page_compute_left` and routes through `change_current_page(left)`.)

And one that tests the paint path rather than the observer — **the shrinking label**, the failure mode `Impl::repaint`'s comment describes:

- Open a document with a 3-digit page count, then open a second tab holding a 1-page or 2-page document and switch to it. The label must read exactly `/ 2` — not `/ 128` with the tail overwritten, and not `/ 28`. Then Ctrl+W until the last tab closes: the label must go **completely** blank, leaving no glyphs behind. If either shows residue, apply the opaque-brush fallback named in `repaint`'s comment before continuing.

- [ ] **Step 4: Run the full suite**

```bash
ctest --test-dir build -C Release --output-on-failure
```

Expected: PASS, count ≥ 290 plus the new `[statusbar]` cases.

- [ ] **Step 5: Commit**

```bash
git add src/ui/MainWindow.cpp
git commit -m "feat(ui): track the current page in the status bar

Driven from the existing page-change observer, which after PR-A2 covers
every page transition including session restore and dual-page snaps. The
empty state is cleared separately: set_view(nullptr) returns before firing
the observer, so closing the last tab reaches no callback at all.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 5: Wire go-to-page

**Files:**
- Modify: `src/ui/MainWindow.cpp` — three `status_bar_->set_on_*` callbacks in `WM_CREATE`, plus the `IDM_FIND_CLOSE` arm in `WM_COMMAND`

**Interfaces:**
- Consumes: `StatusBar::set_on_goto` / `set_on_focus_out` / `set_on_wheel` / `page_box_has_focus` from Task 2; `MainWindow::navigate_click(int)`.
- Produces: a working go-to-page, and ESC scoped so the page box gets it when the find bar does not.

**The navigation call.** `navigate_click(page)` — **not** `change_current_page`. See deviation D1 at the top of this plan. `navigate_click` already canonicalises to the spread's left page, computes `view_moves` before the page changes, installs a `Top` anchor only when the view actually moves, and kicks the render. Every one of those four was a defect PR-A2 fixed; do not re-derive them here.

- [ ] **Step 1: Wire the three callbacks**

In `WM_CREATE`, immediately after `status_bar_ = std::make_unique<StatusBar>(...)` from Task 3:

```cpp
            // PR-B: go-to-page. navigate_click is the same entry point the
            // outline and thumbnail panes use -- it canonicalises to the
            // spread's left page, computes view_moves BEFORE the page changes,
            // installs a Top anchor only when the view actually moves, and
            // kicks the render. change_current_page takes ONE argument and
            // installs no anchor of its own; do not call it directly here.
            status_bar_->set_on_goto([this](int page_index) {
                navigate_click(page_index);
            });
            // Enter and Esc both hand the keyboard back to the canvas, so the
            // next PgDn or wheel notch goes where the reader expects.
            status_bar_->set_on_focus_out([this] {
                if (canvas_ && canvas_->hwnd()) SetFocus(canvas_->hwnd());
            });
            // WM_MOUSEWHEEL goes to the FOCUSED window: with the caret in the
            // page box the canvas would never see a notch.
            status_bar_->set_on_wheel([this](WPARAM w, LPARAM l) {
                if (canvas_ && canvas_->hwnd()) {
                    SendMessageW(canvas_->hwnd(), WM_MOUSEWHEEL, w, l);
                }
            });
```

- [ ] **Step 2: Give ESC back to the page box**

**Why this step exists — do not skip it as defensive coding.** ESC is a **bare accelerator** in this app: `{ FVIRTKEY, VK_ESCAPE, IDM_FIND_CLOSE }` sits in the table built in `MainWindow::run`, and the message pump calls `TranslateAcceleratorW(hwnd_, haccel_, &msg)` *before* `TranslateMessage`/`DispatchMessageW`. `TranslateAcceleratorW` does not care which child holds the focus — only that `msg.hwnd` is a descendant of `hwnd_`, which the page box is. So ESC becomes `WM_COMMAND(IDM_FIND_CLOSE)` and is delivered to MainWindow; **it never reaches any child's `WM_KEYDOWN`.** The existing arm returns 0 on both of its branches, so the keystroke is swallowed outright.

That is why Task 2's edit subclass has no `VK_ESCAPE` case, and why the `WM_GETDLGCODE` → `DLGC_WANTALLKEYS` line copied from FindBar does nothing for ESC here: `DLGC_*` only matters to `IsDialogMessage`, which this pump never calls.

In `WM_COMMAND`, extend the `IDM_FIND_CLOSE` arm:

```cpp
                case IDM_FIND_CLOSE:
                    // Scope ESC: claim it for whichever UI is active.
                    //
                    // NOTE: the comment that used to sit here said the
                    // non-find-bar path would "fall through to DefWindowProc
                    // so other consumers can see ESC as well". It never did --
                    // the arm returns 0 on every branch, and ESC is a bare
                    // accelerator, so no child window has ever received it.
                    // Corrected rather than preserved: PR-B's page box is the
                    // second consumer to be surprised by it.
                    if (find_bar_ && find_bar_->visible()) {
                        on_find_close();
                        return 0;
                    }
                    // PR-B: ESC is a bare accelerator, so it is intercepted
                    // here before any child sees it. This is the ONLY site
                    // that can hand it to the page box. Moving the focus is
                    // the whole action -- the box discards uncommitted text on
                    // WM_KILLFOCUS, which is its single revert point.
                    if (status_bar_ && status_bar_->page_box_has_focus()) {
                        if (canvas_ && canvas_->hwnd()) SetFocus(canvas_->hwnd());
                        return 0;
                    }
                    return 0;
```

- [ ] **Step 3: Build and verify by eye**

```bash
cmake --build build --config Release
```

```bash
./build/Release/litepdf.exe tests/fixtures/large.pdf
```

Expected, each checked in turn:
- Click the box, type a valid page, press Enter → the canvas jumps to that page, at the **top** of it, and the caret leaves the box (a following PgDn advances the page rather than typing into the box).
- Type a page above the count, press Enter → the view does **not** move and the box snaps back to the current page. No dialog, no beep. (This is the D4 case: without the parser's range check, `set_current_page` would clamp and jump to the last page.)
- Paste nonsense (`Ctrl+V` of `abc`), press Enter → same revert, no movement.
- Type `007` on a 128-page document, press Enter → jumps to page 7 and the box normalises to `7`.
- Type digits, press Esc → box reverts, caret returns to the canvas, view unmoved. **This is the Step 2 path; if Step 2 was skipped the box will keep the digits and the caret will stay put.**
- Type digits, then click the canvas without pressing anything → box reverts (the `WM_KILLFOCUS` path, which is a different route from Esc).
- Click into the box, then Ctrl+W until the last tab closes → the box empties, and the keyboard still works: PgDn after opening another document must move the page without a click on the canvas first.
- Click into the box and scroll the wheel → the page scrolls and flips as if the canvas had focus.
- Scroll the wheel with the caret in the box *after typing a digit* → the digits are **not** clobbered by the indicator update (`should_overwrite_page_box`).
- In spread mode (`Ctrl+Shift+D`), type a right-hand page number and press Enter → the spread containing it is shown.

- [ ] **Step 4: Run the full suite and check the size gate**

```bash
ctest --test-dir build -C Release --output-on-failure
```

```bash
ls -l build/Release/litepdf.exe
```

Expected: all tests PASS; `litepdf.exe` well under 19,000,000 bytes (baseline 7,265,792; the spec predicts a negligible delta for one status bar).

- [ ] **Step 5: Commit**

```bash
git add src/ui/MainWindow.cpp
git commit -m "feat(ui): go to a typed page from the status bar

Enter routes through navigate_click - the same path outline and thumbnail
clicks use - so the spread canonicalisation, the view_moves guard and the
Top anchor all come for free. Out-of-range input reverts rather than
navigating: DocumentView::set_current_page clamps, so an accepted 9999
would jump silently to the last page.

ESC is scoped in IDM_FIND_CLOSE rather than in the page box's own key
handler, because ESC is a bare accelerator: TranslateAcceleratorW converts
it before the message reaches any child, so a VK_ESCAPE case in the control
would be dead code.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 6: Documentation and final verification

**Files:**
- Modify: `CHANGELOG.md`

**Interfaces:**
- Consumes: everything above.
- Produces: a PR-ready branch.

- [ ] **Step 1: Add the CHANGELOG entry**

Under `## [Unreleased]` → `### Added`, after the mouse-wheel bullet:

```markdown
- A status bar with a page indicator and a go-to-page box. The box shows the
  current page and the document's page count, tracks every way of moving through
  the document, and jumps to a page typed into it. A page outside the document
  is refused and the box reverts — no dialog. Escape reverts and returns to the
  page; the mouse wheel keeps working while the box has focus.
```

**Do not bump `VERSION` and do not add a version heading.** Bumps happen at phase boundaries only.

- [ ] **Step 2: Verify the claim against the artifact, not the plan**

PR #43 shipped a CHANGELOG bullet that was false about what a previous release contained, and it was caught only by a reviewer who checked `git show`. Confirm the bullet above describes what the built exe actually does — run it and read the bullet next to the running window.

```bash
./build/Release/litepdf.exe tests/fixtures/large.pdf
```

- [ ] **Step 3: Full verification**

```bash
cmake --build build --config Release
```

```bash
ctest --test-dir build -C Release --output-on-failure
```

Expected: PASS. Record the exact test count and the exact `litepdf.exe` byte size — both go in the PR description.

- [ ] **Step 4: Commit**

```bash
git add CHANGELOG.md
git commit -m "docs(changelog): record the page indicator and go-to-page box

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

- [ ] **Step 5: Run the merge gate**

PR-B modifies shipped behaviour (spec §6.4), so it runs the **risk-tiered review stack** before merge — invoke the `risk-tiered-review` skill to classify the tier and run the lenses. Two things are non-negotiable from this project's history: **never drop the Codex lens** (`luna@max` was the only lens of five to find the real defect on each of the last two PRs, n=2), and **dispatch one lens specifically to attack whatever fix the gate produces**, naming the author's least-certain questions explicitly — that move caught a defect inside a defect fix on PR #43.

Two questions to name for that adversarial lens, because they are this plan's least-certain claims:

1. The label paints transparently over the themed bar and relies on the bar erasing underneath it (`status_bar_subclass` returns `NULL_BRUSH`; the bar is deliberately created without `WS_CLIPCHILDREN`). Does stale text ever survive a repaint — on a theme change, a DPI change mid-session, or a bar resize?
2. `should_overwrite_page_box` compares the live text against the last text the bar wrote. Is there a path where the bar writes text *while* the box has focus and the reader has typed, such that `last_written` goes stale and the box then refuses a legitimate update indefinitely?

---

## Out of scope (from spec §7, plus one addition)

True continuous scroll; Shift+wheel horizontal scrolling; a zoom-percentage readout in the bar; click-drag hand-tool panning; a toggle to hide the status bar.

**Added by this plan:** no `Ctrl+G` accelerator or View-menu item to focus the page box. Spec §4.3 specifies the box only, and adding a command means a new `IDM_` (next free is 40064), a menu entry, an accelerator and its own focus semantics. It is the obvious follow-up, and it is not this PR.

**Pre-existing defects, recorded and deliberately unfixed:**

1. `cancel_stale_renders(INT_MAX)` in `on_tab_switch` is a no-op (`priority > INT_MAX` is never true), so the tab-switch drain drains nothing. Spec §7 records it so that nothing here assumes a tab switch cancels in-flight renders.
2. **ESC does not close the results panel.** `ResultsPanel`'s edit subclass has `case VK_ESCAPE: if (impl->on_close) impl->on_close(); return 0;` — a clear intent that ESC closes the panel — and that branch is **dead today**, for the same reason PR-B's would have been: ESC is a bare accelerator consumed by `TranslateAcceleratorW` before any child sees it, and `IDM_FIND_CLOSE` returns 0 without forwarding. Found by the plan gate's `luna@max` lens. **Not fixed here:** it is a shipped-behaviour change to a panel PR-B does not otherwise touch, and spec §4 does not ask for it. The one-line shape of the fix is visible in Task 5 Step 2 — another `else if` on the same arm — so whoever picks it up has the pattern. The same is true of `FindBar`'s own `case VK_ESCAPE`, which is dead for the same reason but harmless, since the accelerator already produces the behaviour that branch intended.
