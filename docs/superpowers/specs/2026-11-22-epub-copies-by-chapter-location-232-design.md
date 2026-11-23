# ePub pages by chapter location in every document copy (#232) — design

Issue #232, the follow-up #230 left open until #229 settled the per-worker copies. Background:
`2026-11-12-epub-page-sizes-by-location-230-design.md` (the table, its invariant, §4.2) and
`2026-11-15-fb2-shared-document-229-design.md` (DocumentShare, the gate).

## 1. Problem

#230 made `Document::page_size` and `Document::with_rendered_page` load an ePub page by its
chapter location (`fz_load_chapter_page`) from a `detail::ChapterPages` table built at open. Every
other per-page path still loads by flat number with `fz_load_page`:

| path | document it loads from | caller's thread |
|---|---|---|
| `load_display_list` (`RenderEngine.cpp`), display-list miss | each render worker's own copy (Own), or the Document's (Shared) | worker |
| `detail::TextPageAccess::build`, text prefetch (#98) | the worker's copy, or the Document's | worker |
| `detail::TextPageAccess::build`, `Document::text_page` | the Document's | UI |
| `detail::page_hits_on`, `SearchSession` scan | the session's own copy (Own), or the Document's (Shared) | search pool |
| `detail::page_hits_on`, `Document::page_hits` | the Document's | caller |
| `Document::page_text` | the Document's | caller (CLI) |

#229 settled the copies: only an FB2 tab shares the Document's `fz_document`
(`MakeDocumentView.hpp`: `doc.is_fb2() && !password`). An ePub tab still opens one copy per
render worker (2) and one for its search, so each copy needs locations of its own.

## 2. Cause (confirmed, MuPDF 1.27.2 source)

`fz_load_page(n)` (`fitz/document.c:784-795`) walks the chapters from the first, asking each
one's page count, then calls `fz_load_chapter_page`. For an ePub the count is
`epub_count_pages`, which walks the spine from its head and then `count_chapter_pages`
(`epub-doc.c:194-210`). That returns the accelerator's cached count, or, on a miss, **parses and
lays out the chapter** to find it.

So the cost has two parts:

- **On every document, per call:** the O(c²) spine walk for a page in chapter c (≤ 2.2 ms at
  1,500 chapters). The whole-document search is the one loop that pays it per page: 864 ms
  by location against 3,611 ms by flat number at 1,500 chapters (the issue's probe).
- **On a fresh copy, once per chapter:** a copy opened with `fz_open_document` has an empty
  accelerator. The first flat load of a page in chapter c lays out chapters 0..c, because
  `fz_load_page` counts chapter c's pages before it loads from it (`document.c:788-792`), where a
  load by location lays out chapter c alone; and
  `TextPageAccess::build`'s `fz_count_pages` lays out the whole book, on each worker copy. The
  issue's table does not count this part; it is found by reading the source, not yet measured.

## 3. Goal and non-goals

Goal: every per-page load of §1 loads a multi-chapter page by its chapter location, on every
document it runs on, so a whole-document search and a worker's first page of a late chapter
cost what loading by location costs.

Non-goals:

- No MuPDF patch. The O(c) spine walk inside `epub_load_page` stays (#230 §7).
- No change to which documents are shared (#229) or to the worker count (#235).
- No change to the borrow seam (§4.5).

## 4. Design

### 4.1 Approaches

1. **Chosen: every copy uses the Document's table, after a check against that copy.** One
   `fz_count_chapters` per copy; no layout.
2. Each copy builds its own table from its own counts: every copy lays out the whole book when
   it opens (each worker and the search), the cost #229 removed for FB2. Rejected.
3. Each copy builds its table lazily, at first need: the same layout, later. Rejected.
4. Patch MuPDF to cache chapter starts in `fz_load_page`: a second local patch to carry across
   MuPDF bumps, for what a table in litepdf already does. Rejected.

### 4.2 Why the Document's table is right for a copy

- **Only one crash.** `epub_load_page` (`epub-doc.c:592-623`) throws on a negative chapter or
  page, returns NULL when the spine ends before `chapter`, and never checks the page number
  against the chapter's page count. `fz_load_chapter_page` (`document.c:953-955`) writes
  `page->chapter` through that NULL. A page number past a chapter's count draws background only
  (`fz_draw_html`). So the only crash is a chapter at or past the copy's chapter count.
- **The check is cheap.** `fz_count_chapters` runs `fz_ensure_layout` → `epub_layout`
  (`epub-doc.c:253-286`), which stores w/h/em and invalidates an empty accelerator, then counts
  the spine list (`epub_count_chapters`). Nothing is parsed or laid out.
- **What the check guards.** For the same file, two opens always give the same spine in 1.27.2:
  a spine item enters it when `path_from_idref` finds it in the manifest (deterministic), and the
  "ignoring chapter" catch in `epub_parse_header` (`epub-doc.c:1057-1069`) is unreachable, because
  `epub_load_chapter` (`epub-doc.c:448-466`) only allocates and so throws only
  `FZ_ERROR_SYSTEM`, which that catch rethrows. The check guards the case where the file on disk
  is replaced between the Document's open and a copy's (§6.2 makes it real), and any future
  MuPDF where the spine can differ. It is one spine walk, so it stays.
- **The counts match.** A copy is the same file, opened with `fz_open_document` (no accelerator
  file), laid out with the same defaults (`fz_ensure_layout`: DEFW, DEFH, DEFEM). Its context is a
  clone of the Document's root context, and `fz_clone_context` shares the style, font and store
  contexts (`context.c:338-343`), so the user CSS, the document-CSS flag and the system CJK font
  loader (`install_system_cjk_font_loader`, on the root) are the same. litepdf calls no
  `fz_layout_document`, `fz_set_user_css` or `fz_set_use_document_css` (`git grep` over `src`).
  §6.2 asserts the counts equal on the fixtures.
- **Accepted residual.** If the file is replaced on disk between the Document's open and a
  copy's, with the same chapter count, a copy may show a blank page or another page of the new
  file. Today it shows the new file's page n or fails the render; the tab is already showing a
  file that is gone either way.

The #230 rule "the table must only ever hold locations of the document it was built from"
becomes: **a table may serve any `fz_document` whose chapter count equals the table's, checked
right after that document is opened (and authenticated)**, under the same no-re-layout invariant
for every document it serves. The `Impl::chapters` comment and the new helpers say so.

### 4.3 Units

`ChapterPages` (`detail/ChapterPages.hpp`, MuPDF-free) gains `page_count()` (`starts_.back()`),
which `TextPageAccess::build` needs (§4.4).

**Ownership.** `Document::Impl::chapters` becomes `std::shared_ptr<const ChapterPages>`, built by
`read_chapter_pages` as today (a `std::bad_alloc` from `make_shared` → no table), assigned at
every successful open and authenticate, reset on close. A copy holds the same pointer, so taking
it allocates nothing and cannot throw; and it stays valid after the Document closes or reopens,
because the copy was checked against its own document. The table has no mutator, so the worker
and search threads read it without a lock.

**Read once from the Document.** `detail::ChapterPagesAccess::table(const Document&)` returns the
pointer, read under a Foreground hold (a friend of `Document`, like `SharedDocumentAccess`).
Called by the RenderEngine and SearchSession constructors, before any of their threads start.
The hold can throw `std::system_error` (a re-entrant caller, #70), so each call sits where a throw
leaves nothing behind:

- SearchSession: right after the escrow clone, whose local owner drops it on a throw, and
  before the Shared/Own split, so both modes read it there. Nothing else is acquired yet.
- RenderEngine: at the top of the constructor, before the worker contexts are cloned, so both
  modes read it there with nothing yet to roll back. (The Shared branch,
  `RenderEngine.cpp:537-538`, has no cleanup of its own for the contexts cloned above it, so a
  read there would leak them.) Own hands the table to `open_worker_documents`.

**Declarations.** `ChapterPagesAccess` and `table_for_copy` are declared in a new
`detail/ChapterPagesAccess.hpp` (forward-declared fz types, like `TextPageAccess.hpp`), the loader
in `detail/PageLoad.hpp`; all three are defined in `Document.cpp`, where the table and MuPDF
already meet, and included by `RenderEngine.cpp` and `SearchSession.cpp`.

**The check.** `detail::table_for_copy(fz_context*, fz_document* copy,
std::shared_ptr<const ChapterPages> table) noexcept` → the table, or null when the table is null,
`fz_count_chapters(copy)` throws (reported with `fz_report_error`, #146) or differs from
`table->chapter_count()`. The count is read into an `int` by a helper whose `fz_try` holds no
object with a destructor (#179); the comparison and the `shared_ptr` live outside it. A null
result is not an open failure: that copy loads by flat number, as today.

**One loader.** `detail::PageAt page_at(const ChapterPages*, std::size_t index) noexcept` returns
plain ints `{by_location, chapter, page}`, computed before the caller's `fz_try`.
`fz_page* load_page_at(fz_context*, fz_document*, PageAt, int index)` is called inside it and
calls `fz_load_chapter_page` when `by_location`, else `fz_load_page(index)` (an index past the
table therefore throws exactly as today). It replaces #230's `chapter_location` in
`page_size` and `with_rendered_page` too, so after this change `fz_load_page(` appears in `src`
only inside `load_page_at`. Declared in a new `detail/PageLoad.hpp` (forward-declared fz types),
defined in `Document.cpp`.

### 4.4 The paths

Each loader gains a `const ChapterPages*` (null = flat number):

- `load_display_list(ctx, doc, chapters, page)`.
- `TextPageAccess::build(ctx, doc, chapters, index)`. With a table, the range check is
  `index < chapters->page_count()` instead of `fz_count_pages` (which on a fresh ePub copy lays
  out the whole book); an index outside it falls to the flat branch with today's
  `fz_count_pages` check, so out-of-range behaves as today. Without a table: unchanged.
- `page_hits_on(ctx, doc, chapters, page, needle, flags, cookie)`.
- `Document::page_text`: `page_at(impl_->chapters.get(), index)`.

Callers:

| caller | passes |
|---|---|
| `Document::text_page`, `page_hits`, `page_text`, `page_size`, `with_rendered_page` | `impl_->chapters.get()` |
| RenderEngine, Own | that worker's checked table |
| SearchSession, Own | the session's checked table |
| RenderEngine and SearchSession, Shared | the Document's table, unchecked (§4.5) |

**RenderEngine (Own).** `open_worker_documents` receives the table the constructor read (§4.3).
Per worker, after the open and authenticate:
`table_for_copy(ctx, wdoc, table)`, stored in a `std::vector<std::shared_ptr<const
ChapterPages>> worker_chapters` reserved together with `worker_docs` (so the store after a
successful check cannot throw) and cleared by the same rollback.

**SearchSession (Own).** The table is read before `open_scan_document` (§4.3). After the open
succeeds: `table_for_copy(escrow.get(), sdoc, table)` (noexcept), moved into `Impl::chapters` in
the commit block, which stays non-throwing.

### 4.5 Shared mode uses the Document's table as it is

A Shared borrower runs on the Document's own document, the one the table was built from, so it
needs no check. The engine and the session read it once at construction, through the same
`ChapterPagesAccess::table` as Own, and pass it on every borrow. It stays right for as long as they
live: a borrower's Document is const to it and is not reopened or closed under it (the #229 rule in
`SharedDocumentAccess.hpp`), and the `shared_ptr` keeps the table alive regardless. The borrow
seam's signature stays as #229 left it.

In the product only FB2 is shared (`MakeDocumentView.hpp`: `doc.is_fb2() && !password`), and FB2
has one chapter (only `epub-doc.c` sets `count_chapters`), so there the table is null. But both
constructors accept `DocumentShare::Shared` for any document, so an ePub constructed Shared loads
by location too; §6.2 tests it.

### 4.6 What changes for a user

- A whole-document search of a many-chapter ePub costs what loading by location costs (§5).
- A worker's first render of a page in a late chapter, and its first text prefetch, no longer
  lay out the chapters before it. Working set should drop with them; measured in §5.
- Nothing else. A chapter that fails to lay out already fails the Document's open
  (`read_page_count` lays out every chapter; `open` returns `Corrupted`), so no copy with a table
  meets one, except through a transient failure such as running out of memory.

## 5. Measurement (the issue's to-do)

In the app, on the issue's books (generated as in #230 §1: N spine items × 10 paragraphs of 60
random words; 800 and 1,500 chapters), before (116070a) and after, 5 interleaved runs pinned to
the P-cores (0x5555), on a quiet machine (`reference_litepdf_bench_machine_noise`):

- whole-document search: wall time from the query to the scan's completion, for a word on every
  page and for a word on no page;
- working set after the scan, and after End on a freshly opened tab (worker layout).

**Instrument.** Nothing in the app reports a scan's completion today (`scan_complete()` only feeds
the find-bar counter), so the measurement uses a probe patch that is applied to both builds and
never merged: when an environment variable (`LITEPDF_PROBE_SEARCH=1`) is set, `SearchSession`
stamps `set_query` and prints one stderr line when the generation's last page task finishes
(`pages_remaining` reaches 0), with the milliseconds between them and the page count. Stamping
inside `set_query` leaves the find bar's typing debounce out of the figure. The baseline is
116070a with that same patch; the after build is the branch with it. The working set is read
from outside (`Get-Process`). The driver (open the book, Ctrl+F, type the word, wait for the
line, read the working set, close) and the patch are written by the plan. Neither is merged into
`src`; both go in the PR description in full (the patch as a diff, the driver as a script), with
the book generator's parameters, so the numbers can be reproduced from the PR alone.

Numbers go in §7 and the PR. Release build only.

## 6. Tests

### 6.1 Fixture

`tests/fixtures/chapter-tokens.epub`, committed, generated by a builder next to the #230
fixture's in `scripts/generate-strip-fixtures.py` (so its `--check`, which CI runs, gates the new
fixture's bytes too): 12 chapters of 1 to 5 pages each, every page carrying one unique token
(`tokC_P`, chapter and page) and nothing else that matches `tok`. Page breaks are forced with
`page-break-after:always` on every page's paragraph but the chapter's last: `page-break-before`
did nothing in MuPDF 1.27.2's ePub layout (probed 2026-11-23, four ways), and a break after the
last paragraph adds an empty page.
The fixture's pagination is pinned by a test that walks it with flat `fz_load_page` + stext on
the test's own document (the independent oracle), so the oracle does not go through any table.

### 6.2 Correctness (`[document][chapters]`, `[render]`, `[search]`)

The oracle is always the test's own document, loaded by flat `fz_load_page` (the `OwnDocument`
helper of `test_document_page_size.cpp`), never a path that reads a table.

- `Document::page_text(i)` and `Document::text_page(i)`'s full-range copy contain exactly the
  token the oracle finds on page i, for every page.
- `Document::page_hits(i, "tokC_P")` hits only page i, for every token.
- SearchSession on `chapter-tokens.epub`, Own and Shared: each token's hits are on the oracle's
  page.
- RenderEngine (2 workers Own, 1 worker Shared; no cache): each page's pixmap bytes equal the
  oracle's render through the same steps; each page's prefetched text holds the oracle's token.
  Shared on an ePub is constructed directly, as the product never does (§4.5).
- `table_for_copy`: the Document's table against a copy of the same file → the table; against a
  copy of a file with another chapter count (`strip-chapters.epub`) → null; null in → null.
- Determinism: for each fixture with chapters, a copy opened on `Document::clone_context()` gives
  per-chapter counts (`fz_count_chapter_pages`) from which `ChapterPages::from_counts` builds a
  table, and that table has the Document's chapter count, the Document's page count
  (`page_count()`), and the same `locate` answer for every index below it (the Document's table
  read through `ChapterPagesAccess`). Equal totals rule out a longer last chapter that the
  per-index comparison alone would miss; every ePub chapter has at least one page
  (`count_laid_out_pages`, `epub-doc.c:178-183`), so equal answers for every index then mean
  equal counts. No per-chapter accessor is added.
- A RenderEngine or SearchSession whose check fails still renders and finds every page by flat
  number. The mismatch is the §4.2 residual made real: the test copies `chapter-tokens.epub` to a
  temporary path, opens the Document on it, then overwrites that path with `strip-chapters.epub`
  (another chapter count) before constructing the engine or session; each page below the
  Document's page count (which stays the old file's, at most 60 pages, while `strip-chapters.epub`
  has 400) must equal the oracle's flat load of the new file. Windows allows the overwrite while
  MuPDF holds the file (probed 2026-11-23), so no test hook is added. Mutation check: using the
  Document's table without the check fails it.
- `page_at` / `load_page_at`: an index past the table throws as `fz_load_page` does.

Mutation checks: a wrong chapter, and `page + 1` within the chapter, in `page_at` fail the token
tests.

### 6.3 Cost oracles (one per path)

The token tests cannot tell flat from by-location loads: both return the same page. Each path
therefore gets a cost oracle that fails if that path alone is left on `fz_load_page`, on a
1,500-chapter book written at test time (#230's `write_many_chapter_epub`, moved to a shared
test helper):

- **Fresh copy, last page**: by location lays out one chapter; by flat number all 1,500.
  - RenderEngine render: a new engine, no cache, one request for the last page; time from submit
    to `on_complete`.
  - RenderEngine text prefetch: the same request with `on_text` and `text_wanted` set to the last
    page; time from `on_complete` to `on_text` (the render has laid out only the last chapter, so a
    flat `fz_count_pages` lays out the other 1,499).
  - SearchSession: a new session on a `DeferredDispatcher` (`search_test_kit.hpp`),
    `set_query(..., start_page = last)`, and only the first queued task run, which scans the last
    page; time of that task.
- **Warm loop** (Document's `page_text`, `page_hits`, `text_page`; SearchSession whole-document
  search, second run on the same session so the chapters are in the store): every page, best of
  3; by flat number each call adds the O(c²) walk.
- `page_size` and `with_rendered_page` keep #230's speed test (`test_document_page_size.cpp`,
  "Document on a 1500-chapter ePub: ..."), which already fails when either is reverted; moving
  them onto `load_page_at` must keep it passing.

Each new oracle asserts `measured <= k * reference + c`, where the reference is the same work the
test times itself on the same book by `fz_load_chapter_page` on its own document, and `k` and `c`
are constants in the test source. They are chosen during planning from a probe of both routes on
this machine: the location route must pass with at least 2× headroom and the flat route must
fail by at least 2×. The probe's readings are recorded in the plan and in a comment next to the
constants, as #230's test records its own. (Chosen 2026-11-23: `k = 4`, `c = 25 ms`, #230's
constants; at 1,500 chapters the flat route reads 156-158 ms on a fresh copy and 797-808 ms on a
warm loop, against bounds of about 30 and 140 ms.) **Acceptance step:** reverting each
path's load alone to flat numbers fails its oracle; the readings of both routes go in the PR.
`TextPageAccess::build` and `page_hits_on` each serve two paths, so a path is reverted at its
call site, by passing `nullptr` for the table there, not inside the shared helper (which would
revert both at once). `load_display_list`, `page_text`, `page_size` and `with_rendered_page`
have one caller each and are reverted the same way.

