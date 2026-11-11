# On-screen view rotation (#53) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The reader turns the whole document by 90° steps in the active tab (View > Rotate Clockwise `Ctrl+Shift+=` / Counterclockwise `Ctrl+Shift+-`), in single page, spread and strip, with fit modes, hits, selection and presses correct in the turned view.

**Architecture:** Bitmaps are rendered unrotated and drawn through a D2D world transform built from one pure page↔view function (`src/core/detail/ViewRotation.hpp`). `DocumentView` holds a per-tab `Rotation`, keeps its size caches unrotated and reports view-frame sizes. The canvas maps every page through that page's **drawn rect** (R3) for paint, overlays and presses; `PdfCanvas::rotate(bool cw)` owns the canvas-side landing; `KickReason::Rotate` rebuilds the strip.

**Tech Stack:** C++17 (MSVC `/W4`), Direct2D, Win32, MuPDF, Catch2, CMake/ctest from VS 2022 BuildTools, PowerShell 5.1 smoke scripts, reportlab fixture generator.

**Spec:** `docs/superpowers/specs/2026-11-11-view-rotation-53-design.md` (gated at `aa8b96e`). Read §3 and §5 before any task; § numbers below refer to it.

## Global Constraints

- Branch `claude/issue-53-3ba59c`, worktree `C:\Users\User\projects\litepdf\.claude\worktrees\issue-218-48da9e`, base `origin/main` @ `82f56a0`. Line numbers below are for `82f56a0`; find code by name if a line moved.
- **Build Release only, never Debug.** `$cmake` / `$ctest` are the BuildTools binaries under `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\`, never the ones on PATH. Unit tests: `& $cmake --build build --target litepdf_unit_tests --config Release`. App (smoke): `--target litepdf`. `third_party\mupdf` is already a junction in this worktree.
- **Run tests from PowerShell, from the worktree root** (fixtures resolve from there): `build\tests\Release\litepdf_unit_tests.exe "<tag or exact name>"`. Full suite: `& $ctest --test-dir build -C Release`, **serially, no `-j`**. Read the count with `-N`; never hard-code it.
- New `TEST_CASE` names are ASCII and prefixed by subsystem: `ViewRotation: `, `DocumentView rotation: `, `EffectiveLayout: `, `PdfCanvas rotation: `. Tags include `[rotation]`.
- New test files register in `tests/CMakeLists.txt` `target_sources(litepdf_unit_tests PRIVATE ...)`.
- No `SetTransform` outside the per-page blocks of §3.4; the render target is identity everywhere else.
- `RenderEngine`, `PageCache`, `StripSchedule`, `ThumbnailRenderer`, `SessionState`, `src/printing/` do not change. The session schema stays v2.
- Every existing test passes **unchanged**, in particular `tests/unit/test_canvas_spread_wiring.cpp` after Task 1 and `test_document_view_strip.cpp:184-186` ("same vector").
- Mutation steps follow the `mutation-test-hygiene` skill: apply, rebuild, see the named case fail, restore, rebuild, see it pass, `git diff` clean of the mutant. Each task records its kills in its commit body (`Mutants: N/N killed`), for Task 9's table.
- `*.ps1` must run on Windows PowerShell 5.1: no `?.`, `??`, ternary (`reference_litepdf_powershell_51_only`). Smoke rows follow `reference_litepdf_smoke_script_rules` (deadline polls via `Wait-UxProbeState`, parse-once step shape, each row proven able to fail).
- No `VERSION` change. One CHANGELOG line under `## [Unreleased]` (Task 8).
- Commits: conventional, English, ending with `Co-Authored-By: <the model that wrote the commit> <noreply@anthropic.com>`. `git add` files by name (the junctioned `third_party/mupdf` breaks `git commit -a`). Do not push.

## Spec corrections carried by this plan

Found while planning; the spec is patched in the same commit as this plan.

1. **§5.4 case 8 angle.** At 90° a page point maps to view `(H − y, x)`: the flipped axis carries the **height** slack, `842 − 841.89 = 0.11` DIP for A4 at zoom 1.0, which a 0.5 px tolerance cannot see. The width slack (`596 − 595.276 = 0.72` DIP) is on the flipped axis at **270°** (view `(y, W − x)`). Case 8 therefore turns counter-clockwise once and asserts exact drawn-rect extents plus an interior press, not a painted edge column (Task 6).
2. **§5.4 fixture: the word moves to page 3 of four.** With the word on page 2, the direction row's precondition "page 2 is not in view" reads page 2's top at `8 + 595.276·z + 8` ≈ `16 + 0.71·client_w` DIP after the turn, which a window of ordinary proportions can show. With the word on the third page (index 2) its top is ≈ `24 + 1.41·client_w`, below any plausible client height, and a fourth blank page keeps the centring unclamped. Fixture: page 1 square + text, page 2 blank, page 3 the word, page 4 blank.
3. **§5.5 range row fixture.** The range row uses `rotation-marker.pdf` (four pages), the same launch as the direction row, so both rows share one launch and one fixture (Task 8). (`simple.pdf` would also work: `SIF_DISABLENOSCROLL`, `PdfCanvasStrip.cpp:574-579`, keeps the bar and its range while the strip fits.)

## Review Focus

1. **A document with mixed portrait and landscape pages in the strip.** After a turn each page's layout height is its own width × zoom, not one shared size. Pinned in Task 4 (`spread-unequal.pdf`, whose pages differ in width and height, every `layout().height(i)` and `width(i)`).
2. **A page whose size cannot be read** (`damaged-pages.pdf`). Its stand-in in `page_sizes()` is turned with the rest, or the strip lays that page out in the wrong frame. Pinned in Task 3.
3. **A selection made before the turn.** It stays in page space, still copies the same text, and is painted over the turned text. Pinned in Task 5.
4. **A window resize after the turn at fit width.** The refit reads the turned size, not the cached unturned one. Pinned in Task 3 (`set_viewport` with a new width after `rotate_cw`).
5. **A failed page in the turned strip.** It has no bitmap, so its drawn rect is the turned layout rect and the placeholder border sits in that rect. Pinned in Task 6 (`damaged-pages.pdf`).

---

## Task 1: Shared canvas rig

Spec §5.0. Pure move, no behaviour change.

**Files:**
- Create: `tests/unit/CanvasRig.hpp`
- Modify: `tests/unit/test_canvas_spread_wiring.cpp:17-366` (the includes, `PdfCanvasTestAccess`, `open_fixture`, `Layout`, `Fail`, `too_tall_for_a_bitmap`, `Rig`, `kWidth`/`kHeight`, the fixture path constants, the helpers at `:306-362`, `kWhite`/`kChrome`)
- Modify: `src/ui/PdfCanvas.hpp:377` and `src/ui/PdfCanvasImpl.hpp:4-6` (both comments name the test file; point them at the new header)

