# ePub page sizes by chapter location (#230) — design

Issue #230. Found while measuring #229. Spec of the strip's sizing gate:
`2026-10-22-continuous-scroll-design.md` §4.4.

## 1. Problem

`DocumentView::page_sizes()` sizes every page on the UI thread the first time a tab is shown in
continuous scroll. For a format other than PDF and CBZ, `Document::page_size(i)` loads page `i`
by its flat number with `fz_load_page`. For an ePub with many chapters (spine items) that grows
about cubically with the chapter count.

Readings of the issue (`litepdf-cli <file> --bench-page-sizes --iterations 3`, median,
generated books of N sections × 10 paragraphs of 60 random words, one XHTML spine item per
section):

| chapters | pages | `page_sizes()` | same text in 1 chapter |
|---|---|---|---|
| 200 | 600 | 4 ms | 0.2 ms |
| 400 | 1,200 | 29 ms | 0.5 ms |
| 800 | 2,400 | 282 ms | 1.0 ms |
| 1,500 | 4,500 | 2,664 ms | 2.0 ms |

From 800 chapters on, an ePub misses the §4.4 gate (100 ms) by far, on the UI thread, with the
window not answering.

## 2. Cause (confirmed)

MuPDF 1.27.2 source:

- `fz_load_page(ctx, doc, n)` (`fitz/document.c`) finds the chapter of flat page `n` by walking
  the chapters from the first and calling `fz_count_chapter_pages(c)` on each until the running
  sum passes `n`.
- For an ePub, `fz_count_chapter_pages(c)` is `epub_count_pages`, which walks the spine linked
  list from its head to chapter `c`. The page count is cached per chapter
  (`acc->pages_in_chapter`); finding the chapter is not.
- So loading one page in chapter `c` walks O(c²) spine nodes, and sizing every page of an
  N-chapter book walks O(N³).

**Probe** (standalone C against the same MuPDF libraries, P-cores, the issue's books; kept out of
the repo): size every page both ways in one process, compare every `fz_rect` bit for bit.

| chapters | pages | by flat number (`fz_load_page`) | by location (`fz_load_chapter_page`) | of which the chapter table | rect mismatches |
|---|---|---|---|---|---|
| 200 | 600 | 3.4 ms | 0.3 ms | 0.02 ms | 0 |
| 400 | 1,200 | 28.3 ms | 0.7 ms | 0.07 ms | 0 |
| 800 | 2,400 | 257.7 ms | 2.2 ms | 0.35 ms | 0 |
| 1,500 | 4,500 | 2,505 ms | 10.9 ms | 2.17 ms | 0 |
| 1,500 in 1 chapter | 3,396 | 0.8 ms | 0.8 ms | 0.00 ms | 0 |

One `fz_load_page` of the last page of the 1,500-chapter book takes 2.2 ms;
`fz_count_chapters` takes 4 µs. The flat path reproduces the issue's numbers, and loading by
location removes almost all of it. What remains by location is still superlinear (×5 for the last
step) because `epub_load_page` and `epub_count_pages` also walk the spine from its head, once per
call: O(N²) in all, 11 ms at 1,500 chapters. That is MuPDF's and stays (§7).

## 3. Goal and non-goals

Goal: `Document::page_size(i)` costs O(spine position) instead of O(spine position²) for a
multi-chapter document, with results bit-identical to today's, so `page_sizes()` on a
1,500-chapter ePub passes the §4.4 gate. `Document::with_rendered_page`, which printing calls
once per page in a loop on the UI thread, uses the same table.

Non-goals:

- No MuPDF patch. The spine walk in `epub_load_page` / `epub_count_pages` stays (§7).
- No change to page counting, layout, or the order or content of `page_sizes()`.
- The per-page paths on the render workers' and the search's own documents keep loading by flat
  number (§6).

## 4. Design

### 4.1 Unit: `detail::ChapterPages` (`src/core/detail/ChapterPages.hpp`, header only)

A pure table from flat page number to `(chapter, page)`, with no MuPDF dependency:

