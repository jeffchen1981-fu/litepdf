# Phase 8: Tier 3 Completion — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use `superpowers:executing-plans` to implement this plan task-by-task. Pattern follows Phases 5–7: implementer → spec reviewer → quality reviewer → fix. **After this plan is written, run `/plan-eng-review` + `pr-review-toolkit:review-pr` (or equivalent stacked review per CLAUDE.md rule 14) BEFORE executing.**

**Goal:** Land the four remaining Tier 3 features so v0.0.9 is feature-complete: (1) modal password prompt for encrypted PDF, (2) end-to-end open path verification for ePub / CBZ / XPS via the existing `Document` allowlist, (3) View → Invert Colors dark-mode toggle that re-tints both chrome and rendered page content, (4) View → Two-Page Spread side-by-side layout. Total budget ≈ 600 LOC.

**Architecture:** Mirrors prior phases. The password dialog is a small Win32 modal (`DialogBoxParamW` + an in-memory `DLGTEMPLATE`, no new framework), invoked from `MainWindow::open_tab_async` when `Document::open` returns `OpenError::NeedsPassword`. The 3-attempt counter is per-open-attempt and dies when the dialog closes. ePub/CBZ/XPS need no engine change — `Document::looks_like_supported_document` already allowlists the extensions and verifies the ZIP magic bytes; Phase 8 adds fixture-driven unit + smoke coverage and tightens the title-bar / outline-pane fallbacks for non-PDF formats. Dark mode adopts MuPDF's `fz_invert_pixmap` at the worker (cheap, no D2D blend mode change) plus a new `Palette` flip in `PdfCanvas::on_paint` for chrome (background, scrollbar, find-bar pass-through). The flag is per-tab (`DocumentView::invert_colors_`) so each document's preference survives tab switch — same per-tab discipline as F4/F5 panes from Phase 7 D11. Two-page spread is layout-only inside `PdfCanvas`: when enabled, the canvas fits TWO pages side-by-side at the canvas DIP-width budget, ships TWO render requests (current + current+1), and double-buffers both into a single Direct2D draw. PgUp/PgDn step by 2 (clamped). Cover page (page 0) is shown alone — book-style — to match Acrobat / Foxit. Per-tab state (`DocumentView::dual_page_`).

**Tech Stack:** C++17, Win32 (USER + GDI), in-memory `DLGTEMPLATE` for the password dialog (no new `.rc` resource — keeps localization story aligned with the existing English-only menu strings), MuPDF's existing `fz_invert_pixmap` from `<mupdf/fitz/pixmap.h>`, Phase 7's existing render pipeline. **No new dependencies.**

---

## Pre-flight

Before starting, confirm:

- Working tree is clean: `git status` shows no uncommitted changes.
- Branch off the latest `origin/main` (= post-Phase-7 tip, currently `c9f3a7d`).
- Release build + tests currently green:
  ```bash
  cmake --build build --config Release
  ctest --test-dir build -C Release --output-on-failure
  ```
  Expected: 130/130 pass + 1 [!shouldfail] (Phase 7 actual baseline per handoff doc 2026-04-27).

**Reference open at all times:**

- `docs/plans/2026-04-15-litepdf-design.md` §1 (size + cold-start budgets), §3 (Tier 3 list), §4.1 (module layout), §6.1 (open path error flow with NeedsPassword + 3-attempt cap), §10 (out-of-scope list — guards against scope creep).
- `docs/plans/2026-04-15-litepdf-roadmap.md` Phase 8 row + LOC table.
- `docs/plans/2026-04-25-phase-7-thumbnails.md` — D11 (per-tab pane state) + D12 (no v1 persistence) precedents that Phase 8 mirrors for both invert + dual-page; T9 version-bump pattern.
- `docs/plans/2026-04-24-phase-6-search.md` — plan-eng-review + pr-review-toolkit incorporation patterns.
- `src/core/Document.{hpp,cpp}` — `OpenError::NeedsPassword`, `Document::authenticate(password)`, and the free function `looks_like_supported_document` in an anonymous namespace inside `Document.cpp` (NOT a `Document::` member). Already wired; Phase 8 just wires the dialog on top.
- `src/ui/TabManager.cpp` — Palette + `WM_SETTINGCHANGE` reference (lines 33–304); pattern reused by FindBar / ResultsPanel / Splitter / ThumbnailPane.
- `src/ui/PdfCanvas.cpp` — `WM_USER_RENDER_DONE` handler (lines 375–410), where the inverted bitmap path will piggy-back; D2D bitmap upload currently uses BGRA + `IGNORE_ALPHA`.
- `src/ui/MainWindow.cpp` — `ACCEL accels[]` at lines 1360-1389 (verify with `grep -n 'ACCEL accels' src/ui/MainWindow.cpp` before edit) for new accelerator additions; `open_tab_async` for the password handshake site; About-dialog literal at line 936 (will bump to v0.0.9). Note line 917 holds an `OPENFILENAME` field — earlier plan drafts pointed there incorrectly.
- `src/core/DocumentView.{hpp,cpp}` — Phase 7 added `ensure_thumb_pane`/`thumb_pane()`. Pattern reused for two new per-tab flags + their getters/setters.
- `~/.claude/compact-dumps/litepdf/rationale.md` — read entries 2026-04-26 (plan-defects from Phase 7) + 2026-05-01 (smoke-test build environment notes; OutlinePane empty-content finding deferred from Phase 7).

**Smoke-test app invocation:**

```bash
build/Release/litepdf.exe tests/fixtures/encrypted.pdf
# Then:
#   Password dialog appears modal with "OK" / "Cancel" + edit field.
#   Type wrong password → status text "Incorrect password (1 of 3)".
#   Type wrong password 2 more times → dialog closes; tab also closes.
#   Re-open with --, type "test" → tab opens, page renders.

build/Release/litepdf.exe tests/fixtures/sample.epub
build/Release/litepdf.exe tests/fixtures/sample.cbz
# Both should open. Title bar shows the basename.
# F5 outline (epub may have one; cbz typically none — pane shows but is empty).

# Then in any open PDF:
#   View > Invert Colors (Ctrl+Shift+I) → page renders white-on-black; chrome background flips.
#   View > Two-Page Spread (Ctrl+Shift+D) → two pages render side-by-side; PgDn skips 2 pages.
```

**Pre-flight verification:**

1. **Re-baseline test count.** `origin/main` is at `c9f3a7d` (post-Phase-7); rationale dump records 130 cases + 1 [!shouldfail]. Verify before quoting "X/X pass" downstream:

   ```bash
   grep -c TEST_CASE tests/unit/*.cpp | awk -F: '{sum+=$2} END {print sum}'
   ```

   If the grep returns a different number, recompute downstream cumulative test counts.

2. **Confirm encrypted fixture password.** `tests/unit/test_document_password.cpp:21` asserts the password is `"test"`. The password literal in the dialog plan and integration test must match.

3. **Confirm fixtures exist.**
   ```bash
   ls tests/fixtures/encrypted.pdf tests/fixtures/sample.epub tests/fixtures/sample.cbz
   ```
   All three must already be present (verified at plan write — they exist; no XPS fixture, see D6 for the deferral path). If any goes missing, generate or `git checkout`.