**Interfaces:**
- Produces: `tests/unit/CanvasRig.hpp`, `#pragma once`, containing `litepdf::ui::PdfCanvasTestAccess` (every existing member, unchanged) and, in a **named** namespace `canvas_rig` (not an unnamed one: a helper with internal linkage that one of the two files does not call raises C4505 under `/W4`), as `inline` functions and `inline constexpr` constants, `Access`, `kWidth`, `kHeight`, `kPairs`, `kDamaged`, `kLarge`, `open_fixture`, `Layout`, `Fail`, `too_tall_for_a_bitmap`, `Rig`, `kWhite`, `kChrome`. Helpers used by spread tests only (`hit_at`, `centred_pan`, `page_dip`, `bottom_pan`, `overlay_has`, `enter_spread`, `leave_spread`, `enter_spread_from`, `hits_on_every_page`) also move, since Task 4 and Task 6 use them.

- [ ] **Step 1: Move the block into `CanvasRig.hpp`** with the includes it needs; `test_canvas_spread_wiring.cpp` keeps its file comment, includes `"CanvasRig.hpp"` and adds `using namespace canvas_rig;`. The build must add no new warning.
- [ ] **Step 2: Build and run the spread-wiring cases**

Run: `build\tests\Release\litepdf_unit_tests.exe -# "[#test_canvas_spread_wiring]"` (every case in the file, by its file-name tag).
Expected: the same case count as before the move (read it first with `--list-tests` on the same filter), all PASS.

- [ ] **Step 3: Commit**

```bash
git add tests/unit/CanvasRig.hpp tests/unit/test_canvas_spread_wiring.cpp src/ui/PdfCanvas.hpp src/ui/PdfCanvasImpl.hpp
git commit -m "test(canvas): move the canvas rig into a shared header (#53)"
```

---

## Task 2: The pure transform

Spec §3.2, §5.1.

**Files:**
- Create: `src/core/detail/ViewRotation.hpp`
- Create: `tests/unit/test_view_rotation.cpp`
- Modify: `tests/CMakeLists.txt` (register the test)

**Interfaces:**
- Produces, `namespace litepdf::core`, no Win32 / D2D / MuPDF includes:

```cpp
struct Rotation {
    int quarters = 0;                                    // 0..3, clockwise
    constexpr Rotation cw() const noexcept;              // (q + 1) & 3
    constexpr Rotation ccw() const noexcept;             // (q + 3) & 3
    constexpr bool swaps_axes() const noexcept;          // q == 1 || q == 3
    friend constexpr bool operator==(Rotation, Rotation) noexcept;
    friend constexpr bool operator!=(Rotation, Rotation) noexcept;
};
struct ViewSize  { float w; float h; };
struct ViewPoint { float x; float y; };
struct ViewRect  { float x; float y; float w; float h; };
// Same layout and row-vector convention as D2D1_MATRIX_3X2_F:
// x' = x*m11 + y*m21 + dx,  y' = x*m12 + y*m22 + dy.
struct Affine    { float m11, m12, m21, m22, dx, dy; };

constexpr ViewSize  view_size(float w, float h, Rotation r) noexcept;
constexpr Affine    draw_affine(Rotation r, ViewRect view) noexcept;
constexpr ViewPoint local_to_view(ViewPoint local, Rotation r, ViewRect view) noexcept;
constexpr ViewPoint view_to_local(ViewPoint v, Rotation r, ViewRect view) noexcept;
constexpr ViewRect  local_rect_to_view(ViewRect local, Rotation r, ViewRect view) noexcept;
```

`view` is the drawn rect in the view frame; the local frame is `[0, u.w] x [0, u.h]` with `u = view_size(view.w, view.h, r)`. With `W = view.w`, `H = view.h`, the map (the corner tests determine it; this is the table to implement):

| quarters | view point of local `(x, y)` | `m11 m12 m21 m22` | `dx, dy` |
|---|---|---|---|
| 0 | `(vx + x, vy + y)` | `1 0 0 1` | `vx, vy` |
| 1 | `(vx + W − y, vy + x)` | `0 1 −1 0` | `vx + W, vy` |
| 2 | `(vx + W − x, vy + H − y)` | `−1 0 0 −1` | `vx + W, vy + H` |
| 3 | `(vx + y, vy + H − x)` | `0 −1 1 0` | `vx, vy + H` |

`local_to_view` applies `draw_affine`; `view_to_local` is its exact inverse; `local_rect_to_view` maps two opposite corners and returns their bounding box.

- [ ] **Step 1: Write the failing tests** in `test_view_rotation.cpp`, tags `[rotation][view_rotation]`:
  - `ViewRotation: cw and ccw wrap` — `Rotation{3}.cw() == Rotation{0}`, `Rotation{0}.ccw() == Rotation{3}`; `swaps_axes` true only at 1 and 3; `view_size(100, 200, {1})` is `{200, 100}`, at `{2}` `{100, 200}`.
  - `ViewRotation: rotation 0 is a translation` — `draw_affine({0}, {10, 20, 30, 40})` is exactly `{1, 0, 0, 1, 10, 20}`.
  - `ViewRotation: local corners land on the named view corners` — `view = {10, 20, 300, 100}` with the local frame `100 x 300` at 1/3 and `300 x 100` at 0/2. Local top-left `(0, 0)` lands at view top-right `(310, 20)` at 1, bottom-right `(310, 120)` at 2, bottom-left `(10, 120)` at 3; check all four corners at every rotation, through a hand-written `apply(Affine, ViewPoint)` in the test (no D2D).
  - `ViewRotation: an interior point maps to absolute view coordinates` — local `(7, 3)` in `view = {0, 0, 50, 80}`: 0 → `(7, 3)`; 1 (local `80 x 50`) → `(47, 7)`; 2 → `(43, 77)`; 3 → `(3, 73)`. Then `view_to_local(local_to_view(p)) == p` at every rotation.
  - `ViewRotation: rounding slack sits on the near edge after a turn` — an 842 pt page at render scale 1.25: `view = {0, 0, 1053, w}` at 1 (`w` any width), page point `(0, 842)` → local `(0, 1052.5)` → view `x == Approx(0.5)`, not 0.
  - `ViewRotation: a local rect maps to its view bounding box` — local `{10, 20, 30, 5}` in `view = {0, 0, 50, 80}`: 0 → `{10, 20, 30, 5}`; 1 → `{25, 10, 5, 30}`; 2 → `{10, 55, 30, 5}`; 3 → `{20, 40, 5, 30}`.
  - `ViewRotation: a hit's vertical extent in the view frame` — local rect `{x0, y0, qw, qh}` on a page `W x H` (view rect `{0, 0, view_size(W, H, r)}`): view top is `y0` at 0, `x0` at 1, `H − y0 − qh` at 2, `W − x0 − qw` at 3.
