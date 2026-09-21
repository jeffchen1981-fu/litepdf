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
already applies `pan_x`. This is an input-routing change, plus one pure helper and
one guard shared with the vertical wheel. The horizontal step writes `pan_x` alone
rather than going through `pan_by`; see H7.

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
  the vertical residual. The two are also cleared by different rules:
  - the vertical residual is also cleared by the pending-flip latch, which cannot
    apply to an axis that never flips;
  - the horizontal residual is cleared only by the bitmap-identity guard (H7).
- Each input reads the setting for the control the user actually moved:
  `WM_MOUSEHWHEEL` → `SPI_GETWHEELSCROLLCHARS`, Shift+wheel →
  `SPI_GETWHEELSCROLLLINES`. Both default to 3, giving 48 DIP per notch, the same as
  the vertical wheel.
- The "one screen at a time" sentinel (`kWheelPageScroll`) scrolls 90 % of the
  viewport **width**. A setting of 0 scrolls nothing. If `SystemParametersInfoW`
  fails, the value stays at 3, matching `on_wheel_scroll` (`:1406-1407`).
- The sentinel is documented only for the **lines** setting. The chars setting has
  no page value, and `UINT_MAX` there would be treated as one screen only because
  `wheel_step_dip` is shared. That is accepted, not designed for: the value cannot be
  set through any Windows UI.
- `wheel_step_dip`'s third parameter is renamed `vp_h` → `vp_extent`. The body is
  already independent of the axis.

**H6. At the horizontal edge the pan clamps. It never turns the page.** Pages are
stacked vertically, and a spread's right page is already inside the content box the
horizontal pan moves through. `PageAnchor` has only vertical kinds. A horizontal
page flip would have no analogue anywhere else in the app.

**H7. A horizontal notch writes `pan_x` only. It takes the vertical wheel's
bitmap-identity guard, but not its pending-flip latch.**

**It writes `pan_x` only, so it does not call `pan_by`.** `pan_by` clamps **both**
axes (`:1339-1347`), so a horizontal notch through it would also re-clamp `pan_y`.
That happens against whatever bitmap and viewport are current, for example right
after a resize and before the replacement render lands. The vertical wheel already
writes `pan_y` only (`:1410-1413`), and the horizontal wheel mirrors it:
`content_extent(box)`, then `pan_x = clamp_pan(pan_x - step, box.w, vp.width)`, then
invalidate.

The vertical wheel has two guards. Both stay exactly as they are (decision #4 in
`project_zoom_pagenav_spec_shipped`, not to be reversed).

- **The bitmap-identity guard applies** (`PdfCanvas.cpp:1387-1397`).
  - A tab switch keeps painting the outgoing document's bitmap until the incoming
    render lands: `set_view` resets `current_bitmap` only for a null view
    (`:330-337`), and `MainWindow` restores the incoming tab's pan right after
    `set_view` (`MainWindow.cpp:662-663`).
  - Without the guard, a horizontal notch in that window would clamp the incoming
    tab's restored `pan_x` to the outgoing document's width. If the outgoing tab
    fits horizontally, that is 0. `apply_anchor(None)` only re-clamps, so the column
    the reader had scrolled to in that tab would be lost.
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

Only a **sender** can read the value, which is why TRUE is chosen:
- Real wheel input is posted to the thread's queue. Every message loop in litepdf
  discards `DispatchMessageW`'s result (`MainWindow.cpp:2353`, `PrintJob.cpp:76`,
  `PrintProgressDlg.cpp:333`), so for real input the value reaches nobody.
- The value is read only by code that `SendMessage`s the message. That means an
  emulating driver, or litepdf's own forwarder (H9). For an emulating driver, the
  only documented meaning is TRUE = handled.

TRUE is therefore expected to cost nothing, and it guards against repeats. The
Shift+wheel arm keeps returning 0, like the rest of the `WM_MOUSEWHEEL` arm.

*Considered and rejected at the spec gate:* both Codex lenses asked for 0, citing the
current reference. Returning 0 buys compliance with a sentence no posted message can
observe, and it risks the repeat that the only sender-facing guidance warns about. No
lens named a failure that TRUE causes. If one turns up, the change is a single
return value.

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
| `src/ui/PdfCanvas.cpp` | `Impl::hwheel_residual`; `bitmap_is_stale()` holding the predicate moved out of `on_wheel_scroll` (`:1387-1397`), which now calls it (behaviour-preserving); new `case WM_MOUSEHWHEEL` returning TRUE (H8); `MK_SHIFT` branch in the `WM_MOUSEWHEEL` arm, after the `MK_CONTROL` branch (H2); `on_hwheel_scroll` = no view → return; `bitmap_is_stale()` → zero `hwheel_residual`, return → `rightward_delta` → `consume_notches(hwheel_residual)`, 0 notches → return → `content_extent(box)` fails → return → SPI read per source → `wheel_step_dip(…, vp.width)` → `pan_x = clamp_pan(pan_x - step, box.w, vp.width)` → invalidate. It never calls `pan_by` and never writes `pan_y` (H7) |
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

