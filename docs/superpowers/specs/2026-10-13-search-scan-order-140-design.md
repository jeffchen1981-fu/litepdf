# Scan the page on screen first — design

**Date:** 2026-10-13 (revised after the spec gate's round 1, same day)
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
order (MSVC pops five equal-priority tasks as 0, 2, 4, 1, 3).

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
(`:476-483`). No code in `src/` calls `set_reference_page` or `current()`
(`rg -n "set_reference_page|\.current\(\)" src` hits only their declaration and
definition in `SearchSession.hpp/.cpp`), so the first Enter after a search
returns `hits[1]`: the second match on the lowest page found so far, not a
match on the page on screen.

## 2. Goal and success criteria

**Goal:** a search scans the page on screen first and continues forward from
it, and Find Next / Find Previous start from the page on screen.

**Success criteria:**

1. Same setup as §1.1 (page index 5, query `e`): page 5 is the first page of
   the search to complete, in every run. Its arrival time after the query is
   reported (not a gate).
2. Total scan time for `e` on `html5_spec.pdf`: the branch's median is at most
   5 % above main's, over 3 runs each, run alternately, with a warm file and the
   process pinned to P-cores (see §6.4).
3. Two tabs scanning at once use both workers, and a tab with cheap pages is not
   held back by one with expensive pages (§6.1 tests 5 and 6, timing-based unit
   tests with wide margins).
4. After a new search, the first Enter goes to the first match on or after the
   page on screen at the moment of the Enter, and the first Shift+Enter to the
   last match before it; both wrap. While the scan is still running, pages
   before the start page have not arrived yet, so a first Shift+Enter then
   wraps to the last match found so far.
5. Pressing Enter while the scan is still running never moves to a match on a
   page before the current match's page, except by wrapping past the last
   match found so far.

## 3. Design

### 3.1 Dispatcher: owners take turns, one task in flight per owner

`ThreadPoolDispatcher` replaces its `std::priority_queue` with an internal
structure; `ISearchDispatcher::Task` and `submit` keep their signatures.

```cpp
// private to ThreadPoolDispatcher
struct OwnerQueue {
    std::weak_ptr<void> owner;   // Task::state_weak of its tasks
    std::deque<Task>    tasks;   // submission order
};
std::map<std::uint8_t, std::list<OwnerQueue>> lanes_;  // key: priority
std::vector<std::weak_ptr<void>>              in_flight_;  // reserved to num_workers
std::size_t                                   queued_ = 0;
```

Owner identity is `same_owner(a, b) = !a.owner_before(b) && !b.owner_before(a)`:
it compares control blocks, so it stays correct after the owner expires, and
the stored `weak_ptr` keeps the control block (and so its address) alive while
any of its tasks is queued or running. Tasks with an empty `state_weak` share
one owner; the runnability check drops them at pop, as today.

- **submit:** in the lane for `t.priority`, append to the `OwnerQueue` of the
  same owner, or add a new `OwnerQueue` at the back of the lane. `++queued_`,
  `notify_one`.
- **pop (under `m_`):** walk the lanes from the smallest key. In each lane walk
  the `OwnerQueue`s from the front and take the first one whose owner is not in
  `in_flight_`. Pop its front task, add its owner to `in_flight_`, `--queued_`.
  If its queue is now empty, erase it; otherwise move it to the back of the lane.
  Erase an empty lane.
- **after `run` returns (or throws):** under `m_`, remove the owner from
  `in_flight_`, then `notify_all` (a waiting worker may now be able to pop that
  owner's next task).
- **wait predicate:** a worker waits until `stop_ && queued_ == 0` (then it
  exits) or some queued task's owner is not in flight. Drain-before-exit keeps
  its meaning: every queued task still runs before the pool's workers return.
- **Ordering contract** (stated on `ISearchDispatcher`): a lower priority number
  runs first; within a priority, one owner's tasks run one at a time in
  submission order, and owners take turns.
- **Exception safety.** `submit` keeps its "if it throws, `t` was neither run
  nor queued" contract (`SearchDispatcher.hpp:43-44`, which #133's rollback
  relies on): everything that allocates (a new lane via `map::emplace`, a new
  `OwnerQueue` built in a one-element temporary `std::list`, `deque::push_back`)
  either happens before anything is linked in or has the strong guarantee, and
  the temporary is linked with `splice` (no-throw). On the worker side nothing
  may throw outside the existing try/catch: rotation is `list::splice`,
  `in_flight_` was reserved to `num_workers` at construction so its
  `push_back` never allocates, and removal is a swap-and-pop.
- Unchanged: the runnability check, the try/catch around `run`,
  `InlineDispatcher` (it runs on submit and has no queue to order).

**Why one task in flight per owner.** Each session's scan holds its own
`scan_mtx` for the whole page (`SearchSession.cpp:357-371`, a blocking
`lock_guard`); per-tab parallelism is 1 by design (#130 spec §3.1,
`2026-10-11-search-scan-own-document-130-design.md:96`). Without the cap, a
worker can pop an owner whose previous task is still running on the other
worker and block on that lock while another tab has work. With it, the second
worker always takes a different owner, `scan_mtx` is never contended between
workers, and one owner's tasks complete in submission order — so the start
page is always the first page of a search to complete.

The number of owners in a lane is the number of tabs with a scan queued, so the
lookups in `submit` and pop walk a handful of entries.

### 3.2 Submission order: from the start page, forward, wrapping

`set_query` gains one defaulted parameter, so `clear()` and every existing call
site (all pass two arguments) compile unchanged:

```cpp
void set_query(std::wstring q, Flags f, std::size_t start_page = 0);
```

- `page_count()` (no lock, `SearchSession.hpp:20-22`) is read at the top of
  `set_query`, before the reset block, and the start page is reduced there:
  `start = pages ? start_page % pages : 0`. The reset block stores it as
  `origin_page` (§3.5), so the scan and the cursor use the same page.
- The submit loop visits `i = (start + k) % pages` for `k = 0 .. pages-1`.
  Everything else in the loop (task body, drain counter, submit-throw rollback
  from #133) is unchanged; only the page each iteration submits changes.
- All tasks stay at priority 2.

**Forward, not outward.** Enter (Find Next) is the common key and walks
forward; the next match after the page on screen arrives as early as possible.
Outward order would help only Shift+Enter.

### 3.3 No priority split

The first draft ran find-bar searches at P1 and cross-tab searches at P2, so a
find-bar search would not wait behind another tab's queued pages under strict
FIFO. With owners taking turns and one task in flight per owner (§3.1), a new
search already gets a worker at the next pop. A P1 lane would instead starve
every P2 owner while it has work — including a closing tab's stale tasks, which
`~SearchSession` spins on, on the UI thread (`SearchSession.cpp:226-236`) — so
everything stays at P2.

### 3.4 The start page each caller passes

`on_find_query_changed` (`MainWindow.cpp:2220`) and `CrossTabSearch::dispatch`
(`CrossTabSearch.cpp:174`) pass the view's own current page, canonicalised to
the left page of the spread when that view is in two-page mode:

```cpp
const int pc  = view->page_count();
const int cur = view->current_page();
const int start = view->dual_page()
    ? litepdf::ui::dual_page_compute_left(cur, pc) : cur;
```

`MainWindow::navigate_click` canonicalises the same way (`MainWindow.cpp:408-410`,
reading `canvas_->dual_page()`, which is kept equal to the active view's flag at
`:788-789` and `:1726-1727`). `current_page()` is clamped to `[0, pc-1]` by
`DocumentView::set_current_page`, and a zero-page document returns before the
submit loop. The right page of a spread is `start + 1` except for the cover and
an odd last page, where there is none (`PdfCanvasLayout.hpp:26-31`), so forward
order scans the visible pages first.

`on_find_next` / `on_find_prev` (`MainWindow.cpp:2228-2250`) pass the same
canonical current page to `next()` / `prev()` (§3.5), so the first Enter uses
the page on screen when it is pressed, not when the query was typed.

`PdfCanvasLayout.hpp` is inline-only with no `#include` of its own, and
`litepdf_core` (which builds `CrossTabSearch.cpp`, `CMakeLists.txt:74`) already
takes a `ui/` header (`core/DocumentView.cpp:16`), so `CrossTabSearch.cpp` can
include it without a new link dependency.

### 3.5 Cursor: anchored on first use, stable under inserts

`State` gains two fields, set in `set_query`'s existing reset block alongside
`cursor = 0` (two plain assignments inside the same locked scope; the #133 seam
test `set_after_reset_hook` keeps guarding that block):

```cpp
std::size_t origin_page = 0;      // this generation's (reduced) start page
bool        anchored    = false;  // false until the cursor is placed on a hit
```

`next()` and `prev()` gain an optional page hint:

```cpp
std::optional<Hit> next(std::optional<std::size_t> from_page = std::nullopt);
std::optional<Hit> prev(std::optional<std::size_t> from_page = std::nullopt);
```

- **No hits:** return `std::nullopt` and stay unanchored.
- **`next` when not anchored:** let `p = from_page.value_or(origin_page)`.
  Place the cursor on the first hit with `page >= p` (wrap to `hits[0]` if
  none), set `anchored`, return it. No increment.
- **`prev` when not anchored:** place the cursor on the hit just before the
  first hit with `page >= p` (wrap to the last hit), set `anchored`, return it.
- **When anchored:** unchanged (`±1`, wrapping); the hint is ignored.
- **`current()`:** `std::nullopt` while not anchored (no match is selected after
  a new search, matching `on_find_query_changed`'s `set_current_hit(nullopt)`);
  otherwise `hits[cursor]`. No code in `src/` calls it.
- **`set_reference_page(p)`:** positions the cursor as today and sets
  `anchored` when there are hits. No code in `src/` calls it.
- **Insert block (#139, `:394-398`):** compute the insert index before the
  insert. If `anchored` and `index <= cursor`, add the number of inserted hits
  to `cursor`, so it keeps pointing at the same hit. (`anchored` implies the
  list was non-empty, and only `set_query` empties it, which also clears
  `anchored`.) All of this is already under `st.m`, after epoch check #2, so a
  superseded worker never reaches it.

Because one session's pages complete in submission order (§3.1), at any moment
during a scan the session holds the pages `start .. k` (wrapping), so the first
Enter lands on the start page's first match once that page has completed.

## 4. Files touched

| file | change |
|---|---|
| `src/app/SearchDispatcher.hpp/.cpp` | lanes, owner queues, in-flight set replace the priority queue; ordering contract on `ISearchDispatcher` |
| `src/core/SearchSession.hpp/.cpp` | `set_query` start page and submit order; `origin_page` / `anchored`; `next` / `prev` page hint; `current()` while unanchored; insert stability |
| `src/ui/MainWindow.cpp` | canonical current page to `set_query`, `next`, `prev` |
| `src/app/CrossTabSearch.cpp` | each tab's canonical current page to `set_query` |
| `tests/unit/test_search_dispatcher.cpp` | §6.1 |
| `tests/unit/test_search_session.cpp` | §6.2, §6.3; two existing tests rewritten |
| `CHANGELOG.md` | `[Unreleased]` entries (§7) |

## 5. Alternatives considered

- **Plain FIFO tie-break (a sequence number in `TaskCmp`).** The first draft.
  Rejected: both workers take consecutive pages of one session and one of them
  blocks on its `scan_mtx`, so a cross-tab search runs on one worker.
- **Turns without the in-flight cap.** Rejected in round 1: a worker can still
  pop an owner that is running on the other worker and block, so a tab with
  cheap pages advances one page per expensive page of another tab.
- **P0 for the current page, P1 for ±1..±5, P2 for the rest, keep the heap.**
  The Phase 6 plan's D4 (`docs/plans/2026-04-24-phase-6-search-design.md:112-118`).
  Fixes the measured symptom in a few lines, but the rest of the walk stays in
  heap order, so Find Next before the scan ends still meets pages arriving
  behind the cursor at random, and two workers still contend on one session.
- **Tasks pick their page at run time, nearest to a focus page the UI updates
  while scrolling.** Follows the user mid-scan, but needs a shared "next page"
  claim inside `State` and touches the task body that #133 hardened. Not
  needed for the measured symptom.
- **Outward order (start, start+1, start-1, …).** See §3.2.

## 6. Testing

TDD: each test is written first and run RED before the code that makes it
pass. RED means a behavioural failure, not a compile error: the plan lands the
new signatures (start page, page hint) with today's behaviour first, then the
tests, then the behaviour.

### 6.1 Dispatcher (`ThreadPoolDispatcher`)

Order tests run `pool(1)` with a gate task (a task that blocks on a flag) so
everything is queued before anything runs, then release it and record the run
order.

1. One owner, five tasks at P2 → run in submission order. RED on main.
2. A P1 task submitted after P2 tasks → runs first.
3. Owners A and B, three P2 tasks each, all of A submitted first → A1 B1 A2 B2
   A3 B3. RED on main.
4. One task in flight per owner: `pool(2)`, one owner, eight tasks that each
   increment an atomic "running" count, record the maximum, sleep 5 ms,
   decrement → maximum is 1. RED on main (2).
5. Two owners use both workers: `pool(2)`, owners A and B, ten tasks each,
   each task holds its owner's mutex (modelling `scan_mtx`) for 20 ms → the
   whole batch finishes in under 300 ms (serial: 400 ms; ideal: 200 ms).
6. A cheap owner is not held back: `pool(2)`, A's ten tasks take 1 ms, B's ten
   take 50 ms, each holding its owner's mutex → A's last task completes within
   150 ms of release (turns without the cap: ~450 ms). Expected RED on main,
   where heap order puts some of A's tasks behind most of B's; confirmed when
   the test is written, and if main happens to pass, the test is kept as a
   regression guard for the cap and recorded as such.
7. An expired owner's tasks are skipped and do not block another owner. The
   task's `run` must not capture the owner (the existing `make_task` does, which
   keeps the owner alive); the test asserts `t.state_weak.expired()` before
   submitting, so it cannot pass vacuously.
8. Destruction still drains every queued task (existing test).

### 6.2 SearchSession (`DeferredDispatcher` / `InlineDispatcher`)

Exact order is asserted on the deterministic dispatchers. The fixture is
`search.pdf` (7 pages); the query `Page` has at least one hit on every page
(each page's first line is "Page N", `scripts/generate-search-fixture.py`), so
after each deferred task runs, the page whose `hits_for_page` grew is the page
that task scanned.

1. `set_query(L"Page", {}, 5)` → `DeferredDispatcher` tasks scan pages 5, 6,
   0, 1, 2, 3, 4.
2. `start = 9` on 7 pages → order starts at page 2; then `next()` on `dolor`
   with `start = 9` returns the p3 hit (first on or after page 2).
3. Insert stability (Fable): `Lorem`, start 5; run page 5, `next()` → a page-5
   hit; run page 0 (12 hits inserted ahead), `next()` → the next page-5 hit.
4. First `next()` after a search lands on the first hit on or after the
   origin; `next(6)` lands on the first hit on or after page 6; first `prev()`
   on the last hit before the origin; both wrap.
5. `next()` / `prev()` with no hits return nothing and leave the session
   unanchored (the first `next()` after hits arrive still anchors).
6. `current()` is empty after `set_query` and equals the hit `next()` returned
   after it.
7. Rewritten on purpose (they pin defect (b)): "dolor query walks
   page-then-position" becomes `current()` empty, `next()` → p1, p3, p6; "cursor
   wraps at document boundaries" becomes `next()` ×3 → p1, p3, p6, then `next()`
   → p1, then `prev()` → p6.
8. Unchanged and still passing: set_reference_page, #139 reading order, #133
   seams, cross-tab tests.

### 6.3 Real pool

`ThreadPoolDispatcher pool(2)`, one session, `Page`, `start = 5`: the first
page whose hits appear is page 5, and pages complete in the order 5, 6, 0, … 4
(one owner runs one task at a time, §3.1). Repeated 20 times in the test.

### 6.4 Probe measurement (not in the PR)

The #135/#139 probe kit (`batch139.ps1`, `analyze139.py`, probe builds of main
and the branch, run alternately; in session scratchpad `44bfb59b…`), extended
to log the time each page's hits land. On `html5_spec.pdf`, warm file (one
discarded run first), process pinned to P-cores (the bench-noise rule:
E-cores run the same MuPDF work ~1.9x slower), 3 runs each:

- page index 5 at query `e`: rank among completed pages and time from the
  query (criterion 1);
- total scan time for `e`, median vs main's median (criterion 2).

Criterion 3 is covered by §6.1 tests 5 and 6; the kit has no cross-tab driver.

## 7. CHANGELOG

Under `[Unreleased]`:

- `### Changed`: a search now reads the page on screen first and continues
  forward from it, so that page's matches appear right away (with the measured
  before/after).
- `### Fixed`: the first Find Next after a new search skipped the first match
  and started from the start of the document; it now goes to the first match on
  or after the page on screen, and Find Previous to the last one before it.
  Find Next could also jump back to an earlier page when pages finished reading
  while the user was stepping through matches.

## 8. Accepted risks and out of scope

- **Scrolling mid-scan does not re-order** the remaining pages (§5).
- **Closing a tab while two or more other tabs are scanning** still makes
  `~SearchSession` wait for its stale queued tasks: they drain one per turn, and
  a turn comes round each time a live page completes, so up to roughly (queued
  pages × a live page's time ÷ 2). The mechanism is the same on main (heap
  order) and its size there was never measured. Follow-up: let the session
  discard its queued tasks on destruction (new issue, filed with this PR).
- **A cross-tab scan's tabs take turns** rather than running one tab at a time;
  the results panel fills for every tab at once.
- **Clicking a cross-tab results row** sets the canvas hit but not the session
  cursor (`MainWindow.cpp:2413-2459`), so the next Enter continues from the
  session's own cursor. Unchanged.
- The find counter still shows totals only (`MainWindow.cpp` TODO
  "phase-6.x ... cursor_index()").
- #136 and #137 are unrelated and stay open.
