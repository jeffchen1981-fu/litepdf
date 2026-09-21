# Horizontal wheel scrolling (#56) — design

Date: 2026-09-21
Base: `main` @ `1d6f83845eef1951178e5f582e6b600c27e3c140`
Issue: [#56](https://github.com/jeffchen1981-fu/litepdf/issues/56)

A zoomed-in page can be panned horizontally with the arrow keys and the hand tool,
but not with the mouse wheel. `WM_MOUSEHWHEEL` is handled nowhere in `src/`, and
the canvas's `WM_MOUSEWHEEL` arm (`src/ui/PdfCanvas.cpp:1023-1043`) tests only
`MK_CONTROL`: a tilt wheel or a horizontal touchpad swipe does nothing, and
Shift+wheel scrolls **vertically**, as if Shift were not held.

The state is already there. `PdfCanvas::pan_by(dx, dy)` (`:1339`) moves and clamps
both axes, `VK_LEFT`/`VK_RIGHT` already drive it (`:1651-1652`), and the paint path
already applies `pan_x`. This is an input-routing change plus one pure helper.

One PR. `VERSION` is not bumped (`project_litepdf_ship_version_convention`).

---

## 1. Behaviour

| Input | Today | After |
|---|---|---|
| `WM_MOUSEHWHEEL` (tilt wheel, horizontal touchpad swipe) | nothing | scrolls horizontally |
| Shift+wheel | scrolls vertically | scrolls horizontally |
| Ctrl+Shift+wheel | zooms | zooms (unchanged) |
| Plain wheel, Ctrl+wheel | unchanged | unchanged |

When the page fits the window horizontally — FitWidth, the default, and FitPage —
both horizontal inputs do nothing.

## 2. Decisions

**H1. Both inputs.** A tilt wheel or a precision touchpad produces
`WM_MOUSEHWHEEL`; a plain wheel can only reach horizontal scrolling through
Shift+wheel. Supporting only one would leave one class of hardware without a
horizontal mouse route.

**H2. Ctrl beats Shift, and `WM_MOUSEHWHEEL` ignores modifiers.** The existing
`MK_CONTROL` test stays first, so Ctrl+Shift+wheel keeps zooming. There is no
horizontal zoom gesture, so a modifier on `WM_MOUSEHWHEEL` changes nothing.

**H3. Shift+wheel does not fall back to vertical scrolling when there is nothing
to scroll horizontally.** This is a deliberate behaviour change. A fallback would
make one gesture scroll along different axes depending on the zoom level: Shift+wheel
would scroll vertically at FitWidth, then switch to horizontal the moment the reader
zoomed in. One gesture, one axis.

**H4. Direction.** Content to the right is revealed by:
- `WM_MOUSEHWHEEL` with delta > 0 (the wheel tilted right, per the Win32 docs);
- Shift+wheel with delta < 0 (wheel rotated toward the user).

Both move `pan_x` in the negative direction, the same way `VK_RIGHT` does
(`pan_by(-100, 0)`). Each input is first converted to a delta where positive means
"toward the right", then accumulated.

**H5. Step size mirrors the vertical wheel.** This reuses `consume_notches` and
`wheel_step_dip` from `src/ui/detail/ScrollMath.hpp`.
- The horizontal axis has its **own** residual (`Impl::hwheel_residual`), separate
  from `wheel_residual`. A diagonal touchpad swipe must not add horizontal motion to
  the vertical residual, and the vertical residual's clearing rules (the pending-flip
  latch and the bitmap-identity guard) do not apply to an axis that never flips.
- Each input reads the setting for the control the user actually moved:
  `WM_MOUSEHWHEEL` → `SPI_GETWHEELSCROLLCHARS`, Shift+wheel →
  `SPI_GETWHEELSCROLLLINES`. Both default to 3, giving 48 DIP per notch, the same as
  the vertical wheel.
- The "one screen at a time" sentinel (`kWheelPageScroll`) scrolls 90 % of the
  viewport **width**. A setting of 0 scrolls nothing. If `SystemParametersInfoW`
  fails, the value stays at 3, matching `on_wheel_scroll` (`:1406-1407`).
- `wheel_step_dip`'s third parameter is renamed `vp_h` → `vp_extent`. The body is
  already independent of the axis.

**H6. At the horizontal edge the pan clamps. It never turns the page.** Pages are
stacked vertically, and a spread's right page is already inside the content box the
horizontal pan moves through. `PageAnchor` has only vertical kinds. A horizontal
page flip would have no analogue anywhere else in the app.

