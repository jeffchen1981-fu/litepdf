# Phase 8 Plan — 3-Layer Stack Review Log

**Date:** 2026-05-01
**Plan reviewed:** [`2026-05-01-phase-8-tier3-completion.md`](2026-05-01-phase-8-tier3-completion.md)
**Plan baseline:** version produced by `general-purpose` plan-writing subagent (1101 lines, 13 Ds, 6 tasks)
**Plan tip after review:** 1466 lines, 13 Ds, 6 tasks (+136 lines absorbed across 25 revisions)

This log documents the three-layer review per CLAUDE.md rule 14 (Stack reviewers — never rely on a single reviewer for substantial work). Each layer found findings; all 25 were applied to the plan inline. See git history of the plan file for the exact text deltas.

## Layer A — `/plan-eng-review` (architecture / code-quality / tests / performance)

**Reviewer lens:** eng-manager — architecture, data flow, edge cases, test coverage, performance.
**Outcome:** 9 findings → 9 revisions applied as **R1-R9**.

### Findings + revisions

| # | Severity | Finding (1-line) | Revision applied |
|---|---|---|---|
| A1 | P1 | PageCache invert axis should key L1 only, not L2 (display lists are polarity-independent) | **R1**: D8 + T3.1 distinguish L1 vs L2 explicitly |
| A2 | P2 | `open_tab_async` UI/worker thread marshalling not specified | **R2**: D2 addendum on UI-thread inline retry loop |
| A3 | P3 | `set_zoom_mode(FitPage)` in dual mode not in T4 step list | **R3**: T4.2.5 inserted with halved per-slot DIP budget logic |
| Q1 | P3 | Step 0.3 ACCEL block had Ctrl+2 → "use Ctrl+D" mid-step warning, easy to misread | **R4**: rewritten as direct Ctrl+Shift+D + history note |
| Q2 | P3 | Step 1.5 `status = ... : L""` is dead-code on attempt 3 | **R5**: `if (attempt < 3) status = ...` (no else) |
| Q3 | P3 | `((page-1) & ~1) + 1` formula uses `int ~1` (signed) | **R6**: D9 addendum on cancellation drain pattern (not Q3 directly — Q3 trivial cosmetic deferred) |
| Q4 | P2 | `set_invert_colors` cancellation discipline not explicit | **R6**: D9 addendum aligns with Phase 7 D16/D17 drain pattern |
| T1 | P2 | Only 1 `[page_cache][invert]` test in T3; no render-engine integration test | **R7**: T3.5 added with [render_engine][invert] integration tests |
| T2 | P3 | Password retry loop not unit-testable (sits inside open_tab_async with modal call) | **R8**: T1.5 extracts `try_authenticate_with_retry` pure fn + 4 tests |
| P2 | P2 | Cache capacity 5/10 churn risk under dual × invert | **R9**: §"Known Limitations" expanded |

## Layer B — `/plan-design-review` (UI/UX / accessibility / interaction)

**Reviewer lens:** designer — visual hierarchy, error states, micro-interactions, AI slop patterns.
**Outcome:** 13 findings → 10 revisions applied as **R10-R19**. (D11/D12 minor consistency notes folded into R18/R19; D13 produced no finding — risk-annotation section judged honest.)

### Findings + revisions

