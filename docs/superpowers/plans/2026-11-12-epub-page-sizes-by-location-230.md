# ePub page sizes by chapter location (#230) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `Document::page_size(i)` and `Document::with_rendered_page(i)` find a multi-chapter document's page by its chapter location, so `page_sizes()` on a 1,500-chapter ePub drops from about 2.5 s to about 11 ms, with every size and render bit-identical to today's.

**Architecture:** a pure table `detail::ChapterPages` (flat page → chapter, page) built once at open from MuPDF's per-chapter page counts, held in `Document::Impl`, and read by `page_size` and `with_rendered_page` to call `fz_load_chapter_page` instead of `fz_load_page`. Core only: no UI change.

**Tech Stack:** C++17 (MSVC, `/W4`), Catch2, CMake/ctest from VS 2022 BuildTools, Python 3 + reportlab/Pillow for the fixture script.

**Spec:** `docs/superpowers/specs/2026-11-12-epub-page-sizes-by-location-230-design.md`. Read §4 and §8 before any task; § numbers below refer to it.

## Global Constraints

- Branch `claude/issue-230-epub-sizing-f3694d`, base `main` @ `75ef154`. Find code by name if a line moved.
- **Build:** Release only, never Debug. `$cmake` / `$ctest` are the BuildTools binaries under `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\`, never the ones on PATH. Build: `& $cmake --build build --target litepdf_unit_tests --config Release`. `third_party\mupdf` is a junction to the main checkout's; remove it with `cmd /c rmdir` before the worktree is cleaned up.
- **Run tests from PowerShell, from the worktree root** (fixtures resolve from there): `build\tests\Release\litepdf_unit_tests.exe "<tag or exact name>"`. Full suite: `& $ctest --test-dir build -C Release`, **serially, no `-j`**. Read the count with `-N`; never hard-code it.
- New `TEST_CASE` names are ASCII and start with `ChapterPages: ` (pure unit) or `Document::page_size` (document tests, as the existing ones). A comma in a name must be escaped as `\,` in a filter.
- MuPDF rules: every `fz_catch` calls `fz_report_error` (#146); no C++ object with a destructor, no allocation and no `throw` inside a `fz_try` (#179); a local assigned inside `fz_try` and read after a throw is `fz_var`'d (#178).
- Mutation steps follow the `mutation-test-hygiene` skill: apply, rebuild, see the named case fail, restore, rebuild, see it pass. `git diff` is clean of the mutation before the commit.
- Commits: conventional, English, ending with `Co-Authored-By: <the model that wrote the commit> <noreply@anthropic.com>`. `git add` files by name (a junctioned `third_party/mupdf` breaks `git commit -a`). Do not push.

## Review Focus

1. **Bit identity.** `fz_load_page(n)` ends in `fz_load_chapter_page(c, p)` for the location of `n`; the table must give that same location for every `n`, including after chapters of 0 pages. Pinned by Task 1 (brute force) and Task 3 (oracle against `fz_load_page` on a fixture whose every chapter has its own width).
2. **Fallback.** No table (single chapter, MuPDF error, inconsistent counts) or an index the table does not hold → today's `fz_load_page` path, so errors and out-of-range behaviour do not change.
3. **The speed tests can fail.** Task 4's two bounds must each fail with only its own call site reverted to `fz_load_page`.

## Task 1: `detail::ChapterPages`

**Files:** create `src/core/detail/ChapterPages.hpp`, `tests/unit/test_chapter_pages.cpp`; modify `tests/CMakeLists.txt`.

- [ ] **Step 1: write the failing tests** in `tests/unit/test_chapter_pages.cpp`, tags `[core][chapters]`:

```cpp
// #230: flat page number -> (chapter, page), the table Document::page_size
// uses so it does not walk the chapters from the first on every page.
#include "core/detail/ChapterPages.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>
#include <vector>

using litepdf::core::detail::ChapterLocation;
using litepdf::core::detail::ChapterPages;

namespace {

// What fz_load_page does: walk the chapters from the first until the running
// sum passes the index.
std::optional<ChapterLocation> walk(const std::vector<int>& counts, std::size_t index) {
    std::size_t start = 0;
    for (std::size_t c = 0; c < counts.size(); ++c) {
        const std::size_t m = static_cast<std::size_t>(counts[c]);
        if (index < start + m)
            return ChapterLocation{static_cast<int>(c), static_cast<int>(index - start)};
        start += m;
    }
    return std::nullopt;
}

std::size_t sum(const std::vector<int>& counts) {
    std::size_t s = 0;
    for (int m : counts) s += static_cast<std::size_t>(m);
    return s;
}

}  // namespace