4. **Confirm `fz_invert_pixmap` availability.** The header `third_party/mupdf/include/mupdf/fitz/pixmap.h:300` exposes `void fz_invert_pixmap(fz_context*, fz_pixmap*)`. Verify before T2:
   ```bash
   grep -n "fz_invert_pixmap" third_party/mupdf/include/mupdf/fitz/pixmap.h
   ```
   Expect exactly the prototype on line ~300. If missing, MuPDF version drift — escalate before continuing (don't try to roll our own SIMD invert).

5. **Confirm `OpenError::NeedsPassword` open path.** `Document::open` returns the error AFTER setting `impl_->path` and AFTER calling `fz_needs_password`, which means the doc is alive and `authenticate(pw)` can be called on the same `Document` object. **Critical for Task 1**: the password dialog calls `view->document().authenticate(...)` on the very `Document` whose `open()` returned NeedsPassword — do NOT close + re-open between attempts. Verify by reading lines 200–248 of `src/core/Document.cpp`.

---

## Architectural Decisions

**D1. Password dialog uses an in-memory `DLGTEMPLATE` constructed at runtime, not a new `.rc` resource.** The dialog has 4 controls (icon stub + static label + edit + 2 buttons) and a single localization (English) shared with menu strings. Adding a `.rc` block per Phase 9 icon work would lock us into a resource-compile dependency chain that the rest of LitePDF avoids. Pattern: a `std::vector<uint8_t>` built with a small helper (similar in spirit to MS docs sample at <https://learn.microsoft.com/en-us/windows/win32/dlgbox/using-dialog-boxes#creating-a-template-in-memory>), passed to `DialogBoxIndirectParamW`. Trade-off: ~50 LOC of dialog-template plumbing. Rejected: full `.rc` dialog (more LOC across `litepdf.rc` + `MainMenu.rc.h` + a new dialog ID) and rejected: a custom `WNDCLASS`-based modal (too heavy for one-shot use).

**D2. Password attempts are bounded at 3 per `open()`-handshake, NOT per session.** Design §6.1 says "3-attempt cap"; Phase 8 implements that as: each call to `MainWindow::open_tab_async` that lands on `NeedsPassword` opens one dialog instance with its own attempt counter. If the user dismisses (Cancel) or exhausts 3 attempts, the tab is destroyed and the file is NOT auto-removed from MRU (per design §6.1's "remove only on file-not-found" rule). If the same user later re-opens the same file, they get a fresh 3-attempt counter — this matches the design's "per session" wording read literally as "per open attempt", which is the only meaningful unit when no persistent rate-limit storage exists. Trade-off: a malicious local actor with shell access can keep retrying; we are not a credential-store, so this is acceptable. Rejected: a global counter in `MruList` or a registry key (would persist failures across runs, hostile UX).

**Threading note (D2 addendum):** `MainWindow::open_tab_async`'s "async" refers to the file-load step — `Document::open` may briefly block on disk I/O and is invoked off the message-pump. The password retry loop, by contrast, runs **inline on the UI thread**: `DialogBoxIndirectParamW` is a blocking modal call that requires the message pump and a parent HWND, neither of which exist on a worker. Concretely: when `open()` returns `NeedsPassword`, the worker stores the still-alive `Document` handle, posts a `WM_USER_PASSWORD_PROMPT` to `hwnd_` carrying the worker's continuation, and the UI thread runs the 3-attempt loop + `authenticate()` calls + tab-creation handshake all in one inline sequence (authenticate is a few-microsecond bcrypt-equivalent — no need to bounce back to the worker). Rationale: keeping the dialog and `authenticate` on the same thread avoids the marshal-twice cost AND eliminates a class of "user clicked Cancel but worker already submitted second auth call" races. Rejected: showing a system-modal dialog from the worker via `MB_TASKMODAL` (loses parent-window centering + tab-strip blur effect).

**D3. Password is held in a `std::string` (UTF-8) for the duration of the dialog and overwritten with zeros before destruction** (`SecureZeroMemory` on Windows, OR `std::fill_n` if the volatile cast suffices). NEVER logged via `std::fprintf`/`OutputDebugStringW`. The dialog edit control uses `ES_PASSWORD` so on-screen display is masked. Even on a wrong-password failure, the rejected string is zeroed before the dialog updates the status label (the label only shows attempt count, never the rejected password). Trade-off: minor LOC overhead; this is the weakest link in any Win32 password handler and worth the discipline.

**D4. ePub / CBZ / XPS open path is verified, NOT extended.** `looks_like_supported_document` (a free function in an anonymous namespace at `Document.cpp:113-152`, NOT a `Document::` member) already allowlists `.epub/.cbz/.xps/.fb2/.svg` and validates ZIP magic bytes for the three ZIP-family formats. MuPDF auto-detects format inside `fz_open_document`. Phase 8's job is to (a) add unit tests that open each fixture format and assert page_count ≥ 1, (b) confirm the title-bar shows the basename for non-PDF formats (it already does, since the title is path-derived and not engine-derived), (c) confirm crash-free render of page 0 via the smoke test. **No code change to `Document` or `RenderEngine`.** The minimal addition is a new test file + smoke-test extension. Rejected: writing per-format adapters or registering custom MuPDF document handlers; MuPDF's defaults already cover everything we need at v1.

**D5. No XPS fixture in v1.** The repo has `simple.pdf`, `bookmarks.pdf`, `corrupt.pdf`, `encrypted.pdf`, `sample.cbz`, `sample.epub`, `search.pdf`, `測試.pdf`, plus `sample.png` (negative test). There is no XPS fixture, and procuring or generating a tiny XPS file requires either Windows print-to-XPS (not headless-friendly) or an external XPS authoring tool (not in our toolchain). Phase 8 ships ePub + CBZ verification, asserts the XPS allowlist code path stays intact (extension allowlist check has a unit test that names `.xps` in its parameter list), and defers the real-fixture XPS test to Phase 11 (binary-size regression) where MuPDF's CMaps / fonts will be re-audited anyway. Trade-off: roadmap exit criterion says "All format fixtures open"; at plan time only ePub + CBZ exist as actual files. Roadmap is satisfied because every fixture present DOES open; XPS is not regressed (allowlist unit test guards). Recorded in §"Out of scope" too. Rejected: synthesizing an XPS fixture by zipping the FixedDocument.fdoc XML schema by hand — fragile and test-only; not worth the LOC.

**D6. FB2 and SVG are explicitly out of Phase 8 scope.** Both are MuPDF-supported (allowlist in `Document.cpp`) but design §3 lists them in Tier 3 without a specific phase assignment. Roadmap §"Phase 8" exit criteria mention `ePub/CBZ/XPS` only. FB2 and SVG fixtures don't exist in `tests/fixtures/`; the allowlist code path is exercised by an extension-only assertion, same as XPS. Adding fixtures + tests for both would push Phase 8 beyond its 600-LOC budget for marginal Tier 3 gain. Recorded in §"Out of scope". Rejected: implicit scope expansion by treating "Tier 3 completion" as encompassing every Tier 3 bullet; doing so would conflate roadmap rows.

**D7. "Invert Colors" (the menu label) = whole-pixmap channel invert at the render boundary, NOT a Direct2D blend mode and NOT semantic dark-mode.** The user-facing menu label is **"Invert Colors"** (matching SumatraPDF), NOT "Dark Mode" — this is a deliberate UX choice. Phase 8 ships **plain channel invert via `fz_invert_pixmap`**: every pixel including images, logos, and color charts gets `R=255-R, G=255-G, B=255-B`. Documents with photos / colored content will look psychedelic when inverted; that is expected and matches the v1 promise. Foxit-style "Night Mode" (selective invert that preserves images) is a Phase 11 polish target — out of scope for v1. Calling the menu item "Invert Colors" sets the right expectation; calling it "Dark Mode" would over-promise. **D7 mechanism (cont.):** When the per-tab `invert_colors_` flag is set, the worker calls `fz_invert_pixmap(ctx, pix)` immediately after the rasterize at `RenderEngine.cpp:189` (`fz_new_pixmap_from_display_list`) BEFORE the pixmap travels to the UI. (The codebase does NOT call `fz_run_page` directly — the rasterize is via display list. See T3 step 3.1 for the exact hook site.) This adds ≤ ~3 ms per A4 page at 150 DPI (memcpy-bound in cache; benchmarked roughly against a manual loop in a scratch test) and requires zero D2D code change. The chrome (frame background, scrollbar fallback rectangle, find-bar pass-through) is repainted by flipping the per-canvas `Palette` brushes — same mechanism the existing TabManager / FindBar / ResultsPanel / Splitter use for system dark-mode flips. The user-visible result: page renders white-on-black instead of black-on-white; chrome flips to dark. Trade-off: the inverted pixmap occupies a separate cache slot from its non-inverted counterpart — see D8. Rejected: D2D blend-mode invert (would require shaders / `ID2D1Effect` factory chain — bigger LOC + harder DPI story). Rejected: gamma-aware invert (Phase 11 polish; v1 does plain channel-invert per the prompt's "anti-scope-creep" note).

**D8. ONLY the L1 (pixmap) cache key gains an `invert: bool` axis. L2 (display list) does NOT.** L2 stores zoom-and-polarity-independent display lists — `fz_display_list` is a recording of drawing primitives (paths + glyphs + colors as authored by the PDF), and the inversion happens AFTER `fz_run_display_list` rasterizes those primitives into a pixmap. If we keyed L2 by invert, we would (a) waste a slot per polarity for an identical display list, AND (b) lose the ~5 ms display-list build savings when toggling Ctrl+Shift+I (cache hit on display list, miss on pixmap is the optimal path). Without this, switching dark mode mid-document either (a) serves stale non-inverted pixmaps from L1, or (b) forces an L1 + L2 nuke — both ugly. Solution: extend ONLY L1's key tuple from `(page, scale)` to `(page, scale, invert)`; L2 stays `(page)`. Of the four cache touch points in `RenderEngine.cpp`, only the L1 pair needs the new axis: L1 read at line 121 + L1 write at line 208. The L2 read at line 139 and L2 write at line 167 are unchanged. (Phase 7 D18 originally cataloged these at L113/L129/L155/L192; lines drifted ~8-16 between Phase 7 ship and Phase 8 plan-write — see the table in T3.1 for the re-anchored numbers.) Capacity stays 5 / 10 (no doubling needed; users typically toggle dark mode once per session). T3 includes a `[page_cache]` test asserting two different `(page, scale)` L1 entries differ when `invert` differs AND that `(page)` L2 hits regardless of invert polarity. Trade-off: minimal patch — one extra bool in the L1 key struct + std::tuple-style hash + comparator update + an explicit comment in `PageCache.hpp` about why L2 stays single-key; ≤ 35 LOC across `PageCache.{hpp,cpp}`. Rejected: bypassing cache entirely when invert is on (kills paging UX). Rejected: a parallel L1 dedicated to inverted pixmaps (more complexity than a third key axis on L1 alone). Rejected: keying both L1 and L2 by invert (rejected on the cache-pollution argument above).

**D9. Both invert AND dual-page are per-tab, default-off, NOT persisted across app restarts.** Mirrors Phase 7 D11 (per-tab pane state) and D12 (no v1 persistence — Phase 12 session.json may revisit). Per-tab because users frequently want one PDF in dark mode and another in light (e.g. a CBZ comic in light, a paper in dark). Default off because no cold-start config to honor and the user can toggle in <1 s. Not persisted because v1 has no `session.json`. Trade-off: settings reset on every relaunch; matches every other Phase-7 toggle. Rejected: app-global flag (loses tab-independence). Rejected: registry persistence (re-introduces a Phase-12 problem as a Phase-8 commitment).

**Toggle-cancellation discipline (D9 addendum):** When `set_invert_colors(b)` flips the flag, in-flight render requests submitted at the OLD polarity may still be running on a worker. Their `on_complete` callbacks would deliver stale-polarity pixmaps that briefly flash the wrong colors before the kicked re-render lands. Mitigation mirrors Phase 7 D16 / D17 (the `cancel_pending` priority + `on_complete(nullptr, ctx)`-on-cancel pattern that the `RenderEngine` worker loop already honors after Phase 7 T0.5):

1. `DocumentView::set_invert_colors` calls `cancel_stale_renders(0)` on its `RenderEngine` reference — Phase 7's existing API path.
2. The `RenderEngine` cancel checkpoints (post-T0.5) invoke each cancelled request's `on_complete(nullptr, ctx)`, so PdfCanvas's `WM_USER_RENDER_DONE` handler observes a null pixmap and discards it without painting.
3. The toggle handler then `kick_render`s the active page at the new polarity. The next paint shows the new polarity uncontaminated.

NOT honoring this drain leaves a 50-100 ms window where a stale-polarity pixmap can land and paint, producing a visible flash. The `[render_engine][invert]` integration test in T3.5 explicitly asserts no flash by counting paint events between toggle and stable state. Same drain pattern applies to `set_dual_page` toggles, since the page-pair render submission also races with single-page in-flights — T4.4 explicitly calls `cancel_stale_renders(0)` before submitting the new pair. Rejected: tear down + rebuild the engine (drops L2 display-list cache for no benefit). Rejected: synchronous wait-on-drain (UI thread blocked for up to 80 ms per toggle — visible jank).

**D10. Two-page spread is layout-only — no separate "spread page" abstraction in `Document` or `RenderEngine`.** When the per-tab `dual_page_` flag is set, `PdfCanvas` (a) reduces the per-page DIP width budget to half the canvas width minus a 8-DIP gutter, (b) submits a render request for `current_page` with `priority=0` AND for `current_page + 1` (when in range) with `priority=1`, (c) blits both into the single client area. PgUp/PgDn arithmetic at `PdfCanvas::on_key_down` advances by `dual_page_ ? 2 : 1` pages. The cover-page rule: page 0 always renders alone (book-style, matching Acrobat default with cover-page enabled). Subsequent: pages (1,2), (3,4), … This means with N pages, last spread is (N-2, N-1) when N is even; (N-1, alone) when N is odd. Trade-off: scrolling within a page-pair is single-axis (vertical) only; horizontal scroll within one of the two pages is not supported in dual mode (very minor UX edge — design doc doesn't promise it). Rejected: a `PdfCanvas::SpreadModel` PIMPL — over-engineering for layout that fits in 50 LOC. Rejected: rendering both pages onto one large pixmap inside `Document` — engine pollution, breaks single-page caching.

**D11. Two-page spread interaction with thumbnail pane: thumbs stay single-column, current-page highlight follows the LEFT page of the spread.** Phase 7's `ThumbnailPane` paints a single-column tile list (Phase 7 D4); Phase 8 does NOT generalize to dual-column thumbs (post-v1 polish per Phase 7 §"Known Limitations" "No dual-page thumbnails"). When `dual_page_` is on and the canvas page changes to a pair (e.g. (5,6)), `set_current_page(5)` is called on the thumbnail model so the highlight border is on the LEFT page. Right-page tile is NOT highlighted — minor UX inaccuracy noted in §"Known Limitations". Rejected: dual-column highlight (cascades into Phase 7 D4 layout invariant). Rejected: highlight both LEFT + RIGHT separately (visual clutter).

**D12. Two-page spread interaction with cross-tab search: jump to page-PAIR containing the hit.** When `SearchSession::next()` returns a hit on page P and `dual_page_` is on, scroll-into-view computes the page-pair (cover rule: P=0 → spread (0,−); else P odd → (P, P+1); P even → (P-1, P)). `PdfCanvas::scroll_into_view(hit)` already calls `change_current_page(hit.page)` which fires the page-changed observer — observer already updates current_page in the canvas, no extra hook needed; only the *layout* code in T4 reads `dual_page_` to decide spread placement. Trade-off: per-pair semantic is implicit in the layout; explicit `current_page = pair_left_page(hit.page)` happens inside `PdfCanvas::dual_page_compute_left()`. Rejected: introducing a `SpreadHit` overlay path; the existing per-page hit overlay already paints quads in PDF user space which the layout transform handles uniformly.

**D13. Tag at end: `v0.0.9-phase8`.** Mirrors phase 5/6/7 convention. Version bump in `VERSION`, About dialog, `litepdf.rc` VERSIONINFO block (sat at `0.0.6.0` since Phase 5 — Phase 8 picks up two phases of debt and writes `0,0,9,0`). No fast-follow tags planned at write time; if something surfaces post-tag, follow `phase8.1` pattern.

**D14. Thumbnails do NOT render inverted in v1.** The `ThumbnailRenderer` generates pixmaps that are scale-keyed only (no `invert` axis); adding an `invert` axis to the thumb cache would double thumb cache footprint for marginal UX value. When `invert_colors_` is on for a tab, the main canvas renders white-on-black but the thumbnail pane continues to show the original (non-inverted) page appearance. This is intentional: thumbs are navigation affordances, not page-accurate previews — users benefit from recognizing page content even while the main view is inverted. Out of scope for v0.0.9; revisit if user feedback surfaces. **Implementer note:** do NOT pass `invert_colors_` into `ThumbnailRenderer::request_render`. That call site is entirely separate from `DocumentView::request_render` and must remain polarity-agnostic in Phase 8.

**D15. Programmatic page-jump in spread mode uses the same snap rule as PgDn.** OutlinePane clicks, MRU re-open navigation, and search-result jumps (other than the `PdfCanvas::scroll_into_view` path already covered by D12) all call `DocumentView::set_current_page()` or equivalent. In spread mode these must apply the same snap: requested page N becomes spread `(N, N+1)` if N is odd, else `(N-1, N)`. Cover-page exception still applies (page 1 alone). This keeps the snap rule centralized in `DocumentView::set_current_page()` (or a shared `dual_page_compute_left` call at entry) rather than duplicated at every programmatic entry point. Rejected: entry-point-specific snap logic in each caller — fragile and likely to diverge as Phase 9+ adds more navigation entry points.

---

## Task List

### Task 0: Reserve menu IDs + accelerators (no behavior change)

**Goal:** Land the menu-ID + accelerator wiring so future tasks can reference them; both menu items are grayed (no handlers yet, but accel falls through to DefWindowProc).

**Files:**
- Modify: `resources/MainMenu.rc.h` — add two new IDs.
- Modify: `resources/litepdf.rc` — add two View-menu items.
- Modify: `src/ui/MainWindow.cpp` — add 2 entries to `ACCEL accels[]`.

**Step 0.1:** Edit `resources/MainMenu.rc.h`. After the Phase 7 thumbnail block (`IDM_VIEW_THUMBS 40060`), add:

```c
// Phase 8: Tier 3 completion.
#define IDM_VIEW_INVERT      40061   // Ctrl+Shift+I
#define IDM_VIEW_DUAL_PAGE   40062   // Ctrl+Shift+D

// Next free ID: 40063. Reserve 40063-40070 for future Phase 8.x cleanups.
```

**Step 0.2:** Edit `resources/litepdf.rc` View popup. After the existing `Toggle &Thumbnails\tF4` line and BEFORE the SEPARATOR before zoom items, add:

```rc
MENUITEM "&Invert Colors\tCtrl+Shift+I", IDM_VIEW_INVERT
MENUITEM "&Two-Page Spread\tCtrl+Shift+D", IDM_VIEW_DUAL_PAGE
```

(Both items will be checkmarked-when-on once T2 / T4 land their `WM_INITMENUPOPUP` handlers; T0 ships them unchecked.)

**Step 0.3:** Edit `src/ui/MainWindow.cpp` `ACCEL accels[]` at lines 1360-1389 (the same array that holds `VK_F4 / VK_F5 / Ctrl+O`; verify exact range with `grep -n 'ACCEL accels' src/ui/MainWindow.cpp` before edit). Add adjacent to the existing F4/F5 lines:

```cpp
{ FCONTROL | FSHIFT | FVIRTKEY, 'I', IDM_VIEW_INVERT    },
{ FCONTROL | FSHIFT | FVIRTKEY, 'D', IDM_VIEW_DUAL_PAGE },
```

Verify `Ctrl+Shift+I` and `Ctrl+Shift+D` are not already bound — read lines 1360-1389. Existing Phase 6/7 bindings (verified against `c9f3a7d` HEAD): `Ctrl+F`, `Ctrl+Shift+F`, `Ctrl+1..9` (tab-goto), `Ctrl+W`, `Ctrl+Tab` / `Ctrl+Shift+Tab`, `Ctrl+O`, `F4`, `F5`. `Ctrl+T` is **not** bound (no `IDM_TAB_NEW` exists; new tab is via `Ctrl+O`). Both `Ctrl+Shift+I` and `Ctrl+Shift+D` are free.

**Why Ctrl+Shift+D instead of Ctrl+D:** Windows-app convention is `Ctrl+Letter` for file/edit operations (Ctrl+O open, Ctrl+W close, Ctrl+T new tab, etc.) and `Ctrl+Shift+Letter` for view-mode toggles (Ctrl+Shift+W in Adobe for window mode, etc.). Phase 8 ships two view-mode toggles (Invert + Dual-page), so making both use `Ctrl+Shift+_` reads as a deliberate modifier-consistent group. Also reserves the bare `Ctrl+D` for a future "Duplicate Tab" affordance.

> **Bound-key history note:** earlier drafts of this plan mapped dual-page to `Ctrl+2` (collides with `IDM_TAB_GOTO_2`) then `Ctrl+D` (modifier-inconsistent with `Ctrl+Shift+I`). The accel array above uses `Ctrl+Shift+D` and the menu literal in T0.2 reads `\tCtrl+Shift+D`. If the implementer sees `Ctrl+2` or bare `Ctrl+D` anywhere, it's stale.

**Step 0.4:** Build + smoke-test:

```bash
cmake --build build --config Release
build/Release/litepdf.exe tests/fixtures/simple.pdf
```

Expected: View menu shows "Invert Colors Ctrl+Shift+I" and "Two-Page Spread Ctrl+Shift+D" entries, both grayed / no-op. Pressing Ctrl+Shift+I or Ctrl+Shift+D does nothing visible. Pressing Ctrl+2 still activates the second tab (no regression on `IDM_TAB_GOTO_2`). **No test changes — this is pure resource wiring.**

**Step 0.5:** Commit.

```bash
git add resources/MainMenu.rc.h resources/litepdf.rc src/ui/MainWindow.cpp
git commit -m "feat(ui): reserve menu IDs + accels for invert + dual-page (Phase 8 Task 0)"
```

---

### Task 1: Password dialog — modal + 3-attempt loop + dialog-template helper

**Goal:** Implement `ui::PasswordDialog::prompt(parent_hwnd, file_basename) -> std::optional<std::string>`. On success returns the entered password; on cancel / 3 failures returns `std::nullopt`. The dialog itself only collects the string and updates an attempt counter on a status label; verification (calling `Document::authenticate`) happens in the caller (`MainWindow::open_tab_async`) — keeping `PasswordDialog` UI-only and unit-testable on the input-validation path.

**Files:**
- Create: `src/ui/PasswordDialog.hpp` (~40 LOC).
- Create: `src/ui/PasswordDialog.cpp` (~200 LOC including in-memory dialog template builder).
- Create: `tests/unit/test_password_dialog.cpp` (~60 LOC — covers the helper that builds the in-memory `DLGTEMPLATE`; cannot test the actual modal in a headless test, so we test only the pure-logic layout-bytes helper).
- Modify: `src/ui/MainWindow.cpp` — wire in `PasswordDialog::prompt` from `open_tab_async` on `NeedsPassword`.
- Modify: `tests/CMakeLists.txt`, top-level `CMakeLists.txt`.

**Step 1.1: Header `src/ui/PasswordDialog.hpp`:**

```cpp
#pragma once

// ui::PasswordDialog — modal Win32 password prompt for encrypted PDF.
// In-memory DLGTEMPLATE; no .rc resource. Caller invokes prompt() from
// the open path on Document::OpenError::NeedsPassword. Returns the
// entered password (UTF-8) on accept, nullopt on cancel or attempt
// exhaustion.
//
// Verification (Document::authenticate) is the caller's responsibility:
// the dialog tracks attempt count internally and re-shows itself with
// an error label up to 3 times, but the password's correctness is
// determined by Document::authenticate which is called BETWEEN dialog
// re-prompts (caller loop). See MainWindow::open_tab_async wiring.
//
// Memory hygiene: returned std::string uses standard alloc; caller is
// responsible for SecureZeroMemory'ing it after Document::authenticate
// consumes it. The dialog's internal edit-control buffer is zeroed
// before WM_DESTROY returns.

#include <optional>
#include <string>
#include <windows.h>

namespace litepdf::ui {

class PasswordDialog {
public:
    // Show the modal. Blocks until OK / Cancel / 3 failures.
    // Returns:
    //   - the entered password on OK,
    //   - std::nullopt on Cancel or after the caller has re-shown the
    //     dialog 3 times (caller-tracked attempt counter; dialog itself
    //     just shows the message text it's given).
    //
    // @param parent       The window the dialog is modal to (typically
    //                     the MainWindow HWND).
    // @param basename     Human-readable file name (UTF-16) shown above
    //                     the input. Empty allowed (just shows generic
    //                     prompt).
    // @param status_text  Text shown next to the input. Use to display
    //                     "Incorrect password (N of 3)" between retries.
    //                     Empty hides the status row.
    static std::optional<std::string> prompt(HWND parent,
                                             const std::wstring& basename,
                                             const std::wstring& status_text);

private:
    PasswordDialog() = delete;
};

}  // namespace litepdf::ui
```

**Step 1.2: Failing tests `tests/unit/test_password_dialog.cpp`:**

```cpp
#include "ui/PasswordDialog_internal.hpp"  // exposes build_template()

#include <catch2/catch_test_macros.hpp>
#include <cstring>

using litepdf::ui::detail::build_dialog_template;

TEST_CASE("PasswordDialog template: header has DS_MODALFRAME + WS_POPUP",
          "[password_dialog]") {
    std::vector<uint8_t> tmpl = build_dialog_template(
        L"foo.pdf", L"", /*dpi=*/96);
    REQUIRE(tmpl.size() >= sizeof(DLGTEMPLATE));
    auto* hdr = reinterpret_cast<const DLGTEMPLATE*>(tmpl.data());
    REQUIRE((hdr->style & DS_MODALFRAME) != 0);
    REQUIRE((hdr->style & WS_POPUP)      != 0);
    // 4 controls: static label + edit + 2 buttons.
    REQUIRE(hdr->cdit == 4);
}

TEST_CASE("PasswordDialog template: empty basename still produces valid bytes",
          "[password_dialog]") {
    std::vector<uint8_t> tmpl = build_dialog_template(L"", L"", 96);
    REQUIRE(tmpl.size() > sizeof(DLGTEMPLATE));
}

TEST_CASE("PasswordDialog template: status text is included when non-empty",
          "[password_dialog]") {
    auto with    = build_dialog_template(L"x.pdf", L"Try again (2 of 3)", 96);
    auto without = build_dialog_template(L"x.pdf", L"",                     96);
    REQUIRE(with.size() > without.size());  // status text adds bytes
}
```

**Step 1.3: Run, confirm tests fail to compile** (header missing).

**Step 1.4: Implement `src/ui/PasswordDialog.cpp`** with in-memory `DLGTEMPLATE` builder. Helper exposed in a `_internal.hpp` for unit testing. Skeleton:

```cpp
// src/ui/PasswordDialog_internal.hpp (private header)
#pragma once
#include <vector>
#include <string>
#include <windows.h>
namespace litepdf::ui::detail {
// Build an in-memory DLGTEMPLATE + 4 DLGITEMTEMPLATE entries packaged as a
// flat byte vector suitable for DialogBoxIndirectParamW. WORD-aligned per
// MSDN. dpi controls font size scaling (default 96 = 9 pt).
std::vector<uint8_t> build_dialog_template(const std::wstring& basename,
                                           const std::wstring& status_text,
                                           UINT dpi);
}
```

```cpp
// src/ui/PasswordDialog.cpp
#include "ui/PasswordDialog.hpp"
#include "ui/PasswordDialog_internal.hpp"

#include <array>
#include <cstring>
#include <vector>
#include <windows.h>

namespace litepdf::ui {

namespace detail {

// Helpers: WORD-align the byte cursor; emit a UTF-16 string + NUL into
// the vector. See Microsoft "Creating a Template in Memory" docs.
namespace {
void align_to_dword(std::vector<uint8_t>& v) {
    while ((v.size() % 4) != 0) v.push_back(0);
}
void emit_wstring(std::vector<uint8_t>& v, const std::wstring& s) {
    const auto* p = reinterpret_cast<const uint8_t*>(s.c_str());
    v.insert(v.end(), p, p + (s.size() + 1) * sizeof(wchar_t));
}
void emit_word(std::vector<uint8_t>& v, WORD w) {
    v.insert(v.end(),
             reinterpret_cast<const uint8_t*>(&w),
             reinterpret_cast<const uint8_t*>(&w) + sizeof(WORD));
}
}  // namespace

std::vector<uint8_t> build_dialog_template(const std::wstring& basename,
                                           const std::wstring& status_text,
                                           UINT /*dpi*/) {
    std::vector<uint8_t> out;
    out.reserve(512);

    DLGTEMPLATE hdr{};
    hdr.style = DS_MODALFRAME | DS_CENTER | DS_SETFONT |
                WS_POPUP | WS_CAPTION | WS_SYSMENU;
    hdr.dwExtendedStyle = 0;
    hdr.cdit = 4;       // label, edit, OK, Cancel
    hdr.x = 0; hdr.y = 0; hdr.cx = 200; hdr.cy = status_text.empty() ? 70 : 90;
    out.insert(out.end(),
               reinterpret_cast<const uint8_t*>(&hdr),
               reinterpret_cast<const uint8_t*>(&hdr) + sizeof(hdr));
    // After header: menu (none), windowClass (none), title, then if
    // DS_SETFONT: pointsize + typeface. Each is a UTF-16 NUL-terminated
    // string (or 0x0000 for "none").
    emit_word(out, 0x0000);              // no menu
    emit_word(out, 0x0000);              // standard dialog class
    emit_wstring(out, std::wstring(L"Password Required"));
    emit_word(out, 9);                    // 9 pt
    emit_wstring(out, std::wstring(L"Segoe UI"));

    // ---- Item 1: static label ("Enter password for <basename>") ----
    align_to_dword(out);
    {
        DLGITEMTEMPLATE item{};
        item.style = WS_CHILD | WS_VISIBLE | SS_LEFT;
        item.x = 7; item.y = 7; item.cx = 186; item.cy = 16;
        item.id = -1;       // static, no notifications needed
        out.insert(out.end(),
                   reinterpret_cast<const uint8_t*>(&item),
                   reinterpret_cast<const uint8_t*>(&item) + sizeof(item));
        emit_word(out, 0xFFFF); emit_word(out, 0x0082);  // STATIC
        std::wstring label = L"Enter password";
        if (!basename.empty()) label += L" for " + basename;
        label += L":";
        emit_wstring(out, label);
        emit_word(out, 0x0000);  // creation data length
    }

    // ---- Item 2: edit (ES_PASSWORD) ----
    align_to_dword(out);
    constexpr WORD kIdEdit = 1001;
    {
        DLGITEMTEMPLATE item{};
        item.style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER |
                     ES_AUTOHSCROLL | ES_PASSWORD;
        item.x = 7; item.y = 25; item.cx = 186; item.cy = 14;
        item.id = kIdEdit;
        out.insert(out.end(),
                   reinterpret_cast<const uint8_t*>(&item),
                   reinterpret_cast<const uint8_t*>(&item) + sizeof(item));
        emit_word(out, 0xFFFF); emit_word(out, 0x0081);  // EDIT
        emit_wstring(out, L"");
        emit_word(out, 0x0000);
    }

    // ---- Optional Item 2.5: status text ----
    constexpr WORD kIdStatus = 1003;
    if (!status_text.empty()) {
        align_to_dword(out);
        DLGITEMTEMPLATE item{};
        item.style = WS_CHILD | WS_VISIBLE | SS_LEFT;
        item.x = 7; item.y = 43; item.cx = 186; item.cy = 14;
        item.id = kIdStatus;
        out.insert(out.end(),
                   reinterpret_cast<const uint8_t*>(&item),
                   reinterpret_cast<const uint8_t*>(&item) + sizeof(item));
        emit_word(out, 0xFFFF); emit_word(out, 0x0082);  // STATIC
        emit_wstring(out, status_text);
        emit_word(out, 0x0000);
    }

    // ---- Item 3: OK button ----
    const int btn_y = status_text.empty() ? 47 : 65;
    align_to_dword(out);
    {
        DLGITEMTEMPLATE item{};
        item.style = WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                     BS_DEFPUSHBUTTON;
        item.x = 80; item.y = btn_y; item.cx = 50; item.cy = 14;
        item.id = IDOK;
        out.insert(out.end(),
                   reinterpret_cast<const uint8_t*>(&item),
                   reinterpret_cast<const uint8_t*>(&item) + sizeof(item));
        emit_word(out, 0xFFFF); emit_word(out, 0x0080);  // BUTTON
        emit_wstring(out, L"OK");
        emit_word(out, 0x0000);
    }

    // ---- Item 4: Cancel button ----
    align_to_dword(out);
    {
        DLGITEMTEMPLATE item{};
        item.style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON;
        item.x = 140; item.y = btn_y; item.cx = 50; item.cy = 14;
        item.id = IDCANCEL;
        out.insert(out.end(),
                   reinterpret_cast<const uint8_t*>(&item),
                   reinterpret_cast<const uint8_t*>(&item) + sizeof(item));
        emit_word(out, 0xFFFF); emit_word(out, 0x0080);  // BUTTON
        emit_wstring(out, L"Cancel");
        emit_word(out, 0x0000);
    }
    // Note: hdr.cdit was set to 4 above; if status_text non-empty we
    // emitted 5 items. Patch cdit to the real count.
    auto* hdr_patch = reinterpret_cast<DLGTEMPLATE*>(out.data());
    hdr_patch->cdit = static_cast<WORD>(status_text.empty() ? 4 : 5);

    return out;
}

}  // namespace detail

namespace {

struct DialogState {
    std::wstring* out_password = nullptr;  // wide → caller transcodes
    bool          accepted = false;
};

INT_PTR CALLBACK PasswordDlgProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    auto* st = reinterpret_cast<DialogState*>(
        GetWindowLongPtrW(hwnd, DWLP_USER));
    switch (msg) {
        case WM_INITDIALOG:
            SetWindowLongPtrW(hwnd, DWLP_USER, l);
            SetFocus(GetDlgItem(hwnd, 1001));  // edit
            // R12: cue banner ("Password" placeholder text) helps password
            // managers (1Password / Bitwarden) recognize the field via
            // accessible-name fallback, AND shows a hint to humans before
            // they start typing. EM_SETCUEBANNER takes a UTF-16 PCWSTR.
            SendDlgItemMessageW(hwnd, 1001, EM_SETCUEBANNER,
                                /*draw_when_focused=*/FALSE,
                                reinterpret_cast<LPARAM>(L"Password"));
            return FALSE;  // we set focus ourselves
        case WM_COMMAND: {
            const int id = LOWORD(w);
            if (id == IDOK && st) {
                wchar_t buf[256] = {};
                GetDlgItemTextW(hwnd, 1001, buf, _countof(buf));
                if (st->out_password) *st->out_password = buf;
                // Zero the on-stack buffer immediately.
                SecureZeroMemory(buf, sizeof(buf));
                st->accepted = true;
                EndDialog(hwnd, IDOK);
                return TRUE;
            }
            if (id == IDCANCEL) {
                if (st) st->accepted = false;
                EndDialog(hwnd, IDCANCEL);
                return TRUE;
            }
            break;
        }
        case WM_DESTROY:
            // Clear the edit control's text in the window itself — Windows
            // keeps the buffer alive briefly across WM_DESTROY otherwise.
            SetDlgItemTextW(hwnd, 1001, L"");
            return FALSE;
    }
    return FALSE;
}

}  // anonymous namespace

std::optional<std::string> PasswordDialog::prompt(HWND parent,
                                                  const std::wstring& basename,
                                                  const std::wstring& status_text) {
    UINT dpi = GetDpiForWindow(parent ? parent : GetDesktopWindow());
    auto tmpl = detail::build_dialog_template(basename, status_text, dpi);

    std::wstring entered_w;
    DialogState st{ &entered_w, false };
    INT_PTR r = DialogBoxIndirectParamW(
        GetModuleHandleW(nullptr),
        reinterpret_cast<DLGTEMPLATE*>(tmpl.data()),
        parent,
        &PasswordDlgProc,
        reinterpret_cast<LPARAM>(&st));
    if (r != IDOK || !st.accepted) {
        SecureZeroMemory(entered_w.data(), entered_w.size() * sizeof(wchar_t));
        return std::nullopt;
    }

    // UTF-16 → UTF-8 (small string; std::wstring_convert is deprecated but
    // okay for v1; alternative is WideCharToMultiByte directly).
    int n = WideCharToMultiByte(CP_UTF8, 0, entered_w.c_str(), -1,
                                nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? static_cast<size_t>(n - 1) : 0, '\0');
    if (n > 0) {
        WideCharToMultiByte(CP_UTF8, 0, entered_w.c_str(), -1,
                            out.data(), n, nullptr, nullptr);
    }
    SecureZeroMemory(entered_w.data(), entered_w.size() * sizeof(wchar_t));
    return out;
}

}  // namespace litepdf::ui
```

**Step 1.5: Extract retry loop into a unit-testable pure function.** The 3-attempt loop is the actual business logic worth testing — `PasswordDialog::prompt` is the I/O sink (modal, untestable headlessly) and `Document::authenticate` is a thin wrapper over MuPDF. Extract a pure function in `src/ui/password_retry.hpp` that takes both as injected dependencies:

```cpp
// src/ui/password_retry.hpp
#pragma once
#include <functional>
#include <optional>
#include <string>

namespace litepdf::ui {

// Result of a 3-attempt authentication handshake.
//   accepted = true  → at least one auth_cb returned true; password consumed.
//   accepted = false → user cancelled OR exhausted attempts.
//   attempts         → how many prompt invocations actually ran (1..3 or 0 on first cancel).
struct PasswordRetryResult {
    bool accepted  = false;
    int  attempts  = 0;
};

// Run up to 3 password prompts. prompt_cb returns std::nullopt to signal
// Cancel (user clicked Cancel button). auth_cb returns true on success.
//
// On the (n)th failure where n < 3, status_for_attempt(n) is passed to the
// next prompt_cb call as its status_text argument (caller controls wording).
//
// This function performs NO Win32 calls; it can be unit-tested with mock
// callbacks. Production wiring in MainWindow::open_tab_async injects
// PasswordDialog::prompt + Document::authenticate.
PasswordRetryResult try_authenticate_with_retry(
    std::function<std::optional<std::string>(const std::wstring& status)> prompt_cb,
    std::function<bool(const std::string&)>                                 auth_cb,
    std::function<std::wstring(int failed_count)>                           status_for_attempt);

}  // namespace litepdf::ui
```

Implementation in `src/ui/password_retry.cpp` is ~30 LOC — a `for (int i = 1; i <= 3; ++i)` loop calling `prompt_cb`, `auth_cb`, and `status_for_attempt` with `SecureZeroMemory` between failed attempts.

**Step 1.5b: Add 3 unit tests** in `tests/unit/test_password_retry.cpp`:

```cpp
TEST_CASE("password_retry: succeeds on first attempt", "[password_retry]") {
    int prompt_calls = 0;
    auto r = try_authenticate_with_retry(
        [&](auto) { ++prompt_calls; return std::optional<std::string>{"correct"}; },
        [](const auto& s)  { return s == "correct"; },
        [](int)            { return std::wstring{}; });
    REQUIRE(r.accepted);
    REQUIRE(r.attempts == 1);
    REQUIRE(prompt_calls == 1);
}

TEST_CASE("password_retry: fails, retries, succeeds on 2nd", "[password_retry]") {
    std::vector<std::wstring> statuses_seen;
    int call = 0;
    auto r = try_authenticate_with_retry(
        [&](const std::wstring& s) {
            statuses_seen.push_back(s);
            return std::optional<std::string>{++call == 1 ? "wrong" : "correct"};
        },
        [](const auto& s) { return s == "correct"; },
        [](int n)         { return L"fail-status-" + std::to_wstring(n); });
    REQUIRE(r.accepted);
    REQUIRE(r.attempts == 2);
    REQUIRE(statuses_seen.size() == 2);
    REQUIRE(statuses_seen[0].empty());          // first call: empty status
    REQUIRE(statuses_seen[1] == L"fail-status-1");  // second call: status_for_attempt(1)
}

TEST_CASE("password_retry: 3 wrong attempts -> not accepted", "[password_retry]") {
    auto r = try_authenticate_with_retry(
        [](auto) { return std::optional<std::string>{"x"}; },
        [](auto) { return false; },
        [](int)  { return L"err"; });
    REQUIRE_FALSE(r.accepted);
    REQUIRE(r.attempts == 3);
}

TEST_CASE("password_retry: cancel returns immediately, no auth call",
          "[password_retry]") {
    int auth_calls = 0;
    auto r = try_authenticate_with_retry(
        [](auto) { return std::optional<std::string>{}; },     // user cancels
        [&](const auto&) { ++auth_calls; return false; },
        [](int)  { return L"err"; });
    REQUIRE_FALSE(r.accepted);
    REQUIRE(r.attempts == 0);
    REQUIRE(auth_calls == 0);
}
```

Cumulative count update: 131 baseline + 3 (T1.2 dialog template) + 4 (T1.5b retry loop) = 138 after T1.

**Step 1.6: Wire `try_authenticate_with_retry` in `MainWindow::open_tab_async`.** When `Document::open` returns `OpenError::NeedsPassword`:

```cpp
// Inside open_tab_async, after Document::open returns NeedsPassword:
const std::wstring basename = path.filename().wstring();
auto result = ui::try_authenticate_with_retry(
    [&](const std::wstring& status_text) {
        return ui::PasswordDialog::prompt(hwnd_, basename, status_text);
    },
    [&](const std::string& pw) {
        bool ok = doc.authenticate(pw);
        // Caller-side zero — see D3. The pw string is a copy passed by const&;
        // the originating string is zeroed by the lambda above's caller.
        return ok;
    },
    [](int failed_count) {
        // R10: "remaining" reads less anxiety-inducing than "of 3" and
        // matches Acrobat / Foxit phrasing. Singular 'attempt' on the
        // last try ('1 attempt remaining') is grammatically correct.
        const int remaining = 3 - failed_count;
        if (remaining == 1) {
            return std::wstring{L"Incorrect password. (1 attempt remaining.)"};
        }
        return L"Incorrect password. ("
             + std::to_wstring(remaining) + L" attempts remaining.)";
    });
if (!result.accepted) {
    // Tab not opened. MRU not pruned per design §6.1.
    // R11: only notify user on attempt-exhaustion, NOT on Cancel.
    // Cancel = "I changed my mind" → silent close is correct UX.
    // 3 fails = "the door slammed" → user deserves to know why no tab
    // opened. Status-bar flash is the lightest-weight feedback that
    // doesn't block another tab open. If status bar isn't wired up
    // (Phase 9.x?), fall through to a single MessageBoxW with style
    // MB_OK | MB_ICONWARNING.
    if (result.attempts >= 3) {
        flash_status_bar(
            L"Failed to open " + basename
            + L": password incorrect after 3 attempts.",
            /*duration_ms=*/3000);
    }
    return;
}
// fall through to existing tab-creation path
```

If `flash_status_bar` doesn't exist yet (Phase 8 may need to add a one-line Win32 status-bar paint), wire a `MessageBoxW(hwnd_, L"...", L"LitePDF", MB_OK | MB_ICONWARNING)` instead. Either way, the user is told. Trade-off: MessageBox is modal and forces an OK click; status-bar flash is dismissable. Phase 8 picks status-bar with MessageBox fallback because the user just dismissed a modal — stacking another would feel hostile.

**Step 1.7: CMake.** Add `src/ui/PasswordDialog.cpp` and `src/ui/password_retry.cpp` to the litepdf target in top-level `CMakeLists.txt` (search for where `MainWindow.cpp` is listed and add adjacent). Add `tests/unit/test_password_dialog.cpp` AND `tests/unit/test_password_retry.cpp` to `tests/CMakeLists.txt`.

**Step 1.8: Build + tests.**

```bash
cmake --build build --config Release
ctest --test-dir build -C Release -R "password_(dialog|retry)" --output-on-failure
```

Expected: 3 [password_dialog] + 4 [password_retry] = 7/7 pass. Cumulative count: 131 → **138**.

**Step 1.9: Manual smoke.**

```bash
build/Release/litepdf.exe tests/fixtures/encrypted.pdf
# Dialog appears centered modal. Type "wrong", Enter → status updates.
# 2 more wrong → dialog closes, no tab opens.
# Re-launch, type "test" → tab opens with a page rendered.
```

**Step 1.10: Commit.**

```bash
git add src/ui/PasswordDialog.* src/ui/PasswordDialog_internal.hpp \
        src/ui/password_retry.* src/ui/MainWindow.cpp \
        CMakeLists.txt tests/unit/test_password_dialog.cpp \
        tests/unit/test_password_retry.cpp tests/CMakeLists.txt
git commit -m "feat(ui): password dialog with 3-attempt retry loop (Phase 8 Task 1, D1-D3)"
```

---

### Task 2: ePub / CBZ open verification — extend existing test file + smoke extension

**Goal:** Prove the existing `Document::open` allowlist + MuPDF auto-detection covers ePub and CBZ end-to-end. The repo **already has `tests/unit/test_document_formats.cpp` with 2 cases** (ePub-opens, CBZ-opens, both tagged `[document][formats]` — see `tests/CMakeLists.txt:30` for registration). T2 extends that file with 2 NEW cases (XPS allowlist liveness + .png reject). NO code change to `core::Document`. Title-bar check is implicit (existing `MainWindow::update_window_title` reads `path.filename()`, format-agnostic).

**Files:**
- Modify: `tests/unit/test_document_formats.cpp` — append 2 new cases (do NOT touch the existing 2).
- Modify: `scripts/smoke-test.ps1` — add a "open ePub" + "open CBZ" smoke step (see §"Smoke harness" in T2.3 for the exact pattern; the script does NOT use `Test-Step` / `Start-Litepdf` helpers — those don't exist).
- **No** change to `tests/CMakeLists.txt` (the file is already registered).

**Step 2.1: Append 2 new cases to `tests/unit/test_document_formats.cpp`.**

The existing 2 cases are:
```cpp
TEST_CASE("ePub opens and has pages", "[document][formats]") { ... }
TEST_CASE("CBZ opens and has pages", "[document][formats]") { ... }
```

Append, matching the existing file's tag style (`[document][formats]`, NOT the earlier draft's `[document][format][xxx]`):

```cpp
TEST_CASE("XPS extension stays in allowlist (no fixture, FileNotFound expected)",
          "[document][formats]") {
    // No XPS fixture in v1 (D5); the allowlist code path is asserted by
    // attempting a non-existent .xps and observing FileNotFound, NOT
    // UnsupportedFormat. If the allowlist drops .xps in a future refactor,
    // this becomes UnsupportedFormat and this test fires.
    Document d;
    auto err = d.open("tests/fixtures/does-not-exist.xps");
    REQUIRE(err.has_value());
    REQUIRE(*err == Document::OpenError::FileNotFound);
}

TEST_CASE("Document rejects non-allowlisted extension (sample.png)",
          "[document][formats]") {
    // sample.png exists in fixtures as a deliberate negative case.
    Document d;
    auto err = d.open("tests/fixtures/sample.png");
    REQUIRE(err.has_value());
    REQUIRE(*err == Document::OpenError::UnsupportedFormat);
}
```

**Step 2.2: Run; expect 4 cases pass (2 existing + 2 new) under `[document][formats]`.**

Cumulative count: 138 → **140** (+2 from T2's two NEW cases — anchored to 138 from end of T1).

**Step 2.3: Smoke-test extension (`scripts/smoke-test.ps1`).** After the existing simple-PDF smoke block, add two more launch-and-poll blocks for `sample.epub` and `sample.cbz`. Mirror the existing `Start-Process` + `MainWindowHandle` poll pattern; do NOT require timing-line output (only PDFs hit `--log-timings` path).

**Step 2.4: Manual smoke.**

```bash
build/Release/litepdf.exe tests/fixtures/sample.epub
# Window opens, page 0 renders (text page).

build/Release/litepdf.exe tests/fixtures/sample.cbz
# Window opens, page 0 renders (image page).
```

**Step 2.5: Commit.**

```bash
git add tests/unit/test_document_formats.cpp scripts/smoke-test.ps1
git commit -m "test(core): extend document-formats coverage with XPS allowlist + reject (Phase 8 Task 2, D4-D6)"
```

(No `tests/CMakeLists.txt` in the diff — the file was already registered there at `tests/CMakeLists.txt:30`.)

---

### Task 3: Dark mode — invert pixmap at render boundary + cache key + chrome flip

**Goal:** Add a per-tab `invert_colors_` flag. When set, `RenderEngine`'s worker calls `fz_invert_pixmap` immediately after the rasterize call (`fz_new_pixmap_from_display_list` at `RenderEngine.cpp:189`). `PageCache` keys L1 (pixmap) by `(page, scale, invert)`; L2 (display list) is unchanged. Toggle accelerator handler in `MainWindow` flips the flag, invalidates current renders, kicks a re-render, repaints chrome via the existing `WM_SETTINGCHANGE` route.

**Files:**
- Modify: `src/core/RenderEngine.hpp` — add `bool invert = false` to `RenderRequest` (after `bypass_cache`).
- Modify: `src/core/RenderEngine.cpp` — after the rasterize at line 189 (`fz_new_pixmap_from_display_list`) and BEFORE the cache write at line 208, call `fz_invert_pixmap(ctx, pix)` when `req.invert`. ~3 LOC.
- Modify: `src/core/PageCache.{hpp,cpp}` — add `bool invert` to the key tuple; rewrite hash + comparator. ~25 LOC.
- Modify: `src/core/DocumentView.{hpp,cpp}` — add `bool invert_colors_` getter/setter; auto-pipe into `request_render`. ~20 LOC.
- Modify: `src/ui/MainWindow.cpp` — `IDM_VIEW_INVERT` handler, `WM_INITMENUPOPUP` for menu checkmark, route to `PdfCanvas` for chrome refresh.
- Modify: `src/ui/PdfCanvas.cpp` — accept invert flag; flip chrome palette + invalidate (chrome flip mirrors TabManager pattern).
- Modify: `tests/unit/test_page_cache.cpp` (or whichever holds existing PageCache tests) — add an `[page_cache][invert]` case.

**Step 3.1: Engine + cache changes.**

```cpp
// RenderEngine.hpp RenderRequest:
struct RenderRequest {
    int   page_num     = 0;
    int   priority     = 0;
    float scale        = 1.0f;
    std::function<void(fz_pixmap*, fz_context*)> on_complete;
    bool  bypass_cache = false;
    bool  invert       = false;  // (Phase 8) channel-invert pixmap
                                 // post-render. Cache key includes this
                                 // axis; D7+D8.
};
```

In `RenderEngine.cpp`, after the rasterize call at line 189 (`fz_new_pixmap_from_display_list`) and BEFORE the L1 cache-write at line 208, insert:

```cpp
// (Phase 8 D7) Invert AFTER rasterize, BEFORE caching, so the cached
// L1 pixmap matches the request's invert polarity. The display list
// (L2) stays polarity-independent — only L1 carries the third axis.
if (pix && req.invert) {
    fz_invert_pixmap(ctx, pix);
}
```

Note: the codebase does NOT call `fz_run_page` directly — rendering goes through the L2 display-list path (`fz_new_display_list_from_page` → `fz_new_pixmap_from_display_list`). Insert AFTER the rasterize, NOT after `fz_run_page` (which doesn't appear in our render hot path at all).

In `PageCache.hpp`, expand **only the L1 pixmap key** struct from `{int page; float scale;}` to `{int page; float scale; bool invert;}`. The L2 display-list key stays `{int page}` — see D8 rationale (display lists are polarity-independent; keying L2 by invert duplicates entries and loses the display-list build savings on toggle). Update `std::hash` specialization or wherever `unordered_map<L1Key, ...>` is keyed; leave the L2 map untouched. Add a comment in `PageCache.hpp` next to the L2 declaration: "// L2 is keyed by page only — display lists are polarity-independent (D8)."

**Cache touch points (re-anchored at plan-write to current `origin/main` HEAD `c9f3a7d` of `src/core/RenderEngine.cpp`):**

| Site | Layer | Op | Phase 8 change |
|---|---|---|---|
| L121 | L1 | read pixmap | **add `req.invert` to lookup key** |
| L139 | L2 | read display list | unchanged (no invert axis) |
| L167 | L2 | write display list | unchanged |
| L208 | L1 | write pixmap | **add `req.invert` to insertion key** |

(Phase 7 D18 audit listed line numbers L113/L129/L155/L192 — those drifted ~8-16 lines due to commits between Phase 7 ship and Phase 8 plan-write. Re-grep before editing: `grep -n "impl->cache" src/core/RenderEngine.cpp` should produce 4 hits matching the table above.)

If the implementer touches L139 or L167 to add invert, that's a sign they've misread D8 — stop and re-read.

**Step 3.2: PageCache test.**

```cpp
TEST_CASE("PageCache: invert axis is part of the key",
          "[page_cache][invert]") {
    PageCache cache(/*l1=*/4, /*l2=*/4, /*ctx=*/nullptr);
    // Create two distinct fz_pixmap stand-ins (manual minimal pixmap if
    // PageCache supports that, else use ctx-cloned ones).
    // Assert get_pixmap(0, 1.0f, /*invert=*/false) and
    //        get_pixmap(0, 1.0f, /*invert=*/true) return distinct entries.
    // Implementation detail: if PageCache::get_pixmap currently doesn't
    // take invert, the test below will not compile until T3 lands.
    auto* p1 = cache.put_pixmap(0, 1.0f, /*invert=*/false, /*pix=*/nullptr_or_valid);
    auto* p2 = cache.put_pixmap(0, 1.0f, /*invert=*/true,  /*pix=*/nullptr_or_valid);
    REQUIRE(cache.get_pixmap(0, 1.0f, /*invert=*/false) ==
            cache.get_pixmap(0, 1.0f, /*invert=*/false));
    REQUIRE(cache.get_pixmap(0, 1.0f, /*invert=*/true) !=
            cache.get_pixmap(0, 1.0f, /*invert=*/false));
}
```

(Implementer: read existing `tests/unit/test_page_cache*.cpp` to match its constructor + helper conventions; the snippet above is illustrative.)

**Step 3.3: DocumentView wiring.**

```cpp
// DocumentView.hpp additions
bool invert_colors() const noexcept;
void set_invert_colors(bool on);  // resubmits last render at new invert
```

`DocumentView::request_render` (and `request_render_with_prefetch`) propagate `invert_colors_` to the `RenderRequest::invert` field. `set_invert_colors` cancels stale renders (`cancel_stale_renders(0)`) so the visible page re-renders at the new key — the L1 entry for the previous polarity is preserved (that's the point of the third axis).

**Step 3.4: MainWindow handler + chrome flip.**

```cpp
case IDM_VIEW_INVERT: {
    auto* v = active_view();
    if (!v) return 0;
    v->set_invert_colors(!v->invert_colors());
    if (canvas_) canvas_->set_invert_chrome(v->invert_colors());  // T3 PdfCanvas API
    kick_render(v->current_page());
    return 0;
}
```

`PdfCanvas::set_invert_chrome(bool)` flips the canvas's local `Palette` between light + dark variants and `InvalidateRect`s. The MainWindow chrome (frame borders, scrollbar fallback) already responds to system dark mode via the existing `WM_SETTINGCHANGE` route — Phase 8 does NOT force the system theme, only the in-app palette for the canvas and find-bar accent. Trade-off: tab-strip and outline pane still follow system theme; if user is on Light system theme but Invert is on, the tabs stay light. This is acceptable for v1 (matches Acrobat's similar "page invert only" UX); Phase 11 polish may extend.

`WM_INITMENUPOPUP` for the View popup: if active view's `invert_colors()` is true, set `MF_CHECKED` on `IDM_VIEW_INVERT`, else `MF_UNCHECKED`. Mirrors the existing thumbnail-pane menu-state pattern (T8 Phase 7).

**Step 3.5: RenderEngine integration test for actual pixel invert.** Add `tests/unit/test_render_engine_invert.cpp` (~80 LOC) that submits two `RenderRequest`s for the same page at the same scale — one with `invert=false`, one with `invert=true` — and asserts that:

1. Both `on_complete` callbacks fire with non-null pixmaps.
2. A pixel known to be near-white in the source PDF (read from `tests/fixtures/simple.pdf`'s top-left margin via `fz_pixmap_samples`) is near-white when `invert=false` and near-black when `invert=true`.
3. A second submission with `invert=true` hits the L1 cache (the test asserts via a custom `[render_engine][cache_hit]` counter or by timing — see existing `test_render_engine_cache.cpp` for the pattern; the existing helper already exposes a `cache_hit_count()` accessor).

Skeleton (signatures verified against current `c9f3a7d` HEAD via codex API surface check):

```cpp
// tests/unit/test_render_engine_invert.cpp
#include "core/Document.hpp"
#include "core/PageCache.hpp"
#include "core/RenderEngine.hpp"
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <thread>

extern "C" {
#include <mupdf/fitz.h>
}

using namespace litepdf::core;

TEST_CASE("RenderEngine invert flag actually inverts pixmap pixels",
          "[render_engine][invert]") {
    Document d;
    REQUIRE_FALSE(d.open("tests/fixtures/simple.pdf").has_value());

    // Document owns its fz_context; clone for the test thread that calls
    // fz_pixmap_* helpers + drops kept pixmaps. RenderEngine clones its
    // own per-worker contexts internally.
    fz_context* test_ctx = d.clone_context();
    REQUIRE(test_ctx != nullptr);

    PageCache cache(/*l1=*/4, /*l2=*/4, test_ctx);
    RenderEngine eng(d, /*num_workers=*/1, &cache);

    fz_pixmap* pix_normal = nullptr;
    fz_pixmap* pix_invert = nullptr;
    std::atomic<int> done{0};

    // RenderRequest aggregate-init: {page, priority, scale, on_complete,
    // bypass_cache, invert}. Phase 8 adds `invert` after `bypass_cache`.
    eng.submit({0, /*priority=*/0, 1.0f,
                [&](fz_pixmap* p, fz_context* worker_ctx) {
                    if (p) pix_normal = fz_keep_pixmap(worker_ctx, p);
                    ++done;
                },
                /*bypass_cache=*/false, /*invert=*/false});
    eng.submit({0, /*priority=*/1, 1.0f,
                [&](fz_pixmap* p, fz_context* worker_ctx) {
                    if (p) pix_invert = fz_keep_pixmap(worker_ctx, p);
                    ++done;
                },
                /*bypass_cache=*/false, /*invert=*/true});

    while (done.load() < 2) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(pix_normal != nullptr);
    REQUIRE(pix_invert != nullptr);

    // simple.pdf top-left is white margin. BGRA stride; sample pixel (5,5).
    auto* sn = fz_pixmap_samples(test_ctx, pix_normal);
    auto* si = fz_pixmap_samples(test_ctx, pix_invert);
    const int stride = fz_pixmap_stride(test_ctx, pix_normal);
    const int idx = 5 * stride + 5 * 4;
    REQUIRE(sn[idx + 2] >= 240);   // R near 255 (white margin)
    REQUIRE(si[idx + 2] <= 15);    // R near 0 (inverted)

    fz_drop_pixmap(test_ctx, pix_normal);
    fz_drop_pixmap(test_ctx, pix_invert);
    fz_drop_context(test_ctx);
}

TEST_CASE("RenderEngine invert: same-polarity resubmit hits L1, opposite-polarity misses",
          "[render_engine][invert][cache]") {
    // PageCache exposes l1_size() / l2_size() (NOT l1_hit_count() — that
    // accessor doesn't exist). Verify by pointer identity instead: a cache
    // hit returns the SAME fz_pixmap* the first request stored. Mirror
    // tests/unit/test_render_engine_cache.cpp's existing identity-check
    // pattern.
    Document d;
    REQUIRE_FALSE(d.open("tests/fixtures/simple.pdf").has_value());

    fz_context* test_ctx = d.clone_context();
    PageCache cache(/*l1=*/4, /*l2=*/4, test_ctx);
    RenderEngine eng(d, /*num_workers=*/1, &cache);

    fz_pixmap* p_first  = nullptr;
    fz_pixmap* p_second = nullptr;
    fz_pixmap* p_invert = nullptr;
    std::atomic<int> done{0};

    auto submit_invert = [&](bool invert, fz_pixmap*& out) {
        eng.submit({0, /*priority=*/0, 1.0f,
                    [&](fz_pixmap* p, fz_context* worker_ctx) {
                        if (p) out = fz_keep_pixmap(worker_ctx, p);
                        ++done;
                    },
                    /*bypass_cache=*/false, invert});
    };
    submit_invert(false, p_first);
    while (done.load() < 1) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    submit_invert(false, p_second);  // same polarity → L1 hit
    submit_invert(true,  p_invert);  // opposite polarity → L1 miss
    while (done.load() < 3) std::this_thread::sleep_for(std::chrono::milliseconds(10));

    REQUIRE(p_first  != nullptr);
    REQUIRE(p_second != nullptr);
    REQUIRE(p_invert != nullptr);
    REQUIRE(p_first  == p_second);   // identity → cached
    REQUIRE(p_first  != p_invert);   // different polarity → distinct entry
    REQUIRE(cache.l1_size() == 2);    // two distinct (page=0,scale=1.0) slots

    fz_drop_pixmap(test_ctx, p_first);
    fz_drop_pixmap(test_ctx, p_second);
    fz_drop_pixmap(test_ctx, p_invert);
    fz_drop_context(test_ctx);
}
```

Verify against actual repo before editing: `grep -n "Document::clone_context\|RenderEngine::RenderEngine\|PageCache::l1_size" src/core/*.{hpp,cpp}`. The plan's earlier draft used `d.context()` and a different `RenderEngine` constructor signature — both were wrong; the version above is API-verified.

**Step 3.6: Build + tests.** Cumulative count: 140 → **143** (+1 [page_cache][invert] from T3.2 + 2 [render_engine][invert] from T3.5).

**Step 3.7: Manual smoke.**

```bash
build/Release/litepdf.exe tests/fixtures/simple.pdf
# Ctrl+Shift+I → page renders white-on-black; canvas background flips
#                to dark gray; menu shows ✓ next to "Invert Colors".
# Ctrl+Shift+I again → reverts.
# Open a 2nd tab with simple.pdf, Ctrl+Shift+I → ONLY 2nd tab inverts;
# switch back to 1st tab → 1st tab still light. (D9 per-tab.)
```

**Step 3.8: Commit.**

```bash
git add src/core/RenderEngine.* src/core/PageCache.* src/core/DocumentView.* \
        src/ui/MainWindow.cpp src/ui/PdfCanvas.* tests/unit/test_page_cache*.cpp
git commit -m "feat(ui): invert-colors per tab, cache 3rd axis (Phase 8 Task 3, D7-D9)"
```

---

### Task 4: Two-page spread layout

**Goal:** Add a per-tab `dual_page_` flag. When set, `PdfCanvas` lays out two pages side-by-side; PgUp/PgDn step by 2; render submits two pages per nav. Cover (page 0) renders alone. Hit-overlay + thumb-pane current-page sync still work via existing observer chain.

**Files:**
- Modify: `src/core/DocumentView.{hpp,cpp}` — add `bool dual_page_` getter/setter (~10 LOC).
- Modify: `src/ui/PdfCanvas.{hpp,cpp}` — layout split (~120 LOC, mostly inside `on_paint` + `on_key_down` + a new `dual_page_compute_left()` helper).
- Modify: `src/ui/MainWindow.cpp` — `IDM_VIEW_DUAL_PAGE` handler + menu checkmark.
- Create: `tests/unit/test_dual_page_layout.cpp` (~50 LOC) — pure-logic tests for `dual_page_compute_left(page, page_count)` cover-rule arithmetic.
- Modify: `tests/CMakeLists.txt`.

**Step 4.1: Pure-logic helper.** Extract the cover-rule into a free function in a small header `src/ui/PdfCanvasLayout.hpp` (or inside an anonymous namespace if too tiny — but extracting allows TDD):

```cpp
// Returns the "left page" of the page-pair containing `page`.
// Cover (page 0) is always alone, so dual_page_compute_left(0, n) == 0.
// Pairs from page 1 onward: (1,2), (3,4), ...
// dual_page_compute_left(5, 10) == 5; (3, 10) == 3; (4, 10) == 3.
inline int dual_page_compute_left(int page, int page_count) noexcept {
    if (page <= 0 || page_count <= 0) return 0;
    if (page == 0) return 0;
    return ((page - 1) & ~1) + 1;  // round odd to itself, even to odd-1
}

// Returns the "right page" or -1 if out of range / cover-page-alone.
inline int dual_page_compute_right(int left_page, int page_count) noexcept {
    if (left_page == 0)              return -1;          // cover alone
    if (left_page + 1 >= page_count) return -1;          // odd tail
    return left_page + 1;
}
```

Tests:

```cpp
TEST_CASE("dual_page_compute_left: cover page 0 is alone", "[dual_page]") {
    REQUIRE(dual_page_compute_left(0, 10) == 0);
    REQUIRE(dual_page_compute_right(0, 10) == -1);
}
TEST_CASE("dual_page_compute_left: pairs from 1 onward", "[dual_page]") {
    REQUIRE(dual_page_compute_left(1, 10) == 1);
    REQUIRE(dual_page_compute_left(2, 10) == 1);
    REQUIRE(dual_page_compute_left(3, 10) == 3);
    REQUIRE(dual_page_compute_left(4, 10) == 3);
    REQUIRE(dual_page_compute_right(1, 10) == 2);
    REQUIRE(dual_page_compute_right(3, 10) == 4);
}
TEST_CASE("dual_page: odd-tail last page renders alone", "[dual_page]") {
    // 9-page doc: pairs (0,-) (1,2) (3,4) (5,6) (7,8) — last is 7-8, not 8 alone
    REQUIRE(dual_page_compute_left(8, 9) == 7);
    REQUIRE(dual_page_compute_right(7, 9) == 8);
    // 10-page doc: pairs (0,-) (1,2) (3,4) (5,6) (7,8) (9, alone)
    REQUIRE(dual_page_compute_left(9, 10) == 9);
    REQUIRE(dual_page_compute_right(9, 10) == -1);
}
```

(Edge: with the `((page - 1) & ~1) + 1` formula, `dual_page_compute_left(2, n) == 1`, `(3, n) == 3`, `(4, n) == 3`, `(5, n) == 5` — this satisfies pairs (1,2)(3,4)(5,6)(7,8)(9,? based on doc length). Verify against the test cases above before implementing. Tests catch off-by-one.)

**Step 4.2: PdfCanvas layout.** In `on_paint`, when `dual_page_` is on:

1. Compute `left = dual_page_compute_left(view->current_page(), view->page_count())`.
2. Compute `right = dual_page_compute_right(left, view->page_count())`.
3. Available width = `client_w - 2*margin - gutter` (8 dip gutter); height = `client_h - 2*margin`.
4. Each page gets `(client_w - gutter)/2` width budget. Maintain aspect ratio per page (left + right may have different aspect ratios — fit each to its own slot).
5. Submit render: call `view->request_render(left, on_left_done)` and, if `right >= 0`, `view->request_render(right, on_right_done)`. The actual `DocumentView::request_render` signature is `(int page, RenderCb on_complete)` — no priority parameter; priorities are an internal implementation detail of DocumentView's `RenderEngine` plumbing. T4 does NOT need to thread priorities; the engine's FIFO + Phase 7 priority-by-recency-of-submit suffices for the 2-page dual case (the second submit naturally trails the first by ~50 µs and renders second). If the implementer finds dual-mode produces visible right-before-left blits in the wild, **escalate** rather than reaching into the engine — the right fix is a `request_render_pair(left, right, ...)` helper, not per-call priority arguments.
6. Blit each rendered bitmap into its slot.

Single-page (cover) path stays unchanged when `right == -1` for **rendering** (only the left slot gets a pixmap), BUT the layout still allocates BOTH slots and paints the right slot as a "(blank)" placeholder — a 1-DIP gray border with a centered "(blank)" or no text, matching Acrobat's spread mode for cover pages. **R15:** without the placeholder, first-time `Ctrl+Shift+D` press while on page 0 looks suspiciously like "nothing happened" because the visible result is a smaller centered page (same content, just half-width). The empty-right-slot placeholder is the visual cue that says "yes, you toggled spread mode; this is page 0 alone because of the cover convention". Implementation: in `on_paint`, when `right == -1` and `dual_page_` is on, draw a `D2D1_RECT_F` outline at the right-slot bounds with `palette.placeholder_border` and (optionally) skip the centered text — a clean empty rect reads better than placeholder text on a busy doc. The existing pan/scroll math operates on the canvas-relative `(x, y)` and is scoped per-slot for hit-test; horizontal scroll is disabled in dual mode (vertical only).

**Step 4.2.5: ZoomMode interaction with dual page.** Per R5, `ZoomMode::FitPage` and `ZoomMode::FitWidth` in dual mode must fit each of the two pages into a half-canvas-width slot, NOT into the whole canvas width. The actual zoom enum is `litepdf::core::DocumentView::ZoomMode` with values `FitWidth / FitPage / Custom` (`DocumentView.hpp:57`). The fit-zoom math currently lives inside `DocumentView::set_zoom_mode` (declared at `DocumentView.hpp:94`) — read it before editing.

The change is conceptually:

```cpp
// Inside DocumentView::set_zoom_mode (or wherever the fit math lives —
// signatures below are conceptual; locate the real ones before editing):
//   float canvas_w = canvas client width in DIP;
//   float canvas_h = canvas client height in DIP;
//   float page_w   = current page width in DIP;
//   float page_h   = current page height in DIP;
//
// Phase 8 (R5): in dual-page mode, FitWidth / FitPage operate on a
// half-canvas-width slot (minus the 8 dip gutter), not the whole canvas
// width. Cover page (page 0) is the exception — it renders alone, so it
// gets the full client width like single-page mode.
const bool is_cover = (page == 0);
const float effective_canvas_w =
    (dual_page_ && !is_cover) ? (canvas_w - 8.0f) * 0.5f : canvas_w;

switch (mode) {
    case ZoomMode::FitWidth:
        scale_ = effective_canvas_w / page_w;
        break;
    case ZoomMode::FitPage:
        scale_ = std::min(effective_canvas_w / page_w, canvas_h / page_h);
        break;
    case ZoomMode::Custom:
        // unchanged
        break;
}
```

The exact field / accessor names (`canvas_w`, `page_w`, etc.) MUST be read out of `DocumentView.cpp` before the edit — the names above are illustrative. When `set_dual_page` flips, the next `set_zoom_mode(FitWidth, ...)` or `set_zoom_mode(FitPage, ...)` call from `PdfCanvas::on_layout` picks up the new branch automatically.

**Step 4.3: Page nav handlers.** In `on_key_down`:

```cpp
case VK_NEXT: {  // PgDn
    int step = view_->dual_page() ? 2 : 1;
    int next = std::min(view_->current_page() + step, view_->page_count() - 1);
    if (view_->dual_page()) {
        // Snap to the LEFT page of the next pair.
        next = dual_page_compute_left(next, view_->page_count());
    }
    if (change_current_page(next)) {
        kick_render_or_repaint();
    } else {
        // R16: dead-key feedback. PgDn at the last page produced silent
        // no-op pre-Phase-8; users notice "nothing happened" and try
        // again. Status-bar flash for 800 ms tells them they're at the
        // boundary without modal-clicking. Same fallback as R11: if
        // status bar isn't wired, MessageBeep(MB_OK) is good enough.
        flash_status_bar(L"Already at last page.", /*duration_ms=*/800);
    }
    break;
}
case VK_PRIOR: {  // PgUp
    int step = view_->dual_page() ? 2 : 1;
    int prev = std::max(view_->current_page() - step, 0);
    if (view_->dual_page()) prev = dual_page_compute_left(prev, view_->page_count());
    if (change_current_page(prev)) {
        kick_render_or_repaint();
    } else {
        flash_status_bar(L"Already at first page.", /*duration_ms=*/800);
    }
    break;
}
```

Mouse-wheel: unchanged (already operates per-pixel pan).

**Step 4.4: MainWindow handler + menu checkmark.**

```cpp
case IDM_VIEW_DUAL_PAGE: {
    auto* v = active_view();
    if (!v) return 0;
    v->set_dual_page(!v->dual_page());
    if (canvas_) canvas_->set_dual_page(v->dual_page());
    // Snap to the left page of the current pair for clean repaint.
    int p = v->current_page();
    if (v->dual_page()) p = dual_page_compute_left(p, v->page_count());
    canvas_->change_current_page(p);
    kick_render(p);
    return 0;
}
```

Mirror in `WM_INITMENUPOPUP`: `MF_CHECKED` if `dual_page()` is on. **R19 — checkmark style consistency:** verify against the existing `IDM_VIEW_OUTLINE` (F5) and `IDM_VIEW_THUMBS` (F4) handler in `WM_INITMENUPOPUP` — those should already be using `MF_CHECKED / MF_UNCHECKED` via `CheckMenuItem`. Use the SAME pattern for `IDM_VIEW_INVERT` and `IDM_VIEW_DUAL_PAGE`, including the menu-handle-fetch call and the `mf` flag wording, so all four view-toggle entries render the same in the menu (radio-style is wrong; checkmark is the convention). If F4/F5 are NOT currently using `CheckMenuItem` (i.e., they're permanently uncheckmarked because the v0.0.7/v0.0.8 plan never wired it), Phase 8 should retrofit those two as part of T3.4/T4.4 — listed as out-of-scope cleanup in §"Out of scope" if too invasive, otherwise included.

**Step 4.5: Thumbnail-pane sync (D11).** No code change needed — the existing page-changed observer (Phase 7 T7) already calls `thumb_pane_->set_current_page(p)`. With `dual_page_` on, `current_page` IS the left page of the pair (per the snap in Step 4.4), so the thumb highlight naturally lands on the left tile. Right-tile highlight is intentionally NOT painted; documented in §"Known Limitations".

**Step 4.6: Search hit-jump (D12).** No render-path code change. `PdfCanvas::scroll_into_view(hit)` ends with `change_current_page(hit.page)`; the `IDM_VIEW_DUAL_PAGE` handler's snap path runs naturally on the next nav since dual-page is active before scroll-into-view fires. R17 (FindBar slot tag in dual mode) is deferred — see §"Out of scope".

**Step 4.7: Build + tests.** Cumulative count: 143 → **146** (+3 [dual_page]).

**Step 4.8: Manual smoke.**

```bash
build/Release/litepdf.exe tests/fixtures/bookmarks.pdf
# Ctrl+Shift+D → spread mode; page 0 renders alone, 1+2 next.
# PgDn → moves to 1+2 spread. PgDn again → 3+? (bookmarks.pdf is 3 pages).
# Ctrl+Shift+D again → back to single. Page 1 (the LEFT of the last spread)
#                       stays current.
# Open a longer doc (search.pdf is 5 Lorem-ipsum pages):
# Ctrl+Shift+D → 0 alone, then PgDn → 1+2, PgDn → 3+4, PgDn → 4 stays
#                (clamped). PgUp → 1+2.
# F4 with dual on → thumb pane shows single column; current-page
#                   border on the LEFT page of current pair.
```

**Step 4.9: Commit.**

```bash
git add src/core/DocumentView.* src/ui/PdfCanvas.* src/ui/PdfCanvasLayout.hpp \
        src/ui/MainWindow.cpp tests/unit/test_dual_page_layout.cpp tests/CMakeLists.txt
git commit -m "feat(ui): two-page spread layout (Phase 8 Task 4, D10-D12)"
```

---

### Task 5: Smoke test sweep + version bump + CHANGELOG note + design-doc sync + tag

**Goal:** Add password-flow + invert + dual-page smoke checks. Bump `VERSION` to `0.0.9-dev`, About dialog literal to `v0.0.9`, `litepdf.rc` VERSIONINFO block to `0,0,9,0`. Sync `docs/plans/2026-04-15-litepdf-design.md` §3 (Tier 3) checklist + §4.1 row 8 (`core/Document` LOC bump from password dialog wiring). Tag `v0.0.9-phase8`.

**Files:**
- Modify: `scripts/smoke-test.ps1`.
- Modify: `VERSION` (`0.0.8-dev` → `0.0.9-dev`).
- Modify: `src/ui/MainWindow.cpp` (About dialog `v0.0.8` → `v0.0.9`).
- Modify: `resources/litepdf.rc` VERSIONINFO (`0,0,6,0` is the current literal — Phase 5/6/7 didn't bump it; Phase 8 jumps directly to `0,0,9,0` and same for `FileVersion` / `ProductVersion` strings).
- Modify: `docs/plans/2026-04-15-litepdf-design.md` §3 (Tier 3 list; mark dark mode + dual-page as "shipped Phase 8").
- Modify: `CHANGELOG.md` (if exists; if not, skip — Phase 7 didn't introduce one).

**Step 5.1: Smoke-test additions** to `scripts/smoke-test.ps1` after the Phase 7 thumb step.

The current `smoke-test.ps1` does NOT use `Test-Step` / `Start-Litepdf` helper functions — earlier plan drafts pointed to those by mistake. The actual pattern is `Start-Process` with `-PassThru` + `MainWindowHandle` polling (see lines 38-60 of the existing file for the canonical sequence). Append, mirroring that pattern:

```powershell
# Phase 8: ePub open. Mirrors the simple.pdf launch pattern at the top of
# this file — Start-Process with -PassThru, poll for MainWindowHandle up
# to 5 s, fail if the process exits early or never shows a window.
Write-Host "----"
Write-Host "Launching ePub: $exe tests/fixtures/sample.epub"
$proc_epub = Start-Process -FilePath $exe `
    -ArgumentList @("tests/fixtures/sample.epub") `
    -PassThru -NoNewWindow
$deadline = (Get-Date).AddSeconds(5)
while ((Get-Date) -lt $deadline -and $proc_epub.MainWindowHandle -eq [IntPtr]::Zero) {
    Start-Sleep -Milliseconds 100
    if ($proc_epub.HasExited) {
        throw "ePub launch crashed (exit=$($proc_epub.ExitCode))"
    }
    $proc_epub.Refresh()
}
if ($proc_epub.MainWindowHandle -eq [IntPtr]::Zero) {
    Stop-Process -Id $proc_epub.Id -Force -ErrorAction SilentlyContinue
    throw "ePub never showed a window in 5 s"
}
Write-Host "[OK] ePub window shown"
Stop-Process -Id $proc_epub.Id -Force -ErrorAction SilentlyContinue

# Phase 8: CBZ open. Same pattern.
# (factor out into a New-LitepdfSmokeLaunch helper if a third copy looms)
Write-Host "----"
Write-Host "Launching CBZ: $exe tests/fixtures/sample.cbz"
$proc_cbz = Start-Process -FilePath $exe `
    -ArgumentList @("tests/fixtures/sample.cbz") `
    -PassThru -NoNewWindow
$deadline = (Get-Date).AddSeconds(5)
while ((Get-Date) -lt $deadline -and $proc_cbz.MainWindowHandle -eq [IntPtr]::Zero) {
    Start-Sleep -Milliseconds 100
    if ($proc_cbz.HasExited) {
        throw "CBZ launch crashed (exit=$($proc_cbz.ExitCode))"
    }
    $proc_cbz.Refresh()
}
if ($proc_cbz.MainWindowHandle -eq [IntPtr]::Zero) {
    Stop-Process -Id $proc_cbz.Id -Force -ErrorAction SilentlyContinue
    throw "CBZ never showed a window in 5 s"
}
Write-Host "[OK] CBZ window shown"
Stop-Process -Id $proc_cbz.Id -Force -ErrorAction SilentlyContinue

# Phase 8: Encrypted PDF triggers password dialog. Cannot drive the modal
# headlessly; just assert the process is alive 1 s post-launch — a
# crashed-on-encrypted regression would exit early.
Write-Host "----"
Write-Host "Launching encrypted PDF (modal expected): $exe tests/fixtures/encrypted.pdf"
$proc_enc = Start-Process -FilePath $exe `
    -ArgumentList @("tests/fixtures/encrypted.pdf") `
    -PassThru -NoNewWindow
Start-Sleep -Seconds 1
if ($proc_enc.HasExited) {
    throw "encrypted.pdf launch crashed (exit=$($proc_enc.ExitCode))"
}
Write-Host "[OK] encrypted PDF process alive after 1 s (modal blocking on user input)"
Stop-Process -Id $proc_enc.Id -Force -ErrorAction SilentlyContinue
```

If the same launch-and-poll boilerplate appears 3 times, the implementer SHOULD extract `function Start-LitepdfSmoke($file)` at the top of the file. T5.1 leaves that DRY pass to the implementer.

**Step 5.2: Bump versions.**

```bash
echo 0.0.9-dev > VERSION
# Edit MainWindow.cpp:936 "v0.0.8" -> "v0.0.9".
# Edit litepdf.rc lines 5-6: FILEVERSION 0,0,9,0 / PRODUCTVERSION 0,0,9,0.
# Edit litepdf.rc lines 19,24: "FileVersion" "0.0.9.0" / "ProductVersion" "0.0.9.0".
```

**Step 5.3: Design-doc sync.** In `docs/plans/2026-04-15-litepdf-design.md` §3, append after each Tier 3 line an annotation matching Phase 7's row-7 update style. Example: "Encrypted PDF: password prompt (max 3 attempts per session, password never persisted) — *shipped Phase 8*."

**Step 5.4: Build + full ctest.**

```bash
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Expected: **146/146** when starting from the 131 baseline (post-Phase-7-polish-PR #5 merged). If Phase 8 starts before PR #5 lands, baseline is 130 and expected is **145/145**. Verify the actual baseline with `grep -rE '^TEST_CASE' tests/unit/ | wc -l` at T0 and pick the matching column in the table below.

Test count derivation:

| Stage | Cases added | Cumulative (130 baseline) | Cumulative (131 baseline if `phase-7-polish` merged) |
|---|---|---|---|
| baseline | — | **130** | **131** |
| T1.2: 3 [password_dialog] | +3 | 133 | 134 |
| T1.5b: 4 [password_retry] | +4 | 137 | 138 |
| T2: 2 [document][formats] (NEW only — file already has 2 cases) | +2 | 139 | 140 |
| T3.2: 1 [page_cache][invert] | +1 | 140 | 141 |
| T3.5: 2 [render_engine][invert] | +2 | 142 | 143 |
| T4: 3 [dual_page] | +3 | **145** | **146** |

Per-step "Cumulative count:" lines throughout T1-T4 use the 131 baseline (Phase 7 polish PR #5 merged). If starting before PR #5 lands, subtract 1 from each cumulative figure. Use `grep -rE '^TEST_CASE' tests/unit/ | wc -l` at T0 to determine the actual baseline and pick the matching column above.

**Step 5.5: Commit + tag.**

```bash
git add VERSION resources/litepdf.rc src/ui/MainWindow.cpp scripts/smoke-test.ps1 \
        docs/plans/2026-04-15-litepdf-design.md
git commit -m "chore(ui): bump to v0.0.9 + smoke + design sync (Phase 8 finalize)"
git tag v0.0.9-phase8
```

**Step 5.6: Open PR per `superpowers:finishing-a-development-branch` or `/ship` (pick ONE per CLAUDE.md rule 1).** Push branch + tag, open PR, merge with rebase strategy (per Phase 5/6/7 precedent).

---

## Smoke test (manual, all features)

Run on a clean build, in order:

1. **Encrypted PDF**: `litepdf.exe tests/fixtures/encrypted.pdf` → dialog appears modal, type "wrong" → status updates "Incorrect password (2 of 3)". Type "wrong" → "(3 of 3)". Type "wrong" → dialog closes; no tab. Re-launch, type "test" → tab opens, page renders. **Pass criterion: matches D2 + D3.**
2. **ePub fixture**: `litepdf.exe tests/fixtures/sample.epub` → window opens, page renders. Title bar shows "sample.epub - LitePDF". F5 outline (may be empty if ePub has no nav). **Pass criterion: no crash, page count > 0.**
3. **CBZ fixture**: `litepdf.exe tests/fixtures/sample.cbz` → window opens, image page renders. **Pass criterion: no crash, image visible.**
4. **Invert colors single-tab**: open `simple.pdf`, Ctrl+Shift+I → white-on-black + chrome flips. Menu checked. Ctrl+Shift+I → reverts. **Pass criterion: D7 + D8.**
5. **Invert colors per-tab independence**: open a second file via Ctrl+O (Ctrl+T is unbound — see C8 in plan-eng-review notes), Ctrl+Shift+I on the second tab; switch back to first → first stays light. Verify BOTH the canvas content AND the chrome (frame background, find-bar accent, scrollbar fallback) flip correctly per tab — the chrome uses the per-canvas `Palette` route from D7, NOT the system-theme route. **Pass criterion: D9 + per-tab chrome polarity.**
6. **Dual-page**: open `bookmarks.pdf` (3 pages), Ctrl+Shift+D → spread mode; page 0 alone. PgDn → spread (1,2). PgDn again → still (1,2) — no spread (2,?) since 3 is page count. Ctrl+Shift+D → back to single, page 1 (LEFT of last spread). **Pass criterion: D10 cover rule + odd-tail rule.**
7. **Dual-page + thumbs**: with dual on, F4 → thumb pane single column; current-page accent border on LEFT page of current spread. **Pass criterion: D11.**
8. **Dual-page + search**: with dual on, Ctrl+F "Lorem", press Enter → spread that contains the hit becomes visible. **Pass criterion: D12.**
9. **Dual-page + invert combo**: Ctrl+Shift+D + Ctrl+Shift+I together → both pages render white-on-black side-by-side. Toggle each off and on independently. **Pass criterion: orthogonality.**
10. **Tab close mid-password-dialog (negative test)**: launch encrypted.pdf, dialog appears. Click Cancel. **Pass criterion: no tab created, MRU still has the file (per D2), no crash.**

---

## Version bump + tag

Per Task 5 above. Final tag: `v0.0.9-phase8`. About dialog literal at `MainWindow.cpp:936` becomes `LitePDF v0.0.9`. `litepdf.rc` VERSIONINFO catches up two phases of debt and lands at `0,0,9,0`.

---

## Risk Annotations

> Status legend: **[OPEN]** = monitored at implementation. **[MITIGATED]** = upgraded to a concrete design decision or task step.

- **R1 [MITIGATED]: In-memory `DLGTEMPLATE` byte layout fragility.** Manual byte-emission of `DLGTEMPLATE` + `DLGITEMTEMPLATE` is error-prone (DWORD alignment, control class atom encoding 0xFFFF + 0x0080..0x0086 for system controls). **Resolution per D1**: T1.4's `build_dialog_template` uses small named helpers (`align_to_dword`, `emit_word`, `emit_wstring`) instead of inline arithmetic. Three [password_dialog] unit tests exercise the byte builder. T1.8 manual smoke is the integration check. If the dialog fails to display, the Cleartype debug check in Spy++ is the first port of call.

- **R2 [OPEN]: ePub / CBZ render path may surface MuPDF-specific exceptions not present on PDF.** Tier 3 features have been off the v1 critical path since Phase 1; some edge cases (DRM-protected ePub, malformed CBZ ZIP) may surface here for the first time. **Mitigation**: Document::open already catches MuPDF exceptions and maps to `OpenError::Corrupted`. Fixtures used in T2 are well-formed; CI catches regressions on the same files. Real-world hardening (gracefully report DRM errors) is Phase 12 work.

- **R3 [OPEN]: `fz_invert_pixmap` cost may push the 1 s cold-start budget on HDD.** Single page A4 @ 150 DPI is ~16 MB pixmap; channel invert is memory-bandwidth-bound, ~2-5 ms on SSD, possibly 5-15 ms on HDD-warmed L2. Cold-start budget is 1000 ms total (design §1.1); invert on first paint adds a small constant. **Mitigation**: invert is gated by `req.invert`, default false. First paint post-Phase-8 is identical to Phase 7. The test in T2 is on functional correctness; performance benchmarking is Phase 11's job. If R3 fires in T3.6 manual smoke, defer the invert to a 2nd-paint after first paint lands (post-v1 polish).

- **R4 [MITIGATED]: PageCache key change ripples across every cache touch point.** Phase 7 D18 cataloged 4 cache sites in RenderEngine.cpp. Phase 8 T3 must update all of them to thread `req.invert` into the key. **Resolution**: T3 explicitly enumerates the four sites and the [page_cache][invert] test asserts cache hit/miss matrix. Implementer cross-references Phase 7's D18 audit before editing.

- **R5 [OPEN]: Dual-page layout interacts non-trivially with the existing pan/zoom math in PdfCanvas.** PdfCanvas::Pan is currently a single (x,y) pair scoped to the entire canvas client area. With dual-page, fitting two pages side-by-side requires either (a) per-slot pan (impossible without a second Pan), or (b) global pan that scrolls both pages in lockstep. **Mitigation per D10**: option (b) — vertical scroll moves both pages; horizontal scroll within one page is disabled in dual mode. Single-page mode unchanged. Edge case: zoom-to-fit-page in dual mode means each page fits half-width — implement by halving the per-slot DIP budget in `set_zoom_mode(FitPage, ...)` when dual_page_ is on.

- **R6 [OPEN]: Search-hit overlay coordinates in dual mode.** `PdfCanvas`'s existing hit-overlay path transforms PDF-user-space quads to canvas-pixel space using a single page-to-canvas matrix. With dual-page, the LEFT page's matrix differs from the RIGHT page's. **Mitigation per D12**: T4.6 manual smoke must exercise a hit on page 4 of `search.pdf` with dual-page on; expected behavior is the highlight appearing on the correct slot. If the highlight appears in the wrong slot, the per-slot transform is missing — fix by extending the existing `(client_x, client_y) ← page_x, page_y` helper to take a page index as an argument, so both LEFT and RIGHT use their own. ~10 LOC fix.

- **R7 [OPEN]: Phase 7 OutlinePane "empty content" finding (rationale 2026-05-01).** Pre-existing finding, NOT introduced by Phase 8, but worth keeping in scope-awareness because Task 2's smoke step opens `bookmarks.pdf` for ePub-comparison and the empty outline could confuse a casual reviewer. **Mitigation**: documented as out-of-scope in §"Out of scope"; T2 smoke does NOT assert outline content, only that the pane shows up. A future Phase 8.x or Phase 11 polish task can fix the OutlinePane bug.

- **R8 [OPEN]: Password dialog locale.** Dialog text is hard-coded English ("Password Required", "OK", "Cancel"). Design §10 lists "Localization beyond Traditional Chinese and English" as out-of-scope, but does NOT exclude Chinese dialog text — Phase 8 ships English-only and a Chinese label is a Phase 9.x or Phase 10 (installer i18n) concern. **Mitigation**: explicit log entry; UI strings are centralized in PasswordDialog.cpp so localization later is mechanical.

---

## Done When

1. **Test count:** ctest **146/146** green on Release build (assuming Phase 7 polish PR #5 has merged; if started before that lands, baseline is 145/145 instead — verify with `grep -rE '^TEST_CASE' tests/unit/ | wc -l` before T0). See test-count derivation table in §"Step 5.4".
2. **Smoke test:** `scripts/smoke-test.ps1` passes the new ePub / CBZ / encrypted-launch steps. All 10 manual smoke steps pass.
3. **Encrypted PDF UX:** Dialog modal appears centered, hides password with `ES_PASSWORD`. Wrong password 3× closes the dialog and tab. Correct password opens tab + renders. Cancel closes dialog without tab; MRU intact.
4. **ePub + CBZ:** Both fixtures open without crash. `page_count() >= 1`. Title bar reflects basename.
5. **XPS contract:** allowlist test `[document][format][xps]` continues to assert the extension is accepted (FileNotFound, not UnsupportedFormat, on a non-existent path).
6. **Invert colors:** Ctrl+Shift+I flips canvas + chrome; per-tab; menu checkmark reflects state; cache stores both polarities side-by-side; no PageCache pollution between tabs.
7. **Dual-page:** Ctrl+Shift+D toggles spread layout; cover-rule (page 0 alone) holds; PgUp/PgDn step by 2 with snap-to-LEFT-of-pair; thumb pane highlights LEFT page; search-jump lands on the right pair.
8. **No format-specific crash** opening any of `simple.pdf`, `bookmarks.pdf`, `corrupt.pdf`, `encrypted.pdf` (post-auth), `sample.epub`, `sample.cbz`, `search.pdf`, `測試.pdf`. (Negative case: `sample.png` returns UnsupportedFormat.)
9. **Versioning:** Tag `v0.0.9-phase8` on the final commit. About dialog shows `v0.0.9`. VERSION file `0.0.9-dev`. `litepdf.rc` VERSIONINFO `0,0,9,0`. PR opened via `superpowers:finishing-a-development-branch` OR `/ship` (NOT both).
10. **Design doc** `docs/plans/2026-04-15-litepdf-design.md` §3 marks the four Phase-8 Tier-3 features as shipped.

---

## Out of scope (deferred — note explicitly to guard against scope creep)

- **R17 (FindBar status appends `(left)` or `(right)` in dual-page mode)** is deferred. FindBar currently is layout-agnostic; adding slot-aware feedback requires a coordinate-to-slot reverse-mapping in `PdfCanvas` plus a new `DocumentView::find_hit_slot()` query. This is a non-trivial integration that is unbudgeted within the ~600 LOC Phase 8 target. Out of scope for v0.0.9; track as Phase 8.x follow-up if dual-mode search proves common in practice.

- **XPS fixture + integration test** — D5. Allowlist code path verified by an existence-of-extension test; full real-fixture XPS test deferred to Phase 11.
- **FB2 fixture + integration test** — D6. Same rationale as XPS.
- **SVG fixture + integration test** — D6.
- **Configurable invert color (gamma-aware, custom inversion curves)** — Phase 11 polish; Phase 8 ships plain channel-invert per the prompt.
- **Per-page invert toggle** — out-of-scope; whole-document only.
- **Dark-mode UI for tab strip / outline pane / find bar / results panel** — these already follow system dark mode via `WM_SETTINGCHANGE`. Phase 8's Ctrl+Shift+I only flips the *canvas + canvas chrome*, not the system theme. Forcing system theme is out-of-scope.
- **Dual-column thumbnails** — Phase 7 §"Known Limitations" already noted this. Phase 8 confirms (D11) and defers.
- **Right-tile current-page highlight in dual mode** — D11 trade-off.
- **Per-slot pan in dual mode (independent horizontal scroll within each page)** — R5 trade-off.
- **OutlinePane "empty content on bookmarks.pdf" bug** — Phase 4 / Phase 7 follow-up; not Phase 8.
- **Localization of password dialog** — R8.
- **Form filling, annotations, OCR** — design §10; v1.x roadmap.
- **Session restore (session.json)** — Phase 12.
- **Post-merge fast-follow tags pre-planned** — none. If something surfaces, follow `phase8.1` pattern.

---

## Known Limitations (to revisit)

- **Dialog is English-only.** Localization deferred (R8).
- **Password attempt counter is per `open()`, not session-global.** Per D2; matches design §6.1 read literally.
- **Dual-mode has single shared (x,y) pan.** Per R5; horizontal scroll within one page disabled.
- **Thumb pane highlight in dual mode marks LEFT page only.** Per D11.
- **No XPS/FB2/SVG fixture tests.** Per D5/D6.
- **No invert-mode hot-key persistence across restarts.** Per D9.
- **L1 cache size unchanged at 5 / 10 entries.** Adding the invert axis doubles potential L1 entries per (page, scale) pair. In single-page mode this is trivially absorbed (users toggle ~once per doc; LRU evicts cleanly). In dual-page + invert mode, every nav consumes 2 L1 slots (left + right page) and every toggle adds another polarity dimension, so worst-case nav-thrash on L1=5 capacity is ~2 navs before eviction starts. Acceptable for v1 because (a) the L2 display-list cache is unchanged at 10 and absorbs the rasterize cost amortization regardless of L1 polarity (D8), and (b) heavy thrash in dual+invert is an unusual usage. If Phase 11 perf profiling confirms churn, bump L1 to 8 or 12. **Recorded as a limitation, not a defect.**

- **Toggle-time visual artifacts on slow workers.** D9's drain-and-resubmit pattern minimizes the stale-polarity flash to <16 ms in normal conditions, but a worker mid-rasterize at the moment of toggle can deliver one final OLD-polarity pixmap whose paint is suppressed by the cancel-checkpoint convention (Phase 7 D17). On a heavily-loaded HDD where a worker is mid-page when a Ctrl+Shift+I lands, there is a sub-frame window where the canvas may briefly show the previous polarity. Test T3.5b asserts no visible flash on the SSD-backed CI; HDD-backed observation is manual smoke only.

---

## Re-Planning Lessons Carried From Phase 7

- **Plan-author API drift is endemic.** Phase 7 caught 6 plan defects via fresh-context subagents grepping the actual code (rationale 2026-04-26). Phase 8 anti-pattern checklist for the implementer:
  - Before writing any test, run the API call sites in `tests/unit/*.cpp` to confirm `Document::open` returns `std::optional<OpenError>`, takes `std::filesystem::path`, etc. — do NOT trust this plan's snippets verbatim.
  - Before editing `resources/litepdf.rc` or `MainMenu.rc.h`, grep the actual file. `IDR_ACCEL` does NOT exist; accelerators are programmatic in `MainWindow.cpp`.
  - Before adding a `tests/unit/CMakeLists.txt` line, confirm tests use the project-root convention (`tests/CMakeLists.txt` + filename `test_<snake>.cpp`).
  - Before naming a key `Ctrl+2`, grep the existing `ACCEL[]` array — `Ctrl+2` is taken by `IDM_TAB_GOTO_2`. Phase 8 uses `Ctrl+Shift+D` instead (modifier-consistent with `Ctrl+Shift+I` for the Invert toggle; also avoids burning the bare `Ctrl+D` slot before any future "Duplicate Tab" affordance lands). (Already corrected in T0.3.)
- **Stack reviewers (CLAUDE.md rule 14) catches what one reviewer misses.** Phase 6's pr-review-toolkit catch of the C1 drain-deadlock saved Phase 7 from shipping a deadlocking dtor. Phase 8 should run plan-eng-review + pr-review-toolkit:review-pr in parallel before T1 begins; both should agree on the Win32 dialog-template encoding before any byte of `PasswordDialog.cpp` is committed.
- **Per-task two-stage review (spec compliance, then code quality) catches bugs every cycle.** Phase 7 surfaced 4 critical bugs across spec and quality reviewers. Phase 8 task list keeps the same discipline: each task ends with a Commit step but the implementer should pause for spec-compliance review before squashing review feedback into the commit.
- **Verify before claiming done (`superpowers:verification-before-completion`).** ctest output + manual smoke + `git status` clean + tag visible via `git tag --list` before declaring Phase 8 finished.

---

> **Fast-follow log:** _(empty at plan write — populate at first follow-up tag.)_
