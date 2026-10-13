# Scan the page on screen first (#140) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A search scans the page on screen first and continues forward from it, and Find Next / Find Previous start from the page on screen.

**Architecture:** `ThreadPoolDispatcher` keeps one FIFO queue per owner (a session's `State`), runs at most one task per owner at a time, gives the next free worker to the least recently served ready owner, and can `discard` an owner's queued tasks. `SearchSession` submits pages from a start page forward with wrap, discards its stale queued tasks on a new query and in its destructor, and anchors the Find Next cursor on first use and keeps it on the same hit as pages are inserted.

**Tech Stack:** C++17 (MSVC, `cxx_std_17`), Win32, MuPDF 1.27.2 (vendored), Catch2, CMake/ctest from VS 2022 BuildTools.

**Spec:** `docs/superpowers/specs/2026-10-13-search-scan-order-140-design.md` (branch `perf/search-scan-order-140`, spec head `ae94c68`). Read it before any task; § numbers below refer to it.

## Global Constraints

- Branch `perf/search-scan-order-140`, baseline `main` @ `dca2d4b` (v1.5.4).
- Build and test **Release** only (Debug fails with LNK2038). Use the full BuildTools paths, never a bare `cmake`/`ctest` (a MinGW CMake is on PATH):
  - `$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"`; `ctest.exe` is in the same directory.
  - build tests: `& $cmake --build build --target litepdf_unit_tests --config Release`; build everything: `& $cmake --build build --config Release`
  - targeted run, from the repo root (fixtures resolve from there): `build\tests\Release\litepdf_unit_tests.exe "<exact test name>"`
  - full suite, from **PowerShell** at the repo root: `& $ctest --test-dir build -C Release`. Read the count with `-N` first (472 at `dca2d4b`); do not quote an old count.
- `TEST_CASE` names: pure ASCII, prefixed by subsystem — `SearchDispatcher: `, `SearchSession: `, `CrossTabSearch: `, `dual_page: ` — because `ctest -R` matches names, not tags.
- All search tasks stay at priority 2 (§3.3). No caller passes another priority.
- Every gate a test uses is opened by a scope guard declared **after** the dispatcher it blocks, so it is destroyed first and a failed `REQUIRE` cannot leave a worker blocked (§6.1). A test that destroys a `SearchSession` that might spin does it on a helper thread with a bounded wait and detaches/leaks on timeout — the pattern at `tests/unit/test_search_session.cpp:608-634`. Never `std::async` for that (its future blocks in its destructor).
- Use `CHECK`, not `REQUIRE`, before a point where queued tasks must still drain (`test_search_session.cpp:569-570`).
- Commit messages: conventional commits, English, ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

1. **Typing fast in the find bar while a long scan runs.** Each debounced retype (120 ms) and every flag toggle (immediate) starts a generation; the old one's queued tasks must vanish, not queue ahead. Pinned by Task 2's retype-discard test plus the existing `SearchSession: rapid query changes settle on correct results (ThreadPool)` staying green.
2. **Closing a tab while other tabs are scanning.** `~SearchSession` must return as soon as its one running page (if any) finishes. Pinned by Task 2's destructor-discard test and the existing `test_crosstab_ctrlw_race` cases staying green.
3. **Two-page mode on the cover or an odd last page.** The start page must be the spread's left page, so the visible pages scan first. Pinned by Task 4's `dual_page: search start page` test.
4. **Enter pressed before the start page has finished.** No hit yet: nothing happens, the session stays unanchored, and the next Enter after hits arrive anchors on the page on screen. Pinned by Task 3's no-hits test.
5. **A one-page document, or a start page past the end.** The start page is reduced modulo the page count; a one-page document scans page 0. Pinned by Task 2's `simple.pdf` test.

---

## Task 1: Dispatcher — owner queues, one task in flight per owner, discard

**Files:**
- Modify: `src/app/SearchDispatcher.hpp` (the `ISearchDispatcher` interface, the `InlineDispatcher` note at `:46-51`, `ThreadPoolDispatcher` at `:53-93`)
- Modify: `src/app/SearchDispatcher.cpp` (constructor `:40-58`, `submit` `:71-77`, `worker_loop` `:79-110`)
- Test: `tests/unit/test_search_dispatcher.cpp`

**Interfaces:**
- Produces, on `ISearchDispatcher`:
  - `virtual std::size_t discard(const std::weak_ptr<void>& owner);` — not pure; the base definition returns `0`. Comment: removes the owner's queued (not yet popped) tasks without running them and returns how many; a task already popped is unaffected.
- Produces, on `ThreadPoolDispatcher`:
  - `explicit ThreadPoolDispatcher(std::size_t num_workers = 2);` — clamps `num_workers` to at least 1; reserves `in_flight_` to that count before starting any worker.
  - `std::size_t discard(const std::weak_ptr<void>& owner) override;`
  - The ordering contract as a comment on the class (§3.1 "Ordering contract", verbatim in substance): among queued tasks whose owner is ready (none of its tasks running), the lowest priority number runs first; an owner runs one task at a time across all lanes, in submission order within a lane; within a lane the least recently served ready owner goes next; a lower lane can starve a higher one.
- Private state (§3.1): `struct OwnerQueue { std::weak_ptr<void> owner; std::deque<Task> tasks; std::uint64_t last_served = 0; };`, `std::map<std::uint8_t, std::list<OwnerQueue>> lanes_;`, `std::vector<std::weak_ptr<void>> in_flight_;`, `std::size_t queued_ = 0;`, `std::uint64_t serve_seq_ = 0;`. The `TaskCmp` and `std::priority_queue` go. Owner identity: `!a.owner_before(b) && !b.owner_before(a)`.
- `InlineDispatcher`: unchanged; its note keeps "runs in submission order, no reordering" and adds that the ordering contract above is `ThreadPoolDispatcher`'s.

- [ ] **Step 1: Land the declarations with today's behaviour.** Add `discard` to `ISearchDispatcher` (base returns 0) and declare the `ThreadPoolDispatcher` override returning 0 for now. Build: `& $cmake --build build --target litepdf_unit_tests --config Release` succeeds; existing dispatcher tests pass.

- [ ] **Step 2: Write the failing tests** in `tests/unit/test_search_dispatcher.cpp`. Add a test helper: a `Gate` (mutex + condvar + `open()` / `wait()`), a `GateGuard` that opens a list of gates in its destructor, and an `Events` recorder (mutex-guarded `std::vector<std::string>` plus `wait_for(name, 5s)` and `started(name)`). Owners are `std::shared_ptr<int>` objects; a task's `run` captures only gates/recorders, never its owner (so `expired()` behaves). Name every case `SearchDispatcher: ...`. One test per §6.1 item:
  1. `SearchDispatcher: one owner runs in submission order` — `pool(1)`, gate task of owner G first, then owner A tasks 0..4 recording `"A0".."A4"`; open the gate; order == `A0 A1 A2 A3 A4`.
  2. `SearchDispatcher: a lower priority number runs first` — `pool(1)`, gate G; B0 (P2), B1 (P2), then A0 (P1); order starts with `A0`.
  3. `SearchDispatcher: owners take turns` — `pool(1)`, gate G; A0 A1 A2 then B0 B1 B2; order == `A0 B0 A1 B1 A2 B2`.
  4. `SearchDispatcher: one task in flight per owner` — `pool(2)`; A1 blocks on gate gA1; A2 queued; after 50 ms `!started("A2")`; open gA1; `wait_for("A2")`.
  5. `SearchDispatcher: the second worker takes another owner` — `pool(2)`; A1 holds owner A's mutex and blocks on gA1; submit A2 (locks A's mutex, records) then B1 (records); `wait_for("B1")` while gA1 is still closed; `!started("A2")`.
  6. `SearchDispatcher: a new owner takes the next free worker` — `pool(2)`; A1, B1 both gated and started; A2, B2 queued; then C1; open gA1; the next recorded start is `C1`.
  7. `SearchDispatcher: a ready lower-priority owner runs while a higher one is in flight` — `pool(2)`; A1 (P1) gated and started; A2 (P1), B1 (P2) queued; `wait_for("B1")` with gA1 closed.
  8. `SearchDispatcher: a task with no run still releases its owner` — `pool(2)`; owner A: first task has an empty `run`, second records `"A1"`; `wait_for("A1")`; pool destroyed at scope end. Comment: without the release the RED run ends at ctest's 120 s TIMEOUT (accepted, §6.1).
  9. `SearchDispatcher: an expired owner's tasks release the owner` — `pool(1)` gated by G; owner E: two tasks built, `E.reset()`, `REQUIRE(t.state_weak.expired())` for both; then B0; open the gate; `wait_for("B0")`; pool destroyed at scope end. Same TIMEOUT comment.
  10. `SearchDispatcher: zero workers still run tasks` — `{ ThreadPoolDispatcher pool(0); pool.submit(task recording "A0"); }` then `started("A0")`.
  11. `SearchDispatcher: discard removes only that owner's queued tasks` — `pool(1)` gated by G; A0 A1 A2, B0 B1; `REQUIRE(pool.discard(A) == 3)`; open; order == `B0 B1`.
  12. `SearchDispatcher: discard does not count a running task` — `pool(2)`; A0 gated and started; A1, A2 queued; `REQUIRE(pool.discard(A) == 2)`; open gA0; `wait_for("A0 done")`.
  13. `SearchDispatcher: after discard a running owner stays busy` — `pool(2)`; A0 gated and started; A1 queued; `REQUIRE(pool.discard(A) == 1)`; submit A2; after 50 ms `!started("A2")` (the in-flight entry survived the erased queue); open gA0; `wait_for("A2")`.
  Keep the existing tests; the existing "skips task when weak state expired" case stays as is.

