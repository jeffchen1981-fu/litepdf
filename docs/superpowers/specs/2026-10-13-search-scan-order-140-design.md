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
(`rg -n "set_reference_page|\.current\(\)" src` hits only their declaration,
their definition and a comment in `SearchSession.hpp/.cpp`), so the first Enter after a search
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
3. Two tabs scanning at once use both workers: while one tab's page is
   running, the other worker takes the other tab's page instead of waiting on
   the first tab (§6.1 tests 4-7, gate-based, no wall-clock thresholds).
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
    std::weak_ptr<void> owner;            // Task::state_weak of its tasks
    std::deque<Task>    tasks;            // submission order
    std::uint64_t       last_served = 0;  // 0 = never served
};
std::map<std::uint8_t, std::list<OwnerQueue>> lanes_;  // key: priority
std::vector<std::weak_ptr<void>>              in_flight_;  // reserved to num_workers
std::size_t                                   queued_ = 0;
std::uint64_t                                 serve_seq_ = 0;
```

The constructor clamps `num_workers` to at least 1 (today `0` is accepted and
creates no worker, so nothing would ever drain).

Owner identity is `same_owner(a, b) = !a.owner_before(b) && !b.owner_before(a)`:
it compares control blocks, so it stays correct after the owner expires, and
the stored `weak_ptr` keeps the control block (and so its address) alive while
any of its tasks is queued or running. Tasks with an empty `state_weak` share
one owner; the runnability check drops them at pop, as today.

- **submit:** in the lane for `t.priority`, append to the `OwnerQueue` of the
  same owner, or add a new `OwnerQueue` (never served) to the lane. `++queued_`,
  `notify_one`.
- **pop (under `m_`):** take the lane with the smallest key that has a
  *ready* owner (one not in `in_flight_`). Within it, take the ready owner
  served least recently (smallest `last_served`; never-served owners first;
  ties by list order). Pop its front task, set `last_served = ++serve_seq_`,
  add the owner to `in_flight_`, `--queued_`. Erase the `OwnerQueue` if it is
  now empty, and the lane if that is now empty. A new search first discards its
  owner's queued tasks (`discard`, below), which erases its `OwnerQueue`; the
  queue it then submits into is new and never served, so it takes the next
  free worker.
- **release:** an RAII guard constructed after the pop's `unique_lock` scope
  has closed (so its destructor can take `m_` itself) removes the owner from
  `in_flight_` and then `notify_all`s, on every exit from the worker's
  iteration: the existing "not runnable" and "no `run`" `continue`s
  (`SearchDispatcher.cpp:103-105`), a normal return, and a throw. A popped task
  that is skipped must still release its owner, or that owner is pinned forever.
  The `notify_all` on every iteration, including the last, is also what lets a
  worker waiting on an in-flight owner see `queued_ == 0` at shutdown.
- **discard (new, on `ISearchDispatcher`):**
  `virtual std::size_t discard(const std::weak_ptr<void>& owner)` removes the
  owner's queued tasks without running them and returns how many it removed.
  The base implementation returns 0 (nothing queued: `InlineDispatcher` and the
  test doubles keep working unchanged). `ThreadPoolDispatcher` erases the
  owner's `OwnerQueue` in every lane under `m_` (no-throw: erasing destroys the
  tasks, and `Task`'s destructor does not throw) and subtracts from `queued_`.
  A task already popped is not affected; it runs and decrements its own drain
  count as today.
- **wait predicate:** a worker waits until `stop_ && queued_ == 0` (then it
  exits) or some queued task's owner is ready. Drain-before-exit keeps its
  meaning: every queued task still runs before the pool's workers return.
- **Ordering contract** (stated on `ThreadPoolDispatcher`; `InlineDispatcher`
  runs on submit and keeps its own "submission order, no reordering" note):
  among queued tasks whose owner is ready, the lowest priority number runs
  first; an owner runs one task at a time across all lanes, in submission order
  within a lane; within a lane, the least recently served ready owner goes
  next. A lower lane can
  therefore starve a higher one while it has ready work; today every
  production submission is P2 (`SearchSession.cpp:333`), and this design keeps
  it that way (§3.3).
- **Exception safety.** `submit` keeps its "if it throws, `t` was neither run
  nor queued" contract (`SearchDispatcher.hpp:43-44`, which #133's rollback
  relies on): everything that allocates (a new lane via `map::emplace`, a new
  `OwnerQueue` built in a one-element temporary `std::list`, `deque::push_back`)
  either happens before anything is linked in or has the strong guarantee, and
  the temporary is linked with `splice` (no-throw). On the worker side nothing
  may throw outside the existing try/catch: popping and erasing are no-throw,
  `in_flight_` is reserved to the (clamped) worker count before any worker
  starts, so its `push_back` never allocates, and removal is a swap-and-pop.
- Unchanged: the runnability check, the try/catch around `run`,
  `InlineDispatcher`.

**Retyping and closing: stale tasks are discarded, not drained.** Every
generation of one session shares the session's `State` (`SearchSession.cpp:58`,
captured by every task at `:318-334`), so it is one owner across generations.
If a retype's tasks were queued behind the previous generation's, those stale
tasks would drain one per turn of that owner, and with two other tabs keeping
both workers busy a turn comes only when one of their pages completes (a
simulation of this pop rule: ~5 s for 752 stale tasks behind two 7 ms-per-page
tabs). The same holds for `~SearchSession`, which spins on the UI thread until
every queued task has run (`SearchSession.cpp:226-236`). So the session
discards instead:

- `set_query`, right after the reset block (epoch bumped) and before its first
  submit: `n = dispatcher.discard(state_weak)`, then
  `pending_tasks.fetch_sub(n)`. The removed tasks were all of the previous
  generations (nothing of the new one is submitted yet), and none of them ran,
  so none decrements itself.
- `~SearchSession`, after its epoch bump and before its spin: the same. The spin
  then waits only for tasks already popped, which is at most one (one in flight
  per owner).

A stale task that was already popped still exits at its epoch check as today.

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
  The task body, the drain counter and the submit-throw rollback from #133 are
  unchanged; only the page each iteration submits changes in the loop.
- All tasks stay at priority 2.

**Forward, not outward.** Enter (Find Next) is the common key and walks
forward; the next match after the page on screen arrives as early as possible.
Outward order would help only Shift+Enter.

### 3.3 No priority split

The first draft ran find-bar searches at P1 and cross-tab searches at P2, so a
find-bar search would not wait behind another tab's queued pages under strict
FIFO. With one task in flight per owner and the least recently served ready
owner going next (§3.1), a new search takes the next free worker, so a P1 lane
buys nothing. It would also cost something: while the find-bar scan has ready
work, every P2 owner — the other tabs of a cross-tab search — gets only the
worker the P1 owner cannot use. Everything stays at P2.

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
  `anchored`.) All of this is already under `st.m`, after epoch check #2: a
  worker that passes check #2 inserts into the current generation's list
  before any later `set_query` can take the lock, and one that was superseded
  before check #2 returns without touching `hits`, `cursor` or `anchored`.

Because one session's pages complete in submission order (§3.1), at any moment
during a scan the session holds the pages `start .. k` (wrapping), so the first
Enter lands on the start page's first match once that page has completed.

## 4. Files touched

| file | change |
|---|---|
| `src/app/SearchDispatcher.hpp/.cpp` | lanes, owner queues, in-flight set replace the priority queue; `discard`; worker count clamped; ordering contract on `ThreadPoolDispatcher` |
| `src/core/SearchSession.hpp/.cpp` | `set_query` start page, submit order and discard of the previous generation; `~SearchSession` discard; `origin_page` / `anchored`; `next` / `prev` page hint; `current()` while unanchored; insert stability |
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
tests, then the behaviour. "RED on main" is against MSVC's `std::priority_queue`,
the project's only toolchain, whose equal-key order is deterministic for a given
push sequence (five tasks pop 0, 2, 4, 1, 3); it is not an STL guarantee.

### 6.1 Dispatcher (`ThreadPoolDispatcher`)

No timing threshold that a correct scheduler could miss. Order tests run
`pool(1)` with a gate task (one that blocks on a flag) so everything is queued
before anything runs, then release it and record the run order. Concurrency
tests run `pool(2)`: tasks record "started" / "finished" events and block on
per-task gates; "has not started" is checked after a 50 ms grace (a wrong
scheduler starts it in microseconds; the grace can only let a wrong scheduler
pass, never fail a right one), "has started" is waited for with a 5 s timeout.
Every gate is released by a scope guard declared after the pool, so it is
destroyed first: a failed `REQUIRE` unwinds into an open gate and the pool's
destructor returns, and RED shows as an assertion failure, not as ctest's
120 s per-test `TIMEOUT` (`tests/CMakeLists.txt:120-125`).

1. One owner, five tasks at P2 → run in submission order. RED on main.
2. A P1 task submitted after P2 tasks of another owner → runs first.
3. Owners A and B, three P2 tasks each, all of A submitted first → A1 B1 A2 B2
   A3 B3. RED on main.
4. One task in flight per owner: `pool(2)`, A1 held on its gate, A2 queued →
   A2 has not started while A1 is held; it starts after A1's release. RED on
   main (A2 starts at once on the second worker).
5. The second worker takes another owner: `pool(2)`, A1 held, then A2 and B1
   queued in that order, A2 modelled as waiting on A's mutex (held by A1, like
   `scan_mtx`) → B1 starts while A1 is still held. RED on main, because main
   has no per-owner cap: the idle second worker wakes on A2's own submit and
   pops it before B1 is queued, then blocks on A's mutex (independent of heap
   order).
6. A new owner takes the next free worker: `pool(2)`, A1 and B1 both held, A2
   and B2 queued, then C1 submitted; release A1 → C1 starts before A2 and B2.
   RED on main (MSVC's heap has A2 on top).
7. A ready lower-priority owner runs while a higher-priority owner is in
   flight: A1 (P1) held, A2 (P1) queued, B1 (P2) queued → B1 starts while A1 is
   held.
8. A popped task that is skipped on the "no `run`" path still releases its
   owner: owner A's first task has no `run`, its second is live → the second
   starts (5 s wait), and the pool's destructor returns.
9. The same on the "not runnable" path: owner E (expired) has two tasks, then
   owner B one. The tasks' `run` must not capture E (the existing `make_task`
   does, which keeps the owner alive); the test asserts
   `t.state_weak.expired()` before submitting, that B's task ran, and that the
   pool's destructor returns. Without the release, E's second task stays queued
   behind a pinned owner.
   For 8 and 9 the missing-release failure keeps the pool's destructor waiting,
   so after the failed assertion the RED run ends at ctest's per-test
   `TIMEOUT`; accepted for these two tests, and stated in their comments.
10. `ThreadPoolDispatcher(0)` runs a submitted task before its destructor
    returns (clamped to one worker). RED on main.
11. `discard`: `pool(1)` held by a gate task of owner G; owners A (three
    tasks) and B (two) queued → `discard(A)` returns 3; after the gate opens
    only B's tasks run. A task of A that is already running when `discard(A)`
    is called still finishes.
12. After `discard(A)` while A has a task in flight, A's next submit does not
    start until that task finishes (the in-flight entry survives the erased
    queue), and then goes before an owner served more recently.
13. Destruction still drains every queued task (existing test).

### 6.2 SearchSession (`DeferredDispatcher` / `InlineDispatcher`)

Exact order is asserted on the deterministic dispatchers. The fixture is
`search.pdf` (7 pages); the query `Page` has at least one hit on every page
(each page's first line is "Page N", `scripts/generate-search-fixture.py`;
PyMuPDF 1.27.2 on the committed file finds 2, 2, 1, 1, 2, 1, 1), so after each
deferred task runs, the page whose `hits_for_page` grew is the page that task
scanned.

1. `set_query(L"Page", {}, 5)` → `DeferredDispatcher` tasks scan pages 5, 6,
   0, 1, 2, 3, 4.
2. `start = 9` on 7 pages → order starts at page 2; then `next()` on `dolor`
   with `start = 9` returns the p3 hit (first on or after page 2).
3. Insert stability (Fable): `Lorem`, start 5; run page 5, `next()` → a page-5
   hit; run page 0 (12 hits inserted ahead), `next()` → the next page-5 hit.
4. First-use anchoring, each case on a fresh `set_query`:
   - `dolor` (p1, p3, p6), origin 0: first `next()` → p1.
   - `dolor`, origin 2: first `next()` → p3; first `prev()` → p1.
   - `dolor`, origin 0: first `prev()` → p6 (wraps backward).
   - `dolor`, origin 0: first `next(4)` → p6 (the hint wins over the origin).
   - `Lorem` (p0 ×12, p5 ×3), origin 6: first `next()` → the first p0 hit
     (nothing on or after p6, wraps forward).
   - `dolor`, origin 0: `next()` → p1, then `next(6)` → p3 (anchored: the hint
     is ignored and the cursor steps +1).
5. `next()` / `prev()` with no hits return nothing and leave the session
   unanchored (the first `next()` after hits arrive still anchors).
6. `current()` is empty after `set_query` and equals the hit `next()` returned
   after it.
7. Rewritten on purpose (they pin defect (b)): "dolor query walks
   page-then-position" becomes `current()` empty, `next()` → p1, p3, p6; "cursor
   wraps at document boundaries" becomes `next()` ×3 → p1, p3, p6, then `next()`
   → p1, then `prev()` → p6.
8. A retype discards the previous generation's queued tasks: the test file's
   `DeferredDispatcher` gains a `discard` override over its own queue. Two
   `set_query` calls → its queue holds only the second generation's 7 tasks;
   after running them, the session's destructor returns (the drain count
   reached 0, so the discarded tasks were subtracted). RED on main (14 queued).
9. `~SearchSession` discards: `ThreadPoolDispatcher pool(1)` held by a gate
   task of another owner; a session with 7 queued tasks is destroyed on a
   `std::async` thread → it returns within 2 s while the gate is still held.
   RED on main (it spins until the gate opens; the gate guard opens it on the
   failed assertion, so RED is an assertion failure).
10. Unchanged and still passing: set_reference_page, #139 reading order, #133
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

Criterion 3 is covered by §6.1 tests 4-7; the kit has no cross-tab driver.

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
- **A tab closed while its page is mid-build** still waits for that one page:
  `~SearchSession` discards everything queued (§3.1) but cannot cut short a
  task already running; the cookie abort stops a PDF build at MuPDF's next
  poll, and formats whose build ignores the cookie finish the page (as today).
- **A cross-tab scan's tabs take turns** rather than running one tab at a time;
  the results panel fills for every tab at once.
- **Clicking a cross-tab results row** sets the canvas hit but not the session
  cursor (`MainWindow.cpp:2413-2459`), so the next Enter continues from the
  session's own cursor. Unchanged.
- The find counter still shows totals only (`MainWindow.cpp` TODO
  "phase-6.x ... cursor_index()").
- #136 and #137 are unrelated and stay open.
