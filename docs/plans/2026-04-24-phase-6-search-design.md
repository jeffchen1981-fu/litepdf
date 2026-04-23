# Phase 6 — Search (In-doc Ctrl+F + Cross-tab Ctrl+Shift+F) — Design

**Date:** 2026-04-24
**Status:** Approved, ready for implementation plan
**Scope:** LitePDF v1 Phase 6 (per roadmap `2026-04-15-litepdf-roadmap.md`)
**Prerequisite:** Phase 5 (multi-tab) landed at tag `v0.0.6-phase5` plus tab-strip polish follow-ups through `d0f23f1`.

---

## 1. Goals & Scope

Phase 6 delivers two related but independently useful features:

- **6.1 In-document find** — `Ctrl+F` opens a floating find bar anchored to the active tab's canvas; user types a query, sees a live match counter, navigates hits with Enter / Shift+Enter / F3 / Shift+F3. Target: parity with the `Ctrl+F` experience of Chrome, Edge, Firefox, and Acrobat.
- **6.2 Cross-tab find** — `Ctrl+Shift+F` opens a dockable results panel at the bottom of the main window; the user's query is fanned out to every currently-open tab in parallel, and results (file / page / snippet) populate the panel as the scan progresses. Clicking a row navigates to the hit.

The two features ship under one design doc and one implementation plan but split into two sub-milestones so 6.1 is demoable on its own:

| Sub-milestone | Tag | Deliverable |
|---|---|---|
| 6.1 | `v0.0.7-phase6.1-indoc` | In-doc find bar fully functional on the active tab |
| 6.2 | `v0.0.7-phase6`         | Cross-tab panel layered on top of 6.1 |

**Non-goals (deferred):**
- Whole-word matching (MuPDF has no native support — would require stext post-processing).
- Regex / wildcard search (same reason).
- Search result export / "highlight all" permanent mode across sessions.
- Thumbnail-pane integration (Phase 7).
- Session restore of panel state (Phase 12).
- True dockable floating panel (panel is bottom-docked with resizable height only — a full docking framework is out of scope).

## 2. Relationship to the Master Design Document

This phase refines two clauses of `2026-04-15-litepdf-design.md`:

- **§5.4 "Per-Document result cache keyed by query string"** is re-interpreted as **"per-SearchSession cache, one SearchSession per DocumentView (i.e., per tab)"**. The spirit — scope-per-document, never shared across tabs — is preserved; the wording is tightened to match the post-Phase-5 architecture where `Document` is wrapped by `DocumentView` and each tab owns its own `DocumentView`.
- **§5.4 "Results render in a dockable panel at the bottom of the main window (VS Code-style)"** is implemented as a **bottom-docked resizable pane**: it can be collapsed with F6 or the close button, its height is adjustable via a splitter bar, but it cannot be un-docked or floated. A full docking framework (~1500+ LOC) is outside Phase 6's ~750 LOC budget and is deferred indefinitely.

No other clause of the master design is affected.

## 3. Module Layout

```
core/Document.hpp          + page_hits(page, query, flags) — stateless MuPDF bridge
core/Document.cpp          uses fz_search_display_list2 when L2 hit, else fz_search_page2
core/PageCache.hpp         + peek_display_list(page) — read-only L2 access, does NOT touch LRU
core/SearchSession.hpp/cpp NEW — per-tab search state: query, flags, cursor, hit cache
                                  (pure logic, no Win32, unit-testable)
core/DocumentView.hpp/cpp  + owns unique_ptr<SearchSession>
app/SearchDispatcher.*     NEW — single app-wide 2-thread task pool for search work
app/CrossTabSearch.*       NEW (6.2) — fan-out orchestrator, aggregates hits from N sessions
ui/FindBar.*               NEW (6.1) — floating Ctrl+F bar (child HWND, Edit + buttons + counter)
ui/ResultsPanel.*          NEW (6.2) — bottom-docked virtual ListView + splitter
ui/PdfCanvas.cpp           + paints hit quads on top of page bitmap;
                           + scroll_into_view(quad) helper
ui/MainWindow.cpp          + Ctrl+F / Ctrl+Shift+F / F3 / Shift+F3 / F6 accelerators;
                           + layout: find-bar anchor + results-panel splitter
resources/MainMenu.rc.h    + IDM_FIND, IDM_FIND_NEXT, IDM_FIND_PREV,
                             IDM_CROSS_TAB_FIND, IDM_TOGGLE_RESULTS_PANEL
tests/unit/test_search_session.cpp NEW
tests/fixtures/search.pdf            NEW (5–10 pages, deterministic hit positions)
```