- [ ] **Step 3: Run them and record RED.** `build\tests\Release\litepdf_unit_tests.exe "SearchDispatcher: *"`. Expected FAIL on today's queue (assertions, not hangs): 1, 3, 4, 5, 6, 10, 11, 12, 13. Expected to pass already on main, kept as guards for the new code: 2 (P1 before P2), 7, 8, 9. Record the actual list in the task report; a test expected RED that passes must be examined, not ignored.

- [ ] **Step 4: Implement** the §3.1 pop/release/wait/discard rules in `SearchDispatcher.cpp`:
  - `submit`: strong guarantee — build a new lane / new `OwnerQueue` in temporaries before linking (`map::emplace`, one-element `std::list` + `splice`), `deque::push_back` into an existing queue; `++queued_`; `notify_one`.
  - pop (under `m_`, inside the existing wait): smallest-key lane with a ready owner; least `last_served` ready owner in it (ties by list order); move out its front task; `last_served = ++serve_seq_`; `in_flight_.push_back(owner)`; `--queued_`; erase an emptied queue and an emptied lane.
  - wait predicate: `(stop_ && queued_ == 0) || <some queued owner is ready>`; exit when `stop_ && queued_ == 0`.
  - release: an RAII object constructed after the `unique_lock` scope closes; its destructor locks `m_`, swap-and-pops the owner out of `in_flight_`, unlocks, `notify_all`. It covers both `continue`s, the normal return and a throw.
  - `discard`: under `m_`, erase the owner's `OwnerQueue` in every lane, erase lanes left empty, subtract from `queued_`, return the count.
  - Update the class comments (ordering contract; drain-before-exit unchanged in meaning).