- [ ] **Step 2: Run, verify the build fails** (`ViewRotation.hpp` missing): `& $cmake --build build --target litepdf_unit_tests --config Release`.
- [ ] **Step 3: Implement `ViewRotation.hpp`** per the Interfaces block. File comment: the `ui/detail/ViewportMath.hpp:3-5` "no Win32" convention, and why it lives in `core/detail` (§3.2).
- [ ] **Step 4: Run** `build\tests\Release\litepdf_unit_tests.exe "[view_rotation]"` → all PASS.
- [ ] **Step 5: Mutants** (each must turn a named case red): swap `dx` of quarter 1 to `vx` (corner case); make `view_to_local` apply the forward map (round-trip case); return the un-swapped `w, h` from `local_rect_to_view` at 1 (rect case).
- [ ] **Step 6: Commit** `feat(core): pure page-to-view rotation transform (#53)` with `src/core/detail/ViewRotation.hpp tests/unit/test_view_rotation.cpp tests/CMakeLists.txt`.

---

## Task 3: Per-tab rotation in `DocumentView`

Spec §3.3, §5.2. Review Focus 2 and 4.

**Files:**
- Modify: `src/core/DocumentView.hpp:114-126` (doc comments of `page_sizes()` / `known_page_size()`; new members), include `core/detail/ViewRotation.hpp`
- Modify: `src/core/DocumentView.cpp:100-128` (`Impl`), `:322-345` (`page_sizes`, `known_page_size`), `:357-386` (`set_viewport`)
- Modify: `docs/superpowers/specs/2026-10-22-continuous-scroll-design.md` §4.4 (one sentence: "the reported frame follows the view rotation, #53 spec §3.3")
- Test: `tests/unit/test_document_view.cpp`, `tests/unit/test_document_view_strip.cpp`

**Interfaces:**
- Consumes: `Rotation`, `view_size` (Task 2).
- Produces on `DocumentView`:

```cpp
[[nodiscard]] Rotation rotation() const noexcept;   // default {0}
void rotate_cw() noexcept;                          // no cancel, no cache touch
void rotate_ccw() noexcept;
[[nodiscard]] bool strip_sized() const noexcept;    // test seam: the full sizing pass has run
```

  `known_page_size(p)` returns `view_size` of the cached size. `page_sizes()` returns `strip_sizes` itself whenever `!rotation().swaps_axes()`; otherwise a second `Impl` vector of swapped sizes, rebuilt lazily inside `page_sizes()` on its first call after the axes changed (a flag `rotate_*` clears), never inside `rotate_*`. `set_viewport` passes `view_size(...)` of `one` and `two` to `fit_percentage`. Internal users (`estimated_bytes`, prefetch plan, CBZ read-ahead, `PageSizeFill`) keep `known_size` / `strip_sizes`.

- [ ] **Step 1: Write the failing tests**, tags `[core][view][rotation]`:
  - `DocumentView rotation: known_page_size swaps at 90 and 270, not 180` — on `simple.pdf` (595.22 x 842): 1 turn `{842, 595.22}`, 2 turns `{595.22, 842}`, 3 turns swapped, 4 turns identity; `rotate_ccw` from 0 gives swapped.
  - `DocumentView rotation: page_sizes swaps every page` (in `test_document_view_strip.cpp`) — after `rotate_cw`, each `page_sizes()[i]` equals the swapped `document().page_size(i)`; at rotation 0 and 2 `&page_sizes() ==` the vector from before the turn.
  - `DocumentView rotation: a stand-in size turns with the rest` — on `damaged-pages.pdf`, the unreadable page's entry after `rotate_cw` is the swapped stand-in (`page_sizes()` at 0 gives the stand-in `s`; at 1 it is `{s.height_pt, s.width_pt}`). (Review Focus 2.)
  - `DocumentView rotation: a turn does not run the full sizing pass` — fresh view, paged, `strip_sized()` false; `rotate_cw()` → still false; `page_sizes()` → true.
  - `DocumentView rotation: fit width refits from the turned size` — `set_viewport(1000, 800, 96)` at FitWidth on `simple.pdf`: zoom `1000/595.22`; `rotate_cw()` then `set_viewport(1000, 800, 96)`: `1000/842`. Custom zoom 1.5: unchanged by the turn and the `set_viewport`.
  - `DocumentView rotation: a resize after the turn fits the turned page` — after `rotate_cw()` and `set_viewport(1000, 800, 96)`, `set_viewport(1200, 800, 96)` gives `1200/842`. (Review Focus 4.)
  - `DocumentView rotation: a turn at custom zoom starts no render` (R1 guard) — a view with the default capacities on the eight-page fixture, custom zoom 1.0, `request_render_with_prefetch(1, ...)`. **Settled** means: `queued_render_count() == 0` and `renders_started()` unchanged across 300 ms (10 s deadline, 10 ms poll, as `test_document_view.cpp:657-663`); this covers renders in flight, which the queue count does not. Wait until settled; baseline `renders_started()`; `rotate_cw()`, `request_render_with_prefetch(1, ...)`, wait for its callback and until settled; `renders_started()` equals the baseline.
- [ ] **Step 2: Run, verify they fail** (missing members): build.
- [ ] **Step 3: Implement** per the Interfaces block. Amend the `page_sizes()` comment ("sized once and kept for the life of the view"): the sizing is still once; the reported frame follows `rotation()`. Amend `known_page_size()`'s comment the same way. Add the continuous-scroll spec sentence.
- [ ] **Step 4: Run** `"[rotation]"` and `"[core][view]"` → all PASS, including `DocumentView: page_sizes returns one size per page` unchanged.
- [ ] **Step 5: Mutants:** `known_page_size` returns the unswapped size (swap case); `page_sizes()` builds the swapped vector inside `rotate_cw` (sizing-pass case); `set_viewport` fits the unswapped size (fit case).
- [ ] **Step 6: Commit** `feat(core): per-tab view rotation in DocumentView (#53)`.

---

## Task 4: `PdfCanvas::rotate`, `KickReason::Rotate` and the landing

Spec §3.5 "Landing", "KickReason::Rotate", §5.3, §5.4 cases 10-16. Review Focus 1. This task also creates the fixture and the rotation test file.

**Files:**
- Create: `scripts/generate-rotation-fixture.py`, `tests/fixtures/rotation-marker.pdf`
- Create: `tests/unit/test_canvas_rotation.cpp` (register in `tests/CMakeLists.txt`)
- Modify: `tests/unit/CanvasRig.hpp` (accessors and rig helpers below)
- Modify: `src/ui/detail/EffectiveLayout.hpp:84-139` (`KickReason::Rotate` and its row)
- Modify: `tests/unit/test_effective_layout.cpp` (the Rotate row)
- Modify: `src/ui/PdfCanvas.hpp` (public `rotate`), `src/ui/PdfCanvas.cpp` (definition, next to `set_pending_anchor` at `:383`)
- Modify: `.github/workflows/benchmark.yml:46-51` (add `python scripts/generate-rotation-fixture.py --check` and its own `if ($LASTEXITCODE -ne 0) { throw "rotation-fixture determinism check failed (exit $LASTEXITCODE)" }` line after the strip check; without the throw line a failure is masked unless it is the block's last command, as the step's comment at `:41-45` says)

