# LitePDF Implementation Roadmap

> **For Claude:** This is a multi-phase roadmap, not an executable plan. Each phase has its own detailed plan (`docs/plans/YYYY-MM-DD-phase-N-<name>.md`) produced via the `superpowers:writing-plans` skill when that phase begins. Execute one phase at a time; re-plan the next phase after the previous one lands.

**Goal:** Ship LitePDF v1.0 per [`2026-04-15-litepdf-design.md`](2026-04-15-litepdf-design.md).

## Sequencing Principle

Phases are ordered so that **each phase ends with a runnable, demoable artifact** and each phase can be verified in isolation. UI-agnostic core (Phases 1–2) is built and fully tested before any Direct2D code exists, so all rendering work in Phase 3 is against a known-good backend.

## Phase Graph

```
Phase 0  (bootstrap)
   │
   ▼
Phase 1  (Document core, TDD, headless)
   │
   ▼
Phase 2  (RenderEngine + PageCache, TDD, headless)
   │
   ▼
Phase 3  (MainWindow + PdfCanvas, single-tab Tier 1 viewer)  ◄── first demoable build
   │
   ├──▶ Phase 4  (Outline pane + MRU + drag-drop)
   │        │
   │        ▼
   │    Phase 5  (Multi-tab)
   │        │
   │        ▼
   │    Phase 6  (In-doc search → cross-tab search + dockable panel)
   │        │
   │        ▼
   │    Phase 7  (Thumbnail pane)
   │        │
   │        ▼
   │    Phase 8  (Encrypted PDF, ePub/CBZ/XPS, dark mode, dual-page) ◄── Tier 3 feature-complete
   │        │
   └────────┼──▶ Phase 9  (Icons: app + PDF document variant, multi-size .ico)
            │
            ▼
        Phase 10  (Inno Setup installer + GitHub Actions release)
            │
            ▼
        Phase 11  (Benchmark regression gate + cold-start tuning)
            │
            ▼
        Phase 12  (Release hardening: crash dump, session restore, smoke-test sweep)
            │
            ▼
            v1.0
```

## Phase Summaries