- [ ] **Step 5: Run to GREEN.** `build\tests\Release\litepdf_unit_tests.exe "SearchDispatcher: *"` all pass; then the full suite from PowerShell: `& $ctest --test-dir build -C Release` — 0 failed.

- [ ] **Step 6: Commit.**
```bash
git add src/app/SearchDispatcher.hpp src/app/SearchDispatcher.cpp tests/unit/test_search_dispatcher.cpp
git commit -m "perf(app): run each search owner's tasks in order, one at a time, taking turns (#140)"
```

## Task 2: SearchSession — start page, forward-with-wrap order, discard stale tasks

**Files:**
- Modify: `src/core/SearchSession.hpp` (`set_query` declaration at `:72-74` and its comment)
- Modify: `src/core/SearchSession.cpp` (`~SearchSession` `:207-237`; `set_query` `:249-425`, including the "Priority: all tasks at P2 in v1 …" comment at `:327-328`)
- Test: `tests/unit/test_search_session.cpp` (`DeferredDispatcher` at `:405-425`; new cases)

**Interfaces:**
- Consumes: `ISearchDispatcher::discard(const std::weak_ptr<void>&) -> std::size_t` (Task 1).
- Produces: `void SearchSession::set_query(std::wstring q, Flags f, std::size_t start_page = 0);` — every existing two-argument call compiles unchanged.
- Produces (internal, used by Task 3): the reduced start page, `start = pages ? start_page % pages : 0`, computed at the top of `set_query` from `impl_->doc.page_count()` (read once, before the reset block; it takes no lock).

