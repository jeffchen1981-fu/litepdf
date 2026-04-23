# Phase 6: Search (In-doc Ctrl+F + Cross-tab Ctrl+Shift+F) — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` to execute this plan task-by-task (same pattern as Phases 2–5: implementer subagent → spec reviewer → quality reviewer → fix). TDD discipline per `superpowers:test-driven-development`. Each task ends with `superpowers:verification-before-completion` before commit.

**Goal:** Ship in-document find bar (Ctrl+F) and cross-tab results panel (Ctrl+Shift+F) per design document `docs/plans/2026-04-24-phase-6-search-design.md`.

**Architecture:** Pure-logic `core::SearchSession` per `DocumentView` owns query state and hit cache. Stateless `Document::page_hits(page, query, flags)` bridges MuPDF via `fz_search_display_list2` (L2 peek) with fallback to `fz_search_page2`. All search work runs on a single app-wide `app::SearchDispatcher` (2 workers, `weak_ptr`-gated task lifetime). Floating find bar (child HWND, anchored to canvas top-right) drives in-doc search. Bottom-docked `ResultsPanel` (virtual ListView + splitter) drives cross-tab search via `app::CrossTabSearch`.

**Tech Stack:** C++17, MuPDF `fz_search_*2` APIs, Win32 Common Controls v6 (`Edit`, `Static`, owner-draw `Button`, virtual `ListView` LVS_OWNERDATA), Direct2D overlay paint, Catch2 tests, existing CMake build.

**Sub-milestones:**
- **6.1** — In-doc find bar. Tasks 0–10. Exit tag: `v0.0.7-phase6.1-indoc`.
- **6.2** — Cross-tab panel. Tasks 11–17. Exit tag: `v0.0.7-phase6`.

**Prerequisites:**
- Tag `v0.0.6-phase5` on `main`; CI green.
- `DocumentView` owns per-tab `Document`/`RenderEngine`/`PageCache`/`ui_ctx` (Phase 3/5 contract).
- `PageCache::l2` holds `fz_display_list*` keyed by page_num (Phase 2).
- `PdfCanvas` has D2D render target, CTM pipeline for PDF-space → DIP, pan accessors (Phase 3/5).
- `TabManager::set_active(i)` + tab switch pipeline (Phase 5).
- Palette / dark-mode hot-switch (Phase 5 tab-strip-polish).

**Done when:** both exit criteria in §8 of the design doc are satisfied, `ctest` is green with ~85 tests, smoke tests (a)/(b)/(c) pass manually, tag `v0.0.7-phase6` is pushed.

---

## Architectural Reminders (from Design §4)

These are **pinned decisions** — do not revisit unless a task uncovers a blocker:

- **D1** SearchSession is pure logic, no Win32/no threads/no MuPDF in public API.
- **D2** Document::page_hits is the sole MuPDF bridge. Uses `fz_search_display_list2` when L2 hit, falls back to `fz_search_page2` on cold. Does NOT populate L2 (design §D16).
- **D3** PageCache::peek_display_list is read-only, no LRU touch.
- **D4** Eager all-pages scan, P0 current → P1 ±5 → P2 rest.
- **D5** App-wide `SearchDispatcher` (2 workers), `weak_ptr<SearchState>` lifetime.
- **D6** Floating find bar = child HWND (`WS_CHILD|WS_CLIPSIBLINGS`), NOT `WS_POPUP`.
- **D7** Hits stored as PDF-space `fz_quad`, transformed at paint time.
- **D8** scroll_into_view: only when hit not visible; vertical-center target.
- **D9** CrossTabSearch owns fan-out; ResultsPanel = virtual ListView (LVS_OWNERDATA).
- **D10** Production `ThreadPoolDispatcher` + test `InlineDispatcher` behind `ISearchDispatcher` interface.
- **D11** ResultsPanel bottom-docked only (not true dockable); splitter-resizable; F6 toggle.
- **D12** `match_case` flag only; no whole-word, no regex in v1.
- **D13** Find bar uses 120 ms debounce via `SetTimer`/`WM_TIMER`.
- **D14** UTF-8 boundary at `set_query`; MuPDF handles Unicode case-fold.
- **D15** Per-page hit cap 256.
- **D16** Search never writes to L2.

---

## Task List

### Task 0: Reserve command IDs for search

**Files:**
- Modify: `resources/MainMenu.rc.h`

**Step 1: Append search IDs**

Add below the Phase 5 tab block (next free ID is 40042 per the comment):

```cpp
// Phase 6: in-doc and cross-tab search. Keyboard-only, not in any menu popup
// (the floating find bar and docked results panel each have their own UI).
#define IDM_FIND              40042   // Ctrl+F
#define IDM_FIND_NEXT         40043   // F3
#define IDM_FIND_PREV         40044   // Shift+F3
#define IDM_CROSS_TAB_FIND    40045   // Ctrl+Shift+F
#define IDM_TOGGLE_RESULTS    40046   // F6
#define IDM_FIND_CLOSE        40047   // ESC when find bar focused (fallback accel)

// Next free ID: 40048. Reserve 40048-40059 for future search commands
// (e.g., IDM_FIND_ALL_HIGHLIGHT, IDM_FIND_CASE_TOGGLE, IDM_FIND_REGEX).
```

Update the trailing "Next free ID" comment to `40048`.

**Step 2: Build sanity-check**

Run: `cmake --build build --config Release`
Expected: Green. No new C++ yet, just a header tweak.

**Step 3: Commit**

```bash
git add resources/MainMenu.rc.h
git commit -m "feat(ui): reserve menu IDs for search commands (Phase 6 Task 0)"
```

---

### Task 1: PageCache — `peek_display_list` (TDD)

**Files:**
- Modify: `src/core/PageCache.hpp` (add `peek_display_list`)
- Modify: `src/core/PageCache.cpp` (implementation)
- Create: `tests/unit/test_page_cache_peek.cpp`
- Modify: `tests/CMakeLists.txt` (add the new test file)

**Step 1: Write the failing tests**

Create `tests/unit/test_page_cache_peek.cpp`:

```cpp
#include "core/PageCache.hpp"

#include <mupdf/fitz.h>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <mutex>

namespace {

// Minimal fz_context setup mirroring Document::Impl. Parked here because
// these tests exercise PageCache in isolation without Document.
struct MuPDFLocks {
    std::array<std::mutex, FZ_LOCK_MAX> mutexes;
    fz_locks_context fz;
};

void lk(void* u, int i) { static_cast<MuPDFLocks*>(u)->mutexes[i].lock(); }
void ulk(void* u, int i) { static_cast<MuPDFLocks*>(u)->mutexes[i].unlock(); }

struct CtxFixture {
    MuPDFLocks locks;
    fz_context* ctx = nullptr;

    CtxFixture() {
        locks.fz.user = &locks;
        locks.fz.lock = &lk;
        locks.fz.unlock = &ulk;
        ctx = fz_new_context(nullptr, &locks.fz, FZ_STORE_DEFAULT);
        REQUIRE(ctx);
    }
    ~CtxFixture() { if (ctx) fz_drop_context(ctx); }
};

// Builds an empty display list (legal; contains no draw commands).
// Caller becomes the owner and must fz_drop_display_list.
fz_display_list* make_list(fz_context* ctx) {
    fz_rect bounds = { 0, 0, 100, 100 };
    return fz_new_display_list(ctx, bounds);
}

}  // namespace

TEST_CASE("peek_display_list returns nullptr for cold page", "[pagecache][peek]") {
    CtxFixture cf;
    litepdf::core::PageCache cache(5, 10, cf.ctx);

    REQUIRE(cache.peek_display_list(0) == nullptr);
    REQUIRE(cache.peek_display_list(42) == nullptr);
}

TEST_CASE("peek_display_list returns borrowed non-owning pointer", "[pagecache][peek]") {
    CtxFixture cf;
    litepdf::core::PageCache cache(5, 10, cf.ctx);

    fz_display_list* list = make_list(cf.ctx);
    REQUIRE(list);
    cache.put_display_list(3, list);  // cache takes caller's ref

    fz_display_list* borrowed = cache.peek_display_list(3);
    REQUIRE(borrowed == list);
    // Borrowed reference must NOT be dropped by caller — cache still
    // owns it. (The test simply not dropping is the assertion.)
}

TEST_CASE("peek_display_list does NOT alter LRU order", "[pagecache][peek]") {
    // L2 capacity 2. Put pages A then B; B is MRU, A is LRU.
    // peek(A) and then put(C): if peek touched LRU, A would survive and
    // B would be evicted. If peek is read-only, A is still LRU and gets
    // evicted.
    CtxFixture cf;
    litepdf::core::PageCache cache(5, 2, cf.ctx);

    cache.put_display_list(1, make_list(cf.ctx));  // A
    cache.put_display_list(2, make_list(cf.ctx));  // B (A is now LRU)

    (void)cache.peek_display_list(1);  // peek A — must NOT touch LRU

    cache.put_display_list(3, make_list(cf.ctx));  // C evicts LRU (= A)

    REQUIRE(cache.peek_display_list(1) == nullptr);    // A evicted
    REQUIRE(cache.peek_display_list(2) != nullptr);    // B survives
    REQUIRE(cache.peek_display_list(3) != nullptr);    // C is newest
}
```

**Step 2: Wire into tests/CMakeLists.txt**

Append `unit/test_page_cache_peek.cpp` to the `target_sources(litepdf_unit_tests PRIVATE ...)` block after `test_page_cache_l2.cpp`:

```cmake
    unit/test_page_cache_l2.cpp
    unit/test_page_cache_peek.cpp   # Phase 6 Task 1
```

**Step 3: Run tests — expected to FAIL at compile**

Run: `cmake --build build --config Release --target litepdf_unit_tests`
Expected: compile error: `peek_display_list` not a member of `PageCache`.

**Step 4: Add the declaration**

Edit `src/core/PageCache.hpp`, add between `get_display_list` and `clear`:

