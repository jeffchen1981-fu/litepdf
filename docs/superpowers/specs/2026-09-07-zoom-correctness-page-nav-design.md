# Zoom correctness, page indicator, and wheel scrolling — design

**Date:** 2026-09-07
**Baseline:** `main` @ `4c2cefe` (v1.2.0 released)
**Ships as:** three PRs.

| PR | scope | risk shape |
|----|-------|-----------|
| **A1** | zoom semantics, unit contract, session migration | pure math + persistence; touches no completion-lifecycle code |
| **A2** | render-completion lifecycle, page anchors, wheel scrolling | ordering and identity of asynchronous renders |
| **B** | status bar, page indicator, go-to-page | additive, plus one shipped-behavior fix |

A2 depends on A1; B depends on A2. The split is drawn where it is because the spec
gate's findings clustered there — see §8.

## 1. Motivation

Two product questions started this: the main window has no page indicator and no
go-to-page control, and the mouse wheel does not scroll. Investigating the second
found that **zoom is non-functional in shipped v1.2.0**, which is *why* nothing ever
overflows the viewport and therefore why in-page scrolling has nothing to scroll.

### 1.1 Empirical evidence

Release build of `main`, `tests/fixtures/large.pdf`, window maximized on a 200 %
display. Driven by `PostMessage(WM_COMMAND, IDM_ZOOM_*)`; state read from
`%LOCALAPPDATA%\LitePDF\session.json`; window captured with `SetWindowPos
HWND_TOPMOST` + `CopyFromScreen`.

| action                  | `zoom_scale` observed         | on-screen page size |
|-------------------------|-------------------------------|---------------------|
| baseline (FitWidth)     | 12.549                        | whole page fits     |
| Zoom In ×3 (IDM 40010)  | 12.549 → 12.549 (unchanged)   | identical           |
| Zoom Out ×3 (IDM 40011) | 12.549 → 4.0 → 3.0 → 2.0      | identical           |

### 1.2 Defect 1 — unit mismatch makes `zoom_in()` a permanent no-op

`DocumentView::scale` holds a *render scale* (PDF point → pixel); measured 12.549.
`zoom_in()` (`src/core/DocumentView.cpp:259`) compares it against the preset table
`0.5 … 4.0`, whose semantics are *zoom percentages*. No preset exceeds 12.549, so
`zoom_in()` always returns false. `zoom_out()` snaps straight to 4.0.

### 1.3 Defect 2 — paint path unconditionally re-fits to the viewport

`src/ui/PdfCanvas.cpp:853-854`:

```cpp
float scale = vp.width / src.width;
if (src.height * scale > vp.height) scale = vp.height / src.height;
```

Unbranched, so the destination rect always fits the viewport — shrinking *and*
enlarging. The Zoom Out ×3 row is the proof: 6.3× less render scale, identical
geometry. The dual-page `draw_slot` lambda (`PdfCanvas.cpp:802-805`) has the same
shape.

### 1.4 Defect 3 — DPI counted twice, renders 2× oversized at 200 %

`MainWindow::kick_render` (`src/ui/MainWindow.cpp:249-255`) reads `GetClientRect`,
which returns *device pixels* (the manifest declares `PerMonitorV2`,
`resources/manifest.xml:18`), and passes it to `DocumentView::set_zoom_mode`'s
`viewport_w_dip` parameter, which multiplies by `dpi/96` again
(`DocumentView.cpp:230-233`). At 200 % the pixmap is rendered 2× too wide — 4× the
pixel area — then shrunk by Defect 2. Four call sites pass raw pixels:
`MainWindow.cpp:249-255`, `:622-628`, `:843-851`, `:1385-1391`.

The intended contract was documented and then lost in implementation:
`docs/plans/2026-04-16-phase-3-minimal-viewer.md:767` — *"viewport_w_dip,
viewport_h_dip = canvas client size in DIPs (GetDpiForWindow adjusts)"*.

### 1.5 Defect 4 — render target and bitmap are in different units

`D2D1::RenderTargetProperties()` defaults `dpiX/dpiY = 0.0`, which makes D2D use the
**desktop** DPI, so `rt->GetSize()` returns DIPs = px × 96/192 at 200 %.
`D2D1::BitmapProperties()` defaults to **96**, so `ID2D1Bitmap::GetSize()` returns
pixels. The two differ by a factor of 2 on a 200 % display.