**LOC budget:** ~750 (vs. roadmap estimate of 500). Overrun explained by the splitter + resizable panel plumbing and Unicode-aware snippet extraction for cross-tab rows.

## 4. Architectural Decisions

Pinned before task breakdown; revisit only if a task uncovers a blocker.

### D1. `SearchSession` is a pure-logic class in `core/`
One instance per `DocumentView`. Owns:
- Current query (UTF-16 wstring) and flags (match_case: bool).
- `std::shared_ptr<SearchState>` containing the hit cache, current cursor (page + hit index within page), epoch counter, and scan completion flag. The shared_ptr lifetime pattern (with dispatcher holding `weak_ptr`) enables safe task cancellation when a tab is closed mid-scan — see D5.
- Hit cache structure: `std::unordered_map<std::wstring, PerQueryResults>` where `PerQueryResults` holds `std::vector<PageHit>` (flattened across pages, sorted by page then position) plus a `scan_complete` bool. Keyed by query string so re-searching a prior query is instant.

The class contains no Win32, no MuPDF types in its public API, and no threads. All background work goes through `SearchDispatcher` (D5). This keeps the class unit-testable with `InlineDispatcher` for deterministic test runs (D10).

### D2. `Document::page_hits` is the sole MuPDF search bridge
A new stateless method on `Document`:

```cpp
struct PageHit { fz_quad quad; std::string snippet_utf8; };
std::vector<PageHit> page_hits(std::size_t page,
                               std::string_view needle_utf8,
                               Flags flags,
                               fz_cookie* cookie = nullptr) const;
```

Internally:
1. Ask `PageCache::peek_display_list(page)` for a cached L2 display list (non-owning, no LRU update).
2. If present: call `fz_search_display_list2(ctx, list, needle, flags, marks, quads, max)`.
3. If absent (cold page): call `fz_search_page2(ctx, page, needle, flags, ...)`. **Do not push the page into L2** — search must not evict render's hot pages.
4. For each quad returned, extract a 30-char snippet from stext (centered on the hit) for later display in the cross-tab panel. For in-doc mode only the quad is used (snippet is cheap enough to always compute, ~μs per hit).
5. `cookie` propagates cooperative cancellation: the MuPDF routines check `cookie->abort` periodically and return early.

The Document class stays UI-agnostic and headless-testable (Phase 1 contract preserved).

### D3. `PageCache::peek_display_list` is read-only
Signature:
```cpp
// Returns a borrowed, non-owning fz_display_list* if the page is currently
// in L2; nullptr otherwise. Does NOT update the LRU recency — search
// must be a neutral observer, never a cache pollutant.
//
// Uses `int page_num` to stay consistent with the existing
// get_display_list / put_display_list siblings from Phase 2.
fz_display_list* peek_display_list(int page_num) const;
```

This is the single new public API added to `PageCache` in Phase 6. Render-path behavior is untouched.

### D4. Eager all-pages scan with priority
When `SearchSession::set_query` is called with a non-empty query:
1. Bump `SearchState::epoch` (cancels any in-flight tasks from previous queries).
2. Emit tasks for **every page** to `SearchDispatcher`, with priorities:
   - P0 (highest): the page the user is currently viewing.
   - P1: current ± 1 … current ± 5 (L2-warm range).
   - P2: all remaining pages.
3. As each task completes, worker posts `WM_USER_SEARCH_HIT` to the UI thread with the new hits. UI thread appends to `SearchState::hits`, fires `OnUpdate` callback so find bar counter (and, in 6.2, results panel) refreshes.
4. When the last page task completes, `scan_complete` flips to `true` — counter changes from "3 / 12+" to "3 / 12".

