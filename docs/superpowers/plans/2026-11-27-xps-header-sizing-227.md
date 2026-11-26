# XPS page sizes from the FixedPage root (#227) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Size every page of an XPS from the start of its FixedPage part, bitwise equal to
`fz_load_page` + `fz_bound_page` outside the spec's §3.6 deviations, so a 1,000-page XPS meets the
continuous-scroll page-sizing gate (≤ 100 ms cold and warm) and XPS joins the strip.

**Architecture:** A new `detail::XpsPageSizer` takes MuPDF's own page list from `xps_document`
(internal header `xps-imp.h`), reads each page's root start tag from a prefix of its zip entry and
hands the cut prefix to `fz_parse_xml`. Before `prepare_all` it opens entries through
`fz_open_archive_entry` (stream route); `prepare_all` binds litepdf's own central-directory parse
(`ZipReadAhead`) to MuPDF's entry list and opens entries by offset through null + flate filters
over MuPDF's file stream (direct route), after one `PrefetchVirtualMemory` read-ahead. `Document`
wires it in beside `CbzPageSizer`; configure hashes the ten MuPDF files whose rules it relies on.

**Tech Stack:** C++17 (`cxx_std_17`, `cmake/CompilerFlags.cmake:5`; `char8_t` off), MSVC v143,
Release /MT only, MuPDF 1.27.2 (static, submodule), Catch2 v3.5.4, Python 3 standard library
(fixture generator), PowerShell 5.1 (gate scripts, smoke).