**H7. The horizontal path takes the vertical wheel's bitmap-identity guard, but
not its pending-flip latch.** The vertical wheel has two guards, and both stay
exactly as they are (decision #4 in `project_zoom_pagenav_spec_shipped`, not to be
reversed).

- **The bitmap-identity guard applies** (`PdfCanvas.cpp:1387-1397`).
  - `pan_by` measures against `current_bitmap` and clamps **both** axes
    (`:1339-1347`). A tab switch keeps painting the outgoing document's bitmap
    until the incoming render lands: `set_view` resets `current_bitmap` only for a
    null view (`:330-337`), and `MainWindow` restores the incoming tab's pan right
    after `set_view` (`MainWindow.cpp:662-663`).
  - Without the guard, a horizontal notch in that window would clamp the incoming
    tab's restored `pan_y` to the outgoing document's range. `apply_anchor(None)`
    only re-clamps, so the reader's vertical position in that tab would be lost.
  - So the predicate at `:1387-1397` moves, unchanged, into a private helper
    (`bool bitmap_is_stale() const`) that both wheels call. Unchanged means:
    `current_bitmap` non-null AND (`bitmap_epoch != view_epoch` OR `bitmap_page` !=
    the **canonical** current page, using `dual_page_compute_left` in spread mode).
  - When it is true, the horizontal path zeroes `hwheel_residual` and drops the
    notch, as the vertical path does.
  - Use this helper, **not** `own_bitmap()` (`:546-550`). `own_bitmap()` compares
    against the raw `current_page` and is also false when there is no bitmap, so it
    is a different predicate.
- **The `wheel_flip_seq` latch does not apply.** It exists only to stop a notch from
  **turning the page again** while a flip is in flight. A horizontal notch cannot
  turn the page. The horizontal path never reads or writes `wheel_flip_seq` or
  `wheel_residual`.

**Single-page mode only:** `pan_x` is **not** reset by a page flip. `apply_anchor`
sets `pan_y` and only re-clamps `pan_x` (`:1521`). A reader who has scrolled right to
follow a column therefore keeps that column when the wheel turns the page. This
already happens today with the arrow keys. The design relies on it and must not
change it. Spread mode does not keep it; see R4.

**H8. `WM_MOUSEHWHEEL` returns `TRUE`, always, including when no document is
open.** The documentation disagrees with itself:
- the current Win32 reference says to return zero, and Chromium returns 0;
- Microsoft's 2004 device guidance says TRUE must be returned. Drivers that
  *emulate* the message (IntelliPoint, IntelliType Pro) read the result, and without
  TRUE "the horizontal scroll action may be repeated". Firefox followed that guidance.

Windows' own delivery of `WM_MOUSEHWHEEL` is not documented to act on the value
(unverified), so TRUE is expected to cost nothing and protects against emulating
drivers. The Shift+wheel arm keeps returning 0, like the rest of the
`WM_MOUSEWHEEL` arm.

The rule is enforced in **one place**, the canvas. Any window that forwards the
message returns whatever the canvas returned (H9). A forwarder that returned its own
0 would bring back the repeat that H8 exists to prevent, and that forwarder is the
window an emulating driver talks to.

**H9. The page box forwards `WM_MOUSEHWHEEL` to the canvas too, and returns the
canvas's result.** The status bar's EDIT subclass already forwards `WM_MOUSEWHEEL`
(`src/ui/StatusBar.cpp:486-495`). Its reason: when a wheel message is delivered to
the focused window and the caret is in the page box, the canvas would otherwise never
see it. (With "Scroll inactive windows when I hover over them" on, Windows may deliver
to the window under the pointer instead. The forward covers the other case and costs
nothing in this one.)

`DefWindowProc` passes an unhandled `WM_MOUSEHWHEEL` up the **parent** chain (EDIT →
status bar → main window), never to the canvas, which is a sibling. So the same
forwarding is needed. `StatusBar::OnWheel` becomes
`std::function<LRESULT(UINT msg, WPARAM, LPARAM)>`:
- it carries the message id, so one callback serves both messages;
- `MainWindow`'s callback returns its `SendMessageW` result;
- the EDIT arm returns that result.

For `WM_MOUSEWHEEL` the result is 0 as before.

## 3. Code changes

