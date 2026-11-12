# One-pass outline resolve, carried as a local MuPDF patch (#229 PR 1) — design

Status: design agreed in chat 2026-11-12 after two independent reviews of the scope and of this design; the user
asked for this written spec before any code.
Spec gate (Full tier): round 1 (Opus + Sonnet) and the Codex lens (terra, gpt-6.1-terra, luna) fixed in place. The
main changes: the negative-y CSS, the empty-heading fixture, the allocation-failure test, the versioned link symbol,
and the `html-outline.c` hash check.
Issue: #229 (body + 3 comments hold the measurements). Base: main at 80d4f4a.
Scope decided with the user: this PR is the MuPDF patch only. PR 2 = one render worker for FB2. A lazy
`SearchSession` is decided after this PR is measured (only for documents that need no password, since #130 records
"opened in the ctor, not lazily"). Sharing one laid-out document across contexts goes to a new issue. A "still
loading" indicator is declined: after this fix the 1,500-section first page is estimated at about 1.7 s. Reporting
the outline cost upstream (bugs.ghostscript.com) is separate from this PR: the user files it, from a draft reviewed
with them.

## 1. Problem

A large FB2 opens slowly, and the cost grows with the square of the number of section titles. MuPDF 1.27.2 lays an
HTML-engine document out in `htdoc_layout` (`third_party/mupdf/source/html/html-doc.c`):

```c
static void
htdoc_layout(fz_context *ctx, fz_document *doc_, float w, float h, float em)
{
	html_document *doc = (html_document*)doc_;

	fz_layout_html(ctx, doc->html, w, h, em);

	htdoc_update_outline(ctx, doc_, doc->outline);
}
```

`htdoc_update_outline` calls `htdoc_resolve_link` once per outline node, and each call runs `fz_find_html_target`
(`source/html/html-outline.c`), which walks the box tree from the root, every flow node included, until it meets the
id. An FB2 has one outline node per section title, so N titles cost O(N^2) flow visits.

Measured in #229 (probe timing each phase, 1,500 sections, 6.5 MB): parse 241 ms, `fz_layout_html` 263 ms, the
outline update 7,240 ms. The update is 78-95 % of `fz_count_pages` at every size. Every copy of the document pays it
on its first layout: `Document` at open, each render worker before the first page, `SearchSession` at the first
search. GUI first page: 16.5 s with 1 worker, 22.7 s with 2.

There is no API to skip it: the update runs inside the document's own layout callback, and `fz_load_outline` forces a
layout first. MuPDF master (2026-11-12) still does one root walk per node. So the fix is a patch to MuPDF, carried
until upstream fixes it.

## 2. Goal and non-goals

Goal: `fz_count_pages` on a laid-out FB2 costs about the layout alone, so its time grows with the book, not with the
square of its titles. Every outline node gets exactly the page and y it gets today.

Non-goals:

- Any litepdf-side change to how many copies of a document exist (PR 2, the new issue).
- `htdoc_resolve_link` itself, used when a link is followed (`resolve_link_dest`). It stays a root walk: one walk per
  click is fine, and it is the reference the tests compare against.
