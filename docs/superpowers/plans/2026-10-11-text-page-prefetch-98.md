# Text-page prefetch on the render worker (#98) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A page's first text-selection press or Select All no longer builds the page's structured text on the UI thread once the page has been shown; the render worker builds it on its own document and hands it over.

**Architecture:** One shared builder (`detail::TextPageAccess::build`) serves both `Document::text_page` (UI fallback, under `doc_mutex`) and the render worker (its own `fz_context`/`fz_document`, no lock). The worker builds after posting the pixmap, skips the build when it could delay newer work, and the handle reaches the UI by `WM_USER_TEXT_READY`, where `DocumentView` keeps one shareable handle per tab.

**Tech Stack:** C++20, Win32, Direct2D, MuPDF 1.27.2 (vendored), Catch2, CMake (VS 2022 BuildTools), Python 3 for the fixture generator.

**Spec:** `docs/superpowers/specs/2026-10-10-text-page-prefetch-98-design.md`.

## Global Constraints

- **Spec:** `C:\Users\User\projects\litepdf\docs\superpowers\specs\2026-10-10-text-page-prefetch-98-design.md`. Each task names the sections it implements; read those sections before starting the task. Where a task and the spec disagree, stop and report.
- Branch `perf/98-text-page-prefetch`, base `main` @ `049d27d`. If `origin/main` has moved, rebase before Task 1 and re-run the baseline suite.
- **#126 is in flight** (worker documents authenticate; it changes the `DocumentView` and `RenderEngine` constructors). Before Task 1 and before Task 7, in Git Bash: `git fetch origin && git log --oneline origin/main | grep -i "#126"`. If it has merged, rebase onto it first, and run Task 8 after Task 7.
- Shells: git commands and the generator in **Git Bash**; builds, tests and the GUI driver in **PowerShell**.
- Build the tests in **Release** only (Debug fails with LNK2038). Never a bare `cmake`/`ctest` (the ones on PATH are MinGW). The two commands:
  - build: `& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build C:\Users\User\projects\litepdf\build --config Release` (add `--target litepdf_unit_tests` for tests only)
  - full suite, from the repo root: `& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" --test-dir build -C Release`
  - one filter, from the repo root: `build\tests\Release\litepdf_unit_tests.exe "<tag or name>"`
