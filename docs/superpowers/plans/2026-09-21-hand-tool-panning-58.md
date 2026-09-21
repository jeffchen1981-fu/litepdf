# Hand-Tool Panning (#58) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the reader pan a zoomed-in page with the mouse — middle-drag, or left-drag while holding Space — without taking left-drag away from text selection.

**Architecture:** The pan model already exists (`PdfCanvas::pan_by`, `content_extent`, `clamp_pan`) and the gesture state machine already has an unused `Gesture::Panning` arm (`GestureState::begin_pan`, `ReleaseAction::EndPan`, merged with #52). This plan adds three pure pieces to `ui/detail/SelectionDrag.hpp` — incremental pan steps, a cursor-precedence function, and `ClickCounter::forget` so a pan press never pairs into a double click — plus `content_overflows` in `ViewportMath.hpp`, then wires them into `PdfCanvas`: a middle-button press/move/release path, a Space branch at the top of the left press, and a cursor that shows the move shape only when there is something to pan.

**Tech Stack:** C++17 (`cxx_std_17`, `cmake/CompilerFlags.cmake`), Win32, Direct2D, Catch2 v3.5.4, CMake + MSVC v143.

**Source spec:** `docs/superpowers/specs/2026-09-16-text-selection-copy-hand-tool-design.md` §5 (PR-2), with §4.2's message ordering and gesture exclusivity. That spec was approved and gated with #52; this plan re-verified §5 against `main` @ `b4bb162` and patches it in the same commit (see "Plan-time corrections").

## Global Constraints

- **Catch2 `TEST_CASE` names are ASCII and start with their subsystem** (`SelectionDrag …`, `ViewportMath …`). `catch_discover_tests` mangles non-ASCII names on Windows, and `ctest -R` matches the **name**, never the tag. Confirm every new filter with `ctest --test-dir build -C Release -N -R <filter>` and check the count before trusting a green run from it.
- **Tests build Release, never Debug** (MuPDF is `MT_StaticRelease`; Debug fails with `LNK2038`). Verify through `ctest --test-dir build -C Release`, not only by running the test exe.
- **Use the VS 2022 BuildTools `cmake` and `ctest`**, which configured `build/`: `"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"` and `ctest.exe` beside it. This plan writes `cmake` / `ctest` for brevity. **Do not trust a bare `cmake` on PATH:** in Git Bash on this machine it resolves a MinGW WinLibs build that did not configure `build/`.
- **Run tests from the repo root** — fixtures resolve relative to it.
- **`VERSION` is not bumped** (stays `1.3.0`); the About-dialog literal in `MainWindow.cpp` stays untouched. Bumps happen at phase boundaries only.
- **Binary size:** `build/Release/litepdf.exe` must stay under **19,000,000 bytes**. `main` @ `b4bb162` is 7,299,072 bytes. Record the baseline and the final size.
- **PIMPL discipline:** `PdfCanvas.hpp` stays free of `<mupdf/fitz.h>`. `SelectionDrag.hpp` and `ViewportMath.hpp` stay free of Win32, Direct2D and MuPDF — they are headless-tested.
- **Cite symbols, not line numbers**, in code comments and commit messages — line numbers go stale.
- **Never use `large.pdf` page 0 to judge rendering** — it overlaps its own text by design. The GUI checks use `simple.pdf`, whose single A4 page carries ~2,200 characters of text, dense enough that a pixel shift is measurable.
- **All artifacts in English.** Commit messages end with the implementing session's own `Co-Authored-By:` trailer; the trailers shown here are Opus 5's.
- **CHANGELOG** entries go under `## [Unreleased]`; no version heading.
- **Do not change** the #52 decisions recorded as not-to-reverse: `on_left_button_up` feeds its own lParam through `on_mouse_move` first; `release()` is called before `ReleaseCapture()`; one selection per tab; pointer thresholds use `GetSystemMetricsForDpi`.

## Branch and PR shape

One PR, branch `feat/hand-tool-pan` off `main` @ `b4bb162`. The branch already holds this plan and the spec patch. Closes #58.

## Plan-time corrections to the spec — read before writing any code

Every claim in spec §5 was re-checked against `main` @ `b4bb162`. `pan_by`, `content_extent`, `clamp_pan`, the arrow-key pans, `Gesture::Panning`, `GestureState::begin_pan` and `ReleaseAction::EndPan` all exist as §5 describes. Ten things the spec does not say, each decided here and patched into §5 (P10 was found at the plan gate):

**P1 — `WM_MBUTTONDBLCLK` must start a pan too.** #52 added `CS_DBLCLKS` to the canvas class, and that style applies to **every** button: the second of two quick middle presses arrives as `WM_MBUTTONDBLCLK`, never as `WM_MBUTTONDOWN`. Left out, every second quick middle-drag silently does nothing. The right-button arm gets `WM_RBUTTONDBLCLK` for the same reason.

**P2 — the pan cursor must be set when the pan starts.** Windows sends `WM_SETCURSOR` only while the mouse is *not* captured, so the handler #52 added never runs during a pan. Without an explicit `SetCursor` at the press, a middle-drag started over the page keeps the I-beam for its whole length. The release re-evaluates the cursor the same way. (Whether Windows also synthesises a mouse move after `ReleaseCapture`, which would bring a `WM_SETCURSOR` of its own, is not relied on either way: the explicit refresh makes the outcome the same.)

**P3 — the Space branch comes BEFORE the selection refusals.** `on_left_button_down` refuses in spread mode, without its own bitmap, and without a text handle. Those guard *selection*; a pan needs none of them, and `pan_by` already works in spread mode (it measures `content_extent`'s union of both slots).

**P4 — pan steps are incremental.** Each `WM_MOUSEMOVE` pans by the pointer motion since the previous one, applied to whatever the pan is *now*. An absolute offset from the press would make the content jump after anything that re-clamps the pan mid-drag (a render landing, a zoom), and would give a dead zone after overshooting an edge. Steps are integers in client pixels, so they sum exactly to the whole drag with no drift.

**P5 — a pan's button-up feeds its own coordinates through the move path.** This is #52's lesson (`on_left_button_up`): injected input delivers a press and a release with no move between, and a release that ignored its own position would pan by nothing. `on_middle_button_up` does the same; `on_left_button_up` already does.

**P6 — the move cursor means "something will pan", during a pan as on hover.** §5 gated only the *hover* cursor on overflow. Applying the same rule while panning costs nothing and keeps the cursor honest when the content fits. With Space held and nothing to pan the cursor is the **arrow**, not the I-beam: a Space press there would pan (a no-op), not select, so an I-beam would promise a selection it will not make. A live **selection** keeps the I-beam even if Space goes down mid-drag, because Space is read only at the press.

**P7 — Space down and up refresh the cursor.** §5's no-latch rule stands: the state is read with `GetKeyState(VK_SPACE)` when it is needed. But without a refresh the move cursor would appear only at the next pointer move after Space goes down. `WM_KEYDOWN` / `WM_KEYUP` for `VK_SPACE` re-run the cursor logic — no state is stored — and only when the pointer is over the canvas, because `SetCursor` changes the shape wherever the pointer is.

**P8 — a middle press does not take keyboard focus.** The left press calls `SetFocus` so PgUp/PgDn reach the canvas. Panning changes only the view; a query half-typed into the find box keeps the keyboard.

**P9 — a pan without the capture is stale, and a move cancels it.** #52's stale-gesture rule cancels at the next *press*. For a pan that is too late: its button is up, so every move over the canvas would drag the page under a pointer that is merely passing. `on_mouse_move` cancels a pan whose capture is gone instead of applying the step. **No GUI check can make P9 fire** — a driver cannot leave a pan live without the capture *and* without the `WM_CAPTURECHANGED` that ends it through its own arm — so P9 is covered by review. The GUI checks do catch it *misfiring*: every pan would stop at its first move.

**P10 — a Space press is not a click** (found at the plan gate). `on_left_button_down` feeds every press to `ClickCounter` before it decides anything, and Windows pairs presses into `WM_LBUTTONDBLCLK` on its own. Uncorrected, Space + a click followed quickly by a plain click at the same spot arrives as a double click and selects a word; and a plain click, then Space + a quick second press, then a plain click is counted as a triple click and selects a line. The Space branch calls `ClickCounter::forget()`, after which the next press starts a new sequence — a single click, even when Windows reports it as the second half of a double click.

## Known limitations (recorded, not fixed)

| | |
|---|---|
| R15 | A page change or a layout toggle during a pan (PgDn, the wheel at an edge, Ctrl+Shift+D) ends the pan; the button must be pressed again. The existing `cancel_gesture` calls in `change_current_page` and `set_dual_page` are gesture-agnostic, and a pan commits nothing, so ending it is safe. |
| R16 | Holding Space while the find box (or another edit control) has the focus types spaces into it until the left press moves focus to the canvas; the cursor refresh on Space runs only when the canvas has the focus. |
| R17 | A right-button press during a pan is ignored and the pan continues. §4.2's second-button abort applies to a selection drag, which a stray press would otherwise corrupt; a pan has nothing to corrupt. |
| R18 | When a pan is *cancelled* (capture lost, tab switched, page changed) the canvas does not re-evaluate the cursor; the move shape can stay until the next `WM_SETCURSOR`, at the latest the next pointer move. Only a normal release refreshes it explicitly. |

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `src/ui/detail/SelectionDrag.hpp` | modify | `PanStep`, `GestureState::pan_step`, `CanvasCursor`, `canvas_cursor`, `ClickCounter::forget` — pure |
| `src/ui/detail/ViewportMath.hpp` | modify | `content_overflows` — pure, next to the `clamp_pan` rule it must agree with |
| `tests/unit/test_selection_drag.cpp` | modify | pan-step, click-sequence and cursor-precedence cases |
| `tests/unit/test_viewport_math.cpp` | modify | `content_overflows` case |
| `src/ui/PdfCanvas.hpp` | modify | include `SelectionDrag.hpp`; declare the middle-button handlers, `begin_pan_gesture`, `cancel_stale_gesture`, `can_pan`, `refresh_cursor` |
| `src/ui/PdfCanvas.cpp` | modify | the handlers, the pan branch of `on_mouse_move`, the cursor, the message arms |
| `CHANGELOG.md`, `README.md` | modify | the feature, the shortcut rows |

No new files; `tests/CMakeLists.txt` already compiles both test files.

## GUI verification

Tasks 2 and 3 each end with GUI checks run through a scratch PowerShell driver, `build/gui/pan-drive.ps1`. **The driver is written out in full inside each of those two tasks**, not here: each task's brief is extracted on its own (`task-brief` copies nothing above `## Task 1`), so a shared copy in this header would reach neither implementer. The two copies must stay identical.

---

## Task 1: Pure pan logic — pan steps, overflow, cursor precedence

**Files:**
- Modify: `src/ui/detail/SelectionDrag.hpp`
- Modify: `src/ui/detail/ViewportMath.hpp`
- Test: `tests/unit/test_selection_drag.cpp`
- Test: `tests/unit/test_viewport_math.cpp`

**Interfaces:**
- Consumes: `GestureState`, `Gesture`, `MouseButton`, `ReleaseAction` (already in `SelectionDrag.hpp`); `clamp_pan` (in `ViewportMath.hpp`).
- Produces (used by Tasks 2 and 3):
  - `struct litepdf::ui::PanStep { int dx_px = 0; int dy_px = 0; };`
  - `PanStep GestureState::pan_step(int x_px, int y_px) noexcept` — pointer motion since the previous step (the first step measures from the `begin_pan` point); `{0, 0}` unless a pan is live.
  - `enum class litepdf::ui::CanvasCursor { Arrow, IBeam, Move };`
  - `CanvasCursor litepdf::ui::canvas_cursor(Gesture live, bool space_held, bool can_pan, bool over_page) noexcept`
  - `bool litepdf::ui::content_overflows(float content_w, float content_h, float vp_w, float vp_h) noexcept`
  - `void ClickCounter::forget() noexcept` — the press just counted was not a click; the next press starts a new sequence, even when it arrives as `WM_LBUTTONDBLCLK`.

- [ ] **Step 1: Baseline**

```bash
git switch feat/hand-tool-pan
git log --oneline main..HEAD        # expect only the docs commit(s) holding this plan
cmake --build build --config Release
ctest --test-dir build -C Release
```

Expected: build clean, all tests pass. Record the passing count as **N0** (340 at `b4bb162`, but re-measure — do not quote) and `(Get-Item build\Release\litepdf.exe).Length` as the baseline size.

- [ ] **Step 2: Write the failing tests — `test_selection_drag.cpp`**

Add to the `using` block at the top of `tests/unit/test_selection_drag.cpp`:

```cpp
using litepdf::ui::canvas_cursor;
using litepdf::ui::CanvasCursor;
using litepdf::ui::PanStep;
```

Append at the end of the file:

```cpp
// --- #58 hand-tool panning -------------------------------------------------

TEST_CASE("SelectionDrag a pan step is the pointer motion since the previous step",
          "[ui][pan]") {
    GestureState g;
    REQUIRE(g.begin_pan(MouseButton::Middle, 100, 200));

    PanStep s = g.pan_step(130, 190);   // the first step measures from the press
    REQUIRE(s.dx_px == 30);
    REQUIRE(s.dy_px == -10);

    s = g.pan_step(125, 250);           // later steps from the previous step
    REQUIRE(s.dx_px == -5);
    REQUIRE(s.dy_px == 60);

    s = g.pan_step(125, 250);           // no motion, no pan
    REQUIRE(s.dx_px == 0);
    REQUIRE(s.dy_px == 0);
}

TEST_CASE("SelectionDrag pan steps add up to the whole drag with no drift",
          "[ui][pan]") {
    // PdfCanvas applies each step to the pan as it is NOW, so the steps must sum
    // to (end - press) exactly, whatever path the pointer took -- including
    // outside the client area, where captured coordinates go negative.
    GestureState g;
    REQUIRE(g.begin_pan(MouseButton::Left, 50, 50));
    int sum_x = 0, sum_y = 0;
    const int path[][2] = { {60, 40}, {-30, 400}, {-1, -1}, {3000, -2000}, {90, 75} };
    for (const auto& p : path) {
        const PanStep s = g.pan_step(p[0], p[1]);
        sum_x += s.dx_px;
        sum_y += s.dy_px;
    }
    REQUIRE(sum_x == 90 - 50);
    REQUIRE(sum_y == 75 - 50);
}

TEST_CASE("SelectionDrag pan steps are zero unless a pan is live", "[ui][pan]") {
    GestureState g;
    PanStep s = g.pan_step(10, 10);                 // nothing live
    REQUIRE(s.dx_px == 0);
    REQUIRE(s.dy_px == 0);

    REQUIRE(g.begin_select(SelectMode::Chars, 0, 0));
    s = g.pan_step(40, 40);                         // a SELECTION is live
    REQUIRE(s.dx_px == 0);
    REQUIRE(s.dy_px == 0);
    REQUIRE(g.abort() == Gesture::Selecting);

    REQUIRE(g.begin_pan(MouseButton::Middle, 0, 0));
    (void)g.pan_step(500, 500);
    REQUIRE(g.release(MouseButton::Middle) == ReleaseAction::EndPan);
    s = g.pan_step(600, 600);                       // the pan is over
    REQUIRE(s.dx_px == 0);
    REQUIRE(s.dy_px == 0);

    // A new pan measures from ITS press, not from where the last one ended.
    REQUIRE(g.begin_pan(MouseButton::Middle, 10, 10));
    s = g.pan_step(12, 13);
    REQUIRE(s.dx_px == 2);
    REQUIRE(s.dy_px == 3);
}

TEST_CASE("SelectionDrag a space pan belongs to the left button and never clears a selection",
          "[ui][pan]") {
    GestureState g;
    // A plain click first, so mode() is Chars and moved() is false -- the exact
    // state in which a LEFT release means "clear the selection".
    REQUIRE(g.begin_select(SelectMode::Chars, 5, 5));
    REQUIRE(g.release(MouseButton::Left) == ReleaseAction::ClearSelection);

    // Space + click with no movement: the same button, the same stale mode, and
    // it must still end as a pan. PdfCanvas's EndPan arm commits and clears
    // nothing, so a Space click keeps the reader's selection.
    REQUIRE(g.begin_pan(MouseButton::Left, 5, 5));
    REQUIRE_FALSE(g.begin_pan(MouseButton::Middle, 5, 5));      // one gesture at a time
    REQUIRE(g.release(MouseButton::Middle) == ReleaseAction::None);
    REQUIRE(g.gesture() == Gesture::Panning);
    REQUIRE(g.release(MouseButton::Left) == ReleaseAction::EndPan);
}

TEST_CASE("SelectionDrag a press that became a pan starts a new click sequence",
          "[ui][pan]") {
    const PointerMetrics m;

    // A real click, then Space + a quick second press, which Windows reports as
    // a double click and the canvas turns into a pan. A plain click right after
    // must be a single click, not the third of a triple (a whole line).
    ClickCounter a;
    REQUIRE(a.press(false, 1000, 100, 100, m) == SelectMode::Chars);
    (void)a.press(true, 1050, 100, 100, m);
    a.forget();
    REQUIRE(a.press(false, 1100, 100, 100, m) == SelectMode::Chars);

    // Space + a press (a pan), then a quick plain press that Windows reports as
    // a double click. Its first half was the pan, so it is a single click, not
    // a word.
    ClickCounter b;
    (void)b.press(false, 2000, 50, 50, m);
    b.forget();
    REQUIRE(b.press(true, 2100, 50, 50, m) == SelectMode::Chars);

    // Counting is normal again afterwards.
    REQUIRE(b.press(false, 3000, 50, 50, m) == SelectMode::Chars);
    REQUIRE(b.press(true, 3050, 50, 50, m) == SelectMode::Words);
}

TEST_CASE("SelectionDrag cursor precedence for selection and panning", "[ui][cursor]") {
    using C = CanvasCursor;
    //                     live               space  can_pan over_page
    // Nothing live, no space: the I-beam over a single-page page, else the arrow.
    REQUIRE(canvas_cursor(Gesture::None,      false, true,  true)  == C::IBeam);
    REQUIRE(canvas_cursor(Gesture::None,      false, true,  false) == C::Arrow);
    REQUIRE(canvas_cursor(Gesture::None,      false, false, true)  == C::IBeam);

    // Space held: the move cursor ANYWHERE in the client, margins included,
    // because a Space press pans from there too -- but only when something can
    // pan. Otherwise the arrow: a Space press would pan (a no-op), not select,
    // so an I-beam would promise a selection it will not make.
    REQUIRE(canvas_cursor(Gesture::None,      true,  true,  true)  == C::Move);
    REQUIRE(canvas_cursor(Gesture::None,      true,  true,  false) == C::Move);
    REQUIRE(canvas_cursor(Gesture::None,      true,  false, true)  == C::Arrow);

    // A live pan: the same rule, space or not.
    REQUIRE(canvas_cursor(Gesture::Panning,   false, true,  true)  == C::Move);
    REQUIRE(canvas_cursor(Gesture::Panning,   false, false, true)  == C::Arrow);

    // A live selection keeps the I-beam even if Space goes down mid-drag: Space
    // is read at the press, so it cannot turn this drag into a pan.
    REQUIRE(canvas_cursor(Gesture::Selecting, true,  true,  false) == C::IBeam);
    REQUIRE(canvas_cursor(Gesture::Selecting, false, false, false) == C::IBeam);
}
```

- [ ] **Step 3: Write the failing test — `test_viewport_math.cpp`**

Add `using litepdf::ui::content_overflows;` to its `using` block and append:

```cpp
TEST_CASE("ViewportMath content overflows exactly when clamp pan leaves a range",
          "[ui][viewport]") {
    REQUIRE_FALSE(content_overflows(800.0f, 600.0f, 800.0f, 600.0f));   // exact fit
    REQUIRE(content_overflows(800.5f, 600.0f, 800.0f, 600.0f));         // width only
    REQUIRE(content_overflows(800.0f, 600.5f, 800.0f, 600.0f));         // height only
    REQUIRE_FALSE(content_overflows(10.0f, 10.0f, 800.0f, 600.0f));
    const float nan = std::numeric_limits<float>::quiet_NaN();
    REQUIRE_FALSE(content_overflows(nan, 10.0f, 800.0f, 600.0f));

    // The cursor must never promise a pan that clamp_pan would refuse, nor hide
    // one it would allow: on every case, "overflows" must equal "clamp_pan
    // leaves some axis a range below zero".
    const float cases[][4] = {
        {800.0f, 600.0f, 800.0f, 600.0f}, {801.0f, 600.0f, 800.0f, 600.0f},
        {800.0f, 601.0f, 800.0f, 600.0f}, {100.0f, 100.0f, 800.0f, 600.0f},
        {2000.0f, 50.0f, 800.0f, 600.0f}, {0.0f, 0.0f, 0.0f, 0.0f},
    };
    for (const auto& c : cases) {
        const bool has_range = clamp_pan(-1e9f, c[0], c[2]) < 0.0f
                            || clamp_pan(-1e9f, c[1], c[3]) < 0.0f;
        REQUIRE(content_overflows(c[0], c[1], c[2], c[3]) == has_range);
    }
}
```

(`<limits>` is already included by that file.)

- [ ] **Step 4: Run to verify they fail**

```bash
cmake --build build --target litepdf_unit_tests --config Release
```

Expected: **compile errors** — `'pan_step': is not a member of 'litepdf::ui::GestureState'`, `'forget': is not a member of 'litepdf::ui::ClickCounter'`, `'canvas_cursor'` / `'CanvasCursor'` / `'PanStep'` / `'content_overflows'` undeclared.

- [ ] **Step 5: Implement — `SelectionDrag.hpp`**

Update the file's opening comment line 1 from "(and, in #58, panning)" to "and hand-tool panning (#58)":

```cpp
// #52 / #58: pure gesture logic for PdfCanvas text selection and hand-tool
// panning. No Win32, no Direct2D, no MuPDF -- headless-testable, the same
// pattern as ViewportMath.hpp and SplitterMath.hpp. PdfCanvas feeds it message
// coordinates and system metrics and acts on what it returns.
```

In `ClickCounter::press`, replace the counting block — from `int count = 1;` through the closing brace of its `else if` — with this, which adds the `restart_` term and resets it (the `last_count_ = count;` … `last_y_ = y_px;` lines and the `switch` after it are unchanged):

```cpp
        int count = 1;
        if (is_double_click_message) {
            // After forget(), Windows' double click pairs this press with a
            // press that was not a click, so it starts a new sequence.
            count = restart_ ? 1 : 2;
        } else if (last_count_ == 2
                   // Unsigned subtraction: correct across GetMessageTime's wrap.
                   && static_cast<std::uint32_t>(time_ms - last_time_ms_) <= m.dblclk_ms
                   && std::abs(x_px - last_x_) <= m.dblclk_cx / 2
                   && std::abs(y_px - last_y_) <= m.dblclk_cy / 2) {
            count = 3;
        }
        restart_ = false;
```

Add this member function after `press()`:

```cpp
    // The press just counted was not a click: it started a pan (#58). The next
    // press begins a new sequence -- a single click, even when Windows reports
    // it as the second half of a double click. Without this, Space + click
    // followed by a quick plain click selects a word, and a click, Space + a
    // second press, then a plain click selects a whole line.
    void forget() noexcept {
        last_count_ = 0;
        restart_    = true;
    }
```

and add to `ClickCounter`'s private members, after `last_y_`:

```cpp
    bool          restart_      = false;   // set by forget()
```

Above `enum class ReleaseAction`, add:

```cpp
// Pointer motion between two pan steps, in client pixels.
struct PanStep {
    int dx_px = 0;
    int dy_px = 0;
};
```

In `GestureState::begin_pan`, record the press as the first step's origin — add two lines before `return true;`:

```cpp
        last_x_   = x_px;
        last_y_   = y_px;
```

Add this member function after `move()`:

```cpp
    // A pan's pointer motion (#58): the displacement since the previous step,
    // or since the press for the first one. {0, 0} unless a pan is live.
    //
    // Incremental, not measured from the press: PdfCanvas applies each step to
    // the pan as it is NOW, so anything that re-clamps the pan mid-drag (a
    // render landing, a zoom) never makes the content jump to catch up with a
    // stale absolute offset, and dragging back after overshooting an edge moves
    // the content at once instead of through a dead zone. Integer steps sum to
    // the whole drag exactly.
    PanStep pan_step(int x_px, int y_px) noexcept {
        if (gesture_ != Gesture::Panning) return {};
        const PanStep s{ x_px - last_x_, y_px - last_y_ };
        last_x_ = x_px;
        last_y_ = y_px;
        return s;
    }
```

Add to the private members, after `origin_y_`:

```cpp
    int              last_x_   = 0;   // previous pan step (#58)
    int              last_y_   = 0;
```

After the `GestureState` class and before `canvas_dip_to_page_point`, add:

```cpp
// The canvas cursor (spec §4.5, §5). The move shape is the hand tool's; the
// system has no grab hand (IDC_HAND is the hyperlink pointer).
enum class CanvasCursor { Arrow, IBeam, Move };

// In precedence order:
//   - a live SELECTION keeps the I-beam, Space or not -- Space is read at the
//     press, so going down mid-drag cannot turn the drag into a pan;
//   - a live pan, or Space held: the move shape when something can pan, and
//     the arrow when nothing can. Anywhere in the client, margins included,
//     because a Space press pans from there too. Not the I-beam when nothing
//     can pan: a Space press would pan (a no-op), not select;
//   - otherwise the I-beam over a single-page page and the arrow elsewhere.
inline CanvasCursor canvas_cursor(Gesture live, bool space_held, bool can_pan,
                                  bool over_page) noexcept {
    if (live == Gesture::Selecting) return CanvasCursor::IBeam;
    if (live == Gesture::Panning || space_held) {
        return can_pan ? CanvasCursor::Move : CanvasCursor::Arrow;
    }
    return over_page ? CanvasCursor::IBeam : CanvasCursor::Arrow;
}
```

- [ ] **Step 6: Implement — `ViewportMath.hpp`**

After `clamp_pan`, add:

```cpp
// True when the content overflows the viewport on either axis -- exactly when
// clamp_pan leaves some axis a range to pan through. The same `content >
// viewport` test as clamp_pan, so the hand-tool cursor and the pan can never
// disagree about whether there is anything to move. NaN never overflows,
// matching clamp_pan's "degenerate -> pinned" branch.
inline bool content_overflows(float content_w, float content_h,
                              float vp_w, float vp_h) noexcept {
    return content_w > vp_w || content_h > vp_h;
}
```

- [ ] **Step 7: Run to verify they pass**

```bash
cmake --build build --config Release
ctest --test-dir build -C Release -N -R "SelectionDrag|ViewportMath"
ctest --test-dir build -C Release -R "SelectionDrag|ViewportMath" --output-on-failure
ctest --test-dir build -C Release
```

Expected: the `-N` listing includes the seven new names; all pass; the full run passes **N0 + 7**. (The "space pan … never clears a selection" case pins behaviour `release()` already has — it passes on first run once the file compiles. It is there because Task 3 now depends on it.)

- [ ] **Step 8: Commit**

```bash
git add src/ui/detail/SelectionDrag.hpp src/ui/detail/ViewportMath.hpp tests/unit/test_selection_drag.cpp tests/unit/test_viewport_math.cpp
git commit -m "feat(canvas): pure pan steps and cursor precedence for hand-tool panning

GestureState::pan_step reports the pointer motion since the previous step,
so a pan follows the pointer without drift and without jumping after a
re-clamp. canvas_cursor settles the I-beam / move / arrow precedence, and
content_overflows shares clamp_pan's test so the cursor never promises a pan
that would not happen. ClickCounter::forget lets a press that became a pan
drop out of the click sequence, so it never pairs into a double click.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 2: Middle-drag panning and the pan cursor

**Files:**
- Modify: `src/ui/PdfCanvas.hpp`
- Modify: `src/ui/PdfCanvas.cpp`

**Interfaces:**
- Consumes (Task 1): `PanStep`, `GestureState::pan_step`, `CanvasCursor`, `canvas_cursor`, `content_overflows`.
- Produces (used by Task 3): `void PdfCanvas::begin_pan_gesture(MouseButton button, int x_px, int y_px)`, `void PdfCanvas::refresh_cursor()`, and `update_cursor()` routed through `canvas_cursor` with a `space_held` local that Task 3 turns into a real key read.

- [ ] **Step 1: `PdfCanvas.hpp` — include and declarations**

Add after `#include "ui/detail/ViewportMath.hpp"`:

```cpp
#include "ui/detail/SelectionDrag.hpp"
```

After the `on_left_button_up` declaration, add:

```cpp
    // #58 hand-tool panning (spec §5): middle-drag here; left-drag with space
    // held is routed to begin_pan_gesture by on_left_button_down.
    void on_middle_button_down(int x_px, int y_px);
    void on_middle_button_up(int x_px, int y_px);

    // Start a pan owned by `button`: begin_pan, SetCapture, and the pan cursor
    // -- which must be set here, because Windows sends no WM_SETCURSOR to a
    // window that holds the capture.
    void begin_pan_gesture(MouseButton button, int x_px, int y_px);

    // End a gesture that is live while this window does NOT hold the capture.
    void cancel_stale_gesture();
```

Replace the `update_cursor` declaration and its comment with:

```cpp
    // True when the painted content overflows the viewport on either axis --
    // when pan_by can move anything at all.
    bool can_pan() const;

    // WM_SETCURSOR for the client area.
    void update_cursor();

    // update_cursor outside WM_SETCURSOR: when a pan ends, and when space goes
    // down or up with the pointer still. Does nothing unless the pointer is
    // over this window or this window holds the capture.
    void refresh_cursor();
```

- [ ] **Step 2: `PdfCanvas.cpp` — usings**

Append to the `using litepdf::ui::…` block (after `using litepdf::ui::ReleaseAction;`):

```cpp
using litepdf::ui::canvas_cursor;
using litepdf::ui::CanvasCursor;
using litepdf::ui::content_overflows;
using litepdf::ui::PanStep;
```

- [ ] **Step 3: Extract `cancel_stale_gesture`**

In `on_left_button_down`, replace the stale-gesture block — the comment beginning "A gesture that is live while this window does NOT hold the capture is stale" and the `if (… && GetCapture() != hwnd_) { cancel_gesture(); }` under it — with one call:

```cpp
    cancel_stale_gesture();
```

and add the function after `cancel_gesture()`'s definition, carrying the comment:

```cpp
void PdfCanvas::cancel_stale_gesture() {
    // A gesture that is live while this window does NOT hold the capture is
    // stale: SetCapture did not take (the capture belongs to the foreground
    // thread), so the matching button-up went elsewhere and no
    // WM_CAPTURECHANGED will ever arrive to end it. Refusing the next press
    // would leave the canvas ignoring every press for the rest of the session.
    if (impl_->gesture.gesture() != Gesture::None && GetCapture() != hwnd_) {
        cancel_gesture();
    }
}
```

- [ ] **Step 4: The middle-button handlers and `begin_pan_gesture`**

Add after `on_left_button_up`'s definition:

```cpp
void PdfCanvas::begin_pan_gesture(MouseButton button, int x_px, int y_px) {
    // No bitmap, page or text handle is needed -- a pan only moves what is
    // painted, and pan_by is a no-op until something is. So none of
    // on_left_button_down's selection refusals apply, spread mode included.
    if (!impl_->view) return;
    if (!impl_->gesture.begin_pan(button, x_px, y_px)) return;
    SetCapture(hwnd_);
    update_cursor();   // no WM_SETCURSOR arrives while this window holds the capture
}

void PdfCanvas::on_middle_button_down(int x_px, int y_px) {
    // One gesture owns the capture at a time. A middle press during a
    // selection drag cancels the drag -- keeping anything already committed --
    // and starts nothing of its own (spec §4.2). A live space pan makes
    // begin_pan refuse this press (spec §5).
    //
    // No SetFocus, unlike the left press: a pan changes only the view, so a
    // query half-typed into the find box keeps the keyboard.
    if (impl_->gesture.gesture() == Gesture::Selecting) {
        cancel_gesture();
        return;
    }
    cancel_stale_gesture();
    begin_pan_gesture(MouseButton::Middle, x_px, y_px);
}

void PdfCanvas::on_middle_button_up(int x_px, int y_px) {
    // The release position counts, exactly as in on_left_button_up: injected
    // input can deliver a press and a release with no move in between.
    on_mouse_move(x_px, y_px);
    // Decide FIRST, release SECOND: ReleaseCapture's synchronous
    // WM_CAPTURECHANGED must find the pan already over (spec §4.2). None means
    // no live gesture belongs to the middle button -- a space pan owns the
    // capture, and this release must leave it alone.
    if (impl_->gesture.release(MouseButton::Middle) == ReleaseAction::None) return;
    if (GetCapture() == hwnd_) ReleaseCapture();
    refresh_cursor();   // no WM_SETCURSOR arrives until the pointer next moves
}
```

- [ ] **Step 5: The pan branch of `on_mouse_move`**

Insert at the top of `on_mouse_move`, **before** its existing `gesture() != Selecting` guard (which stays exactly as it is):

```cpp
    if (impl_->gesture.gesture() == Gesture::Panning) {
        // A pan whose capture is gone is stale (see cancel_stale_gesture) and
        // its button is up: panning here would drag the page under a pointer
        // that is merely passing over it.
        if (GetCapture() != hwnd_) {
            cancel_gesture();
            return;
        }
        // Grab-and-drag: the content follows the pointer, so moving right moves
        // the content right -- pan_by's positive direction, the one VK_LEFT
        // uses to reveal the page's left side. Steps arrive in client pixels;
        // pan_by takes DIPs.
        const PanStep s = impl_->gesture.pan_step(x_px, y_px);
        if (s.dx_px != 0 || s.dy_px != 0) {
            const float dpi = static_cast<float>(GetDpiForWindow(hwnd_));
            pan_by(client_px_to_dip(static_cast<float>(s.dx_px), dpi),
                   client_px_to_dip(static_cast<float>(s.dy_px), dpi));
        }
        return;
    }
```

- [ ] **Step 6: `on_left_button_up` — the None and EndPan arms, and the cursor**

Replace the `ReleaseAction::None` and `ReleaseAction::EndPan` arms of its `switch`:

```cpp
        case ReleaseAction::None:
            // No live gesture belongs to the left button -- including a
            // middle-button pan, whose capture this release must not touch.
            return;
```

```cpp
        case ReleaseAction::EndPan:
            break;   // a space pan: applied as the pointer moved, nothing to commit
```

and append after the function's final `InvalidateRect(hwnd_, nullptr, FALSE);`:

```cpp
    refresh_cursor();   // no WM_SETCURSOR arrives until the pointer next moves
```

- [ ] **Step 7: `can_pan`, `update_cursor`, `refresh_cursor`**

Replace `update_cursor`'s definition with these three functions:

```cpp
bool PdfCanvas::can_pan() const {
    ContentBox box{};
    if (!content_extent(box)) return false;   // nothing painted yet
    const D2D1_SIZE_F vp = impl_->rt->GetSize();
    return content_overflows(box.w, box.h, vp.width, vp.height);
}

void PdfCanvas::update_cursor() {
    bool over_page = false;
    Placement pl;
    if (!impl_->dual_page && own_bitmap() && single_page_placement(pl)) {
        POINT pt;
        if (GetCursorPos(&pt) && ScreenToClient(hwnd_, &pt)) {
            const float dpi = static_cast<float>(GetDpiForWindow(hwnd_));
            const float x = client_px_to_dip(static_cast<float>(pt.x), dpi);
            const float y = client_px_to_dip(static_cast<float>(pt.y), dpi);
            over_page = x >= pl.x && x < pl.x + pl.w && y >= pl.y && y < pl.y + pl.h;
        }
    }
    const bool space_held = false;   // becomes a GetKeyState read with space-drag panning
    LPCWSTR shape = IDC_ARROW;
    switch (canvas_cursor(impl_->gesture.gesture(), space_held, can_pan(), over_page)) {
        case CanvasCursor::Move:  shape = IDC_SIZEALL; break;
        case CanvasCursor::IBeam: shape = IDC_IBEAM;   break;
        case CanvasCursor::Arrow: break;
    }
    SetCursor(LoadCursorW(nullptr, shape));
}

void PdfCanvas::refresh_cursor() {
    if (!hwnd_) return;
    // SetCursor changes the shape wherever the pointer is. Over a sibling --
    // the find bar sits on top of the canvas -- or another window that would
    // be wrong until the pointer next moved, so act only when the pointer is
    // over this window or this window holds the capture.
    if (GetCapture() != hwnd_) {
        POINT pt;
        if (!GetCursorPos(&pt) || WindowFromPoint(pt) != hwnd_) return;
    }
    update_cursor();
}
```

- [ ] **Step 8: Wire the messages**

In `handle_message`, replace the existing `case WM_MBUTTONDOWN: case WM_RBUTTONDOWN:` arm (its comment, the `if (… == Gesture::Selecting) cancel_gesture();` and the `break;`) with:

```cpp
        case WM_MBUTTONDOWN:
        case WM_MBUTTONDBLCLK:
            // CS_DBLCLKS covers every button: the second of two quick middle
            // presses arrives as WM_MBUTTONDBLCLK and must start a pan too.
            on_middle_button_down(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            return 0;
        case WM_MBUTTONUP:
            on_middle_button_up(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            return 0;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONDBLCLK:
            // A right press during a selection drag cancels it, keeping anything
            // already committed, and starts nothing of its own (spec §4.2). A
            // pan ignores it: it has nothing a stray press could corrupt.
            if (impl_->gesture.gesture() == Gesture::Selecting) cancel_gesture();
            break;   // DefWindowProc: right-button WM_CONTEXTMENU generation unchanged
```

- [ ] **Step 9: Build and test**

```bash
cmake --build build --config Release
ctest --test-dir build -C Release
```

Expected: build clean with no new warnings; **N0 + 7** passing.

- [ ] **Step 10: GUI checks**

**The driver.** The GUI checks use this scratch driver. **It is not committed.** Save it as `build/gui/pan-drive.ps1` (`build/` is git-ignored) — if a previous task already wrote it, confirm it is identical to the block below and overwrite it if not. It is written out in full in both Task 2 and Task 3 because each task's brief is extracted on its own; keep the two copies identical.

What it measures, and why that is enough:

- **A pan is observed as a pixel translation of the canvas.** `Measure-Shift $before $after $dx $dy` samples a grid and reports `Match` — the share of samples where `after(x, y) == before(x - dx, y - dy)` — and `Changed` — the share where `after(x, y) != before(x, y)`. A pan by exactly `(dx, dy)` client pixels gives `Match ≥ 0.98` **and** `Changed ≥ 0.05` on `simple.pdf`; the second number is the positive control that there was content under the grid (white matches white under any shift). **No pan** gives `Changed ≤ 0.002`. `-MaxX` limits the sampled `after` points to `x < MaxX`, for spread mode, where each page is clipped to its own half of the canvas. The Direct2D canvas is capturable on this build (re-tested 2026-09-16), but **only from a DPI-aware process** — the driver calls `SetProcessDPIAware()` first and `Capture-Canvas` prints the captured size; it must equal the canvas client size, or every number is meaningless.
- **"Is there a selection" is read from the Edit menu, not pixels.** `Copy-Enabled` sends `WM_INITMENUPOPUP` for the Edit popup and reads `IDM_EDIT_COPY`'s state. Copy is also enabled whenever an edit control has the focus, so each check that uses it first requires `Copy-Enabled` to be `$false`.
- **Where the text is depends on the DPI, the window size and the zoom preset**, so no fixed client point is guaranteed to be over text (at 100 % scaling the canvas centre can sit in `simple.pdf`'s top margin). `Find-TextPoint` tries candidate points and returns the first where a 160 px horizontal left-drag makes Copy enabled, then clears the selection. A check that relies on "this drag would have selected text" uses that point — `Find-TextPoint` has already proved it.
- **The cursor is read with `GetCursorInfo`** and compared against the shared system cursor handles (`IDC_ARROW` 32512, `IDC_IBEAM` 32513, `IDC_SIZEALL` 32646). This needs litepdf in the foreground with the real pointer over the canvas; `Point-At` does both. If `Cursor-Is` throws "cursor not showing", the system is suppressing the pointer (`CURSOR_SUPPRESSED`, after touch or pen input): that check is **BLOCKED, not failed** — report it as blocked for the controller to run with the user.
- **Presses are kept apart by `Wait-DoubleClick`**, which sleeps the system double-click time plus a margin — never a fixed number: a user who has lengthened the double-click time would otherwise see a "clearing" click counted as a triple click that selects a line.
- **Space is a real key event** (`keybd_event`), because the canvas reads it with `GetKeyState` and a posted `WM_KEYDOWN` does not change key state. `With-Space` asserts litepdf is the foreground window first — injected keys go to whatever is foreground — and releases Space in a `finally`, so a failing check cannot leave the machine's Space key logically held.

**Run the whole check sequence as ONE script** — e.g. write it to `build/gui/task-checks.ps1` (dot-sourcing the driver at its top) and run `powershell -NoProfile -ExecutionPolicy Bypass -File build\gui\task-checks.ps1` from the repo root. A PowerShell tool call does not keep variables into the next one, and the checks depend on pan state built up by the checks before them.

```powershell
# pan-drive.ps1 -- scratch driver for the #58 GUI checks. Not committed.
# Dot-source from the repo root in Windows PowerShell 5.1:  . .\build\gui\pan-drive.ps1
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName Microsoft.VisualBasic
if (-not ('PanU' -as [type])) {
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class PanU {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll", CharSet=CharSet.Unicode)]
  public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, IntPtr title);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern IntPtr GetMenu(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetSubMenu(IntPtr m, int pos);
  [DllImport("user32.dll")] public static extern uint GetMenuState(IntPtr m, uint id, uint flags);
  [DllImport("user32.dll")] public static extern bool GetCursorInfo(ref CURSORINFO ci);
  [DllImport("user32.dll")] public static extern IntPtr LoadCursorW(IntPtr inst, IntPtr id);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern uint GetDoubleClickTime();
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [StructLayout(LayoutKind.Sequential)] public struct CURSORINFO { public int cbSize; public int flags; public IntPtr hCursor; public POINT pt; }
}
"@
}
[void][PanU]::SetProcessDPIAware()

$WM_COMMAND = 0x0111; $WM_KEYDOWN = 0x0100; $WM_INITMENUPOPUP = 0x0117; $WM_CLOSE = 0x0010
$WM_CAPTURECHANGED = 0x0215
$WM_MOUSEMOVE = 0x0200; $WM_LBUTTONDOWN = 0x0201; $WM_LBUTTONUP = 0x0202
$WM_MBUTTONDOWN = 0x0207; $WM_MBUTTONUP = 0x0208; $WM_MBUTTONDBLCLK = 0x0209
$MK_LBUTTON = 0x01; $MK_MBUTTON = 0x10
$IDM_ZOOM_IN = 40010; $IDM_ZOOM_OUT = 40011; $IDM_ZOOM_RESET = 40012
$IDM_VIEW_DUAL_PAGE = 40062; $IDM_EDIT_COPY = 40071
$IDC_ARROW = 32512; $IDC_IBEAM = 32513; $IDC_SIZEALL = 32646

function Start-LitePdf([string[]]$files) {
  $env:LITEPDF_NO_RESTORE = '1'
  $p = Start-Process -PassThru (Resolve-Path .\build\Release\litepdf.exe) -ArgumentList $files
  for ($i = 0; $i -lt 50 -and [int64]$p.MainWindowHandle -eq 0; $i++) { Start-Sleep -Milliseconds 100; $p.Refresh() }
  $script:Proc   = $p
  $script:Main   = [IntPtr]$p.MainWindowHandle
  $script:Canvas = [PanU]::FindWindowExW($script:Main, [IntPtr]::Zero, 'LitePDFPdfCanvas', [IntPtr]::Zero)
  if ([int64]$script:Canvas -eq 0) { throw 'canvas HWND not found' }
  Start-Sleep -Milliseconds 1000
}

function Send-Command([int]$id, [int]$times = 1) {
  for ($i = 0; $i -lt $times; $i++) {
    [void][PanU]::PostMessageW($script:Main, $WM_COMMAND, [IntPtr]$id, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 400
  }
  Start-Sleep -Milliseconds 800   # let the render land
}

function Client-Size { $r = New-Object PanU+RECT; [void][PanU]::GetClientRect($script:Canvas, [ref]$r); return @($r.Right, $r.Bottom) }
function Center { $s = Client-Size; return @([int]($s[0] / 2), [int]($s[1] / 2)) }
function Offset([int[]]$p, [int]$dx, [int]$dy) { return @(($p[0] + $dx), ($p[1] + $dy)) }

function Post-Mouse([int]$msg, [int[]]$at, [int]$keys) {
  $l = [IntPtr]((($at[1] -band 0xFFFF) -shl 16) -bor ($at[0] -band 0xFFFF))
  [void][PanU]::PostMessageW($script:Canvas, $msg, [IntPtr]$keys, $l)
  Start-Sleep -Milliseconds 40
}

# Middle-drag from $from to $to. -NoMove posts only the press and the release.
function Middle-Drag([int[]]$from, [int[]]$to, [int]$steps = 8, [switch]$NoMove) {
  Post-Mouse $WM_MBUTTONDOWN $from $MK_MBUTTON
  if (-not $NoMove) {
    for ($i = 1; $i -le $steps; $i++) {
      $x = [int]($from[0] + ($to[0] - $from[0]) * $i / $steps)
      $y = [int]($from[1] + ($to[1] - $from[1]) * $i / $steps)
      Post-Mouse $WM_MOUSEMOVE @($x, $y) $MK_MBUTTON
    }
  }
  Post-Mouse $WM_MBUTTONUP $to 0
  Start-Sleep -Milliseconds 300
}

function Left-Drag([int[]]$from, [int[]]$to, [int]$steps = 8) {
  Post-Mouse $WM_LBUTTONDOWN $from $MK_LBUTTON
  for ($i = 1; $i -le $steps; $i++) {
    $x = [int]($from[0] + ($to[0] - $from[0]) * $i / $steps)
    $y = [int]($from[1] + ($to[1] - $from[1]) * $i / $steps)
    Post-Mouse $WM_MOUSEMOVE @($x, $y) $MK_LBUTTON
  }
  Post-Mouse $WM_LBUTTONUP $to 0
  Start-Sleep -Milliseconds 300
}

# Wait out the system double-click time (500 ms by default, but user-set), so the
# next press cannot pair with the previous one into a double or triple click.
function Wait-DoubleClick { Start-Sleep -Milliseconds ([int][PanU]::GetDoubleClickTime() + 150) }

function Left-Click([int[]]$at) { Post-Mouse $WM_LBUTTONDOWN $at $MK_LBUTTON; Post-Mouse $WM_LBUTTONUP $at 0; Start-Sleep -Milliseconds 200 }

# Pin the pan to (0, 0): a drag far past the top-left clamp.
function Reset-Pan { $c = Center; Middle-Drag $c (Offset $c 3000 3000) }

function Activate { [Microsoft.VisualBasic.Interaction]::AppActivate($script:Proc.Id); Start-Sleep -Milliseconds 400 }

function Capture-Canvas {
  Activate
  $s = Client-Size
  $pt = New-Object PanU+POINT
  [void][PanU]::ClientToScreen($script:Canvas, [ref]$pt)
  $bmp = New-Object System.Drawing.Bitmap $s[0], $s[1]
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($pt.X, $pt.Y, 0, 0, (New-Object System.Drawing.Size $s[0], $s[1]))
  $g.Dispose()
  Write-Host "captured $($bmp.Width)x$($bmp.Height) (canvas client $($s[0])x$($s[1]))"
  return $bmp
}

function Same-Color($a, $b) {
  return ([math]::Abs($a.R - $b.R) -le 8) -and ([math]::Abs($a.G - $b.G) -le 8) -and ([math]::Abs($a.B - $b.B) -le 8)
}

# -MaxX > 0 samples only after-points with x < MaxX.
function Measure-Shift($before, $after, [int]$dx, [int]$dy, [int]$MaxX = 0) {
  if ($before.Width -ne $after.Width -or $before.Height -ne $after.Height) { throw 'capture size changed between captures' }
  $w = $before.Width; $h = $before.Height
  $n = 0; $match = 0; $changed = 0
  for ($y = 24; $y -lt $h - 24; $y += 16) {
    for ($x = 24; $x -lt $w - 24; $x += 16) {
      if ($MaxX -gt 0 -and $x -ge $MaxX) { continue }
      $sx = $x - $dx; $sy = $y - $dy
      if ($sx -lt 0 -or $sy -lt 0 -or $sx -ge $w -or $sy -ge $h) { continue }
      $n++
      $a = $after.GetPixel($x, $y)
      if (Same-Color $a $before.GetPixel($sx, $sy)) { $match++ }
      if (-not (Same-Color $a $before.GetPixel($x, $y))) { $changed++ }
    }
  }
  if ($n -lt 500) { throw "only $n samples overlap -- canvas too small or shift too large" }
  $r = [pscustomobject]@{ Samples = $n; Match = [math]::Round($match / $n, 4); Changed = [math]::Round($changed / $n, 4) }
  Write-Host "shift ($dx, $dy) maxX=$MaxX : $r"
  return $r
}

function Copy-Enabled {
  $edit = [PanU]::GetSubMenu([PanU]::GetMenu($script:Main), 1)   # File | Edit | View | Help
  [void][PanU]::SendMessageW($script:Main, $WM_INITMENUPOPUP, $edit, [IntPtr]1)
  return (([PanU]::GetMenuState($edit, $IDM_EDIT_COPY, 0) -band 0x3) -eq 0)   # MF_GRAYED|MF_DISABLED
}

# A client point where a 160 px horizontal left-drag selects text at the current
# zoom and pan. Leaves no selection behind (the closing click clears it).
function Find-TextPoint {
  $s = Client-Size
  foreach ($fy in 0.5, 0.35, 0.65, 0.2, 0.8) {
    foreach ($fx in 0.3, 0.5) {
      $p = @([int]($s[0] * $fx), [int]($s[1] * $fy))
      Left-Drag $p (Offset $p 160 0)
      $hit = Copy-Enabled
      Left-Click $p
      if ($hit) { Write-Host "text point: $p"; return $p }
    }
  }
  throw 'no candidate point selects text -- pan onto the text block and retry'
}

function Point-At([int[]]$client) {
  Activate
  $pt = New-Object PanU+POINT
  $pt.X = $client[0]; $pt.Y = $client[1]
  [void][PanU]::ClientToScreen($script:Canvas, [ref]$pt)
  [void][PanU]::SetCursorPos($pt.X, $pt.Y)
  Start-Sleep -Milliseconds 150
}

function Cursor-Is([int]$idc) {
  Start-Sleep -Milliseconds 150
  $ci = New-Object PanU+CURSORINFO
  $ci.cbSize = [System.Runtime.InteropServices.Marshal]::SizeOf($ci)
  [void][PanU]::GetCursorInfo([ref]$ci)
  if ($ci.flags -ne 1) { throw "cursor not showing (flags=$($ci.flags)) -- BLOCKED, not failed" }   # CURSOR_SHOWING
  return ($ci.hCursor -eq [PanU]::LoadCursorW([IntPtr]::Zero, [IntPtr]$idc))
}

# Run $body with Space held. Space goes to the FOREGROUND window, so assert
# litepdf is it; always release, even when $body throws.
function With-Space([scriptblock]$body) {
  Activate
  if ([PanU]::GetForegroundWindow() -ne $script:Main) { throw 'litepdf is not the foreground window' }
  [PanU]::keybd_event(0x20, 0x39, 0, [UIntPtr]::Zero)
  Start-Sleep -Milliseconds 300   # let litepdf read the key before any posted mouse message
  try { & $body } finally { [PanU]::keybd_event(0x20, 0x39, 2, [UIntPtr]::Zero); Start-Sleep -Milliseconds 300 }
}

# WM_CLOSE, then WAIT: the app saves session.json on its way out, and a restore
# copied back before that save lands is overwritten by it.
function Close-LitePdf {
  [void][PanU]::PostMessageW($script:Main, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
  if (-not $script:Proc.WaitForExit(10000)) { throw 'litepdf did not exit within 10 s' }
}
```

**Hygiene for every GUI run.** Before the first launch, back up the live session — the app auto-saves over it within ~1.5 s: `Copy-Item "$env:LOCALAPPDATA\LitePDF\session.json" "$env:TEMP\litepdf-session-58.bak"`. After the run, `Close-LitePdf` (never `Stop-Process`: a force-kill leaves `running.lock` and the next launch offers a restore) and only then copy the backup back. Do not touch the mouse or keyboard while a run is in progress. Confirm the binary under test is the one just built: `(Get-Item .\build\Release\litepdf.exe).LastWriteTime` must be newer than your last source edit.

**The checks.** At the top of the check script:

```powershell
. .\build\gui\pan-drive.ps1
Start-LitePdf @('tests\fixtures\simple.pdf')
Send-Command $IDM_ZOOM_IN 3        # from the FitWidth default: both axes overflow
Reset-Pan
$C = Center
```

Each check states what makes it **fail**. Record every `Measure-Shift` line and every cursor result in the task report. **P9 has no check:** a posted `WM_CAPTURECHANGED` ends the pan through its own arm before any move arrives, and no driver can leave a pan live without the capture and without that message. P9 is covered by review; these checks catch it *misfiring* (every pan would stop at its first move).

1. **Middle-drag pans by exactly the pointer motion.** `$a = Capture-Canvas; Middle-Drag $C (Offset $C -120 -100); $b = Capture-Canvas; Measure-Shift $a $b -120 -100`. Pass: `Match ≥ 0.98` and `Changed ≥ 0.05`. **Fails if** `Changed` is ~0 (no pan) or `Match` is low (panned by the wrong amount — a missing px→DIP conversion shows as exactly twice the shift at 200 % scaling).
2. **The release position counts** (P5). `$a = Capture-Canvas; Middle-Drag $C (Offset $C -80 -60) -NoMove; $b = Capture-Canvas; Measure-Shift $a $b -80 -60`. Pass as in 1. **Fails if** `Changed` is ~0: the release ignored its own coordinates.
3. **The pan clamps at the edge, then stops.** The pan is now (-200, -160) client pixels from the top-left clamp. `$a = Capture-Canvas; Middle-Drag $C (Offset $C 400 400); $b = Capture-Canvas; Measure-Shift $a $b 200 160` → pass as in 1 (it moved back exactly to the edge, no further). Then the negative control: `$a = Capture-Canvas; Middle-Drag $C (Offset $C 100 100); $b = Capture-Canvas; (Measure-Shift $a $b 0 0).Changed` ≤ 0.002. **Fails if** the first shift is not (200, 160) or the second moves anything.
4. **A quick second middle press pans** (P1). This posts `WM_MBUTTONDBLCLK` directly, so it tests the new arm, not Windows' conversion of the press. `$a = Capture-Canvas; Post-Mouse $WM_MBUTTONDOWN $C $MK_MBUTTON; Post-Mouse $WM_MBUTTONUP $C 0; Post-Mouse $WM_MBUTTONDBLCLK $C $MK_MBUTTON; Post-Mouse $WM_MOUSEMOVE (Offset $C -90 -70) $MK_MBUTTON; Post-Mouse $WM_MBUTTONUP (Offset $C -90 -70) 0; Start-Sleep -Milliseconds 300; $b = Capture-Canvas; Measure-Shift $a $b -90 -70` → pass as in 1. **Fails if** `Changed` is ~0: `WM_MBUTTONDBLCLK` started nothing.
5. **A left press during a middle pan is refused, and its release does not end the pan.** `Reset-Pan; $T = Find-TextPoint`; require `Copy-Enabled` `$false`. Then:
   ```powershell
   $a = Capture-Canvas
   Post-Mouse $WM_MBUTTONDOWN $T                    $MK_MBUTTON
   Post-Mouse $WM_MOUSEMOVE   (Offset $T -50 0)     $MK_MBUTTON
   Post-Mouse $WM_LBUTTONDOWN (Offset $T -50 0)     ($MK_MBUTTON -bor $MK_LBUTTON)
   Post-Mouse $WM_MOUSEMOVE   (Offset $T -100 -40)  ($MK_MBUTTON -bor $MK_LBUTTON)
   Post-Mouse $WM_LBUTTONUP   (Offset $T -100 -40)  $MK_MBUTTON
   Post-Mouse $WM_MOUSEMOVE   (Offset $T -150 -40)  $MK_MBUTTON
   Post-Mouse $WM_MBUTTONUP   (Offset $T -150 -40)  0
   Start-Sleep -Milliseconds 300
   $b = Capture-Canvas
   Measure-Shift $a $b -150 -40
   ```
   Pass as in 1, and `Copy-Enabled` still `$false`. **Fails if** the shift stops at (-100, -40) — the left release ended the pan — or Copy became enabled: the left press started a selection (`$T` is a point where a left-drag is known to select text).
6. **A middle press during a selection drag cancels it and pans nothing** (spec §4.2). `Reset-Pan; $T = Find-TextPoint`; require `Copy-Enabled` `$false`. Then:
   ```powershell
   $a = Capture-Canvas
   Post-Mouse $WM_LBUTTONDOWN $T                    $MK_LBUTTON
   Post-Mouse $WM_MOUSEMOVE   (Offset $T 160 0)     $MK_LBUTTON
   Post-Mouse $WM_MBUTTONDOWN (Offset $T 160 0)     ($MK_LBUTTON -bor $MK_MBUTTON)
   Post-Mouse $WM_MOUSEMOVE   (Offset $T 100 -80)   ($MK_LBUTTON -bor $MK_MBUTTON)
   Post-Mouse $WM_MBUTTONUP   (Offset $T 100 -80)   $MK_LBUTTON
   Post-Mouse $WM_MOUSEMOVE   (Offset $T 160 0)     $MK_LBUTTON
   Post-Mouse $WM_LBUTTONUP   (Offset $T 160 0)     0
   Start-Sleep -Milliseconds 300
   $b = Capture-Canvas
   (Measure-Shift $a $b 0 0).Changed     # expect <= 0.002
   Copy-Enabled                          # expect $false
   ```
   `Find-TextPoint` has already proved that this same left-drag, `$T` to `$T + (160, 0)`, selects text when nothing interrupts it, so a grayed Copy here is the cancel's doing. Then recovery: `$a = Capture-Canvas; Middle-Drag $C (Offset $C -60 -30); $b = Capture-Canvas; Measure-Shift $a $b -60 -30` → pass as in 1. **Fails if** anything moved or stayed highlighted, Copy is enabled (the drag committed), or the recovery drag does not pan (a gesture left latched).
7. **Losing the capture ends the pan, and the canvas recovers.** `Reset-Pan; $a = Capture-Canvas; Post-Mouse $WM_MBUTTONDOWN $C $MK_MBUTTON; Post-Mouse $WM_MOUSEMOVE (Offset $C -40 0) $MK_MBUTTON; [void][PanU]::PostMessageW($Canvas, $WM_CAPTURECHANGED, [IntPtr]::Zero, [IntPtr]::Zero); Start-Sleep -Milliseconds 100; Post-Mouse $WM_MOUSEMOVE (Offset $C -140 0) $MK_MBUTTON; Post-Mouse $WM_MBUTTONUP (Offset $C -140 0) 0; Start-Sleep -Milliseconds 300; $b = Capture-Canvas; Measure-Shift $a $b -40 0` → pass as in 1. Then `$a = Capture-Canvas; Middle-Drag $C (Offset $C -60 0); $b = Capture-Canvas; Measure-Shift $a $b -60 0` → pass as in 1. **Fails if** the first shift is (-140, 0) (the pan outlived its capture) or the second drag does not pan.
8. **The cursor during a pan** (P2, P6). `Reset-Pan; Point-At $C`; `Cursor-Is $IDC_IBEAM` must be `$true` (precondition: over the page, nothing live). `Post-Mouse $WM_MBUTTONDOWN $C $MK_MBUTTON; Cursor-Is $IDC_SIZEALL` → `$true`. `Post-Mouse $WM_MBUTTONUP $C 0; Cursor-Is $IDC_IBEAM` → `$true` (this half checks the outcome: it passes whether the canvas's own `refresh_cursor` or a system-generated post-release mouse move restored the I-beam). Negative control, content that fits: `Send-Command $IDM_ZOOM_OUT 8; Point-At (Center)`; `Cursor-Is $IDC_IBEAM` `$true`; `Post-Mouse $WM_MBUTTONDOWN (Center) $MK_MBUTTON; Cursor-Is $IDC_ARROW` → `$true` (nothing to pan); `Post-Mouse $WM_MBUTTONUP (Center) 0`; and `$a = Capture-Canvas; Middle-Drag (Center) (Offset (Center) -100 -100); $b = Capture-Canvas; (Measure-Shift $a $b 0 0).Changed` ≤ 0.002. **Fails if** the pan keeps the I-beam, or shows the move cursor over content that cannot move.

`Close-LitePdf` (it waits for the process to exit); then restore `session.json`.

- [ ] **Step 11: Commit**

```bash
git add src/ui/PdfCanvas.hpp src/ui/PdfCanvas.cpp
git commit -m "feat(canvas): pan a zoomed page by dragging with the middle button

The press captures and starts GestureState's pan; each move pans by the
pointer motion since the last one, converted from client pixels to DIPs;
the release decides before ReleaseCapture, as the selection path does.
WM_MBUTTONDBLCLK starts a pan too, since CS_DBLCLKS covers every button.
The move cursor is set when the pan starts -- no WM_SETCURSOR arrives while
the canvas holds the capture -- and only when the content can move.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 3: Space + left-drag panning and the Space cursor

**Files:**
- Modify: `src/ui/PdfCanvas.cpp`

**Interfaces:**
- Consumes (Task 2): `PdfCanvas::begin_pan_gesture(MouseButton, int, int)`, `PdfCanvas::refresh_cursor()`, `update_cursor()`'s `space_held` local; (Task 1) `canvas_cursor`, `ClickCounter::forget()` (`impl_->clicks` is the canvas's `ClickCounter`).
- Produces: nothing later tasks call.

- [ ] **Step 1: The Space branch of `on_left_button_down`**

In `on_left_button_down`, immediately **after** the line `if (impl_->gesture.gesture() != Gesture::None) return;` and **before** the spread-mode refusal and its comment ("Refuse spread mode HERE, not only in paint"), insert:

```cpp
    // #58: space held makes this press a pan. Read at the press with
    // GetKeyState, never latched across WM_KEYDOWN / WM_KEYUP (spec §5): hold
    // space, Alt+Tab away, release it there, and a latch never sees the key-up.
    // Checked BEFORE the refusals below, which guard SELECTION -- a pan is as
    // valid in spread mode, or before this page's bitmap lands, as anywhere.
    if ((GetKeyState(VK_SPACE) & 0x8000) != 0) {
        // Not a click: the press counted above must not pair with the next one
        // into a double or triple click.
        impl_->clicks.forget();
        begin_pan_gesture(MouseButton::Left, x_px, y_px);
        return;
    }
```

`on_left_button_up` needs no change: Task 2 already made its `EndPan` arm release the capture and refresh the cursor, and `release(MouseButton::Left)` returns `EndPan` for a left-owned pan whatever the selection mode (Task 1's "space pan … never clears a selection" test).

- [ ] **Step 2: Read Space in `update_cursor`**

Replace the line `const bool space_held = false;   // becomes a GetKeyState read with space-drag panning` with:

```cpp
    // Read, never latched (spec §5): a latch set by WM_KEYDOWN would stick
    // whenever space is released in another window.
    const bool space_held = (GetKeyState(VK_SPACE) & 0x8000) != 0;
```

- [ ] **Step 3: Refresh the cursor when Space goes down or up**

In `handle_message`'s `case WM_KEYDOWN: {` arm, insert as the arm's first statement, **before** its long "Defense-in-depth for tab-navigation shortcuts" comment:

```cpp
            if (w == VK_SPACE) {
                // The hand-tool cursor follows space at once, not at the next
                // pointer move. Nothing is stored: the key is still read on
                // demand, by the press and by update_cursor.
                refresh_cursor();
                return 0;
            }
```

and add a new arm after the `WM_KEYDOWN` arm's closing brace:

```cpp
        case WM_KEYUP:
            if (w == VK_SPACE) refresh_cursor();
            break;
```

(`GetKeyState` reflects the message being processed: during this `WM_KEYDOWN` it reports Space down, during `WM_KEYUP` up. Space is not in `MainWindow`'s accelerator table, so both reach the canvas when it has the focus.)

- [ ] **Step 4: Build and test**

```bash
cmake --build build --config Release
ctest --test-dir build -C Release
```

Expected: build clean; **N0 + 7** passing.

- [ ] **Step 5: GUI checks**

**The driver.** The GUI checks use this scratch driver. **It is not committed.** Save it as `build/gui/pan-drive.ps1` (`build/` is git-ignored) — if a previous task already wrote it, confirm it is identical to the block below and overwrite it if not. It is written out in full in both Task 2 and Task 3 because each task's brief is extracted on its own; keep the two copies identical.

What it measures, and why that is enough:

- **A pan is observed as a pixel translation of the canvas.** `Measure-Shift $before $after $dx $dy` samples a grid and reports `Match` — the share of samples where `after(x, y) == before(x - dx, y - dy)` — and `Changed` — the share where `after(x, y) != before(x, y)`. A pan by exactly `(dx, dy)` client pixels gives `Match ≥ 0.98` **and** `Changed ≥ 0.05` on `simple.pdf`; the second number is the positive control that there was content under the grid (white matches white under any shift). **No pan** gives `Changed ≤ 0.002`. `-MaxX` limits the sampled `after` points to `x < MaxX`, for spread mode, where each page is clipped to its own half of the canvas. The Direct2D canvas is capturable on this build (re-tested 2026-09-16), but **only from a DPI-aware process** — the driver calls `SetProcessDPIAware()` first and `Capture-Canvas` prints the captured size; it must equal the canvas client size, or every number is meaningless.
- **"Is there a selection" is read from the Edit menu, not pixels.** `Copy-Enabled` sends `WM_INITMENUPOPUP` for the Edit popup and reads `IDM_EDIT_COPY`'s state. Copy is also enabled whenever an edit control has the focus, so each check that uses it first requires `Copy-Enabled` to be `$false`.
- **Where the text is depends on the DPI, the window size and the zoom preset**, so no fixed client point is guaranteed to be over text (at 100 % scaling the canvas centre can sit in `simple.pdf`'s top margin). `Find-TextPoint` tries candidate points and returns the first where a 160 px horizontal left-drag makes Copy enabled, then clears the selection. A check that relies on "this drag would have selected text" uses that point — `Find-TextPoint` has already proved it.
- **The cursor is read with `GetCursorInfo`** and compared against the shared system cursor handles (`IDC_ARROW` 32512, `IDC_IBEAM` 32513, `IDC_SIZEALL` 32646). This needs litepdf in the foreground with the real pointer over the canvas; `Point-At` does both. If `Cursor-Is` throws "cursor not showing", the system is suppressing the pointer (`CURSOR_SUPPRESSED`, after touch or pen input): that check is **BLOCKED, not failed** — report it as blocked for the controller to run with the user.
- **Presses are kept apart by `Wait-DoubleClick`**, which sleeps the system double-click time plus a margin — never a fixed number: a user who has lengthened the double-click time would otherwise see a "clearing" click counted as a triple click that selects a line.
- **Space is a real key event** (`keybd_event`), because the canvas reads it with `GetKeyState` and a posted `WM_KEYDOWN` does not change key state. `With-Space` asserts litepdf is the foreground window first — injected keys go to whatever is foreground — and releases Space in a `finally`, so a failing check cannot leave the machine's Space key logically held.

**Run the whole check sequence as ONE script** — e.g. write it to `build/gui/task-checks.ps1` (dot-sourcing the driver at its top) and run `powershell -NoProfile -ExecutionPolicy Bypass -File build\gui\task-checks.ps1` from the repo root. A PowerShell tool call does not keep variables into the next one, and the checks depend on pan state built up by the checks before them.

```powershell
# pan-drive.ps1 -- scratch driver for the #58 GUI checks. Not committed.
# Dot-source from the repo root in Windows PowerShell 5.1:  . .\build\gui\pan-drive.ps1
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName Microsoft.VisualBasic
if (-not ('PanU' -as [type])) {
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class PanU {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll", CharSet=CharSet.Unicode)]
  public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, IntPtr title);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern IntPtr GetMenu(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetSubMenu(IntPtr m, int pos);
  [DllImport("user32.dll")] public static extern uint GetMenuState(IntPtr m, uint id, uint flags);
  [DllImport("user32.dll")] public static extern bool GetCursorInfo(ref CURSORINFO ci);
  [DllImport("user32.dll")] public static extern IntPtr LoadCursorW(IntPtr inst, IntPtr id);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern uint GetDoubleClickTime();
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [StructLayout(LayoutKind.Sequential)] public struct CURSORINFO { public int cbSize; public int flags; public IntPtr hCursor; public POINT pt; }
}
"@
}
[void][PanU]::SetProcessDPIAware()

$WM_COMMAND = 0x0111; $WM_KEYDOWN = 0x0100; $WM_INITMENUPOPUP = 0x0117; $WM_CLOSE = 0x0010
$WM_CAPTURECHANGED = 0x0215
$WM_MOUSEMOVE = 0x0200; $WM_LBUTTONDOWN = 0x0201; $WM_LBUTTONUP = 0x0202
$WM_MBUTTONDOWN = 0x0207; $WM_MBUTTONUP = 0x0208; $WM_MBUTTONDBLCLK = 0x0209
$MK_LBUTTON = 0x01; $MK_MBUTTON = 0x10
$IDM_ZOOM_IN = 40010; $IDM_ZOOM_OUT = 40011; $IDM_ZOOM_RESET = 40012
$IDM_VIEW_DUAL_PAGE = 40062; $IDM_EDIT_COPY = 40071
$IDC_ARROW = 32512; $IDC_IBEAM = 32513; $IDC_SIZEALL = 32646

function Start-LitePdf([string[]]$files) {
  $env:LITEPDF_NO_RESTORE = '1'
  $p = Start-Process -PassThru (Resolve-Path .\build\Release\litepdf.exe) -ArgumentList $files
  for ($i = 0; $i -lt 50 -and [int64]$p.MainWindowHandle -eq 0; $i++) { Start-Sleep -Milliseconds 100; $p.Refresh() }
  $script:Proc   = $p
  $script:Main   = [IntPtr]$p.MainWindowHandle
  $script:Canvas = [PanU]::FindWindowExW($script:Main, [IntPtr]::Zero, 'LitePDFPdfCanvas', [IntPtr]::Zero)
  if ([int64]$script:Canvas -eq 0) { throw 'canvas HWND not found' }
  Start-Sleep -Milliseconds 1000
}

function Send-Command([int]$id, [int]$times = 1) {
  for ($i = 0; $i -lt $times; $i++) {
    [void][PanU]::PostMessageW($script:Main, $WM_COMMAND, [IntPtr]$id, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 400
  }
  Start-Sleep -Milliseconds 800   # let the render land
}

function Client-Size { $r = New-Object PanU+RECT; [void][PanU]::GetClientRect($script:Canvas, [ref]$r); return @($r.Right, $r.Bottom) }
function Center { $s = Client-Size; return @([int]($s[0] / 2), [int]($s[1] / 2)) }
function Offset([int[]]$p, [int]$dx, [int]$dy) { return @(($p[0] + $dx), ($p[1] + $dy)) }

function Post-Mouse([int]$msg, [int[]]$at, [int]$keys) {
  $l = [IntPtr]((($at[1] -band 0xFFFF) -shl 16) -bor ($at[0] -band 0xFFFF))
  [void][PanU]::PostMessageW($script:Canvas, $msg, [IntPtr]$keys, $l)
  Start-Sleep -Milliseconds 40
}

# Middle-drag from $from to $to. -NoMove posts only the press and the release.
function Middle-Drag([int[]]$from, [int[]]$to, [int]$steps = 8, [switch]$NoMove) {
  Post-Mouse $WM_MBUTTONDOWN $from $MK_MBUTTON
  if (-not $NoMove) {
    for ($i = 1; $i -le $steps; $i++) {
      $x = [int]($from[0] + ($to[0] - $from[0]) * $i / $steps)
      $y = [int]($from[1] + ($to[1] - $from[1]) * $i / $steps)
      Post-Mouse $WM_MOUSEMOVE @($x, $y) $MK_MBUTTON
    }
  }
  Post-Mouse $WM_MBUTTONUP $to 0
  Start-Sleep -Milliseconds 300
}

function Left-Drag([int[]]$from, [int[]]$to, [int]$steps = 8) {
  Post-Mouse $WM_LBUTTONDOWN $from $MK_LBUTTON
  for ($i = 1; $i -le $steps; $i++) {
    $x = [int]($from[0] + ($to[0] - $from[0]) * $i / $steps)
    $y = [int]($from[1] + ($to[1] - $from[1]) * $i / $steps)
    Post-Mouse $WM_MOUSEMOVE @($x, $y) $MK_LBUTTON
  }
  Post-Mouse $WM_LBUTTONUP $to 0
  Start-Sleep -Milliseconds 300
}

# Wait out the system double-click time (500 ms by default, but user-set), so the
# next press cannot pair with the previous one into a double or triple click.
function Wait-DoubleClick { Start-Sleep -Milliseconds ([int][PanU]::GetDoubleClickTime() + 150) }

function Left-Click([int[]]$at) { Post-Mouse $WM_LBUTTONDOWN $at $MK_LBUTTON; Post-Mouse $WM_LBUTTONUP $at 0; Start-Sleep -Milliseconds 200 }

# Pin the pan to (0, 0): a drag far past the top-left clamp.
function Reset-Pan { $c = Center; Middle-Drag $c (Offset $c 3000 3000) }

function Activate { [Microsoft.VisualBasic.Interaction]::AppActivate($script:Proc.Id); Start-Sleep -Milliseconds 400 }

function Capture-Canvas {
  Activate
  $s = Client-Size
  $pt = New-Object PanU+POINT
  [void][PanU]::ClientToScreen($script:Canvas, [ref]$pt)
  $bmp = New-Object System.Drawing.Bitmap $s[0], $s[1]
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($pt.X, $pt.Y, 0, 0, (New-Object System.Drawing.Size $s[0], $s[1]))
  $g.Dispose()
  Write-Host "captured $($bmp.Width)x$($bmp.Height) (canvas client $($s[0])x$($s[1]))"
  return $bmp
}

function Same-Color($a, $b) {
  return ([math]::Abs($a.R - $b.R) -le 8) -and ([math]::Abs($a.G - $b.G) -le 8) -and ([math]::Abs($a.B - $b.B) -le 8)
}

# -MaxX > 0 samples only after-points with x < MaxX.
function Measure-Shift($before, $after, [int]$dx, [int]$dy, [int]$MaxX = 0) {
  if ($before.Width -ne $after.Width -or $before.Height -ne $after.Height) { throw 'capture size changed between captures' }
  $w = $before.Width; $h = $before.Height
  $n = 0; $match = 0; $changed = 0
  for ($y = 24; $y -lt $h - 24; $y += 16) {
    for ($x = 24; $x -lt $w - 24; $x += 16) {
      if ($MaxX -gt 0 -and $x -ge $MaxX) { continue }
      $sx = $x - $dx; $sy = $y - $dy
      if ($sx -lt 0 -or $sy -lt 0 -or $sx -ge $w -or $sy -ge $h) { continue }
      $n++
      $a = $after.GetPixel($x, $y)
      if (Same-Color $a $before.GetPixel($sx, $sy)) { $match++ }
      if (-not (Same-Color $a $before.GetPixel($x, $y))) { $changed++ }
    }
  }
  if ($n -lt 500) { throw "only $n samples overlap -- canvas too small or shift too large" }
  $r = [pscustomobject]@{ Samples = $n; Match = [math]::Round($match / $n, 4); Changed = [math]::Round($changed / $n, 4) }
  Write-Host "shift ($dx, $dy) maxX=$MaxX : $r"
  return $r
}

function Copy-Enabled {
  $edit = [PanU]::GetSubMenu([PanU]::GetMenu($script:Main), 1)   # File | Edit | View | Help
  [void][PanU]::SendMessageW($script:Main, $WM_INITMENUPOPUP, $edit, [IntPtr]1)
  return (([PanU]::GetMenuState($edit, $IDM_EDIT_COPY, 0) -band 0x3) -eq 0)   # MF_GRAYED|MF_DISABLED
}

# A client point where a 160 px horizontal left-drag selects text at the current
# zoom and pan. Leaves no selection behind (the closing click clears it).
function Find-TextPoint {
  $s = Client-Size
  foreach ($fy in 0.5, 0.35, 0.65, 0.2, 0.8) {
    foreach ($fx in 0.3, 0.5) {
      $p = @([int]($s[0] * $fx), [int]($s[1] * $fy))
      Left-Drag $p (Offset $p 160 0)
      $hit = Copy-Enabled
      Left-Click $p
      if ($hit) { Write-Host "text point: $p"; return $p }
    }
  }
  throw 'no candidate point selects text -- pan onto the text block and retry'
}

function Point-At([int[]]$client) {
  Activate
  $pt = New-Object PanU+POINT
  $pt.X = $client[0]; $pt.Y = $client[1]
  [void][PanU]::ClientToScreen($script:Canvas, [ref]$pt)
  [void][PanU]::SetCursorPos($pt.X, $pt.Y)
  Start-Sleep -Milliseconds 150
}

function Cursor-Is([int]$idc) {
  Start-Sleep -Milliseconds 150
  $ci = New-Object PanU+CURSORINFO
  $ci.cbSize = [System.Runtime.InteropServices.Marshal]::SizeOf($ci)
  [void][PanU]::GetCursorInfo([ref]$ci)
  if ($ci.flags -ne 1) { throw "cursor not showing (flags=$($ci.flags)) -- BLOCKED, not failed" }   # CURSOR_SHOWING
  return ($ci.hCursor -eq [PanU]::LoadCursorW([IntPtr]::Zero, [IntPtr]$idc))
}

# Run $body with Space held. Space goes to the FOREGROUND window, so assert
# litepdf is it; always release, even when $body throws.
function With-Space([scriptblock]$body) {
  Activate
  if ([PanU]::GetForegroundWindow() -ne $script:Main) { throw 'litepdf is not the foreground window' }
  [PanU]::keybd_event(0x20, 0x39, 0, [UIntPtr]::Zero)
  Start-Sleep -Milliseconds 300   # let litepdf read the key before any posted mouse message
  try { & $body } finally { [PanU]::keybd_event(0x20, 0x39, 2, [UIntPtr]::Zero); Start-Sleep -Milliseconds 300 }
}

# WM_CLOSE, then WAIT: the app saves session.json on its way out, and a restore
# copied back before that save lands is overwritten by it.
function Close-LitePdf {
  [void][PanU]::PostMessageW($script:Main, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
  if (-not $script:Proc.WaitForExit(10000)) { throw 'litepdf did not exit within 10 s' }
}
```

**Hygiene for every GUI run.** Before the first launch, back up the live session — the app auto-saves over it within ~1.5 s: `Copy-Item "$env:LOCALAPPDATA\LitePDF\session.json" "$env:TEMP\litepdf-session-58.bak"`. After the run, `Close-LitePdf` (never `Stop-Process`: a force-kill leaves `running.lock` and the next launch offers a restore) and only then copy the backup back. Do not touch the mouse or keyboard while a run is in progress. Confirm the binary under test is the one just built: `(Get-Item .\build\Release\litepdf.exe).LastWriteTime` must be newer than your last source edit.

**The checks.** At the top of the check script:

```powershell
. .\build\gui\pan-drive.ps1
Start-LitePdf @('tests\fixtures\simple.pdf')
Send-Command $IDM_ZOOM_IN 3
Reset-Pan
$C = Center
Middle-Drag $C (Offset $C -200 -200)   # room to pan in every direction
$T = Find-TextPoint                    # also gives the canvas the focus; leaves no selection
```

Each check states what makes it **fail**. Record every `Measure-Shift` line and every cursor result in the task report.

1. **A plain left-drag selects and does not pan** — the control that makes check 2 discriminating. Require `Copy-Enabled` `$false`. `$a = Capture-Canvas; Left-Drag $T (Offset $T 160 0)`; `Copy-Enabled` → `$true`. `Wait-DoubleClick; Left-Click $T`; `Copy-Enabled` → `$false`; `$b = Capture-Canvas; (Measure-Shift $a $b 0 0).Changed` ≤ 0.002 (the page did not move). **Fails if** Copy stays grayed after the drag (then check 2 proves nothing — fix the harness first) or the page moved.
2. **Space + left-drag pans and selects nothing.** `$a = Capture-Canvas; With-Space { Left-Drag $T (Offset $T 160 0) }; $b = Capture-Canvas; Measure-Shift $a $b 160 0` → `Match ≥ 0.98`, `Changed ≥ 0.05`; `Copy-Enabled` still `$false`. **Fails if** it did not pan, or a selection was made.
3. **A Space click keeps the selection.** Undo check 2's pan so `$T` is over the same text again: `Middle-Drag $C (Offset $C -160 0)`. `Left-Drag $T (Offset $T 160 0)`; `Copy-Enabled` → `$true`. `Wait-DoubleClick; With-Space { Left-Click (Offset $T 10 0) }`; `Copy-Enabled` → still `$true`. Control: `Left-Click (Offset $T 10 0)`; `Copy-Enabled` → `$false` (a plain click clears — proving the click reached the canvas). **Fails if** the Space click cleared the selection.
4. **A Space press is not a click** (P10). Control first — a real double click at `$T` selects: `Wait-DoubleClick; Left-Click $T; Post-Mouse $WM_LBUTTONDBLCLK $T $MK_LBUTTON; Post-Mouse $WM_LBUTTONUP $T 0`; `Copy-Enabled` → `$true`. `Wait-DoubleClick; Left-Click $T`; `Copy-Enabled` → `$false`. Then the check — the same pair, with the first press made under Space: `Wait-DoubleClick; With-Space { Left-Click $T }; Post-Mouse $WM_LBUTTONDBLCLK $T $MK_LBUTTON; Post-Mouse $WM_LBUTTONUP $T 0`; `Copy-Enabled` → `$false`. **Fails if** Copy is enabled: the pan press paired with the next one into a double click and selected a word. If the control does not select, pick another point (`Find-TextPoint` proves a drag selects, not that a double click at exactly `$T` lands on a word) and re-run.
5. **Space + left-drag pans in spread mode** (P3). `Send-Command $IDM_ZOOM_RESET; Send-Command $IDM_VIEW_DUAL_PAGE; Send-Command $IDM_ZOOM_IN 4; Reset-Pan; $s = Client-Size; $mx = [int]($s[0] * 0.45) - 100`. The single page is drawn in the left slot and clipped to its half of the canvas (the spread branch of `on_paint` pushes one axis-aligned clip per slot), so after a pan to the left, the right edge of that half shows page where the earlier capture showed surround; `-MaxX $mx` keeps the samples clear of that band. `$a = Capture-Canvas; With-Space { Left-Drag $C (Offset $C -100 -80) }; $b = Capture-Canvas; Measure-Shift $a $b -100 -80 -MaxX $mx` → `Match ≥ 0.98`, `Changed ≥ 0.01`. Negative control, no Space: `$a = Capture-Canvas; Left-Drag $C (Offset $C -100 -80); $b = Capture-Canvas; (Measure-Shift $a $b 0 0).Changed` ≤ 0.002 over the whole canvas (spread mode refuses a selection, and a plain drag does not pan). **Fails if** the Space drag does not pan — the Space branch sits after the spread refusal. If *both* show `Changed` ~0, first confirm the spread overflows (`Middle-Drag` must move it; if it does not, `Send-Command $IDM_ZOOM_IN` once more and repeat). Finish with `Send-Command $IDM_VIEW_DUAL_PAGE; Send-Command $IDM_ZOOM_RESET; Send-Command $IDM_ZOOM_IN 3; Reset-Pan; Left-Click $C`.
6. **Space alone changes the hover cursor** (P6, P7). `Point-At $C`; `Cursor-Is $IDC_IBEAM` `$true`. `With-Space { Cursor-Is $IDC_SIZEALL }` → `$true` — the pointer never moved, so only the `WM_KEYDOWN` refresh can have changed it. After `With-Space` returns: `Cursor-Is $IDC_IBEAM` → `$true` (the `WM_KEYUP` refresh). Negative control, content that fits: `Send-Command $IDM_ZOOM_OUT 8; Left-Click (Center); Point-At (Center)`; `Cursor-Is $IDC_IBEAM` `$true`; `With-Space { Cursor-Is $IDC_ARROW }` → `$true`; afterwards `Cursor-Is $IDC_IBEAM` `$true`. **Fails if** Space leaves the I-beam over pannable content, or shows the move cursor over content that fits.
7. **Space pressed mid-selection keeps the drag a selection.** `Send-Command $IDM_ZOOM_RESET; Send-Command $IDM_ZOOM_IN 3; Reset-Pan; Middle-Drag $C (Offset $C -200 -200); $T = Find-TextPoint`; require `Copy-Enabled` `$false`. `$a = Capture-Canvas; Post-Mouse $WM_LBUTTONDOWN $T $MK_LBUTTON; Post-Mouse $WM_MOUSEMOVE (Offset $T 80 0) $MK_LBUTTON; With-Space { Post-Mouse $WM_MOUSEMOVE (Offset $T 160 0) $MK_LBUTTON; Post-Mouse $WM_LBUTTONUP (Offset $T 160 0) 0 }`; `Copy-Enabled` → `$true`. `Wait-DoubleClick; Left-Click $T; $b = Capture-Canvas; (Measure-Shift $a $b 0 0).Changed` ≤ 0.002. **Fails if** Copy stays grayed (Space turned a live selection into something else) or the page moved.

`Close-LitePdf` (it waits for the process to exit); then restore `session.json`.

- [ ] **Step 6: Commit**

```bash
git add src/ui/PdfCanvas.cpp
git commit -m "feat(canvas): pan by dragging with the left button while space is held

Space is read with GetKeyState at the press, never latched, so releasing
it in another window cannot leave the canvas panning. The branch sits
before the selection refusals: a pan needs no text and works in spread
mode. Space down and up re-run the cursor logic, so the move cursor appears
without waiting for the pointer to move.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 4: Docs, final verification, merge gate

**Files:**
- Modify: `CHANGELOG.md`
- Modify: `README.md`

**Interfaces:**
- Consumes: the shipped behaviour of Tasks 2 and 3.
- Produces: the PR.

- [ ] **Step 1: CHANGELOG**

Under `## [Unreleased]` → `### Added`, after the text-selection bullet:

```markdown
- Hand-tool panning. Drag with the middle mouse button, or hold Space and drag
  with the left button, to move a zoomed-in page; the pointer shows a move
  cursor whenever there is something to move. Works in two-page spread mode
  too. Dragging with the left button alone still selects text.
```

- [ ] **Step 2: README**

In `## Features (v1.3.0)`, after the "Text selection and copy" bullet:

```markdown
- **Hand-tool panning** — middle-drag, or hold Space and drag, to move a zoomed-in page; the arrow keys pan too (unreleased)
```

In the `## Keyboard shortcuts` table, after the `PgDn / PgUp` row:

```markdown
| Arrow keys         | Pan a zoomed-in page                |
| Space + drag       | Pan a zoomed-in page (middle-drag also pans) |
```

- [ ] **Step 3: Verify the claims against the artifact**

Run `build\Release\litepdf.exe tests\fixtures\simple.pdf`, zoom in, and read each new bullet and row next to the running window: middle-drag, Space + drag, the move cursor, spread mode (Ctrl+Shift+D), a plain left drag still selecting, the arrow keys. A CHANGELOG bullet once shipped false (PR #43); this step is why.

- [ ] **Step 4: Full verification**

```bash
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Expected: **N0 + 7** passing. Record the count and `(Get-Item build\Release\litepdf.exe).Length`; it must be under 19,000,000 bytes.

- [ ] **Step 5: Commit**

```bash
git add CHANGELOG.md README.md
git commit -m "docs: record hand-tool panning

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

- [ ] **Step 6: Merge gate, then PR**

Invoke the `risk-tiered-review` skill (never drop the Codex lens). Name these least-certain claims for the adversarial lens:

1. **P2's premise** — that no `WM_SETCURSOR` reaches a window holding the capture, so the cursor must be set at the press. Is there a path where `update_cursor` runs mid-pan and changes the shape wrongly (the `WM_KEYDOWN` refresh during a middle pan, a `WM_SETCURSOR` from a non-client hit-test)?
2. **P9's stale-pan cancel** — `on_mouse_move` cancels a pan when `GetCapture() != hwnd_`. Is there a legitimate pan during which `GetCapture()` does not return the canvas (a press on a background window, a pan started by a posted message), so the first move ends a pan the user is still dragging?
3. **Re-entrancy** — `cancel_gesture` can now run from inside `on_mouse_move`, which `on_left_button_up` and `on_middle_button_up` call before `release()`. Walk each: after a cancel there, does `release()` return `None`, and is the capture released exactly once?
4. **The Space branch's position** — it runs after the click counter and `cancel_stale_gesture`, before the spread / bitmap / text refusals, and calls `ClickCounter::forget()`. Can a Space press ever start a selection, a plain press ever start a pan, or a pan press still pair into a double or triple click?

Push, open the PR titled `feat: hand-tool panning`, body with the test count, exe size, every GUI check's `Measure-Shift` line and cursor result, the plan-time corrections P1-P10 in one line each, the limitations R15-R18, and `Closes #58`.