Verified against the SDK header on this machine, the primary source for both
defaults (it is not vendored in this repo):
`C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\um\d2d1helper.h` —
`BitmapProperties` at `:406-410` (`FLOAT dpiX = 96.0f, FLOAT dpiY = 96.0f`),
`RenderTargetProperties` at `:427-434` (`FLOAT dpiX = 0.0, FLOAT dpiY = 0.0`).

Today this is invisible: Defect 2's shrink-to-fit is a pure ratio, so the units
cancel. The moment the paint path draws at natural size the mismatch becomes a
visible 2× error.

A second consequence: `create_render_target` (`PdfCanvas.cpp:573-576`) always takes
the desktop DPI, so the `WM_DPICHANGED_BEFOREPARENT` comment at `PdfCanvas.cpp:454`
("next paint rebuilds at new DPI") does not hold on a secondary monitor with a
different scale factor.

---

## 2. PR-A1 — zoom semantics, unit contract, migration

Fixes Defects 1–4. **Deliberately touches no render-completion code**: the pan reset
in `WM_USER_RENDER_DONE` (`PdfCanvas.cpp:547-553`) stays exactly as it is. Every
completion-lifecycle change is A2's.

**Interim default is FitPage.** With Defect 2 fixed, FitWidth would mean page width =
canvas width, and an A4 page on a maximized 16:9 window renders about 2.8× taller
than the viewport — but A1 ships no wheel scrolling, and PgDn jumps to the next page,
so a reader would silently never see the lower two thirds of any page. A1 therefore
sets the default zoom mode to FitPage (`DocumentView::Impl::zm`, `DocumentView.cpp:48`).
The observable result is what users see today — the whole page fits — so A1 changes
the machinery without changing the default view. A2 restores FitWidth when the wheel
lands.

Zoom in from that default is now genuinely usable: Ctrl+`=` enlarges, and the arrow
keys pan within the enlarged page, clamped (§2.3).

### 2.1 Zoom semantics (`core/DocumentView`)

- **`zoom_pct_`** — user-facing magnification. `1.0` means **one PDF point maps to
  one DIP**, the conventional 96-DPI screen ratio browsers also call 100 %. It is
  deliberately *not* physical actual size: a PDF point is 1/72 inch, so 1.0 renders
  at 0.75× ruler size. No numeric percentage is surfaced today
  (`resources/MainMenu.rc.h` has only Zoom In / Out / Reset) and §7 defers a readout,
  so this has no user-visible consequence — it is recorded so an eventual readout is
  not built on a wrong premise.
- **`render_scale()`** == `zoom_pct_ × (dpi / 96)` — the point→**pixel** factor.
  `dpi` is applied exactly once, here.
- **`set_viewport(w_px, h_px, dpi)`** replaces `set_zoom_mode`'s DIP-named
  parameters. Renaming to `_px` is the point: it turns the four call sites in §1.4
  into compile errors rather than silent unit bugs. For FitWidth/FitPage it derives
  `fit_w_pct = w_px / (page_pt_w × dpi/96)` and the height analogue.

**Every consumer of the old `impl_->scale`, and which half it takes.** Splitting one
value into two makes each existing read a decision; leaving one wrong either renders
at percentage units or makes the ladder compare a render scale against percentage
presets.

| site | takes |
|------|-------|
| `DocumentView.cpp:287` `req.scale` (request_render) | `render_scale()` |
| `:338`, `:355`, `:364` `r0/r1.scale` (prefetch requests) | `render_scale()` |
| `PageCache` L1 key (via `req.scale`) | `render_scale()` |
| `:237`, `:240` fit assignment | `zoom_pct_` |
| `:256` `set_zoom_scale` | `zoom_pct_` |
| `:260`, `:263`, `:272`, `:278` ladder walk | `zoom_pct_` |
| `:191` page-change fit recompute | `zoom_pct_` (re-derives from mode) |
| `MainWindow.cpp:714` session save | `zoom_pct_` |
| `MainWindow.cpp:839` session restore `set_zoom_scale` | `zoom_pct_` |
| `PdfCanvas.cpp:292` `scroll_into_view` quad mapping | **`zoom_pct_`** — see below |
| `PdfCanvas.cpp:882` search-hit overlay quad mapping | **`zoom_pct_`** — see below |