| File | Change |
|---|---|
| `src/ui/detail/ScrollMath.hpp` | `enum class HWheelSource { Tilt, Shift }`; `rightward_delta(int raw, HWheelSource)` (H4); `wheel_step_dip` parameter rename (H5); header comment extended to the horizontal sign convention |
| `src/ui/PdfCanvas.hpp` | `LRESULT on_hwheel_scroll(int raw_delta, HWheelSource src);` next to `on_wheel_scroll`; `bool bitmap_is_stale() const;` (H7) |
| `src/ui/PdfCanvas.cpp` | `Impl::hwheel_residual`; `bitmap_is_stale()` holding the predicate moved out of `on_wheel_scroll` (`:1387-1397`), which now calls it (behaviour-preserving); new `case WM_MOUSEHWHEEL` returning TRUE (H8); `MK_SHIFT` branch in the `WM_MOUSEWHEEL` arm, after the `MK_CONTROL` branch (H2); `on_hwheel_scroll` = no view → return; `bitmap_is_stale()` → zero `hwheel_residual`, return → `rightward_delta` → `consume_notches(hwheel_residual)` → SPI read per source → `wheel_step_dip(…, vp.width)` → `pan_by(-step, 0)` |
| `src/ui/StatusBar.hpp` / `.cpp` | `OnWheel` = `LRESULT(UINT msg, WPARAM, LPARAM)`; the EDIT subclass handles `WM_MOUSEWHEEL` and `WM_MOUSEHWHEEL` together and returns the callback's result (H9) |
| `src/ui/MainWindow.cpp` | the `set_on_wheel` callback (`:1154-1160`) sends the message it was given and returns the `SendMessageW` result (0 when there is no canvas) |
| `tests/unit/test_scroll_math.cpp` | §4.1 |
| `CHANGELOG.md` | `[Unreleased]` → `Added`: horizontal wheel scrolling, noting the Shift+wheel behaviour change |
| `README.md` | the mouse-wheel feature bullet (`:44`) mentions Shift+wheel and the tilt wheel; one shortcut-table row: `Shift+wheel` → "Scroll a zoomed-in page sideways (a tilt wheel or touchpad also works)" |

## 4. Verification

### 4.1 Unit tests (`test_scroll_math.cpp`)

Test names are ASCII and start with `ScrollMath`
(`reference_litepdf_ctest_ascii_test_names`).

- `rightward_delta`: Tilt +120 → +120; Tilt −120 → −120; Shift −120 (wheel toward
  the user) → +120; Shift +120 → −120.
- `rightward_delta` + `consume_notches` on a residual: a Shift sequence and a Tilt
  sequence that describe the same motion produce the same notches.

The `wheel_step_dip` rename needs no new test: the existing sentinel test
(`test_scroll_math.cpp:81-86`) already covers the body, and a rename cannot make it
fail.

Run with `ctest --test-dir build -C Release`. Release build only; Debug fails with
LNK2038 (`reference_litepdf_build_test_commands`).

### 4.2 GUI probe (scripted, no human needed)

Throwaway-worktree probe that appends `pan_x pan_y` to a log after every wheel
message (`reference_litepdf_scripted_gui_smoke`). Messages are posted with
`PostMessage`. The Shift branch reads `MK_SHIFT` from wParam, not from
`GetKeyState`, so a posted message exercises it faithfully. Every row names the
observation that would make it fail.

**Each row starts from its own Setup, not from the previous row's end state.** "Zoomed"
means single-page mode, one zoom rung above FitWidth (so the page overflows on both
axes), `pan_x` = `pan_y` = 0, unless the row says otherwise.