TEST_CASE("ChapterPages: locate matches a walk from the first chapter", "[core][chapters]") {
    const std::vector<std::vector<int>> tables = {
        {1},
        {3, 1, 4},
        {0, 2, 0, 0, 3, 0},   // empty chapters at the start, middle and end
        {5, 0, 1, 1, 2, 7},
    };
    for (const auto& counts : tables) {
        const std::size_t n = sum(counts);
        const auto table = ChapterPages::from_counts(counts, n);
        REQUIRE(table.has_value());
        CHECK(table->chapter_count() == counts.size());
        for (std::size_t i = 0; i <= n; ++i) {   // one past the end too
            INFO("index " << i);
            const auto want = walk(counts, i);
            const auto got = table->locate(i);
            REQUIRE(got.has_value() == want.has_value());
            if (want) {
                CHECK(got->chapter == want->chapter);
                CHECK(got->page == want->page);
            }
        }
    }
}

TEST_CASE("ChapterPages: refuses counts that do not describe the document", "[core][chapters]") {
    CHECK_FALSE(ChapterPages::from_counts({2, -1, 3}, 4).has_value());
    CHECK_FALSE(ChapterPages::from_counts({2, 3}, 4).has_value());
    CHECK_FALSE(ChapterPages::from_counts({2, 3}, 6).has_value());
    const auto empty = ChapterPages::from_counts({}, 0);
    REQUIRE(empty.has_value());
    CHECK_FALSE(empty->locate(0).has_value());
}
```

Register it in `tests/CMakeLists.txt` next to `unit/test_document_page_size.cpp`: `unit/test_chapter_pages.cpp        # #230`.

- [ ] **Step 2: build; it fails to compile** (no header).

- [ ] **Step 3: write `src/core/detail/ChapterPages.hpp`:**

```cpp
#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

// #230: where a flat page number lies in a multi-chapter document (an ePub's
// spine items). MuPDF's fz_load_page finds it by walking the chapters from the
// first and asking each one's page count; for an ePub each of those asks walks
// the spine from its head, so one page in chapter c costs O(c^2) and sizing
// every page O(N^3). Document builds this table once at open and loads pages
// with fz_load_chapter_page. Pure, so it is testable without MuPDF.

namespace litepdf::core::detail {

struct ChapterLocation {
    int chapter = 0;
    int page = 0;   // within the chapter
};

class ChapterPages {
public:
    // counts[c] = the pages in chapter c. Nothing when the counts cannot be
    // the document's: a negative count, or a sum other than page_count.
    static std::optional<ChapterPages> from_counts(const std::vector<int>& counts,
                                                   std::size_t page_count) {
        std::vector<std::size_t> starts;
        starts.reserve(counts.size() + 1);
        std::size_t start = 0;
        for (int m : counts) {
            if (m < 0) return std::nullopt;
            starts.push_back(start);
            start += static_cast<std::size_t>(m);
        }
        if (start != page_count) return std::nullopt;
        starts.push_back(start);
        return ChapterPages(std::move(starts));
    }

    // The chapter holding flat page `index` and the page within it, as
    // fz_load_page would find them; nothing past the last page. A chapter of
    // 0 pages shares its start with the next chapter and is skipped.
    std::optional<ChapterLocation> locate(std::size_t index) const noexcept {
        if (index >= starts_.back()) return std::nullopt;
        // The first start greater than index, minus one: the last chapter
        // starting at or before it.
        const auto it = std::upper_bound(starts_.begin(), starts_.end(), index);
        const std::size_t chapter = static_cast<std::size_t>(it - starts_.begin()) - 1;
        return ChapterLocation{static_cast<int>(chapter),
                               static_cast<int>(index - starts_[chapter])};
    }

    std::size_t chapter_count() const noexcept { return starts_.size() - 1; }

private:
    explicit ChapterPages(std::vector<std::size_t> starts) : starts_(std::move(starts)) {}
    std::vector<std::size_t> starts_;   // first page of each chapter, then the page count
};

}  // namespace litepdf::core::detail
```

