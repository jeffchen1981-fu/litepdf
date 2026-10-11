# Run a search scan on the session's own document — design

**Date:** 2026-10-11
**Baseline:** `main` @ `4adbda7`
**Issue:** #130 (a search scan holds `doc_mutex` through each page's cold text
build, so page turns can wait behind it)
**Ships as:** one PR.

## 1. Motivation

`SearchSession` workers call `Document::page_hits` (`SearchSession.cpp:219`),
which takes the tab Document's `doc_mutex` (`Document.cpp:502`) and holds it
through the cold structured-text build (`fz_new_stext_page_from_page_number`,
`:522`). The scan's abort flag is polled only in the match loop (`:527`), after
the build, so a cancel cannot cut a build short.

The UI thread takes the same lock on paths that run far more often than a
search (#98 spec §1.1):

- every fit-mode page turn and every resize refit (#109):
  `DocumentView::set_viewport` → `Document::page_size` (`DocumentView.cpp:258,
  266`; `Document.cpp:404-409`), which loads the page on every call;
- every tab switch: `Document::outline()` (`MainWindow.cpp:633, 796`);
- find-bar typing with regex or whole word: `query_compiles` (`Document.cpp:582`);
- a press or Select All whose text has not reached the #98 slot yet:
  `Document::text_page` (`DocumentView.cpp:441`).

### 1.1 Measurement

`litepdf-cli <file> --bench-selection --iterations 3` on the HTML5 spec printed
to PDF by headless Edge (`third_party/mupdf/thirdparty/gumbo-parser/benchmarks/html5_spec.html`,
753 pages, 52.9 MB). The `scan1st` column is one cold `page_hits` call, which is
how long a scan holds `doc_mutex` on that page:

| | value |
|---|---|
| median per page | 18.1 ms |
| worst page | 91.5 ms (p336) |
| pages > 16.7 ms (one frame) | 555 of 753 |
| warm `search` column, worst page | 11.6 ms |

The cold/warm gap is font loading on the tab Document, which no other document
can share (MuPDF keys PDF resources in the store by owning document,
`pdf-store.c:35`). This copy has no page above 100 ms. #98 measured 100-220 ms
on Type3-heavy pages of a copy printed by a different Edge build, so the
worst case depends on the file.

The peak matters less than the duty cycle. Two dispatcher workers alternate on
the one lock, so across a scan (753 × ~18 ms ≈ 14 s) `doc_mutex` is almost
always held. A page turn during a scan waits for whatever build is in progress,
and MSVC's `std::mutex` (an SRW lock) is not fair, so the UI thread can lose
the lock to the other worker more than once in a row. That loss rate was not
measured.

## 2. Goal and success criteria

**Goal:** a search scan never takes the tab Document's `doc_mutex`.

**Success criteria:**

1. While `doc_mutex` is held by another thread, a full scan still completes with
   the correct hit count (§6, T1).
2. On a user-password PDF, a scan finds the same hits it finds today (§6, T2).
3. For PDF, closing a tab or retyping mid-scan no longer waits for a whole
   cold build. MuPDF 1.27.2's PDF interpreter checks the abort before each
   content-stream token, so the wait ends at the next token, after any font load
   or page load already in progress (§3.4). Other formats keep today's
   whole-build wait. XPS also polls the cookie, but no XPS fixture exists, so
   nothing is claimed for it.
4. No new UI-thread stall: the extra document opens off the UI thread for
   files without a password (§3.5).

## 3. Design

### 3.1 The session's own context and document

`SearchSession` opens its own `fz_document` in its constructor, on its own
context, and every scan task runs on that pair. Nothing else ever touches it.

- **Context:** an `EscrowContext` cloned from the tab Document's context. A bare
  `Document::clone_context()` would also work today, because every owner
  destroys the session before the Document (§3.6). An `EscrowContext` keeps that
  true by construction if a future owner breaks the order, at no extra cost.
  `Document` gains one accessor that returns an `EscrowContext` cloned from its
  context; the clone needs no `doc_mutex` (`fz_clone_context` locks through the
  family's lock table, `Document.cpp:478-481`).
- **Document:** `fz_open_document` on `doc.source_path()` (UTF-8, the same
  conversion as `RenderEngine.cpp:368-374`).
- **Lock:** a session-owned `std::mutex` serializes the two dispatcher threads on
  the one context and document. MuPDF allows a document to be used from several
  threads as long as the uses are never simultaneous
  (`third_party/mupdf/docs/reference/c/overview.md`, "Multi-threading", rules 1-2),
  and `fz_context` keeps no thread-local state (no `thread_local`,
  `__declspec(thread)` or `_Thread_local` anywhere in MuPDF's `source/` or
  `include/`). Today's `doc_mutex` relies on the same rule. Per-tab search
  parallelism stays at 1, as it is today. A task re-checks the generation
  after taking the lock, because it may have waited there while a newer query
  replaced its own.

### 3.2 Password

The constructor takes `std::optional<std::string_view> password = std::nullopt`,
the type #126 gave `RenderEngine` (it rejects integer arguments). It copies the
password into a NUL-terminated string, authenticates, and wipes the copy on
every exit (Phase 8 D3, as in `RenderEngine.cpp:389-394`).

Unlike `RenderEngine.cpp:414`, which authenticates only when the password is
non-empty, the session asks MuPDF: if `fz_needs_password` is true and
authentication does not succeed (no password, or a wrong one), the constructor
throws. An unauthenticated document does not fail: MuPDF's blank-password
attempt at open leaves a key behind, and every content stream decrypts to
garbage (#126). The scan would report zero hits on every page, silently.
Throwing makes a missing pass-through fail when the tab opens.

`fz_needs_password` is called once, right after the open and before any
`fz_authenticate_password`, and never after one. `pdf_needs_password`
re-runs a blank-password authentication (`pdf-crypt.c:826-833`). On an
already-authenticated document, that failed attempt resets the access and key
(`:803`), which undoes the authentication.

`DocumentView` already receives the password (`DocumentView.hpp:75`) and passes
it to `RenderEngine`. It passes the same value to `SearchSession`.

### 3.3 Running a scan

The body of `Document::page_hits` moves into a helper in `Document.cpp` that
takes a context, a document and a cookie, and takes no lock. `Document::page_hits`
stays as a wrapper that takes `doc_mutex` and calls the helper with no cookie.
Its `std::atomic<int>* abort_flag` parameter is removed. Once scans use the
helper, the only non-null use left is one leg of a test, which moves to the
cookie test (§6, T3). The callers that pass `nullptr` drop the argument:
`bench_selection.cpp:80` and five calls in `test_document_search.cpp`. The
wrapper keeps today's guard for an unopened document or an empty needle.
`litepdf-cli --bench-selection` and the existing `page_hits` tests use the
wrapper. Scan tasks call the helper under the session mutex with the
session's pair. The helper is exposed through the narrowest surface that lets
`SearchSession.cpp` call it; the plan picks the exact form.

Behaviour that does not change: hit geometry, the 256-quad cap and its log line,
needle construction, the empty-result-on-failure contract, epoch checks,
`pending_tasks` accounting, hit sorting, and `query_compiles`, which keeps using
the tab Document. After this change only the UI thread takes `doc_mutex`, so
that lock no longer waits on a search.

### 3.4 Aborting a build in progress

The build changes to `fz_load_page` + `fz_new_stext_page_from_page_with_cookie`
(`util.h:85`), with the same `FZ_STEXT_DEHYPHENATE` option. MuPDF polls
`cookie->abort` before every content-stream token (`pdf-interpret.c:1556-1561`)
and stops the build early **without throwing**. It returns a partial page,
which is safe because the match loop checks the abort before searching it.

Each search generation owns one `fz_cookie`, replacing today's `atomic<int>`
token from `GenerationAbort`:

- the generation's tasks share it. The session mutex serializes them, so MuPDF's
  writes to `progress`, `errors` and `incomplete` never race each other;
- `begin_generation` (the previous generation's cookie) and `abort_active`
  (`~SearchSession`) set `abort` to 1 with an interlocked store and never reset
  it. That matches MuPDF's contract: once the application sets `abort`, it may
  not change it again (`device.h`, the `fz_cookie` comment);
- the match loop reads the same `abort` field in place of the `atomic<int>`,
  with an interlocked read (`_InterlockedCompareExchange(&abort, 0, 0)`).
  `std::atomic_ref` would be the portable form, but it needs C++20, and the
  project is C++17 (`cmake/CompilerFlags.cmake:5`).

**Accepted: MuPDF's read of `abort` is a formal data race.** Our writes and our
read are interlocked, but MuPDF reads the field with plain loads
(`pdf-interpret.c:1558`, `pdf-run.c:291, 318, 601`). Under the C++ (and C11)
memory model, a plain read concurrent with another thread's write is a data
race. This is MuPDF's defined API, not a misuse:

- the `fz_cookie` comment (`device.h:440-445`) describes it as intended for an
  application where "another thread wants to … abort a job", with
  communication "unsynchronized without locking";
- MuPDF's own code writes `abort = 1` from a controlling thread the same way
  (`source/tools/muraster.c:719`, `platform/java/jni/cookie.c:59`).

No race-free alternative exists short of not using the cookie, which would drop
§3.4 altogether. The practical exposure on this toolchain (MSVC, x64) is small:

- the field is an aligned 32-bit `int`, so a load or store is a single
  instruction;
- MuPDF re-reads it through the pointer on every token, between calls the
  compiler cannot see into, so the read is not hoisted;
- the value only ever goes 0 → 1, so a stale read can only delay an abort,
  never undo one.

`GenerationAbort.hpp` is MuPDF-free today, and `fz_cookie` is a typedef of an
anonymous struct, so it cannot be forward-declared. The plan decides how the
token carries the cookie: an opaque handle, or the header including
`mupdf/fitz/device.h`. The constraint is that `SearchSession.hpp` stays
MuPDF-free.

**What the cookie bounds, and what it does not:** MuPDF's contract promises no
bound. The `fz_cookie` comment says "there is no upper bound on exactly when the
rendering will abort". The bound here is an observation of 1.27.2's PDF
interpreter, which checks `abort` before every token (`pdf-interpret.c:1556`),
so an abort lands at the next token there. A MuPDF upgrade must re-check it. Some work cannot be interrupted, so a close or retype can still wait for
one such item, but no longer for the rest of the page:

- a font load in progress (`pdf_load_font`, which takes no cookie and is most of
  the cold cost in §1.1);
- `fz_load_page` itself;
- a single large token, such as an inline image.

The bound holds only where MuPDF polls the cookie: PDF (`pdf-interpret.c`,
`pdf-run.c`), XPS (`xps-common.c`) and display lists. The HTML-based formats
(ePub, FB2) accept a cookie in `htdoc_run_page`/`epub_run_page` and never read
it, and so do SVG and CBZ. Those formats keep today's whole-build wait on close
or retype. Their scans still run on the session's own document, so they no
longer take the tab Document's `doc_mutex` either, which is the issue's main
point.

### 3.5 Where the extra open runs

The session's `fz_document` is the fourth document per tab, after the tab
Document and the two render workers (`DocumentView.hpp:76`, `num_workers = 2`;
neither `MainWindow` construction site overrides it).

- **Files without a password:** `DocumentView`, and therefore the session, is
  built on the open worker thread (`MainWindow.cpp:241`, `:296`). The UI thread
  does not see the cost, but the tab appears one open later
  (`WM_USER_OPEN_OK` is posted after construction, `MainWindow.cpp:309`).
  Accepted: one open, bounded below.
- **Files with a password:** `DocumentView` is built on the UI thread after the
  password dialog (`MainWindow.cpp:2111`), which already opens the two render
  worker documents there. This adds a third open. Accepted: password-protected
  files are rare, and the dialog has just closed.

Measured (Fable review, 2026-10-11) on the 52.9 MB spec PDF with a warm file
cache: one more document costs at most ~11 MB of peak working set and at most
~50 ms to open. These figures are upper bounds derived from
`litepdf-cli --render 0`, not direct measurements of `fz_open_document`. They
are re-measured in §6, T6.

Resources the session's document loads, such as fonts, go into the family's
shared store (256 MB, `context.h:317`). That is not new memory pressure: today's
scans load the same resources keyed on the tab Document, which after this change
loads them only for its own rare calls.

### 3.6 Lifetime

- The session's `Impl` owns the `EscrowContext` and the document. They never go
  into `State`: a task lambda holds `State` by `shared_ptr`, and its last
  reference can be released on a worker after the Document has died (the #61
  shape).
- `~SearchSession` keeps its order: bump the epoch, abort the active
  generation (now also its cookie), and spin until `pending_tasks` reaches 0
  (`SearchSession.cpp:89-116`). After that, no task can still be using the
  document, so the destructor drops the document on its own context, then the
  context.
- Constructor failure (clone fails, open fails, password rejected) rolls back
  what it acquired, drops the document before the context, and throws
  `std::runtime_error`. That is the shape of `RenderEngine.cpp:429-440`.
  `DocumentView`'s constructor already unwinds its `Impl` members in order
  (#74).
- Owners today, all of which destroy the session before the Document:
  `DocumentView::Impl` (session declared after `doc`, `DocumentView.cpp:56, 99`;
  reset first in the destructor, `:197`) and the tests in
  `test_search_session.cpp` (fixture member order `doc` → dispatcher → session,
  and block-scoped sessions). `CrossTabSearch` holds `DocumentView*` only.

## 4. Files touched

| File | Change |
|---|---|
| `src/core/Document.hpp/.cpp` | `page_hits` body moves into a lock-free helper with a cookie; the wrapper loses its abort parameter (§3.3); new `EscrowContext` accessor; every `doc_mutex` comment that names search workers or `page_hits` is rewritten (`Document.cpp:51-67, 275-276, 407-408, 431-432, 607-608`, `Document.hpp:139-146, 216-220`): search no longer contends |
| `src/cli/bench_selection.cpp` | drop the `nullptr` abort argument (`:80`) |
| `src/core/EscrowContext.hpp` | the `clone_from` caller list (`:49-50`) names the new accessor |
| `src/core/SearchSession.hpp/.cpp` | password parameter; own context, document and mutex; tasks call the helper; generation token carries the cookie |
| `src/core/GenerationAbort.hpp` | token type (§3.4) |
| `src/core/DocumentView.cpp` | pass the password to `SearchSession` |
| `tests/unit/…` | §6 |
| `CHANGELOG.md` | one line under `[Unreleased]` → Changed (perf), per the ship convention |

## 5. Alternatives considered

- **Cookie abort + cache `page_size`, no own document (the issue's first and
  third remedies).** The cookie shortens the wait only after a cancel, not
  during a scan that keeps running. A lazy page-size cache still takes the lock
  on a page's first visit, which is exactly when a page turn happens; an
  eager cache loads all pages at open. `outline`, `query_compiles` and the text
  fallback keep waiting.
- **Open the session's document lazily, on the first search, on a worker.**
  Saves the open for tabs that never search, but a password-protected file
  would have to keep its password in memory until then, which undoes Phase 8 D3.
- **Run the scan on the RenderEngine workers.** They already hold authenticated
  documents (#126) and a text builder (#98). A scan request at the lowest
  priority would yield to P0/P1 renders, and this would need no fourth document.
  Rejected for this issue: scans would compete with P1 prefetch and thumbnails
  for the same two workers, `RenderEngine` would grow a second request kind,
  and the `ISearchDispatcher`/`InlineDispatcher` seam that the search,
  cross-tab and Ctrl+W race tests are built on would need a replacement.
  Worth revisiting if the fourth document's memory ever matters.

## 6. Testing

Release build, ctest (see the build/test reference memory). Test names are
ASCII and prefixed `SearchSession:` / `Document:`.

- **T1 — a scan does not need `doc_mutex` (RED before the change).**
  `Document::with_rendered_page` holds `doc_mutex` while it calls its callback
  (`Document.cpp:277`, `:360`). Build `search.pdf`'s Document, a
  `ThreadPoolDispatcher(2)` and the `SearchSession` **outside** the callback.
  Inside it, only call `set_query(L"Lorem")` and poll `scan_complete()` for at
  most 5 s, recording the outcome in locals. Assert after the callback returns.
  Expected: `scan_complete()` and `hit_count() == 15`.
  - The count is pinned because `scan_complete()` alone cannot tell a correct
    scan from one where every page failed: `pages_remaining` is decremented
    whether or not `page_hits` found anything (`SearchSession.cpp:244-246`),
    and `page_hits` reports failure as an empty result (`Document.cpp:550-553`).
  - Before the change the workers block on `doc_mutex`, the poll times out, the
    callback returns, the lock is released, and the scan drains, so the test
    fails cleanly instead of hanging. Destroying the session inside the
    callback would hang instead: `~SearchSession` spins on tasks blocked on
    the held lock.
  - No `REQUIRE` inside the callback: it runs outside `fz_try`, and a throw
    from it leaks the pixmap (`Document.cpp:360`). No Document method inside
    it either: same-thread re-entry of `doc_mutex` throws on MSVC.
- **T2 — password.** Open and authenticate `encrypted-text.pdf`. A session built
  with the password finds `ENCRYPTED` exactly 8 times
  (`scripts/generate-encrypted-text-fixture.py:63-65` draws 8 lines). A session
  built without it throws. This pins §3.2's fail-loud branch, and the
  exact count rules out the garbage-decrypt mode, which yields 0.
- **T3 — the cookie reaches the build.** Call the helper directly on a page
  with text: with a fresh cookie, `cookie.progress > 0` afterwards. With
  `abort` preset to 1, `progress == 0`, because MuPDF checks `abort` before
  incrementing `progress` (`pdf-interpret.c:1558-1561`). The first leg fails if
  the cookie is not passed to the build. Without a cookie, both legs read 0, so
  the pair is not vacuous.
- **T4 — generation abort sets the cookie.** Extend
  `test_generation_abort.cpp`: `begin_generation` sets the previous
  generation's cookie `abort`, and `abort_active` sets the current one's;
  a fresh generation's is 0.
- **T5 — constructor failure leaks no context.** Using the ctor-unwind leak
  oracle (`detail::family_context_count` before and after), a session whose
  constructor throws (T2's missing password) leaves the family's context count
  unchanged. The count cannot see a leaked `fz_document`, which holds no context
  reference. A second check therefore runs the throwing constructor on a temp
  copy of the fixture, closes the tab Document, and deletes the copy. On
  Windows the delete fails while a leaked document still holds the file open.
  A companion test validates that oracle and shows the session holds its own
  document: with the tab Document closed and a live session on a temp copy, the
  delete fails, and after the session is destroyed it succeeds.
- **Existing tests stay green unchanged**: `test_search_session`,
  `test_cross_tab_search`, `test_crosstab_ctrlw_race`, `test_document_view`,
  `test_page_anchor`, and every `page_hits` test (they use the wrapper), except
  that every call drops its `nullptr` abort argument and `page_hits abort_flag +
  leak stress` loses its pre-aborted leg to T3.
  `test_generation_abort` changes with the token type (T4). The default `password = std::nullopt` keeps every existing
  `SearchSession(doc, dispatcher)` call compiling, and every fixture they use is
  unencrypted.
- **T6 — GUI probe (nothing committed).** On the spec PDF, start a search for a
  word that never matches, then page with PgDn in fit mode, and press Ctrl+W
  mid-scan. Compare page-turn latency (`render: accepted` trace, as in #128's
  check) and tab-close time, before and after. Also time the session's
  `fz_open_document` and peak working set per tab, to replace §3.5's upper
  bounds.

## 7. Accepted risks and out of scope

- **Damaged PDFs.** The tab Document repairs a damaged file during open (#119's
  page-0 lookup), while the session's document repairs lazily on first use.
  Whether the two can number pages differently is unverified. This is the same
  accepted risk as #98 §8 for the render workers, and the worst outcome is
  hits reported on the wrong page of a damaged file, never memory safety.
- **A font load in progress is not abortable** (§3.4).
- **Retyping still serializes behind the old generation's in-flight build** on
  the session mutex. The cookie shortens it to one font load. This affects
  search latency, not the UI thread.
- **`doc_mutex` contention from the UI's own paths** (`page_size` loading the
  page each call, `outline` on every tab switch) is unchanged. It is UI-thread
  work, not waiting on another thread.
- **MuPDF's fine-grained family locks** (`FZ_LOCK_ALLOC` on every allocation,
  FreeType, the glyph cache) are still shared by the UI, render and search
  threads. They are held for microseconds, the contention exists today, and
  T6 is the end-to-end check.
