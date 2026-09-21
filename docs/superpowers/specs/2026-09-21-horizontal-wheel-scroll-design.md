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

**H7. The horizontal path takes none of the vertical wheel's flip guards.** It
reads and writes neither `wheel_flip_seq` nor `wheel_residual`, and it does not run
the bitmap-identity test. Those guards exist to stop a notch from **turning the page
again** while a flip is in flight (decision #4 in
`project_zoom_pagenav_spec_shipped`, not to be reversed). A horizontal notch cannot
flip, so it behaves exactly like `VK_LEFT`/`VK_RIGHT`, which also go straight to
`pan_by`. A horizontal notch that arrives while a flip is pending is clamped
against the outgoing bitmap, and `apply_anchor` re-clamps `pan_x` when the new page
lands (`:1521`).

`pan_x` is **not** reset by a page flip. `apply_anchor` sets `pan_y` and only
re-clamps `pan_x`. A reader who has scrolled right to follow a column therefore keeps
that column when the wheel turns the page. This already happens today with the arrow
keys; the design relies on it and must not change it.

**H8. `WM_MOUSEHWHEEL` returns `TRUE`, always, including when no document is
open.** The documentation disagrees with itself:
- the current Win32 reference says to return zero, and Chromium returns 0;
- Microsoft's 2004 device guidance says TRUE must be returned. Drivers that
  *emulate* the message (IntelliPoint, IntelliType Pro) read the result, and without
  TRUE "the horizontal scroll action may be repeated". Firefox followed that guidance.

Native Windows ignores the value, so TRUE costs nothing and protects against
emulating drivers. The Shift+wheel arm keeps returning 0, like the rest of the
`WM_MOUSEWHEEL` arm.

**H9. The page box forwards `WM_MOUSEHWHEEL` to the canvas too.** The status bar's
EDIT subclass already forwards `WM_MOUSEWHEEL` (`src/ui/StatusBar.cpp:486-495`),
because a wheel message goes to the focused window, and with the caret in the page box
the canvas would never see it. `DefWindowProc` passes an unhandled `WM_MOUSEHWHEEL` up
the **parent** chain (EDIT → status bar → main window), never to the canvas, which is a
sibling. So the same forwarding is needed. `StatusBar::OnWheel` gains the message
id, so one callback carries both messages.

## 3. Code changes

| File | Change |
|---|---|
| `src/ui/detail/ScrollMath.hpp` | `enum class HWheelSource { Tilt, Shift }`; `rightward_delta(int raw, HWheelSource)` (H4); `wheel_step_dip` parameter rename (H5); header comment extended to the horizontal sign convention |
| `src/ui/PdfCanvas.hpp` | `LRESULT on_hwheel_scroll(int raw_delta, HWheelSource src);` next to `on_wheel_scroll` |
| `src/ui/PdfCanvas.cpp` | `Impl::hwheel_residual`; new `case WM_MOUSEHWHEEL` returning TRUE (H8); `MK_SHIFT` branch in the `WM_MOUSEWHEEL` arm, after the `MK_CONTROL` branch (H2); `on_hwheel_scroll` = `rightward_delta` → `consume_notches(hwheel_residual)` → SPI read per source → `wheel_step_dip(…, vp.width)` → `pan_by(-step, 0)` |
| `src/ui/StatusBar.hpp` / `.cpp` | `OnWheel` = `void(UINT msg, WPARAM, LPARAM)`; the EDIT subclass handles `WM_MOUSEWHEEL` and `WM_MOUSEHWHEEL` together (H9) |
| `src/ui/MainWindow.cpp` | the `set_on_wheel` callback (`:1154-1160`) sends the message it was given |
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
- `wheel_step_dip` with the page sentinel and a width-sized extent returns 0.9 ×
  that extent. This records that the rename is load-bearing and the function is
  axis-agnostic.

Run with `ctest --test-dir build -C Release`. Release build only; Debug fails with
LNK2038 (`reference_litepdf_build_test_commands`).

### 4.2 GUI probe (scripted, no human needed)

Throwaway-worktree probe that appends `pan_x pan_y` to a log after every wheel
message (`reference_litepdf_scripted_gui_smoke`). Messages are posted with
`PostMessage`. The Shift branch reads `MK_SHIFT` from wParam, not from
`GetKeyState`, so a posted message exercises it faithfully. Every row names the
observation that would make it fail.

| # | Setup | Input | Pass | Fails if |
|---|---|---|---|---|
| 1 | zoomed past FitWidth, `pan_x` = 0 | `WM_MOUSEHWHEEL` +120 | `pan_x` = −48.00, `pan_y` unchanged | `pan_x` stays 0, or moves positive |
| 2 | same | Shift+wheel −120 | `pan_x` −48 further, `pan_y` unchanged | `pan_y` moves (today's behaviour) |
| 3 | same | repeated notches right | `pan_x` stops at exactly `vp_w − box_w`, and no page change | overshoot, or the page flips |
| 4 | same | `WM_MOUSEHWHEEL` +40 ×3 | one 48-DIP step, taken on the third message | a step on every message |
| 5 | FitWidth (negative control) | both inputs | `pan_x` = `pan_y` = 0 throughout | anything moves |
| 6 | positive control | plain wheel −120 | `pan_y` moves, `pan_x` unchanged | nothing moves (driver or probe broken) |
| 7 | zoomed, `pan_x` < 0 | wheel to the next page | `pan_x` preserved (re-clamped) on the new page | `pan_x` reset to 0 |
| 8 | caret in page box, zoomed | `WM_MOUSEHWHEEL` posted **to the EDIT** | canvas `pan_x` moves | nothing moves |
| 9 | any | `WM_MOUSEHWHEEL` | `SendMessage` result = 1 | 0 |

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
  (`hwnd_message_handler.cc`). That workaround is **not** adopted: without hardware
  it cannot be tested, and its failure mode is an extra vertical notch, not a lost
  one. File an issue if a user reports it.
- **R3. A wheel notch during a live selection drag moves the page under a stationary
  pointer, and the selection's extent updates on the next mouse move.** This is
  already true of the vertical wheel. It is inherited, not introduced, and not fixed
  here.

## 6. Out of scope

- A horizontal scroll **bar**. The canvas has none, in either direction.
- Smooth / animated scrolling, and partial-notch (sub-48 DIP) steps. Both axes
  scroll in whole notches today; changing that is a separate decision covering both.
- Horizontal page turning (H6).
- Resetting the residual on focus change, which the 2004 guidance recommends. The
  vertical wheel does not do it either, and the two axes should stay consistent.
