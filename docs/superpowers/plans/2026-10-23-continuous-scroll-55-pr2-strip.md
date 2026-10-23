# Continuous scroll (#55), PR 2: The strip renders and scrolls — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire PR 1's pure units into the canvas and `MainWindow` so that, with `HKCU\SOFTWARE\LitePDF\Settings\ContinuousScroll = 1`, a document opens as a continuous vertical strip that renders, scrolls, navigates, switches layouts and keeps per-tab position — with paged single and paged spread unchanged.

**Architecture:** A new pure model, `StripViewport`, owns everything about the strip that is not Win32 (layout, scroll, navigation pin, keys and wheel, hit scrolling, position carry, tab state, scrollbar units) and is unit-tested headless. The canvas keeps its D2D side in a new translation unit, `PdfCanvasStrip.cpp`, sharing `PdfCanvas::Impl` through a new private header; the strip's bitmaps and `StripRenderState` live in a canvas-internal `PageBitmapSet`. `MainWindow` threads a `KickReason` through all 21 `kick_render` call sites and gains `apply_effective_layout`, the only owner of layout switches. GUI behaviour is verified by new `ux-probe.ps1` probes and `smoke-test.ps1` rows.

**Tech Stack:** C++17 (MSVC), Win32, Direct2D, MuPDF 1.27.2 (vendored), Catch2, CMake/ctest from VS 2022 BuildTools, PowerShell 5.1 (`ux-probe.ps1`, `smoke-test.ps1`).