**The two overlay sites take the percentage, not the render scale.** They map PDF
points into the canvas's DIP space. Bitmap DIP width = bitmap px × 96/dpi =
`page_pt × pct × (dpi/96) × (96/dpi)` = `page_pt × pct`. So in DIP space one PDF
point is exactly `zoom_pct_` DIPs, and `render_scale()` would double every hit
rectangle at 200 %. (An earlier draft of this spec asserted the opposite; the
arithmetic above is the correction.)

**Preset ladder.** Extend to `0.25 / 0.5 / 0.75 / 1.0 / 1.25 / 1.5 / 2.0 / 3.0 /
4.0 / 6.0 / 8.0`. A fit-derived percentage can legitimately exceed 4.0 (a narrow
page on a wide canvas), and with the old ceiling that reproduces Defect 1's exact
symptom. `zoom_in()` = first rung strictly above the current pct; `zoom_out()` =
last rung strictly below; both no-op at the ends. `set_zoom_scale`'s clamp bounds
(`DocumentView.cpp:249-256`) move to `[0.25, 8.0]`.

### 2.2 Unit contract (`ui/PdfCanvas`)

**One unit for all canvas geometry: DIPs.**

- `create_render_target` passes the **window's** DPI (`GetDpiForWindow`) explicitly
  in `D2D1::RenderTargetProperties`, instead of inheriting the desktop DPI. This
  also makes the existing `WM_DPICHANGED` teardown correct on mixed-DPI setups.
- Bitmaps stay at 96 DPI, so bitmap pixels convert to DIPs by dividing by
  `rt_dpi / 96` at exactly one place: where the bitmap size enters layout math.

`rt->GetSize()` has exactly three consumers (`PdfCanvas.cpp:286`, `:792`, `:850`),
all fit-ratio math; `HwndRenderTargetProperties` and `rt->Resize` both take
**pixels** and are unaffected by the target's DPI; `impl_->last_size` is only ever
written; and no mouse coordinate is hit-tested against DIP geometry
(`WM_LBUTTONDOWN` only calls `SetFocus`). So the change has no other dependents.

**Rejected alternative:** forcing the render target to 96 DPI so DIP == px is
simpler, but `PdfCanvas` has four DIP-by-intent constants — the 24 margin in
`scroll_into_view` (`:304`), the 100 arrow-key pan step (`:690-702`), and the 8
dual-page gutter (`:793`). Under a 96-DPI target they become raw pixels and halve
in physical size at 200 %: a silent visual regression.

### 2.3 Layout math extracted (`ui/detail/ViewportMath.hpp`)

New pure-logic header, no UI dependency, unit-tested — the pattern already used by
`SplitterMath.hpp` and `PdfCanvasLayout.hpp`.

**The pan origin changes, and this is a real semantic change.** Today `pan_x/pan_y`
is an offset from a **centered** position: `dx = (vp.width - dst_w) * 0.5f`
(`PdfCanvas.cpp:857-858`), then `dst = dx + pan_x`. Centering is right while content
always fits, but it makes `pan == 0` mean "centered", so on an overflowing axis
`pan_y = 0` would leave the page top *above* the viewport. From A1 on:

- an axis whose content **fits** the viewport is centered, and its pan is 0;
- an axis whose content **overflows** uses a **top-left origin**: `pan = 0` puts the
  content's leading edge at the viewport's leading edge, and the valid range is
  `[viewport - content, 0]`.

Nothing persists a pan value — `SessionTab` carries only path, page, zoom mode and
zoom scale — so this needs no migration. The per-tab in-memory snapshots at
`MainWindow.cpp:555` are all written and read under the new regime.

- `clamp_pan(pan, content, viewport)` — implements the rule above.
- `place_bitmap(src_w, src_h, vp_w, vp_h, pan_x, pan_y) -> Placement` — destination
  rect at **natural size**: `dst_w == src_w` and `dst_h == src_h` for every viewport,
  larger or smaller. No shrink-to-fit. That equality is the literal regression target
  of Defect 2 and is asserted directly (§5).

Three current copies of the fit math — single-page paint, dual-page `draw_slot`, and
`scroll_into_view` (`PdfCanvas.cpp:287-289`) — all call the helper. Arrow-key panning
routes through `clamp_pan`, fixing today's unclamped pan.

### 2.4 Two-page spread

Two separate problems, both from drawing at natural size.

