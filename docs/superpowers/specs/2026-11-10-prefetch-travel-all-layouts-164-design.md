# Prefetch travel direction follows the reader in every layout (#164) — design

Status: design proposed in chat 2026-11-10; the user asked for this written spec before any code.
Issue: #164 (split out of #49, PR #160). Upstream: `project_litepdf_l1_byte_budget_49` decisions,
`src/core/detail/PrefetchPlan.hpp`.

## 1. Problem

The single-page prefetch picks one neighbour, the one in the reader's travel direction, when the
current page and both neighbours do not fit the 1 GiB L1 budget together (#49). The direction is
updated in one place only, `DocumentView::request_render_with_prefetch`
(`src/core/DocumentView.cpp:491-492`):

```cpp
impl_->travel = travel_direction(page, impl_->last_prefetch_page, impl_->travel);
impl_->last_prefetch_page = page;
```

Only single-page view calls it. Two other layouts move the reader without it:

- **Two-page spread** renders through `request_render` (`src/ui/MainWindow.cpp:398`,
  `src/ui/PdfCanvas.cpp:1612`, `:2151`). This is the case the issue names.
- **Continuous scroll (#55)**, added after the issue was filed, renders through
  `request_page_render`. The strip keeps its own direction (`StripViewport::travel_`) for its own
  prefetch range and never hands it back.

On the way back to single-page view, the first prefetch compares against the last page that was
shown *in single-page view*. Issue example: single page 10; spread forward to 30, then back to 20;
single again. The direction comes out Forward (20 > 10), although the last moves were backward.
The same happens after scrolling backward in the strip.

Cost: one on-demand render on the next page turn, once, only past the 3-page budget (for example
800 % on a 200 % display).

## 2. Goal and non-goals

Goal: on the first single-page prefetch after leaving the spread or the strip, the direction is
the one the reader last moved in, in whatever layout. A layout switch is not a move.

Non-goals:
- Prefetch in the spread or the strip. The spread prefetches nothing today; the strip has its own
  schedule (`StripSchedule`). Neither changes.
- Changing `prefetch_plan` or `travel_direction` (`PrefetchPlan.hpp`). Both stay as they are.
- Wiring the strip's `StripViewport::travel_` into the view. The current-page change carries the
  same information (§3.1), with no second source of truth.

## 3. Design

### 3.1 One writer: `DocumentView::set_current_page`

Every layout moves the reader through `DocumentView::set_current_page`
(`src/core/DocumentView.cpp:271`):

- single page: `PdfCanvas::change_current_page` (`src/ui/PdfCanvas.cpp:369`) from navigation,
  then `request_render_with_prefetch` for the same page;
- spread: the same, plus the snap to the pair's left page on every spread render
  (`src/ui/MainWindow.cpp:393`, `src/ui/PdfCanvas.cpp:343`, `:2147`);
- strip: `PdfCanvas::set_strip_current_page` (`src/ui/PdfCanvasStrip.cpp:541`) from
  `strip_after_change`, on every scroll that changes the current page.

So the direction is recorded there, and only there:

```cpp
bool DocumentView::set_current_page(int idx) {
    const int pc = page_count();
    if (pc <= 0) return false;
    const int clamped = std::clamp(idx, 0, pc - 1);
    note_travel(clamped);                       // NEW: before the no-change return
    if (clamped == impl_->current_page) return false;
    ...                                         // unchanged
}
```

`note_travel(page)` (private, in `Impl` or a file-local helper):

```cpp
const int key = litepdf::ui::canonical_page(page, pc,
                                            impl_->dual_page && !impl_->continuous);
impl_->travel = travel_direction(key, impl_->last_travel_key, impl_->travel);
impl_->last_travel_key = key;
```

- It runs **before** the early return for an unchanged page, so the first call after a layout
  switch (§3.3) always sets the key, even when the page does not move.
- `request_render_with_prefetch` stops writing: lines 491-492 go, and it only reads
  `impl_->travel`. Its callers set the current page before they call it: `navigate_click`
  (`MainWindow.cpp:461` then `:465`), every other kick with `current_page()`, PdfCanvas `:1626` and
  `:2169`, and the #49 tests' `hold_worker` / `queued_by`. One exception: the session restore kick
  passes the saved page as written (`MainWindow.cpp:1226`), which can be past the end while the
  view holds the clamped page. Once the prefetch no longer writes the direction, that page cannot
  disturb it. One writer keeps the invariant simple: *the direction is the direction of the last
  change of the reader's page.*
- `last_prefetch_page` is renamed `last_travel_key`: it is no longer the page the prefetch last
  ran for. The `Impl` comment (`DocumentView.cpp:85-87`) is rewritten to match.

### 3.2 A spread moves by pair, not by page

In the spread the key is the pair's left page (`canonical_page`, the cover rule of
`src/ui/PdfCanvasLayout.hpp`). Without it, a go-to or a search hit that names a right page records
two moves: to the right page, then the snap back to its left page. The snap would read as a
backward move. Example on 8 pages: on pair (3,4), go to page 6. Per page this is 3→6 Forward, then
6→5 Backward. Per pair it is 3→5 Forward, then 5→5, which is no move.

`DocumentView.cpp` may include `ui/PdfCanvasLayout.hpp`: the header uses only the standard
library, and core already includes STL-only ui headers (`DocumentView.cpp:19`
`ui/ThumbnailPane.hpp`, `TabList.hpp:21` `ui/detail/StripLayout.hpp`). This avoids a second copy
of the cover rule.

The key uses `dual_page && !continuous`. The strip never runs with the spread flag on today
(`continuous_toggle` turns spread off on the way in), but the strip moves page by page whatever
that flag says.

### 3.3 A layout switch is not a move

`set_dual_page` and `set_continuous` set `last_travel_key = -1` **when the flag changes**, and keep
`travel`. `travel_direction` already treats `-1` as "no previous page" and keeps the direction, so
the first page recorded in the new layout becomes the reference, and the direction is the one the
old layout left.

Without the reset, a switch records a move that the reader did not make:
- spread → single, where the single page is the pair's right page (a carried held page): L → L+1
  reads as Forward;
- single → spread: p → left(p) reads as Backward whenever p was a right page.

The guard on an actual change is required. `MainWindow::apply_effective_layout` calls
`set_continuous` on **every** call, a strip-to-strip tab switch included (`MainWindow.cpp:955-957`).
An unconditional reset would make every tab switch forget the reference.

The order of calls is already right on every switch path: the view's flag changes first, then the
page is snapped or carried.
- `IDM_VIEW_DUAL_PAGE`: `v->set_dual_page` (`MainWindow.cpp:1860`), then `apply_effective_layout`,
  then the snap `change_current_page(p)` (`:1869`).
- `IDM_VIEW_CONTINUOUS`: `v->set_dual_page` (`:1888`), then `apply_effective_layout`, which calls
  `set_continuous` (`:957`) before `switch_layout`, whose carry sets the page
  (`PdfCanvasStrip.cpp:424-428`).

If a future path snaps the page *before* changing the flag, the effect stays small. The switch's
own move is recorded, and the reset then drops the reference, so the next move does not count
either. That is one stale direction, the size of this bug. Not worth a guard.

### 3.4 The issue's example, after

Single page 10: key 10. Spread on: the reset gives key −1, and the snap to left(10) = 9 sets key 9
with no move. Forward to pair (29,30): Forward. Back to (19,20): Backward. Spread off: the reset
gives key −1, direction Backward. The single-page kick for page 19 sets key 19 with no move. The
prefetch runs Backward and prepares page 18. The reader was going backward, so this is right.

## 4. Accepted behaviour (not changed, written down)

- **A jump is a move**, in every layout: go-to page, a thumbnail click, a search hit, a bookmark.
  Single-page view behaves this way today; the spread and the strip now do too.
- **The strip's current page can change without a scroll**, after a zoom or a resize that
  re-lays out the strip. That change counts as a move. Its direction is where the reader's current
  page went, which is what the next single-page turn starts from.
- **Each tab keeps its own direction.** Each tab has its own `DocumentView`, so a tab switch
  neither records nor resets anything (§3.3 guard).
- **Session restore** sets the page through `set_current_page` once, from the initial key −1: no
  move, and the default Forward stays. This is the same as today.

## 5. Testing

New `[core][view][prefetch]` cases in `tests/unit/test_document_view.cpp`, on the 8-page
`selection.pdf` view the #49 cases use (pairs (1,2) (3,4) (5,6) (7)).

The direction needs a test observer. `queued_render_count()` counts the neighbours, but it cannot
tell which one was queued. Add, beside the other test observers in `DocumentView.hpp`:

```cpp
// (#164) The direction the next single-page prefetch uses. Test observability only.
[[nodiscard]] TravelDirection travel_direction() const noexcept;
```

Cases 1, 2 and 6 have the issue's shape. The single-page phase calls
`request_render_with_prefetch` (through `queued_by`), so before the fix the old line holds a
single-page reference that the later moves cannot reach:
1. **Issue example, spread.** Single page 1 (prefetch). `set_dual_page(true)`, snap to 1, then
   pairs 3, 5, then back to 3. `set_dual_page(false)`, single page 3 (prefetch): the direction is
   Backward. Before the fix it is Forward (3 > 1, the reference from single page 1). This is the
   falsifier.
2. **Strip, the same shape.** Single page 1 (prefetch). `set_continuous(true)`, pages 2 → 5 → 3.
   `set_continuous(false)`, single page 3 (prefetch): Backward. Before the fix, Forward.
3. **Snap is not a move.** Spread on pair (3,4) with direction Backward set by an earlier move;
   `set_current_page(6)` then `set_current_page(5)`: the direction is Forward. Mutation: the key
   without `canonical_page` ends Backward.
4. **Switch is not a move.** Direction Forward on single page 4 (a right page in spread terms);
   `set_dual_page(true)`, snap to 3: still Forward. Mutation: without the reset it ends Backward.
5. **The same flag again does not reset.** In the strip, pages 4 → 5 (Forward), then
   `set_continuous(true)` again (what a tab switch does), then page 4: the direction is Backward.
   Mutation: an unconditional reset leaves no reference, and the direction stays Forward.
6. **End to end.** The #49 one-byte budget. Every page is then over a third of it, so the plan
   prefetches one neighbour, and L1 always admits its newest entry. With one worker, P0 runs before
   P1, so after the renders finish L1 holds exactly the prefetched neighbour. Case 1's sequence,
   with the final single-page prefetch of page 3 allowed to finish: `l1_contains(2)` and not
   `l1_contains(4)`. Before the fix, the reverse. This pins that the prefetch reads the direction
   `set_current_page` wrote, now that the prefetch no longer writes it.

The existing #49 cases ("prefetches one neighbour past the byte budget") must pass unchanged:
`queued_by` sets the page first, so the single writer records the same moves the old line did.

Each new case is proven able to fail. Revert the production change, or apply the named mutation,
and the case fails. Then restore it (`mutation-test-hygiene`).

Build **Release** only; `ctest --test-dir build -C Release` serially. No GUI smoke: no canvas or
MainWindow code changes, and §3.3's call order is existing code, cited above.

## 6. Out of scope, release

- `VERSION` is not bumped (issue constraint).
- No CHANGELOG line. The reader cannot see the change apart from one render's latency, as with
  #129.
- The P0/P1 race for a page larger than half the budget is #163, not this.
