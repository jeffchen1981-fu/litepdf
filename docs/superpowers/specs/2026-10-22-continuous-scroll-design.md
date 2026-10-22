# Continuous scroll (#55) — design

- **Issue:** #55 "Continuous scroll: pages are one-at-a-time, with no multi-page strip layout"
- **Status:** design approved in brainstorming; a Fable design review and the spec gate (Opus,
  Sonnet, Codex terra and luna) are folded in (§12); awaiting the user's review
- **Code read at:** `5a40d5e` (main after #165/#166, unreleased after v1.5.6)
- **Supersedes:** the "its own phase" note in
  `docs/superpowers/specs/2026-09-07-zoom-correctness-page-nav-design.md` §7
- **Follow-ups filed:** #167 (two-column continuous strip), #168 (cross-page selection)

## 1. Goal and decisions

LitePDF shows one page (or one two-page spread) at a time; scrolling past a page edge turns the
page. This phase adds a **continuous vertical strip** of pages that scrolls through the whole
document.

Decisions taken with the user (U1-U5 were also checked by the design review, §12):

| # | Decision |
|---|---|
| U1 | Continuous scroll is a **toggle**, and it is **on by default**. Paged mode stays available. |
| U2 | The default flips to on only in the **last PR** of the phase. Before that, continuous mode is reachable only through the registry value (§7.2); the menu item lands in PR 3, together with the overlays, so no build from `main` offers a user a continuous mode without search navigation, highlighting and selection. |
| U3 | Continuous mode and two-page spread are **mutually exclusive** this phase. Turning spread on in a tab shows the paged spread. The two-column strip is #167. |
| U4 | Continuous mode gets a **vertical scrollbar only**. Horizontal panning keeps today's Shift+wheel, tilt wheel and drag. Paged mode gets no scrollbar. |
| U5 | Text selection stays **single-page**: bound to the page under the cursor at button-down. Cross-page selection is #168. |
| U6 | Approach A: one canvas with a strip layout beside the paged one. **Paged behaviour and the existing tests do not change.** Shared entry points (§3.4) do change: each gains a strip branch, and its paged branch keeps today's behaviour. |

## 2. Premises verified against the code

The issue was written at v1.3.0. Verified at `5a40d5e`:

- **Still true.**
  - The canvas holds exactly one bitmap (`PdfCanvas.cpp` `Impl::current_bitmap`), plus
    `right_bitmap` for a spread. It derives `content_extent`, `clamp_pan`, staleness and anchors
    from them.
  - "Current page" (`DocumentView::Impl::current_page`) is authoritative. The status bar, the
    thumbnail highlight and `session.json` read it, mainly through the canvas's page-change
    observer (`MainWindow.cpp` `set_on_page_changed`). `on_tab_switch` also calls
    `ThumbnailPane::set_current_page` directly, and calls `StatusBar::set_empty` for a null view.
  - The canvas has no scrollbars.
  - There is no public all-page-sizes API. `DocumentView::Impl::page_sizes` is a private lazy
    cache: it is sized to the page count and filled for each page that prefetch planning has ever
    looked up (cur ±1 at each turn), and never cleared. A size that throws is not cached.
- **Stale in the issue.**
  - Prefetch is no longer unconditional ±1. Since #49 it fetches both neighbours only while
    cur+prev+next fit the 1 GiB L1 budget, and otherwise only the travel-direction neighbour
    (`PrefetchPlan.hpp`).
  - L1 is bounded by count (5) **and** bytes (1 GiB) since #49. #159 adds a background-tab pin
    (`PageCache::retain_l1_pages`), which only `request_render` and
    `request_render_with_prefetch` release.
  - A page anchor binds to a submission-batch `seq`, not to a page, and an unapplied anchor
    carries forward to the next batch (`PageAnchor.hpp` `AnchorSlot::stamp/take`).
- **Sequencing.** The issue asks for #55 to come after #49, and #49 shipped in v1.5.6. Open #163
  (a page over half the budget is evicted by its own prefetched neighbour) does not block this
  phase: strip prefetch is gated on room (§5.2), so a page that big gets no prefetch at all.

## 3. Architecture

### 3.1 The key simplification

In continuous mode every page size is known up front, and the zoom is one value for the whole
document. So every page's position in the strip can be computed without rendering anything.
Navigation (go-to-page, a search hit, session restore) sets the scroll position
**synchronously**.

The paged mode's asynchronous anchor machinery (`next_seq`, `AnchorSlot`, the wheel identity
guard, `wheel_flip_seq`) is not used by the strip. §3.4 says how it is kept from acting while the
strip is shown.

### 3.2 New units

| Unit | Kind | Responsibility |
|---|---|---|
| `StripLayout` | pure header, `src/ui/detail/` | From all page sizes (pt), `zoom_pct` and the gap: each page's rect in document coordinates, the total height, the page at a given y (binary search on prefix sums), and the visible page range for a y-interval. |
| `StripScroll` | pure header | Clamping `scroll_y`; wheel, key and page steps; re-layout that keeps the viewport-top point in place; the derived current page, including the bottom/top rule and the navigation-intent pin; stored vs effective `pan_x` (§4.2). |
| `StripSchedule` | pure header | From the visible set, travel direction, L1 room and per-page state: which pages get P0, which get P1, which are promoted, and which pending requests to cancel. |
| `StripCompletion` | pure header | The per-page acceptance rule for strip completions. Coexists with `accept_completion`; does not replace it. |
| `EffectiveLayout` | pure header | `(spread, preference, format passed §4.4) → {PagedSingle, PagedSpread, Strip}` (§7.3). |
| `PageBitmapSet` | canvas-internal | Page-keyed D2D bitmaps plus per-page state, kept only for the visible range plus one viewport of hysteresis. |
| `ViewPrefs` | core, registry | Reads and writes the continuous-scroll preference (§7.2). |

New logic lives in these units, not inline in `PdfCanvas.cpp` (already ~2100 lines).

### 3.3 Changes to existing core units

- **`DocumentView`**
  - A continuous flag. While it is set, `set_current_page` does not re-derive the fit (§4.3).
  - `page_sizes()` returns every page's size, built on the existing private cache (§4.4).
  - `request_page_render(page, priority, on_complete, cancel_flag, on_text = {}) ->
    RenderEngine::RenderToken`, where `cancel_flag` is caller-owned. It calls `release_l1_pin()` first, as both existing request
    functions do. Today `request_render` discards the token.
  - `l1_contains(page)`: whether L1 has the page at the current scale and polarity, without
    changing its LRU recency (`PageCache::l1_contains`). The existing `l1_holds` refreshes
    recency and stays test-only.
  - `cancel_thumbnail_renders()` = `cancel_all_below_priority(2)`, which flags queued P3 only
    (§5.2).
  - PR 3: a set-taking **overload** of `trim_l1_for_background` and
    `PageCache::retain_l1_pages`. The existing one- and two-page signatures stay, so the existing
    tests compile unchanged (§9.2).