**The fit must be computed after the left-page snap, and from both pages.**
`kick_render` calls `set_zoom_mode` at `MainWindow.cpp:249-255` and only then snaps
`set_current_page(left)` at `:272`, while `DocumentView` derives the fit from
`impl_->current_page` alone (`DocumentView.cpp:218`). So the fit is computed for
whatever page was current *before* the pair was canonicalized. In dual mode
`kick_render` snaps first, then calls `set_viewport` with the **slot** width in
*pixels* — `(canvas_px_w - gutter_px) / 2`, where `gutter_px = 8 × dpi/96` — and the
full client height; `DocumentView` derives the fit from `max(left_pt_w, right_pt_w)`
and `max(left_pt_h, right_pt_h)` so one shared scale fits both slots. (Unit note:
`slot_w` inside `draw_slot` is in DIPs because it comes from `rt->GetSize()`;
`set_viewport` takes pixels.)

**The pan content box is the painted union, not `left + gutter + right`.**
`draw_slot` centers each bitmap inside a fixed half-width slot
(`PdfCanvas.cpp:792-808`), so with unequal page widths the painted extent is not the
sum of the two bitmaps. `clamp_pan` takes the union of the two destination rects
actually produced by `place_bitmap`.

### 2.5 Session migration

`validate()` rejects any `version != kSessionVersion`
(`src/core/SessionState.cpp:280-281`) and `from_json` returns `nullopt`, so bumping
the constant alone would make every existing `session.json` unrestorable — window
placement, tab set and pages all lost — rather than migrating one field.

**Where each step actually happens.** `from_json` only parses a string; it cannot
rewrite anything in place. The migration is a step inside it: parse, then if the
parsed version is 1, transform the value (Custom → FitWidth per tab) and stamp
version 2 *before* `validate` runs, which then checks against 2. The returned
`SessionState` is always v2.

**A missing `version` key counts as v1.** The parser writes `out.version` only when
the key is present (`SessionState.cpp:267`) and `SessionState` default-initialises it
to `kSessionVersion` (`SessionState.hpp:14`), so once the constant becomes 2 a
versionless file would silently claim to be v2 and skip the migration, restoring an
old render scale as a percentage. The parser tracks whether the key was seen and
treats absence as version 1.

*Why reset rather than convert:* v1's stored `zoom_scale` is a render scale, but
`set_zoom_scale` already clamped it to `[0.5, 4.0]` on write
(`DocumentView.cpp:249-256`), so the only Custom values v1 can hold are preset-table
numbers. They are valid percentages by coincidence, reached through the broken
`zoom_out()` ladder, and never corresponded to what the user saw. FitWidth is the
honest reset.

> **Deviation (2026-09-09):** PR-A1 shipped resetting Custom to **FitPage**, not
> FitWidth as designed above. This was discovered late in the branch: with the
> paint-path fix in this same PR, an A4 page in FitWidth stands roughly 2.8x
> taller than the viewport, and this release ships no wheel scrolling -- so a
> v1 Custom tab migrated into FitWidth would come back unreadable, with no way
> to reach the rest of the page. FitPage keeps the restored tab actually
> navigable. PR-A2 revisits this once ScrollMath makes FitWidth navigable
> again. See `migrate_v1_to_v2` in `SessionState.cpp` and the mirrored restore
> mapping in `MainWindow::restore_on_tab_ready`. The design intent above is
> left as originally written; this note records what actually shipped.

**Downgrade is a one-way door; the backup is the mitigation, and it is fail-closed.**
Once A1 writes a v2 file the shipped v1.2.0 binary rejects it outright — `git show
v1.2.0:src/core/SessionState.cpp` carries the same version check at `:281`. The
backup cannot live in `from_json` (no filesystem access there); it belongs in
`save_session` (`SessionStore.cpp:10-27`), which today writes a temp file and
`MoveFileExW`s it over the target. Before that replacement, if the existing
`session.json` parses as v1, copy it to `session.v1.bak`. **If that copy fails, abort
the save** and leave the v1 file intact — a save that silently destroys the only
recoverable copy is the failure this section exists to prevent. The CHANGELOG entry
for the release carrying A1 states the one-way door and names the backup file.

**Tests to update — the full set, not a range.** `tests/unit/test_session_state.cpp`
touches v1 in four places and only one of them is a fixture string:

