# Scan the page on screen first — design

**Date:** 2026-10-13
**Baseline:** `main` @ `dca2d4b` (v1.5.4)
**Issue:** #140 (a search scans pages in an arbitrary order, so the page on
screen can get its highlights last)
**Ships as:** one PR.

## 1. Motivation

`SearchSession::set_query` submits one task per page, all at priority 2
(`SearchSession.cpp:327-333`), to the one `ThreadPoolDispatcher` that every
tab shares (`SearchDispatcher.hpp`). Its queue is a `std::priority_queue` whose
`TaskCmp` compares the priority alone (`SearchDispatcher.hpp:78-80`), so tasks
of equal priority come out in heap order: neither page order nor submission
order.

### 1.1 Measurement (from #140, taken while measuring #135)

`html5_spec.pdf` (753 pages), go to page 6 (index 5), search for `e`. Page 5's
256 hits arrived only in the last ~0.1 s of each scan, 8.6 to 12.8 s after the
query, in all 6 runs (3 on main, 3 on the #135 branch).

### 1.2 Two cursor defects the fix has to deal with

Both are pre-existing. Section 3 changes the order pages arrive in, which turns
the first one from occasional into systematic, so both are fixed here.

**(a) The cursor is not stable when a page is inserted.** `State::cursor` is a
raw index into `hits` (`SearchSession.cpp:51`). The #139 block insert
(`:394-398`) puts a page's hits in place and does not adjust the cursor, so a
page landing before the cursor's hit moves the cursor onto a different hit.
`MainWindow::on_find_next` calls `next()` directly (`MainWindow.cpp:2228-2239`),
so the next Enter can jump back to an earlier page. Today this happens at
random heap positions. If the scan starts at the page on screen and wraps, the
pages before it are by construction the last to arrive, so it would happen on
every search where the user presses Enter before the scan finishes.

**(b) The first Find Next skips the first match.** `set_query` resets
`cursor = 0` (`:272`), and `next()` increments before it reads
(`:476-483`). Nothing in `src/` calls `set_reference_page` or `current()`
(`rg -n "set_reference_page|\.current\(\)" src` hits only `SearchSession.cpp`
itself), so the first Enter after a search returns `hits[1]`: the second match
on the lowest page found so far, not a match on the page on screen.

## 2. Goal and success criteria

**Goal:** a search scans the page on screen first and continues forward from
it, and Find Next / Find Previous start from that page.

**Success criteria:**

1. Same setup as §1.1 (page index 5, query `e`): page 5's hits arrive within
   the first two completed pages of the scan, in every run.
2. Total scan time for `e` on `html5_spec.pdf` is not worse than main's
   (main measured 6.4 / 6.7 / 6.7 s after #139).
3. Two tabs searched at once (cross-tab search) still use both workers: the
   wall time of a two-tab cross-tab scan is not the sum of the two single-tab
   scans.
4. After a new search, the first Enter goes to the first match on or after the
   page on screen; the first Shift+Enter goes to the last match before it.
   Both wrap.
5. Pressing Enter while the scan is still running never moves to a match on a
   page before the current match's page, except by wrapping past the last
   match found so far.

## 3. Design

### 3.1 Dispatcher: owners take turns, each owner in submission order

`ThreadPoolDispatcher` replaces its `std::priority_queue` with an internal
structure; `ISearchDispatcher::Task` and `submit` keep their signatures.

```cpp
// private to ThreadPoolDispatcher
struct OwnerQueue {
    std::weak_ptr<void> owner;   // Task::state_weak of its tasks
    std::deque<Task>    tasks;   // submission order
};
std::map<std::uint8_t, std::deque<OwnerQueue>> lanes_;  // key: priority
```

- **submit:** find the lane for `t.priority`, then the `OwnerQueue` whose
  `owner` is the same owner as `t.state_weak` (`!a.owner_before(b) &&
  !b.owner_before(a)`; compares control blocks, so it stays correct after the
  owner expires and cannot be fooled by a reused address). Append to it, or
  append a new `OwnerQueue` to the back of the lane.
- **pop:** take the lane with the smallest key (highest urgency). Pop the front
  task of its front `OwnerQueue`. If that queue is now empty, erase it;
  otherwise move it to the back of the lane. Erase an empty lane.
- **Ordering contract** (stated on `ISearchDispatcher`): a lower priority
  number always runs first; within one priority, one owner's tasks run in
  submission order, and owners take turns one task at a time.
- Unchanged: the drain-before-exit predicate (`stop_` and nothing queued), the
  runnability check, the try/catch around `run`, `InlineDispatcher` (it runs
  on submit, so it has no queue to order).

**Why turns and not plain FIFO.** Each session's scan holds its own
`scan_mtx` (`SearchSession.cpp:361`, blocking `lock_guard`) — per-tab
parallelism is 1 by design (#130 spec §3.3). Under plain FIFO both workers pop
consecutive pages of the same session and one waits on that lock, so a
cross-tab search runs on one worker, and a search on tab B waits for all of
tab A's remaining pages (a tab switch does not cancel A's scan,
`MainWindow.cpp:855-863`). Taking turns gives each worker a different session
whenever two sessions have work, which is today's average behaviour made
deterministic.

The number of owners in a lane is the number of tabs with a scan queued, so
the linear owner lookup in `submit` is over a handful of entries.

### 3.2 Submission order: from the start page, forward, wrapping

`set_query` gains two parameters with defaults, so `clear()` and the existing
test call sites compile unchanged:

```cpp
enum class Urgency { Foreground, Background };   // P1, P2
void set_query(std::wstring q, Flags f,
               std::size_t start_page = 0,
               Urgency     urgency    = Urgency::Background);
```

- `start_page %= pages` inside `set_query` (after the `pages == 0` early
  return), so any value is safe.
- The submit loop visits `i = (start_page + k) % pages` for `k = 0 .. pages-1`.
  Everything else in the loop (task body, drain counter, submit-throw rollback
  from #133) is unchanged; only the page each iteration submits changes.

**Forward, not outward.** Enter (Find Next) is the common key and walks
forward; the next match after the page on screen arrives as early as possible.
Outward order would help only Shift+Enter.

### 3.3 Priorities: the find bar is foreground

| caller | urgency | priority |
|---|---|---|
| `MainWindow::on_find_query_changed` (`MainWindow.cpp:2220`) | `Foreground` | 1 |
| `CrossTabSearch::dispatch` (`CrossTabSearch.cpp:174`) | `Background` | 2 |

A find-bar search typed while a cross-tab scan is still queued on other tabs
runs ahead of their remaining pages. On the active tab both callers drive the
same `SearchSession`; each `set_query` bumps the epoch, so the superseded
generation's tasks exit at their first epoch check whatever their priority.

### 3.4 The start page each caller passes

Both callers pass the view's own current page, canonicalised to the left page
of the spread when that view is in two-page mode:

```cpp
const int pc  = view->page_count();
const int cur = view->current_page();
const int start = view->dual_page()
    ? litepdf::ui::dual_page_compute_left(cur, pc) : cur;
```

`MainWindow::navigate_click` already canonicalises this way
(`MainWindow.cpp:408-410`). `current_page()` is clamped to `[0, pc-1]` by
`DocumentView::set_current_page`, and a zero-page document returns before the
submit loop, so `start` needs no further guard. The right page of a spread is
`start + 1` except for the cover and an odd last page, where there is no right
page (`PdfCanvasLayout.hpp:26-31`), so forward order always scans the visible
pages first.

`CrossTabSearch.cpp` already includes `core/DocumentView.hpp` and holds each
tab's `view`. `PdfCanvasLayout.hpp` is inline-only with no `#include` of its
own, and `litepdf_core` (which builds `CrossTabSearch.cpp`, `CMakeLists.txt:74`)
already takes a `ui/` header (`core/DocumentView.cpp:16`), so including it
there adds no link dependency.

### 3.5 Cursor: anchored on first use, stable under inserts

`State` gains two fields, set in `set_query`'s existing reset block alongside
`cursor = 0`:

```cpp
std::size_t origin_page = 0;   // the start page of this generation
bool        anchored    = false;  // false until next/prev/set_reference_page places the cursor
```

- **`next()` when not anchored:** place the cursor on the first hit with
  `page >= origin_page` (wrap to `hits[0]` if none), set `anchored`, return
  it. No increment.
- **`prev()` when not anchored:** place the cursor on the hit just before the
  first hit with `page >= origin_page` (wrap to the last hit), set
  `anchored`, return it.
- **`next()` / `prev()` when anchored:** unchanged (`±1`, wrapping).
- **`set_reference_page(p)`:** sets `origin_page = p`. With hits, positions the
  cursor as today and sets `anchored`. With no hits, leaves `anchored` false,
  so the first `next()` anchors on `p`.
- **`current()`:** unchanged (`hits[cursor]`; `cursor` is 0 until anchored,
  which keeps the existing tests' meaning).
- **Insert block (#139, `:394-398`):** compute the insert index before the
  insert. If `anchored`, the list was non-empty before the insert, and
  `index <= cursor`, add the number of inserted hits to `cursor`, so it keeps
  pointing at the same hit. All of this is already under `st.m`.

The reset block is the #133 race-sensitive region; this change only adds two
plain assignments next to `cursor = 0` inside the same locked scope, and the
#133 seam test (`set_after_reset_hook`) keeps guarding it.

## 4. Files touched

| file | change |
|---|---|
| `src/app/SearchDispatcher.hpp/.cpp` | lanes + owner queues replace the priority queue; ordering contract on `ISearchDispatcher` |
| `src/core/SearchSession.hpp/.cpp` | `Urgency`, `set_query` start page + urgency, submit order, `origin_page` / `anchored`, cursor anchoring and insert stability |
| `src/ui/MainWindow.cpp` | `on_find_query_changed` passes the canonical start page, `Foreground` |
| `src/app/CrossTabSearch.cpp` | passes each tab's canonical start page, `Background` |
| `tests/unit/test_search_dispatcher.cpp` | ordering tests (§6.1) |
| `tests/unit/test_search_session.cpp` | submission order, cursor tests (§6.2) |
| `CHANGELOG.md` | `[Unreleased]` entries (§7) |

## 5. Alternatives considered

- **Plain FIFO tie-break (sequence number in `TaskCmp`).** The first draft.
  Rejected: it serialises sessions on one worker (§3.1).
- **Bump only the start page and its neighbour to P0/P1, keep the heap.** The
  Phase 6 plan's D4 (`docs/plans/2026-04-24-phase-6-search-design.md:112-118`).
  Fixes the measured symptom in a few lines, but the rest of the walk stays in
  heap order, so Find Next before the scan ends still meets pages arriving
  behind the cursor at random, and two foreground pages per search still
  compete with nothing in between.
- **Tasks pick their page at run time, nearest to a focus page the UI updates
  while scrolling.** Follows the user mid-scan, but needs a shared "next page"
  claim inside `State` and touches the task body that #133 hardened. Not
  needed for the measured symptom; revisit if a probe shows users scroll away
  during long scans and wait on highlights.
- **Outward order (start, start+1, start-1, …).** See §3.2.

## 6. Testing

TDD: each test below is written first and run RED against `main` (or against
the previous task's tree) before the code that makes it pass.

### 6.1 Dispatcher (`ThreadPoolDispatcher`, deterministic)

Tests hold the pool's workers on a gate task so everything is queued before
anything runs, then release and record the run order (one worker for exact
order; the contract is about pop order, not completion order).

1. One owner, five tasks at P2 → run in submission order. RED on main (heap
   order).
2. P1 submitted after P2 tasks → P1 runs first.
3. Two owners A, B, three P2 tasks each, all of A submitted first → runs
   A1 B1 A2 B2 A3 B3.
4. An expired owner's tasks are skipped without blocking the other owner.
5. Destruction still drains every queued task (existing test keeps passing).

### 6.2 SearchSession (`DeferredDispatcher` / `InlineDispatcher`)

Exact order is asserted only on the deterministic dispatchers; on the pool the
order of completions is not strictly FIFO (`std::mutex` handoff is not fair).

1. `set_query(q, f, start=5)` on the 7-page fixture → `DeferredDispatcher`
   records pages 5, 6, 0, 1, 2, 3, 4 (the task lambda's page is observable via
   which page's hits appear when that task runs).
2. `start` out of range (`start = 9` on 7 pages) → order starts at page 2.
3. Fable's #140 test: start 5, run page 5, `next()` → a page-5 hit; run page 0
   (12 hits inserted ahead), `next()` → still a page-5 hit. RED on main (with
   the main build's submission order, run page 5's task before page 0's).
4. First `next()` after a search lands on the first hit on or after the start
   page; first `prev()` on the last hit before it; both wrap. RED on main
   (returns `hits[1]`).
5. `set_reference_page` on an empty list, then hits arrive, then `next()` →
   anchored on the reference page.
6. Existing tests (`current()` → `hits[0]`, set_reference_page, #139 reading
   order, #133 seams) pass unchanged.

### 6.3 Real-pool check

`ThreadPoolDispatcher pool(2)`, one session on a fixture with ≥ 7 pages,
`start = 5`: page 5 is among the first two pages to complete. Run 20 times in
the test to catch an ordering that only usually holds.

### 6.4 Probe measurement (not in the PR)

The #135/#139 probe kit (`batch139.ps1`, `analyze139.py`, probe builds of main
and the branch, run alternately), extended to log the time each page's hits
land. On `html5_spec.pdf`, 3 runs each:

- page index 5 at query `e`: time from query to its hits (criterion 1);
- total scan time for `e` (criterion 2);
- a two-tab cross-tab scan's wall time vs the two single-tab times
  (criterion 3).

## 7. CHANGELOG

Under `[Unreleased]`:

- `### Changed`: a search now reads the page on screen first and continues
  forward from it, so that page's matches appear right away (with the measured
  before/after).
- `### Fixed`: the first Find Next after a new search skipped the first match;
  it now goes to the first match on or after the page on screen (and Find
  Previous to the last one before it). Find Next could also jump back to an
  earlier page when pages finished reading while the user was stepping.

## 8. Accepted risks and out of scope

- **Scrolling mid-scan does not re-order** the remaining pages (§5).
- **A cross-tab scan's tabs are scanned in turns**, not one tab at a time; the
  results panel fills for every tab at once rather than tab by tab. Same as
  today on average.
- **#137, #136** (open-failure logging, first-page render miss) are unrelated
  and stay open.
- The find counter still shows totals only (`MainWindow.cpp` TODO
  "phase-6.x ... cursor_index()"); not changed here.