**Spec:** `docs/superpowers/specs/2026-10-22-continuous-scroll-design.md` (on `main` since #170). Read §3–§7 and §9.3 before any task; § numbers below refer to it. This plan covers **PR 2 only** (§8 item 2). PR 1's plan, `docs/superpowers/plans/2026-10-22-continuous-scroll-55-pr1-foundations.md`, defines every PR 1 interface this plan consumes; the headers under `src/ui/detail/Strip*.hpp` and `EffectiveLayout.hpp` are the source of truth.

## Global Constraints

- Branch `claude/issue-55-pr2`, baseline `main` @ `55c9bc0`. Line numbers below are for `55c9bc0` (`MainWindow.cpp` is unchanged since `5a40d5e`, so the spec's §3.4 line numbers still hold); find code by name if a line moved.
- **Worktree build setup:** as in PR 1's plan (MuPDF junction, Catch2 source copy, `-DFETCHCONTENT_FULLY_DISCONNECTED=ON`). Release only. Full BuildTools paths for `cmake`/`ctest`; never bare. Every "run" step means build first: `& $cmake --build build --config Release` (the GUI tasks need `litepdf.exe`, not only the test target).
- Full suite from PowerShell at the repo root: `& $ctest --test-dir build -C Release` — **without `-j`** (the `ViewPrefs` and `MruList` tests share a registry parent key). Read the count with `-N`; never hard-code it.
- `TEST_CASE` names: pure ASCII, prefixed by subsystem (`StripViewport: `, `StripScrollbar: `, `StripCarry: `, `EffectiveLayout: `, `StripSchedule: `, `DocumentView: `).
- **Existing tests are not edited** (§9.2's list). New cases go into new test files, registered in `tests/CMakeLists.txt`. New functions are added beside PR 1's in its headers; no existing signature changes.
- Pure headers: `src/ui/detail/`, namespace `litepdf::ui`, standard library plus other `ui/detail`/`core/detail` headers only. New UI `.cpp` files go into `add_executable(litepdf WIN32 ...)` in `CMakeLists.txt` (beside `src/ui/PdfCanvas.cpp`, `:129`).
- **Paged behaviour does not change** (U6). Every strip branch is guarded by the canvas's layout being `LayoutKind::Strip`; every paged branch keeps today's code path, including today's ordering quirks.
- **PowerShell 5.1 floor** for `*.ps1`: no `?.`, `??`, ternary, or `-AsHashtable`. Probes print exactly one line of JSON and exit 0 when the window is missing (the `find-bar-state` contract).
- **Cross-process messages:** only system messages are marshalled (`WM_GETTEXT`, `WM_SETTEXT`). Never send a pointer-carrying control message with a local pointer (#155). `GetScrollInfo`, `GetWindowLongPtrW` and `GetDpiForWindow` are direct calls and work across processes (checked at the plan gate with a throwaway process pair). Windows clamps a stored `nPage` to `nMax + 1`, so the probe reads that clamped value for a strip that fits.
- **`ux-probe.ps1` must be per-monitor DPI aware.** `powershell.exe` is DPI-unaware and litepdf is PerMonitorV2, so an unaware probe reads `GetClientRect` virtualised (halved at 200 %) while `GetDpiForWindow` still returns the real DPI; every zoom the smoke derives would be wrong on a scaled display. Task 3 calls `SetProcessDpiAwarenessContext(-4)` (PER_MONITOR_AWARE_V2) at the top of the script, before any action reads a size (the `win32-dpi-measurement` skill).
- **`FindWindowEx` name argument:** pass `[NullString]::Value`, never `[String]::Empty`, when the target window may have text: PS 5.1 marshals `""` as a title filter that matches only empty-titled windows (checked at the plan gate), so the page box would never be found once it shows a number.
- **Running the GUI locally is destructive unless wrapped.** Every launch rewrites `%LOCALAPPDATA%\LitePDF\session.json` and the MRU key `HKCU\Software\LitePDF\MRU`, a force-kill leaves `running.lock`, the strip rows write `HKCU\SOFTWARE\LitePDF\Settings\ContinuousScroll` — the user's real setting — and a launch while the user's own litepdf runs is forwarded into it by the single-instance mutex (`Local\LitePDF_SingleInstance_v1`), after which ux-probe drives the user's window. For every local run of `smoke-test.ps1`, a probe session or a manual check in this plan: **refuse to start if `Get-Process litepdf` returns anything**; move `session.json` aside (rename, so the run starts with none) and put it back in `finally`; export the MRU key (`reg export`) and re-import it in `finally` (deleting the key first if it did not exist); read the prior `ContinuousScroll` value (or its absence) and restore it in `finally`; delete `running.lock` afterwards; set `$env:LITEPDF_NO_RESTORE = "1"` (except for Task 7's restore check, which needs restore); and list `%LOCALAPPDATA%\LitePDF\crashes` before and after (a new dump fails the run). Never run `smoke-test.ps1` bare.
- Spec values used across tasks: `G = 8` DIP; arrow step 100 DIP; page step 0.9 × viewport; wheel 16 DIP per line (48 per notch at the default 3 lines); hit margin 24 DIP (the paged `scroll_into_view` margin); L1 capacity and budget read from the view (Task 5), never hard-coded; `WM_USER_STRIP_DONE = WM_USER + 6`.
- Commits: conventional, English, ending with `Co-Authored-By: <the model that wrote the commit> <noreply@anthropic.com>`. `git add` files by name (the MuPDF junction). No `VERSION` change, no CHANGELOG entry (the feature is registry-only until PR 3).
- **Spec deviations, decided here** (the owning task patches the spec in its commit):
  1. **Carry is applied after the target layout's fit, scaled by the zoom ratio** (§7.3). The spec applies the carried offset before the kick refits, so a fit change (the strip's FitWidth vs a spread's half-width slot) shifts the reader by the zoom ratio, and the paged completion only re-clamps. `apply_effective_layout` therefore sets the current page to the carried page, refits for the new layout (`apply_viewport`), and applies `offset × zoom_after / zoom_before`. The `Spread` kick's refit is then idempotent. Owner: Task 8.
  2. **The §3.4 reset and §7.3 carry run only for a switch into or out of the strip.** A paged single ↔ paged spread switch keeps today's `set_dual_page` path (which zeroes the pan), so paged behaviour is unchanged (U6). Owner: Task 5.
  3. **`on_tab_switch` calls `apply_effective_layout` where it calls `set_dual_page` today** (after `set_pan` and `set_invert_chrome`), not literally right after `set_view`: those two calls are independent of the layout, and moving `set_pan` after the switch would change paged pan restore across a spread/single tab pair. Owner: Task 5.
  4. **"Fully visible" for a search hit uses the paged 24-DIP margin** (§6.3), so a hit flush against an edge is centred exactly as in paged mode. Owner: Task 2.
  5. **Select All and selection stay off in the strip until PR 3** (§7.1 is PR 3): `can_select_all()` returns false in the strip, and a press there starts no selection gesture (it already cannot: `own_bitmap()` is false). Owner: Task 6.
  6. **§9.3's two-page row runs at a Custom zoom on a left page.** At FitWidth a spread's half-width page fits the viewport, so the carried offset clamps to 0 by design, and a carry into a spread is keyed on the spread's left page. The row zooms in twice first and uses page 30 (index 29, a left page). Owner: Task 8.

## Review Focus

1. **Toggling two-page spread rapidly in a strip tab.** Late completions of the outgoing layout must be dropped (the switch bumps the epoch) and the final state must match the final toggle: `WS_VSCROLL` present iff the strip is shown, no crash dump. Pinned by Task 8's ten-toggle smoke row.
2. **Flinging through the 1000-page document.** Live work stays bounded and the last page renders: after forty back-to-back maximum wheel deltas, the page box shows 1000 and the scroll position is the largest one. Pinned by Task 6's fling row.
3. **Resizing the window while mid-document at FitWidth.** The viewport-top point is kept (§4.5): the page box stays put and the scroll position equals the new layout's anchor of that page. Pinned by Task 7's resize row.
4. **Closing the last tab while the strip is shown.** The scrollbar goes away and the canvas is empty, with no crash. Pinned by Task 5's close row.
5. **A one-page document in the strip.** A strip shorter than the viewport keeps a disabled scrollbar (`SIF_DISABLENOSCROLL`), the page box reads 1, and wheel and keys are harmless. Pinned by Task 6's one-page row.

---

## Task 1: Pure helpers — scrollbar units, spread geometry, format gate, bitmap predicate

**Files:**
- Create: `src/ui/detail/StripScrollbar.hpp`
- Modify: `src/ui/detail/StripCarry.hpp`, `src/ui/detail/EffectiveLayout.hpp`, `src/ui/detail/StripSchedule.hpp` (additions only)
- Test: `tests/unit/test_strip_helpers.cpp`

**Interfaces:**
- Produces:
  - `struct StripScrollInfo { int max = 0; int page = 0; int pos = 0; };` — `nMin` is always 0.
  - `StripScrollInfo strip_scroll_info(double total, double vp_h, double scroll_y) noexcept;` — §6.2: `max = max(0, lround(total) − 1)`, `page = max(0, lround(vp_h))`, `largest = max(0, max − page + 1)`; `pos = largest` when `total − vp_h >= 1` and `scroll_y >= (total − vp_h) − 0.5`, else `clamp(lround(scroll_y), 0, largest)`.
  - `double strip_scroll_from_pos(int pos, double total, double vp_h) noexcept;` — `pos >= largest` → `max(0, total − vp_h)` exactly; `pos <= 0` → 0; else `min(pos, max(0, total − vp_h))`.
  - In `StripCarry.hpp`: `struct SpreadVertical { double left_top_in_union = 0; double union_h = 0; };` and `SpreadVertical spread_vertical(double left_h, double right_h, double slot_h) noexcept;` — `right_h < 0` means no right page; each page's y is `(slot_h − h) / 2` when `h <= slot_h`, else 0 (`place_bitmap` at pan 0); the union spans both; `base_y = union_h > slot_h ? −union_top : 0`; `left_top_in_union = left_y + base_y`. This is exactly `PdfCanvas::content_extent` + `on_paint`'s dual branch, vertically.
  - In `EffectiveLayout.hpp`: `bool strip_format_ok(const std::filesystem::path& source, int page_count);` — lower-cased extension in `{.pdf, .epub, .cbz, .svg}` and `page_count >= 1` (§4.4, §7.3).
  - In `StripSchedule.hpp`: `inline bool slot_holds_bitmap(const PageSlot& s) noexcept;` — Ready, or Pending with `has_stale_bitmap`. The canvas keeps a page's bitmap exactly while this holds.

- [ ] **Step 1: Write the failing tests** in `tests/unit/test_strip_helpers.cpp` (register it):
  1. `StripScrollbar: units are rounded DIPs` — `(632, 300, 0)` → `{631, 300, 0}`; `(632, 300, 100.4)` → pos 100.
  2. `StripScrollbar: the largest position maps to the exact maximum` — total 632.6, vp 300.2 (max scroll 332.4; `max` 632, `page` 300, largest 333): scroll `632.6 - 300.2` → pos 333; `from_pos(333) == 632.6 - 300.2` (compare against the expression: in `double` it is 332.40000000000003, not the literal 332.4); `from_pos(332) == 332`; `from_pos(0) == 0`; `from_pos(500) == 632.6 - 300.2`.
  3. `StripScrollbar: a strip that fits gives a disabled range` — `(500, 700, 0)` → `page > max`, pos 0; `from_pos(5, 500, 700) == 0`.
  4. `StripScrollbar: 67 million DIPs fit in int` — total 67473268, vp 600 → `max == 67473267`, and `from_pos(largest)` is the exact maximum.
  5. `StripCarry: spread_vertical matches the spread paint geometry` — slot 600: `(400, 500)` → `{100, 500}`; `(1000, 800)` → `{0, 1000}`; `(500, 900)` → `{50, 900}`; `(400, -1)` → `{100, 400}`.
  6. `EffectiveLayout: strip_format_ok accepts the measured formats` — `a.PDF`, `b.epub`, `c.cbz`, `d.svg` with 1 page → true; `e.xps`, `f.fb2` → false; `a.pdf` with 0 pages → false.
  7. `StripSchedule: slot_holds_bitmap` — Ready → true; Pending with stale → true; Pending without, None, Failed → false.
- [ ] **Step 2: Run and see them fail** — `build\tests\Release\litepdf_unit_tests.exe "StripScrollbar: *"` (and the other three names) → compile error.
- [ ] **Step 3: Implement** the four additions.
- [ ] **Step 4: Run and see them pass.**
- [ ] **Step 5: Commit** — `feat(ui): strip scrollbar units, spread geometry and format gate (#55)`.

## Task 2: StripViewport — the strip's viewport model

**Files:**
- Create: `src/ui/detail/StripViewport.hpp`
- Modify: `src/ui/detail/StripLayout.hpp` (add `StripTabState`), spec §6.3 (deviation 4)
- Test: `tests/unit/test_strip_viewport.cpp`

**Interfaces:**
- Consumes: `StripLayout`, `StripPos`, `StripRange` (`StripLayout.hpp`); `clamp_strip_scroll`, `relayout_scroll`, `derive_current_page`, `CurrentPageTracker`, `effective_pan_x`, `apply_pan_x_delta`, `page_x`, `kStripArrowStepDip`, `kStripPageStepFraction` (`StripScroll.hpp`); `CarriedPosition`, `carry_from_strip`, `strip_scroll_for` (`StripCarry.hpp`); `StripTravel`, `prefetch_range`, `ScheduleInput` (`StripSchedule.hpp`); Task 1's scrollbar functions.
- Produces:
  - In `StripLayout.hpp`: `struct StripTabState { StripPos top; double desired_pan_x = 0; double zoom_pct = 1; };` (stored in `core::Tab`, Task 8; `zoom_pct` is the zoom the pan was saved at, so a restore after a zoom or DPI change can scale it, §4.5).
  - In `StripViewport.hpp`: `inline constexpr double kStripHitMarginDip = 24.0;`, `struct StripPageRect { double x = 0, y = 0, w = 0, h = 0; };` (viewport-relative DIPs), `enum class StripKey { LineUp, LineDown, PageUp, PageDown, Home, End, Left, Right };`
  - `class StripViewport` (all DIPs, `double`):
    - `void reset() noexcept;` — no pages, scroll 0, desired pan 0, tracker reset, travel Down, viewport kept
    - `void build(std::vector<StripPageSize> sizes, double zoom_pct);` — fresh layout, scroll 0, unpinned
    - `bool has_layout() const noexcept;` (`page_count() > 0`), `const StripLayout& layout() const noexcept;`
    - `bool relayout(double zoom_pct);` — §4.5: capture `position_at(scroll)`, rebuild at the new zoom, `scroll = relayout_scroll(...)`, desired pan × new/old zoom; false (no-op) when the zoom is unchanged
    - `void set_viewport(double w, double h);` — keeps the top point (re-clamps)
    - `bool scroll_by(double dy);` and `bool scroll_to(double y);` — scroll input: ends the pin, travel = Down when the target is below the current scroll, Up when above (unchanged when equal); return whether `scroll_y` moved
    - `bool pan_x_by(double dx);` — `desired = apply_pan_x_delta(desired, dx, layout.widest(visible()), vp_w)`
    - `bool key(StripKey k);` — LineUp/LineDown: `scroll_by(∓100)`; PageUp/PageDown: `scroll_by(∓0.9 × vp_h)`; Home: scroll 0, `navigate`-style pin on page 0; End: scroll to max, pin the last page; Left/Right: `pan_x_by(±100)`; returns whether anything moved or a pin was set
    - `void navigate(int page);` — `scroll = clamp(anchor(page))`, pin `page` (§6.3)
    - `bool scroll_hit_into_view(int page, double q_top_pt, double q_bottom_pt);` — §6.3 with deviation 4: the quad in document y is `top(page) + q × zoom`; if `q_top − scroll >= 24` and `q_bottom − scroll <= vp_h − 24`, no scroll; else centre the quad (`scroll = clamp(centre − vp_h / 2)`); pins `page` either way; returns whether it scrolled
    - `int recompute_current();` — `tracker.recompute(layout, scroll, vp_h)`
    - `int current_page() const noexcept; bool pinned() const noexcept; double scroll_y() const noexcept; double viewport_w() const noexcept; double viewport_h() const noexcept;`
    - `StripRange visible() const;` — `layout.visible(scroll, scroll + vp_h)`
    - `int top_visible_page() const;` — `visible().first`, else `current_page()` (§6.3 search start page)
    - `StripRange prefetch() const;` — `prefetch_range(layout, scroll, vp_h, travel)`
    - `StripRange retain() const;` — `layout.visible(scroll − vp_h, scroll + 2 × vp_h)` (so P ⊆ retain)
    - `std::vector<int> visible_by_area() const;` — `current_page()` (the tracker's value: the pinned page, or the page the last `recompute_current()` derived — never a fresh derivation here) first when it is in V (PR 1's caller contract), then the rest of V by visible height descending, ties to the lower index
    - `double pan_x_eff() const;` — `effective_pan_x(desired, widest(visible()), vp_w)`
    - `StripPageRect page_rect(int page) const;` — `x = page_x(width, widest(visible()), pan_x_eff(), vp_w)`, `y = top(page) − scroll`
    - `bool can_pan() const;` — `total > vp_h` or `widest(visible()) > vp_w` (§6.1)
    - `CarriedPosition carry() const;` — `carry_from_strip(layout, current_page(), scroll)`
    - `void apply_carry(const CarriedPosition& c);` — `scroll = strip_scroll_for(layout, c, vp_h)`, pin `c.page`
    - `StripTabState save() const;` and `void restore(const StripTabState& s);` — save records the layout's zoom; restore sets `scroll = relayout_scroll(s.top, layout, vp_h)` and desired pan = `s.desired_pan_x × layout.zoom_pct() / s.zoom_pct`, unpinned
    - `StripScrollInfo scroll_info() const;` and `bool scroll_to_pos(int pos);` (`scroll_to(strip_scroll_from_pos(...))`)

- [ ] **Step 1: Write the failing tests** in `tests/unit/test_strip_viewport.cpp` (register it). Layout A = sizes `{100×200, 50×100, 100×300}` pt at zoom 1 (tops 8, 216, 324; total 632). Layout B = 10 pages of 100×100 pt at zoom 1 (tops `8 + 108k`; total 1088).
  1. `StripViewport: navigation puts the page at its anchor and pins it` — A, viewport 300×150: `navigate(1)` → `scroll_y() == 208`, `current_page() == 1`, `pinned()`; `recompute_current() == 1` after `set_viewport(300, 140)`.
  2. `StripViewport: scroll input ends the pin and sets the travel` — after `navigate(1)`, `scroll_by(-50)` → `!pinned()`, `scroll_y() == 158`, and `prefetch()` equals `prefetch_range(layout, 158, 150, StripTravel::Up)`.
  3. `StripViewport: relayout keeps the viewport-top point and scales the pan` — A, viewport 80×150, `scroll_to(108)`, `pan_x_by(-10)` (desired −10): `relayout(2.0)` → `scroll_y() == 208`, `pan_x_eff() == -20` (the desired pan scaled by 2); `relayout(2.0)` again → false.
  4. `StripViewport: keys follow the spec table` — A, viewport 300×150 from scroll 0: LineDown → 100; PageDown → 235; End → 482, `current_page() == 2`, pinned; Home → 0, `current_page() == 0`, pinned; LineDown → unpinned.
  5. `StripViewport: a visible hit does not scroll and an off-screen one is centred` — B, viewport 300×300 at 0: hit on page 1 at pt 10–20 (doc 126–136) → returns false, scroll 0, `current_page() == 1`, pinned; hit on page 5 at pt 40–60 (doc 588–608) → returns true, scroll 448, `current_page() == 5`; then a hit on page 4 at pt 10–20 (doc 450–460, 2 DIP below the viewport top: inside the 24-DIP margin) → returns true, scroll 305.
  6. `StripViewport: visible_by_area starts with the current page` — B, viewport 300×300. `scroll_to(788)` alone leaves the tracker on page 0 (scroll input only ends the pin), so `visible_by_area() == {8, 9, 7}` (by area, ties to the lower index); after `recompute_current()` (rule 1 → page 9) → `{9, 8, 7}`. `navigate(9)` from scroll 0 (anchor 972 clamps to 788, page 9 pinned, no recompute) → `{9, 8, 7}`: the pin, not the area, puts 9 first.
  7. `StripViewport: retain covers the prefetch range` — B, viewport 300×300, for each scroll in `{0, 50, 400, 788}` and both travels (via `scroll_by` direction): every page of `prefetch()` is inside `retain()`.
  8. `StripViewport: a gap position survives a zoom change through save and restore` — A, viewport 300×150, `scroll_to(212)` (gap after page 0, fraction 0.5): `save()`; `build(A sizes, 2.0)`; `restore(saved)` → `scroll_y() == 412`, unpinned. Pan: at viewport 80×150, `pan_x_by(-10)` then `save()` (zoom 1); `build(A sizes, 2.0)`; `restore` → `pan_x_eff() == -20`.
  9. `StripViewport: carry round trip` — A, viewport 300×150: `navigate(1)`, `scroll_by(30)` (scroll 238, derived current page 1): `carry() == {1, 22}`; a second viewport with the same layout `apply_carry` → `scroll_y() == 238`, `current_page() == 1`, pinned.
  10. `StripViewport: page_rect is viewport-relative and centres narrow pages` — A, viewport 300×150 at 108: page 0 `{100, -100, 100, 200}`, page 1 `x == 125`; viewport 80×150 with desired pan −10: page 0 `x == -10`, page 1 `x == 15`.
  11. `StripViewport: can_pan` — A at viewport 300×700 → false; 300×150 → true; 80×700 → true.
  12. `StripViewport: the scrollbar's largest position reaches the exact bottom` — B, viewport 300×300.4: `scroll_to_pos(scroll_info().max - scroll_info().page + 1)` → `scroll_y() == 1088 - 300.4`, and `recompute_current() == 9`.
  13. `StripViewport: no pages` — after `reset()`: `!has_layout()`, `visible().empty()`, `scroll_by(10)` is false, `key(StripKey::End)` is false.
- [ ] **Step 2: Run and see them fail** — `"StripViewport: *"` → compile error.
- [ ] **Step 3: Implement `StripViewport.hpp`.** In the spec's §6.3 Find row, after "No scroll if the hit quad is fully visible", add "(with the 24-DIP margin paged mode uses)".
- [ ] **Step 4: Run and see them pass.**
- [ ] **Step 5: Commit** — `feat(ui): strip viewport model (#55)`; include the spec edit.

## Task 3: ux-probe probes for the strip

**Files:**
- Modify: `scripts/ux-probe.ps1` (usage header, P/Invoke block, five new actions, `send-cmd`'s count)

**Interfaces:**
- Produces (each prints one line of JSON, or performs one posted action):
  - `canvas-scroll` → `{"present":bool,"vscroll":bool,"min":int,"max":int,"page":int,"pos":int,"client_w":int,"client_h":int,"dpi":int}` — the `LitePDFPdfCanvas` child of the main window; `vscroll` is `WS_VSCROLL` (0x00200000) in `GetWindowLongPtrW(GWL_STYLE)`; the range from `GetScrollInfo(SB_VERT, SIF_ALL)`; `client_*` in pixels from `GetClientRect`; `dpi` from `GetDpiForWindow`. Missing window → `present:false`, zeros.
  - `page-box` → `{"present":bool,"text":string}` — the `Edit` child of the `msctls_statusbar32` child; text by `SendMessage(WM_GETTEXT)` into a `StringBuilder` (marshalled).
  - `page-box-go <n>` — `SendMessage(WM_SETTEXT)` with `"<n>"` to that Edit, then `PostMessage` `WM_KEYDOWN`/`WM_KEYUP` `VK_RETURN` to it (the box's subclass commits on `VK_RETURN`, `StatusBar.cpp:518`).
  - `canvas-wheel <delta> [count]` — posts `count` (default 1) `WM_MOUSEWHEEL` messages to the canvas, `wParam = (delta << 16)` as a 32-bit value (keystate 0), `lParam` = the canvas centre in screen coordinates.
  - `canvas-key <vk_hex>` — posts `WM_KEYDOWN` then `WM_KEYUP` for `vk` to the canvas (not the main window: Home/End/PgDn are canvas keys).
  - `send-cmd <id> [count]` — the existing action gains an optional `count` (default 1): it posts the `WM_COMMAND` `count` times from one process with no waits, so a race row (Task 8) really is back to back. One-argument calls behave as today.

- [ ] **Step 1: Add the P/Invoke members** (`SetProcessDpiAwarenessContext`, `GetScrollInfo` with a `SCROLLINFO` struct, `GetWindowLongPtrW`, `GetDpiForWindow`, a `SendMessage` overload taking `System.Text.StringBuilder` and one taking `string`, `ClientToScreen`), the DPI-awareness call at the top of the script (Global Constraints), the five new actions (`FindWindowEx` with a `[NullString]::Value` name) and `send-cmd`'s `count`.
- [ ] **Step 2: Verify against today's paged build** (wrapped as in Global Constraints; `ContinuousScroll` absent): launch `tests\fixtures\large.pdf`; `canvas-scroll` → `present:true`, `vscroll:false` (the negative control: paged mode has no scrollbar), and `client_w × 96 / dpi` equals the canvas width in DIPs (on this 200 % machine `dpi` is 192 and `client_w` is the real pixel width, not half of it); `page-box` → `text:"1"`; `page-box-go 3` then poll `page-box` until `"3"` (≤ 5 s); `canvas-key 0x23` (End) → page box `"500"`; `canvas-key 0x21` (PgUp, which `navigate_to_page` serves without waiting for a bitmap) → `"499"`; wait 2 s for page 499's render to land (the paged wheel drops notches while `bitmap_is_stale()`), then `canvas-wheel 120` → `"498"` (the paged wheel flips back at the top of the page — a positive control that the wheel driver reaches the canvas). Record each reading.
- [ ] **Step 3: Commit** — `test(smoke): ux-probe probes for the canvas scrollbar, page box and canvas input (#55)`.

## Task 4: KickReason through every kick_render call site

**Files:**
- Modify: `src/ui/MainWindow.hpp` (`:70`), `src/ui/MainWindow.cpp` (`kick_render` `:331` and its 21 call sites)

**Interfaces:**
- Consumes: `KickReason`, `tab_switch_reason` (`EffectiveLayout.hpp`).
- Produces: `void kick_render(int page, litepdf::ui::KickReason reason);` — no default argument (§3.4: a new call site must choose). The paged branches ignore `reason`; this task adds no strip branch.

- [ ] **Step 1: Change the signature and classify every call site** exactly per the spec's §3.4 table: `Navigate` — `navigate_click` (424), Find Next/Prev (2257, 2268), cross-tab result (2478); `Layout` — outline toggle (644), status bar toggle (661), thumbnails toggle (730), results splitter (1254), left splitter (1318), WM_SIZE (1410), DPI (1490), results panel show/toggle/close (2362, 2387, 2491); `TabSwitch`/`Navigate` via `tab_switch_reason(new_index, old_index)` — `on_tab_switch` (863); `Restore` — session restore (1118); `Restyle` — invert (1727); `Spread` — two-page toggle (1745); `Zoom` — Zoom In (1831), Zoom Out (1838), Reset Zoom (1854).
- [ ] **Step 2: Verify** — the build succeeds (a missed site is a compile error); `grep -n "kick_render(" src/ui/MainWindow.cpp` lists 22 lines — the definition at `:331` and 21 call sites, each naming a reason; the full suite passes; the Task 3 paged readings repeat unchanged.
- [ ] **Step 3: Commit** — `refactor(ui): give every kick_render call a KickReason (#55)`.

## Task 5: The strip renders — canvas pipeline and the effective layout

**Files:**
- Create: `src/ui/PdfCanvasImpl.hpp` (private), `src/ui/PdfCanvasStrip.cpp` (add to the exe target)
- Modify: `src/ui/PdfCanvas.hpp`, `src/ui/PdfCanvas.cpp`, `src/ui/MainWindow.hpp`, `src/ui/MainWindow.cpp`, `src/core/DocumentView.hpp`, `src/core/DocumentView.cpp`, `CMakeLists.txt`, `tests/CMakeLists.txt`, spec §3.4 (`apply_effective_layout` row, deviation 2) and §3.4 `on_tab_switch` row (deviation 3)
- Modify: `scripts/smoke-test.ps1` (a new strip section after the thumb-pane block)
- Test: `tests/unit/test_document_view_l1_limits.cpp`

**Interfaces:**
- Consumes: Task 1 (`slot_holds_bitmap`, `strip_format_ok`), Task 2 (`StripViewport`), Task 3 probes, Task 4 (`kick_render(page, reason)`); PR 1's `StripRenderState`, `classify_strip_completion`, `after_bitmap`, `after_null`, `StripCompletionMeta`, `effective_layout`, `strip_kick_actions`, `DocumentView::{set_continuous, page_sizes, request_page_render, l1_contains, cancel_thumbnail_renders}`, `ViewPrefs`.
- Produces on `DocumentView`: `[[nodiscard]] std::size_t l1_capacity() const noexcept;` and `[[nodiscard]] std::size_t l1_budget_bytes() const noexcept;` — the values the constructor was given.
- Produces on `PdfCanvas` (public):
  - `inline constexpr UINT WM_USER_STRIP_DONE = WM_USER + 6;` — WPARAM = `fz_pixmap*` or null; LPARAM = a heap `StripMeta { core::EscrowContext escrow; StripCompletionMeta id; }`, allocated for null results too (§5.3). The two reservation comments (`PdfCanvas.hpp:47`, `MainWindow.cpp:58`) become "WM_USER + 3 to + 6".
  - `LayoutKind layout() const noexcept;` — derived, never stored separately from the dual flag: `Impl` gains `bool strip = false`, and `layout()` returns `Strip` when it is set, else `dual_page ? PagedSpread : PagedSingle`. So a paged single ↔ spread toggle through `set_dual_page` (deviation 2) keeps `layout()` right without touching the strip, and Task 8's capture from a spread sees `PagedSpread`.
  - `void switch_layout(LayoutKind next, bool carry_position);` — the §3.4 layout-switch reset, for a switch into or out of the strip (deviation 2). In this task `carry_position` is accepted and ignored (Task 8 implements it). Order: (capture — Task 8); **assign `Impl::strip = (next == Strip)` immediately, before any step below** — the `SetWindowPos` further down sends `WM_SIZE` synchronously, and `SetScrollInfo` can set `WS_VSCROLL` on a window that lacks it (slot 1 reproduced this on a window that never had the style; slot 2 could not after clearing it — the guard makes both cases harmless), so no strip code may still believe it is in charge; leaving the strip, also `StripViewport::reset()`; if `thumb_drag_live`, set `thumb_drag_dead` (Task 6), so queued track messages are ignored until `SB_ENDSCROLL`; cancel the old layout's work (strip: `PageBitmapSet::clear`; paged: `view->cancel_stale_renders(0)`); `anchor.clear()`; `wheel_flip_seq = 0`; reset both slot bitmaps and the strip bitmaps; `++view_epoch`; set the dual flag (`next == PagedSpread`, through `set_dual_page`'s body); add or remove `WS_VSCROLL` (`SetWindowLongPtrW` + `SetWindowPos(SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)`), before anything reads `GetClientRect`; entering the strip, `StripViewport::reset()` (the layout is built by the next kick).
  - `void strip_kick(const StripKickActions& a, const StripTabState* restore);` — the strip branch after `MainWindow` has run `refit` and `refresh_readout`: build the layout from `view->page_sizes()` at `view->zoom_pct()` if there is none, else `relayout(view->zoom_pct())` when `a.relayout`; apply `restore` when non-null; `restyle` → `change_render_params()`; then the common tail `strip_after_change()` (below).
- Produces (private, `PdfCanvasStrip.cpp`): `strip_paint()`, `strip_on_done(WPARAM, LPARAM)`, `strip_reschedule()`, `strip_update_scrollbar()`, `strip_after_change()` (recompute current page → `set_strip_current_page`, scrollbar, reschedule, invalidate), `set_strip_current_page(int)` (`view->set_current_page`; **only when it returns true**, the page-change observer — §4.6 "fires only when the value changes" — **without** `cancel_gesture()`, §3.4); `strip_after_change`, `strip_update_scrollbar` and `strip_reschedule` return at once unless `layout() == Strip` and `StripViewport::has_layout()`; `static bool post_strip_done(HWND, fz_pixmap*, fz_context*, StripCompletionMeta)`, `strip_viewport_dip()` (client rect ÷ dpi/96, so it works before a render target exists).
- `PageBitmapSet` (in `PdfCanvasImpl.hpp`): `std::map<int, ComPtr<ID2D1Bitmap>> bitmaps; StripRenderState state; std::uint64_t next_seq = 0; StripRange previous_visible; float last_scale = 0; bool last_invert = false;` plus a `Submitter` implementation: `submit` takes its seq from `++next_seq` (never the paged `next_seq`, which binds paged anchors) and calls `view->request_page_render(page, priority, cb, flag, with_text ? text_cb : {})`, where `cb` posts `post_strip_done` with `{view_epoch at submit, page, seq, flag->load()}` and `text_cb` posts the existing `post_text_ready`; `cancel_thumbnails` → `view->cancel_thumbnail_renders()`; `released(page)` → erase the bitmap.
- Produces on `MainWindow`: member `litepdf::core::ViewPrefs view_prefs_;` loaded beside `mru_.load()` (`:177`); `void apply_effective_layout(litepdf::core::Tab* tab, bool carry_position);` — `auto* v = tab ? tab->view.get() : nullptr;` target = `v ? effective_layout(true, v->dual_page(), view_prefs_.continuous_scroll(), strip_format_ok(v->source_path(), v->page_count())) : LayoutKind::PagedSingle` (the null path is the last-tab close: `close_tab` fires `on_switch(-1, …)`); first, for a non-null view, `view->set_continuous(target == Strip)` on every call (a strip-to-strip tab switch changes no canvas layout but the incoming view still needs the flag, and Task 8's carry refits after it); then, when the target or the canvas's layout is `Strip` and they differ → `canvas_->switch_layout(target, carry_position)`; otherwise `canvas_->set_dual_page(target == PagedSpread)` (today's path).

Rules the implementation must follow (each is spec-pinned; cite the section in a comment):
- `strip_reschedule` (§5.2): if `view->render_scale()` or `view->invert_colors()` differs from `last_scale`/`last_invert`, `state.change_render_params()` first and record the new values; build `ScheduleInput` from `StripViewport` (`visible`, `visible_by_area`, `prefetch`, `retain`, `previous_visible`), `scale = view->render_scale()`, `invert = view->invert_colors()`, `l1_capacity()`/`l1_budget_bytes()`, `bytes_of(p) = core::estimated_pixmap_bytes(w_pt, h_pt, scale)` from `page_sizes()`; `run_pass`; store `previous_visible`; erase every bitmap whose slot fails `slot_holds_bitmap`. The current page is set before the pass, in the same message (PR 1's caller contract).
- `strip_on_done` (§5.3): adopt the meta and escrow first; call `on_completion_arrived` **before any early return**; **every return that leaves a non-null pixmap undropped first calls `fz_drop_pixmap(escrow.get(), pix)`** — `Drop` always carries one (every §5.2 text-only P1 lands in rule 2, and every completion after a layout or tab switch fails rule 1), and the escrow must outlive that drop exactly as in the paged handler (`PdfCanvas.cpp:1271`); a completion while `layout() != Strip` is a `Drop` by its epoch; `classify_strip_completion(state.slot(page), id, view_epoch, pix != nullptr)`; `CreateBitmap` → `create_render_target()`, `CreateBitmap` exactly as the paged handler does (`PdfCanvas.cpp:1286-1293`), drop the pixmap through the escrow, map the HRESULT to `BitmapResult` (`!rt` or `D2DERR_RECREATE_TARGET` → `RecreateOrNoTarget`; other failure → `OtherFailure`), **store `after_bitmap` into the slot first**, and only then, for `RecreateOrNoTarget`, call `discard_render_target()` and reschedule (rescheduling while the matched slot is still Pending at P0 would submit nothing for it — `plan_strip_pass` resubmits a Pending page only at P1 — and leave it blank); Ok → keep the bitmap, `ColdStartTimer::mark(3)` on the first strip bitmap, and if the page is the current page, run §5.2's text request (`state.request_text(page, view->has_text_for(page), view->pending_text_page() == page, view->l1_contains(page), submitter)`); `CancelAndReschedule` → `after_null(.., true)` then `strip_reschedule()`; `MarkFailed` → `after_null(.., false)`. After **every** slot change in this handler (not only via `strip_reschedule`), erase the page's bitmap if its slot no longer passes `slot_holds_bitmap` — `OtherFailure` and `MarkFailed` turn a Pending page holding a stale bitmap into Failed, and the map must not keep it. Invalidate.
- `strip_paint` (§4.2, §5.1): clear with the chrome colour; for each page of `visible()`, the rect from `page_rect` narrowed to `float`; a Ready, non-stale bitmap is drawn at its natural DIP size (`bitmap_px_to_dip` of its pixel size) at the rect's top-left, so MuPDF's 1-px rounding is not resampled (§4.2 "Pixel rounding"); a stale bitmap (Ready-stale, or Pending with `has_stale_bitmap`) is drawn stretched to the rect (§5.1); otherwise fill the rect with the page colour (white; `0x000000` when `invert_chrome`), and for a Failed page add a 1-DIP border in the placeholder colour (`0xC0C0C0` / `0x404040`, as the spread placeholder). `ColdStartTimer::mark(4)` + `emit_if_complete(log_timings_)` on the first successful `EndDraw` that drew a bitmap; `D2DERR_RECREATE_TARGET` → `discard_render_target()` + `resubmit_current_page()`.
- `strip_update_scrollbar` (§6.2): `SetScrollInfo(SB_VERT, SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL, redraw)` from `StripViewport::scroll_info()`.
- Existing entry points gain a strip branch, first in the function: `set_view` (after its `cancel_gesture()`, §3.4 row: same view in the strip → return with no epoch bump and no reset; a different view → `PageBitmapSet::clear`, `StripViewport::reset()`, then today's body), `discard_render_target` (also `state.drop_bitmaps()` and clear the map), `resubmit_current_page` (relayout to `view->zoom_pct()`, then `strip_after_change()`), `on_size` (**after** the existing `rt->Resize` block, never instead of it, and only when `layout() == Strip`: `StripViewport::set_viewport`, then `strip_after_change()`), `on_paint` (→ `strip_paint`), `handle_message` (`WM_USER_STRIP_DONE` → `strip_on_done`), `can_select_all` (false in the strip; deviation 5).
- `MainWindow::kick_render`, strip branch first: `const auto a = strip_kick_actions(reason); if (a.refit) canvas_->apply_viewport(); if (a.refresh_readout) refresh_zoom_readout(); canvas_->strip_kick(a, restore_ptr);` where `restore_ptr` is the active tab's stored state when `a.restore_tab_position` (null until Task 8).
- `on_tab_switch`: `apply_effective_layout(incoming, false)` replaces the `set_dual_page` call (`:801-803`, deviation 3). The two-page toggle (`:1730-1746`) calls `apply_effective_layout(tab, true)` instead of `canvas_->set_dual_page`, keeping its snap and kick.

- [ ] **Step 1: Write the failing unit test** `tests/unit/test_document_view_l1_limits.cpp` (registered): `DocumentView: l1 limits report the constructor values` — a view built with capacity 3 and budget 64 MiB reports `l1_capacity() == 3`, `l1_budget_bytes() == 64 << 20`; a default-built view reports 5 and `kDefaultL1BudgetBytes`. Run → compile error; implement the accessors; run → pass.
- [ ] **Step 2: Move `struct PdfCanvas::Impl` verbatim** from `PdfCanvas.cpp:230-355` into `src/ui/PdfCanvasImpl.hpp` (with the includes it needs); `PdfCanvas.cpp` includes it. Build; the full suite and Task 3's paged readings are unchanged. Commit separately — `refactor(ui): move PdfCanvas::Impl into a private header (#55)` — so the review diff of the move is pure.
- [ ] **Step 3: Write the failing smoke rows** — a strip section in `smoke-test.ps1` that saves the prior `ContinuousScroll` (value or absence) and restores it in `finally`, sets it to 1, and runs:
  1. **cold start in continuous mode** — launch `large.pdf --log-timings` with stderr redirected; poll `canvas-scroll` until `vscroll` (≤ 5 s; fail otherwise — so the run really is the strip); after stopping, the stderr holds a `LitePDF cold-start:` line.
  2. **the scrollbar shows the real range** (§9.3's probe self-check) — on `large.pdf` (500 Letter pages): `z = (client_w × 96 / dpi) / 612`; `expected = 8 + 500 × (792 × z + 8)`; assert `|max + 1 − expected| <= 2` and `page == round(client_h × 96 / dpi)` ± 1.
  3. **Zoom In re-lays out** — post `IDM_ZOOM_IN` (40010); poll until `max` changes; `z2` = the first preset of `core/detail/ZoomMath.hpp`'s `kZoomPresets` (copied into the script) above `z + 1e-4`; assert `|max + 1 − (8 + 500 × (792 × z2 + 8))| <= 2`.
  4. **invert keeps the strip** — post `IDM_VIEW_INVERT` (40061) twice; process alive, `vscroll` still true.
  5. **closing the last tab removes the scrollbar** (Review Focus 4) — first assert `present && vscroll` (the precondition: the end state below is also the paged state); post `IDM_TAB_CLOSE` (40030); poll until `vscroll` is false and `page-box` text is empty; process alive.
  Run the section wrapped (Global Constraints) → rows fail (no strip yet).
- [ ] **Step 4: Implement** the canvas pipeline, `apply_effective_layout`, the `kick_render` strip branch and `ViewPrefs` loading, following the rules above. Update `DocumentView::pending_text_page()`'s header comment: the strip now reads it, so "nothing in the product reads it" no longer holds. In the spec's §3.4, add to the `apply_effective_layout` row: "The reset and the carry run only for a switch into or out of the strip; a paged single ↔ spread switch keeps today's `set_dual_page` path (U6)." and to the `on_tab_switch` row: "`apply_effective_layout` takes the place of today's `set_dual_page` call, after `set_pan` and `set_invert_chrome`."
- [ ] **Step 5: Run the strip section wrapped** → all five rows pass. Then remove `ContinuousScroll` and repeat Task 3's paged readings → unchanged. Full suite passes.
- [ ] **Step 6: Commit** — `feat(ui): render the continuous strip behind the registry preference (#55)`; include the spec edits.

## Task 6: Input, the scrollbar and the derived current page

**Files:**
- Modify: `src/ui/PdfCanvas.cpp` (strip branches at the top of `on_wheel_scroll`, `on_hwheel_scroll`, `on_key_down`, `pan_by`, `can_pan`, `handle_message`), `src/ui/PdfCanvasStrip.cpp`, `src/ui/PdfCanvasImpl.hpp`
- Modify: `scripts/smoke-test.ps1` (strip section)

**Interfaces:**
- Consumes: Task 2 (`scroll_by`, `scroll_to_pos`, `pan_x_by`, `key`, `can_pan`), Task 5 (`strip_after_change`, `set_strip_current_page`).
- Produces (private): `LRESULT strip_on_vscroll(WPARAM w);` and `Impl` state `bool thumb_drag_live = false; bool thumb_drag_dead = false;`.

Rules:
- **Wheel** (§6.1): `consume_notches(delta, wheel_residual)`; `step = wheel_step_dip(notches, SPI_GETWHEELSCROLLLINES, vp_h)`; `scroll_by(−step)`. No identity guard and no `wheel_flip_seq` (§6.1 "the wheel's two guards"). **Horizontal wheel**: `consume_notches(rightward_delta(...), hwheel_residual)`, `pan_x_by(−step)` with the tilt/Shift settings as paged mode reads them.
- **Keys**: PgDn/PgUp/Home/End/↑/↓/←/→ → `StripKey`; Ctrl+Tab and Ctrl+W keep their existing forwarding (they are handled before `on_key_down`).
- **Drag pan**: `pan_by(dx, dy)`'s strip branch → `scroll_by(−dy)` and `pan_x_by(dx)`; `bitmap_is_stale()` is not consulted in the strip. `can_pan()`'s strip branch → `StripViewport::can_pan()`, so the existing cursor rules apply (§6.1).
- **Scrollbar** (`WM_VSCROLL`, §6.2, §4.5): `SB_LINEUP/DOWN` → LineUp/LineDown; `SB_PAGEUP/DOWN` → PageUp/PageDown; `SB_TOP/BOTTOM` → Home/End; `SB_THUMBTRACK`/`SB_THUMBPOSITION` → set `thumb_drag_live`; unless `thumb_drag_dead`, read `GetScrollInfo(SIF_TRACKPOS)` (never `HIWORD(wParam)`, 16-bit) and `scroll_to_pos`; `SB_ENDSCROLL` → clear both flags. Every re-layout (`strip_kick` with relayout, `resubmit_current_page`, `on_size`, `set_view` to another view) sets `thumb_drag_dead` while `thumb_drag_live`, after applying §4.5 and calling `SetScrollInfo`.
- **Every** input above ends in `strip_after_change()`, moved or not: a scroll input that hits the clamp still ends a navigation pin, and §4.6 recomputes the current page after every scroll input. (`strip_after_change` fires the observer only on a change, so a no-op costs one pass.) The derived page change fires the observer through `set_strip_current_page` (status bar, thumbnail highlight, session save — §4.6 table) and runs §5.2's text request for the new current page when `l1_contains` it.

- [ ] **Step 1: Write the failing smoke rows** (strip section, after Task 5's rows, on a fresh `large.pdf` launch):
  1. **wheel to the bottom** (§9.3) — `canvas-wheel -32640 200` (272 notches each: about 13,000 DIP at the default 3 lines, so 200 posts cover the strip at any window width); poll until `pos == max − page + 1` and `page-box` text `"500"`.
  2. **Home and PgDn** — first assert `present && vscroll && page > 0` (in paged mode `pos` and `page` are both 0, so the checks below would pass there); `canvas-key 0x24` → `pos == 0`, page box `"1"`; `canvas-key 0x22` → `|pos − round(0.9 × page)| <= 1`.
  3. **fling through 1000 pages** (Review Focus 2) — a second launch on `strip-1000.pdf`; one `canvas-wheel -32640 400` call (400 posts back to back from one process); poll (≤ 10 s) until page box `"1000"` and `pos == max − page + 1`; process alive. This row checks the model, not the pixels: no pixel-free signal says a page's bitmap was accepted, and a capture-based oracle in CI is flaky (the scripted-GUI-smoke memory note), so Step 3 adds a manual visual check of the final pages instead.
  4. **a one-page document** (Review Focus 5) — launch `sample.svg` (200×100 pt; at FitWidth the strip is `vp_w / 2 + 16` DIP tall, shorter than the default 1024×768 window's canvas): `vscroll` true, `page > max` (disabled range; Windows stores `page` clamped to `max + 1`), page box `"1"`; `canvas-wheel -120 3` and `canvas-key 0x23` → page box still `"1"`, `pos == 0`, process alive.
  Run wrapped → rows 1–4 fail.
- [ ] **Step 2: Implement** the branches and `strip_on_vscroll`, and make `can_select_all()` false in the strip if Task 5 did not (deviation 5; the spec's §7.1 already places selection in PR 3, so no spec edit).
- [ ] **Step 3: Run wrapped** → rows pass; Task 5's rows still pass. Manually, after row 3's fling, look at the canvas: pages 999 and 1000 show their "Strip page N of 1000." headings within 5 s (no blank page left Pending). Manually (not in smoke; a posted `WM_VSCROLL` sets no track position): drag the thumb on `strip-1000.pdf` with the mouse and press Ctrl+0 mid-drag → the view stops following the thumb until release, then the thumb matches the position. Record it in the PR body.
- [ ] **Step 4: Commit** — `feat(ui): scroll the strip by wheel, keys, drag and scrollbar (#55)`.

## Task 7: Navigation in strip coordinates

**Files:**
- Modify: `src/ui/PdfCanvas.hpp`, `src/ui/PdfCanvas.cpp` (`scroll_into_view` strip branch), `src/ui/PdfCanvasStrip.cpp`, `src/ui/MainWindow.cpp` (`navigate_click` `:404`, `restore_on_tab_ready` `:1094-1118`, `find_start_page` `:2220`, `on_find_next/prev`, `on_results_row_click` `:2433`, whose `scroll_into_view` + kick are at `:2476-2478`)
- Modify: `scripts/smoke-test.ps1` (strip section)

**Interfaces:**
- Consumes: Task 2 (`navigate`, `scroll_hit_into_view`, `top_visible_page`), Task 5 (`strip_after_change`).
- Produces on `PdfCanvas` (public): `void strip_navigate(int page);` — `StripViewport::navigate(page)` then `strip_after_change()`; `int strip_top_visible_page() const;`.
- `find_start_page` takes the canvas: `std::size_t find_start_page(const DocumentView& v, const PdfCanvas* canvas)` — `canvas && canvas->layout() == LayoutKind::Strip` → `strip_top_visible_page()` (§6.3, #140); else today's expression. All three callers pass `canvas_.get()`.

Rules (§3.4, §6.3):
- `navigate_click`: strip branch **first, before the early return** — `canvas_->strip_navigate(page); kick_render(page, KickReason::Navigate); return;`. So going to the current page mid-page scrolls back to its anchor.
- `scroll_into_view` strip branch: `scroll_hit_into_view(h.page, min y of the quad, max y of the quad)`, then `strip_after_change()`; no `PageAnchor` is installed. The callers' existing `kick_render(.., Navigate)` then only reschedules.
- Session restore in the strip, in the spec's order (§3.4 Restore row: refit, then navigate, then reschedule): replace `change_current_page(st.page)` + `set_pending_anchor(top)` with `canvas_->strip_navigate(st.page)` — **before** the zoom block, so the view's current page is the saved page for every fit (`DocumentView::set_viewport` fits the current page, and today's paged restore sets it first; a clamped scroll here is harmless) — run the zoom block unchanged, `kick_render(st.page, KickReason::Restore)` (refit, readout, relayout on the final zoom), **then** `canvas_->strip_navigate(st.page)` again for the exact anchor on the final layout; it ends in `strip_after_change()` and so reschedules. Navigating before the relayout would clamp `anchor(p)` against the pre-refit layout, which is shorter than the final one at a high Custom zoom near the end of a document.

- [ ] **Step 1: Write the failing smoke rows** (fresh `large.pdf` launch, FitWidth; `h = 792 × z`, `anchor(i) = i × (h + 8)`):
  1. **Ctrl+G 30** (§9.3) — `page-box-go 30`; poll until page box `"30"` and `|pos − round(anchor(29))| <= 1`.
  2. **Ctrl+G to the current page while mid-page** (§9.3) — `canvas-wheel -120 2` (pos grows by 96 ± 1, page box still `"30"`); `page-box-go 30` → `pos` returns to `round(anchor(29))` ± 1.
  3. **resize keeps the viewport-top point** (Review Focus 3) — from that state, `canvas-wheel -120 2` again so the viewport top is mid-page (not an anchor, which a "re-navigate to the current page" implementation would also satisfy); record `pos₀` and `z₀`, so the offset into page 29 is `d₀ = pos₀ − (anchor₀(29) + 8)`; `ux-probe resize -100 0` (narrower) and poll until `client_w` changed; page box `"30"`; with the new `z₁`, expected `pos = anchor₁(29) + 8 + d₀ × z₁ / z₀` (§4.5: the same point of page 29 stays at the top) → `|pos − round(expected)| <= 2`.
  Run wrapped → rows fail.
- [ ] **Step 2: Implement** the branches and `find_start_page`'s new parameter.
- [ ] **Step 3: Run wrapped** → rows pass; earlier rows still pass. Manually check session restore in the strip with the crash-restore recipe of the scripted-GUI-smoke memory note (write `session.json` with a tab on page 120 and `"zoom_mode":"fit_width"`, an empty `running.lock`, launch without `LITEPDF_NO_RESTORE`, post `IDYES` to the prompt): page box `"121"`, `pos == round(anchor(120))` ± 1. Repeat with a tab on `tests/fixtures/strip-1000.pdf` page index 9 (a landscape page; page 0 is portrait) at `fit_width`: the zoom readout after restore equals the readout after Ctrl+0 on that page (the fit used the saved page, not page 0). Manually check Find in the strip on `search.pdf` (F3 through hits on two pages: the page box follows; a hit already visible does not scroll). Record both in the PR body.
- [ ] **Step 4: Commit** — `feat(ui): navigate the strip by page box, thumbnails, outline, search and restore (#55)`.

## Task 8: Layout transitions and per-tab strip state

**Files:**
- Modify: `src/core/TabList.hpp` (`Tab`), `src/ui/PdfCanvas.hpp`, `src/ui/PdfCanvas.cpp`, `src/ui/PdfCanvasStrip.cpp`, `src/ui/MainWindow.cpp` (`on_tab_switch` snapshot `:750-781`, `kick_render` restore pointer), spec §7.3 (deviation 1) and §9.3 two-page row (deviation 6)
- Modify: `scripts/smoke-test.ps1` (strip section)

**Interfaces:**
- Consumes: Task 1 (`spread_vertical`), Task 2 (`carry`, `apply_carry`, `save`, `restore`), PR 1's `carry_from_paged_single`, `carry_from_spread`, `paged_pan_y_for`, `spread_pan_y_for`.
- Produces on `core::Tab`: `std::optional<litepdf::ui::StripTabState> strip_state;` (`TabList.hpp` includes `ui/detail/StripLayout.hpp`, a standard-library-only header; in memory only, never in `session.json`, §7.2).
- Produces on `PdfCanvas` (public): `std::optional<StripTabState> strip_tab_state() const;` — `save()` while the strip shows a layout, else empty.
- `switch_layout(next, true)` now implements §7.3 with deviation 1. All geometry comes from `view->page_sizes()` × the zoom, never from bitmaps, so a toggle that arrives before the previous layout's render has landed still carries the position (`h = h_pt × zoom`, `w = w_pt × zoom`, DIPs; `vp` = the client size in DIPs):
  1. **Capture first**, before anything changes the canvas, at `zoom_before = view->zoom_pct()`: from the strip → `StripViewport::carry()` and `pan_before = pan_x_eff()`; from paged single → `carry_from_paged_single(cur, pan_y, h <= vp_h ? (vp_h − h) / 2 : 0, h)` and `pan_before = pan_x`; from paged spread → `carry_from_spread(left, pan_y, sv.left_top_in_union, h_left)` with `sv = spread_vertical(h_left, h_right or −1, vp_h)`, and `pan_before = 0`.
  2. The Task 5 reset.
  3. **Apply after the fit**: set the current page to the carried page (into a spread, its left page, through `change_current_page`; into the strip, `set_strip_current_page`); `apply_viewport()`; `k = view->zoom_pct() / zoom_before`; `c.offset_dip *= k`. Into the strip → build the layout, `apply_carry(c)`, desired pan = `pan_before × k`. Into paged single → `pan_y = paged_pan_y_for(c, h, vp_h)`, `pan_x = clamp_pan(pan_before × k, w, vp_w)` (§7.3: `pan_x = pan_x_eff`). Into paged spread → `pan_y = spread_pan_y_for(c, sv.left_top_in_union, sv.union_h, vp_h)` with `sv` from the pair's heights at the new zoom, `pan_x = 0` (§7.3).
  4. `switch_layout(next, false)` (a tab switch) skips the capture and the apply.

Rules:
- `on_tab_switch`: in the outgoing snapshot, `outgoing->strip_state = canvas_->strip_tab_state();` beside `pan_x/pan_y` (before `set_view`). The incoming tab's state is restored by its `TabSwitch` kick: `kick_render` passes `&tab->strip_state.value()` when `restore_tab_position` and the state is present.
- `set_view` to a different view resets `StripViewport` (Task 5), which ends the navigation pin, so a pin cannot leak across strip tabs (§3.4, luna's finding).

- [ ] **Step 1: Write the failing smoke rows** (fresh launch of `large.pdf`):
  1. **two-page on, then off, from mid-page** (§9.3, deviation 6) — post `IDM_ZOOM_IN` twice; `page-box-go 30`; `canvas-wheel -120 3`; record `pos` and page box (`"30"`); post `IDM_VIEW_DUAL_PAGE` (40062), poll until `vscroll` false; post it again, poll until `vscroll` true; page box `"30"`, `|pos − recorded| <= 1`.
  2. **rapid toggling** (Review Focus 1) — `send-cmd 40062 10` (ten posts from one process, no waits); then poll ≤ 5 s: `vscroll` true (an even count ends in the strip), page box non-empty; then read `canvas-scroll` five times over 500 ms: `vscroll` stays true and `max` stays constant (no late strip or paged work flips the state); process alive; no new crash dump. Then `send-cmd 40062 1`, poll until `vscroll` false, and read it five times over 500 ms: it stays false (no strip completion or reschedule re-adds the scrollbar after leaving the strip).
  3. **a tab with two-page on has no scrollbar** (§9.3) — (tab 1 is now in spread: post 40062 once and poll `vscroll` true first) forward `bookmarks.pdf` as a second tab (single-instance forwarding); poll until `tab-enum` count is 2 and `vscroll` true (the forwarded tab entered the strip — without this the row passes in paged mode); post 40062 → `vscroll` false.
  4. **switching back to a strip tab restores the scrollbar** (§9.3) — post `IDM_TAB_GOTO_1` (40033) → `vscroll` true, page box `"30"`.
  5. **strip tab A at page 20, strip tab B keeps its own page** (§9.3) — post `IDM_TAB_GOTO_2` (40034) and poll `vscroll` false (tab 2 is still in spread); post 40062 and poll `vscroll` true; `page-box-go 3`; record tab 2's `pos`; switch to tab 1 (40033), `page-box-go 20`; switch to tab 2 (40034) without scrolling → page box `"3"`, not `"20"`, and `pos` equals the recorded value ± 1 (the page box alone follows `DocumentView::current_page`, which survives a lost scroll position).
  Run wrapped → rows fail (Task 5 ignores the carry and stores no tab state).
- [ ] **Step 2: Implement** the capture/apply, `Tab::strip_state`, the snapshot and the restore pointer. In the spec's §7.3, after "Into either paged layout, …", add: "Every carry is applied after the new layout's fit: the current page is set to the carried page, the viewport refits, and the offset is scaled by the zoom ratio (after / before), so a fit change does not shift the reader." In §9.3, change the two-page row's action to "two-page on, then off, from mid-page at a Custom zoom where the spread overflows, on a left page (page 30)".
- [ ] **Step 3: Run wrapped** → rows pass; every earlier strip row and the paged readings of Task 3 still pass. Full suite passes.
- [ ] **Step 4: Commit** — `feat(ui): carry position across layout switches and keep per-tab strip state (#55)`; include the spec edits.

## Task 9: Regression and the PR

**Files:**
- Modify: none, unless a check fails

- [ ] **Step 1: The §9.2 tests are untouched** — `git fetch origin`, then `git diff --stat origin/main -- tests/unit/test_scroll_math.cpp tests/unit/test_completion_math.cpp tests/unit/test_page_anchor.cpp tests/unit/test_viewport_math.cpp tests/unit/test_dual_page_layout.cpp tests/unit/test_prefetch_plan.cpp tests/unit/test_document_view.cpp tests/unit/test_page_cache_l1.cpp tests/unit/test_text_prefetch_view.cpp tests/unit/test_selection_drag.cpp tests/unit/test_session_state.cpp tests/unit/test_render_engine_cancel.cpp` → empty.
- [ ] **Step 2: Full suite** — `& $ctest --test-dir build -C Release -N` (record the count), then the run → all pass.
- [ ] **Step 3: Full smoke, wrapped** — `scripts\smoke-test.ps1 -ExpectDev` → passes, including the exe size gate (12 MB) and the cold-start line in both layouts; afterwards `ContinuousScroll` and `session.json` are back to their prior state and no crash dump is new.
- [ ] **Step 4: Paged spot check, wrapped, preference off** — on `large.pdf`: wheel flips at page edges, PgDn turns the page, Ctrl+Shift+D shows the spread with no scrollbar, Find Next lands on a hit; Task 3's readings repeat.
- [ ] **Step 5: Open the PR** — title `feat: continuous scroll strip renders and scrolls (#55, PR 2/4)`; the body lists the six spec deviations, the manual checks of Tasks 6 and 7, the `ctest -N` count, and states that the feature is reachable only through the registry value until PR 3.
