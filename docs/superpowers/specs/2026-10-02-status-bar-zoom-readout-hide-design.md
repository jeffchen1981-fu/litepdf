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

The rules, in this order, on `p = pct * 100.0f`:

1. `p` is not finite → empty. This also catches a finite `pct` whose product
   overflows to an infinity, in either sign.
2. `p < 0.5f` → empty (zero, negatives, and anything that would round to 0).
3. `p >= 9999.5f` → empty. The label is sized for four digits (§2.3); a value
   that cannot be shown whole is not shown.
4. Otherwise the nearest integer, halves away from zero (`std::lround`
   semantics), followed by `%`: `0.125f` → `13%`.

Rules 1–3 run before the conversion, so `std::lround` only ever sees a value in
`[0.5, 9999.5)`. Inside that range there is no clamp.

The domain is wider than the preset ladder, which is why this is specified.
`fit_percentage` (`core/detail/ZoomMath.hpp:34-42`) is unbounded above — a wide
window on a narrow page yields four-digit percentages — and a minimized window
drives it to exactly 0 through the unguarded `WM_SIZE` arm
(`MainWindow.cpp:1331-1338`). Only `set_zoom_pct` clamps.

### 2.3 Layout

`StatusBarChildRects` gains `zoom_x / zoom_y / zoom_w / zoom_h`, and
`status_bar_child_rects` gains a **required fifth parameter, last**:

```cpp
status_bar_child_rects(int bar_h, int pad_px, int edit_w_px, int label_w_px,
                       int zoom_w_px)
```

No default value. All three existing call sites are updated: `StatusBar.cpp:224`
and the two tests at `test_status_bar_math.cpp:63` and `:82`.

The zoom rectangle starts one padding step right of the `/ N` label's
rectangle, with the same `y` and height as the other two children. The `/ N`
rectangle is a fixed 96 DIP (`kLabelWDip`), so with a short count such as `/ 12`
there is a visible gap before the readout. Accepted: closing it means measuring
text, which the fixed-offset layout deliberately avoids.

Width: the plan measures `9999%` at 9 pt Segoe UI with `GetTextExtentPoint32W`
and sets the constant to that extent plus one padding step, in DIP. `9999%` is
the longest string the formatter can return (§2.2), so nothing is ever clipped.

The label is a `STATIC` with `SS_LEFT | SS_CENTERIMAGE`, created like the existing
one. It needs no colour code of its own: the `WM_CTLCOLORSTATIC` arm's final
branch (`StatusBar.cpp:368-376`) already serves every child that is not the page
box, in High Contrast and in both palettes.

Every place that names the existing label by hand gains the new one:

- `update_dpi` (`StatusBar.cpp:599-606`) sends it `WM_SETFONT`. Missing this
  leaves the label holding the old font handle after the function deletes it.
- `Impl::relayout()` positions it.
- `Impl::repaint()` invalidates it. The label paints transparently, so a
  shrinking string (`137%` → `25%`) depends on the bar erasing underneath it,
  exactly as `/ 128` → `/ 2` does today.
- The theme arm (`StatusBar.cpp:406-408`) invalidates it.

Comments that count "the two children" (`StatusBarMath.hpp:50`,
`StatusBar.cpp:295`) and the note in `DocumentView.hpp:88-89` that nothing
surfaces a percentage are brought up to date.

### 2.4 `StatusBar` API

```cpp
// Show the magnification. `pct` is DocumentView::zoom_pct(). Skips the write and
// the repaint when the formatted text is unchanged.
void set_zoom(float pct);
```

`set_empty()` also clears the zoom label and the remembered text.

### 2.5 Update path

One helper, two call sites, instead of one hook per zoom source.

```cpp
// MainWindow: push the active view's live percentage to the status bar.
// No-op without a status bar or without an active view (the empty state is
// set_empty()'s job).
void refresh_zoom_readout();
```

**Call site 1 — every completion message.** `PdfCanvas` gains an owner callback,
same shape as `set_on_page_changed`:

```cpp
using CompletionArrivedCb = std::function<void()>;
void set_on_completion_arrived(CompletionArrivedCb cb);
```

It fires at the **entry** of the `WM_USER_RENDER_DONE` / `WM_USER_RENDER_DONE_RIGHT`
arm (`PdfCanvas.cpp:1127`), before the null-pixmap check and before
`accept_completion`. The name says what it is: it fires for accepted, stale,
superseded and failed completions alike. `MainWindow` wires it to
`refresh_zoom_readout()`.