- Do not hard-code the ctest count anywhere; read `ctest -N` when a number is needed.
- New test files register in `tests/CMakeLists.txt` under `target_sources(litepdf_unit_tests PRIVATE ...)` with a trailing `# #98` comment.
- `TEST_CASE` names are ASCII and start with their subsystem (`DocumentSelection`, `DocumentPassword`, `RenderEngine`, `DocumentView`, `TextPrefetch`).
- `src/core/Document.hpp`, `src/core/RenderEngine.hpp` and `src/core/DocumentView.hpp` stay MuPDF-free (no `fitz.h`; no `fz_*` type except the existing `fz_context`/`fz_pixmap` forward declarations).
- The text-page builder (`TextPageAccess::build`, used by `Document::text_page` and the worker) uses `fz_stext_options opts = {}` — never `FZ_STEXT_COLLECT_STRUCTURE` or `FZ_STEXT_DEHYPHENATE`, and never `fz_new_stext_page_from_display_list` (test code and the Task 4 mutant excepted). `Document::page_hits` keeps its own `FZ_STEXT_DEHYPHENATE`; it is not this builder.
- `WM_USER_TEXT_READY = WM_USER + 5`.
- All `text_wanted` writes are `->store(v, std::memory_order_relaxed)` on the UI thread; that `shared_ptr` is never reassigned.
- **EscrowContext probe** (used by Tasks 1, 2 and 5 to count a MuPDF context family): `fz_context* tmp = doc.clone_context(); auto probe = EscrowContext::clone_from(tmp); fz_drop_context(tmp);` — in a `DocumentView` test, `doc` is `view.document()` — then read `litepdf::core::detail::family_context_count(probe.get())` (declared in `core/EscrowContext.hpp`). Declare `fz_drop_context` as `extern "C" { struct fz_context; void fz_drop_context(fz_context*); }` as `tests/unit/test_document_view.cpp:16-19` does, or include `<mupdf/fitz.h>`. Never use a bare clone as the probe (it reads 0 once the Document dies, and dropping it then is the #61 use-after-free). A `live_mupdf_roots()` baseline is always read **before the Document is opened**: opening one adds a root.
- **Park latch** (Task 4 stops the worker inside `on_complete` without timing): one `std::mutex` + `std::condition_variable` and two bools. `on_complete` sets `entered` and notifies, waits until `release`, drops its pixmap, returns. The test waits for `entered`, acts as the UI thread, sets `release` and notifies. No `sleep_for` anywhere. (`test_render_engine_cancel.cpp` uses fixed sleeps; do not copy it.)
- **Sentinel** (how a negative Task 4 case knows the parked request's text check has already run): while the worker is parked, also submit a **priority-1** request S with no `on_text`, whose `on_complete` drops its pixmap and fulfils a promise. With one worker, S runs only after the parked request's `maybe_deliver_text` has returned, and a priority-1 request does not trigger skip rule 3. Wait on S's future (`wait_for` 10 s, then `REQUIRE` ready), assert, and only then let the engine be destroyed. Never use destruction as the completion marker for a negative case: `~RenderEngine` sets `stopping`, which would mask the rule under test.
- Every artifact (code, comments, commits, test names) in English; match the surrounding comment density.
- Commit per task; every message ends with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

1. **Tab closed while its text build is in flight** — expected: no crash, no leak; the handle dies through its own escrow. Pinned by Task 5's lifetime test and Task 7's close-storm case.
2. **Held PgDn / repeated zoom before the first text lands** — expected: the page the reader stops on gets correct text, and passed-through pages do not delay bitmaps. Pinned by Task 4's queued-P0 test and Task 7's paging-storm case.
3. **A page that renders but has no text (CBZ, image-only, blank)** — expected: same as today (no range, nothing copied). Pinned by Task 4's equivalence oracle (`selection.pdf` page 2 is blank; `sample.cbz`).
4. **A damaged-but-openable PDF** (the #119 inflated `/Count` copy) — expected: the worker-built text equals the Document's. Pinned by Task 4's equivalence oracle on that copy; a failure there is a finding to report, not a fixture to drop (spec §8).
5. **Spread mode** — expected: no prefetch is requested. Pinned by Task 5's gate test. A late handle cannot cause a selection there, because presses and Select All are already refused in spread mode (`PdfCanvas.cpp:600`, `:738`); no new code is involved.

---

## Task 1: Type3 and annotation fixtures, and today's Type3 lifetime

Closes the review's coverage gap (no fixture has a Type3 font) on today's code, and gives Task 4 the fixture its non-vacuity check needs. Spec: §5.3, §5.1-6 (last paragraph).

**Files:**
- Create: `scripts/generate-prefetch-fixtures.py`, `tests/fixtures/type3.pdf`, `tests/fixtures/annot.pdf`
- Modify: `tests/unit/test_document_selection.cpp` (new cases at the end)

**Interfaces:**
- Produces: `tests/fixtures/type3.pdf` (1 page; extracted text exactly `ABBA`, drawn with a Type3 font); `tests/fixtures/annot.pdf` (1 page; body text exactly `Body text` in Helvetica, plus one FreeText annotation whose `/AP /N` stream draws `Annotation text`).

- [ ] **Step 1: Write the generator.** `scripts/generate-prefetch-fixtures.py`, standard library only, writes both files to `tests/fixtures/` as literal uncompressed PDF objects with a correct xref (compute byte offsets while writing). Follow the docstring style of `scripts/generate-selection-fixture.py` (what each file pins, usage). Both deterministic: no timestamps, no `/ID`.
  - `type3.pdf`: a `/Type3` font with `/FontMatrix [0.001 0 0 0.001 0 0]`, `/CharProcs` for glyph names **`A` and `B`** (uppercase: MuPDF maps Type3 glyph names to Unicode through the AGL, `pdf-type3.c:133`, so `/a` would extract as lowercase `a`), each a `d1` plus a filled rectangle of a different width, `/Encoding << /Differences [65 /A /B] >>`, `/FirstChar 65 /LastChar 66`, `/Widths`, `/FontBBox [0 0 1000 1000]`, and content `BT /T3 24 Tf 72 700 Td (ABBA) Tj ET`.
  - `annot.pdf`: Helvetica body `Body text` at (72, 700); one `/FreeText` annotation with `/Rect [72 600 300 640]` and `/AP << /N <form XObject> >>`, the form having `/BBox [0 0 228 40]`, whose stream draws `Annotation text` at (4, 12) with its own `/Resources` Helvetica.
- [ ] **Step 2: Generate and check determinism.** Git Bash, repo root: `python -I scripts/generate-prefetch-fixtures.py && cp tests/fixtures/type3.pdf /tmp/t3a && cp tests/fixtures/annot.pdf /tmp/ana && python -I scripts/generate-prefetch-fixtures.py && cmp /tmp/t3a tests/fixtures/type3.pdf && cmp /tmp/ana tests/fixtures/annot.pdf` → exits 0.
- [ ] **Step 3: Write the tests** in `tests/unit/test_document_selection.cpp`, tagged `[core][selection]`:
  - `DocumentSelection extracts Type3 text`: `doc.text_page(0)` on `type3.pdf` is valid; `without_whitespace(copy(full_range)) == "ABBA"`.
  - `DocumentSelection a Type3 text page outlives its Document`: read `live_mupdf_roots()` = R0 first. Then mirror `DocumentSelection a text page outlives its Document` (same file, ~line 325) on `type3.pdf` page 0, with an EscrowContext probe (Global Constraints) taken inside the Document's scope. After the scope: `without_whitespace(copy(full_range)) == "ABBA"`; resetting the handle lowers `family_context_count(probe.get())` by exactly 1; after resetting the probe, `live_mupdf_roots() == R0`.
  - `DocumentSelection skips annotation text`: on `annot.pdf`, `without_whitespace(copy(full_range))` contains `Bodytext` and does not contain `Annotation`.
  - `DocumentSelection the annotation fixture draws its annotation text` (fixture validity, so Task 4's mutant cannot be vacuous): include `<mupdf/fitz.h>` (the test target has MuPDF's include path, see `test_mupdf_root.cpp`); with a test-owned `fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT)` + `fz_register_document_handlers`, open `annot.pdf`, load page 0, `fz_new_display_list_from_page`, `fz_new_stext_page_from_display_list(ctx, list, nullptr)`, `fz_new_buffer_from_stext_page`; the buffer's text contains `Annotation text`. Drop everything in reverse order inside `fz_try`/`fz_always`.
- [ ] **Step 4: Run** `build\tests\Release\litepdf_unit_tests.exe "[selection]"` → all pass. They pin today's behaviour, so they pass at once; the fixture-validity case is what proves `annot.pdf` can tell the two builders apart.
- [ ] **Step 5: Commit** generator, both fixtures and tests: `test: Type3 and annotation fixtures for #98, and a Type3 text page outliving its Document`.

## Task 2: One builder, and a shareable TextPage

Pure refactor plus `share()`; no behaviour change. Spec: §3.1, §3.2.

**Files:**
- Create: `src/core/detail/TextPageAccess.hpp`
- Modify: `src/core/Document.hpp:228-309`, `src/core/Document.cpp:630-654` (Impl moves out), `:686-689` (special members), `:823-892` (`text_page`)
- Test: `tests/unit/test_document_selection.cpp`

**Interfaces:**
- Produces:
  - `src/core/detail/TextPageAccess.hpp`: includes `core/Document.hpp`, `core/EscrowContext.hpp` and `<mupdf/fitz.h>`; holds the moved `struct Document::TextPage::Impl` definition (verbatim, comments kept) and `namespace litepdf::core::detail { struct TextPageAccess { static Document::TextPage build(fz_context* ctx, fz_document* doc, int index) noexcept; }; }`.
  - `build` is **defined in `Document.cpp`** (which includes the new header). No new `.cpp`, no CMake change.
  - `[[nodiscard]] Document::TextPage Document::TextPage::share() const noexcept;` — empty from an empty handle.
  - `Document.hpp`: `namespace detail { struct TextPageAccess; }` before `class Document`; `friend struct detail::TextPageAccess;` inside `TextPage`; `impl_` becomes `std::shared_ptr<Impl>`.

- [ ] **Step 1: Write the failing test** `DocumentSelection a shared text page survives dropping the other handle` (`[core][selection]`): `a = doc.text_page(kWords)`; `b = a.share()`; `REQUIRE(b.valid() && b.page() == a.page())`; record `copy(full_range)` = T; with an EscrowContext probe (Global Constraints) read count C; `a = Document::TextPage{}` → count still C and `b`'s copy equals T; `b = Document::TextPage{}` → count C-1. Also `REQUIRE_FALSE(Document::TextPage{}.share().valid())`.
- [ ] **Step 2: Run it** → fails to compile (`share` not declared).
- [ ] **Step 3: Implement.** `build` holds today's `text_page` body from the escrow clone through the `full_range` warm-up (`Document.cpp:854-891`), with `ctx`/`doc` as parameters and the Impl allocated by `std::make_shared` inside a `try` that returns an empty handle on `std::bad_alloc`. `Document::text_page` keeps its `is_open`/`INT_MAX` checks and its deferred `unique_lock`, drops its own `make_unique<TextPage::Impl>` (the allocation moves into `build`), and returns `detail::TextPageAccess::build(impl_->ctx, impl_->doc, static_cast<int>(index))` under the lock. `share()` copies the `shared_ptr`. Copy ctor/assignment stay deleted.
- [ ] **Step 4: Update comments.** `Document.hpp:248-253`: the single-thread rule now covers every handle sharing one `Impl`; the render worker gives up its only handle when it posts it. `Impl`: its destructor runs when the last share is released. `text_page`: the build itself is in `TextPageAccess::build`.
- [ ] **Step 5: Run** `"[selection]"` then `"[core]"` → all pass, every pre-existing selection case unchanged.
- [ ] **Step 6: Commit** `refactor(core): one text-page builder and a shareable TextPage (#98)`.

## Task 3: `Document::needed_password()`

Spec: §3.6.

**Files:**
- Modify: `src/core/Document.hpp` (after `authenticate`), `src/core/Document.cpp`
- Test: `tests/unit/test_document_password.cpp`

**Interfaces:**
- Produces: `[[nodiscard]] bool Document::needed_password() const noexcept;` — true iff `open()` returned `NeedsPassword` for the current file; unchanged by `authenticate()`; false after `close()` and for a never-opened Document. Reads `impl_->needs_password` (written only at `Document.cpp:226` and `:251`).

- [ ] **Step 1: Write the failing test** `DocumentPassword needed_password survives authenticate and clears on close`, tagged `[document][password]` like the file's other cases: a never-opened `Document` → false; `simple.pdf` → false. `encrypted.pdf` → `open` returns `NeedsPassword` and `needed_password()` is true; after `authenticate("test")` (the file's existing password) it is still true and `is_open()` is true; after `close()` it is false.
- [ ] **Step 2: Run** → fails to compile.
- [ ] **Step 3: Implement**, with a header comment: the render workers' own documents are not authenticated (spec §3.6), so the text prefetch must not run for such a file.
- [ ] **Step 4: Run** `"[password]"` → passes.
- [ ] **Step 5: Commit** `feat(core): Document::needed_password (#98)`.

## Task 4: Worker-side build with skip rules

Spec: §3.3 (rules, re-check, residual), §5.1-1, §5.1-5.

**Files:**
- Modify: `src/core/RenderEngine.hpp:35-101`, `src/core/RenderEngine.cpp:87-269` (worker loop), `:373-391` (destructor)
- Create: `tests/unit/test_text_prefetch_engine.cpp`; register in `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `detail::TextPageAccess::build(fz_context*, fz_document*, int)` from `core/detail/TextPageAccess.hpp` (Task 2); `Document::TextPage` with `valid()`, `page()`, `full_range`, `snap`, `highlight`, `copy`.
- Produces:
  - `RenderEngine.hpp` includes `core/Document.hpp`.
  - `RenderRequest` gains, as its last two members: `std::function<void(Document::TextPage)> on_text;` and `std::shared_ptr<const std::atomic<int>> text_wanted;` (doc comments from spec §3.3).
  - `void RenderEngine::begin_shutdown() noexcept;` — sets `stopping` under `mtx`, `notify_all`; idempotent. `~RenderEngine` calls it, then joins as today. After it, the worker still drains the queue (`RenderEngine.cpp:95-96`).
  - `[[nodiscard]] std::size_t RenderEngine::text_builds_started() const noexcept;` — an `std::atomic<std::size_t>` incremented immediately before every `TextPageAccess::build` call in the helper. Test observability only (the `live_mupdf_roots` precedent); nothing in the product reads it. It lets a negative case prove the build was **skipped**, not built and then dropped by the re-check.

Worker rule, applied after `on_complete` has run with a **non-null** pixmap, on both the L1-hit path (`RenderEngine.cpp:120-131`, before its `continue`) and the full-render path (`:261-266`). Factor it into one private static helper `maybe_deliver_text(Impl*, const RenderRequest&, const std::shared_ptr<std::atomic<bool>>& canceled, fz_context* ctx, fz_document* wdoc)`:
1. Return if `!req.on_text`.
2. Under one `mtx` acquisition: return if `stopping`, or if any queued entry has `priority == 0` and its `canceled` flag is clear.
3. Return if `!req.text_wanted`, or `req.text_wanted->load(std::memory_order_relaxed) != req.page_num`, or `canceled && canceled->load()`.
4. Increment the build counter, then `auto t = detail::TextPageAccess::build(ctx, wdoc, req.page_num);`
5. Re-check `stopping` (under `mtx`), `text_wanted` and `canceled`; on any, return (dropping `t`).
6. If `t.valid()`, `req.on_text(std::move(t))`.

- [ ] **Step 1: Write the failing tests** in the new file, tagged `[core][render][text_prefetch]`, each with `RenderEngine engine(doc, 1)` (one worker, no cache), a test-owned `auto wanted = std::make_shared<std::atomic<int>>(page)`, and the park latch and sentinel from Global Constraints where a case says "parked". Copy into the file's anonymous namespace, with the includes and using-declarations they need: `without_whitespace` and `center_of_word` from `tests/unit/test_document_selection.cpp:39-62` (they need `<string>`, `<string_view>` and `using litepdf::core::SelPoint;`, lines 12-17), and `with_page_count` plus `RemoveOnExit` from `tests/unit/test_document_page_count.cpp:23-54` (they need `<windows.h>`, `<atomic>`, `<filesystem>`, `<fstream>`, `<iterator>`, lines 10-17). Every request in a case carries `text_wanted = wanted` and the page `*wanted` holds, unless the case says otherwise.
  - `RenderEngine delivers text after a non-null pixmap` (positive control): `selection.pdf` page 3, not parked → `on_text` once; handle valid; `page() == 3`; `full_range` true.
  - `RenderEngine skips text when the page is no longer wanted`: A parked; submit sentinel S; the test stores -1 into `wanted`; release; wait for S → A's pixmap was non-null, no `on_text`, `text_builds_started() == 0`.
  - `RenderEngine skips text when a newer P0 is queued`: A parked; the test submits P0 request B for the same page, with its own `on_text` fulfilling a promise; release; wait for B's `on_text` → A's `on_text` never ran; B's ran once; `text_builds_started() == 1`.
  - `RenderEngine skips text when the request is cancelled after its pixmap`: A submitted with a caller-owned `cancel_flag`; parked; submit sentinel S; the test sets the flag; release; wait for S → no `on_text`, `text_builds_started() == 0`.
  - `RenderEngine skips text once shutdown has begun`: A parked; P0 B queued for the same page as A, with `text_wanted = wanted` (`*wanted` still equal to that page) and its own `on_text`; then sentinel S (P1, queued behind B); `engine.begin_shutdown()`; release; wait for S (the worker drains the queue after `begin_shutdown`) → B's `on_complete` received a non-null pixmap; neither `on_text` ran; `text_builds_started() == 0`. Without rule 1, B would build: it is wanted, S is not a P0, and nothing is cancelled.
  - `RenderEngine delivers text on a pixmap cache hit`: `fz_context* cache_ctx = doc.clone_context();`, `PageCache cache(5, 10, cache_ctx);`, `RenderEngine engine(doc, 1, &cache);` (idiom: `tests/unit/test_render_engine_cache.cpp:32-36`; destroy engine then cache, then `fz_drop_context(cache_ctx)`). Render page 3 once with no `on_text` and wait for it. Non-vacuity: `fz_pixmap* hit = cache.get_pixmap(3, scale)` is non-null (drop it with `cache_ctx`), so the next request is an L1 hit. Submit page 3 again at the same `scale` with `on_text` → `on_text` once, valid handle, `page() == 3`.
  - `RenderEngine sends no text for a request cancelled before submit`: `cancel_flag` set before submit → `on_complete(nullptr, ...)`, no `on_text`. The guard at the full-render delivery site (`on_text` only after a non-null pixmap) is reachable when rasterising or inverting fails after a good page load (`RenderEngine.cpp:213-215`, `:229-232`), but no fixture fails rasterisation deterministically, so it has no separate test.
  - `TextPrefetch worker text equals Document text` (equivalence oracle, spec §5.1-1): for each page below, get the worker handle from a one-worker engine (fresh `wanted` per page) and compare with `doc.text_page(p)` built on the same Document: equal `full_range` result and points, equal `highlight(first,last)` quads (exact float equality), equal `copy(first,last)`. Pages: every page of `selection.pdf`, `search.pdf`, `simple.pdf`, `bookmarks.pdf`, `cjk-ja.pdf`, `cjk-ko.pdf`, `cjk-zh-hant.pdf`, `type3.pdf`, `annot.pdf`, `sample.epub`, `sample.cbz`; pages 0, `n/2` and `n-1` of `large.pdf`; and every page (`doc.page_count()`) of `with_page_count("tests/fixtures/simple.pdf", "/Count 2 ")` (hold its `RemoveOnExit`). On `selection.pdf` page 3 only, for the pairs (`alpha`,`gamma`) and (`beta`,`beta`) from `center_of_word`, also compare `snap` in Chars, Words and Lines mode, then `highlight` and `copy` of each snapped pair.
- [ ] **Step 2: Run** `"[text_prefetch]"` → fails to compile.
- [ ] **Step 3: Implement** the members, `begin_shutdown`, the helper and its two call sites. `wdoc` is `impl->worker_docs[idx]`, already in scope in `worker_loop` (`:89`).
- [ ] **Step 4: Run** `"[text_prefetch]"` then `"[render]"` → all pass. If the `/Count` copy fails the oracle, stop and report it (spec §8 residual); do not drop it.
- [ ] **Step 5: Commit** `feat(core): build a page's text on the render worker (#98)`.
- [ ] **Step 6: Non-vacuity of the oracle (mutant, after the commit).** In `maybe_deliver_text`, replace item 4's builder call with a build from the page's display list: `fz_load_page` + `fz_new_display_list_from_page` + `fz_new_stext_page_from_display_list`, wrapped into a handle by a scratch function added to `TextPageAccess`. Build and run the oracle → it must FAIL on `annot.pdf` (the copy contains `Annotation`). Restore only the mutated files: `git checkout -- src/core/RenderEngine.cpp src/core/detail/TextPageAccess.hpp src/core/Document.cpp`, then `git status --short` prints nothing. Rebuild and re-run `"[text_prefetch]"` → passes. Record the failing assertion with `git commit --amend` (message only, no file change).

## Task 5: The per-tab slot in DocumentView

Spec: §3.5 (the table, reproduced below), §5.1-2, -3, -5, -6.

**Files:**
- Modify: `src/core/DocumentView.hpp:130-190`, `src/core/DocumentView.cpp:40-122` (Impl), `:124-191` (ctor/dtor), `:201-213` (`set_current_page`), `:315-321` (`set_dual_page`), `:340-385` (`request_render_with_prefetch`)
- Create: `tests/unit/test_text_prefetch_view.cpp`; register in `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `Document::TextPage::share()` (Task 2), `Document::needed_password()` (Task 3), `RenderRequest::on_text` / `text_wanted` (Task 4).
- Produces (all UI-thread only):
  - `using TextCb = std::function<void(Document::TextPage)>;`
  - `void request_render_with_prefetch(int page, RenderCb on_current_complete, TextCb on_text = {});` (replaces the two-argument declaration; existing callers compile unchanged)
  - `[[nodiscard]] bool has_text_for(int page) const noexcept;`
  - `[[nodiscard]] bool wants_text_prefetch(int page) const noexcept;`
  - `void offer_text(Document::TextPage t);`
  - `[[nodiscard]] Document::TextPage text_for(int page);`
  - `[[nodiscard]] int pending_text_page() const noexcept;` — `text_wanted->load(relaxed)`: the page whose text a queued P0 may still build, -1 for none. Test observability (the `live_mupdf_roots` precedent); nothing in the product reads it.
  - Impl members `Document::TextPage text_slot;` and `std::shared_ptr<std::atomic<int>> text_wanted` (created once in the ctor as `std::make_shared<std::atomic<int>>(-1)`, never reassigned).

Behaviour ("`text_wanted = v`" means `text_wanted->store(v, std::memory_order_relaxed)`):

| Member | Behaviour |
|---|---|
| `has_text_for(page)` | `text_slot.page() == page`. |
| `offer_text(t)` | Stores `t` iff `t.valid()`, `t.page() == current_page` and `!has_text_for(current_page)`; otherwise drops it. Never replaces a handle already in the slot. On store: `text_wanted = -1`. |
| `text_for(page)` | If `has_text_for(page)`: return `text_slot.share()` (never move out of the slot). Otherwise build with `impl_->doc.text_page(page)`; if valid and `page == current_page`, store it, set `text_wanted = -1`, and return `text_slot.share()`; else return the built handle. |
| `set_current_page` | When the page actually changes: drop the slot and set `text_wanted = -1`, **before** `set_viewport` (which can throw). |
| `set_dual_page(true)` | `text_wanted = -1`. |
| `~DocumentView` | `text_wanted = -1` immediately after the existing `if (!impl_) return;`, before the thumbnail teardown and `engine.reset()`. |
| `request_render_with_prefetch(page, cb, on_text)` | Iff `on_text && wants_text_prefetch(page)`: set `text_wanted = page`, and give the P0 request `on_text` and `text_wanted`. Never the P1 neighbours. |
| `wants_text_prefetch(page)` | `!dual_page && !doc.needed_password() && !has_text_for(page)`. |

- [ ] **Step 1: Write the failing tests** (`[core][view][text_prefetch]`), with `InlineDispatcher` and **`selection.pdf` (8 pages)** unless a case says otherwise. Right after constructing every view, call `view.set_viewport(800.0f, 600.0f, 96.0f)`: without a viewport the fit percentage, and so the render scale, is 0. Helper `get_worker_text(view, page)`: `set_current_page(page)`, call `request_render_with_prefetch(page, cb-that-drops-its-pixmap, on_text-that-fulfils-a-promise)`, wait on the future (`wait_for` 10 s, then `REQUIRE` it is ready), return the handle **without offering it**. Probe = the EscrowContext probe in Global Constraints.
  - `DocumentView prefetch gate`: `wants_text_prefetch(0)` true; false after `set_dual_page(true)`, true again after `set_dual_page(false)`; false once the slot holds page 0 (`view.offer_text(get_worker_text(view, 0))`); on `encrypted.pdf` (open, `authenticate("test")`, then construct the view) false.
  - `DocumentView offer_text keeps only the current page and never replaces`: on page 0, `view.offer_text(view.document().text_page(1))` → `has_text_for(1)` false. Never-replace: `view.offer_text(get_worker_text(view, 0))`; hold `h_first = view.text_for(0)`; `second = view.document().text_page(0)`; read count C; `view.offer_text(std::move(second))` → count C-1 (the second was dropped; a replacing implementation would keep both alive, the first through `h_first`, leaving C).
  - `DocumentView a page change empties the slot`: slot filled for page 0; `set_current_page(1)` → `has_text_for(0)` false; fill page 1, call `set_current_page(1)` again → `has_text_for(1)` still true.
  - `DocumentView text_for reuses the slot`: `h1 = view.text_for(0)` (fills the slot); read count C; `h2 = view.text_for(0)` → C; non-vacuity: `view.set_current_page(1); h3 = view.text_for(1)` → C+1.
  - `DocumentView keeps text_wanted in step with the page`: on page 0, `pending_text_page() == -1`; after `request_render_with_prefetch(0, drop-cb, on_text)` it is 0; after `set_current_page(1)` it is -1; after a second request for page 1 it is 1, and after `view.offer_text(view.document().text_page(1))` it is -1; after a request for page 1 (slot now holds page 1) it stays -1 (no text requested); after `set_current_page(2)`, a request for page 2, then `set_dual_page(true)`, it is -1. (The worker side of rule 2 is Task 4's test; this pins the view's writes.)
  - `DocumentView a worker-built text page outlives its view`: on `selection.pdf` and on `type3.pdf`: read `live_mupdf_roots()` = R0 **before opening the Document**; open it, build the view, take a probe (from `view.document()`), `h = get_worker_text(view, 0)`, destroy the view → `copy(full_range)` on `h` still works (`ABBA` for `type3.pdf`); resetting `h` lowers the count by 1; resetting the probe → `live_mupdf_roots() == R0`.
- [ ] **Step 2: Run** `"[view][text_prefetch]"` → fails to compile.
- [ ] **Step 3: Implement** per the table.
- [ ] **Step 4: Run** `"[view]"`, then the full suite → all pass.
- [ ] **Step 5: Commit** `feat(core): keep one prefetched text page per tab (#98)`.

## Task 6: Deliver the text to the canvas, and use it

Spec: §3.4, §3.7, §7.

**Files:**
- Modify: `src/ui/PdfCanvas.hpp:30-130`, `src/ui/PdfCanvas.cpp:104-202` (meta + post helper), `:609-626` (`select_all`), `:707-772` (`on_left_button_down`), `:1136` (message switch), `:1339-1373` (`resubmit_current_page`), `:1682-1742` (`navigate_to_page`)
- Modify: `src/ui/MainWindow.cpp:59` (reservation comment), `:383-388` (`kick_render`)
- Modify: `CHANGELOG.md` (`## [Unreleased]`)

**Interfaces:**
- Consumes: `DocumentView::request_render_with_prefetch(int, RenderCb, TextCb)`, `offer_text(Document::TextPage)`, `text_for(int)` (Task 5).
- Produces: `inline constexpr UINT WM_USER_TEXT_READY = WM_USER + 5;` in `PdfCanvas.hpp`; `static bool PdfCanvas::post_text_ready(HWND target, litepdf::core::Document::TextPage text, std::uint64_t epoch);`, public, declared beside `post_render_done` (MainWindow calls it).

- [ ] **Step 1: Message and post helper.** `PdfCanvas.hpp` includes `core/Document.hpp` and defines the message beside `WM_USER_RENDER_DONE_RIGHT` with a comment (LPARAM = heap `TextMeta`, freed by the handler). `MainWindow.cpp:59`'s comment names `WM_USER + 3` to `+ 5` as PdfCanvas's. In `PdfCanvas.cpp`'s anonymous namespace: `struct TextMeta { litepdf::core::Document::TextPage text; std::uint64_t epoch = 0; };`. `post_text_ready`: `new (std::nothrow) TextMeta{std::move(text), epoch}` → null: return false (the handle dies on the worker); `PostMessageW(target, WM_USER_TEXT_READY, 0, reinterpret_cast<LPARAM>(meta))` fails → `delete meta`, return false; else true. For the never-dispatched-at-exit case, point at the #74 paragraph at `:168-179` instead of repeating it.
- [ ] **Step 2: Handler.** `case WM_USER_TEXT_READY:` adopt the meta into a `std::unique_ptr<TextMeta>`; if `!impl_->view || meta->epoch != impl_->view_epoch` return 0 (the handle is dropped; the view may belong to a closed tab); else `impl_->view->offer_text(std::move(meta->text))`; return 0. No `InvalidateRect`.
- [ ] **Step 3: Three submission sites** pass a third argument capturing that site's HWND local and epoch: in `MainWindow::kick_render` (`:385`) and `PdfCanvas::resubmit_current_page` (`:1369`) the local is `target`: `[target, epoch](litepdf::core::Document::TextPage t) { PdfCanvas::post_text_ready(target, std::move(t), epoch); }`; in `PdfCanvas::navigate_to_page` (`:1736`) the local is `target_hwnd` (there `target` is the `int` page parameter), so capture and pass `target_hwnd`. The spread branches use `request_render` and stay untouched.
- [ ] **Step 4: Consumers.** In `select_all` (`:613`) and `on_left_button_down` (`:743-744`) replace `impl_->view->document().text_page(static_cast<std::size_t>(page))` with `impl_->view->text_for(page)`. Nothing else in either function changes.
- [ ] **Step 5: CHANGELOG.** Under `## [Unreleased]` add a `### Fixed` heading (none exists yet) with: "A page's text is now read in the background once the page is shown, so a text-selection press or Select All on it no longer pauses LitePDF. A press that comes in before that background read has finished still reads the text on the spot, as before (#98)."
- [ ] **Step 6: Build all targets and run the full suite** with the two Global Constraints commands → `litepdf.exe` and `litepdf-cli.exe` link; all tests pass.
- [ ] **Step 7: Commit** `perf(ui): select with the page text the render worker prefetched (#98)`.

## Task 7: Scripted GUI check and measurement (nothing committed)

Spec: §5.2, §5.4. Everything runs from a throwaway worktree and `build/gui-check/` (untracked). Before writing the driver read `C:\Users\User\.claude\projects\C--Users-User-projects-litepdf\memory\reference_litepdf_scripted_gui_smoke.md` ("Read first" list); worktree build recipe: `reference_litepdf_build_test_commands.md`, section "Building in a Claude worktree" (junction `third_party/mupdf`; remove it with `cmd /c rmdir` before deleting the worktree).

**Files (untracked):** `build/gui-check/make-probe-98.sh`, `build/gui-check/check98.ps1`, `build/gui-check/results-98.md`

- [ ] **Step 1: Probe exes.** In a throwaway worktree at this branch's HEAD add `fprintf(stderr, ...)` + `fflush(stderr)`: `text: slot` / `text: build` in `DocumentView::text_for`; `text: stored <page>` in `offer_text` when it stores; `text: worker <page> <ms>` around the builder call in `maybe_deliver_text` (`QueryPerformanceCounter`); `render: accepted <page> <qpc-ms>` where the `WM_USER_RENDER_DONE` handler assigns `bitmap_page`. Build Release; copy `litepdf.exe` to `build/gui-check/probe98.exe`. Then force `request_render_with_prefetch`'s attach condition false, rebuild, copy to `probe98-off.exe`. The main checkout's `git status` is unchanged afterwards.
- [ ] **Step 2: Driver cases** (back up `session.json` first and restore it byte-for-byte in a `finally` block, even when a case fails; set `LITEPDF_NO_RESTORE=1`; close with `WM_CLOSE`; redirect stderr to a file and poll it). Ids: `IDM_EDIT_SELECT_ALL` 40072, `IDM_EDIT_COPY` 40071, `IDM_TAB_CLOSE` 40030, sent as `WM_COMMAND` to the main window; key and mouse messages go to the `LitePDFPdfCanvas` HWND.
  1. **Prefetch hit:** open `tests\fixtures\selection.pdf`; wait (timeout 10 s) for `text: stored 0`; post `WM_LBUTTONDOWN`/`WM_LBUTTONUP` at the canvas centre → the next `text:` line is `text: slot`.
  2. **Negative control:** post `WM_KEYDOWN VK_NEXT` and select-all back to back → `text: build`. If not, the trace is untrustworthy: stop and report.
  3. **Correctness:** post `VK_NEXT` until on page 3; wait for `text: stored 3`; post select-all, then copy; find `editPopup` as the submenu of `GetMenu(main)` that contains command 40071 (`GetMenuState(sub, 40071, MF_BYCOMMAND) != (UINT)-1`), then `SendMessage(main, WM_INITMENUPOPUP, editPopup, 1)` then `GetMenuState(editPopup, 40071, MF_BYCOMMAND)` is enabled; the clipboard text with all whitespace removed equals `alphabetagammadeltaepsilon` (page 3 of the fixture is "alpha beta gamma" over "delta epsilon", per `scripts/generate-selection-fixture.py`).
  4. **Password gate:** open `tests\fixtures\encrypted.pdf`, answer the prompt with `test`; select-all twice → `text: build`, then `text: slot`; no `text: stored` line appears before the first select-all.
  5. **Paging storm:** fresh `selection.pdf`; post 7 `VK_NEXT` back to back; wait for `text: stored 7`; select-all → `text: slot`. Record the number of `text: worker` lines (fewer than 8 is the expected shape; not asserted).
  6. **Close storm:** 20 times: launch `probe98.exe tests\fixtures\type3.pdf` (the running instance receives it through the single-instance forwarder and opens a tab), wait for its `render: accepted`, then post 40030 → the process is alive at the end, no crash dialog, stderr has no `error:` lines from MuPDF.
- [ ] **Step 3: Measurement (spec §5.4).** #73's sample PDFs were never kept; regenerate them into `build/gui-check/` with Edge (`C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe --headless=new --print-to-pdf=<out.pdf> <file:///in.html>`) from: `third_party/mupdf/thirdparty/gumbo-parser/benchmarks/html5_spec.html` (the HTML spec), `.../benchmarks/xinhua.html` (the news page) and `C:\Python314\Doc\html\genindex-all.html`. Today's Edge may paginate differently from #73's, so first run `build\Release\litepdf-cli.exe <pdf> --bench-selection` on each and pick the pages with the highest `acq1st` as the Type3-heavy targets (in #73 they were the spec's p145 and p752 and the news page's p2). With `probe98.exe`: visit those pages; record each `text: worker` ms on first visit, and after paging away and back (served from L1, possibly on the other worker). With `probe98.exe` and then `probe98-off.exe`: post 20 `VK_NEXT` at 150 ms intervals on the spec and record each post-to-`render: accepted` latency. Write both tables into `results-98.md`; nothing is asserted.
- [ ] **Step 4: Report** each case's observed line(s) and the two tables. Confirm `session.json` matches the backup; remove the junction (`cmd /c rmdir`), then the throwaway worktree. Nothing is committed; the tables go into the PR description later.

## Task 8: Lift the password gate once #126 has merged (conditional)

Run only if `origin/main` contains #126's merge (see Global Constraints). Otherwise skip, and the PR description says the gate stays until #126 lands. Spec: §3.6, §8.

**Files:**
- Modify: `src/core/DocumentView.cpp` (`wants_text_prefetch`), `src/core/Document.hpp/.cpp` (remove `needed_password` if unused), `tests/unit/test_text_prefetch_view.cpp`, `tests/unit/test_text_prefetch_engine.cpp`, `tests/unit/test_document_password.cpp`, spec §3.6 (a one-line addendum: lifted by #126; the PR-merge gate reviews this addendum with the rest of the diff)

- [ ] **Step 1: Rebase** on `origin/main`. Resolve the `DocumentView`/`RenderEngine` constructor conflicts by keeping #126's password parameter (third in `DocumentView`, in whatever form #126 merged it) and this branch's additions. Update every positional worker count in this branch's tests to pass `{}` first (e.g. `DocumentView(std::move(doc), disp, {}, 1)`). Build; run the full suite.
- [ ] **Step 2: Failing test first.** Add `tests/fixtures/encrypted-text.pdf` (from #126) to Task 4's equivalence oracle, opened, authenticated, and with the engine constructed with the password the way #126's tests construct it → passes. Change Task 5's gate case to expect `wants_text_prefetch(0)` **true** on `encrypted-text.pdf` after `authenticate` → fails.
- [ ] **Step 3:** Remove `!doc.needed_password()` from `wants_text_prefetch`. If `rg needed_password src tests` then finds only its own test, remove the accessor and that test.
- [ ] **Step 4:** Full suite passes. Commit `perf(core): prefetch text on password-protected PDFs too (#98, after #126)`.