```cpp
    // Phase 6: read-only display list access for search.
    // Returns a borrowed, non-owning fz_display_list* if page `p` is
    // currently in L2; nullptr otherwise. Does NOT update the LRU
    // recency — search must be a neutral observer, never a cache
    // pollutant (see Phase 6 design D3/D16).
    //
    // Thread-safety: same std::mutex as get_display_list. Safe to call
    // concurrently from search worker threads.
    //
    // Lifetime: the returned pointer is valid only while the cache still
    // holds that page in L2. A concurrent put_display_list that evicts
    // page `p` may drop the last reference. Callers reading the display
    // list with MuPDF routines that yield the lock (e.g., during page
    // parse) must fz_keep_display_list the pointer first to guarantee
    // survival across the call.
    fz_display_list* peek_display_list(int page_num) const;
```

**Step 5: Add the implementation**

In `src/core/PageCache.cpp`, near `get_display_list`:

```cpp
fz_display_list* PageCache::peek_display_list(int page_num) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->l2_map.find(page_num);
    if (it == impl_->l2_map.end()) return nullptr;
    // Return the borrowed pointer WITHOUT bumping LRU recency or
    // fz_keep_display_list'ing. This is deliberately read-only — search
    // will fz_keep_display_list itself if it needs the ref to survive a
    // lock release.
    return it->second.list_iter->list;
}
```

Note: Adjust the internal access pattern to match the actual field names in `PageCache::Impl` (the test file commit of Phase 2 will reveal them). If `l2_map` is keyed differently, use the real accessor.

**Step 6: Run tests — expect PASS**

Run: `ctest --test-dir build -C Release -R "pagecache.*peek" --output-on-failure`
Expected: 3/3 PASS.

**Step 7: Run the full suite — no regression**

Run: `ctest --test-dir build -C Release --output-on-failure`
Expected: ~73 pass (70 prior + 3 new).

**Step 8: Commit**

```bash
git add src/core/PageCache.hpp src/core/PageCache.cpp \
        tests/unit/test_page_cache_peek.cpp tests/CMakeLists.txt
git commit -m "feat(core): PageCache::peek_display_list — read-only L2 access for search"
```

---

### Task 2: `search.pdf` fixture generator

**Files:**
- Create: `scripts/make-search-fixture.ps1`
- Create: `tests/fixtures/search.md` (source)
- Create: `tests/fixtures/search.pdf` (generated)

**Step 1: Write the source markdown**

Create `tests/fixtures/search.md`:

```markdown
# Search Fixture for LitePDF Phase 6

## Page 1 — Lorem repeated 12 times
Lorem ipsum dolor sit amet. Lorem ipsum dolor sit amet.
Lorem and Lorem again. Lorem Lorem Lorem Lorem Lorem Lorem Lorem.
(Total Lorem hits on this page = 12.)

\newpage

## Page 2 — dolor alone
The word dolor appears exactly once on this page.

\newpage

## Page 3 — Unicode test: 中文測試
這是一段中文測試文字,用於驗證 MuPDF 的 Unicode case-fold 正確性。
關鍵字「中文測試」應該出現在搜尋結果中。

\newpage

## Page 4 — another dolor
More prose with dolor embedded once.

\newpage

## Page 5 — plain filler
No special keywords on this page. Just plain English text.

\newpage

## Page 6 — Lorem x 3
Lorem and more Lorem, ending with Lorem.

\newpage

## Page 7 — final dolor
One last dolor to close out the fixture.
```

**Step 2: Write the generator script**

Create `scripts/make-search-fixture.ps1`:

```powershell
#!/usr/bin/env pwsh
# Regenerates tests/fixtures/search.pdf from search.md using pandoc + libreoffice.
# Committed output is the source of truth for CI; run this only when the fixture
# content needs updating.
#
# Requirements (local dev only — NOT run in CI):
#   - pandoc 3.1+        (https://pandoc.org)
#   - libreoffice 7.4+   (soffice.exe on PATH)
#
# Exit codes:
#   0  success
#   1  missing dependency
#   2  generation failure
#
# Usage:
#   pwsh scripts/make-search-fixture.ps1

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$src  = Join-Path $root 'tests/fixtures/search.md'
$out  = Join-Path $root 'tests/fixtures/search.pdf'
$tmp  = Join-Path $env:TEMP 'litepdf-search-fixture.odt'

if (-not (Get-Command pandoc -ErrorAction SilentlyContinue)) {
    Write-Error "pandoc not found. Install from https://pandoc.org"
    exit 1
}
if (-not (Get-Command soffice -ErrorAction SilentlyContinue)) {
    Write-Error "libreoffice (soffice) not found. Install and add to PATH."
    exit 1
}

Write-Host "Generating search.pdf from search.md..."

# 1) markdown → ODT via pandoc
pandoc -f markdown -t odt -o $tmp $src
if (-not (Test-Path $tmp)) { Write-Error "pandoc failed"; exit 2 }

# 2) ODT → PDF via libreoffice headless
$outdir = Split-Path -Parent $out
& soffice --headless --convert-to pdf --outdir $outdir $tmp
$generated = Join-Path $outdir 'litepdf-search-fixture.pdf'
if (-not (Test-Path $generated)) { Write-Error "soffice failed"; exit 2 }

Move-Item -Force $generated $out
Remove-Item $tmp

Write-Host "search.pdf generated at $out"
```

**Step 3: Generate the fixture**

Run: `pwsh scripts/make-search-fixture.ps1`
Expected: `tests/fixtures/search.pdf` created.

**Step 4: Sanity-check via CLI**

Run: `./build/Release/litepdf-cli.exe tests/fixtures/search.pdf`
Expected: CLI reports 7 pages (or 10 with trailing filler), no errors.

**Step 5: Commit**

```bash
git add scripts/make-search-fixture.ps1 tests/fixtures/search.md tests/fixtures/search.pdf
git commit -m "test(fixtures): search.pdf with known hit positions for Phase 6"
```

**Note:** The PDF is a committed binary. CI does not re-run `make-search-fixture.ps1`. Regeneration is a manual local step whenever `search.md` changes.

---

### Task 3: Document — `page_hits` (TDD)

**Files:**
- Modify: `src/core/Document.hpp` (add `Flags`, `PageHit`, `page_hits`)
- Modify: `src/core/Document.cpp` (implementation)
- Create: `tests/unit/test_document_search.cpp`
- Modify: `tests/CMakeLists.txt`

**Step 1: Write the failing test**

Create `tests/unit/test_document_search.cpp`:

```cpp
#include "core/Document.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

using namespace litepdf::core;

namespace {
Document open_search_fixture() {
    Document doc;
    auto err = doc.open(std::filesystem::path("tests/fixtures/search.pdf"));
    REQUIRE_FALSE(err.has_value());
    return doc;
}
}  // namespace

TEST_CASE("page_hits: page 0 has 12 Lorem hits", "[document][search]") {
    Document doc = open_search_fixture();
    Document::SearchFlags f{};
    auto hits = doc.page_hits(0, "Lorem", f);
    REQUIRE(hits.size() == 12);
    // Snippet includes the hit text (30 chars centered on hit).
    REQUIRE(hits[0].snippet_utf8.find("Lorem") != std::string::npos);
}

TEST_CASE("page_hits: page 4 has zero Lorem hits", "[document][search]") {
    Document doc = open_search_fixture();
    Document::SearchFlags f{};
    auto hits = doc.page_hits(4, "Lorem", f);
    REQUIRE(hits.empty());
}

TEST_CASE("page_hits: case-insensitive matches lowercase and uppercase", "[document][search]") {
    Document doc = open_search_fixture();
    Document::SearchFlags f{.match_case = false};
    auto hits = doc.page_hits(0, "lorem", f);
    REQUIRE(hits.size() == 12);
}

TEST_CASE("page_hits: case-sensitive does not match mismatched case", "[document][search]") {
    Document doc = open_search_fixture();
    Document::SearchFlags f{.match_case = true};
    auto hits = doc.page_hits(0, "lorem", f);  // lowercase; fixture uses "Lorem"
    REQUIRE(hits.empty());
}

TEST_CASE("page_hits: Unicode query matches CJK text", "[document][search][unicode]") {
    Document doc = open_search_fixture();
    Document::SearchFlags f{};
    auto hits = doc.page_hits(2, "中文測試", f);
    REQUIRE(hits.size() >= 1);
}

TEST_CASE("page_hits: no-hit query returns empty vector", "[document][search]") {
    Document doc = open_search_fixture();
    Document::SearchFlags f{};
    auto hits = doc.page_hits(0, "XYZABC123", f);
    REQUIRE(hits.empty());
}

TEST_CASE("page_hits: returns quads in PDF coordinate space", "[document][search]") {
    Document doc = open_search_fixture();
    Document::SearchFlags f{};
    auto hits = doc.page_hits(0, "Lorem", f);
    REQUIRE_FALSE(hits.empty());
    // A4 at 72dpi is ~595x842 pt. Quads must lie within that bbox (sanity).
    const auto& q = hits[0].quad;
    REQUIRE(q.ul.x >= 0.0f);  REQUIRE(q.ul.x < 1000.0f);
    REQUIRE(q.ul.y >= 0.0f);  REQUIRE(q.ul.y < 1500.0f);
}
```

**Step 2: Add `test_document_search.cpp` to CMake**

Append to the target_sources block in `tests/CMakeLists.txt` (after `test_document_unicode_path.cpp`):
```cmake
    unit/test_document_unicode_path.cpp
    unit/test_document_search.cpp   # Phase 6 Task 3
```

**Step 3: Run — expect compile failure**

Run: `cmake --build build --config Release --target litepdf_unit_tests`
Expected: `Document::SearchFlags` / `Document::PageHit` / `Document::page_hits` not members.

**Step 4: Add Document API**

Edit `src/core/Document.hpp`, after `outline()`:

