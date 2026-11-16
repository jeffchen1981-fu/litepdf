# An FB2 tab shares one laid-out document (#229, last item) — design

Status: design agreed in chat 2026-11-15, after a probe and a Fable review of the draft. The six findings of
that review are folded in (§3.2 page lifetime, §3.3 priority gate, §3.1 format predicate, §6.2 and §6.3 oracles).
This spec is written for the user's review before any plan or code.
Spec gate (Full tier, routine): round 1 (Opus) fixed in place: the page-lifetime oracle's parking hook, the gate's
re-entry check before `waiting[cls]++`, the unencrypted PDF-as-`.fb2` test, FB2 content under `.svg`, shared fonts,
and the comment list. Codex (terra, gpt-6.1-terra, luna) on 807dfb8, fixed in one batch: print renders under the gate
but calls back after releasing it (a tab close dispatched by print's message pump could otherwise deadlock), tickets
inside a class, a bounded wait for Scan, named tests for every mutation, and a defined workload for §7's pass bars.
Issue: #229 (body + 3 comments; item 3 of the third comment: "Sharing the Document's laid-out copy"). Base: main at
dde66b6.
Earlier #229 work, all merged, none released: #234 one-pass outline resolve (MuPDF patch), #235 one render worker for
FB2, #236 an FB2 tab opens its search document at the first search.
Related: #232 (ePub workers and search still load pages by flat number) waits on where #229 leaves the copies. This
spec leaves ePub as it is; §8 says how #232 could reuse the gate.

## 1. Problem

An FB2 tab holds several copies of the same book, each parsed and laid out on its own:

- `Document` (`src/core/Document.cpp`, `Document::open`): parsed and laid out at open, on the open thread.
- The render worker (`src/core/RenderEngine.cpp`, constructor): `fz_open_document` per worker, at view
  construction on the open thread. Since #235 an FB2 view has one worker. Its first `fz_load_page` lays the book out
  again before the first page shows.
- The search session (`src/core/SearchSession.cpp`): since #236 it opens its own copy in the first scan task of the
  first search, and checks a `FileStamp` before and after reading the file.

For FB2, `fz_open_document` already runs the whole HTML parse; memory is in the parse, time is in the layout
(#229 comment 3). Measured on 2026-11-15 with a probe linked against main's Release libraries (pinned to the
P-cores; source and readings in `~/.claude/compact-dumps/litepdf/229-tools/share/`), one more copy costs:

| book | parse | layout + count | private bytes |
|---|---|---|---|
| 800 sections, 3.5 MB | 164-259 ms | 394-417 ms | +48.5 MiB |
| 1,500 sections, 6.5 MB | 250-361 ms | 653-728 ms | +91.8 MiB |

GUI today (PR #236's readings, medians of 5, 1,500 sections): first page (T0->T4) 1.96 s, `view_built` 1.39 s,
working set 244 MiB. The first search pays one more parse and layout (about 1 s, +92 MiB) before it finds anything.

## 2. Goal and non-goals

Goal: a tab whose document MuPDF opened as FB2 keeps one `fz_document`, the `Document`'s own. The render worker and
the search session borrow it. Estimated from the per-copy cost above (to be measured, §7): 1,500 sections, first page
about 1.15 s and working set about 150 MiB; the first search starts scanning at once and adds no copy.

Non-goals:
- Any other format. PDF keeps two workers with their own documents, so two pages interpret in parallel. ePub, CBZ,
  XPS and SVG are unchanged (#232 stays open).
- A document thread that runs all page work (MuPDF's recommended "server" shape, `docs/reference/c/overview.md`,
  "Either he can construct it so that a single nominated thread opens the document ..."). It would turn every
  synchronous UI call into a round trip. Considered and declined in the design discussion.
- Search scans on the render worker's queue. Declined: it moves the search protocol of #130, #133 and #140.
- More than one render worker for a shared document. Rasterising is outside the lock (§3.2) and an FB2 page is
  small; a second worker would mostly wait on the gate.

## 3. Design

### 3.1 Which documents share

A view shares its document when both hold:

1. MuPDF opened it as FB2: `fz_lookup_metadata(ctx, doc, FZ_META_FORMAT, ...)` returns `"FictionBook2"`
   (`third_party/mupdf/source/html/html-doc.c`, `htdoc_lookup_metadata` and the FB2 handler's format string). Read
   once in `Document::open` after the open succeeds, and kept in `Document::Impl`. Public reader:
   `bool Document::is_fb2() const noexcept`.
2. No password reached the view. An FB2 never asks for one; the rule keeps "a password is never kept past the
   constructor" (Phase 8 D3) true by construction, with no FB2 special case to reason about.

The extension predicate `one_render_worker` (`src/ui/detail/EffectiveLayout.hpp`) is not used for sharing. A PDF
named `.fb2` opens as a PDF (`tests/unit/test_make_document_view.cpp`, "A PDF named .fb2 opens as a PDF ... and gets
one worker by name"): it keeps its one worker by name, as #235 decided, and a document of its own, because a PDF
page's interpretation is a long hold. The same holds for any other content named `.fb2`.

`ui::make_document_view` (`src/ui/detail/MakeDocumentView.hpp`) decides: `share = doc.is_fb2() && !password`. It
passes `DocumentShare::Shared` or `DocumentShare::Own` (new enum, `src/core/DocumentShare.hpp`, replacing
`SearchOpen`) through `DocumentView` to `RenderEngine` and `SearchSession`. Worker count:
`share || one_render_worker(path) ? 1 : RenderEngine::kDefaultWorkers`. A shared view always gets one worker. That
covers FB2 content under another allowed name: `.svg` has no magic check (`looks_like_supported_document` in
`Document.cpp`), MuPDF scores content above extension, so such a file opens as FB2 and is shared. A `.fb2`-named
file that is not FB2 keeps one worker by name, as #235 decided.

### 3.2 The borrow seam and page lifetime

A core-internal seam, in the style of `detail::TextPageAccess` and `detail::PageHitsAccess`:

```cpp
// core/detail/SharedDocumentAccess.hpp (includes <mupdf/fitz.h>; core only)
namespace litepdf::core::detail {
enum class GateClass { Foreground, Render, Scan };
struct SharedDocumentAccess {
    // Runs fn(fz_document*) with the Document's document, holding the Document's
    // gate in class `cls` for the whole call. `fn` uses the CALLER's own
    // fz_context. Returns false (fn not run) when the document is not open.
    template <class F> static bool with_document(const Document&, GateClass cls, F&& fn);
};
}
```

Rules, each one stated in the seam's header:

- **Every `fz_page` lives inside one critical section.** `fz_load_page`, everything that reads the page
  (`fz_new_display_list_from_page`, `fz_new_stext_page_from_page`, the search on it) and `fz_drop_page` run inside the
  same `with_document` call. `fz_drop_page` writes `page->doc`, `page->chapter` and `page->number` and drops the
  document reference with no lock of its own, and `fz_load_chapter_page` walks the document's open-page list
  (`third_party/mupdf/source/fitz/document.c`, `fz_drop_page` and `fz_load_chapter_page`), so a drop outside the gate
  races another thread's load. No `fz_page*` leaves `fn`.
- **What may leave `fn`:** a display list, an stext page (inside a `Document::TextPage` handle) and copied hits.
  Display lists and stext pages keep fonts and images, not the document or the page, and their refcounts go
  through `FZ_LOCK_ALLOC`; they are dropped later on any context of the family, as today.
- **Fonts are now shared too.** Before, each copy had its own FB2 font set (`html-font.c`, one set per document).
  Now the worker rasterises a display list outside the gate while a search or the UI builds stext inside it, on the
  same `fz_font` objects. The font fields MuPDF fills lazily without a lock (`get_gid_bbox`, `encoding_cache` in
  `source/fitz/font.c`) are reached from rasterising only through clip text and Type 3 glyphs, which the HTML engine
  does not produce. Base-14 and fallback fonts are already shared across threads in the same family today, so this
  is no new class of exposure; it is a re-check item on a MuPDF upgrade (§8).
- **No exception or longjmp crosses `fn`'s boundary.** `fn` wraps its MuPDF calls in its own `fz_try` on its own
  context and is `noexcept` in effect (#179 hygiene: no C++ object with a destructor inside the `fz_try`).
- **The document is dropped on the Document's own context, last**, exactly as today: `~DocumentView` resets the
  session (which drains its tasks), then the engine (which joins its workers), then the `Document` dies
  (`src/core/DocumentView.cpp`, `~DocumentView` and `Impl`'s declaration order). Nothing borrowed outlives it.
  `TextPage` handles may, as today: they hold their own escrow and no document reference.

The engine and the session in shared mode store the `Document&` they were given and no `fz_document*`, so the seam is
the only way to reach the document: skipping the gate does not compile.

### 3.3 The priority gate

`doc_mutex` (`Document::Impl`) becomes `detail::PriorityGate gate` for every Document, shared or not. A Document that
lends nothing has only Foreground users, so its behaviour does not change.

```
acquire(cls):  lock m;
               if (held && owner == this thread) throw std::system_error(resource_deadlock_would_occur);
               my = next_ticket[cls]++;                       // FIFO inside a class
               cv.wait(m, [&]{ return !held && admissible(cls, my); });
               serving[cls]++; held = true; owner = this thread;
               if (cls == Render) renders_since_scan++; if (cls == Scan) renders_since_scan = 0;
release():     lock m; held = false; owner = none; cv.notify_all();

admissible(cls, my):
    my == serving[cls]                                       // the oldest waiter of its class
    && (cls == Foreground || no Foreground waiting)
    && (cls != Scan || no Render waiting || renders_since_scan >= kScanAfterRenders)
    && (cls != Render || no Scan waiting || renders_since_scan < kScanAfterRenders)
```

`waiting(cls)` is `next_ticket[cls] - serving[cls]`. `kScanAfterRenders = 4`.

The re-entry check comes BEFORE the ticket is taken. Placed after it, a throw would leave a Foreground ticket that is
never served, and from then on no Render or Scan is admitted: the worker stops, and closing the tab hangs in
`~SearchSession`'s spin-wait and `~RenderEngine`'s join (§6.2 pins this).

- Order: Foreground first, always. Between Render and Scan, Render first, but a waiting Scan is admitted after at
  most `kScanAfterRenders` consecutive Render holds. Inside a class, first come, first served (tickets), so a
  Foreground caller on another thread (the CLI, tests) cannot be passed by a newer one. Every decision is made under
  the gate's own mutex, so a borrower that started waiting before the UI cannot slip in after it (the
  check-then-lock gap of the first draft).
- Bounds: a Foreground caller waits for the hold in progress plus older Foreground callers (in the app, only the UI
  thread is Foreground, so: the hold in progress). A Render caller waits for the hold in progress, any Foreground
  holds that arrive meanwhile, and at most one Scan hold. A Scan caller waits for at most `kScanAfterRenders` Render
  holds plus Foreground holds, so a search keeps moving while the reader scrolls, and a scan task that
  `~SearchSession` waits for on a tab close is admitted after a bounded number of holds (the engine is still alive
  then, and its queue may still hold prefetch and thumbnail requests).
- RAII guard only (`GateHold`). Methods under it throw (`page_size`, `page_text`), and a leaked hold would hang
  `~RenderEngine` and `~SearchSession`.
- Re-entry on the owning thread keeps today's behaviour: today `std::mutex::lock()` on the owning thread throws
  `std::system_error` (`resource_deadlock_would_occur`), which `Document::text_page` turns into an empty handle
  (#70) and `DocumentView::Impl::known_size` into "no size". The gate throws the same `std::system_error` when
  `owner == this thread`.
- **Print no longer holds the gate across its callback.** Today `with_rendered_page` holds `doc_mutex` while its
  callback runs, and the callback's `StretchDIBits` can run `PrintJob`'s `abort_proc`, which dispatches every queued
  message on the UI thread (`src/printing/PrintJob.cpp`). With borrowers, that would let a dispatched tab close
  (`IDM_TAB_CLOSE`, which `PdfCanvas` posts) destroy the view and join a worker that waits for the gate the UI
  thread holds: a deadlock. It would also let a dispatched command reach `outline()` and throw the re-entry error
  out of a window procedure. So `with_rendered_page` renders, composites over white, drops the page and RELEASES
  the gate, and only then calls the callback with the pixmap, which it still owns and drops after the callback on
  its own context. The pixmap does not depend on the document. The callback's bytes are unchanged.
- Invariant: **no code other than MuPDF calls and their result copying runs under the gate.** In particular no
  message pump, no callback into UI code, no wait on another thread. So the UI thread never waits for a borrower
  while it holds the gate, with no reliance on which windows are disabled. Stated in the gate's header comment.
- Lock order: `scan_mtx` (SearchSession) may be held while acquiring the gate. Nothing else is held while acquiring
  it, and nothing is acquired under it: the gate's holder takes no other lock (the MuPDF family's lock table aside,
  which is a leaf). A borrower never takes `scan_mtx` under the gate.

### 3.4 RenderEngine in shared mode

`RenderEngine(Document&, num_workers, PageCache*, password, DocumentShare)`. With `Shared`:

- The constructor clones the worker contexts as today and opens no document. `worker_docs` stays empty, and no
  shared-mode path reads it: `worker_loop` today loads `wdoc = worker_docs[idx]` up front and passes it to both
  `maybe_deliver_text` calls. In shared mode both build sites reach the document only through `with_document`; the
  plan restructures `worker_loop` so that no `fz_document*` variable exists on that path.
- `worker_loop`, display-list miss: one `with_document(doc, Render, ...)` around load, display list, drop page. A
  failure inside leaves `dlist` null, as today's `fz_catch` does.
- `maybe_deliver_text`: one `with_document(doc, Render, ...)` around `TextPageAccess::build`. The skip rules and the
  re-check after the build are unchanged.
- Outside the gate, unchanged: L1 and L2 lookups, rasterising from the display list, invert, cache puts, the cancel
  checkpoints, `on_complete`, `on_text`.
- New test observability: `std::size_t documents_opened() const noexcept` (count of `fz_open_document` calls this
  engine made: `num_workers` for `Own`, 0 for `Shared`).

`Own` is today's code path, unchanged.

### 3.5 SearchSession in shared mode

`SearchSession(const Document&, ISearchDispatcher&, password, DocumentShare)`. With `Shared`:

- The constructor clones the escrow context as today and opens no document.
- The task lambda keeps its structure: drain guard, epoch check #1, `scan_mtx`, epoch check #1b. Inside, one
  `with_document(doc, Scan, ...)` around `page_hits_on(escrow, doc, i, ...)`, which loads, scans and drops the page.
- Epoch check #1c and everything deferred go (§3.6). A superseded task now costs at most one page scan.
- The cookie abort stays wired; FB2 ignores it, as it does today (#130).
- New test observability: `std::size_t documents_opened() const noexcept` (1 for `Own`, 0 for `Shared`).
- A gate wait sits inside `scan_mtx` and inside the drain guard. `~SearchSession` waits for the running task on the
  UI thread; that task can be waiting for the gate, whose holder is either the render worker (finishes its hold) or a
  Foreground caller on another thread. It is never the UI thread itself, by the invariant in §3.3.

### 3.6 What #236 added and this removes

With the document shared, nothing reads an FB2 by name after open, so these go:

- `SearchOpen` (`src/core/SearchOpen.hpp`), replaced by `DocumentShare`.
- The deferred open: `Impl::open_deferred`, its fields (`path`, `path_utf8`, `stamp`), `failed_epoch`,
  `SearchSession::open_failed()`, epoch check #1c, and `set_scan_hooks` with all three hooks. Every caller of
  `set_scan_hooks` is a `[lazy]` test (`tests/unit/test_search_session.cpp`), so none is left; §6.3's scan test
  uses the gate's own hook instead.
- `FileStamp` (`src/core/FileStamp.{hpp,cpp}`, its CMakeLists line) and `Document::source_stamp()`.
- The find bar's `Reopen file` counter (`MainWindow::update_find_counter`).
- The `[lazy]` tests in `tests/unit/test_search_session.cpp` and the two `make_document_view` tests that pin the
  deferred open; the password test returns to its pre-#236 oracle plus `documents_opened()`.
- The residuals #236 accepted go with it: a saved FB2 that cannot be searched until reopened, a gone share retried
  per query, the FAT32 time-zone false alarm, the same-size same-write-time rewrite.

`open_scan_document` stays for `Own`. #236 is unreleased (no tag contains dde66b6), so its CHANGELOG entry is
rewritten, not answered by a new one (§5).

### 3.7 Comments that are wrong today

Several comments say MuPDF forbids sharing a document across contexts. MuPDF says the opposite: "The document can be
used from several different threads as long as there are safeguards in place to prevent the usages being
simultaneous" (`docs/reference/c/overview.md`, rule 2). Corrected wherever this change touches them:
`RenderEngine.cpp` (the `worker_docs` member, the constructor's open loop, the rollback comment, `maybe_deliver_text`),
`Document.hpp` (`source_path`, `page_hits`, `with_rendered_page`), `Document.cpp` (`Impl::doc_mutex`),
`SearchSession.cpp` (the session's pair), `DocumentView.cpp` (teardown).

Comments that this change makes false in shared mode, rewritten to say what holds in each mode:
`SearchSession.hpp` (the thread-safety block: "A scan never touches the tab Document (#130)", "a search never waits
on a print or a text-selection build"), `detail/TextPageAccess.hpp` ("the render worker (on its own document,
lock-free)"), `detail/PageHitsAccess.hpp` (who serialises `ctx` and `doc`), `EscrowContext.hpp` ("under
doc_mutex"), `RenderEngine.cpp` (`maybe_deliver_text`: "no doc_mutex: nothing else touches them").

## 4. Data flow, one FB2 tab

```
open thread:  Document::open  -> parse + layout (the only copy), is_fb2 = true
              make_document_view -> DocumentView(Shared): engine (1 worker, no document),
                                                         session (escrow, no document)
render worker: [gate Render] load page -> display list -> drop page   | rasterise, cache, post
search task:   scan_mtx -> [gate Scan] load page -> stext -> search -> drop page | sort, publish hits
UI thread:     Document::text_page / page_size / outline / print  [gate Foreground]
```

## 5. User-visible text

- The `Reopen file` counter disappears with the code that sets it.
- CHANGELOG `[Unreleased]`, Fixed: the #236 entry ("An FB2 book that is never searched no longer pays for search
  when it opens ...") is replaced by one entry for this change: an FB2 book keeps one copy in memory for showing and
  searching; it opens sooner and uses less memory, and the first search starts at once. Numbers from §7.

## 6. Tests

### 6.1 Sharing is in place

- `test_make_document_view.cpp`: an FB2 view's engine and session report `documents_opened() == 0`; a PDF view's
  report one per worker and 1.
- An UNENCRYPTED PDF (`simple.pdf`) copied as `.fb2`, no password: engine 1, session 1, one worker by name. This is
  the case that tells the format predicate from the extension predicate. The existing encrypted `.fb2` test cannot:
  its password alone makes it `Own`.
- FB2 content copied as `.svg`: shared (both counters 0) and one worker (§3.1).
- `test_render_engine.cpp`, `test_search_session.cpp`: the same counters on the classes directly, for both modes.
- Behaviour, not a sharing oracle: an FB2 view finds every hit of a search after its file was moved away. An FB2
  lets go of its file at open, so a session that opened its own copy at construction passes this too; only the
  counters above prove sharing.

### 6.2 The gate

New `test_priority_gate.cpp`, deterministic, with no sleeps as oracles (latches and a gate test hook that reports
"waiting in class X"):

- A Foreground waiter goes first: A holds; a Scan and a Render wait (both seen waiting); a Foreground arrives; A
  releases; the Foreground runs before either.
- The first draft's gap: A holds; a Scan is already waiting; the UI arrives; A releases; the UI runs first.
- Render goes before Scan when both wait.
- Re-entry on the owning thread throws `std::system_error`; `Document::text_page` under a held Foreground gate on the
  same thread returns an empty handle (the #70 contract, re-pinned).
- A failed re-entry leaves the gate usable: after the throw and the release, a Render and then a Scan acquire on
  another thread succeed within a deadline (the case where the check sits after the ticket is taken).
- First come, first served inside a class: A holds; Foreground F1 waits, then F2 waits (both seen waiting, in that
  order); A releases; F1 runs before F2. The same for two Render waiters.
- A waiting Scan is not starved: A holds; a Scan waits; Render requests keep arriving (a new one waits before each
  release); the Scan is admitted after exactly `kScanAfterRenders` Render holds.
- A throw under a hold releases it (RAII).
- Print releases the gate before its callback: in the callback passed to `with_rendered_page`, a Foreground
  acquire on the same thread (`page_size`) succeeds, and a Render acquire on another thread completes before the
  callback returns. Today's code throws on the first and blocks the second.

### 6.3 Borrowers go through the gate

- Render: the test holds the gate in class Foreground (seam hook) and submits an uncached render to a shared engine;
  the worker reaches "waiting in class Render" and no display list is built until the test releases.
- Scan: the same with a search task; it reaches "waiting in class Scan" and no hit is published until release.
- Page lifetime: the seam has a test hook `after_release`, run on the borrower's thread inside `with_document` after
  the gate is released and before `with_document` returns. The test parks the borrower there and, from the test
  thread, reads the document's open-page list under a Foreground hold: no entry is live (every entry has
  `page->doc == NULL`). A dropped page stays on the list, marked dead, until the next load reaps it
  (`fz_reap_dead_pages`), so "the list is empty" would be the wrong check. A borrower that keeps its page past the
  gate and drops it after `with_document` returns is parked with that page still live, so the check sees it. Done
  for the render build, the text build and the scan.

### 6.4 Results under contention

`ThreadPoolDispatcher` search + render of every page + UI-thread `text_page` on random pages, all at once, on
`tests/fixtures/strip-chapters.fb2`:

- the search's hits equal a single-threaded scan of the same book (page and quad);
- every pixmap equals, byte for byte, the same page rendered by the same view with nothing else running (a baseline
  taken first, on the same shared document, so a second parse cannot differ);
- every text handle's `copy()` of the whole page equals the single-threaded text.

### 6.5 Test names

ASCII, prefixed by subsystem, so `ctest -R` finds each one (the project's rule for TEST_CASE names). The plan may
split a case, but keeps these names as prefixes:

| id | TEST_CASE | covers |
|---|---|---|
| S1 | `MakeDocumentView: an FB2 view opens no document of its own` | §6.1 counters, FB2 and PDF |
| S2 | `MakeDocumentView: an unencrypted PDF named .fb2 keeps its own documents` | §6.1 predicate |
| S3 | `MakeDocumentView: FB2 content named .svg is shared with one worker` | §6.1, §3.1 worker count |
| S4 | `SearchSession: a shared session opens no document` / `RenderEngine: a shared engine opens no document` | §6.1 on the classes |
| G1 | `PriorityGate: a foreground waiter goes before waiting render and scan` | §6.2 |
| G2 | `PriorityGate: a foreground arriving after a waiting scan goes first` | §6.2 |
| G3 | `PriorityGate: render goes before scan` | §6.2 |
| G4 | `PriorityGate: re-entry on the owning thread throws` and `Document: text_page under a held gate is empty` | §6.2 |
| G5 | `PriorityGate: a failed re-entry leaves the gate usable` | §6.2 |
| G6 | `PriorityGate: waiters of one class are served in arrival order` | §6.2 |
| G7 | `PriorityGate: a waiting scan is admitted after four render holds` | §6.2 |
| G8 | `PriorityGate: a throw under a hold releases it` | §6.2 |
| G9 | `Document: with_rendered_page calls back with the gate released` | §6.2 |
| B1 | `RenderEngine: a shared worker waits for the gate before building` | §6.3 |
| B2 | `SearchSession: a shared scan waits for the gate before scanning` | §6.3 |
| B3 | `SharedDocument: no live page is left past the gate` (render build, text build, scan) | §6.3 |
| C1 | `SharedDocument: results under contention equal the single-threaded results` | §6.4 |

### 6.6 Proving the tests can fail (mutation checks, from a committed tree)

Each mutant must fail the named test, run as `ctest -C Release -R "^<name>"` from PowerShell:

1. The gate ignores class order (one queue by arrival): G1, G2, G3.
2. The worker's display-list build skips the gate (calls the document through a test-only raw accessor): B1.
3. The engine drops the page after leaving `with_document`: B3.
4. `make_document_view` passes `Own` for FB2: S1.
5. The session opens its own document in `Shared` mode: S1, S4.
6. The gate's re-entry check removed: G4 (the deadlock is bounded by the test's own deadline, reported as a failure).
7. The re-entry check moved after the ticket is taken: G5.
8. `make_document_view` shares by extension (`one_render_worker(path) && !password`): S2.
9. Tickets removed (any waiter of the class may go): G6.
10. `kScanAfterRenders` check removed (strict Render over Scan): G7.
11. `with_rendered_page` calls back before releasing the gate: G9.

## 7. Development and measurement

- Build the "before" exe (main at the base SHA) in a detached worktree; never check main's sources out over the
  branch (the #236 lesson: the hook refused to restore them).
- GUI, `litepdf.exe --log-timings`, Release, pinned to 0x5555, `LITEPDF_NO_RESTORE=1`, session.json, running.lock and
  the HKCU MRU key backed up and restored (driver: `~/.claude/compact-dumps/litepdf/229-tools/pr3/measure.ps1`,
  extended for (2)-(4)), the OS file cache warmed by reading the book before each run, before and after
  interleaved, 5 runs per build and book, 800 and 1,500 sections:
  1. first page (T0->T4), `view_built`, working set and private bytes after open (median of 5);
  2. time from a search's start to its first hit and to scan complete (median of 5);
  3. UI-thread gate waits during a scan. Workload per run: 2 s after the first page, the driver starts a search for
     `ab` from page 1, then, while the scan runs, 40 rounds 150 ms apart: a Ctrl+G jump to a page at least 20 pages
     from any page shown or prefetched so far (fixed seed), then 30 ms later a left press on that page. A temporary
     probe (not committed) logs every Foreground acquire's wait. Statistic: the maximum over all acquires of all 5
     runs, with the sample count; under 150 acquires is "not measured". (Amended at the plan gate, 2026-11-17: the
     first workload, 40 presses on one page, reached `Document::text_page` about once per run, because
     `DocumentView::text_for` reuses the text slot after the first press.)
  4. P0 render latency. Workload per run: the same search, and during the scan 40 go-to-page jumps (Ctrl+G) to
     pages chosen by a fixed seed, at least 20 pages from any page shown or prefetched so far, 150 ms apart. The
     probe logs submit to `on_complete` of each P0. Control: the same jumps with no search. Statistic: p99 by
     nearest rank over the 200 pooled samples of each, after against control.
- Pass bars, decided before measuring: (3) under 16 ms, (4) the scan's p99 within 30 ms of the control's p99. A miss
  stops the PR for a design change, not a merge with a residual.
- ctest serial from PowerShell, Release.

## 8. Risks

- **A MuPDF upgrade adds per-document state touched outside a page.** The safety reading of §3.2 is for 1.27.2's HTML
  handler. Re-read `html-doc.c` and `fitz/document.c`'s page list on any MuPDF bump; the #234 patch-level check already
  forces attention there.
- **A future Document method forgets the gate.** Every const method that touches `impl_->doc` takes `doc_mutex`
  today (`with_rendered_page`, `page_size`, `read_ahead_page_sizes`, `page_text`, `page_hits`, `query_compiles`,
  `outline`, `text_page`); those become Foreground holds. `open`, `close`, `authenticate`, `is_open` and the context
  clones touch it without the lock. They stay safe because a view's Document is reached only as `const Document&`
  once the view exists, and `open`/`close`/`authenticate` are non-const; the spec keeps that rule and the seam's
  header states it. A shared borrower cannot skip the gate (§3.2, no stored `fz_document*`).
- **Fonts shared across threads (§3.2).** Re-check MuPDF's lazily filled font fields on any MuPDF bump, with the
  HTML engine's output (no clip text, no Type 3).
- **Search finishes later while the reader turns pages.** By design (Scan is the lowest class, with one Scan hold
  admitted per `kScanAfterRenders` Render holds); measured in §7 (2).
- **#232.** If ePub later shares, the gate and seam are reusable, but ePub's chapter walk makes a hold longer; it
  needs its own measurement first.
