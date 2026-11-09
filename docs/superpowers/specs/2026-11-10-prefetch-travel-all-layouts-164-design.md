# Prefetch travel direction follows the reader in every layout (#164) — design

Status: design proposed in chat 2026-11-10; the user asked for this written spec before any code.
Spec gate (Full tier): round 1 (Opus + Sonnet) fixed in place, the spread-off seed (§3.1) above all.
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

`note_travel(page)` is a file-local helper at namespace scope in `DocumentView.cpp`, taking the
`Impl&`. It must not be a member of `DocumentView` or of `Impl`: inside either, the unqualified
`travel_direction` would find the test observer of §5 if it bore that name, and ordinary lookup
that finds a class member suppresses argument-dependent lookup. The call is written qualified,
`litepdf::core::travel_direction(...)`, all the same.

```cpp
const int key = litepdf::ui::canonical_page(page, pc,
                                            impl.dual_page && !impl.continuous);
impl.travel = litepdf::core::travel_direction(key, impl.last_travel_key, impl.travel);
impl.last_travel_key = key;
```

- It runs **before** the early return for an unchanged page. A layout switch that carries or
  snaps the page (§3.3) therefore sets the key, even when the page does not move.
- `request_render_with_prefetch` no longer records a move: lines 491-492 go. It does one thing in
  their place. When the key is −1, it **seeds** the key from the view's own current page:
  `if (impl_->last_travel_key < 0) note_travel(*impl_, impl_->current_page);`. From −1,
  `travel_direction` keeps the direction, so a seed is never a move.
  - The seed covers the one switch that sets no page: spread → single. `IDM_VIEW_DUAL_PAGE` snaps
    the page only when the spread turns **on** (`MainWindow.cpp:1865-1870`), and
    `PdfCanvas::set_dual_page` and `apply_viewport`'s single-page branch never call
    `set_current_page`. Without the seed the key stays −1, and the reader's first page turn after
    the switch (19 → 20) reads as no move. That is a regression against today's code, where the
    kick records page 19.
  - The seed also covers a tab whose continuous flag flips on a tab switch (§4).
  - It seeds from `current_page`, not from the `page` argument, so the session restore kick cannot
    seed the key. That kick passes the saved page as written (`MainWindow.cpp:1226`), which can be
    past the end while the view holds the clamped page.
  - Seeding inside `set_dual_page` / `set_continuous` instead would not work. It would key the page
    before the switch's carry moves it, and bring back the L → L+1 move that §3.3 removes.

  One writer of moves keeps the invariant simple: *the direction is the direction of the last
  change of the reader's page.*
- `last_prefetch_page` is renamed `last_travel_key`: it is no longer the page the prefetch last
  ran for. The `Impl` comment (`DocumentView.cpp:85-87`) is rewritten to match.

### 3.2 A spread moves by pair, not by page

In the spread the key is the pair's left page (`canonical_page`, the cover rule of
`src/ui/PdfCanvasLayout.hpp`). Without it, a go-to or a search hit that names a right page records
two moves: to the right page, then the snap back to its left page. The snap would read as a
backward move. Example on 8 pages: on pair (3,4), go to page 6. Per page this is 3→6 Forward, then
6→5 Backward. Per pair it is 3→5 Forward, then 5→5, which is no move.

`DocumentView.cpp` may include `ui/PdfCanvasLayout.hpp`. That header includes only `<vector>`, and
core already includes an STL-only ui header (`TabList.hpp:21` `ui/detail/StripLayout.hpp`).
`DocumentView.cpp:19` also includes `ui/ThumbnailPane.hpp`, which is not STL-only (it includes
`<windows.h>`), so core's dependency on ui headers already exists. This avoids a second copy of the
cover rule.

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

- Spread off (`IDM_VIEW_DUAL_PAGE`, spread → single): the flag changes and no page is set. The
  kick's seed (§3.1) sets the key.