```cpp
struct ChapterLocation { int chapter; int page; };

class ChapterPages {
public:
    // counts[c] = pages in chapter c. Empty optional when the counts cannot
    // describe `page_count` pages: a negative count, or a sum other than
    // page_count.
    static std::optional<ChapterPages> from_counts(std::vector<int> counts,
                                                   std::size_t page_count);
    // The location of flat page `index`, or nullopt when index >= page_count.
    std::optional<ChapterLocation> locate(std::size_t index) const noexcept;
    std::size_t chapter_count() const noexcept;
};
```

It stores the first flat page of each chapter (`starts`, size chapters + 1, last = page count)
and finds a page's chapter with `std::upper_bound` on `starts`, O(log N). A chapter with 0 pages
is skipped: `upper_bound` returns the last chapter whose start is ≤ `index`, which is the one
that holds it. (MuPDF gives every ePub chapter at least one page, `count_laid_out_pages`, but the
table does not rely on that.)

### 4.2 Built at open, on the open thread

`Document::Impl` gains `std::optional<detail::ChapterPages> chapters`. One helper,
`read_chapter_pages(ctx, doc, page_count)`, builds it, and both places that read the page count
assign its result right after a successful read: `open` (on the open thread) and `authenticate`
(on the UI thread, for a document that needed a password). It is assigned, never merged, so a
document without a table replaces any earlier one. It is also reset in `close()` and at the start
of `open` (next to `reset_cbz()`; `open` does not clear `page_count` there today, the table is
the new addition).

Only ePub has chapters in MuPDF 1.27.2 (`epub-doc.c` is the only handler that sets
`count_chapters`), and an ePub never asks for a password, so `authenticate` never builds a table
today. It calls the helper anyway, so the page count and the table cannot come from different
reads if that ever changes; that branch is untested by design.

The helper:

- First `fz_try`: `nc = fz_count_chapters(ctx, doc)`. When it is 1 or less, no table (PDF, CBZ,
  FB2, XPS, SVG and single-chapter ePubs: `fz_load_page` already finds the page on the first
  chapter, so nothing changes for them).
- Between the two `fz_try` blocks, in plain C++: a `std::vector<int>` of `nc` zeros.
- Second `fz_try`: `fz_count_chapter_pages(c)` for every chapter, written through the vector's
  `data()` pointer. The counts come from the per-chapter cache the page count has just filled,
  so this costs only the spine walks: 2.2 ms at 1,500 chapters, 0.02 ms at 200 (§2).