- EPUB: its outline comes from the NCX or nav file and is resolved only when followed (#229 comment 1). HTML, XHTML
  and MOBI take the same `htdoc` path but are disabled in litepdf's build (`FZ_ENABLE_HTML 0`, `FZ_ENABLE_MOBI 0`).

## 3. The patch (`source/html/html-doc.c` only)

### 3.1 Index

`htdoc_update_outline` builds an index, then resolves each node from it. The index is an `fz_tree` (string keys, no
length limit; `fz_hash_table` has a 48-byte key limit, which would leave long ids on the slow path) of the ids the
outline points at, nothing else (PR-gate change: indexing every id in the tree would make a book with a few titles and
an id on every paragraph pay an entry per paragraph):

- Built only when the outline is non-empty.
- First, one slot array (one slot per outline node, an upper bound) and the tree of wanted ids: each node's target id
  (`strchr(uri, '#')`, non-empty after it), inserted once, its value the address of its own slot. These are the
  index's only allocations: one array plus two per distinct wanted id.
- Then one pre-order walk of `html->tree.root`, in exactly the order of `find_box_target`, that records the first
  match of each wanted id in its slot and ignores every other id. It stops as soon as every wanted id has a match;
  it allocates nothing. The order:
  1. the box's own id, if any. Value: the address of `find_first_content(box)->y` when that returns a flow, else the
     address of `box->s.layout.y`;
  2. if the box is `BOX_FLOW`: each flow of `box->u.flow.head` in order. Key `flow->box->id` when non-NULL, value
     `&flow->y`;
  3. otherwise: recurse into `box->down`;
  4. then `box->next`.
- `find_first_content` is `static` in `html-outline.c`, so the patch has its own copy. It follows the `down` chain
  of the id'd box until a `BOX_FLOW`, so the walk costs O(boxes + ids × that chain's depth). A chain of N nested
  id'd boxes would make it quadratic in N; real documents nest a few levels, and MuPDF's own layout recursion is
  as deep as that chain. Accepted, not optimised.
- Each wanted id is inserted once (`fz_tree_lookup` first: `fz_tree_insert` sends an equal key to the right), and a
  slot is filled only while it is still empty, so it holds the first match in walk order, the one the root walk
  would reach first.
- A slot holds a `float *` into the laid-out tree; an empty slot (NULL) means "no match", without ambiguity (a stored y
  of exactly 0.0f is still a match). Each tree entry costs two allocations, the node and `fz_strdup` of its key
  (`tree.c:36-41`); the tree is dropped with `fz_drop_tree(ctx, tree, NULL)` and the array with `fz_free`. The
  pointers are only read inside this call, while the layout that wrote them is current.

### 3.2 Resolving each node

For each outline node (recursion into `node->down` unchanged):

- Parse `node->uri` as `htdoc_resolve_link` does: `strchr(uri, '#')`; no `#`, or nothing after it, gives
  `fz_make_link_dest_none()`.