| # | Setup | Input | Pass | Fails if |
|---|---|---|---|---|
| 1 | zoomed | `WM_MOUSEHWHEEL` +120 | `pan_x` = −48.00, `pan_y` = 0 | `pan_x` stays 0, or moves positive |
| 2 | zoomed | Shift+wheel −120 | `pan_x` = −48.00, `pan_y` = 0 | `pan_y` moves (today's behaviour) |
| 3 | zoomed | `WM_MOUSEHWHEEL` +120, repeated past the edge | `pan_x` stops at exactly `vp_w − box_w`; the page does not change | overshoot, or the page flips |
| 4 | zoomed | `WM_MOUSEHWHEEL` +40 ×3 | `pan_x` 0, 0, then −48.00 | `pan_x` moves on the first or second message |
| 5 | FitWidth (negative control) | both inputs | `pan_x` = `pan_y` = 0 throughout | anything moves |
| 6 | zoomed (positive control) | plain wheel −120 | `pan_y` moves, `pan_x` unchanged | nothing moves (driver or probe broken) |
| 7 | zoomed, on a document whose pages all share one size; `pan_x` = −48 (via row 1) | plain wheel until the page turns | `pan_x` = −48.00 on the new page | `pan_x` reset to 0 |
| 8 | zoomed, caret in page box | `WM_MOUSEHWHEEL` +120 **sent to the EDIT** | canvas `pan_x` = −48.00 | nothing moves |
| 9 | zoomed | `WM_MOUSEHWHEEL` `SendMessage`d to the **canvas** | result = 1 | 0 |
| 9b | zoomed, caret in page box | `WM_MOUSEHWHEEL` `SendMessage`d to the **EDIT** | result = 1 | 0 (the forwarder swallowed the canvas's TRUE) |
| 10 | tab A zoomed with `pan_y` < 0; tab B at FitWidth, active; A uses a slow-rendering page (`large.pdf`, high zoom) | `SendMessage` `WM_COMMAND IDM_TAB_NEXT` to the main window, then immediately `SendMessage` `WM_MOUSEHWHEEL` +120 to the canvas | the probe logs the notch as dropped by `bitmap_is_stale()`, and after A's render lands `pan_y` equals A's saved value | `pan_y` differs from the saved value. **VOID, not PASS,** if no drop was logged (the render landed first, so the window was never exercised) |

### 4.3 Real hardware (user)

Only a plain wheel is available, so the user checks rows 1-3 using real
**Shift+wheel**: wheel toward you scrolls right, away scrolls left, it stops at the
edge, and Ctrl+Shift+wheel still zooms.

## 5. Residual risks

- **R1. The `WM_MOUSEHWHEEL` sign is not checked on real hardware.** No tilt wheel
  or precision touchpad is available. The sign comes from the Win32 documentation
  (positive = right), and §4.2 proves only that the code follows it.
- **R2. Emulating drivers (Logitech SetPoint and similar) are untested.** H8 is the
  mitigation. Chromium also carries a workaround for Logitech drivers that follow a
  `WM_MOUSEHWHEEL` with a stray `WM_MOUSEWHEEL` carrying the same message time
  (`hwnd_message_handler.cc`). That workaround is **not** adopted, because without
  hardware it cannot be tested. The failure it prevents is an extra vertical notch,
  and on a page that fits vertically one notch **turns the page**
  (`ScrollMath.hpp:81-84`). So the visible failure is an unwanted page turn during a
  tilt. File an issue if a user reports it.
- **R3. A wheel notch during a live selection drag moves the page under a stationary
  pointer, and the selection's extent updates on the next mouse move.** This is
  already true of the vertical wheel. It is inherited, not introduced, and not fixed
  here.
- **R4. In spread mode a page flip can reset `pan_x`.** Spread-mode
  `navigate_to_page` resets both bitmaps (`PdfCanvas.cpp:1548-1554`). When the left
  half lands first, `content_extent` measures the left page alone (`:1320-1326`) and
  `apply_anchor` re-clamps `pan_x` against it (`:1521`). At a zoom where the pair
  overflows the window but one page does not, that clamp returns 0, and the right
  half landing later cannot restore it.
  - This is the existing behaviour with the arrow keys and is outside #56. H7's
    column-keeping claim is scoped to single-page mode for this reason.
  - Also in spread mode: a horizontal notch while both bitmaps are reset is consumed
    and does nothing, because `content_extent` fails. The only cost is the lost notch.
- **R5. The arrow keys and the hand tool have the tab-switch exposure that H7 closes
  for the wheel.** They call `pan_by` with no bitmap-identity guard. A keystroke or
  drag in the window between `set_view` and the incoming render can therefore clamp
  the restored pan to the outgoing document's range.
  - This predates #56 and is not fixed here, so that this PR does not change keyboard
    or drag behaviour.
  - It is a follow-up, to be filed as its own issue.

## 6. Out of scope

- A horizontal scroll **bar**. The canvas has none, in either direction.
- Smooth / animated scrolling, and partial-notch (sub-48 DIP) steps. Both axes
  scroll in whole notches today; changing that is a separate decision covering both.
- Horizontal page turning (H6).
- Resetting the residual on focus change, which the 2004 guidance recommends. The
  vertical wheel does not do it either, and the two axes should stay consistent.
