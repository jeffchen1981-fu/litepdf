# Scan the page on screen first (#140) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A search scans the page on screen first and continues forward from it, and Find Next / Find Previous start from the page on screen.

**Architecture:** `ThreadPoolDispatcher` keeps one FIFO queue per owner (a session's `State`), runs at most one task per owner at a time, gives the next free worker to the least recently served ready owner, and can `discard` an owner's queued tasks. `SearchSession` submits pages from a start page forward with wrap, discards its stale queued tasks on a new query and in its destructor, and anchors the Find Next cursor on first use and keeps it on the same hit as pages are inserted.

**Tech Stack:** C++17 (MSVC, `cxx_std_17`), Win32, MuPDF 1.27.2 (vendored), Catch2, CMake/ctest from VS 2022 BuildTools.

**Spec:** `docs/superpowers/specs/2026-10-13-search-scan-order-140-design.md` (branch `perf/search-scan-order-140`, spec head `4e3b031`). Read it before any task; § numbers below refer to it.

## Global Constraints

- Branch `perf/search-scan-order-140`, baseline `main` @ `dca2d4b` (v1.5.4). Line numbers below are for `dca2d4b`; find the code by name if a line has moved.
- Build and test **Release** only (Debug fails with LNK2038). Never a bare `cmake`/`ctest` (a MinGW CMake is on PATH). In every PowerShell call, set both:
  - `$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"`
  - `$ctest = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"`
  - build tests: `& $cmake --build build --target litepdf_unit_tests --config Release`; build everything: `& $cmake --build build --config Release`
  - targeted run, from the repo root (fixtures resolve from there): `build\tests\Release\litepdf_unit_tests.exe "<test name or wildcard>"`
  - full suite, from **PowerShell** at the repo root: `& $ctest --test-dir build -C Release`. Read the count with `-N` first (472 at `dca2d4b`).
- `TEST_CASE` names: pure ASCII, prefixed by subsystem — `SearchDispatcher: `, `SearchSession: `, `CrossTabSearch: `, `dual_page: ` — because `ctest -R` matches names, not tags. The five existing dispatcher tests keep their names; the `"SearchDispatcher: *"` filter does not cover them, so always finish with the full suite.
- All search tasks stay at priority 2 (§3.3). No production caller passes another priority.
- **Shared test kit** `tests/unit/search_test_kit.hpp` (created in Task 1, extended in Task 2; header-only, included by the test `.cpp` files that need it, not registered in CMake): `Gate` (`open()`, `wait()`), `GateGuard` (opens the gates it holds in its destructor), `Events` (mutex-guarded ordered list of strings: `record(name)`, `started(name)`, `wait_for(name, timeout = 5s) -> bool`, `snapshot()`), and from Task 2 `DeferredDispatcher`.
- **Test lifetimes:** declare `Gate`s and `Events` *before* the dispatcher (the pool's drain at scope end must not touch destroyed objects), and the `GateGuard` *after* it (destroyed first, so a failed `REQUIRE` never leaves a worker blocked). Before submitting work that relies on a gate or a held mutex, wait until the gated task has recorded "started". A test that destroys a `SearchSession` that might spin does it on a helper thread with a bounded wait and detaches/leaks on timeout — the pattern at `tests/unit/test_search_session.cpp:608-634`; never `std::async` for that (its future blocks in its destructor).
- **Until Task 2 lands its destructor discard, every test that queues a session's tasks on a `DeferredDispatcher` runs `d.run_all()` before the session is destroyed**, and uses `CHECK`, not `REQUIRE`, before that point (the rule at `test_search_session.cpp:571-572`); otherwise `~SearchSession` spins forever.
- Commit messages: conventional commits, English, last line `Co-Authored-By: <the model that wrote the commit> <noreply@anthropic.com>` (e.g. `Claude Opus 5.5`).

## Review Focus

1. **Typing fast in the find bar while a long scan runs.** Each debounced retype (120 ms) and every flag toggle (immediate) starts a generation; the old one's queued tasks must vanish, not queue ahead. Pinned by Task 2's retype-discard test plus the existing `SearchSession: rapid query changes settle on correct results (ThreadPool)` staying green.
2. **Closing a tab while other tabs are scanning.** `~SearchSession` must return as soon as its one running page (if any) finishes. Pinned by Task 2's destructor-discard test and the existing `test_crosstab_ctrlw_race` cases staying green.
3. **Two-page mode on the cover or an odd last page.** The start page must be the spread's left page, so the visible pages scan first. Pinned by Task 4's `dual_page: search start page` test.
4. **Enter pressed before the start page has finished.** No hit yet: nothing happens, the session stays unanchored, and the next Enter after hits arrive anchors on the page on screen. Pinned by Task 3's no-hits test.
5. **A one-page document, or a start page past the end.** The start page is reduced modulo the page count. Pinned by Task 2's `simple.pdf` and past-the-end tests and Task 3's reduced-anchor test.

---

## Task 1: Dispatcher — owner queues, one task in flight per owner, discard

**Files:**
- Create: `tests/unit/search_test_kit.hpp` (`Gate`, `GateGuard`, `Events`)
- Modify: `src/app/SearchDispatcher.hpp` (`ISearchDispatcher` `:24-45`, the `InlineDispatcher` note `:48-51`, `ThreadPoolDispatcher` `:57-94`)
- Modify: `src/app/SearchDispatcher.cpp` (constructor `:40-58`, `submit` `:71-77`, `worker_loop` `:79-110`)
- Test: `tests/unit/test_search_dispatcher.cpp`

**Interfaces:**
- Produces, on `ISearchDispatcher`: `virtual std::size_t discard(const std::weak_ptr<void>& owner);` — not pure; the base definition returns `0`. Comment: removes the owner's queued (not yet popped) tasks without running them and returns how many; a task already popped is unaffected.
- Produces, on `ThreadPoolDispatcher`:
  - `explicit ThreadPoolDispatcher(std::size_t num_workers = 2);` — clamps to at least 1; reserves `in_flight_` to that count before starting any worker.
  - `std::size_t discard(const std::weak_ptr<void>& owner) override;`
  - The ordering contract as a class comment (§3.1): among queued tasks whose owner is ready (none of its tasks running), the lowest priority number runs first; an owner runs one task at a time across all lanes, in submission order within a lane; within a lane the least recently served ready owner goes next; a lower lane can starve a higher one.
- Private state (§3.1): `struct OwnerQueue { std::weak_ptr<void> owner; std::deque<Task> tasks; std::uint64_t last_served = 0; };`, `std::map<std::uint8_t, std::list<OwnerQueue>> lanes_;`, `std::vector<std::weak_ptr<void>> in_flight_;`, `std::size_t queued_ = 0;`, `std::uint64_t serve_seq_ = 0;`. `TaskCmp` and the `std::priority_queue` go. Owner identity: `!a.owner_before(b) && !b.owner_before(a)`.
- `InlineDispatcher`: unchanged; its note keeps "runs in submission order, no reordering" and adds that the ordering contract is `ThreadPoolDispatcher`'s.

- [ ] **Step 1: Land the declarations with today's behaviour.** Add `discard` to `ISearchDispatcher` (base returns 0) and the `ThreadPoolDispatcher` override returning 0 for now. Build the test target; full suite passes.

- [ ] **Step 2: Write `search_test_kit.hpp` and the failing tests** in `tests/unit/test_search_dispatcher.cpp`. Owners are `std::shared_ptr<int>`; a task's `run` captures only gates/events/mutexes, never its owner. "Gated and started" means: the task records `"<name>"` then blocks on its gate, and the test `wait_for("<name>")`s before going on. Gate task G is gated and started before anything else is submitted.
  1. `SearchDispatcher: one owner runs in submission order` — `pool(1)`, G; A tasks 0..4 recording `"A0".."A4"`; open G; order == `G A0 A1 A2 A3 A4`.
  2. `SearchDispatcher: a lower priority number runs first` — `pool(1)`, G; B0 (P2), B1 (P2), A0 (P1); open G; `A0` comes right after `G`.
  3. `SearchDispatcher: owners take turns` — `pool(1)`, G; A0 A1 A2 then B0 B1 B2; open G; order == `G A0 B0 A1 B1 A2 B2`.
  4. `SearchDispatcher: one task in flight per owner` — `pool(2)`; A1 gated and started; A2 submitted; after 50 ms `!started("A2")`; open gA1; `wait_for("A2")`.
  5. `SearchDispatcher: the second worker takes another owner` — `pool(2)`; A1 locks owner A's `std::mutex mA`, records, blocks on gA1 — gated and started; then A2 (locks `mA`, records) and B1 (records); `wait_for("B1")` with gA1 closed and `!started("A2")`.
  6. `SearchDispatcher: a new owner takes the next free worker` — `pool(2)`; A1, B1 gated and started; A2, B2, then C1 submitted; open gA1; the next start after that is `C1`.
  7. `SearchDispatcher: a ready lower-priority owner runs while a higher one is in flight` — `pool(2)`; A1 (P1) gated and started; A2 (P1), B1 (P2); `wait_for("B1")` with gA1 closed.
  8. `SearchDispatcher: a task with no run still releases its owner` — `pool(2)`; owner A: first task with an empty `run`, second records `"A1"`; `wait_for("A1")`. Comment: without the release the pool's destructor waits on a pinned owner, so RED ends at ctest's 120 s TIMEOUT (accepted, §6.1).
  9. `SearchDispatcher: an expired owner's tasks release the owner` — `pool(1)`, G; owner E: two tasks built, `E.reset()`, `REQUIRE(t.state_weak.expired())` for both; then B0; open G; `wait_for("B0")`. Same TIMEOUT comment.
  10. `SearchDispatcher: zero workers still run tasks` — `{ ThreadPoolDispatcher pool(0); pool.submit(A0); }` then `started("A0")`.
  11. `SearchDispatcher: discard removes only that owner's queued tasks` — `pool(1)`, G; A0 A1 A2, B0 B1; `REQUIRE(pool.discard(A) == 3)`; open G; order after G == `B0 B1`.
  12. `SearchDispatcher: discard does not count a running task` — `pool(2)`; A0 gated and started; A1, A2; `REQUIRE(pool.discard(A) == 2)`; open gA0; A0 records `"A0 done"` after its gate; `wait_for("A0 done")`.
  13. `SearchDispatcher: after discard a running owner keeps its turn` — `pool(2)`; A0 and B0 gated and started; A1; `REQUIRE(pool.discard(A) == 1)`; A2, then C0 (never served); after 50 ms `!started("A2")`; open gA0; the next start is `C0`, then `A2` follows after gB0 or C0's completion (C0 not gated).
  Keep the existing dispatcher tests unchanged.

- [ ] **Step 3: Run and record RED.** `build\tests\Release\litepdf_unit_tests.exe "SearchDispatcher: *"`. Expected to fail on assertions (not hang) on today's queue: 1, 3, 4, 5, 6, 10, 11, 12, 13. Expected to pass already and kept as guards: 2, 7, 8, 9. Report the actual list; a test expected RED that passes must be examined, not ignored.

- [ ] **Step 4: Implement** the §3.1 rules in `SearchDispatcher.cpp`:
  - `submit` (strong guarantee): if the owner has a queue in the lane, `deque::push_back` into it; otherwise build a one-element `std::list<OwnerQueue>` holding the task first, then link it — `lanes_.emplace(prio, std::move(list))` when the lane is missing, else `splice` to the lane's back. No empty lane is ever created. `++queued_`; `notify_one`.
  - pop (under `m_`, inside the wait): the smallest-key lane with a ready owner; in it the ready owner with the least `last_served` (ties by list order); move its front task out; `last_served = ++serve_seq_`; `in_flight_.push_back(owner)`; `--queued_`. **Keep an emptied queue** (its owner is now in flight).
  - wait predicate: `(stop_ && queued_ == 0) || <some lane has a ready owner with tasks>`; exit when `stop_ && queued_ == 0`.
  - release: an RAII object constructed after the `unique_lock` scope closes; its destructor locks `m_`, swap-and-pops the owner out of `in_flight_`, erases that owner's queues that are empty (and lanes left empty), unlocks, `notify_all`. It covers both `continue`s, the normal return and a throw.
  - `discard`: under `m_`, clear the owner's queue in every lane, erase a cleared queue unless the owner is in `in_flight_`, erase lanes left empty, subtract from `queued_`, return the count.
  - Class comments: the ordering contract; drain-before-exit unchanged in meaning.

- [ ] **Step 5: Run to GREEN.** `"SearchDispatcher: *"` all pass; run test 5 and 6 fifty times each (`--repeat`-style loop in PowerShell) — no failure; then the full suite — 0 failed.

- [ ] **Step 6: Commit.**
```bash
git add src/app/SearchDispatcher.hpp src/app/SearchDispatcher.cpp tests/unit/search_test_kit.hpp tests/unit/test_search_dispatcher.cpp
git commit -m "perf(app): run each search owner's tasks in order, one at a time, taking turns (#140)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

## Task 2: SearchSession — start page, forward-with-wrap order, discard stale tasks

**Files:**
- Modify: `src/core/SearchSession.hpp` (`set_query` declaration and comment at `:72-74`)
- Modify: `src/core/SearchSession.cpp` (`~SearchSession` `:208-244`; `set_query` `:249-427`, including the "Priority: all tasks at P2 in v1 …" comment at `:327-328`)
- Modify: `tests/unit/search_test_kit.hpp` (gains `DeferredDispatcher`, moved from `test_search_session.cpp:405-425`)
- Test: `tests/unit/test_search_session.cpp`

**Interfaces:**
- Consumes: `ISearchDispatcher::discard(const std::weak_ptr<void>&) -> std::size_t`; `search_test_kit.hpp` `Gate`/`GateGuard`/`Events` (Task 1).
- Produces: `void SearchSession::set_query(std::wstring q, Flags f, std::size_t start_page = 0);` — every existing two-argument call compiles unchanged.
- Produces (internal, used by Task 3): the reduced start page `start = pages ? start_page % pages : 0`, computed at the top of `set_query` from `impl_->doc.page_count()` (read once there; it takes no lock).
- Produces, in `search_test_kit.hpp`: `class DeferredDispatcher final : public litepdf::app::ISearchDispatcher` with `submit`, `run(i)`, `run_all()` (as today), `std::size_t queued() const` (slots with a non-empty `run`), and `discard(owner) override`, which replaces each slot of that owner with an empty `Task{}` and counts it — slots are kept, so `run(i)` indices in the #133 seam tests (`:557-606`) stay valid.

- [ ] **Step 1: Land the signature with today's behaviour** (parameter accepted, unused). Move `DeferredDispatcher` into `search_test_kit.hpp` and add `queued()` and the `discard` override. Build; full suite passes.

- [ ] **Step 2: Write the failing tests** (`SearchSession: ...`). Every `DeferredDispatcher` case ends with `d.run_all()` (Global Constraints):
  - `SearchSession: scan starts at the start page and wraps` — `set_query(L"Page", {}, 5)`; for `i` in 0..6: `d.run(i)`, record the page whose `hits_for_page` just became non-empty; `CHECK` the sequence == `5 6 0 1 2 3 4`. (Every fixture page has a `Page` hit: 2,2,1,1,2,1,1.)
  - `SearchSession: start page past the end is reduced` — `set_query(L"Page", {}, 9)`; `d.run(0)`; `CHECK` the first scanned page is `2`; `d.run_all()`.
  - `SearchSession: one-page document with any start page` — `simple.pdf` (1 page), `InlineDispatcher`, `set_query(L"a", {}, 5)` → `scan_complete()` and `hit_count() > 0`.
  - `SearchSession: a retype discards the previous generation` — two `set_query` calls → `CHECK(d.queued() == 7)`; `d.run_all()`; destroy the session on a helper thread with a 5 s bound (the `:608-634` pattern) → it returns. RED on main: 14 queued.
  - `SearchSession: destructor discards queued tasks` — `ThreadPoolDispatcher pool(1)` with G (another owner) gated and started; a heap-allocated session with `set_query(L"Lorem", {})` (7 queued); destroy it on a helper thread → returns within 2 s while G is closed. On timeout: open G, join, `FAIL`.
  - `SearchSession: the start page completes first on the pool` — `ThreadPoolDispatcher pool(2)`; before `set_query`, `set_on_update` installs a callback (it runs on a worker, outside the session's lock) that checks `hits_for_page(p)` for every page and appends each page that has become non-empty to a mutex-guarded vector; `set_query(L"Page", {}, 5)`; wait (5 s bound) until the vector holds 7 entries — not `scan_complete()`, which turns true before the last callback runs; order == `5 6 0 1 2 3 4`. Loop 20 times, a fresh session each time.

- [ ] **Step 3: Run and record RED.** `build\tests\Release\litepdf_unit_tests.exe "SearchSession: *"` — expected to fail on assertions: scan order, past-the-end start, retype (14 queued), destructor (timeout path), pool order. Expected to pass already (guard): the one-page case. All old cases pass.

- [ ] **Step 4: Implement** in `SearchSession.cpp`:
  - top of `set_query`: `const std::size_t pages = impl_->doc.page_count();` and the reduced `start`; remove the later read at `:301`.
  - right after the reset block's scope (after the after-reset hook fires) and before `begin_generation()`: `const std::size_t n = impl_->dispatcher.discard(impl_->state);` then `st.pending_tasks.fetch_sub(static_cast<int>(n), std::memory_order_acq_rel);` — this precedes the empty-query return, so `clear()` discards too.
  - submit loop: `const std::size_t i = (start + k) % pages;` for `k = 0 .. pages-1`; the lambda captures `i` as before.
  - replace the `:327-328` comment: all tasks at P2; order comes from the submission order and the dispatcher's per-owner FIFO (§3.1-3.3).
  - `~SearchSession`: after `abort_active()` and before the spin, the same discard + `fetch_sub`; update the "Worst case … In practice this is microseconds" comment: queued tasks are discarded, so the spin waits only for the task (at most one) already popped.
  - header comment on `set_query`: start page; forward with wrap; the previous generation's queued tasks are discarded.

- [ ] **Step 5: Run to GREEN**, targeted (run the pool-order case 10 times) then the full suite — 0 failed, including the #133 seam tests and `test_crosstab_ctrlw_race`.

- [ ] **Step 6: Commit.**
```bash
git add src/core/SearchSession.hpp src/core/SearchSession.cpp tests/unit/search_test_kit.hpp tests/unit/test_search_session.cpp
git commit -m "perf(core): scan from the start page forward and discard a superseded scan's queued pages (#140)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

## Task 3: Find Next cursor — anchored on first use, stable under inserts

**Files:**
- Modify: `src/core/SearchSession.hpp` (`set_reference_page`, `current`, `next`, `prev` at `:93-100`)
- Modify: `src/core/SearchSession.cpp` (`State` `:34-54`; reset block `:268-274`; insert block `:390-402`; `set_reference_page` `:456-468`; `current`/`next`/`prev` `:470-492`)
- Test: `tests/unit/test_search_session.cpp` (rewrite `:102-116` and `:168-180`; new cases)

**Interfaces:**
- Consumes: the reduced start page from Task 2; `DeferredDispatcher` from `search_test_kit.hpp`.
- Produces:
  - `std::optional<Hit> next(std::optional<std::size_t> from_page = std::nullopt);`
  - `std::optional<Hit> prev(std::optional<std::size_t> from_page = std::nullopt);`
  - `std::optional<Hit> current() const;` — empty while unanchored.
  - `State` fields `std::size_t origin_page = 0;` and `bool anchored = false;`, set in the reset block (`origin_page = start`, `anchored = false`).

- [ ] **Step 1: Land the signatures with today's behaviour** (`from_page` accepted, ignored). Build; full suite passes.

- [ ] **Step 2: Rewrite the two cases that pin defect (b)** (§6.2 item 7) **and write the new failing cases** (`InlineDispatcher` unless noted; `dolor` hits p1, p3, p6; `Lorem` 12 on p0, 3 on p5):
  - rewrite `SearchSession: dolor query walks page-then-position`: `!current()`; `next()` → p1, p3, p6.
  - rewrite `SearchSession: cursor wraps at document boundaries`: `next()` ×3 → p1, p3, p6; `next()` → p1; `prev()` → p6.
  - `SearchSession: first next anchors on the start page` — a fresh `set_query` per case: (`dolor`, start 0) `next()` → p1; (`dolor`, start 2) `next()` → p3; (`dolor`, start 2) `prev()` → p1; (`dolor`, start 0) `prev()` → p6; (`dolor`, start 0) `next(4)` → p6; (`Lorem`, start 6) `next()` → page 0; (`dolor`, start 0) `next()` → p1 then `next(6)` → p3.
  - `SearchSession: start page past the end anchors on the reduced page` — (`dolor`, start 13 → 6) `next()` → p6. (Start 9 → 2 would expect p3, which today's `hits[1]` also returns, so it could not go RED.)
  - `SearchSession: cursor stays on its hit when an earlier page lands` — `DeferredDispatcher`; `set_query(L"Lorem", {}, 5)`; `d.run(0)` (page 5); `a = next()` → page 5; `d.run(2)` (page 0: 12 hits inserted ahead); `b = next()` → page 5 and `b` is the hit after `a` (compare `geom.ul_x`); `d.run_all()`.
  - `SearchSession: next with no hits stays unanchored` — `DeferredDispatcher`; `set_query(L"dolor", {}, 0)`; `!next()`, `!prev()`; `d.run_all()`; `next()` → p1.
  - `SearchSession: current follows next` — `!current()` after `set_query`; `h = next()`; `current()->page == h->page`.
  Keep `walk_order`/`expected_order` and the #139 reading-order cases unchanged; they call `set_reference_page(0)` first, which anchors.

- [ ] **Step 3: Run and record RED** — every rewritten and new case fails on today's cursor (first `next()` returns `hits[1]`; `current()` returns `hits[0]`; an insert moves the cursor). Report any that pass.

- [ ] **Step 4: Implement** the §3.5 rules: unanchored `next`/`prev` place the cursor relative to `from_page.value_or(origin_page)` with `std::lower_bound` on `page`, and set `anchored`; anchored ones step ±1 and ignore the hint; an empty list returns `std::nullopt` without anchoring; `current()` is empty while unanchored; `set_reference_page` sets `anchored = !hits.empty()`; the insert block computes the index before `insert` and, if `anchored && index <= cursor`, adds `page_hits.size()` to `cursor`. Update the header comments.

- [ ] **Step 5: Run to GREEN**, targeted then the full suite — 0 failed.

- [ ] **Step 6: Commit.**
```bash
git add src/core/SearchSession.hpp src/core/SearchSession.cpp tests/unit/test_search_session.cpp
git commit -m "fix(core): start Find Next at the page on screen and keep its place as pages arrive (#140)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

## Task 4: Callers pass the page on screen

**Files:**
- Modify: `src/ui/PdfCanvasLayout.hpp` (new helper after `dual_page_compute_left`)
- Modify: `src/ui/MainWindow.cpp` (`on_find_query_changed` `:2206-2226`, `on_find_next` `:2228-2239`, `on_find_prev` `:2241-2250`)
- Modify: `src/app/CrossTabSearch.cpp` (`dispatch`, the `sess->set_query(query, flags)` at `:174`)
- Test: `tests/unit/test_dual_page_layout.cpp`, `tests/unit/test_cross_tab_search.cpp`

**Interfaces:**
- Consumes: `set_query(q, f, start_page)` (Task 2); `next(from_page)`, `prev(from_page)` (Task 3); `DeferredDispatcher` from `search_test_kit.hpp` (Task 2). `DocumentView::current_page() -> int`, `page_count() -> int`, `dual_page() -> bool`, `set_current_page(int) -> bool`, `search() -> SearchSession&`.
- Produces: `inline int search_start_page(int current_page, int page_count, bool dual_page) noexcept;` in `litepdf::ui` — `dual_page ? dual_page_compute_left(current_page, page_count) : current_page`.

- [ ] **Step 1: Add the helper as a stub returning `current_page`** (so the tests compile), then write the failing tests:
  - `dual_page: search start page is the spread's left page` — `search_start_page(0, 10, true) == 0`; `(2, 10, true) == 1`; `(9, 10, true) == 9`; `(4, 10, false) == 4`.
  - `CrossTabSearch: each tab scans from its own page first` — two `DocumentView`s on `search.pdf` with one `DeferredDispatcher d`; `v1->set_current_page(5)`, `v2->set_current_page(2)`; `xts.dispatch(L"Page", {}, {{v1.get(), L"a"}, {v2.get(), L"b"}})`; `d.run(0)` and `d.run(7)` (v1's seven tasks are queued before v2's); `CHECK(!v1->search().hits_for_page(5).empty())` and `CHECK(!v2->search().hits_for_page(2).empty())`; `d.run_all()` before the views go out of scope.

- [ ] **Step 2: Run and record RED** — the dual-page case fails on `(2, 10, true)`; the cross-tab case fails because both views start at page 0.

- [ ] **Step 3: Implement** the helper, then:
  - `on_find_query_changed`: `v->search().set_query(q, f, start)` with `const auto start = static_cast<std::size_t>(litepdf::ui::search_start_page(v->current_page(), v->page_count(), v->dual_page()));`
  - `on_find_next` / `on_find_prev`: pass the same value to `next(...)` / `prev(...)`.
  - `CrossTabSearch::dispatch`: the same from each `view` (include `ui/PdfCanvasLayout.hpp`; `litepdf_core` already takes a `ui/` header, §3.4).

- [ ] **Step 4: Run to GREEN**, then build everything (`& $cmake --build build --config Release`) and the full suite — 0 failed.

- [ ] **Step 5: Liveness check.** From PowerShell at the repo root: `.\scripts\check-version-sync.ps1 -ExpectDev` then `.\scripts\smoke-test.ps1 -ExpectDev` — both pass. This only proves the app still starts and its find bar and results panel open: the smoke test never types a query, so it does not reach the changed call sites. Task 5 drives them.

- [ ] **Step 6: Commit.**
```bash
git add src/ui/PdfCanvasLayout.hpp src/ui/MainWindow.cpp src/app/CrossTabSearch.cpp tests/unit/test_dual_page_layout.cpp tests/unit/test_cross_tab_search.cpp
git commit -m "perf(ui): search from the page on screen and start Find Next there (#140)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

## Task 5: Measure, drive the call sites, CHANGELOG

Run by the controller, not a subagent. Scratch files stay in the session scratchpad; nothing but `CHANGELOG.md` is committed.

**Files:**
- Modify: `CHANGELOG.md` (`## [Unreleased]`)
- Scratch: two worktrees, a probe patcher, a driver, an analysis script (all new; the #139 kit at `C:\Users\User\AppData\Local\Temp\claude\C--Users-User-projects-litepdf\44bfb59b-c756-4d7f-96d3-5b131f8c459d\scratchpad\` is reference only — its `build_variant139.sh` targets a deleted worktree and its patch's `MainWindow.cpp` hunk does not apply after Task 4). `html5_spec.pdf` is at the path the kit's `i139.json` names.

- [ ] **Step 1: Two probe builds.** Worktrees `wt-main` (detached at `dca2d4b`) and `wt-branch` (detached at the branch head) under the session scratchpad. In each: junction `third_party/mupdf` to the main checkout's built submodule, copy `build/_deps/catch2-src`, configure with `-DFETCHCONTENT_FULLY_DISCONNECTED=ON -DFETCHCONTENT_SOURCE_DIR_CATCH2=<wt>/build/_deps/catch2-src`. Remove each junction with `cmd /c rmdir` before removing the worktree.

- [ ] **Step 2: Probe patcher.** One Python script, anchored on code text (not line numbers) so it applies to both trees, that adds a buffered logger (QPC milliseconds, flushed to `%TEMP%\probe140-<pid>.log` at process exit, one line `<ms> <tag> <int>`) and these log points: `set_query` entry in `MainWindow::on_find_query_changed` (`query 0`); after the hit insert in `SearchSession`'s task (`page_landed <page>`); where `pages_remaining` reaches 0 (`scan_done 0`); after `next()` in `MainWindow::on_find_next` (`find_next <hit page or -1>`). After patching, `touch` every TU that includes a patched header before building (stale objects crash at startup).

- [ ] **Step 3: Driver.** A PowerShell script (5.1-compatible) that, per run: backs up and restores the session file as `batch139.ps1` does; launches `litepdf.exe html5_spec.pdf` with `-PassThru` and sets `$proc.ProcessorAffinity = 0xFFFF` (logical 0-15 are the P-cores); presses PgDn 5 times to reach page index 5 (the kit's `hitpos` mode); opens the find bar and types `e`; waits for `scan_done` in the log; presses Enter once; closes the app. One discarded warm-up run per build, then 3 measured runs per build, alternating main / branch.

- [ ] **Step 4: Analysis script.** Per run: rank of page 5 among `page_landed` lines and its time after `query`; total `scan_done - query`; the `find_next` page. Print a table per build and the medians.

- [ ] **Step 5: Judge.** Criterion 1: on the branch page 5 has rank 1 in every run. Criterion 2: branch median scan time ≤ main median × 1.05. Criterion 4 (drives the new call site): on the branch the first `find_next` is page 5. **If criterion 2 fails, stop here and report both tables to the user** (spec §8, open risk); do not write the CHANGELOG or continue.

- [ ] **Step 6: CHANGELOG.** Under `## [Unreleased]`:
  - `### Changed` — a search now reads the page on screen first and continues forward from it, so that page's matches appear right away, with the measured before/after for page 6 of the 750-page PDF ("arrived 8.6-12.8 s after the query, now within <measured>").
  - `### Fixed` — the first Find Next after a new search skipped the first match and started from the start of the document; it now goes to the first match on or after the page on screen, and Find Previous to the last one before it. Find Next could also jump back to an earlier page when pages finished reading while stepping through matches.
  Every number is one Step 5 measured; no claim about tab close or retyping speed (not measured).

- [ ] **Step 7: Final check.** Full build, `& $ctest --test-dir build -C Release -N` (report the new count) then the full suite — 0 failed; `check-version-sync.ps1 -ExpectDev` and `smoke-test.ps1 -ExpectDev` pass. Remove both worktrees (junctions first).

- [ ] **Step 8: Commit.**
```bash
git add CHANGELOG.md
git commit -m "docs(changelog): #140 search from the page on screen" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```