| # | Severity | Finding (1-line) | Revision applied |
|---|---|---|---|
| D1 | P2 | Dialog DIP-unit footprint not validated across 96/144/192 DPI | (Folded into T1.4 testing note; no separate R) |
| D2 | P1 | "Incorrect password (2 of 3)" reads as anxiety-counter; "remaining" is friendlier | **R10**: prompt callback uses "(N attempts remaining.)" with grammatical singular on last |
| D3 | P2 | 3-fail tab close is silent — user gets no feedback | **R11**: status-bar flash after exhausted attempts (MessageBox fallback) |
| D4 | P2 | Edit control lacks cue banner / accLabel for password managers | **R12**: `EM_SETCUEBANNER` "Password" placeholder in `WM_INITDIALOG` |
| D5 | P1 | "Invert Colors" semantics — channel invert ≠ dark mode; menu label needs UX-honest framing | **R13**: D7 expanded with user-facing label note |
| D6 | P2 | Tab-switch invert visual feedback ambiguity | (Accepted minimal: chrome flip is the feedback) |
| D7 | P3 | Smoke step 5 doesn't verify chrome flip alongside canvas | **R14**: step 5 expanded |
| D8 | P1 | First Ctrl+Shift+D press at page 0 looks like nothing happened | **R15**: T4.1 cover-mode placeholder in right slot (Acrobat-style) |
| D9 | P2 | PgUp/PgDn at boundary is silent dead-key | **R16**: `flash_status_bar` "Already at first/last page." (800 ms) |
| D10 | P2 | Search hit-jump in dual mode — user can't tell which slot has highlight | **R17**: FindBar status text appends `(left)` or `(right)` in dual mode |
| D11 | P3 | `Ctrl+D` modifier inconsistent with `Ctrl+Shift+I` (file/edit vs view convention) | **R18**: changed to `Ctrl+Shift+D` everywhere (modifier-consistent group; reserves bare Ctrl+D for future "Duplicate Tab") |
| D12 | P3 | Menu checkmark style not pinned to existing F4/F5 convention | **R19**: T3.4 + T4.4 explicitly cross-reference `IDM_VIEW_OUTLINE`/`IDM_VIEW_THUMBS` `CheckMenuItem` style |
| D13 | P3 | Risk annotations honest, no slop | (no R — finding was "no issue") |

### Design score (post-revisions)

| Dimension | Pre-R10..R19 | Post |
|---|---|---|
| Visual hierarchy | 6/10 | 8/10 |
| Interaction consistency | 6/10 | 9/10 |
| Error states | 5/10 | 8/10 |
| Micro-interactions | 7/10 | 9/10 |
| AI slop patterns | 9/10 | 9/10 |
| Accessibility | 4/10 | 7/10 |
| **Overall** | **6.2** | **8.3** |

## Layer C — Codex-style API surface verification (independent / adversarial)

**Reviewer lens:** API-drift gatekeeper — does every claimed signature, line number, fixture path actually exist?
**Outcome:** 15 drift findings (4 P0, 8 P1, 3 P2) + 5 ambiguous notes → 15 revisions applied as **C1-C15**. Background subagent transcript at [`2026-05-01-phase-8-codex-api-verify.md`](2026-05-01-phase-8-codex-api-verify.md).

### Highlight: All 4 P0 findings clustered in T3 (dark mode)

Phase 7 saw 6 plan-author API drift cases — Phase 8's plan-writing subagent inherited the pattern. This layer was the most valuable of the three. **Without layer C, T3 would have failed to compile in 4 separate places at execution time.**

