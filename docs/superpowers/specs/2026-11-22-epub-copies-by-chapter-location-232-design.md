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
  accelerator. The first flat load of a page in chapter c lays out chapters 0..c-1 first, and
  `TextPageAccess::build`'s `fz_count_pages` lays out the whole book, on each worker copy. The
  issue's table does not count this part; it is found by reading the source, not yet measured.

## 3. Goal and non-goals

Goal: every per-page load of §1 loads a multi-chapter page by its chapter location, on every
document it runs on, so a whole-document search and a worker's first page of a late chapter
cost what loading by location costs.

Non-goals:

- No MuPDF patch. The O(c) spine walk inside `epub_load_page` stays (#230 §7).
- No change to which documents are shared (#229) or to the worker count (#235).
- No new chapter table for a Shared borrower (§4.5).

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
  against the chapter's page count. `fz_load_chapter_page` (`document.c:949-952`) writes
  `page->chapter` through that NULL. A page number past a chapter's count draws background only
  (`fz_draw_html`). So the only crash is a chapter at or past the copy's chapter count.
- **The check is cheap.** `fz_count_chapters` runs `fz_ensure_layout` → `epub_layout`
  (`epub-doc.c:253-286`), which stores w/h/em and invalidates an empty accelerator, then counts
  the spine list (`epub_count_chapters`). Nothing is parsed or laid out.
- **The check is needed even for the same file.** `epub_parse_header` skips a chapter whose load
  throws ("ignoring chapter", `epub-doc.c:1057-1069`), so a copy can have a shorter spine than
  the Document's.
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
pointer, read under a Foreground hold (the same friend pattern as `TextPageAccess`). Called by
the RenderEngine and SearchSession constructors, before any of their threads start.

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
| RenderEngine and SearchSession, Shared | `nullptr` (§4.5) |

**RenderEngine (Own).** `open_worker_documents` takes the table from
`ChapterPagesAccess::table(doc)` before the loop. Per worker, after the open and authenticate:
`table_for_copy(ctx, wdoc, table)`, stored in a `std::vector<std::shared_ptr<const
ChapterPages>> worker_chapters` reserved together with `worker_docs` (so the store after a
successful check cannot throw) and cleared by the same rollback.

**SearchSession (Own).** After `open_scan_document` succeeds: `table_for_copy(escrow.get(), sdoc,
table)`, moved into `Impl::chapters` in the commit block, which stays non-throwing.

### 4.5 Shared mode passes no table

A Shared borrower runs on the Document's own document, so the Document's table would be right
for it. But only FB2 is shared, and FB2 has one chapter (only `epub-doc.c` sets
`count_chapters`), so that table is always null. The Shared branches pass `nullptr` with a
comment saying why and what to do if another format becomes shareable (hand the Document's table
through the borrow, read under the same hold). The borrow seam's signature stays as #229 left it.

### 4.6 What changes for a user

- A whole-document search of a many-chapter ePub costs what loading by location costs (§5).
- A worker's first render of a page in a late chapter, and its first text prefetch, no longer
  lay out the chapters before it. Working set should drop with them; measured in §5.
- A copy whose earlier chapter fails to lay out can still load a later page (today the flat walk
  throws at the earlier chapter's count). Better, and outside the tests.

## 5. Measurement (the issue's to-do)

In the app, on the issue's books (generated as in #230 §1: N spine items × 10 paragraphs of 60
random words; 800 and 1,500 chapters), before (116070a) and after, 5 interleaved runs pinned to
the P-cores (0x5555), on a quiet machine (`reference_litepdf_bench_machine_noise`):

- whole-document search: wall time from the query to the scan's completion, for a word on every
  page and for a word on no page;
- working set after the scan, and after End on a freshly opened tab (worker layout).

Numbers go in §7 and the PR. Release build only.

## 6. Tests

### 6.1 Fixture

`tests/fixtures/chapter-tokens.epub`, committed, generated by a script next to the #230 fixture's:
12 chapters of 1 to 5 pages each (page breaks forced with `page-break-before`), every page
carrying one unique token (`tokC_P`, chapter and page) and nothing else that matches `tok`.
The fixture's pagination is pinned by a test that walks it with flat `fz_load_page` + stext on
the test's own document (the independent oracle), so the oracle does not go through any table.

### 6.2 Correctness (`[document][chapters]`, `[render]`, `[search]`)

The oracle is always the test's own document, loaded by flat `fz_load_page` (the `OwnDocument`
helper of `test_document_page_size.cpp`), never a path that reads a table.

- `Document::page_text(i)` and `Document::text_page(i)`'s full-range copy contain exactly the
  token the oracle finds on page i, for every page.
- `Document::page_hits(i, "tokC_P")` hits only page i, for every token.
- SearchSession (Own) on `chapter-tokens.epub`: each token's hits are on the oracle's page.
- RenderEngine (Own, 2 workers, no cache): each page's pixmap bytes equal the oracle's render
  through the same steps; each page's prefetched text holds the oracle's token.
- `table_for_copy`: the Document's table against a copy of the same file → the table; against a
  copy of a file with another chapter count (`strip-chapters.epub`) → null; null in → null.
- Determinism: for each fixture with chapters, a copy opened on `Document::clone_context()` has,
  for every chapter, `fz_count_chapter_pages` equal to the Document's table (through
  `ChapterPagesAccess`).