(`upper_bound` gives the last chapter whose start is ≤ `index`; with empty chapters several share a start, and the last of them is the one with pages, matching the walk, which skips a chapter of 0 pages because `index < start + 0` is false.)

- [ ] **Step 4: build and run** `build\tests\Release\litepdf_unit_tests.exe "[chapters]"` — both pass.

- [ ] **Step 5: mutation.** Replace `upper_bound` with `lower_bound` (then a page at a chapter start maps to the chapter before). Rebuild, see `ChapterPages: locate matches a walk from the first chapter` fail; restore; rebuild; pass.

- [ ] **Step 6: commit** `feat(core): ChapterPages maps a flat page to its chapter (#230)`.

## Task 2: the fixture `chapter-sizes.epub`

**Files:** modify `scripts/generate-strip-fixtures.py`, `tests/unit/test_strip_fixtures.cpp`; create `tests/fixtures/chapter-sizes.epub` (generated).

- [ ] **Step 1:** in the generator, add `SIZES_EPUB_NAME = "chapter-sizes.epub"`, `SIZES_CHAPTERS = 12`, `SIZES_TEXT = ("Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod " "tempor incididunt ut labore et dolore magna aliqua.")` (spec §8.1: NOT the module's longer `LOREM`), chapter files `OEBPS/c%02d.xhtml`, and:

```python
def sizes_chapter_xhtml(k):
    # @page side margins of 200 + 10k pt push MuPDF's text width under its
    # 72 pt floor (fz_layout_html), so the page is 72 + 2 * margin wide: each
    # chapter has its own width. 1 + 7k % 25 paragraphs give the chapters
    # different page counts.
    margin = 200 + 10 * k
    paragraphs = "\n".join("<p>%s</p>" % SIZES_TEXT for _ in range(1 + (7 * k) % 25))
    return """<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml">
<head><title>Chapter %d</title><style>@page{margin:0 %dpt}</style></head>
<body>
<h1>Chapter %d</h1>
%s
</body>
</html>
""" % (k, margin, k, paragraphs)
```

and `build_sizes_epub(out_path)` writing `mimetype`, `META-INF/container.xml` (the existing `CONTAINER_XML`), an OPF for 12 items (generalise `content_opf()` to take the chapter count and a name function rather than copying it), and the 12 chapters, through `write_stored_zip`. Add `(SIZES_EPUB_NAME, build_sizes_epub)` to `BUILDERS`. Update the module docstring (the fixture list and "regenerates all four" → five).

- [ ] **Step 2:** run `python scripts/generate-strip-fixtures.py`, then `python scripts/generate-strip-fixtures.py --check` → five `[OK]` lines; `git status` shows only the new `.epub` and the script among fixtures (the four existing fixtures byte-identical, so unchanged).

- [ ] **Step 3:** in `test_strip_fixtures.cpp` add a section `chapter-sizes.epub: 12 page widths, all pages 600 pt high`: open, `CHECK(doc.page_count() == <the count the committed file gives>)`, collect `page_size(i).width_pt` of every page into a `std::set<float>` and `CHECK(widths.size() == 12)`, and every height `== 600.0f`. Read the real page count from a run first (the prototype gave 68; the committed text decides). `Document` has no chapter count and none is added (spec §8.2).

- [ ] **Step 4:** build, run `"[fixtures][strip]"` → pass. Commit `test(fixtures): chapter-sizes.epub, one page width per chapter (#230)`.

## Task 3: `Document` builds the table and `page_size` uses it

**Files:** modify `src/core/Document.cpp`, `tests/unit/test_document_page_size.cpp`.

- [ ] **Step 1: tests first.** (a) Add `"tests/fixtures/chapter-sizes.epub"` to the fixture list of `Document::page_size equals the loaded page's bounds on every fixture`. It passes today too (both routes load the same page); it must keep passing, and its mutants (Step 5) prove it can fail. (b) Add `CHECK_THROWS_AS(doc.page_size(doc.page_count()), std::runtime_error)` on `chapter-sizes.epub` as its own case `Document::page_size past the last page of a multi-chapter ePub throws`. (c) New case `Document::with_rendered_page draws each page of a multi-chapter ePub as fz_load_page does` (spec §8.2 "Print path, content"). Give `OwnDocument` a method `bool rendered(int index, std::vector<std::uint8_t>& bgra, int& w, int& h)` that does exactly what `Document::with_rendered_page` does at scale 1, no rotation, on `fz_load_page(index)`: `fz_round_rect(fz_transform_rect(fz_bound_page(...), fz_scale(1,1)))`, `fz_new_pixmap_with_bbox(ctx, fz_device_bgr(ctx), bbox, nullptr, 1)`, `fz_clear_pixmap_with_value(ctx, pix, 0xFF)`, `fz_new_draw_device(ctx, fz_identity, pix)`, `fz_run_page(ctx, page, dev, m, nullptr)`, `fz_close_device`; then copy the samples out AFTER the `fz_try` (read `with_rendered_page`'s body first and mirror its exact calls and order, including how it hands rows to the callback — stride vs width × 4). The case, for every page: call `doc.with_rendered_page(i, 1.0f, 1.0f, false, cb)` where `cb` copies `w`, `h` and the bytes, then `CHECK` width, height and `bytes == own_bytes`. On a mismatch report the page and the first differing offset, not the whole buffer.

- [ ] **Step 2:** in `Document.cpp`: `#include "core/detail/ChapterPages.hpp"`; `Impl` gains `std::optional<detail::ChapterPages> chapters;` with a comment pointing at #230 and at the NULL-write precondition (spec §4.2 last paragraph). Add, in the anonymous namespace after `read_page_count`:

```cpp
// #230: the chapter table of a multi-chapter document, read right after its
// page count from the same fz_document. Nothing for a single chapter
// (fz_load_page finds every page on its first try), when MuPDF throws, or
// when the counts do not add up; pages then load by flat number.
std::optional<detail::ChapterPages> read_chapter_pages(fz_context* ctx, fz_document* doc,
                                                       std::size_t page_count) {
    int nc = 0;
    fz_try(ctx) { nc = fz_count_chapters(ctx, doc); }
    fz_catch(ctx) { fz_report_error(ctx); return std::nullopt; }   // also marks it handled (#146)
    if (nc <= 1) return std::nullopt;
    std::vector<int> counts;
    try {
        counts.assign(static_cast<std::size_t>(nc), 0);   // outside any fz_try (#179)
    } catch (const std::bad_alloc&) {
        return std::nullopt;
    }
    int* const out = counts.data();
    fz_try(ctx) {
        for (int c = 0; c < nc; ++c) out[c] = fz_count_chapter_pages(ctx, doc, c);
    }
    fz_catch(ctx) { fz_report_error(ctx); return std::nullopt; }
    try {
        return detail::ChapterPages::from_counts(counts, page_count);
    } catch (const std::bad_alloc&) {
        return std::nullopt;
    }
}
```

Assign it after the page count in `open` (`impl_->chapters = read_chapter_pages(impl_->ctx, impl_->doc, impl_->page_count);` after a successful `read_page_count`) and in `authenticate` (inside the `read_page_count` success branch, before `authenticated = true`). Reset it (`impl_->chapters.reset()`) in `close()` and at the start of `open` next to `reset_cbz()`.

- [ ] **Step 3:** a file-local helper, used by both `page_size` (non-PDF branch) and `with_rendered_page`:

```cpp
// Where fz_load_page would find flat page `index`, from the #230 table:
// filled into plain ints before a fz_try (no object with a destructor
// crosses into it). False when there is no table or the index is past it;
// the caller then loads by flat number, as before, with the same errors.
bool chapter_location(const std::optional<detail::ChapterPages>& table, std::size_t index,
                      int& chapter, int& page) noexcept {
    if (!table) return false;
    const auto loc = table->locate(index);
    if (!loc) return false;
    chapter = loc->chapter;
    page = loc->page;
    return true;
}
```

Before each `fz_try`: `int chapter = 0, page_in_chapter = 0; const bool by_location = chapter_location(impl_->chapters, index, chapter, page_in_chapter);`. Inside, the `fz_load_page` call becomes:

```cpp
page = by_location
    ? fz_load_chapter_page(ctx, doc, chapter, page_in_chapter)
    : fz_load_page(ctx, doc, static_cast<int>(index));
```

Extend the comments above both calls: why (the #230 walk) and that the fallback keeps today's errors. `with_rendered_page` already holds `doc_mutex` before the lookup.

- [ ] **Step 4:** build, run `"[document][pages]"` → all pass.

- [ ] **Step 5: mutations** (each: apply, rebuild, the named case fails, restore). In `chapter_location`, (i) `chapter = std::min(loc->chapter + 1, <last chapter>)` and (ii) `chapter = 0`: `Document::page_size equals the loaded page's bounds on every fixture` and `Document::with_rendered_page draws each page …` both fail. (iii) In `with_rendered_page` only, `page_in_chapter + 1` when the page is not the chapter's last (the size stays right): the render case fails on its bytes while the page_size case passes.

- [ ] **Step 6: commit** `fix(core): load a multi-chapter page by its chapter location (#230)`.

## Task 4: the speed test

**Files:** modify `tests/unit/test_document_page_size.cpp`.

- [ ] **Step 1:** add a helper that writes a 1,500-chapter ePub to a fresh directory under `%TEMP%` with MuPDF's zip writer (`fz_new_zip_writer`, `fz_write_zip_entry` — it always stores, `compress` is ignored —, `fz_close_zip_writer`, `fz_drop_zip_writer`; buffers from `fz_new_buffer_from_copied_data`), following #178/#179 (all strings built before each `fz_try`; one `fz_try` per entry is fine). Entries: `mimetype`, `META-INF/container.xml`, `OEBPS/content.opf` (1,500 items + itemrefs), and chapter k = the spec §8.1 template without the `<style>`, with one `<p>S</p>` (one page each). The directory is removed at the end of the case (RAII guard outside any `fz_try`). Measure the flat route on this exact shape once (mutation in Step 3) before trusting the 250 ms bound; if the flat route is under 750 ms on this machine, raise the chapter count, not the bound.

- [ ] **Step 2:** the case `Document on a 1500-chapter ePub: sizing and printing every page cost what loading by chapter costs` (spec §8.2 "Speed"): open the book with `Document` and with `OwnDocument`; check `page_count() == 1500` on both. Reference timings in `OwnDocument`, each best of 3, walking `fz_count_chapters` / `fz_count_chapter_pages` / `fz_load_chapter_page` in chapter order: (a) bounds of every page; (b) a pixmap of every page at `fz_scale(0.05f, 0.05f)` (`fz_new_pixmap_from_page`, BGR, alpha 1). Then, best of 3 each: (A) `doc.page_size(i)` for every page; (B) `doc.with_rendered_page(i, 0.05f, 0.05f, false, noop)` for every page. `INFO` all four readings; `CHECK(A < 4 * a + 25)` and `CHECK(B < 4 * b + 25)` (ms). Use `std::chrono::steady_clock`. Time only the loops, never the open.

- [ ] **Step 3: mutations.** (i) `page_size` forced to the flat route: the sizing check fails (expected ~1,000 ms against a bound of ~50 ms), the print check passes. (ii) `with_rendered_page` forced to the flat route: the print check fails (expected ~1,000 ms against ~250-350 ms), the sizing check passes. Restore; rebuild; pass. Record all readings for the PR body. If (ii) does not fail by at least 2× on this machine, raise the chapter count (the flat cost grows with its cube, the reference linearly), not the factor.

- [ ] **Step 4: commit** `test(core): a 1500-chapter ePub is sized and printed at chapter-load cost (#230)`.

## Task 5: measurements, spec record, CHANGELOG

- [ ] **Step 1:** Release build of the branch; copy its `litepdf-cli.exe` aside. Baseline: the copy of main's (75ef154) `litepdf-cli.exe` made before Task 1. Books from the session's `make-epub.py` (seed 169; 200/400/800/1,500 chapters; the 1,500-section single chapter control) plus `tests/fixtures/strip-chapters.epub`. For each, interleaving before/after, pinned (`start "" /b /wait /affinity FFFF`), file read once first: `--bench-page-sizes --iterations 3`, three runs each, report the median of the three medians. `--benchmark --iterations 3` `open_ms` at 1,500 chapters before/after.
- [ ] **Step 2:** fill spec §5 with the table; add the 1,500-chapter before/after line to the continuous-scroll spec §4.4's EPUB notes. Commit `docs(spec): #230 readings`.
- [ ] **Step 3:** CHANGELOG `[Unreleased]` → `### Fixed` line (spec §9), with the measured numbers. Commit `docs(changelog): #230`.
- [ ] **Step 4:** full ctest serially from PowerShell; record the count.
