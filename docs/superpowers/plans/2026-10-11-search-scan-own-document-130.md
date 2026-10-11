# Search scan on the session's own document (#130) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A `SearchSession` scan never takes the tab Document's `doc_mutex`, and a cancel or tab close stops a cold text build at the next content-stream token.

**Architecture:** `SearchSession` clones an `EscrowContext` from the tab Document, opens and (if needed) authenticates its own `fz_document`, and runs every scan task on that pair under a session mutex. The `page_hits` body becomes a lock-free helper that builds text with an `fz_cookie`; each search generation owns one cookie, which replaces today's `atomic<int>` abort token.

**Tech Stack:** C++17 (MSVC, `cxx_std_17`), MuPDF 1.27.2 (vendored), Catch2, CMake/ctest from VS 2022 BuildTools.

**Spec:** `docs/superpowers/specs/2026-10-11-search-scan-own-document-130-design.md` — read it before any task; section numbers below refer to it.

## Global Constraints

- Branch `perf/130-search-scan-own-document` (spec committed at `0bc8f06`). Baseline `main` @ `4adbda7`.
- Build and test **Release** only (Debug fails with LNK2038). Use the full BuildTools paths, never a bare `cmake`/`ctest`:
  - `$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"` (ctest.exe in the same dir)
  - build: `& $cmake --build build --target litepdf_unit_tests --config Release`
  - targeted run, from the repo root: `build\tests\Release\litepdf_unit_tests.exe "<exact test name or [tag]>"`
  - full suite (the CI path), from **PowerShell** at the repo root: `& $ctest --test-dir build -C Release`. Read the baseline count with `-N` first; do not quote an old count.
- `TEST_CASE` names: pure ASCII, prefixed with the subsystem (`Document: `, `SearchSession: `, `GenerationAbort: `), because `ctest -R` matches names, not tags.
- Headers named in `src/core/*.hpp` that other layers include stay MuPDF-free; `SearchSession.hpp` in particular must not include MuPDF. Only `.cpp` files and `src/core/detail/*` headers may include `<mupdf/fitz.h>`. `GenerationAbort.hpp` is included only by `SearchSession.cpp` and its test, so it may include a detail header.
- Passwords: every NUL-terminated copy is wiped with `SecureZeroMemory` on every exit (Phase 8 D3), as in `RenderEngine.cpp:389-394`.
- Every MuPDF document is dropped on the context it was opened on, before that context.
- Commit messages: conventional commits, English, ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

1. **A wrong or missing password must fail at tab open, never as silent zero hits.** Pinned by Task 3's throw test and Task 2's exact `== 8` count.
2. **Rapid retyping and tab close mid-scan stay safe.** The cookie is written by the UI thread while a worker builds. Pinned by the existing `SearchSession: rapid query changes settle on correct results (ThreadPool)` and `test_crosstab_ctrlw_race` (must stay green, unchanged) plus Task 2's T4 tests.
3. **Constructor failure leaks no context.** Pinned by Task 3's leak-oracle test.
4. **The session's document is never touched after `~SearchSession` drops it.** The drain in `~SearchSession` must finish before the drop. Pinned by Task 2's destructor order plus the existing `SearchSession: destruct while holding state is safe`.
5. **Regex, whole-word and case flags behave exactly as before.** The helper is a move of the existing body. Pinned by the unchanged `test_document_search.cpp` cases, which run through the wrapper.

---

## Task 1: Lock-free page-hits helper with a cookie, and an escrow accessor

**Files:**
- Create: `src/core/detail/PageHitsAccess.hpp`
- Modify: `src/core/Document.hpp` (`page_hits` declaration and its comment at `:130-151`; new accessor), `src/core/Document.cpp` (`page_hits` at `:489-580`; `Impl::doc_mutex` comment at `:51-67`)
- Modify: `src/core/SearchSession.cpp:216-220` (call site loses the abort argument, interim; see Step 6)
- Modify: `src/cli/bench_selection.cpp:80` (drop the `nullptr` 4th argument; this file is also compiled into `litepdf_unit_tests`, `tests/CMakeLists.txt:101`)
- Modify: `src/core/EscrowContext.hpp:49-50` (the "every caller today" list gains `Document::clone_escrow_context`)
- Test: `tests/unit/test_page_hits_cookie.cpp` (new; register in `tests/CMakeLists.txt` next to `test_document_search.cpp`), `tests/unit/test_document_search.cpp:94, 100, 101, 119-129`

