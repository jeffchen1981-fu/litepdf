# Continuous scroll (#55), PR 1: Foundations — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land every pure unit, core API, fixture and measurement the continuous-scroll strip needs, with no user-visible change, so PR 2 can wire the canvas onto tested parts.

**Architecture:** The strip's logic lives in header-only pure units under `src/ui/detail/` (layout, scroll, position carry, scheduling with its per-page state, completion rules, effective layout and kick dispatch). `DocumentView`, `PageCache` and `RenderEngine` gain the small APIs the scheduler drives. A registry-backed `ViewPrefs` holds the preference (default off). Fixtures and a CLI bench measure the page-sizing gate; an in-process test with a held worker gates the scheduler's cancellation.

**Tech Stack:** C++17 (MSVC, `cxx_std_17`), Win32, MuPDF 1.27.2 (vendored), Catch2, CMake/ctest from VS 2022 BuildTools, Python 3 + reportlab 4.4.10 + Pillow for fixtures.

**Spec:** `docs/superpowers/specs/2026-10-22-continuous-scroll-design.md` (branch `claude/issue-55-042ab7`, spec head `24ecbf4`). Read §3–§5, §8 (PR 1 list) and §9 before any task; § numbers below refer to it. This plan covers **PR 1 only**. PR 2–4 each get their own plan once the previous PR has merged.

## Global Constraints

- The spec is `docs/superpowers/specs/2026-10-22-continuous-scroll-design.md`; every "§" in a task refers to it. Read the sections a task cites.
- Branch `claude/issue-55-042ab7`, baseline `main` @ `5a40d5e`. Line numbers are for `5a40d5e`; find code by name if a line moved.
- **Worktree build setup (once per worktree):** `third_party/mupdf` is empty in a worktree. From PowerShell: `cmd /c mklink /J third_party\mupdf C:\Users\User\projects\litepdf\third_party\mupdf`; if `build\_deps\catch2-src` is missing, copy it from `C:\Users\User\projects\litepdf\build\_deps\catch2-src`; reconfigure with `-DFETCHCONTENT_FULLY_DISCONNECTED=ON -DFETCHCONTENT_SOURCE_DIR_CATCH2=<worktree>\build\_deps\catch2-src`. Before the worktree is removed, delete the junction with `cmd /c rmdir third_party\mupdf` (never a recursive delete).
- Build and test **Release** only (Debug fails with LNK2038). Never a bare `cmake`/`ctest` (a MinGW CMake is on PATH). In every PowerShell call set:
  - `$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"`
  - `$ctest = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"`
  - build tests: `& $cmake --build build --target litepdf_unit_tests --config Release`; everything: `& $cmake --build build --config Release`
  - targeted run from the repo root: `build\tests\Release\litepdf_unit_tests.exe "<name or wildcard>"`
  - **Every "run" step in a task means: build the test target first, then run.** The executable does not rebuild itself; running it without a build tests a stale binary. A "see it fail" step that expects a compile error means the *build* fails.
  - full suite from **PowerShell** at the repo root: `& $ctest --test-dir build -C Release` (read the count with `-N` first; 534 at `5a40d5e`).
- `TEST_CASE` names: pure ASCII, prefixed by subsystem (`StripLayout: `, `StripScroll: `, `StripCarry: `, `StripSchedule: `, `StripCompletion: `, `EffectiveLayout: `, `DocumentView: `, `PageCache: `, `RenderEngine: `, `ViewPrefs: `, `StripFixtures: `, `CliArgs: `), because `ctest -R` matches names.
- **Existing tests are not edited** (spec §9.2). New behaviour comes through new functions or overloads; no existing signature changes.
- New pure headers live in `src/ui/detail/`, namespace `litepdf::ui`, include only the standard library and other `ui/detail` or `core/detail` headers (no Win32, no MuPDF), and need no CMake entry. New test files are added to `tests/CMakeLists.txt`'s `target_sources(litepdf_unit_tests PRIVATE ...)`; new core `.cpp` files to `CMakeLists.txt`'s `add_library(litepdf_core STATIC ...)`.
- Spec values used across tasks: gap `G = 8.0` DIP (`kStripGapDip`); arrow step 100 DIP; page step and "one screen" wheel step 0.9 × viewport; L1 defaults 5 entries / `kDefaultL1BudgetBytes` (1 GiB); sizing gates 1000-page PDF ≤ 16 ms, EPUB and CBZ ≤ 100 ms each; US Letter fallback 612 × 792 pt; registry `HKCU\SOFTWARE\LitePDF\Settings`, DWORD `ContinuousScroll`, default 0 in this PR.
- Document coordinates are `double` DIPs (§4.1). Pages are 0-indexed.
- **Spec deviations, decided here** (each task that owns one patches the spec in its commit):
  1. §3.3 names a new `DocumentView::l1_holds(page)`. One already exists (test observability, `DocumentView.cpp:485`) and it calls `PageCache::get_pixmap`, which refreshes the entry's LRU recency — a scheduler query must not reorder L1. This PR adds a non-bumping `PageCache::l1_contains` and `DocumentView::l1_contains` and leaves `l1_holds` untouched. Task 7 replaces every `l1_holds` mention in the spec with `l1_contains`.
  2. §3.4's Spread row says the spread-off kick "runs as `TabSwitch` (refit, restore, reschedule)". Restoring the tab's stored position would overwrite the §7.3 carry just applied, so the `Spread` actions are refit, readout, relayout and reschedule **without** restore. Task 6 patches that row.
  3. §4.6's rules 1–2 (bottom → last page, top → page 0) apply only when the strip is taller than the viewport by at least 1 DIP (`total − vp_h >= 1`). Otherwise max scroll is (about) 0, both rules would fire at once, and rule 1 would report the last page at the top; rule 3 then decides. Task 2 patches §4.6.
  4. §7.3 "Into the strip" says `scroll_y = anchor(page) + offset`, but the carried offset is measured from the page's top, and `anchor = top − G`, so a scrolled page would land 8 DIP short. The rule is: offset 0 → `anchor(page)` (where navigation puts the page); offset > 0 → `top(page) + offset`. Task 3 patches §7.3.
- Commit messages: conventional commits, English, ending with `Co-Authored-By: <the model that wrote the commit> <noreply@anthropic.com>`. In this worktree `git add` files by name (`git commit -a` trips over the MuPDF junction).
- No `VERSION` change and no CHANGELOG entry in this PR (no user-visible change).

## Review Focus