**Interfaces:**
- Consumes: `DocumentView::rotate_cw/ccw`, `rotation()`, `page_sizes()` (Task 3).
- Produces:
  - `KickReason::Rotate` → `strip_kick_actions` `{refit, refresh_readout, relayout}` (restore, restyle false; reschedule true).
  - `void PdfCanvas::rotate(bool cw);` public. Sequence, exactly §3.5 steps 1-5: `view->rotate_cw()` / `rotate_ccw()`; `cancel_gesture()`; paged/spread: `pan_x = pan_y = 0`, `set_pending_anchor(PageAnchor::top())`; strip: `impl_->strip_view.reset()` only; `InvalidateRect`. No-op without a view.
  - Fixture constants, in the generator's docstring and in `CanvasRig.hpp`: `kMarker = "tests/fixtures/rotation-marker.pdf"`; four A4 portrait pages (595.276 x 841.89 pt; spec correction 2). Page index 0: black square, page space `[36, 108] x [36, 108]`, and the line `Rotation marker text` (Helvetica 12, baseline 60 pt from the top, starting at x = 150). Index 1: blank. Index 2: the single word `ZENITH` (Helvetica 12, baseline 80 pt from the top, starting at x = 470). Index 3: blank. `kMarkWordPage = 2`; `kMarkWordCentreXPt`: the measured centre x of that hit (Step 1).
  - `CanvasRig.hpp` additions: `Access::anchor_kind(const PdfCanvas&) -> PageAnchor::Kind` (the slot's kind, `None` when empty; an applied anchor keeps its kind until the next `stamp()`, `PageAnchor.hpp:117-123`), `Access::anchor_applied(const PdfCanvas&) -> bool` (the slot's applied mark), `Access::held_place(const PdfCanvas&) -> bool`, `Access::gesture_live(const PdfCanvas&) -> bool`, `Access::left_down(PdfCanvas&, int x, int y)` / `mouse_move` / `left_up` (the canvas's `on_left_button_down(false, x, y)` etc.; `left_down` ends with `REQUIRE(GetCapture() == c.hwnd())` whenever a gesture began, so every press test is diagnosable: without the capture `cancel_stale_gesture`, `PdfCanvas.cpp:812-823`, ends the drag on the first move. A hidden non-foreground window did take the capture when measured on 2026-11-12), `Access::strip_layout(const PdfCanvas&) -> const StripLayout&`, `Access::strip_scroll_y(const PdfCanvas&) -> double`, `Access::strip_page_rect(const PdfCanvas&, int page) -> StripPageRect`, constant `kUnequal = "tests/fixtures/spread-unequal.pdf"`. `marker_hit(DocumentView&) -> SearchSession::Hit` (`{kMarkWordPage, document().page_hits(kMarkWordPage, "ZENITH", {})[0]}`). `Rig::enter_strip()` (`set_dual_page(false)`, `set_continuous(true)`, `switch_layout(LayoutKind::Strip, true)`, then `strip_kick(KickReason::Layout)`), `Rig::strip_kick(KickReason r)` (MainWindow's strip branch: `apply_viewport()` when `refit`, then `canvas.strip_kick(strip_kick_actions(r), nullptr)`), `Rig::land_strip(int page)` (dispatch `WM_USER_STRIP_DONE` until the page's slot is Ready and not stale; 20 s deadline like `land`), `Rig::land_strip_failed(int page)` (the same, until the slot is Failed), `Rig::rotate(bool cw)` (the command handler: `canvas.rotate(cw)`, then the strip kick with `Rotate` in the strip, else `kick(page)` with the spread's left page).

