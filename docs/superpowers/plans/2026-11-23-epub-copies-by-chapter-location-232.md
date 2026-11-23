# ePub pages by chapter location in every document copy (#232) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every per-page load in `src` (render workers, text prefetch, search scans, `page_text`,
`text_page`, `page_hits`, `page_size`, printing) loads a multi-chapter page by its chapter location,
on the Document's own `fz_document` and on every copy, so a whole-book search and a worker's first
page of a late chapter cost what loading by location costs.

**Architecture:** The Document's `ChapterPages` table becomes a `shared_ptr<const ChapterPages>`.
Each copy (a render worker's, a search session's) takes that pointer after one check,
`fz_count_chapters(copy) == table->chapter_count()`; a mismatch gives no table and that copy loads
by flat number, as today. One loader, `page_at` + `load_page_at`, replaces every `fz_load_page`
call and #230's `chapter_location`. Shared-mode borrowers use the Document's table unchecked.

**Tech Stack:** C++20 (MSVC v143, Release /MT only), MuPDF 1.27.2 (static, submodule), Catch2 v3.5.4,
Python 3 (fixture generator), PowerShell 5.1 (measurement driver).

**Spec:** `docs/superpowers/specs/2026-11-22-epub-copies-by-chapter-location-232-design.md`
(spec gate passed 2026-11-23, d2c965a). This plan's commit also patches the spec where planning
overturned it; see "Planning findings" below.

## Global Constraints

- Build and test **Release only**. cmake/ctest from BuildTools:
  `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`
  (`ctest.exe` beside it); never the `cmake` on PATH. Build: `& $cmake --build build --target litepdf_unit_tests --config Release`.
  Run tests **from the worktree root** (fixtures resolve relative to it), from **PowerShell**:
  `.\build\tests\Release\litepdf_unit_tests.exe "<name or [tag]>"`. Full suite, serial (no `-j`),
  through the BuildTools `ctest.exe` (a bare `ctest` resolves to a MinGW CMake on PATH):
  `& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" --test-dir build -C Release`.
- The worktree's `third_party\mupdf` is a junction to the main checkout's built submodule. Stage
  with `git add <paths>`; `git commit -a` fails while the junction is in place.