- Then `from_counts(counts, page_count)`.
- A MuPDF throw: `fz_report_error` in that `fz_catch` (#146), no table. `from_counts` refusing
  the counts: no table, nothing reported (there is no MuPDF error then, and `fz_report_error`
  would print a stale one). A `std::bad_alloc` from the vector or the table: caught around the
  plain C++ part, no table. In every case the open still succeeds, because the page count was
  read, and pages load by flat number as they do today.

No allocation, C++ object with a destructor, or `throw` lives inside either `fz_try` (#179).

The table must only ever hold locations of the document it was built from: for a chapter at or
past the chapter count, `epub_load_page` returns NULL and `fz_load_chapter_page` then writes
through it (`document.c:950-953`). Building it from the same `fz_document` right after its page
count, assigning it at every successful open, and resetting it on `close` is what guarantees
that.

### 4.3 `Document::page_size` and `Document::with_rendered_page`

`page_size` is unchanged for PDF (#165) and for CBZ header sizing (#176). For the other formats,
and in `with_rendered_page` for every format, the `fz_load_page` call inside the existing
`fz_try` becomes:

- table present and `locate(index)` answers → `fz_load_chapter_page(ctx, doc, chapter, page)`;
- otherwise → `fz_load_page(ctx, doc, index)`, as today (an out-of-range index therefore throws
  exactly as before, and `page_size` turns it into its `std::runtime_error`, `with_rendered_page`
  into its `false`).

`fz_load_page` itself ends in `fz_load_chapter_page(chapter, page)` for the same location, so the
loaded page, its bounds, its content and its errors are the same; only the chapter search before
it changes. The location is computed before the `fz_try`, into two plain ints and a bool (no
object with a destructor crosses into the `fz_try`).

### 4.4 Threads

The table is written only by `open`, `authenticate` and `close`, which run before the document
is shared or after it is dropped, as `page_count` already is. `page_size` reads it under
`doc_mutex`, as it reads everything else.

## 5. Results (to be filled at implementation)

Before/after `litepdf-cli <file> --bench-page-sizes --iterations 3` at 200 / 400 / 800 / 1,500
chapters, plus the single-chapter control and `strip-chapters.epub`, on a Release build of this
branch and of main, P-cores, interleaved. Also `--benchmark` `open_ms` at 1,500 chapters before
and after, to show the table's cost at open is within noise.

## 6. The other per-page paths

Every `fz_load_page` in litepdf (`git grep fz_load_page src`: `Document.cpp` ×5,
`RenderEngine.cpp` ×1), with the cost of loading by flat number at 1,500 chapters / 4,500 pages.
One `fz_load_page` of the last page costs 2.2 ms there; `fz_count_pages` costs about the same
(it runs the same per-chapter walk as the table build).

| path | document | thread | loop over every page? | extra cost by flat number | here |
|---|---|---|---|---|---|
| `Document::page_size` → `page_sizes()`, `known_page_size`, print sizing | Document's | UI | yes (`page_sizes`) | 2.5 s per `page_sizes()` | **fixed** |
| `Document::with_rendered_page` (print) | Document's | UI, modal, messages pumped by `abort_proc` | yes, a whole-book print | ≤ 2.2 ms per page, 2.5 s over a 4,500-page print | **fixed** (same table, one line) |
| `Document::page_text` | Document's | CLI (`main.cpp`, page 0) | no | ≤ 2.2 ms | flat |
| `Document::page_hits` → `page_hits_on` | Document's | CLI (`bench_selection`) | no | ≤ 2.2 ms | flat |
| `Document::text_page` → `TextPageAccess::build` (`text_for` at a press) | Document's | UI | no | ≤ 4.5 ms (it also calls `fz_count_pages`) | flat |
| `TextPageAccess::build` (text prefetch, #98) | worker's own | worker | no | ≤ 4.5 ms per page | flat |
| `RenderEngine` worker, display-list miss | worker's own | worker | no | ≤ 2.2 ms per render | flat |
| `page_hits_on` from `SearchSession` | search's own | search | yes, a whole-document search | text scan 0.86 → 3.6 s at 1,500 chapters; 459 → 709 ms at 800; none at 200 (probe: load + `fz_new_stext_page_from_page`) | flat |

Kept on flat numbers:

- `page_text`, `page_hits` and `text_page` load from the Document's own document and could use
  the table, but each is one page per call, off any loop. `page_hits` and `text_page` go through
  `page_hits_on` and `TextPageAccess::build`, which the search and the workers share on their own
  documents, so routing them would change those shared helpers for a few ms per press.
- The workers and the search each run on a `fz_document` of their own, so each would need a
  table of its own, and #229 is reconsidering exactly those copies (one worker for reflowable
  documents, sharing the Document's laid-out copy, opening the search copy at the first search).
  The whole-document search is the one remaining loop: about 4× slower at 1,500 chapters, on its
  own thread, with hits appearing as it goes. `ChapterPages` is the unit any of them can reuse.

A follow-up issue for the search scan and the workers is proposed in the PR, not filed without
the user's approval.

## 7. MuPDF

The remaining O(N²) (`epub_load_page` and `epub_count_pages` walk the spine from its head) is
11 ms at 1,500 chapters and is left to upstream; whether to report it is the user's call, as for
#229's outline walk.

## 8. Tests

### 8.1 Fixture `tests/fixtures/chapter-sizes.epub`

Written by `scripts/generate-strip-fixtures.py` (one more builder in `BUILDERS`, so `--check`
covers it byte for byte; stored entries, fixed date, as the other zips; the module docstring's
fixture list and "all four" are updated). 12 chapters, `OEBPS/c01.xhtml` … `c12.xhtml`, with
the existing `CONTAINER_XML` and an OPF of the existing shape. Chapter k (1-based) is exactly:

```
<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml">
<head><title>Chapter k</title><style>@page{margin:0 Mpt}</style></head>
<body>
<h1>Chapter k</h1>
<p>S</p> repeated 1 + (7k mod 25) times, one per line
</body>
</html>
```

with M = 200 + 10k and S the one sentence `Lorem ipsum dolor sit amet, consectetur adipiscing
elit, sed do eiusmod tempor incididunt ut labore et dolore magna aliqua.` (not the module's
longer `LOREM`, which gives 125 pages). The side margins push MuPDF's text width under its 72 pt
floor (`fz_layout_html`), so the page is 72 + 2M pt wide: every chapter has its own width
(492 … 712 pt, step 20), every page is 600 pt high. The paragraph counts give the chapters
different page counts (4, 6, 9, 2, 5, 8, 10, 3, 6, 9, 2, 4: 68 pages with the prototype; the
committed file's count is read from it and pinned). A page loaded from the wrong chapter
therefore has the wrong width. A page from the right chapter but the wrong page within it has
the right size, so only the unit test of `locate` (§8.2) sees that mistake.

### 8.2 Unit tests

`tests/unit/test_chapter_pages.cpp`, `[core][chapters]`:

- `locate` on a hand-made table, every index, against a brute-force walk of the counts,
  including chapters of 0 pages at the start, middle and end, and one past the end (nullopt).
- `from_counts` refuses a negative count and a sum other than the page count, and accepts an
  empty table for 0 pages.

`tests/unit/test_document_page_size.cpp`, `[document][pages]`:

- **Oracle:** on `chapter-sizes.epub`, `Document::page_size(i)` equals the test's own
  `fz_load_page` + `fz_bound_page` for every page, bit for bit (the existing `OwnDocument`
  helper), and the fixture's widths are not all equal (so the oracle is not vacuous).
  Mutation check: a `locate` that maps to the next chapter, or to chapter 0, fails it.
- **Print path:** on `chapter-sizes.epub`, `with_rendered_page(i, 1, 1, false, …)` hands the
  callback the width and height of `fz_round_rect` of the test's own `fz_load_page` +
  `fz_bound_page`, for every page. Mutation check: the same wrong-chapter mutants, applied to
  `with_rendered_page`'s lookup, fail it.
- **Speed (the gate as a test):** a 1,500-chapter ePub is written to a fresh temporary directory
  at test time with MuPDF's zip writer (`fz_write_zip_entry` always stores). Each chapter is
  `<h1>Chapter k</h1><p>S</p>` in the template above without the `<style>`, so one page each.
  It is generated rather than committed, as a binary of over a megabyte that only a timing
  test reads. Sizing every page with `Document::page_size`, best of 3 on one open document, must
  take under 250 ms. Probe on that shape (one 60-word paragraph per chapter): 860-1,129 ms by
  flat number, 5.3-8.5 ms by location across three runs, so the bound fails the old code by at
  least 3.4× on this machine and leaves the new code a 29× margin for slower runners. The
  implementer re-measures on the committed shape and records both readings. Mutation check:
  reverting `page_size` to `fz_load_page` fails it.
- `page_size(page_count())` on `chapter-sizes.epub` still throws `std::runtime_error`.

`tests/unit/test_strip_fixtures.cpp` pins the new fixture's page count (read from the committed
file) and, through `Document` only, that its pages have 12 distinct widths (`Document` has no
chapter count, and none is added for a test).

### 8.3 Gate

The §4.4 note in the continuous-scroll spec gains the 1,500-chapter reading before and after.

## 9. CHANGELOG

Under `[Unreleased]` → `### Fixed`: an ePub split into many hundreds of files no longer freezes
the window when it is first shown in continuous scroll, with the 1,500-chapter before/after
numbers (#230).