| site | why it breaks |
|------|---------------|
| `:40-59` round-trip | builds `s.version = 1` in C++ and round-trips it; `to_json` emits that value verbatim (`SessionState.cpp:311`), so this is a v1 document. Its `:57` assertion `tabs[1].zoom_mode == SessionZoom::Custom` fails once migration resets it. |
| `:90-183` fixtures | hardcode `"version":1` in JSON literals. |
| `:120` strict-version | `REQUIRE_FALSE(from_json("{\"version\":2,…}"))` inverts once 2 is current. |
| `:182-185` | "rejects a Custom tab with zoom_scale 0" flips from rejecting to accepting, because migration resets Custom before `validate` sees it. The case must move to a v2 document to stay meaningful. |

---

## 3. PR-A2 — completion lifecycle, page anchors, wheel scrolling

Everything asynchronous. A1 left the pan reset untouched; A2 replaces it, and in
doing so must fix the identity and ordering problems that make "which completion is
this?" answerable at all. It ends by restoring FitWidth as the default zoom mode.

### 3.1 Completion identity

Three separate gaps make a completion unidentifiable today:

- `RenderMeta` is `{escrow, epoch}` (`PdfCanvas.cpp:47-50`) — no page, no slot, no
  request identity.
- `cancel_all_below_priority(p)` cancels only `priority > p`
  (`RenderEngine.cpp:417-425`), so `cancel_stale_renders(0)` never cancels a P0 and
  two P0 renders for the same page can be in flight after a zoom, resize, DPI change
  or invert toggle.
- A cancelled or failed render posts `LPARAM = 0` with no metadata at all
  (`PdfCanvas.cpp:56-63`), so the null-completion path cannot say what it was for.

**`RenderMeta` becomes `{escrow, epoch, page, slot, seq}`**, where `slot` is
`Left`/`Right` and `seq` is a per-submit monotonic counter owned by `PdfCanvas`. The
accept predicate is extracted as a pure function so it is unit-testable rather than
buried in the WndProc:

```cpp
bool accept_completion(std::uint64_t meta_epoch, std::uint64_t cur_epoch,
                       int meta_page, Slot meta_slot,
                       int cur_page, bool dual, int page_count);
```

**Slot is load-bearing.** In dual mode the right-slot render is submitted for
`left + 1` (`MainWindow.cpp:274-278`) while `current_page()` has already been snapped
to `left` (`:272`), and both messages share one `case` block
(`PdfCanvas.cpp:463-465`). A predicate comparing every completion against
`current_page()` would drop *every* right-slot pixmap, leaving the right half of each
spread as the grey placeholder of `PdfCanvas.cpp:823-833` — permanently, since each
resubmit takes the same path. `Left`/single completions compare against `cur_page`;
`Right` compares against `dual_page_compute_right(cur_page, page_count)`.

**`set_view` must also clear `right_bitmap`.** Today it clears only `current_bitmap`,
and only on the null-view branch (`PdfCanvas.cpp:154-170`); `set_dual_page` returns
early when the flag already matches (`:238-239`). Switching between two tabs that are
both in dual mode therefore paints the previous document's right page until a new
right completion lands.

### 3.2 Page anchors

| anchor        | applied pan (top-left origin, §2.3)      | set by |
|---------------|------------------------------------------|--------|
| `Top`         | `pan_y = 0`                              | PgDn / Home / End / outline click / thumbnail click / go-to-page / wheel flip forward |
| `Bottom`      | `pan_y = vp_h - content_h`               | wheel flip backward |
| `Hit{quad}`   | centers the quad, 24 DIP margin          | search navigation |
| *none*        | keep current pan, re-clamp it            | same-page re-render (resize, DPI, zoom, pane toggle, invert, tab switch) |

Both formulas depend on A1 having moved the origin: under the old centered origin
`pan_y = 0` would mean "centered", not "top".

**API shape.** `PageAnchor` is a small value type — a tag plus an optional `fz_quad`
payload — declared in `PdfCanvas.hpp`. `PdfCanvas` owns the single `pending_anchor_`
slot; nothing outside reads or writes it. `change_current_page` gains a defaulted
parameter so existing callers keep compiling:

```cpp
bool change_current_page(int idx, PageAnchor anchor = PageAnchor::top());
```

**Anchor lifetime, keyed by `seq`.** `(epoch, page, slot)` does not identify a
request: a same-page zoom or resize produces a second P0 with an identical triple, so
the older completion could consume an anchor meant for the newer render. The anchor
therefore records the `seq` of the submit that created it and is consumed only by a
completion whose `seq` matches. It follows that:

- a newer page change or resubmit **replaces** the anchor, carrying the intent
  forward to the new `seq` — which is also what makes a failed render recover, since
  the retry re-issues it;