- [ ] **Step 1: Land the signature with today's behaviour** (parameter accepted, unused). Build succeeds; full suite passes.

- [ ] **Step 2: Give the test file's `DeferredDispatcher` a `discard` override** that replaces each queued slot whose `state_weak` is the same owner (`owner_before` both ways) with an empty `Task{}` and counts it — slots are kept, not erased, so the existing `run(i)` indices in the #133 seam tests (`:557-606`) stay valid. Add `std::size_t queued() const` (count of slots with a non-empty `run`).

- [ ] **Step 3: Write the failing tests** (`SearchSession: ...`):
  - `SearchSession: scan starts at the start page and wraps` — `DeferredDispatcher d`; `s.set_query(L"Page", {}, 5)`; for each `i` in 0..6: `d.run(i)` and find the page whose `hits_for_page` just became non-empty → sequence `5 6 0 1 2 3 4`. (Every fixture page has a `Page` hit: 2,2,1,1,2,1,1.)
  - `SearchSession: start page past the end is reduced` — start `9` on the 7-page fixture → first scanned page `2`.
  - `SearchSession: one-page document with any start page` — `simple.pdf` (1 page), `InlineDispatcher`, `set_query(L"a", {}, 5)` → `scan_complete()` and `hit_count() > 0`.
  - `SearchSession: a retype discards the previous generation` — two `set_query` calls on `DeferredDispatcher` → `CHECK(d.queued() == 7)`; `d.run_all()`; destroy the session on a helper thread with a 5 s wait (the `:608-634` pattern) → it returns. RED on main: 14 queued.
  - `SearchSession: destructor discards queued tasks` — `ThreadPoolDispatcher pool(1)`; a gate task of another owner occupies the worker (gate guard declared after the pool); a session with `set_query(L"Lorem", {})` (7 queued); destroy it on a helper thread → returns within 2 s while the gate is closed. On timeout: open the gate, join, `FAIL`.
  - `SearchSession: the start page completes first on the pool` — `ThreadPoolDispatcher pool(2)`; before `set_query`, `set_on_update` installs a callback (it runs on a worker, outside the session's lock) that checks `hits_for_page(p)` for every page and appends each page that has become non-empty to a mutex-guarded vector; `set_query(L"Page", {}, 5)`; wait (5 s bound) for `scan_complete()`; first == 5 and full order == `5 6 0 1 2 3 4`. Loop 20 times, a fresh session each time.

- [ ] **Step 4: Run and record RED.** `build\tests\Release\litepdf_unit_tests.exe "SearchSession: *"` — expected to fail on assertions: scan order, past-the-end start, retype (14 queued), destructor (timeout path), pool order. Expected to pass already (guard): the one-page case. All old cases pass.

- [ ] **Step 5: Implement** in `SearchSession.cpp`:
  - top of `set_query`: `const std::size_t pages = impl_->doc.page_count();` and the reduced `start`; remove the later read at `:301`.
  - right after the reset block's scope (after the after-reset hook fires) and before `begin_generation()`: `const std::size_t n = impl_->dispatcher.discard(impl_->state);` then `st.pending_tasks.fetch_sub(static_cast<int>(n), std::memory_order_acq_rel);` — this precedes the empty-query return, so `clear()` discards too.
  - submit loop: `const std::size_t i = (start + k) % pages;` for `k = 0 .. pages-1`; the lambda captures `i` as before.
  - replace the `:327-328` comment with: all tasks at P2; order comes from the submission order and the dispatcher's per-owner FIFO (§3.1-3.3).
  - `~SearchSession`: after `abort_active()` and before the spin, the same discard + `fetch_sub`; update the "Worst case … In practice this is microseconds" comment: queued tasks are discarded, so the spin waits only for the one task (at most) already popped.
  - Update the `set_query` comment in the header (start page; forward with wrap; previous generation's queued tasks discarded).

- [ ] **Step 6: Run to GREEN**, targeted then the full suite from PowerShell — 0 failed, including the #133 seam tests and `test_crosstab_ctrlw_race`.

- [ ] **Step 7: Commit.**
```bash
git add src/core/SearchSession.hpp src/core/SearchSession.cpp tests/unit/test_search_session.cpp
git commit -m "perf(core): scan from the start page forward and discard a superseded scan's queued pages (#140)"
```

## Task 3: Find Next cursor — anchored on first use, stable under inserts

**Files:**
- Modify: `src/core/SearchSession.hpp` (`next`, `prev`, `current`, `set_reference_page` declarations and comments at `:93-100`)
- Modify: `src/core/SearchSession.cpp` (`State` at `:36-56`; reset block `:268-274`; insert block `:390-402`; `set_reference_page` `:456-468`; `current`/`next`/`prev` `:470-492`)
- Test: `tests/unit/test_search_session.cpp` (rewrite two cases at `:102-116` and `:168-180`; new cases)

**Interfaces:**
- Consumes: the reduced start page from Task 2.
- Produces:
  - `std::optional<Hit> next(std::optional<std::size_t> from_page = std::nullopt);`
  - `std::optional<Hit> prev(std::optional<std::size_t> from_page = std::nullopt);`
  - `std::optional<Hit> current() const;` — empty while unanchored.
  - `State` fields `std::size_t origin_page = 0;` and `bool anchored = false;`, set in the reset block (`origin_page = start`, `anchored = false`).

- [ ] **Step 1: Land the signatures with today's behaviour** (`from_page` accepted, ignored). Build; full suite passes.

- [ ] **Step 2: Rewrite the two cases that pin defect (b)** (§6.2 item 7) and write the new failing cases (`InlineDispatcher` unless noted; `dolor` hits p1, p3, p6; `Lorem` 12 on p0, 3 on p5):
  - rewrite `SearchSession: dolor query walks page-then-position`: `!current()`; `next()` → p1, p3, p6.
  - rewrite `SearchSession: cursor wraps at document boundaries`: `next()` ×3 → p1, p3, p6; `next()` → p1; `prev()` → p6.
  - `SearchSession: first next anchors on the start page` — fresh query per case: (`dolor`, start 0) `next()` → p1; (`dolor`, start 2) `next()` → p3; (`dolor`, start 2) `prev()` → p1; (`dolor`, start 0) `prev()` → p6; (`dolor`, start 0) `next(4)` → p6; (`Lorem`, start 6) `next()` → page 0; (`dolor`, start 0) `next()` → p1 then `next(6)` → p3.
  - `SearchSession: start page past the end anchors on the reduced page` — (`dolor`, start 9) `next()` → p3.
  - `SearchSession: cursor stays on its hit when an earlier page lands` — `DeferredDispatcher`; `set_query(L"Lorem", {}, 5)`; `d.run(0)` (page 5); `a = next()` → page 5; `d.run(2)` (page 0, 12 hits inserted ahead); `b = next()` → page 5 and `b` is the hit after `a` (compare `geom.ul_x`). Then `d.run_all()`.
  - `SearchSession: next with no hits stays unanchored` — `DeferredDispatcher`; `set_query(L"dolor", {}, 0)`; `!next()`, `!prev()`; `d.run_all()`; `next()` → p1.
  - `SearchSession: current follows next` — `!current()` after `set_query`; `h = next()`; `current()->page == h->page`.
  Keep `walk_order`/`expected_order` and the #139 reading-order cases unchanged; they call `set_reference_page(0)` first, which anchors.

- [ ] **Step 3: Run and record RED** — the rewritten and new cases fail on today's cursor (first `next()` returns `hits[1]`; insert moves the cursor).

- [ ] **Step 4: Implement** the §3.5 rules: unanchored `next`/`prev` place the cursor relative to `from_page.value_or(origin_page)` with `std::lower_bound` on `page` and set `anchored`; anchored ones step ±1 and ignore the hint; empty list returns `std::nullopt` without anchoring; `current()` empty while unanchored; `set_reference_page` sets `anchored = !hits.empty()`; the insert block computes the index before `insert` and, if `anchored && index <= cursor`, adds `page_hits.size()` to `cursor`. Update the header comments.

- [ ] **Step 5: Run to GREEN**, targeted then the full suite from PowerShell — 0 failed.

- [ ] **Step 6: Commit.**
```bash
git add src/core/SearchSession.hpp src/core/SearchSession.cpp tests/unit/test_search_session.cpp
git commit -m "fix(core): start Find Next at the page on screen and keep its place as pages arrive (#140)"
```

## Task 4: Callers pass the page on screen

**Files:**
- Modify: `src/ui/PdfCanvasLayout.hpp` (new helper after `dual_page_compute_left`)
- Modify: `src/ui/MainWindow.cpp` (`on_find_query_changed` `:2206-2226`, `on_find_next` `:2228-2239`, `on_find_prev` `:2241-2250`)
- Modify: `src/app/CrossTabSearch.cpp` (`dispatch`, the `sess->set_query(query, flags)` at `:174`)
- Test: `tests/unit/test_dual_page_layout.cpp`, `tests/unit/test_cross_tab_search.cpp`

**Interfaces:**
- Consumes: `set_query(q, f, start_page)` (Task 2); `next(from_page)`, `prev(from_page)` (Task 3).
- Produces: `inline int search_start_page(int current_page, int page_count, bool dual_page) noexcept;` in `litepdf::ui` — `dual_page ? dual_page_compute_left(current_page, page_count) : current_page`.

- [ ] **Step 1: Write the failing tests.**
  - `dual_page: search start page is the spread's left page` — `search_start_page(0, 10, true) == 0`; `(2, 10, true) == 1`; `(9, 10, true) == 9` (odd tail: page 9 is the left of the last pair); `(4, 10, false) == 4`.
  - `CrossTabSearch: each tab scans from its own page first` — two `DocumentView`s on `search.pdf` with a `DeferredDispatcher` (copy the Task 2 double, **including its `discard` override**, into `test_cross_tab_search.cpp`'s anonymous namespace; without it a view destroyed with queued tasks would spin), `v1->set_current_page(5)`, `v2->set_current_page(2)`; `xts.dispatch(L"Page", {}, {...})`; `d.run(0)` and `d.run(7)` (v1's seven tasks are queued before v2's); `CHECK` that `v1->search().hits_for_page(5)` and `v2->search().hits_for_page(2)` are non-empty; then `d.run_all()` before the views go out of scope.

- [ ] **Step 2: Run and record RED** (compile error for the helper is expected first; add a stub returning `current_page` to get a behavioural RED for the dual-page cases, and the cross-tab case fails because both views start at page 0).

- [ ] **Step 3: Implement** the helper, then:
  - `on_find_query_changed`: `v->search().set_query(q, f, start)` with `start = static_cast<std::size_t>(search_start_page(v->current_page(), v->page_count(), v->dual_page()))`.
  - `on_find_next` / `on_find_prev`: pass the same value to `next(...)` / `prev(...)`.
  - `CrossTabSearch::dispatch`: the same from each `view` (include `ui/PdfCanvasLayout.hpp`; `litepdf_core` already takes a `ui/` header, §3.4).

- [ ] **Step 4: Run to GREEN**, then build everything (`& $cmake --build build --config Release`) and the full suite from PowerShell — 0 failed.

- [ ] **Step 5: Smoke.** From PowerShell at the repo root: `.\scripts\check-version-sync.ps1 -ExpectDev` then `.\scripts\smoke-test.ps1 -ExpectDev` — both print OK / "smoke test passed." (the find-bar and cross-tab steps exercise the new call sites).

- [ ] **Step 6: Commit.**
```bash
git add src/ui/PdfCanvasLayout.hpp src/ui/MainWindow.cpp src/app/CrossTabSearch.cpp tests/unit/test_dual_page_layout.cpp tests/unit/test_cross_tab_search.cpp
git commit -m "perf(ui): search from the page on screen and start Find Next there (#140)"
```

## Task 5: Measure, CHANGELOG, final check

**Files:**
- Modify: `CHANGELOG.md` (`## [Unreleased]`)
- Scratch only (not committed): the probe kit in session scratchpad `C:\Users\User\AppData\Local\Temp\claude\C--Users-User-projects-litepdf\44bfb59b-c756-4d7f-96d3-5b131f8c459d\scratchpad\` (`batch139.ps1`, `analyze139.py`, `t6.ps1`, `build_variant139.sh`, `A-*.cpp` probe copies)

- [ ] **Step 1: Probe builds.** Build a probe variant of `main` (`dca2d4b`) and of this branch with the kit's probe logging (copy the kit to this session's scratchpad first; follow `build_variant139.sh`), adding one log line when a page's hits land: `page_hits_landed page=<i> t=<ms since query>`. In a worktree build, junction `third_party/mupdf` and `touch` every TU that includes a rewritten header (memory: worktree build gotchas).

- [ ] **Step 2: Measure.** `html5_spec.pdf` (753 pages; path as in the kit), fit width, go to page 6 (index 5), query `e`. Pin the process to P-cores (affinity mask of the 8 P-core threads on the i7-12700) and do one discarded warm-up run per build. 3 runs per build, alternating main/branch. Record per run: the rank of page 5 among landed pages and its time after the query (criterion 1: rank 1 on the branch in every run), and total scan time (criterion 2: branch median ≤ main median × 1.05).

- [ ] **Step 3: CHANGELOG.** Under `## [Unreleased]`:
  - `### Changed` — a search now reads the page on screen first and continues forward from it, so that page's matches appear right away; give the measured before/after for page 6 of the 750-page PDF (e.g. "arrived 8.6-12.8 s after the query, now within <measured>").
  - `### Fixed` — the first Find Next after a new search skipped the first match and did not start from the page on screen; it now goes to the first match on or after the page on screen, and Find Previous to the last one before it. Find Next could also jump back to an earlier page when pages finished reading while stepping through matches.
  Keep every number to what Step 2 measured; no claim about tab close or retyping speed (not measured).

- [ ] **Step 4: Final check.** Full build, `& $ctest --test-dir build -C Release -N` (report the new count) then the full suite — 0 failed; `check-version-sync.ps1 -ExpectDev` and `smoke-test.ps1 -ExpectDev` pass.

- [ ] **Step 5: Commit.**
```bash
git add CHANGELOG.md
git commit -m "docs(changelog): #140 search from the page on screen"
```