1. **A 10,000-page document at 800%.** Positions above 2^24 DIP must not quantise: adjacent page tops stay distinct and exact. Pinned by Task 1's precision test with its `float` control.
2. **Scrolling to the very bottom at a zoom where several pages fit.** The current page must be the last page, not the top fully visible one. Pinned by Task 2's bottom-rule test.
3. **A request cancelled from outside the strip (tab switch, invert) with no completion ever posted.** The page must leave Pending at the next pass and be requested again if still visible. Pinned by Task 4's sweep test and Task 11's step 4.
4. **A page whose size cannot be read, including page 0.** `page_sizes()` must still return one size per page (page 0's, else US Letter) instead of throwing. Pinned by Task 7's fallback test.
5. **Zooming twice quickly.** A page that already had a bitmap must keep a stale bitmap through both cancels, never drop to None (no white flash). Pinned by Task 4's double-scale-change test.

---

## Task 1: StripLayout and StripPos

**Files:**
- Create: `src/ui/detail/StripLayout.hpp`
- Test: `tests/unit/test_strip_layout.cpp`

**Interfaces:**
- Produces (namespace `litepdf::ui`):
  - `inline constexpr double kStripGapDip = 8.0;`
  - `struct StripPageSize { double w_pt = 0; double h_pt = 0; };`
  - `struct StripRange { int first = 0; int last = -1; bool empty() const noexcept { return last < first; } bool contains(int p) const noexcept; };` (inclusive)
  - `struct StripPos { enum class Kind { InPage, InGapAfter }; Kind kind = Kind::InPage; int page = 0; double fraction = 0; };` — `InGapAfter` with `page == -1` is the gap above page 0; `fraction` ∈ [0, 1).
  - `class StripLayout` with:
    - `StripLayout() = default;` (0 pages) and `StripLayout(std::vector<StripPageSize> sizes, double zoom_pct);`
    - `int page_count() const noexcept; double zoom_pct() const noexcept;`
    - `double width(int i) const; double height(int i) const;` — DIP, `pt × zoom_pct`
    - `double top(int i) const;` — `G + Σ_{j<i}(h_j + G)`
    - `double anchor(int i) const;` — `top(i) − G`; `anchor(0) == 0`
    - `double total_height() const noexcept;` — `G + Σ(h_j + G)`; 0 for 0 pages
    - `StripRange visible(double y0, double y1) const;` — pages whose `[top, top+h)` intersects `[y0, y1)`; binary search on the prefix sums
    - `double widest(StripRange r) const;` — max width in `r`, 0 when empty
    - `StripPos position_at(double y) const;` and `double y_of(const StripPos& p) const;` — exact inverses for any y in `[0, total_height())`

- [ ] **Step 1: Write the failing tests** in `tests/unit/test_strip_layout.cpp` (register it in `tests/CMakeLists.txt`):
  1. `StripLayout: tops follow the gap and prefix sums` — sizes `{100×200, 50×100, 100×300}` pt at zoom 1.0: `top(0)==8`, `top(1)==8+200+8==216`, `top(2)==216+100+8==324`, `total_height()==324+300+8==632`, `anchor(0)==0`, `anchor(1)==208`.
  2. `StripLayout: zoom scales heights not the gap` — same sizes at 2.0: `height(0)==400`, `top(1)==8+400+8==416`.
  3. `StripLayout: visible range at boundaries` — at zoom 1.0: `visible(0,8)` is empty (the top gap); `visible(0,9)` is `{0,0}`; `visible(208,216)` is empty (gap after page 0); `visible(207,217)` is `{0,1}`; `visible(631,632)` is empty; `visible(0,632)` is `{0,2}`.
  4. `StripLayout: position_at and y_of round-trip in pages and gaps` — for y in `{0, 4, 8, 100, 208, 212, 216, 623.9, 628}`: `y_of(position_at(y)) == Approx(y).margin(1e-9)`; `position_at(4)` is `InGapAfter{-1, 0.5}`; `position_at(212)` is `InGapAfter{0, 0.5}`; `position_at(108)` is `InPage{0, 0.5}`.
  5. `StripLayout: widest of a range` — widths `{100, 50, 100}`: `widest({1,1})==50`, `widest({0,2})==100`, `widest({})==0`.
  6. `StripLayout: double precision at 10000 pages and 800 percent` — 10,000 pages of 595×**842.5** pt at 8.0 (page height 6740, step 6748, `top(9999) == 67473260`): `top(9999) - top(9998) == 6748` exactly; `position_at(top(9999) + 1)` is `InPage{9999, f}` with `f > 0` and `y_of` of it `== top(9999) + 1`. Two controls in the same test, proving the assertions can fail under `float`: (a) accumulating the same 10,000 steps in a local `float` gives `top9999_f - top9998_f != 6748` (842.5 is chosen so the sums are not all multiples of `float`'s spacing; with 842 pt every sum is a multiple of 8 and exact in `float`, which would blind the test); (b) `CHECK(static_cast<float>(l.top(9999)) + 1.0f == static_cast<float>(l.top(9999)))` — at 67 M `float`'s spacing is 8, so a 1-DIP step is lost.
  7. `StripLayout: zero pages` — `page_count()==0`, `total_height()==0`, `visible(0,100).empty()`.

- [ ] **Step 2: Run them and see them fail** — `build\tests\Release\litepdf_unit_tests.exe "StripLayout: *"` → compile error (header missing).

- [ ] **Step 3: Implement `StripLayout.hpp`.** Store sizes and a `std::vector<double>` of tops built once in the constructor; `visible` and `position_at` use `std::upper_bound` on the tops.

- [ ] **Step 4: Run them and see them pass** — same command → all pass.

- [ ] **Step 5: Commit** — `git add src/ui/detail/StripLayout.hpp tests/unit/test_strip_layout.cpp tests/CMakeLists.txt`; `feat(ui): strip layout with exact document coordinates (#55)`.

## Task 2: StripScroll — clamps, steps, re-layout, current page, horizontal placement

**Files:**
- Create: `src/ui/detail/StripScroll.hpp`
- Test: `tests/unit/test_strip_scroll.cpp`
- Modify: `docs/superpowers/specs/2026-10-22-continuous-scroll-design.md` §4.6 (spec deviation 3)

**Interfaces:**
- Consumes: Task 1 (`StripLayout`, `StripPos`, `StripRange`, `kStripGapDip`).
- Produces (namespace `litepdf::ui`):
  - `inline constexpr double kStripArrowStepDip = 100.0;` and `inline constexpr double kStripPageStepFraction = 0.9;`
  - `double clamp_strip_scroll(double y, double total, double vp_h) noexcept;` — `[0, max(0, total − vp_h)]`; NaN → 0
  - `double relayout_scroll(const StripPos& top_point, const StripLayout& next, double vp_h);` — `clamp_strip_scroll(next.y_of(top_point), …)` (§4.5)
  - `int derive_current_page(const StripLayout& l, double scroll_y, double vp_h);` — §4.6 rules in order, rules 1–2 only when `total − vp_h >= 1` (spec deviation 3): at max scroll → last page; at 0 → page 0; else largest visible height, ties to the lower index; 0 pages → −1
  - `class CurrentPageTracker { public: void navigate(int page) noexcept; void scroll_input() noexcept; int recompute(const StripLayout& l, double scroll_y, double vp_h); int current() const noexcept; bool pinned() const noexcept; void reset() noexcept; };` — `navigate` sets and pins; `scroll_input` ends the pin; `recompute` keeps the pinned page while pinned, else derives; `reset` clears page to 0 and the pin (used by `set_view` in PR 2)
  - `double effective_pan_x(double desired, double widest, double vp_w) noexcept;` — `clamp_pan` semantics in `double`: `widest <= vp_w` → 0, else clamp to `[vp_w − widest, 0]`
  - `double apply_pan_x_delta(double desired, double delta, double widest, double vp_w) noexcept;` — `effective_pan_x(effective_pan_x(desired,…) + delta, …)`
  - `double page_x(double page_w, double widest, double pan_x_eff, double vp_w) noexcept;` — §4.2: `page_w <= vp_w` → `(vp_w − page_w) / 2`; else `pan_x_eff + (widest − page_w) / 2`

- [ ] **Step 1: Write the failing tests** in `tests/unit/test_strip_scroll.cpp` (register it). Use the 3-page layout of Task 1 test 1 (`total_height()==632`) unless stated:
  1. `StripScroll: clamp keeps scroll inside the strip` — `vp_h=300`: `clamp(-5)==0`, `clamp(400)==332`, `clamp(NaN)==0`; `vp_h=700` (strip fits): every input → 0.
  2. `StripScroll: relayout keeps the viewport-top point` — top point `position_at(108)` (`InPage{0,0.5}`) re-laid out at zoom 2.0 → `top(0)+0.5*400 == 208`; a point in the gap after page 0 at zoom 2.0 → `top(0)+400+4 == 412`.
  3. `StripScroll: current page at the bottom is the last page` — 10 pages of 100×100 pt at zoom 1.0, `vp_h=500` (four pages fully visible): at `scroll_y == total − 500`, `derive_current_page == 9`; at `scroll_y == 0`, `== 0`; at `scroll_y == anchor(3)`, `== 3` (tie among 3..6 → lowest).
  4. `StripScroll: current page is the one with the largest visible height` — 3-page layout, `vp_h=150`, `scroll_y=150` (page 0 shows 58, page 1 shows 84): `== 1`.
  4b. `StripScroll: a strip that fits the viewport is decided by visible height` — the 3-page layout (heights 200/100/300, `total == 632`), `vp_h=700`: `derive_current_page(l, 0, 700) == 2` (rule 3: page 2 shows the most; rule 1 must not fire, and a "fits → page 0" shortcut would wrongly give 0). Near-fit: the same layout with `vp_h = 631.75` (`total − vp_h = 0.25 < 1`) at `scroll_y = 0` → also 2, not "last page by rule 1" and not "page 0 by rule 2".
  5. `StripScroll: navigation pins until scroll input` — tracker: `navigate(9)` at a scroll where derivation says 8 → `recompute == 9`; a second `recompute` after a re-layout still 9; `scroll_input()` then `recompute` → derived value.
  6. `StripScroll: desired pan_x survives a wide page leaving view` — `vp_w=600`: desired −300 with widest 1000 → effective −300; with widest 500 (wide page gone) → effective 0 while the stored desired stays −300; widest 1000 again → −300. `apply_pan_x_delta(-300, -500, 1000, 600) == -400`.
  7. `StripScroll: page_x centres a narrow page and pans a wide one` — `vp_w=600`, widest 1000, pan −200: `page_x(400,…) == 100`; `page_x(1000,…) == -200`; `page_x(800,…) == -200 + 100 == -100`.

- [ ] **Step 2: Run and see them fail** — `build\tests\Release\litepdf_unit_tests.exe "StripScroll: *"` → compile error.

- [ ] **Step 3: Implement `StripScroll.hpp`.** `derive_current_page` uses `l.visible(scroll_y, scroll_y + vp_h)` and compares clipped heights; "tall" means `total - vp_h >= 1`; "at max" means `tall && scroll_y >= (total - vp_h) - 0.5`, and "at 0" means `tall && scroll_y <= 0.5` (with `tall`, the two cannot both hold). In the spec's §4.6, after the three rules, add: "Rules 1–2 apply only when the strip is at least 1 DIP taller than the viewport; otherwise rule 3 decides."

- [ ] **Step 4: Run and see them pass.**

- [ ] **Step 5: Commit** — `feat(ui): strip scroll, current-page derivation and horizontal placement (#55)`; include the spec edit.

## Task 3: StripCarry — position carried across a layout switch

**Files:**
- Create: `src/ui/detail/StripCarry.hpp`
- Test: `tests/unit/test_strip_carry.cpp`
- Modify: `docs/superpowers/specs/2026-10-22-continuous-scroll-design.md` §7.3 "Into the strip" (spec deviation 4)

**Interfaces:**
- Consumes: Task 1 (`StripLayout`), Task 2 (`clamp_strip_scroll`), `ui/detail/ViewportMath.hpp` (`clamp_pan`).
- Produces (namespace `litepdf::ui`, §7.3):
  - `struct CarriedPosition { int page = 0; double offset_dip = 0; };`
  - `CarriedPosition carry_from_paged_single(int page, double pan_y, double page_origin_y, double page_h) noexcept;` — offset `clamp(−pan_y − page_origin_y, 0, page_h)`
  - `CarriedPosition carry_from_spread(int left_page, double pan_y, double left_top_in_union, double left_h) noexcept;` — offset `clamp(−pan_y − left_top_in_union, 0, left_h)`
  - `CarriedPosition carry_from_strip(const StripLayout& l, int page, double scroll_y);` — offset `clamp(scroll_y − l.top(page), 0, l.height(page))`
  - `double strip_scroll_for(const StripLayout& l, const CarriedPosition& c, double vp_h);` — `clamp_strip_scroll(c.offset_dip == 0 ? l.anchor(c.page) : l.top(c.page) + c.offset_dip, …)` (spec deviation 4)
  - `float paged_pan_y_for(const CarriedPosition& c, double page_h, double vp_h) noexcept;` — `clamp_pan(−offset, page_h, vp_h)`
  - `float spread_pan_y_for(const CarriedPosition& c, double left_top_in_union, double union_h, double vp_h) noexcept;` — `clamp_pan(−(left_top_in_union + offset), union_h, vp_h)`

- [ ] **Step 1: Write the failing tests** (`tests/unit/test_strip_carry.cpp`, registered):
  1. `StripCarry: a fitting paged page carries offset zero` — `carry_from_paged_single(4, 0, 37.5, 300).offset_dip == 0`, and `strip_scroll_for` of it equals `anchor(4)` (the same place navigation puts page 4).
  2. `StripCarry: an overflowing paged page carries how far it is scrolled` — `carry_from_paged_single(2, -250, 0, 1000).offset_dip == 250`; into a strip layout tall enough not to clamp: `strip_scroll_for(l, c, 600) - l.top(2) == 250` (not 242: the offset is from the page's top, not its anchor); back out: `carry_from_strip(l, 2, that scroll).offset_dip == 250` and `paged_pan_y_for(…, 1000, 600) == -250`.
  3. `StripCarry: a spread with unequal heights is measured on the left page` — left page top inside the union at 40 DIP, `pan_y = -140`, left height 500: offset 100; `spread_pan_y_for(c, 40, 800, 600) == -140`.
  4. `StripCarry: offsets clamp to the page` — `carry_from_strip` at a `scroll_y` inside the gap after the page gives `offset == height(page)`; a `scroll_y` above `top(page)` gives 0.

- [ ] **Step 2: Run and see them fail** — `"StripCarry: *"`.
- [ ] **Step 3: Implement `StripCarry.hpp`.** In the spec's §7.3, replace "**Into the strip.** `scroll_y = anchor(page) + offset`." with "**Into the strip.** `scroll_y = anchor(page)` when the offset is 0, else `top(page) + offset` (the offset is measured from the page's top)."
- [ ] **Step 4: Run and see them pass.**
- [ ] **Step 5: Commit** — `feat(ui): position carry between paged and strip layouts (#55)`; include the spec edit.

## Task 4: StripSchedule — per-page state and the scheduling pass

**Files:**
- Create: `src/ui/detail/StripSchedule.hpp`
- Test: `tests/unit/test_strip_schedule.cpp`

**Interfaces:**
- Consumes: Task 1 (`StripRange`, `StripLayout`).
- Produces (namespace `litepdf::ui`, §5.1–5.2):
  - `enum class StripTravel { Down, Up };`
  - `enum class PageStateKind { None, Pending, Ready, Failed };`
  - `struct PageSlot { PageStateKind kind = PageStateKind::None; std::uint64_t seq = 0; int priority = 0; float scale = 0; bool invert = false; std::shared_ptr<std::atomic<bool>> cancel_flag; bool has_stale_bitmap = false; float stale_scale = 0; bool stale_invert = false; };` — `priority` is the submitted priority for Pending and the failed request's priority for Failed; `scale`/`invert` are the submitted (Pending) or rendered (Ready) values; `has_stale_bitmap` marks a Pending page still holding its old bitmap, whose render parameters are `stale_scale`/`stale_invert`.
  - `bool is_stale(const PageSlot& s, float scale, bool invert) noexcept;` — Ready and (scale or invert differs)
  - `PageSlot cancel_transition(const PageSlot& s) noexcept;` — Pending with stale bitmap → Ready with `scale = stale_scale`, `invert = stale_invert`; Pending without → None; other states unchanged
  - `StripRange prefetch_range(const StripLayout& l, double scroll_y, double vp_h, StripTravel t);` — the pages intersecting the one viewport height beyond the viewport in the travel direction, minus V (needs Task 1's `StripLayout`)
  - `struct ScheduleInput { StripRange visible; std::vector<int> visible_by_area; StripRange prefetch; StripRange retain; float scale; bool invert; std::size_t l1_capacity; std::size_t l1_budget; std::function<std::size_t(int)> bytes_of; int previous_first_visible; int previous_last_visible; };` — `visible_by_area` is V with `derive_current_page(...)`'s page first (the predicted current page, so at the bottom it is the last page, not the top tied one), then the rest by visible height descending, ties to the lower index; `prefetch` is P (caller computes it from the travel direction, one viewport ahead); `retain` is V plus one viewport each side.
  - `struct SchedulePlan { std::vector<int> sweep; std::vector<int> release; std::vector<int> fail_reset; std::vector<int> cancel; std::vector<int> submit_p0; std::vector<int> submit_p1; int text_on_p0 = -1; bool visible_changed = false; };` — the canvas applies it in exactly this order: sweep (cancel transition), release (cancel if Pending, then drop), fail_reset (→ None), cancel (cancel transition), submit_p0 in order, submit_p1 in order; `text_on_p0` is the page among `submit_p0` that carries `on_text` (−1 for none).
  - `SchedulePlan plan_strip_pass(const std::map<int, PageSlot>& slots, const ScheduleInput& in);` — pure. Swept pages are judged by every later rule **as their `cancel_transition` result** (so a swept page in V is resubmitted in the same pass). Rules: sweep = Pending with `cancel_flag` set; release = slots outside `retain`; fail_reset = Failed pages leaving V, plus Failed-at-P1 pages in V; P0 for each V page (in `visible_by_area` order) that is None, Ready-stale, Failed-at-P1, or Pending at P1 (those also go into `cancel`); P1 for each P page that is None, only while `l1_capacity − |V|` entries and `l1_budget − Σ bytes_of(V)` bytes remain (accumulating); cancel = Pending pages outside V ∪ P **but inside `retain`** (a Pending page outside `retain` is handled once, by `release`; the two lists never share a page); `text_on_p0` = `visible_by_area[0]` iff it is in `submit_p0`; `visible_changed` = V differs from the previous first/last.
  - `class StripRenderState` — owns `std::map<int, PageSlot>` and applies a plan:
    - `struct Submitter { virtual ~Submitter() = default; virtual std::uint64_t submit(int page, int priority, bool with_text, std::shared_ptr<std::atomic<bool>> flag) = 0; virtual void cancel_thumbnails() = 0; virtual void released(int page) = 0; };`
    - `SchedulePlan run_pass(const ScheduleInput& in, Submitter& out);` — computes the plan, applies it in the order above (cancelling = storing `true` to the slot's flag, then `cancel_transition`), creates a fresh flag per submit, records `{seq, priority, scale, invert}`; when a Ready-stale page is resubmitted it becomes Pending with `has_stale_bitmap = true` and `stale_scale`/`stale_invert` copied from the Ready slot's `scale`/`invert`; calls `cancel_thumbnails()` iff `visible_changed`; returns the plan.
    - `void change_render_params(float scale, bool invert);` — §5.1/§5.2 render-scale/polarity change: cancel every Pending (cancel transition); every Failed → None (so a page that failed at P0 is requested again at the new scale, §5.3 rule 5); Ready stays and is now stale by `is_stale`.
    - `void drop_bitmaps();` — `discard_render_target`: Ready → None; Pending keeps state but `has_stale_bitmap = false`.
    - `bool request_text(int page, bool slot_has_text, bool text_pending, bool l1_contains, Submitter& out);` — §5.2 second bullet, for a current-page change or the current page's P0 completing: when the page's text is neither in the slot nor pending and `l1_contains`, cancel the previous text-only request (if any) and `submit(page, 1, true, fresh flag)`; returns whether it submitted. The text-only request is tracked apart from the page's slot and never changes the slot's state. A P0 carrying text is never cancelled here.
    - `const PageSlot* slot(int page) const; std::map<int, PageSlot>& slots();` (the latter for Task 5 and the canvas)
    - `void clear(Submitter& out);` — cancel every Pending, release all.

- [ ] **Step 1: Write the failing tests** (`tests/unit/test_strip_schedule.cpp`, registered). A `FakeSubmitter` records `(page, priority, with_text, flag)` tuples (keeping each submitted flag), counts `cancel_thumbnails`, records releases, and returns increasing seqs. Unless stated: `scale=1`, `invert=false`, `l1_capacity=5`, budget 1 GiB, `bytes_of` = 1 MB per page.
  1. `StripSchedule: visible pages get P0 largest area first and the first carries text` — V {2,3,4}, `visible_by_area` {3,2,4}: P0 submits `3,2,4`; only 3 `with_text`.
  2. `StripSchedule: prefetch only while L1 has room` — V {0..3}, P {4,5}, capacity 5: one P1 (page 4); with `bytes_of`= 300 MB and budget 1 GiB, V uses 1.2 GB → no P1.
  3. `StripSchedule: pages leaving V and P are cancelled and their flags set` — after a pass on V {0,1}, run V {5,6} with `retain = {0,10}` (so the slots stay observable): pages 0 and 1's flags (kept from the first pass) read `true`, and their slots are None.
  4. `StripSchedule: a pending page whose flag was set outside is swept and requested again` — after a pass on V {0}, set page 0's flag directly (no completion), run the same V: page 0 is resubmitted at P0 with a new seq.
  5. `StripSchedule: a P1 page entering V is promoted` — pass V {0}, P {1}; then V {1}: page 1's old flag set, new P0 submitted.
  6. `StripSchedule: a page that failed at P1 gets one P0 when it enters V, one that failed at P0 does not` — set page 2 Failed(priority 1) → entering V submits P0; set page 3 Failed(priority 0) in V → no submit; page 3 leaving V → None.
  7. `StripSchedule: two quick scale changes keep the stale bitmap` — page 0 Ready at 1.0; `change_render_params(2.0,false)`; pass → page 0 Pending with `has_stale_bitmap`; `change_render_params(3.0,false)` → page 0 Ready (stale, scale 1.0), not None; pass → Pending with stale bitmap again.
  8. `StripSchedule: thumbnails are cancelled only when V changes` — two passes on the same V → `cancel_thumbnails` count 1; a third on a new V → 2.
  9. `StripSchedule: pages outside retain are released` — V {10}, retain {9..11}, a Ready slot at page 2 → `released(2)` called and the slot gone.
  10. `StripSchedule: drop_bitmaps keeps pending pages pending` — Ready → None; Pending with stale bitmap → Pending without.
  11. `StripSchedule: a text request goes out only for a page still in L1` — `request_text(4, false, false, true, s)` submits `(4, 1, true)`; `request_text(5, …, true)` then cancels page 4's text flag and submits for 5; `l1_contains=false`, `slot_has_text=true` or `text_pending=true` → no submit; page 4's slot state is unchanged throughout.
  12. `StripSchedule: prefetch range follows the travel direction` — 3-page layout of Task 1 (pages at [8,208), [216,316), [324,624)), `vp_h=100`: Down at `scroll_y=100` (V {0}, window [200,300)) → `{1,1}`; Up at `scroll_y=400` (V {2}, window [300,400)) → `{1,1}`; Down at `scroll_y=400` (window [500,600) is page 2, already in V) → empty; Up at `scroll_y=0` → empty.
  13. `StripSchedule: a page that failed at P0 is requested again after a scale change` — page 3 Failed(priority 0), V {3}; `change_render_params(2.0,false)`; pass at scale 2.0 → `submit_p0 == {3}`.
  14. `StripSchedule: a pending page outside retain is cancelled once and released` — page 2 Pending, then a pass with V {10}, retain {9..11}: page 2 is in `release` and not in `cancel`, its flag reads `true`, `released(2)` is called once, and the slot is gone.

- [ ] **Step 2: Run and see them fail** — `"StripSchedule: *"`.
- [ ] **Step 3: Implement `StripSchedule.hpp`.**
- [ ] **Step 4: Run and see them pass.**
- [ ] **Step 5: Commit** — `feat(ui): strip render scheduling with per-page state (#55)`.

## Task 5: StripCompletion — the completion rules

**Files:**
- Create: `src/ui/detail/StripCompletion.hpp`
- Test: `tests/unit/test_strip_completion.cpp`

**Interfaces:**
- Consumes: Task 4 (`PageSlot`, `PageStateKind`, `cancel_transition`, `StripRenderState`).
- Produces (namespace `litepdf::ui`, §5.3):
  - `struct StripCompletionMeta { std::uint64_t epoch = 0; int page = 0; std::uint64_t seq = 0; bool cancelled = false; };` — the `WM_USER_STRIP_DONE` payload's identity (PR 2 adds the escrow beside it)
  - `enum class StripCompletionAction { Drop, CreateBitmap, CancelAndReschedule, MarkFailed };`
  - `StripCompletionAction classify_strip_completion(const PageSlot* slot, const StripCompletionMeta& m, std::uint64_t view_epoch, bool has_pixmap) noexcept;` — rules 1–5 in order (`slot == nullptr` or not Pending or seq mismatch → Drop)
  - `enum class BitmapResult { Ok, RecreateOrNoTarget, OtherFailure };`
  - `PageSlot after_bitmap(const PageSlot& pending, BitmapResult r) noexcept;` — Ok → Ready with the Pending's submitted `scale`/`invert`; RecreateOrNoTarget → None; OtherFailure → Failed with the Pending's `priority`
  - `PageSlot after_null(const PageSlot& pending, bool cancelled) noexcept;` — cancelled → `cancel_transition`; else Failed with the Pending's `priority`

- [ ] **Step 1: Write the failing tests** (`tests/unit/test_strip_completion.cpp`, registered):
  1. `StripCompletion: a completion from another epoch is dropped` — epoch 3 vs view 4 → Drop.
  2. `StripCompletion: a completion for a page that is not pending is dropped` — Ready slot, or null slot → Drop.
  3. `StripCompletion: a stale seq is dropped` — Pending seq 7, meta seq 6 → Drop.
  4. `StripCompletion: a pixmap is created and lands at the submitted scale` — Pending `{seq 7, scale 2.0}` + pixmap → CreateBitmap; `after_bitmap(Ok)` is Ready with scale 2.0, and `is_stale(…, 3.0, false)` is true (a change the cancel missed).
  5. `StripCompletion: bitmap failures` — RecreateOrNoTarget → None; OtherFailure from a P1 → Failed with priority 1.
  6. `StripCompletion: cancelled null takes the cancel transition` — with stale bitmap → Ready (stale); without → None; action CancelAndReschedule.
  7. `StripCompletion: uncancelled null fails` — → MarkFailed; `after_null(…, false)` Failed.

- [ ] **Step 2: Run and see them fail** — `"StripCompletion: *"`.
- [ ] **Step 3: Implement `StripCompletion.hpp`.**
- [ ] **Step 4: Run and see them pass.**
- [ ] **Step 5: Commit** — `feat(ui): strip completion rules (#55)`.

## Task 6: EffectiveLayout and the KickReason dispatch table

**Files:**
- Create: `src/ui/detail/EffectiveLayout.hpp`
- Test: `tests/unit/test_effective_layout.cpp`
- Modify: `docs/superpowers/specs/2026-10-22-continuous-scroll-design.md` §3.4 Spread row (spec deviation 2)

**Interfaces:**
- Produces (namespace `litepdf::ui`, §3.4, §7.3):
  - `enum class LayoutKind { PagedSingle, PagedSpread, Strip };`
  - `LayoutKind effective_layout(bool has_view, bool spread, bool pref_continuous, bool format_ok) noexcept;` — no view → PagedSingle; spread → PagedSpread; pref and format_ok → Strip; else PagedSingle
  - `enum class KickReason { Layout, Zoom, Navigate, TabSwitch, Restore, Restyle, Spread };`
  - `struct StripKickActions { bool refit = false; bool refresh_readout = false; bool restore_tab_position = false; bool relayout = false; bool restyle = false; bool reschedule = true; };`
  - `StripKickActions strip_kick_actions(KickReason r) noexcept;` — the strip branch only (callers in paged layouts never consult it):
    - Layout: refit, readout, relayout
    - Zoom: readout, relayout
    - Navigate: reschedule only
    - TabSwitch: refit, readout, restore, relayout
    - Restore: refit, readout, relayout (the caller navigates to the saved page)
    - Restyle: restyle
    - Spread (spread turned off, entering the strip; the carry was applied by `apply_effective_layout`): refit, readout, relayout — **no** restore (spec deviation 2)
  - `KickReason tab_switch_reason(int new_index, int old_index) noexcept;` — equal → Navigate, else TabSwitch

- [ ] **Step 1: Write the failing tests** (`tests/unit/test_effective_layout.cpp`, registered):
  1. `EffectiveLayout: spread wins over the preference` — `(true,true,true,true) == PagedSpread`.
  2. `EffectiveLayout: the strip needs the preference and a format that passed` — `(true,false,true,true) == Strip`; `(true,false,true,false) == PagedSingle`; `(true,false,false,true) == PagedSingle`; `(false,…) == PagedSingle`.
  3. `EffectiveLayout: kick reasons map to the spec table` — one `CHECK` row per reason, every field, against the list above.
  4. `EffectiveLayout: a same-index tab switch only reschedules` — `tab_switch_reason(2,2) == Navigate`, `tab_switch_reason(2,1) == TabSwitch`.

- [ ] **Step 2: Run and see them fail** — `"EffectiveLayout: *"`.
- [ ] **Step 3: Implement `EffectiveLayout.hpp`.** In the spec's §3.4 `kick_render` reasons table, change the `Spread` row's strip branch to: "`apply_effective_layout` has already run, in either direction. Turning spread on leaves the strip: the kick is paged. Turning it off with the preference on enters the strip: refit, refresh the readout, re-layout, reschedule — **not** a tab-position restore, which would overwrite the §7.3 carry `apply_effective_layout` just applied."
- [ ] **Step 4: Run and see them pass.**
- [ ] **Step 5: Commit** — `feat(ui): effective layout and kick reason dispatch (#55)`; include the spec edit.

## Task 7: Core APIs — continuous flag, page_sizes, request_page_render, l1_contains, thumbnail cancel

**Files:**
- Modify: `src/core/DocumentView.hpp` (add `#include "core/RenderEngine.hpp"` — `RenderEngine::RenderToken` is a nested type and cannot be forward-declared; the header is MuPDF-free), `src/core/DocumentView.cpp` (`set_current_page` `:253-270`, `request_render` `:340-350`, `Impl` beside `page_sizes` `:93`)
- Create: `src/core/detail/PageSizeFill.hpp`
- Modify: `src/core/PageCache.hpp`, `src/core/PageCache.cpp` (beside `get_pixmap`)
- Modify: `docs/superpowers/specs/2026-10-22-continuous-scroll-design.md` (every `l1_holds` outside §12; spec deviation 1)
- Create: `tests/unit/test_document_view_strip.cpp`

**Interfaces:**
- Produces on `DocumentView`:
  - `void set_continuous(bool on) noexcept; bool continuous() const noexcept;` — while on, `set_current_page` still drops the text slot but does not call `set_viewport` (§4.3)
  - `const std::vector<PageSize>& page_sizes();` — sizes every page once into a **new** `Impl` member `std::vector<PageSize> strip_sizes` (built by `detail::fill_page_sizes`, reading through the existing `std::optional` cache where it already holds a size, and writing successful sizes into that cache, but **never writing a fallback into it**: `estimated_bytes` must keep seeing an unreadable page as unknown, §4.4 and #49); a page whose `page_size` throws gets page 0's size, and page 0 throwing gives `{612, 792}`; never throws for a size failure; UI thread only
  - `RenderEngine::RenderToken request_page_render(int page, int priority, RenderCb on_complete, std::shared_ptr<std::atomic<bool>> cancel_flag, TextCb on_text = {});` — calls `release_l1_pin()` first; sets scale/invert like `request_render`; passes `cancel_flag`; attaches `on_text`/`text_wanted` (storing `page`) only when `on_text` is given and `wants_text_prefetch(page)`; does not cancel anything
  - `bool l1_contains(int page) const;` — current scale and polarity, via `PageCache::l1_contains`
  - `void cancel_thumbnail_renders();` — `engine->cancel_all_below_priority(2)`
- Produces on `PageCache`: `bool l1_contains(int page_num, float scale, bool invert) const;` — under the mutex, no recency change, no refcount change.

- [ ] **Step 1: Write the failing tests** in `tests/unit/test_document_view_strip.cpp` (registered; reuse the `HeldWorker`/`ReleaseOnExit`/`one_worker_view` pattern of `test_document_view.cpp:361-416`, copied into an anonymous namespace here, since existing tests are not edited):
  1. `DocumentView: continuous mode does not refit on a page change` — `spread-unequal.pdf` (2 pages: 420 and 595 pt wide), viewport 800×600 @96, FitWidth, two **separate** views starting on page 0: view A `set_continuous(true)`, record `zoom_pct()`, `set_current_page(1)` → `zoom_pct()` unchanged; view B (continuous off, the control) `set_current_page(1)` → `zoom_pct()` changes.
  2. `DocumentView: page_sizes returns one size per page` — `selection.pdf`: size 8; each equals `Document::page_size(i)`; a second call returns the same object (no re-sizing).
  3. `DocumentView: page size fallback when a size cannot be read` — tests the pure helper `litepdf::core::detail::fill_page_sizes(std::size_t n, const std::function<PageSize(std::size_t)>& size_of) -> std::vector<PageSize>` in the new `src/core/detail/PageSizeFill.hpp` (which `page_sizes()` uses): a `size_of` throwing `std::runtime_error` for index 3 → result[3] equals result[0]; throwing for index 0 → result[0] is `{612, 792}` and every other throwing index takes it too; no exception escapes.
  4. `DocumentView: request_page_render returns a token whose flag cancels` — hold the worker; submit page 3 with a fresh flag; store `true` to the flag; release; the callback receives `nullptr`.
  5. `DocumentView: request_page_render releases the background pin` — render page 0, `trim_l1_for_background(0)`, then `request_page_render(5, 0, …)` and wait → `l1_contains(5)`.
  6. `PageCache: l1_contains does not change LRU order` — capacity 2: put A, put B, `l1_contains(A)`, put C → A is evicted (with `get_pixmap(A)` instead, B would be).
  7. `DocumentView: cancel_thumbnail_renders leaves strip requests alone` — hold the worker; `request_page_render` page 2 at P0 and page 3 at P1; `cancel_thumbnail_renders()`; release → both callbacks receive non-null pixmaps and neither flag reads true. (That `cancel_all_below_priority(2)` flags a queued P3 is the engine's existing contract; this test pins only that the strip's own priorities survive it.)

- [ ] **Step 2: Run and see them fail** — `"DocumentView: *"` and `"PageCache: l1_contains*"`.
- [ ] **Step 3: Implement the APIs** and `src/core/detail/PageSizeFill.hpp`. In the spec's §3.3, replace the `l1_holds(page)` bullet with: "`l1_contains(page)`: whether L1 has the page at the current scale and polarity, without changing its LRU recency (`PageCache::l1_contains`). The existing `l1_holds` refreshes recency and stays test-only." Then replace every other `l1_holds` in the spec with `l1_contains`, except inside §12 (the review record keeps its history): `rg -n l1_holds docs/superpowers/specs/2026-10-22-continuous-scroll-design.md` must afterwards list only that sentence and §12 lines.
- [ ] **Step 4: Run them, then the full suite** — `"DocumentView: *"` passes; `& $ctest --test-dir build -C Release` all pass (the existing `DocumentView ...` tests unchanged).
- [ ] **Step 5: Commit** — `feat(core): page sizes, token-returning render requests and non-bumping L1 query (#55)`; include the spec edit.

## Task 8: RenderEngine started-render counter

**Files:**
- Modify: `src/core/RenderEngine.hpp` (beside `text_builds_started` `:141`), `src/core/RenderEngine.cpp` (worker loop, after the L1 check `:179-196`; `Impl` beside `text_builds` `:76`)
- Modify: `src/core/DocumentView.hpp`, `src/core/DocumentView.cpp` (the forwarding accessor)
- Test: `tests/unit/test_render_engine_counter.cpp`

**Interfaces:**
- Produces: `[[nodiscard]] std::size_t RenderEngine::renders_started() const noexcept;` — incremented once per request with `bypass_cache == false` that passes checkpoint A **and** misses L1 (it will build or reuse a display list and rasterise). Requests cancelled at A, L1 hits, and `bypass_cache` (thumbnail) requests are not counted. Test observability only.
- Produces: `[[nodiscard]] std::size_t DocumentView::renders_started() const noexcept;` forwarding to the engine.

- [ ] **Step 1: Write the failing tests** (`tests/unit/test_render_engine_counter.cpp`, registered). Build at engine level, as `test_thumbnail_renderer.cpp:51` and `test_render_engine_cancel.cpp:22-24` do: open `selection.pdf` into a `Document`, a `PageCache cache(/*l1=*/4, /*l2=*/4, <a context cloned from the document>)`, and `RenderEngine engine(doc, 1, &cache)`; submit `RenderRequest`s directly and wait on promises. Hold the single worker by submitting a P0 for page 0 whose callback signals "entered" and blocks on a release future (release in a scope guard declared after the engine). Drop the cloned cache context after the cache's scope, as `test_thumbnail_renderer.cpp:73` does.
  1. `RenderEngine: renders_started counts L1 misses only` — render page 0 and wait (count 1); render page 0 again at the same scale (L1 hit, still 1); render page 1 (2).
  2. `RenderEngine: renders_started skips cancelled and bypass requests` — hold the worker on page 0 (count 1 once it entered); submit page 2 with a caller-owned `cancel_flag` set to true before release; submit page 3 with `bypass_cache = true`; release and wait for all three callbacks → count still 1.
- [ ] **Step 2: Run and see them fail** — `"RenderEngine: renders_started*"`.
- [ ] **Step 3: Implement** — one `std::atomic<std::size_t>` in `Impl`, `fetch_add(1)` right after the L1 block when `!req.bypass_cache`.
- [ ] **Step 4: Run and see them pass**, then the full suite.
- [ ] **Step 5: Commit** — `test(core): count renders that miss L1 (#55)`.

## Task 9: ViewPrefs — the continuous-scroll preference

**Files:**
- Create: `src/core/ViewPrefs.hpp`, `src/core/ViewPrefs.cpp` (add to `add_library(litepdf_core ...)` in `CMakeLists.txt`, beside `src/core/MruList.cpp`)
- Test: `tests/unit/test_view_prefs.cpp`

**Interfaces:**
- Produces (namespace `litepdf::core`), modelled on `MruList`:
  - `class ViewPrefs { public: static constexpr wchar_t kDefaultKey[] = L"SOFTWARE\\LitePDF\\Settings"; static constexpr bool kContinuousScrollDefault = false; explicit ViewPrefs(const wchar_t* registry_subkey = kDefaultKey); void load(); bool save() const; bool continuous_scroll() const noexcept; void set_continuous_scroll(bool on) noexcept; };`
  - `load()` reads DWORD `ContinuousScroll` under HKCU; a missing key, missing value or wrong type leaves the default. `save()` writes it, returning false on failure.

- [ ] **Step 1: Write the failing tests** (`tests/unit/test_view_prefs.cpp`, registered; test key `SOFTWARE\LitePDF\TestSettings`, deleted at the start of each test):
  1. `ViewPrefs: a missing value gives the default` — `continuous_scroll() == kContinuousScrollDefault` (false).
  2. `ViewPrefs: save and load round trip` — set true, save, new instance load → true; set false, save, load → false.
  3. `ViewPrefs: a value of the wrong type is ignored` — write `ContinuousScroll` as `REG_SZ` "1" → load gives the default.
- [ ] **Step 2: Run and see them fail** — `"ViewPrefs: *"`.
- [ ] **Step 3: Implement `ViewPrefs`.**
- [ ] **Step 4: Run and see them pass.**
- [ ] **Step 5: Commit** — `feat(core): registry-backed continuous-scroll preference, default off (#55)`.

## Task 10: Fixtures and the page-sizing gate

**Files:**
- Create: `scripts/generate-strip-fixtures.py`
- Create (generated, committed): `tests/fixtures/strip-1000.pdf`, `tests/fixtures/strip-chapters.epub`, `tests/fixtures/strip-images.cbz`
- Modify: `src/cli/cli_args.hpp`, `src/cli/cli_args.cpp` (all argument parsing lives here, #100: unknown flags are rejected at `:56`, and `--iterations` outside `--benchmark`/`--bench-selection` at `:74`)
- Modify: `src/cli/main.cpp` (usage text `:242-246`; a `--bench-page-sizes` branch beside `--bench-selection`)
- Create: `tests/unit/test_cli_args_page_sizes.cpp` (the existing `test_cli_args.cpp` is not edited)
- Modify: `.github/workflows/benchmark.yml` (`--check` line beside `generate-large-fixture.py --check` `:44`)
- Test: `tests/unit/test_document_formats.cpp` is NOT edited; add `tests/unit/test_strip_fixtures.cpp`

**Interfaces:**
- Produces: `python scripts/generate-strip-fixtures.py [--check] [--pdf-pages N --pdf-out PATH]`. Default writes the three fixtures; `--check` regenerates to a temp dir and asserts byte identity with the committed files (as `generate-large-fixture.py` does: `rl_config.invariant = 1`, `pageCompression=0`, white page fill, reportlab 4.4.10); `--pdf-pages 10000 --pdf-out <temp>` writes the 10,000-page PDF for the recorded measurement and is never committed.
  - PDF: 1000 Letter pages, one heading line each (`"Strip page %d of %d."`), pages with 0-based index `i % 10 == 9` (9, 19, …) landscape (792×612) so widths differ.
  - EPUB: 200 chapters, each one `<h1>` and 10 paragraphs, every paragraph exactly this sentence: `Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore et dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea commodo consequat.` (a scratch EPUB of this shape opened as 400 pages in the current `litepdf-cli`, so each chapter spills to a second page); written with `zipfile`, **every entry `ZIP_STORED`** (deflate would make `--check` depend on the interpreter's zlib, the trap `generate-large-fixture.py`'s docstring records), `mimetype` first, then `META-INF/container.xml`, `OEBPS/content.opf`, `OEBPS/ch001.xhtml`…; every `ZipInfo.date_time = (2026,1,1,0,0,0)`.
  - CBZ: 300 PNGs, 200×300 grayscale (`"L"`), each a flat fill with its index drawn by Pillow's default font, named `001.png`…; every entry `ZIP_STORED`, same fixed `date_time`. The PNG bytes come from Pillow's own bundled zlib, so they are reproducible only with the pinned Pillow version (Step 5).
- Produces: `CliOptions::bench_page_sizes` (bool). `parse_cli_args` accepts `--bench-page-sizes`; with it, `--iterations N` is allowed (default 9), and `--benchmark`, `--render`, `--json`, `--bench-selection` and `--page` are rejected with `"--bench-page-sizes excludes --benchmark, --render, --json, --bench-selection and --page"`.
- Produces: `litepdf-cli <file> --bench-page-sizes [--iterations N]` — per iteration: open a fresh `Document` from the path, build a `DocumentView` on it with an `app::InlineDispatcher` (`src/app/SearchDispatcher.hpp`), and time only the `page_sizes()` call (cold cache every time); print `page_sizes: pages=<n> median_ms=<x> max_ms=<y>`.

- [ ] **Step 1: Write the generator** and run it: `python scripts/generate-strip-fixtures.py`, then `python scripts/generate-strip-fixtures.py --check` → exit 0.
- [ ] **Step 2: Write the failing fixture test** `StripFixtures: the strip fixtures open with the expected page counts` — `strip-1000.pdf` has 1000 pages and page 9 is 792 wide; `strip-chapters.epub` has more than 200 pages; `strip-images.cbz` has 300 pages. Run `"StripFixtures: *"` → fails before the files exist, passes after Step 1 (commit order: generator and fixtures together, test after; run the test now to see it pass).
- [ ] **Step 3: Implement `--bench-page-sizes`.** First write `tests/unit/test_cli_args_page_sizes.cpp` (registered): `CliArgs: bench-page-sizes takes iterations` (`{exe, f, "--bench-page-sizes", "--iterations", "3"}` → no error, `bench_page_sizes`, `iterations == 3`; without `--iterations` → 9) and `CliArgs: bench-page-sizes excludes the other modes` (each of `--benchmark`, `--json`, `--render 0`, `--bench-selection`, `--page 1` beside it → the error string above). The exclusion check must come before `parse_cli_args`'s `if (out.bench_selection) { … return std::nullopt; }` block (`cli_args.cpp:60-67`), or `--bench-selection` beside it would parse. Run `"CliArgs: bench-page-sizes*"` → fail; implement in `cli_args.cpp` and `main.cpp`; run → pass; run the existing `test_cli_args.cpp` cases → still pass.
- [ ] **Step 4: Measure the gate** — build everything, then from PowerShell at the repo root:
  - `build\Release\litepdf-cli.exe tests\fixtures\strip-1000.pdf --bench-page-sizes --iterations 9` → `median_ms <= 16`
  - `build\Release\litepdf-cli.exe tests\fixtures\strip-chapters.epub --bench-page-sizes --iterations 9` → `median_ms <= 100`
  - `build\Release\litepdf-cli.exe tests\fixtures\strip-images.cbz --bench-page-sizes --iterations 9` → `median_ms <= 100`
  - `python scripts/generate-strip-fixtures.py --pdf-pages 10000 --pdf-out $env:TEMP\strip-10000.pdf`, then the bench on it → record the median.
  Write the four lines into the PR body. **A miss is not a fix-in-place:** stop and report it to the controller — the spec (§4.4) then disables the strip for that format and files a follow-up issue, which changes PR 2's scope. The 10,000-page figure above 100 ms files a follow-up issue (§10) but does not block.
- [ ] **Step 5: Wire the CI check** — add `python scripts/generate-strip-fixtures.py --check` to `benchmark.yml` beside the large-fixture check (Pillow is required there: add it to that job's pinned `pip install` line with the version printed by `python -c "import PIL; print(PIL.__version__)"`, 12.2.0 on the dev box).
- [ ] **Step 6: Commit** — `test: strip fixtures and the page-sizing bench (#55)`.

## Task 11: The in-process scheduling gate

**Files:**
- Create: `tests/unit/test_strip_schedule_engine.cpp`

**Interfaces:**
- Consumes: Task 1 (`StripLayout`), Task 2 (`clamp_strip_scroll`), Task 4 (`StripRenderState`, `ScheduleInput`, `PageSlot`), Task 5 (`classify_strip_completion`, `after_bitmap`, `after_null`), Task 7 (`page_sizes`, `request_page_render`, `cancel_background_renders` existing), Task 8 (`renders_started`), Task 10 (`strip-1000.pdf`).

- [ ] **Step 1: Write the test** `StripSchedule: cancellation bounds live work over a 1000-page drag` (§9.4):
  - A `DocumentView` on `strip-1000.pdf`, one worker, `set_zoom_pct(1.0)`, viewport 800×600 @96; `StripLayout` from `page_sizes()` at 1.0.
  - A `Submitter` that takes its seq from its **own** counter before calling `request_page_render(page, priority, cb, flag)` (never from the engine's token id), and whose `cb` captures `page`, that seq and the flag, then pushes `{page, seq, has_pixmap = pixmap != nullptr, cancelled = flag->load()}` into a mutex-guarded queue (dropping the pixmap with its ctx); `cancel_thumbnails()` calls `cancel_thumbnail_renders()`; `released()` does nothing.
  - Hold the single worker on page 0 through `request_page_render` — **not** `request_render_with_prefetch`, which would also queue a P1 for page 1 — with a callback that signals "entered" and blocks on a release future (the `HeldWorker` / `ReleaseOnExit` shape of `test_document_view.cpp:365-396`, copied into this file).
  - Drive 1000 viewports, `scroll_y` from 0 to `total − 600` in equal steps; for each: build `ScheduleInput` (V from `visible`; `visible_by_area` = `derive_current_page`'s page first, then the rest by visible height; P from `prefetch_range(…, StripTravel::Down)`; retain V ± one viewport; `bytes_of` = `litepdf::core::estimated_pixmap_bytes(w_pt, h_pt, scale)` from `core/detail/PrefetchPlan.hpp`), `run_pass`, then assert `live_queued <= |V| + |P| + 1`, where `live_queued` counts Pending slots whose flag reads false.
  - At step 500: call `cancel_background_renders()` and run one pass on the same viewport; assert no slot is Pending with its flag set.
  - Release the worker; on the test thread, drain the completion queue through `classify_strip_completion` / `after_bitmap(Ok)` / `after_null`, running a pass after each rule-4 action, until no slot in the final V is Pending (bounded wait, 30 s).
  - Assert `renders_started() <= |final V| + |final P| + 1` (the 1 is the held render of page 0) and every final-V slot is Ready.
- [ ] **Step 2: Run it** — `"StripSchedule: cancellation bounds*"` → pass.
- [ ] **Step 3: Mutation check, cancel (do not commit).** Follow the `mutation-test-hygiene` skill: in `StripSchedule.hpp`, make `run_pass` skip storing `true` to cancelled flags; rebuild; the test must FAIL at the final `renders_started()` bound (expect hundreds). Restore the file (`git checkout -- src/ui/detail/StripSchedule.hpp`), rebuild, re-run → pass. Record both readings for the PR body.
- [ ] **Step 4: Mutation check, sweep (do not commit).** Make `plan_strip_pass` return an empty `sweep`; rebuild; the step-500 assertion must FAIL. Restore, rebuild, re-run → pass. Record both readings.
- [ ] **Step 5: Full suite** — `& $ctest --test-dir build -C Release` → all pass; read the new count with `-N`.
- [ ] **Step 6: Commit** — `test(ui): gate strip scheduling on bounded live work (#55)`.
