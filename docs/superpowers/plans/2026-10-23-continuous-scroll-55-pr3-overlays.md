# Continuous scroll (#55), PR 3: Overlays and the menu item — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** In the continuous strip, draw search hits on every visible page, let the user select text on the page under the cursor, keep all visible pages in a background strip tab's L1 (#159), and add the `&Continuous Scroll` menu item — so that the mode the menu offers has search highlighting, selection and copy (spec U2).

**Architecture:** The canvas's paged overlay and selection code gain strip branches that take their geometry from `StripViewport::page_rect` (page sizes × zoom, never the bitmap), sharing the paged drawing and gesture code through small extracted helpers. New pure helpers (hit-testing a viewport point to a page, the overlay hit union, the menu state and toggle rule, the background-retain set) are unit-tested headless. #159 gains set-taking overloads beside the existing ones. The menu item writes `ViewPrefs` and re-applies the active tab's layout through `apply_effective_layout`, PR 2's only layout owner.

**Tech Stack:** C++17 (MSVC), Win32, Direct2D, MuPDF 1.27.2 (vendored), Catch2, CMake/ctest from VS 2022 BuildTools, PowerShell 5.1 (`ux-probe.ps1`, `smoke-test.ps1`).

**Spec:** `docs/superpowers/specs/2026-10-22-continuous-scroll-design.md`. Read §3.3, §3.4, §6.1, §7 and §9 before any task; § numbers below refer to it. This plan covers **PR 3 only** (§8 item 3). PR 2's plan, `docs/superpowers/plans/2026-10-23-continuous-scroll-55-pr2-strip.md`, defines the strip interfaces this plan consumes; `src/ui/detail/StripViewport.hpp`, `src/ui/PdfCanvasImpl.hpp` and `src/ui/PdfCanvasStrip.cpp` are the source of truth.

## Global Constraints