**Spec:** `docs/superpowers/specs/2026-11-26-xps-header-sizing-227-design.md` (spec gate passed
2026-11-27, d052fe5; this plan's gate patched §3.4, §5.1, §5.2 and §6). Section numbers below
(§3.4 etc.) are that spec's.

**Plan gate** (not a task): Tier Full (src/core, CMake), routine — Opus slot-1, then
gpt-5.6-terra, gpt-6.1-terra, gpt-5.6-luna on one commit (`risk-tiered-review`). Before it,
`bash <task-brief> <this plan> N <scratch>` for N = 1..8 (`reference_plan_task_headings_numeric`).

## Global Constraints

- **Read the spec** (`docs/superpowers/specs/2026-11-26-xps-header-sizing-227-design.md`) for
  every section a task cites (§3.3 prefix parse, §3.4 direct route, §5.1 fixture table, §6 gate).
  A task's own text wins where they differ.
- Non-ASCII paths in tests are built from wide literals with `\u` escapes
  (`fs::path(L"...\u6e2c\u8a66...")`, as `tests/unit/test_cbz_page_sizer.cpp:248`), never `u8"..."`
  (with `char8_t` off it is a narrow string decoded with the ANSI code page; on this machine,
  code page 950, the path constructor throws). UTF-8 for MuPDF from `utf8_of(path)`
  (`tests/unit/chapter_test_kit.hpp:48`).
- A worktree build through the junction rebuilds the main checkout's MuPDF libs in place with the
  worktree's paths (`reference_litepdf_build_test_commands`, 2026-11-24). After the branch merges
  and the junction is removed, rebuild them from the main checkout as that memory says.

- Build and test **Release only**, cmake/ctest from BuildTools, never the `cmake`/`ctest` on PATH:
  `$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"`
  (`ctest.exe` beside it). Build: `& $cmake --build build --target litepdf_unit_tests --config Release`.
  Run tests **from the worktree root, from PowerShell**:
  `.\build\tests\Release\litepdf_unit_tests.exe "<name or [tag]>"`. Full suite serially (no `-j`):
  `& "<BuildTools>\ctest.exe" --test-dir build -C Release`.
- The worktree's `third_party\mupdf` is an empty directory until Task 1 junctions it to the main
  checkout's built submodule. While the junction exists, stage with `git add <paths>`
  (`git commit -a` fails). The junction is removed with `cmd /c rmdir third_party\mupdf` and the
  empty directory recreated (`New-Item -ItemType Directory third_party\mupdf`) before the
  worktree is removed — never a recursive delete through it.
- `fz_try` bodies hold no object with a destructor, throw no C++ exception, and are never left by
  `return`, `break` or `continue` (#179). Every `fz_catch` marks its error handled; expected-failure
  probes use `fz_convert_error(ctx, nullptr)` (#146). `fz_var` only a variable assigned inside the
  `fz_try` and read in its `fz_always`/`fz_catch` (#178).
- No MuPDF patch. `xps-imp.h` is included by `src/core/XpsPageSizer.cpp` only, inside
  `extern "C" { }` after `<mupdf/fitz.h>`.
- Constants (spec §3.3, §3.9): `kXpsReadStep = 512` bytes, `kXpsRootCap = 16 * 1024` bytes,
  `kXpsCutTries = 16`, `kXpsReadAheadSpan = 12 * 1024` bytes.
- Size arithmetic (§3.7): `PageSize{w * 72.0f / 96.0f - 0.0f, h * 72.0f / 96.0f - 0.0f}`, `w`/`h`
  the `int`s from `atoi`.
- `TEST_CASE` names are ASCII and start with the subsystem (`XpsPageSizer:`, `ZipReadAhead:`,
  `EffectiveLayout:`). New XPS cases carry `[core][xps]`; zip cases keep `[core][zip]`.
- Size equality in tests is bitwise: `std::memcmp` of the `float`s (a `CHECK(a == b)` would pass
  `-0.0f == 0.0f`).
- Fixtures: `tests/fixtures/xps-sizing.xps`, `tests/fixtures/xps-escaped-name.xps`. Gate files are
  never committed (§6).
- All artifacts in English. Commit messages end with
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

Inputs the spec implies that no §5 test names, most likely to bite first; each has its test in the
task named.

1. **A file under a CJK path** (a Traditional Chinese user's documents folder): the direct route
   needs the UTF-16 path for `read_zip_directory`; a lossy conversion would silently drop every
   page to the stream route and miss the gate. Test in Task 4.
2. **Closing a tab and opening another XPS in the same `Document`**: the old sizer (and its names)
   must not size the new file. Test in Task 5.
3. **Declined pages leave MuPDF's error state clean**: a page that falls back must not leave a
   pending error that the next MuPDF call reports or that `fz_drop_context` prints. Test in Task 3.
4. **A relative path in a directory with no UTF-8 form** (#182): `xps_path` stays empty, no sizer,
   every page loads as today and sizes correctly. Test in Task 5.
5. **A page index past the end**: `header_size` returns nullopt and `page_size` throws as it does
   today. Test in Task 5.

## File Structure

| File | Responsibility |
|---|---|
| `src/core/detail/ZipReadAhead.hpp`, `src/core/ZipReadAhead.cpp` (modify) | `ZipEntryRef` gains both sizes; `plan_read_ahead` gains a fixed-span overload |
| `scripts/generate-xps-sizing-fixture.py` (create) | writes both XPS fixtures and asserts their properties |
| `tests/fixtures/xps-sizing.xps`, `tests/fixtures/xps-escaped-name.xps` (create) | equality fixtures (§5.1) |
| `src/core/detail/XpsPageSizer.hpp`, `src/core/XpsPageSizer.cpp` (create) | the sizer: page list, prefix parse, stream and direct routes, read-ahead |
| `src/core/Document.cpp` (modify) | owns the sizer beside the CBZ one |
| `CMakeLists.txt` (modify) | new source, PRIVATE include of `third_party/mupdf/source/xps` |
| `cmake/ImportMuPDF.cmake` (modify) | SHA-256 guard of ten MuPDF files |
| `tests/unit/test_xps_page_sizer.cpp` (create), `tests/unit/test_zip_read_ahead.cpp`, `tests/unit/test_strip_helpers.cpp` (modify), `tests/CMakeLists.txt` (modify) | tests |
| `src/ui/detail/EffectiveLayout.hpp`, `scripts/smoke-test.ps1`, `docs/SMOKE-CHECKLIST.md`, `CHANGELOG.md`, `README.md`, both specs (modify) | XPS in the strip, docs and records |

## Task 1: Zip directory sizes and a fixed read-ahead span

**Files:**
- Modify: `src/core/detail/ZipReadAhead.hpp` (struct `ZipEntryRef`, `plan_read_ahead` declarations)
- Modify: `src/core/ZipReadAhead.cpp` (`parse_zip_directory`, `plan_read_ahead`)
- Test: `tests/unit/test_zip_read_ahead.cpp`

**Interfaces:**
- Produces:
  - `struct ZipEntryRef { std::string name; std::uint64_t local_header_offset = 0; std::uint32_t compressed_size = 0; std::uint32_t uncompressed_size = 0; };` — the central record's 32-bit fields at +20 and +24, as stored (`0xFFFFFFFF` kept as is; the caller decides).
  - `std::vector<FileRange> plan_read_ahead(const std::vector<std::uint64_t>& offsets, std::uint64_t file_size, std::size_t span);` — every range `span` bytes from its offset, clipped to the file, a range at or past the end dropped. The two-argument overload computes its clamp and calls this one.

- [ ] **Step 1: Set up the worktree build** (once; later tasks reuse it). From PowerShell at the worktree root: `Remove-Item third_party\mupdf` (alone in its own command; it fails safely if not empty); `cmd /c mklink /J third_party\mupdf C:\Users\User\projects\litepdf\third_party\mupdf`; copy `C:\Users\User\projects\litepdf\build\_deps\catch2-src` to `build\_deps\catch2-src`; configure `& $cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DFETCHCONTENT_FULLY_DISCONNECTED=ON "-DFETCHCONTENT_SOURCE_DIR_CATCH2=build\_deps\catch2-src"`; build `litepdf_unit_tests` (a first `MSB8066` on `mupdf_ext` is retried once with `--target mupdf_ext`). Expected: the existing suite passes serially; record the count from `ctest -N` in the task report.

- [ ] **Step 2: Write the failing tests** in `tests/unit/test_zip_read_ahead.cpp`:
  - `ZipReadAhead: parse_zip_directory returns both sizes` — a crafted directory of two records (reuse the file's existing record builder), record 0 with compressed 0x1234 / uncompressed 0x5678, record 1 with `0xFFFFFFFF` / `0xFFFFFFFF`; assert the four values exactly, and that the names and offsets are unchanged.
  - `ZipReadAhead: plan_read_ahead with a fixed span` — `plan_read_ahead({0, 100, 5000}, 6000, 4096)` gives `{0,4096}, {100,4096}, {5000,1000}`; `plan_read_ahead({6000}, 6000, 4096)` is empty; `plan_read_ahead({}, 10, 4096)` is empty.
  - Extend `ZipReadAhead: the directory matches MuPDF's entry list` with two entries of `tests/fixtures/cbz-sizing.cbz`, values read with Python's `zipfile` at plan time: `p12_deflated.jpg` compressed 472 / uncompressed 2833, `p01_jfif300.jpg` (stored) 4429 / 4429.

- [ ] **Step 3: Run** `.\build\tests\Release\litepdf_unit_tests.exe "[zip]"`. Expected: the build fails (no such members / overload).

- [ ] **Step 4: Implement** the two fields in `parse_zip_directory` (`le32(r + 20)`, `le32(r + 24)`) and the overload; the existing `plan_read_ahead` body becomes `return plan_read_ahead(offsets, file_size, std::clamp(...))` with its current clamp.

- [ ] **Step 5: Run** `"[zip]"` then `"[cbz]"`. Expected: all pass; the existing `plan_read_ahead spreads the budget` case unchanged.

- [ ] **Step 6: Commit** `git add src/core/detail/ZipReadAhead.hpp src/core/ZipReadAhead.cpp tests/unit/test_zip_read_ahead.cpp` — `feat(core): zip directory entry sizes and a fixed read-ahead span (#227)`.

## Task 2: XPS sizing fixtures

**Files:**
- Create: `scripts/generate-xps-sizing-fixture.py`, `tests/fixtures/xps-sizing.xps`, `tests/fixtures/xps-escaped-name.xps`
- Create: `tests/unit/test_xps_page_sizer.cpp` (first case only), modify `tests/CMakeLists.txt`

**Interfaces:**
- Produces: the two fixtures; in `test_xps_page_sizer.cpp` a file-local `OwnDocument` copied from
  `tests/unit/test_cbz_page_sizer.cpp:53-120` (warnings callback into `std::string warnings`,
  `throw_on_context()`, `page_count()`), with `sizer(std::size_t page_count)` returning
  `detail::XpsPageSizer::create(ctx, doc, path, page_count)` (added in Task 3, when the class
  exists); a file-local table `struct XpsRow { int page; bool direct; };` for the 15 pages below
  (`direct` true for `Direct`, false for `None`; before `prepare_all` a `Direct` row takes
  `Stream`); and helper `PageSize loaded_size(OwnDocument& own, int index, bool* threw)` computing
  `fz_load_page` + `fz_bound_page` (`x1 - x0`, `y1 - y0`) on the test's own context, `*threw` set
  when MuPDF throws.

The pages of `xps-sizing.xps` (spec §5.1, with sizes fixed here; `W × H` are the root's
`Width`/`Height` attribute values):

| page | FixedPage part | W × H | route after `prepare_all` |
|---|---|---|---|
| p01 | plain, deflated; local header gets a 28-byte 0xA220 extra field the central record lacks | 816 × 1056 | Direct |
| p02 | `<?xml version="1.0" encoding="utf-8"?>` then `<!-- c -->` before the root; deflated; local CRC/sizes zeroed | 612 × 792 | Direct (third cut) |
| p03 | `Width="1000.9" Height="  700"` | 1000 × 700 after `atoi` | Direct |
| p04 | `Width="&#56;16" Height="500"` | 816 × 500 | Direct |
| p05 | stored (method 0); local CRC/sizes zeroed | 500 × 400 | Direct |
| p06 | `<mc:AlternateContent xmlns:mc="http://schemas.openxmlformats.org/markup-compatibility/2006"><mc:Choice Requires="xps"><FixedPage Width="640" Height="480" ...>` (`Requires="xps"` is the one MuPDF takes, xps-common.c:43-48, so it loads) | 640 × 480 | None |
| p07 | UTF-16LE with BOM, deflated | 700 × 900 | None |
| p08 | `xml:lang="中文"` (UTF-8) before `Width` | 720 × 960 | None |
| p09 | no `Height` (MuPDF's load throws) | 800 × – | None |
| p10 | a 20 KB attribute value (no `>`) before `Width` | 610 × 810 | None |
| p11 | referenced as `/Documents/1/Pages/P11.FPAGE`, stored as `Documents/1/Pages/p11.fpage` | 620 × 820 | Direct |
| p12 | written as `Documents/1/Pages/p12.fpage/[0].piece` (root tag) + `.../[1].last.piece` | 630 × 830 | None |
| p13 | `p13.fpage` (300 × 300) written before `P13.fpage` (400 × 400), referenced as `p13.fpage` | 300 × 300 | Direct |
| p14 | about 64 KB of random-word `Glyphs` (seed 227) after a short root | 640 × 840 | Direct |
| p15 | `Name="a>b"` before `Width` (the first cut ends inside the value and throws) | 650 × 850 | Direct |

- [ ] **Step 1: Write the generator.** Python standard library only (`zipfile`, `struct`, `zlib`,
  `random`). Docstring as `generate-cbz-sizing-fixture.py`'s: what each page covers, that the
  deflated bytes depend on the zlib version (name the one used: `zlib.ZLIB_RUNTIME_VERSION`), and
  that there is no `--check` mode. Fixed `date_time=(2026, 11, 27, 0, 0, 0)`, `create_system=0`,
  `external_attr=0`. Parts, in this zip order: `_rels/.rels` (Relationship `Type="http://schemas.microsoft.com/xps/2005/06/fixedrepresentation"` `Target="/FixedDocumentSequence.fdseq"`), `FixedDocumentSequence.fdseq` (one `DocumentReference Source="/Documents/1/FixedDocument.fdoc"`), `Documents/1/FixedDocument.fdoc` (one `PageContent Source=` per page, in page order), then the page parts p01-p15 exactly as the table above. Every page part except p05 is deflated.
  After writing: rewrite p01's local header in place to carry a 28-byte extra field (`20 A2 18 00` + 24 zero bytes; type 0xA220, size 24), shifting every later local-header offset in the central directory and the end record's directory offset by 28; zero the local CRC-32, compressed and uncompressed size fields (local header +14, +18, +22) of p02 and p05. Then assert, by re-reading the file with `zipfile` and `struct`: every entry reads back (CRC checked by `zipfile`), p01's local extra length is 28 and its central extra length 0, p02's and p05's local sizes are 0 and their central sizes are not.
  `xps-escaped-name.xps`: three pages, `page1.fpage` 300 × 200, `#U0070age2.fpage` raw name (ASCII, so no UTF-8 flag; MuPDF renames it `page2.fpage`) 400 × 300, `page3.fpage` 500 × 400; the FixedDocument references `page1/2/3.fpage`. Assert the raw name and the absence of flag 0x800 on it.

- [ ] **Step 2: Run** `python -I scripts/generate-xps-sizing-fixture.py`. Expected: both files written, every assertion passes, a one-line summary per file.

- [ ] **Step 3: Write the test** `XpsPageSizer: the fixtures load as the table says` (`[core][xps]`): with the file-local `OwnDocument` on each fixture, page counts 15 and 3; for `xps-sizing.xps` every page but p09 loads and its size equals the table's `W × H` × 72 / 96 (14 hard-coded sizes), p09 throws; for `xps-escaped-name.xps` the three sizes. Register the file in `tests/CMakeLists.txt` next to `unit/test_cbz_page_sizer.cpp` with a `# #227` comment.

- [ ] **Step 4: Run** `"[xps]"`. Expected: PASS (this test checks the fixtures, not new code).

- [ ] **Step 5: Commit** the generator, both fixtures, the test file and `tests/CMakeLists.txt` — `test: XPS page-sizing fixtures (#227)`.

## Task 3: XpsPageSizer — page list, prefix parse, stream route

**Files:**
- Create: `src/core/detail/XpsPageSizer.hpp`, `src/core/XpsPageSizer.cpp`
- Modify: `CMakeLists.txt` (`src/core/XpsPageSizer.cpp  # #227` after the `ZipReadAhead.cpp` line; `target_include_directories(litepdf_core PRIVATE "${CMAKE_SOURCE_DIR}/third_party/mupdf/source/xps")`)
- Test: `tests/unit/test_xps_page_sizer.cpp`

**Interfaces:**
- Consumes: Task 2's table and `loaded_size`.
- Produces (namespace `litepdf::core::detail`):
  - `class XpsPageSizer` with `enum class Route { None, Direct, Stream };`
  - `static std::unique_ptr<XpsPageSizer> create(fz_context* ctx, fz_document* doc, const char* utf8_path, std::size_t page_count) noexcept;`
  - `std::optional<PageSize> header_size(std::size_t index, Route* route = nullptr) noexcept;` — `*route` set to the route that produced the size, `None` when nullopt.
  - `bool prepare_all() noexcept;` — declared here, returns false and does nothing until Task 4.
  - `using PrefetchFn = bool (*)(const std::wstring&, const std::vector<FileRange>&) noexcept;` and `static void set_prefetch_for_test(PrefetchFn fn) noexcept;` (nullptr restores `prefetch_file_ranges`) — declared here, used in Task 4.

- [ ] **Step 1: Write the failing tests:**
  - Add `OwnDocument::sizer(std::size_t page_count)` (Task 2's Interfaces).
  - `XpsPageSizer: create applies only to an XPS with a matching page list` — non-null for both fixtures with the right `page_count`; null for `tests/fixtures/simple.pdf`; null for `xps-sizing.xps` with `page_count` 14 and 16.
  - `XpsPageSizer: before prepare_all every page takes the stream route or none` — for each table row: rows `None` give nullopt and `Route::None`; every other row gives `Route::Stream` and a size bitwise equal to `loaded_size`; p13 gives 300 × 300; the escaped fixture's three pages `Stream` and equal.
  - `XpsPageSizer: declined pages leave no MuPDF error behind` (Review Focus 3) — as `CbzPageSizer: declined pages leave no MuPDF error behind` (`test_cbz_page_sizer.cpp:262-281`): `header_size` on p15 (a throwing cut), p06, p07, p08, p10, p12, each followed by `own.throw_on_context()`; then `CHECK(own.warnings.find("UNHANDLED EXCEPTION!") == std::string::npos)`. A check of `fz_caught` at the end only would miss a residue that a later throw overwrites (error.c:335-339).

- [ ] **Step 2: Run** `"[xps]"`. Expected: build fails (no `XpsPageSizer`).

- [ ] **Step 3: Implement `create`** per §3.2: guard `doc->load_page == xps_load_page`; `zip` non-null and `strcmp(zip->format, "zip") == 0`; walk `first_page` copying `name`; count == `page_count`; store `ctx`, `doc`, `zip`, names, `utf8_path`. No file read.

- [ ] **Step 4: Implement the prefix parse** as a file-local `std::optional<std::pair<int, int>> read_root_size(fz_context*, fz_stream*)` per §3.3, with the Global Constraints' constants. Structure (#179): `read_root_size` itself holds no `fz_try`; its buffer is a `std::vector<unsigned char>` and it may `return` freely. Each MuPDF call it makes goes through a small file-local helper that wraps only that call in its own `fz_try` and reports by return value: `bool read_some(fz_context*, fz_stream*, unsigned char* dst, std::size_t n, std::size_t* got)` (false on a throw) and `enum class Parse { Error, NoRoot, NotSizable, Sized }; Parse parse_cut(fz_context*, const unsigned char* data, std::size_t n, int* w, int* h)` (creates the `fz_buffer` from a copy, parses, reads the root, drops buffer and tree in `fz_always`; a throw → `fz_convert_error(ctx, nullptr)`, `Parse::Error`). A `read_some` failure → nullopt. Algorithm:
  ```
  buffer = empty; tried = 0; next_scan = 0
  loop:
    if buffer.size >= kXpsRootCap: return nullopt
    got = fz_read(stm, step of min(kXpsReadStep, cap - size)); if got == 0: return nullopt
    if buffer starts FF FE or FE FF: return nullopt
    for i in [next_scan, buffer.size) where buffer[i] == '>':
      if ++tried > kXpsCutTries: return nullopt
      cut = buffer[0 .. i]                                  // inclusive
      if any byte of cut after an optional EF BB BF is >= 0x80: return nullopt
      switch parse_cut(cut):
        Error, NoRoot: continue                             // next '>'
        NotSizable:    return nullopt                       // root not FixedPage / no Width / no Height
        Sized:         return (w, h)                        // atoi of each
    next_scan = buffer.size
  ```

- [ ] **Step 5: Implement `header_size`'s stream route** per §3.5: index past the list → nullopt; open `fz_open_archive_entry(ctx, zip, name + (name[0] == '/'))`, `read_root_size`, drop the stream in `fz_always`, a throw → `fz_convert_error` and nullopt; a pair → §3.7's `PageSize`, `Route::Stream`.

- [ ] **Step 6: Run** `"[xps]"`. Expected: PASS.

- [ ] **Step 7: Mutation checks** (`mutation-test-hygiene`; restore after each, rebuild, confirm green): cut only at the first `>` → p02 must fail; `Parse::Error` returning nullopt instead of trying the next `>` → p15 must fail; drop `fz_convert_error` from `parse_cut`'s catch → the error-state test must fail; drop the ASCII rule → p08 must fail (it then gives a size where the test expects `None`); accept an `AlternateContent` root's inner FixedPage → p06 must fail. Record each RED in the task report.

- [ ] **Step 8: Commit** the new sources, `CMakeLists.txt`, the test — `feat(core): XPS page sizes from the FixedPage root, stream route (#227)`.

## Task 4: Direct route and read-ahead (`prepare_all`)

**Files:**
- Modify: `src/core/detail/XpsPageSizer.hpp`, `src/core/XpsPageSizer.cpp`
- Test: `tests/unit/test_xps_page_sizer.cpp`

**Interfaces:**
- Consumes: Task 1's `read_zip_directory`, `ZipEntryRef` sizes, fixed-span `plan_read_ahead`, `prefetch_file_ranges`; Task 3's class.
- Produces: `prepare_all()` and the direct route of `header_size`, behaviour per §3.4 and §3.9.

- [ ] **Step 1: Write the failing tests:**
  - `XpsPageSizer: after prepare_all the routes are the table's` — `prepare_all()` true, a second call false; every row's route and size as spec §5.1 (`Direct` rows bitwise equal to `loaded_size`, p13 300 × 300, `None` rows nullopt).
  - `XpsPageSizer: a rewritten zip name keeps every page on the stream route` — escaped fixture: after `prepare_all`, all three `Route::Stream`, sizes equal.
  - `XpsPageSizer: a failed read-ahead keeps the direct route` — `set_prefetch_for_test` with a function returning false (restore with nullptr in a scope guard outside any `fz_try`); after `prepare_all` every `Direct` row still `Direct`.
  - `XpsPageSizer: a page damaged past its root still sizes` — copy `xps-sizing.xps` to a temp file, overwrite p14's compressed bytes from offset data_start + 4096 to its end with `0xFF` (find data_start from the local header: offset + 30 + name length + extra length, all read from the copy); `loaded_size` on the copy throws; after `prepare_all`, `header_size` gives p14's size via `Direct` (§3.6 item 1).
  - `XpsPageSizer: a CJK path takes the direct route` (Review Focus 1) — copy the fixture to `fs::temp_directory_path() / fs::path(L"litepdf-xps-\u6e2c\u8a66") / fs::path(L"\u6587\u4ef6.xps")` (wide literals, Global Constraints), open it with `OwnDocument(utf8_of(path).c_str())`, `create` + `prepare_all`, p01 `Direct`; remove the directory at the end.

- [ ] **Step 2: Run** `"[xps]"`. Expected: the new cases fail (`prepare_all` returns false; routes `Stream`).

- [ ] **Step 3: Implement `prepare_all`** (once; sets a done flag first): UTF-8 → UTF-16 with `MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS)` (failure → stream route, return true); `read_zip_directory`; binding per §3.4 (count equal, every `fz_list_archive_entry` byte-equal including length); build `std::unordered_map<std::u32string, std::size_t>` of `fold(MuPDF name i)` keeping the first i (`emplace`, which keeps an existing key); resolve each page's key per §3.4 (strip `/`, `fz_cleanname` on a `std::vector<char>` of `max(2, len + 1)` bytes, strip `/`, fold) into a per-page `std::optional<std::size_t>` entry index; then plan with `kXpsReadAheadSpan` over the resolved entries' local offsets and call the prefetch function (its result ignored). Directory or binding failure: no index, no read-ahead. The fold and the key are file-local functions; `fold` uses `fz_chartorune` and `fz_tolower` only.

- [ ] **Step 4: Implement the direct route** in `header_size`, tried before the stream route when the page has an entry index: an entry with any of its two sizes or its offset at `0xFFFFFFFF` → stream route; otherwise per §3.4: `fz_seek(zip->file, offset, SEEK_SET)`, read 30 bytes (short → stream route), signature `50 4B 03 04` (else stream route), general flag bit 0 → nullopt, method / name length / extra length from the local header (+8, +26, +28); data = offset + 30 + name + extra; method 0 → `fz_open_null_filter(ctx, zip->file, uncompressed_size, data)`; method 8 → that null filter with `compressed_size`, then `fz_open_flated(ctx, null, -15)`; other methods → nullopt. Two `fz_var` stream locals, both dropped in `fz_always` (§3.4); the opening happens in a helper holding the `fz_try`, `read_root_size` runs on the opened stream outside it (Task 3's structure). Then `read_root_size`; a pair → `Route::Direct`; nullopt (including a flate error inside `read_some`) → nullopt, not the stream route (§3.4: it would inflate the same bytes).

- [ ] **Step 5: Run** `"[xps]"` then `"[zip]"`. Expected: PASS.

- [ ] **Step 6: Mutation checks** (restore after each): fold map keeps the last duplicate (`insert_or_assign`) → p13 must fail; skip the binding → the escaped-fixture case must fail; data start from the central record's name/extra lengths (pass the central extra length, 0) → p01 must leave `Direct`; sizes from the local header → p02 and p05 must leave `Direct`; the read-ahead failure dropping the index → the failed-read-ahead case must fail. Record each RED.

- [ ] **Step 7: Commit** — `feat(core): XPS direct route bound to MuPDF's zip directory, with read-ahead (#227)`.

## Task 5: Document owns the XPS sizer

**Files:**
- Modify: `src/core/Document.cpp` (`Impl` members and `reset_cbz` at :83-97, `~Impl` :136-139, `open` :429, `close` :457, `page_size_held` :684-698, `read_ahead_page_sizes` :745-754)
- Test: `tests/unit/test_xps_page_sizer.cpp`

**Interfaces:**
- Consumes: Task 3/4's `XpsPageSizer`.
- Produces: no new public API. `Document::read_ahead_page_sizes()` returns true for the first call on an XPS the sizer applies to (as for CBZ).

- [ ] **Step 1: Write the failing tests:**
  - `XpsPageSizer: Document::page_size equals the loaded page's bounds` — `Document` on a temp copy of `xps-sizing.xps`: before `read_ahead_page_sizes`, every page but p09 equals `loaded_size` bitwise and p09 throws `std::runtime_error`; `read_ahead_page_sizes()` true then false; after it, the same equalities; `size_pages` on a fresh `std::vector<std::optional<PageSize>>(15)` fills 14 entries equal to the same values and leaves p09 empty.
  - `XpsPageSizer: page_sizes asks for the read-ahead` — through `DocumentView::page_sizes()` (pattern of `ZipReadAhead: Document reads ahead once, and page_sizes asks for it`), then `read_ahead_page_sizes()` returns false.
  - `XpsPageSizer: closing the document releases the file` (spec §5.2 item 8) — temp copy, open, `read_ahead_page_sizes`, `page_size` over every page, `close()`; `DeleteFileW` on the copy succeeds.
  - `XpsPageSizer: close and open size the new document` (Review Focus 2) — open `xps-sizing.xps`, size page 0, `close()`, open `xps-escaped-name.xps` on the same `Document` (as `CbzPageSizer: close and open size the new document`, `test_cbz_page_sizer.cpp:283-296`), `read_ahead_page_sizes()` true (a new sizer), page 0 is 300 × 200 × 72/96.
  - `XpsPageSizer: a page past the end` (Review Focus 5) — `page_size(15)` throws as today; `XpsPageSizer::header_size(15)` nullopt.
  - `XpsPageSizer: a relative path in a directory with no UTF-8 form` (Review Focus 4) — as the CBZ case of the same name (`test_cbz_page_sizer.cpp:327`): chdir into a temp directory whose name holds a lone U+D800, open the fixture by relative name; every page but p09 equals `loaded_size`; `read_ahead_page_sizes()` false (no sizer).

- [ ] **Step 2: Run** `"[xps]"`. Expected: the Document cases fail (no sizer; `read_ahead_page_sizes` false on XPS).

- [ ] **Step 3: Implement** per §3.8: members `xps_path`, `xps_probed`, `xps` after the CBZ ones; `reset_cbz()` renamed `reset_sizers()`, resetting both, at every current call site; `~Impl` resets `xps` before `fz_drop_document`; `open()` sets `xps_path = absolute_utf8(path)` for an `.xps` extension in any case (an `is_xps_path` beside `is_cbz_path`); `page_size_held` after the CBZ block: probe once, then `header_size`; `read_ahead_page_sizes` early return `!impl_->doc || (cbz_path.empty() && xps_path.empty())`, then under its hold the CBZ branch or the XPS branch (create if not probed, `prepare_all`). Update `DocumentView::page_sizes()`'s comment to "every page is about to be sized", and `Document.hpp:106-110`'s comment on `read_ahead_page_sizes` ("nothing for other formats" becomes: for an XPS it binds the direct route and reads every page part's first bytes ahead, #227).

- [ ] **Step 4: Run** `"[xps]"`, `"[cbz]"`, `"[zip]"`, then the full suite serially. Expected: all pass; record the new count.

- [ ] **Step 5: Commit** — `feat(core): Document sizes XPS pages from the FixedPage root (#227)`.

## Task 6: MuPDF bump guard

**Files:**
- Modify: `cmake/ImportMuPDF.cmake` (after the #229 hash block, :268-284, and the `CMAKE_CONFIGURE_DEPENDS` property at :288-289)

**Interfaces:**
- Produces: configure fails with a message naming #227 when any of the ten files changes.

- [ ] **Step 1: Record the hashes.** For each of `source/xps/xps-doc.c`, `source/xps/xps-zip.c`, `source/xps/xps-imp.h`, `source/fitz/unzip.c`, `source/fitz/archive.c`, `source/fitz/string.c`, `source/fitz/xml.c`, `source/fitz/text-decoder.c`, `source/fitz/filter-basic.c`, `source/fitz/filter-flate.c` under `third_party/mupdf`, compute SHA-256 with CRLF → LF (the #229 block's normalisation), e.g. `python -I -c "import hashlib,sys;print(hashlib.sha256(open(sys.argv[1],'rb').read().replace(b'\r\n',b'\n')).hexdigest())" <file>`.

- [ ] **Step 2: Implement** a list of `<relative path>|<sha256>` pairs and a `foreach` doing the #229 block's read / replace / `string(SHA256)` / compare; on mismatch `message(FATAL_ERROR "#227 XPS sizer: MuPDF <file> changed (SHA-256 <got>, recorded <want>). XpsPageSizer relies on its rules (docs/superpowers/specs/2026-11-26-xps-header-sizing-227-design.md, section 3.11): re-check them against the new file, then update the recorded hash in cmake/ImportMuPDF.cmake.")`. Append the ten paths to the existing `CMAKE_CONFIGURE_DEPENDS` property.

- [ ] **Step 3: Verify both ways.** Reconfigure: passes. Then change one recorded hash's last character, reconfigure: fails with the message naming that file; restore, reconfigure: passes. (Do not edit a MuPDF file to test it.)

- [ ] **Step 4: Commit** — `build: hash the MuPDF files the XPS sizer relies on (#227)`.

## Task 7: Gate (local, not in CI)

**Files:**
- Create (outside the repo): `C:\Users\User\.claude\compact-dumps\litepdf\227-tools\scatter-xps.py`
- Modify: `docs/superpowers/specs/2026-11-26-xps-header-sizing-227-design.md` (new §6.1 "Record")

**Interfaces:**
- Consumes: a Release build of `litepdf-cli` from this branch's head (`& $cmake --build build --target litepdf-cli --config Release`).
- Produces: the readings that decide Task 8.

- [ ] **Step 1: Make both gate files** (spec §6). Dense: `powershell -NoProfile -STA -File 227-tools\make-wpf-xps.ps1 -Out <scratch>\dense.xps -Words 540000 -Seed 228` (expect `pages=1003`). Scattered: write `scatter-xps.py <in> <out>` (Python standard library; run with `python -I`): copy every entry of `<in>` in order with its `ZipInfo` (name, date, compress type), and after each entry whose name ends in `.fpage` add a stored entry `Resources/pad/<n>.bin` of 65,536 bytes from `random.Random(227).randbytes`. It then asserts: the `.fpage` count equals the input's, and in zip order every `.fpage` but the last is followed by a pad entry. Record both files' page count, size and entry count.

- [ ] **Step 2: Run the protocol** on the dense file (`227-tools` README: 1,003 pages) and the scattered file, saving every command's raw output to a dated file next to the scripts: per file three cold singles (`227-tools\..\229-tools\evict.ps1 -Path <file>`, then `litepdf-cli <file> --bench-page-sizes --iterations 1`), a warm median (`--iterations 9`), a warm median pinned to an E-core (`cmd /c "start "" /b /wait /affinity 0x10000 <cli> <file> --bench-page-sizes --iterations 9 > <out>"`; `/b` keeps the output in the redirect, without it the file stays empty; first confirm with `Get-CimInstance Win32_Processor` / Task Manager that logical CPU 16 is an E-core on this i7-12700, else pick one that is), one cold single after at least an hour untouched. Recorded, not gated, from a local build with an uncommitted edit (never pushed): the scattered file with the read-ahead disabled (`set_prefetch_for_test` with a function returning false, called at the top of `litepdf-cli`'s `main`), and the dense file with the binding forced to fail (every page on the stream route), three cold singles and a warm median each. Then revert that edit (`git diff --stat` empty) and rebuild `litepdf-cli` and `litepdf`, so no later step runs the edited binary.

- [ ] **Step 3: Decide.** Pass: on both files every cold single, the hour-idle single and the warm median ≤ 100 ms, and the E-core warm median ≤ 100 ms. A miss: stop, report the readings to the user, do not start Task 8.

- [ ] **Step 4: Record** the readings as spec §6.1: per file its page count, size and (scattered) that every page part is followed by a 64 KiB pad part; a table of cold singles, warm median, E-core warm median, hour-idle single; the two recorded-only rows (scattered without read-ahead, dense on the stream route); drive NVMe; the build commit. Commit — `docs(spec): #227 gate record`.

## Task 8: XPS in the strip

**Files:**
- Modify: `src/ui/detail/EffectiveLayout.hpp:66-78` (the comment above `strip_format_ok` and the function), `tests/unit/test_strip_helpers.cpp:98-114`
- Modify: `scripts/smoke-test.ps1` (row 0c after row 0b, :883-895), `docs/SMOKE-CHECKLIST.md:29` and `:31`
- Modify: `CHANGELOG.md` (`## [Unreleased]`), `README.md:41`
- Modify: `docs/superpowers/specs/2026-10-22-continuous-scroll-design.md` §4.4 (the XPS block and the "the format passed §4.4" sentence, :594)

**Interfaces:**
- Consumes: Task 7's pass.

- [ ] **Step 1: Change the test first:** in `EffectiveLayout: strip_format_ok accepts the measured formats`, the two `CHECK_FALSE` XPS lines become `CHECK(strip_format_ok(std::filesystem::path("e.xps"), 1))`, `CHECK(... "e.XPS" ...)`, plus `CHECK_FALSE(... "e.xps", 0)`; the comment says XPS passes since #227 (sized from the FixedPage root).

- [ ] **Step 2: Run** `"EffectiveLayout: strip_format_ok accepts the measured formats"`. Expected: FAIL.

- [ ] **Step 3: Implement:** add `|| extension_is(ext, ".xps")` to `strip_format_ok`, and replace the comment's "XPS stays out: ..." sentence with "XPS passes since #227: a page is sized from the root of its FixedPage part, its zip entry found through a directory bound to MuPDF's (docs/superpowers/specs/2026-11-26-xps-header-sizing-227-design.md)."

- [ ] **Step 4: Run** the case, then the full suite serially. Expected: PASS.

- [ ] **Step 5: Smoke row 0c**, modelled on row 0b: `tests/fixtures/xps-sizing.xps` opens in the strip (`Start-StripLaunch`), menu item 40066 found, not grayed, checked; `[OK] continuous strip: an XPS document opens in the strip with the item enabled`. Checklist: replace :29 (`` An `.xps` file opens one page at a time, with View → Continuous Scroll greyed ``) with `` - [ ] `tests\fixtures\xps-sizing.xps` opens in continuous scroll; the scroll bar reaches its last page (#227) `` — a replacement, as #176 did for CBZ (3a42e57), never a second line that contradicts the first. Run the smoke script wrapped as the project's smoke rules require (it overwrites `session.json` and the MRU: back them up and restore, `reference_litepdf_smoke_script_rules`), and a negative control: with `strip_format_ok` reverted locally the row fails; restore.

- [ ] **Step 6: Docs.** CHANGELOG under `## [Unreleased]`, a new `### Added` above `### Changed`: XPS documents open in continuous scroll; each page is sized from the start of its page part instead of the whole page (#227), with the gate's dense-file cold and warm figures in plain words. README:41: drop "XPS documents stay out of the strip" and append "XPS since #227" inside the parenthesis; the release task replaces "#227" with the version (`project_litepdf_ship_version_convention`). Upstream spec §4.4: XPS block says XPS passes since #227 with Task 7's readings, the drive, and that a hard drive was not measured; the :594 sentence lists XPS.

- [ ] **Step 7: Commit** — `feat(ui): XPS joins continuous scroll (#227)`.