- `set_view` clears it (new view, new epoch);
- epoch-mismatch drops and null completions leave it alone. They must: `view_epoch`
  is incremented in exactly one place, `set_view` (`PdfCanvas.cpp:164`), so an epoch
  mismatch means the pixmap belongs to a *previous view* while the pending anchor
  belongs to the current one. Clearing there would drop a live anchor — switch to tab
  B, press PgDn, then tab A's older P0 lands and mismatches. With `seq` matching, a
  stale anchor can never be consumed by the wrong completion, so leaving it pending
  is safe.

### 3.3 Every path that changes the current page

Four sites bypass `change_current_page` and call `DocumentView::set_current_page`
directly, so neither the anchor nor the page-changed observer fires. A2 routes all
four through `change_current_page`:

| site | symptom today |
|------|---------------|
| `MainWindow.cpp:271-272` (kick_render dual snap) | thumbnail highlight wrong in spread mode |
| `MainWindow.cpp:1300` (`IDM_VIEW_DUAL_PAGE`) | same, on toggling spread |
| `MainWindow.cpp:836` (`restore_on_tab_ready`) | restored page not broadcast |
| `PdfCanvas.cpp:729` (defensive re-snap in `resubmit_current_page`) | dual-mode End reports the pre-snap page |

The last two are the ones a page indicator would expose most visibly: after a session
restore, and after End in spread mode, the model and the observer disagree.

**The dual snap must not clobber a pending `Hit`.** Routing the snap through
`change_current_page` with a `Top` anchor would overwrite a `Hit` installed moments
earlier when a search lands on the right page of a spread. The snap passes the
existing pending anchor through unchanged when one is set, and `Top` only when none
is.

### 3.4 Search navigation

`scroll_into_view` computes `pan_y` from the *previous* page's bitmap and is followed
by `kick_render`, whose completion resets the pan (`MainWindow.cpp:1717-1720`,
`:1730`, `:1903`). Harmless today; with tall pages the hit ends up off-screen. The
`Hit{quad}` anchor carries the geometry to the completion, where the new page's real
height is known.

**`scroll_into_view` is not the only entry point.** `on_results_row_click` calls
`change_current_page(h.page)` itself at `MainWindow.cpp:1896` — taking the default
`Top` — and only then calls `scroll_into_view`, which finds the page already correct
(`PdfCanvas.cpp:261-264`) and never installs the `Hit`. That call site passes
`PageAnchor::hit(...)` explicitly.

### 3.5 Ctrl+wheel zoom

`PdfCanvas.cpp:436-445` submits a single render for `current_page()` through
`post_render_done`. In dual mode that refreshes the left slot only, so after a zoom
the two halves of a spread would be at different magnifications — invisible today
because zoom does not change displayed size at all. Ctrl+wheel routes through
`resubmit_current_page` (`PdfCanvas.cpp:618-632`), which already submits both slots.

### 3.6 Wheel scrolling (`ui/detail/ScrollMath.hpp`)

- `wheel_step_dip(delta, lines_per_notch, vp_h)` — magnitude from
  `SystemParametersInfo(SPI_GETWHEELSCROLLLINES)`; the `WHEEL_PAGESCROLL` sentinel
  degrades to 90 % of viewport height. High-resolution wheels deliver
  `|delta| < WHEEL_DELTA`, so the canvas keeps a residual accumulator and consumes
  whole steps; a flip fires only when an accumulated step crosses the edge, never on
  a fractional notch.
- `apply_wheel(pan_y, content_h, vp_h, step) -> { new_pan_y, Flip::None|Next|Prev }`
  — reports a Flip when already at the edge and the step pushes further.

Scrolling past the bottom flips to the next page landing at its top (`Top`);
scrolling past the top flips to the previous page landing at its **bottom**
(`Bottom`), so back-and-forth scrolling shows continuous content. When the page fully
fits, one notch flips a page. In spread mode the flip steps by spread via
`dual_page_step_next_left` / `_prev_left`.

With the wheel in place, the default zoom mode returns to **FitWidth**.

---

## 4. PR-B — page indicator and go-to-page

Additive except for the layout change; the observer-bypass fixes it depends on are
A2's. Not tiered as a low-risk change: per project rule, tier by risk, never by size.

### 4.1 Status bar (`ui/StatusBar.{hpp,cpp}`)