This model gives Chrome-like perceived responsiveness: the "first batch" counter arrives in ~10 ms (P0 + nearby L2-hit pages), then the tail fills in over seconds for large PDFs.

### D5. `SearchDispatcher` = 2-worker shared pool with `weak_ptr` task lifetime
A single `app::SearchDispatcher` instance owned by `AppController` (or by `MainWindow` pending AppController extraction):

```cpp
class SearchDispatcher {
public:
    explicit SearchDispatcher(std::size_t num_workers = 2);
    ~SearchDispatcher();  // joins workers

    struct Task {
        std::weak_ptr<SearchState>    state;   // null-safe: task skips if expired
        std::shared_ptr<Document>     doc;     // shared_ptr so task can outlive DocumentView
                                               // ONLY if Document itself is shared — see lifetime note below.
        std::size_t                   page;
        std::wstring                  query_snapshot;
        SearchSession::Flags          flags;
        std::uint64_t                 epoch;   // if != state->epoch on pickup, skip
        std::uint8_t                  priority; // P0=0, P1=1, P2=2
    };

    void submit(Task t);
    void cancel_epoch_before(std::shared_ptr<SearchState> s, std::uint64_t epoch_cutoff);
};
```

**Thread model:** 2 fixed worker threads. Each loop:
1. Pop highest-priority task from mutex-guarded `std::priority_queue`.
2. Lock `state` weak → if expired, drop task, continue.
3. Compare task's `epoch` with `state->epoch` atomic — if newer epoch exists, drop.
4. `fz_context* ctx = task.doc->clone_context();` (thread-safe, documented in `Document.hpp`).
5. Build `fz_cookie` bound to `state->abort_flag`.
6. Call `doc->page_hits(page, query, flags, &cookie)` using `ctx`.
7. Post `WM_USER_SEARCH_HIT` (wParam = raw pointer to a heap `HitBatch{state, page, hits}` — UI thread deletes).
8. `fz_drop_context(ctx)`.

**Lifetime note:** `Document` is currently owned by `DocumentView` via a non-shared path (Phase 5 changed it to be move-constructed into `DocumentView`). To make dispatcher tasks lifetime-safe, we must either:
- (a) Promote `Document` to be held by `shared_ptr<Document>` inside `DocumentView` (minor refactor, one indirection added to every `DocumentView::document()` caller), or
- (b) Have the dispatcher task reference the `SearchState` only — Document pointer is resolved on UI thread before submission and captured by value, relying on `weak_ptr<SearchState>` to gate validity (Document outlives SearchState because DocumentView owns both in the destruction order Document-last, SearchState-first).

Phase 6 picks **(b)**. DocumentView's destructor order is already Document-last (see DocumentView.hpp lifetime contract); as long as SearchSession is destructed before Document inside DocumentView, any dispatcher task that successfully locks `state.lock()` is guaranteed Document is still alive, because SearchState holds a raw `Document*` that is only ever read while the strong lock on state is held and destruction clears `state` first. We enforce this with an explicit destructor order note in `DocumentView::Impl` matching the Phase 3 Task 4 contract.

**Cancellation priority:** `set_query` first bumps epoch, then atomically sets `abort_flag` true for a short interval (so in-flight MuPDF calls abort), then resets `abort_flag` false before submitting new tasks. This pattern matches the per-render escrow in Phase 5 I-2.

### D6. Floating find bar (6.1)
A child HWND of MainWindow (style `WS_CHILD | WS_CLIPSIBLINGS`, **no** `WS_POPUP` — popups detach from parent lifecycle and leave residue when MainWindow minimizes, as the insights during Q2 noted).

**Layout:** Anchored to canvas's top-right corner with margin (right: 16 DIP, top: 8 DIP). On `WM_SIZE` of MainWindow, `FindBar::reposition(canvas_rect)` recalculates.

**Z-order:** Above PdfCanvas; below any modal dialog MainWindow may create (none currently). `SetWindowPos(..., HWND_TOP, ...)` on show.

**Children (left-to-right):**

