# Continuous scroll (#55), PR 4: Default on and release readiness — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn continuous scroll on by default, prove it against the two PR 4 measurement gates (memory, GUI thumb drag), make the smoke run deterministic about the layout it tests, and write the phase's user-facing documentation — so that `main` after this PR is release-ready for #55 (spec §8 item 4, §9.5).

**Architecture:** The product change is one constant (`ViewPrefs::kContinuousScrollDefault`) and its comments. Everything else is verification and documentation: the smoke script takes ownership of the `ContinuousScroll` value for the whole run (captured once, the default for every row that does not set it, restored at the end and on failure), a new smoke row pins the default, two local scripted measurements produce the §9.4 gate readings, and README, `docs/SMOKE-CHECKLIST.md` and CHANGELOG describe the feature.

**Tech Stack:** C++17 (MSVC), Win32, Catch2, CMake/ctest from VS 2022 BuildTools, PowerShell 5.1 (`smoke-test.ps1`, `ux-probe.ps1`, local gate drivers).

**Spec:** `docs/superpowers/specs/2026-10-22-continuous-scroll-design.md`. Read §1 (U1, U2), §6.1, §6.2, §7.2, §7.3, §9.3, §9.4 and §9.5 before any task; § numbers below refer to it. This plan covers **PR 4 only** (§8 item 4). PR 3's plan, `docs/superpowers/plans/2026-10-23-continuous-scroll-55-pr3-overlays.md`, describes the menu item and the smoke helpers this plan reuses.

## Global Constraints

