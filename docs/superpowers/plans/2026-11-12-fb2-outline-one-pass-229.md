# One-pass outline resolve (#229 PR 1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Carry a local MuPDF 1.27.2 patch that resolves an HTML-engine document's outline in one pass, so a large FB2's layout costs about the layout alone, with identical outline results.

**Architecture:** The patch rewrites `htdoc_update_outline` in `third_party/mupdf/source/html/html-doc.c` to build an `fz_tree` index (first y per id, walk order of `find_box_target`) and fall back to the original root walk for any non-`>= 0` value or an allocation failure. `cmake/ImportMuPDF.cmake` applies it at configure time like the existing `config.h` prune, and checks that `html-outline.c` (whose rules the patch copies) is unchanged. A versioned exported symbol carries the AGPL modification notice and makes a stale library fail the link.

**Tech Stack:** C (MuPDF 1.27.2), C++20, CMake 3.x + MSBuild (VS 2022 BuildTools), Catch2, Win32.

**Spec:** `docs/superpowers/specs/2026-11-12-fb2-outline-one-pass-229-design.md`

## Global Constraints

- Release builds only; `cmake`/`ctest` by full BuildTools path (`C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\`); full `ctest` serial, no `-j`, from PowerShell.
- The shared checkout `C:\Users\User\projects\litepdf\third_party\mupdf` is never modified before merge (no junction to it in this worktree).
- Stage explicit paths; never `git commit -a`.
- Marker: `LITEPDF_PATCH_HTDOC_OUTLINE_V1`. Symbol: `const char litepdf_mupdf_patch_notice_v1[] = "Modified by litepdf on 2026-11-12: one-pass outline resolve (#229)";`
- Patch file: `third_party/mupdf-patches/0001-htdoc-one-pass-outline.patch`, LF, generated with `git diff -- source/html/html-doc.c` from the MuPDF root.
- Test file `tests/unit/test_mupdf_outline_patch.cpp`, ASCII TEST_CASE names prefixed `MuPDF outline patch:`, tag `[mupdf][outline]`.
- Every MuPDF `fz_catch` that does not rethrow clears the error (`fz_report_error`) — the #146 rule.
- All code, comments, commit messages in English; the licence text line in zh-TW (the file's language).

## Review Focus

1. **A real FB2 whose title ids repeat paragraph ids** (books converted from HTML often keep ids): every outline entry must open the same place as before. Pinned by Task 2's duplicate fixture and the no-lookup mutant (Task 3 Step 9).
2. **An FB2 with no titles at all** (empty outline): `fz_count_pages` must work and build no index. Task 2 adds `MuPDF outline patch: book without titles opens` (outline NULL, page count > 0).
3. **Opening the same FB2 twice in one process, as the GUI does for each worker**: each copy's outline must match. Task 2's equivalence test opens two documents on one context and checks both.
4. **A link-following click after the patch** (`fz_resolve_link_dest`, unchanged root walk) must keep working on the patched build: it is the reference in every test, so any regression there shows as a mismatch.
5. **Low memory while opening a large book**: the open must not newly fail. Task 2's allocation-failure test (spec 6.5).

---

## Task 1: Private MuPDF tree, unpatched build, "before" numbers

No commit. Produces the build the RED half of the tests runs on, and the baseline executables.

**Files:** none in the repo. Scratch: `<scratch>` = this session's scratchpad.

**Interfaces:**
- Produces: `third_party/mupdf` populated (git worktree of the shared submodule repo at `73d3100`, `thirdparty/` copied); `build/` configured and built Release at the branch base; `<scratch>\before\litepdf-cli.exe`, `<scratch>\before\litepdf.exe`; books `<scratch>\fx\s200.fb2`, `s800.fb2`, `s1500.fb2`.

- [ ] **Step 1: Create the private MuPDF tree**

From the worktree root (PowerShell): the empty `third_party\mupdf` must be removed first (`Remove-Item third_party\mupdf`, alone in its call — see the build-commands memory).

```
git -C C:\Users\User\projects\litepdf\third_party\mupdf worktree add --detach <worktree>\third_party\mupdf 73d3100d46d8a9ad634f6ef035bbe78f0f947886
robocopy C:\Users\User\projects\litepdf\third_party\mupdf\thirdparty <worktree>\third_party\mupdf\thirdparty /E /XF .git /NFL /NDL /NP
```

Expected: `git -C third_party\mupdf status --short` lists only untracked `thirdparty/tesseract` content or nothing, and no "not a git repository" error; `git -C third_party\mupdf log -1 --format=%H` prints `73d3100d…`. `git status` in the litepdf worktree shows no `third_party/mupdf` change except possibly ` m` (dirty submodule, ignored by `ignore = dirty`).

- [ ] **Step 2: Configure and build Release**

Copy `C:\Users\User\projects\litepdf\build\_deps\catch2-src` into `<worktree>\build\_deps\catch2-src`, then configure with `-DFETCHCONTENT_FULLY_DISCONNECTED=ON -DFETCHCONTENT_SOURCE_DIR_CATCH2=<worktree>\build\_deps\catch2-src` and `cmake --build build --config Release` (background; a full MuPDF build, minutes; if it fails once with MSB8066 on `mupdf_ext`, re-run `--target mupdf_ext` then the full build). Expected: configure prints `MuPDF config.h pruned (LITEPDF_PRUNE_V10)` (private tree only) and the build ends with 0 errors; `git -C C:\Users\User\projects\litepdf\third_party\mupdf status --short` is unchanged from before Step 1.

- [ ] **Step 3: Copy the "before" executables aside**

`build\Release\litepdf-cli.exe` and `build\Release\litepdf.exe` → `<scratch>\before\`.

- [ ] **Step 4: Generate the books**

`python C:\Users\User\.claude\compact-dumps\litepdf\229-tools\make-fb2.py <scratch>\fx\s<N>.fb2 <N> 10` for N = 200, 800, 1500.

- [ ] **Step 5: Smoke the baseline**

`<scratch>\before\litepdf-cli.exe <scratch>\fx\s800.fb2 --benchmark --iterations 1` prints an `open_ms` in the seconds range (≈ 1.7-2.9 s per #229). The full before/after series runs in Task 5 from both executable sets, interleaved.

## Task 2: Tests and the notice accessor (RED)

**Files:**
- Create: `src/core/MuPdfPatchNotice.hpp`, `src/core/MuPdfPatchNotice.cpp`
- Create: `tests/unit/test_mupdf_outline_patch.cpp`
- Modify: `CMakeLists.txt` (`litepdf_core` list: add `src/core/MuPdfPatchNotice.cpp  # #229`), `tests/CMakeLists.txt` (`target_sources` list: add `unit/test_mupdf_outline_patch.cpp  # #229`)

**Interfaces:**
- Produces: `namespace litepdf::core { std::string_view mupdf_patch_notice() noexcept; }` returning the text of `extern "C" const char litepdf_mupdf_patch_notice_v1[];`.
- Produces (test-local, anonymous namespace): `std::string make_fb2(const BookSpec&)`; `fz_document* open_fb2(fz_context*, const std::string&)` (calls `fz_register_document_handlers` once per context, opens via `fz_open_memory` + `fz_open_document_with_stream(ctx, "application/x-fictionbook", stm)`); `void require_outline_matches_root_walk(fz_context*, fz_document*)` (walks `fz_load_outline` recursively; for each node `REQUIRE` `node->page.chapter/page`, `node->x`, `node->y` equal `fz_resolve_link_dest(ctx, doc, node->uri)`'s `loc` / `x` / `y`, exact float equality — both sides compute the same expression).

- [ ] **Step 1: Accessor**

`MuPdfPatchNotice.hpp`: the declaration above with a comment that the symbol comes from `third_party/mupdf-patches/0001-htdoc-one-pass-outline.patch` and why the name is versioned (spec 3.4). `MuPdfPatchNotice.cpp`: `extern "C" const char litepdf_mupdf_patch_notice_v1[];` and the one-line accessor.

- [ ] **Step 2: Write the tests** (all on their own `fz_context` created with `fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT)` and dropped at the end; FB2 text built in memory; words from a fixed 20-word list chosen by a fixed-seed LCG so books are identical every run)

1. `MuPDF outline patch: notice symbol is linked` — `REQUIRE(litepdf::core::mupdf_patch_notice() == "Modified by litepdf on 2026-11-12: one-pass outline resolve (#229)")`.
2. `MuPDF outline patch: outline matches the root walk` — the spec 6.1 book: nested sections (3 levels) with titles and subtitles, one `<h1>`; titles without ids; one title id of 60 characters; one empty `<title id="empty-title"/>`; about 20 titles each repeating the id of a `<p id>` at least one page earlier, the duplicated ids sorting in the middle of about 200 other `<p id>` user ids. Fixture-proving first: outline node count equals the number of headings generated; some node has `page.page > 0`; for each duplicated id, `fz_resolve_link_dest(ctx, doc, "#<id>").loc.page` < the page of the unique id on the title's next paragraph. Then `require_outline_matches_root_walk`. Then a second `open_fb2` of the same text on the same context, `fz_count_pages`, `require_outline_matches_root_walk` again (Review Focus 3).
3. `MuPDF outline patch: book without titles opens` — sections with paragraphs only: `fz_load_outline` returns NULL and `fz_count_pages` > 0.
4. `MuPDF outline patch: negative first match falls back to the root walk` — `fz_set_user_css(ctx, "section>title{padding-top:1px;margin-top:-2000px}")` before opening (and `fz_set_use_document_css(ctx, 1)` unchanged); the book's first title carries `id="neg"`, a `<p id="neg">` sits several pages later. Precondition: `fz_resolve_link_dest(ctx, doc, "#neg").loc.page` equals the page of that paragraph's unique neighbour id and is > 0 (if the CSS failed to make y < 0 this lands on page 0 and the test fails here). Then `require_outline_matches_root_walk`. If this CSS does not produce the precondition on the unpatched build, adjust it (larger margin, border instead of padding) and record the working CSS in a comment.
5. `MuPDF outline patch: allocation failure in the index falls back` — spec 6.5. Allocator: `fz_alloc_context{&state, alloc, realloc, free}` where `alloc`/`realloc` increment `state.calls` and return NULL when `state.armed && state.calls >= state.fail_from`. `state.calls` is reset to 0 right before each `fz_count_pages`, after the open, so N counts that call only. Measure N on a freshly opened small book (4 sections, user ids known, D = distinct ids counted by the builder, generated `'N` ids included) twice and `REQUIRE` equal and `N - 2*D > 0`. `fz_count_pages` always runs inside the test's own `fz_try`, recording whether it threw (`fz_ignore_error` in that catch). For each K in `N-2D+1 .. N`: fresh document, `REQUIRE(fz_caught(ctx) == 0)` first, arm at K, count pages, disarm; `REQUIRE` it did not throw, then `require_outline_matches_root_walk`, then `REQUIRE(fz_caught(ctx) == 0)` — `fz_push_try` does not reset `errcode` (`error.c:227`), so an index catch that forgot `fz_report_error` leaves it non-zero here. For each K in `1 .. N-2D`: fresh document, arm, count pages; a throw is accepted; no crash. Silence MuPDF's error/warning output for this test with `fz_set_error_callback`/`fz_set_warning_callback` to a no-op.
6. `MuPDF outline patch: page count grows with the book, not its titles` — spec 6.3 speed guard: books of 200 and 800 sections, each a title + 10 paragraphs of 60 words. Pin with `SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{1} << GetCurrentProcessorNumber())` (restore the previous mask at the end); for each size, 3 times: open, `QueryThreadCycleTime` before/after `fz_count_pages`, drop; keep the minimum. `REQUIRE(c800 / c200 < 12.0)`; print both on failure via `INFO`.

- [ ] **Step 3: Register files, build, record RED on the unpatched tree**

Build `litepdf_unit_tests`. Expected: **link fails** with `LNK2001: unresolved external symbol litepdf_mupdf_patch_notice_v1` (spec 6.4 second bullet). Record the line.

- [ ] **Step 4: Throwaway stub, record the other RED**

Temporarily make `MuPdfPatchNotice.cpp` define the symbol itself (never committed), build, run `litepdf_unit_tests.exe "[mupdf][outline]"` from the repo root. Expected: tests 2, 3, 4 PASS (the unpatched code is the reference itself; test 4's precondition holds), test 5 FAILS (K in the top 2D range are layout allocations on the unpatched build, so `fz_count_pages` throws), test 6 FAILS with a ratio ≈ 20 (spec: ≈ x21). Record the ratio. `git checkout -- src/core/MuPdfPatchNotice.cpp`.

- [ ] **Step 5: Commit (branch not pushed until Task 3 is green)**

```bash
git add src/core/MuPdfPatchNotice.hpp src/core/MuPdfPatchNotice.cpp tests/unit/test_mupdf_outline_patch.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "test(core): pin the one-pass outline patch's results and cost (#229)"
```

## Task 3: The patch and its CMake application (GREEN)

**Files:**
- Modify (private tree, then captured): `third_party/mupdf/source/html/html-doc.c`
- Create: `third_party/mupdf-patches/0001-htdoc-one-pass-outline.patch`
- Modify: `cmake/ImportMuPDF.cmake` (new block after the prune-effective assertion, before `ExternalProject_Add`)

**Interfaces:**
- Consumes: Task 2's tests and accessor.
- Produces: the symbol `litepdf_mupdf_patch_notice_v1` in `libmupdf.lib`.

- [ ] **Step 1: Write the patch in the private tree's `html-doc.c`**

Above `htdoc_update_outline`: the marker comment block (`/* LITEPDF_PATCH_HTDOC_OUTLINE_V1 — applied by litepdf's cmake/ImportMuPDF.cmake; see third_party/mupdf-patches/. One-pass outline resolve, #229. */`), the symbol definition, and these static functions:

- `static fz_html_flow *lp_find_first_content(fz_html_box *box)` — a copy of `html-outline.c`'s `find_first_content`.
- `static void lp_put_first(fz_context *ctx, fz_tree **index, const char *id, float *y)` — `fz_tree_lookup(ctx, *index, id)` first; only if absent `*index = fz_tree_insert(ctx, *index, id, y)`.
- `static void lp_index_boxes(fz_context *ctx, fz_tree **index, fz_html_box *box)` — the walk of spec 3.1, in this order:

```c
while (box)
{
	if (box->id)
	{
		fz_html_flow *flow = lp_find_first_content(box);
		lp_put_first(ctx, index, box->id, flow ? &flow->y : &box->s.layout.y);
	}
	if (box->type == BOX_FLOW)
	{
		fz_html_flow *flow;
		for (flow = box->u.flow.head; flow; flow = flow->next)
			if (flow->box->id)
				lp_put_first(ctx, index, flow->box->id, &flow->y);
	}
	else
		lp_index_boxes(ctx, index, box->down);
	box = box->next;
}
```

  The root is updated in place after every insert, never carried in return values: `fz_tree_insert` rebalances (skew/split) on the way back up, so an older root pointer can become an inner node, and a root held in a frame that a throw unwinds would be stale. `fz_tree_new_node` throws before linking anything, so after a throw `*index` is still the complete tree and `fz_drop_tree` frees every node.
- `static void lp_update_outline_from_index(fz_context *ctx, fz_document *doc_, fz_tree *index, fz_outline *node)` — spec 3.2: `strchr(node->uri, '#')`, none/empty → `fz_make_link_dest_none()`; lookup; absent → none; `*y >= 0` → the `htdoc_resolve_link` arithmetic (`int page = y / doc->html->page_h; fz_make_link_dest_xyz(0, page, 0, y - page * doc->html->page_h, 0)`); else → `htdoc_resolve_link(ctx, doc_, node->uri)`. Writes `node->page/x/y` like the original; recurses into `node->down`, iterates `node->next`.

`htdoc_update_outline(ctx, doc, node)` becomes: `if (!node) return;` `fz_tree *index = NULL; fz_var(index);` build it in `fz_try` with `lp_index_boxes(ctx, &index, ((html_document*)doc)->html->tree.root)`, `fz_catch` → `fz_drop_tree(ctx, index, NULL); index = NULL; fz_report_error(ctx);`. Then `index ? lp_update_outline_from_index(...) : <the original per-node loop, kept as a static function lp_update_outline_root_walk>`; finally `fz_drop_tree(ctx, index, NULL)`. `htdoc_layout` and `htdoc_resolve_link` are unchanged.

- [ ] **Step 2: Generate the patch file**

From `third_party/mupdf`: `git diff -- source/html/html-doc.c` → `third_party/mupdf-patches/0001-htdoc-one-pass-outline.patch`, then prepend a header (plain text before the first `diff --git` line; `git apply` ignores it): what it changes, why (#229, the O(N^2) outline update), date 2026-11-12, the marker, "drop when upstream resolves the outline in one pass; then remove `MuPdfPatchNotice`, its test, the About line and the html-outline.c hash check". Ensure LF (`.gitattributes`). Then `git -C third_party/mupdf checkout -- source/html/html-doc.c` so the CMake block applies it.

- [ ] **Step 3: CMake block** in `cmake/ImportMuPDF.cmake`, same style as the prune block:

- `set(_LITEPDF_PATCH_VER "LITEPDF_PATCH_HTDOC_OUTLINE_V1")`, `set(_LITEPDF_PATCH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mupdf-patches/0001-htdoc-one-pass-outline.patch")`, `set(_HTML_DOC_C "${_MUPDF_ROOT}/source/html/html-doc.c")`.
- If `_HTML_DOC_C` does not match the marker: `git -C "${_MUPDF_ROOT}" checkout -- source/html/html-doc.c` (FATAL on rc, message per spec 4 step 2, naming the no-`.git` case); `git -C "${_MUPDF_ROOT}" apply --whitespace=nowarn "${_LITEPDF_PATCH}"` (FATAL on rc, spec 4 step 3 message); re-read, FATAL if marker absent; `message(STATUS "MuPDF html-doc.c patched (${_LITEPDF_PATCH_VER}).")`; `file(REMOVE ...mupdf_ext-build)` as the prune block does.
- Every configure: read `${_MUPDF_ROOT}/source/html/html-outline.c`, `string(REPLACE "\r\n" "\n" ...)`, `string(SHA256 ...)`, compare with `set(_HTML_OUTLINE_SHA256_1272 "<value>")`; FATAL on mismatch per spec section 4. Compute `<value>` once with a throwaway `cmake -P` script doing the same three calls on the pristine file, and paste it.

- [ ] **Step 4: Configure, build, check the apply**

Expected configure output: `MuPDF html-doc.c patched (LITEPDF_PATCH_HTDOC_OUTLINE_V1).`; second configure prints nothing for it (marker present). `git -C third_party/mupdf diff --stat` shows only `source/html/html-doc.c` (plus the existing vcxproj/config.h rewrites). Check line endings of the patched file (`file third_party/mupdf/source/html/html-doc.c`) and note mixed/CRLF in the PR.

- [ ] **Step 5: Run the tests — GREEN**

`litepdf_unit_tests.exe "[mupdf][outline]"` from the repo root: all 6 pass; test 6 prints/records a ratio ≈ 4.

- [ ] **Step 6: Hash-check falsifier**

Append a blank line to the private tree's `html-outline.c`, configure: FATAL with the spec 4 message. `git -C third_party/mupdf checkout -- source/html/html-outline.c`, configure again: passes.

- [ ] **Step 7: Mutants (spec 6.4), each a throwaway edit of the private `html-doc.c`, rebuild, run `"[mupdf][outline]"`, then `git -C third_party/mupdf checkout -- source/html/html-doc.c` and reconfigure:**

| Mutant | Must fail |
|---|---|
| `lp_put_first` inserts without the lookup | test 2 (duplicates). If it passes, change the fixture (spec 6.1) until it fails, and record the construction |
| the `*y >= 0` branch also takes negative values | test 4 |
| `lp_index_boxes` skips an id'd box whose `lp_find_first_content` is NULL | test 2 (`empty-title`) |
| the `fz_catch` without `fz_report_error` | test 5 (`fz_caught == 0`) |
| the `fz_catch` rethrows | test 5 (index range) |

Record each result.

- [ ] **Step 8: Full suite**

`cmake --build build --config Release` (whole tree) then `ctest --test-dir build -C Release --output-on-failure` serially from PowerShell. Expected: all pass (`ctest -N` count + 6).

- [ ] **Step 9: Commit**

```bash
git add third_party/mupdf-patches/0001-htdoc-one-pass-outline.patch cmake/ImportMuPDF.cmake
git commit -m "perf(mupdf): resolve an HTML document's outline in one pass (#229)"
```

## Task 4: Modification notice in the app, licence text and CHANGELOG

**Files:**
- Modify: `src/ui/MainWindow.cpp` (About box, after `L"Engine: MuPDF 1.27.2\n"`)
- Modify: `installer/make-license-rtf.py:27`, regenerate `installer/LICENSE-DISPLAY.rtf`
- Modify: `CHANGELOG.md` (`[Unreleased]` → `### Fixed`)

**Interfaces:**
- Consumes: `litepdf::core::mupdf_patch_notice()` (Task 2).

- [ ] **Step 1: About box** — after the engine line, `text += litepdf::core::ascii_to_wide(litepdf::core::mupdf_patch_notice()); text += L"\n";` (split the string literal concatenation as needed). Include `core/MuPdfPatchNotice.hpp`.

- [ ] **Step 2: Licence line** — `• MuPDF 1.27.2 — (c) Artifex Software, Inc. — AGPL-3.0（litepdf 於 2026-11-12 修改：目錄一次解析，見原始碼 third_party/mupdf-patches/）`. Run `python installer\make-license-rtf.py`; `git diff --stat installer/LICENSE-DISPLAY.rtf` shows the regenerated file (read the installer-encoding memory before running: the RTF is generated, never hand-edited).

- [ ] **Step 3: CHANGELOG** — under `[Unreleased]` `### Fixed`: a large FB2 book opened slowly, and the cost grew with the square of its section titles (#229): MuPDF re-searched the whole book for every title after laying it out. litepdf now carries one local change to MuPDF that finds them in a single pass; the outline is the same. No numbers yet: Task 5 adds one sentence with the 1,500-section book's open time before and after.

- [ ] **Step 4: Build the app and check the About text** — `cmake --build build --config Release --target litepdf`; launch `build\Release\litepdf.exe`, open Help → About, the notice line shows. (Or read the string through a UI smoke if a scripted check is simpler; a screenshot is enough evidence.)

- [ ] **Step 5: Commit**

```bash
git add src/ui/MainWindow.cpp installer/make-license-rtf.py installer/LICENSE-DISPLAY.rtf CHANGELOG.md
git commit -m "docs: say MuPDF is carried with one local change (#229)"
```

## Task 5: Before/after measurements

No new code. Results go into one CHANGELOG sentence (the entry Task 4 wrote) and the PR body.

- [ ] **Step 1: CLI series** — for N in 200, 800, 1500; 3 rounds; within each round run `before` then `after` (`build\Release\litepdf-cli.exe`), each `start /affinity FFFF /wait <exe> <book> --benchmark --iterations 1` with the book read once before the run. Record best-of-3 `open_ms` and `render_ms` per exe and size.

- [ ] **Step 2: GUI first page** — for N in 800, 1500; 3 rounds alternating before/after: launch `<exe> --log-timings <book>` with `LITEPDF_NO_RESTORE=1`, stderr redirected to a file, wait for the `T0->T4=` line (deadline 120 s), close the process. Record the medians of `T0->T4`.

- [ ] **Step 3: Write the numbers** into `CHANGELOG.md` (1,500 sections, `open_ms` before → after) and keep the full table for the PR body in `<scratch>\229-measurements.md`. Commit the CHANGELOG change:

```bash
git add CHANGELOG.md
git commit -m "docs(changelog): FB2 open times before and after the outline patch (#229)"
```

## After the plan

Rebase on `origin/main` if it moved; PR-merge gate (risk-tiered review, Full: cmake/native chain); PR referencing #229 without "closes" (PR 2 and maybe PR 3 follow); before merging, tell the user that the first configure on main through a junction patches the shared checkout and rebuilds its `libmupdf.lib`. Teardown after merge: `git -C C:\Users\User\projects\litepdf\third_party\mupdf worktree remove --force <worktree>\third_party\mupdf` (it holds only copied files, no junctions), then recreate the empty `third_party\mupdf` directory.
