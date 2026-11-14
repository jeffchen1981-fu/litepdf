# One-pass outline resolve (#229 PR 1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Carry a local MuPDF 1.27.2 patch that resolves an HTML-engine document's outline in one pass, so a large FB2's layout costs about the layout alone, with identical outline results.

**Architecture:** The patch rewrites `htdoc_update_outline` in `third_party/mupdf/source/html/html-doc.c` to build an `fz_tree` index (first y per id, walk order of `find_box_target`) and fall back to the original root walk for any non-`>= 0` value or an allocation failure. `cmake/ImportMuPDF.cmake` applies it at configure time like the existing `config.h` prune, and checks that `html-outline.c` (whose rules the patch copies) is unchanged. A versioned exported symbol carries the AGPL modification notice and makes a stale library fail the link.

**Tech Stack:** C (MuPDF 1.27.2), C++20, CMake 3.x + MSBuild (VS 2022 BuildTools), Catch2, Win32.

**Spec:** `docs/superpowers/specs/2026-11-12-fb2-outline-one-pass-229-design.md`

## Global Constraints

- Release builds only. **Every `cmake` and `ctest` in this plan means the BuildTools binary by full path** (`& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"`, same folder for `ctest.exe`): the bare names on this machine's PATH are a MinGW CMake. Full `ctest` serial, no `-j`, from PowerShell. Executables are run with a path (`& .\build\tests\Release\litepdf_unit_tests.exe …` from the repo root; PowerShell does not search the current directory).
- Test-side MuPDF catches follow the same rule: allocation failures are `FZ_ERROR_SYSTEM`, so they are cleared with `fz_report_error` (output silenced by the test's error callback), never `fz_ignore_error` (only for TRYLATER/ABORT, `context.h:210-214`). Every `fz_open_memory` stream and `fz_load_outline` result in the tests is dropped (`fz_drop_stream`, `fz_drop_outline`).
- The shared checkout `C:\Users\User\projects\litepdf\third_party\mupdf` is never modified before merge (no junction to it in this worktree).
- Stage explicit paths; never `git commit -a`.
- Marker: `LITEPDF_PATCH_HTDOC_OUTLINE_V1`. Symbol: `const char litepdf_mupdf_patch_notice_v1[] = "Modified by litepdf on 2026-11-12: one-pass outline resolve (#229)";`
- Patch file: `third_party/mupdf-patches/0001-htdoc-one-pass-outline.patch`, LF, generated with `git diff -- source/html/html-doc.c` from the MuPDF root.
- Test file `tests/unit/test_mupdf_outline_patch.cpp`, ASCII TEST_CASE names prefixed `MuPDF outline patch:`, tag `[mupdf][outline]`.
- Every MuPDF `fz_catch` that does not rethrow clears the error (`fz_report_error`) — the #146 rule.
- All code, comments, commit messages in English; the licence text line in zh-TW (the file's language).

## Review Focus

1. **A real FB2 whose title ids repeat paragraph ids** (books converted from HTML often keep ids): every outline entry must open the same place as before. Pinned by Task 2's duplicate fixture and the no-lookup mutant (Task 3 Step 7).
2. **An FB2 with no titles at all** (empty outline): `fz_count_pages` must work, and spend nothing on an index. Task 2 adds `MuPDF outline patch: book without titles opens` (outline NULL, page count > 0, and the allocation count of `fz_count_pages` is the same with or without 100 extra `<p id>`s, which would cost 200 index allocations if the `if (!node) return;` guard were gone).
3. **Opening the same FB2 twice in one process, as the GUI does for each worker**: each copy's outline must match. Task 2's equivalence test opens two documents on one context and checks both.
4. **A link-following click after the patch** (`fz_resolve_link_dest`) must behave as before. The patch leaves `htdoc_resolve_link` and everything outside `htdoc_update_outline` untouched, so this holds by construction; Task 3 Step 4 checks the patch's diff touches only the update path. The tests use link resolution as the reference, not as a subject, so they would not see a change made to both sides at once.
5. **Low memory while opening a large book**: the open must not newly fail. Task 2's allocation-failure test (spec 6.5).

---

## Task 1: Private MuPDF tree, unpatched build, "before" numbers

No commit. Produces the build the RED half of the tests runs on, and the baseline executables.

**Files:** none in the repo. Scratch: `<scratch>` = this session's scratchpad.

**Interfaces:**
- Produces: `third_party/mupdf` populated (a `git clone --shared` of the shared submodule repo, detached at `73d3100`, `thirdparty/` copied); `build/` configured and built Release at the branch base; `<scratch>\before\litepdf-cli.exe`, `<scratch>\before\litepdf.exe`; books `<scratch>\fx\s200.fb2`, `s800.fb2`, `s1500.fb2`.

- [ ] **Step 1: Create the private MuPDF tree**

From the worktree root (PowerShell): the empty `third_party\mupdf` must be removed first (`Remove-Item third_party\mupdf`, alone in its call — see the build-commands memory).

```
git clone --shared --no-checkout C:\Users\User\projects\litepdf\third_party\mupdf <worktree>\third_party\mupdf
git -C <worktree>\third_party\mupdf checkout --detach 73d3100d46d8a9ad634f6ef035bbe78f0f947886
robocopy C:\Users\User\projects\litepdf\third_party\mupdf\thirdparty <worktree>\third_party\mupdf\thirdparty /E /XF .git /NFL /NDL /NP
```

A clone with `--shared` borrows the shared repo's objects through `objects/info/alternates` and writes nothing into it. (A `git worktree add` from the shared submodule gitdir is avoided: that gitdir sets a relative `core.worktree`, and how a linked worktree resolves it is unverified.) `robocopy` exit codes below 8 are success; 1 means files were copied.

Expected: `git -C third_party\mupdf rev-parse --show-toplevel` prints `<worktree>/third_party/mupdf`; `git -C third_party\mupdf status --short` lists nothing or only untracked `thirdparty/` content, with no "not a git repository" error; `git -C third_party\mupdf log -1 --format=%H` prints `73d3100d…`. `git status` in the litepdf worktree shows no `third_party/mupdf` change except possibly ` m` (dirty submodule, ignored by `ignore = dirty`).

- [ ] **Step 2: Configure and build Release**

Copy `C:\Users\User\projects\litepdf\build\_deps\catch2-src` into `<worktree>\build\_deps\catch2-src`, then (PowerShell, full BuildTools `cmake` path) `cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DFETCHCONTENT_FULLY_DISCONNECTED=ON -DFETCHCONTENT_SOURCE_DIR_CATCH2=<worktree>\build\_deps\catch2-src` and `cmake --build build --config Release` (background; a full MuPDF build, minutes; if it fails once with MSB8066 on `mupdf_ext`, re-run `--target mupdf_ext` then the full build). Expected: configure prints `MuPDF config.h pruned (LITEPDF_PRUNE_V10)` (private tree only) and the build ends with 0 errors; `git -C C:\Users\User\projects\litepdf\third_party\mupdf status --short` is unchanged from before Step 1.

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
- Produces (test-local, anonymous namespace): `std::string make_fb2(const BookSpec&)`; `fz_document* open_fb2(fz_context*, const std::string&)` (calls `fz_register_document_handlers` once per context, opens via `fz_open_memory` + `fz_open_document_with_stream(ctx, "application/x-fictionbook", stm)`, then `fz_drop_stream`: the caller keeps ownership of the stream, `document.h:506-509`); a counting allocator `struct AllocState { int calls; int armed; int fail_from; }` with `alloc`/`realloc` that increment `calls` and return NULL when `armed && calls >= fail_from` (tests 3 and 5); `void require_outline_matches_root_walk(fz_context*, fz_document*)` (walks `fz_load_outline` recursively and drops it with `fz_drop_outline` at the end; for each node `REQUIRE` `node->page.chapter/page` equal `fz_resolve_link_dest(ctx, doc, node->uri).loc`, and `node->x` / `node->y` equal its `x` / `y` with exact float equality, both sides computing the same expression — except that a "none" destination has NaN coordinates (`link.c:98`), so when either value is NaN the check is `std::isnan` on both sides).

- [ ] **Step 1: Accessor**

`MuPdfPatchNotice.hpp`: the declaration above with a comment that the symbol comes from `third_party/mupdf-patches/0001-htdoc-one-pass-outline.patch` and why the name is versioned (spec 3.4). `MuPdfPatchNotice.cpp`: `extern "C" const char litepdf_mupdf_patch_notice_v1[];` and the one-line accessor.

- [ ] **Step 2: Write the tests** (each on its own `fz_context`, dropped at the end: `fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT)`, except tests 3 and 5, which pass the counting allocator as the first argument; FB2 text built in memory; words from a fixed 20-word list chosen by a fixed-seed LCG so books are identical every run)

1. `MuPDF outline patch: notice symbol is linked` — `REQUIRE(litepdf::core::mupdf_patch_notice() == "Modified by litepdf on 2026-11-12: one-pass outline resolve (#229)")`.
2. `MuPDF outline patch: outline matches the root walk` — the spec 6.1 book: nested sections (3 levels) with titles and subtitles, every title and subtitle inside a `section` (directly under `body` its heading level is 0, so it is no heading, `html-parse.c:1328-1331`); titles without ids; one title id of 60 characters; one empty `<title id="empty-title"/>`; about 20 titles each repeating the id of a `<p id>` at least one page earlier, the duplicated ids sorting in the middle of about 200 other `<p id>` user ids. Fixture-proving first: outline node count equals the number of headings generated; some node has `page.page > 0`; for each duplicated id, `fz_resolve_link_dest(ctx, doc, "#<id>").loc.page` < the page of the unique id on the title's next paragraph. Then `require_outline_matches_root_walk`. Then a second `open_fb2` of the same text on the same context, `fz_count_pages`, `require_outline_matches_root_walk` again (Review Focus 3).
3. `MuPDF outline patch: book without titles opens` — sections with paragraphs only: `fz_load_outline` returns NULL and `fz_count_pages` > 0. Then two such books, identical except that the second gives 100 of its paragraphs a unique `id`: after a warm-up open, `calls` counted over `fz_count_pages` is equal for both (ids cost nothing at layout; with the empty-outline guard gone, the index would add 200 allocations).
4. `MuPDF outline patch: negative first match falls back to the root walk` — its own book, whose only `subtitle` is the first child of the first section and carries `id="neg"`; `fz_set_user_css(ctx, "subtitle{padding-top:1px;margin-top:-200px}")` before opening, so only that one element gets the negative margin (a `section>title` selector would hit every title). Everything after it moves up by the same 200 pt, so a `<p id="neg">` at least 4 pages later, followed by a paragraph with a unique id, stays at y > 0. Precondition: `fz_resolve_link_dest(ctx, doc, "#neg").loc.page` equals the page of that unique neighbour id and is > 0 (if the CSS failed to make the subtitle's y < 0 this lands on page 0 and the test fails here). Then `require_outline_matches_root_walk`. If the precondition fails on the unpatched build, adjust the CSS (larger margin, `border-top` instead of padding) and record the working CSS in a comment.
5. `MuPDF outline patch: allocation failure in the index falls back` — spec 6.5. Allocator: the counting allocator, `fz_alloc_context{&state, alloc, realloc, free}`. `state.calls` is reset to 0 right before each `fz_count_pages`, after the open, so N counts that call only. Open and count one warm-up document first (first-use allocations such as font setup), then measure N on a freshly opened small book (4 sections, user ids known, D = distinct ids counted by the builder, generated `'N` ids included) twice and `REQUIRE` equal and `N - 2*D > 0`. `fz_count_pages` always runs inside the test's own `fz_try`, and the error state is read **inside that try body, right after the call**: `fz_try(ctx) { fz_count_pages(ctx, doc); stale = fz_caught(ctx); } fz_catch(ctx) { threw = 1; fz_report_error(ctx); }` with `stale` and `threw` plain `int`s declared `fz_var`, and no C++ (no `REQUIRE`, no destructors) inside the try. Reading it after the try would prove nothing: `fz_catch` runs `fz_do_catch` even when nothing was thrown, and that resets `errcode` to the test frame's code (`context.h:65`, `error.c:278-281`). For each K in `N-2D+1 .. N`: fresh document, arm at K, count pages, disarm; `REQUIRE(threw == 0)`, `REQUIRE(stale == 0)` (an index catch that forgot `fz_report_error` leaves a non-zero code there), then `require_outline_matches_root_walk`. For each K in `1 .. N-2D`: fresh document, arm, count pages; a throw is accepted; no crash. Silence MuPDF's error/warning output for this test with `fz_set_error_callback`/`fz_set_warning_callback` to a no-op.
6. `MuPDF outline patch: page count grows with the book, not its titles` — spec 6.3 speed guard: books of 200 and 800 sections, each a title + 10 paragraphs of 60 words. Pin with `SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{1} << GetCurrentProcessorNumber())` (restore the previous mask at the end); for each size, 3 times: open, `QueryThreadCycleTime` before/after `fz_count_pages`, drop; keep the minimum. `INFO` both values and the ratio, then `REQUIRE(static_cast<double>(c800) / static_cast<double>(c200) < 12.0)` (the counts are `ULONG64`; an integer division would turn 12.5 into 12 and pass). Runs that must record the ratio (Task 2 Step 4, Task 3 Step 5) pass `-s` so Catch2 prints passing assertions with their `INFO`.

- [ ] **Step 3: Register files, build, record RED on the unpatched tree**

Build `litepdf_unit_tests`. Expected: **link fails** with an unresolved external `litepdf_mupdf_patch_notice_v1` (`LNK2001` or `LNK2019`; spec 6.4 second bullet). Record the line.

- [ ] **Step 4: Throwaway stub, record the other RED**

The file is not tracked yet, so `git checkout` cannot restore it: copy `src/core/MuPdfPatchNotice.cpp` to `<scratch>\MuPdfPatchNotice.cpp.good` first. Then make it define the symbol itself, build, run `& .\build\tests\Release\litepdf_unit_tests.exe "[mupdf][outline]" -s` from the repo root. Expected: tests 2, 3, 4 PASS (the unpatched code is the reference itself; test 4's precondition holds), test 5 FAILS (K in the top 2D range are layout allocations on the unpatched build, so `fz_count_pages` throws), test 6 FAILS with a ratio ≈ 20 (spec: ≈ x21). Record the ratio. Restore the file from `<scratch>\MuPdfPatchNotice.cpp.good` and confirm `Select-String -Path src\core\MuPdfPatchNotice.cpp -Pattern '\[\] *='` finds no definition before Step 5 stages it.

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

Above `htdoc_update_outline`: the marker comment block (ASCII only — MuPDF compiles under the system code page: `/* LITEPDF_PATCH_HTDOC_OUTLINE_V1 - applied by litepdf's cmake/ImportMuPDF.cmake; see third_party/mupdf-patches/. One-pass outline resolve, #229. */`), the symbol definition, and these static functions:

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

`htdoc_update_outline(ctx, doc, node)` becomes: `if (!node) return;` `fz_tree *index = NULL; fz_var(index);` build it in `fz_try` with `lp_index_boxes(ctx, &index, ((html_document*)doc)->html->tree.root)`, `fz_catch` → `fz_drop_tree(ctx, index, NULL); index = NULL; fz_report_error(ctx);`. Then `index ? lp_update_outline_from_index(...) : lp_update_outline_root_walk(ctx, doc, node)`; finally `fz_drop_tree(ctx, index, NULL)`. `lp_update_outline_root_walk` is the original loop body with its recursion pointed at itself (`lp_update_outline_root_walk(ctx, doc, node->down)`), not at `htdoc_update_outline`, which would rebuild the index for every child list. `htdoc_layout` and `htdoc_resolve_link` are unchanged.

- [ ] **Step 2: Generate the patch file**

`git -C third_party/mupdf diff --output=<absolute scratch path>\body.patch -- source/html/html-doc.c` (never a PowerShell `>` redirect: PowerShell 5.1 writes a BOM and CRLF, and `git apply` then rejects the file). Write the header (plain text before the first `diff --git` line; `git apply` ignores it: what it changes, why (#229, the O(N^2) outline update), date 2026-11-12, the marker, "drop when upstream resolves the outline in one pass; then remove `MuPdfPatchNotice`, its test, the About line and the html-outline.c hash check") into `<scratch>/header.txt` with the Write tool, and concatenate from Git Bash with absolute paths only: `mkdir -p <worktree>/third_party/mupdf-patches && cat <scratch>/header.txt <scratch>/body.patch > <worktree>/third_party/mupdf-patches/0001-htdoc-one-pass-outline.patch` — never with PowerShell text cmdlets. Check: no BOM (`head -c 3 | xxd`), no CR (`grep -c $'\r'` is 0). Then `git -C third_party/mupdf checkout -- source/html/html-doc.c` and `git -C third_party/mupdf apply --check <absolute patch path>` returns 0, so the CMake block can apply it.

- [ ] **Step 3: CMake block** in `cmake/ImportMuPDF.cmake`, same style as the prune block:

- `set(_LITEPDF_PATCH_VER "LITEPDF_PATCH_HTDOC_OUTLINE_V1")`, `set(_LITEPDF_PATCH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mupdf-patches/0001-htdoc-one-pass-outline.patch")`, `set(_HTML_DOC_C "${_MUPDF_ROOT}/source/html/html-doc.c")`.
- If `_HTML_DOC_C` does not match the marker: `git -C "${_MUPDF_ROOT}" checkout -- source/html/html-doc.c` (FATAL on rc, message per spec 4 step 2, naming the no-`.git` case); `git -C "${_MUPDF_ROOT}" apply --whitespace=nowarn "${_LITEPDF_PATCH}"` (FATAL on rc, spec 4 step 3 message); re-read, FATAL if marker absent; `message(STATUS "MuPDF html-doc.c patched (${_LITEPDF_PATCH_VER}).")`; `file(REMOVE ...mupdf_ext-build)` as the prune block does.
- Every configure: read `${_MUPDF_ROOT}/source/html/html-outline.c`, `string(REPLACE "\r\n" "\n" ...)`, `string(SHA256 ...)`, compare with `set(_HTML_OUTLINE_SHA256_1272 "<value>")`; FATAL on mismatch per spec section 4. Compute `<value>` once with a throwaway `cmake -P` script doing the same three calls on the pristine file, and paste it.
- `set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_LITEPDF_PATCH}" "${_HTML_DOC_C}" "${_MUPDF_ROOT}/source/html/html-outline.c")`, so a changed patch, a reverted `html-doc.c` or a changed `html-outline.c` re-runs configure, and with it the apply and the hash check, on the next `cmake --build`. (The configure step writes `html-doc.c` before generation finishes, so its own write does not cause a second configure.)

- [ ] **Step 4: Configure, build, check the apply**

Expected configure output: `MuPDF html-doc.c patched (LITEPDF_PATCH_HTDOC_OUTLINE_V1).`; second configure prints nothing for it (marker present). `git -C third_party/mupdf diff --stat` shows only `source/html/html-doc.c` (plus the existing vcxproj/config.h rewrites), and `git -C third_party/mupdf diff -- source/html/html-doc.c` changes nothing outside `htdoc_update_outline` and the new code above it (`htdoc_resolve_link`, `htdoc_layout` and the handler table untouched; Review Focus 4). A second `cmake --build` with nothing changed does not re-run configure. Check line endings of the patched file (`file third_party/mupdf/source/html/html-doc.c`) and note mixed/CRLF in the PR.

- [ ] **Step 5: Run the tests — GREEN**

`& .\build\tests\Release\litepdf_unit_tests.exe "[mupdf][outline]" -s` from the repo root: all 6 pass; record test 6's ratio (≈ 4).

- [ ] **Step 6: Hash-check falsifier**

Append a blank line to the private tree's `html-outline.c` and run `cmake --build build --config Release --target litepdf_unit_tests` (not an explicit configure): the build re-runs configure through `CMAKE_CONFIGURE_DEPENDS` and stops with the spec 4 FATAL. `git -C third_party/mupdf checkout -- source/html/html-outline.c`, build again: passes.

Then the release tarball's case, a tree with no `.git` that already carries the marker: copy the worktree to `<scratch>\nogit\` with `robocopy <worktree> <scratch>\nogit /E /XD .git build /XF .git` (no repo above the scratch folder), and run only the configure there (`cmake -S <scratch>\nogit -B <scratch>\nogit\build -G "Visual Studio 17 2022" -A x64` with the same Catch2 options as Task 1). Expected: no "patched" line (marker present, the `git` calls are skipped), the hash check passes, configure succeeds. Delete `<scratch>\nogit` afterwards.

- [ ] **Step 7: Mutants (spec 6.4), each a throwaway edit of the private `html-doc.c` that keeps the marker. A mutant edit does not trigger a MuPDF rebuild by itself (`mupdf_ext` is `BUILD_ALWAYS OFF` and rebuilds only when its stamp is missing), so for each one: edit, delete `build/mupdf_ext-prefix/src/mupdf_ext-stamp/Release/mupdf_ext-build`, `cmake --build build --config Release --target litepdf_unit_tests`, confirm the log shows MSBuild compiling `html-doc.c` and `libmupdf.lib`'s timestamp moved, run `"[mupdf][outline]"`; afterwards `git -C third_party/mupdf checkout -- source/html/html-doc.c`, reconfigure (re-applies the patch and deletes the stamp) and rebuild:**

| Mutant | Must fail |
|---|---|
| `lp_put_first` inserts without the lookup | test 2 (duplicates). If it passes, change the fixture (spec 6.1) until it fails, and record the construction |
| the `*y >= 0` branch also takes negative values | test 4 |
| `lp_index_boxes` skips an id'd box whose `lp_find_first_content` is NULL | test 2 (`empty-title`) |
| the `fz_catch` without `fz_report_error` | test 5 (`stale == 0`, read inside the test's try body) |
| the `fz_catch` rethrows | test 5 (index range) |
| the `if (!node) return;` guard removed | test 3 (allocation counts) |

Record each result.

- [ ] **Step 8: Full suite**

`cmake --build build --config Release` (whole tree) then `ctest --test-dir build -C Release --output-on-failure` serially from PowerShell. Expected: all pass (`ctest -N` count + 6).

- [ ] **Step 9: Commit**

```bash
git add third_party/mupdf-patches/0001-htdoc-one-pass-outline.patch cmake/ImportMuPDF.cmake
# and tests/unit/test_mupdf_outline_patch.cpp if Step 7 changed the fixture
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

- [ ] **Step 4: Build the app and check the About text** — `cmake --build build --config Release --target litepdf`. LitePDF is single-instance (`Local\LitePDF_SingleInstance_v1`, `src/app/SingleInstance.cpp`): a launch while any LitePDF runs, the user's installed one included, forwards its argument there and exits. So first `Get-Process litepdf -ErrorAction SilentlyContinue`: if one runs that this task did not start, ask the user to close it instead of killing it. Launch `build\Release\litepdf.exe`, open Help → About, the notice line shows; a screenshot is the evidence.

- [ ] **Step 5: Commit**

```bash
git add src/ui/MainWindow.cpp installer/make-license-rtf.py installer/LICENSE-DISPLAY.rtf CHANGELOG.md
git commit -m "docs: say MuPDF is carried with one local change (#229)"
```

## Task 5: Before/after measurements

No new code. Results go into one CHANGELOG sentence (the entry Task 4 wrote) and the PR body.

- [ ] **Step 1: CLI series** — for N in 200, 800, 1500; 3 rounds; within each round run `before` then `after` (`build\Release\litepdf-cli.exe`), each as `cmd /c "start /B /affinity FFFF /wait <exe> <book> --benchmark --iterations 1" > <out>` (P-cores are logical 0-15 on this i7-12700; `/B` keeps the console output in the redirect; PowerShell's own `start` is `Start-Process` and takes no `/affinity`), with the book read once before the run. Record best-of-3 `open_ms` and `render_ms` per exe and size. The benchmark gives the first render 10 s (`src/cli/main.cpp`); the unpatched 1,500-section render took 6.9 s in #229, so a "before" run may time out: record that as "render > 10 s", keep its `open_ms`, and rely on Step 2 for the first page.

- [ ] **Step 2: GUI first page** — for N in 800, 1500; 3 rounds alternating before/after. Before every launch: no `litepdf` process may run (single-instance gate, see Task 4 Step 4; after closing one, wait for it to exit, `WaitForExit(5000)`, before the next launch). Launch with `$p = Start-Process -FilePath <exe> -ArgumentList '--log-timings', '"<book>"' -RedirectStandardError <err> -PassThru` (with `$env:LITEPDF_NO_RESTORE = '1'` set first) and straight after it `$p.ProcessorAffinity = 0xFFFF` (the few ms before it lands are startup, far below the seconds measured); wait for the `T0->T4=` line in `<err>` (deadline 120 s, failing if `$p.HasExited` first), then `$p.CloseMainWindow()` and `$p.WaitForExit(5000)` on that same object, with `Stop-Process -Id $p.Id` if it has not exited. Record best-of-3 `T0->T4` (spec 7) and keep all values for the PR.

- [ ] **Step 3: Write the numbers** into `CHANGELOG.md` (1,500 sections, `open_ms` before → after) and keep the full table for the PR body in `<scratch>\229-measurements.md`. Commit the CHANGELOG change:

```bash
git add CHANGELOG.md
git commit -m "docs(changelog): FB2 open times before and after the outline patch (#229)"
```

## After the plan

Rebase on `origin/main` if it moved; PR-merge gate (risk-tiered review, Full: cmake/native chain); PR referencing #229 without "closes" (PR 2 and maybe PR 3 follow); before merging, tell the user that the first configure on main through a junction patches the shared checkout and rebuilds its `libmupdf.lib`. Teardown after merge: the private `third_party\mupdf` is a plain `--shared` clone with copied files and no junctions; delete it and recreate the empty `third_party\mupdf` directory. Nothing to clean in the shared repo.
