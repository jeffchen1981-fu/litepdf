# Prefetch travel direction follows the reader in every layout (#164) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The first single-page prefetch after the spread or the strip uses the direction the reader last moved in, in whatever layout, and a layout switch is never counted as a move.

**Architecture:** `DocumentView::set_current_page` becomes the one writer of the travel direction, keyed by spread pair when the spread is on. `request_render_with_prefetch` stops recording moves and only seeds the key when it is unset. `set_dual_page` / `set_continuous` reset the key when their flag actually changes. Core only: no canvas or MainWindow change.

**Tech Stack:** C++17 (MSVC, `/W4`), Catch2, CMake/ctest from VS 2022 BuildTools.

**Spec:** `docs/superpowers/specs/2026-11-10-prefetch-travel-all-layouts-164-design.md`. Read §3 and §5 before any task; § numbers below refer to it.

## Global Constraints

- Branch `claude/issue-164-f1ecfe`, base `main` @ `20aa62b`. Line numbers below are for `71a6d1c`; find code by name if a line moved.
- **Build:** Release only, never Debug. `$cmake` / `$ctest` are the BuildTools binaries under `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\`, never the ones on PATH. Build: `& $cmake --build build --target litepdf_unit_tests --config Release`. If `third_party\mupdf` is empty in this worktree, junction it to the main checkout's first (`cmd /c mklink /J third_party\mupdf C:\Users\User\projects\litepdf\third_party\mupdf`). Remove it with `cmd /c rmdir` before the worktree is cleaned up.
- **Run tests from PowerShell, from the worktree root** (fixtures resolve from there): `build\tests\Release\litepdf_unit_tests.exe "<tag or exact name>"`. Full suite: `& $ctest --test-dir build -C Release`, **serially, no `-j`**. Read the count with `-N`; never hard-code it.
- New `TEST_CASE` names are ASCII and start with `DocumentView travel: `. Tags: `[core][view][prefetch][travel]`.
- `travel_direction` and `prefetch_plan` (`src/core/detail/PrefetchPlan.hpp`) do not change. No change under `src/ui/`. No GUI smoke (§5).
- The existing #49 cases (`tests/unit/test_document_view.cpp:550-575`) and `test_text_prefetch_view` pass **unchanged**.
- Mutation steps follow the `mutation-test-hygiene` skill: apply, rebuild, see the named case fail, restore, rebuild, see it pass. `git diff` is clean of the mutation before the commit.
- No `VERSION` change, no CHANGELOG line (§6).
- Commits: conventional, English, ending with `Co-Authored-By: <the model that wrote the commit> <noreply@anthropic.com>`. `git add` files by name (a junctioned `third_party/mupdf` breaks `git commit -a`). Do not push.

## Review Focus

1. **A kick whose page argument is not the view's page, such as a saved page past the end of a document that shrank.** The restore kick passes the saved page as written (`MainWindow.cpp:1226`), which can be past the end while the view holds the clamped page.
   - On today's restore path the key is already set by then: `change_current_page` runs first. So the seed does not fire there.
   - The rule still matters. A seed taken from the argument would make the reader's next move wrong on any future path that kicks with an unset key.
   - Pinned in Task 2 by the "own page" case, proven by the mutation that seeds from the argument.
2. **Two-page view turned off with the continuous preference on, on a held right page.** The view goes spread → strip in one command (`set_dual_page(false)`, then `set_continuous(true)`, then the carry onto the right page L+1). The carry must not read as a forward move, and the reader's backward direction must survive into single page. Pinned in Task 2 by the "held right page" case, which fails on the pre-fix code and before Task 2's reset.

Checked and left out: a resubmit of the same page (zoom step, resize) records nothing, because the kick no longer records. A zero-page view never reaches `note_travel` from `set_current_page` (the `pc <= 0` return comes first), and the seed on one is arithmetic only. The cover page and the odd tail go through the same `canonical_page` call that case 3 pins.

`set_continuous`'s reset is pinned by no case, and that is accepted. On today's paths it changes the outcome only with the spread and continuous flags on together, which the app never sets (§3.2). It stays because §3.3 requires it, so that any switch drops the reference.

---

## Task 1: The page change records the direction

**Files:**
- Modify: `src/core/DocumentView.hpp` (include; observer beside `queued_render_count()` at :303-305)
- Modify: `src/core/DocumentView.cpp` (include block :10-19; `Impl` :85-92; `set_current_page` :271; `request_render_with_prefetch` :491-492; observer beside `queued_render_count()` :550)
- Test: `tests/unit/test_document_view.cpp` (new block after the #49 cases, before the #159 block at :577)

**Interfaces:**
- Produces, `DocumentView` (public):
  `[[nodiscard]] TravelDirection prefetch_travel() const noexcept;` returns `impl_->travel`. Comment: `// (#164) The direction the next single-page prefetch uses. Test observability only.` Not named `travel_direction` (§5.1).