If a future path snaps the page *before* changing the flag, the effect stays small. The switch's
own move is recorded, then the reset drops the reference and the next kick seeds it again. That is
one stale direction at most, the size of this bug. Not worth a guard.

### 3.4 The issue's example, after

1. Single page 10: key 10.
2. Spread on: the reset gives key −1. The snap to left(10) = 9 sets key 9, with no move.
3. Forward to pair (29,30): Forward. Back to (19,20): Backward.
4. Spread off: the reset gives key −1, and the direction stays Backward.
5. The single-page kick for page 19 seeds key 19, with no move. Its prefetch runs Backward and
   prepares page 18. The reader was going backward, so this is right.
6. The reader's next PgDn, to page 20, is Forward from key 19, and its prefetch prepares page 21.

## 4. Accepted behaviour (not changed, written down)

- **A jump is a move**, in every layout: go-to page, a thumbnail click, a search hit, a bookmark.
  Single-page view behaves this way today; the spread and the strip now do too.
- **The strip's current page can change without a scroll**, after a zoom or a resize that
  re-lays out the strip. That change counts as a move. Its direction is where the reader's current
  page went, which is what the next single-page turn starts from.
- **Each tab keeps its own direction**, in its own `DocumentView`. A tab switch can still touch the
  incoming tab's direction in two ways.
  - The continuous preference is global, but only the active tab's view gets the flag
    (`MainWindow.cpp:957`). Turn Continuous off in tab A, and a tab B last shown in the strip flips
    `continuous` when it is switched to. That flip resets B's key, and B's first kick seeds it
    again (§3.1). This is a real switch of B's layout, so it is handled as one.
  - A strip tab-switch kick restores B's strip position, and `strip_after_change` can then change
    B's current page. That change is recorded as a move, like any other strip page change.
- **Session restore**, in paged view, sets the page through `set_current_page` once, from the
  initial key −1. That is no move, and the default Forward stays, as today. In the strip, the kick
  navigates to page 0 and then `strip_navigate(st.page)` records a Forward move. That is also
  Forward.

## 5. Testing

New `[core][view][prefetch]` cases in `tests/unit/test_document_view.cpp`, on the 8-page
`selection.pdf` view the #49 cases use (`one_worker_view`). Its spread pairs are (0) (1,2) (3,4)
(5,6) (7).

### 5.1 Observer

The direction needs a test observer. `queued_render_count()` counts the neighbours, but it cannot
tell which one was queued. Add, beside the other test observers in `DocumentView.hpp`, which then
includes `core/detail/PrefetchPlan.hpp` (standard library only):

```cpp
// (#164) The direction the next single-page prefetch uses. Test observability only.
[[nodiscard]] TravelDirection prefetch_travel() const noexcept;
```

It is not named `travel_direction`. A member of that name would hide the free function from
every unqualified call inside `DocumentView` (§3.1).

### 5.2 The tests drive the view the way the app does

The cases call the view in the order the app does, with two helpers:
- `move(view, p)`: `set_current_page(p)` only. This is what `change_current_page`,
  `set_strip_current_page`, a snap and a carry all reduce to.
- `kick(view, p)`: `request_render_with_prefetch(p, drop, {})` only, with **no**
  `set_current_page` before it. This is `MainWindow::kick_render`'s single-page branch
  (`MainWindow.cpp:418`).

A single-page page turn is `move` then `kick`. After spread → single, the app calls `kick` alone,
with no `move` (§3.1). The #49 helper `queued_by` calls `set_current_page` before the prefetch, so
it would hide exactly that path. The new cases do not use it for the switch.

### 5.3 Cases

1. **Issue example, spread.** Single page 1: `move 1`, `kick 1`. `set_dual_page(true)`, then
   `move 1` (the snap), `move 3`, `move 5`, `move 7`, `move 5`. `set_dual_page(false)`, then
   `kick 5` (no move). The direction is Backward.
   - Before the fix it is Forward: the old line compares 5 with the reference 1 it kept from single
     page 1.