| Control | Style | Purpose |
|---|---|---|
| `Edit`        | `ES_AUTOHSCROLL` | query input |
| `Static`      |                  | counter "3 / 12" or "3 / 12+" or "" |
| `ButtonEx` `‹`| `BS_OWNERDRAW`   | previous hit |
| `ButtonEx` `›`| `BS_OWNERDRAW`   | next hit |
| `ButtonEx` Aa | `BS_OWNERDRAW` + toggle | match-case |
| `ButtonEx` `✕`| `BS_OWNERDRAW`   | close |

All owner-draw buttons paint from the Phase 5 `Palette` struct to respect dark mode / HiDPI.

**Keyboard (Edit subclass WM_KEYDOWN):**
- `VK_ESCAPE` → close find bar, clear current-hit highlight, `SetFocus(canvas_hwnd)`.
- `VK_RETURN` → next (same as `›` button).
- `VK_RETURN` + `GetKeyState(VK_SHIFT) & 0x8000` → previous.
- `VK_F3` / `VK_F3` + Shift → next / previous (also handled globally by MainWindow accelerator so F3 works when focus is on canvas).

**Debounce:** `EN_CHANGE` notifies MainWindow → `SetTimer(find_bar_hwnd, kDebounceTimer, 120, nullptr)`; `WM_TIMER` fires → `SearchSession::set_query(text, flags)`. 120 ms matches human typing cadence without visible lag.

**Palette-driven:** WM_SETTINGCHANGE propagates to FindBar (reuses Phase 5 hot-switch wiring) so dark-mode toggle is instant.

### D7. Hit quad rendering
Hits are stored as PDF-space `fz_quad` in `SearchState::hits`. Transform occurs at paint time:

1. After `on_paint` blits the page bitmap, iterate `hits_for_page(current_page)`.
2. For each quad `q`, compute DIP-space corners: `q_dip = q * scale` where `scale = DocumentView::zoom_scale() * dpi / 72.0`. Apply canvas pan.
3. Draw with `ID2D1RenderTarget::FillGeometry`:
   - Non-current hits: `#FFFF00` α=0.40 solid fill, no stroke.
   - Current hit: `#FFA500` α=0.50 solid fill + 1px `#CC7700` stroke.
4. Clip to page rect to avoid draw-over-next-page on dual-page spreads (Phase 8 concern; current single-page layout is fine).

Zoom/pan invalidations automatically re-run the transform — no hit list refresh needed.

### D8. Scroll-into-view on navigate
`PdfCanvas::scroll_into_view(page_index, fz_quad q)`:

1. If `page_index != current_page`, `DocumentView::set_current_page(page_index)`, kick_render, and continue once the new page is set.
2. Transform `q` to DIP-space (same math as D7).
3. If the quad's DIP bbox fully lies within the current viewport (with a 24 DIP buffer from edges) → do nothing (just change current-hit color via `InvalidateRect`).
4. Else → compute `new_pan_y = -(q.center.y * scale - viewport_h / 2)` and `new_pan_x` similarly; clamp to `[min_pan, max_pan]` to stop at document edges.

Called by: find-bar next/prev, cross-tab panel row-click.

### D9. Cross-tab architecture (6.2)
`app::CrossTabSearch` owns:
- Current panel query (`std::wstring`).
- `std::vector<std::weak_ptr<SearchState>>` — one entry per tab snapshotted at search-start (D12).
- Aggregated result list `std::vector<CrossTabHit>` where `CrossTabHit = { tab_index_at_submit, file_path, page, quad, snippet }`.

**Flow:**
1. User presses Ctrl+Shift+F → `ResultsPanel::show_or_focus()`. Focus goes to panel's Edit control.
2. User types query + Enter → `CrossTabSearch::dispatch(query, snapshot_open_tabs())`.
3. For each tab in snapshot, CrossTabSearch obtains the tab's SearchSession, calls `session.set_query_shared(query, flags)` which reuses the dispatcher. The "shared" suffix means results are also mirrored to `CrossTabSearch::hits`.
4. Panel ListView is virtual (LVS_OWNERDATA); CrossTabSearch bumps `SetItemCount` as hits arrive. LVN_GETDISPINFO fills rows on-demand.
5. Row click → `TabManager::set_active(hit.tab_index_at_submit)` → `DocumentView::set_current_page(hit.page)` → `SearchSession::set_current_hit(hit.page, hit.quad)` → canvas highlights + scrolls into view.