**Call site 2 — inside `MainWindow::kick_render`, once on each branch,**
immediately after that branch's `apply_viewport()` (`MainWindow.cpp:361` and
`378`). The spread branch returns early at `:375`, so a single call at the end of
the function would never run in two-page mode. This makes the readout
synchronous for everything that goes through `kick_render` — the zoom commands,
window resize, pane toggles, tab switch, session restore, the §3 toggle — so the
readout and the page box change in the same message. Call site 1 then covers what
`kick_render` does not: Ctrl+wheel and the canvas's own page turns
(`resubmit_current_page`, `navigate_to_page`).

The existing `set_on_zoom_changed` is not reused: it fires only for Ctrl+wheel
and means "persist the session" (`MainWindow.cpp:1296`).

**Why the entry, not after acceptance.** The readout is a function of live state,
not of the pixmap. `accept_completion` exists to keep stale pixmaps off the
screen; gating the readout on it protects nothing and opens a hole: a failed
render returns at `PdfCanvas.cpp:1133-1140`, before acceptance, while a fit-mode
page turn has already recomputed the percentage synchronously
(`DocumentView.cpp:206-211`) and the page box has already moved. The two
indicators would disagree. Reading live state on every completion — accepted,
stale, superseded or null — is always correct and at worst redundant, and the
text comparison in `set_zoom` makes the redundant case free.

**Why not `next_render_seq()` as the single hook.** All three submission sites
pass through it, but on the paths where a fit is re-derived it runs before
`apply_viewport()` (`MainWindow.cpp:343` vs `361/378`; the spread branches at
`PdfCanvas.cpp:1325` vs `1336` and `1669` vs `1691`), so a resize or pane toggle
would read the old value there. On other paths the percentage is already written
by then (a zoom step, or `change_current_page` at `PdfCanvas.cpp:1665`). A hook
that is right on some paths and stale on others is worse than none.

**Coverage.** Every write to the percentage is followed by a submission from the
same UI handler: Zoom In/Out/Reset, Ctrl+wheel, window resize, the F4/F5 pane
toggles, the two-page toggle, fit-mode page turns, tab switches, session restore,
and the status-bar toggle in §3. A submission normally produces a completion
message, cache hits included (`RenderEngine.cpp:120-131`).

**Accepted costs.**

- On the two canvas-only paths the readout updates when the first completion
  message arrives, not at the input.
- A completion that is never posted — an escrow clone, allocation or
  `PostMessageW` failure in `post_render_done_impl` (`PdfCanvas.cpp:146-167`) —
  leaves the readout stale on those two paths until the next render. This is the
  exposure the canvas already documents for `wheel_flip_seq`.
- In spread mode call site 1 fires twice per batch; the second is a no-op.

**Not covered, and not a readout defect.** Five paths call `on_layout()` without
`kick_render`: `on_cross_tab_find` and `on_toggle_results`
(`MainWindow.cpp:2215-2251`), `on_results_close` (`:2344-2354`), and the two
splitter drags (`:1185-1188`, `:1238-1250`). They resize the canvas without
re-deriving a fit, so the percentage does not change and the readout stays
truthful. The missing re-fit is pre-existing — the left-dock splitter drag shows
it in FitWidth, the default mode — and is left to its own issue (§6).

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

- `IDM_VIEW_STATUS_BAR = 40064`, with its `MENUITEM` in `resources/litepdf.rc.in`
  and a `WM_COMMAND` case. Both comments in `resources/MainMenu.rc.h` that name
  the reservation are rewritten: line 69 ("Next free ID: 40064" → 40065, range
  40065–40070) and line 71 ("leaving the 40064-40070 reservation alone").
- New `on_toggle_status_bar()`, modelled on `toggle_outline`
  (`MainWindow.cpp:621-626`): flip visibility, `on_layout()`, then
  `kick_render(current_page)` when a view is active. **The re-render is required.**
  The canvas's own `on_size` resubmits only on `D2DERR_RECREATE_TARGET`
  (`PdfCanvas.cpp:1782-1795`); without the kick a fit mode keeps the old fit for
  the taller canvas and the readout never catches up.
- `on_layout` skips `set_bounds` while the bar is hidden. The three readers of the
  status height (`MainWindow.cpp:444`, `581`, `1185`) need no change: they read
  `height_px()` only.
- The checkmark is set from `status_bar_->visible()` **inside** the existing
  `popup_owns(popup, IDM_VIEW_INVERT)` block (`MainWindow.cpp:1501-1512`), before
  its `return 0`. A separate arm after it would be unreachable; one before it
  would skip the Invert and Two-Page checkmarks.
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
  `1.374f` → `137%`; `1.375f` → `138%`; `0.125f` → `13%` (the tie rule);
  `38.4f` → `3840%`; `99.0f` → `9900%`; `100.0f`, `1e30f`, `FLT_MAX`,
  `-FLT_MAX`, `0.0f`, `-1.0f`, `0.004f`, NaN and both infinities → empty.
