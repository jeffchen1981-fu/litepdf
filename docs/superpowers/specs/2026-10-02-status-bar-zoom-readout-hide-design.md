# Status bar: zoom readout and a toggle to hide it — design

**Date:** 2026-10-02
**Baseline:** `main` @ `d5cba03`
**Issues:** #57 (zoom-percentage readout), #59 (hide the status bar)
**Ships as:** one PR. Both changes edit `ui/StatusBar` and the same layout and
View-menu code in `MainWindow`; two PRs would conflict with each other.

## 1. Motivation

Both items were recorded as out of scope in §7 of
`2026-09-07-zoom-correctness-page-nav-design.md`.

**#57.** Nothing on screen shows the zoom level. Zoom In at the top of the preset
ladder is a legitimate no-op, and without a readout it is indistinguishable from a
broken key. `DocumentView::zoom_pct()` already defines the number
(`DocumentView.hpp:85-90`: 1.0 = one PDF point per DIP); nothing displays it.

**#59.** The status bar is always visible. There is no way to give its strip back
to the page.

## 2. Zoom readout (#57)

### 2.1 What the reader sees

A read-only label to the right of the `/ N` page-count label, showing the resolved
percentage: `137%`.

- **Read-only, not an editable box.** An editable box needs the page box's focus
  and ESC handling a second time (ESC is a bare accelerator, #47), and the preset
  ladder already covers changing the zoom.
- **Percentage only, no mode name.** The View menu has no Fit Width / Fit Page
  items (`resources/litepdf.rc.in:69-79`); Ctrl+0 is the only mode control. A mode
  name would describe a state the reader cannot choose.
- **Empty with no document.** `set_empty()` clears it with the other label.

### 2.2 Formatting (`ui/detail/StatusBarMath.hpp`)

A pure function, no `<windows.h>`:

```cpp
// "137%" for 1.37f. Empty for a value that is not a displayable magnification.
std::wstring format_zoom_pct(float pct);
```

- Rounds `pct * 100` to the nearest integer.
- Returns an empty string when `pct` is non-finite or rounds to `<= 0`.
- No upper clamp: the value is shown as it is.

The domain is wider than the preset ladder, which is why this is specified.
`fit_percentage` (`core/detail/ZoomMath.hpp:34-42`) is unbounded above — a wide
window on a narrow page yields four-digit percentages — and a minimized window
drives it to exactly 0 through the unguarded `WM_SIZE` arm
(`MainWindow.cpp:1331-1338`). Only `set_zoom_pct` clamps.

### 2.3 Layout

`status_bar_child_rects` gains a third rectangle and a width parameter for it:
the zoom label sits one padding step right of the `/ N` label, same `y` and
height as the other two children. Width: 56 DIP as a starting value, to be
confirmed against `9999%` at 9 pt Segoe UI in the GUI check; a wider string is
clipped by the control, not wrapped.

The label is a `STATIC` with `SS_LEFT | SS_CENTERIMAGE`, created like the existing
one. It needs no colour code of its own: the `WM_CTLCOLORSTATIC` arm's final
branch (`StatusBar.cpp:368-376`) already serves every child that is not the page
box, in High Contrast and in both palettes.

`Impl::repaint()` invalidates the new label as well as the bar and the existing
label. The label paints transparently, so a shrinking string (`137%` → `25%`)
depends on the bar erasing underneath it, exactly as `/ 128` → `/ 2` does today.

### 2.4 `StatusBar` API

```cpp
// Show the magnification. `pct` is DocumentView::zoom_pct(). Skips the write and
// the repaint when the formatted text is unchanged.
void set_zoom(float pct);
```

`set_empty()` also clears the zoom label and the remembered text.

### 2.5 Update path

One hook, not one per zoom source.

`PdfCanvas` gains an owner callback, same shape as `set_on_page_changed`:

```cpp
using RenderSettledCb = std::function<void()>;
void set_on_render_settled(RenderSettledCb cb);
```

It fires at the **entry** of the `WM_USER_RENDER_DONE` / `WM_USER_RENDER_DONE_RIGHT`
arm (`PdfCanvas.cpp:1127`), before the null-pixmap check and before
`accept_completion`. `MainWindow` responds by reading
`active_view()->zoom_pct()` and calling `status_bar_->set_zoom()`; with no active
view it does nothing (the empty state is `set_empty()`'s job).

**Why the entry, not after acceptance.** The readout is a function of live state,
not of the pixmap. `accept_completion` exists to keep stale pixmaps off the
screen; gating the readout on it protects nothing and opens a hole: a failed
render returns at `PdfCanvas.cpp:1133-1140`, before acceptance, while a fit-mode
page turn has already recomputed the percentage synchronously
(`DocumentView.cpp:206-211`) and the page box has already moved. The two
indicators would disagree. Reading live state on every completion — accepted,
stale, superseded or null — is always correct and at worst redundant, and the
text comparison in `set_zoom` makes the redundant case free.

**Why not `next_render_seq()`.** All three submission sites pass through it, but
each calls it before `apply_viewport()` (`MainWindow.cpp:343` vs `361/378`,
`PdfCanvas.cpp:1325` vs `1336`, `1669` vs `1691`), so the value read there is the
old one.

**Coverage.** Every write to the percentage is followed by a submission from the
same UI handler, and every submission produces a completion message, cache hits
included (`RenderEngine.cpp:120-131`). That covers Zoom In/Out/Reset, Ctrl+wheel,
window resize, the F4/F5 pane toggles, the two-page toggle, fit-mode page
turns, tab switches, session restore, and the status-bar toggle in §3.

**Not covered, and not a readout defect.** The results-panel paths
(`on_toggle_results`, `on_cross_tab_find`, `MainWindow.cpp:2215-2251`) call
`on_layout()` without `kick_render`, so they change the canvas height without
re-deriving a fit. The percentage does not change there either, so the readout
stays truthful; the missing re-fit is a pre-existing gap visible only in FitPage
and is left to its own issue (§6).

**Accepted cost.** The readout updates when the first completion message for the
new state arrives, not at the keystroke. In spread mode the hook fires twice per
batch; the second call is a no-op.

## 3. Hiding the status bar (#59)

### 3.1 What the reader sees

A checkable View-menu item, **Status &Bar**, placed after Two-Page Spread and
before the separator above the zoom items. Checked by default.

- **A standalone toggle**, not part of a full-screen mode. #54 can drive the same
  flag when it lands.
- **One state per window**, not per tab: the bar is a single window-level control.
- **No accelerator.** F3–F6 are taken and no convention exists for this command;
  the accelerator table is left alone.
- **Works with no document open.** Neither the command arm nor the checkmark may
  copy the `active_view()` gates the Invert and Two-Page items use
  (`MainWindow.cpp:1502-1510`, `1627-1628`, `1639-1640`).

### 3.2 `StatusBar` API

```cpp
void set_visible(bool visible);
bool visible() const;
```

- `set_visible(false)`: if the page box holds the focus, call `on_focus_out`
  **first**; then `ShowWindow(SW_HIDE)` and clear the flag. Hiding a window does
  not move the focus off its children (the project met this in `ResultsPanel`,
  `ResultsPanel.cpp:996-1002`), so the order matters.
- `set_visible(true)`: set the flag, `ShowWindow(SW_SHOWNA)`.
- `height_px()` returns 0 while hidden. **The measured height is never zeroed:**
  `update_dpi()` runs for a hidden bar too (`MainWindow.cpp:1385`) and overwrites
  it through `measure()`, so a zeroed value would come back as a reserved blank
  strip after a DPI change. The getter is gated on the flag instead.
- `page_box_has_focus()` returns false while hidden, the same defence
  `ResultsPanel::has_focus()` carries.

Theme and DPI updates already reach a hidden bar (`MainWindow.cpp:1385`,
`1453-1461`), so it is correct when shown again; `set_page` and `set_zoom` keep
writing to the hidden children for the same reason.

### 3.3 `MainWindow`

- `IDM_VIEW_STATUS_BAR = 40064`. The "Next free ID: 40064" comment in
  `resources/MainMenu.rc.h` is rewritten to 40065, with the reservation now
  40065–40070.
- New `on_toggle_status_bar()`, modelled on `toggle_outline`
  (`MainWindow.cpp:621-626`): flip visibility, `on_layout()`, then
  `kick_render(current_page)` when a view is active. **The re-render is required.**
  The canvas's own `on_size` resubmits only on `D2DERR_RECREATE_TARGET`
  (`PdfCanvas.cpp:1782-1795`); without the kick a fit mode keeps the old fit for
  the taller canvas and the readout never catches up.
- `on_layout` skips `set_bounds` while the bar is hidden. The three readers of the
  status height (`MainWindow.cpp:444`, `581`, `1185`) need no change: they read
  `height_px()` only.
- The `WM_INITMENUPOPUP` View arm (`MainWindow.cpp:1501`) sets the checkmark from
  `status_bar_->visible()`.
- With the bar hidden there is no keyboard route to a page number; #48 (Ctrl+G)
  does not exist yet. Accepted: the reader chose to hide it.

### 3.4 Persistence

**Not persisted.** The bar is visible at every launch.

This follows the existing convention rather than a cost. No window-level UI state
other than window placement survives a restart: outline and thumbnail visibility
live only on `core::Tab`, and `left_pane_width_px_` is documented as "no
persistence" (`MainWindow.hpp:191-193`). A store for it does exist — the MRU
writes `HKCU\SOFTWARE\LitePDF\MRU` — so a registry `DWORD` would be cheap; it was
considered and declined (user decision, 2026-10-02) because it would make the
status bar the one remembered UI preference. `session.json` was never a
candidate: its parser rejects unknown keys, so a new field means a v3 bump that
older builds refuse.

## 4. Testing

Catch2, ASCII `TEST_CASE` names prefixed by subsystem; build **Release**; verify
with `ctest --test-dir build -C Release`.

**Unit (`tests/unit/test_status_bar_math.cpp`), written before the code:**

- `format_zoom_pct`: `1.0f` → `100%`; `0.25f` → `25%`; `8.0f` → `800%`;
  `1.374f` → `137%`; `1.375f` → `138%`; `38.4f` → `3840%`; `0.0f`, a negative
  value, `0.004f`, NaN and infinity → empty.
- `status_bar_child_rects`: the zoom rectangle starts one padding step right of
  the label's right edge, shares `y` and height with the other two, and has the
  requested width; the existing edit and label rectangles are unchanged.

**GUI checks (scripted smoke where the existing driver reaches, otherwise manual):**

- Readout tracks Ctrl+`=` / Ctrl+`-` / Ctrl+0, Ctrl+wheel, a window resize in
  FitWidth, F4/F5, the two-page toggle, a tab switch between documents at
  different zooms, and session restore.
- Zoom In at 800% leaves the readout at `800%`.
- `137%` → `25%` leaves no stale glyphs; `9999%` fits the label unclipped.
- Closing the last tab clears the readout; minimize and restore never shows `0%`.
- View › Status Bar hides the bar, the canvas takes the strip, and the page
  re-fits; toggling again restores both. Works with no document open. The
  checkmark matches the state.
- With the caret in the page box, hiding the bar returns the focus to the canvas:
  arrow keys scroll and ESC does not target the hidden box.
- Hidden bar, then move the window to a monitor at a different scale: no blank
  strip; showing the bar again gives the right height.
- With the results panel open, hiding the bar extends the panel to the window
  bottom and the splitter drag clamp follows.

## 5. Risks

1. **The completion-arm hook** sits in the code #35 and PR-A2 hardened. It is one
   callback invocation ahead of all existing logic, reads no completion state and
   changes none of the arm's control flow.
2. **A re-entrant owner callback.** `MainWindow`'s handler only writes status-bar
   text; it must not submit renders or change the view from inside the hook.
3. Binary size: one `STATIC`, one menu item, one small function. Negligible
   against the 19,000,000-byte ceiling.
4. Shipped behaviour changes, so the PR runs the risk-tiered review stack before
   merge. `VERSION` is not bumped.

## 6. Out of scope

An editable zoom box. Fit Width / Fit Page menu items. A mode name in the
readout. Persisting the bar's visibility. An accelerator for the toggle.
Full-screen mode (#54). Ctrl+G (#48). Re-fitting after the results panel opens or
closes (§2.5).

## 7. Provenance

The pre-spec draft was reviewed by Fable against `d5cba03`: six findings, all
anchor-verified by the controller. Adopted: the re-render after the toggle (§3.3),
`SW_HIDE` with a flag-gated `height_px()` (§3.2), the hook moved from acceptance
to the arm's entry (§2.5), the formatter's domain (§2.2), focus handback before
hiding (§3.2), and the corrected persistence rationale (§3.4). One reviewer
premise was not tested by experiment — that a zero-height `SetWindowPos` makes
`msctls_statusbar32` re-dock at full height — and the design does not depend on
it, since the bar is hidden with `ShowWindow`.