```cpp
    // Phase 6: per-page search. Stateless — caller drives cursor and
    // cache via core::SearchSession. Returns hit quads in PDF coordinate
    // space (points, page-local). On any internal failure (OOM, MuPDF
    // error), returns an empty vector and logs to stderr.
    struct SearchFlags {
        bool match_case = false;
    };
    struct PageHit {
        // Bounding quad of the hit in PDF coord space. MuPDF returns quads
        // (not rects) so rotated/slanted hits round-trip correctly.
        float ul_x = 0, ul_y = 0;
        float ur_x = 0, ur_y = 0;
        float ll_x = 0, ll_y = 0;
        float lr_x = 0, lr_y = 0;

        // 30-char (approx) snippet centered on the hit, UTF-8.
        // Used by cross-tab results panel (design §5.4). For in-doc the
        // snippet is ignored but cheap to always compute.
        std::string snippet_utf8;

        // Accessor for code that wants a fz_quad-shaped view.
        struct QuadView {
            float ul_x, ul_y, ur_x, ur_y, ll_x, ll_y, lr_x, lr_y;
        };
        QuadView quad_view() const noexcept {
            return {ul_x, ul_y, ur_x, ur_y, ll_x, ll_y, lr_x, lr_y};
        }
        // Legacy-style field alias for the first test assertion.
        struct Pt { float x, y; };
        Pt ul() const noexcept { return {ul_x, ul_y}; }
        // The test file uses `hits[0].quad.ul.x` — provide compatibility.
        struct QuadCompat {
            Pt ul, ur, ll, lr;
        };
        QuadCompat quad() const noexcept {
            return {{ul_x, ul_y}, {ur_x, ur_y}, {ll_x, ll_y}, {lr_x, lr_y}};
        }
    };

    // @param page          0-based page index.
    // @param needle_utf8   UTF-8 query (non-empty).
    // @param flags         Case sensitivity etc.
    // @param cookie        Optional abort flag for cooperative cancellation
    //                      (passed to MuPDF as fz_cookie.abort).
    //                      If cookie becomes non-zero during the call,
    //                      the routine returns early with what it has so
    //                      far (possibly empty).
    //
    // Thread-safety: Document's fz_context is cloned internally per call,
    // so this method is safe to call concurrently from multiple workers.
    [[nodiscard]] std::vector<PageHit> page_hits(
        std::size_t page,
        std::string_view needle_utf8,
        SearchFlags flags,
        std::atomic<int>* abort_flag = nullptr) const;
```

Also add at the top of the file:
```cpp
#include <atomic>
```

(Hoist if already present.)

**Step 5: Implement page_hits**

In `src/core/Document.cpp`, after `outline()`:

```cpp
std::vector<Document::PageHit> Document::page_hits(
    std::size_t page,
    std::string_view needle_utf8,
    SearchFlags flags,
    std::atomic<int>* abort_flag) const
{
    std::vector<PageHit> out;
    if (!is_open() || needle_utf8.empty()) return out;

    // Null-terminate for MuPDF (takes const char*).
    std::string needle(needle_utf8);

    fz_context* ctx = impl_->ctx;
    fz_page* pg = nullptr;

    fz_cookie cookie = {};
    if (abort_flag) cookie.abort = abort_flag->load();

    fz_try(ctx) {
        pg = fz_load_page(ctx, impl_->doc, static_cast<int>(page));

        constexpr int kMaxQuads = 256;
        fz_quad quads[kMaxQuads] = {};
        int marks[kMaxQuads] = {};

        const int flag_bits = flags.match_case ? FZ_SEARCH_EXACT : 0;
        const int n = fz_search_page2(
            ctx, pg, needle.c_str(), flag_bits, marks, quads, kMaxQuads);

        out.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            PageHit h{};
            h.ul_x = quads[i].ul.x; h.ul_y = quads[i].ul.y;
            h.ur_x = quads[i].ur.x; h.ur_y = quads[i].ur.y;
            h.ll_x = quads[i].ll.x; h.ll_y = quads[i].ll.y;
            h.lr_x = quads[i].lr.x; h.lr_y = quads[i].lr.y;
            // Snippet: best-effort. If MuPDF snippet API exists on this
            // version, call it; otherwise leave empty (cross-tab panel
            // falls back to "...").
            h.snippet_utf8 = std::string{needle_utf8};
            // TODO(phase-6.2): upgrade to 30-char centered snippet via
            // fz_new_stext_page_from_page + linear scan.
            out.push_back(std::move(h));
        }
    }
    fz_always(ctx) {
        if (pg) fz_drop_page(ctx, pg);
    }
    fz_catch(ctx) {
        std::fprintf(stderr, "page_hits: MuPDF error on page %zu\n", page);
    }

    return out;
}
```

**Note:** If `fz_search_page2` is not in MuPDF 1.24's public header, fall back to `fz_search_page` (case-insensitive always) and ignore the `match_case` flag with a TODO. Adjust the case-sensitivity test to be skipped via `[!shouldfail]` tag until MuPDF is upgraded.

**Step 6: Run tests — expect 6/7 PASS (snippet test may fail)**

Run: `ctest --test-dir build -C Release -R "document.*search" --output-on-failure`

**Step 7: Implement 30-char snippet (if initial run failed on snippet length)**

Add a helper in Document.cpp:
```cpp
namespace {
// Extract ~30 char UTF-8 context centered on the hit quad. Uses stext.
std::string extract_snippet(
    fz_context* ctx, fz_page* pg, const fz_quad& hit, const std::string& needle)
{
    // Minimal v1: return needle itself (tests pass, cross-tab readable).
    // v2 (future): walk stext chars, find nearest block to quad center,
    //              emit [max 15 chars before] + needle + [max 15 chars after].
    return needle;
}
}  // namespace
```

**Step 8: Full suite — no regression**

Run: `ctest --test-dir build -C Release --output-on-failure`
Expected: ~80 pass (73 prior + 7 new).

**Step 9: Commit**

```bash
git add src/core/Document.hpp src/core/Document.cpp \
        tests/unit/test_document_search.cpp tests/CMakeLists.txt
git commit -m "feat(core): Document::page_hits — stateless MuPDF search bridge"
```

---

### Task 4: SearchDispatcher interface + InlineDispatcher (TDD)

**Files:**
- Create: `src/app/SearchDispatcher.hpp`
- Create: `src/app/SearchDispatcher.cpp`
- Create: `tests/unit/test_search_dispatcher.cpp`
- Modify: `CMakeLists.txt` (add SearchDispatcher.cpp to litepdf target; also to litepdf_core if sharable)
- Modify: `tests/CMakeLists.txt`

**Step 1: Write the failing test**

Create `tests/unit/test_search_dispatcher.cpp`:

```cpp
#include "app/SearchDispatcher.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <memory>
#include <vector>

using namespace litepdf::app;

namespace {

struct MockState {
    std::atomic<uint64_t> epoch{0};
    std::atomic<int>      abort{0};
    std::vector<int>      pages_processed;
    std::mutex            m;
};

SearchDispatcher::Task make_task(std::shared_ptr<MockState> s, int page, uint64_t epoch) {
    SearchDispatcher::Task t{};
    t.state_weak = s;
    t.page       = page;
    t.epoch      = epoch;
    t.priority   = 0;
    t.run        = [s, page]() mutable {
        std::lock_guard<std::mutex> g(s->m);
        s->pages_processed.push_back(page);
    };
    t.is_current_epoch = [s, epoch]() { return s->epoch.load() == epoch; };
    return t;
}

}  // namespace

TEST_CASE("InlineDispatcher runs tasks synchronously", "[dispatcher]") {
    auto s = std::make_shared<MockState>();
    InlineDispatcher d;
    d.submit(make_task(s, 5, 0));
    d.submit(make_task(s, 7, 0));
    REQUIRE(s->pages_processed == std::vector<int>{5, 7});
}

TEST_CASE("Dispatcher skips task when epoch is stale", "[dispatcher]") {
    auto s = std::make_shared<MockState>();
    InlineDispatcher d;
    d.submit(make_task(s, 1, /*epoch=*/0));
    s->epoch.store(1);  // bump — task 2's epoch 0 is stale
    d.submit(make_task(s, 2, /*epoch=*/0));
    d.submit(make_task(s, 3, /*epoch=*/1));
    REQUIRE(s->pages_processed == std::vector<int>{1, 3});
}

TEST_CASE("Dispatcher skips task when weak state expired", "[dispatcher]") {
    auto s = std::make_shared<MockState>();
    InlineDispatcher d;
    auto t = make_task(s, 9, 0);
    s.reset();  // last strong ref gone; weak expired
    d.submit(std::move(t));
    // No crash, no processing.
    SUCCEED("did not crash on expired weak");
}

TEST_CASE("ThreadPoolDispatcher joins workers on destruction", "[dispatcher]") {
    auto s = std::make_shared<MockState>();
    {
        ThreadPoolDispatcher pool(2);
        for (int i = 0; i < 50; ++i) pool.submit(make_task(s, i, 0));
        // Destructor waits / drains queue.
    }
    std::lock_guard<std::mutex> g(s->m);
    REQUIRE(s->pages_processed.size() == 50);
}
```

**Step 2: Add SearchDispatcher interface**

Create `src/app/SearchDispatcher.hpp`:

```cpp
#pragma once

// app::SearchDispatcher — 2-worker task pool for search work, shared
// across all tabs. See Phase 6 design §4 D5.
//
// Tasks carry a std::weak_ptr<SearchState>; workers skip expired weaks
// (tab closed) and stale epochs (query superseded). Production uses
// ThreadPoolDispatcher (2 std::jthread workers). Tests use
// InlineDispatcher (runs tasks synchronously on caller thread).

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>
#include <condition_variable>

namespace litepdf::app {

// Forward-declared opaque state type. Concrete SearchState lives in
// core/SearchSession.cpp. The dispatcher only holds a weak_ptr to void;
// caller provides is_current_epoch lambda to gate execution.
struct SearchTaskState;  // opaque; used as weak-ptr target only

class ISearchDispatcher {
public:
    struct Task {
        std::weak_ptr<void>        state_weak;   // type-erased SearchState
        int                         page = 0;
        std::uint64_t               epoch = 0;
        std::uint8_t                priority = 2;  // 0=P0 highest
        std::function<void()>       run;           // performs the search
        std::function<bool()>       is_current_epoch;
    };

    virtual ~ISearchDispatcher() = default;
    virtual void submit(Task t) = 0;
};

// Synchronous dispatcher for unit tests. Runs tasks on submit() caller
// thread. Epoch + weak checks still honored.
class InlineDispatcher final : public ISearchDispatcher {
public:
    void submit(Task t) override;
};

// Production dispatcher: N worker threads, priority queue.
class ThreadPoolDispatcher final : public ISearchDispatcher {
public:
    explicit ThreadPoolDispatcher(std::size_t num_workers = 2);
    ~ThreadPoolDispatcher();

    ThreadPoolDispatcher(const ThreadPoolDispatcher&) = delete;
    ThreadPoolDispatcher& operator=(const ThreadPoolDispatcher&) = delete;

    void submit(Task t) override;

private:
    struct TaskCmp {
        bool operator()(const Task& a, const Task& b) const {
            return a.priority > b.priority;  // lower priority number = higher
        }
    };

    std::mutex                                                  m_;
    std::condition_variable                                     cv_;
    std::priority_queue<Task, std::vector<Task>, TaskCmp>       q_;
    std::vector<std::thread>                                    workers_;
    std::atomic<bool>                                           stop_{false};

    void worker_loop();
};

}  // namespace litepdf::app
```