### 6.4 Existing tests and helpers

`test_page_hits_cookie.cpp` (3 calls) gains the `nullptr` argument. `OwnDocument`,
`write_many_chapter_epub` and the `utf8_of` it calls move from `test_document_page_size.cpp`'s
anonymous namespace to a shared test header (`tests/unit/chapter_test_kit.hpp`), with `inline`
functions so two test files can include it, as `search_test_kit.hpp` does. The render and search
tests can then use them; `test_document_page_size.cpp` changes only by that move, and its tests
keep their behaviour.

New test sources: `tests/unit/test_chapter_copies.cpp` (§6.2) and
`tests/unit/test_chapter_copies_cost.cpp` (§6.3), each added to the explicit source list in
`tests/CMakeLists.txt`; the fixture's builder is in `scripts/generate-strip-fixtures.py` (§6.1).

## 7. Results

Filled in after implementation: the §5 in-app numbers, and the §6.3 readings of both routes per
path.

## 8. CHANGELOG

Under `[Unreleased]` → `### Changed`: searching a many-chapter ePub is faster, with the §5
before/after numbers (#232). If §5 shows a working-set drop, the same line says so.

## 9. Review

**Design review (Fable, read-only), 2026-11-22**, on the in-chat design. Premises verified against
the MuPDF source (only crash is a chapter past the spine; `fz_count_chapters` lays out nothing;
clones share style/font/store; the four paths are every flat load in `src`). Two findings on the
tests, both taken: no test failed when a non-search path stayed flat (→ §6.3 per-path oracles and
the mutation acceptance step); the 4× + 25 ms bound could pass a flat search (→ warm runs and a
probe-fixed bound). Also taken: Shared passes `nullptr` instead of changing the seam (§4.5; the
spec gate later replaced `nullptr` with the Document's table, still without changing the seam);
`shared_ptr<const ChapterPages>` so a copy takes no allocation; `page_count()`; the independent
flat oracle; the determinism assertion; the second cost part of §2.

**Spec gate (Full, routine), 2026-11-23.** Round 1, Opus on b73d096: 2 findings, both verified
against the source and fixed here. (1) §4.2 claimed the same file can give a shorter spine; the
"ignoring chapter" catch is unreachable in 1.27.2, so the check guards a replaced file only, and
the §4.6 bullet that rested on it is gone. (2) §5 had no instrument for a scan's completion; it now
names an env-gated probe patch applied to both builds. Its 5 questions, all taken: where the
table read sits relative to each rollback (§4.3), the helper move (§6.4), the determinism test
without a per-chapter accessor (§6.2), call-site mutations for the shared helpers (§6.3), and the
friend-pattern wording (§4.3).

Codex lens 3 on efa8bb0, all three runs on that one commit, valid reads (bundle + ≤ 3 files):

- terra@medium: the flat fresh-copy load lays out chapters 0..c, not 0..c-1 (taken, §2, §6.3);
  the cost bounds had no stated predicate (taken, with luna's same finding: §6.3 now states the
  predicate, the 2× margins and where the readings are recorded; the constants themselves come
  from the planning probe, since they are a measurement).
- gpt-6.1-terra: both constructors accept Shared for any document, so a Shared ePub stayed on
  flat numbers (taken: Shared uses the Document's table, §4.5, tested in §6.2); `page_size` and
  `with_rendered_page` had no named cost oracle (taken: #230's test, §6.3); the determinism test
  missed a longer last chapter (taken: compare totals, §6.2). Its questions on the mismatch
  test's page range and on isolating one page of a search were answered in §6.2 and §6.3.
- luna@max: the helper move left `utf8_of` behind and needed `inline` (taken, §6.4); the
  measurement's patch and driver lived only outside the repo (taken: they go in the PR in full,
  §5); test sources and declaration headers were unnamed (taken, §4.3, §6.4). **Declined:** that
  a same-chapter-count replacement can give a wrong page. It is the residual §4.2 already
  accepts; preventing it would need every copy's per-chapter counts, which is the whole-book
  layout approach 2 rejects.