- Baseline `main` @ `5621242` (PR 3 merged). Line numbers below are for `5621242`; find code by name if a line moved.
- **Build and test:** as in PR 2's plan — Release only, full BuildTools paths for `cmake`/`ctest`, MuPDF junction in the worktree, `-DFETCHCONTENT_FULLY_DISCONNECTED=ON`. Every "run" step builds first (`& $cmake --build build --config Release`). Full suite: `& $ctest --test-dir build -C Release` **without `-j`**; read the count with `-N`, never hard-code it.
- `TEST_CASE` names: pure ASCII, prefixed by subsystem (`ViewPrefs: `).
- **Existing tests:** exactly one existing assertion changes — `tests/unit/test_view_prefs.cpp:20`, `REQUIRE_FALSE(prefs.continuous_scroll())` → `REQUIRE(prefs.continuous_scroll())` — because it pins the old default (spec §7.2: "0 in PR 1–3, 1 from PR 4"). The PR body justifies it by name. No other existing test file changes.
- **Paged behaviour does not change** (U6): with `ContinuousScroll = 0` every paged reading of PR 1–3 still holds.
- **PowerShell 5.1 floor** for `*.ps1` (no `?.`, `??`, ternary). Probes print one line of JSON. Cross-process: only system messages carry data.
- **Local GUI runs go through the wrapper** `~/.claude/compact-dumps/issue-165-36f011/run-gui.ps1` (`& <path> -Body { ... }`); never run `smoke-test.ps1` or a gate driver bare. Reading state after a posted command polls (≤ 5 s), never a fixed sleep (#149).
- **Real input steals the user's foreground.** Only Task 5 sends real input (`SendInput`). Before it runs, ask the user in chat whether they are at the machine and run only on a yes. Before **every** `SendInput`, assert `GetForegroundWindow()` is litepdf's main window and `WindowFromPoint(target)` is the canvas; on a failed guard mid-drag, send the left-button-up, abort, and report the run as invalid. Tasks 1–4 and 6 post messages only.
- Gate drivers (Tasks 3 and 5) are local and **not committed**: they live beside the wrapper in `~/.claude/compact-dumps/issue-165-36f011/` (as the #49 measurement did). Their readings are recorded in the spec (§9.4) and the PR body.
- Spec values: `IDM_VIEW_CONTINUOUS = 40066`, `IDM_VIEW_THUMBS = 40060`, `IDM_ZOOM_IN = 40010`; `HKCU\SOFTWARE\LitePDF\Settings` DWORD `ContinuousScroll`; idle = the process uses ≤ 20 ms of CPU time in a 1000 ms window (2 % of one core, §9.4's "CPU below 2% for 1 s"); the memory bound is 1.25×; the drag oracle's deadline is 5 s after release.
- Commits: conventional, English, ending with `Co-Authored-By: <the model that wrote the commit> <noreply@anthropic.com>`. `git add` files by name (the MuPDF junction). No `VERSION` change (release PR only). CHANGELOG `[Unreleased]` entries are written here (Task 4).
- **Decisions made here** (the owning task patches the spec in its commit):
  1. **The smoke run owns `ContinuousScroll` for its whole length.** Today only the strip section saves and restores it, so every other row runs with whatever the registry holds: on CI (no value, and no `Settings` key at all) that becomes continuous mode the moment the default flips; locally it is the user's own setting (the strip section's `finally` puts it back before the Phase 8 rows, so they run with it too). The run now captures the value before the first launch, removes it so every row that does not set it runs in **the default**, and restores it at the end and from the `trap`; the strip section's `finally` returns to the default instead of restoring, because Row 24 leaves 0 behind. The pair must not depend on the order of `finally` and `trap` (5.1 runs the `finally` first, checked; CI runs the script under pwsh 7, `ci.yml:38`, unchecked): after `Restore-ContinuousPref` has run once, `Set-SmokeDefaultPref` does nothing. Owner: Task 2 (a sentence in §9.3).
  2. **No row outside the strip section tests paged behaviour**, so none sets the preference off. Each was read for a paged-only assertion (cold start, bookmarks title, multi-tab, find bar, cross-tab find, thumbnail pane, ePub/CBZ/encrypted liveness); all hold in either layout, and running them in the default is the coverage the default needs. The only numeric budget among them, the cold-start T0→T4 ≤ 1500 ms on `simple.pdf`, has headroom in the strip: CI's strip cold-start line on `large.pdf` read 99 ms (run 40063866816). §9.3's "set the preference off explicitly" applies to the strip section's paged rows, which already set the preference themselves (0 for paged single; the two-page rows keep 1 on purpose, since the spread wins over it). Paged mode's smoke coverage is then the strip section's paged rows: Row 0's second half, the two-page rows (paged spread) and the menu-item rows (paged single); the PR body says so. Owner: Task 2.
  3. **The memory gate reads two peaks, on private bytes, with GPU memory recorded beside them.** `PeakPagedMemorySize64` (the process's lifetime peak private bytes, `PeakPagefileUsage`, the #49 reading) includes the zoom-up to 800%, and the zoom-up is **not** the same in the two arms: paged `IDM_ZOOM_IN` (`MainWindow.cpp:1962`) re-renders through `MainWindow::kick_render`'s `request_render_with_prefetch` (`MainWindow.cpp:408`), so the paged arm also renders a neighbour at every zoom step, while the strip only re-lays out and its prefetch range (one viewport below, `StripSchedule.hpp:91-96`) holds no page from about 300% up. The lifetime peak is therefore biased toward passing, and the discriminating reading is the second one: the largest private-bytes sample taken after each scroll step. The gate is both ratios ≤ 1.25. The strip's distinguishing cost is one D2D bitmap per Ready page (§5.4) on a hardware render target (`D2D1_RENDER_TARGET_TYPE_DEFAULT`, `PdfCanvas.cpp:1370`). So every sample also records the process's GPU memory (`\GPU Process Memory(pid_<pid>*)\Dedicated Usage` and `\Shared Usage`, summed over every instance the wildcard matches) — **recorded and reported, not gated**: §5.4 accepts that a visible page exists twice (L1 pixmap and D2D bitmap) and the strip retains bitmaps one viewport each side (`StripSchedule.hpp:114`), so at 800% on a 200% display (~496 MB a page) the private-plus-GPU ratio is expected near 1.3–1.6 by design; gating on it would gate on an accepted cost the spec's gate does not contain. The readings go into §9.4 and the PR body so the cost is visible. Owner: Task 3 (sentences under §9.4's table).
  4. **A gate miss stops the PR.** The drivers, their parameters (window size, idle rule, post caps, the 5 s deadline) and the bounds are frozen after each task's Step 1 dry-run. A miss — in any run of either gate, including only the second pair — ends work on the remaining tasks: report DPI, client size, every reading and the ratios to the user, and do not rerun, resize, re-tune, start Task 4 or 6, or open the PR until the user rules. Task 6 requires both gates recorded as passes (or the user's written ruling on a miss). Owner: Tasks 3, 5 and 6.

## Review Focus

1. **A first launch with no `ContinuousScroll` value — every upgrade from 1.5.6 and every new install.** The document opens as the strip and View → Continuous Scroll is checked. Pinned by Task 1's smoke row 0.
2. **A user who turned it off.** A value of 0 keeps paged mode across restarts, with the item unchecked. Pinned by Task 1's smoke row 0 (second half); PR 3's menu-item row pins the write.
3. **A crash-restore of a session saved by a paged build.** `session.json` is unchanged (§7.2), so after the flip the restored tab must enter the strip at its saved page, not page 1. Pinned by Task 6's scripted check, step 4.
4. **Dragging the scroll bar thumb through a 1000-page document with the thumbnails pane open.** The last page shows and the process goes idle within 5 s of release. That is a liveness reading: a thumbnail queue that grew with every track position (§5.2 step 4 bounds it) would keep the process busy past the deadline, but the gate counts nothing. Pinned by Task 5's gate.
5. **Find bar, cross-tab find, thumbnails, ePub, CBZ and the encrypted-PDF prompt in the default layout.** Before this PR the smoke run exercised them in paged mode only. Pinned by Task 2 (those rows now run in the default; the thumbnail-pane row and the ePub/CBZ launches assert that the run really is the strip and the value is absent).

---

## Task 1: Continuous scroll on by default

**Files:**
- Modify: `src/core/ViewPrefs.hpp` (`:14` comment, `:23` constant), `src/ui/MainWindow.cpp` (`:178` comment)
- Test: `tests/unit/test_view_prefs.cpp` (`:20`, the one existing-assertion change), `scripts/smoke-test.ps1` (strip section)

**Interfaces:**
- Consumes: PR 3's smoke helpers `Start-StripLaunch [-Paged]`, `Stop-LitePDF`, the `menu-state` probe, `$stripKey`, `$largeFixture`.
- Produces: `static constexpr bool kContinuousScrollDefault = true;`

- [ ] **Step 1: Change the unit assertion** — `test_view_prefs.cpp:20`: `REQUIRE(prefs.continuous_scroll());`. The test's name ("a missing value gives the default") stays true.
- [ ] **Step 2: Write the failing smoke row** — in the strip section's `try`, right after `if (-not $stripHadKey) { New-Item ... }` and before Row 1's `Set-ItemProperty ... 1`, add **Row 0: the default**:
  1. `Remove-ItemProperty -Path $stripKey -Name ContinuousScroll -ErrorAction SilentlyContinue`; `$procStrip = Start-StripLaunch -Fixture $largeFixture -Step "default layout"` (it waits for the scroll bar and a laid-out strip, and throws otherwise); `& $uxProbe menu-state 40066` → item `found`, not `grayed`, `checked`; `Stop-LitePDF`.
  2. `Set-ItemProperty ... ContinuousScroll -Value 0 -Type DWord`; `Start-StripLaunch ... -Step "default layout, preference off" -Paged`; `menu-state 40066` → not `checked`; `Stop-LitePDF`.
  3. One `[OK] continuous strip: no value opens the strip with the item checked; 0 opens paged with it unchecked` line.
- [ ] **Step 3: Run and see both fail** — the unit test fails on line 20; the smoke row, run wrapped, throws "the default layout launch shows no laid-out strip".
- [ ] **Step 4: Flip the default** — `kContinuousScrollDefault = true`. Header comment `:14`: `ContinuousScroll (REG_DWORD)  0 = single/dual page view, non-zero = strip; missing = strip (#55).` `MainWindow.cpp:178`: `// #55: ContinuousScroll; missing or invalid -> on`.
- [ ] **Step 5: Run and see them pass** — the unit test, the full suite, and the smoke run wrapped (Row 0 and every later strip row).
- [ ] **Step 6: Commit** — `feat(core): continuous scroll on by default (#55)`.

## Task 2: The smoke run owns the preference

**Files:**
- Modify: `scripts/smoke-test.ps1` (the `trap` `:92`; the strip section's capture `:627-639` and `Get-ContinuousPref` `:720-726` move; its `finally` restore `:1623-1636`; the thumbnail-pane row `:558-614`; `Start-LitePDFSmokeLaunch` `:1648`), spec §9.3 (decisions 1 and 2)

**Interfaces:**
- Consumes: Task 1's Row 0; `Wait-UxProbeState` (`:409`).
- Produces (functions in `smoke-test.ps1`, defined before the `trap`; PowerShell defines a function only when its line runs, so a call above its definition throws `CommandNotFoundException`):
  - `Set-SmokeDefaultPref` — removes the `ContinuousScroll` value (the default layout) and leaves the key; does nothing when the key does not exist (CI has no `Settings` key: an unguarded `Remove-ItemProperty` under `$ErrorActionPreference = "Stop"` throws `ItemNotFoundException`, checked) and nothing once `$prefRestored` is set.
  - `Restore-ContinuousPref` — today's `finally` restore, moved as it is: put back the prior value with its kind, or remove the value, and remove the key only if the run created it and it is empty; then sets `$script:prefRestored = $true`. A second call does nothing.
  - `Get-ContinuousPref` — moved up unchanged, so the thumbnail-pane row can read it.

Rules (decisions 1 and 2):
- Move the capture (`$stripKey`, `$stripHadKey`, `$stripHadValue`, `$stripPriorValue`, `$stripPriorKind`) above the `trap`, keep the names, and set `$prefRestored = $false`. Call `Set-SmokeDefaultPref` right **after** the `trap` statement and before the first launch, so a failure after the value is removed always reaches the restore.
- The `trap` calls `Restore-ContinuousPref` first, inside its own `try { } catch { }`, then does today's crash-dump listing and `break`. With the latch, the result is the same whichever of the strip `finally` and the `trap` runs first.
- The strip section's `finally` calls `Set-SmokeDefaultPref` instead of the restore (the Phase 8 rows after it run in the default; Row 24 has just left 0 there). The script's last lines, before `[OK] smoke test passed.`, call `Restore-ContinuousPref`.
- Precondition in the thumbnail-pane row: after its launch and before the first F4, `Get-ContinuousPref` is `$null` (the value is absent, not a leftover 1) and `Wait-UxProbeState -Probe canvas-scroll -Until { param($s) $s.present -and $s.vscroll }` reports `vscroll`; throw otherwise.
- `Start-LitePDFSmokeLaunch` (ePub, CBZ) gains the same two checks after the window shows and before `Stop-LitePDF` — the rows after the strip section are where a 0 left by Row 24 would bite. Both formats pass the §4.4 gate (`EffectiveLayout.hpp:68`). The encrypted row cannot check: its modal blocks before a view exists.

- [ ] **Step 1: Write the preconditions** (thumbnail-pane row, `Start-LitePDFSmokeLaunch`) and move the capture and `Get-ContinuousPref` above the `trap` — nothing else yet. Run the smoke wrapped with `ContinuousScroll` set to 0 inside `-Body` before calling the script → the run fails with the thumbnail-pane precondition's **own** message (assert the text in `-Body`'s `catch`; a `CommandNotFoundException` means the move is missing, not that the precondition works).
- [ ] **Step 2: Implement** the latch, the two functions, the `Set-SmokeDefaultPref` call after the `trap`, the `trap`'s restore call, the `finally` change and the end-of-run call.
- [ ] **Step 3: Run wrapped, three times** — with the value absent, set to 0, and set to 1 before the script starts. Every row passes each time, and the oracle is read **inside `-Body`, right after the script returns** (the wrapper restores the value itself in its own `finally`, so a read after the wrapped run cannot tell a working script restore from a broken one): `Get-ItemProperty` shows the value exactly as it was before that run (absent, 0, 1), and with the value absent the `Settings` key is gone again if the run created it.
- [ ] **Step 4: Check the failure path** — a temporary `throw "probe"` right after the find-bar row (not committed): inside `-Body`, catch the script's error, assert it is `probe`, and read the value as in Step 3 → restored. Then once more with the throw inside the strip section (after Row 24's 0, before its `finally`): the value is restored there too. Remove the throws and check `git diff` shows no trace of them.
- [ ] **Step 5: Mutation checks** (reverted after each, `mutation-test-hygiene`): remove the strip `finally`'s `Set-SmokeDefaultPref` call → the ePub launch's precondition fails; make `Restore-ContinuousPref` a no-op → Step 3's in-`-Body` oracle fails for the 0 and 1 runs.
- [ ] **Step 6: Patch the spec** — §9.3, after "From PR 4 onward, existing smoke steps that test paged behaviour set the preference off explicitly. Otherwise they would silently run in continuous mode and test something else." add: "The smoke run captures the value before its first launch, runs every row that does not set it in the default, and restores the value at the end and on failure. Only the strip section's rows test paged behaviour, and each sets the preference itself: 0 for paged single, or 1 with Two-Page Spread on for the paged spread."
- [ ] **Step 7: Commit** — `test(smoke): run every row in a known layout and restore the preference (#55)`; include the spec edit.