**Interfaces:**
- Produces, in `src/core/detail/PageHitsAccess.hpp` (includes `core/Document.hpp`, `<mupdf/fitz.h>`, `<intrin.h>`; namespace `litepdf::core::detail`):
  - `void request_abort(fz_cookie& c) noexcept;`: `_InterlockedExchange` on `c.abort` (with `static_assert(sizeof(long) == sizeof(int))`); inline.
  - `bool abort_requested(const fz_cookie& c) noexcept;`: a `volatile` read of `c.abort`; inline.
  - `std::vector<Document::PageHit> page_hits_on(fz_context* ctx, fz_document* doc, std::size_t page, std::string_view needle_utf8, Document::SearchFlags flags, fz_cookie* cookie);`: defined in `Document.cpp`. It takes no lock (the caller serialises `ctx`/`doc`), and `cookie` may be null.
- Produces, in `Document`:
  - `[[nodiscard]] std::vector<PageHit> page_hits(std::size_t page, std::string_view needle_utf8, SearchFlags flags) const;`: the `std::atomic<int>* abort_flag` parameter is **removed**. It keeps today's `if (!is_open() || needle_utf8.empty()) return {};` guard (`Document.cpp:496`) before taking `doc_mutex`, then calls `page_hits_on(impl_->ctx, impl_->doc, …, nullptr)`. The helper itself returns `{}` on a null `ctx` or `doc`, or an empty needle.
  - `[[nodiscard]] EscrowContext clone_escrow_context() const;`: `EscrowContext::clone_from(impl_->ctx)` when a document is open, otherwise empty. Takes no `doc_mutex`. `Document.hpp` includes `core/EscrowContext.hpp`, which is MuPDF-free.

- [ ] **Step 1: Write the failing tests** in `tests/unit/test_page_hits_cookie.cpp`. Open `tests/fixtures/search.pdf` as a `Document`, take `auto esc = doc.clone_escrow_context()`, open a second `fz_document` on `esc.get()` with `fz_open_document`, and drop it on `esc.get()` at the end of each case.
  - `Document: page_hits_on passes the cookie to the text build`: a zero-initialised `fz_cookie c{}`; `page_hits_on(esc.get(), d, 0, "Lorem", {}, &c).size() == 12` and `c.progress > 0`.
  - `Document: page_hits_on stops before the build when the cookie is aborted`: `fz_cookie c{}; detail::request_abort(c);` then the result is empty and `c.progress == 0`. MuPDF checks `abort` before incrementing `progress` (`pdf-interpret.c:1558-1561`).
  - `Document: clone_escrow_context is empty on an unopened document`: `!Document{}.clone_escrow_context().valid()`.
  - `Document: page_hits on an unopened document is empty`: `REQUIRE(Document{}.page_hits(0, "x", {}).empty());`. This pins the wrapper's guard.
  - In `test_document_search.cpp`, update the `page_hits abort_flag + leak stress` case:
    - delete its pre-aborted leg (now covered above);
    - drop the abort argument from **every** remaining call, at lines 125, 126 and 128;
    - rename it `Document: page_hits leak stress`.

    Also drop the `nullptr` 4th argument at lines 94, 100 and 101. After the edit, `rg -n "page_hits\(.*nullptr" src tests` must print nothing.