- Look the id up.
  - Absent: `fz_make_link_dest_none()` (the root walk returns -1 for an absent id, which gives the same).
  - Present, `*value >= 0`: `int page = y / page_h;` then `fz_make_link_dest_xyz(0, page, 0, y - page * page_h, 0)`,
    the same arithmetic as `htdoc_resolve_link`.
  - Present, any other value (negative, or NaN, which fails `>= 0` as it fails the root walk's `y >= 0`): call
    `htdoc_resolve_link(ctx, doc, node->uri)`. The code tests `>= 0` and sends everything else to the fallback; it
    never tests `< 0`.

Why the fallback is needed and sufficient. `find_box_target` returns the first match it reaches; a parent passes a
child's result up only when it is `>= 0`, and `find_flow_target` returns the first matching flow of a list even when
negative. So when the first match in walk order has y >= 0, the root walk returns exactly it, at any depth. When it is
negative, the root walk may go on to a later match. The fallback is the original code, so the result is identical in
that case too, at the original cost, which is fine for a case that needs a negative layout position.

Generated ids (`'1`, `'2`, ... from `add_html_outline`) can coincide with a user id; both sides take the first in walk
order, so this does not change the result.

### 3.3 Failure

The index build runs in `fz_try`, with the tree pointer declared `fz_var`. On any throw (allocation), the tree built
so far is dropped and every node is resolved with `htdoc_resolve_link`, the code as it is today. Today the outline
update cannot fail; it must not start failing the page count, and through it the open.

The `fz_catch` calls `fz_report_error(ctx)`. A catch that neither reports, ignores nor converts the error leaves
`errcode` set, and the next throw on that context prints "UNHANDLED EXCEPTION!" (`error.c:334-339`). litepdf's rule
since #146 is that every `fz_catch` that does not rethrow clears it this way. Upstream's own catch in
`fz_load_html_outline` does not, so this is litepdf's policy applied to litepdf's patch.

### 3.4 Marker and notice

- The patched file carries a marker comment `LITEPDF_PATCH_HTDOC_OUTLINE_V1`. Any change to the patch bumps the
  number, so the configure step (section 4) re-applies it.
- The patch defines one external symbol:
  `const char litepdf_mupdf_patch_notice_v1[] = "Modified by litepdf on 2026-11-12: one-pass outline resolve (#229)";`
  It serves two purposes: the AGPL-3.0 section 5(a) notice that MuPDF was modified and when, and a link-time proof
  that the patched `html-doc.c` is in the linked library (section 5.1). The symbol's name carries the marker's
  version and is renamed with it (`_v2`, ...), so a library built from an older patch level also fails the link, not
  only an unpatched one.
- The patch file starts with a header saying what it changes, why, the date, and that it is to be dropped once
  upstream fixes the outline update.

## 4. Applying it (`cmake/ImportMuPDF.cmake`)

- The patch lives at `third_party/mupdf-patches/0001-htdoc-one-pass-outline.patch`, LF (`.gitattributes`
  `* text=auto eol=lf`), generated with `git diff -- source/html/html-doc.c` from the MuPDF root so paths are
  `a/source/html/html-doc.c`.
- New block after the `config.h` prune block, same shape:
  1. Read `source/html/html-doc.c`. If it contains the current marker, do nothing.
  2. Else, unless the file carries an earlier version of this marker, `git diff --quiet` on it: local changes that
     are not this patch (someone's experiment in the shared checkout) are a FATAL_ERROR asking to save or revert
     them, never silently discarded (PR-gate change). Then `git -C <mupdf root> checkout --
     source/html/html-doc.c`. Non-zero rc: FATAL_ERROR, saying the file is not the current patch level and could not
     be restored, which also happens in a tree with no `.git`.
  3. `git -C <mupdf root> apply --whitespace=nowarn <absolute patch path>` (a relative path is resolved against
     `-C` and not found). Non-zero rc: FATAL_ERROR, saying MuPDF's `html-doc.c` no longer matches the patch (most
     likely a MuPDF bump) and the patch must be re-based or dropped.
  4. Re-read the file; FATAL_ERROR if the marker is still absent (do not trust the exit code alone).
  5. Delete the `mupdf_ext` build stamp, as the prune block does, so MSBuild rebuilds.
- Each build directory also records, in `mupdf-patch-level.txt`, the patch version its MuPDF was last built for; a
  different version deletes that directory's stamp (PR-gate change). Step 5 alone only reaches the build directory
  whose configure applied the patch, while worktrees share one MuPDF tree through a junction.
- A second check, on every configure, outside the marker guard: the patch copies `find_first_content` and the value
  rules of `find_box_target` / `find_flow_target` from `source/html/html-outline.c`, a file the patch does not touch.
  Upstream master has already changed `find_flow_target` (it returns `find_flow_top(flow)`, which skips anchors and
  adjusts the baseline), so a MuPDF bump could keep `html-doc.c` applying cleanly while the copied rules go stale.
  The block reads `html-outline.c`, normalises CRLF to LF, and compares its SHA-256 with the 1.27.2 value recorded in
  `ImportMuPDF.cmake`. A mismatch is a FATAL_ERROR saying the copied rules must be re-checked against the new
  `html-outline.c` and the recorded hash updated. The equivalence test (6.1) is the second guard.
- Line endings: the working file is CRLF under `core.autocrlf=true`; `git apply` converts the preimage, and an LF
  patch applied with `--check` to the shared checkout's CRLF file returns 0 (git 2.53). The CI runner's behaviour is
  proven by the PR's first CI run, not before.
- Release: `git-archive-all` packs working-tree files, so the source tarball carries the patched `html-doc.c` with
  its marker, and the tarball build-back (no `.git`) skips the block. The tarball is then the source that was built.
- Rejected: a CMake `string(REPLACE)` that injects multi-line C (fragile against CRLF and upstream edits); a vcxproj
  edit compiling a replacement `html-doc.c` (heavier, and diverges from upstream's file list).

## 5. litepdf changes

### 5.1 Notice in the app and the licence text

- A small core accessor, `litepdf::core::mupdf_patch_notice()` (new `src/core/MuPdfPatchNotice.{hpp,cpp}`), returns
  `litepdf_mupdf_patch_notice_v1` (declared `extern "C"`). The About box adds its text on the line after
  `Engine: MuPDF 1.27.2` (`src/ui/MainWindow.cpp`), through `ascii_to_wide`, and `litepdf-cli --version` prints it
  as a `mupdf=` line. Because the app and the CLI reference the symbol, a `libmupdf.lib` built without the patch fails
  their links, not only the tests' (so a benchmark cannot measure the old code).
- `installer/make-license-rtf.py`: the MuPDF line notes the modification, in the file's own language (zh-TW), with
  the date and the patch folder. The generated `installer/LICENSE-DISPLAY.rtf` is regenerated and committed in the
  same PR: its sync gate runs only in `release.yml`, so a stale `.rtf` passes PR CI and fails the next release.
- `MuPdfPatchNotice.cpp` joins the explicit `litepdf_core` source list (`CMakeLists.txt`), and the new test file the
  `tests/CMakeLists.txt` list; neither uses a glob.
- `CHANGELOG.md` `[Unreleased]` `### Fixed`: one entry for the faster FB2 open, saying MuPDF is now carried with one
  local change.

### 5.2 Nothing else

No change to `Document`, `DocumentView`, `RenderEngine` or `SearchSession`.

## 6. Tests

New `tests/unit/test_mupdf_outline_patch.cpp`, raw MuPDF API on its own `fz_context`, FB2 generated in memory and
opened with `fz_open_document_with_stream(ctx, "application/x-fictionbook", stream)` after
`fz_register_document_handlers(ctx)` (a fresh context has none, `document.c:242-245`). ASCII test names prefixed
`MuPDF outline patch:`.

### 6.1 Equivalence

One generated FB2 covering:

- nested `section`s with `title`s and `subtitle`s, each inside a `section` (only these two become headings in an
  FB2, `html-parse.c:1328-1331`, at level `section_depth`, so not directly under `body`; `h1`-`h6` would be headings
  too, `:1314-1326`, but heading levels are set only for block boxes, `:1384-1386`, and FB2's default CSS makes no
  `h1`-`h6` a block, `:108`; only headings become outline nodes, `html-outline.c:409-411`);
- titles without ids (generated `'N` ids);
- an id longer than 48 characters on a title;
- an empty heading, `<title id="…"/>`, whose box has no flow content, so its value is `box->s.layout.y` (the
  `find_first_content` NULL path). An empty `section` would not do: it is no heading, so no outline node names it;
- duplicates: about 20 `<title id>`s each repeating the id of an earlier `<p id>` (the first match is the
  paragraph), with the duplicated ids sorting in the middle of about 200 other user ids. With one duplicate and
  only generated `'N` ids around it (they sort below letters), `fz_tree` happens to keep the first insert even
  without the lookup-before-insert, so such a fixture cannot tell the lookup is there. Whether a given insertion
  sequence exposes the mutant depends on the tree's rotations, so the fixture is not trusted on its shape: 6.4 runs
  the no-lookup mutant against it, and the fixture is adjusted (more duplicates, three or more repeats of one id,
  other key orders) until that mutant fails. The final construction is recorded in the test.

After `fz_count_pages` and `fz_load_outline`, for every node: `node->page` (as a location) and `node->y` equal
`fz_resolve_link_dest(ctx, doc, node->uri)`, which still runs the root walk. Fixture-proving assertions first, all
from the public API: the node count; at least one node with page > 0; and each duplicate-id title resolves to its
paragraph, which the fixture puts at least one page before the title (so "resolves to page < the title's own page",
with the title's page known from a second, unique id on the title's following paragraph).

### 6.2 Negative y (the fallback)

MuPDF collapses a first child's top margin into its parent with `fz_max` unless that child itself has a top border
or padding (`html-layout.c:3055`); a later sibling's top margin collapses with the previous sibling's bottom margin
the same way (`:3086-3090`). The parent's padding plays no part. So the test's user CSS gives the heading itself a top
padding and a negative top margin. A selector such as `section>title` would hit every title, so the test's book has
exactly one `subtitle`, the first child of the first section, and the CSS is `subtitle{padding-top:1px;
margin-top:-200px}`. That subtitle carries an id which a paragraph in the next section, several pages later,
repeats. Everything after the subtitle moves up by the same 200 pt, so that paragraph stays at y > 0.

Two facts found while writing the test (Task 2) shape it. The subtitle is empty: a heading with text resolves to its
first line, and MuPDF places a line that would start above the page at the page's top, so only a heading without
text keeps its box's negative y. And the repeat is in the next section, not the same one: `find_box_target` returns
an own-id match, negative or not, for the whole sibling list it is in, and only a parent skips a negative result
and goes on (a same-section repeat resolves to "none").

- Precondition, asserted from the public API: `fz_resolve_link_dest` for that id lands on the later paragraph's page
  (the root walk skipped the negative first match). If the CSS does not reach y < 0, the id resolves to page 0 and
  the test fails here, not silently.
- Then the equivalence check of 6.1 on this document. A wrong implementation that returns the negative first y fails
  it.
- The exact CSS is found while writing the test and recorded in it.

### 6.3 The patch is in the binary

- Link-time: the test references `litepdf_mupdf_patch_notice_v1` through the accessor and checks its text. An unpatched
  library fails the test executable's link (and the app's, 5.1). Deterministic.
- Speed guard (regression, not the stale-lib oracle): 200 vs 800 sections of the probe's density (a title plus 10
  paragraphs of 60 words each, about 1,200 flow nodes per section; sparser sections shrink the unpatched ratio
  toward the per-document fixed cost), `fz_count_pages` on a fresh document each
  time (open excluded), thread pinned to one core with `SetThreadAffinityMask` for the measurement, cost read with
  `QueryThreadCycleTime`, minimum of 3 per size. Ratio 800/200 must be < 12. From the probe's table, layout plus
  outline update goes 35 + 83 = 118 ms -> 142 + 2,356 = 2,498 ms over the same step (about x21) unpatched; patched it
  is the layout alone, about x4. Cycles on one pinned core remove the P-core / E-core split
  (`reference_litepdf_bench_machine_noise`).