- Baseline `main` @ `222a82a` (PR 2 merged). Line numbers below are for `222a82a`; find code by name if a line moved.
- **Build and test:** as in PR 2's plan — Release only, full BuildTools paths for `cmake`/`ctest`, MuPDF junction in the worktree, `-DFETCHCONTENT_FULLY_DISCONNECTED=ON`. Every "run" step builds first (`& $cmake --build build --config Release`). Full suite: `& $ctest --test-dir build -C Release` **without `-j`**; read the count with `-N`, never hard-code it.
- `TEST_CASE` names: pure ASCII, prefixed by subsystem (`StripViewport: `, `StripOverlay: `, `EffectiveLayout: `, `PageCache: `, `DocumentView: `).
- **Existing tests are not edited** (§9.2's list). New cases go into new test files, registered in `tests/CMakeLists.txt`. New APIs are overloads or new functions; no existing signature changes.
- **Paged behaviour does not change** (U6). Every strip branch is guarded by `layout() == LayoutKind::Strip`; a paged code path may be extracted into a helper only if the paged caller's behaviour is byte-for-byte the same.
- Pure headers: `src/ui/detail/`, namespace `litepdf::ui`, standard library plus other `ui/detail`/`core` headers only.
- **PowerShell 5.1 floor** for `*.ps1`; probes print exactly one line of JSON and exit 0 when the window is missing. Cross-process: only system messages carry data (`WM_GETTEXT`, `WM_SETTEXT`); `WM_INITMENUPOPUP` carries an `HMENU`, not a pointer, and `GetMenuState` works on another process's menu (the existing `file-menu` probe does both).
- **Local GUI runs go through the wrapper** `~/.claude/compact-dumps/issue-165-36f011/run-gui.ps1` (`& <path> -Body { ... }`): it refuses when litepdf runs and restores `session.json`, the MRU key, `ContinuousScroll` and `running.lock`. Never run `smoke-test.ps1` bare. The menu item **writes** `ContinuousScroll`, so the wrapper's restore matters for every row of Task 5. Rows that use the clipboard save its text before (`Get-Clipboard -Raw`) and put it back in `finally`.
- **Reading state after a posted command** (#149): a positive oracle polls with `Wait-UxProbeState` (or an equivalent ≤ 5 s poll), never a fixed sleep. A negative oracle ("X did not change") first posts a **FIFO barrier** after the command under test — `send-cmd 40064` (Status Bar toggle), then poll until `canvas-scroll`'s `client_h` changes — reads its oracle, then posts 40064 again to restore the bar. Posted `WM_COMMAND`s are handled in order, so the command under test has run once the barrier's effect shows. `menu-state` takes several ids, so smoke rows call `& $uxProbe menu-state <ids>` and parse its JSON directly (`Read-UxProbeJson` passes one argument only).
- New test files cannot reach helpers in other test files' anonymous namespaces: copy the helper they need (for example `one_worker_view`, `render_and_wait`, layouts A and B) into the new file; never move or edit the original.
- Spec values: `IDM_VIEW_CONTINUOUS = 40066` (the next free id, `MainMenu.rc.h:75`); hit-test edges are left/top inclusive, right/bottom exclusive (the paged `update_cursor` rule, `PdfCanvas.cpp:866`); L1 capacity read from the view (`l1_capacity()`), never hard-coded.
- Commits: conventional, English, ending with `Co-Authored-By: <the model that wrote the commit> <noreply@anthropic.com>`. `git add` files by name (the MuPDF junction). No `VERSION` change; no CHANGELOG entry (§8: PR 4 writes the phase's entries).
- **PR 2 obligations closed here:** `switch_layout` calls `cancel_gesture()` (Task 4); `painted_hits` becomes strip-aware instead of cleared (Task 3); PR 2's deviation 5 (no selection in the strip) is reverted (Task 4).
- **Spec deviations and decisions, made here** (the owning task patches the spec in its commit):
  1. **The #135 overlay diff in the strip compares the union of V's hits and invalidates the whole canvas.** §7.1 says the diff "works per page rect". An `ID2D1HwndRenderTarget` paint redraws the whole target whatever rect was invalidated, so a per-rect invalidation saves nothing; what matters is that a search update for a page outside V costs no repaint, and the union diff gives that. Owner: Task 3 (edit §7.1).
  2. **Strip overlays and the selection press do not wait for a bitmap.** Paged mode draws overlays and accepts a press only over its own bitmap, because its geometry comes from the bitmap. The strip's geometry comes from `page_sizes()` × zoom, which a bitmap (Ready, stale-stretched or not yet rendered) is drawn into, so a hit or a selection is placed correctly over any of them. Owner: Tasks 3 and 4 (a sentence in §7.1).
  3. **The menu item's kick is `KickReason::Spread`.** That reason already means "a within-tab layout switch that `apply_effective_layout` has applied": refit, readout, relayout, no tab-position restore (which would overwrite the carry). A new reason would dispatch identically. Owner: Task 5 (add the menu item to §3.4's `Spread` row).
  4. **One enable predicate for the menu item.** The handler refuses exactly when the item is greyed (the #48 rule), so a posted `WM_COMMAND` cannot toggle the preference with no document or an ungated format. Owner: Task 5.
  5. **A tab that enters the strip with no saved strip position starts at its current page.** The menu item makes a new case reachable: a background tab left in paged mode becomes a strip tab at its next `on_tab_switch`, and it has no `strip_state`, so PR 2's `TabSwitch` kick would build the layout at scroll 0 and the derived page would overwrite the tab's page with 0. Rule: when `strip_kick` builds a fresh layout and has no `restore`, it navigates to `view->current_page()` (scroll = `anchor(page)`, pinned, §6.3), so the tab keeps its page. The reverse case (a tab left in the strip comes back paged) snapshots `pan_x = pan_y = 0` when the canvas showed the strip, since the canvas's paged pan is stale there; the tab lands at its current page's top. The in-page offset is not carried across a background tab's layout change (§10 known limit). Owner: Task 5 (edit §7.4, add the limit to §10).
  6. **A failed preference write keeps the in-memory value for this session.** §7.2 says only that the value is written; `ViewPrefs::save()` returns false on failure, and the menu item still applies the new layout, so the user's choice holds until exit and the next start reads whatever the registry holds. Owner: Task 5 (a sentence in §7.2).

## Review Focus

1. **Wheel-scrolling or Ctrl+wheel zooming in the middle of a selection drag.** The start page moves under a still pointer; the selection must stay on the start page, keep its page-point anchor, extend from the next pointer move in the page's new placement, and never jump to another page. Pinned by Task 1's clamp test and Task 4's manual check, steps 4–5.
2. **Turning Continuous Scroll on from a two-page tab at mid-document.** The tab leaves the spread, enters the strip at the same page, and the preference stays on. Pinned by Task 5's smoke row 2.
3. **A background paged tab entering the strip after the preference is turned on.** It must open at its own page, not page 1 (deviation 5). Pinned by Task 5's smoke row 4.
4. **Leaving a strip tab with more visible pages than the L1 cap (a tall window at a low zoom).** The background trim keeps the cap's worth of the most visible pages, the current page first, and switching back is still an L1 hit for those. Pinned by Task 1's `retained_for_background` test and Task 2's `DocumentView` test; the `on_tab_switch` glue that connects them has no automated check (L1 contents are not observable from a probe) and is checked by Task 2's reviewer against the rule in Step 3.
5. **A press in the gap between pages after a committed selection.** In Chars mode the selection is cleared and no gesture starts; a double-click in the gap does nothing. Pinned by Task 1's `page_at` gap test and Task 4's manual check, step 3.

---

## Task 1: Pure helpers — hit-testing, overlay union, retain set, menu rules

**Files:**
- Create: `src/ui/detail/StripOverlay.hpp`
- Modify: `src/ui/detail/StripViewport.hpp`, `src/ui/detail/EffectiveLayout.hpp` (additions only)
- Test: `tests/unit/test_strip_overlay.cpp` (register it)

**Interfaces:**
- Consumes: `StripViewport` (`page_rect`, `visible`, `visible_by_area`), `StripPageRect`, `StripRange`; `Placement` (`ViewportMath.hpp`); `canvas_dip_to_page_point` (`SelectionDrag.hpp`); `core::SearchSession::Hit`.
- Produces:
  - In `StripViewport.hpp`, members of `StripViewport`:
    - `std::optional<int> page_at(double x, double y) const;` — viewport-relative DIPs → the page of V whose `page_rect` contains the point (left/top inclusive, right/bottom exclusive); `nullopt` in a gap, beside a narrow page, or with no layout.
    - `std::vector<int> retained_for_background(std::size_t cap) const;` — the first `cap` entries of `visible_by_area()` (§7.4: V capped at the L1 count cap, the current page first).
  - In `StripOverlay.hpp`:
    - `inline Placement strip_page_placement(const StripPageRect& r) noexcept;` — the rect narrowed to `float`.
    - `using HitsForPage = std::function<std::vector<core::SearchSession::Hit>(std::size_t)>;`
    - `std::vector<core::SearchSession::Hit> strip_overlay_hits(StripRange visible, const HitsForPage& hits_for_page);` — the concatenation of `hits_for_page(p)` for each `p` of `visible` in page order; empty for an empty range or a null function.
  - In `EffectiveLayout.hpp`:
    - `struct ContinuousMenuState { bool enabled = false; bool checked = false; };` and `ContinuousMenuState continuous_menu_state(bool has_view, bool format_ok, bool pref_on) noexcept;` — `enabled = has_view && format_ok`, `checked = pref_on` (§7.3).
    - `struct ContinuousToggle { bool pref_on = false; bool spread_on = false; };` and `ContinuousToggle continuous_toggle(bool pref_on, bool spread_on) noexcept;` — in a spread tab `{true, false}` (§7.3: choosing it there turns the preference on and the spread off); otherwise `{!pref_on, false}`.

- [ ] **Step 1: Write the failing tests** in `tests/unit/test_strip_overlay.cpp`. Layout A and B are PR 2's (`test_strip_viewport.cpp`): A = `{100×200, 50×100, 100×300}` pt at zoom 1 (tops 8, 216, 324); B = 10 pages of 100×100 pt (tops `8 + 108k`).
  1. `StripViewport: page_at finds the page under a point` — A, viewport 300×150, `scroll_to(108)`: page 0's rect is `{100, -100, 100, 200}` and page 1's `{125, 108, 50, 100}`: `(150, 50)` → 0; `(100, 0)` → 0 (left edge inclusive); `(200, 50)` → nullopt (right edge exclusive); `(150, 100)` → nullopt (page 0's bottom edge is exclusive; the gap is y 100–108); `(150, 108)` → 1 (top edge inclusive); `(110, 120)` → nullopt (left of the narrow page 1, x 125–175); after `reset()`, `(150, 50)` → nullopt.
  2. `StripViewport: retained_for_background caps the visible pages by area` — B, viewport 300×300, `scroll_to(788)`, `recompute_current()`: cap 5 → `{9, 8, 7}`; cap 2 → `{9, 8}`; cap 0 → `{}`.
  3. `StripOverlay: overlay hits are the visible pages' hits in page order` — a `HitsForPage` returning one hit per page for pages 1–5 (hit `page` = the argument, a distinct quad each): range `{2, 4}` → three hits, pages 2, 3, 4 in that order; an empty range → empty; a null function → empty; and `same_overlay(strip_overlay_hits({2,4}, f), strip_overlay_hits({2,4}, f))` while `!same_overlay` against range `{2, 5}`.
  4. `StripOverlay: a drag past the start page clamps to that page` — A, viewport 300×150 at scroll 108, start page 1 (`page_rect` `{125, 108, 50, 100}`): `canvas_dip_to_page_point(150, 300, strip_page_placement(page_rect(1)), 1.0f)` → `{25, 100}` (bottom of page 1, though y 300 is over page 2); `(100, 50)` → `{0, 0}`. Then `relayout(2.0)` and `r = page_rect(1)`: `canvas_dip_to_page_point(r.x + 50, r.y + 100, strip_page_placement(r), 2.0f)` → `{25, 50}` — a page-point anchor maps to the same glyph after a zoom.
  5. `EffectiveLayout: continuous menu state` — `(false, true, true)` → `{false, true}`; `(true, false, false)` → `{false, false}`; `(true, true, true)` → `{true, true}`; `(true, true, false)` → `{true, false}`.
  6. `EffectiveLayout: continuous toggle` — `(false, false)` → `{true, false}`; `(true, false)` → `{false, false}`; `(true, true)` → `{true, false}`; `(false, true)` → `{true, false}`.
- [ ] **Step 2: Run and see them fail** — `build\tests\Release\litepdf_unit_tests.exe "StripOverlay: *"` (and the other prefixes) → compile error.
- [ ] **Step 3: Implement** the additions.
- [ ] **Step 4: Run and see them pass**; the full suite passes.
- [ ] **Step 5: Commit** — `feat(ui): strip hit-testing, overlay union and menu rules (#55)`.

## Task 2: #159 keeps a background strip tab's visible pages

**Files:**
- Modify: `src/core/PageCache.hpp` (`:111`), `src/core/PageCache.cpp` (`Pin` `:52-62`, `retain_l1_pages` `:195`), `src/core/DocumentView.hpp` (`:229`), `src/core/DocumentView.cpp` (`:541`), `src/ui/PdfCanvas.hpp`, `src/ui/PdfCanvasStrip.cpp`, `src/ui/MainWindow.cpp` (`on_tab_switch` trim, `:808-815`)
- Test: `tests/unit/test_l1_retain_set.cpp` (register it)

**Interfaces:**
- Consumes: Task 1's `retained_for_background`.
- Produces:
  - `void PageCache::retain_l1_pages(float scale, bool invert, const std::vector<int>& pages);` — the set version; `Pin` holds `std::vector<int> pages` and `admits` checks membership. The existing `(scale, invert, page, other_page = -1)` overload forwards `std::vector<int>{page}` or `std::vector<int>{page, other_page}` (no `-1` entry), so its behaviour and its tests are unchanged. **Overload trap:** a braced list of zero or one `int` (`{}`, `{page}`) converts to `int` by identity and binds to the int overload — forwarding `{page}` recurses forever, and `retain_l1_pages(s, i, {})` means page 0. Always construct `std::vector<int>` explicitly, in the forwarders, the tests and every caller; say so in the header comment.
  - `void DocumentView::trim_l1_for_background(const std::vector<int>& pages);` — same forwarding pattern over the existing `(page, other_page = -1)` overload.
  - `std::vector<int> PdfCanvas::strip_retained_pages(std::size_t cap) const;` — `retained_for_background(cap)` while `strip_active()`, else empty.

- [ ] **Step 1: Write the failing tests** in `tests/unit/test_l1_retain_set.cpp`, in the shape of the existing #159 cases (`test_page_cache_l1.cpp:295-365`, `test_document_view.cpp:475-505`):
  1. `PageCache: retain_l1_pages keeps a set of pages and pins L1 to it` — put pages 0–4 at scale 1, retain `{1, 3, 4}` → `l1_size() == 3`, pages 1, 3, 4 hit, 0 and 2 miss; a `put_pixmap` of page 2 is dropped (pinned), of page 3 at scale 1.5 is dropped (other scale); after `release_l1_pin()`, page 2 is admitted.
  2. `PageCache: an empty retain set empties L1` — retain `std::vector<int>{}` → `l1_size() == 0`, and nothing is admitted until `release_l1_pin()`.
  3. `DocumentView: trim_l1_for_background keeps every page of the set` — render pages 0–4 of `large.pdf` into L1 (capacity 5), `trim_l1_for_background(std::vector<int>{0, 2, 3})` → `l1_contains` true for 0, 2, 3 and false for 1, 4; `request_page_render` of page 4 then releases the pin and page 4 is admitted.
- [ ] **Step 2: Run and see them fail** → compile error.
- [ ] **Step 3: Implement** the overloads and `strip_retained_pages`. In `on_tab_switch`'s trim (`:810-815`), branch first: when `canvas_ && canvas_->layout() == LayoutKind::Strip` and `strip_retained_pages` is non-empty, `v.trim_l1_for_background(canvas_->strip_retained_pages(v.l1_capacity()))`; otherwise today's `shown_pages` call, unchanged. The canvas still shows the outgoing view at that point (its `set_view` is at `:822`); say so in the comment.
- [ ] **Step 4: Run and see them pass**; the full suite passes, the §9.2 tests untouched.
- [ ] **Step 5: Commit** — `perf(core): keep every visible page of a background strip tab in L1 (#159, #55)`.

## Task 3: Search hits on every visible page

**Files:**
- Modify: `src/ui/PdfCanvas.cpp` (`overlay_hits` `:536`, the on_paint overlay block `:1986-2042`), `src/ui/PdfCanvas.hpp`, `src/ui/PdfCanvasStrip.cpp` (`strip_paint` `:785`), spec §7.1 (deviations 1 and 2)

**Interfaces:**
- Consumes: Task 1's `strip_overlay_hits`.
- Produces (private on `PdfCanvas`): `void paint_hit(const litepdf::core::SearchSession::Hit& hit, float ox, float oy, float pct);` — the body of today's per-hit loop (current-hit fill and stroke, else the other fill), extracted from `on_paint`; the paged loop calls it with today's arguments.

Rules (§7.1, deviations 1 and 2):
- `overlay_hits()`: a strip branch first — when `layout() == Strip`, return `strip_overlay_hits(strip_view.visible(), hits_fn)` if `strip_active()`, `hits_fn` is set and both hit brushes exist, else `{}`. The paged branch is unchanged. `on_search_hits_changed` then compares the visible pages' hits against the last strip paint with the existing `same_overlay` rule and needs no change.
- `strip_paint`: replace PR 2's `painted_hits.clear()` (and its comment) with `painted_hits = overlay_hits()`, recorded before `BeginDraw` as the paged paint does (`:1862`). After the page loop, for each hit of `painted_hits`, `paint_hit(hit, r.x, r.y, strip_view.layout().zoom_pct())` with `r = page_rect(hit.page)` — the layout's zoom, the one `page_rect` uses; one point is `zoom_pct` DIPs, as in paged mode (the comment at `:1998-2002`). Hits are drawn over every visible page whatever its bitmap state (deviation 2).

- [ ] **Step 1: Implement** the extraction (paged paint unchanged: the same calls in the same order) and the two strip branches. In the spec's §7.1, after "#135's overlay-diff repaint works per page rect, with the same diff rule.", add: "In practice the diff compares the union of V's hits and repaints the whole canvas, since a Direct2D window target redraws all of it; a search update for a page outside V costs no repaint." After "A selection is painted whenever its page is visible.", add: "Hits and the selection are placed from the page's rect (page sizes × zoom), so they do not wait for the page's bitmap."
- [ ] **Step 2: Verify** — the full suite passes. Manual, wrapped, `ContinuousScroll=1`, scripted with real `SendInput` and a DPI-aware capture as in PR 2's manual checks open `tests\fixtures\strip-1000.pdf`, then post `IDM_ZOOM_OUT` (40011) until `canvas-scroll` shows at least three page pitches in the viewport (`client_h × 96 / dpi >= 3 × (max + 1) / 1000`; at FitWidth a Letter page is taller than the canvas, so a tall window alone cannot do it); Ctrl+F, type `Strip page`, Enter → the capture shows a highlight on the heading of **every** visible page, and the current-hit style on one; F3 twice → the current-hit style moves to the next page's heading and the others keep the other-hit style; scroll by the wheel → the newly visible pages' headings are highlighted. Negative control: the same steps with `ContinuousScroll` absent highlight only the one shown page. Record the captures and readings for the PR body.
- [ ] **Step 3: Commit** — `feat(ui): draw search hits on every visible strip page (#55)`; include the spec edits.

## Task 4: Selection on the page under the cursor

**Files:**
- Modify: `src/ui/PdfCanvas.cpp` (`can_select_all` `:556`, `on_left_button_down` `:666`, `on_mouse_move` `:736`, `update_cursor` `:855`, the on_paint selection block `:2032-2041`), `src/ui/PdfCanvas.hpp`, `src/ui/PdfCanvasStrip.cpp` (`switch_layout` `:174`, `strip_paint`)
- Modify: `scripts/ux-probe.ps1` (one new action), `scripts/smoke-test.ps1` (strip section)

**Interfaces:**
- Consumes: Task 1 (`page_at`, `strip_page_placement`), Task 3's paint position.
- Produces (private on `PdfCanvas`):
  - `void begin_selection(int page, const Placement& pl, litepdf::core::SelectMode mode, int x_px, int y_px, litepdf::core::Document::TextPage text);` — today's press body from `page_point_at` through the final `InvalidateRect` (`:709-733`), extracted; the paged press calls it with today's values.
  - `void paint_selection_quads(const litepdf::core::TextSelection& sel, float ox, float oy, float pct);` — today's selection fill loop, extracted.
- Produces in `ux-probe.ps1`: `menu-state <id> [id...]` → `{"present":bool,"items":{"<id>":{"found":bool,"grayed":bool,"checked":bool}}}` — for each id, find the top-level submenu that owns it (`GetMenuState(sub, id, MF_BYCOMMAND) -ne [uint32]::MaxValue` — in PS 5.1 the literal `0xFFFFFFFF` is the `Int32` −1, so comparing against it reads "not found" as found), send that submenu `WM_INITMENUPOPUP` first (as `File-Menu` does), then read `MF_GRAYED` (0x1) and `MF_CHECKED` (0x8). An id no submenu owns reports `found:false, grayed:false, checked:false`. Add the action to the usage header (`:3-21`).

Rules (§7.1, §3.4):
- **Press** — in `on_left_button_down`, a strip branch right after the Space-pan branch and before the paged refusals: return unless `strip_active()`; `page = strip_view.page_at(dip(x), dip(y))`; no page → in Chars mode `view->clear_selection()` and invalidate, then return (no gesture, no capture); else `text = view->text_for(page)` (the #98 slot for the current page, else a synchronous build — already `text_for`'s contract), return if `!text.valid()`, and `begin_selection(page, strip_page_placement(strip_view.page_rect(page)), mode, x, y, std::move(text))`. Remove PR 2's `layout() == Strip` refusal and its comment.
- **Drag** — in `on_mouse_move`'s selecting branch, a strip branch before the paged `own_bitmap()` check: the placement is `strip_page_placement(strip_view.page_rect(live_selection->page))`, recomputed on every move, so a scroll or zoom mid-drag maps into the start page's current placement and `canvas_dip_to_page_point` clamps to it.
- **Paint** — in `strip_paint`, after the hits: `painted_selection()`, drawn with `paint_selection_quads` at `page_rect(sel->page)` with the layout's zoom when that page is in `visible()`.
- **Select All** — `can_select_all()`'s strip branch: `strip_active()` and today's gesture rule (replacing PR 2's `return false`); `select_all()` is unchanged (it selects the view's current page, which in the strip is the derived or pinned page).
- **Cursor** — `update_cursor()`'s strip branch: `over_page = strip_active() && page_at(dip(cursor)).has_value()`; the paged branch is unchanged.
- **Cancellation** — `switch_layout` calls `cancel_gesture()` first, before the carry capture (PR 2 obligation: `set_dual_page` cancels only when the dual flag changes, which a single ↔ strip switch does not). Nothing else new cancels a strip selection: a derived page change does not (§3.4, `set_strip_current_page`), and `set_view` already does.

- [ ] **Step 1: Add the `menu-state` probe** and check it against today's build (wrapped, `ContinuousScroll` absent, `simple.pdf`): `menu-state 40071 40072 40062` → Copy (40071) grayed, Select All (40072) not grayed, Two-Page Spread (40062) not checked; `send-cmd 40072`, then poll `menu-state 40071` until not grayed (≤ 5 s). Record the readings.
- [ ] **Step 2: Write the failing smoke rows** (strip section, `ContinuousScroll=1`, clipboard saved and restored):
  1. **Select All and Copy in the strip** — launch `strip-1000.pdf`; assert `vscroll` (the precondition: the row is in the strip); `Set-Clipboard` a marker string; `page-box-go 30` and poll the page box `"30"`; `send-cmd 40072`, `send-cmd 40071`; poll (≤ 5 s) until `Get-Clipboard -Raw` contains `Strip page 30 of 1000.`, and assert it does not contain `Strip page 31`.
  2. **Select All on a page with no text** — launch `strip-images.cbz`; assert `vscroll`; `Set-Clipboard` a marker string; `send-cmd 40072`, `send-cmd 40071`, then the FIFO barrier; `menu-state 40071` grayed and the clipboard still holds the marker. This row is a guard, not a fail-first row: it passes today (Select All is off in the strip) and must keep passing once Select All is on, so a strip Select All that builds an empty or garbage selection on a textless page fails it. `select_all()` keeps today's behaviour when the page has no text — it returns and leaves any existing selection, as in paged mode — so no row claims more than that.
  Run wrapped → row 1 fails (Select All is off in the strip); row 2 passes.
- [ ] **Step 3: Implement** the branches, the two extractions and the `switch_layout` cancel.
- [ ] **Step 4: Run wrapped** → both rows pass; every PR 2 strip row still passes; the full suite passes. Manual, wrapped, scripted with real `SendInput` (as in PR 2's manual checks), on `strip-1000.pdf` zoomed out as in Task 3's manual check (page N and N+1 both on screen), record each reading:
  1. Drag from just left of page N's heading to its end, Ctrl+C → the clipboard is `Strip page N of 1000.`.
  2. Drag from page N's heading down into page N+1 → the copy holds page N's text only, and the highlight stops at page N's end.
  3. With that selection committed, click in the gap between two pages → Copy greyed (`menu-state 40071`); double-click in the gap → still greyed and no highlight (Review Focus 5).
  4. Press on page N's heading, hold, wheel down three notches with the button held, move the pointer → the highlight stays on page N, extended to the pointer's clamped point on page N (Review Focus 1).
  5. Same, with Ctrl+wheel instead of the wheel → the anchor stays on the same glyph after the zoom.
  6. Cursor: `GetCursorInfo` shows the I-beam over a page and the arrow over a gap; with Space held, the move shape (§6.1).
  7. Paged negative control (`ContinuousScroll` absent): step 1 still selects as before.
- [ ] **Step 5: Commit** — `feat(ui): select text on the strip page under the cursor (#55)`.

## Task 5: The Continuous Scroll menu item

**Files:**
- Modify: `resources/MainMenu.rc.h` (`:75`), `resources/litepdf.rc.in` (`:74`), `src/ui/MainWindow.hpp`, `src/ui/MainWindow.cpp` (View `WM_INITMENUPOPUP` block `:1650-1680`, `WM_COMMAND` beside `IDM_VIEW_DUAL_PAGE` `:1804`, `on_tab_switch` snapshot `:782-788`), `src/ui/PdfCanvasStrip.cpp` (`strip_kick` `:385-404`), spec §3.4 `Spread` row (deviation 3), §7.3 menu bullet (deviation 4), §7.4 and §10 (deviation 5), §7.2 (deviation 6)
- Modify: `scripts/smoke-test.ps1` (strip section)

**Interfaces:**
- Consumes: Task 1 (`continuous_menu_state`, `continuous_toggle`), Task 4's `menu-state` probe; PR 2's `apply_effective_layout`, `strip_format_ok`, `ViewPrefs::{set_continuous_scroll, save}`.
- Produces:
  - `#define IDM_VIEW_CONTINUOUS 40066   // View > Continuous Scroll (no accelerator)`; the "Next free ID" comment becomes 40067, reserving 40067–40070.
  - `MENUITEM "&Continuous Scroll", IDM_VIEW_CONTINUOUS` right after `&Two-Page Spread`.
  - `litepdf::ui::ContinuousMenuState MainWindow::continuous_item_state();` (non-const: `active_view()` is non-const, `MainWindow.hpp:136`) — `litepdf::ui::continuous_menu_state(v != nullptr, v && strip_format_ok(v->source_path(), v->page_count()), view_prefs_.continuous_scroll())` for `v = active_view()`, qualified (a member of the same name would hide the free function); the one predicate both the menu and the handler read.

Rules (§7.2, §7.3):
- `WM_INITMENUPOPUP`, inside the View block: `EnableMenuItem(IDM_VIEW_CONTINUOUS, enabled ? MF_ENABLED : MF_GRAYED)` and `CheckMenuItem(.., checked ? MF_CHECKED : MF_UNCHECKED)` from `continuous_item_state()`.
- `IDM_VIEW_CONTINUOUS`: return unless `continuous_item_state().enabled`; `t = continuous_toggle(pref, v->dual_page())`; `view_prefs_.set_continuous_scroll(t.pref_on)` and `view_prefs_.save()` (a failed write keeps the in-memory value for this session, deviation 6); `if (v->dual_page() != t.spread_on) v->set_dual_page(t.spread_on);` then `apply_effective_layout(active tab, true)` and `kick_render(v->current_page(), KickReason::Spread)` (deviation 3). The order matches `IDM_VIEW_DUAL_PAGE`'s: the view's flag changes first, so the canvas still reports the old layout when the carry captures. Only the active tab is re-applied; a background tab picks the preference up at its next `on_tab_switch` (§7.3).
- `strip_kick` (deviation 5): track whether this call built the layout (`!sv.has_layout()` on entry); when it did and `restore` is null, `sv.navigate(view->current_page())` before `strip_after_change()`. A first open (page 0) is unaffected: `anchor(0)` is scroll 0.
- `on_tab_switch`'s outgoing snapshot (`:782-788`): when `canvas_->layout() == LayoutKind::Strip`, store `pan_x = pan_y = 0` instead of `canvas_->pan()` (deviation 5).

- [ ] **Step 1: Write the failing smoke rows** (strip section; each reads `HKCU\SOFTWARE\LitePDF\Settings\ContinuousScroll` with `Get-ItemProperty` as its pixel-free oracle for the write):
  1. **the transition row through the menu item** (§9.3) — `ContinuousScroll=1`, launch `large.pdf`; `IDM_ZOOM_IN` twice; `page-box-go 30`; `canvas-wheel -120 3`; record `pos` and page box `"30"`; `send-cmd 40066` → poll `vscroll` false, registry value 0, `menu-state 40066` not checked; `send-cmd 40066` → poll `vscroll` true, registry value 1, page box `"30"`, `|pos − recorded| <= 1`.
  2. **from a two-page tab** (Review Focus 2) — from row 1's end: `send-cmd 40062` → `vscroll` false; `send-cmd 40066` → poll `vscroll` true; registry value still 1; `menu-state 40062 40066` → 40062 not checked, 40066 checked; page box `"30"`.
  3. **no document greys the item and the handler refuses** — `send-cmd 40030` (close the tab), poll `page-box` empty; `menu-state 40066` → grayed; `send-cmd 40066`, then the FIFO barrier; registry value still 1. The format half of the predicate has no fixture (no `.xps`/`.fb2` in `tests/fixtures`) and is covered by Task 1's `continuous_menu_state(true, false, …)` case and PR 2's `strip_format_ok` test.
  4. **a background paged tab keeps its page when the preference turns on** (Review Focus 3, deviation 5) — a fresh launch with `ContinuousScroll=0`: `large.pdf`; `page-box-go 30`, poll `"30"`; forward `simple.pdf` as a second tab (single-instance forwarding, as PR 2's tab rows do) and poll `tab-enum` count 2; `send-cmd 40066` → poll `vscroll` true (tab 2 entered the strip); `send-cmd 40033` (tab 1) → poll `vscroll` true and page box `"30"`, and `|pos − round(anchor(29))| <= 1` at FitWidth (`anchor(i) = i × (792 × z + 8)`, `z = (client_w × 96 / dpi) / 612`). Then `send-cmd 40066` (preference off; tab 1 leaves the strip) and `send-cmd 40034` (tab 2, last shown in the strip) → poll `vscroll` false and page box `"1"`; process alive.
  Run wrapped → rows 1–3 fail (no item yet); row 4 fails at its `40066` step for the same reason, and after Step 2 it would fail on page box `"1"` without the `strip_kick` rule.
- [ ] **Step 2: Implement** the id, the menu item, `continuous_menu_state()`, the `WM_INITMENUPOPUP` arm and the handler. In the spec's §3.4 kick table, `Spread` row, after "two-page toggle (1745)" add "and the Continuous Scroll menu item: a within-tab layout switch `apply_effective_layout` has applied". In §7.3's menu bullet, add: "The handler refuses exactly when the item is greyed, so a posted command cannot toggle it either."
- [ ] **Step 3: Run wrapped** → the four rows pass; every earlier strip row and PR 2's paged readings still pass; full suite passes. Manually open the View menu once with the mouse and check the item's position (below Two-Page Spread), its checkmark, and that it is greyed on the start page with no document.
- [ ] **Step 4: Commit** — `feat(ui): add the Continuous Scroll menu item (#55)`; include the spec edits.

## Task 6: Regression and the PR

**Files:**
- Modify: none, unless a check fails

- [ ] **Step 1: The §9.2 tests are untouched** — `git fetch origin`, then `git diff --stat origin/main -- tests/unit/test_scroll_math.cpp tests/unit/test_completion_math.cpp tests/unit/test_page_anchor.cpp tests/unit/test_viewport_math.cpp tests/unit/test_dual_page_layout.cpp tests/unit/test_prefetch_plan.cpp tests/unit/test_document_view.cpp tests/unit/test_page_cache_l1.cpp tests/unit/test_text_prefetch_view.cpp tests/unit/test_selection_drag.cpp tests/unit/test_session_state.cpp tests/unit/test_render_engine_cancel.cpp` → empty.
- [ ] **Step 2: Full suite** — `& $ctest --test-dir build -C Release -N` (record the count), then the run → all pass.
- [ ] **Step 3: Full smoke, wrapped** — `scripts\smoke-test.ps1 -ExpectDev` → passes, including the 12 MB size gate and the cold-start line; afterwards `ContinuousScroll`, `session.json` and the clipboard are back to their prior state and no crash dump is new.
- [ ] **Step 4: Paged spot check, wrapped, preference off** — on `selection.pdf` and `search.pdf`: a drag selects and copies, Find highlights the shown page's hits, Ctrl+Shift+D shows the spread with no overlay; the View menu shows Continuous Scroll unchecked and enabled.
- [ ] **Step 5: Open the PR** — title `feat: continuous scroll overlays and menu item (#55, PR 3/4)`; the body lists the six deviations and decisions, closes PR 2's two obligations by name, includes the manual checks of Tasks 3 and 4 and the `ctest -N` count, and states that the default stays off until PR 4.