- `status_bar_child_rects`: both existing tests pass the fifth argument and pin
  the four zoom fields — including the tiny-bar test, whose point is that every
  field is pinned on the fallback branch. The existing edit and label
  expectations are unchanged.

**GUI checks.** Text is read from the label with `WM_GETTEXT`; paint is checked on
the control's own pixels. The plan reads
`reference_litepdf_scripted_gui_smoke` before choosing the driver for each.

- Readout text tracks Ctrl+`=` / Ctrl+`-` / Ctrl+0, Ctrl+wheel, a window resize
  in FitWidth, F4/F5, the two-page toggle, a fit-mode page turn in
  `spread-unequal.pdf`, a tab switch between documents at different zooms, and
  session restore.
- After a zoom command, a tab switch and a resize, the text is already correct
  when the command returns — before any completion is pumped. This is the check
  that fails if call site 2 is missing. Run it in single-page **and** in
  two-page mode: the two branches of `kick_render` carry separate calls.
- With and without a document: toggle Status Bar, reopen View, and all three
  checkmarks (Status Bar, Invert Colors, Two-Page Spread) are correct.
- Ctrl+wheel updates the text. This is the check that fails if call site 1 is
  missing.
- Zoom In at 800% leaves the readout at `800%`.
- `137%` → `25%` leaves no stale glyphs.
- A drag-resize in FitWidth does not make the bar flicker visibly.
- After moving the window to a monitor at a different scale, the zoom label uses
  the same font as the `/ N` label.
- After a light/dark switch the zoom label repaints in the new colours.
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

**Not testable from outside.** No listed check distinguishes call site 1 at the
arm's entry from the same call placed after `accept_completion`: the two differ
only when a render fails on a canvas-only path, and no fixture is known to
contain a page that opens but fails to render (`corrupt.pdf` has not been
checked for this). The placement is pinned by code review and by a comment at
the call site stating why it precedes the null check. If the plan finds a
fixture that produces a null completion, it adds the check.

## 5. Risks

1. **The completion-arm hook** sits in the code #35 and PR-A2 hardened. It is one
   callback invocation ahead of all existing logic, reads no completion state and
   changes none of the arm's control flow.
2. **A re-entrant owner callback.** `refresh_zoom_readout()` only writes
   status-bar text; it must not submit renders or change the view.
3. Binary size: one `STATIC`, one menu item, one small function. Negligible
   against the 19,000,000-byte ceiling.
4. Shipped behaviour changes, so the PR runs the risk-tiered review stack before
   merge. `VERSION` is not bumped.

## 6. Out of scope

An editable zoom box. Fit Width / Fit Page menu items. A mode name in the
readout. Persisting the bar's visibility. An accelerator for the toggle.
Full-screen mode (#54). Ctrl+G (#48). Re-fitting after a splitter drag or after
the results panel opens or closes (§2.5).

## 7. Provenance

The pre-spec draft was reviewed by Fable against `d5cba03`: six findings, all
anchor-verified by the controller. Adopted: the re-render after the toggle (§3.3),
`SW_HIDE` with a flag-gated `height_px()` (§3.2), the hook moved from acceptance
to the arm's entry (§2.5), the formatter's domain (§2.2), focus handback before
hiding (§3.2), and the corrected persistence rationale (§3.4). One reviewer
premise was not tested by experiment — that a zero-height `SetWindowPos` makes
`msctls_statusbar32` re-dock at full height — and the design does not depend on
it, since the bar is hidden with `ShowWindow`.

The spec gate ran Full tier. Round 1 (Opus and Sonnet, against `81eab06`) found
no blocking defect and eight points, all anchor-verified and adopted: the new
label in `update_dpi` and the theme arm (§2.3), the explicit
`status_bar_child_rects` signature and test updates (§2.3, §4), the corrected
`next_render_seq()` rationale and the never-posted completion as an accepted
cost (§2.5), the full list of layout-only paths (§2.5), the tie and overflow
rules (§2.2), the second comment in `MainMenu.rc.h` (§3.3), and checks that can
fail for each call site (§4). The synchronous call in `kick_render`
was added in response to the round-1 observation that a tab switch would
otherwise show the previous tab's zoom until its first completion.

Lens 3 ran against `e3a755c`, `terra@medium` then `luna@max`, on the spec plus a
pre-extracted evidence bundle. Both reported the four-digit label width against a
five-digit formatter range (§2.2, §2.3) and the spread branch's early return in
`kick_render` (§2.5). `luna@max` alone reported the overflow of a finite
negative input past the formatter's guard (§2.2) and the checkmark's position
relative to the View arm's `return 0` (§3.3). All four were anchor-verified and
adopted. Both also restated that the completion hook's position has no failing
check; §4 already records that as review-only, and it stays so.