- `fz_try` bodies hold no object with a destructor and throw no C++ exception (#179). Every
  `fz_catch` marks its error handled (`fz_report_error` or `fz_convert_error`, #146). `fz_var` only
  a variable assigned inside the `fz_try` and read after a throw (#178).
- A `GateHold` throws `std::system_error` on re-entry (#70): every new hold sits where a throw
  leaves nothing acquired.
- No MuPDF patch. No change to which documents are shared (#229), to the worker count (#235), or
  to the borrow seam's signature (`SharedDocumentAccess`).
- After Task 5 (when `load_display_list` moves too), `git grep -n "fz_load_page(" -- src` matches
  only inside `load_page_at`. After Task 3 it still matches `RenderEngine.cpp`'s `load_display_list`.
- `TEST_CASE` names are ASCII and start with the subsystem (`Document chapters:`,
  `RenderEngine chapters:`, `SearchSession chapters:`, `PageLoad:`, `ChapterPagesAccess:`,
  `ChapterPages:`, `ChapterTokens:`). Tags: every new case carries `[chapters]`, plus
  `[document]` (Document, PageLoad, ChapterPagesAccess, ChapterTokens), `[core][render]`
  (RenderEngine) or `[search]` (SearchSession), and `[cost]` for the cost oracles, so
  `"[chapters]"` runs all of #232's tests.
- Cost oracles assert `measured <= kCostK * reference + kCostC` with **`kCostK = 4.0`,
  `kCostC = 25.0` ms** (Planning findings, P3), log both readings with `WARN`, and run on the
  1,500-chapter book from `write_many_chapter_epub`.
- All artifacts in English. Commit messages end with
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Planning findings (2026-11-23, probe on this machine)

A throwaway probe (two test files, never committed) settled four premises before tasks were cut.
Both are kept beside the plan gate's evidence, in the session scratchpad: `probe_232_record.cpp`
(P3's timings) and `probe_232b_record.cpp` (P1's pagination walk and P2's overwrite); the
`page-break-before` variants were four generator edits run through the latter.

- **P1. `page-break-before` had no effect in an ePub here; `page-break-after` works.** MuPDF has
  code for it (`html-layout.c:1099-1125`), but a chapter of `<p>` blocks styled
  `page-break-before:always` (class selector, inline style, `<h1>`, wrapping `<div>`: all four)
  laid out as ONE page per chapter. Why was not chased: the fixture only needs a break that works. `page-break-after:always` breaks, and on the
  chapter's last block adds an empty trailing page. So the fixture styles every page's paragraph
  except the chapter's last with `page-break-after:always`. Verified: 12 chapters with counts
  `1 4 2 5 3 1 4 2 5 3 1 4`, 35 pages, flat page *i* holding exactly one token, the expected one.
  **Spec §6.1 patched** (it named `page-break-before`).
- **P2. A file MuPDF holds open can be overwritten.** `std::filesystem::copy_file(...,
  overwrite_existing)` onto an ePub open in a `fz_document` succeeded; a fresh open then saw
  `strip-chapters.epub` (200 chapters, 400 pages). So the mismatch tests overwrite the file; the
  spec's fallback test hook on `ChapterPagesAccess` is **not added**. **Spec §6.2 patched.**
- **P3. Cost constants.** MuPDF-only routes, 1,500 chapters, best of 3, pinned to P-cores (0x5555):

  | route | by location (ms) | by flat number (ms) |
  |---|---|---|
  | fresh copy: load last page + display list + scale-1 render | 1.29 | 158.23 |
  | fresh copy: text of the last page after that render | 0.04 | 157.65 |
  | fresh copy: load last page + dehyphenated stext (a scan) | 0.63 | 155.87 |
  | warm, every page: load + stext | 28.76 | 796.63 |
  | warm, every page: load + dehyphenated stext | 29.13 | 808.35 |

  (3,000 chapters: 1.33 / 509.90, 0.04 / 507.55, 0.77 / 492.05, 66.28 / 9,922.51, 66.39 /
  9,915.70.) With `k = 4`, `c = 25 ms` (#230's constants) the bound is about 30 ms for a
  fresh-copy oracle and about 140 ms for a warm loop: the flat route fails it by at least 5×. The
  location route keeps at least 2.5× to spare while the product adds under ~10 ms per fresh oracle
  (25.16 / 10.04 = 2.51× for the text route, the tightest), and 3× while it adds under ~15 ms per
  warm loop (140.04 / 43.76 = 3.2×); the 2× stop rule below fires at about 12.5 ms and 40 ms extra. Every oracle `WARN`s both
  readings; **if any location-route reading has less than 2× headroom, stop and re-derive k and c**
  (spec §6.3) before committing.
- **P4. Fixture generator location.** The fixture's builder joins `scripts/generate-strip-fixtures.py`
  (next to #230's `chapter-sizes.epub` builder), not a new script: it shares the stored-zip helpers,
  and that script's `--check` already runs in CI (`benchmark.yml`), so the new fixture's byte
  identity is gated with no workflow change. **Spec §6.4 patched** (it named a new script).

## Review Focus

1. **A copy of a file replaced after open, with a different chapter count** — must render and find
   every page as a flat load of the new file does, never crash on a chapter past the copy's spine.
   Pinned by the mismatch tests (Tasks 5, 6) and the `table_for_copy` tests (Task 4).
2. **An index past the table** (`page_count()` or later, a negative `page_num` from a request) —
   must fail exactly as `fz_load_page` does today (throw → null display list / empty text / no
   hits), never `fz_load_chapter_page` on a chapter past the spine. Pinned in Task 2 (`load_page_at`
   throws past the last) and Task 3 (`text_page(page_count())` is empty).
3. **A shared (`DocumentShare::Shared`) ePub** — the product never builds one, but the constructors
   accept it; it must load by location and find/render the right pages. Pinned by the Shared token
   tests (Tasks 5, 6) and the Shared warm-search cost oracle (Task 6).
4. **A Document closed or reopened while an engine or session still holds its table** — the copy
   keeps the `shared_ptr`, so its table outlives the Document's reset. Pinned in Task 4: the table
   handed out survives `Document::close()` with every `locate` answer unchanged.
5. **A non-ePub document** (PDF, FB2, CBZ): no table, every path unchanged. Pinned in Task 4
   (`table` is null for `search.pdf` and `strip-chapters.fb2`; a PDF copy checked against an ePub
   table gets null) and by the existing suite staying green.

---

## File Structure

| file | change | responsibility |
|---|---|---|
| `src/core/detail/ChapterPages.hpp` | modify | + `page_count()` |
| `src/core/detail/PageLoad.hpp` | create | `PageAt`, `page_at`, `load_page_at` declarations (fz types forward-declared) |
| `src/core/detail/ChapterPagesAccess.hpp` | create | `ChapterPagesAccess::table`, `table_for_copy` declarations |
| `src/core/Document.hpp` | modify | forward-declare + befriend `detail::ChapterPagesAccess` |
| `src/core/Document.cpp` | modify | `Impl::chapters` as `shared_ptr`; define the five new functions; every Document path through `load_page_at` |
| `src/core/detail/TextPageAccess.hpp` | modify | `build(ctx, doc, chapters, index)` |
| `src/core/detail/PageHitsAccess.hpp` | modify | `page_hits_on(ctx, doc, chapters, page, needle, flags, cookie)` |
| `src/core/RenderEngine.cpp` | modify | read the table first; per-worker checked tables; pass them on |
| `src/core/SearchSession.cpp` | modify | read the table after the escrow; checked (Own) or as is (Shared); pass it on |
| `scripts/generate-strip-fixtures.py` | modify | + `chapter-tokens.epub` builder |
| `tests/fixtures/chapter-tokens.epub` | create | 12 chapters, 35 pages, one token per page |
| `tests/unit/chapter_test_kit.hpp` | create | moved helpers + token and render-via-list oracles |
| `tests/unit/test_document_page_size.cpp` | modify | use the kit (move only) |
| `tests/unit/test_chapter_copies.cpp` | create | correctness tests (§6.2) |
| `tests/unit/test_chapter_copies_cost.cpp` | create | cost oracles (§6.3) |
| `tests/unit/test_chapter_pages.cpp` | modify | + `page_count` test |
| `tests/unit/test_page_hits_cookie.cpp` | modify | 3 calls gain `nullptr` |
| `tests/CMakeLists.txt` | modify | register the two new test sources |
| `docs/superpowers/specs/...-232-design.md` | modify | P1, P2, P4 patches (this plan's commit); §7 results (Task 8) |
| `CHANGELOG.md` | modify | one `[Unreleased]` → `### Changed` line (Task 8) |

---

### Task 1: Token fixture and shared chapter test kit

**Files:**
- Modify: `scripts/generate-strip-fixtures.py` (docstring list, a `TOKENS_EPUB_NAME` constant, a builder, a `BUILDERS` row)
- Create: `tests/fixtures/chapter-tokens.epub` (written by the script)
- Create: `tests/unit/chapter_test_kit.hpp`
- Modify: `tests/unit/test_document_page_size.cpp:30-133, 250-397` (helpers move out)
- Create: `tests/unit/test_chapter_copies.cpp`
- Modify: `tests/CMakeLists.txt` (after `unit/test_chapter_pages.cpp`: `unit/test_chapter_copies.cpp       # #232`)

**Interfaces:**
- Produces (all `inline`, namespace `litepdf::test_kit`, header includes `<catch2/catch_test_macros.hpp>` and `<mupdf/fitz.h>`):
  - moved unchanged: `struct TempDir` (prefix becomes `"litepdf-chapters-"`), `std::string utf8_of(const std::filesystem::path&)`, `struct OwnDocument` (with `page_count`, `loaded_bounds`, `rendered`), `bool write_many_chapter_epub(const std::filesystem::path&, int chapters)`, `double best_of_3_ms(const std::function<void()>&)`, `bool for_each_page_by_location(OwnDocument&, void (*)(fz_context*, fz_page*))`, `void bound_only(fz_context*, fz_page*)`, `void small_pixmap(fz_context*, fz_page*)`.
  - new on `OwnDocument`: `std::string text(int index)` — flat `fz_load_page` + default-option stext + `fz_print_stext_page_as_text`; empty on a throw. `bool rendered_via_list(int index, float scale, std::vector<std::uint8_t>& bgra, int& w, int& h)` — the render worker's steps exactly (`RenderEngine.cpp` worker_loop): flat load, `fz_new_display_list_from_page`, bbox `fz_round_rect(fz_transform_rect(fz_bound_display_list(dl), fz_scale(s, s)))`, BGR+alpha pixmap cleared to `0xFF`, draw device at identity, `fz_run_display_list(dl, dev, m, fz_infinite_rect, nullptr)`; bytes copied row by row through `fz_pixmap_stride`; no compositing.
  - new free functions: `std::vector<std::string> tokens_in(std::string_view text)` (every `tok` + 2 digits + `_` + 1 digit, in order); `inline constexpr int kTokenChapterPages[12] = {1, 4, 2, 5, 3, 1, 4, 2, 5, 3, 1, 4};` `std::string expected_token(int flat_index)` (walks `kTokenChapterPages`; `"tok%02d_%d"`); `inline constexpr const char* kTokensFixture = "tests/fixtures/chapter-tokens.epub";`.
- Consumes: nothing new.

- [ ] **Step 1: Add the fixture builder.** In `generate-strip-fixtures.py`: `TOKENS_EPUB_NAME = "chapter-tokens.epub"`, `TOKENS_CHAPTER_PAGES = [1, 4, 2, 5, 3, 1, 4, 2, 5, 3, 1, 4]`, identifier `litepdf-chapter-tokens`, via the existing `content_opf`, `CONTAINER_XML`, `write_stored_zip`. `content_opf` calls its name function with `n` = 1..12, so the token builder's name function is `lambda n: "t%02d.xhtml" % (n - 1)` and the chapter entries are written for `n` in 1..12 with `c = n - 1`: the OPF's hrefs are then exactly the zip's `OEBPS/t00.xhtml`..`t11.xhtml`. Chapter `c`'s body is one `<p>tok%02d_%d</p>` per page `p`; every one but the chapter's last has `class="brk"`, and the head carries `<style>.brk{page-break-after:always}</style>` (P1: `page-break-before` is ignored). No `<h1>`; `<title>` is not rendered. Add the docstring entry (12 chapters, counts above, 35 pages, "the oracle for loading pages by chapter location in every copy (#232), not a gate fixture") and the `BUILDERS` row.

- [ ] **Step 2: Generate and check it.**
  The writer rewrites ALL six fixtures, and the PDF and CBZ bytes are identical only with the CI pins. First: `pip show reportlab Pillow` must print 4.4.10 and 12.2.0 (else `pip install "reportlab==4.4.10" "Pillow==12.2.0"`, as `benchmark.yml` does).
  Run: `python scripts/generate-strip-fixtures.py` then `python scripts/generate-strip-fixtures.py --check`
  Expected: every fixture `[OK] ... byte-identical`, and `git status --short tests/fixtures` shows ONLY `?? tests/fixtures/chapter-tokens.epub` (the `--check` cannot see drift the writer just wrote, so this status line is the guard; restore any other with `git checkout -- <file>` and fix the pins).

- [ ] **Step 3: Move the helpers into `chapter_test_kit.hpp` and add the new ones.** `test_document_page_size.cpp` keeps only `read_operations()` in its anonymous namespace, includes the kit and adds `using namespace litepdf::test_kit;`. Its tests do not change.

- [ ] **Step 4: Write the pagination pin** in `test_chapter_copies.cpp`:

```cpp
TEST_CASE("ChapterTokens: each page of the fixture holds its own token, by flat load",
          "[document][chapters]") {
    OwnDocument own(kTokensFixture);
    REQUIRE(own.page_count() == 35);
    int nc = -1;
    fz_var(nc);   // assigned in the try, read after a catch that falls through (#178)
    fz_try(own.ctx) { nc = fz_count_chapters(own.ctx, own.doc); }
    fz_catch(own.ctx) { fz_report_error(own.ctx); }
    REQUIRE(nc == 12);
    for (int c = 0; c < 12; ++c) {
        int m = -1;
        fz_var(m);
        fz_try(own.ctx) { m = fz_count_chapter_pages(own.ctx, own.doc, c); }
        fz_catch(own.ctx) { fz_report_error(own.ctx); }
        CHECK(m == kTokenChapterPages[c]);
    }
    for (int i = 0; i < 35; ++i) {
        INFO("page " << i);
        CHECK(tokens_in(own.text(i)) == std::vector<std::string>{expected_token(i)});
    }
}
```

- [ ] **Step 5: Build and run.**
  Run: build; then `.\build\tests\Release\litepdf_unit_tests.exe "[chapters]"` and `"[pages]"`
  Expected: all pass, including #230's `Document on a 1500-chapter ePub: ...` (the move changed nothing).

- [ ] **Step 6: Commit** — `git add scripts/generate-strip-fixtures.py tests/fixtures/chapter-tokens.epub tests/unit/chapter_test_kit.hpp tests/unit/test_document_page_size.cpp tests/unit/test_chapter_copies.cpp tests/CMakeLists.txt`; message `test(core): chapter-token ePub fixture and shared chapter test kit (#232)`.

---

### Task 2: One loader, and a shared table

**Files:**
- Modify: `src/core/detail/ChapterPages.hpp` (+ `page_count()`)
- Create: `src/core/detail/PageLoad.hpp`
- Modify: `src/core/Document.cpp:59-69` (`Impl::chapters`), `:270-307` (`read_chapter_pages`), `:324-338` (`chapter_location` removed), `:342-409` (open), `:440-499` (`with_rendered_page`), `:596-611` (authenticate), `:620-690` (`page_size`)
- Test: `tests/unit/test_chapter_pages.cpp`, `tests/unit/test_chapter_copies.cpp`

**Interfaces:**
- Produces:
  - `std::size_t ChapterPages::page_count() const noexcept` → `starts_.back()`.
  - In `detail/PageLoad.hpp` (forward-declares `fz_context`, `fz_document`, `fz_page`; includes `ChapterPages.hpp`):
    ```cpp
    namespace litepdf::core::detail {
    struct PageAt { bool by_location = false; int chapter = 0; int page = 0; };
    PageAt page_at(const ChapterPages* table, std::size_t index) noexcept;
    fz_page* load_page_at(fz_context* ctx, fz_document* doc, PageAt at, int index);
    }
    ```
    `page_at`: `by_location` only when `table` is non-null and `table->locate(index)` has a value; plain ints, computed before the caller's `fz_try`. `load_page_at`: called **inside** the caller's `fz_try`; `fz_load_chapter_page(ctx, doc, at.chapter, at.page)` when `at.by_location`, else `fz_load_page(ctx, doc, index)`, so it throws as MuPDF does. A caller holding an `int` index casts with `static_cast<std::size_t>`; a negative one wraps past any table and loads by flat number, failing as today.
  - `Document::Impl::chapters` is `std::shared_ptr<const detail::ChapterPages>`; `read_chapter_pages` returns one (`std::make_shared<const detail::ChapterPages>(std::move(*table))` in a `try`, `std::bad_alloc` → null). Its count read becomes `bool read_chapter_count(fz_context*, fz_document*, int& out) noexcept` (same `fz_try`, reused by Task 4).
- Consumes: Task 1's kit.

- [ ] **Step 1: Write the failing tests.**

  In `test_chapter_pages.cpp`:
  ```cpp
  TEST_CASE("ChapterPages: page_count is the sum of the chapter counts", "[core][chapters]") {
      const auto t = ChapterPages::from_counts({1, 4, 0, 2}, 7);
      REQUIRE(t);
      CHECK(t->page_count() == 7);
  }
  ```
  In `test_chapter_copies.cpp` (`#include "core/detail/PageLoad.hpp"`):
  - `PageLoad: page_at locates an index from the table` — table `from_counts({1, 4, 2}, 7)`: index 0 → `{true, 0, 0}`, 1 → `{true, 1, 0}`, 4 → `{true, 1, 3}`, 5 → `{true, 2, 0}`, 6 → `{true, 2, 1}`.
  - `PageLoad: page_at loads by flat number without a table or past it` — `page_at(nullptr, 0).by_location == false`; with the table above, index 7 and `static_cast<std::size_t>(-1)` → `by_location == false`.
  - `PageLoad: load_page_at loads the located page and throws past the last` — on `OwnDocument own(kTokensFixture)` and its table (`from_counts` of its chapter counts, 35): for every `i`, a `fz_try` that loads `load_page_at(own.ctx, own.doc, page_at(&table, i), i)` and builds its text gives `{expected_token(i)}`; for `i = 35`, the `fz_try` lands in `fz_catch` (record a bool; `fz_report_error` there).

- [ ] **Step 2: Run to see them fail.** Build: fails to compile (`page_count`, `PageLoad.hpp` missing).

- [ ] **Step 3: Implement** `page_count`, `PageLoad.hpp`, `page_at` and `load_page_at` (defined in `Document.cpp`), the `shared_ptr` table, and move `page_size` and `with_rendered_page` onto `PageAt at = page_at(impl_->chapters.get(), index);` before their `fz_try` and `load_page_at(ctx, doc, at, static_cast<int>(index))` inside it. Delete `chapter_location`. Rewrite the `Impl::chapters` comment to the spec §4.2 rule: a table may serve any `fz_document` whose chapter count equals the table's, checked right after that document is opened (and authenticated), under the same no-re-layout invariant; the Document's is assigned at every successful open and authenticate and reset on close; copies keep their own pointer.

- [ ] **Step 4: Run.** `"[chapters]"` and `"[pages]"`: all pass, #230's cost test included.

- [ ] **Step 5: Commit** — `refactor(core): one page loader, page_at + load_page_at, and a shared chapter table (#232)`.

---

### Task 3: The Document's own paths

**Files:**
- Modify: `src/core/detail/TextPageAccess.hpp:57-58`, `src/core/detail/PageHitsAccess.hpp:50-52`
- Modify: `src/core/Document.cpp` — `page_text` (`:702-755`), `page_hits_on` (`:777-870`), `page_hits` (`:872-885`), `TextPageAccess::build` (`:1122-1171`), `text_page` (`:1173-1197`)
- Modify: `src/core/RenderEngine.cpp:162,167`, `src/core/SearchSession.cpp` scan task (the two `page_hits_on` calls) — pass `nullptr` for now (Tasks 5, 6 replace it)
- Modify: `tests/unit/test_page_hits_cookie.cpp:59,75,105` (`nullptr` after the document)
- Test: `tests/unit/test_chapter_copies.cpp`; create `tests/unit/test_chapter_copies_cost.cpp` and register it after `test_chapter_copies.cpp` (`# #232 cost oracles`)

**Interfaces:**
- Produces:
  - `static Document::TextPage TextPageAccess::build(fz_context* ctx, fz_document* doc, const ChapterPages* chapters, int index) noexcept;` (`TextPageAccess.hpp` includes `ChapterPages.hpp`). `PageAt at = page_at(chapters, index)` before the `fz_try`; inside: `if (at.by_location || index < fz_count_pages(ctx, doc)) { page = load_page_at(ctx, doc, at, index); ... }`. With a table the range check is the table's (`at.by_location` implies `index < chapters->page_count()`), so a fresh ePub copy never runs `fz_count_pages`; without one, or past it, unchanged.
  - `std::vector<Document::PageHit> page_hits_on(fz_context* ctx, fz_document* doc, const ChapterPages* chapters, std::size_t page, std::string_view needle_utf8, Document::SearchFlags flags, fz_cookie* cookie);`
  - `Document::text_page`, `page_hits`, `page_text` pass `impl_->chapters.get()`.
- Consumes: Task 2's `page_at`, `load_page_at`, `page_count()`.

- [ ] **Step 1: Write the token tests** in `test_chapter_copies.cpp` (Document on `kTokensFixture`, oracle `OwnDocument own(kTokensFixture)`, `own_tokens(i) = tokens_in(own.text(i))`):
  - `Document chapters: page_text holds each page's own token` — `tokens_in(doc.page_text(i)) == own_tokens(i)` for all 35.
  - `Document chapters: text_page copies each page's own token` — `tp = doc.text_page(i)`, `REQUIRE(tp.valid())`, `full_range(a, b)`, `tokens_in(tp.copy(a, b)) == own_tokens(i)`; and `doc.text_page(35).valid() == false`.
  - `Document chapters: page_hits finds each token on its own page only` — for every `i` and `j`: `doc.page_hits(j, expected_token(i), {}).size() == (i == j ? 1u : 0u)`.

- [ ] **Step 2: Write the warm cost oracles** in `test_chapter_copies_cost.cpp`, each on a fresh 1,500-chapter book (`TempDir`, `write_many_chapter_epub(file, 1500)`, `Document` open, `OwnDocument own(utf8_of(file).c_str())`), measured and reference each `best_of_3_ms`, reference `for_each_page_by_location(own, stext_default)` (or `stext_dehyphenate` for hits; both small `void(fz_context*, fz_page*)` functions in this file):
  - `Document chapters cost: page_text of every page of a 1500-chapter ePub costs what loading by chapter costs` — measured: `doc.page_text(i)` for every page.
  - `Document chapters cost: page_hits of every page ...` — `doc.page_hits(i, "Lorem", {})`.
  - `Document chapters cost: text_page of every page ...` — `REQUIRE(doc.text_page(i).valid())` per page.
  Each: `INFO` both, `CHECK(measured <= kCostK * ref + kCostC)`, `WARN` both (constants at the file's top with the P3 table in a comment).

- [ ] **Step 3: Run to see them fail.** Build, run `"[chapters]"`. Expected: the token tests PASS (flat loads return the same pages — they guard correctness, not cost); the three cost oracles FAIL, flat readings ≈ 0.8 s against bounds ≈ 0.14 s.

- [ ] **Step 4: Implement** the two signatures, the three Document paths, the `nullptr` at the four RenderEngine/SearchSession call sites and the three cookie-test calls.

- [ ] **Step 5: Run.** `"[chapters]"`, `"[search]"`, `"[document]"`: all pass. Check each `WARN`: location readings ≥ 2× under their bound (P3 rule).

- [ ] **Step 6: Commit** — `perf(core): the Document's text, hits and text pages load an ePub page by its chapter (#232)`.

---

### Task 4: A table for a copy, checked

**Files:**
- Create: `src/core/detail/ChapterPagesAccess.hpp`
- Modify: `src/core/Document.hpp:31-33` (forward declaration), `:357` (friend)
- Modify: `src/core/Document.cpp` (definitions, next to the borrow seam)
- Test: `tests/unit/test_chapter_copies.cpp`

**Interfaces:**
- Produces (in `ChapterPagesAccess.hpp`; forward-declares `fz_context`, `fz_document`; includes `core/Document.hpp`, `ChapterPages.hpp`, `<memory>`):
  ```cpp
  namespace litepdf::core::detail {
  struct ChapterPagesAccess {
      // The Document's table, read under a Foreground hold; null when there is none or the
      // Document is not open. Throws std::system_error from the hold on re-entry (#70).
      static std::shared_ptr<const ChapterPages> table(const Document& d);
  };
  // `table` when `copy`'s chapter count equals table->chapter_count(); null when `table` is
  // null (no MuPDF call), the count throws (reported, #146) or differs. Call it right after the
  // copy is opened and authenticated, on the copy's context.
  std::shared_ptr<const ChapterPages> table_for_copy(fz_context* ctx, fz_document* copy,
                                                     std::shared_ptr<const ChapterPages> table) noexcept;
  }
  ```
  The count goes through Task 2's `read_chapter_count`; the comparison and the `shared_ptr` live outside its `fz_try` (#179).
- Consumes: Task 2's `read_chapter_count`, `page_count()`.

- [ ] **Step 1: Write the failing tests** (a test-local RAII `CopyDocument` in `test_chapter_copies.cpp`: `ctx = doc.clone_context()`, `copy = fz_open_document(ctx, path)` in a `fz_try`; drops the copy, then the context):
  - `ChapterPagesAccess: a copy of the same file gets the Document's table` — `t = table(doc)` non-null; `table_for_copy(c.ctx, c.copy, t).get() == t.get()`.
  - `ChapterPagesAccess: a copy with another chapter count gets no table` — the tokens Document's table against a copy of `strip-chapters.epub` → null; against a copy of `tests/fixtures/search.pdf` → null.
  - `ChapterPagesAccess: no table in, no table out` — `table_for_copy(c.ctx, c.copy, nullptr) == nullptr`.
  - `ChapterPagesAccess: a non-ePub Document has no table` — `search.pdf`, `strip-chapters.fb2` → null.
  - `ChapterPagesAccess: the table outlives the Document's close` — take `t`, `doc.close()`, `table(doc) == nullptr`, `t->page_count() == 35`, `t->locate(34)` → chapter 11, page 3.
  - `ChapterPagesAccess: a copy's own chapter counts give the Document's table` (spec §6.2 determinism), for `chapter-tokens.epub`, `chapter-sizes.epub`, `strip-chapters.epub`: on a `CopyDocument`, read `fz_count_chapters` and every `fz_count_chapter_pages`, build `ChapterPages::from_counts(counts, sum)`; `REQUIRE` it; `CHECK` its `chapter_count()` and `page_count()` equal the Document's table's and `doc.page_count()`, and `locate(i)` (chapter and page) equal for every `i < page_count()`.

- [ ] **Step 2: Run to see them fail** (no `ChapterPagesAccess.hpp`: compile error).

- [ ] **Step 3: Implement.**

- [ ] **Step 4: Run.** `"[chapters]"`: all pass.

- [ ] **Step 5: Commit** — `feat(core): a document copy takes the Document's chapter table after a chapter-count check (#232)`.

---

### Task 5: Render workers and their text prefetch

**Files:**
- Modify: `src/core/RenderEngine.cpp` — includes; `load_display_list` (`:35-57`); `Impl` (`:84-100`, two members); `maybe_deliver_text` (`:160-169`); `open_worker_documents` (`:188-275`); `build_display_list` (`:280-293`); start-workers rollback (`:545-560`); constructor (`:509-543`)
- Test: `tests/unit/test_chapter_copies.cpp`, `tests/unit/test_chapter_copies_cost.cpp`

**Interfaces:**
- Produces:
  - `fz_display_list* load_display_list(fz_context*, fz_document*, const detail::ChapterPages* chapters, int page_num) noexcept` (`PageAt` before the `fz_try`).
  - `Impl::chapters` (`std::shared_ptr<const detail::ChapterPages>`, the Document's, Shared only) and `Impl::worker_chapters` (`std::vector<std::shared_ptr<const detail::ChapterPages>>`, Own, index = worker).
  - `void open_worker_documents(Document& doc, std::optional<std::string_view> password, const std::shared_ptr<const detail::ChapterPages>& table)`.
- Consumes: Task 4's `ChapterPagesAccess::table`, `table_for_copy`; Task 3's `TextPageAccess::build` signature.

Constructor order (spec §4.3): the **first** statement after the `n == 0` check is `auto table = detail::ChapterPagesAccess::table(doc);` — before any context is cloned, so a throw from its hold leaves nothing behind. Shared: `impl_->chapters = std::move(table);`. Own: `open_worker_documents(doc, password, table)`, which reserves `worker_chapters` next to `worker_docs` and, per worker after its open and authenticate, `worker_chapters.push_back(detail::table_for_copy(ctx, wdoc, table));` (cannot throw after the reserve). Both rollbacks clear `worker_chapters`. **Also move the pre-existing `impl_->workers.reserve(n);` (`:544`) inside the start-workers `try`** (plan-gate finding, luna): today a `std::bad_alloc` from it leaks every worker document and context, because a throwing constructor runs no `~RenderEngine` and `Impl` has no destructor. Not testable without allocation-failure injection; say so in the PR. Call sites: Own passes `impl->worker_chapters[idx].get()`, Shared `impl->chapters.get()`, to `load_display_list` and `TextPageAccess::build`.

- [ ] **Step 1: Write the token tests.** A test-local helper `render_and_text(RenderEngine&, int page, std::vector<std::uint8_t>& bgra, int& w, int& h, std::string& text)`: one request `{page, 0, 1.0f, on_complete}` with `bypass_cache = true`, `on_text` storing `tp.copy(full range)`, `text_wanted` an atomic holding `page`; `on_complete` copies the pixmap's rows (`fz_pixmap_stride`) and drops it; waits for both callbacks (promises, 10 s bound). One request at a time.
  - `RenderEngine chapters: two own workers render and prefetch each page as a flat load does` — `RenderEngine engine(doc, 2, nullptr)` on `kTokensFixture`; for all 35 pages: bytes, `w`, `h` equal `own.rendered_via_list(i, 1.0f, ...)`, and `tokens_in(text) == tokens_in(own.text(i))`.
  - `RenderEngine chapters: a shared worker renders and prefetches each page as a flat load does` — the same with `RenderEngine engine(doc, 1, nullptr, std::nullopt, DocumentShare::Shared)` (spec §4.5: the product never builds a Shared ePub).
  - `RenderEngine chapters: a copy of a file replaced after open renders every page by flat number` — `TempDir`; copy `kTokensFixture` to `dir/book.epub`; open the Document there; `copy_file("tests/fixtures/strip-chapters.epub", dir/book.epub, overwrite_existing)` (P2); then `RenderEngine engine(doc, 2, nullptr)`; `OwnDocument fresh(utf8_of(dir/book.epub).c_str())`; for every `i < doc.page_count()` (35): bytes equal `fresh.rendered_via_list(i, 1.0f, ...)`.

- [ ] **Step 2: Write the fresh-copy cost oracles** (1,500-chapter book; each measured run builds a NEW `RenderEngine(doc, 1, nullptr)`, untimed; reference on a NEW `OwnDocument` per run, `fz_count_chapters` untimed, then timed `fz_load_chapter_page(ctx, doc, 1499, 0)` + the kit's render-via-list steps at scale 1 — add a `void render_scale1_via_list(fz_context*, fz_page*)` to the kit):
  - `RenderEngine chapters cost: a fresh worker renders the last page of a 1500-chapter ePub as cheaply as loading by chapter` — measured: submit of page 1499 to `on_complete`.
  - `RenderEngine chapters cost: a fresh worker prefetches the last page's text as cheaply as loading by chapter` — measured: `on_complete` (stamped inside it) to `on_text`; reference: untimed load + render of chapter 1499, then timed `fz_load_chapter_page` + `stext_default`.
  Best of 3, `CHECK(measured <= kCostK * ref + kCostC)`, `WARN` both.

- [ ] **Step 3: Run to see them fail.** Token tests and the replaced-file test pass (today's flat loads are correct). The render oracle FAILS (flat ≈ 158 ms against a bound ≈ 30 ms). The prefetch oracle PASSES here, and that is expected: today's flat render has already counted every chapter into the copy's accelerator (`fz_load_page` counts chapters 0..1499 before it loads, `document.c:784-795`), so the flat text build that follows pays only spine walks. Its red comes in Step 5.

- [ ] **Step 4: Implement.**

- [ ] **Step 5: Run.** `"[chapters]"`, `"[render]"` (every render-engine and text-prefetch case) and `"[shared]"`: all pass. `WARN` headroom ≥ 2×.

- [ ] **Step 6: The prefetch oracle's red.** Pass `nullptr` at `maybe_deliver_text`'s Own call site only (Task 7's M2: render by location, text flat), rebuild, run the prefetch oracle: it must FAIL (≈ 158 ms, the flat `fz_count_pages` lays out the other 1,499 chapters). The file also holds this task's uncommitted work, so undo that one edit by hand (not `git checkout`), rebuild, and rerun to see it pass.

- [ ] **Step 7: Commit** — `perf(core): render workers load an ePub page by its chapter on their own copies (#232)`.

---

### Task 6: Search sessions

**Files:**
- Modify: `src/core/SearchSession.cpp` — includes; `Impl` (`:96-100`, one member); constructor (`:198-260`); scan task (`:440-475`, both `page_hits_on` calls)
- Test: `tests/unit/test_chapter_copies.cpp`, `tests/unit/test_chapter_copies_cost.cpp`

**Interfaces:**
- Produces: `Impl::chapters` (`std::shared_ptr<const detail::ChapterPages>`): the Document's (Shared) or the checked one (Own).
- Consumes: Task 4's `ChapterPagesAccess::table`, `table_for_copy`; Task 3's `page_hits_on` signature.

Constructor order (spec §4.3): `auto table = detail::ChapterPagesAccess::table(doc);` right after the `escrow.valid()` check, before the Shared/Own split (a throw drops the local escrow and nothing else). Shared: `impl_->chapters = std::move(table);` beside the escrow move. Own: after `open_scan_document` succeeds, `auto checked = detail::table_for_copy(escrow.get(), sdoc, std::move(table));` (noexcept), and the commit block gains `impl_->chapters = std::move(checked);` (a `shared_ptr` move: still non-throwing). The scan passes `self->chapters.get()` in both modes.

- [ ] **Step 1: Write the token tests** (`app/SearchDispatcher.hpp`'s `InlineDispatcher`):
  - `SearchSession chapters: an own session finds each token on its own page` — for each `i`: `set_query(widen(expected_token(i)), {})`, `REQUIRE(scan_complete())`, `hits_for_page(j).size() == (i == j ? 1u : 0u)` for every `j`.
  - `SearchSession chapters: a shared session finds each token on its own page` — the same, `DocumentShare::Shared`.
  - `SearchSession chapters: a session on a file replaced after open finds hits by flat number` — the Task 5 replacement setup; query `L"Chapter"`; for every `i < 35`, `hits_for_page(i)` equals (count and each quad's `ul_x`, `ul_y`) the oracle `detail::page_hits_on(fresh.ctx, fresh.doc, nullptr, i, "Chapter", {}, nullptr)` (with `nullptr` the helper loads by flat number: an independent route). "Chapter" is on the first page of each `strip-chapters.epub` chapter only, so a location from the old table lands on the wrong page.

- [ ] **Step 2: Write the cost oracles** (1,500-chapter book):
  - `SearchSession chapters cost: a fresh session scans the last page as cheaply as loading by chapter` — per run a NEW `test_kit::DeferredDispatcher deferred` declared first, then a NEW `SearchSession s(doc, deferred, std::nullopt)` (a reused dispatcher never frees its slots, so `run(0)` would run run 1's emptied slot), `set_query(L"Lorem", {}, 1499)`, timed `deferred.run(0)`; afterwards `REQUIRE(!s.hits_for_page(1499).empty())` (the task scanned the last page). Reference per run: new `OwnDocument`, untimed `fz_count_chapters`, timed `fz_load_chapter_page(.., 1499, 0)` + `stext_dehyphenate`.
  - `SearchSession chapters cost: a warm whole-book search costs what loading by chapter costs, own session` — one session; `set_query(L"Lorem", {})` + `run_all()` untimed (lays out its copy); measured best of 3 of `set_query` + `run_all`; reference `for_each_page_by_location(own, stext_dehyphenate)`.
  - `SearchSession chapters cost: ... shared session` — the same with `DocumentShare::Shared` (pins Review Focus 3; the Shared session's document is the Document's, already laid out, so only the O(c²) walk separates the routes — the P3 warm row).

- [ ] **Step 3: Run to see them fail.** Token and replaced-file tests pass; the three cost oracles FAIL.

- [ ] **Step 4: Implement.**

- [ ] **Step 5: Run.** `"[chapters]"`, `"[search]"`, `"[crosstab]"` and `"[shared]"`: all pass. `WARN` headroom ≥ 2×.

- [ ] **Step 6: Commit** — `perf(core): search scans load an ePub page by its chapter on their own copies (#232)`.

---

### Task 7: Acceptance — every path's oracle fails when that path alone is flat

**Files:** none committed (mutations are reverted). Use the `mutation-test-hygiene` skill: one mutant at a time, rebuild, run the named test, restore with `git checkout -- <file>`, confirm `git status` clean before the next.

- [ ] **Step 1: Grep.** `git grep -n "fz_load_page(" -- src` → only the line in `load_page_at`. `git grep -n "chapter_location" -- src` → nothing.
- [ ] **Step 2: Run the mutants.** Each, built alone, must make its oracle FAIL; record the flat reading.

  | # | mutant (at the call site) | must fail |
  |---|---|---|
  | M1 | `build_display_list`, Own: `nullptr` for the table | RenderEngine cost: fresh render |
  | M2 | `maybe_deliver_text`, Own: `nullptr` | RenderEngine cost: fresh prefetch |
  | M3 | scan task, Own: `nullptr` | SearchSession cost: fresh scan, and warm own |
  | M4 | scan task, Shared: `nullptr` | SearchSession cost: warm shared |
  | M5 | `Document::page_text`: `nullptr` | Document cost: page_text |
  | M6 | `Document::page_hits`: `nullptr` | Document cost: page_hits |
  | M7 | `Document::text_page`: `nullptr` | Document cost: text_page |
  | M8 | `Document::page_size`: `nullptr` | #230's 1500-chapter test (page_size) |
  | M9 | `Document::with_rendered_page`: `nullptr` | #230's 1500-chapter test (printing) |
  | M10 | `table_for_copy` returns `table` without comparing | both replaced-file tests |
  | M11 | `page_at`: `chapter + 1` below the last chapter (`chapter + 1 < table->chapter_count()`); a plain `+ 1` sends pages 31-34 to chapter 12, past the spine, and the run dies in the §4.2 crash instead of failing cleanly | Document chapters token tests |
  | M12 | `page_at`: `page + 1` | Document chapters token tests |

  Not pinned by a cost oracle, by decision: the Shared render worker's two call sites (the product never builds a Shared ePub; their token test proves the pages are right). Say so in the PR.
- [ ] **Step 3: Full suite.** The BuildTools `ctest.exe` (full path, Global Constraints) `--test-dir build -C Release`, serial, from PowerShell: all pass; note the count against `-N`.
- [ ] **Step 4:** Record every oracle's two readings (product location route from the passing run's `WARN`, flat from the mutant) in a table for spec §7 and the PR. No commit.

---

### Task 8: In-app measurement (spec §5), results, CHANGELOG

**Files:**
- Modify: `docs/superpowers/specs/2026-11-22-epub-copies-by-chapter-location-232-design.md` (§7)
- Modify: `CHANGELOG.md` (`[Unreleased]` → `### Changed`)
- Never committed (go into the PR description in full): the probe patch, the book generator, the driver.

- [ ] **Step 1: Books.** A Python generator (stdlib only, `random.Random(232)`): N spine items × 10 paragraphs of 60 words drawn from a fixed 500-word lowercase vocabulary, every paragraph starting AND ending with `quartz` (the word on every page: a paragraph is far shorter than a page, so every page holds a paragraph's start or end, including a chapter's last page that holds only the tail of one); `xylograph` is in no paragraph (the word on no page). Stored zip, as `write_stored_zip`. N = 800 and 1,500, written outside the repo.
- [ ] **Step 2: Probe patch**, applied to both builds. In `SearchSession`: `State` gains `std::chrono::steady_clock::time_point probe_t0` and `std::size_t probe_pages`; when `std::getenv("LITEPDF_PROBE_SEARCH")` is set, `set_query` stamps both where it sets `pages_remaining = pages`, and the scan task that decrements `pages_remaining` to 0 prints `litepdf-probe-search: <ms> ms, <pages> pages` to stderr (outside the state lock).
- [ ] **Step 3: Two builds.** After: this branch + patch, target `litepdf`, Release. Before: a throwaway worktree at `116070a` (junction + configure as in the build memory) + the same patch. Confirm both print the probe line (launch with `Start-Process -RedirectStandardError`; if a GUI-subsystem launch loses stderr, switch the patch to append the line to the file named by the variable's value, in both builds).
- [ ] **Step 4: Driver** (PowerShell 5.1, no `?.`/`??`/ternary). Wrap every run: back up and restore `%LOCALAPPDATA%\LitePDF\session.json`, `HKCU\Software\LitePDF\MRU` and `crashes\`, set `LITEPDF_NO_RESTORE=1`. Per run: launch the exe on the book, set `ProcessorAffinity = 0x5555`, wait for the window; search: `scripts/ux-probe.ps1 send-cmd 40042` (IDM_FIND), `WM_SETTEXT` the word into the `LitePDFFindBar`'s edit child, wait (60 s deadline) for the probe line, read `WorkingSet64`; End: a fresh launch, the End key to the canvas, wait until `WorkingSet64` is stable for 2 s, read it. Close with `CloseMainWindow` + `WaitForExit(5000)`.
- [ ] **Step 5: Run.** 5 rounds × {800, 1,500} × {quartz, xylograph, End}, before/after interleaved per round, quiet machine. Report medians and the spread.
- [ ] **Step 6: Write §7** (the in-app table and Task 7's readings) and the CHANGELOG line: searching an ePub with many chapters is faster, with the before/after search times for 800 and 1,500 chapters; if the working set drops, the same line says by how much (#232).
- [ ] **Step 7: Commit** — `docs: #232 results and CHANGELOG line`.

---

## Plan gate (Full, routine), 2026-11-23, on c058e88

Evidence: the spec gate's bundle plus 22 excerpts the plan relies on (3,607 lines), the two probe
records. All findings verified against the source before they were taken.

- **Opus (slot-1):** 4 findings, 3 questions, all taken. The fresh-prefetch oracle cannot go red
  before Task 5, because today's flat render has already counted every chapter (Task 5 Steps 3, 6);
  the pagination pin read `nc`/`m` after a fall-through catch without `fz_var` (#178); the
  `fz_load_page` grep checkpoint belongs after Task 5, not Task 3; P3's "3× to spare" was 2.5×
  for the fresh oracles. Questions taken: a new `DeferredDispatcher` per fresh-scan run; M11 kept
  below the last chapter (a plain `+ 1` crashes); the probe records named. Also from its negative
  sweep: the fixture writer rewrites every fixture, so Task 1 Step 2 pins the generator's packages
  and guards with `git status`.
- **Codex terra@medium:** 1 finding (the 3× arithmetic, = Opus).
- **Codex gpt-6.1-terra@medium:** 2 findings (`content_opf` numbers chapters from 1, so a 0-based
  file name breaks the spine — fixed in Task 1 Step 1; the `fz_var` finding, = Opus), and one
  question taken: `quartz` at a paragraph's start alone could miss a chapter's last page.
- **Codex luna@max:** 4 findings, all taken: the `content_opf` numbering (= 6.1); the
  `DeferredDispatcher` reuse (= Opus Q2); a bare `ctest` resolves to the wrong CMake; and a
  pre-existing leak, `workers.reserve(n)` outside the start-workers `try` in `RenderEngine`'s
  constructor (Task 5 moves it in).

## After the plan

PR gate (Full, routine: Opus slot-1 + Codex terra → gpt-6.1-terra → luna, sequential), then the
PR. Its description carries: the probe patch as a diff, the book generator, the driver, Task 7's
mutant table, §7's numbers, and the not-pinned Shared render call sites. Remove the MuPDF junction
(`cmd /c rmdir third_party\mupdf`, then recreate the empty directory) before the worktree is cleaned up.
