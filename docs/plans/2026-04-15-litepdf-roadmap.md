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
| 9 | Icons | "Lightning document" app icon + red PDF document variant; 7 sizes each; multi-res `.ico` | Icons visible in Explorer at all DPIs; installer welcome shows icon |
| 10 | Installer | Inno Setup script, per-user default, optional associations, CI release job | `litepdf-setup.exe` installs/upgrades/uninstalls cleanly on a fresh VM |
| 11 | Benchmark gate | Cold-start benchmark harness, CI regression threshold (+10 % fails) | Baseline captured; regression PR correctly blocked in CI |
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
| 8 | ~500 | 4980 |
| 9 | ~100 (resource embedding; icons are assets not code) | 5080 |
| 10 | ~150 (Inno Setup .iss is declarative) | 5230 |
| 11 | ~200 (benchmark harness) | 5430 |
| 12 | ~150 | 5580 |

Slightly over the design estimate (4500); the 5500-ish range accounts for tests and harness code the design section under-counted.

## Re-Planning Protocol

Before starting any phase after Phase 0:

1. Read the previous phase's exit commit and review what changed vs. the plan.
2. Re-invoke `superpowers:writing-plans` for the new phase.
3. Incorporate anything learned (API surprises, performance findings) into the new plan.
4. Do not attempt to execute more than one phase without re-planning.

## Out of Scope (post-v1.0)

See §10 of the design doc. Post-v1.0 roadmap is not planned here.