**Snapshot semantics:** The `tab_index_at_submit` captures the tab's identity at submission. If the user closes that tab, clicking the result shows a transient status message "result is no longer available" and the row is greyed but not removed (removing mid-display would shift the ListView awkwardly). Implementation: `CrossTabHit` holds a `std::weak_ptr<DocumentView>` through `SearchState`; resolution on click tests the weak.

### D10. `InlineDispatcher` for unit tests
`SearchDispatcher` is accessed through an interface:
```cpp
class ISearchDispatcher {
public:
    virtual ~ISearchDispatcher() = default;
    virtual void submit(Task t) = 0;
    virtual void cancel_epoch_before(std::shared_ptr<SearchState>, uint64_t) = 0;
};
```

Production = `ThreadPoolDispatcher`. Tests = `InlineDispatcher` that calls `task()` synchronously on the caller's thread. Tests then assert hit lists without mocking threads. This is the same DI pattern hinted at in the Q6 insights.

### D11. Results panel docking & splitter
- Fixed location: below canvas row, above status bar (none yet, so above bottom of MainWindow client).
- Default height: 200 DIP. Persisted in memory only for Phase 6 (persisted to registry in Phase 12 session restore).
- Splitter = custom window class `litepdf::ui::Splitter`, height 4 DIP, `WM_SETCURSOR` → `IDC_SIZENS`, drag posts WM_COMMAND to MainWindow asking for height change, MainWindow re-runs `on_layout()`.
- `F6` toggles visibility (hide — height 0; show — restore last height). Panel's close button `✕` = hide.

### D12. Feature toggle set (minimal)
- **match_case** (toggle in find bar and in results panel) — native MuPDF support via `fz_search_*2` + `FZ_SEARCH_EXACT` flag. No LOC cost beyond passing the flag.
- **whole word / regex** — out of scope. Future phases.

### D13. Debounce + cancellation interaction
Every keystroke in the find bar's Edit restarts a 120 ms timer. On timer fire:
1. If query text unchanged from last submitted → no-op.
2. Else bump `SearchState::epoch`, set `abort_flag = true` briefly, clear hits, reset `abort_flag`, dispatch new tasks.

This prevents "user types faster than 120 ms" from launching N overlapping scans; only the last settled query survives.

### D14. Unicode & case folding
`fz_search_*2` handles case folding via ICU-based Unicode fold tables (bundled in MuPDF). Traditional Chinese / Japanese / Korean searches work by default; `match_case` flag switches between case-fold-enabled and byte-exact. Snippet extraction uses the same stext character stream (UTF-8), preserving multi-byte characters. Find bar's Edit is UTF-16 (Windows native); conversion to UTF-8 happens once at `set_query` boundary.

### D15. Hit cap per page
`max_quads = 256` passed to `fz_search_*2`. Rationale: a technical PDF might have "the" appear 500 times on a page of small-font prose; rendering 500 overlay rectangles is wasteful and the user will only navigate a few anyway. 256 is generous for typical documents. When hit, a `hit_limit_reached` flag on that page's result triggers a log warning but does not surface in the UI (there is no room in the find bar for a "truncated" indicator; Phase 11 revisits if it becomes a problem).

### D16. No search-path writes to PageCache L2
Reaffirming D3: `peek_display_list` is read-only. This guarantees search never causes render-hot pages to be evicted from L2, which would defeat L1 prefetching. Trade-off: cold pages searched once never warm the L2 cache for future render — that's intentional. Render's own prefetch is the L2 populator.

## 5. Runtime Behavior

