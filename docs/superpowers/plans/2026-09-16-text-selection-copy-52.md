# Text Selection + Copy (#52), with the Escrow Lock-Table Fix (#61) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the reader select text on a page by drag, double-click and triple-click, copy it with Ctrl+C, and select a whole page with Ctrl+A — after first making every cloned MuPDF context safe to drop once its Document is gone.

**Architecture:** Two PRs. **PR-0 (#61)** extracts the MuPDF lock table into a shared-ownership `detail::MuPDFLocks` and adds `core::EscrowContext`, a move-only cloned context that holds the table alive and releases it last; the canvas's per-render escrow moves onto it. **PR-1 (#52)** builds `Document::TextPage` — an stext page on its own `EscrowContext` — over MuPDF's selection API, stores one `TextSelection` per `DocumentView`, drives it from a pure gesture state machine in `ui/detail/SelectionDrag.hpp`, paints it in the canvas overlay, and exposes Copy / Select All through a new Edit menu and focus-dispatched accelerators.

**Tech Stack:** C++20, Win32, Direct2D, MuPDF 1.27.2 (`fz_snap_selection` / `fz_highlight_selection` / `fz_copy_selection`), Catch2 v3.5.4, CMake + MSVC v143, reportlab 4.4.10 for the fixture.

**Source spec:** `docs/superpowers/specs/2026-09-16-text-selection-copy-hand-tool-design.md` (on this branch). Issue #61 is the spec for Part A.

**Scope of this plan:** PR-0 (#61) and PR-1 (#52). **PR-2 (#58, hand-tool panning) gets its own plan after PR-1 merges** — the same one-plan-per-PR shape as PR-A1 / PR-A2 / PR-B. PR-1 does build the shared groundwork §5 consumes (`Gesture::Panning`, `MouseButton`, `begin_pan`, `ReleaseAction::EndPan` in the pure state machine), because the exclusivity rules in spec §4.2 cannot be stated or tested without it.

---

## Plan-time corrections to the spec — read before writing any code

Every claim in the spec was re-verified against the vendored MuPDF source and `main` @ `88513f2` while writing this plan. Seven corrections. **The spec file has been patched in the same commit as this plan**, so a reviewer comparing the two sees them agree; this section records *why*, so nobody "fixes" the plan back toward the old text.

**C1 — Select All by character ORIGINS drops the page's last character.** Spec §3.4 fed `full_range()` the first and last characters' origins. MuPDF resolves a point to the nearest character *boundary* (`find_closest_in_line`, `stext-search.c`: each character contributes a boundary at `idx` from its `ll` edge and one at `idx + 1` from its `lr` edge), and a selection is the half-open range `[start, end)` (`fz_enumerate_selection` returns on `++idx == end`). A character's origin sits on its *leading* edge, so the last character's origin resolves to the boundary **before** it and the range stops one short. `full_range()` therefore returns the **leading-edge midpoint of the first character** (`mid(ll, ul)`) and the **trailing-edge midpoint of the last** (`mid(lr, ur)`). Task 6 tests the final character explicitly.

**C2 — the page-box-origin problem in spec §4.1 does not exist.** `pdf_page_obj_transform_box` (`source/pdf/pdf-page.c`) ends with `*page_ctm = fz_concat(*page_ctm, fz_translate(-cropbox.x0, -cropbox.y0))` — MuPDF already moves the CropBox origin to `(0,0)` in page space, so `fz_bound_page`, the render bbox and stext quads all share an origin-free frame. Every other format LitePDF opens hard-codes a zero origin (`xps_bound_page`, `svg_bound_page`, `epub_bound_page`, `htdoc_bound_page` for FB2, `cbz_bound_page`, `img_bound_page`). Consequences: **no translation code in `TextPage`, and `Document::page_hits` is not touched.** The spec's non-zero-origin fixture is kept — as a CropBox-offset page that *pins* this MuPDF behaviour, so an upgrade that changed it fails a test instead of silently misplacing highlights.

**C3 — `snap()` passes points through unchanged in `Chars` mode.** MuPDF's own viewer snaps only for words and lines (`platform/gl/gl-main.c`, the `fz_snap_selection` calls under `GLUT_ACTIVE_CTRL`). This makes spec §6.1 item 5 (Chars round-trip idempotence) moot: nothing is snapped, so nothing can drift.

**C4 — `fz_snap_selection` never writes the far end when it lies past the page's last character.** Its end branch runs only for a character at `idx >= end`; when `end` is the index after the last character there is none, and the caller's raw **second** point is left in place. On a backward word or line drag that point is the *earlier* one, collapsing the selection to part of a word. Reachable: double-click in the blank space below the last line, then drag up. `TextPage::snap` detects "either raw point resolves to the end of the text" and restores the far end to the trailing edge from C1. Tested in Task 6.

**C5 — the Edit menu's enable state must follow the focus.** Spec §4.8 grays Copy when there is no document selection. But `TranslateAcceleratorW` sends `WM_INITMENUPOPUP` before acting on an accelerator, and **an accelerator whose menu item is grayed is disabled — the keystroke is consumed and no `WM_COMMAND` is sent.** Live-verified while writing this plan (Windows 11 build 26200, a scratch Win32 window with a menu and an accelerator table): with the item grayed, `WM_INITMENUPOPUP` arrived with `HIWORD(lParam) == 0`, `TranslateAcceleratorW` returned 1, and no `WM_COMMAND` followed; re-enabling the item inside that `WM_INITMENUPOPUP` handler made the `WM_COMMAND` arrive. Graying as specified would therefore kill Ctrl+C and Ctrl+A inside the find box whenever the page has no selection. The Edit arm enables both items whenever an edit control holds the focus — the same test `WM_COMMAND` dispatches on. Select All is also grayed in two-page spread mode, where it cannot do anything (spec §1). Task 9 re-checks this in the real app with real keystrokes.

**C6 — spec §6.1 item 4's "more than 0.5 em" gap is wrong; the threshold is 0.8 em.** In `stext-device.c`, forward motion between `SPACE_DIST` (0.15 em) and `SPACE_MAX_DIST` (0.8 em) inserts a **synthetic space** whose quad spans the gap — `fz_highlight_selection` then merges both runs into one quad. Only motion past 0.8 em starts a new stext line. The fixture's gap is ~22 em.

**C7 — #61 lands first, and the three-member release order collapses into `EscrowContext`.** Spec §3.3 put `locks`, `escrow` and `page` in `TextPage::Impl` with a hand-ordered destructor. PR-0 moves "drop the context, then release the lock table" into `EscrowContext`, so `TextPage::Impl` is `{EscrowContext escrow; fz_stext_page* stext;}` with a destructor body that drops `stext` through the escrow. The same class fixes #61's render escrow, which is why it is one abstraction and not two.

Smaller refinements, not contradictions: entering two-page spread mode cancels a live drag; the canvas paints a **live** selection while a gesture is in progress and commits to `DocumentView` only at the end (so cancelling a drag leaves a double click's committed word intact); a page change cancels a live drag; a gesture that captures no text commits nothing; the pure-logic test file is `test_selection_drag.cpp`, named after its header `SelectionDrag.hpp`, not the spec's `test_selection_math.cpp`; the spec's §6.3 duplicated closing paragraph is removed.

---

## Plan-gate findings, folded in

Full tier, high-stakes (architecture-changing: MuPDF context and lock-table ownership, the canvas's mouse-capture lifecycle). Round 1 ran Fable (slot 1, mechanical) and Sonnet (slot 2, consistency) in parallel against `3a59dce`. Everything below is resolved in the text.

**Sonnet** — one Critical, one Important:

- **The task headings could not be extracted.** They were `Task A0` … `Task B6`, and `superpowers:subagent-driven-development`'s `scripts/task-brief` only recognises `Task <digits>` (`/^#+[ \t]+Task[ \t]+[0-9]+/`); it exited 3 for every task. Confirmed by running it. Tasks are now numbered 1-11 (Part A = 1-4, Part B = 5-11), and `task-brief` extracts each.
- **The GUI driver lived in an appendix no task brief contains.** It is now written out in full inside Task 9 (to the git-ignored `build/gui/selection-drive.ps1`) and repeated in Task 10, and the known-limitations table moved into this header.
- Noted below its bar and fixed anyway: Task 7 quoted a column-aligned source line with single spaces.

**Fable** — no Critical or Important; two Minor and four questions. It drove every Task 6 fixture expectation through MuPDF 1.27.2 itself (PyMuPDF 1.27.2.2 wraps the vendored version) and all 17 reproduced, as did both Task 6 Step 9 discriminations.

- **Entering spread mode mid-drag did not cancel the drag** (Minor, verified: `DocumentView::set_current_page` returns false for the page it is already on, so the page snap after `IDM_VIEW_DUAL_PAGE` does not always reach `change_current_page`'s cancel). `set_dual_page` now cancels; Task 10 GUI check 9b tests it.
- **The encrypted-document test passed vacuously** (Minor, verified: `encrypted.pdf` extracts `''`). Renamed and rewritten to assert only what the fixture can show — no handle before `authenticate`, a valid one after — and to say plainly that no query runs on decrypted text.
- Questions folded in: `EscrowContext::clone_from`'s real precondition is that the lock table is alive, not merely the source pointer; `at_text_end`'s accepted false positive is "glyphs narrower than 1 pt" (`same_point` truncates to `int`), not "zero-extent"; the commit trailer is the implementing session's own; and a bare `cmake` in Git Bash resolves a MinGW build here, so the constraint now names the BuildTools copy explicitly.

---

## Global Constraints

- **Catch2 `TEST_CASE` names are ASCII and start with their subsystem** (`EscrowContext …`, `DocumentSelection …`, `DocumentView …`, `SelectionDrag …`, `ClipboardText …`, `ViewportMath …`). `catch_discover_tests` mangles non-ASCII names on Windows, and `ctest -R` matches the **name**, never the tag. Confirm every new filter with `ctest --test-dir build -C Release -N -R <filter>` and check the count before trusting a green run from it.
- **Tests build Release, never Debug** (MuPDF is `MT_StaticRelease`; Debug fails with `LNK2038`). Verify through `ctest --test-dir build -C Release`, not only by running the test exe.
- **Use the VS 2022 BuildTools `cmake` and `ctest`**, which configured `build/`: `"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"` and `ctest.exe` beside it. This plan writes `cmake` / `ctest` for brevity. **Do not trust a bare `cmake` on PATH:** in Git Bash on this machine `which cmake` resolves a MinGW WinLibs build (`…\WinGet\Packages\BrechtSanders.WinLibs…\mingw64\bin\cmake`), which is not the generator that configured `build/`.
- **Run tests from the repo root** — fixtures resolve relative to it.
- **`VERSION` is not bumped** (stays `1.3.0`); the About-dialog literal in `MainWindow.cpp` stays untouched. Bumps happen at phase boundaries only.
- **Binary size:** `build/Release/litepdf.exe` must stay under **19,000,000 bytes**. v1.3.0 shipped at 7,276,544 bytes. Record the baseline and the final size.
- **PIMPL discipline:** `Document.hpp`, `DocumentView.hpp`, `EscrowContext.hpp`, `TextSelection.hpp` and `PdfCanvas.hpp` stay free of `<mupdf/fitz.h>`. `MuPDFLocks.hpp` includes it and is included only from `.cpp` files in `litepdf_core`.
- **Cite symbols, not line numbers**, in code comments and commit messages — line numbers go stale (PR #43 paid for that twice).
- **Never use `large.pdf` page 0 to judge rendering or hit geometry** — it overlaps its own text by design.
- **All artifacts in English.** Commit messages end with the implementing session's own `Co-Authored-By:` attribution trailer. The trailers written into this plan's commit examples are Opus 5's; a session running a different model substitutes its own.
- **CHANGELOG** entries go under `## [Unreleased]`; no version heading.

---

## Branches and PR shape

| Part | Branch | Base | Closes |
|---|---|---|---|
| A — PR-0 | `fix/escrow-lock-lifetime` | `main` | #61 |
| B — PR-1 | `feat/text-selection-copy` (already holds the spec and this plan) | rebased onto `main` **after PR-0 merges** | #52 |

Part B depends on Part A's `EscrowContext`. Do not start Part B until PR-0 is merged and `feat/text-selection-copy` is rebased.

---

## File Structure

### Part A — PR-0 (#61)

| File | Status | Responsibility |
|---|---|---|
| `src/core/MuPDFLocks.hpp` | create | Internal. The lock table as a `shared_ptr`-owned object; recovers the table from any context in its family. Includes `fitz.h`. |
| `src/core/MuPDFLocks.cpp` | create | Lock callbacks, `create()`, `of(ctx)`, the live-table counter. |
| `src/core/EscrowContext.hpp` | create | Public, MuPDF-free. Move-only cloned context that keeps the lock table alive and releases it last. |
| `src/core/EscrowContext.cpp` | create | `clone_from`, ordered release. |
| `src/core/Document.cpp` | modify | Drop the file-local lock table; `Impl::locks` becomes `shared_ptr<detail::MuPDFLocks>`. |
| `src/core/Document.hpp` | modify | `clone_context` doc comment names the hazard and points at `EscrowContext`. |
| `src/ui/PdfCanvas.cpp` | modify | `RenderMeta` holds an `EscrowContext`; every drop path uses it. |
| `src/ui/PdfCanvas.hpp` | modify | Comments on `WM_USER_RENDER_DONE` and `post_render_done`. |
| `CMakeLists.txt` | modify | Two new `litepdf_core` sources. |
| `tests/unit/test_escrow_context.cpp` | create | The #61 regression test. |
| `tests/CMakeLists.txt` | modify | Register it. |
| `CHANGELOG.md` | modify | `### Fixed` bullet. |

### Part B — PR-1 (#52)

| File | Status | Responsibility |
|---|---|---|
| `src/core/TextSelection.hpp` | create | Pure data: `SelPoint`, `SelectMode`, `Quad`, `TextSelection`. |
| `src/core/Document.hpp` / `.cpp` | modify | `Document::TextPage` and `Document::text_page`. |
| `scripts/generate-selection-fixture.py` | create | Deterministic generator for `selection.pdf`, with `--check`. |
| `tests/fixtures/selection.pdf` | create | Generated; six pages, each pinning one behaviour. |
| `tests/unit/test_document_selection.cpp` | create | Engine-layer tests. |
| `src/core/DocumentView.hpp` / `.cpp` | modify | One `std::optional<TextSelection>` per tab. |
| `tests/unit/test_document_view.cpp` | modify | Selection storage tests. |
| `src/ui/detail/ViewportMath.hpp` | modify | `client_px_to_dip`, `dip_to_pdf_point`. |
| `src/ui/detail/SelectionDrag.hpp` | create | Pure gesture logic: `Gesture`, `MouseButton`, `PointerMetrics`, `ClickCounter`, `GestureState`, `ReleaseAction`, `canvas_dip_to_page_point`. |
| `src/ui/detail/ClipboardText.hpp` | create | `utf8_to_utf16`. |
| `tests/unit/test_viewport_math.cpp` | modify | The two new mappings. |
| `tests/unit/test_selection_drag.cpp` | create | State machine, click counting, clamping. |
| `tests/unit/test_clipboard_text.cpp` | create | Conversion. |
| `src/ui/Clipboard.hpp` / `.cpp` | create | `set_clipboard_text` with the full failure-path discipline. |
| `src/ui/PdfCanvas.hpp` / `.cpp` | modify | Overlay guard + selection painting, `select_all`, `copy_selection_to_clipboard` (Task 9); gestures, cursor, teardown (Task 10). |
| `src/ui/MainWindow.cpp` | modify | Accelerators, Edit `WM_COMMAND` arms, ownership-based `WM_INITMENUPOPUP`. |
| `src/ui/MainWindow.hpp` | modify | The `tabs_` / `canvas_` declaration-order comment. |
| `resources/MainMenu.rc.h` | modify | `IDM_EDIT_COPY`, `IDM_EDIT_SELECT_ALL`. |
| `resources/litepdf.rc.in` | modify | The Edit popup. |
| `CMakeLists.txt` | modify | `src/ui/Clipboard.cpp` in the `litepdf` executable. |
| `tests/CMakeLists.txt` | modify | Three new test files. |
| `CHANGELOG.md` | modify | `### Added` and `### Fixed` bullets. |

---

## Known limitations (recorded, not fixed)

| | |
|---|---|
| R1 | No selection in two-page spread mode — neither painted nor startable (spec §1). |
| R2 | No cross-page selection; revisit with continuous scroll (#55). |
| R3 | Rotated text highlights as an axis-aligned box, as search hits already do. |
| R4 | Acquiring a `TextPage` (every left press on a page, and Select All) builds the page's stext on the UI thread under `doc_mutex`, contending with a running search scan. Bounded: once per gesture; the query path is lock-free. |
| R5 | Marquee selection and the right-click context menu are their own issues. |
| R6 | `popup_owns` would false-positive if a popup gained a nested submenu. |
| R7 | Right-to-left text: `full_range` uses the left/right quad edges MuPDF uses for left-to-right characters. Untested — no RTL fixture exists. |
| R8 | A double click in blank space below the last line selects the page's last word — MuPDF resolves the point to the end of the text, and word snapping extends back to the word start. Chrome selects nothing there. |
| R9 | The I-beam cursor shows over any page area, including images and blank margins inside the page box. |

---

# Part A — PR-0: escrow lock-table lifetime (#61)

## Task 1: Branch and baseline

**Files:** none.

- [ ] **Step 1: Create the branch in the main checkout — not a worktree**

```bash
git status --short
git fetch origin
git switch -c fix/escrow-lock-lifetime origin/main
```

`git status --short` must print nothing first. **Do not use a `git worktree` here.** MuPDF's build output lives inside the submodule's own tree (`third_party/mupdf/platform/win32/x64/Release`, see `cmake/ImportMuPDF.cmake`), so a fresh worktree starts with an uninitialised submodule and a full MuPDF rebuild. The main checkout's `build/` is already configured and its MuPDF is already built.

- [ ] **Step 2: Record the baseline**

```bash
cmake --build build --config Release
ctest --test-dir build -C Release
```

Record the passing count (v1.3.0 shipped with 303/303 — re-measure, do not quote) and the byte size of `build/Release/litepdf.exe`. Both go into the PR description.

---

## Task 2: `MuPDFLocks` and `EscrowContext`

**Files:**
- Create: `src/core/MuPDFLocks.hpp`, `src/core/MuPDFLocks.cpp`, `src/core/EscrowContext.hpp`, `src/core/EscrowContext.cpp`
- Create: `tests/unit/test_escrow_context.cpp`
- Modify: `src/core/Document.cpp`, `src/core/Document.hpp`, `CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `Document::clone_context()` (unchanged).
- Produces:
  - `class litepdf::core::EscrowContext` — `EscrowContext() noexcept`, move-only, `static EscrowContext clone_from(fz_context* source) noexcept`, `bool valid() const noexcept`, `fz_context* get() const noexcept`. Destruction drops the context, then releases the lock table.
  - `std::size_t litepdf::core::detail::live_lock_tables() noexcept` — test observability.
  - `struct litepdf::core::detail::MuPDFLocks` (internal) — `static std::shared_ptr<MuPDFLocks> create()`, `static std::shared_ptr<MuPDFLocks> of(fz_context*) noexcept`, members `mutexes`, `fz`.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_escrow_context.cpp`:

```cpp
// #61: a cloned fz_context must stay safe to drop after the Document it came
// from has been destroyed.
//
// fz_clone_context copies the whole context, including the lock callbacks whose
// `user` pointer names the Document's lock table. MuPDF keeps its own master
// context alive as a husk until the last clone dies, but it knows nothing about
// OUR table -- so a bare clone dropped after its Document calls fz_lock through
// freed memory. core::EscrowContext holds the table alive and releases it only
// after it has dropped its own context.
//
// The table count is what makes this test discriminating: release-mode heap
// reuse would let a use-after-free "pass" silently, but a table freed with its
// Document shows up as a count that dropped too early.
#include "core/Document.hpp"
#include "core/EscrowContext.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <utility>

// The unit-test target has no MuPDF include path (litepdf_core links MuPDF
// privately). fz_drop_context is plain extern "C", so a local declaration is
// enough -- the same approach as test_document_clone_context.cpp.
extern "C" {
struct fz_context;
void fz_drop_context(fz_context* ctx);
}

using litepdf::core::Document;
using litepdf::core::EscrowContext;
using litepdf::core::detail::live_lock_tables;

TEST_CASE("EscrowContext is empty for a null source", "[core][escrow]") {
    EscrowContext escrow = EscrowContext::clone_from(nullptr);
    REQUIRE_FALSE(escrow.valid());
    REQUIRE(escrow.get() == nullptr);
}

TEST_CASE("EscrowContext keeps the lock table alive after its Document is destroyed",
          "[core][escrow]") {
    const std::size_t before = live_lock_tables();
    EscrowContext escrow;
    {
        Document doc;
        REQUIRE(live_lock_tables() == before + 1);
        REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());

        // The render path clones its escrow from a WORKER context, which is
        // itself a clone of the Document's. Mirror that.
        fz_context* worker = doc.clone_context();
        REQUIRE(worker != nullptr);
        escrow = EscrowContext::clone_from(worker);
        fz_drop_context(worker);
        REQUIRE(escrow.valid());
    }

    // The Document is gone. Its lock table must not be.
    REQUIRE(live_lock_tables() == before + 1);

    // Dropping the escrow drops its context through the table, then the table.
    escrow = EscrowContext{};
    REQUIRE(live_lock_tables() == before);
}

TEST_CASE("EscrowContext move transfers ownership exactly once", "[core][escrow]") {
    const std::size_t before = live_lock_tables();
    {
        Document doc;
        REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());
        fz_context* worker = doc.clone_context();
        REQUIRE(worker != nullptr);

        EscrowContext a = EscrowContext::clone_from(worker);
        fz_drop_context(worker);
        REQUIRE(a.valid());
        const fz_context* raw = a.get();

        EscrowContext b = std::move(a);
        REQUIRE_FALSE(a.valid());
        REQUIRE(b.get() == raw);

        EscrowContext c;
        c = std::move(b);
        REQUIRE_FALSE(b.valid());
        REQUIRE(c.get() == raw);
    }
    REQUIRE(live_lock_tables() == before);
}
```

Register it in `tests/CMakeLists.txt`, inside `target_sources(litepdf_unit_tests PRIVATE ...)`, after `unit/test_status_bar_math.cpp`:

```cmake
    unit/test_escrow_context.cpp       # #61 escrow lock-table lifetime
```

- [ ] **Step 2: Run it to verify it fails**

```bash
cmake --build build --config Release --target litepdf_unit_tests
```

Expected: **compile error** — `core/EscrowContext.hpp` does not exist.

- [ ] **Step 3: Create `src/core/MuPDFLocks.hpp`**

```cpp
#pragma once

// INTERNAL to litepdf_core: includes <mupdf/fitz.h>. Include it only from .cpp
// files in src/core -- never from a public header.
//
// The lock table MuPDF calls through for every context in one Document's family:
// the Document's own context and every fz_clone_context of it. MuPDF requires a
// table at fz_new_context time before it will clone at all, because clones share
// the store, the font cache and the glyph cache and must serialise access to them.
//
// It is owned through std::shared_ptr, not by the Document alone. fz_clone_context
// copies the lock callbacks -- including `fz.user`, which points HERE -- into
// every clone, and a clone can outlive the Document (a render completion still in
// the message queue when its tab closes; a text-selection handle mid-drag). Every
// such long-lived clone is a core::EscrowContext, which holds a strong reference.
// See #61.

#include <mupdf/fitz.h>

#include <array>
#include <memory>
#include <mutex>

namespace litepdf::core::detail {

struct MuPDFLocks : std::enable_shared_from_this<MuPDFLocks> {
    std::array<std::mutex, FZ_LOCK_MAX> mutexes;
    fz_locks_context                    fz{};

    MuPDFLocks();
    ~MuPDFLocks();
    MuPDFLocks(const MuPDFLocks&)            = delete;
    MuPDFLocks& operator=(const MuPDFLocks&) = delete;

    // A new table with its callbacks installed and `fz.user` pointing at it,
    // ready to pass to fz_new_context.
    static std::shared_ptr<MuPDFLocks> create();

    // The table `ctx` locks through, or empty if `ctx` is null or does not lock
    // through a litepdf table (a context created by the CLI, a test, or MuPDF
    // itself). PRECONDITION: that table must still be alive -- this reads
    // `ctx->locks.user` before taking any reference. See
    // EscrowContext::clone_from.
    static std::shared_ptr<MuPDFLocks> of(fz_context* ctx) noexcept;
};

}  // namespace litepdf::core::detail
```

- [ ] **Step 4: Create `src/core/MuPDFLocks.cpp`**

```cpp
#include "core/MuPDFLocks.hpp"

#include "core/EscrowContext.hpp"  // declares detail::live_lock_tables

#include <atomic>
#include <cstddef>

namespace litepdf::core::detail {

namespace {

std::atomic<std::size_t> g_live_tables{0};

void litepdf_lock(void* user, int lock) {
    static_cast<MuPDFLocks*>(user)->mutexes[static_cast<std::size_t>(lock)].lock();
}

void litepdf_unlock(void* user, int lock) {
    static_cast<MuPDFLocks*>(user)->mutexes[static_cast<std::size_t>(lock)].unlock();
}

}  // namespace

MuPDFLocks::MuPDFLocks() {
    g_live_tables.fetch_add(1, std::memory_order_relaxed);
}

MuPDFLocks::~MuPDFLocks() {
    g_live_tables.fetch_sub(1, std::memory_order_relaxed);
}

std::shared_ptr<MuPDFLocks> MuPDFLocks::create() {
    auto table = std::make_shared<MuPDFLocks>();
    table->fz.user   = table.get();
    table->fz.lock   = &litepdf_lock;
    table->fz.unlock = &litepdf_unlock;
    return table;
}

std::shared_ptr<MuPDFLocks> MuPDFLocks::of(fz_context* ctx) noexcept {
    // Recognise our table by its callback before trusting `user`: a context made
    // anywhere else may carry a different lock implementation, or none.
    if (!ctx || ctx->locks.lock != &litepdf_lock || !ctx->locks.user) return {};
    return static_cast<MuPDFLocks*>(ctx->locks.user)->weak_from_this().lock();
}

std::size_t live_lock_tables() noexcept {
    return g_live_tables.load(std::memory_order_relaxed);
}

}  // namespace litepdf::core::detail
```

- [ ] **Step 5: Create `src/core/EscrowContext.hpp`**

```cpp
#pragma once

// core::EscrowContext -- a cloned fz_context that stays safe to drop after the
// Document it was cloned from has been destroyed.
//
// A bare fz_clone_context is NOT that. The clone carries a raw pointer to the
// Document's MuPDF lock table, and every fz_keep_* / fz_drop_* -- including the
// clone's own fz_drop_context -- locks through it. MuPDF keeps its master context
// alive until the last clone dies, but it knows nothing about our table (#61).
// An EscrowContext holds a strong reference to that table for its whole life and
// releases it LAST, after the context has been dropped.
//
// Contract for anything freed through get() -- a pixmap, an stext page: free it
// BEFORE this object is destroyed or assigned over. The destructor drops only the
// context.
//
// Header stays MuPDF-free (PIMPL discipline): the lock table is held type-erased.

#include <cstddef>
#include <memory>

struct fz_context;

namespace litepdf::core {

class EscrowContext {
public:
    EscrowContext() noexcept = default;
    ~EscrowContext();

    EscrowContext(EscrowContext&& other) noexcept;
    EscrowContext& operator=(EscrowContext&& other) noexcept;
    EscrowContext(const EscrowContext&)            = delete;
    EscrowContext& operator=(const EscrowContext&) = delete;

    // Clone `source`, a Document's context or a clone of one. PRECONDITION: the
    // lock table `source` locks through must still be alive for the duration of
    // this call -- i.e. its Document, or some EscrowContext of the same family,
    // is alive. `source` merely being a live pointer is NOT enough: recovering
    // the table reads `source`'s lock-callback `user` pointer before any
    // reference is taken, so cloning from a bare clone whose Document has died
    // is exactly the #61 use-after-free, one call earlier. Every caller today
    // runs while the Document is alive (Document::text_page under doc_mutex; the
    // render callback on a RenderEngine worker, which the Document outlives).
    // Empty on a null source, a context that does not lock through a litepdf
    // lock table, or a failed clone (out of memory). Thread-safe.
    [[nodiscard]] static EscrowContext clone_from(fz_context* source) noexcept;

    [[nodiscard]] bool        valid() const noexcept { return ctx_ != nullptr; }
    [[nodiscard]] fz_context* get()   const noexcept { return ctx_; }

private:
    // Drop the context, THEN release the table. fz_drop_context takes
    // FZ_LOCK_ALLOC through the table, so the order is the whole point.
    void reset() noexcept;

    std::shared_ptr<void> locks_;
    fz_context*           ctx_ = nullptr;
};

namespace detail {

// Number of litepdf MuPDF lock tables alive in the process. Test observability
// for #61 only -- nothing in the product reads it.
[[nodiscard]] std::size_t live_lock_tables() noexcept;

}  // namespace detail

}  // namespace litepdf::core
```

- [ ] **Step 6: Create `src/core/EscrowContext.cpp`**

```cpp
#include "core/EscrowContext.hpp"

#include "core/MuPDFLocks.hpp"

#include <utility>

namespace litepdf::core {

EscrowContext::~EscrowContext() {
    reset();
}

EscrowContext::EscrowContext(EscrowContext&& other) noexcept
    : locks_(std::move(other.locks_)),
      ctx_(std::exchange(other.ctx_, nullptr)) {}

EscrowContext& EscrowContext::operator=(EscrowContext&& other) noexcept {
    if (this != &other) {
        reset();
        locks_ = std::move(other.locks_);
        ctx_   = std::exchange(other.ctx_, nullptr);
    }
    return *this;
}

void EscrowContext::reset() noexcept {
    if (ctx_) {
        fz_drop_context(ctx_);
        ctx_ = nullptr;
    }
    locks_.reset();
}

EscrowContext EscrowContext::clone_from(fz_context* source) noexcept {
    EscrowContext out;
    std::shared_ptr<detail::MuPDFLocks> table = detail::MuPDFLocks::of(source);
    if (!table) return out;
    fz_context* clone = fz_clone_context(source);
    if (!clone) return out;
    out.locks_ = std::move(table);
    out.ctx_   = clone;
    return out;
}

}  // namespace litepdf::core
```

- [ ] **Step 7: Move `Document` onto the shared table**

In `src/core/Document.cpp`:

1. Add `#include "core/MuPDFLocks.hpp"` after `#include "core/Document.hpp"`.
2. **Delete** the first anonymous-namespace block of the file — the one containing `struct MuPDFLocks`, `litepdf_lock` and `litepdf_unlock` (it starts with the comment "MuPDF requires a lock table (FZ_LOCK_MAX entries)"). That code now lives in `MuPDFLocks.cpp`.
3. In `struct Document::Impl`, replace

```cpp
    std::unique_ptr<MuPDFLocks> locks;
```

with

```cpp
    // Shared, not unique: every core::EscrowContext cloned from this Document's
    // family holds a copy, so the table outlives the last escrow even when this
    // Document is destroyed first (#61). Declared FIRST so it is destroyed LAST,
    // after ~Impl has dropped the document and the context through it.
    std::shared_ptr<detail::MuPDFLocks> locks;
```

4. Replace the `Impl()` constructor's first four lines

```cpp
    Impl() : locks(std::make_unique<MuPDFLocks>()) {
        locks->fz.user = locks.get();
        locks->fz.lock = &litepdf_lock;
        locks->fz.unlock = &litepdf_unlock;
        ctx = fz_new_context(nullptr, &locks->fz, FZ_STORE_DEFAULT);
```

with

```cpp
    Impl() : locks(detail::MuPDFLocks::create()) {
        ctx = fz_new_context(nullptr, &locks->fz, FZ_STORE_DEFAULT);
```

The rest of the constructor and `~Impl()` are unchanged.

In `src/core/Document.hpp`, in the comment above `clone_context()`, append after the "Caller owns the returned pointer…" paragraph:

```cpp
    // A bare clone must NOT outlive this Document: its lock callbacks point into
    // the Document's lock table. A context that has to survive the Document --
    // anything posted across a thread or held across a tab close -- must be a
    // core::EscrowContext (core/EscrowContext.hpp), which keeps the table alive.
```

In `CMakeLists.txt`, in `add_library(litepdf_core STATIC ...)`, after `src/core/Document.cpp`:

```cmake
    src/core/EscrowContext.cpp     # #61
    src/core/MuPDFLocks.cpp        # #61
```

- [ ] **Step 8: Run the tests**

```bash
cmake --build build --config Release --target litepdf_unit_tests
ctest --test-dir build -C Release -N -R EscrowContext
ctest --test-dir build -C Release -R EscrowContext --output-on-failure
```

Expected: `-N` lists **3** tests; all 3 PASS.

- [ ] **Step 9: Prove the count test discriminates**

Temporarily edit `EscrowContext::clone_from` so it does **not** keep the table: replace `out.locks_ = std::move(table);` with `table.reset();`. Rebuild and run `ctest --test-dir build -C Release -R "EscrowContext keeps"`.

Expected: **FAIL** on `live_lock_tables() == before + 1` after the Document block. The test may instead **crash** when the escrow is dropped afterwards — that is the very use-after-free #61 describes, and it counts as the expected failure too. Revert the edit, rebuild, confirm PASS again, and confirm the file matches your Step 6 content.

- [ ] **Step 10: Full suite and commit**

```bash
ctest --test-dir build -C Release
```

Expected: baseline count + 3, all passing.

```bash
git add src/core/MuPDFLocks.hpp src/core/MuPDFLocks.cpp src/core/EscrowContext.hpp src/core/EscrowContext.cpp src/core/Document.cpp src/core/Document.hpp CMakeLists.txt tests/unit/test_escrow_context.cpp tests/CMakeLists.txt
git commit -m "fix(core): keep the MuPDF lock table alive for escrowed contexts

A cloned fz_context carries a raw pointer to its Document's lock table.
Document owned that table through a unique_ptr, so a clone dropped after
the Document called fz_lock through freed memory (#61).

The table is now shared-owned (detail::MuPDFLocks), and core::EscrowContext
holds a strong reference for as long as its clone lives, releasing it only
after dropping the context.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 3: Move the render escrow onto `EscrowContext`

**Files:**
- Modify: `src/ui/PdfCanvas.cpp`, `src/ui/PdfCanvas.hpp`

**Interfaces:**
- Consumes: `core::EscrowContext::clone_from`, `valid()`, `get()` (Task 2).
- Produces: no new interface. `post_render_done` / `post_render_done_right` keep their signatures and refcount contract.

- [ ] **Step 1: Includes and forward declarations**

In `src/ui/PdfCanvas.cpp`, add `#include "core/EscrowContext.hpp"` after `#include "core/DocumentView.hpp"`, and in the `extern "C"` block **delete** these two now-unused declarations:

```cpp
    fz_context* fz_clone_context(fz_context*);
    void        fz_drop_context(fz_context*);
```

- [ ] **Step 2: `RenderMeta`**

Replace the `struct RenderMeta` definition with:

```cpp
struct RenderMeta {
    litepdf::core::EscrowContext escrow;
    std::uint64_t     epoch = 0;
    int               page  = 0;
    litepdf::ui::Slot slot  = litepdf::ui::Slot::Left;
    std::uint64_t     seq   = 0;
};
```

- [ ] **Step 3: `post_render_done_impl`**

Replace everything in `post_render_done_impl` after the `if (!pix) { ... return true; }` block with:

```cpp
    // The escrow keeps the Document's lock table alive as well as cloning the
    // context, so the UI thread can drop this pixmap even if the tab closes
    // before the message is handled (#61).
    litepdf::core::EscrowContext escrow =
        litepdf::core::EscrowContext::clone_from(worker_ctx);
    if (!escrow.valid()) {
        fz_drop_pixmap(worker_ctx, pix);
        return false;
    }
    // Allocate BEFORE moving the escrow in, so the failure path still owns it.
    auto* meta = new (std::nothrow) RenderMeta{};
    if (!meta) {
        fz_drop_pixmap(escrow.get(), pix);
        return false;   // `escrow` drops its context, then the table
    }
    meta->escrow = std::move(escrow);
    meta->epoch  = epoch;
    meta->page   = page;
    meta->slot   = slot;
    meta->seq    = seq;
    if (!PostMessageW(target, msg,
                      reinterpret_cast<WPARAM>(pix),
                      reinterpret_cast<LPARAM>(meta))) {
        fz_drop_pixmap(meta->escrow.get(), pix);
        delete meta;
        return false;
    }
    return true;
```

Add `#include <utility>` after `#include <stdexcept>` (it is not included today).

- [ ] **Step 4: The completion handler**

In `handle_message`, in the `case WM_USER_RENDER_DONE: case WM_USER_RENDER_DONE_RIGHT:` block, replace

```cpp
            fz_context* escrow = meta->escrow;
            const std::uint64_t epoch = meta->epoch;
            const int           page  = meta->page;
            const Slot          slot  = meta->slot;
            const std::uint64_t seq   = meta->seq;
            delete meta;
            if (!escrow) {
                // Defensive: meta without escrow — nothing safe to drop.
                return 0;
            }
```

with

```cpp
            // Adopt the escrow before freeing the meta. It must outlive every
            // fz_drop_pixmap below; it is destroyed when this block exits, which
            // drops its context and only then the lock table (#61).
            litepdf::core::EscrowContext escrow = std::move(meta->escrow);
            const std::uint64_t epoch = meta->epoch;
            const int           page  = meta->page;
            const Slot          slot  = meta->slot;
            const std::uint64_t seq   = meta->seq;
            delete meta;
            if (!escrow.valid()) {
                // Defensive: meta without escrow — nothing safe to drop.
                return 0;
            }
            fz_context* const ectx = escrow.get();
```

Then, **in the rest of that same block only**:
- Replace each of the three `fz_drop_pixmap(escrow, pix);` + `fz_drop_context(escrow);` pairs with the single line `fz_drop_pixmap(ectx, pix);`.
- Replace `escrow` with `ectx` in the four accessor calls `fz_pixmap_width`, `fz_pixmap_height`, `fz_pixmap_stride`, `fz_pixmap_samples`.

After the edit, `grep -n "fz_drop_context\|fz_clone_context" src/ui/PdfCanvas.cpp` must print nothing.

- [ ] **Step 5: Header comments**

In `src/ui/PdfCanvas.hpp`, replace the first six lines of the comment above `WM_USER_RENDER_DONE` —

```cpp
// Posted by render-done callback. WPARAM = fz_pixmap* (kept by worker),
// LPARAM = a heap RenderMeta* { fz_context* escrow clone; the render's
// {epoch, page, slot, seq} identity }.
// On cancel/fail both are null. Canvas drops the pixmap through escrow,
// then drops escrow — staying on the pixmap's own MuPDF root even if the
// producing DocumentView has been swapped or destroyed. The identity is
```

— with

```cpp
// Posted by render-done callback. WPARAM = fz_pixmap* (kept by worker),
// LPARAM = a heap RenderMeta* { core::EscrowContext escrow; the render's
// {epoch, page, slot, seq} identity }.
// On cancel/fail both are null. Canvas drops the pixmap through the escrow,
// then lets the escrow go — staying on the pixmap's own MuPDF root even if the
// producing DocumentView has been swapped or destroyed. The escrow also keeps
// the Document's lock table alive; without that, the drop would call through
// freed memory once the tab had closed (#61). The identity is
```

In the comment above `post_render_done`, replace

```cpp
    // Post WM_USER_RENDER_DONE to `target` for the (pixmap, ctx) pair,
    // where `ctx` is a clone-escrow made from `worker_ctx` so the UI
    // thread can drop the pixmap with the correct MuPDF root — even if
    // the producing DocumentView is torn down before the message lands.
```

with

```cpp
    // Post WM_USER_RENDER_DONE to `target` for the pixmap, together with a
    // core::EscrowContext cloned from `worker_ctx` so the UI thread can drop
    // the pixmap with the correct MuPDF root and a live lock table — even if
    // the producing DocumentView is torn down before the message lands.
```

and in the same comment replace `escrow otherwise — and then drops the escrow ctx.` with `escrow otherwise — and then releases the escrow.`

- [ ] **Step 6: Build, test, smoke**

```bash
cmake --build build --config Release
ctest --test-dir build -C Release
```

Expected: build clean, same count as after Task 2, all passing.

Smoke (a smoke, **not** proof — the race cannot be forced from outside; Task 2's test is the proof): with `LITEPDF_NO_RESTORE=1`, open `tests/fixtures/large.pdf` and `tests/fixtures/search.pdf` in one instance, hold PgDn on one tab while pressing Ctrl+W, reopen, repeat five times. Expected: no crash, the remaining tab keeps rendering.

- [ ] **Step 7: Commit**

```bash
git add src/ui/PdfCanvas.cpp src/ui/PdfCanvas.hpp
git commit -m "fix(canvas): carry render completions on an EscrowContext

The per-render escrow was a bare fz_clone_context, dropped on the UI
thread when WM_USER_RENDER_DONE is handled. A completion still queued when
its tab closed dropped through the freed lock table (#61). RenderMeta now
holds a core::EscrowContext; every path drops the pixmap through it first.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 4: CHANGELOG, PR, merge gate

**Files:**
- Modify: `CHANGELOG.md`

- [ ] **Step 1: CHANGELOG**

Under `## [Unreleased]` add:

```markdown
### Fixed

- A page render that finished just after its tab was closed could free its image
  through memory the closed document had already released. The window was narrow
  and no crash has been observed; the lock table that drop depends on now lives as
  long as the last thing that uses it (#61).
```

```bash
git add CHANGELOG.md
git commit -m "docs(changelog): record the escrow lock-table fix

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

- [ ] **Step 2: Full verification**

```bash
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Record the passing count and the exact `litepdf.exe` byte size.

- [ ] **Step 3: Merge gate, then PR**

Invoke the `risk-tiered-review` skill to classify the tier and run the lenses (never drop the Codex lens). Name this adversarial question for it: *is there any other path where a context cloned from a Document outlives it — `DocumentView`'s `ui_ctx` / `cache_ctx`, `RenderEngine`'s worker contexts, `ThumbnailRenderer` — and does anything posted across a thread still carry a bare clone?*

Then push and open the PR: title `fix: keep the MuPDF lock table alive for escrowed contexts`, body with the test counts, exe size, the Step 9 discrimination result from Task 2, and `Closes #61`. Merge when `build-windows` is green.

---

# Part B — PR-1: text selection + copy (#52)

## Task 5: Rebase and baseline

**Files:** none.

- [ ] **Step 1: Rebase onto the merged PR-0**

```bash
git switch feat/text-selection-copy
git fetch origin
git rebase origin/main
git log --oneline origin/main..HEAD
```

Expected: only the spec and plan commits on top of `origin/main`, and `src/core/EscrowContext.hpp` present.

- [ ] **Step 2: Baseline**

```bash
cmake --build build --config Release
ctest --test-dir build -C Release
```

Record the passing count and `litepdf.exe` size.

---

## Task 6: The engine layer — `TextSelection` and `Document::TextPage`

**Files:**
- Create: `src/core/TextSelection.hpp`
- Modify: `src/core/Document.hpp`, `src/core/Document.cpp`
- Create: `scripts/generate-selection-fixture.py`, `tests/fixtures/selection.pdf` (generated)
- Create: `tests/unit/test_document_selection.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `core::EscrowContext` (Part A).
- Produces:
  - `litepdf::core::SelPoint { float x, y; }`, `enum class SelectMode { Chars, Words, Lines }`, `Quad { float ul_x, ul_y, ur_x, ur_y, ll_x, ll_y, lr_x, lr_y; }`, `TextSelection { int page; SelPoint anchor, extent; SelectMode mode; std::vector<Quad> quads; std::string text_utf8; }`.
  - `class Document::TextPage` — `TextPage() noexcept`, move-only; `bool valid() const noexcept`; `int page() const noexcept`; `struct Snapped { SelPoint a, b; }`; `Snapped snap(SelPoint a, SelPoint b, SelectMode mode) const noexcept`; `std::vector<Quad> highlight(SelPoint a, SelPoint b) const`; `bool full_range(SelPoint& first, SelPoint& last) const noexcept`; `std::string copy(SelPoint a, SelPoint b) const`.
  - `Document::TextPage Document::text_page(std::size_t page) const noexcept`.

- [ ] **Step 1: Write the fixture generator**

Create `scripts/generate-selection-fixture.py`:

```python
#!/usr/bin/env python3
"""
Generate tests/fixtures/selection.pdf, the fixture for the text-selection engine
tests in tests/unit/test_document_selection.cpp (#52).

Page index -> what it pins:

  0  Two columns of unequal length (8 lines left, 4 right). Select All must copy
     both columns, down to the page's final character.
  1  One baseline, two runs ~270 pt apart. MuPDF starts a new stext line for
     horizontal motion wider than 0.8 em (SPACE_MAX_DIST in stext-device.c), so
     the highlight is two quads rather than one bar across the whitespace.
  2  No text at all.
  3  "alpha beta gamma" over "delta epsilon": word, line and backward-drag
     snapping.
  4  30 rows x 10 runs, 55 pt apart: 300 separate highlight quads -- past the
     256-quad initial buffer, so the grow-and-retry path runs.
  5  CropBox [36 36 576 756], "ORIGIN" drawn at user space (108, 684). MuPDF moves
     the CropBox origin to (0, 0), so the word starts at page space (72, 72).
     MUST STAY THE LAST PAGE: reportlab applies a CropBox to the page that sets
     it and to every page after.

Byte-reproducible: rl_config.invariant pins the timestamp and document ID, and
pageCompression=0 leaves no zlib stream whose bytes would depend on the
interpreter's zlib build (see scripts/generate-large-fixture.py). Generated with
reportlab==4.4.10.

Usage:
  python scripts/generate-selection-fixture.py           # write the fixture
  python scripts/generate-selection-fixture.py --check   # exit 1 if it would change
"""

import argparse
import io
import os
import sys

from reportlab import rl_config

rl_config.invariant = 1

from reportlab.lib.colors import black, white  # noqa: E402
from reportlab.pdfgen import canvas  # noqa: E402

PAGE_W, PAGE_H = 612, 792  # US Letter, points

OUT_PATH = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "tests", "fixtures", "selection.pdf"))


def fill_page_white(c):
    """Explicit white page fill; see generate-search-fixture.py for why."""
    c.saveState()
    c.setFillColor(white)
    c.rect(0, 0, PAGE_W, PAGE_H, fill=1, stroke=0)
    c.restoreState()
    c.setFillColor(black)


def page_two_columns(c):
    c.setFont("Helvetica", 12)
    for i in range(8):
        c.drawString(72, 720 - 20 * i, "Left column line %d" % (i + 1))
    for i in range(4):
        c.drawString(324, 720 - 20 * i, "Right column line %d" % (i + 1))


def page_wide_gap(c):
    c.setFont("Helvetica", 12)
    c.drawString(72, 720, "LEFTRUN")
    c.drawString(400, 720, "RIGHTRUN")


def page_blank(c):
    pass


def page_words(c):
    c.setFont("Helvetica", 12)
    c.drawString(72, 720, "alpha beta gamma")
    c.drawString(72, 700, "delta epsilon")


def page_many_runs(c):
    c.setFont("Helvetica", 8)
    for row in range(30):
        for col in range(10):
            c.drawString(40 + 55 * col, 740 - 20 * row, "xx")


def page_crop_offset(c):
    c.setCropBox((36, 36, 576, 756))
    c.setFont("Helvetica", 12)
    c.drawString(108, 684, "ORIGIN")


PAGES = [
    page_two_columns,
    page_wide_gap,
    page_blank,
    page_words,
    page_many_runs,
    page_crop_offset,  # must stay last -- see the module docstring
]


def build():
    buf = io.BytesIO()
    c = canvas.Canvas(buf, pagesize=(PAGE_W, PAGE_H), pageCompression=0)
    for draw in PAGES:
        fill_page_white(c)
        draw(c)
        c.showPage()
    c.save()
    return buf.getvalue()


def main():
    parser = argparse.ArgumentParser(
        description="Generate tests/fixtures/selection.pdf")
    parser.add_argument("--check", action="store_true",
                        help="exit 1 if the committed fixture would change")
    args = parser.parse_args()

    data = build()
    if b"/FlateDecode" in data:
        print("selection.pdf unexpectedly contains a compressed stream",
              file=sys.stderr)
        return 1

    if args.check:
        with open(OUT_PATH, "rb") as f:
            if f.read() != data:
                print("tests/fixtures/selection.pdf is stale; rerun without --check",
                      file=sys.stderr)
                return 1
        return 0

    with open(OUT_PATH, "wb") as f:
        f.write(data)
    print("wrote %s (%d bytes)" % (OUT_PATH, len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 2: Generate and check it**

```bash
python -c "import reportlab; print(reportlab.Version)"
python scripts/generate-selection-fixture.py
python scripts/generate-selection-fixture.py --check
```

Expected: `4.4.10`; `wrote ...selection.pdf (N bytes)`; `--check` exits 0. Then confirm the CropBox is on page 5 only-and-after:

```bash
python -c "import re; d=open('tests/fixtures/selection.pdf','rb').read(); print(len(re.findall(rb'/CropBox', d)), len(re.findall(rb'/MediaBox', d)))"
```

Expected: `1 6`.

- [ ] **Step 3: Write the failing tests**

Create `tests/unit/test_document_selection.cpp`:

```cpp
// Text selection, engine layer (#52): Document::TextPage against
// tests/fixtures/selection.pdf (scripts/generate-selection-fixture.py documents
// what each page pins) and the existing multi-format fixtures.
#include "core/Document.hpp"
#include "core/EscrowContext.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

using litepdf::core::Document;
using litepdf::core::SelectMode;
using litepdf::core::SelPoint;

namespace {

constexpr std::size_t kTwoColumns = 0;
constexpr std::size_t kWideGap    = 1;
constexpr std::size_t kBlank      = 2;
constexpr std::size_t kWords      = 3;
constexpr std::size_t kManyRuns   = 4;
constexpr std::size_t kCropOffset = 5;

Document open_fixture(const char* path) {
    Document doc;
    REQUIRE_FALSE(doc.open(std::filesystem::path(path)).has_value());
    return doc;
}

Document open_selection_fixture() {
    return open_fixture("tests/fixtures/selection.pdf");
}

// A selection copy and page_text() lay out line breaks differently -- CRLF
// versus LF, and page_text adds a blank line after every block -- but must agree
// on every character. Compare with whitespace removed.
std::string without_whitespace(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c != ' ' && c != '\r' && c != '\n' && c != '\t') out.push_back(c);
    }
    return out;
}

// Centre of the single whole-word hit for `word` on `page`.
SelPoint center_of_word(const Document& doc, std::size_t page, const char* word) {
    Document::SearchFlags flags;
    flags.match_case = true;
    flags.whole_word = true;
    const auto hits = doc.page_hits(page, word, flags);
    REQUIRE(hits.size() == 1);
    const auto& h = hits[0];
    return SelPoint{ (h.ul_x + h.ur_x + h.ll_x + h.lr_x) * 0.25f,
                     (h.ul_y + h.ur_y + h.ll_y + h.lr_y) * 0.25f };
}

std::string copy_snapped(const Document::TextPage& text, SelPoint a, SelPoint b,
                         SelectMode mode) {
    const auto s = text.snap(a, b, mode);
    return text.copy(s.a, s.b);
}

// Select All must copy exactly the characters page_text() extracts. On a page
// with no text there is no range, and page_text must agree that it is empty.
void require_select_all_matches_page_text(const Document& doc, std::size_t page) {
    const auto text = doc.text_page(page);
    REQUIRE(text.valid());
    SelPoint first, last;
    if (text.full_range(first, last)) {
        REQUIRE(without_whitespace(text.copy(first, last))
                == without_whitespace(doc.page_text(page)));
    } else {
        REQUIRE(without_whitespace(doc.page_text(page)).empty());
    }
}

}  // namespace

TEST_CASE("DocumentSelection text page is empty when there is nothing to extract",
          "[core][selection]") {
    Document unopened;
    REQUIRE_FALSE(unopened.text_page(0).valid());

    const Document doc = open_selection_fixture();
    REQUIRE_FALSE(doc.text_page(6).valid());      // one past the last page
    REQUIRE_FALSE(doc.text_page(99999).valid());

    const Document::TextPage empty;
    REQUIRE_FALSE(empty.valid());
    REQUIRE(empty.page() == -1);
    SelPoint a, b;
    REQUIRE_FALSE(empty.full_range(a, b));
    REQUIRE(empty.copy(SelPoint{0, 0}, SelPoint{100, 100}).empty());
    REQUIRE(empty.highlight(SelPoint{0, 0}, SelPoint{100, 100}).empty());
}

TEST_CASE("DocumentSelection select all copies both columns and the final character",
          "[core][selection]") {
    const Document doc = open_selection_fixture();
    const auto text = doc.text_page(kTwoColumns);
    REQUIRE(text.valid());
    REQUIRE(text.page() == static_cast<int>(kTwoColumns));

    SelPoint first, last;
    REQUIRE(text.full_range(first, last));
    const std::string copied = without_whitespace(text.copy(first, last));

    REQUIRE(copied == without_whitespace(doc.page_text(kTwoColumns)));
    // The corner-points approach drops the shorter column; the origins approach
    // drops the last character. Name both failures directly.
    REQUIRE(copied.find("Rightcolumnline1") != std::string::npos);
    REQUIRE(copied.substr(copied.size() - 16) == "Rightcolumnline4");
}

TEST_CASE("DocumentSelection select all matches page text across existing fixtures",
          "[core][selection]") {
    for (const char* path : { "tests/fixtures/search.pdf", "tests/fixtures/simple.pdf" }) {
        const Document doc = open_fixture(path);
        for (std::size_t page = 0; page < doc.page_count(); ++page) {
            INFO(path << " page " << page);
            require_select_all_matches_page_text(doc, page);
        }
    }
}

TEST_CASE("DocumentSelection a wide gap on one baseline yields separate quads",
          "[core][selection]") {
    const Document doc = open_selection_fixture();
    const auto text = doc.text_page(kWideGap);
    SelPoint first, last;
    REQUIRE(text.full_range(first, last));
    // One quad would be a highlight bar painted straight across ~270 pt of
    // whitespace -- what a hand-rolled one-quad-per-line walk produces.
    REQUIRE(text.highlight(first, last).size() >= 2);
}

TEST_CASE("DocumentSelection a page with no text has no range and copies nothing",
          "[core][selection]") {
    const Document doc = open_selection_fixture();
    const auto text = doc.text_page(kBlank);
    REQUIRE(text.valid());
    SelPoint first, last;
    REQUIRE_FALSE(text.full_range(first, last));
    REQUIRE(text.highlight(SelPoint{0, 0}, SelPoint{612, 792}).empty());
    REQUIRE(text.copy(SelPoint{0, 0}, SelPoint{612, 792}).empty());
}

TEST_CASE("DocumentSelection highlight grows past its initial quad buffer",
          "[core][selection]") {
    const Document doc = open_selection_fixture();
    const auto text = doc.text_page(kManyRuns);
    SelPoint first, last;
    REQUIRE(text.full_range(first, last));
    // The page has 300 separate runs. A buffer that never grows returns 256.
    REQUIRE(text.highlight(first, last).size() > 256);
}

TEST_CASE("DocumentSelection coordinates are page box relative on a CropBox offset page",
          "[core][selection]") {
    const Document doc = open_selection_fixture();

    // CropBox [36 36 576 756]: the page is 540 x 720 and its corner is page
    // space (0, 0). "ORIGIN" was drawn at user space (108, 684), i.e. 72 pt in
    // from the crop box's left edge, baseline 72 pt below its top.
    const auto size = doc.page_size(kCropOffset);
    REQUIRE(size.width_pt  == Catch::Approx(540.0f).margin(0.01f));
    REQUIRE(size.height_pt == Catch::Approx(720.0f).margin(0.01f));

    Document::SearchFlags flags;
    const auto hits = doc.page_hits(kCropOffset, "ORIGIN", flags);
    REQUIRE(hits.size() == 1);
    REQUIRE(hits[0].ll_x == Catch::Approx(72.0f).margin(1.0f));   // 108 if untranslated
    REQUIRE(hits[0].ul_y < 72.0f);                                 // glyph top above the baseline
    REQUIRE(hits[0].ll_y > 72.0f);                                 // descender below it
    REQUIRE(hits[0].ll_y < 80.0f);

    const auto text = doc.text_page(kCropOffset);
    SelPoint first, last;
    REQUIRE(text.full_range(first, last));
    const auto quads = text.highlight(first, last);
    REQUIRE(quads.size() == 1);
    REQUIRE(quads[0].ll_x == Catch::Approx(72.0f).margin(1.0f));
    REQUIRE(text.copy(first, last) == "ORIGIN");
}

TEST_CASE("DocumentSelection chars mode passes points through unsnapped",
          "[core][selection]") {
    const Document doc = open_selection_fixture();
    const auto text = doc.text_page(kWords);
    const SelPoint a{ 80.0f, 70.0f };
    const SelPoint b{ 150.0f, 90.0f };
    const auto s = text.snap(a, b, SelectMode::Chars);
    REQUIRE(s.a.x == a.x);
    REQUIRE(s.a.y == a.y);
    REQUIRE(s.b.x == b.x);
    REQUIRE(s.b.y == b.y);
}

TEST_CASE("DocumentSelection a stationary double click selects one word",
          "[core][selection]") {
    const Document doc = open_selection_fixture();
    const auto text = doc.text_page(kWords);
    const SelPoint beta = center_of_word(doc, kWords, "beta");
    REQUIRE(copy_snapped(text, beta, beta, SelectMode::Words) == "beta");
}

TEST_CASE("DocumentSelection a triple click selects the whole line",
          "[core][selection]") {
    const Document doc = open_selection_fixture();
    const auto text = doc.text_page(kWords);
    const SelPoint beta = center_of_word(doc, kWords, "beta");
    REQUIRE(copy_snapped(text, beta, beta, SelectMode::Lines) == "alpha beta gamma");
}

TEST_CASE("DocumentSelection a backward word drag selects the same text as a forward one",
          "[core][selection]") {
    const Document doc = open_selection_fixture();
    const auto text = doc.text_page(kWords);
    const SelPoint alpha = center_of_word(doc, kWords, "alpha");
    const SelPoint gamma = center_of_word(doc, kWords, "gamma");

    REQUIRE(copy_snapped(text, alpha, gamma, SelectMode::Words) == "alpha beta gamma");
    REQUIRE(copy_snapped(text, gamma, alpha, SelectMode::Words) == "alpha beta gamma");

    // snap returns the ends in reading order -- which is exactly why a caller
    // must never write the result back over its raw anchor (spec §2).
    const auto s = text.snap(gamma, alpha, SelectMode::Words);
    REQUIRE(s.a.x < s.b.x);
}

TEST_CASE("DocumentSelection a word drag from below the text keeps its far end",
          "[core][selection]") {
    // Plan correction C4. A point below every line resolves to the end of the
    // text, and fz_snap_selection never writes an end that lies past the last
    // character: the caller's raw second point stays in place. Dragging BACKWARD
    // from there, that point is the earlier one, and without the fix the
    // selection collapses to part of "beta".
    const Document doc = open_selection_fixture();
    const auto text = doc.text_page(kWords);
    const SelPoint below{ 300.0f, 600.0f };
    const SelPoint beta = center_of_word(doc, kWords, "beta");

    REQUIRE(copy_snapped(text, below, beta, SelectMode::Words)
            == "beta gamma\r\ndelta epsilon");
    REQUIRE(copy_snapped(text, beta, below, SelectMode::Words)
            == "beta gamma\r\ndelta epsilon");
    REQUIRE(copy_snapped(text, below, beta, SelectMode::Lines)
            == "alpha beta gamma\r\ndelta epsilon");
}

TEST_CASE("DocumentSelection copied text uses CRLF line endings",
          "[core][selection]") {
    const Document doc = open_selection_fixture();
    const auto text = doc.text_page(kWords);
    SelPoint first, last;
    REQUIRE(text.full_range(first, last));
    REQUIRE(text.copy(first, last) == "alpha beta gamma\r\ndelta epsilon");
}

TEST_CASE("DocumentSelection a text page outlives its Document",
          "[core][selection]") {
    const std::size_t tables_before = litepdf::core::detail::live_lock_tables();
    Document::TextPage text;
    {
        const Document doc = open_selection_fixture();
        text = doc.text_page(kWords);
        REQUIRE(text.valid());
    }
    // The Document -- and the context the page was extracted on -- is gone.
    // Closing a tab mid-drag produces exactly this state (spec §3.3).
    REQUIRE(litepdf::core::detail::live_lock_tables() == tables_before + 1);

    SelPoint first, last;
    REQUIRE(text.full_range(first, last));
    REQUIRE(text.copy(first, last) == "alpha beta gamma\r\ndelta epsilon");

    text = Document::TextPage{};
    REQUIRE(litepdf::core::detail::live_lock_tables() == tables_before);
}

TEST_CASE("DocumentSelection CJK text round trips through copy",
          "[core][selection]") {
    const Document doc = open_fixture("tests/fixtures/cjk-zh-hant.pdf");
    const auto text = doc.text_page(0);
    SelPoint first, last;
    REQUIRE(text.full_range(first, last));
    REQUIRE_FALSE(without_whitespace(text.copy(first, last)).empty());
    require_select_all_matches_page_text(doc, 0);
}

TEST_CASE("DocumentSelection a text page is available only after authenticate",
          "[core][selection]") {
    // What this proves, and what it does not. encrypted.pdf's only page has NO
    // text (MuPDF 1.27.2 extracts ''), so no selection query runs on decrypted
    // content here -- the fixture cannot exercise that, and the test does not
    // pretend to. It pins the acquisition contract only: no handle before
    // authenticate (is_open() is false), a valid handle after it, and an empty
    // range that agrees with page_text.
    Document doc;
    const auto err = doc.open("tests/fixtures/encrypted.pdf");
    REQUIRE(err.has_value());
    REQUIRE(*err == Document::OpenError::NeedsPassword);
    REQUIRE_FALSE(doc.text_page(0).valid());   // not open until authenticated
    REQUIRE(doc.authenticate("test"));
    const auto text = doc.text_page(0);
    REQUIRE(text.valid());
    SelPoint first, last;
    REQUIRE_FALSE(text.full_range(first, last));        // the fixture has no text
    REQUIRE(without_whitespace(doc.page_text(0)).empty());
}

TEST_CASE("DocumentSelection works on a reflowable epub page",
          "[core][selection]") {
    const Document doc = open_fixture("tests/fixtures/sample.epub");
    const auto text = doc.text_page(0);
    SelPoint first, last;
    REQUIRE(text.full_range(first, last));
    REQUIRE(without_whitespace(text.copy(first, last)).find("HellofromLitePDFtestePub.")
            != std::string::npos);
    require_select_all_matches_page_text(doc, 0);
}
```

Register in `tests/CMakeLists.txt`, after the `test_escrow_context.cpp` line:

```cmake
    unit/test_document_selection.cpp   # #52 Task 6
```

- [ ] **Step 4: Run to verify it fails**

```bash
cmake --build build --config Release --target litepdf_unit_tests
```

Expected: **compile error** — `SelPoint` / `Document::TextPage` undeclared.

- [ ] **Step 5: Create `src/core/TextSelection.hpp`**

```cpp
#pragma once

// core::TextSelection -- one tab's text selection (#52). Pure data: no MuPDF, no
// Win32, so it is headless-testable and cheap to read on the paint path.

#include <string>
#include <vector>

namespace litepdf::core {

// A position on a page in MuPDF page space: points, top-left origin, y down.
// MuPDF places the page box's top-left corner at (0, 0) for every format LitePDF
// opens -- pdf_page_obj_transform_box translates the CropBox origin, and the
// other formats' bound_page functions hard-code it -- so this is also the frame
// of the rendered bitmap. No translation exists anywhere in the selection path;
// tests/unit/test_document_selection.cpp pins it with a CropBox-offset page.
struct SelPoint {
    float x = 0.0f;
    float y = 0.0f;
};

// FZ_SELECT_CHARS / FZ_SELECT_WORDS / FZ_SELECT_LINES.
enum class SelectMode { Chars, Words, Lines };

struct Quad {
    float ul_x = 0.0f, ul_y = 0.0f;
    float ur_x = 0.0f, ur_y = 0.0f;
    float ll_x = 0.0f, ll_y = 0.0f;
    float lr_x = 0.0f, lr_y = 0.0f;
};

struct TextSelection {
    int page = -1;

    // RAW, UN-SNAPPED pointer positions. fz_snap_selection reorders the points
    // it is given, so writing a snapped result back here would move the anchor
    // on every backward drag and walk the selection across the page (spec §2).
    // Select All is an ordinary selection whose points bracket the page's text.
    SelPoint   anchor{};
    SelPoint   extent{};
    SelectMode mode = SelectMode::Chars;

    // Materialised when the gesture ends. Afterwards painting, Ctrl+C and a tab
    // switch are pure data -- no MuPDF, no lock.
    std::vector<Quad> quads;
    std::string       text_utf8;   // CRLF line endings
};

}  // namespace litepdf::core
```

- [ ] **Step 6: Declare `TextPage` in `src/core/Document.hpp`**

Add `#include "core/TextSelection.hpp"` after `#include <vector>`.

Immediately before the `private:` line of `class Document`, add:

```cpp
    // ------------------------------------------------------------------
    // Text selection (#52)
    // ------------------------------------------------------------------
    // One page's extracted text, independent of this Document's lifetime.
    //
    // The handle owns a ref on the page's fz_stext_page and its OWN
    // core::EscrowContext -- a cloned context that also keeps the MuPDF lock
    // table alive. Both are required: closing a tab destroys the Document
    // before anything tells the canvas (TabList::remove runs before
    // PdfCanvas::set_view), so a handle bound to the Document's context would
    // be dropped through a freed context, and a bare clone through a freed lock
    // table (spec §3.3, #61).
    //
    // Coordinates are MuPDF page space; see SelPoint in core/TextSelection.hpp.
    //
    // Thread-safety: only acquiring a handle (Document::text_page) takes
    // doc_mutex. Every method below is lock-free: it reads an immutable
    // structure the handle holds a ref to, and copy() allocates on the handle's
    // own escrow, whose error stack is its own (spec §3.2).
    class TextPage {
    public:
        TextPage() noexcept;
        ~TextPage();
        TextPage(TextPage&&) noexcept;
        TextPage& operator=(TextPage&&) noexcept;
        TextPage(const TextPage&)            = delete;
        TextPage& operator=(const TextPage&) = delete;

        [[nodiscard]] bool valid() const noexcept;
        [[nodiscard]] int  page()  const noexcept;   // -1 when empty

        // Snapped COPIES. Deliberately returns rather than taking in-out
        // references: fz_snap_selection reorders its in-out points, and writing
        // that back over a raw anchor walks every backward drag (spec §2).
        // Words / Lines: the ends come back in reading order (a before b).
        // Chars: the inputs come back unchanged -- MuPDF's own viewer passes raw
        // points through in that mode, and nothing is gained by snapping.
        struct Snapped { SelPoint a, b; };
        [[nodiscard]] Snapped snap(SelPoint a, SelPoint b, SelectMode mode) const noexcept;

        // Merged per-line highlight quads for the text between a and b.
        [[nodiscard]] std::vector<Quad> highlight(SelPoint a, SelPoint b) const;

        // Two points bracketing every character on the page, for Select All
        // through the ordinary highlight()/copy() path (spec §3.4): the LEADING
        // edge of the first character and the TRAILING edge of the last, each
        // at mid-height. False on a page with no text, or on an empty handle.
        [[nodiscard]] bool full_range(SelPoint& first, SelPoint& last) const noexcept;

        // UTF-8 text between a and b, CRLF line endings. Empty on an empty
        // handle or an allocation failure; never throws a MuPDF error.
        [[nodiscard]] std::string copy(SelPoint a, SelPoint b) const;

    private:
        friend class Document;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    // A handle on page `page`'s text. Empty if the document is not open (an
    // encrypted one counts as not open until authenticate succeeds), `page` is
    // out of range, or extraction or the context clone fails. Callers MUST
    // tolerate an empty handle -- a drag that cannot acquire one simply does not
    // start.
    [[nodiscard]] TextPage text_page(std::size_t page) const noexcept;
```

- [ ] **Step 7: Implement it in `src/core/Document.cpp`**

Add `#include "core/EscrowContext.hpp"` after `#include "core/MuPDFLocks.hpp"`, and `#include <climits>` among the standard includes.

Append, immediately before the file's final `} // namespace litepdf::core`:

```cpp
// ----------------------------------------------------------------------
// Text selection (#52)
// ----------------------------------------------------------------------

struct Document::TextPage::Impl {
    // The destructor body drops `stext` THROUGH `escrow`. Members are destroyed
    // only after the body has run, so the escrow is alive for that drop; the
    // escrow itself then drops its context before releasing the lock table.
    // Net order: stext page -> escrow context -> lock table (spec §3.3).
    EscrowContext  escrow;
    fz_stext_page* stext = nullptr;
    int            page  = -1;

    Impl() = default;
    ~Impl() {
        if (stext) fz_drop_stext_page(escrow.get(), stext);
    }
    Impl(const Impl&)            = delete;
    Impl& operator=(const Impl&) = delete;
};

namespace {

constexpr int kSelectionQuadsInitial = 256;
constexpr int kSelectionQuadsMax     = 1 << 16;

fz_point to_fz(SelPoint p) noexcept { return fz_make_point(p.x, p.y); }
SelPoint from_fz(fz_point p) noexcept { return SelPoint{ p.x, p.y }; }

fz_point midpoint(fz_point a, fz_point b) noexcept {
    return fz_make_point((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
}

// True when `p` resolves to the end of the page's text: nothing lies between it
// and the trailing edge of the last character. One false positive, accepted: a
// trailing run of characters that on_highlight_char skips as "zero-extent" also
// reads as the end. That test is MuPDF's same_point, which truncates to int
// before comparing with 0.1 -- so it skips glyphs narrower than 1 pt, not only
// zero-width ones. Treating such a run as the end extends a copy by characters
// too small to see; it never moves the visible highlight.
bool at_text_end(fz_context* ctx, fz_stext_page* stext, SelPoint p,
                 SelPoint text_end) noexcept {
    fz_quad probe;
    return fz_highlight_selection(ctx, stext, to_fz(p), to_fz(text_end), &probe, 1) == 0;
}

}  // namespace

Document::TextPage::TextPage() noexcept = default;
Document::TextPage::~TextPage() = default;
Document::TextPage::TextPage(TextPage&&) noexcept = default;
Document::TextPage& Document::TextPage::operator=(TextPage&&) noexcept = default;

bool Document::TextPage::valid() const noexcept {
    return impl_ && impl_->stext;
}

int Document::TextPage::page() const noexcept {
    return valid() ? impl_->page : -1;
}

bool Document::TextPage::full_range(SelPoint& first, SelPoint& last) const noexcept {
    if (!valid()) return false;
    const fz_stext_char* head = nullptr;
    const fz_stext_char* tail = nullptr;
    // Top-level text blocks only, in list order: exactly the walk
    // fz_enumerate_selection makes, so "first" and "last" name the same
    // characters to it.
    for (fz_stext_block* block = impl_->stext->first_block; block; block = block->next) {
        if (block->type != FZ_STEXT_BLOCK_TEXT) continue;
        for (fz_stext_line* line = block->u.t.first_line; line; line = line->next) {
            for (fz_stext_char* ch = line->first_char; ch; ch = ch->next) {
                if (!head) head = ch;
                tail = ch;
            }
        }
    }
    if (!head) return false;
    // Edges, NOT origins. MuPDF resolves a point to the nearest character
    // BOUNDARY and selects the half-open range [start, end). A character's
    // origin sits on its leading edge, so the last character's origin resolves
    // to the boundary BEFORE it and would drop the page's final character. The
    // trailing edge resolves to the boundary after it.
    first = from_fz(midpoint(head->quad.ll, head->quad.ul));
    last  = from_fz(midpoint(tail->quad.lr, tail->quad.ur));
    return true;
}

Document::TextPage::Snapped Document::TextPage::snap(SelPoint a, SelPoint b,
                                                     SelectMode mode) const noexcept {
    if (!valid() || mode == SelectMode::Chars) return Snapped{ a, b };

    fz_context* ctx = impl_->escrow.get();
    fz_point pa = to_fz(a);
    fz_point pb = to_fz(b);
    // Cannot throw: walks the page, never allocates.
    fz_snap_selection(ctx, impl_->stext, &pa, &pb,
                      mode == SelectMode::Words ? FZ_SELECT_WORDS : FZ_SELECT_LINES);

    // fz_snap_selection writes the far end only when it finds a character at or
    // after it. When the later point lies past the page's last character there is
    // none, so the caller's raw SECOND point is left in place -- and on a
    // backward drag that is the EARLIER point, collapsing the selection to part
    // of one word. Put the end back.
    SelPoint text_start, text_end;
    if (full_range(text_start, text_end)
        && (at_text_end(ctx, impl_->stext, a, text_end)
            || at_text_end(ctx, impl_->stext, b, text_end))) {
        pb = to_fz(text_end);
    }
    return Snapped{ from_fz(pa), from_fz(pb) };
}

std::vector<Quad> Document::TextPage::highlight(SelPoint a, SelPoint b) const {
    std::vector<Quad> out;
    if (!valid()) return out;

    // fz_highlight_selection fills a caller-owned buffer and silently drops what
    // does not fit. It merges into the last quad BEFORE its capacity test and
    // drops only at len == cap, so "returned count == capacity" is an exact
    // signal to grow and retry (spec §3.1).
    std::vector<fz_quad> buf(static_cast<std::size_t>(kSelectionQuadsInitial));
    int n = 0;
    for (;;) {
        n = fz_highlight_selection(impl_->escrow.get(), impl_->stext, to_fz(a), to_fz(b),
                                   buf.data(), static_cast<int>(buf.size()));
        if (n < static_cast<int>(buf.size())
            || buf.size() >= static_cast<std::size_t>(kSelectionQuadsMax)) {
            break;
        }
        buf.resize(buf.size() * 2);
    }

    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const fz_quad& q = buf[static_cast<std::size_t>(i)];
        out.push_back(Quad{ q.ul.x, q.ul.y, q.ur.x, q.ur.y,
                            q.ll.x, q.ll.y, q.lr.x, q.lr.y });
    }
    return out;
}

std::string Document::TextPage::copy(SelPoint a, SelPoint b) const {
    std::string out;
    if (!valid()) return out;

    fz_context* ctx = impl_->escrow.get();
    char* text = nullptr;
    fz_var(text);
    fz_try(ctx) {
        text = fz_copy_selection(ctx, impl_->stext, to_fz(a), to_fz(b), /*crlf*/ 1);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        return out;   // allocation failure: an empty copy, never an error into a window procedure
    }
    if (!text) return out;
    try {
        out.assign(text);
    } catch (...) {
        fz_free(ctx, text);
        throw;
    }
    fz_free(ctx, text);
    return out;
}

Document::TextPage Document::text_page(std::size_t index) const noexcept {
    TextPage result;
    if (!is_open() || index >= static_cast<std::size_t>(INT_MAX)) return result;

    std::unique_ptr<TextPage::Impl> handle;
    try {
        handle = std::make_unique<TextPage::Impl>();
    } catch (...) {
        return result;
    }

    // Only ACQUISITION serialises with the other users of this Document's
    // context: building the stext page loads a page from impl_->doc. Everything
    // afterwards runs on the handle's escrow, lock-free (spec §3.2).
    std::lock_guard<std::mutex> lk(impl_->doc_mutex);

    handle->escrow = EscrowContext::clone_from(impl_->ctx);
    if (!handle->escrow.valid()) return result;

    fz_context*    ctx   = impl_->ctx;
    fz_page*       page  = nullptr;
    fz_stext_page* stext = nullptr;
    fz_var(page);
    fz_var(stext);
    fz_try(ctx) {
        if (static_cast<int>(index) < fz_count_pages(ctx, impl_->doc)) {
            page = fz_load_page(ctx, impl_->doc, static_cast<int>(index));
            // Pinned to default options, the same as page_text. Two flags must
            // NOT be added (spec §3.1): FZ_STEXT_COLLECT_STRUCTURE nests text
            // under struct blocks the selection walkers skip, and
            // FZ_STEXT_DEHYPHENATE (page_hits) changes line joining so a copy
            // would disagree with page_text.
            fz_stext_options opts = {};
            stext = fz_new_stext_page_from_page(ctx, page, &opts);
        }
    }
    fz_always(ctx) {
        if (page) fz_drop_page(ctx, page);
    }
    fz_catch(ctx) {
        // fz_new_stext_page_from_page drops its partial page before rethrowing.
        fz_report_error(ctx);
        return result;
    }
    if (!stext) return result;

    handle->stext = stext;
    handle->page  = static_cast<int>(index);
    result.impl_  = std::move(handle);
    return result;
}
```

- [ ] **Step 8: Run the tests**

```bash
cmake --build build --config Release --target litepdf_unit_tests
ctest --test-dir build -C Release -N -R DocumentSelection
ctest --test-dir build -C Release -R DocumentSelection --output-on-failure
```

Expected: `-N` lists **17**; all PASS.

**If a fixture expectation fails, investigate the fixture before touching an assertion.** Print `doc.page_text(page)` and the quad list for that page. The expectations follow from MuPDF source read while writing this plan (the plan's corrections C1, C2, C4 and C6); an assertion loosened to pass is a check that can only pass.

- [ ] **Step 9: Prove the two correction tests discriminate**

Each on its own, rebuild, run, confirm FAIL, revert, confirm PASS:

1. In `full_range`, change `last  = from_fz(midpoint(tail->quad.lr, tail->quad.ur));` to `last = from_fz(tail->origin);`. Expected: **"select all copies both columns and the final character" FAILS** (the copied text ends `Rightcolumnline`), and "copied text uses CRLF line endings" FAILS.
2. In `snap`, delete the `if (full_range(...) && ...) { pb = to_fz(text_end); }` block. Expected: **"a word drag from below the text keeps its far end" FAILS**.

Confirm `git diff src/core/Document.cpp` shows only the Step 7 additions afterwards.

- [ ] **Step 10: Full suite and commit**

```bash
ctest --test-dir build -C Release
```

```bash
git add src/core/TextSelection.hpp src/core/Document.hpp src/core/Document.cpp scripts/generate-selection-fixture.py tests/fixtures/selection.pdf tests/unit/test_document_selection.cpp tests/CMakeLists.txt
git commit -m "feat(core): text selection engine layer (Document::TextPage)

A handle on one page's stext, held on its own EscrowContext so it survives
the Document. snap/highlight/full_range/copy wrap MuPDF's selection API;
only acquisition takes doc_mutex.

Select All brackets the page with the first character's leading edge and
the last character's trailing edge -- the last character's origin resolves
to the boundary before it and dropped the final character. snap restores
the far end fz_snap_selection leaves unwritten past the last character.

selection.pdf pins both, the wide-gap quad split, the quad-buffer growth,
and MuPDF's CropBox-origin translation.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 7: One selection per tab in `DocumentView`

**Files:**
- Modify: `src/core/DocumentView.hpp`, `src/core/DocumentView.cpp`
- Modify: `tests/unit/test_document_view.cpp`

**Interfaces:**
- Consumes: `core::TextSelection` (Task 6).
- Produces: `const std::optional<TextSelection>& DocumentView::selection() const noexcept`, `void DocumentView::set_selection(TextSelection selection)`, `void DocumentView::clear_selection() noexcept`.

- [ ] **Step 1: Write the failing tests**

Append to `tests/unit/test_document_view.cpp`:

```cpp
TEST_CASE("DocumentView selection persists across a page change until cleared",
          "[core][view][selection]") {
    InlineDispatcher disp;
    Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/search.pdf").has_value());
    DocumentView view(std::move(doc), disp);
    REQUIRE_FALSE(view.selection().has_value());

    litepdf::core::TextSelection sel;
    sel.page      = 0;
    sel.anchor    = { 10.0f, 20.0f };
    sel.extent    = { 200.0f, 40.0f };
    sel.text_utf8 = "Lorem";
    view.set_selection(sel);

    // Spec §2, model 1b: a page change does not clear it.
    REQUIRE(view.set_current_page(3));
    REQUIRE(view.selection().has_value());
    REQUIRE(view.selection()->page == 0);
    REQUIRE(view.selection()->text_utf8 == "Lorem");

    view.clear_selection();
    REQUIRE_FALSE(view.selection().has_value());
}

TEST_CASE("DocumentView set_selection replaces the previous selection",
          "[core][view][selection]") {
    InlineDispatcher disp;
    DocumentView view(open_simple(), disp);

    litepdf::core::TextSelection first;
    first.page      = 0;
    first.text_utf8 = "first";
    view.set_selection(first);

    litepdf::core::TextSelection second;
    second.page      = 0;
    second.text_utf8 = "second";
    view.set_selection(second);

    REQUIRE(view.selection()->text_utf8 == "second");
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --build build --config Release --target litepdf_unit_tests
```

Expected: compile error — `selection` is not a member of `DocumentView`.

- [ ] **Step 3: Declare it**

In `src/core/DocumentView.hpp`, add `#include <optional>` after `#include <memory>`, and `#include "core/TextSelection.hpp"` after `#include "core/Document.hpp"`.

Immediately before the `// Bulk cancel on rapid nav (Phase 3 Task 11 wiring).` comment, add:

```cpp
    // ------------------------------------------------------------------
    // (#52) This tab's text selection -- at most one (spec §2, model "1b").
    //
    // Document-bound state, so it lives here beside current_page and the zoom,
    // and a tab switch carries it with no code in MainWindow. It also survives
    // a page change: it is cleared only by the canvas (the next click, a new
    // drag) or by clear_selection(). Not persisted to session.json. UI thread
    // only.
    const std::optional<TextSelection>& selection() const noexcept;
    void set_selection(TextSelection selection);
    void clear_selection() noexcept;
```

- [ ] **Step 4: Implement it**

In `src/core/DocumentView.cpp`, in `struct DocumentView::Impl`, after this line (column-aligned in the file, quoted exactly):

```cpp
    bool                   dual_page     = false;  // Phase 8 D10
```

add:

```cpp
    std::optional<TextSelection> selection;       // #52; pure data, order-free
```

After the definition of `DocumentView::set_dual_page`, add:

```cpp
const std::optional<TextSelection>& DocumentView::selection() const noexcept {
    return impl_->selection;
}

void DocumentView::set_selection(TextSelection selection) {
    impl_->selection = std::move(selection);
}

void DocumentView::clear_selection() noexcept {
    impl_->selection.reset();
}
```

Add `#include <utility>` after `#include <stdexcept>` in `DocumentView.cpp`.

- [ ] **Step 5: Run and commit**

```bash
cmake --build build --config Release --target litepdf_unit_tests
ctest --test-dir build -C Release -N -R "DocumentView selection|DocumentView set_selection"
ctest --test-dir build -C Release
```

Expected: `-N` lists **2**; full suite passes.

```bash
git add src/core/DocumentView.hpp src/core/DocumentView.cpp tests/unit/test_document_view.cpp
git commit -m "feat(core): one text selection per DocumentView

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 8: Pure UI logic — mappings, gesture state machine, UTF-16 conversion

**Files:**
- Modify: `src/ui/detail/ViewportMath.hpp`, `tests/unit/test_viewport_math.cpp`
- Create: `src/ui/detail/SelectionDrag.hpp`, `tests/unit/test_selection_drag.cpp`
- Create: `src/ui/detail/ClipboardText.hpp`, `tests/unit/test_clipboard_text.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `core::SelPoint`, `core::SelectMode` (Task 6); `ui::Placement`, `ui::bitmap_px_to_dip` (existing).
- Produces:
  - `float ui::client_px_to_dip(float px, float dpi) noexcept`; `float ui::dip_to_pdf_point(float dip, float zoom_pct) noexcept`.
  - `enum class ui::Gesture { None, Selecting, Panning }`; `enum class ui::MouseButton { Left, Middle }`; `struct ui::PointerMetrics { int drag_cx, drag_cy, dblclk_cx, dblclk_cy; std::uint32_t dblclk_ms; }`.
  - `class ui::ClickCounter` — `core::SelectMode press(bool is_double_click_message, std::uint32_t time_ms, int x_px, int y_px, const PointerMetrics&) noexcept`.
  - `enum class ui::ReleaseAction { None, ClearSelection, CommitSelection, EndPan }`.
  - `class ui::GestureState` — `Gesture gesture() const noexcept`, `core::SelectMode mode() const noexcept`, `bool moved() const noexcept`, `bool begin_select(core::SelectMode, int x_px, int y_px) noexcept`, `bool begin_pan(MouseButton, int x_px, int y_px) noexcept`, `void move(int x_px, int y_px, const PointerMetrics&) noexcept`, `ReleaseAction release(MouseButton) noexcept`, `Gesture abort() noexcept`.
  - `core::SelPoint ui::canvas_dip_to_page_point(float x_dip, float y_dip, const Placement& page, float zoom_pct) noexcept`.
  - `std::wstring ui::utf8_to_utf16(std::string_view utf8)`.

- [ ] **Step 1: Write the failing tests**

Append to `tests/unit/test_viewport_math.cpp` (add `#include <limits>` at the top):

```cpp
TEST_CASE("ViewportMath dip to pdf point inverts pdf point to dip", "[ui][viewport]") {
    using litepdf::ui::dip_to_pdf_point;
    using litepdf::ui::pdf_point_to_dip;
    for (float pct : { 0.25f, 1.0f, 1.5f, 8.0f }) {
        REQUIRE(dip_to_pdf_point(pdf_point_to_dip(123.5f, pct), pct)
                == Catch::Approx(123.5f));
    }
    // Degenerate zoom must not divide by zero or propagate NaN.
    REQUIRE(dip_to_pdf_point(100.0f,  0.0f) == 0.0f);
    REQUIRE(dip_to_pdf_point(100.0f, -1.0f) == 0.0f);
    REQUIRE(dip_to_pdf_point(100.0f, std::numeric_limits<float>::quiet_NaN()) == 0.0f);
}

TEST_CASE("ViewportMath client px to dip uses the render target dpi ratio", "[ui][viewport]") {
    using litepdf::ui::client_px_to_dip;
    REQUIRE(client_px_to_dip(300.0f,  96.0f) == Catch::Approx(300.0f));
    REQUIRE(client_px_to_dip(300.0f, 144.0f) == Catch::Approx(200.0f));
    REQUIRE(client_px_to_dip(300.0f,   0.0f) == Catch::Approx(300.0f));
}
```

Create `tests/unit/test_selection_drag.cpp`:

```cpp
// #52: the pure gesture logic PdfCanvas drives (ui/detail/SelectionDrag.hpp).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "ui/detail/SelectionDrag.hpp"

using litepdf::core::SelectMode;
using litepdf::ui::canvas_dip_to_page_point;
using litepdf::ui::ClickCounter;
using litepdf::ui::Gesture;
using litepdf::ui::GestureState;
using litepdf::ui::MouseButton;
using litepdf::ui::Placement;
using litepdf::ui::PointerMetrics;
using litepdf::ui::ReleaseAction;

TEST_CASE("SelectionDrag click count maps presses to select modes", "[ui][selection]") {
    const PointerMetrics m;   // dblclk 500 ms, 4 x 4 px rectangle
    ClickCounter clicks;
    REQUIRE(clicks.press(false, 1000, 50, 50, m) == SelectMode::Chars);
    REQUIRE(clicks.press(true,  1200, 50, 50, m) == SelectMode::Words);
    REQUIRE(clicks.press(false, 1400, 51, 49, m) == SelectMode::Lines);
    // A fourth press is a fresh single click. (Win32 itself reports the fourth
    // as the second half of a new double click, i.e. WM_LBUTTONDBLCLK -> Words.)
    REQUIRE(clicks.press(false, 1600, 50, 50, m) == SelectMode::Chars);
}

TEST_CASE("SelectionDrag a third press too late or too far is a single click",
          "[ui][selection]") {
    const PointerMetrics m;
    ClickCounter late;
    late.press(false, 1000, 50, 50, m);
    late.press(true,  1100, 50, 50, m);
    REQUIRE(late.press(false, 1601, 50, 50, m) == SelectMode::Chars);

    ClickCounter far;
    far.press(false, 1000, 50, 50, m);
    far.press(true,  1100, 50, 50, m);
    REQUIRE(far.press(false, 1200, 53, 50, m) == SelectMode::Chars);   // |dx| 3 > 4/2

    ClickCounter plain;   // a single click followed by another is not a triple
    plain.press(false, 1000, 50, 50, m);
    REQUIRE(plain.press(false, 1100, 50, 50, m) == SelectMode::Chars);
}

TEST_CASE("SelectionDrag triple click timing survives the message clock wrapping",
          "[ui][selection]") {
    const PointerMetrics m;
    ClickCounter clicks;
    clicks.press(false, 0xFFFFFE00u, 50, 50, m);
    clicks.press(true,  0xFFFFFF00u, 50, 50, m);
    // 0x100 ms after the double click, across the 32-bit wrap.
    REQUIRE(clicks.press(false, 0x00000000u, 50, 50, m) == SelectMode::Lines);
}

TEST_CASE("SelectionDrag a click that never moves clears and a drag commits",
          "[ui][selection]") {
    const PointerMetrics m;   // drag threshold 4 px
    GestureState g;

    REQUIRE(g.begin_select(SelectMode::Chars, 100, 100));
    g.move(104, 96, m);                                   // exactly at the threshold
    REQUIRE_FALSE(g.moved());
    REQUIRE(g.release(MouseButton::Left) == ReleaseAction::ClearSelection);
    REQUIRE(g.gesture() == Gesture::None);

    REQUIRE(g.begin_select(SelectMode::Chars, 100, 100));
    g.move(105, 100, m);                                  // one past it
    g.move(100, 100, m);                                  // and back: still a drag
    REQUIRE(g.moved());
    REQUIRE(g.release(MouseButton::Left) == ReleaseAction::CommitSelection);
}

TEST_CASE("SelectionDrag a stationary double click commits a word", "[ui][selection]") {
    // CS_DBLCLKS delivers DOWN, UP, DBLCLK, UP -- and no WM_MOUSEMOVE at all.
    // A rule that cleared every release without movement would destroy the word
    // the double click just selected (spec §4.2).
    const PointerMetrics m;
    ClickCounter clicks;
    GestureState g;

    SelectMode mode = clicks.press(false, 1000, 50, 50, m);
    REQUIRE(g.begin_select(mode, 50, 50));
    REQUIRE(g.release(MouseButton::Left) == ReleaseAction::ClearSelection);

    mode = clicks.press(true, 1100, 50, 50, m);
    REQUIRE(mode == SelectMode::Words);
    REQUIRE(g.begin_select(mode, 50, 50));
    REQUIRE(g.release(MouseButton::Left) == ReleaseAction::CommitSelection);
}

TEST_CASE("SelectionDrag release decides before the capture changed abort runs",
          "[ui][selection]") {
    const PointerMetrics m;
    GestureState g;

    // PdfCanvas order: release() first, then ReleaseCapture -- whose synchronous
    // WM_CAPTURECHANGED calls abort(). abort() must find nothing live.
    REQUIRE(g.begin_select(SelectMode::Chars, 100, 100));
    g.move(140, 100, m);
    REQUIRE(g.release(MouseButton::Left) == ReleaseAction::CommitSelection);
    REQUIRE(g.abort() == Gesture::None);

    // The other order loses the drag: abort() ends it, release() has nothing to
    // decide, and a selection highlighted under the held button copies nothing.
    REQUIRE(g.begin_select(SelectMode::Chars, 100, 100));
    g.move(140, 100, m);
    REQUIRE(g.abort() == Gesture::Selecting);
    REQUIRE(g.release(MouseButton::Left) == ReleaseAction::None);
}

TEST_CASE("SelectionDrag only one gesture may be live under interleaved buttons",
          "[ui][selection]") {
    GestureState g;

    REQUIRE(g.begin_select(SelectMode::Chars, 10, 10));
    REQUIRE_FALSE(g.begin_pan(MouseButton::Middle, 10, 10));
    REQUIRE_FALSE(g.begin_select(SelectMode::Words, 10, 10));
    REQUIRE(g.release(MouseButton::Middle) == ReleaseAction::None);   // not its gesture
    REQUIRE(g.gesture() == Gesture::Selecting);
    REQUIRE(g.release(MouseButton::Left) == ReleaseAction::ClearSelection);

    REQUIRE(g.begin_pan(MouseButton::Middle, 0, 0));
    REQUIRE_FALSE(g.begin_select(SelectMode::Chars, 0, 0));
    REQUIRE(g.release(MouseButton::Left) == ReleaseAction::None);
    REQUIRE(g.gesture() == Gesture::Panning);
    REQUIRE(g.release(MouseButton::Middle) == ReleaseAction::EndPan);
    REQUIRE(g.gesture() == Gesture::None);
}

TEST_CASE("SelectionDrag losing the capture ends a pan as well as a selection",
          "[ui][selection]") {
    // Were Panning left out of abort(), a pan interrupted by another window
    // taking the capture would leave the canvas refusing every later press.
    GestureState g;
    REQUIRE(g.begin_pan(MouseButton::Middle, 0, 0));
    REQUIRE(g.abort() == Gesture::Panning);
    REQUIRE(g.begin_select(SelectMode::Chars, 0, 0));
    REQUIRE(g.abort() == Gesture::Selecting);
    REQUIRE(g.abort() == Gesture::None);
}

TEST_CASE("SelectionDrag canvas position maps to a clamped page point",
          "[ui][selection]") {
    // US Letter at 150%, drawn with its top-left corner at canvas (100, 50).
    const Placement page{ 100.0f, 50.0f, 612.0f * 1.5f, 792.0f * 1.5f };

    const auto inside = canvas_dip_to_page_point(100.0f + 150.0f, 50.0f + 300.0f, page, 1.5f);
    REQUIRE(inside.x == Catch::Approx(100.0f));
    REQUIRE(inside.y == Catch::Approx(200.0f));

    const auto before = canvas_dip_to_page_point(0.0f, 0.0f, page, 1.5f);
    REQUIRE(before.x == 0.0f);
    REQUIRE(before.y == 0.0f);

    const auto after = canvas_dip_to_page_point(5000.0f, 5000.0f, page, 1.5f);
    REQUIRE(after.x == Catch::Approx(612.0f));
    REQUIRE(after.y == Catch::Approx(792.0f));

    const auto no_zoom = canvas_dip_to_page_point(250.0f, 350.0f, page, 0.0f);
    REQUIRE(no_zoom.x == 0.0f);
    REQUIRE(no_zoom.y == 0.0f);
}
```

Create `tests/unit/test_clipboard_text.cpp`:

```cpp
// #52: UTF-8 -> UTF-16 for CF_UNICODETEXT (ui/detail/ClipboardText.hpp).
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "ui/detail/ClipboardText.hpp"

TEST_CASE("ClipboardText utf8 to utf16 keeps CJK and CRLF and adds no terminator",
          "[ui][clipboard]") {
    // "ab", CRLF, U+4E2D U+6587 -- spelled as escapes so this file stays ASCII.
    const std::string utf8 = "ab\r\n\xE4\xB8\xAD\xE6\x96\x87";
    const std::wstring wide = litepdf::ui::utf8_to_utf16(utf8);
    REQUIRE(wide == std::wstring(L"ab\r\n\u4E2D\u6587"));
    REQUIRE(wide.size() == 6);
}

TEST_CASE("ClipboardText utf8 to utf16 of empty text is empty", "[ui][clipboard]") {
    REQUIRE(litepdf::ui::utf8_to_utf16(std::string{}).empty());
}
```

Register both in `tests/CMakeLists.txt`, after `test_document_selection.cpp`:

```cmake
    unit/test_selection_drag.cpp       # #52 Task 8
    unit/test_clipboard_text.cpp       # #52 Task 8
```

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --build build --config Release --target litepdf_unit_tests
```

Expected: compile errors — `SelectionDrag.hpp` / `ClipboardText.hpp` not found, `dip_to_pdf_point` undeclared.

- [ ] **Step 3: Extend `src/ui/detail/ViewportMath.hpp`**

Append before the closing `}  // namespace litepdf::ui`:

```cpp
// Client-area pixels (mouse message coordinates) -> render-target DIPs. The same
// factor as bitmap_px_to_dip; a separate name so each call site says which kind
// of pixel it holds.
inline float client_px_to_dip(float px, float dpi) noexcept {
    return bitmap_px_to_dip(px, dpi);
}

// Render-target DIPs -> PDF points. The inverse of pdf_point_to_dip, and pinned
// to zoom_pct for the same reason.
inline float dip_to_pdf_point(float dip, float zoom_pct) noexcept {
    if (!(zoom_pct > 0.0f)) return 0.0f;   // also rejects NaN
    return dip / zoom_pct;
}
```

- [ ] **Step 4: Create `src/ui/detail/SelectionDrag.hpp`**

```cpp
#pragma once

// #52: pure gesture logic for PdfCanvas text selection (and, in #58, panning).
// No Win32, no Direct2D, no MuPDF -- headless-testable, the same pattern as
// ViewportMath.hpp and SplitterMath.hpp. PdfCanvas feeds it message coordinates
// and system metrics and acts on what it returns.

#include "core/TextSelection.hpp"
#include "ui/detail/ViewportMath.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace litepdf::ui {

// Exactly one gesture owns the mouse capture at a time. One enum, not a flag per
// gesture: two independent booleans are what let a middle-button pan start in
// the middle of a left-button selection drag (spec §4.2).
enum class Gesture { None, Selecting, Panning };

// The button whose release ends the live gesture.
enum class MouseButton { Left, Middle };

// System metrics, read by PdfCanvas at message time (GetSystemMetrics,
// GetDoubleClickTime) and passed in so tests can pin them. The defaults are the
// values measured on the development machine.
struct PointerMetrics {
    int           drag_cx   = 4;     // SM_CXDRAG
    int           drag_cy   = 4;     // SM_CYDRAG
    int           dblclk_cx = 4;     // SM_CXDOUBLECLK
    int           dblclk_cy = 4;     // SM_CYDOUBLECLK
    std::uint32_t dblclk_ms = 500;   // GetDoubleClickTime()
};

// Presses -> selection granularity. With CS_DBLCLKS, Win32 reports the second
// press of a pair as WM_LBUTTONDBLCLK but has no triple-click message: the third
// press arrives as a plain WM_LBUTTONDOWN, recognised here by its time and
// distance from the double click.
class ClickCounter {
public:
    core::SelectMode press(bool is_double_click_message, std::uint32_t time_ms,
                           int x_px, int y_px, const PointerMetrics& m) noexcept {
        int count = 1;
        if (is_double_click_message) {
            count = 2;
        } else if (last_count_ == 2
                   // Unsigned subtraction: correct across GetMessageTime's wrap.
                   && static_cast<std::uint32_t>(time_ms - last_time_ms_) <= m.dblclk_ms
                   && std::abs(x_px - last_x_) <= m.dblclk_cx / 2
                   && std::abs(y_px - last_y_) <= m.dblclk_cy / 2) {
            count = 3;
        }
        last_count_   = count;
        last_time_ms_ = time_ms;
        last_x_       = x_px;
        last_y_       = y_px;
        switch (count) {
            case 2:  return core::SelectMode::Words;
            case 3:  return core::SelectMode::Lines;
            default: return core::SelectMode::Chars;
        }
    }

private:
    int           last_count_   = 0;
    std::uint32_t last_time_ms_ = 0;
    int           last_x_       = 0;
    int           last_y_       = 0;
};

// What a button release asks PdfCanvas to do. By the time the caller acts, the
// gesture is already over -- see GestureState::release.
enum class ReleaseAction {
    None,              // no live gesture belongs to this button
    ClearSelection,    // a single click that never moved (Chars mode): spec §2, "the next click clears"
    CommitSelection,   // materialise quads + text into the view
    EndPan,            // #58
};

class GestureState {
public:
    Gesture          gesture() const noexcept { return gesture_; }
    core::SelectMode mode()    const noexcept { return mode_; }
    bool             moved()   const noexcept { return moved_; }

    // A left press that selects. Refused while any gesture is live.
    bool begin_select(core::SelectMode mode, int x_px, int y_px) noexcept {
        if (gesture_ != Gesture::None) return false;
        gesture_  = Gesture::Selecting;
        owner_    = MouseButton::Left;
        mode_     = mode;
        moved_    = false;
        origin_x_ = x_px;
        origin_y_ = y_px;
        return true;
    }

    // A press that pans (#58). Refused while any gesture is live -- the mirror of
    // the canvas rule that a middle press cancels a live selection drag.
    bool begin_pan(MouseButton button, int x_px, int y_px) noexcept {
        if (gesture_ != Gesture::None) return false;
        gesture_  = Gesture::Panning;
        owner_    = button;
        moved_    = false;
        origin_x_ = x_px;
        origin_y_ = y_px;
        return true;
    }

    // Pointer motion while a gesture is live. The threshold is sticky: once
    // crossed, returning to the press point is still a drag. SM_CXDRAG counts
    // pixels on EITHER side of the press point, so crossing means exceeding it.
    void move(int x_px, int y_px, const PointerMetrics& m) noexcept {
        if (gesture_ == Gesture::None || moved_) return;
        if (std::abs(x_px - origin_x_) > m.drag_cx
            || std::abs(y_px - origin_y_) > m.drag_cy) {
            moved_ = true;
        }
    }

    // A button release: decides what it means AND ends the gesture, before
    // returning.
    //
    // ORDER IS LOAD-BEARING. ReleaseCapture delivers WM_CAPTURECHANGED
    // synchronously, inside the call, and PdfCanvas answers that with abort().
    // So the caller must call release() FIRST and ReleaseCapture() SECOND;
    // abort() then finds nothing live. The other order ends the drag before
    // release() can read it. Splitter.cpp clears its flag before ReleaseCapture
    // for the same reason.
    ReleaseAction release(MouseButton button) noexcept {
        if (gesture_ == Gesture::None || button != owner_) return ReleaseAction::None;
        const Gesture ended = gesture_;
        gesture_ = Gesture::None;
        if (ended == Gesture::Panning) return ReleaseAction::EndPan;
        if (mode_ == core::SelectMode::Chars && !moved_) return ReleaseAction::ClearSelection;
        return ReleaseAction::CommitSelection;
    }

    // Capture lost, a second button pressed, the page changed, or the view torn
    // down. Ends WHATEVER is live without committing -- Panning included, or a
    // pan interrupted by another window would leave the canvas refusing every
    // later press. Returns what was live (None if nothing was).
    Gesture abort() noexcept {
        const Gesture was = gesture_;
        gesture_ = Gesture::None;
        return was;
    }

private:
    Gesture          gesture_  = Gesture::None;
    MouseButton      owner_    = MouseButton::Left;
    core::SelectMode mode_     = core::SelectMode::Chars;
    bool             moved_    = false;
    int              origin_x_ = 0;
    int              origin_y_ = 0;
};

// A canvas position in DIPs -> a point on the page in PDF points, clamped to the
// page. `page` is where on_paint drew the bitmap: its top-left corner is the
// page's (0, 0) (see SelPoint) and it spans page.w x page.h DIPs.
inline core::SelPoint canvas_dip_to_page_point(float x_dip, float y_dip,
                                               const Placement& page,
                                               float zoom_pct) noexcept {
    const float w_pt = dip_to_pdf_point(page.w, zoom_pct);
    const float h_pt = dip_to_pdf_point(page.h, zoom_pct);
    core::SelPoint p;
    p.x = std::clamp(dip_to_pdf_point(x_dip - page.x, zoom_pct), 0.0f, w_pt);
    p.y = std::clamp(dip_to_pdf_point(y_dip - page.y, zoom_pct), 0.0f, h_pt);
    return p;
}

}  // namespace litepdf::ui
```

- [ ] **Step 5: Create `src/ui/detail/ClipboardText.hpp`**

```cpp
#pragma once

// #52: UTF-8 -> UTF-16 for CF_UNICODETEXT. Header-only so it is unit-testable;
// the clipboard calls themselves live in ui/Clipboard.cpp.

#include <windows.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace litepdf::ui {

// An explicit byte length, so the result holds no terminator -- the same
// convention as OutlinePane and ResultsPanel. Invalid sequences become U+FFFD
// (no MB_ERR_INVALID_CHARS). Selection text is far below INT_MAX bytes.
inline std::wstring utf8_to_utf16(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int len  = static_cast<int>(utf8.size());
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), len, nullptr, 0);
    if (wlen <= 0) return {};
    std::wstring out(static_cast<std::size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), len, out.data(), wlen);
    return out;
}

}  // namespace litepdf::ui
```

- [ ] **Step 6: Run and commit**

```bash
cmake --build build --config Release --target litepdf_unit_tests
ctest --test-dir build -C Release -N -R "SelectionDrag|ClipboardText|ViewportMath"
ctest --test-dir build -C Release
```

Expected: `-N` lists **9 SelectionDrag + 2 ClipboardText + 8 ViewportMath = 19** (6 existing ViewportMath cases + 2 new); full suite passes.

```bash
git add src/ui/detail/ViewportMath.hpp src/ui/detail/SelectionDrag.hpp src/ui/detail/ClipboardText.hpp tests/unit/test_viewport_math.cpp tests/unit/test_selection_drag.cpp tests/unit/test_clipboard_text.cpp tests/CMakeLists.txt
git commit -m "feat(ui): pure selection gesture logic and mappings

GestureState holds the one live gesture (Selecting or Panning) and
decides a release before the caller releases the capture. ClickCounter
recognises the triple click Win32 has no message for.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 9: Paint, Select All, Copy, and the Edit menu

**Files:**
- Create: `src/ui/Clipboard.hpp`, `src/ui/Clipboard.cpp`
- Modify: `src/ui/PdfCanvas.hpp`, `src/ui/PdfCanvas.cpp`
- Modify: `src/ui/MainWindow.cpp`
- Modify: `resources/MainMenu.rc.h`, `resources/litepdf.rc.in`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `Document::text_page`, `TextPage::full_range/highlight/copy` (Task 6); `DocumentView::selection/set_selection` (Task 7); `utf8_to_utf16` (Task 8).
- Produces:
  - `bool ui::set_clipboard_text(HWND owner, std::string_view utf8)`.
  - `void PdfCanvas::select_all()`, `void PdfCanvas::copy_selection_to_clipboard() const`.
  - Private: `bool PdfCanvas::own_bitmap() const noexcept`, `bool PdfCanvas::single_page_placement(Placement& out) const`, `const core::TextSelection* PdfCanvas::painted_selection() const noexcept`.
  - `IDM_EDIT_COPY = 40071`, `IDM_EDIT_SELECT_ALL = 40072`.

This task has no unit test: every line is Win32 or Direct2D glue in the `litepdf` executable, which the test target does not link. Its logic is in Tasks 6-8. Verification is the build, the unchanged suite, and the GUI checks in Step 9 — each of which names what would make it fail.

- [ ] **Step 1: The clipboard**

Create `src/ui/Clipboard.hpp`:

```cpp
#pragma once

// #52: write text to the Windows clipboard.

#include <windows.h>

#include <string_view>

namespace litepdf::ui {

// Replace the clipboard contents with `utf8` as CF_UNICODETEXT. Returns true iff
// the clipboard now holds it.
//
// Empty text touches nothing -- opening and emptying the clipboard to write
// nothing would destroy whatever the user had there. A clipboard held by another
// process (Explorer, Office -- usually transient) is a silent failure: no retry,
// no message box. Ctrl+C doing nothing once beats a modal interrupting a copy.
bool set_clipboard_text(HWND owner, std::string_view utf8);

}  // namespace litepdf::ui
```

Create `src/ui/Clipboard.cpp`:

```cpp
#include "ui/Clipboard.hpp"

#include "ui/detail/ClipboardText.hpp"

#include <cstddef>
#include <cstring>
#include <string>

namespace litepdf::ui {

namespace {

// Every path past a successful OpenClipboard must reach CloseClipboard. A
// clipboard left open stays held for the rest of the process's life and breaks
// copy and paste in every other application. A guard, not a sequence of early
// returns that each have to remember.
class ClipboardSession {
public:
    explicit ClipboardSession(HWND owner) noexcept
        : open_(OpenClipboard(owner) != FALSE) {}
    ~ClipboardSession() {
        if (open_) CloseClipboard();
    }
    ClipboardSession(const ClipboardSession&)            = delete;
    ClipboardSession& operator=(const ClipboardSession&) = delete;

    bool is_open() const noexcept { return open_; }

private:
    bool open_;
};

}  // namespace

bool set_clipboard_text(HWND owner, std::string_view utf8) {
    if (utf8.empty()) return false;
    const std::wstring wide = utf8_to_utf16(utf8);
    if (wide.empty()) return false;

    // Build the handle BEFORE opening the clipboard, so it is held open for as
    // short a time as possible.
    const std::size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!h) return false;
    auto* dst = static_cast<wchar_t*>(GlobalLock(h));
    if (!dst) {
        GlobalFree(h);
        return false;
    }
    std::memcpy(dst, wide.c_str(), bytes);   // c_str() includes the terminator
    GlobalUnlock(h);

    ClipboardSession clip(owner);
    if (!clip.is_open()) {
        GlobalFree(h);
        return false;
    }
    if (!EmptyClipboard()) {
        GlobalFree(h);
        return false;
    }
    if (!SetClipboardData(CF_UNICODETEXT, h)) {
        GlobalFree(h);   // the call failed, so we still own the handle
        return false;
    }
    return true;         // the system owns `h` now -- never free it
}

}  // namespace litepdf::ui
```

In `CMakeLists.txt`, in `add_executable(litepdf WIN32 ...)`, after `src/app/CrashHandler.cpp`:

```cmake
    src/ui/Clipboard.cpp         # #52
```

- [ ] **Step 2: Menu IDs and the Edit popup**

In `resources/MainMenu.rc.h`, immediately after the line `// Next free ID: 40064. Reserve 40064-40070 for future Phase 8.x cleanups.`, add:

```c

// #52: text selection. A fresh block, leaving the 40064-40070 reservation alone.
#define IDM_EDIT_COPY        40071   // Ctrl+C
#define IDM_EDIT_SELECT_ALL  40072   // Ctrl+A
```

In `resources/litepdf.rc.in`, between the closing `}` of `POPUP "&File"` and `POPUP "&View"`, add:

```
    POPUP "&Edit"
    {
        MENUITEM "&Copy\tCtrl+C",       IDM_EDIT_COPY
        MENUITEM "Select &All\tCtrl+A", IDM_EDIT_SELECT_ALL
    }
```

- [ ] **Step 3: `PdfCanvas.hpp`**

Add `#include "core/TextSelection.hpp"` and `#include "ui/detail/ViewportMath.hpp"` after `#include "ui/detail/ScrollMath.hpp"`.

After the `scroll_into_view` declaration (end of the public section), add:

```cpp
    // --- #52: text selection ---

    // Select every character on the current page (Edit > Select All). No-op in
    // two-page spread mode (spec §1) or on a page with no text -- which leaves
    // any existing selection alone.
    void select_all();

    // Put the active view's selection on the clipboard (Edit > Copy). Touches
    // the clipboard not at all when there is no selection.
    void copy_selection_to_clipboard() const;
```

After the `content_extent` declaration in the private section, add:

```cpp
    // True when current_bitmap is THIS view's rendering of THIS page. set_view
    // and navigate_to_page's single-page branch both keep painting the outgoing
    // bitmap until the incoming render lands, so anything that measures the
    // bitmap or draws page-space geometry over it has to ask first.
    bool own_bitmap() const noexcept;

    // Where on_paint draws the single-page bitmap, in canvas DIPs. False when
    // there is no bitmap or render target yet.
    bool single_page_placement(Placement& out) const;

    // The selection on_paint draws, or null.
    const litepdf::core::TextSelection* painted_selection() const noexcept;
```

- [ ] **Step 4: `PdfCanvas.cpp` — brush, guard helper, overlay**

1. Add `#include "ui/Clipboard.hpp"` after `#include "ui/ColdStartTimer.hpp"`.

2. In the first anonymous namespace (the one with `kCanvasClassName`), after `using litepdf::ui::WheelResult;`, add:

```cpp

// Axis-aligned bounds of a page-space quad, in canvas DIPs. Serves both
// Document::PageHit and core::Quad, which share their corner field names.
template <class QuadT>
D2D1_RECT_F quad_bounds_dip(const QuadT& q, float ox, float oy, float zoom_pct) {
    const float min_x = std::min({ q.ul_x, q.ur_x, q.ll_x, q.lr_x });
    const float max_x = std::max({ q.ul_x, q.ur_x, q.ll_x, q.lr_x });
    const float min_y = std::min({ q.ul_y, q.ur_y, q.ll_y, q.lr_y });
    const float max_y = std::max({ q.ul_y, q.ur_y, q.ll_y, q.lr_y });
    return D2D1::RectF(ox + pdf_point_to_dip(min_x, zoom_pct),
                       oy + pdf_point_to_dip(min_y, zoom_pct),
                       ox + pdf_point_to_dip(max_x, zoom_pct),
                       oy + pdf_point_to_dip(max_y, zoom_pct));
}
```

3. In `struct PdfCanvas::Impl`, after `ComPtr<ID2D1SolidColorBrush>  brush_hit_current_stroke;`, add:

```cpp
    // #52: selection fill. Device-bound like the hit brushes above.
    ComPtr<ID2D1SolidColorBrush>  brush_selection_fill;
```

4. At the end of `create_render_target()`, after the `brush_hit_current_stroke` creation, add:

```cpp
    // #52: the Windows selection blue, far enough in hue from the yellow and
    // orange hit fills that the two channels never read as one. Polarity does
    // not follow Invert Colors, matching the hit brushes.
    impl_->rt->CreateSolidColorBrush(
        D2D1::ColorF(0.0f, 0.47f, 0.84f, 0.35f),
        &impl_->brush_selection_fill);
```

5. In `discard_render_target()`, after `impl_->brush_hit_current_stroke.Reset();`, add `impl_->brush_selection_fill.Reset();`.

6. Add these definitions immediately before `void PdfCanvas::register_class_once`:

```cpp
bool PdfCanvas::own_bitmap() const noexcept {
    return impl_ && impl_->view && impl_->current_bitmap
        && impl_->bitmap_epoch == impl_->view_epoch
        && impl_->bitmap_page  == impl_->view->current_page();
}

bool PdfCanvas::single_page_placement(Placement& out) const {
    if (!impl_ || !impl_->rt || !impl_->current_bitmap || !hwnd_) return false;
    const D2D1_SIZE_F src_px = impl_->current_bitmap->GetSize();  // PIXELS
    const D2D1_SIZE_F vp     = impl_->rt->GetSize();              // DIPs
    const float rt_dpi = static_cast<float>(GetDpiForWindow(hwnd_));
    out = place_bitmap(bitmap_px_to_dip(src_px.width,  rt_dpi),
                       bitmap_px_to_dip(src_px.height, rt_dpi),
                       vp.width, vp.height, impl_->pan_x, impl_->pan_y);
    return true;
}

const litepdf::core::TextSelection* PdfCanvas::painted_selection() const noexcept {
    if (!impl_ || !impl_->view) return nullptr;
    const auto& committed = impl_->view->selection();
    return committed ? &*committed : nullptr;
}

void PdfCanvas::select_all() {
    if (!impl_ || !impl_->view || impl_->dual_page) return;
    const int page = impl_->view->current_page();
    const auto text = impl_->view->document().text_page(static_cast<std::size_t>(page));
    litepdf::core::SelPoint first, last;
    if (!text.full_range(first, last)) return;

    litepdf::core::TextSelection sel;
    sel.page      = page;
    sel.anchor    = first;
    sel.extent    = last;
    sel.mode      = litepdf::core::SelectMode::Chars;
    sel.quads     = text.highlight(first, last);
    sel.text_utf8 = text.copy(first, last);
    impl_->view->set_selection(std::move(sel));
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void PdfCanvas::copy_selection_to_clipboard() const {
    if (!impl_ || !impl_->view || !hwnd_) return;
    const auto& sel = impl_->view->selection();
    if (!sel || sel->text_utf8.empty()) return;
    set_clipboard_text(hwnd_, sel->text_utf8);
}
```

7. In `scroll_into_view`, replace

```cpp
    const bool own_bitmap = impl_->current_bitmap
                            && impl_->bitmap_epoch == impl_->view_epoch
                            && impl_->bitmap_page  == impl_->view->current_page();

    if (page_moved || !own_bitmap || !impl_->rt) {
```

with

```cpp
    if (page_moved || !own_bitmap() || !impl_->rt) {
```

(The comment block above it stays; it still describes exactly this test.)

8. In `on_paint`, replace the whole single-page block — from `if (impl_->current_bitmap) {` down to and including the `}` that closes it immediately before `HRESULT hr = impl_->rt->EndDraw();` — with:

```cpp
    Placement pl;
    if (single_page_placement(pl)) {
        // Natural size: no shrink-to-fit. The shipped code scaled the
        // destination to fit the viewport unconditionally, which is why a
        // changed render scale never changed the displayed size.
        const D2D1_RECT_F dst = D2D1::RectF(pl.x, pl.y, pl.x + pl.w, pl.y + pl.h);
        impl_->rt->DrawBitmap(impl_->current_bitmap.Get(), dst, 1.0f,
                              D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);

        // --- Overlays: search hits (Phase 6 Task 9), then the selection (#52) ---
        // PDF-point -> canvas-DIP mapping:
        //   canvas_DIP = pdf_point_to_dip(pdf_pt, zoom_pct) + (dst.left, dst.top)
        // MuPDF page coords are top-left origin, Y-down -- matching D2D -- so no
        // Y-flip. Quads are drawn as axis-aligned bounding boxes (v1); rotated
        // text would need transformed geometry.
        //
        // Only over THIS view's bitmap of THIS page. After a tab switch or a
        // page turn the canvas keeps painting the outgoing bitmap until the new
        // render lands, and the incoming page's quads drawn over it would sit on
        // the wrong glyphs (spec §4.4).
        if (own_bitmap()) {
            // One PDF point is exactly zoom_pct DIPs: the pixmap is
            // page_pt * render_scale pixels wide, and dividing that by
            // rt_dpi/96 to reach DIPs cancels the dpi factor back out. Using
            // render_scale() here instead would double every rectangle at
            // 200% scaling.
            const float pct = impl_->view->zoom_pct();
            const float ox  = dst.left;
            const float oy  = dst.top;
            const int   pg  = impl_->view->current_page();

            if (impl_->hits_fn
                && impl_->brush_hit_other_fill && impl_->brush_hit_current_fill) {
                const auto hits = impl_->hits_fn(static_cast<std::size_t>(pg));
                for (const auto& hit : hits) {
                    const auto& q = hit.geom;
                    const D2D1_RECT_F r = quad_bounds_dip(q, ox, oy, pct);

                    const bool is_current =
                        impl_->current_hit.has_value()
                        && impl_->current_hit->page == hit.page
                        && impl_->current_hit->geom.ul_x == q.ul_x
                        && impl_->current_hit->geom.ul_y == q.ul_y
                        && impl_->current_hit->geom.lr_x == q.lr_x
                        && impl_->current_hit->geom.lr_y == q.lr_y;

                    if (is_current) {
                        impl_->rt->FillRectangle(r, impl_->brush_hit_current_fill.Get());
                        if (impl_->brush_hit_current_stroke) {
                            impl_->rt->DrawRectangle(
                                r, impl_->brush_hit_current_stroke.Get(), 1.0f);
                        }
                    } else {
                        impl_->rt->FillRectangle(r, impl_->brush_hit_other_fill.Get());
                    }
                }
            }

            // The selection, after the hits so it reads as the active thing.
            // Painted only on its own page and only in single-page mode (this
            // branch is unreachable in spread mode -- spec §1).
            const litepdf::core::TextSelection* sel = painted_selection();
            if (sel && sel->page == pg && impl_->brush_selection_fill) {
                for (const auto& q : sel->quads) {
                    impl_->rt->FillRectangle(quad_bounds_dip(q, ox, oy, pct),
                                             impl_->brush_selection_fill.Get());
                }
            }
        }
    }
```

- [ ] **Step 5: `MainWindow.cpp` — helpers and accelerators**

1. Add `#include <cwchar>` after `#include <cwctype>`, and `#include <iterator>` (for `std::size`) after `#include <filesystem>`.

2. In the file's top anonymous namespace, immediately before its closing `}  // namespace` (the one that precedes `namespace litepdf::ui {`), add:

```cpp

// #52. Accelerators pre-empt every child window (see IDM_FIND_CLOSE), so a
// Ctrl+C or Ctrl+A meant for an edit control reaches MainWindow as WM_COMMAND
// and must be handed back. Keyed on the window CLASS, so an edit control added
// later needs no change here. A standard EDIT reports exactly L"Edit";
// _wcsicmp makes the L"EDIT" creation spelling irrelevant; a subclassed edit
// (the status bar's page box) keeps its class name.
HWND focused_edit_control() {
    HWND focus = GetFocus();
    if (!focus) return nullptr;
    wchar_t cls[16] = {};
    if (GetClassNameW(focus, cls, static_cast<int>(std::size(cls))) == 0) return nullptr;
    return _wcsicmp(cls, L"Edit") == 0 ? focus : nullptr;
}

// A popup identified by a command it OWNS, never by its position in the bar.
// GetMenuState returns 0xFFFFFFFF for a command the popup does not contain.
// Caveat: MF_BYCOMMAND also searches nested submenus, so if a popup ever gains
// one, choose probe IDs from its own top level.
bool popup_owns(HMENU popup, UINT id) {
    return GetMenuState(popup, id, MF_BYCOMMAND) != static_cast<UINT>(-1);
}
```

3. In `MainWindow::run`'s `ACCEL accels[]`, after the `IDM_FILE_PRINT` entry, add:

```cpp
        // #52. Bare accelerators like every other entry, dispatched by focus in
        // the WM_COMMAND arms so an edit control still gets its own copy.
        { FCONTROL | FVIRTKEY, 'C',          IDM_EDIT_COPY       },
        { FCONTROL | FVIRTKEY, 'A',          IDM_EDIT_SELECT_ALL },
```

- [ ] **Step 6: `MainWindow.cpp` — `WM_COMMAND`**

In `handle_message`'s `WM_COMMAND` switch, immediately before the comment `// Phase 6 Task 10: find bar keyboard entrypoints.`, add:

```cpp
                // #52: Ctrl+C / Ctrl+A and the Edit menu. An edit control that
                // holds the keyboard gets the key back as the message it would
                // have handled. Anywhere else -- the canvas, the outline, the
                // thumbnail and results lists -- acts on the document's
                // selection, so Ctrl+C still works after clicking an outline
                // entry, which does not return focus to the canvas.
                case IDM_EDIT_COPY:
                    if (HWND edit = focused_edit_control()) {
                        SendMessageW(edit, WM_COPY, 0, 0);
                        return 0;
                    }
                    if (canvas_) canvas_->copy_selection_to_clipboard();
                    return 0;
                case IDM_EDIT_SELECT_ALL:
                    if (HWND edit = focused_edit_control()) {
                        SendMessageW(edit, EM_SETSEL, 0, -1);
                        return 0;
                    }
                    if (canvas_) canvas_->select_all();
                    return 0;
```

- [ ] **Step 7: `MainWindow.cpp` — `WM_INITMENUPOPUP` by ownership**

In `case WM_INITMENUPOPUP:`, replace

```cpp
            auto popup = reinterpret_cast<HMENU>(w);
            HMENU main = GetMenu(hwnd);
            if (!main) return 0;
            // (Phase 8 T3/T4) View popup (index 1): reflect Invert Colors
            // and Two-Page Spread state on each show. The flags are
            // per-tab (D9), so the checkmark is read off active_view().
            // When no tab is open, both default to unchecked.
            if (popup == GetSubMenu(main, 1)) {
```

with

```cpp
            auto popup = reinterpret_cast<HMENU>(w);
            // Popups are identified by a command they own, never by position:
            // inserting the Edit menu (#52) moved View from index 1 to 2, and a
            // positional test would have run the View arm against Edit, leaving
            // the View checkmarks silently stale.

            // (Phase 8 T3/T4) View popup: reflect Invert Colors and Two-Page
            // Spread state on each show. The flags are per-tab (D9), so the
            // checkmark is read off active_view(). When no tab is open, both
            // default to unchecked.
            if (popup_owns(popup, IDM_VIEW_INVERT)) {
```

Then replace

```cpp
            // Only rebuild MRU when the File popup (index 0) is about to show.
            if (popup != GetSubMenu(main, 0)) return 0;
```

with

```cpp
            // #52 Edit popup. TranslateAcceleratorW sends WM_INITMENUPOPUP
            // before acting on Ctrl+C / Ctrl+A, and an accelerator whose item is
            // GRAYED is disabled: the keystroke is consumed and no WM_COMMAND is
            // sent. So this enable state must match the WM_COMMAND dispatch
            // exactly -- an edit control holding the keyboard always gets both
            // items, or "no document selection" would kill Ctrl+C in the find
            // box.
            if (popup_owns(popup, IDM_EDIT_COPY)) {
                auto* v = active_view();
                const bool edit_has_focus = focused_edit_control() != nullptr;
                const bool has_doc        = v && v->document().is_open();
                const bool has_selection  = has_doc && v->selection().has_value();
                const bool can_select_all = has_doc && !v->dual_page();
                EnableMenuItem(popup, IDM_EDIT_COPY,
                               MF_BYCOMMAND
                               | ((edit_has_focus || has_selection) ? MF_ENABLED : MF_GRAYED));
                EnableMenuItem(popup, IDM_EDIT_SELECT_ALL,
                               MF_BYCOMMAND
                               | ((edit_has_focus || can_select_all) ? MF_ENABLED : MF_GRAYED));
                return 0;
            }
            // Only rebuild MRU when the File popup is about to show.
            if (!popup_owns(popup, IDM_FILE_OPEN)) return 0;
```

After this step, `grep -n "GetSubMenu" src/ui/MainWindow.cpp` must print nothing.

- [ ] **Step 8: Build and test**

```bash
cmake --build build --config Release
ctest --test-dir build -C Release
```

Expected: build clean; same count as after Task 8, all passing. Record `litepdf.exe` size (must be < 19,000,000).

- [ ] **Step 9: GUI checks**

**First, write the GUI driver.** Save the script below as `build/gui/selection-drive.ps1` (create `build/gui/`). `build/` is git-ignored, so the driver is never committed and survives into Task 10, which uses it again. It is Windows PowerShell 5.1 syntax only (no `?.`, `??`, ternary); dot-source it from the repo root.

```powershell
# selection-drive.ps1 -- scratch driver for the #52 GUI checks. Not committed.
# Dot-source from the repo root:  . .\build\gui\selection-drive.ps1
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName Microsoft.VisualBasic
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class U {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll", CharSet=CharSet.Unicode)]
  public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, IntPtr title);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern IntPtr GetMenu(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetSubMenu(IntPtr m, int pos);
  [DllImport("user32.dll")] public static extern uint GetMenuState(IntPtr m, uint id, uint flags);
  [DllImport("user32.dll")] public static extern bool GetCursorInfo(ref CURSORINFO ci);
  [DllImport("user32.dll")] public static extern IntPtr LoadCursorW(IntPtr inst, IntPtr id);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [StructLayout(LayoutKind.Sequential)] public struct CURSORINFO { public int cbSize; public int flags; public IntPtr hCursor; public POINT pt; }
}
"@
[void][U]::SetProcessDPIAware()

$WM_COMMAND = 0x0111; $WM_KEYDOWN = 0x0100; $WM_INITMENUPOPUP = 0x0117; $WM_CLOSE = 0x0010
$WM_MOUSEMOVE = 0x0200; $WM_LBUTTONDOWN = 0x0201; $WM_LBUTTONUP = 0x0202; $WM_LBUTTONDBLCLK = 0x0203
$WM_MBUTTONDOWN = 0x0207; $WM_MBUTTONUP = 0x0208
$MK_LBUTTON = 1; $MK_MBUTTON = 0x10; $VK_NEXT = 0x22

function Start-LitePdf([string[]]$files) {
  $env:LITEPDF_NO_RESTORE = '1'
  $p = Start-Process -PassThru (Resolve-Path .\build\Release\litepdf.exe) -ArgumentList $files
  for ($i = 0; $i -lt 50 -and [int64]$p.MainWindowHandle -eq 0; $i++) { Start-Sleep -Milliseconds 100; $p.Refresh() }
  $script:Proc   = $p
  $script:Main   = [IntPtr]$p.MainWindowHandle
  $script:Canvas = [U]::FindWindowExW($script:Main, [IntPtr]::Zero, 'LitePDFPdfCanvas', [IntPtr]::Zero)
  if ([int64]$script:Canvas -eq 0) { throw 'canvas HWND not found' }
  Start-Sleep -Milliseconds 800
}

function Send-Command([int]$id) { [void][U]::PostMessageW($script:Main, $WM_COMMAND, [IntPtr]$id, [IntPtr]::Zero); Start-Sleep -Milliseconds 300 }
function Page-Down([int]$n) { for ($i = 0; $i -lt $n; $i++) { [void][U]::PostMessageW($script:Canvas, $WM_KEYDOWN, [IntPtr]$VK_NEXT, [IntPtr]::Zero); Start-Sleep -Milliseconds 400 } }

# FitWidth at the top of an overflowing page: client px = page pt * canvas width / page width.
function To-Client([double]$xPt, [double]$yPt, [double]$pageWidthPt = 612) {
  $r = New-Object U+RECT; [void][U]::GetClientRect($script:Canvas, [ref]$r)
  $s = ($r.Right - $r.Left) / $pageWidthPt
  return @([int][math]::Round($xPt * $s), [int][math]::Round($yPt * $s))
}

function Post-Mouse([int]$msg, [int]$x, [int]$y, [int]$keys) {
  $l = [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF))
  [void][U]::PostMessageW($script:Canvas, $msg, [IntPtr]$keys, $l)
  Start-Sleep -Milliseconds 40
}

function Drag([int[]]$from, [int[]]$to, [int]$steps = 10) {
  Post-Mouse $WM_LBUTTONDOWN $from[0] $from[1] $MK_LBUTTON
  for ($i = 1; $i -le $steps; $i++) {
    $x = [int]($from[0] + ($to[0] - $from[0]) * $i / $steps)
    $y = [int]($from[1] + ($to[1] - $from[1]) * $i / $steps)
    Post-Mouse $WM_MOUSEMOVE $x $y $MK_LBUTTON
  }
  Post-Mouse $WM_LBUTTONUP $to[0] $to[1] 0
}

function Double-Click([int[]]$at) {
  Post-Mouse $WM_LBUTTONDOWN $at[0] $at[1] $MK_LBUTTON; Post-Mouse $WM_LBUTTONUP $at[0] $at[1] 0
  Post-Mouse $WM_LBUTTONDBLCLK $at[0] $at[1] $MK_LBUTTON; Post-Mouse $WM_LBUTTONUP $at[0] $at[1] 0
}

function Set-Sentinel { Set-Clipboard -Value 'SENTINEL-52' }
function Get-Clip { Start-Sleep -Milliseconds 200; return (Get-Clipboard -Raw) }

function Keys([string]$keys) {
  [Microsoft.VisualBasic.Interaction]::AppActivate($script:Proc.Id)
  Start-Sleep -Milliseconds 300
  [System.Windows.Forms.SendKeys]::SendWait($keys)
  Start-Sleep -Milliseconds 300
}

function Close-LitePdf { [void][U]::PostMessageW($script:Main, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero) }
```

Example — `. .\build\gui\selection-drive.ps1; Set-Sentinel; Start-LitePdf @('tests\fixtures\selection.pdf'); Page-Down 3; Send-Command 40072; Send-Command 40071; Get-Clip`.

Set `$env:LITEPDF_NO_RESTORE = '1'` and back up `%LOCALAPPDATA%\LitePDF\session.json` **before the first launch**. Confirm the exe you run is `build\Release\litepdf.exe` from this branch.

Each check sets the clipboard to a sentinel first, so "nothing happened" is visible.

1. **Select All + Copy, final character included.** Open `tests/fixtures/selection.pdf`, PgDn ×3 (page 4, `alpha beta gamma`), post `IDM_EDIT_SELECT_ALL` then `IDM_EDIT_COPY`. Expected clipboard, exactly: `alpha beta gamma` CRLF `delta epsilon`. **Fails if:** the sentinel remains, or the text ends `epsilo`.
2. **The highlight is drawn, and only on its page.** DPI-aware capture of the canvas (see `reference_litepdf_scripted_gui_smoke`; assert the capture size equals the canvas client size): blue boxes over both lines. PgDn: none on page 5. PgUp: back. Post `IDM_EDIT_COPY` again with the sentinel set — the same text (the selection persisted). **Fails if:** blue boxes on page 5, or the second copy yields the sentinel.
3. **Copy with no selection does not touch the clipboard.** Fresh launch, same page, post `IDM_EDIT_COPY` only. Expected: sentinel intact.
4. **Spread mode.** Post `IDM_VIEW_DUAL_PAGE` (40062), then `IDM_EDIT_SELECT_ALL`, then `IDM_EDIT_COPY`. Expected: no *new* selection — the clipboard holds whatever check 1 put there if you continued that session, or the sentinel on a fresh launch.
5. **Real Ctrl+C / Ctrl+A in the find box, with NO document selection** (plan correction C5 — this needs real keystrokes, because a posted `WM_COMMAND` skips `TranslateAcceleratorW`). Fresh launch, bring litepdf to the foreground, `SendKeys` `^f`, type `alpha`, `SendKeys` `^a`, then `^c`. Expected clipboard: `alpha`. **Fails if** the sentinel remains — that is the grayed-accelerator swallow. Negative control, same session: click the canvas, `SendKeys` `^c`: sentinel (re-set it first) intact.
6. **View checkmarks still track state.** Send `WM_INITMENUPOPUP` for `GetSubMenu(GetMenu(main), 2)` (View is now index 2) and read `GetMenuState(view, IDM_VIEW_INVERT, MF_BYCOMMAND) & MF_CHECKED` → 0. Post `IDM_VIEW_INVERT` (40061), resend, read again → non-zero. **Fails if** it stays 0 — the positional-`GetSubMenu` defect.
7. **Edit enable state.** With a document and no selection, send `WM_INITMENUPOPUP` for index 1 with focus on the canvas: Copy `MF_GRAYED`, Select All enabled. After check 1's Select All: both enabled.

Restore `session.json`, close the app with `WM_CLOSE`.

- [ ] **Step 10: Commit**

```bash
git add src/ui/Clipboard.hpp src/ui/Clipboard.cpp src/ui/PdfCanvas.hpp src/ui/PdfCanvas.cpp src/ui/MainWindow.cpp resources/MainMenu.rc.h resources/litepdf.rc.in CMakeLists.txt
git commit -m "feat(ui): Select All, Copy and the Edit menu

The canvas paints the view's selection after the search hits, and only
over its own bitmap -- the bitmap-identity guard also stops search hits
being drawn over the previous page while the next one renders.

Ctrl+C / Ctrl+A are accelerators dispatched by focus: an edit control gets
WM_COPY / EM_SETSEL back, everything else acts on the document. The Edit
menu's enable state follows the same test, because TranslateAcceleratorW
swallows the accelerator of a grayed item. WM_INITMENUPOPUP now finds
popups by a command they own; the new Edit menu shifted View's position.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 10: Mouse gestures, cursor and teardown

**Files:**
- Modify: `src/ui/PdfCanvas.hpp`, `src/ui/PdfCanvas.cpp`
- Modify: `src/ui/MainWindow.hpp`

**Interfaces:**
- Consumes: `GestureState`, `ClickCounter`, `PointerMetrics`, `MouseButton`, `ReleaseAction`, `canvas_dip_to_page_point`, `client_px_to_dip` (Task 8); `TextPage::snap/highlight/copy` (Task 6); `own_bitmap`, `single_page_placement`, `painted_selection` (Task 9).
- Produces: no public interface. Private: `on_left_button_down`, `on_mouse_move`, `on_left_button_up`, `cancel_gesture`, `refresh_live_selection`, `commit_live_selection`, `page_point_at`, `update_cursor`.

No unit test, for the reason given in Task 9; the state machine is Task 8's. GUI checks in Step 8.

- [ ] **Step 1: `PdfCanvas.hpp` declarations**

After the `painted_selection` declaration, add:

```cpp
    // #52 selection gestures. The state machine is ui/detail/SelectionDrag.hpp;
    // spec §4.2 has the message ordering these depend on.
    void on_left_button_down(bool is_double_click_message, int x_px, int y_px);
    void on_mouse_move(int x_px, int y_px);
    void on_left_button_up();

    // End any live gesture WITHOUT committing, drop its text handle and release
    // the capture. Never dereferences impl_->view: set_view calls it before
    // repointing, on the tab-close path where the outgoing view is already
    // destroyed (spec §3.3). Safe to re-enter from WM_CAPTURECHANGED.
    void cancel_gesture();

    // Recompute the live selection's highlight from its RAW anchor and extent.
    void refresh_live_selection();

    // Materialise the live selection -- quads and text -- into the active view.
    void commit_live_selection();

    // Client pixels -> a clamped point on the page drawn at `page`.
    litepdf::core::SelPoint page_point_at(int x_px, int y_px, const Placement& page) const;

    // WM_SETCURSOR for the client area.
    void update_cursor();
```

- [ ] **Step 2: `PdfCanvas.cpp` — includes, usings, metrics, Impl state**

1. Add `#include "ui/detail/SelectionDrag.hpp"` after `#include "ui/detail/ViewportMath.hpp"`, and `#include <windowsx.h>` after `#include <d2d1_1.h>`.

2. In the first anonymous namespace, after the `using litepdf::ui::WheelResult;` line, add:

```cpp
using litepdf::ui::canvas_dip_to_page_point;
using litepdf::ui::ClickCounter;
using litepdf::ui::client_px_to_dip;
using litepdf::ui::Gesture;
using litepdf::ui::GestureState;
using litepdf::ui::MouseButton;
using litepdf::ui::PointerMetrics;
using litepdf::ui::ReleaseAction;

PointerMetrics pointer_metrics() {
    PointerMetrics m;
    m.drag_cx   = GetSystemMetrics(SM_CXDRAG);
    m.drag_cy   = GetSystemMetrics(SM_CYDRAG);
    m.dblclk_cx = GetSystemMetrics(SM_CXDOUBLECLK);
    m.dblclk_cy = GetSystemMetrics(SM_CYDOUBLECLK);
    m.dblclk_ms = GetDoubleClickTime();
    return m;
}
```

3. In `struct PdfCanvas::Impl`, after `brush_selection_fill`, add:

```cpp
    // #52: the one live mouse gesture. Its gesture() is the single source of
    // truth for "is a drag in progress" -- there is no separate flag (spec §4.2).
    GestureState                  gesture;
    ClickCounter                  clicks;
    // The page's text, held only while a selection gesture is live.
    // Escrow-backed, so it stays safe to drop even if closing the tab destroys
    // its Document mid-drag (spec §3.3).
    litepdf::core::Document::TextPage drag_text;
    // The selection being dragged out. Painted INSTEAD of the view's committed
    // selection while a gesture is live, and simply discarded on cancel --
    // which is what leaves a double click's committed word intact when a
    // following drag is cancelled.
    std::optional<litepdf::core::TextSelection> live_selection;
```

4. In `register_class_once`, change `wc.style = CS_HREDRAW | CS_VREDRAW;` to:

```cpp
        // CS_DBLCLKS: without it WM_LBUTTONDBLCLK is never delivered and a double
        // click cannot select a word (#52).
        wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
```

- [ ] **Step 3: Prefer the live selection when painting; refuse Select All mid-gesture**

Replace the body of `painted_selection` with:

```cpp
    if (!impl_) return nullptr;
    if (impl_->live_selection) return &*impl_->live_selection;
    if (!impl_->view) return nullptr;
    const auto& committed = impl_->view->selection();
    return committed ? &*committed : nullptr;
```

In `select_all`, change the first line to:

```cpp
    if (!impl_ || !impl_->view || impl_->dual_page) return;
    if (impl_->gesture.gesture() != Gesture::None) return;   // a drag owns the selection
```

- [ ] **Step 4: Teardown on view swap, layout change and page change**

In `set_dual_page`, immediately after its first line `if (!impl_ || impl_->dual_page == on) return;`, add:

```cpp
    // #52: a live selection drag cannot survive a switch to the spread layout.
    // It would keep extending with single-page geometry while the canvas paints
    // the spread, and commit a selection nothing draws but Ctrl+C still copies
    // -- the state on_left_button_down refuses to create (spec §1). The page
    // snap that follows in MainWindow does not reliably cancel it: it calls
    // change_current_page only when the page is not already a spread's left.
    cancel_gesture();
```

At the very top of `set_view`, before the existing comment block, add:

```cpp
    // #52: end any gesture FIRST, before `view` replaces impl_->view. On the tab
    // close path the outgoing view has already been destroyed by TabList::remove,
    // which is why cancel_gesture never touches it.
    cancel_gesture();
```

In `change_current_page`, replace

```cpp
    const bool changed = impl_->view->set_current_page(idx);
    if (changed && impl_->on_page_changed) {
        impl_->on_page_changed(impl_->view->current_page());
    }
    return changed;
```

with

```cpp
    const bool changed = impl_->view->set_current_page(idx);
    if (changed) {
        // A selection is bound to one page (spec §1). A drag that outlived its
        // page would extend the old page's text with the new page's geometry.
        cancel_gesture();
        if (impl_->on_page_changed) {
            impl_->on_page_changed(impl_->view->current_page());
        }
    }
    return changed;
```

- [ ] **Step 5: The gesture handlers**

Add these definitions immediately after `copy_selection_to_clipboard`:

```cpp
litepdf::core::SelPoint PdfCanvas::page_point_at(int x_px, int y_px,
                                                 const Placement& page) const {
    const float dpi = static_cast<float>(GetDpiForWindow(hwnd_));
    return canvas_dip_to_page_point(client_px_to_dip(static_cast<float>(x_px), dpi),
                                    client_px_to_dip(static_cast<float>(y_px), dpi),
                                    page, impl_->view ? impl_->view->zoom_pct() : 1.0f);
}

void PdfCanvas::cancel_gesture() {
    if (!impl_) return;
    // End the gesture BEFORE releasing: ReleaseCapture re-enters this function
    // through WM_CAPTURECHANGED, which must find nothing left to cancel.
    if (impl_->gesture.abort() == Gesture::None) return;
    impl_->live_selection.reset();
    impl_->drag_text = litepdf::core::Document::TextPage{};
    if (hwnd_ && GetCapture() == hwnd_) ReleaseCapture();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void PdfCanvas::refresh_live_selection() {
    if (!impl_->live_selection || !impl_->drag_text.valid()) return;
    auto& sel = *impl_->live_selection;
    // snap() returns COPIES; sel.anchor and sel.extent stay raw (spec §2).
    const auto snapped = impl_->drag_text.snap(sel.anchor, sel.extent, sel.mode);
    sel.quads = impl_->drag_text.highlight(snapped.a, snapped.b);
}

void PdfCanvas::commit_live_selection() {
    if (!impl_->view || !impl_->live_selection || !impl_->drag_text.valid()) return;
    litepdf::core::TextSelection sel = *impl_->live_selection;
    const auto snapped = impl_->drag_text.snap(sel.anchor, sel.extent, sel.mode);
    sel.quads     = impl_->drag_text.highlight(snapped.a, snapped.b);
    sel.text_utf8 = impl_->drag_text.copy(snapped.a, snapped.b);
    if (sel.text_utf8.empty()) {
        // Nothing under the gesture. Keeping an empty selection would enable
        // Copy for nothing.
        impl_->view->clear_selection();
        return;
    }
    impl_->view->set_selection(std::move(sel));
}

void PdfCanvas::on_left_button_down(bool is_double_click_message, int x_px, int y_px) {
    // Count every press, refused or not, so a triple click is measured against
    // the double click that really preceded it.
    const litepdf::core::SelectMode mode = impl_->clicks.press(
        is_double_click_message, static_cast<std::uint32_t>(GetMessageTime()),
        x_px, y_px, pointer_metrics());

    if (impl_->gesture.gesture() != Gesture::None) return;
    // Refuse spread mode HERE, not only in paint: current_bitmap holds the LEFT
    // slot in that mode, so a drag would compute points with single-page
    // geometry and build an invisible selection Ctrl+C would still copy (spec §1).
    if (!impl_->view || impl_->dual_page || !own_bitmap()) return;
    Placement pl;
    if (!single_page_placement(pl)) return;

    const int page = impl_->view->current_page();
    litepdf::core::Document::TextPage text =
        impl_->view->document().text_page(static_cast<std::size_t>(page));
    if (!text.valid()) return;

    const litepdf::core::SelPoint pt = page_point_at(x_px, y_px, pl);
    impl_->view->clear_selection();
    impl_->gesture.begin_select(mode, x_px, y_px);
    impl_->drag_text = std::move(text);

    litepdf::core::TextSelection live;
    live.page   = page;
    live.anchor = pt;
    live.extent = pt;
    live.mode   = mode;
    impl_->live_selection = std::move(live);
    SetCapture(hwnd_);

    if (mode != litepdf::core::SelectMode::Chars) {
        // A double or triple click selects at PRESS time. A stationary click
        // never crosses the drag threshold, so waiting for the release would
        // leave nothing to tell "select this word" from "click to clear"
        // (spec §4.2). A following drag extends it in the same mode.
        refresh_live_selection();
        commit_live_selection();
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void PdfCanvas::on_mouse_move(int x_px, int y_px) {
    if (impl_->gesture.gesture() != Gesture::Selecting || !impl_->live_selection) return;
    impl_->gesture.move(x_px, y_px, pointer_metrics());
    Placement pl;
    if (!own_bitmap() || !single_page_placement(pl)) return;
    // RAW: the pointer position, never a snapped one (spec §2).
    impl_->live_selection->extent = page_point_at(x_px, y_px, pl);
    refresh_live_selection();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void PdfCanvas::on_left_button_up() {
    // Decide and commit FIRST, release SECOND. ReleaseCapture delivers
    // WM_CAPTURECHANGED synchronously; had it run first, that arm would cancel
    // the drag before this one read it, and the selection highlighted under the
    // held button would vanish on release (spec §4.2).
    switch (impl_->gesture.release(MouseButton::Left)) {
        case ReleaseAction::None:
            return;
        case ReleaseAction::ClearSelection:
            if (impl_->view) impl_->view->clear_selection();
            break;
        case ReleaseAction::CommitSelection:
            commit_live_selection();
            break;
        case ReleaseAction::EndPan:
            break;   // #58
    }
    impl_->live_selection.reset();
    impl_->drag_text = litepdf::core::Document::TextPage{};
    if (GetCapture() == hwnd_) ReleaseCapture();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void PdfCanvas::update_cursor() {
    LPCWSTR shape = IDC_ARROW;
    Placement pl;
    if (!impl_->dual_page && own_bitmap() && single_page_placement(pl)) {
        POINT pt;
        if (GetCursorPos(&pt) && ScreenToClient(hwnd_, &pt)) {
            const float dpi = static_cast<float>(GetDpiForWindow(hwnd_));
            const float x = client_px_to_dip(static_cast<float>(pt.x), dpi);
            const float y = client_px_to_dip(static_cast<float>(pt.y), dpi);
            if (x >= pl.x && x < pl.x + pl.w && y >= pl.y && y < pl.y + pl.h) {
                shape = IDC_IBEAM;
            }
        }
    }
    SetCursor(LoadCursorW(nullptr, shape));
}
```

- [ ] **Step 6: Wire the messages**

In `handle_message`, replace

```cpp
        case WM_LBUTTONDOWN:
            // Click-to-focus: ensures keystrokes (PgUp/PgDn/Home/End) reach us.
            SetFocus(hwnd_);
            return 0;
```

with

```cpp
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            // Click-to-focus: ensures keystrokes (PgUp/PgDn/Home/End) reach us.
            SetFocus(hwnd_);
            on_left_button_down(msg == WM_LBUTTONDBLCLK, GET_X_LPARAM(l), GET_Y_LPARAM(l));
            return 0;
        case WM_MOUSEMOVE:
            on_mouse_move(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            return 0;
        case WM_LBUTTONUP:
            on_left_button_up();
            return 0;
        case WM_MBUTTONDOWN:
        case WM_RBUTTONDOWN:
            // One gesture owns the capture at a time. A second button during a
            // selection drag cancels it -- keeping anything already committed --
            // and starts nothing of its own (spec §4.2).
            if (impl_->gesture.gesture() == Gesture::Selecting) cancel_gesture();
            break;   // DefWindowProc: right-button WM_CONTEXTMENU generation unchanged
        case WM_CAPTURECHANGED:
            // Another window took the capture. After an ordinary button-up the
            // gesture is already over and this finds nothing to do.
            cancel_gesture();
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(l) == HTCLIENT) {
                update_cursor();
                return TRUE;   // or DefWindowProc resets it to the class cursor
            }
            break;
```

- [ ] **Step 7: The declaration-order comment**

In `src/ui/MainWindow.hpp`, immediately above `std::unique_ptr<TabManager>   tabs_;`, add:

```cpp
    // DECLARATION ORDER IS LOAD-BEARING (#52): tabs_ before canvas_, so the
    // canvas is destroyed FIRST at exit. The canvas holds a raw DocumentView*
    // that must never be read after tabs_ has destroyed the views. (Its
    // mid-gesture Document::TextPage is escrow-backed and would survive either
    // order; the raw view pointer would not.)
```

- [ ] **Step 8: Build, test, GUI checks**

```bash
cmake --build build --config Release
ctest --test-dir build -C Release
```

Expected: build clean, all passing, same count as Task 9.

**The driver.** Task 9 wrote `build/gui/selection-drive.ps1`. If it is missing (a fresh clone, a cleaned build tree), recreate it with exactly this content:

```powershell
# selection-drive.ps1 -- scratch driver for the #52 GUI checks. Not committed.
# Dot-source from the repo root:  . .\build\gui\selection-drive.ps1
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName Microsoft.VisualBasic
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class U {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll", CharSet=CharSet.Unicode)]
  public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, IntPtr title);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern IntPtr GetMenu(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetSubMenu(IntPtr m, int pos);
  [DllImport("user32.dll")] public static extern uint GetMenuState(IntPtr m, uint id, uint flags);
  [DllImport("user32.dll")] public static extern bool GetCursorInfo(ref CURSORINFO ci);
  [DllImport("user32.dll")] public static extern IntPtr LoadCursorW(IntPtr inst, IntPtr id);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [StructLayout(LayoutKind.Sequential)] public struct CURSORINFO { public int cbSize; public int flags; public IntPtr hCursor; public POINT pt; }
}
"@
[void][U]::SetProcessDPIAware()

$WM_COMMAND = 0x0111; $WM_KEYDOWN = 0x0100; $WM_INITMENUPOPUP = 0x0117; $WM_CLOSE = 0x0010
$WM_MOUSEMOVE = 0x0200; $WM_LBUTTONDOWN = 0x0201; $WM_LBUTTONUP = 0x0202; $WM_LBUTTONDBLCLK = 0x0203
$WM_MBUTTONDOWN = 0x0207; $WM_MBUTTONUP = 0x0208
$MK_LBUTTON = 1; $MK_MBUTTON = 0x10; $VK_NEXT = 0x22

function Start-LitePdf([string[]]$files) {
  $env:LITEPDF_NO_RESTORE = '1'
  $p = Start-Process -PassThru (Resolve-Path .\build\Release\litepdf.exe) -ArgumentList $files
  for ($i = 0; $i -lt 50 -and [int64]$p.MainWindowHandle -eq 0; $i++) { Start-Sleep -Milliseconds 100; $p.Refresh() }
  $script:Proc   = $p
  $script:Main   = [IntPtr]$p.MainWindowHandle
  $script:Canvas = [U]::FindWindowExW($script:Main, [IntPtr]::Zero, 'LitePDFPdfCanvas', [IntPtr]::Zero)
  if ([int64]$script:Canvas -eq 0) { throw 'canvas HWND not found' }
  Start-Sleep -Milliseconds 800
}

function Send-Command([int]$id) { [void][U]::PostMessageW($script:Main, $WM_COMMAND, [IntPtr]$id, [IntPtr]::Zero); Start-Sleep -Milliseconds 300 }
function Page-Down([int]$n) { for ($i = 0; $i -lt $n; $i++) { [void][U]::PostMessageW($script:Canvas, $WM_KEYDOWN, [IntPtr]$VK_NEXT, [IntPtr]::Zero); Start-Sleep -Milliseconds 400 } }

# FitWidth at the top of an overflowing page: client px = page pt * canvas width / page width.
function To-Client([double]$xPt, [double]$yPt, [double]$pageWidthPt = 612) {
  $r = New-Object U+RECT; [void][U]::GetClientRect($script:Canvas, [ref]$r)
  $s = ($r.Right - $r.Left) / $pageWidthPt
  return @([int][math]::Round($xPt * $s), [int][math]::Round($yPt * $s))
}

function Post-Mouse([int]$msg, [int]$x, [int]$y, [int]$keys) {
  $l = [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF))
  [void][U]::PostMessageW($script:Canvas, $msg, [IntPtr]$keys, $l)
  Start-Sleep -Milliseconds 40
}

function Drag([int[]]$from, [int[]]$to, [int]$steps = 10) {
  Post-Mouse $WM_LBUTTONDOWN $from[0] $from[1] $MK_LBUTTON
  for ($i = 1; $i -le $steps; $i++) {
    $x = [int]($from[0] + ($to[0] - $from[0]) * $i / $steps)
    $y = [int]($from[1] + ($to[1] - $from[1]) * $i / $steps)
    Post-Mouse $WM_MOUSEMOVE $x $y $MK_LBUTTON
  }
  Post-Mouse $WM_LBUTTONUP $to[0] $to[1] 0
}

function Double-Click([int[]]$at) {
  Post-Mouse $WM_LBUTTONDOWN $at[0] $at[1] $MK_LBUTTON; Post-Mouse $WM_LBUTTONUP $at[0] $at[1] 0
  Post-Mouse $WM_LBUTTONDBLCLK $at[0] $at[1] $MK_LBUTTON; Post-Mouse $WM_LBUTTONUP $at[0] $at[1] 0
}

function Set-Sentinel { Set-Clipboard -Value 'SENTINEL-52' }
function Get-Clip { Start-Sleep -Milliseconds 200; return (Get-Clipboard -Raw) }

function Keys([string]$keys) {
  [Microsoft.VisualBasic.Interaction]::AppActivate($script:Proc.Id)
  Start-Sleep -Milliseconds 300
  [System.Windows.Forms.SendKeys]::SendWait($keys)
  Start-Sleep -Milliseconds 300
}

function Close-LitePdf { [void][U]::PostMessageW($script:Main, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero) }
```

Example (check 1 below) — `. .\build\gui\selection-drive.ps1; Set-Sentinel; Start-LitePdf @('tests\fixtures\selection.pdf'); Page-Down 3; Double-Click (To-Client 116 68); Send-Command 40071; Get-Clip` → `beta`.

GUI checks, with that driver, `LITEPDF_NO_RESTORE=1`, `session.json` backed up first, sentinel set before each. Open `tests/fixtures/selection.pdf`, PgDn ×3 to `alpha beta gamma`. Page-space targets (points, top-left origin) — convert with the driver's `To-Client`:

| name | x | y |
|---|---|---|
| left of `alpha` | 60 | 68 |
| `beta` centre | 116 | 68 |
| right of `gamma` | 200 | 68 |
| below all text | 300 | 400 |

1. **Stationary double click selects a word.** At `beta`: DOWN, UP, DBLCLK, UP; post `IDM_EDIT_COPY`. Expected: `beta`. **Fails if** the sentinel remains (the click-clears rule ate the word).
2. **A single click clears.** After check 1, DOWN+UP at `below all text`, copy. Expected: sentinel intact.
3. **Triple click selects the line.** DOWN, UP, DBLCLK, UP, DOWN, UP at `beta` (all within 500 ms), copy. Expected: `alpha beta gamma`.
4. **Forward drag.** DOWN at `left of alpha`, MOVE in 10 steps to `right of gamma`, UP, copy. Expected: `alpha beta gamma`.
5. **Backward drag keeps its anchor.** DOWN at `right of gamma`, MOVE in 10 steps to `left of alpha`, UP, copy. Expected: `alpha beta gamma`. **Fails if** the text is shorter — the walking-anchor defect needs several moves to show, which is why 10.
6. **Word drag from below the text** (plan correction C4). DOWN, UP, DBLCLK at `below all text`, MOVE to `beta`, UP, copy. Expected: `beta gamma` CRLF `delta epsilon`.
7. **A second button cancels, and the canvas recovers.** DOWN at `left of alpha`, MOVE to `right of gamma`, `WM_MBUTTONDOWN`, `WM_MBUTTONUP`, `WM_LBUTTONUP`, copy → sentinel intact. Then repeat check 4 → `alpha beta gamma`. **Fails if** the second drag copies nothing (a gesture left latched).
8. **Tab close mid-drag.** Open `search.pdf` in the same instance (a second tab), DOWN + MOVE on it, post `IDM_TAB_CLOSE` (40030) while the button is "held". Expected: no crash; on the remaining tab, check 4 still passes.
9. **Spread mode refuses.** Post `IDM_VIEW_DUAL_PAGE`, then drag as in check 4, copy. Expected: sentinel intact.
   **9b. Entering spread mode mid-drag cancels the drag.** Back in single-page mode (post `IDM_VIEW_DUAL_PAGE` again), sentinel set: DOWN at `left of alpha`, MOVE 5 steps toward `right of gamma`, post `IDM_VIEW_DUAL_PAGE`, MOVE to `right of gamma`, UP, copy. Expected: sentinel intact. **Fails if** the clipboard holds `alpha beta gamma` — a selection committed in a layout that cannot display it.
10. **Cursor.** Bring litepdf to the foreground (`AppActivate`), `ClientToScreen` the `beta` point on the canvas, `SetCursorPos` there, wait 100 ms, `GetCursorInfo`: `hCursor == LoadCursorW(NULL, IDC_IBEAM)` (32513). Negative control: FitWidth leaves no grey surround beside the page, so post `IDM_ZOOM_OUT` (40011) twice, wait for the render, and move the cursor to canvas client `(width - 5, 20)`, which is now outside the page: `IDC_ARROW` (32512). **Fails if** the I-beam never appears, or appears over the surround.

Restore `session.json`, close with `WM_CLOSE`.

- [ ] **Step 9: Commit**

```bash
git add src/ui/PdfCanvas.hpp src/ui/PdfCanvas.cpp src/ui/MainWindow.hpp
git commit -m "feat(canvas): select text by drag, double click and triple click

The canvas keeps a live selection while a gesture is in progress and
commits it to the view on release -- deciding before ReleaseCapture, whose
synchronous WM_CAPTURECHANGED would otherwise cancel the drag first. A
double or triple click commits at press time. A second button, a page
change, a view swap or a lost capture cancels without committing.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 11: CHANGELOG, final verification, merge gate

**Files:**
- Modify: `CHANGELOG.md`

- [ ] **Step 1: CHANGELOG**

Under `## [Unreleased]`, add an `### Added` section **above** the `### Fixed` section PR-0 created (Keep a Changelog order), and one bullet to `### Fixed`:

```markdown
### Added

- Text selection and copy. Drag across a page to select text, double-click to
  select a word, triple-click to select a line; Ctrl+C copies the selection and
  Ctrl+A selects the whole page. A new Edit menu carries both. A selection stays
  when you turn the page and is cleared by the next click. Selection works in
  single-page mode and does not cross pages.
```

```markdown
- Search highlights are no longer drawn over the previous page, or the previous
  tab's page, in the moment before the new one finishes rendering.
```

- [ ] **Step 2: Verify the claims against the artifact**

Run `build/Release/litepdf.exe tests/fixtures/selection.pdf` and read each bullet next to the running window — drag, double-click, triple-click, Ctrl+C, Ctrl+A, the Edit menu, a page turn. PR #43 shipped a CHANGELOG bullet that was false; this step is why.

- [ ] **Step 3: Full verification**

```bash
python scripts/generate-selection-fixture.py --check
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Expected: `--check` exits 0; all tests pass. Record the exact passing count (Task 5 baseline + 17 + 2 + 2 + 2 + 9 = baseline + 32) and the `litepdf.exe` byte size.

- [ ] **Step 4: Commit**

```bash
git add CHANGELOG.md
git commit -m "docs(changelog): record text selection and copy

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

- [ ] **Step 5: Merge gate, then PR**

Invoke the `risk-tiered-review` skill (never drop the Codex lens). Name these least-certain claims for the adversarial lens:

1. **C5's premise** — that `TranslateAcceleratorW` consumes the keystroke of a grayed item without sending `WM_COMMAND`. Step 9 check 5 of Task 9 is the live evidence; is there a focus state (a list view, the tab strip, a modeless print progress dialog) where the Edit arm's enable state and the `WM_COMMAND` dispatch disagree?
2. **C4's restore** — can `at_text_end` report true for a raw point that is *not* at the end of the text, beyond the one accepted case (a trailing run of glyphs narrower than 1 pt, which MuPDF's `same_point` skips), so that `snap` extends a selection visibly?
3. **Re-entrancy** — `cancel_gesture` runs from `set_view`, `change_current_page`, `WM_CAPTURECHANGED` and the second-button arm. Is there a path where it runs inside `on_left_button_down` or `on_left_button_up` between `begin_select` / `release` and the capture call, leaving the capture held with no gesture, or a gesture with no capture?

Push, open the PR titled `feat: text selection and copy`, body with the counts, exe size, the discrimination results from Task 6 Step 9, the GUI check results, a note that the overlay's bitmap-identity guard is a search-path fix carried by this PR, and `Closes #52`.