| # | Severity | Drift | Fix (in plan) |
|---|---|---|---|
| C1 | P0 | `d.context()` does not exist; actual API is `clone_context()` | T3.5 test snippet rewritten to use `Document::clone_context()` |
| C2 | P0 | `RenderEngine eng(d.context(), cache, 1)` — wrong constructor | T3.5 uses `RenderEngine(Document&, num_workers, PageCache*)` |
| C3 | P0 | `cache.l1_hit_count()` does not exist | T3.5 cache-hit verification uses pointer identity + `l1_size()` |
| C4 | P0 | "after `fz_run_page`" — `fz_run_page` not called anywhere | T3.1 + D7 anchor on `fz_new_pixmap_from_display_list` (RenderEngine.cpp:189) |
| C5 | P1 | Cache touch-point lines L113/L129/L155/L192 → drifted to 121/139/167/208 | D8 + T3.1 table updated; history note added |
| C6 | P1 | About-dialog literal at MainWindow.cpp:917 → :936 | Pre-flight + Step 5.2 corrected |
| C7 | P1 | `tests/unit/test_document_formats.cpp` already exists with 2 cases | T2 reframed: extend existing file, no `tests/CMakeLists.txt` change, +2 not +4 |
| C8 | P1 | Plan claimed Ctrl+T bound; actually unbound | Step 0.3 + smoke step 5 corrected |
| C9 | P1 | `accels[]` line "around 1360" → 1379-1408 | Pre-flight + Step 0.3 corrected |
| C10 | P1 | `looks_like_supported_document` is anonymous-namespace free function, not `Document::` member | Pre-flight + D4 corrected |
| C11 | P1 | `Test-Step` / `Start-Litepdf` helpers don't exist in `smoke-test.ps1` | T5.1 rewritten with raw `Start-Process` + MainWindowHandle poll mirroring lines 38-60 of existing script |
| C12 | P1 | `request_render(int, RenderCb)` has no priority arg | T4.2 reframed: priorities are internal to DocumentView; T4 doesn't thread them |
| C13 | P2 | T4.2.5 `FitMode::FitPage`/`compute_fit_zoom`/`client_dip_width` were pseudocode | T4.2.5 uses `ZoomMode`/`set_zoom_mode` actual names with "read DocumentView.cpp before editing" warning |
| C14 | P2 | T2 cumulative count drifted across baseline change | T2 chain re-anchored: 137 → 139; T3: 139 → 142; T4: 142 → 145 |
| C15 | P2 | "near line 300" → "exactly line 300" for `fz_invert_pixmap` | Pre-flight #4 corrected |

### Ambiguous / not actionable

- `Document::OpenError::BadPassword` exists in enum but never returned (Document::open returns `NeedsPassword` instead) — dead enum value worth cleanup but out-of-scope for Phase 8.
- Whether thumbnails inherit per-tab invert is unspecified in T3 — implementer judgment call.
- ZIP-magic check for `.cbz` (`Document.cpp:143-149`) couldn't be verified without binary inspection of the fixture; relies on plan's pre-flight #3.
- Win32 dialog struct fields and style constants are MSDN-documented but not greppable without Windows SDK locally.
- All Phase-8-introduced symbols (`PasswordDialog`, `try_authenticate_with_retry`, `RenderRequest::invert`, etc.) flagged as "PLANNED — not yet present" (correct).

## Net effect

- **Plan delta:** 1101 → 1466 lines (+136 = +33% — most absorbed in T1 password_retry extraction + T3 RenderEngine integration tests + T4.2.5 zoom-mode interaction).
- **Tests added vs original draft:** original plan claimed +11 tests (130 → 141). Post-review claim: +15 tests (130 → 145, or 131 → 146 if `phase-7-polish` PR has merged before Phase 8 start).
- **Critical bugs prevented:** 4 P0 compile failures in T3 + 1 logical inconsistency (cache key axis on L2 would have wasted slots and lost display-list reuse).
- **Design score:** 6.2 → 8.3 (post-R10-R19).

## Process learnings

1. **Plan-author API drift is endemic at the plan-writing-subagent layer.** Phase 7 caught 6 cases; Phase 8 caught 15. The codex-style independent verification is the highest-ROI single review type.
2. **Stack reviewers (CLAUDE.md rule 14) is empirically validated again.** Layer A (eng-review) caught 9 issues; layer B (design-review) caught 10 different ones; layer C (API-drift) caught 15 different ones. Net: 34 distinct findings, with near-zero overlap. A single reviewer at any layer would have shipped most of the others.
3. **The "/codex consult" layer is best run in parallel as a background subagent**, since it's static analysis on a known artifact (the plan) and doesn't need user interaction. Layers A and B are inherently interactive (0-10 ratings, scope decisions).
4. **Cumulative test-count chains drift fast under revision.** Every time a task's test count changes, the downstream chain breaks. Use a derivation table at plan-tail with the per-task delta + cumulative, both plan-time-constant and revision-friendly.