### 5.1 In-doc Ctrl+F flow
```
t=0       User presses Ctrl+F
t=1ms     MainWindow accelerator → FindBar::show_or_focus()
          FindBar visible, Edit gets focus, text restored from last session
t=1ms     If text non-empty, 120 ms timer starts
t=121ms   WM_TIMER → SearchSession::set_query(text, flags)
          SearchState::epoch bumped; tasks submitted at P0/P1/P2
t=125ms   Dispatcher worker 0 picks P0 (current page); clones ctx; calls
          fz_search_display_list2 on L2 hit → returns ~ms
t=128ms   WM_USER_SEARCH_HIT posted; UI appends hits, fires OnUpdate
          FindBar counter updates to "1 / 3+"
t=500ms   Dispatcher has burned through P1 pages; counter now "1 / 12+"
t=5s      All pages scanned; scan_complete true; counter "1 / 12"
```

### 5.2 Cross-tab Ctrl+Shift+F flow
```
t=0       User presses Ctrl+Shift+F
t=1ms     ResultsPanel::show_or_focus(); MainWindow::on_layout() re-runs
          with results panel visible (canvas row shrinks by 204 DIP)
t=1ms     Focus to panel Edit; user types query
          (Unlike find bar, cross-tab Edit does NOT debounce-auto-submit —
           user must press Enter, matching VS Code's "find in files" UX.)
t=2s      User hits Enter → CrossTabSearch::dispatch
          For each of N open tabs, session.set_query_shared submitted.
          Dispatcher now has 2 * N tasks (2 priority tiers per tab)
          — actually flattened into a single priority queue; the 2 workers
          drain it round-robin across tabs naturally.
t=2.2s    First hits from whichever tab's current page comes up first;
          virtual ListView SetItemCount bumps to reflect new count.
t=30s     All tabs' all pages scanned; scan_complete across the board.
```

### 5.3 Tab closed mid-cross-tab-scan
```
t=0     Cross-tab scan running; tab index 2 of 5 has 3 page tasks pending
t=1s    User closes tab 2 (Ctrl+W)
        → MainWindow::on_tab_close_request(2)
        → TabList::remove(2) → Tab destroyed
        → DocumentView destroyed (SearchSession destroyed first per D5 order)
        → SearchState::shared_ptr's last strong ref released
        → weak in SearchDispatcher::Task slots expires
t=1.1s  Worker 0 picks up a pending page-7-of-tab-2 task
        → weak_ptr.lock() returns nullptr → drop task, continue
        → No MuPDF call, no crash, no hang
t=...   Panel still shows tab 2's already-accumulated hits with weak refs;
        user clicks one → weak expired → status bar "result is no longer
        available"; ListView row stays but greyed.
```

## 6. Error Handling

| Error | Strategy |
|---|---|
| MuPDF OOM during `fz_search_*2` | Task logs, posts empty hit list for page, continues. Other pages unaffected. |
| Document freed mid-task | Protected by `weak_ptr<SearchState>` + destructor order (D5). Worst case: worker drops task. |
| Dispatcher worker exception | Caught at worker-loop boundary, logged, loop continues. Same pattern as RenderEngine workers (Phase 2). |
| Out-of-memory appending hit vector | Hit appending guarded by `try { hits.push_back(...); } catch (std::bad_alloc&) { scan_complete = true; log; }`. Scan effectively aborted, remaining tasks become no-ops. |
| Invalid UTF-8 from snippet extraction | Replace offending bytes with U+FFFD via `fz_utf8_to_runelen` check; log once per document. |
| User closes tab during dispatch queue full | Task already enqueued is safely dropped (D5). |
| User closes window during scan | MainWindow's destructor joins dispatcher (in its destructor). All pending tasks skip on weak expiry; workers exit cleanly. |

## 7. Testing

### 7.1 Unit tests (Catch2)

**`test_search_session.cpp`** — 8 core scenarios using `InlineDispatcher`:

| # | Scenario | Assertion |
|---|---|---|
| 1 | Empty query | No hits; scan_complete immediately; counter 0. |
| 2 | Single-page hit | All 12 "Lorem" on p.1 appear; cursor starts at hit 0. |
| 3 | Multi-page hit | "dolor" on p.2/p.4/p.7; cursor next() walks page-then-hit order. |
| 4 | No-hit query | `XYZABC123` → 0 hits; scan_complete true. |
| 5 | Query change cancels | set_query("Lor") → set_query("Lorem") → only Lorem hits appear. |
| 6 | Cursor wrap | next() on last hit wraps to first; prev() on first wraps to last. |
| 7 | Page change during scan | User navigates p.1 → p.5 mid-scan; current-hit tracking follows. |
| 8 | Session destruct mid-scan | Destruct session with pending tasks; no crash; InlineDispatcher completes all tasks as no-ops. |