- [ ] **Step 2: Build and confirm it fails to compile** (`page_hits_on`, `request_abort`, `clone_escrow_context` undeclared).
- [ ] **Step 3: Implement.** Move the body of `Document::page_hits` into `page_hits_on` unchanged, except:
  - the build becomes `fz_load_page` + `fz_new_stext_page_from_page_with_cookie(ctx, page, &so, cookie)` with the same `FZ_STEXT_DEHYPHENATE` options, and the page is dropped in the `fz_always`. Add `page` to the existing `fz_var(...)` list: `fz_try` is setjmp-based (`system.h`), and `context.h:87-91` asks for `fz_var` on any local changed inside it;
  - the match-loop check becomes `if (cookie && abort_requested(*cookie)) break;`;
  - the failure log line keeps its text.

  Rewrite every comment that ties `doc_mutex` to search workers or `page_hits`. The lock now serialises only the tab Document's own context, and search workers no longer take it (#130). The sites:
  - `Document.cpp:51-67`, `:275-276`, `:407-408`, `:431-432`, `:607-608`;
  - `Document.hpp:139-146` (drop the "SearchSession workers rely on this guarantee" line) and `:216-220`.

  Check with `rg -n "page_hits|search worker" src/core/Document.cpp src/core/Document.hpp`: the only remaining hits are the function itself and its declaration.
- [ ] **Step 4: Run** `build\tests\Release\litepdf_unit_tests.exe "[document][search]"` and the four new cases by name. Expected: all pass, including the unchanged `page_hits: page 0 has 12 Lorem hits`.
- [ ] **Step 5: CLI.** Build `litepdf-cli` too (`--target litepdf-cli`). Expected: it compiles with the `bench_selection.cpp:80` edit.
- [ ] **Step 6: Interim call site.** `SearchSession.cpp:219` calls `doc_ref.page_hits(i, needle_utf8, df)` without the token. Until Task 2, a superseded generation stops only between pages (epoch check). Leave `GenerationAbort` untouched. Run `"[search]"`; expected: all pass.
- [ ] **Step 7: Commit** `refactor(core): lock-free page_hits helper that builds text with a cookie (#130)`.

## Task 2: SearchSession scans its own document; each generation owns a cookie

**Files:**
- Modify: `src/core/GenerationAbort.hpp` (token type and comments)
- Modify: `src/core/SearchSession.hpp` (ctor, header comment `:1-18`), `src/core/SearchSession.cpp` (Impl, ctor, dtor, task lambda)
- Modify: `src/core/DocumentView.cpp:172` (pass `password`)
- Test: `tests/unit/test_generation_abort.cpp`, `tests/unit/test_search_session.cpp`

**Interfaces:**
- Consumes (Task 1): `detail::page_hits_on`, `detail::request_abort`, `detail::abort_requested`, `Document::clone_escrow_context()`.
- Produces:
  - `GenerationAbort::begin_generation()` returns `std::shared_ptr<fz_cookie>` (value-initialised). It calls `request_abort` on the previous cookie, and `abort_active()` on the current one. A cookie is never reset (MuPDF contract, spec §3.4). `GenerationAbort.hpp` includes `core/detail/PageHitsAccess.hpp`.
  - `SearchSession(const Document& doc, litepdf::app::ISearchDispatcher& dispatcher, std::optional<std::string_view> password = std::nullopt);` Throws `std::runtime_error` if the clone, the open or the authentication fails.

- [ ] **Step 1: Write T1 alone.** No other test change yet: the unit tests are one executable (`tests/CMakeLists.txt:20`), so anything that does not compile against Task 1's code would block this RED run. In `test_search_session.cpp`, add `SearchSession: a scan completes while doc_mutex is held` (tag `[search][session][threads]`).
  - Build `Document doc = open_search_fixture(); ThreadPoolDispatcher pool(2); SearchSession s(doc, pool);` **outside** the callback.
  - Call `doc.with_rendered_page(0, 1.0f, 1.0f, false, cb)`. `cb` only calls `s.set_query(L"Lorem", {})`, polls `s.scan_complete()` every 5 ms for at most 5 s, and stores `complete` and `count = s.hit_count()` in locals.
  - After it returns: `REQUIRE(complete); REQUIRE(count == 15);`.
  - Explain in a comment why there is no `REQUIRE` and no locking Document call inside the callback (spec §6 T1). `set_query`'s `page_count()` takes no lock.
