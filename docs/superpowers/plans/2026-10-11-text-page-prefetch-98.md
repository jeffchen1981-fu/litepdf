# Text-page prefetch on the render worker (#98) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A page's first text-selection press or Select All no longer builds the page's structured text on the UI thread once the page has been shown; the render worker builds it on its own document and hands it over.

**Architecture:** One shared builder (`detail::TextPageAccess::build`) serves both `Document::text_page` (UI fallback, under `doc_mutex`) and the render worker (its own `fz_context`/`fz_document`, no lock). The worker builds after posting the pixmap, skips the build when it could delay newer work, and the handle reaches the UI by `WM_USER_TEXT_READY`, where `DocumentView` keeps one shareable handle per tab.

**Tech Stack:** C++20, Win32, Direct2D, MuPDF 1.27.2 (vendored), Catch2, CMake (VS 2022 BuildTools), Python 3 for the fixture generator.

**Spec:** `docs/superpowers/specs/2026-10-10-text-page-prefetch-98-design.md` (§ references below are to it). Read it before any task.

## Global Constraints

- Branch `perf/98-text-page-prefetch`, base `main` @ `049d27d`. If `origin/main` has moved, rebase before Task 1 and re-run the baseline.
- **#126 is in flight** (worker documents authenticate; changes the `DocumentView` and `RenderEngine` constructors). Before Task 1 and before Task 7: `git fetch origin && git log --oneline origin/main | grep -i "#126"`. If it has merged, rebase onto it first, then run Task 8 after Task 7.
- Build the tests in **Release** only (Debug fails with LNK2038). Use the full BuildTools paths, never a bare `cmake`/`ctest`:
  - `& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build C:\Users\User\projects\litepdf\build --target litepdf_unit_tests --config Release`
  - run tests **from the repo root, in PowerShell**: `build\tests\Release\litepdf_unit_tests.exe "<tag or name>"`; the full suite is `ctest --test-dir build -C Release` with the same BuildTools `ctest.exe`.
- Do not hard-code the ctest count anywhere; read `ctest -N` when a number is needed.
- New test files register in `tests/CMakeLists.txt` under `target_sources(litepdf_unit_tests PRIVATE ...)` with a trailing `# #98` comment.
- `TEST_CASE` names are ASCII and start with their subsystem (`DocumentSelection`, `RenderEngine`, `DocumentView`, `TextPrefetch`).
- `src/core/Document.hpp`, `src/core/RenderEngine.hpp` and `src/core/DocumentView.hpp` stay MuPDF-free (no `fitz.h`, no `fz_*` type except the existing `fz_context`/`fz_pixmap` forward declarations).
- Builder options are `fz_stext_options opts = {}` — never `FZ_STEXT_COLLECT_STRUCTURE` or `FZ_STEXT_DEHYPHENATE`, and never `fz_new_stext_page_from_display_list`.
- `WM_USER_TEXT_READY = WM_USER + 5`.
- All `text_wanted` writes are `->store(v, std::memory_order_relaxed)` on the UI thread; the `shared_ptr` is never reassigned.
- Every artifact (code, comments, commits, test names) in English; match the surrounding comment density.
- Commit per task; message ends with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