**Additional:** `test_page_cache_peek.cpp` — verifies `peek_display_list` returns non-null for an L2-resident page, nullptr for a cold page, and does NOT alter LRU order (before/after key comparison).

Target coverage: 15 new test cases, pushing total from ~70 (post-Phase-5) to ~85.

### 7.2 Fixture: `tests/fixtures/search.pdf`

Structure (5–10 pages, generated once via LibreOffice headless from a markdown source committed to the repo for reproducibility):

| Page | Content | Known hits |
|---|---|---|
| 1 | Lorem ipsum filler (12 `Lorem`) | 12 × "Lorem" |
| 2 | …with dolor sit amet… (3 `dolor`) | 1 × "dolor" |
| 3 | 中文測試段落(Unicode case-fold sanity) | 1 × "中文測試" |
| 4 | More Latin filler with dolor | 1 × "dolor" |
| 5 | Plain text, no special keywords | 0 hits for test keywords |
| 6 | Lorem appears 3 times | 3 × "Lorem" |
| 7 | Final dolor | 1 × "dolor" |
| 8–10 | (optional) additional Lorem spread | remainder |

Known totals (for Q11 exit criteria): "Lorem" = 15, "dolor" = 3, "中文測試" = 1, "XYZABC123" = 0.

Build: `scripts/make-search-fixture.ps1` (new) runs pandoc + libreoffice-headless; CI does NOT re-run this; the fixture is a committed binary. Local regeneration is a documented step in the script's header comment.

### 7.3 Manual smoke additions (scripts/smoke-test.ps1)

Three new items:

- **(a) In-doc find happy path:** Open `search.pdf`, Ctrl+F, type "Lorem" letter by letter; verify counter flows cleanly and settles at "1 / 15"; press F3 several times observing highlight movement and scroll-into-view.
- **(b) Cross-tab happy path:** Open `search.pdf` + a second fixture containing "Lorem"; Ctrl+Shift+F, type "Lorem", Enter; verify panel populates from both files; click a row from the second tab; verify active tab switches, page navigates, hit highlighted.
- **(c) Incremental typing & debounce:** Open a large PDF (500+ pages); type "a", wait, "ab", wait, "abc"; verify counter settles at each step without UI freeze; verify that rapid consecutive keystrokes collapse into a single scan (monitor via task manager: one burst of CPU, not three).

### 7.4 `scripts/ux-probe.ps1` extension

Add `--search <query>` flag:
1. Launch LitePDF with given fixture.
2. Post Ctrl+F via SendInput.
3. Set find-bar Edit text to `<query>` directly via WM_SETTEXT.
4. Wait up to 30 s for counter Static's text to stop changing (scan_complete heuristic).
5. Emit JSON: `{ query, total_hits, current_page, current_hit_quad, elapsed_ms }`.

Enables future Phase 11 benchmark regression gate on search latency.

## 8. Done When

### 8.1 Phase 6.1 exit (tag `v0.0.7-phase6.1-indoc`)

1. Ctrl+F on `search.pdf` opens floating find bar; typing "Lorem" yields counter converging to 15 within 2 seconds.
2. Typing incrementally (L, Lo, Lor, Lore, Lorem) shows counter update smoothly after each 120 ms debounce.
3. Enter/F3 advance hits across pages with scroll-into-view; Shift+Enter/Shift+F3 reverse.
4. `Aa` toggle switches between case-fold and exact; fixture "Lorem" vs "lorem" behaves correctly.
5. ESC closes find bar, clears highlights, returns focus to canvas.
6. `ctest --test-dir build -C Release` passes all prior tests plus new SearchSession + PageCache peek tests (~85 total).
7. Manual smoke (a) and (c) pass.
8. No memory leak across 100 Ctrl+F open/close cycles (Task Manager RAM stable).
9. Tag `v0.0.7-phase6.1-indoc` pushed.