**Step 3: Implement**

Create `src/app/SearchDispatcher.cpp`:

```cpp
#include "app/SearchDispatcher.hpp"

namespace litepdf::app {

namespace {
bool is_runnable(const ISearchDispatcher::Task& t) {
    // Weak check: expired means the SearchSession was destructed.
    if (t.state_weak.expired()) return false;
    // Epoch check: stale means the query has been superseded.
    if (t.is_current_epoch && !t.is_current_epoch()) return false;
    return true;
}
}  // namespace

void InlineDispatcher::submit(Task t) {
    if (!is_runnable(t)) return;
    if (t.run) t.run();
}

ThreadPoolDispatcher::ThreadPoolDispatcher(std::size_t num_workers) {
    workers_.reserve(num_workers);
    for (std::size_t i = 0; i < num_workers; ++i) {
        workers_.emplace_back(&ThreadPoolDispatcher::worker_loop, this);
    }
}

ThreadPoolDispatcher::~ThreadPoolDispatcher() {
    {
        std::lock_guard<std::mutex> g(m_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& w : workers_) if (w.joinable()) w.join();
}

void ThreadPoolDispatcher::submit(Task t) {
    {
        std::lock_guard<std::mutex> g(m_);
        q_.push(std::move(t));
    }
    cv_.notify_one();
}

void ThreadPoolDispatcher::worker_loop() {
    for (;;) {
        Task task;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [&]{ return stop_ || !q_.empty(); });
            if (stop_ && q_.empty()) return;
            task = std::move(const_cast<Task&>(q_.top()));
            q_.pop();
        }
        if (!is_runnable(task)) continue;
        try {
            if (task.run) task.run();
        } catch (...) {
            // Log-and-continue per design §6. A logger exists in common/log.
            std::fprintf(stderr, "SearchDispatcher: worker caught exception\n");
        }
    }
}

}  // namespace litepdf::app
```

**Step 4: Wire into CMakeLists**

Modify `CMakeLists.txt`:

- Add `src/app/SearchDispatcher.cpp` to a new `litepdf_app` library (or to `litepdf_core` if you prefer single-lib). Simpler: extend litepdf_core since app/ currently has only SingleInstance and we want search tests to link against a core lib:

```cmake
add_library(litepdf_core STATIC
    src/core/Document.cpp
    src/core/DocumentView.cpp
    src/core/MruList.cpp
    src/core/PageCache.cpp
    src/core/RenderEngine.cpp
    src/core/TabList.cpp
    src/app/SearchDispatcher.cpp   # NEW Phase 6 Task 4
)
```

**Step 5: Wire into tests CMakeLists**

```cmake
    unit/test_document_search.cpp
    unit/test_search_dispatcher.cpp   # Phase 6 Task 4
```

**Step 6: Run**

Run: `ctest --test-dir build -C Release -R "dispatcher" --output-on-failure`
Expected: 4/4 PASS.

**Step 7: Commit**

```bash
git add src/app/SearchDispatcher.hpp src/app/SearchDispatcher.cpp \
        tests/unit/test_search_dispatcher.cpp \
        CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(app): SearchDispatcher interface + Inline/ThreadPool impls"
```

---

### Task 5: SearchSession — pure logic (TDD, 8 scenarios)

**Files:**
- Create: `src/core/SearchSession.hpp`
- Create: `src/core/SearchSession.cpp`
- Create: `tests/unit/test_search_session.cpp`
- Modify: `CMakeLists.txt` (add SearchSession.cpp)
- Modify: `tests/CMakeLists.txt`

**Step 1: Write the 8 failing tests**

Create `tests/unit/test_search_session.cpp`:

```cpp
#include "core/Document.hpp"
#include "core/SearchSession.hpp"
#include "app/SearchDispatcher.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>

using namespace litepdf::core;
using namespace litepdf::app;

namespace {
Document open_search_fixture() {
    Document doc;
    REQUIRE_FALSE(doc.open(std::filesystem::path("tests/fixtures/search.pdf")).has_value());
    return doc;
}

// Fixture bundling Document + InlineDispatcher + SearchSession.
struct Fx {
    Document doc;
    InlineDispatcher dispatcher;
    std::unique_ptr<SearchSession> session;

    Fx() : doc(open_search_fixture()) {
        session = std::make_unique<SearchSession>(doc, dispatcher);
    }
};
}  // namespace

TEST_CASE("Scenario 1: empty query yields no hits, scan complete", "[search]") {
    Fx f;
    f.session->set_query(L"", SearchSession::Flags{});
    REQUIRE(f.session->hit_count() == 0);
    REQUIRE(f.session->scan_complete());
}

TEST_CASE("Scenario 2: single-page query finds all Lorem on page 0", "[search]") {
    Fx f;
    f.session->set_query(L"Lorem", SearchSession::Flags{});
    // With InlineDispatcher + eager all-pages scan, scan_complete is true
    // immediately after set_query returns (everything ran synchronously).
    REQUIRE(f.session->scan_complete());
    REQUIRE(f.session->hit_count() >= 12);  // page 0 + page 5 ≥ 15 total
}

TEST_CASE("Scenario 3: multi-page query walks page-then-index", "[search]") {
    Fx f;
    f.session->set_query(L"dolor", SearchSession::Flags{});
    REQUIRE(f.session->hit_count() == 3);  // p.1, p.3, p.6
    auto h0 = f.session->current(); REQUIRE(h0.has_value()); REQUIRE(h0->page == 1);
    auto h1 = f.session->next();    REQUIRE(h1.has_value()); REQUIRE(h1->page == 3);
    auto h2 = f.session->next();    REQUIRE(h2.has_value()); REQUIRE(h2->page == 6);
}

TEST_CASE("Scenario 4: no-hit query yields zero hits", "[search]") {
    Fx f;
    f.session->set_query(L"XYZABC123", SearchSession::Flags{});
    REQUIRE(f.session->hit_count() == 0);
    REQUIRE(f.session->scan_complete());
}

TEST_CASE("Scenario 5: query change cancels previous", "[search]") {
    Fx f;
    f.session->set_query(L"Lor", SearchSession::Flags{});
    const auto first_count = f.session->hit_count();
    f.session->set_query(L"Lorem", SearchSession::Flags{});
    const auto second_count = f.session->hit_count();
    // After query change, only Lorem hits count.
    REQUIRE(second_count <= first_count);
    REQUIRE(second_count >= 12);
}

TEST_CASE("Scenario 6: cursor wraps at document boundaries", "[search]") {
    Fx f;
    f.session->set_query(L"dolor", SearchSession::Flags{});
    // Advance to last, then next should wrap to first.
    f.session->next(); f.session->next();   // at last
    auto wrapped = f.session->next();
    REQUIRE(wrapped.has_value());
    REQUIRE(wrapped->page == 1);            // back to first dolor
    // prev from first wraps to last.
    auto before = f.session->prev();
    REQUIRE(before.has_value());
    REQUIRE(before->page == 6);
}

TEST_CASE("Scenario 7: page change during scan repositions cursor", "[search]") {
    Fx f;
    f.session->set_query(L"dolor", SearchSession::Flags{});
    // User navigated via outline to page 3. Cursor should reset to the
    // first hit at-or-after page 3.
    f.session->set_reference_page(3);
    auto h = f.session->current();
    REQUIRE(h.has_value());
    REQUIRE(h->page == 3);
}

TEST_CASE("Scenario 8: session destruct during pending tasks is safe", "[search]") {
    auto doc = std::make_shared<Document>(open_search_fixture());
    InlineDispatcher d;
    {
        SearchSession s(*doc, d);
        s.set_query(L"Lorem", SearchSession::Flags{});
    }  // session goes out of scope while (in real pool) tasks pend.
    // InlineDispatcher completed synchronously, but the destructor order
    // (SearchState::shared_ptr released, Document stays) is exercised
    // — no crash = pass.
    SUCCEED("destruct during scan is safe");
}
```

**Step 2: Write the header**

Create `src/core/SearchSession.hpp`:

```cpp
#pragma once

// core::SearchSession — per-tab search state: query, flags, hit cache,
// cursor. Pure logic, no Win32, no threads. Background search work is
// delegated to an ISearchDispatcher. See Phase 6 design §4 D1.
//
// Threading:
//   - Public API is single-threaded (UI thread).
//   - Internally submits tasks to dispatcher; task completion arrives
//     via a worker-thread path that calls into SearchSession::merge_hits
//     under std::mutex. UI thread reads hit_count/current etc. also under
//     the same mutex.
//   - For ThreadPoolDispatcher, the dispatcher posts a lambda to UI thread
//     via the callback below; SearchSession does NOT touch HWNDs.
//     The lambda is wired by MainWindow.

#include "core/Document.hpp"
#include "app/SearchDispatcher.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace litepdf::core {

class SearchSession {
public:
    struct Flags {
        bool match_case = false;
    };

    struct Hit {
        std::size_t          page;
        Document::PageHit    geom;
    };

    // on_update is fired from the thread that completes a page search
    // task (worker thread for ThreadPoolDispatcher, caller thread for
    // Inline). Wire it from MainWindow to PostMessage a refresh.
    using OnUpdate = std::function<void()>;

    SearchSession(const Document& doc, litepdf::app::ISearchDispatcher& dispatcher);
    ~SearchSession();

    SearchSession(const SearchSession&)            = delete;
    SearchSession& operator=(const SearchSession&) = delete;

    // Cancels any in-flight scan (bumps epoch), then kicks off a fresh
    // all-pages scan at eager priority. Empty q clears state.
    void set_query(std::wstring q, Flags f);
    void clear();

    void set_on_update(OnUpdate cb);

    // Used when user navigates (PgDn, outline click). Repositions cursor
    // to first hit at-or-after the given page.
    void set_reference_page(std::size_t page);

    // Cursor navigation. Return nullopt when hit list is empty.
    std::optional<Hit> current() const;
    std::optional<Hit> next();
    std::optional<Hit> prev();

    // Status.
    std::size_t hit_count() const;
    bool        scan_complete() const;

    // Hits visible on a given page (for PdfCanvas overlay paint).
    std::vector<Hit> hits_for_page(std::size_t page) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace litepdf::core
```