| # | Name | Deliverable | Exit Criteria |
|---|---|---|---|
| 0 | Bootstrap | CMake project, MuPDF submodule, hello-world window, CI green | `litepdf.exe` builds, runs, shows empty window ≤ 500 ms; CI passes |
| 1 | Document core | MuPDF wrapper, format detect, page count, text extract, outline parse | ≥ 80 % unit-test coverage on `core/Document.*`; headless CLI opens fixtures and prints metadata |
| 2 | RenderEngine + PageCache | Thread pool, LRU, `fz_pixmap` output, cancellation | Stress test: 1000 sequential page renders under 25 MB RAM cap |
| 3 | Minimal viewer (Tier 1) | MainWindow, PdfCanvas, Direct2D blit, keyboard nav, zoom, drag-drop | Open PDF, page up/down/zoom/drag-drop works; cold-start ≤ 1 s on HDD fixture |
| 4 | Outline + MRU | TreeView outline, click-to-jump, recent files registry | Outline clicks navigate correctly; MRU persists across restart |
| 5 | Multi-tab | TabManager, per-tab Document/cache, single-instance IPC | Open 5 tabs, switch, close middle one; no cross-tab state leak |
| 6 | Search | In-doc search (Ctrl+F), then cross-tab (Ctrl+Shift+F), dockable results panel | Search finds known fixture hits; panel docks/undocks; click navigates |
| 7 | Thumbnails | Owner-draw ListView, lazy render, F4 toggle, hidden by default | Thumbnails render on demand; opening doesn't block on thumb generation |
| 8 | Tier 3 completion | Password dialog, ePub/CBZ/XPS, dark mode, dual-page spread | All fixtures open; toggles work; no format-specific crashes |
| 8.5 | Print support (T2) | Standard `PrintDlg` + page range + copies + scale modes (fit/actual/custom%) + auto-rotate; new `src/printing/` module | **SHIPPED 2026-05-04** — Active doc prints to default printer + Microsoft Print to PDF; cancel mid-job works; spec at `docs/superpowers/specs/2026-05-03-print-support-design.md`; tag `v0.0.10-phase8.5` |
| 9 | Icons | "Lightning document" app icon + red PDF document variant; 7 sizes each; multi-res `.ico` | **SHIPPED 2026-05-31** — IDI_APPICON wired to WNDCLASSEX; IDI_PDFDOC embedded for Phase 10; spec at `docs/superpowers/specs/2026-05-05-phase-9-icons-design.md`; tag `v0.0.11-phase9` |
| 10 | Installer | Inno Setup script, per-user default, optional associations, informational license page (design §8.5), CI release job with source tarball attached | `litepdf-setup.exe` installs/upgrades/uninstalls cleanly on a fresh VM; license page follows design §8.5 wording (no "I agree" radio); release bundles source tarball per AGPL §13 |
| 11 | Benchmark gate | Cold-start benchmark harness, CI regression threshold (+10 % fails) | **DONE 2026-06-05** — CI benchmark gate live: rebuilds the PR's base SHA on the same runner and fails a PR that regresses `large.pdf` `open_render_ms` beyond +10 % AND +5 ms, or grows `litepdf.exe` beyond 256 KB. Baseline harness in PR1 ([#19](https://github.com/jeffchen1981-fu/litepdf/pull/19), measure-only); rebuild-base gate in PR2 ([#20](https://github.com/jeffchen1981-fu/litepdf/pull/20)). Deliberate +40 ms regression correctly blocked in CI ([run 26993548079](https://github.com/jeffchen1981-fu/litepdf/actions/runs/26993548079)). Infra-only: no VERSION bump/tag per plan. Cold-start *tuning* deferred to Phase 11.5+ |
| 11.5 | Size prune + cold-start instrumentation | Prune unused MuPDF features (JS/OCR/DOCX off + symbol fonts) + downgrade CJK to Droid Sans Fallback Full; add `T1→T2` cold-start sub-marks + an absolute size ceiling on the benchmark gate | **DONE 2026-06-08** — `litepdf.exe` 39.6 MB → **18.25 MB** (−54%) on branch `phase-11.5`. CJK renders Droid Sans Fallback Full (23.66 MB Source Han Serif dropped); zh-Hant/ja/ko fixtures render real glyphs, no tofu, no coverage gap in the sampled sets. Cold-start `T1→T2` unchanged (after ≈192 ms vs before ≈183 ms, within noise) — the size win is decoupled from cold-start (`T1→T2` is dominated by thread + UI-pump warm-up, not binary footprint). Benchmark gate gained an absolute `size_ceiling_bytes` (19 MB). The 8 MB exe target + full CJK drop (system-font loader) are deferred to post-v1.0 (**C-real**). Spec/plan at `docs/superpowers/{specs,plans}/2026-06-07-phase-11.5-size-prune-coldstart*` |
| 12 | Release hardening | Minidump on unhandled exception, session.json restore, full manual smoke pass | All 6 smoke-test checklist items pass; v1.0 tag + GitHub release |

## What Each Phase Costs (Rough LOC)