- **`RenderEngine`.** No new cancel API: the strip cancels a request by storing `true` to the
  caller-owned `cancel_flag` it passed in (#117), which needs no live engine. One
  test-observability counter is added (§9.4).
- **`ux-probe.ps1`.** Two new probes (§9.3), in PR 2.
- **Message id.** `WM_USER_STRIP_DONE = WM_USER + 6` (free at `5a40d5e`). The two reservation
  comments (`PdfCanvas.hpp`, `MainWindow.cpp`, "WM_USER + 3 to + 5") become "+ 3 to + 6".

### 3.4 Shared entry points: what each does in the strip layout

These run in both layouts. Each gets a strip branch; its paged branch keeps today's behaviour.

| Entry point | Today (paged) | Strip layout |
|---|---|---|
| `MainWindow::kick_render(page)`, 21 call sites | `apply_viewport` (refit), refresh the zoom readout, then `request_render_with_prefetch` (or two P0s in a spread) | Gains a `KickReason` argument. The paged branch ignores it. The strip branch dispatches by reason, per the table below. |
| `MainWindow::navigate_click(page)` | `change_current_page`; **returns early if the page did not change**; Top anchor; kick | The strip branch runs **first, before the early return**: strip navigation (§6.3), then a `Navigate` kick. Going to the derived current page while mid-page therefore scrolls to its top. |
| `MainWindow::on_tab_switch` | snapshot, `set_view`, pan, `set_dual_page`, `set_viewport`, kick | Calls `apply_effective_layout` (below) right after `set_view`. Same-index calls (`new_index == old_index`) kick with `Navigate`, not `TabSwitch`. |
| `MainWindow::apply_effective_layout(tab)` (new) | — | The **only** owner of layout switches. Computes `EffectiveLayout` for the tab (or `PagedSingle` for a null view) and, if it differs from the canvas's current layout, does the layout-switch reset below and sets or clears `WS_VSCROLL` (§6.2). Called from `on_tab_switch`, the two-page toggle (both directions), the Continuous Scroll menu item, and tab close. |
| `PdfCanvas::apply_viewport` | refit from the current page, then `set_viewport` | Called only for the reasons that refit (below). |
| `PdfCanvas::resubmit_current_page` (Ctrl+wheel zoom; `on_size`, `on_paint` and the completion handler on `D2DERR_RECREATE_TARGET`) | resubmit the current page | Re-layout (§4.5), then reschedule. |
| `PdfCanvas::discard_render_target` (`WM_DPICHANGED_BEFOREPARENT`, `D2DERR_RECREATE_TARGET`) | reset both slot bitmaps and the render target | Also drops every `PageBitmapSet` bitmap: Ready → None, and a Pending page loses its stale bitmap but stays Pending. On the recreate path `resubmit_current_page` follows and reschedules; on the DPI path the DPI `Layout` kick (`MainWindow.cpp:1490`) does. |
| `PdfCanvas::set_view(v)` | always `++view_epoch` and resets per-view state | Same view and strip layout: no epoch bump and no state reset. A different view: cancel every Pending strip request of the old view (flag store, §3.3), clear `PageBitmapSet`, reset the `StripScroll` state (position, desired `pan_x`, the navigation pin), then bump the epoch. The incoming tab's position is restored by its `TabSwitch` kick (§7.4). |
| `PdfCanvas::change_current_page` | `set_current_page`, then `cancel_gesture()`, then the page-change observer | Not used by the strip. The strip sets its current page through a new `set_strip_current_page` that calls `DocumentView::set_current_page` and fires the observer **without** `cancel_gesture()`, for both the derived page (§4.6) and navigation (§6.3). |
| `WM_USER_RENDER_DONE` / `_RIGHT` handler | `on_completion_arrived` (#57), then `accept_completion`, bitmap, anchor | Unchanged. A layout switch bumps `view_epoch` (below), so a paged completion that lands while the strip is shown fails the epoch check and drops its pixmap. |
| `WM_USER_STRIP_DONE` handler (new) | — | Calls `on_completion_arrived` **first**, before any early return, exactly as the paged handler does (#57), then applies §5.3. |
| `ColdStartTimer::mark(3)` / `mark(4)` | marked when the first `current_bitmap` is created / first painted | The strip marks 3 when it creates its first page bitmap and 4 when it first paints one, so `smoke-test.ps1`'s `LitePDF cold-start:` line is emitted in either layout. |
| Layout-switch reset (inside `apply_effective_layout`) | — | **First** captures the position to carry (§7.3), before anything — including `set_dual_page`, which zeroes both pan axes today — changes the canvas. Then cancels the old layout's outstanding work, `anchor.clear()`, `wheel_flip_seq = 0`, resets both slot bitmaps and `PageBitmapSet`, and does `++view_epoch`. A switch within one tab then applies the carried position (§7.3); a switch caused by a tab switch discards it — the incoming tab restores its own (§7.4). |

**`kick_render` reasons.** Every call site is classified, as of `5a40d5e`. Every strip branch that
changes the zoom calls `refresh_zoom_readout` in the same message, as the paged branch does
(#57: "the readout changes in the same message as the command that caused it").

| `KickReason` | Call sites (`MainWindow.cpp`) | Strip branch |
|---|---|---|
| `Layout`: the canvas size changed | outline toggle (644), status bar toggle (661), thumbnails toggle (730), results splitter (1254), left splitter (1318), WM_SIZE (1410), DPI (1490), results panel show/toggle/close (2362, 2387, 2491) | `apply_viewport` (refits only when the zoom mode is not Custom), refresh the readout, re-layout (§4.5), reschedule |
| `Zoom` | Zoom In (1831), Zoom Out (1838), Reset Zoom (1854) | The zoom value is already set by the command (Reset sets FitWidth and refits). Refresh the readout, re-layout (§4.5), reschedule |
| `Navigate` | `navigate_click` (424: Ctrl+G, page box, thumbnails, outline), Find Next/Prev (2257, 2268), cross-tab result (2478), and a same-index `on_tab_switch` | The caller already ran the strip navigation (§6.3), so this only reschedules. No refit. |
| `TabSwitch` | `on_tab_switch` (863), different index only | A refit event: `apply_viewport`, refresh the readout, restore the tab's strip position (§7.4), re-layout, reschedule |
| `Restore` | session restore (1118) | A refit event (refresh the readout), then navigate to the saved page (§6.3), then reschedule |
| `Restyle` | invert colours (1727) | Cancel all, keep bitmaps as stale, reschedule (§5.2) |
| `Spread` | two-page toggle (1745) | `apply_effective_layout` has already run, in either direction. Turning spread on leaves the strip: the kick is paged. Turning it off with the preference on enters the strip: refit, refresh the readout, re-layout, reschedule — **not** a tab-position restore, which would overwrite the §7.3 carry `apply_effective_layout` just applied. |

A new `kick_render` call site must pick a reason. A call that defaults to one would compile, and
would silently do the wrong thing in the strip.

## 4. Coordinates, layout, zoom

### 4.1 Coordinates

Document coordinates are **`double` DIPs**. A 10,000-page A4 document at 800% is ~67 M DIP tall,
and `float` is exact for integers only up to 2^24 (~16.7 M), beyond which page positions
quantise. Only viewport-relative values (document y minus `scroll_y`) are narrowed to `float`
for D2D.

### 4.2 Layout

- **Gap.** `G = 8` DIP between pages, and also above the first page and below the last (the
  same as the spread gutter).
- **Vertical position.** Pages are indexed from 0, like `current_page`; the page box shows
  index + 1. `top(i) = G + Σ_{j<i} (h_j + G)`, where `h_j = height_pt_j × zoom_pct`. The prefix
  sums are built once, in O(N), and rebuilt on every zoom change. A page's **scroll anchor** is
  `top(i) − G`: the scroll position that shows page i with its gap above it. `anchor(0) = 0`.
- **Horizontal position.** Let `W` be the width of the widest currently visible page.
  - A page narrower than the viewport is centred in the viewport.
  - A page wider than the viewport is centred on the centre line of `W`:
    `x_i = pan_x_eff + (W − w_i) / 2`, where `pan_x_eff ∈ [vp_w − W, 0]`.
  - So a single landscape page does not decentre the portrait pages around it, and every
    overflowing page pans together. Paint, hit-test and selection mapping all use this `x_i`.
- **Stored vs effective `pan_x`.**
  - The canvas stores a *desired* `pan_x` and never clamps the stored value on vertical
    movement.
  - `pan_x_eff = clamp_pan(desired, W, vp_w)`, computed at paint, hit-test and input time.
  - A horizontal input (Shift+wheel, tilt wheel, ←/→, horizontal drag) applies its delta to
    `pan_x_eff` and stores the clamped result as the new desired value.
  - So a reader on the right half of a wide page keeps that position when the page scrolls out
    of view and back in.
- **Pixel rounding.** A rendered bitmap may be 1 px larger than its layout rect (MuPDF pixel
  rounding). It is drawn at the rect's top-left, so the error does not accumulate.

### 4.3 Zoom

- **One zoom for the strip.** The whole strip has one `zoom_pct`, hence one `render_scale`, so
  the L1 key shape is unchanged.
- **Fit events.** FitWidth and FitPage are computed from the current page only at fit events:
  - entering the strip (a layout switch, a `Spread`-off kick);
  - Reset Zoom (Ctrl+0);
  - a `Layout`, `TabSwitch` or `Restore` kick (§3.4), DPI included.

  They are **not** re-derived when the derived current page changes, and not on a `Navigate`
  kick. Otherwise scrolling past, or jumping to, a page of a different size would re-layout the
  whole strip.
- **Scrollbar always present.** In continuous mode the vertical scrollbar stays visible
  (`SIF_DISABLENOSCROLL`). A scrollbar that appears and disappears changes the client width,
  which refits FitWidth and can oscillate.

### 4.4 Page sizes

- `DocumentView::page_sizes()` sizes every page once per document and caches the result. PDF
  uses the #165 path (`pdf_lookup_page_obj`, no page load); other formats use `fz_load_page` +
  `fz_bound_page`. It runs on the UI thread when a tab first needs the strip.
- **Gate (PR 1).** Measured on the UI thread, on the fixtures of §9.4:
  - a 1000-page PDF: ≤ 16 ms;
  - a multi-chapter EPUB and a 300-image CBZ: ≤ 100 ms each.

  A format that misses its gate opens in paged mode, with the menu item greyed for that tab, and
  a follow-up issue is filed. There is no lazy or estimated sizing in this phase.

  Measured in PR 1 (medians of 9): 1000-page PDF 0.410 ms, EPUB 2.329 ms, CBZ 1.770 ms; the
  10,000-page PDF took 3.619 ms.
- **Formats without a gate reading.** `Document` also opens XPS, FB2 and SVG
  (`Document.cpp`'s `kAllowedExts`). XPS and FB2 have no reading above, so they get
  `format_ok = false` and stay paged until a follow-up measures them. SVG always opens as a
  single page and counts as passing.
- **Large documents.** PR 1 also records (not gates) the time for a 10,000-page PDF. Sizing is
  O(N) on the UI thread; above 100 ms at 10,000 pages a follow-up issue is filed (§10).
- **A page whose size cannot be read.** If `page_size` throws for page `i`, page `i` takes page
  0's size; if page 0 itself throws, US Letter (612 × 792 pt). This is new behaviour of
  `page_sizes()`, not of the existing cache. The render, if it succeeds, is drawn at its rect's
  top-left.

### 4.5 Re-layout keeps the viewport-top point

A strip position is a `StripPos`: either `InPage{p, f}` (fraction f ∈ [0, 1) down page p) or
`InGapAfter{p, f}` (fraction f down the gap after page p; `p = −1` is the gap above page 0).
§7.4 stores the same type.

Every re-layout (zoom change, viewport resize, DPI change, refit) follows one rule:

1. Before: record the viewport-top point as a `StripPos`.
2. Re-layout.
3. Set `scroll_y` so that the same point is at the viewport top.

The desired `pan_x` scales by the zoom ratio. One rule covers every event, instead of a
centre-anchored zoom beside a top-anchored resize.

**During a scrollbar thumb drag**, a re-layout ends the drag logically: the canvas applies the
rule above, calls `SetScrollInfo` at once, and ignores every `SB_THUMBTRACK` / `SB_THUMBPOSITION`
until the next `SB_ENDSCROLL`. A tab switch (`set_view` to another view) during a drag counts as
a re-layout. Without this, the track position and the re-layout would fight over `scroll_y`.

### 4.6 Derived current page

- **Rule**, in order:
  1. `scroll_y` at its maximum (the bottom) → the last page;
  2. `scroll_y == 0` → page 0;
  3. otherwise, the page with the largest visible height; ties go to the lower index.

  Rules 1-2 matter at low zoom, where several pages are fully visible and tie: without them,
  scrolling to the bottom would report a page several pages before the last. Rules 1-2 apply
  only when the strip is at least 1 DIP taller than the viewport; otherwise rule 3 decides.
  Both use a 0.5-DIP tolerance: "at its maximum" means within 0.5 DIP of `total − vp_h`, and
  "== 0" means within 0.5 DIP of 0 (`StripScroll.hpp`'s `derive_current_page`).
- **When it is recomputed.** After every scroll input and every re-layout. The observer fires
  only when the value changes.
- **Navigation intent.** An explicit navigation (§6.3) sets the current page to its target
  directly. That pin survives re-layouts and ends at the next scroll input (wheel, keys, drag,
  scrollbar) or the next navigation. While it holds, recomputation keeps the pinned page.
- **Side effects of a change:**

| Side effect | Continuous mode |
|---|---|
| Status bar page box | kept |
| Thumbnail highlight + auto-scroll | kept |
| `session.json` save (1500 ms debounce) | kept; the debounce bounds the write rate |
| Text slot drop (#98 follows the current page) | kept, plus the text request of §5.2 |
| Cancel the selection gesture (`change_current_page`) | **not done**: a selection is bound to its own page |
| Fit re-derivation | **not done** (§4.3) |

- **Degenerate documents.** 0 pages: no strip. 1 page: a strip of one page.

## 5. Rendering

### 5.1 Per-page state

`PageBitmapSet` keeps the following state for each page in the visible range, plus one viewport
of hysteresis:

| State | Holds |
|---|---|
| **None** | nothing |
| **Pending** | `{seq, priority, scale, invert, cancel_flag, stale_bitmap?}` (scale and invert as submitted) |
| **Ready** | `{bitmap, scale, invert}`; *stale* when scale or invert differs from the view's |
| **Failed** | `{priority}` of the request that failed |

A stale bitmap is still painted, stretched to the page's current rect, so a zoom never flashes
the strip white.

Transitions:

- None → Pending: a submit.
- Ready (stale) → Pending: a submit; the bitmap moves into `stale_bitmap`.
- Pending → Ready / Failed / None: a completion (§5.3).
- Pending → (cancel) → Ready (stale) if it holds a `stale_bitmap`, otherwise None. This happens
  **at cancel time**; the late null is then dropped by §5.3 rule 2, so a page that re-enters V
  before that null arrives is simply re-requested.
- Ready → None, and Pending loses its `stale_bitmap`: `discard_render_target` (§3.4).
- Failed → None: the page leaves V, or the render scale or polarity changes, or — for a page
  that failed at P1 — the page enters V (so a failed prefetch gets one P0 attempt). A page that
  failed at P0 is not re-requested while it stays in V, so a page that keeps failing costs one
  render, not one per scroll event.
- Any state → released: the page leaves the range (its request, if Pending, is cancelled
  first). This bounds the GPU bitmap count.

### 5.2 Scheduling (`StripSchedule`)

Run after every scroll or re-layout, after every kick, and after a §5.3 rule-4 completion:

0. **Sweep.** Every Pending page whose `cancel_flag` reads `true` takes its cancel transition
   (§5.1) now. Code outside the strip flags strip requests — `on_tab_switch`'s
   `cancel_background_renders()` (even for a same-index switch) and `set_invert_colors` — and a
   request cancelled after a worker found it in L1 posts **no** completion at all
   (`RenderEngine.cpp:183-185` drops the pixmap and skips `on_complete`, unlike checkpoints A, B
   and C). So the strip cannot rely on a completion to learn of a cancel. Both outside paths are
   followed by a kick in the same message (`MainWindow.cpp:863` / `1727`), so the sweep runs
   before the user can see the page stuck. A late completion for a swept request is then
   dropped by §5.3 rule 2.
1. **Visible pages (V)** = pages intersecting the viewport. Each page in V that is None, or
   Ready-stale, gets **P0**. A page in V that is Pending at **P1** is promoted: its P1 is
   cancelled and it is submitted at P0. Pages are submitted largest-visible-area first; the
   engine orders by priority, then arrival.
2. **Prefetch pages (P)** = pages within one viewport height ahead, in the travel direction.
   Each page in P that is None gets **P1**, but only if it fits L1 alongside V: there must be
   `l1_capacity − |V|` entries free **and** `l1_budget − bytes(V)` bytes free. Otherwise it gets
   no prefetch, because a prefetched page would be evicted by the visible ones. This keeps #49's
   "prefetch only what fits" rule.
3. **Cancel.** Pending pages not in V ∪ P are cancelled (flag store) and take their cancel
   transition (§5.1). A cancelled entry stays in the engine's heap until a worker pops it and
   skips it at checkpoint A; at most `num_workers` (2) requests a worker had already taken finish,
   and their results are discarded. So the **live** work stays at about |V| + |P| during a fast
   scrollbar drag, without throttling.
4. **Thumbnails.** When V changed in this pass, call `cancel_thumbnail_renders()`, exactly as
   every paged page turn's `cancel_stale_renders(0)` cancels the queued thumbnails today. The
   #117 handler redraws the cancelled rows that are still visible.

**Text prefetch (#98).** `on_text` can only ride on a submitted request, and a second request
for a page that is not in L1 would rasterise it again. So:

- When a P0 is submitted for the page that will be the current page (the largest-visible-area
  page of the pass), it carries `on_text`, with `text_wanted` set to that page.
- When the derived current page changes to page c, or c's P0 completes while c is current, and
  c's text is neither in the slot nor pending: if `l1_contains(c)`, submit a text-carrying
  `request_page_render(c, P1, cb, on_text)` and cancel the earlier **text-only P1**, if any. A P0
  that carries `on_text` is never cancelled for text reasons: moving `text_wanted` to c already
  makes its text build skip (#98 skip rule 2). If c is not in L1, submit nothing; a press on c
  builds the text synchronously (R4 of the selection spec).
- The worker also skips a text build while any P0 is queued (#98 skip rule 3). That skip is not
  retried, exactly as in paged mode: #98 allows any skip, and the press then builds the text
  synchronously (§10).
- That text request does not change c's page state. Its pixmap completion never matches a
  Pending seq of c, so §5.3 rule 2 drops it; the worker builds the text regardless, because
  `on_text` depends on the pixmap having been delivered, not on the canvas accepting it.
- The existing `wants_text_prefetch` gate still applies.

**Render scale or polarity change** — a zoom, a refit, a DPI change (`render_scale` includes the
DPI), or invert. Cancel every Pending request (§5.1 cancel transition), mark Ready bitmaps stale,
and reschedule.

### 5.3 Completion (`StripCompletion`)

Paged mode posts a null completion with no identity (`PdfCanvas.cpp` `post_render_done_impl`:
`WPARAM=0, LPARAM=0`); this is why #129 cannot cancel P0. Strip completions use
`WM_USER_STRIP_DONE`, whose payload **always** carries `{epoch, page, seq, cancelled}`, for null
results too.

The handler first calls `on_completion_arrived` (§3.4), then decides, in order:

1. epoch ≠ view epoch → drop (the view, or the layout, changed);
2. the page is not Pending, or seq ≠ its Pending seq → drop;
3. a pixmap is present → create the D2D bitmap (creating the render target first if needed):
   - success → Ready at the Pending entry's submitted scale and polarity; invalidate that page's
     rect. If those differ from the view's (a change the cancel missed), the page is Ready-stale
     and the next pass re-requests it;
   - no render target could be created, or `D2DERR_RECREATE_TARGET` → `discard_render_target`,
     the page → None, reschedule;
   - any other failure (for example a bitmap above the device's maximum size) → Failed;
4. null and `cancelled` → the cancel transition (§5.1), then reschedule. Usually the §5.2 sweep
   has already done this and rule 2 drops the completion; this rule covers a flag set after the
   last sweep;
5. null and not cancelled → Failed. Paint a white page with a thin border, and do not retry
   automatically. The page is re-requested when it leaves V and comes back, or on a zoom or
   invert change.

The #117 caveat applies: a flag store can hit a request that a worker has already taken, so a
genuine failure can be reported as cancelled. Rule 4 then reschedules, so the page is requested
again at most once per pass.

**Known limit.** If the payload allocation or the `PostMessageW` itself fails on the null path
(out of memory, a full message queue, or process exit), nothing is posted, and the page stays
Pending (blank) until it leaves V and comes back.

### 5.4 Memory

- **Unchanged from today.** A visible page exists twice: as an L1 pixmap and as a D2D bitmap.
  High zoom still renders whole pages (no tiling).
- **High zoom.** At 800% one page is much taller than the viewport, so V is usually 1–2 pages.
- **Many visible pages.** When more pages are visible than the L1 count cap of 5 (a tall
  window at 25%, for example), the visible pages are shown from the canvas's own bitmaps, so L1
  eviction does not re-render them. It does disable prefetch, and it makes the §5.2 text request
  rarer (it is submitted only for a page still in L1).
- **Tab switch.** Handled by `set_view` (§3.4). #159 keeps all of V for a background strip tab
  (§7.4).

## 6. Input, scrollbar, navigation

### 6.1 Input (continuous mode; paged mode unchanged)

| Input | Paged (today) | Continuous |
|---|---|---|
| Wheel | scroll within the page, flip at the edge | `scroll_y` via `consume_notches` / `wheel_step_dip`: 48 DIP per notch, or 90% of the viewport for "one screen at a time". Never flips. |
| ↑ / ↓ | `pan_by` ±100 DIP | `scroll_y` ±100 DIP |
| ← / → | `pan_x` ±100 DIP | desired `pan_x` (§4.2) |
| Shift+wheel, tilt wheel | `pan_x` only | desired `pan_x` |
| PgDn / PgUp | next/previous page, Top | scroll 90% of the viewport height |
| Home / End | first/last page, Top | document top / bottom |
| Drag pan | pan within the page | dy → `scroll_y`, dx → desired `pan_x` |
| Ctrl+wheel | zoom | zoom, then `resubmit_current_page` → re-layout (§3.4, §4.5) |

**The wheel's two guards.** The identity guard and the `wheel_flip_seq` latch exist to hold the
wheel while a flip's render is pending. Continuous mode does not flip, so it uses neither. Paged
mode keeps both: decision 4 in the project memory note on the zoom/page-nav phase
(`project_zoom_pagenav_spec_shipped`).

**PgDn changes meaning in continuous mode.** This follows Chrome and Edge, the readers that the
text-selection spec models. Page-by-page jumping stays available through the page box, Ctrl+G
and the thumbnails.

**Hand cursor.** `can_pan()` in the strip layout is true when the strip is taller than the
viewport or `W` is wider than it. The existing cursor rules then apply unchanged.

### 6.2 Vertical scrollbar

- `WS_VSCROLL` is set on the canvas window only while the canvas shows the strip, and only
  `apply_effective_layout` changes it (§3.4). The style is changed first (`SetWindowLongPtr` +
  `SetWindowPos` with `SWP_FRAMECHANGED`), and only then does `apply_viewport` read
  `GetClientRect`, so the first fit already uses the right client width. A paged tab, a null
  view, and an empty canvas after the last tab closes have no scrollbar.
- `SIF_DISABLENOSCROLL` keeps it visible while the content fits.
- Scrollbar units are rounded DIPs: `nMin = 0`, `nMax = total − 1`, `nPage` = viewport height.
  Win32's largest `nPos` is then `nMax − nPage + 1`, which, because of the rounding, can differ
  from the exact maximum scroll (`total − vp_h`) by up to about 1 DIP. So a thumb or track
  position equal to the largest `nPos` maps to the exact maximum scroll, 0 maps to 0, and every
  other position maps 1:1 to `scroll_y`. 67 M is well within `int`.
- A thumb drag reads `GetScrollInfo(SIF_TRACKPOS)`, which is 32-bit. `WM_VSCROLL`'s
  `HIWORD(wParam)` is only 16-bit and wraps above 65535.
- The scrollbar arrows act as ↑/↓, and track clicks act as PgUp/PgDn.
- Every `SB_THUMBTRACK` reschedules; §5.2's cancellation keeps live work bounded. A re-layout
  during the drag ends it (§4.5).

### 6.3 Navigation

An explicit navigation sets the current page directly and pins it (§4.6), runs synchronously in
the canvas, and is followed by a `Navigate` kick, which only reschedules.

| Source | Continuous behaviour |
|---|---|
| Ctrl+G / page box, thumbnail click, outline click (`navigate_click`) | `scroll_y = anchor(p)`, clamped; current page = p. No anchor object is installed. Runs even when p is already the current page (§3.4). |
| End | bottom; current page = last |
| Home | top (`scroll_y = 0 = anchor(0)`); current page = 0 |
| Find Next/Prev, cross-tab result row (`scroll_into_view`) | No scroll if the hit quad is fully visible. Otherwise, centre it vertically (the `pan_y_for_hit` rule, computed in document coordinates). Current page = the hit's page. |
| Session restore | `scroll_y = anchor(saved page)`; current page = that page |
| Print | reads the current page; unchanged |

Because positions do not wait for a render, the two right-page residuals recorded on #50 do not
arise in continuous mode.

**Search start page (#140).** In the strip layout, `find_start_page` returns the **top visible
page** (the first page of V), not the current page. Every page on screen is then scanned first,
in reading order.

## 7. Overlays, mode, preference, tab state

### 7.1 Overlays (PR 3)

- **Search hits.** `hits_fn(page)` is called for every page in V, and quad origins come from that
  page's viewport rect. The current-hit style is unchanged. #135's overlay-diff repaint works
  per page rect, with the same diff rule.
- **Selection: press.**
  - The page under the cursor, looked up in document coordinates, becomes the selection's page.
  - A press in a gap, or outside every page, starts **no** gesture. In Chars mode it clears the
    selection, as a click without a drag does today. The hand tool's drag-pan is unaffected.
- **Selection: drag.** Points map into the **start page's** placement (its `x_i` and `top`), and
  `canvas_dip_to_page_point` keeps clamping them to that page's rect, whatever page is under the
  cursor. So the selection stops at the start page's start or end.
- **Selection: text and painting.**
  - Text comes from the #98 slot when the start page is the current page and its text is ready;
    otherwise it is built synchronously (cost recorded in R4 of the selection spec).
  - A selection is painted whenever its page is visible.
  - Select All selects the current page, as today. Copy is unchanged. There is no autoscroll
    during a drag (none exists today).
- **Cursor.** An I-beam over any page rect, an arrow over gaps; the hand-cursor rule is in §6.1.

### 7.2 Preference

- The preference is stored at `HKCU\SOFTWARE\LitePDF\Settings` as DWORD `ContinuousScroll`, a
  sibling of the MRU key `SOFTWARE\LitePDF\MRU`.
- A missing value means the default: 0 in PR 1–3, 1 from PR 4.
- It is read once at start-up, and written when the menu item is toggled (PR 3 onward). A
  registry change while the app runs takes effect at the next start.
- `session.json` is **unchanged**. Its parser rejects unknown keys, so a new key would make an
  older build drop the whole session.

### 7.3 Mode

- **Menu item (PR 3).** View menu: `&Continuous Scroll` below `&Two-Page Spread`, checked when the
  preference is on, with no accelerator. It is greyed when no document is open, or when the
  format missed the §4.4 gate.
- **Effective layout per tab** (`EffectiveLayout`):

| Condition | Layout |
|---|---|
| spread on in that tab | paged spread |
| else preference on and the format passed §4.4 | continuous |
| else | paged single |

"The format passed §4.4" means both that the format passes (XPS and FB2 do not yet; SVG does)
and that the document has at least one page: a 0-page view stays paged (§4.6, "0 pages: no
strip").

- **Choosing Continuous Scroll in a spread tab** turns the preference on and turns that tab's
  spread off. Turning spread on does not change the preference, and turning it off returns to
  the preference's layout. Changing the preference re-applies the effective layout to the active
  tab only; a background tab picks it up at its next `on_tab_switch`.
- **Every layout switch** goes through `apply_effective_layout`, which first does the reset in
  §3.4.
- **Position carry, within one tab** (spread toggle, menu item). The carried value is
  (page, in-page offset in DIP), captured before the reset (§3.4):
  - **From paged single.** page = the current page; offset = how far the page has been scrolled
    into, `−pan_y − page_origin_y`, clamped to [0, the page's height] (0 when the page fits).
  - **From paged spread.** page = the spread's left page; offset = how far the viewport top is
    below that page's top inside the union placement (`content_extent`'s box), clamped to
    [0, the left page's height]. So unequal page heights are measured on the left page alone.
  - **From the strip.** page = the derived current page; offset = viewport top − `top(page)`,
    clamped to [0, the page's height].
  - **Into the strip.** `scroll_y = anchor(page)` when the offset is 0, else `top(page) + offset` (the offset is measured from the page's top). A page that fits lands exactly where
    navigating to it would put it. The current page is the carried page, pinned (§4.6).
  - **Into paged single.** Paged `pan_y` = −offset, clamped; `pan_x` = `pan_x_eff`.
  - **Into paged spread.** The spread containing the page; `pan_y` = −(the left page's top in
    the union + offset), clamped to the union; `pan_x` = 0, because the union's horizontal layout
    differs from the strip's.
  - Into either paged layout, the pan is set before the render is kicked. The switch cleared the
    anchor, so the paged completion path only re-clamps the pan and the position survives.
- **A tab switch does not carry position** between tabs: the incoming tab restores its own
  (§7.4).

### 7.4 Tab state

- `Tab` keeps the strip position in memory only, as a `StripPos` (§4.5, so a viewport top in a
  gap is kept too) plus the desired `pan_x`, rather than a raw `scroll_y`. It is written by `on_tab_switch`'s outgoing snapshot (beside
  `pan_x/pan_y` today) and restored by a `TabSwitch` kick. A background tab whose viewport or DPI
  changed then returns to the same place after re-layout. A same-index switch restores nothing
  (its kick is `Navigate`).
- #159 (PR 3): in continuous mode the trim keeps all of V, through the new set-taking overload;
  `shown_pages` is unchanged, and a strip-specific helper returns V, capped at the L1 count cap.
- The thumbnail highlight follows the derived current page through the existing observer. §5.2
  step 4 bounds the queued thumbnail work during a fast scroll.

## 8. PR decomposition

Every PR leaves paged single and paged spread behaviour, and the existing tests, unchanged
(§9.2).

1. **Foundations** (no user-visible change):
   - `StripLayout`, `StripScroll`, `StripSchedule`, `StripCompletion` and `EffectiveLayout`, with
     unit tests;
   - the `DocumentView` continuous flag (no refit on a page change), with its test;
   - `page_sizes()`, the §9.4 fixtures, and the §4.4 gate measurement;
   - `request_page_render` (releases the pin), `l1_contains`, `cancel_thumbnail_renders`;
   - `ViewPrefs`, default off;
   - the started-render counter and the §9.4 in-process scheduling test harness.
2. **The strip renders and scrolls**, reachable only through the registry value:
   - `PageBitmapSet`, painting and the `WM_USER_STRIP_DONE` handler;
   - every shared entry point of §3.4, including `apply_effective_layout` and the `KickReason`
     classification;
   - wheel, keys, drag, PgDn, Home/End and the scrollbar;
   - scheduling, promotion, cancellation and the thumbnail cancel;
   - the derived current page and its side effects, including the text request;
   - re-layout on zoom, resize and DPI change;
   - navigation (go-to-page, thumbnails, outline, restore) and Find navigation in strip
     coordinates;
   - layout transitions (through the two-page toggle) and tab state;
   - the two `ux-probe.ps1` probes and the PR 2 smoke rows of §9.3.
3. **Overlays and the menu item:**
   - search hits on every visible page;
   - selection on the page under the cursor, and the cursor rules;
   - #159 retaining V (the set-taking overloads);
   - the `&Continuous Scroll` menu item.
4. **Default on and release readiness:**
   - the default flipped to 1;
   - the memory measurement and the GUI drag check (§9.4);
   - smoke updated (§9.3) and `docs/SMOKE-CHECKLIST.md`;
   - CHANGELOG `[Unreleased]` entries.

`VERSION` is not bumped in any of these PRs; under the release convention only the release PR
bumps it.

## 9. Testing and measurement

### 9.1 Unit tests (pure headers and core)

- **`StripLayout`**
  - prefix sums and `anchor(i)`;
  - page-at-y at a gap, at the first page's top, at the last page's bottom;
  - `double` precision at 10,000 pages × 800%, with a control case that fails under `float`;
  - the visible range;
  - mixed-width horizontal placement, including a page wider than the viewport but narrower
    than `W`.
- **`StripScroll`**
  - clamping;
  - viewport-top preservation across re-layout, including a `StripPos` in a gap;
  - the derived current page: bottom → last, top → 0, ties, the navigation pin surviving a
    re-layout and ending on scroll input;
  - desired vs effective `pan_x` when a wide page leaves V and comes back.
- **Position carry** (pure, beside `StripScroll`): the offset capture from paged single, paged
  spread with unequal page heights, and the strip, and its application into each layout.
- **`StripSchedule`**
  - the sweep: a Pending page whose flag was set from outside, with no completion ever posted,
    leaves Pending in the next pass and is re-requested if still wanted;
  - V and P; promotion of a P1-pending page entering V; a page that failed at P1 entering V
    gets one P0, a page that failed at P0 does not;
  - count room and byte room;
  - travel direction;
  - the cancel set and both cancel transitions (to None, to Ready-stale);
  - the thumbnail cancel only when V changed;
  - the text request: on the predicted current page's P0, and as a P1 only when `l1_contains`;
  - full reschedule on a scale change, keeping stale bitmaps.
- **`StripCompletion`**
  - epoch mismatch;
  - a page that is not Pending;
  - stale seq;
  - cancelled (with and without a stale bitmap) vs failed;
  - success, recreate-target and other bitmap failures;
  - a success whose submitted scale differs from the view's lands Ready-stale.
- **`KickReason` dispatch** and **`EffectiveLayout`.** Table tests, including Spread off with the
  preference on, and a same-index tab switch mapping to `Navigate`.
- **`ViewPrefs`.** A registry round trip under a test subkey, as the `MruList` tests do.
- **`DocumentView`**
  - no refit on a page change in continuous mode;
  - a token is returned;
  - `request_page_render` releases the pin: after `trim_l1_for_background(0)`, a rendered page 5
    is admitted to L1;
  - `l1_contains`;
  - `page_sizes()`, including the size-failure fallbacks;
  - the set-taking `retain_l1_pages` overload (PR 3).

### 9.2 Regression

In PR 1–3, the existing tests that pin single-page behaviour are not edited and must pass:

- `test_scroll_math`
- `test_completion_math`
- `test_page_anchor`
- `test_viewport_math`
- `test_dual_page_layout`
- `test_prefetch_plan`
- `test_document_view`
- `test_page_cache_l1`
- `test_text_prefetch_view`
- `test_selection_drag`
- `test_session_state`
- `test_render_engine_cancel`

New APIs are added as overloads or new functions, never by changing a signature these tests call
(`retain_l1_pages`, `trim_l1_for_background`, `shown_pages`). Any unavoidable edit is justified
line by line in the PR body. The suite size is read from `ctest -N` at the time, never
hard-coded.

### 9.3 GUI smoke (pixel-free oracles first)

New `ux-probe.ps1` probes (PR 2):

- the canvas's scrollbar state (`GetScrollInfo` on a standard scrollbar, read cross-process; PR 2
  checks first that the probe returns the canvas's real range, against a known document);
- the page-box text.

Scripted on a 50+ page fixture, with `ContinuousScroll=1` set before launch:

| Action | Expected |
|---|---|
| wheel to the bottom | scroll pos = `nMax − nPage + 1`; page box = last page |
| Ctrl+G 30 (index 29) | page box = 30; scroll pos = `anchor(29)` |
| Ctrl+G to the current page while mid-page | scroll pos moves to that page's anchor |
| two-page on, then off, from mid-page | after off: same page box, same in-page offset (±1 DIP) |
| a tab with two-page on (negative control) | no `WS_VSCROLL` on the canvas |
| switch back to a strip tab | `WS_VSCROLL` present |
| cold start in continuous mode | `WS_VSCROLL` present first (so the run really is the strip), then the `LitePDF cold-start:` line present |
| strip tab A: Ctrl+G 20, then switch to strip tab B without scrolling | page box = B's own page, not 20 |

PR 3 adds the same transition row through the menu item. From PR 4 onward, existing smoke steps
that test paged behaviour set the preference off explicitly. Otherwise they would silently run in
continuous mode and test something else.

### 9.4 Fixtures and measurement gates

**New fixtures (PR 1).** Generated by scripts under `scripts/`, like the existing ones:

- a 1000-page PDF (today's `large.pdf` has 500 pages) and a 10,000-page PDF for the §4.4
  measurement;
- a multi-chapter EPUB (≥ 200 chapters);
- a CBZ of ≥ 300 images.

The existing `sample.epub` (1.5 KB) and `sample.cbz` (18 KB) cannot produce a failing reading
for a 100 ms gate.

**The started-render counter** counts main-view renders (`bypass_cache == false`) that miss L1,
i.e. that actually build a display list or rasterise. Requests skipped at checkpoint A, L1 hits
(including §5.2 text requests served from L1) and P3 thumbnails are not counted. It is read
in-process by tests.

**Scheduling gate (PR 1, in-process).** A worker throughput figure cannot test cancellation, so
the gate holds the workers instead, using the #49 test seam (hold the single worker inside the
first callback):

1. Build a `DocumentView` on the 1000-page fixture with one worker, and hold that worker.
2. Drive `StripSchedule` through 1000 successive viewports, top to bottom, as a drag would.
3. Assert that the live (non-cancelled) queued requests never exceed |V| + |P| + 1.
4. Halfway, call `cancel_background_renders()` from outside and run one pass: no page may stay
   Pending with its flag set (the §5.2 sweep).
5. Release the worker and drain. Assert that started renders ≤ |final V| + |final P| + 1, and
   that every page of the final V ends Ready.
6. Mutation checks: with the cancel step removed, step 5's count exceeds the bound by hundreds;
   with the sweep disabled (a flagged Pending page never counts as swept), step 4 finds a stuck
   page. Emptying only the plan's sweep list is not a discriminating mutant: the plan already
   judges a flagged page by its cancel transition and resubmits or cancels it in the same pass,
   so the test also asserts that every flagged page appears in that list.

| Gate | PR | Condition |
|---|---|---|
| Page sizing | 1 | Measured on the new fixtures: 1000-page PDF ≤ 16 ms; EPUB and CBZ ≤ 100 ms each. A miss disables continuous for that format, and a follow-up issue is filed. The 10,000-page figure is recorded. |
| Scheduling | 1 | The in-process test above, plus its mutation check. |
| Memory | 4 | Scroll down 10 pages at 800%: peak private bytes ≤ 1.25 × paged mode's 10 × PgDn (the #49 recipe). CPU time per scroll step is recorded beside it, as the L1-hit oracle (#49): a P0 miss shows as a second full render. |
| GUI drag | 4 | A real `SendInput` thumb drag from top to bottom of the 1000-page fixture (a posted `WM_VSCROLL` sets no track position), with the thumbnails pane closed and open. Oracle: within 5 s of release the page box shows the last page and the process is idle (CPU below 2% for 1 s). A sanity check, not a count gate. |
| Existing CI | every PR | benchmark regression check; exe size gates (smoke 12 MB, bench 19 MB); `smoke-test.ps1` including the cold-start line |

### 9.5 Phase acceptance

- Continuous scroll is on by default, with the behaviour of §4–§7.
- Paged single and paged spread behave as before.
- #167 and #168 are filed (done).

## 10. Out of scope / known limits

- The two-column continuous strip (#167) and cross-page selection (#168).
- Tiled rendering at high zoom: whole pages are rendered, as today. A page above the device's
  maximum bitmap size shows as Failed (§5.3 rule 3).
- A horizontal scrollbar, and cursor-anchored zoom.
- Autoscroll while drag-selecting.
- Lazy or estimated page sizing: a format that misses the §4.4 gate stays paged; sizing a very
  large document is O(N) on the UI thread (§4.4).
- A null completion that cannot be posted leaves a page blank until it re-enters V (§5.3).
- Changing the registry value while the app runs takes effect at the next start (§7.2).
- A text build the worker skips because a P0 is queued (#98 skip rule 3) is not retried; a press
  on that page builds its text synchronously, as in paged mode (§5.2).

## 11. Risk and review

- **Risk.** The highest risk is in PR 2 (the shared entry points of §3.4, painting and the
  completion path) and PR 3 (overlay coordinates). PR 1 is mostly pure math.
- **Review.** Each PR's gate tier is set by risk at that gate (`risk-tiered-review`).

## 12. Review record

### 12.1 Fable design review (before the spec was written)

A Fable review of the brainstorming draft at `5a40d5e` found 15 issues and raised 7 questions.
No finding was discarded. Each is resolved in this spec:

| Finding | Resolution |
|---|---|
| F1 `kick_render` → `apply_viewport` refits on every navigation | §3.4 `KickReason`; §4.3 fit events |
| F2 same-view `set_view` bumps the epoch | §3.4 `set_view` row |
| F3 the strip completion skips `on_completion_arrived` (#57 readout) | §3.4, §5.3 |
| F4 `discard_render_target` leaves strip bitmaps on a dead target | §3.4, §5.1 |
| F5 a new request path does not release the #159 pin | §3.3, §9.1 |
| F6 the cancel transition is undefined | §5.1, §5.2 step 3, §5.3 rule 2 |
| F7 #98 text prefetch never fires in the strip | §5.2 text prefetch |
| F8 late paged completions and a carried anchor after a layout switch | §3.4 layout-switch reset, §7.3 |
| F9 no `ColdStartTimer` marks in the strip → smoke throws once default-on | §3.4, §9.3 |
| F10 gates that can only pass; no 1000-page fixture; counter undefined | §9.4 |
| F11 no thumbnail cancel in the strip | §5.2 step 4 |
| F12 a gap press is undefined | §7.1 |
| F13 a user-reachable mode before Find works; `retain_l1_pages` placed in two PRs | U2, §8 |
| F14 `pan_x` lost when a wide page leaves V | §4.2 stored vs effective `pan_x` |
| F15 re-layout during a thumb drag | §4.5, §6.2 |
| U6 "paged code is not modified" is impossible as a code boundary | U6 rewritten; §3.4 |
| Questions: drag clamp; memory gate attribution; `WS_VSCROLL` timing; search start page; hand cursor; page 0 size failure; the "decision 4" citation | §7.1; §9.4; §6.2; §6.3; §6.1; §4.4; §6.1 |

### 12.2 Spec gate, round 1 (Full tier: Opus slot 1, Sonnet slot 2, at `2e9381b`)

Opus raised 16 findings and 1 question; Sonnet raised 8 important and 5 minor findings and 4
questions. Every anchor was verified against the code; none was discarded. Overlaps are merged:

| Finding (lens) | Resolution |
|---|---|
| `navigate_click` returns early on the same page (Opus F3, Sonnet 1) | §3.4 `navigate_click` row; §6.3; §9.3 row |
| No owner for layout switches; tab switch across layouts; spread off (Opus F4, F12, Sonnet 2) | §3.4 `apply_effective_layout`, Spread row; `EffectiveLayout`; §6.2; §7.3 |
| Registry read once vs the PR 2 transition smoke row (Opus F7, Sonnet 3) | §9.3 uses the two-page toggle; §7.2, §10 |
| `CreateBitmap` / render-target failure in the strip handler (Opus F2, Sonnet 4) | §5.3 rule 3; §10 |
| Ctrl+G oracle off by one (Sonnet 5) | §4.2 indexing; §9.3 |
| `top(cur)` vs `top(p) − G` (Sonnet 6) | §4.2 `anchor(i)`; §6.3; §7.3 |
| Fast-scroll gate measures throughput, has no channel, vacuous mutation (Opus F6, Sonnet 7, Opus question) | §9.4 in-process scheduling gate; GUI drag as a sanity check with `SendInput` |
| Rule 4 is live, not defensive; P1-pending pages never promoted (Opus F10, Sonnet 8) | §5.2 step 1 promotion; §5.3 rule 4 reschedules |
| Ready-stale has no transition (Opus F1) | §5.1 `stale_bitmap`, cancel transitions |
| Same-index `TabSwitch` refits (Opus F5) | §3.4 `on_tab_switch` row, `Navigate`; §7.4 |
| Wheel to the bottom reports N−k at low zoom (Opus F8) | §4.6 rules 1-2 |
| Text request re-rasterises (Opus F9, Sonnet question 1) | §5.2 text prefetch (`l1_holds`, P0-carried); §5.4 |
| Pin vs re-layout (Opus F11) | §4.6 navigation intent |
| Readout refresh on refitting kicks (Opus F13) | §3.4 reasons preamble and rows |
| Horizontal origin of a mid-width overflowing page (Opus F14) | §4.2 `x_i` |
| PR 3 API change vs unedited tests (Opus F15) | §3.3 overloads; §9.2 |
| Scrollbar `nMax` off by one (Opus F16) | §6.2 |
| `page_sizes` "only cur ±1" is false (Opus, Sonnet M1) | §2; §4.4 |
| `resubmit`/`discard` prose loose (Opus, Sonnet M2) | §3.4 rows |
| Counter definition contradicts itself (Sonnet M3) | §9.4 |
| 25% arithmetic (Sonnet M4) | §5.4 |
| PR placement of the flag and the probes (Sonnet M5) | §8 |
| Queue-size claim; flag store vs `engine->cancel`; null post failure (Opus negatives) | §5.2 step 3; §3.3; §5.3 known limit |
| Large-document sizing time; message id (Sonnet questions 2, 3) | §4.4, §10; §3.3 |

### 12.3 Spec gate, Codex lens (terra@medium, then luna@max, both at `f6b7916`)

Both ran on the same pre-extracted evidence bundle. terra raised 1 finding and 2 questions; luna
raised 8 findings. No overlap. Every anchor was verified; none was discarded.

| Finding (lens) | Resolution |
|---|---|
| A late cancel on an L1 hit posts no completion, so a Pending page never learns it was cancelled (terra) | §5.2 step 0 sweep; §5.3 rule 4; §9.1; §9.4 steps 4 and 6 |
| Which text request is cancelled is ambiguous (terra question) | §5.2 text prefetch: the text-only P1, never a P0 |
| The scheduling gate never exercises an outside cancel (terra question) | §9.4 step 4 |
| A derived-page change through `change_current_page` cancels the selection gesture (luna) | §3.4 `change_current_page` row (`set_strip_current_page`) |
| The navigation pin leaks across strip-tab switches (luna) | §3.4 `set_view` row; §9.3 row |
| A DPI change can accept an old-scale Pending render as current (luna) | §5.1 Pending records scale; §5.2 render-scale change; §5.3 rule 3 |
| A text build skipped behind a queued P0 is never retried (luna) | Accepted as in paged mode (#98 allows any skip): §5.2, §10 |
| A page that failed at P1 is never requested at P0 when it enters V (luna) | §5.1 Failed → None |
| Spread position carry has no source coordinate, and `set_dual_page` zeroes the pan first (luna) | §3.4 layout-switch reset captures first; §7.3 carry rules; §9.1 |
| Tab position cannot represent a viewport top in a gap (luna) | §4.5 `StripPos`; §7.4 |
| The cold-start smoke row passes in paged mode too (luna) | §9.3 row checks `WS_VSCROLL` first |