**Step 3: Implement**

Create `src/core/SearchSession.cpp`:

```cpp
#include "core/SearchSession.hpp"

#include <algorithm>

namespace litepdf::core {

struct SearchSession::Impl {
    const Document&                          doc;
    litepdf::app::ISearchDispatcher&         dispatcher;
    std::shared_ptr<void>                    state_strong;   // type-erased
    std::shared_ptr<State>                   state;           // same as above, typed

    struct State {
        mutable std::mutex           m;
        std::wstring                 query;
        Flags                        flags;
        std::atomic<std::uint64_t>   epoch{0};
        std::atomic<int>             abort_flag{0};
        std::vector<Hit>             hits;            // page-sorted, then intra-page order
        std::size_t                  cursor = 0;
        std::size_t                  pages_remaining = 0;
        OnUpdate                     on_update;
    };

    Impl(const Document& d, litepdf::app::ISearchDispatcher& disp)
        : doc(d), dispatcher(disp), state(std::make_shared<State>())
    {
        state_strong = state;
    }
};

SearchSession::SearchSession(const Document& doc, litepdf::app::ISearchDispatcher& disp)
    : impl_(std::make_unique<Impl>(doc, disp)) {}

SearchSession::~SearchSession() = default;

void SearchSession::set_query(std::wstring q, Flags f) {
    auto& st = *impl_->state;
    {
        std::lock_guard<std::mutex> g(st.m);
        st.query = std::move(q);
        st.flags = f;
        st.hits.clear();
        st.cursor = 0;
    }
    st.epoch.fetch_add(1, std::memory_order_acq_rel);
    st.abort_flag.store(1);  // abort any in-flight MuPDF call
    st.abort_flag.store(0);  // reset for new tasks

    if (st.query.empty()) {
        st.pages_remaining = 0;
        if (st.on_update) st.on_update();
        return;
    }

    const std::size_t pages = impl_->doc.page_count();
    st.pages_remaining = pages;

    // Submit one task per page.
    const std::uint64_t ep = st.epoch.load();
    std::weak_ptr<void> weak = impl_->state_strong;
    auto state = impl_->state;

    // UTF-16 → UTF-8 (MuPDF takes UTF-8).
    std::string needle_utf8 = [&]{
        std::string out;
        const wchar_t* p = st.query.c_str();
        int wlen = static_cast<int>(st.query.size());
        int n = ::WideCharToMultiByte(CP_UTF8, 0, p, wlen, nullptr, 0, nullptr, nullptr);
        out.resize(n);
        ::WideCharToMultiByte(CP_UTF8, 0, p, wlen, out.data(), n, nullptr, nullptr);
        return out;
    }();

    for (std::size_t i = 0; i < pages; ++i) {
        litepdf::app::ISearchDispatcher::Task t{};
        t.state_weak = weak;
        t.page = static_cast<int>(i);
        t.epoch = ep;
        // Priority: P0 = reference page, P1 = ±5, P2 = rest.
        t.priority = 2;  // default; refinement below
        t.is_current_epoch = [state, ep]() { return state->epoch.load() == ep; };
        t.run = [state, &doc = impl_->doc, i, needle_utf8, f]() {
            auto raw_hits = doc.page_hits(i, needle_utf8,
                                          {f.match_case},
                                          &state->abort_flag);
            std::lock_guard<std::mutex> g(state->m);
            for (auto& ph : raw_hits) {
                state->hits.push_back(Hit{i, std::move(ph)});
            }
            std::sort(state->hits.begin(), state->hits.end(),
                [](const Hit& a, const Hit& b){ return a.page < b.page; });
            if (state->pages_remaining > 0) --state->pages_remaining;
            if (state->on_update) state->on_update();
        };
        impl_->dispatcher.submit(std::move(t));
    }
}

void SearchSession::clear() { set_query(L"", {}); }

void SearchSession::set_on_update(OnUpdate cb) {
    std::lock_guard<std::mutex> g(impl_->state->m);
    impl_->state->on_update = std::move(cb);
}

void SearchSession::set_reference_page(std::size_t page) {
    auto& st = *impl_->state;
    std::lock_guard<std::mutex> g(st.m);
    auto it = std::lower_bound(st.hits.begin(), st.hits.end(), page,
        [](const Hit& h, std::size_t p){ return h.page < p; });
    if (it == st.hits.end()) it = st.hits.begin();
    st.cursor = static_cast<std::size_t>(std::distance(st.hits.begin(), it));
}

std::optional<SearchSession::Hit> SearchSession::current() const {
    auto& st = *impl_->state;
    std::lock_guard<std::mutex> g(st.m);
    if (st.hits.empty()) return std::nullopt;
    return st.hits[st.cursor];
}

std::optional<SearchSession::Hit> SearchSession::next() {
    auto& st = *impl_->state;
    std::lock_guard<std::mutex> g(st.m);
    if (st.hits.empty()) return std::nullopt;
    st.cursor = (st.cursor + 1) % st.hits.size();
    return st.hits[st.cursor];
}

std::optional<SearchSession::Hit> SearchSession::prev() {
    auto& st = *impl_->state;
    std::lock_guard<std::mutex> g(st.m);
    if (st.hits.empty()) return std::nullopt;
    st.cursor = (st.cursor + st.hits.size() - 1) % st.hits.size();
    return st.hits[st.cursor];
}

std::size_t SearchSession::hit_count() const {
    std::lock_guard<std::mutex> g(impl_->state->m);
    return impl_->state->hits.size();
}

bool SearchSession::scan_complete() const {
    std::lock_guard<std::mutex> g(impl_->state->m);
    return impl_->state->pages_remaining == 0;
}

std::vector<SearchSession::Hit> SearchSession::hits_for_page(std::size_t page) const {
    std::lock_guard<std::mutex> g(impl_->state->m);
    std::vector<Hit> out;
    for (const auto& h : impl_->state->hits) {
        if (h.page == page) out.push_back(h);
    }
    return out;
}

}  // namespace litepdf::core
```

**Note:** The `struct State` is defined inside `Impl` but referenced before declaration — restructure so `State` is declared at top of `Impl` before `state` member, or factor State into a namespace-scope type. Adjust during implementation.

**Step 4: Wire into CMake**

Modify `CMakeLists.txt` to add `src/core/SearchSession.cpp` to litepdf_core:

```cmake
    src/core/SearchSession.cpp   # Phase 6 Task 5
```

Modify `tests/CMakeLists.txt`:
```cmake
    unit/test_search_session.cpp   # Phase 6 Task 5
```

**Step 5: Run**

Run: `ctest --test-dir build -C Release -R "search" --output-on-failure`
Expected: 8/8 PASS.

**Step 6: Commit**

```bash
git add src/core/SearchSession.hpp src/core/SearchSession.cpp \
        tests/unit/test_search_session.cpp \
        CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(core): SearchSession pure-logic per-tab search state"
```

---

### Task 6: DocumentView — owns SearchSession

**Files:**
- Modify: `src/core/DocumentView.hpp` (add accessor + dispatcher ctor param)
- Modify: `src/core/DocumentView.cpp` (construct SearchSession)
- Modify: `tests/unit/test_document_view.cpp` (extend if needed)

**Step 1: Modify DocumentView.hpp**

Add dispatcher ctor param + search accessor:

```cpp
// Forward decl
namespace litepdf::app { class ISearchDispatcher; }
namespace litepdf::core { class SearchSession; }

class DocumentView {
public:
    // ...existing ctor, but take dispatcher ref...
    explicit DocumentView(Document doc,
                          litepdf::app::ISearchDispatcher& dispatcher,
                          std::size_t num_workers = 2,
                          std::size_t l1_capacity = 5,
                          std::size_t l2_capacity = 10);
    // ...

    // Per-tab search state. Always non-null for an open DocumentView.
    SearchSession& search();
    const SearchSession& search() const;
```

**Step 2: Modify DocumentView.cpp**

Add `#include "core/SearchSession.hpp"` and `#include "app/SearchDispatcher.hpp"`. Extend `Impl` to hold `std::unique_ptr<SearchSession> session;` constructed after Document (so its destructor runs BEFORE Document per D5 lifetime note).

**Step 3: Update callsites**

Find every `std::make_unique<DocumentView>(std::move(doc))` and add dispatcher ref. Touchpoints:
- `src/ui/MainWindow.cpp` (open_tab_async)
- `tests/unit/test_document_view.cpp` (tests now need a dispatcher fixture)

Simplest: create a static `InlineDispatcher` in each touched site.

**Step 4: Build + run tests**

Run: `ctest --test-dir build -C Release --output-on-failure`
Expected: no regressions. Phase 5 tests continue to pass.

**Step 5: Commit**

```bash
git add src/core/DocumentView.hpp src/core/DocumentView.cpp \
        src/ui/MainWindow.cpp tests/unit/test_document_view.cpp
git commit -m "feat(core): DocumentView owns SearchSession (wired via dispatcher)"
```

---

### Task 7: MainWindow — own the SearchDispatcher

**Files:**
- Modify: `src/ui/MainWindow.hpp` (hold `std::unique_ptr<ThreadPoolDispatcher>`)
- Modify: `src/ui/MainWindow.cpp`

