# An FB2 tab shares one laid-out document (#229, last item) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An FB2 tab keeps one `fz_document`, the `Document`'s own. The render worker and the search session borrow it through a priority gate that replaces `doc_mutex`. The deferred search open of #236 is removed.

**Architecture:** A `detail::PriorityGate` (Foreground > Render > Scan, FIFO tickets per class, one Scan admitted after 4 Render holds) replaces `Document::Impl::doc_mutex` for every Document. A core-internal seam, `detail::SharedDocumentAccess::with_document`, lends the Document's `fz_document*` to a caller's own context for one gate hold. Every `fz_page` is loaded, used and dropped inside that one hold. `RenderEngine` and `SearchSession` take a `DocumentShare` (`Own` keeps today's code path; `Shared` opens no document and goes through the seam). `ui::make_document_view` shares when `doc.is_fb2() && !password`.

**Tech Stack:** C++17 (`cmake/CompilerFlags.cmake`), MuPDF 1.27.2 (with the local #234 patch), Win32, CMake + MSBuild (VS 2022 BuildTools), Catch2.

**Spec:** `docs/superpowers/specs/2026-11-15-fb2-shared-document-229-design.md` (§ numbers below refer to it). Executors read both.

## Global Constraints

- Release builds only. **Every `cmake` and `ctest` means the BuildTools binary by full path** (`& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"`, `ctest.exe` in the same folder). The bare names on PATH are a MinGW CMake. Full `ctest` runs serially (no `-j`), from PowerShell, from the worktree root. The unit test exe is `& .\build\tests\Release\litepdf_unit_tests.exe "<name or [tag]>"`, run from the worktree root so fixtures resolve.
- Worktree build: `third_party\mupdf` is a junction to `C:\Users\User\projects\litepdf\third_party\mupdf` (main already carries the #234 patch). While the junction exists, `git commit -a` fails: stage explicit paths. Remove the junction with `cmd /c rmdir third_party\mupdf` and recreate the empty dir before the worktree is cleaned up.
- TEST_CASE names: ASCII, prefixed by subsystem, exactly the names in spec §6.5 for S1-S4, G1-G9, B1-B3, C1 (a case may be split, keeping the name as a prefix). `ctest -R "^<name>"` must find each one.
- C++17: no `std::latch`, `std::barrier`, `std::jthread`, or `.contains` on standard containers.
- Catch2 assertions run on the test's main thread only. A helper thread records its outcome into state the main thread reads (a `shared_ptr` for anything passed to `finishes_within`), and the main thread asserts. Every `std::thread` a case starts is owned by a `test_kit::JoinAll` declared BEFORE the case's `GateGuard`, so on a failed REQUIRE the guard opens the latches first and the threads are then joined, never left joinable (which would be `std::terminate`).
- No sleep is an oracle. A wait for a state is a deadline poll (`steady_clock`, 5 s). A test whose mutant can deadlock runs the blocking part through `finishes_within` (Task 2), whose helper thread is detached with heap-owned state, so a failure is reported and never hangs the test process.
- MuPDF hygiene: no C++ object with a destructor and no C++ throw inside `fz_try` (#179); every `fz_catch` that does not rethrow clears the error with `fz_report_error` or `fz_convert_error` (#146); `fz_var` a variable assigned in a `fz_try` and read after a throw, and only those (#178).
- **Only MuPDF calls and copying their results run under the gate**: no callback into UI or caller code, no message pump, no wait on another thread, no other lock taken (the MuPDF family lock table aside). `scan_mtx` may be held while acquiring the gate; nothing else may (§3.3).
- **No `fz_page*` leaves a `with_document` call.** Load, use and drop in the same `fn` (§3.2).
- A password is never kept past a constructor (Phase 8 D3). A `Shared` engine or session takes no copy of one.
- `DocumentShare::Own` is today's code path. Existing tests of PDF, ePub, CBZ, XPS and SVG stay green unchanged, except the ones this plan names.
- All code, comments, commit messages and CHANGELOG text in English. Commit subjects follow the repo style, e.g. `perf(core): ... (#229)`, and end with the `Co-Authored-By` line.

## Review Focus

1. **Closing an FB2 tab while its search scans and its worker renders.** The tab must close promptly: `~SearchSession` spins on a scan task that may be waiting for the gate, and `~RenderEngine` joins a worker that may hold it. Task 6 adds `MakeDocumentView: closing an FB2 view mid-search and mid-render returns`.
2. **Printing an FB2 page while the worker renders.** The printed bytes must equal a quiet print's. Task 7 adds `SharedDocument: a print during renders equals a quiet print`.
3. **A page that fails to load in a shared engine.** The failure is caught inside `fn`; the gate must be released and the next page must render. Task 4 adds `RenderEngine: a shared worker's failed page releases the gate`.
4. **A shared session destroyed before and after scanning.** Its escrow context must be released either way. Task 5 converts the #236 context-release test to `SearchSession: a shared session releases its context, scanned or not`.
5. **A shared engine destroyed with requests still queued.** Every context it cloned must be released and no worker may be left on the gate. Task 4 adds `RenderEngine: a shared engine releases its contexts`.

---

## Task 1: Worktree build and baseline

No commit.

**Files:** none in the repo.

**Interfaces:**
- Produces: a configured and built `build\` for this worktree; the baseline ctest count, recorded in the task report.

- [ ] **Step 1: Junction MuPDF**

`build\CMakeCache.txt` exists. Check that its `CMAKE_HOME_DIRECTORY` is this worktree; if it is another tree, delete nothing and reconfigure into `build\` per the build-commands memory. Then, from the worktree root in PowerShell: `Remove-Item third_party\mupdf` alone in its call (it is empty), then `cmd /c mklink /J third_party\mupdf C:\Users\User\projects\litepdf\third_party\mupdf`.
Expected: `Test-Path third_party\mupdf\include\mupdf\fitz.h` is `True`.

- [ ] **Step 2: Build and run the full suite**

`cmake --build build --config Release` (background; if it fails once with MSB8066 on `mupdf_ext`, rebuild `--target mupdf_ext`, then the full build). Then `ctest --test-dir build -C Release` serially.
Expected: 0 build errors; every test passes. Record the count (`ctest -N`).

---

## Task 2: The priority gate

**Files:**
- Create: `src/core/detail/PriorityGate.hpp`, `src/core/PriorityGate.cpp`
- Modify: `CMakeLists.txt` (`litepdf_core` sources, next to `src/core/Document.cpp`)
- Create: `tests/unit/gate_test_kit.hpp`, `tests/unit/test_priority_gate.cpp`
- Modify: `tests/CMakeLists.txt` (add `unit/test_priority_gate.cpp  # #229 priority gate`)

**Interfaces:**
- Produces (namespace `litepdf::core::detail`, header MuPDF-free):
  - `enum class GateClass { Foreground, Render, Scan };`
  - `inline constexpr unsigned kScanAfterRenders = 4;`
  - `struct GateState { bool held = false; std::uint64_t next_ticket[3] = {}; std::uint64_t serving[3] = {}; unsigned renders_since_scan = 0; };` and the pure admission rule `bool admissible(const GateState& s, GateClass cls, std::uint64_t ticket) noexcept;` (spec §3.3's `admissible`, `!held` included). The gate calls it under its mutex; tests call it directly, so a rule defect fails deterministically instead of by thread wake order.
  - `class PriorityGate` (not copyable or movable): `void acquire(GateClass cls);` `void release() noexcept;` `std::size_t waiting(GateClass cls) const;` (test observability: tickets taken minus served). Its state is one `GateState`.
  - `class GateHold` (RAII, not copyable or movable): `GateHold(PriorityGate&, GateClass);` releases in its destructor.
- Produces (tests, namespace `litepdf::test_kit`, header-only):
  - `bool finishes_within(std::chrono::milliseconds limit, std::function<void()> fn);` runs `fn` on a detached thread whose state (a done flag, mutex and condvar) is held by a `shared_ptr` the thread keeps. Returns false on timeout, leaving the thread blocked. Each Catch2 case runs in its own process (`catch_discover_tests`), so a leaked thread dies with it. A `fn` passed here must not touch objects on the caller's stack after a timeout: callers give it heap-owned objects (`std::shared_ptr`) or objects that outlive the process (a leaked `new`).
  - `bool poll_until(const std::function<bool()>& pred, std::chrono::milliseconds limit = std::chrono::seconds(5));`
  - `class JoinAll` (holds `std::vector<std::thread>`; `std::thread& add(std::thread)`; joins every joinable thread in its destructor).

- [ ] **Step 1: Write the failing tests** in `tests/unit/test_priority_gate.cpp`, tag `[core][gate]`

Pattern for each case: a holder thread takes the gate and parks on a `test_kit::Gate` latch (from `search_test_kit.hpp`). Waiter threads each `acquire(cls)`, `events.record("<tag>")` (`test_kit::Events`), `release()`. The test polls `waiting(cls)` with `poll_until` until every waiter is seen waiting, in the stated order (start a waiter only after the previous one is seen), then opens the holder's latch, joins the waiters, and checks the recorded order.

| TEST_CASE | holder | waiters, in arrival order | expected order |
|---|---|---|---|
| `PriorityGate: a foreground waiter goes before waiting render and scan` | Foreground | Scan, Render, Foreground | F, R, S |
| `PriorityGate: a foreground arriving after a waiting scan goes first` | Render | Scan, Foreground | F, S |
| `PriorityGate: render goes before scan` | Foreground | Scan, Render | R, S |
| `PriorityGate: waiters of one class are served in arrival order` | Foreground | F1..F5; then (second section) R1..R5; each section repeated for 50 rounds | arrival order in every round |

`PriorityGate: waiters of one class are served in arrival order` also has a first, pure section that does not depend on wake order. With `GateState s{}` where `s.next_ticket[F] = 2` and `s.serving[F] = 0`, `admissible(s, Foreground, 0)` is true and `admissible(s, Foreground, 1)` is false. The same holds for Render.

Further cases:
- `PriorityGate: a waiting scan is admitted after four render holds`: on a fresh gate, the holder takes Foreground. S (Scan) waits, then R1 waits. Release the holder. Each R*k* that runs parks on its own latch until R*k+1* is seen waiting, then records and releases; R1..R5 are started this way. Expected order: R1, R2, R3, R4, S, R5.
- `PriorityGate: re-entry on the owning thread throws`: run through `finishes_within(5 s, ...)` on a heap-allocated gate: `acquire(Foreground)`, then a second `acquire(Render)` inside try/catch that records whether it threw `std::system_error` with code `std::errc::resource_deadlock_would_occur`, then `release()`. The main thread checks that `finishes_within` returned true and the recorded flag is set.
- `PriorityGate: a failed re-entry leaves the gate usable`: on a heap gate, thread T: `acquire(Foreground)`, catch the throw of a second `acquire(Foreground)`, `release()`. Then `finishes_within(5 s)` of another thread's `acquire(Render); release(); acquire(Scan); release();`, and `waiting(Foreground) == 0`.
- `PriorityGate: a throw under a hold releases it`: `try { GateHold h(g, GateClass::Foreground); throw std::runtime_error("x"); } catch (...) {}`, then `finishes_within(5 s)` of another thread's `GateHold(g, GateClass::Render)`.

- [ ] **Step 2: Run them to see them fail**

Build `litepdf_unit_tests`. Expected: compile failure (no `core/detail/PriorityGate.hpp`).

- [ ] **Step 3: Implement the gate**

Members: `std::mutex m; std::condition_variable cv; GateState st; std::thread::id owner;`. `acquire` follows spec §3.3 exactly, re-entry check first:

```
acquire(cls): unique_lock lk(m);
    if (st.held && owner == this_thread::get_id())
        throw std::system_error(std::make_error_code(std::errc::resource_deadlock_would_occur));
    const auto my = st.next_ticket[cls]++;
    cv.wait(lk, [&]{ return admissible(st, cls, my); });
    ++st.serving[cls]; st.held = true; owner = this_thread::get_id();
    if (cls == Render) ++st.renders_since_scan; else if (cls == Scan) st.renders_since_scan = 0;
release(): { lock_guard lk(m); st.held = false; owner = {}; } cv.notify_all();
admissible(s, cls, ticket): !s.held && (spec §3.3's four clauses, with my = ticket and
    waiting(c) = s.next_ticket[c] - s.serving[c]).
```

The header comment states: the class order and its bounds (§3.3), the invariant (only MuPDF calls and their result copying run under the gate), the lock order (`scan_mtx` may be held while acquiring; the holder takes no other lock), and why the re-entry check comes before the ticket.

- [ ] **Step 4: Run them to see them pass**

`& .\build\tests\Release\litepdf_unit_tests.exe "[gate]"`. Expected: all pass.

- [ ] **Step 5: Commit**

`git add src/core/detail/PriorityGate.hpp src/core/PriorityGate.cpp CMakeLists.txt tests/unit/gate_test_kit.hpp tests/unit/test_priority_gate.cpp tests/CMakeLists.txt`, then `git commit -m "feat(core): a priority gate for one document's users (#229)"`.

---

## Task 3: The Document's gate, the borrow seam, `is_fb2`, print outside the gate

**Files:**
- Create: `src/core/detail/SharedDocumentAccess.hpp`
- Modify: `src/core/Document.hpp`, `src/core/Document.cpp`
- Comments only: `src/core/detail/TextPageAccess.hpp`, `src/core/detail/PageHitsAccess.hpp`, `src/core/EscrowContext.hpp` (line ~49), `src/cli/bench_selection.hpp` (lines 10, 23), `tests/unit/test_search_session.cpp` (file header, line 16)
- Create: `tests/unit/test_document_gate.cpp`; modify `tests/CMakeLists.txt` (`unit/test_document_gate.cpp  # #229 Document gate`)
- Modify: `tests/unit/test_search_session.cpp` (the #130 T1 case at ~line 338)

**Interfaces:**
- Consumes: Task 2's `PriorityGate`, `GateHold`, `GateClass`.
- Produces:
  - `bool Document::is_fb2() const noexcept;` true iff the document is open and MuPDF reported `FZ_META_FORMAT == "FictionBook2"` at open.
  - `src/core/detail/SharedDocumentAccess.hpp` (includes `<mupdf/fitz.h>`; core and tests only):
    ```cpp
    namespace litepdf::core::detail {
    struct SharedDocumentAccess {
        // Runs fn(fz_document*) with the Document's document, holding its gate in
        // class `cls` for the whole call. fn uses the CALLER's own fz_context.
        // False (fn not run) when the document is not open. Throws what
        // PriorityGate::acquire throws; a C++ exception out of fn releases the gate.
        template <class F> static bool with_document(const Document& d, GateClass cls, F&& fn);
        // Test seams.
        static PriorityGate& gate(const Document& d) noexcept;
        // Runs on the borrower's thread inside with_document, after the gate is
        // released and before it returns, with the borrower's class. Set before
        // any borrower runs.
        static void set_after_release_hook(const Document& d, std::function<void(GateClass)> hook);
    private:
        static bool borrow(const Document& d, GateClass cls,
                           void (*call)(void*, fz_document*), void* fn);
    };
    }
    ```
    The header states the §3.2 rules (one critical section per `fz_page`; what may leave `fn`; no longjmp across it; the document dies last, on the Document's context) and §8's: once a view exists its Document is reached only as `const Document&`, so `open`, `close` and `authenticate` (non-const, ungated) never run beside a borrower. `with_document` wraps `fn` into `call`/`fn` (no allocation). `borrow` is defined in `Document.cpp`: `if (!d.is_open()) return false; { GateHold h(gate, cls); call(fn, doc); } if (hook) hook(cls); return true;`.
  - `Document.hpp`: forward-declare `detail::SharedDocumentAccess` next to `TextPageAccess` and befriend it. `Document::Impl` gains `mutable detail::PriorityGate gate;` (replacing `doc_mutex`), `bool is_fb2 = false;` and `std::function<void(detail::GateClass)> after_release_hook;`.

- [ ] **Step 1: Write the failing tests** in `tests/unit/test_document_gate.cpp`, tag `[core][gate][document]`

- `Document: is_fb2 follows the content, not the name`: true for `tests/fixtures/strip-chapters.fb2` and for a temp copy of it named `.svg`; false for `simple.pdf`, a temp copy of `simple.pdf` named `.fb2`, `sample.svg`, `sample.epub`, and a closed `Document`. (Reuse the `TempCopy` shape of `test_make_document_view.cpp`.) **If the `.svg` copy does not open as FB2, stop and report: spec §3.1's premise is wrong.**
- `SharedDocument: with_document runs on an open document only`: on an open FB2 it returns true and `fn` gets a non-null `fz_document*`, through which `fz_count_pages(ctx, d)` on a context from `doc.clone_context()` equals `doc.page_count()`. On a closed Document it returns false and `fn` is not run.
- `Document: text_page under a held gate is empty` (G4, second half): inside `with_document(doc, GateClass::Foreground, ...)`, `doc.text_page(0)` is invalid; outside it, valid. Run the whole sequence through `finishes_within(5 s)` on a heap-owned Document (a removed re-entry check would deadlock it); the helper records both validities and the main thread asserts them.
- `Document: with_rendered_page calls back with the gate released` (G9): in the callback, `doc.page_size(0)` does not throw, and a thread running `with_document(doc, GateClass::Render, ...)` finishes within 5 s while the callback waits for it. The callback holds no REQUIRE: it records booleans into locals, asserted after `with_rendered_page` returns true. Run through `finishes_within(10 s)` on a heap-owned Document, because today's code blocks the second thread forever.

Rewrite `SearchSession: a scan completes while doc_mutex is held` (test_search_session.cpp ~line 338) as `SearchSession: an own-document scan completes while the gate is held`. A helper thread holds `with_document(doc, GateClass::Foreground, ...)` parked on a latch, and an `Own` session on a 2-thread pool must reach `scan_complete()` with 15 hits before the latch opens. The old case relied on `with_rendered_page` holding the lock across its callback, which this task ends. Keep the session outside the hold and a `GateGuard` that opens the latch on every exit.

- [ ] **Step 2: Run them to see them fail**

Expected: compile failure (`is_fb2`, `SharedDocumentAccess` unknown).

- [ ] **Step 3: Implement**

- Every `std::lock_guard<std::mutex> lk(impl_->doc_mutex)` in `Document.cpp` becomes `detail::GateHold hold(impl_->gate, detail::GateClass::Foreground)`: `page_size`, `read_ahead_page_sizes`, `page_text`, `page_hits`, `query_compiles` and `outline`. In `text_page`, a `std::optional<detail::GateHold>` is `emplace`d in the existing try/catch, so a re-entry gives an empty handle (#70).
- `is_fb2`: a file-local `bool read_is_fb2(fz_context*, fz_document*) noexcept` (a `char buf[32]`, `fz_lookup_metadata(ctx, doc, FZ_META_FORMAT, buf, sizeof buf)` in `fz_try`, `fz_report_error` in `fz_catch` → false; true iff the result is > 0 and `buf` is `"FictionBook2"`). `open()` sets `impl_->is_fb2` after `read_page_count` succeeds; `close()` resets it.
- `with_rendered_page` (§3.3, "Print no longer holds the gate across its callback"): inside one Foreground hold, render as today, drop the page, and read `w`, `h`, `samples`. Release. Then composite over white (pure memory on a pixmap only this call owns; moved out of the hold so the hold carries MuPDF work only) and call the callback. A scope guard drops the pixmap under a fresh Foreground hold after the callback returns or throws. The drop goes back under the gate so every use of `impl_->ctx` stays under it. If that acquire throws (it cannot, since the thread holds nothing), the guard leaks the pixmap rather than terminate. The callback's bytes are unchanged.
- Comments (§3.7). Every `doc_mutex` mention in `Document.hpp` and `Document.cpp` (`rg -n doc_mutex src/core/Document.*`: today Document.hpp 202, 243, 273, 338-341 and the lock-site comments in Document.cpp) is rewritten for the gate, the `TextPage` class's thread-safety comment (Document.hpp ~272) included. `Document.hpp`: `source_path` (workers reopen the file only for documents they do not share), `page_hits` (serialised by the gate), `with_rendered_page` (the gate is released before the callback, and why: `abort_proc` dispatches messages), `text_page` (#70 contract under the gate) and `clone_escrow_context`. `Document.cpp`: the `Impl::gate` comment, quoting MuPDF's rule 2 (`docs/reference/c/overview.md`) instead of "forbids". Also `TextPageAccess.hpp`, `PageHitsAccess.hpp`, `EscrowContext.hpp` and `bench_selection.hpp`: "doc_mutex" becomes "the Document's gate", and who serialises `ctx`/`doc` is restated per mode.

- [ ] **Step 4: Run them to see them pass, then the suite**

`litepdf_unit_tests.exe "[gate]"`, then `"[search]"`, then full `ctest`. Expected: all pass; count = Task 1's + Tasks 2-3's new cases.

- [ ] **Step 5: Commit**

Stage the files above, then `git commit -m "refactor(core): a priority gate replaces doc_mutex; print calls back outside it (#229)"`.

---

## Task 4: RenderEngine in shared mode

**Files:**
- Create: `src/core/DocumentShare.hpp`
- Modify: `src/core/RenderEngine.hpp`, `src/core/RenderEngine.cpp`
- Create: `tests/unit/test_shared_document.cpp`; modify `tests/CMakeLists.txt` (`unit/test_shared_document.cpp  # #229 shared FB2 document`)

**Interfaces:**
- Consumes: Task 3's `SharedDocumentAccess::with_document`, `gate`, `set_after_release_hook`; `GateClass::Render`.
- Produces:
  - `src/core/DocumentShare.hpp`: `namespace litepdf::core { enum class DocumentShare { Own, Shared }; }`, with a comment: `Own` opens the file again per worker or session, `Shared` borrows the Document's document under its gate (§3.2-3.5).
  - `RenderEngine(Document& doc, std::size_t num_workers = kDefaultWorkers, PageCache* cache = nullptr, std::optional<std::string_view> password = std::nullopt, DocumentShare share = DocumentShare::Own);`. With `Shared`, the password is not read.
  - `std::size_t RenderEngine::documents_opened() const noexcept;` the number of `fz_open_document` calls this engine made: `num_workers` for `Own`, 0 for `Shared`.

- [ ] **Step 1: Write the failing tests** in `tests/unit/test_shared_document.cpp`, tag `[core][shared]`, on `tests/fixtures/strip-chapters.fb2`

- `RenderEngine: a shared engine opens no document` (S4, engine half): `Shared` with 1 worker → `documents_opened() == 0`; `Own` with 2 workers → 2. A `Shared` render of page 0 at scale 0.5, no cache, delivers a non-null pixmap.
- `RenderEngine: a shared worker waits for the gate before building` (B1): a helper thread holds the Foreground gate parked on a latch. Submit an uncached page-0 render to a `Shared` engine with no cache. `poll_until(gate(doc).waiting(GateClass::Render) == 1)`, and `on_complete` has not run. Open the latch; `on_complete` then delivers a non-null pixmap within 5 s.
- `SharedDocument: no live page is left past the gate` (B3, render build and text build sections). Set the after-release hook to park each `Render` borrower on a FRESH latch per park (`test_kit::Gate` is one-shot: the hook takes park k's latch from a mutex-guarded vector, records the park, and waits on it), so the second park really blocks. While a borrower is parked, the test thread runs `with_document(doc, GateClass::Foreground, ...)` and walks `d->open` / `p->next`, counting entries with `p->doc != nullptr`; the count must be 0. A dead page stays listed until the next load reaps it, so "the list is empty" is not the check. Render section: one uncached render. Text section: a render with `on_text` and `text_wanted` holding the page; it parks twice (display list, then text), and the check runs at each park.
- `RenderEngine: a shared worker's failed page releases the gate` (Review Focus 3): a render of page `page_count() + 5` delivers null. Then a page-0 render delivers non-null, and afterwards `waiting()` is 0 for every class.
- `RenderEngine: a shared engine releases its contexts` (Review Focus 5): take the `family_context_count` probe of `test_search_session.cpp` (~line 1253). With 1 `Shared` worker, 20 renders submitted and the engine destroyed at once, the count returns to its value before the engine was built, and the destructor finishes within 10 s (`finishes_within`, heap-owned engine and document).

- [ ] **Step 2: Run them to see them fail**

Expected: compile failure (`DocumentShare`, `documents_opened` unknown).

- [ ] **Step 3: Implement**

- `Impl` gains `const Document* shared = nullptr;` (set only for `Shared`) and `std::size_t opened = 0;`. In `Shared` mode the constructor clones `n` contexts as today and does nothing more before spawning workers: no path, no password copy, no open. `Own` is unchanged, and counts its opens into `opened`.
- `worker_loop`: no `fz_document* wdoc` local. Two helpers on `Impl` take the worker index:
  - `static fz_display_list* build_display_list(Impl*, fz_context*, std::size_t idx, int page) noexcept;` with `Own`: today's load/list/drop on `worker_docs[idx]`. With `Shared`: the same body inside `with_document(*shared, GateClass::Render, [&](fz_document* d){ ... })`. Both use one file-local `fz_display_list* load_display_list(fz_context*, fz_document*, int page) noexcept` holding today's `fz_try` block, the page drop included.
  - `maybe_deliver_text(Impl*, const RenderRequest&, const std::shared_ptr<std::atomic<bool>>&, fz_context*, std::size_t idx)`: its build is `TextPageAccess::build(ctx, worker_docs[idx], page)` for `Own` and the same call inside `with_document(*shared, GateClass::Render, ...)` for `Shared`. Skip rules and the post-build re-check are unchanged.
  - L1/L2 lookups, rasterising, invert, cache puts, the cancel checkpoints, `on_complete` and `on_text` stay outside the gate.
- Destructor and rollback paths: `worker_docs` is empty in `Shared` mode, so the existing loops drop nothing.
- Comments (§3.7). The `worker_docs` member, the constructor's open loop, the rollback comment and `maybe_deliver_text` now say: `Own` opens a copy per worker so two pages interpret in parallel; MuPDF allows one document on several threads when uses never overlap (overview.md rule 2), which `Shared` relies on. `RenderEngine.hpp` also documents the constructor's `num_workers` and `share`, and rewrites the `on_text` comment (~line 77, "built on the worker's own document": now from its own document or, shared, from the Document's under the gate) and the `kDefaultWorkers` comment (~line 86: a shared FB2 has one worker because a second would mostly wait on the gate; a `.fb2`-named non-FB2 keeps one by name).

- [ ] **Step 4: Run them to see them pass, then the suite**

`litepdf_unit_tests.exe "[shared]"`, then `"[render]"`, then full `ctest`. Expected: all pass.

- [ ] **Step 5: Commit**

`git commit -m "perf(core): a render engine can borrow the Document's own document (#229)"`, after staging the files above.

---

## Task 5: SearchSession in shared mode; #236's deferred open removed

**Files:**
- Modify: `src/core/SearchSession.hpp`, `src/core/SearchSession.cpp`
- Delete: `src/core/SearchOpen.hpp`, `src/core/FileStamp.hpp`, `src/core/FileStamp.cpp`
- Modify: `CMakeLists.txt` (drop `src/core/FileStamp.cpp`), `src/core/Document.hpp` and `src/core/Document.cpp` (drop `source_stamp`, `Impl::stamp`, the `file_stamp` call in `open`, `#include "core/FileStamp.hpp"`)
- Modify: `src/core/DocumentView.hpp`, `src/core/DocumentView.cpp` (constructor parameter, comments), `src/ui/detail/MakeDocumentView.hpp` (temporary `Own`), `src/ui/MainWindow.cpp` (`update_find_counter`'s `open_failed` block)
- Modify: `tests/unit/test_search_session.cpp`, `tests/unit/test_make_document_view.cpp`, `tests/unit/test_shared_document.cpp`

**Interfaces:**
- Consumes: Task 4's `DocumentShare`; Task 3's seam with `GateClass::Scan`.
- Produces:
  - `SearchSession(const Document& doc, litepdf::app::ISearchDispatcher& dispatcher, std::optional<std::string_view> password = std::nullopt, DocumentShare share = DocumentShare::Own);`. `Shared` clones the escrow context, opens nothing, and reads no password. It throws `std::runtime_error` only if the clone fails.
  - `std::size_t SearchSession::documents_opened() const noexcept;` returns `Impl::opened`, which the constructor increments where `open_scan_document` returns a document (as the engine counts its opens), never derived from the mode: 1 for `Own`, 0 for `Shared`.
  - Removed: `SearchOpen`, `SearchSession::open_failed()`, `set_scan_hooks` and its `SearchSessionTestAccess` forwarder, `FileStamp`, `file_stamp`, `Document::source_stamp()`.
  - `DocumentView(Document, ISearchDispatcher&, std::optional<std::string_view> password = std::nullopt, std::size_t num_workers = RenderEngine::kDefaultWorkers, std::size_t l1_capacity = kDefaultL1Capacity, std::size_t l2_capacity = kDefaultL2Capacity, std::size_t l1_budget_bytes = kDefaultL1BudgetBytes, DocumentShare share = DocumentShare::Own);` passes `share` to the engine and the session.
  - `std::size_t DocumentView::render_documents_opened() const noexcept;` forwards to the engine (test observability).
  - Until Task 6, `make_document_view` passes `DocumentShare::Own` (an FB2 opens its search copy with the view, as before #236; one render worker by name stays).

- [ ] **Step 1: Write the failing tests** (`[core][shared]` in `test_shared_document.cpp`)

- `SearchSession: a shared session opens no document` (S4, session half): `Shared` → `documents_opened() == 0`, and `set_query(L"Lorem")` on an `InlineDispatcher` finds the same total as summing `doc.page_hits(p, "Lorem", {})` over every page. `Own` → 1.
- `SearchSession: a shared scan waits for the gate before scanning` (B2): a helper thread holds Foreground parked on a latch. A `Shared` session on `ThreadPoolDispatcher pool(1)` runs `set_query(L"Lorem")`. `poll_until(waiting(Scan) == 1)`; `hit_count() == 0` and `!scan_complete()`. Open the latch; `scan_complete()` within 10 s, with the full count.
- `SharedDocument: no live page is left past the gate`, scan section (B3): a new SECTION inside Task 4's existing TEST_CASE (never a second TEST_CASE of the same name and tags, which Catch2 rejects at registration). The after-release hook parks `Scan` borrowers, one fresh latch per park. The check is the same as Task 4's, run at the first park; then the latch opens and the scan completes.
- `SearchSession: a shared session releases its context, scanned or not` (Review Focus 4): the #236 test `a deferred session releases its context, scanned or not` with `DocumentShare::Shared` in place of `SearchOpen::AtFirstScan`, moved into `test_shared_document.cpp` (with its own fixture path literal and includes).

- [ ] **Step 2: Run them to see them fail**

Expected: compile failure.

- [ ] **Step 3: Implement**

- `Impl` loses `path`, `path_utf8`, `stamp`, `failed_epoch`, the three scan hooks and `open_deferred`; it gains `bool shared`. The constructor: clone the escrow; with `Shared`, commit it and return; with `Own`, today's open with the password, wiped on exit.
- Task lambda: keep the drain guard, epoch check #1, `scan_mtx` and epoch check #1b; remove #1c and the deferred-open block. Under `scan_mtx`: with `shared`, `raw_hits` is assigned inside `with_document(self->doc, GateClass::Scan, [&](fz_document* d){ raw_hits = detail::page_hits_on(self->escrow.get(), d, i, needle_utf8, df, cookie.get()); })`; otherwise today's call on `sdoc`. Everything after is unchanged.
- `~SearchSession`: the drain is unchanged. Step 4 drops `sdoc` only when set (null in `Shared`), and its comment no longer mentions a deferred open. Update the step 3 comment: a task may wait for the gate, whose holder is the render worker (finishes its hold) or a Foreground caller on another thread, never the UI thread (§3.5).
- `MainWindow::update_find_counter`: delete the `open_failed` block.
- `DocumentView` and `SearchSession.hpp`: `SearchOpen` → `DocumentShare` as above; `#include "core/DocumentShare.hpp"` replaces `#include "core/SearchOpen.hpp"` in both headers.
- Delete the whole `[lazy]` block in `test_search_session.cpp` (lines ~956-1294: `kFb2Fixture`, `MovableCopy`, `total_hits`, `bump_write_time` and every case through `a password makes a deferred session open at once`) and the `set_scan_hooks` forwarder (~lines 64-72). Nothing in it is reused in place; the one converted case lives in `test_shared_document.cpp`. Delete both `[lazy]` cases in `test_make_document_view.cpp`. Restore `the password reaches a one-worker view` to the pre-#236 oracle plus counters: `render_workers() == 1`, `hit_count() == 8`, `render_documents_opened() == 1`, `search().documents_opened() == 1`, with no `open_failed` line.
- Comments (§3.7): `SearchSession.hpp` (the thread-safety block per mode: `Own` scans its own document under `scan_mtx`, `Shared` scans the tab Document's under the gate's Scan class and waits for the UI and the render worker; the constructor doc) and `SearchSession.cpp` (the session's pair comment); `DocumentView.hpp` and `DocumentView.cpp` (constructor doc, the teardown comment: a shared session's tasks DO use `impl_->doc`, so the drain before the Document dies is load-bearing).

- [ ] **Step 4: Run them to see them pass, then the suite**

`"[shared]"`, `"[search]"`, `"[make_view]"`, then full `ctest`. Expected: all pass. `rg -n "SearchOpen|open_failed|FileStamp|file_stamp|source_stamp|set_scan_hooks|Reopen file" src tests CMakeLists.txt` prints nothing.

- [ ] **Step 5: Commit**

`git add -A src/core/SearchOpen.hpp src/core/FileStamp.hpp src/core/FileStamp.cpp` (stages the deletions) plus the modified files, then `git commit -m "perf(core): a search session can scan the Document's own document; drop the deferred open (#229)"`.

---

## Task 6: `make_document_view` shares an FB2

**Files:**
- Modify: `src/ui/detail/MakeDocumentView.hpp`, `tests/unit/test_make_document_view.cpp`
- Comments only: `src/ui/detail/EffectiveLayout.hpp` (`one_render_worker`'s comment, ~line 78: it now decides only the worker count of a view that does not share; a shared view gets one worker from `make_document_view`)

**Interfaces:**
- Consumes: `Document::is_fb2()`, `one_render_worker(path)`, `DocumentShare`, the `DocumentView` constructor and counters from Task 5.
- Produces: `make_document_view` passes `share = doc.is_fb2() && !password ? Shared : Own`, and `workers = share == Shared || one_render_worker(doc.source_path()) ? 1 : RenderEngine::kDefaultWorkers` (§3.1). Its header comment names both predicates and why the extension one is not used for sharing.

- [ ] **Step 1: Write the failing tests** (`[ui][make_view]`)

- `MakeDocumentView: an FB2 view opens no document of its own` (S1): `strip-chapters.fb2` → `render_documents_opened() == 0`, `search().documents_opened() == 0`, `render_workers() == 1`; `simple.pdf` → 2 and 1.
- `MakeDocumentView: an unencrypted PDF named .fb2 keeps its own documents` (S2): a `TempCopy` of `simple.pdf` as `plain.fb2`, no password → render 1, search 1, `render_workers() == 1`.
- `MakeDocumentView: FB2 content named .svg is shared with one worker` (S3): a `TempCopy` of `strip-chapters.fb2` as `book.svg` → render 0, search 0, `render_workers() == 1`.
- `MakeDocumentView: an FB2 view searches after its file is moved away` (§6.1 behaviour): count the expected hits on a separate `Document` of the copy, build the view, rename the copy away; `set_query(L"Lorem")` on an `InlineDispatcher` → `hit_count()` equals that count.
- `MakeDocumentView: closing an FB2 view mid-search and mid-render returns` (Review Focus 1): a view on a `ThreadPoolDispatcher pool(2)`. Start `set_query(L"a")`, then `view->request_render(p, cb)` for every page, after the same viewport setup `test_document_view.cpp` uses before its renders (`cb` drops any pixmap it gets, on the context it gets). While the scan is incomplete, destroy the view. Destruction finishes within 10 s (`finishes_within`, with the view and pool heap-owned).

- [ ] **Step 2: Run them to see them fail**

Expected: S1 and S3 fail on the counters (`Own` from Task 5); S2 and the rest may already pass.

- [ ] **Step 3: Implement** the predicate above.

- [ ] **Step 4: Run them to see them pass, then the suite.** Expected: all pass.

- [ ] **Step 5: Commit**

`git commit -m "perf(ui): an FB2 tab keeps one copy of its book (#229)"`, after staging the three files.

---

## Task 7: Results under contention

**Files:**
- Modify: `tests/unit/test_shared_document.cpp`

**Interfaces:**
- Consumes: everything above. No production change, unless a case fails: a failure is a bug in Tasks 3-6, fixed there, never a looser test.

- [ ] **Step 1: Write the tests** (`[core][shared][contention]`)

- `SharedDocument: results under contention equal the single-threaded results` (C1), on `strip-chapters.fb2`, one `Document doc` throughout:
  1. Baselines, nothing else running. `RenderEngine eng(doc, 1, nullptr, std::nullopt, DocumentShare::Shared)` renders every page at scale 0.5, one at a time; keep `(w, h, stride, bytes)` per page. Single-threaded hits: `doc.page_hits(p, "Lorem", {})` for every page. Single-threaded text: `doc.text_page(p)` → `full_range` → `copy` for every page.
  2. Contention, all at once: a `Shared` `SearchSession` on `ThreadPoolDispatcher pool(2)` with `set_query(L"Lorem")`; the same engine re-renders every page (no cache, so each builds a display list under the gate); the test thread loops `doc.text_page(p)` + `copy` on pages from `std::mt19937 rng(229)` until the scan and every render are done (deadline 60 s).
  3. Check: the session's hits equal the baseline hits by page and quad (`ul_x`...`lr_y`, exact float equality: same document, same code path); every pixmap equals its baseline byte for byte; every contention text equals that page's baseline text.
- `SharedDocument: a print during renders equals a quiet print` (Review Focus 2): `doc.with_rendered_page(0, 2.0f, 2.0f, false, cb)` copies its bytes once quietly. A second time, while a `Shared` engine renders every page in a loop on its worker, the bytes are equal.

- [ ] **Step 2: Run them.** Expected: pass. Run each 20 times (`--repeat`-style loop in PowerShell over the exe with the case name); expected 20/20.

- [ ] **Step 3: Commit**

`git commit -m "test(core): a shared FB2 under contention gives the single-threaded results (#229)"`.

---

## Task 8: Prove the tests can fail (spec §6.6)

No commit. **Invoke the `mutation-test-hygiene` skill first** and follow it: work from the committed tree, one mutant at a time, restore with `git checkout -- <file>`, rebuild, and confirm `git status` is clean before the next one.

**Files:** temporary edits only.

- [ ] **Step 1: Run each mutant** and record the named test's result. Each must FAIL (a `finishes_within` timeout counts as a failure).

| # | mutant | must fail |
|---|---|---|
| 1 | `admissible` ignores class: one FIFO over all tickets by arrival (a global ticket counter) | G1, G2, G3 |
| 2 | `build_display_list`'s `Shared` branch calls `load_display_list` on the document reached through a temporary raw accessor added to `SharedDocumentAccess`, without the gate | B1 |
| 3 | `load_display_list` returns the page through an out-parameter and the engine drops it after `with_document` returns | B3 (render) |
| 4 | `make_document_view` passes `Own` for FB2 | S1 |
| 5 | `SearchSession` opens its own document in `Shared` mode too | S1, S4 (session) |
| 6 | `acquire`'s re-entry check removed | G4 (both halves) |
| 7 | re-entry check moved after `next_ticket[cls]++` | G5 |
| 8 | `make_document_view` shares by extension: `one_render_worker(path) && !password` | S2 |
| 9 | tickets removed (`admissible` drops `ticket == serving[cls]`) | G6 (its pure section fails deterministically) |
| 10 | the `kScanAfterRenders` clauses removed (Render strictly over Scan) | G7 |
| 11 | `with_rendered_page` calls back before releasing the gate | G9 |

Run as `ctest --test-dir build -C Release -R "^<name>"` from PowerShell.

- [ ] **Step 2: Clean tree.** After the last restore: rebuild, run `"[gate]"`, `"[shared]"` and `"[make_view]"`, all passing; `git status --short` shows only the MuPDF junction's usual state. Report the table with each mutant's failing case and message.

---

## Task 9: Measure (spec §7)

No commit of probes. Pass bars are fixed in advance: **(3) every UI-thread Foreground wait < 16 ms; (4) the scan's P0 p99 within 30 ms of the control's p99. A miss stops the PR for a design change** (report to the controller; do not loosen a bar or merge with a residual).

**Files:** scratch only: `~/.claude/compact-dumps/litepdf/229-tools/share-pr/` (driver, probe patch, results).

Read first: `reference_litepdf_smoke_script_rules` (back up session.json, running.lock and the MRU key; deadline polls), `reference_litepdf_scripted_gui_smoke` ("Read first" list: DPI-aware input), `reference_litepdf_bench_machine_noise` (pin to 0x5555, warm the file cache).

- [ ] **Step 0: Books.** `python ~/.claude/compact-dumps/litepdf/229-tools/make-fb2.py <S>/fx/s800.fb2 800 10`, and the same with `s1500.fb2 1500` (no cover; seed 169 is built in). Expected sizes about 3.5 MB and 6.5 MB, the books of every earlier #229 measurement.
- [ ] **Step 1: The "before" build.** `git worktree add --detach .claude/worktrees/m229-before dde66b6`, junction its `third_party\mupdf` as in Task 1, copy catch2-src, configure, and build target `litepdf` Release. Never check main's sources out over this branch.
- [ ] **Step 2: The probe patch** (both trees, uncommitted, one patch file applied with `git apply`; hand-merge `update_find_counter` in "before"). Under `--log-timings`, write to stderr:
  - `search-start` when the find bar starts a query, then `search-first-hit <ms>` the first time `hit_count() > 0`, and `search-complete <ms>` when `scan_complete()` (for (2)).
  - "after" only: `gate-wait-fg <us>` for every Foreground acquire on the UI thread (QPC around `cv.wait`), and `p0 <ms>` from P0 `submit` to its `on_complete`.
- [ ] **Step 3: Extend the driver** (`229-tools/pr3/measure.ps1` → `share-pr/measure.ps1`) with the §7 workloads, books `s800` and `s1500` from `229-tools`, 5 runs per build and book, before and after interleaved:
  1. open only: T0→T4, `view_built`, working set and private bytes (median of 5);
  2. 2 s after the first page, search `ab` from page 1: time to the first hit and to complete (median of 5);
  3. the same search, plus 40 rounds while it scans: a Ctrl+G jump to a page at least 20 pages from any page shown or prefetched so far (fixed seed), then 30 ms later a left press on that page; rounds 150 ms apart. A press on a page whose text slot is empty calls `Document::text_page` (`DocumentView::text_for`, DocumentView.cpp ~690), so each round makes about one Foreground acquire while the worker and the scan contend. Statistic: max `gate-wait-fg` over all runs ("after"). **Also report the sample count: fewer than 150 Foreground acquires over the 5 runs is "not measured"**: fix the driver and re-run; never judge the bar on it. (Spec §7 (3) amended to this workload at the plan gate: 40 presses on one page reach `Document::text_page` about once.)
  4. the same search plus 40 Ctrl+G jumps (fixed seed, each ≥ 20 pages from any page shown or prefetched so far, 150 ms apart) vs the same jumps with no search: nearest-rank p99 of `p0` over 200 pooled samples each ("after").
- [ ] **Step 4: Run, tabulate, judge.** Write `share-pr/results.md`: medians and ranges for (1)-(2), max for (3) and p99s for (4), each against its bar. Expected per spec §2: 1,500 sections, first page about 1.15 s and working set about 150 MiB; the first search starts without a parse.
- [ ] **Step 5: Teardown.** Restore session.json, running.lock and MRU; remove the before-worktree's junction with `cmd /c rmdir`, then `git worktree remove .claude/worktrees/m229-before`.

---

## Task 10: CHANGELOG, final sweep, hand-off to the PR gate

**Files:**
- Modify: `CHANGELOG.md`

- [ ] **Step 1: CHANGELOG** (§5). In `[Unreleased]`, Fixed: replace the #236 entry ("An FB2 book that is never searched no longer pays for search ...", ending "... (#229).") with one entry. An FB2 book keeps one copy in memory for showing and searching; it opens sooner and uses less memory, and the first search starts at once. Use Task 9's medians for 800 and 1,500 sections against the release before #229's FB2 work only if Task 9 measured that; otherwise against the base (dde66b6), said as such. Remove the "Reopen file" sentence. Keep the #235 entry above it untouched.
- [ ] **Step 2: Sweep.** `rg -n "doc_mutex|SearchOpen|open_failed|FileStamp|source_stamp|AtFirstScan|forbids sharing" src tests CMakeLists.txt` prints nothing (historical docs under `docs/` may keep the words). Full `ctest` serially: all pass, count = Task 1's − deleted `[lazy]` cases + new ones.
- [ ] **Step 3: Commit** `docs(changelog): an FB2 book keeps one copy (#229)`.
- [ ] **Step 4: Hand off** to the PR gate: `risk-tiered-review`, Full tier (architecture-changing: a lock replaced for every format, a cross-thread document share). The gate's reviewers get this plan, the spec and `git diff origin/main...HEAD`, not the shared checkout.