- [ ] **Step 1: Generator and fixture.** `scripts/generate-rotation-fixture.py` in the shape of `generate-selection-fixture.py` (docstring per page, `rl_config.invariant = 1`, `pageCompression=0`, `--check`). Generate; run `--check` twice → exit 0 (byte identity). Then write `PdfCanvas rotation: the fixture's word sits where the smoke row expects`: `page_hits(kMarkWordPage, "ZENITH", {})` has exactly one hit; record its quad's centre x as `kMarkWordCentreXPt` (rounded to 0.1 pt) and assert `Approx(kMarkWordCentreXPt).margin(0.5)`. The smoke direction row (Task 8) carries the same constant.
- [ ] **Step 2: Write the failing tests**, tags `[canvas][rotation]`. The rig has never landed strip completions or read strip pixels before, so the first case written is a plain one that proves `enter_strip` + `land_strip` work at rotation 0 (`PdfCanvas rotation: the rig lands a strip page`: `kMarker`, strip, custom zoom 1.0, `land_strip(0)`, the square's centre `(r.x + 72, r.y + 72)` is dark); it passes before any production change and stays as the rig's own check.
  - `EffectiveLayout: a rotation refits, refreshes the readout and relays out` — `matches({KickReason::Rotate, true, true, false, true, false, true})`.
  - Case 10 `PdfCanvas rotation: the strip at custom zoom relays out turned pages` — `kMarker`, `enter_strip()`, zoom 1.0, `canvas.strip_navigate(1)`, land; `rig.rotate(true)`: `Access::strip_layout(c).height(0) == Approx(595.276)`, `view.current_page() == 1`, `Access::strip_scroll_y(c) == Approx(strip_layout(c).anchor(1))`.
  - Review Focus 1 `PdfCanvas rotation: every strip page takes its own turned height` — `kUnequal = "tests/fixtures/spread-unequal.pdf"` (not `kPairs`: every `spread-pairs.pdf` page is 420 pt wide, so one shared turned size would pass), strip at custom zoom 1.0. First `REQUIRE` two pages whose `document().page_size(i).width_pt` differ. `rig.rotate(true)`: for each page `i`, `height(i) == Approx(page_size(i).width_pt)` and `width(i) == Approx(page_size(i).height_pt)`.
  - Case 11 `PdfCanvas rotation: the strip at fit width keeps the page and refits` — `kLarge`, strip at FitWidth, `strip_navigate(30)`; `rig.rotate(true)`: current page 30, `strip_layout(c).zoom_pct() == Approx(view.zoom_pct())`, and that zoom is the zoom before the turn × `612 / 792` (Letter, width over turned width) within `1e-3` relative.
  - Case 12 `PdfCanvas rotation: a turn ends the strip's held place` — `kLarge`, custom zoom 1.0, `enter_spread_from(rig, 5, 300.0)` (sets `held_place`); `rig.rotate(true)`; `Access::held_place(c)` false, `anchor_kind == Top`; land both; `leave_spread(rig)`: `strip_position` is `{5, 0.0}`, not offset 300.
  - Case 13 `PdfCanvas rotation: a turn returns a panned single page to its top-left` — `kMarker`, Single, custom zoom 3.0 (the page overflows 1000 x 850 both ways in both frames), land; `pan_by(-200, -300)`, assert both pans nonzero; `rig.rotate(true)`, land: `pan_x() == 0` and `pan_y() == 0`.
  - Case 14 `PdfCanvas rotation: at fit width the Top anchor lands with the new scale` — `Rig(1600, 850, Layout::Single, kMarker)` (wide, so the turned page still overflows at fit width: `595.276 · 1600/841.89 ≈ 1131 > 850`; in the default 1000-wide rig it is 707 tall and `pan_y == 0` would hold by the clamp alone), FitWidth, land, `pan_by(0, -200)`; `rig.rotate(true)`: before landing `anchor_kind == Top` and `!anchor_applied`; land the completion: `pan_y() == 0` and `anchor_applied`.
  - Case 15 `PdfCanvas rotation: a panned spread lands at the pair's top` — `kPairs`, spread, custom zoom **3.0** (every page is 420 pt wide, so the turned pair is `420 · 3 = 1260` DIP tall and still overflows the 850 canvas; at 2.0 it is 840 and `pan_y == 0` would hold by the clamp alone), `kick(1)` (pair 1-2), land both, `pan_by(0, -400)`; `rig.rotate(true)`, land both: `REQUIRE(Access::content_extent(c).h > kHeight)`, then `pan_y() == 0`.
  - Case 16 `PdfCanvas rotation: a turn during a drag ends the gesture` — `kMarker`, Single, land; `left_down` on page 0's text line (its capture `REQUIRE` runs here), `mouse_move` 40 px right; `gesture_live` true; `canvas.rotate(true)`: `gesture_live` false.
- [ ] **Step 3: Run, verify they fail** (no `KickReason::Rotate`, no `rotate`).
- [ ] **Step 4: Implement** `KickReason::Rotate` (comment: the layout is reset by `PdfCanvas::rotate`, so `strip_kick`'s `built_here` branch builds from the turned sizes and navigates, `PdfCanvasStrip.cpp:490-502`) and `PdfCanvas::rotate` per Interfaces, with the §3.5 step-4 comment on why the strip clears no held page.
- [ ] **Step 5: Run** `"[rotation]"`, `"[effective_layout]"` → PASS.
- [ ] **Step 6: Mutants:** drop `pan_x = 0` (case 13); drop the Top anchor install (case 14's pre-landing `Top` and `anchor_applied`; case 12); drop the paged `pan_y = 0` **and** the Top anchor together (case 15); drop `strip_view.reset()` (case 10); `KickReason::Rotate` without `refit` (case 11). The `cw`/`ccw` swap inside `rotate` cannot be seen here (90° and 270° give the same sizes); Task 5 case 1 kills it.
- [ ] **Step 7: Commit** `feat(canvas): rotate command lands at the page top (#53)` with the generator, fixture, tests, `EffectiveLayout.hpp`, `PdfCanvas.hpp/.cpp`, `CanvasRig.hpp`, `benchmark.yml`, `tests/CMakeLists.txt`.

---

## Task 5: Paged drawing and presses (single page, spread)

Spec §3.4 (`view_px`, drawn rect, per-page transform, spread clip), §3.5 "Presses" for the paged branches, §5.4 cases 1-4, 6, 7 (paged), 9, 17. Review Focus 3.

**Files:**
- Modify: `src/ui/PdfCanvasImpl.hpp` (two helpers), `src/ui/PdfCanvas.hpp` (private `view_rotation`)
- Modify: `src/ui/PdfCanvas.cpp`: `:628` (`shown_page_height`), `:644` (`shown_page_pct`), `:652` (`single_page_placement`), `:1670` (`content_extent`), `:2308` (spread `slot_placement`), `:2340-2373` (`draw_slot`), `:2418-2450` (single-page draw and overlays), `:791-799` (`page_point_at`)
- Modify: `tests/unit/CanvasRig.hpp`, `tests/unit/test_canvas_rotation.cpp`

**Interfaces:**
- Consumes: `draw_affine`, `view_to_local`, `view_size` (Task 2); `DocumentView::rotation()` (Task 3); `Rig::rotate`, `kMarker` (Task 4).
- Produces:

```cpp
// PdfCanvasImpl.hpp, namespace litepdf::ui
D2D1_SIZE_F       view_px(ID2D1Bitmap& bm, litepdf::core::Rotation r) noexcept; // GetSize(), swapped at 1/3
D2D1_MATRIX_3X2_F to_d2d(const litepdf::core::Affine& a) noexcept;
// PdfCanvas, private
litepdf::core::Rotation view_rotation() const noexcept;   // view ? view->rotation() : {0}
```

  `page_point_at(x, y, page)`: `page` is now the page's drawn rect **in the view frame**. Map the DIP point through `view_to_local(p, rot, {page.x, page.y, page.w, page.h})`, then `canvas_dip_to_page_point(local.x, local.y, Placement{0, 0, u.w, u.h}, pct)` with `u = view_size(page.w, page.h, rot)`. Per page drawn: `SetTransform(to_d2d(draw_affine(rot, rect)))`, `DrawBitmap` into `[0, u.w] x [0, u.h]`, hits with `paint_hit(hit, 0, 0, pct)`, selection (single page only) with `ox = oy = 0`, then `SetTransform(D2D1::Matrix3x2F::Identity())`. The spread's `PushAxisAlignedClip` is pushed before the slot's `SetTransform` and popped after the reset.
  `CanvasRig.hpp` additions: `Access::placement(PdfCanvas&) -> Placement` (`single_page_placement`, REQUIRE true), `Access::content_extent(const PdfCanvas&) -> ContentBox`, `Rig::second_view(const char* path) -> DocumentView&` (a second view on the rig's dispatcher, destroyed in `~Rig` before the canvas), `Rig::kick_view(DocumentView& v, int page)` (the single-page branch of `kick`, rendering from `v`: `kick` itself renders from the rig's own view, so after `set_view(&second)` it would post the first view's pixmap).

- [ ] **Step 1: Write the failing tests** (`kBlack = 0x000000`; "dark" = every channel < 0x40). In each, `pl = Access::placement(c)` after landing, and expected positions are written from the Task 2 table:
  - Case 1 `PdfCanvas rotation: the single page draws turned in each direction` — `kMarker`, Single, custom zoom 1.0, land. The square's local centre is `(72, 72)`. After `rig.rotate(true)` + land: pixel at `(pl.x + pl.w − 72, pl.y + 72)` dark, at `(pl.x + 72, pl.y + 72)` white. Two turns: dark at `(pl.x + pl.w − 72, pl.y + pl.h − 72)`. Three: dark at `(pl.x + 72, pl.y + pl.h − 72)`. Fresh rig, one `rig.rotate(false)`: dark at `(pl.x + 72, pl.y + pl.h − 72)`. Also `pl.w == 842` and `pl.h == 596` after one turn (the bitmap is 596 x 842 px).
  - Case 2 `PdfCanvas rotation: the content extent swaps` — same setup at zoom 3.0: `content_extent` `w`/`h` before the turn equal `h`/`w` after it.
  - Case 3 `PdfCanvas rotation: a press maps to the page point in each rotation` — zoom 1.0, press at view offset `(dx, dy) = (100, 50)` from `(pl.x, pl.y)`, local extent `u = (596, 842)` unturned: expected page point at 0 `(100, 50)`, at 1 `(50, 842 − 100)`, at 2 `(596 − 100, 842 − 50)`, at 3 `(596 − 50, 100)`, via `Access::page_point_at`, `Approx` margin 0.01.
  - Case 4 `PdfCanvas rotation: a hit highlight lands on the turned ink` — hits source returning one hit on page 0 whose quad is the square's bounds; after one turn the pixel at the square's turned centre is neither dark-black (`kBlack`) nor white, and the pixel at the unturned centre `(pl.x + 72, pl.y + 72)` is white.
  - Case 6 `PdfCanvas rotation: a turned spread places each page in its slot` — `kPairs`, spread at FitWidth, pair 1-2, `hits_on_every_page()`, `rig.rotate(true)`, land both. `view.zoom_pct() == Approx(496.0f / 1600.0f)` (turned widths 400 and 1600); `content_extent(c).h == Approx(std::ceil(420.0f * z))`; the right page's hit (local `100..200 x 100..118` pt) is painted at its turned position inside the right slot: the slot starts at `right_x0 = 504`, the turned page is `ceil(1600·z) x ceil(420·z)` centred in the `496 x 850` slot by `place_bitmap`, and the hit's centre is at local `(150, 109)·z`, mapped by the quarter-1 row of the table; that pixel is neither `kWhite` nor `kChrome` (with the clip pushed under the turned transform, D2D boxes the turned clip below the canvas and the slot reads chrome).
  - Case 7 paged `PdfCanvas rotation: a press over a stale-zoom bitmap maps through its drawn rect` — Single, zoom 1.0, land; `set_zoom_pct(2.0)`, kick but **do not land**; `canvas.rotate(true)`; `pl` read now (the zoom-1.0 bitmap, turned: `842 x 596`, integral at these sizes); press at `(pl.x + pl.w − 1, pl.y)`: page point `(0, 1)` within 0.01 (that DIP is local `(0, 1)` at 90°, at the bitmap's own zoom 1.0). A drawn rect from `page × 2.0` would give a point hundreds of points away.
  - Case 9 `PdfCanvas rotation: drag-select on the turned page copies the line` — one turn, zoom 1.0, land; `left_down` / `mouse_move` / `left_up` at the turned images of local `(149, 56)` and `(300, 56)` (y 56 is the line's mid-height: baseline 60, Helvetica 12; x 300 is past the line's end), mapped through the Task 2 table onto `pl`; `view.selection()->text_utf8 == "Rotation marker text"`.
  - Review Focus 3 `PdfCanvas rotation: a selection survives the turn` — select the line at rotation 0; `rig.rotate(true)`, land: `view.selection()` unchanged (`text_utf8`, page, quads count) and a pixel inside the turned first quad is the selection colour, not white.
  - Case 17 `PdfCanvas rotation: rotation belongs to the view` — `second_view(kMarker)`; **both views at custom zoom 1.0** (a fresh view is FitWidth, and the square centre `72` below assumes zoom 1.0); rotate the first, land; `set_view(&second)`, `kick_view(second, 0)`, land: dark at the unturned square centre; `set_view(&view)`, `kick(0)`, land: dark at the turned one.
- [ ] **Step 2: Run, verify they fail** (the page draws unturned).
- [ ] **Step 3: Implement** per Interfaces; the five `GetSize()` readers in `PdfCanvas.cpp` become `view_px` (the sixth, `PdfCanvasStrip.cpp:950`, is Task 6's). Update the overlay comment at `:2423-2428` ("rotated text would need transformed geometry" → overlays draw in the page's local frame under the page's transform).
- [ ] **Step 4: Run** `"[rotation]"` and `-# "[#test_canvas_spread_wiring]"` → PASS (rotation 0 must stay pixel-identical: that whole file, its 19 `PdfCanvas single page:` cases included, is the guard; a `"PdfCanvas spread*"` name filter would skip them).
- [ ] **Step 5: Mutants:** `view_px` never swaps (case 1's `pl.w`); `draw_affine` quarter-1 `dx = vx` (case 1); `page_point_at` skips `view_to_local` (case 3); `rotate(cw)` calls `rotate_ccw` (case 1 — this is the Task 4 direction mutant); spread clip pushed after `SetTransform` (case 6's hit pixel: the turned clip no longer covers the slot).
- [ ] **Step 6: Commit** `feat(canvas): draw single page and spread turned (#53)`.

---

## Task 6: Strip drawing and presses

Spec §3.4 (strip drawn rect, R3), §3.5 "Presses" strip sites, §5.4 cases 7 (strip) and 8. Review Focus 5.

**Files:**
- Modify: `src/ui/PdfCanvas.hpp` (private `strip_drawn_rect`), `src/ui/PdfCanvasStrip.cpp:934-990` (paint loop, hits, selection), `src/ui/PdfCanvas.cpp:898, 973` (strip press sites)
- Modify: `tests/unit/CanvasRig.hpp`, `tests/unit/test_canvas_rotation.cpp`

**Interfaces:**
- Consumes: Task 5's `view_px`, `to_d2d`, `view_rotation`, `page_point_at` semantics.
- Produces:

```cpp
// PdfCanvas, private, defined in PdfCanvasStrip.cpp
Placement strip_drawn_rect(int page) const;
```

  Ready and not `is_stale(*slot, scale, invert)`: `{r.x, r.y, view_px(bm).width / (rt_dpi/96), view_px(bm).height / (rt_dpi/96)}` (via `bitmap_px_to_dip`); otherwise (stale stretch, Pending over a bitmap, no bitmap, Failed) the layout rect `strip_page_placement(sv.page_rect(page))`. The paint loop draws each visible page under its drawn rect's transform: the bitmap (natural or stretched into the local frame), the blank fill and the Failed border in the local frame, then that page's hits and, on its page, the selection, then identity. The press sites call `strip_drawn_rect(page)` instead of `strip_page_placement(page_rect(page))`.
  `CanvasRig.hpp` additions: `Access::strip_drawn_rect(const PdfCanvas&, int page) -> Placement`, `Access::strip_page_point_at(PdfCanvas&, int page, int x, int y) -> SelPoint` (`page_point_at` with `strip_drawn_rect(page)`), `Access::live_anchor(const PdfCanvas&) -> SelPoint` / `live_extent` (the live selection's two points, REQUIRE a live selection).

- [ ] **Step 1: Write the failing tests:**
  - Case 8 `PdfCanvas rotation: a fresh strip page is drawn and pressed at its natural size` — `kMarker`, strip, custom zoom 1.0, land page 0. At rotation 0: `strip_drawn_rect(0)` `w == 596`, `h == 842` (natural, not 595.276). `rig.rotate(false)` (270°, where the width slack is on the flipped axis), land page 0: `w == 842`, `h == 596` exactly; with `r = strip_page_rect(0)`, a press at `(x, y) = (int(r.x) + 200, int(r.y + 596) − 100)` maps to page x `== Approx(r.y + 596 − y).margin(0.05)` (a transform from the layout rect is off by 0.72 DIP); the pixel at the turned square centre `(r.x + 72, r.y + 596 − 72)` is dark. Then, with a hits source returning one hit over the square, that pixel is **not dark** (the 0.40-alpha yellow fill over black, about `(102, 102, 0)`; brush at `PdfCanvas.cpp:1546-1547`).
  - `PdfCanvas rotation: a real press and drag in the turned strip map through the drawn rect` — same setup at 270°, landed. `left_down` at `(x0, y0) = (int(r.x) + 200, int(r.y + 596) − 100)`: `live_anchor(c).x == Approx(r.y + 596 − y0).margin(0.05)`; `mouse_move` to `(x0 + 30, y0 − 150)`: `live_extent(c).x == Approx(r.y + 596 − (y0 − 150)).margin(0.05)`. These go through the production press sites (`PdfCanvas.cpp:898` and `:973`), which `strip_page_point_at` does not.
  - Case 7 strip `PdfCanvas rotation: a stale strip page is pressed through its layout rect` — strip, zoom 1.0, land; `set_zoom_pct(2.0)`, `strip_kick(KickReason::Zoom)` without landing; `canvas.rotate(true)` + `strip_kick(Rotate)` without landing: `strip_drawn_rect(0)` equals `strip_page_placement(strip_page_rect(0))`; with `r` that rect, a press at integer `(vx, vy)` inside it maps to page `((vy − r.y) / 2.0, (r.x + r.w − vx) / 2.0)` within 0.01 (quarter-1 inverse at the layout zoom). The natural rect would be `842 x 596` instead of `1683.8 x 1190.6`.
  - Review Focus 5 `PdfCanvas rotation: a failed strip page keeps its placeholder in the turned rect` — `kDamaged`, strip, **custom zoom 1.0** (at a fit mode the turn refits, the render scale changes, and `change_render_params` resets every Failed slot, `StripSchedule.hpp:300-307`, so no border would be drawn), `strip_navigate(2)` (page index 2 is damaged; its stand-in is page 0's `420 x 595`), `land_strip_failed(2)`; `rig.rotate(true)`; `REQUIRE` the slot is still Failed (a slot that is not is a defect to report, not a test to loosen): its `strip_drawn_rect` equals the turned layout rect (`w` = the stand-in's height × zoom); the pixel on the rect's left border column, mid-height, is neither page white nor chrome.
- [ ] **Step 2: Run, verify they fail.**
- [ ] **Step 3: Implement** per Interfaces.
- [ ] **Step 4: Run** `"[rotation]"`, `-# "[#test_canvas_spread_wiring]"` and `"[strip]"` → PASS.
- [ ] **Step 5: Mutants:** `strip_drawn_rect` returns the layout rect for a fresh page (case 8); `PdfCanvas.cpp:898` keeps `strip_page_placement` (the real-press case's anchor); `:973` keeps it (its extent); the strip skips `SetTransform` for hits (case 8's hit pixel, "not dark").
- [ ] **Step 6: Commit** `feat(canvas): draw the strip turned (#53)`.

---

## Task 7: Scroll-into-view in the view frame

Spec §3.5 "Scroll-into-view", §5.4 case 5, §5.1 last bullet's hit edges.

**Files:**
- Modify: `src/ui/detail/PageAnchor.hpp:92-98` (`hit_top_pt` / `hit_bottom_pt`)
- Modify: `src/ui/PdfCanvas.cpp:474-567` (`scroll_into_view`, both branches), `:1953-1983` (`pan_y_for_hit`)
- Modify: `src/ui/PdfCanvasStrip.cpp:270-280` (landing hit), `:326-344` (`landing_pan_y`)
- Test: `tests/unit/test_view_rotation.cpp` (no file tests `hit_top_pt` today; this one builds its own hit, since `hit_at` lives in `CanvasRig.hpp`), `tests/unit/test_canvas_rotation.cpp`

**Interfaces:**
- Consumes: `local_rect_to_view` (Task 2).
- Produces (the old one-argument overloads are **removed**, so every call site fails to compile until it passes the rotation):

```cpp
// PageAnchor.hpp: the quad's top / bottom in points from the top of the page AS DRAWN.
// view_w_pt / view_h_pt: the page's view-frame size (DocumentView::known_page_size).
float hit_top_pt(const SearchSession::Hit& h, litepdf::core::Rotation r,
                 float view_w_pt, float view_h_pt) noexcept;
float hit_bottom_pt(const SearchSession::Hit& h, litepdf::core::Rotation r,
                    float view_w_pt, float view_h_pt) noexcept;
```

  Paged callers take the size from `known_page_size(page).value_or({0, 0})`. A page with no size most likely failed to render too (`PdfCanvas.cpp:1748`); if it did render, its hit is misplaced at 180°/270° only, which is accepted; the strip from `page_sizes()[page]`. `scroll_into_view`'s two inline min/max blocks and `pan_y_for_hit`'s become these calls.

- [ ] **Step 1: Write the failing tests:**
  - `ViewRotation: hit edges in the view frame` — a hit whose quad is x `100..200`, y `100..118` (built in the test, like `hit_at`) on a page `420 x 600` unturned: at 0 top/bottom `100/118`; at 1 (view size `600 x 420`) `100/200`; at 2 `600 − 118 / 600 − 100`; at 3 `420 − 200 / 420 − 100`.
  - Case 5 `PdfCanvas rotation: a hit is scrolled into view by its turned extent` — `kMarker`, Single, custom zoom 2.0, `Access::navigate(c, kMarkWordPage)`, land; `rig.rotate(true)`, land; `h = marker_hit(view)`; `canvas.scroll_into_view(h)`. The turned page is `1191` px tall (`ceil(595.276 · 2)`), so the centring pan `425 − 2·kMarkWordCentreXPt` is clamped: expect `pan_y() == 850 − 1191`, and the hit's turned centre `2·kMarkWordCentreXPt + pan_y` within `[0, 850]`. (The raw-y mutant centres y ≈ 75 pt and clamps to 0.) Then the strip: `kMarker`, strip, custom zoom 2.0, `strip_navigate(0)`, land, `rig.rotate(true)`, `scroll_into_view(h)`: `strip_scroll_y == Approx(strip_layout(c).top(kMarkWordPage) + 2·kMarkWordCentreXPt − 425).margin(2.0)` (unclamped: page index 3 follows).
- [ ] **Step 2: Run, verify they fail** (compile errors at every old call site count as the red).
- [ ] **Step 3: Implement** and thread rotation + size through every caller the compiler names.
- [ ] **Step 4: Run** `"[rotation]"`, `-# "[#test_canvas_spread_wiring]"`, `"[strip]"`, `"[anchor]"` → PASS (rotation 0 gives the old values).
- [ ] **Step 5: Mutant:** `local_rect_to_view` in `hit_top_pt` replaced by the raw y (case 5, both layouts).
- [ ] **Step 6: Commit** `feat(canvas): scroll hits into view in the turned frame (#53)`.

---

## Task 8: Commands, menu, smoke rows, CHANGELOG

Spec §3.6, §5.5, §6.

**Files:**
- Modify: `resources/MainMenu.rc.h:78` (`IDM_VIEW_ROTATE_CW 40067`, `IDM_VIEW_ROTATE_CCW 40068`; "Next free ID: 40069. Reserve 40069-40070")
- Modify: `resources/litepdf.rc.in:75` (after Continuous Scroll: `MENUITEM "Rotate C&lockwise\tCtrl+Shift+=", IDM_VIEW_ROTATE_CW`, `MENUITEM "Rotate Counterclock&wise\tCtrl+Shift+-", IDM_VIEW_ROTATE_CCW`)
- Modify: `src/ui/MainWindow.cpp:1689-1725` (enable both with `active_view() != nullptr`), `:1853` (handler beside `IDM_VIEW_DUAL_PAGE`), `:2749-2750` (accelerators)
- Modify: `scripts/ux-probe.ps1` (`find-text`, `send-chord`), `scripts/smoke-test.ps1` (four rows)
- Modify: `CHANGELOG.md` (`## [Unreleased]` → `### Added` → "Rotate the view in 90° steps: View > Rotate Clockwise / Counterclockwise (#53).")

**Interfaces:**
- Consumes: `PdfCanvas::rotate`, `KickReason::Rotate` (Task 4); `kMarkWordCentreXPt` (Task 4 Step 1).
- Produces:
  - Accelerators `{FCONTROL | FSHIFT | FVIRTKEY, VK_OEM_PLUS, IDM_VIEW_ROTATE_CW}`, `{FCONTROL | FSHIFT | FVIRTKEY, VK_OEM_MINUS, IDM_VIEW_ROTATE_CCW}`.
  - Handler (one `case` pair): no view or canvas → return; `canvas_->rotate(id == IDM_VIEW_ROTATE_CW)`; page = `current_page()`, the pair's left page in the spread (as the dual-page handler, `MainWindow.cpp:1867-1871`); `kick_render(page, KickReason::Rotate)`.
  - `ux-probe.ps1 find-text <query>`: `WM_SETTEXT` the `LitePDFFindBar`'s EDIT child (as `page-box-go`, `:559-571`), no Enter. `ux-probe.ps1 send-chord <vk>...`: `SendInput` key-downs in order then key-ups in reverse; refuses (exit 2, message) unless `GetForegroundWindow()` is the litepdf main window. The P/Invoke `INPUT` struct must be the full union size (40 bytes on x64, `type` + 4 padding + the largest member, `MOUSEINPUT`); a struct holding only `KEYBDINPUT` makes `SendInput` return 0 with error 87. Check its return value equals the number of inputs, else throw.

- [ ] **Step 1: Write the smoke rows** (red first: the app has no command yet), in a new launch of `rotation-marker.pdf` with the strip preference on (`Start-StripLaunch`), FitWidth:
  - **Range row:** post `send-cmd 40067`; `Wait-UxProbeState -Probe canvas-scroll` until `max + 1` is within 2 of `8 + 4·(595.276·z + 8)`, `z = (client_w·96/dpi) / 841.89` computed from that same reading; then three more turns restore `8 + 4·(841.89·z0 + 8)`, `z0 = client_w_dip / 595.276`.
  - **Direction row:** fresh state (four turns done), post `40067` and wait for the turned range. `$markCentreXPt` is a literal in the script equal to Task 4's `kMarkWordCentreXPt`, with a comment naming that test. From the reading: `z` as above, `vp = client_h_dip`, `top2 = 8 + 2·(595.276·z + 8)`. Preconditions, each a throw with the reading: `pos == 0`; `top2 > vp` (the word's page is off screen); both predictions below lie inside `[0, max + 1 − page]`. Then `send-cmd 40042` (find bar), `find-text ZENITH`, and wait for `pos` to change with a `Wait-UxProbeState` whose `-Until` block **re-posts `send-cmd 40043` on every poll** while `pos` is unchanged. One post is not enough: `WM_SETTEXT` only arms the find bar's 120 ms debounce (`FindBar.cpp:59`, `:627-638`), the scan must then reach page index 2, and `on_find_next` with no hit yet returns and is never re-fired (`MainWindow.cpp:2410-2414`). Re-posting is safe: the document has one hit and `next` wraps to it. Predictions: clockwise `top2 + markCentreXPt·z − vp/2`; counter-clockwise `top2 + (595.276 − markCentreXPt)·z − vp/2`; `gap = |2·markCentreXPt − 595.276|·z`. Assert `gap > 32` and the reading within `gap/4` of the clockwise prediction.
  - **Accelerator row:** `send-chord 0x11 0x10 0xBB` (Ctrl, Shift, VK_OEM_PLUS) on a fresh `rotation-marker.pdf` launch; on exit 2 log `[SKIP] accelerator row: litepdf is not the foreground window` and continue; else assert the turned range as in the range row.
  - **Menu row:** with no document, `Read-MenuState -Ids @("40067","40068")`: both `found` and `grayed`; with `rotation-marker.pdf` open: both `found` and not `grayed`.
- [ ] **Step 2: Build the app, run the smoke, verify the rows fail** for the right reason: the range row times out with the range unchanged (the smoke stops at the first failing row, so run the menu row once on its own as well: items not found). The direction row cannot reach its position check before Step 3 (`find-text` does not exist yet and its turned-range wait throws first); its position check is proven in Step 5.
  `& $cmake --build build --target litepdf --config Release`, then `powershell -ExecutionPolicy Bypass -File scripts\smoke-test.ps1 -ExpectDev`.
- [ ] **Step 3: Implement** ids, menu, enable state, handler, accelerators, probe actions, CHANGELOG line.
- [ ] **Step 4: Run the smoke** → all rows `[OK]` (or the accelerator row `[SKIP]` with its reason).
- [ ] **Step 5: Prove each row can fail** (`reference_litepdf_smoke_script_rules`): range row with the `send-cmd` removed (times out, message names the range); direction row posting `40068` instead (fails on position, message shows both predictions); menu row with the enable state forced to `MF_ENABLED` (fails the no-document read); handler binding swapped (`rotate(id == IDM_VIEW_ROTATE_CCW)`) → direction row fails; accelerator row with the `VK_OEM_PLUS` rotate entry removed, run with litepdf in the foreground → times out on the range. Revert each. The accelerator row must also record **one non-SKIP pass** (a run with litepdf in the foreground, e.g. started from an interactive desktop); a run where it only ever skipped does not count as proof, and the PR description says which run it was.
- [ ] **Step 6: Commit** `feat(ui): View > Rotate Clockwise / Counterclockwise (#53)`.

---

## Task 9: Full run and mutation record

Spec §5.6, §5.7.

- [ ] **Step 1: Full suite:** `& $ctest --test-dir build -C Release` serially from PowerShell; read the count with `-N` first. Expected: all PASS.
- [ ] **Step 2: Smoke:** the full `smoke-test.ps1 -ExpectDev` run → exit 0.
- [ ] **Step 3: Mutation table** in the PR description: one row per spec §5.6 seam (size swap, `view_px`, `draw_affine`, `view_to_local`, `local_rect_to_view`, strip `drawn_rect`, `pan_x = 0`, Top anchor, `strip_view.reset()`, `rotate(cw)` direction, command binding), the killing test, and the total `N/11` (from the task commit bodies). A survivor is a defect: add the missing test in the owning task's file before going on.
- [ ] **Step 4:** No commit unless Step 3 added a test (`test(canvas): ... (#53)`).