1. **Tab closed while its text build is in flight** — expected: no crash, no leak; the handle dies through its own escrow. Pinned by Task 5's lifetime test and Task 7's close-storm case.
2. **Held PgDn / repeated zoom before the first text lands** — expected: the page the reader stops on gets correct text; pages passed through do not delay bitmaps. Pinned by Task 4's queued-P0 test and Task 7's paging-storm case.
3. **A page that renders but has no text (CBZ, image-only, blank)** — expected: same as today (no range, nothing copied). Pinned by Task 4's equivalence oracle (`selection.pdf` page 2 is blank; `sample.cbz` page 0).
4. **A damaged-but-openable PDF** (the #119 inflated `/Count` copy) — expected: the worker-built text equals the Document's. Pinned by Task 4's equivalence oracle on that file.
5. **Spread mode toggled while a build is in flight** — expected: no prefetch is requested in spread mode, and a late handle never causes a selection there. Pinned by Task 5's gate test.

---

## Task 1: Type3 and annotation fixtures, and today's Type3 lifetime

Closes the coverage gap the review found (no fixture has a Type3 font) on today's code, and gives Task 4 the annotation fixture its non-vacuity check needs.

**Files:**
- Create: `scripts/generate-prefetch-fixtures.py`, `tests/fixtures/type3.pdf`, `tests/fixtures/annot.pdf`
- Modify: `tests/unit/test_document_selection.cpp` (new cases at the end)

**Interfaces:**
- Produces: `tests/fixtures/type3.pdf` (1 page, body text exactly `ABBA` drawn with a Type3 font), `tests/fixtures/annot.pdf` (1 page, body text exactly `Body text` in Helvetica, plus one FreeText annotation whose `/AP /N` stream draws `Annotation text`).

- [ ] **Step 1: Write the generator.** `scripts/generate-prefetch-fixtures.py`, standard library only, writes both files as literal uncompressed PDF objects with a correct xref (compute byte offsets while writing). Follow the header-comment style of `scripts/generate-selection-fixture.py` (what each page is for, how to run it). `type3.pdf`: a `/Type3` font with `/FontMatrix [0.001 0 0 0.001 0 0]`, `/CharProcs` for glyph names `a` and `b` (each a `d1` + filled rectangle of different widths), `/Encoding << /Differences [65 /a /b] >>`, `/FirstChar 65 /LastChar 66`, `/Widths`, and content `BT /T3 24 Tf 72 700 Td (ABBA) Tj ET`. `annot.pdf`: Helvetica body `Body text` at 72 700; one `/FreeText` annotation with `/Rect [72 600 300 640]` and an `/AP << /N <form XObject> >>` whose stream draws `Annotation text` with its own `/Resources` Helvetica. Both deterministic: no timestamps, no IDs.
- [ ] **Step 2: Generate and commit-stage the fixtures.** Run: `python -I scripts/generate-prefetch-fixtures.py` from the repo root. Run it twice and `cmp` the outputs: identical bytes.
- [ ] **Step 3: Write the tests** in `tests/unit/test_document_selection.cpp`:
  - `DocumentSelection extracts Type3 text`: `doc.text_page(0)` on `type3.pdf` is valid; `copy(full_range)` with whitespace removed `== "ABBA"`.
  - `DocumentSelection a Type3 text page outlives its Document`: mirror the existing `DocumentSelection a text page outlives its Document` case (line ~325) on `type3.pdf`, page 0, and additionally hold an `EscrowContext` probe (recipe: `test_document_view.cpp` "DocumentView drops its context clones when a member ctor throws" — `clone_from(doc.clone_context())`, drop the temp clone). Assert: after the Document's scope ends, `copy(full_range)` still yields `ABBA`; `family_context_count(probe.get())` falls by exactly 1 when the handle is reset; after the probe is reset, `live_mupdf_roots()` equals its value read before the Document was opened.
  - `DocumentSelection skips annotation text`: on `annot.pdf`, `copy(full_range)` contains `Bodytext` (whitespace removed) and does not contain `Annotation`.
  - `DocumentSelection the annotation fixture draws its annotation text` (fixture validity, so Task 4's mutant check is not vacuous): with a test-owned `fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT)` + `fz_register_document_handlers`, open `annot.pdf`, `fz_new_display_list_from_page` on page 0, then `fz_new_stext_page_from_display_list(ctx, list, nullptr)` and `fz_new_buffer_from_stext_page`: its text contains `Annotation text`. Drop everything in reverse order. The unit-test target already has MuPDF's include path (see `test_mupdf_root.cpp`).
- [ ] **Step 4: Run.** `build\tests\Release\litepdf_unit_tests.exe "[selection]"` — all pass. These pin today's behaviour, so they pass immediately; the fixture-validity case is what proves `annot.pdf` can tell the two builders apart.
- [ ] **Step 5: Commit** fixtures, generator and tests: `test: Type3 and annotation fixtures for #98, and a Type3 text page outliving its Document`.

## Task 2: One builder, and a shareable TextPage

Pure refactor plus `share()`. No behaviour change.

**Files:**
- Create: `src/core/detail/TextPageAccess.hpp`
- Modify: `src/core/Document.hpp:228-309`, `src/core/Document.cpp:630-654` (Impl moves out), `src/core/Document.cpp:686-689` (special members), `src/core/Document.cpp:823-892` (`text_page`)
- Test: `tests/unit/test_document_selection.cpp`

**Interfaces:**
- Produces:
  - `namespace litepdf::core::detail { struct TextPageAccess { static Document::TextPage build(fz_context* ctx, fz_document* doc, int index) noexcept; }; }` in `src/core/detail/TextPageAccess.hpp` (includes `<mupdf/fitz.h>`; also holds the moved `struct Document::TextPage::Impl` definition, verbatim with its comments).
  - `[[nodiscard]] Document::TextPage Document::TextPage::share() const noexcept;` — an empty handle from an empty handle.
  - `Document.hpp`: `namespace detail { struct TextPageAccess; }` before `class Document`, and `friend struct detail::TextPageAccess;` inside `TextPage`; `impl_` becomes `std::shared_ptr<Impl>`.

- [ ] **Step 1: Write the failing test** `DocumentSelection a shared text page survives dropping the other handle`: `a = doc.text_page(kWords)`; `b = a.share()`; `REQUIRE(b.valid() && b.page() == a.page())`; `a = Document::TextPage{}`; `b.copy(full_range)` equals the copy taken before the reset; hold an `EscrowContext` probe and assert `family_context_count` drops by 1 only when `b` is reset, not when `a` is. Also `REQUIRE_FALSE(Document::TextPage{}.share().valid())`.
- [ ] **Step 2: Run it** — fails to compile (`share` not declared).
- [ ] **Step 3: Implement.** Move `Impl` into the new header; `TextPageAccess::build` contains today's `text_page` body from the escrow clone through the `full_range` warm-up (lines 854-891), taking `ctx`/`doc` as parameters; `Document::text_page` keeps its `is_open`/`INT_MAX` checks, its `make_unique`-free shape and its deferred `unique_lock`, then returns `detail::TextPageAccess::build(impl_->ctx, impl_->doc, static_cast<int>(index))`. Allocate the Impl with `std::make_shared` inside `build`, catching `std::bad_alloc` into an empty handle (the function is noexcept). `share()` copies the `shared_ptr`. Copy ctor/assignment stay deleted.
- [ ] **Step 4: Update the comments.** `Document.hpp:248-253`: the single-thread rule now covers every handle sharing one `Impl`; the worker gives up its only handle when it posts (Task 6). `Impl`'s comment: its destructor runs when the last share is released. `Document.cpp` `text_page`: point at the builder for the build itself.
- [ ] **Step 5: Run** `"[selection]"` and `"[core]"` — all pass, including every pre-existing selection case unchanged.
- [ ] **Step 6: Commit** `refactor(core): one text-page builder and a shareable TextPage (#98)`.

## Task 3: `Document::needed_password()`

**Files:**
- Modify: `src/core/Document.hpp` (after `authenticate`), `src/core/Document.cpp`
- Test: `tests/unit/test_document_password.cpp`

**Interfaces:**
- Produces: `[[nodiscard]] bool Document::needed_password() const noexcept;` — true iff `open()` returned `NeedsPassword` for the current file; unchanged by `authenticate()`; false after `close()` and for a never-opened Document. Reads `impl_->needs_password`.

- [ ] **Step 1: Write the failing test** `DocumentPassword needed_password survives authenticate and clears on close`: `simple.pdf` → false. `encrypted.pdf` → `open` returns `NeedsPassword`, `needed_password()` true; after a successful `authenticate` (use the password the existing tests in this file use) still true and `is_open()` true; after `close()` false.
- [ ] **Step 2: Run** — fails to compile.
- [ ] **Step 3: Implement** the accessor with a header comment saying why it exists: the render workers' documents are not authenticated (§3.6), so a prefetch must not run for such a file.
- [ ] **Step 4: Run** `"[password]"` (or the file's tag) — passes.
- [ ] **Step 5: Commit** `feat(core): Document::needed_password (#98)`.

## Task 4: Worker-side build with skip rules

**Files:**
- Modify: `src/core/RenderEngine.hpp:35-101`, `src/core/RenderEngine.cpp:87-269` (worker loop), `:373-391` (destructor)
- Create: `tests/unit/test_text_prefetch_engine.cpp`; register in `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `detail::TextPageAccess::build` (Task 2).
- Produces:
  - `RenderEngine.hpp` includes `core/Document.hpp`.
  - `RenderRequest` gains, as its last two members: `std::function<void(Document::TextPage)> on_text;` and `std::shared_ptr<const std::atomic<int>> text_wanted;` (comments from §3.3).
  - `void RenderEngine::begin_shutdown() noexcept;` — sets `stopping` under `mtx`, `notify_all`; idempotent. `~RenderEngine` calls it, then joins as today.

Worker rule, applied after `on_complete` has run with a **non-null** pixmap on both the L1-hit path (`:120-131`) and the full-render path (`:261-266`) — factor it into one private helper `maybe_deliver_text(Impl*, const RenderRequest&, const std::shared_ptr<std::atomic<bool>>& canceled, fz_context*, fz_document*)`:
1. Return if `!req.on_text`.
2. Under one `mtx` acquisition: return if `stopping`, or if any queued entry has `priority == 0` and is not cancelled.
3. Return if `text_wanted` is null or `text_wanted->load(relaxed) != req.page_num`, or `canceled && canceled->load()`.
4. `auto t = detail::TextPageAccess::build(ctx, wdoc, req.page_num);`
5. Re-check `stopping` (under `mtx`), `text_wanted`, `canceled`; return (dropping `t`) on any.
6. If `t.valid()`, `req.on_text(std::move(t))`.

- [ ] **Step 1: Write the failing tests** in the new file, all with `RenderEngine engine(doc, 1)` (no cache) and a test-owned `auto wanted = std::make_shared<std::atomic<int>>(page)`. Park the worker with a latch in `on_complete` (the idiom in `test_render_engine_cancel.cpp:19-60`):
  - `RenderEngine delivers text after a non-null pixmap` (positive control): one request, not parked → `on_text` called once, handle valid, `page()` equals the request page.
  - `RenderEngine skips text when the page is no longer wanted`: parked; test stores `-1`; release → pixmap delivered, no `on_text`.
  - `RenderEngine skips text when a newer P0 is queued`: A parked; submit P0 B (same page, own `on_text`); release → A no `on_text`, B `on_text` once.
  - `RenderEngine skips text once shutdown has begun`: A parked, P0 B with `on_text` queued; `engine.begin_shutdown()`; release; destroy the engine (scope end) → B's `on_complete` got a non-null pixmap, neither `on_text` ran.
  - `RenderEngine sends no text with a null pixmap`: (a) a request whose `cancel_flag` is set before submit, (b) a request for `page_num = page_count` (past the end) → each gets `on_complete(nullptr, ...)`, zero `on_text`.
  - `TextPrefetch worker text equals Document text` (equivalence oracle, §5.1-1): for each fixture page below, build the worker handle via a one-worker engine and compare with `doc.text_page(p)`: equal `full_range` result and points, equal `highlight(first,last)` quads (exact float equality), equal `copy(first,last)`. Fixtures: every page of `selection.pdf`, `search.pdf`, `simple.pdf`, `bookmarks.pdf`, `cjk-ja.pdf`, `cjk-ko.pdf`, `cjk-zh-hant.pdf`, `type3.pdf`, `annot.pdf`, `sample.epub`, `sample.cbz`; pages 0, `n/2`, `n-1` of `large.pdf`; and the `/Count`-inflated copy built by `with_page_count` (copy that helper from `test_document_page_count.cpp:23-40` into this file). On every page of `selection.pdf` also compare, for the pairs (`alpha`,`gamma`) and (`beta`,`beta`) on page `kWords` (3) from `center_of_word`, `snap` in Chars/Words/Lines then `highlight` and `copy` of each snapped pair.
- [ ] **Step 2: Run** `"[text_prefetch]"` (tag every case `[core][render][text_prefetch]`) — fails to compile.
- [ ] **Step 3: Implement** the members, `begin_shutdown`, the helper, and its two call sites. `wdoc` is `impl->worker_docs[idx]`, already in scope in `worker_loop`.
- [ ] **Step 4: Run** `"[text_prefetch]"` then `"[render]"` — all pass.
- [ ] **Step 5: Non-vacuity of the equivalence oracle.** Temporarily replace the builder call in step 4 of the helper with a build from the request page's L2 display list (`fz_load_page` + `fz_new_display_list_from_page` + `fz_new_stext_page_from_display_list`, wrapped into a handle through a scratch helper in `TextPageAccess`). Run the oracle: it must FAIL on `annot.pdf` (copy contains `Annotation`). Revert with `git checkout -- src/core` and confirm `git diff --stat src/core` shows only the intended change. Note the failing assertion in the commit message.
- [ ] **Step 6: Commit** `feat(core): build a page's text on the render worker (#98)`.

## Task 5: The per-tab slot in DocumentView

**Files:**
- Modify: `src/core/DocumentView.hpp:130-190`, `src/core/DocumentView.cpp:40-122` (Impl), `:124-191` (ctor/dtor), `:201-213` (`set_current_page`), `:315-321` (`set_dual_page`), `:340-385` (`request_render_with_prefetch`)
- Create: `tests/unit/test_text_prefetch_view.cpp`; register in `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: Tasks 2-4.
- Produces (all UI-thread only):
  - `using TextCb = std::function<void(Document::TextPage)>;`
  - `void request_render_with_prefetch(int page, RenderCb on_current_complete, TextCb on_text = {});` (replaces the two-argument declaration; existing callers compile unchanged)
  - `[[nodiscard]] bool has_text_for(int page) const noexcept;`
  - `[[nodiscard]] bool wants_text_prefetch(int page) const noexcept;` — `!dual_page && !doc.needed_password() && !has_text_for(page)`
  - `void offer_text(Document::TextPage t);`
  - `[[nodiscard]] Document::TextPage text_for(int page);`
  - Impl members: `Document::TextPage text_slot;` and `std::shared_ptr<std::atomic<int>> text_wanted` (constructed as `std::make_shared<std::atomic<int>>(-1)` in the ctor, never reassigned).

Behaviour: the §3.5 table, exactly. In particular `set_current_page` drops the slot and stores -1 **before** `set_viewport`; `set_dual_page(true)` stores -1; `~DocumentView` stores -1 as its first statement; `request_render_with_prefetch` attaches `on_text` + `text_wanted` to the P0 only when `on_text && wants_text_prefetch(page)`, storing `page` first.

- [ ] **Step 1: Write the failing tests** (`[core][view][text_prefetch]`), using `InlineDispatcher` and the two-worker default unless noted. A helper `prefetch(view, page)` sets the page, calls `request_render_with_prefetch(page, drop-pixmap cb, on_text that fulfils a promise)`, waits up to 10 s, then calls `view.offer_text(std::move(handle))`.
  - `DocumentView prefetch gate`: `wants_text_prefetch(0)` true on `simple.pdf`; false after `set_dual_page(true)`; false on `encrypted.pdf` after `authenticate`; false once `has_text_for(0)` (after `prefetch(view, 0)`).
  - `DocumentView offer_text keeps only the current page and never replaces`: offering a handle built for page 1 while on page 0 leaves `has_text_for(1)` false. Never-replace, with an `EscrowContext` probe: fill the slot for page 0 and hold `h_first = view.text_for(0)`; build a second page-0 handle (via `prefetch`'s on_text, not offered) and read `family_context_count` = C; `view.offer_text(std::move(second))` → count is C-1 (the second was dropped). A replacing implementation would keep both alive (the first through `h_first`), leaving C.
  - `DocumentView a page change empties the slot`: after `prefetch(view, 0)`, `set_current_page(1)` → `has_text_for(0)` false; `set_current_page(1)` again on a filled page-1 slot → still filled.
  - `DocumentView text_for reuses the slot`: hold `h1 = view.text_for(0)` and an `EscrowContext` probe; read `family_context_count`; `h2 = view.text_for(0)` → count unchanged; non-vacuity: `view.set_current_page(1); h3 = view.text_for(1)` → count +1.
  - `DocumentView page change while parked skips the text`: `DocumentView(..., /*num_workers=*/1)`; the P0's `on_complete` parks on a latch; the test calls `set_current_page(1)`, releases → no `on_text`.
  - `DocumentView a worker-built text page outlives its view`: on `selection.pdf` and `type3.pdf`: get the handle through `prefetch`'s on_text (do not offer it), destroy the view, then `copy(full_range)` still works; with a probe held, dropping the handle lowers `family_context_count` by 1; after dropping the probe, `live_mupdf_roots()` returns to the value read before the Document was opened.
- [ ] **Step 2: Run** `"[view][text_prefetch]"` — fails to compile.
- [ ] **Step 3: Implement** per the Interfaces block. `text_for` builds through `impl_->doc.text_page(page)` on a miss and stores only when valid and `page == current_page`.
- [ ] **Step 4: Run** `"[view]"` and the full suite (`ctest`) — all pass.
- [ ] **Step 5: Commit** `feat(core): keep one prefetched text page per tab (#98)`.

## Task 6: Deliver the text to the canvas, and use it

**Files:**
- Modify: `src/ui/PdfCanvas.hpp:30-130`, `src/ui/PdfCanvas.cpp:104-202` (meta + post helper), `:609-626` (`select_all`), `:707-772` (`on_left_button_down`), `:1136` area (new message case), `:1339-1373` (`resubmit_current_page`), `:1682-1742` (`navigate_to_page`)
- Modify: `src/ui/MainWindow.cpp:59` (reservation comment), `:383-388` (`kick_render`)
- Modify: `CHANGELOG.md` (`## [Unreleased]`)

**Interfaces:**
- Consumes: `DocumentView::request_render_with_prefetch(page, cb, TextCb)`, `offer_text`, `text_for` (Task 5); `Document::TextPage` (Task 2).
- Produces: `inline constexpr UINT WM_USER_TEXT_READY = WM_USER + 5;` in `PdfCanvas.hpp`; `static bool PdfCanvas::post_text_ready(HWND target, litepdf::core::Document::TextPage text, std::uint64_t epoch);`.

- [ ] **Step 1: Message and post helper.** `PdfCanvas.hpp` includes `core/Document.hpp`, defines the message beside `WM_USER_RENDER_DONE_RIGHT` with a comment (LPARAM = heap `TextMeta`, freed by the handler). `MainWindow.cpp:59`'s comment names `WM_USER + 3..5` as PdfCanvas's. In `PdfCanvas.cpp`'s anonymous namespace: `struct TextMeta { litepdf::core::Document::TextPage text; std::uint64_t epoch = 0; };`. `post_text_ready`: `new (std::nothrow) TextMeta` → on null return false (the handle dies on the worker); `PostMessageW(target, WM_USER_TEXT_READY, 0, meta)` → on failure `delete meta`, return false; else true. Comment the never-dispatched-at-exit case by pointing at the #74 paragraph at `:168-179`, not by repeating it.
- [ ] **Step 2: Handler.** `case WM_USER_TEXT_READY:` take ownership in a `std::unique_ptr<TextMeta>`; if `!impl_->view || meta->epoch != impl_->view_epoch` return 0 (handle dropped); else `impl_->view->offer_text(std::move(meta->text))`; return 0. No `InvalidateRect`.
- [ ] **Step 3: Three submission sites** pass `[hwnd, epoch](litepdf::core::Document::TextPage t) { PdfCanvas::post_text_ready(hwnd, std::move(t), epoch); }` as the third argument, capturing the site's HWND local: `target` in `MainWindow::kick_render` (`:385`) and `PdfCanvas::resubmit_current_page` (`:1369`), `target_hwnd` in `PdfCanvas::navigate_to_page` (`:1736`). The spread branches are untouched.
- [ ] **Step 4: Consumers.** `select_all` (`:613`) and `on_left_button_down` (`:743-744`): replace `impl_->view->document().text_page(static_cast<std::size_t>(page))` with `impl_->view->text_for(page)`. `select_all`'s `const auto text` stays a local handle (a share).
- [ ] **Step 5: CHANGELOG.** Under `## [Unreleased]` add `### Fixed` (create the heading if absent) with the §7 sentence: a page's text is now read in the background once the page is shown, so a text-selection press or Select All on it no longer pauses LitePDF; a press that comes in before that background read has finished still reads the text on the spot, as before (#98).
- [ ] **Step 6: Build everything and run the suite.** `cmake --build build --config Release` (all targets, so `litepdf.exe` and `litepdf-cli.exe` link) and `ctest --test-dir build -C Release` from PowerShell → all pass.
- [ ] **Step 7: Commit** `perf(ui): select with the page text the render worker prefetched (#98)`.

## Task 7: Scripted GUI check and measurement (no committed product code)

Everything here runs in a throwaway worktree and under `build/gui-check/` (untracked). Read `C:\Users\User\.claude\projects\C--Users-User-projects-litepdf\memory\reference_litepdf_scripted_gui_smoke.md` ("Read first" list) before writing the driver; worktree build recipe: `reference_litepdf_build_test_commands.md`, "Building in a Claude worktree" (junction `third_party/mupdf`, remove it with `cmd /c rmdir` afterwards).

**Files (untracked):**
- `build/gui-check/make-probe-98.sh`, `build/gui-check/check98.ps1`, `build/gui-check/results-98.md`

- [ ] **Step 1: Probe exe.** In a throwaway worktree at this branch's HEAD, add `fprintf(stderr, ...)` lines: `text: slot` / `text: build` in `DocumentView::text_for`; `text: stored <page>` in `offer_text` when it stores; and, for measurement only, `text: worker <page> <ms>` around the builder call in the worker helper (`QueryPerformanceCounter`). Build Release, copy `litepdf.exe` to `build/gui-check/probe98.exe`. Build a second exe `probe98-off.exe` with `request_render_with_prefetch`'s attach condition forced false (prefetch off). Main checkout: `git status` clean afterwards.
- [ ] **Step 2: Driver cases** (back up `session.json` first, set `LITEPDF_NO_RESTORE=1`, close with `WM_CLOSE`; capture stderr to a file):
  1. **Prefetch hit:** open `tests\fixtures\selection.pdf`; wait (timeout 10 s) for `text: stored 0`; post `WM_LBUTTONDOWN`/`WM_LBUTTONUP` at the canvas centre to the `LitePDFPdfCanvas` HWND → next `text:` line is `text: slot`.
  2. **Negative control:** post a page change (`WM_KEYDOWN VK_NEXT` to the canvas) and select-all (`WM_COMMAND` 40072, `IDM_EDIT_SELECT_ALL`, to the main window) back to back → `text: build`.
  3. **Correctness:** wait for `text: stored <page>`; post select-all then copy (40071, `IDM_EDIT_COPY`); after `WM_INITMENUPOPUP` on the Edit popup, `GetMenuState(IDM_EDIT_COPY)` is enabled; clipboard text with whitespace removed equals `litepdf-cli`'s page text for that page with whitespace removed.
  4. **Password gate:** `tests\fixtures\encrypted.pdf`, answer the password prompt, select-all twice → `text: build` then `text: slot`; no `text: stored` line ever appears for it.
  5. **Paging storm:** `selection.pdf`, post 7 `VK_NEXT` back to back, wait for `text: stored 7`, select-all → `text: slot`; count `text: worker` lines (record it; fewer than 8 is the expected shape).
  6. **Close storm:** 20 times: open `type3.pdf` in a new tab and immediately post `WM_COMMAND` 40030 (`IDM_TAB_CLOSE`) → process alive at the end, no crash dialog, stderr has no MuPDF error lines.
- [ ] **Step 3: Measurement (§5.4).** Ask the user for the paths of #73's Edge-printed HTML spec PDF and the news-page PDF if they are not under `%USERPROFILE%\Downloads`. With `probe98.exe`: page to p145 and p752 of the spec and p2 of the news page, record each `text: worker` ms (first visit, and after the other worker served the page from L1: page away and back). With `probe98.exe` vs `probe98-off.exe`: post 20 `VK_NEXT` at 150 ms intervals on the spec and record the time from each post to the canvas's accepted `WM_USER_RENDER_DONE` (add that line to both probes). Do not assert; write the numbers into `results-98.md`.
- [ ] **Step 4: Report.** Cases 1-6 pass (each with its observed line). If case 2 does not print `text: build`, the trace is not trustworthy — stop and report. Remove the junction and the throwaway worktree. Paste the measurement table into the PR description later; nothing is committed.

## Task 8: Lift the password gate once #126 has merged (conditional)

Run only if `origin/main` contains #126's merge (see Global Constraints). Otherwise skip, and leave a line in the PR description that the gate stays until #126 lands.

**Files:**
- Modify: `src/core/DocumentView.cpp` (`wants_text_prefetch`), `src/core/Document.hpp/.cpp` (remove `needed_password` if nothing else uses it), the Task 3 and Task 5 tests, spec §3.6 (one-line addendum that the gate was lifted by #126)

- [ ] **Step 1:** Rebase on `origin/main`; resolve the `DocumentView`/`RenderEngine` constructor conflicts by keeping #126's password parameter (third in `DocumentView`) and this branch's additions; build and run the full suite.
- [ ] **Step 2: Failing test first:** add `tests/fixtures/encrypted-text.pdf` (from #126) to Task 4's equivalence oracle, opened and authenticated, with the engine constructed with the password exactly as #126's tests construct it → passes (workers authenticate). Change the Task 5 gate test to expect `wants_text_prefetch(0)` **true** on `encrypted-text.pdf` after authenticate → fails.
- [ ] **Step 3:** Drop `!doc.needed_password()` from `wants_text_prefetch`; remove `needed_password()` and its test if `rg needed_password src tests` finds no other user.
- [ ] **Step 4:** Full suite passes. Commit `perf(core): prefetch text on password-protected PDFs too (#98, after #126)`.
