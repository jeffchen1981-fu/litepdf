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
1,500-chapter ePub passes the §4.4 gate.

Non-goals:

- No MuPDF patch. The spine walk in `epub_load_page` / `epub_count_pages` stays (§7).
- No change to page counting, layout, or the order or content of `page_sizes()`.
- The other per-page paths keep loading by flat number (§6).

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

`Document::Impl` gains `std::optional<detail::ChapterPages> chapters`. It is filled right after
the page count is read, in both places that read it (`open`, and `authenticate` for a document
that needed a password), and cleared wherever the page count is cleared (`close`, and at the
start of `open`):

- `fz_count_chapters`; when it returns 1 or less, no table (PDF, CBZ, FB2, XPS, SVG and
  single-chapter ePubs: `fz_load_page` already finds the page on the first chapter, so nothing
  changes for them).
- Otherwise `fz_count_chapter_pages(c)` for every chapter, inside one `fz_try`, then
  `from_counts(counts, page_count)`. The counts come from the per-chapter cache that the page
  count has just filled, so this costs only the spine walks: 2.2 ms at 1,500 chapters, 0.02 ms
  at 200 (§2), off the UI thread.
- If MuPDF throws, or `from_counts` refuses the counts, there is no table and the error is
  reported and marked handled (`fz_report_error`, #146). The open still succeeds: the page count
  was read, and `page_size` then loads by flat number as it does today.

No allocation or C++ object lives inside the `fz_try` (#179): the `std::vector<int>` is sized
with `fz_count_chapters`'s result before the `fz_try` and only written to inside it.

### 4.3 `Document::page_size`

Unchanged for PDF (#165) and for CBZ header sizing (#176). For the other formats, inside the
existing `fz_try`:

- table present and `locate(index)` answers → `fz_load_chapter_page(ctx, doc, chapter, page)`;
- otherwise → `fz_load_page(ctx, doc, index)`, as today (an out-of-range index therefore throws
  exactly as before, and `page_size` turns it into its `std::runtime_error`).

`fz_load_page` itself ends in `fz_load_chapter_page(chapter, page)` for the same location, so the
loaded page, its bounds and its errors are the same; only the chapter search before it changes.
The location is computed before the `fz_try` (it is plain C++).

### 4.4 Threads

The table is written only by `open`, `authenticate` and `close`, which run before the document
is shared or after it is dropped, as `page_count` already is. `page_size` reads it under
`doc_mutex`, as it reads everything else.

## 5. Results (to be filled at implementation)

Before/after `litepdf-cli <file> --bench-page-sizes --iterations 3` at 200 / 400 / 800 / 1,500
chapters, plus the single-chapter control and `strip-chapters.epub`, on a Release build of this
branch and of main, P-cores, interleaved. Also `--benchmark` `open_ms` at 1,500 chapters before
and after, to show the table's cost at open is within noise.

## 6. The other per-page paths: out of scope

Every `fz_load_page` in litepdf (`git grep fz_load_page src`), with the cost of loading by flat
number at 1,500 chapters / 4,500 pages:

| path | thread | pages per call | extra cost by flat number |
|---|---|---|---|
| `Document::page_size` → `page_sizes()`, `known_page_size`, print sizing | UI | every page | **2.5 s total: fixed here** |
| `RenderEngine` worker, display-list miss | worker | one | ≤ 2.2 ms per render |
| `TextPageAccess::build` (text prefetch on a worker; `text_for` on the UI thread at a press) | worker / UI | one | ≤ 2.2 ms per page |
| `page_hits_on` from `SearchSession` (whole-document search) | search | one per task, every page in a search | 0.86 s → 3.6 s for a whole-book text scan (probe: load + `fz_new_stext_page_from_page`); 459 → 709 ms at 800 chapters; none at 200 |
| `Document::with_rendered_page` (print), `page_text` (CLI), `page_hits` | UI / CLI | one | ≤ 2.2 ms per page |

Only `page_sizes()` loops over every page on the UI thread, so only it turns the per-page cost
into a frozen window. The others pay at most 2.2 ms per page on a book far beyond a typical one.
The whole-document search is the one other loop: about 4× slower at 1,500 chapters, on its own
thread, with hits appearing as it goes.

They are left as they are because each of them runs on a `fz_document` of its own (one per
render worker and one for search), so each would need its own table, and #229 is reconsidering
exactly those copies (one worker for reflowable documents, sharing the Document's laid-out copy,
opening the search copy at the first search). `ChapterPages` is the unit any of them can reuse.
A follow-up issue for the search scan and the workers is proposed in the PR, not filed without
the user's approval.

## 7. MuPDF

The remaining O(N²) (`epub_load_page` and `epub_count_pages` walk the spine from its head) is
11 ms at 1,500 chapters and is left to upstream; whether to report it is the user's call, as for
#229's outline walk.

## 8. Tests

### 8.1 Fixture `tests/fixtures/chapter-sizes.epub`

Written by `scripts/generate-strip-fixtures.py` (one more builder in `BUILDERS`, so `--check`
covers it byte for byte; stored entries, fixed date, as the other zips). 12 chapters. Chapter k
(1-based) carries `<style>@page{margin:0 Mpt}</style>` with M = 200 + 10k, which pushes MuPDF's
text width under its 72 pt floor, so the page is 72 + 2M pt wide: every chapter has its own width
(492 … 712 pt). Chapter k has 1 + (7k mod 25) short paragraphs, so chapters hold different page
counts (2 to 10, 68 pages in all with the probe). A page loaded from the wrong chapter therefore
has the wrong width.

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
- **Speed (the gate as a test):** a 1,500-chapter ePub of one short paragraph per chapter is
  written to a temporary directory at test time with MuPDF's zip writer (stored entries; not
  committed: 1,500 chapters is about 700 KB). Sizing every page with `Document::page_size`, best
  of 3 on one open document, must take under 250 ms. Probe on that shape: 860 ms by flat number,
  5.3 ms by location, so the bound fails the old code by 3.4× on this machine and leaves the new
  code a 47× margin for slower runners. Mutation check: reverting `page_size` to `fz_load_page`
  fails it.
- `page_size(page_count())` on `chapter-sizes.epub` still throws `std::runtime_error`.

`tests/unit/test_strip_fixtures.cpp` pins the new fixture's page count (68) and chapter count.

### 8.3 Gate

The §4.4 note in the continuous-scroll spec gains the 1,500-chapter reading before and after.

## 9. CHANGELOG

Under `[Unreleased]` → `### Fixed`: an ePub split into many hundreds of files no longer freezes
the window when it is first shown in continuous scroll, with the 1,500-chapter before/after
numbers (#230).