| Phase | LOC added | Cumulative |
|---|---|---|
| 0 | ~80 (mostly CMake/manifest) | 80 |
| 1 | ~600 (core/Document + tests) | 680 |
| 2 | ~700 (core/RenderEngine + tests) | 1380 |
| 3 | ~1400 (MainWindow + PdfCanvas + DocumentView) | 2780 |
| 4 | ~400 | 3180 |
| 5 | ~400 | 3580 |
| 6 | ~500 | 4080 |
| 7 | ~400 | 4480 |
| 8 | ~500 (roadmap-level estimate; PR #6 plan body refines to ~600) | 4980 |
| 8.5 | ~345 (`src/printing/` module + wiring) | 5325 |
| 9 | ~100 (resource embedding; icons are assets not code) | 5425 |
| 10 | ~150 (Inno Setup .iss is declarative) | 5575 |
| 11 | ~200 (benchmark harness) | 5775 |
| 12 | ~150 | 5925 |

Slightly over the design estimate (4500); the 5900-ish range accounts for tests, harness code the design section under-counted, and the Phase 8.5 print module added 2026-05-03 (was excluded from design v1 §10 but determined to be table-stakes for launch — see `docs/superpowers/specs/2026-05-03-print-support-design.md`). Still well under the 8 MB exe budget.

## Re-Planning Protocol

Before starting any phase after Phase 0:

1. Read the previous phase's exit commit and review what changed vs. the plan.
2. Re-invoke `superpowers:writing-plans` for the new phase.
3. Incorporate anything learned (API surprises, performance findings) into the new plan.
4. Do not attempt to execute more than one phase without re-planning.

## Known Limitations (Post-Phase-6)

Captured at `v0.0.7-phase6` tag. See `docs/plans/2026-04-24-phase-6-search-design.md` §9 for full context.

- **Whole-word and regex search are not supported. — RESOLVED in v1.1.0.** The MuPDF 1.27.2 upgrade activated the incremental `fz_search` matcher, so the FindBar and cross-tab results panel now support regex (`FZ_SEARCH_REGEXP`, ECMAScript grammar, run on Enter) and whole-word (regex `\bword\b`, live). Remaining limitation: whole-word boundaries are ASCII-only.
- **Case-sensitive search is a no-op in v1. — RESOLVED in v1.1.0.** The MuPDF 1.27.2 incremental matcher honors `FZ_SEARCH_EXACT` / `FZ_SEARCH_IGNORE_CASE`, so `SearchFlags::match_case` is now honored end-to-end (FindBar `Aa` toggle + cross-tab panel). The old `TODO(post-v1.0)` markers in `src/core/Document.cpp` are gone; the matcher operates on an extracted `fz_stext_page` as predicted.
- **ResultsPanel is not a true dockable panel.** It is a bottom-docked resizable pane only. A full docking framework (undock-to-float, left/right dock) is out of scope for v1 and would require ~1500 additional LOC.
- **L2 display list cache is not warmed by search.** Pages searched once but never viewed stay cold for subsequent renders. Intentional per design §D16 (search must not pollute render's hot cache).
- **Mid-page search cancellation is not honored by MuPDF 1.24.11's search path. — RESOLVED in v1.1.0.** `Document::page_hits` now checks `abort_flag` at the top of every incremental `fz_search_forwards` iteration, so an in-progress per-page search stops promptly when the query changes or a tab closes (cross-page cancellation still uses the `SearchSession` epoch bump). NB: the planned `fz_match_stext_page_cb` callback-abort proved to be a no-op in 1.27.2; the working path is the incremental `fz_new_search` / `fz_feed_search` / `fz_search_forwards` loop. Remaining limitation: a catastrophic-backtracking regex cannot be interrupted mid-match.
- **SearchDispatcher is 2-worker fixed.** Adequate per design §5.4 "tabs run in parallel", but the Phase 11 benchmark harness may surface data motivating DPI-/CPU-count-adaptive sizing post-v1.0.
- **Cross-tab results `N hits` counter is total-only.** Plan's ideal format is "m / n" (current / total) but `SearchSession` doesn't expose cursor index; a Phase 6.x follow-up adds `cursor_index()` and flips the counter format.
- **FindBar counter refreshes during cross-tab scan: resolved.** `CrossTabSearch::clear()` restores each tab's previous on_update observer, so the per-tab find-bar counter resumes updating the moment the results panel is dismissed. `dispatch()` also calls `clear()` first so repeated Ctrl+Shift+F invocations don't stack chained lambdas.

## Out of Scope (post-v1.0)

See §10 of the design doc. Post-v1.0 roadmap is not planned here.

**Deferred to post-v1.0 (v1.1 candidate): MuPDF 1.27+ upgrade.** Phase 11 was
re-scoped to the benchmark regression gate only; its original "MuPDF upgrade"
sub-goal was dropped and is NOT part of v1.0. Upgrading from the pinned 1.24.11
to MuPDF 1.27+ (experimental stext matcher: `fz_match_stext_page` /
`fz_match_stext_page_cb` + the `fz_search_options` enum) clears the search
known-limitations above in one move: case-sensitive (`FZ_SEARCH_EXACT`), regex
(`FZ_SEARCH_REGEXP`), whole-word (regex `\bword\b`), and true mid-page
cancellation (callback returns 1). Held back because it is a 3-minor dependency
jump onto an API still flagged experimental as of 1.27.0 (2025-12) — best done
after v1.0 ships rather than during Phase 12 release hardening. Latest upstream
at time of writing: 1.27.2 (2026-02).