- A RenderEngine or SearchSession whose check fails still renders and finds every page by flat
  number. The mismatch is the §4.2 residual made real: the test copies `chapter-tokens.epub` to a
  temporary path, opens the Document on it, then overwrites that path with `strip-chapters.epub`
  (another chapter count) before constructing the engine or session; each page must equal the
  oracle's flat load of the new file. If Windows refuses the overwrite while MuPDF holds the
  file, the plan substitutes a test-only hook on `ChapterPagesAccess` that hands out a table of
  another chapter count. Mutation check: using the Document's table without the check fails it.
- `page_at` / `load_page_at`: an index past the table throws as `fz_load_page` does.

Mutation checks: a wrong chapter, and `page + 1` within the chapter, in `page_at` fail the token
tests.

### 6.3 Cost oracles (one per path)

The token tests cannot tell flat from by-location loads: both return the same page. Each path
therefore gets a cost oracle that fails if that path alone is left on `fz_load_page`, on a
1,500-chapter book written at test time (#230's `write_many_chapter_epub`, moved to a shared
test helper):

- **Fresh copy, last page** (RenderEngine render, RenderEngine text prefetch, one SearchSession
  `page_hits_on` of the last page): by location lays out one chapter; by flat number it lays out
  1,499 (render, search) or the whole book (prefetch's `fz_count_pages`).
- **Warm loop** (Document's `page_text`, `page_hits`, `text_page`; SearchSession whole-document
  search, second run on the same session so the chapters are in the store): every page, best of
  3; by flat number each call adds the O(c²) walk.

Each is bounded against a reference the test times itself on the same book: the same work by
`fz_load_chapter_page` on the test's own document. The multiplier and constant are fixed by a
probe of both routes during planning, chosen so the location route passes and the flat route
fails each by at least 2×; the plan records the probe. **Acceptance step:** reverting each
path's load alone to `fz_load_page` fails its oracle; the readings of both routes go in the PR.

### 6.4 Existing tests

`test_page_hits_cookie.cpp` (3 calls) gains the `nullptr` argument; `test_document_page_size.cpp`
keeps passing unchanged.

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
probe-fixed bound). Also taken: Shared passes `nullptr` instead of changing the seam (§4.5);
`shared_ptr<const ChapterPages>` so a copy takes no allocation; `page_count()`; the independent
flat oracle; the determinism assertion; the second cost part of §2.