- Runtime: about 1-2 s patched.

### 6.4 Proving the tests can fail

Before the CMake block exists on the branch, the unpatched private build (section 7) runs:

- the test file with the accessor temporarily stubbed out (a throwaway local edit, never committed): 6.3's speed
  guard must fail (ratio about x21);
- the test file as written: the link must fail on `litepdf_mupdf_patch_notice_v1`.

After the patch exists, mutants of the patch (throwaway, never committed), each of which the named test must fail:

- no lookup before insert: 6.1's duplicate assertions;
- a negative first y used directly instead of the fallback: 6.2;
- the `box->s.layout.y` value dropped (an id'd box with no flow content skipped): 6.1's empty-heading node;
- the `fz_catch` without `fz_report_error`: 6.5's error-state check;
- the catch rethrowing instead of falling back: 6.5's index-allocation range.

All results are recorded in the PR.

### 6.5 The fallback under allocation failure

A context built with a custom `fz_alloc_context` that counts allocation calls and, once armed at K, fails the K-th
call and every later one until disarmed. Failing only the K-th call is not enough: on a NULL return MuPDF scavenges
its store and calls the allocator again (`memory.c`, `do_scavenging_malloc`), so a single failure can be absorbed
without any throw reaching the index's catch.

A first, non-failing run of `fz_count_pages` on a small fixture counts the allocation calls N of that call. The index
build is the last thing in the call that allocates: one slot array, then two allocations per distinct wanted id (node
and key copy, `tree.c:36-41`). The fixture's wanted ids W are known from its construction (one generated `'N` id per
heading without an id; paragraph ids are not wanted), so the last A = 1 + 2W calls are the index's. Below, read
"N - 2D" as N - A.

- Every K in N - 2D + 1 .. N (any index allocation fails, and so does everything after it): `fz_count_pages`
  succeeds, every outline node equals the reference of 6.1, and `fz_caught(ctx)` is 0 when read inside the test's
  own `fz_try` body, right after the call (the patch's catch cleared the error). It must be read there: the test's
  own `fz_catch` runs `fz_do_catch` even when nothing was thrown, which resets `errcode` (`context.h:65`,
  `error.c:278-281`), so a read after the try proves nothing. The fallback itself allocates nothing, so it runs
  under the armed allocator.
- Every K in 1 .. N - 2D on a fresh document: no crash. Those are layout allocations, and `fz_count_pages` throws,
  as it does today.
- Fixture-proving: in the non-failing run, N - 2D is greater than 0 and the count is the same on a second run.

## 7. Development and measurement

- The shared checkout `C:\Users\User\projects\litepdf\third_party\mupdf` is not modified before merge. This worktree's
  `third_party/mupdf` (empty today) becomes a `git clone --shared` of the shared submodule repo (objects borrowed
  through alternates, nothing written to the shared repo), detached at 73d3100, plus a copy of the
  shared `thirdparty/` made with `robocopy /E /XF .git`: the nested submodules' `.git` files hold relative gitdir
  paths that would point nowhere from the new location and break git there. Copy, not junctions, so removing the
  worktree later cannot delete through a junction. (A `git worktree add` from the shared submodule gitdir was the
  first idea; it is avoided because that gitdir sets a relative `core.worktree`.)
- Order: configure and build this tree at 80d4f4a (unpatched; the configure rewrites the vcxproj and config.h in the
  private tree only), copy the unpatched `litepdf-cli.exe` and `litepdf.exe` aside (both link MuPDF statically, so a
  copy is self-contained), run 6.4's unpatched half, then add the CMake block and the patch and rebuild. Before and
  after are then measured from the two sets of executables, interleaved run by run. The full MuPDF build runs in the
  background.
- After the first apply, check the patched file's line endings (`git apply` under autocrlf may write the added lines
  as LF in a CRLF file). Mixed endings do not matter to the compiler or the marker check, but the patch is always
  regenerated from a clean `git diff`, never from a hand-edited file.
- Measurements, before and after on the same base and machine, interleaved, pinned to the P-cores, best of 3:
  `litepdf-cli <book> --benchmark` at 200 / 800 / 1,500 sections (`make-fb2.py`, seed 169, 10 paragraphs per
  section), and the GUI first page (`--log-timings` T0 -> T4) at 800 and 1,500 sections.

## 8. Risks

- After merge, the first configure on main in a worktree whose `third_party/mupdf` is a junction patches the shared
  checkout and rebuilds the shared `libmupdf.lib` in place. Another session linking it at that moment may fail its
  link once. To be said in the PR and to the user before merging.
- Worktrees on older branches then build against the patched library. Harmless: the outline results are identical.
- Two build directories sharing one MuPDF tree: the stamp is per build directory, so one directory can link a library
  built before the patch, or before its current version, was applied. The versioned link-time symbol (3.4, 6.3)
  turns either into a link error instead of a silently stale build.
- A MuPDF bump: the configure fails until the patch is re-based or dropped (section 4 step 3), or until the copied
  rules are re-checked against a changed `html-outline.c` (the hash check in section 4). The equivalence test (6.1)
  is the guard behind both. Dropping the patch once upstream fixes the update also means removing the symbol's users
  (5.1, 6.3) and the hash check.