2. **Strip, the same shape.** Single page 1: `move 1`, `kick 1`. `set_continuous(true)`, then
   `move 1` (the carry), `move 2`, `move 5`, `move 3`. `set_continuous(false)`, then `move 3` (the
   carry, `PdfCanvasStrip.cpp:428`) and `kick 3`. The direction is Backward.
   - Before the fix, Forward.
3. **Snap is not a move.** `move 4`. `set_dual_page(true)`, then `move 3` (the snap), `move 5`,
   `move 3` (Backward). Then `move 6` (a go-to on a right page) and `move 5` (its snap). The
   direction is Forward.
   - Mutation: key by page instead of `canonical_page`. Then 3 → 6 reads Forward and 6 → 5 reads
     Backward, so it ends Backward.
4. **Switch is not a move.** `move 2`, `move 4` (Forward, page 4 is a right page in spread terms).
   `set_dual_page(true)`, then `move 3` (the snap). The direction is still Forward.
   - Mutation: no reset in `set_dual_page`. The key is then 4, and 4 → 3 reads Backward.
5. **The same flag again does not reset.** `set_continuous(true)`, `move 4`, `move 5` (Forward).
   `set_continuous(true)` again, which is what every tab switch does (`MainWindow.cpp:957`).
   Then `move 4`. The direction is Backward.
   - Mutation: reset on every call. The key is then −1 at page 4, so the direction stays Forward.
6. **End to end, through L1.** The #49 one-byte budget makes every page over a third of it, so the
   plan prefetches one neighbour, and L1 admits only its newest entry (`PageCache::put_pixmap`).
   With one worker, P0 runs before P1, so once the prefetch lands L1 holds just that neighbour.
   - Run case 1's sequence with every `kick` really rendering, and the worker not held.
   - After the final `kick 5`, poll with a 10 s deadline until `l1_contains(4) || l1_contains(6)`,
     then require `l1_contains(4)` and not `l1_contains(6)`.
   - Neither candidate can be left over from earlier: the only earlier renders are page 1 and its
     neighbour (0 or 2), and the final P0 for page 5 evicts them. A deadline miss fails the case,
     because it means no prefetch landed.
   - Before the fix, page 6 lands instead. This pins that the prefetch acts on the direction
     `set_current_page` recorded, now that the prefetch no longer records it.
7. **The first turn after spread off counts** (the regression the review found). Continue case 1:
   `move 6`, `kick 6`. The direction is Forward.
   - Mutation: no seed in `request_render_with_prefetch`. The key is then still −1 at page 6, so
     the direction stays Backward.
8. **The first turn after a tab switch that flips continuous off counts** (§4).
   `set_continuous(true)`, `move 5`, `move 3` (Backward). `set_continuous(false)`, then `kick 3`
   with no move. A tab switch to a paged-single target sets no page. Then `move 4`, `kick 4`. The
   direction is Forward.
   - Mutation: no seed. The direction stays Backward.

Two ways to prove a case can fail:
- Cases 1, 2 and 6 fail on the pre-fix production code: revert `DocumentView.cpp` and keep only
  the observer.
- Cases 3, 4, 5, 7 and 8 pass on the pre-fix code too, because it never records a move outside
  the prefetch. Each one is proven by its named mutation instead.

In both cases, apply the change, see the case fail, and restore (`mutation-test-hygiene`).

The existing #49 cases must pass unchanged. `hold_worker` and `queued_by` set the page and then
prefetch, so the single writer records the same moves the old line did. `test_text_prefetch_view`
uses the default budget, under which both neighbours are prefetched whatever the direction.

Build **Release** only; `ctest --test-dir build -C Release` serially. No GUI smoke: no canvas or
MainWindow code changes, and the call orders in §3.1 and §3.3 are existing code, cited above.

## 6. Out of scope, release

- `VERSION` is not bumped (issue constraint).
- No CHANGELOG line. The reader cannot see the change apart from one render's latency, as with
  #129.
- The P0/P1 race for a page larger than half the budget is #163, not this.