- [ ] **Step 2: Record the RED.** Build and run that case by name against Task 1's code. Expected: it fails on `REQUIRE(complete)` after ~5 s **and does not hang**. Record the output before implementing; it is the evidence the test is not vacuous.
- [ ] **Step 3: Write the remaining tests.**
  - `test_generation_abort.cpp`: convert each existing case from `tok->load()` to `detail::abort_requested(*tok)`, keeping each case's meaning and name. Add `GenerationAbort: a fresh cookie has no progress`, which checks `tok->progress == 0 && tok->abort == 0`.
  - `test_search_session.cpp`, `SearchSession: scans a user-password PDF with the password`:
    - open `tests/fixtures/encrypted-text.pdf` and `REQUIRE(doc.authenticate("test"))`;
    - construct `SearchSession s(doc, inline_dispatcher, std::string_view{"test"})`;
    - `set_query(L"ENCRYPTED", {})` gives `hit_count() == 8`, since the fixture draws 8 lines (`scripts/generate-encrypted-text-fixture.py:63-65`).

    If the real count differs, stop and report it; do not edit the number. These do not compile until Step 4.
- [ ] **Step 4: Implement.**
  - `Impl` gains `EscrowContext escrow; fz_document* sdoc = nullptr; std::mutex scan_mtx;`. None of these go into `State` (spec §3.6).
  - The constructor clones via `doc.clone_escrow_context()` and opens `doc.source_path()` on it, using the UTF-8 conversion from `RenderEngine.cpp:368-374`. If `password` is non-empty, it authenticates a wiped NUL-terminated copy. On any failure it drops what it opened (the document on `escrow.get()`) and throws `std::runtime_error`.
  - The task lambda captures `fz_context* sctx`, `fz_document* sdoc`, `std::mutex* smtx` and the generation's `std::shared_ptr<fz_cookie>` in place of `&doc_ref` and the `atomic<int>` token. It calls `page_hits_on(sctx, sdoc, i, needle_utf8, df, cookie.get())` with `std::lock_guard(*smtx)` held.
  - The lock lives in a block scope around that call only, opened **after** epoch check #1, so it is inside the drain `Guard`'s scope (`SearchSession.cpp:205-214`) and released before the Guard decrements `pending_tasks`. Declared ahead of the Guard, it would unlock after the decrement, and `~SearchSession` could already have destroyed the mutex. Placed after the epoch check, stale tasks never queue on it.
  - `~SearchSession` keeps the epoch bump, `gen.abort_active()` and the `pending_tasks` spin in that order, then `fz_drop_document(escrow.get(), sdoc)`. The escrow drops its context when `Impl` dies.
  - `set_query`'s `impl_->doc.page_count()` stays on the tab Document; it takes no lock (`Document.cpp:399-402`).
  - Rewrite the stale comments: `SearchSession.hpp:7-18` (the session scans its own document under its own mutex; the tab Document's `doc_mutex` is never taken), the `GenerationAbort.hpp` header (the token is the generation's `fz_cookie`), and `SearchSession.cpp:94-103`.
  - `DocumentView.cpp:172`: `std::make_unique<SearchSession>(impl_->doc, dispatcher, password)`.
- [ ] **Step 5: Run** `"[search]"`, `"[abort]"`, `"[crosstab]"` and the full exe. Expected: all pass, including T1, the unchanged `SearchSession: rapid query changes settle on correct results (ThreadPool)`, and the Ctrl+W race test.
- [ ] **Step 6: Commit** `perf(core): run a search scan on the session's own document (#130)`.

## Task 3: Fail loud on a password-protected file without its password

**Files:**
- Modify: `src/core/SearchSession.cpp` (constructor)
- Test: `tests/unit/test_search_session.cpp`

**Interfaces:**
- Consumes (Task 2): the `SearchSession` constructor.
- Produces: the constructor throws `std::runtime_error` when `fz_needs_password(ctx, sdoc)` is true and `fz_authenticate_password` with the given password does not return non-zero. That covers no password, an empty one, or a wrong one. Authentication is attempted whenever `fz_needs_password` is true, even for an empty password, and no longer only when the password is non-empty.
- **Order:** call `fz_needs_password` once, right after the open and **before** `fz_authenticate_password`; never call it after a successful authentication. `pdf_needs_password` re-runs `pdf_authenticate_password(ctx, doc, "")` (`pdf-crypt.c:826-833`), and that failed blank attempt resets the document's access and key (`:803`), which would de-authenticate it.