## Task 3: The memory gate (§9.4)

**Files:**
- Create (local, not committed): `~/.claude/compact-dumps/issue-165-36f011/gate-memory.ps1`
- Modify: spec §9.4 (the readings, decision 3)

The driver, run through the wrapper with `LITEPDF_NO_RESTORE=1`, measures two arms on `tests\fixtures\large.pdf` (500 Letter pages), each a fresh launch:
- **Paged arm:** `ContinuousScroll = 0`.
- **Strip arm:** the value absent (the default after Task 1).

The driver calls `SetProcessDpiAwarenessContext(-4)` (Per-Monitor V2) before any window call (`win32-dpi-measurement`), and reads every `Process` counter after `.Refresh()` — a `Process` object caches `PrivateMemorySize64` and `PeakPagedMemorySize64` until refreshed, so an unrefreshed driver reads its first value at every step and both ratios come out 1.0.

Per arm:
1. Launch, wait for the main window, read `canvas-scroll` once, then `SetWindowPos` it to the same fixed outer size in both arms (1600×1200 physical pixels at the monitor's position 0,0; the dry-run confirms the monitor is at least that large), and poll `canvas-scroll` until `client_w`/`client_h` differ from the pre-resize reading. Record DPI and client size per arm. `client_h` must be equal in both arms and `client_w` may differ only by the vertical scroll bar (the strip has `WS_VSCROLL`, paged mode does not, §6.2): `paged client_w − strip client_w` equals `GetSystemMetricsForDpi(SM_CXVSCROLL, dpi)` ±1, else the run is invalid.
2. Post `IDM_ZOOM_IN` (40010) one at a time, waiting for idle after each, until the status-bar zoom readout (the `Static` child whose text matches `^\d+%$`, #57) reads `800%`; fail after 20 posts.
3. Record the scroll baseline: private bytes (`PrivateMemorySize64`) and GPU memory (decision 3's two counters, `Get-Counter`, matched on the litepdf pid).
4. Scroll, waiting for idle after each step and sampling private bytes, GPU memory and the step's CPU time (`TotalProcessorTime` delta from the post to idle):
   - paged: 10 × `canvas-key 0x22` (PgDn), page box 1 → 11 (page 11 at its top);
   - strip: first assert `|canvas-scroll max + 1 − (500 × (792 × 8 + 8) + 8)| <= 1` (rounded DIPs, §6.2; the strip is laid out at 800% on Letter pages), then `canvas-key 0x22` until `pos >= round(anchor(10))`, `anchor(10) = 10 × (792 × 8 + 8)` DIP — page 11's top at the viewport top, the place the paged arm ends (fail after 400 posts; record the count). The page box is not the stop condition: the derived current page reads 11 about half a viewport earlier (§4.6), before page 12 enters the prefetch range, which would bias the strip toward passing.
5. Read `PeakPagedMemorySize64` (lifetime peak private bytes) and close with `WM_CLOSE`.

Gate (decision 3): strip lifetime peak ≤ 1.25 × paged lifetime peak **and** strip max scroll sample ≤ 1.25 × paged max scroll sample (private bytes). GPU memory is recorded, not gated. Record, per arm: DPI, client size, steps, baseline, max sample and lifetime peak (private), baseline and max (GPU dedicated, shared), CPU per step (median and max). The P0-miss signature (#49's oracle, a second full render) is measured against the **paged arm's median step** (one full page render at 800%): list the strip steps whose CPU is at least 1.8 × that value. The strip's own median step is near 0 — most PgDn steps render nothing — so it is not the reference.

- [ ] **Step 1: Write the driver** and dry-run it on one arm: the zoom readout reaches `800%` and a negative control (stop after one post) reads less; the strip's `max + 1` check passes; one `Get-Counter` sample returns both GPU counters for the litepdf pid; the 800% baseline's private bytes exceed a sample taken before the first zoom post by at least 100 MiB (the sampling moves — positive control for `.Refresh()`). After this step the driver and the gate are frozen (decision 4).
- [ ] **Step 2: Run both arms twice, wrapped** (order paged, strip, paged, strip). Both strip runs must pass against both paged runs; the second pair is a second chance to miss, not a second chance to pass.
- [ ] **Step 3: On a miss, stop** (decision 4): report DPI, client size, every reading and the ratios to the user; no rerun, no change to the driver or the gate, no further task.
- [ ] **Step 4: Record** — in spec §9.4's gate table, the Memory row's condition becomes "peak private bytes — the lifetime peak and the largest sample after each scroll step — ≤ 1.25 × paged mode's …", so the table states both bounds. Below the table: "Measured in PR 4 (large.pdf, 800%, DPI <d>, client <w>×<h>): lifetime peak private bytes paged <a> MiB / strip <b> MiB (<ratio>); max scroll sample paged <c> / strip <e> (<ratio>); GPU memory max paged <f> / strip <g>; CPU per step …". Add decision 3's sentences: "The gate reads both the lifetime peak and the largest sample after each scroll step. The lifetime peak includes the zoom-up to 800%, where paged mode also renders a neighbour at every step and the strip does not, so it leans toward passing; the per-step sample is the discriminating reading. The strip's page bitmaps live in GPU memory on a hardware render target, so GPU memory is recorded beside private bytes; it is not gated, because §5.4 accepts a page existing twice and the strip retains bitmaps one viewport each side."
- [ ] **Step 5: Commit** — `docs(spec): record the continuous scroll memory gate (#55)`.

**Outcome (2026-10-24).** The measurement contradicted decision 3's premise that private bytes do not include the bitmap: on the measured GPU each D2D page bitmap costs its size in private bytes as well as in GPU-dedicated memory. The per-step private-bytes ratio therefore missed (1.24–1.27, structural ≈ 1.31) while the lifetime peak passed (0.82). The user ruled the miss accepted; Step 4's record states the miss, the ruling and the corrected cause instead of the sentences quoted above.

## Task 4: README, smoke checklist and CHANGELOG

**Files:**
- Modify: `README.md` (`:40-44` features, `:72` keys), `docs/SMOKE-CHECKLIST.md`, `CHANGELOG.md` (`[Unreleased]`)

- [ ] **Step 1: README.**
  - New feature bullet after **Two-page spread**: `**Continuous scroll** — pages in one vertical strip with a scroll bar, on by default; View → Continuous Scroll switches back to one page at a time and is remembered. Two-page spread still shows two pages at a time. XPS and FB2 documents stay one page at a time (v1.6.0)`.
  - **Mouse-wheel scrolling** bullet: append `In continuous scroll the wheel moves through the pages without turning them.`
  - Keys: the PgDn/PgUp row becomes `Next / previous page (or pair in spread mode); one screen down / up in continuous scroll`; add a row `Home / End` — `First / last page (top / bottom of the document in continuous scroll)`.
  - The `(v1.6.0)` tag assumes the next release is a minor one; the release PR checks it.
- [ ] **Step 2: `docs/SMOKE-CHECKLIST.md`** — a new section `## Continuous scroll (#55)` after "Core viewer", unchecked boxes:
  - With no `HKCU\SOFTWARE\LitePDF\Settings\ContinuousScroll` value, a PDF opens as a vertical strip with a scroll bar, and View → Continuous Scroll is checked
  - View → Continuous Scroll off shows the same page one at a time; relaunch → still off; on again → the strip at the same page
  - Wheel, PgDn/PgUp, Home/End and the arrow keys scroll; the page box follows the page in view
  - Drag the scroll bar thumb from top to bottom of `tests\fixtures\strip-1000.pdf` with the thumbnails pane (F4) open: the last page shows within 5 s and the window stays responsive
  - Ctrl+wheel zoom keeps the top of the window on the same place in the document
  - Ctrl+Shift+D in a strip tab shows the paged spread; pressing it again returns to the strip at the same page
  - Find highlights the hits on every page on screen; F3 moves through hits on later pages
  - A drag selection that runs into the next page stops at the end of the page it started on; Ctrl+C copies it
  - Thumbnail and outline clicks jump to the top of the page
  - Two strip tabs keep their own positions across tab switches
  - An `.xps` file opens one page at a time, with View → Continuous Scroll greyed
- [ ] **Step 3: CHANGELOG** — a `### Added` section above `### Changed` in `[Unreleased]`, one entry ending `(#55)`. It must say: pages scroll as one continuous strip with a vertical scroll bar, on by default; View → Continuous Scroll turns it off, and the choice is remembered; the wheel never turns a page there, PgDn/PgUp scroll by most of a screen, Home/End go to the top/bottom, and the page box, Ctrl+G, thumbnails and outline jump to a page; Two-page spread shows the paged spread for that tab, and turning it off returns to the layout Continuous Scroll selects (the strip while it is on); search highlights hits on every page on screen and Find starts from the top page on screen; a text selection stays within the page it started on; XPS and FB2 documents stay one page at a time. Follow the entry rules in the ship-convention memory (claims only what was measured or tested; every caveat names every action it covers). No memory figures unless Task 3's readings are quoted exactly.
- [ ] **Step 4: Check** — every claim in the three files against the spec section it paraphrases (§6.1, §6.3, §7.1, §7.3, §4.4); the README key table against the accelerator table built in code (`ACCEL accels[]`, `MainWindow.cpp:2711`) and the canvas's key handling (`PdfCanvas.cpp:1862-1905`; PgDn/PgUp/Home/End are canvas keys, not accelerators).
- [ ] **Step 5: Commit** — `docs: continuous scroll in README, smoke checklist and CHANGELOG (#55)`.

## Task 5: The GUI drag gate (§9.4)

**Files:**
- Create (local, not committed): `~/.claude/compact-dumps/issue-165-36f011/gate-drag.ps1`
- Modify: spec §9.4 (the readings)

**Before running: ask the user in chat** whether they are at the machine (the run moves the real pointer and needs litepdf in the foreground for about a minute). Run only on a yes.

The driver (Per-Monitor-V2 aware, `Wake-Display` first in the measured run only — it sends real `mouse_event` nudges, so never in the dry-run — wrapped, `LITEPDF_NO_RESTORE=1`, `ContinuousScroll` absent) runs two arms on `tests\fixtures\strip-1000.pdf`, each a fresh launch: thumbnails pane **closed**, then **open** (post `IDM_VIEW_THUMBS` 40060 and poll until a visible `SysListView32` child exists).
1. Fixed window size as in Task 3; poll `canvas-scroll` until `vscroll` and the new client size.
2. `GetScrollBarInfo(canvas, OBJID_VSCROLL)` (`0xFFFFFFFB`; `cbSize` = 60): the thumb centre is `(rcScrollBar.left + rcScrollBar.right) / 2`, `rcScrollBar.top + (xyThumbTop + xyThumbBottom) / 2`; the target is the same x at `rcScrollBar.bottom − 1`.
3. Guards (Global Constraints) before each `SendInput`: foreground and `WindowFromPoint` == the canvas. Press at the thumb centre, then 50 absolute moves evenly to the target, 20 ms apart, then release at the target. `INPUT` is the 40-byte x64 union (`reference_litepdf_scripted_gui_smoke`, "Real keyboard/mouse").
4. From the release: poll every 100 ms; record when the page box first reads `1000` and when the process is first idle (≤ 20 ms CPU in a 1000 ms window) after that. Read `canvas-scroll`'s `pos` at the end.
5. Close with `WM_CLOSE`.

Pass, per arm: the page box reads `1000` and the process is idle, both within 5 s of release; `pos == max − page + 1` (the drag reached the end — the positive control: a drag that fell short fails here, not on timing). This is a sanity check, not a count gate (§9.4).

- [ ] **Step 1: Write the driver**; dry-run its geometry on one launch with no input sent (no `Wake-Display` either): print the scroll bar rect and the thumb centre, and assert `WindowFromPoint(thumb centre)` is the canvas. After this step the driver and the gate are frozen (decision 4).
- [ ] **Step 2: Ask the user**, then run both arms wrapped. A failed guard is an invalid run, not a fail: say so and ask before retrying.
- [ ] **Step 3: On a miss, stop** (decision 4): report the readings; no rerun, no change to the driver or the gate, no further task.
- [ ] **Step 4: Record** — in spec §9.4, below Task 3's readings: "GUI drag, PR 4 (strip-1000.pdf, DPI <d>): thumbnails closed — last page at <t1> s, idle at <t2> s; open — <t3> s, <t4> s."
- [ ] **Step 5: Commit** — `docs(spec): record the continuous scroll drag gate (#55)`.

## Task 6: Regression and the PR

**Files:**
- Modify: none, unless a check fails

- [ ] **Step 1: Test files** — `git fetch origin`, then `git diff --name-status origin/main...HEAD -- tests` (three dots: this branch's changes only, whatever `main` gained meanwhile) lists only `M tests/unit/test_view_prefs.cpp`, and `git diff origin/main...HEAD -- tests` shows only line 20's change.
- [ ] **Step 2: Full suite** — `& $ctest --test-dir build -C Release -N` (record the count), then the run → all pass.
- [ ] **Step 3: Full smoke, wrapped** — `scripts\smoke-test.ps1 -ExpectDev` → passes, including the 12 MB size gate, the cold-start line and Row 0; afterwards `ContinuousScroll`, `session.json` and the clipboard are back to their prior state and no crash dump is new.
- [ ] **Step 4: Crash-restore of a paged session** (Review Focus 3), wrapped with `-AllowRestore`: `ContinuousScroll = 0`; launch `large.pdf` without `LITEPDF_NO_RESTORE`; `page-box-go 30`, poll `"30"`; wait until `session.json` holds page index 29 (the ~1.5 s debounce); `Stop-Process -Force` (leaves `running.lock`), then `WaitForExit(5000)` on that process and fail if it is still alive (a launch that finds the dying instance's single-instance mutex forwards to it and exits 1, `smoke-test.ps1:104-106`); remove the value (the default); launch with no file; post `IDYES` (6) to the `#32770` prompt → poll `canvas-scroll` `vscroll` true, page box `"30"`, and `|pos − round(anchor(29))| <= 1` at FitWidth (`anchor(i) = i × (792 × z + 8)`, `z = (client_w × 96 / dpi) / 612`, as in PR 3's background-tab row).
- [ ] **Step 5: Paged spot check, wrapped, `ContinuousScroll = 0`** — posted messages only, each with a named oracle: on `search.pdf`, `canvas-key 0x22` → page box `"2"` and `canvas-scroll` `vscroll` false; `send-cmd 40062` (Two-Page Spread) → `menu-state 40062` checked, then again → unchecked; `menu-state 40066` → found, not grayed, not checked. On `selection.pdf` (clipboard saved before and restored in `finally`, as smoke Row 19 does): `Set-Clipboard` a marker, `send-cmd 40072` (Select All), `send-cmd 40071` (Copy) → poll `Get-Clipboard -Raw` until it differs from the marker and is non-empty. A drag selection and Find highlighting have no pixel-free oracle and need real input; PR 3's checks covered them and nothing here changes their paged code, so they are not repeated.
- [ ] **Step 6: Open the PR** — precondition: both gates are recorded in spec §9.4 as passes, or the user has ruled on a miss in writing (decision 4). Title `feat: continuous scroll on by default (#55, PR 4/4)`; the body says `Closes #55`, lists the four decisions, justifies the one existing-assertion change, quotes both gate readings and the `ctest -N` count, records Step 4's reading, states that paged mode's smoke coverage is now Row 0's second half and Row 24 (plus Step 5 locally), and names the open follow-ups (#167, #168, #169, #171).