### 8.2 Phase 6.2 exit (tag `v0.0.7-phase6`)

1. Ctrl+Shift+F opens bottom-docked results panel; canvas row shrinks to make room.
2. Typing a query + Enter populates ListView with hits from all open tabs.
3. Row click navigates to the correct tab + page + highlights + scrolls into view.
4. Splitter drag resizes the panel between 100 DIP and (client_height - 200 DIP); F6 toggles; ✕ hides.
5. Closing a tab mid-scan does not crash; previously-displayed rows from that tab grey out on click.
6. Manual smoke (b) passes.
7. Tag `v0.0.7-phase6` pushed.

## 9. Known Limitations (Post-Phase-6)

- **Whole-word / regex search unavailable.** Requires stext post-processing. Track as post-v1 enhancement.
- **Hit snippet is 30 chars only.** Configurable in a future phase if users request wider context.
- **Cross-tab results do not persist across app restarts.** Phase 12 session restore may cover this.
- **ResultsPanel is not a true dock.** Cannot be un-docked to floating window. Docking framework out of scope for v1.
- **SearchDispatcher is 2-worker fixed.** Phase 11 benchmark data may motivate making it DPI-/CPU-count-adaptive.
- **No "find all and show highlight" permanent mode.** Highlights disappear when find bar closes (per D6). Chrome-style persistent "yellow bar" visualization is out of scope.
- **L2 display list is not warmed by search.** Cold pages searched once do not become render-hot. Intentional per D16.

## 10. Interactions with Prior Phases

- **Phase 2 PageCache:** gains `peek_display_list` — strictly additive.
- **Phase 2 RenderEngine:** untouched. Separate thread pool from SearchDispatcher.
- **Phase 3 PdfCanvas:** +hit quad painting + scroll_into_view helper. WM_PAINT path unchanged except post-blit overlay.
- **Phase 4 OutlinePane / MRU:** untouched. Find bar does not interact with outline tree.
- **Phase 5 TabManager / TabList:** Tab struct unchanged; `DocumentView` gains `unique_ptr<SearchSession>` but existing destruction order contract holds.
- **Phase 5 Palette / dark mode:** Reused for FindBar + ResultsPanel owner-draw.
- **Phase 5 single-instance IPC:** Unaffected; IPC payload continues to be `L'\0'`-terminated path; search is a per-instance feature.

## 11. Summary

| Dimension | Decision |
|---|---|
| Delivery cadence | Single plan, two sub-milestone tags: 6.1 (in-doc) then 6.2 (cross-tab) |
| Find bar UX | Floating child HWND in canvas top-right; Chrome/Edge/Firefox parity |
| State ownership | `core::SearchSession` per DocumentView; `Document::page_hits` stateless |
| Iteration | Eager all-pages P0/P1/P2 priority scan |
| MuPDF primitive | `fz_search_display_list2` via L2 peek; fallback `fz_search_page2` |
| Thread model | Single app-wide `SearchDispatcher`, 2 workers, shared with all tabs |
| Feature flags | `match_case` only (native MuPDF `FZ_SEARCH_EXACT`). No whole-word, no regex |
| Keyboard | Enter/Shift+Enter + F3/Shift+F3 both supported; ESC closes + refocus canvas |
| Debounce | 120 ms via `SetTimer`/`WM_TIMER` |
| Cancellation | `SearchState::epoch` bump + `fz_cookie::abort` cooperative abort |
| Cross-tab panel | Bottom-docked, resizable via splitter; F6 toggle; virtual ListView (LVS_OWNERDATA) |
| Highlight | Current hit orange (#FFA500 α=0.5 + outline); others yellow (#FFFF00 α=0.4) |
| Scroll-into-view | Only when hit not currently visible; vertical-center target |
| Lifetime safety | `weak_ptr<SearchState>` + destruction order in DocumentView |
| Tests | 15 new Catch2 cases w/ `InlineDispatcher`; new `search.pdf` fixture; ux-probe `--search` flag |
| LOC budget | ~750 (exceeds roadmap 500 due to splitter + panel) |

Implementation plan to follow via `superpowers:writing-plans`, task-by-task breakdown with TDD discipline matching Phases 2–5.