- Produces, `DocumentView::Impl` (in the .cpp):
  `int last_travel_key = -1;` (renamed from `last_prefetch_page`)
  `void note_travel(int page, int count) noexcept;` (parameter `count`, not `page_count`)
- Produces, in the test file's anonymous namespace, for Task 2:
  `void move_to(DocumentView& view, int page);` = `(void)view.set_current_page(page);` only.
  `void kick(DocumentView& view, int page);` = `view.request_render_with_prefetch(page, <drop pixmap>, {});` only, with no `set_current_page` before it.
  `void spread_issue_example(DocumentView& view);` = case 1's sequence, through the final kick.
  (The spec calls `move_to` `move`; renamed so it is never read as `std::move`.)

- [ ] **Step 1: Add the observer**

`DocumentView.hpp` includes `core/detail/PrefetchPlan.hpp`. Declare `prefetch_travel()` beside `queued_render_count()`, and define it in `DocumentView.cpp` beside `queued_render_count()`. Build: it compiles, nothing else changes.

- [ ] **Step 2: Write the failing tests**

Add after the #49 cases:

```cpp
// #164: the direction the single-page prefetch uses follows the reader's moves
// in every layout. move_to() is what every layout's page change reduces to;
// kick() is MainWindow::kick_render's single-page branch, which calls no
// set_current_page first. 8 pages: spread pairs (0) (1,2) (3,4) (5,6) (7).
namespace {

using litepdf::core::TravelDirection;

void move_to(DocumentView& view, int page) { (void)view.set_current_page(page); }

void kick(DocumentView& view, int page) {
    view.request_render_with_prefetch(page,
        [](fz_pixmap* p, fz_context* ctx) { if (p) fz_drop_pixmap(ctx, p); }, {});
}

// Spec 5.3 case 1: single page 1; spread forward to (7), back to (5,6); spread
// off; the single-page kick for page 5, with no page change before it.
void spread_issue_example(DocumentView& view) {
    move_to(view, 1);
    kick(view, 1);
    view.set_dual_page(true);
    for (int p : {1, 3, 5, 7, 5}) move_to(view, p);
    view.set_dual_page(false);
    kick(view, 5);
}

}  // namespace
```

Four cases. Each opens with `InlineDispatcher disp; auto view = one_worker_view(disp, 1);`, which gives one worker and the #49 one-byte budget. In the tables, `== Backward` is shorthand for `REQUIRE(view->prefetch_travel() == TravelDirection::Backward);`. The enumerators of a scoped enum need the qualification. A row with an empty name continues the case above it.

| Name (after `DocumentView travel: `) | Body | Assertion |
|---|---|---|
| `spread moves set the direction single page resumes with (#164)` | `spread_issue_example(*view);` | `prefetch_travel() == Backward` |
| `strip moves set the direction single page resumes with (#164)` | `move_to 1; kick 1; set_continuous(true); move_to 1, 2, 5, 3; set_continuous(false); move_to 3; kick 3` | `== Backward` |
| `a spread snap to the left page is not a move (#164)` | `move_to 4; set_dual_page(true); move_to 3, 5, 3` | `== Backward` |
| | then `move_to 6; move_to 5` (a go-to on a right page, then its snap) | `== Forward` |
| `the prefetch after spread off renders the neighbour behind (#164)` | `spread_issue_example(*view);` then poll below | `l1_contains(4)`, not `l1_contains(6)` |

The last case polls, and a deadline miss fails it (no prefetch landed). Add `#include <thread>`:

```cpp
const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
while (!view->l1_contains(4) && !view->l1_contains(6) &&
       std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
}
REQUIRE(view->l1_contains(4));
REQUIRE_FALSE(view->l1_contains(6));
```

Why it is exact (§5.3 case 6): every page is over a third of the one-byte budget, so one neighbour is prefetched, and L1 keeps only its newest entry (`PageCache::put_pixmap`). With one worker, the final P0 for page 5 runs before its P1. The only earlier renders are page 1 and page 2, so neither 4 nor 6 can be left over.

- [ ] **Step 3: Run them to verify they fail**

Build, then run `build\tests\Release\litepdf_unit_tests.exe "[travel]"`.
Expected: all four cases FAIL.
- The spread and strip cases read `Forward`, and the L1 case finds page 6 in L1 rather than page 4.
- The snap case fails at its **first** assertion. The pre-fix code records no move outside the prefetch, so the direction is still the default `Forward` where it should be `Backward`.
- That first assertion keeps the case honest: the direction really is `Backward` before the go-to. The final `Forward` is proven by the mutation in Step 6.