The plan (`docs/superpowers/plans/2026-09-21-horizontal-wheel-scroll-56.md`,
corrections C1-C8) refines the mechanics below. It does not change what any row
tests. In particular: the probe is reverted with `git checkout`, not run in a
worktree; a flag-gated render delay opens the race window for rows 10 and 11; and
row 12 (Ctrl+Shift+wheel still zooms) is added.

A throwaway-worktree probe (`reference_litepdf_scripted_gui_smoke`) appends one
line per event to a log file:
- **for every wheel message the canvas handles:** the message id, the wParam key
  state, `current_page()`, `pan_x` and `pan_y` on entry **and** on exit, and which
  branch ran: `stale-drop`, `fraction` (0 notches), `no-extent`, or `step <dip>`.
  A row's "`pan_x` = …" means the exit value;
- **for every render completion the canvas accepts:** the page.

Rows read the branch and the page from this log, not from the pan alone. That is what
lets row 3 prove the page did not change, row 7 prove it did, and rows 10-11 prove
their race window was actually exercised.

Messages are posted with `PostMessage` unless a row says `SendMessage`. The Shift
branch reads `MK_SHIFT` from wParam, not from `GetKeyState`, so a posted message
exercises it faithfully.

**Expected steps come from the machine's settings, not from constants.** The driver
first reads `SPI_GETWHEELSCROLLCHARS` (C) and `SPI_GETWHEELSCROLLLINES` (L) and
records them. A tilt step is C × 16 DIP and a Shift step is L × 16 DIP. The table
writes 48, the value when both are 3, as they are on the development machine.
Substitute if they differ.

**Each row starts from its own Setup, not from the previous row's end state.** "Zoomed"
means single-page mode, one zoom rung above FitWidth (so the page overflows on both
axes), `pan_x` = `pan_y` = 0, unless the row says otherwise.

| # | Setup | Input | Pass | Fails if |
|---|---|---|---|---|
| 1 | zoomed | `WM_MOUSEHWHEEL` +120 | `pan_x` = −48.00, `pan_y` = 0 | `pan_x` stays 0, or moves positive |
| 2 | zoomed | Shift+wheel −120 | `pan_x` = −48.00, `pan_y` = 0 | `pan_y` moves (today's behaviour) |
| 3 | zoomed | `WM_MOUSEHWHEEL` +120, repeated past the edge | `pan_x` stops at exactly `vp_w − box_w`; logged page constant throughout | overshoot, or the logged page changes |
| 4 | zoomed | `WM_MOUSEHWHEEL` +40 ×3 | branches `fraction`, `fraction`, `step 48`; `pan_x` 0, 0, then −48.00 | a `step` on the first or second message |
| 5 | FitWidth (negative control) | both inputs | `pan_x` = `pan_y` = 0 throughout | anything moves |
| 6 | zoomed (positive control) | plain wheel −120 | `pan_y` moves, `pan_x` unchanged | nothing moves (driver or probe broken) |
| 7 | zoomed, on a document whose pages all share one size; `pan_x` = −48 (via row 1) | plain wheel until the page turns | logged page = the next page, and `pan_x` = −48.00 after its completion | `pan_x` reset to 0 |
| 8 | zoomed, caret in page box | `WM_MOUSEHWHEEL` +120 **sent to the EDIT** | canvas `pan_x` = −48.00 | nothing moves |
| 9 | zoomed | `WM_MOUSEHWHEEL` `SendMessage`d to the **canvas** | result = 1 | 0 |
| 9b | zoomed, caret in page box | `WM_MOUSEHWHEEL` `SendMessage`d to the **EDIT** | result = 1 | 0 (the forwarder swallowed the canvas's TRUE) |
| 10 | tab A zoomed with `pan_x` < 0; tab B at FitWidth, active; A uses a slow-rendering page (`large.pdf`, high zoom) | `SendMessage` `WM_COMMAND IDM_TAB_NEXT` to the main window, then immediately `SendMessage` `WM_MOUSEHWHEEL` +120 to the canvas | branch `stale-drop`, and after A's completion `pan_x` equals A's saved value | `pan_x` differs from the saved value. **VOID, not PASS,** if A's completion is logged before the notch |
| 11 | zoomed, `pan_y` at the bottom (`vp_h − box_h`), `pan_x` = 0 | `SetWindowPos` making the main window taller, then immediately `SendMessage` `WM_MOUSEHWHEEL` +120 to the canvas | branch `step 48`; the notch's exit `pan_y` equals its entry `pan_y` | exit `pan_y` ≠ entry `pan_y` (a `pan_by`-based step would re-clamp it against the taller viewport). **VOID** if a completion is logged between the resize and the notch |

### 4.3 Real hardware (user)

Only a plain wheel is available, so the real-device check covers the **Shift+wheel
branch only**. On a zoomed page:
- wheel toward you scrolls right, and wheel away scrolls left;
- it stops at the edge without turning the page;
- plain wheel still scrolls vertically;
- Ctrl+Shift+wheel still zooms.

It does **not** exercise the `WM_MOUSEHWHEEL` branch. Real Shift+wheel arrives as
`WM_MOUSEWHEEL`, so a wrong sign, wrong routing or wrong return value on the native
path would pass this check. That branch is covered only by §4.2's posted messages;
see R1.

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