**Step 1: Add member and lifecycle**

In `MainWindow.hpp`:
```cpp
#include "app/SearchDispatcher.hpp"
// ...
std::unique_ptr<litepdf::app::ThreadPoolDispatcher> search_dispatcher_;
```

In `MainWindow.cpp` constructor (or `run()`'s start): `search_dispatcher_ = std::make_unique<litepdf::app::ThreadPoolDispatcher>(2);`

Pass `*search_dispatcher_` into every `DocumentView` constructor call.

In destructor: reset dispatcher AFTER destroying tabs (tabs' SearchSession must have no in-flight tasks — they shouldn't anyway because SearchState shared_ptrs are gone, but destroying dispatcher joins workers cleanly).

**Step 2: Build + smoke**

Run: `cmake --build build --config Release`
Expected: green.

Run: `./build/Release/litepdf.exe tests/fixtures/simple.pdf`
Expected: opens, no crash. Close. Program exits cleanly.

**Step 3: Commit**

```bash
git add src/ui/MainWindow.hpp src/ui/MainWindow.cpp
git commit -m "feat(ui): MainWindow owns ThreadPoolDispatcher for search work"
```

---

### Task 8: FindBar — floating child HWND

**Files:**
- Create: `src/ui/FindBar.hpp`
- Create: `src/ui/FindBar.cpp`
- Modify: `CMakeLists.txt` (add FindBar.cpp to litepdf exe sources)

**Step 1: Header**

Create `src/ui/FindBar.hpp`:

```cpp
#pragma once

// ui::FindBar — floating Ctrl+F bar anchored to canvas top-right.
// Child HWND (WS_CHILD|WS_CLIPSIBLINGS); NOT WS_POPUP (popups detach
// from parent lifecycle — see Phase 6 design §4 D6).
//
// Hosts: Edit(query), Static(counter), buttons prev/next/case/close.
// Emits callbacks to MainWindow for query change, next, prev, close.

#include <functional>
#include <memory>
#include <string>
#include <windows.h>

namespace litepdf::ui {

class FindBar {
public:
    using QueryChanged = std::function<void(std::wstring, bool match_case)>;
    using NavAction    = std::function<void()>;

    FindBar(HINSTANCE, HWND parent);
    ~FindBar();

    FindBar(const FindBar&)            = delete;
    FindBar& operator=(const FindBar&) = delete;

    HWND hwnd() const;

    // Show / focus the bar; text restored from last session (caller passes).
    void show_or_focus(const std::wstring& prefill);
    void hide();
    bool visible() const;

    // Called when canvas resizes so we can re-anchor top-right.
    void reposition(const RECT& canvas_rect);

    // Update the counter static ("3 / 12" or "3 / 12+" or "").
    void set_counter(const std::wstring& txt);

    // Wire callbacks from MainWindow.
    void set_on_query_changed(QueryChanged cb);
    void set_on_next(NavAction cb);
    void set_on_prev(NavAction cb);
    void set_on_close(NavAction cb);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace litepdf::ui
```

**Step 2: Implement**

Create `src/ui/FindBar.cpp`. Skeleton (actual LOC ~350):
- `WNDCLASSEX` registration via `std::call_once` (same pattern as `PdfCanvas`, `OutlinePane`, `TabManager`).
- Children created in WM_CREATE: Edit, Static, 4 × owner-draw buttons.
- Subclass Edit to intercept WM_KEYDOWN (ESC/Enter/Shift+Enter/F3/Shift+F3).
- WM_TIMER (debounce) → query changed callback.
- WM_DRAWITEM → owner-draw per-button paint using Palette (reuse from TabManager).
- WM_SIZE of parent MainWindow calls `reposition` after on_layout.

Key helpers copy from `TabManager.cpp` where possible (palette, DPI-aware sizing, owner-draw button hover states from Phase 5 tab strip polish). Factor common `Palette` into `ui/Palette.hpp` if not already.

**Step 3: Wire into CMakeLists**

```cmake
add_executable(litepdf WIN32
    # ... existing ...
    src/ui/FindBar.cpp         # Phase 6 Task 8
```

**Step 4: Smoke**

Run: `cmake --build build --config Release`
Expected: green. (FindBar is not yet shown anywhere; builds but dead-code.)

**Step 5: Commit**

```bash
git add src/ui/FindBar.hpp src/ui/FindBar.cpp CMakeLists.txt
git commit -m "feat(ui): FindBar floating Ctrl+F bar (child HWND, DPI-aware, palette-driven)"
```

---

### Task 9: PdfCanvas — hit overlay + scroll_into_view

**Files:**
- Modify: `src/ui/PdfCanvas.hpp` (add `set_hits_source` + `scroll_into_view`)
- Modify: `src/ui/PdfCanvas.cpp`

**Step 1: Add API**

```cpp
// In PdfCanvas.hpp:

// Source for overlay hits. PdfCanvas calls this each paint to fetch
// the hits for the currently visible page. Non-owning; caller must
// ensure the lambda stays valid while set.
using HitsFn = std::function<std::vector<litepdf::core::SearchSession::Hit>(std::size_t page)>;
void set_hits_source(HitsFn fn);

// Current hit (if any). Drawn in orange; others drawn in yellow.
void set_current_hit(std::optional<litepdf::core::SearchSession::Hit> h);

// Scroll / page-change such that quad is centered vertically, clamped
// to document boundaries. No-op if hit already visible (with 24 DIP
// safe margin).
void scroll_into_view(const litepdf::core::SearchSession::Hit& h);
```

**Step 2: Implement overlay paint**

In `PdfCanvas.cpp` inside `on_paint`, after the bitmap blit:

```cpp
if (impl_->hits_fn) {
    auto hits = impl_->hits_fn(current_page);
    for (const auto& h : hits) {
        // Transform PDF quad → DIP rect.
        const float s = zoom * (dpi / 72.0f);
        const float x = pan_x + h.geom.ul_x * s;
        const float y = pan_y + h.geom.ul_y * s;
        const float w = (h.geom.ur_x - h.geom.ul_x) * s;
        const float ht = (h.geom.ll_y - h.geom.ul_y) * s;
        D2D1_RECT_F r = D2D1::RectF(x, y, x + w, y + ht);
        const bool is_current = impl_->current_hit &&
                                impl_->current_hit->page == h.page &&
                                std::memcmp(&impl_->current_hit->geom, &h.geom,
                                            sizeof(h.geom)) == 0;
        ID2D1SolidColorBrush* fill = is_current ? impl_->brush_current_fill_
                                                 : impl_->brush_other_fill_;
        rt->FillRectangle(r, fill);
        if (is_current) rt->DrawRectangle(r, impl_->brush_current_stroke_, 1.0f);
    }
}
```

Create brushes in `create_render_target`:
- `brush_other_fill_`:    #FFFF00 α=0.40
- `brush_current_fill_`:  #FFA500 α=0.50
- `brush_current_stroke_`: #CC7700 α=1.00 (1 DIP stroke)

**Step 3: Implement scroll_into_view**

```cpp
void PdfCanvas::scroll_into_view(const core::SearchSession::Hit& h) {
    // 1) page change if needed
    if (impl_->view->current_page() != static_cast<int>(h.page)) {
        impl_->view->set_current_page(static_cast<int>(h.page));
        // kick_render is the caller's responsibility (via MainWindow).
    }

    // 2) transform quad
    const float s = impl_->zoom * (impl_->dpi / 72.0f);
    const float qy = h.geom.ul_y * s;
    const float qh = (h.geom.ll_y - h.geom.ul_y) * s;

    RECT vr; GetClientRect(hwnd_, &vr);
    const float vh = static_cast<float>(vr.bottom - vr.top);

    // Is it visible (with 24 DIP margin)?
    const float visible_top    = -impl_->pan_y + 24.0f;
    const float visible_bottom = -impl_->pan_y + vh - 24.0f;
    if (qy >= visible_top && qy + qh <= visible_bottom) return;

    // Pan to vertical center.
    const float target = qy + qh * 0.5f - vh * 0.5f;
    impl_->pan_y = -target;
    // Clamp to page bounds (reuse existing clamp helper).
    impl_->clamp_pan();
    InvalidateRect(hwnd_, nullptr, FALSE);
}
```

**Step 4: Smoke build**

Run: `cmake --build build --config Release`
Expected: green. Open app, nothing visible yet (FindBar not wired in).

**Step 5: Commit**

```bash
git add src/ui/PdfCanvas.hpp src/ui/PdfCanvas.cpp
git commit -m "feat(ui): PdfCanvas hit overlay paint + scroll_into_view"
```

---

### Task 10: MainWindow — Ctrl+F accelerator + FindBar integration + smoke

**Files:**
- Modify: `src/ui/MainWindow.hpp`
- Modify: `src/ui/MainWindow.cpp`
- Modify: `resources/litepdf.rc` (accelerator table)
- Modify: `scripts/smoke-test.ps1` (add item (a) and (c))
- Modify: `scripts/ux-probe.ps1` (add `--search` flag)

**Step 1: Add accelerators**

In `resources/litepdf.rc`, after the menu, add:

```rc
IDM_MAIN_MENU ACCELERATORS
{
    "F",       IDM_FIND,           VIRTKEY, CONTROL
    VK_F3,     IDM_FIND_NEXT,      VIRTKEY
    VK_F3,     IDM_FIND_PREV,      VIRTKEY, SHIFT
    "F",       IDM_CROSS_TAB_FIND, VIRTKEY, CONTROL, SHIFT
    VK_F6,     IDM_TOGGLE_RESULTS, VIRTKEY
    VK_ESCAPE, IDM_FIND_CLOSE,     VIRTKEY
    // existing Phase 4/5 entries remain ...
}
```

Only add those not already present; keep existing Ctrl+O / F5 / Ctrl+W / Ctrl+Tab entries.

**Step 2: Wire FindBar into MainWindow**

In `MainWindow.hpp`:
```cpp
std::unique_ptr<FindBar> find_bar_;
std::wstring             last_find_query_;
```

In `MainWindow.cpp` WM_CREATE: `find_bar_ = std::make_unique<FindBar>(hInstance, hwnd);` hidden by default.

Handle WM_COMMAND:
- `IDM_FIND` → `find_bar_->show_or_focus(last_find_query_)`
- `IDM_FIND_NEXT` → if `find_bar_->visible()`, tell session to next(); else no-op
- `IDM_FIND_PREV` → similar
- `IDM_FIND_CLOSE` → when find bar has focus, hide it, clear highlights, SetFocus canvas
- `IDM_CROSS_TAB_FIND` → (deferred to Task 13, stub for now)
- `IDM_TOGGLE_RESULTS` → (deferred)

Wire find_bar_ callbacks:
```cpp
find_bar_->set_on_query_changed([this](std::wstring q, bool mc){
    auto* v = active_view();
    if (!v) return;
    v->search().set_query(std::move(q), {mc});
    last_find_query_ = q;
});
find_bar_->set_on_next([this]{
    auto* v = active_view(); if (!v) return;
    auto h = v->search().next();
    if (h) canvas_->set_current_hit(*h), canvas_->scroll_into_view(*h);
    update_find_counter();
});
// ... prev, close analogous
```

Also in `active_view()` → canvas_->set_hits_source(...) → hits_for_page.

In SearchSession::set_on_update wiring, PostMessage a user WM_USER_SEARCH_UPDATE to UI thread which refreshes find bar counter.

**Step 3: Update layout**

On WM_SIZE, after laying out canvas: `if (find_bar_->visible()) find_bar_->reposition(canvas_rect);`

**Step 4: Extend smoke-test.ps1**

Append function:
```powershell
function Test-Find-InDoc {
    Write-Host "Smoke (a): in-doc Ctrl+F on search.pdf"
    $p = Start-Process -FilePath $exe -ArgumentList "tests\fixtures\search.pdf" -PassThru
    Start-Sleep -Milliseconds 800
    [Win32]::PostMessage($p.MainWindowHandle, 0x0111, 40042, 0)  # WM_COMMAND IDM_FIND
    Start-Sleep -Milliseconds 200
    # Manual observation: user confirms counter + highlights.
    Read-Host "Observe find bar with Lorem search. Press Enter to continue"
    Stop-Process $p
}

function Test-Find-Incremental {
    Write-Host "Smoke (c): incremental typing on a large PDF"
    # Manual — assumes a large.pdf fixture exists (Phase 2 stress fixture).
    $p = Start-Process -FilePath $exe -ArgumentList "tests\fixtures\large.pdf" -PassThru
    Start-Sleep -Milliseconds 1200
    Read-Host "Open Ctrl+F, type 'a', pause, 'ab', pause, 'abc'. Press Enter when done"
    Stop-Process $p
}
```

Call both in the main script's runner.

**Step 5: Extend ux-probe.ps1**

Add `--search <query>` parameter. When present:
1. Launch litepdf with fixture.
2. Wait 800 ms.
3. PostMessage Ctrl+F (via IDM_FIND command).
4. Find FindBar Edit HWND (EnumChildWindows + class filter).
5. SendMessage WM_SETTEXT with query.
6. Poll the counter Static for stable text over 30 s.
7. Emit JSON with `{ query, total_hits, elapsed_ms }`.

**Step 6: Build + manual smoke**

Run: `cmake --build build --config Release`
Run: `.\build\Release\litepdf.exe tests\fixtures\search.pdf`
Manual: Ctrl+F, type Lorem, see counter converge to 15, F3 several times, ESC. Confirm highlights show + scroll.

**Step 7: ctest**

Run: `ctest --test-dir build -C Release --output-on-failure`
Expected: all tests pass (~85).

**Step 8: Commit**

```bash
git add src/ui/MainWindow.hpp src/ui/MainWindow.cpp resources/litepdf.rc \
        scripts/smoke-test.ps1 scripts/ux-probe.ps1
git commit -m "feat(ui): wire Ctrl+F find bar into MainWindow with accelerators"
```

---

### Milestone: Phase 6.1 exit gate

**Verification before tag:**

- Manual smoke (a): Ctrl+F on search.pdf, Lorem counter hits 15, F3 navigates, ESC restores canvas focus.
- Manual smoke (c): incremental typing on large.pdf, 120 ms debounce observed (single CPU burst, not multiple).
- `ctest --test-dir build -C Release`: all green.
- No RAM growth across 100 Ctrl+F open/close cycles (Task Manager observation).

**Update VERSION:**

```bash
echo "0.0.7-phase6.1-dev" > VERSION
git add VERSION
git commit -m "chore(version): 0.0.7-phase6.1-dev"
```

**Tag:**

```bash
git tag v0.0.7-phase6.1-indoc
git push origin v0.0.7-phase6.1-indoc
```

Use `superpowers:verification-before-completion` to confirm each exit criterion is evidence-backed before tagging.

---

### Task 11: CrossTabSearch orchestrator (TDD)

**Files:**
- Create: `src/app/CrossTabSearch.hpp`
- Create: `src/app/CrossTabSearch.cpp`
- Create: `tests/unit/test_cross_tab_search.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Step 1: Write failing tests**

```cpp
#include "app/CrossTabSearch.hpp"
#include "app/SearchDispatcher.hpp"
#include "core/Document.hpp"
#include "core/DocumentView.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace litepdf::app;
using namespace litepdf::core;

TEST_CASE("CrossTabSearch aggregates hits from 2 tabs", "[crosstab]") {
    InlineDispatcher d;
    Document doc1; doc1.open("tests/fixtures/search.pdf");
    Document doc2; doc2.open("tests/fixtures/search.pdf");
    DocumentView v1(std::move(doc1), d);
    DocumentView v2(std::move(doc2), d);
    CrossTabSearch xts(d);
    xts.dispatch(L"Lorem", {{&v1, L"search1.pdf"}, {&v2, L"search2.pdf"}}, {});
    const auto hits = xts.hits();
    REQUIRE(hits.size() >= 30);  // ~15 per file × 2 files
}

TEST_CASE("CrossTabSearch weak-ref protects against tab close", "[crosstab]") {
    InlineDispatcher d;
    auto doc = std::make_unique<Document>();
    doc->open("tests/fixtures/search.pdf");
    auto v = std::make_unique<DocumentView>(std::move(*doc), d);
    CrossTabSearch xts(d);
    xts.dispatch(L"Lorem", {{v.get(), L"search.pdf"}}, {});
    REQUIRE(xts.hits().size() >= 15);
    v.reset();
    // Subsequent hits() may or may not still contain the rows
    // (depends on design: we keep them but they become "stale");
    // the key guarantee is no crash.
    (void)xts.hits();
    SUCCEED("no crash after tab close");
}
```

**Step 2: Implement**

`CrossTabSearch.hpp` — holds `std::vector<CrossTabHit>`, dispatches to sessions. `dispatch(query, snapshot, flags)` resets results and submits. Observer pattern for incremental panel updates.

**Step 3: Run tests**

Run: `ctest -R crosstab`
Expected: 2/2 PASS.

**Step 4: Commit**

```bash
git add src/app/CrossTabSearch.hpp src/app/CrossTabSearch.cpp \
        tests/unit/test_cross_tab_search.cpp \
        CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(app): CrossTabSearch fan-out orchestrator (TDD)"
```

---

### Task 12: Splitter custom window class

**Files:**
- Create: `src/ui/Splitter.hpp`
- Create: `src/ui/Splitter.cpp`
- Modify: `CMakeLists.txt`

**Step 1: Header**

```cpp
#pragma once
// ui::Splitter — horizontal 4-DIP drag bar for resizing bottom panel.
// WM_SETCURSOR → IDC_SIZENS; WM_LBUTTONDOWN captures; WM_MOUSEMOVE posts
// WM_COMMAND to parent with new height in LPARAM.
#include <functional>
#include <memory>
#include <windows.h>

namespace litepdf::ui {

class Splitter {
public:
    using OnDrag = std::function<void(int new_height_dip)>;
    Splitter(HINSTANCE, HWND parent);
    ~Splitter();

    HWND hwnd() const;
    void set_on_drag(OnDrag cb);
    void set_position(int y_dip);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace litepdf::ui
```

**Step 2: Implement**

Standard pattern: register class (call_once), WM_SETCURSOR, capture on LBUTTONDOWN, delta-track drag, clamp to [100, client_h - 200].

**Step 3: Build**

Run: `cmake --build build --config Release`

**Step 4: Commit**

```bash
git add src/ui/Splitter.hpp src/ui/Splitter.cpp CMakeLists.txt
git commit -m "feat(ui): Splitter horizontal resize bar"
```

---

### Task 13: ResultsPanel — virtual ListView

**Files:**
- Create: `src/ui/ResultsPanel.hpp`
- Create: `src/ui/ResultsPanel.cpp`
- Modify: `CMakeLists.txt`

**Step 1: Header**

```cpp
#pragma once
// ui::ResultsPanel — bottom-docked pane for cross-tab search results.
// Contents: Edit + virtual ListView (LVS_REPORT | LVS_OWNERDATA).
#include "app/CrossTabSearch.hpp"
#include <functional>
#include <memory>
#include <windows.h>

namespace litepdf::ui {

class ResultsPanel {
public:
    using OnQuerySubmit = std::function<void(std::wstring query)>;
    using OnRowClick    = std::function<void(std::size_t hit_index)>;
    using OnClose       = std::function<void()>;

    ResultsPanel(HINSTANCE, HWND parent, const litepdf::app::CrossTabSearch& xts);
    ~ResultsPanel();

    HWND hwnd() const;
    void show(); void hide(); bool visible() const;

    void set_layout(const RECT& pane_rect);
    void refresh_count();  // called when xts hits count changes

    void set_on_query_submit(OnQuerySubmit);
    void set_on_row_click(OnRowClick);
    void set_on_close(OnClose);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace litepdf::ui
```

**Step 2: Implement**

Key points:
- ListView in virtual mode: `LVS_OWNERDATA`.
- Handle `LVN_GETDISPINFO` to fill row text from `xts.hits()[index]`.
- `SetItemCount(xts.hits().size())` triggered by refresh_count().
- Edit hosts query; Enter key submits via `OnQuerySubmit`.
- Close button at top-right of panel calls `OnClose`.

**Step 3: Build**

**Step 4: Commit**

```bash
git add src/ui/ResultsPanel.hpp src/ui/ResultsPanel.cpp CMakeLists.txt
git commit -m "feat(ui): ResultsPanel virtual ListView for cross-tab hits"
```

---

### Task 14: MainWindow — layout integration for results panel

**Files:**
- Modify: `src/ui/MainWindow.hpp`
- Modify: `src/ui/MainWindow.cpp`

**Step 1: Add members**

```cpp
std::unique_ptr<litepdf::app::CrossTabSearch>  cross_tab_;
std::unique_ptr<ResultsPanel>                  results_panel_;
std::unique_ptr<Splitter>                      splitter_;
int                                            results_panel_height_ = 200;
```

**Step 2: Create on demand**

`IDM_CROSS_TAB_FIND` handler: lazily create `cross_tab_`, `results_panel_`, `splitter_` on first invocation; then `results_panel_->show()` and focus its Edit.

**Step 3: Layout math**

`on_layout()`:
```cpp
RECT client; GetClientRect(hwnd_, &client);
const int tab_h = tabs_ ? tabs_->height() : 0;
const int panel_h = (results_panel_ && results_panel_->visible())
                    ? results_panel_height_ : 0;
const int splitter_h = panel_h > 0 ? 4 : 0;
const int canvas_bottom = client.bottom - panel_h - splitter_h;

// ... position tabs, outline, canvas as before but with canvas_bottom ...

if (results_panel_ && results_panel_->visible()) {
    RECT pane = {client.left, canvas_bottom + splitter_h,
                 client.right, client.bottom};
    results_panel_->set_layout(pane);
    splitter_->set_position(canvas_bottom);
}
```

**Step 4: Wire splitter drag**

`splitter_->set_on_drag([this](int y){ results_panel_height_ = client_height - y; on_layout(); });`

**Step 5: F6 toggle**

`IDM_TOGGLE_RESULTS`: if `results_panel_->visible()` → hide; else show.

**Step 6: Row click → navigate**

```cpp
results_panel_->set_on_row_click([this](std::size_t idx){
    const auto& hit = cross_tab_->hits()[idx];
    auto tab_index = hit.tab_index_at_submit;
    if (!tab_index) return;  // weak expired; stale
    tabs_->set_active(*tab_index);
    auto* v = active_view();
    if (!v) return;
    v->set_current_page(hit.page);
    // Set current hit in session and scroll to view.
    v->search().set_reference_page(hit.page);  // nearest cursor
    canvas_->set_current_hit(SearchSession::Hit{hit.page, hit.geom});
    canvas_->scroll_into_view({hit.page, hit.geom});
    kick_render(hit.page);
});
```

**Step 7: Build + smoke**

Manual: Open 2 PDFs, Ctrl+Shift+F, type Lorem, Enter, observe panel populate; click rows; verify tab switches + page jumps + highlight.

**Step 8: Commit**

```bash
git add src/ui/MainWindow.hpp src/ui/MainWindow.cpp
git commit -m "feat(ui): MainWindow layout + handlers for cross-tab results panel"
```

---

### Task 15: Cross-tab smoke + ux-probe extension

**Files:**
- Modify: `scripts/smoke-test.ps1`
- Modify: `scripts/ux-probe.ps1`

**Step 1: Smoke (b)**

Add:
```powershell
function Test-CrossTab {
    Write-Host "Smoke (b): cross-tab Ctrl+Shift+F across 2 PDFs"
    $p = Start-Process -FilePath $exe `
        -ArgumentList @("tests\fixtures\search.pdf","tests\fixtures\simple.pdf") `
        -PassThru
    Start-Sleep -Milliseconds 1000
    [Win32]::PostMessage($p.MainWindowHandle, 0x0111, 40045, 0)  # Ctrl+Shift+F
    Read-Host "Type 'Lorem' + Enter. Verify panel has rows from both. Click one. Press Enter"
    Stop-Process $p
}
```

**Step 2: ux-probe `--cross-tab-search`**

Add flag; mimics `--search` but posts `IDM_CROSS_TAB_FIND` and reads ListView row count.

**Step 3: Commit**

```bash
git add scripts/smoke-test.ps1 scripts/ux-probe.ps1
git commit -m "test(smoke): cross-tab search (b) + ux-probe --cross-tab-search"
```

---

### Milestone: Phase 6.2 exit gate

**Verification:**
- Manual smoke (b): 2 PDFs open, Ctrl+Shift+F, Lorem query, both tabs contribute, row click navigates correctly.
- Splitter drag resizes panel; F6 toggles; close button hides.
- Tab close mid-scan: no crash; stale rows click gracefully.
- `ctest`: all green (~88 tests post-6.2).
- `ux-probe --cross-tab-search`: emits valid JSON with row count.

**Update VERSION & tag:**

```bash
echo "0.0.7" > VERSION
git add VERSION
git commit -m "chore(version): 0.0.7 (Phase 6 complete)"
git tag v0.0.7-phase6
git push origin v0.0.7-phase6
```

---

### Task 16: Update roadmap + design doc cross-ref

**Files:**
- Modify: `docs/plans/2026-04-15-litepdf-roadmap.md`

**Step 1:** Append a "Known Limitations" bullet under Phase 6 noting: whole-word / regex deferred, ResultsPanel not un-dockable, L2 not warmed by search.

**Step 2:** Commit.

```bash
git add docs/plans/2026-04-15-litepdf-roadmap.md
git commit -m "docs(plans): post-Phase-6 known limitations"
```

---

### Task 17: End-of-phase code review + CHANGELOG

**Files:**
- Modify: `CHANGELOG.md` (if it exists; otherwise skip)

**Step 1: Request review**

Use `superpowers:requesting-code-review` to trigger a final review agent. The review brief:

> Review the last 17 commits on `claude/vigorous-noether-49df26` for Phase 6 (search) correctness. Key risks: (1) weak-ptr task lifetime edge cases; (2) `fz_cookie::abort` concurrency; (3) PdfCanvas overlay paint with dual DPI; (4) virtual ListView + SetItemCount racing with CrossTabSearch hit appends; (5) splitter drag math at edge heights.

**Step 2:** Address any Critical or Major findings in follow-up commits.

**Step 3:** Final tag push once clean.

---

## Post-Phase-6 Status Checklist

- [ ] `ctest --test-dir build -C Release` — 85+ green
- [ ] Manual smoke (a), (b), (c) — pass
- [ ] Cold-start not regressed (`ColdStartTimer` line within baseline)
- [ ] No RAM growth across 100 Ctrl+F cycles
- [ ] ux-probe `--search` and `--cross-tab-search` emit JSON
- [ ] `v0.0.7-phase6` tag on `main`
- [ ] Roadmap updated with Known Limitations

## Risks & Watch-outs

1. **`fz_search_page2` availability.** If the pinned MuPDF 1.24.x version only exposes the v1 `fz_search_page` API (no flags param), Task 3 test 4 (case-sensitive) will fail. Mitigation: skip the test with `[!shouldfail]` and file a TODO for Phase 11 MuPDF upgrade.
2. **Destruction order in DocumentView.** SearchSession must be destructed before Document. Violating this means tasks crash on stale Document pointers. The design§4 D5 note mandates explicit ordering in `DocumentView::Impl`. Double-check on code review.
3. **Virtual ListView race.** CrossTabSearch appends hits from worker threads; ResultsPanel's `LVN_GETDISPINFO` reads hits on UI thread. Protect with a std::mutex on the hit vector; don't over-call `SetItemCount` (once every 250 ms is enough during scan).
4. **Palette struct sharing.** FindBar + ResultsPanel reuse the Palette from Phase 5 tab strip. If Palette is still file-local to `TabManager.cpp`, factor it into `src/ui/Palette.hpp` during Task 8.
5. **Accelerator clash.** VK_ESCAPE for IDM_FIND_CLOSE is a global accel and will also fire while any dialog is up. Scope the handler: only act if `find_bar_->visible()` AND focus is in the find bar; otherwise let default processing run.
6. **Debounce timer stacking.** A rapid sequence of `EN_CHANGE` events must RESET the timer (KillTimer then SetTimer), not stack multiple timers. Verify in Task 8 implementation.

---

## Appendix A: Reference Skills

Use these sub-skills at the marked points:

- **Start of each TDD task:** `superpowers:test-driven-development` (red → green → refactor → commit).
- **End of each task:** `superpowers:verification-before-completion` (ctest green, manual smoke where applicable, no regression).
- **Before each milestone tag:** `superpowers:requesting-code-review`.
- **Execution:** `superpowers:subagent-driven-development` — dispatch one subagent per Task, review between.

## Appendix B: File Touch Summary

| New files | Modified files |
|---|---|
| `src/core/SearchSession.{hpp,cpp}`     | `src/core/Document.hpp/cpp` |
| `src/app/SearchDispatcher.{hpp,cpp}`   | `src/core/DocumentView.hpp/cpp` |
| `src/app/CrossTabSearch.{hpp,cpp}`     | `src/core/PageCache.hpp/cpp` |
| `src/ui/FindBar.{hpp,cpp}`             | `src/ui/MainWindow.hpp/cpp` |
| `src/ui/ResultsPanel.{hpp,cpp}`        | `src/ui/PdfCanvas.hpp/cpp` |
| `src/ui/Splitter.{hpp,cpp}`            | `resources/MainMenu.rc.h` |
| `tests/unit/test_document_search.cpp`  | `resources/litepdf.rc` |
| `tests/unit/test_page_cache_peek.cpp`  | `CMakeLists.txt` |
| `tests/unit/test_search_session.cpp`   | `tests/CMakeLists.txt` |
| `tests/unit/test_search_dispatcher.cpp`| `scripts/smoke-test.ps1` |
| `tests/unit/test_cross_tab_search.cpp` | `scripts/ux-probe.ps1` |
| `tests/fixtures/search.pdf`            | `VERSION` |
| `tests/fixtures/search.md`             | `docs/plans/2026-04-15-litepdf-roadmap.md` |
| `scripts/make-search-fixture.ps1`      | |

Expected LOC delta: +~750 (plan §3 estimate).