- [ ] **Step 4: Make `set_current_page` the one writer**

In `DocumentView.cpp`:
- Include `ui/PdfCanvasLayout.hpp`, before `ui/ThumbnailPane.hpp`.
- In `Impl`, rename `last_prefetch_page` to `last_travel_key`. Rewrite the comment at :85-87 to say what the two fields are now:
  - `last_travel_key` is the key of the reader's last page change: the page, or the pair's left page in the spread. It is −1 before the first change and after a layout switch.
  - `travel` is the direction of the last real move.
  - Only `note_travel` writes `travel`. (Task 2 adds the layout setters' reset of the key and updates this comment.)
- Add `Impl::note_travel(int page, int count) noexcept` beside `estimated_bytes`, with the body in §3.1:
  - The key is `litepdf::ui::canonical_page(page, count, dual_page && !continuous)`.
  - `travel = litepdf::core::travel_direction(key, last_travel_key, travel)`. The call is **qualified** (§3.1).
  - Then `last_travel_key = key`.
- In `set_current_page`, call `impl_->note_travel(clamped, pc);` right after `clamped` is computed and **before** `if (clamped == impl_->current_page) return false;`.
- In `request_render_with_prefetch`, delete the two lines at :491-492. Nothing replaces them in this task.

- [ ] **Step 5: Run the tests to verify they pass**

Build, then run `build\tests\Release\litepdf_unit_tests.exe "[prefetch],[text_prefetch]"`. In Catch2 the comma means OR. The text-prefetch cases carry `[text_prefetch]` only, and they exercise `set_current_page`.
Expected: all four new cases, the two #49 cases and the six text-prefetch cases PASS.

- [ ] **Step 6: Prove the snap case can fail**

Mutation: in `note_travel`, key by `page` instead of `canonical_page(...)`. Build and run `"[travel]"`.
Expected: `a spread snap to the left page is not a move` FAILS at the final `== Forward` (3 → 6 reads Forward, then 6 → 5 reads Backward). Restore it, rebuild, and run `"[travel]"`: all pass.

- [ ] **Step 7: Commit**

```bash
git add src/core/DocumentView.hpp src/core/DocumentView.cpp tests/unit/test_document_view.cpp
git commit -m "fix(core): record the prefetch travel direction where the page changes (#164)"
```

---

## Task 2: A layout switch is not a move

**Files:**
- Modify: `src/core/DocumentView.cpp` (`set_continuous` :290; `set_dual_page` :427; `request_render_with_prefetch`, after `const int total = page_count();`)
- Test: `tests/unit/test_document_view.cpp` (append to Task 1's #164 block)

**Interfaces:**
- Consumes (Task 1): `DocumentView::prefetch_travel()`, `Impl::note_travel(int page, int count) noexcept`, `Impl::last_travel_key`. Test helpers from the anonymous namespace: `move_to(view, p)` (`set_current_page` only), `kick(view, p)` (`request_render_with_prefetch` only), `spread_issue_example(view)` (single page 1; spread on; pages 1, 3, 5, 7, 5; spread off; `kick 5`; the direction ends Backward). Fixture: `one_worker_view(disp, 1)`.
- Produces: no new names.

- [ ] **Step 1: Write the tests**

Seven cases, named `DocumentView travel: ...` and tagged `[core][view][prefetch][travel]`.
- Each opens with `InlineDispatcher disp; auto view = one_worker_view(disp, 1);`.
- `== Forward` is shorthand for `REQUIRE(view->prefetch_travel() == TravelDirection::Forward);`.
- A row with an empty name continues the case above it.

| Name (after `DocumentView travel: `) | Body | Assertion |
|---|---|---|
| `turning the spread on is not a move (#164)` | `move_to 2; move_to 4; set_dual_page(true); move_to 3` | `== Forward` |
| `setting continuous to its current value keeps the reference (#164)` | `set_continuous(true); move_to 4; move_to 5; set_continuous(true); move_to 4` | `== Backward` |
| `the first page turn after spread off counts (#164)` | `spread_issue_example(*view); move_to 6; kick 6` | `== Forward` |
| `the first page turn after a continuous flip on a tab switch counts (#164)` | `set_continuous(true); move_to 5; move_to 3; set_continuous(false); kick 3; move_to 4; kick 4` | `== Forward` |
| `the first kick after a switch keys the view's own page (#164)` | `move_to 6; move_to 7; set_continuous(true); set_continuous(false); kick(*view, 20); move_to 7` | `== Forward` |
| `spread off into the strip onto the held right page is not a move (#164)` | `set_dual_page(true); move_to 3, 5, 3;` | `== Backward` |
| | `set_dual_page(false); set_continuous(true); move_to 4;` (the carry onto the held right page) `set_continuous(false); move_to 4; kick 4` | `== Backward` |
| `a carry onto the current page sets the reference (#164)` | `move_to 5; move_to 3;` | `== Backward` |
| | `set_continuous(true); move_to 3` (single page 3 → strip: the carry lands on the page already current) `; move_to 4` (the first scroll) | `== Forward` |

The last case pins §3.1's rule that `note_travel` runs **before** the unchanged-page return. Without that rule, the carry onto page 3 records nothing, the key stays −1, and the first scroll keeps the old `Backward`.

`kick(*view, 20)` is the session restore kick on a document that shrank (Review Focus 1): the saved page is past the end of 8 pages, and the view holds page 7. Its renders fail inside `RenderEngine`'s `fz_catch` (MuPDF prints an error), as they would in the app.

- [ ] **Step 2: Run them, before the change**

Build, then run `"[travel]"`.
Expected: `turning the spread on` FAILS (key 4, then 4 → 3 reads Backward). `held right page` FAILS at its last assertion (3 → 4 reads Forward). The other five, and Task 1's cases, PASS.
- The `continuous ... current value` case passes here because Task 1 already records moves. §5.3 lists it as failing on the pre-fix code, but this order never runs it there. The proof that matters for it is mutation (a) in Step 7, the only one that tests the guard.

- [ ] **Step 3: Reset the key on an actual flag change**

- `set_dual_page`: after the existing `if (impl_->dual_page == on) return;`, add `impl_->last_travel_key = -1;` beside `impl_->dual_page = on;`.
- `set_continuous`: `if (impl_->continuous == on) return;`, then assign, then `impl_->last_travel_key = -1;`. It stays `noexcept`. Neither setter touches `travel`.
- Comment each reset briefly: a switch is not a move (§3.3), and the guard exists because `apply_effective_layout` calls `set_continuous` on every tab switch.
- Update Task 1's `Impl` comment: `note_travel` is the only writer of `travel`, and the two layout setters also reset `last_travel_key` to −1.

- [ ] **Step 4: Run them, with the reset and no seed**

Build, then run `"[travel]"`.
Expected: `turning the spread on`, `held right page`, `a carry onto the current page` and the other passing cases PASS. `the first page turn after spread off counts` and `... continuous flip on a tab switch counts` FAIL: the key is −1 at the page turn, so the direction stays `Backward`. This is the no-seed proof (§5.3 cases 7 and 8). Record both failures in the commit body.

- [ ] **Step 5: Seed the key in the kick**

In `request_render_with_prefetch`, right after `const int total = page_count();`:
`if (impl_->last_travel_key < 0) impl_->note_travel(impl_->current_page, total);`
It seeds from `impl_->current_page`, **never** from the `page` argument (§3.1, Review Focus 1). Add a comment of two or three lines:
- Spread → single sets no page, so this is where the key comes back.
- From −1 it is never a move.
- The argument can be past the end on a restore.

- [ ] **Step 6: Run them to verify they pass**

Build, then run `"[prefetch],[text_prefetch]"`.
Expected: all eleven #164 cases, the two #49 cases and the six text-prefetch cases PASS.

- [ ] **Step 7: Prove the guard, the seed's source and the call's place can fail**

Apply each mutation alone, rebuild, run `"[travel]"`, then restore and rebuild:
- (a) In `set_continuous`, reset on every call, by removing the `== on` return. Expected: `setting continuous to its current value keeps the reference` FAILS (`Forward`).
- (b) Seed from `page` instead of `impl_->current_page`. Expected: `the first kick after a switch keys the view's own page` FAILS (key 20, so 7 reads `Backward`).
- (c) In `set_current_page`, move `impl_->note_travel(clamped, pc);` below the `if (clamped == impl_->current_page) return false;`. Expected: `a carry onto the current page sets the reference` FAILS at its last assertion (key −1 at page 4, so it stays `Backward`).

After all three restores, `"[travel]"` passes and `git diff` shows only the intended change.

- [ ] **Step 8: Run the full suite**

Run `& $ctest --test-dir build -C Release -N`, then `& $ctest --test-dir build -C Release`, serially.
Expected: every test passes, and the count equals `-N`. "Render timed out" lines on stderr are known noise. The MuPDF error from the page-20 kick is expected.

- [ ] **Step 9: Commit**

```bash
git add src/core/DocumentView.cpp tests/unit/test_document_view.cpp
git commit -m "fix(core): a layout switch is not a prefetch travel move (#164)"
```

The body names the five proofs:
- Step 4's two no-seed failures.
- Step 7's three mutations.