A `msctls_statusbar32` child of MainWindow hosting an `EDIT`
(`ES_NUMBER | ES_RIGHT`) and a `STATIC` showing `/ 128`. Single part, children at
fixed offsets — no `SB_SETPARTS`. Add `ICC_BAR_CLASSES` to the
`INITCOMMONCONTROLSEX` flags (`MainWindow.cpp:1957-1959`).

**Layout.** `on_layout()` (`MainWindow.cpp:306`) reserves `status_h` at the bottom.
Three places subtract it, not one: `canvas_bottom`, the results-panel rect (whose
bottom is currently the full client height, `MainWindow.cpp:397-399`), and the
splitter drag clamp (`client.bottom - 100`, `:971`). Missing the latter two puts the
results panel's last row under the status bar.

### 4.2 Update path

Hook the existing `canvas_->set_on_page_changed` lambda (`MainWindow.cpp:1024`).
After A2 §3.3 it covers every page transition, including session restore and the
dual-page snaps.

**The empty state is not on that path.** `set_view(nullptr)` returns before reaching
the callback (`PdfCanvas.cpp:158-166`), and `nullptr` is exactly what a last-tab
close passes (`MainWindow.cpp:554`). Driving the indicator only from the observer
would leave it showing the last document's page forever after Ctrl+W on the final
tab. The empty state is cleared explicitly from `on_tab_switch`'s existing `else`
branch — the same branch that already calls `set_pan(0, 0)`.

### 4.3 Go-to-page

The `EDIT` needs subclassing to receive Enter/Esc; reuse the pattern at
`FindBar.cpp:481-531` (`WM_GETDLGCODE` + `VK_RETURN`/`VK_ESCAPE`). Enter parses a
1-based page; out of range reverts the text to the current page (no dialog, no beep);
valid calls `change_current_page(idx, PageAnchor::top())` and returns focus to the
canvas. Esc reverts and returns focus. UI is 1-based, internals stay 0-based.

`WM_MOUSEWHEEL` goes to the focused window, so with the page box focused the wheel
would not reach the canvas. The page-box subclass forwards it. The FindBar and
ResultsPanel edits (`FindBar.cpp:848`, `ResultsPanel.cpp:807`) deliberately do
**not** — their wheel scrolls their own list.

---

## 5. Testing

Catch2, **ASCII `TEST_CASE` names** — non-ASCII names mangle under
`catch_discover_tests` on Windows and fail CI only. Verify through
`ctest --test-dir build -C Release`, not only the test executable. Tests build
**Release** (MuPDF's static libs are `MT_StaticRelease`; Debug fails with
`LNK2038`).

**A1 unit tests**

- `place_bitmap` — `dst_w == src_w` and `dst_h == src_h` for a viewport larger than,
  smaller than, and equal to the source. This is the direct regression assertion for
  Defect 2: a shrink-to-fit reimplementation would pass a centering-only suite.
- `clamp_pan` — centered with pan 0 when content fits; top-left origin and range
  `[viewport - content, 0]` when it overflows; bitmap-px → DIP conversion at
  96 / 144 / 192.
- Zoom math — fit percentages at 96 / 144 / 192 DPI (regression for Defect 3);
  ladder walking from a fit-derived percentage, including one above 4.0 (regression
  for Defect 1); `render_scale()` at 192 DPI is twice `zoom_pct_`.
- Quad mapping is DPI-invariant: the same PDF quad maps to the same DIP rect at 96
  and 192 DPI for a fixed `zoom_pct_` (regression for the reversed-unit error).
- Dual fit — a spread of unequal pages (595×842 and 1200×2000) produces one scale
  that fits both inside a slot.
- Session — a v1 fixture with a Custom zoom restores as v2/FitWidth with tabs and
  window placement intact; a versionless document is migrated as v1, not accepted as
  v2; `session.v1.bak` is byte-identical to the original before the first v2 write;
  an induced backup failure leaves `session.json` at v1.

**A2 unit tests**

- `accept_completion` — a `Right` completion for `left+1` is **accepted** while
  `cur_page == left` (the spread-blanking regression); a `Left` completion for
  another page is rejected; epoch mismatch is rejected; single-page mode rejects
  `Right`.
- Anchor lifetime — an anchor issued at `seq` N is not consumed by a completion at
  seq N−1; a replacement page change carries the intent to the new seq; a null
  completion leaves it pending.
- `ScrollMath` — stepping, residual accumulation for `|delta| < WHEEL_DELTA`, edge
  detection, flip direction, anchor selection.

**B unit tests**