- [ ] **Step 1: Write the failing tests.**
  - `SearchSession: refuses a user-password PDF without its password`: with `encrypted-text.pdf` opened and authenticated as in Task 2, both `SearchSession(doc, d)` and `SearchSession(doc, d, std::string_view{"wrong"})` give `REQUIRE_THROWS_AS(…, std::runtime_error)`.
  - `SearchSession: constructor failure and destruction release the session context`. Take an `EscrowContext` probe from the tab Document, using the `clone_from` + `fz_drop_context` pattern of `test_document_selection.cpp:414-417`, and read `before = detail::family_context_count(probe.get())`. After the throwing construction, the count equals `before`. After a successful session is constructed, it is `before + 1`; after that session is destroyed, it is `before` again.
  - `SearchSession: constructor failure releases the session document`. The context count cannot see a leaked `fz_document`: the document holds no context reference, so the escrow drops the context either way. Instead:
    - copy `encrypted-text.pdf` to a temp path (`std::filesystem::temp_directory_path()`);
    - open and authenticate a `Document` on the copy;
    - `REQUIRE_THROWS` a `SearchSession(doc, d)` on it;
    - close the tab Document (`doc.close()`);
    - `REQUIRE(std::filesystem::remove(copy))`.

    On Windows a leaked document still holds the file open, so the delete fails.
- [ ] **Step 2: Run; expect** the no-password construction not to throw (RED: Task 2's branch skips authentication for an empty password). The file-handle case is RED for the same reason: nothing throws yet, so `REQUIRE_THROWS` fails. The context-count case may already pass; that is acceptable, because it pins the rollback.
- [ ] **Step 3: Implement** the `fz_needs_password` branch, with the same wipe and rollback. Exception messages name the cause in English (`"SearchSession: document needs a password"` / `"SearchSession: password rejected"`).
- [ ] **Step 4: Run** `"[search]"`, then the full suite through ctest (Global Constraints). Expected: all pass. Also run `ctest -N -R "SearchSession: "` to confirm every new case is registered by name.
- [ ] **Step 5: Commit** `fix(core): refuse to scan a password-protected file without its password (#130)`.

## Task 4: CHANGELOG and the end-to-end check

**Files:**
- Modify: `CHANGELOG.md` (`## [Unreleased]` → `### Changed`)

- [ ] **Step 1: CHANGELOG.** `## [Unreleased]` is empty today (`CHANGELOG.md:10`). Add a `### Changed` heading under it with one line, per the ship convention (perf → Changed). Claim only what T6 measures: page turns, resizes and tab switches no longer wait for a running search to finish a page; closing a tab or retyping stops the current page's text build early. Reference `(#130)`.
- [ ] **Step 2: T6 GUI probe (nothing committed).** Regenerate the spec PDF if needed: headless Edge `--print-to-pdf` of `third_party/mupdf/thirdparty/gumbo-parser/benchmarks/html5_spec.html` into the scratchpad. Read the scripted-GUI-smoke reference memory's "Read first" list before driving the app. On the `main` build and on this branch, start a search for a never-matching word, then:
  - page with PgDn in fit mode and record the page-turn latency (the `render: accepted` trace, as in #128's check);
  - press Ctrl+W mid-scan and record the tab-close time;
  - record the per-tab peak working set and the session's `fz_open_document` time.

  Instrument with a temporary, uncommitted patch: a `QueryPerformanceCounter` stderr line around the session's open, and the existing render trace. Read the peak working set from PowerShell (`(Get-Process litepdf).PeakWorkingSet64`). Revert the patch with `git checkout -- src` before Step 3, and confirm `git status` shows only `CHANGELOG.md`. The ePub/FB2/SVG/CBZ formats get no cookie bound (spec §3.4); the probe uses the PDF only.

  Put the before/after numbers in the PR body; they replace spec §3.5's upper bounds.
- [ ] **Step 3: Full suite through ctest from PowerShell.** Expected: all pass, and the count equals the baseline `-N` count plus the new cases.
- [ ] **Step 4: Commit** `docs(changelog): note the search-scan responsiveness change (#130)`.