- Page-input parsing — valid, out of range, empty, non-numeric.

**Existing tests these change.** `tests/unit/test_document_view.cpp` breaks two ways
in A1: three `set_zoom_mode` call sites (`:82`, `:96`, `:109`) stop compiling under
the `set_viewport` rename, and seven assertions treat `4.0`/`0.5` as ladder endpoints
(`:130`, `:140`, `:153`, `:155`, `:157-158`, `:160`) and fail at runtime under the
extended ladder. `tests/unit/test_session_state.cpp` changes per the table in §2.5.

**GUI checks** reuse the topmost-window screenshot driver from this session
(`SetWindowPos HWND_TOPMOST` + `CopyFromScreen`; foreground-based capture loses the
race against other applications on this machine).

- A1: Ctrl+`=` visibly enlarges the page; the rendered pixmap width equals the canvas
  width at 200 % (no 2× oversize); arrow keys pan a zoomed page and stop at its
  edges; the default view still shows the whole page.
- A2: pan survives a same-page re-render — scroll mid-page, then resize, toggle F4,
  toggle invert, each leaving the page where it was; spread renders both halves;
  Ctrl+wheel in spread mode leaves both halves at the same magnification; switching
  between two spread tabs never shows the other document's right page; wheel scrolls
  and flips at both edges with the correct landing position.
- B: the page box tracks every navigation path including session restore and
  dual-mode End, and clears when the last tab closes.

## 6. Risks

1. **Session migration** (§2.5). Forward: an explicit v1-accepting parse path plus
   fixture tests. Backward: a genuine one-way door, mitigated by the fail-closed
   `session.v1.bak` copy and a CHANGELOG note.
2. **Completion identity** (§3.1) touches the render-epoch machinery issue #35
   hardened, and its predicate is what keeps spreads rendering at all. The pure
   function exists so it is covered by unit tests rather than GUI observation.
3. Binary size impact is negligible (one status bar, three headers) against the
   19,000,000-byte absolute ceiling.
4. All three PRs modify shipped behavior, so each runs the risk-tiered review stack
   before merge. `VERSION` bumps at the phase boundary only, per project convention.

**Not a risk — checked and refuted:** the CI benchmark gate does not measure this
code path. Timings come from `litepdf-cli --benchmark`, which submits to
`RenderEngine` at a hardcoded `scale = 1.0f` (`src/cli/bench_iteration.cpp:79-82`);
`scripts/benchmark.ps1` uses the GUI exe only for `(Get-Item $GuiExe).Length`.

## 7. Out of scope

True continuous scroll (a multi-page strip layout) — its own phase. Shift+wheel
horizontal scrolling. A zoom-percentage readout. Click-drag hand-tool panning. A
toggle to hide the status bar.

**Pre-existing defect, deliberately not fixed here:**
`cancel_stale_renders(INT_MAX)` at `MainWindow.cpp:547` is a no-op —
`priority > INT_MAX` is never true — so the tab-switch "drain" drains nothing.
Recorded because §3.1 must not assume a tab switch cancels in-flight renders.

## 8. Provenance

Defects 1–3 were found by measurement (§1.1). Defect 4 and the §3 completeness gaps
came from a Fable review of the pre-spec draft (12 findings). The spec gate ran Full
tier — session persistence is a blocklist dimension — with Opus and Sonnet in round 1
(11 findings) and Codex `terra@high` then `luna@max` as lens 3 against the round-1
baseline (16 findings, ~13 unique after dedup). Every finding was anchor-verified
before adoption; none was discarded for a missing contract field.

**Why three PRs.** The original design was one PR, then two. Lens 3's findings were
not uniformly distributed: 13 of 16 landed on one thing — the identity and ordering
of asynchronous render completions in `PdfCanvas`, and the pan/anchor state layered
on top of it. Both Codex lenses independently cleared the *arithmetic* (the unit
contract and the `max(left, right)` fit rule) while faulting the *lifecycle*. The
split follows that evidence: A1 carries the math and the persistence change and
deliberately does not touch a completion path, keeping its default view identical to
today's; A2 carries the lifecycle work whose failure modes are all asynchronous. Each
is separately reviewable, and each is shippable on its own.

The §2.2 render-target-DPI decision rejects the Fable review's suggested fix
direction after checking its cost against the four DIP-intent constants in
`PdfCanvas`. The §2.1 overlay-unit table corrects an assertion this spec itself made
in an earlier draft and Codex refuted.
