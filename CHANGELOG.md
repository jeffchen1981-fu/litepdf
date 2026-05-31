# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html). Each
release also has a matching `vX.Y.Z-phaseN[.M]` git tag corresponding to a
phase in [docs/plans/2026-04-15-litepdf-roadmap.md](docs/plans/2026-04-15-litepdf-roadmap.md).

## [Unreleased]

### Changed
- **Embedded Win32 version resource now derives from `VERSION` at build time.**
  `resources/litepdf.rc` became a `configure_file` template (`litepdf.rc.in`);
  CMake fills `FILEVERSION` / `PRODUCTVERSION` / `FileVersion` / `ProductVersion`
  from `PROJECT_VERSION`, so `litepdf.exe`'s embedded version can no longer drift
  from the canonical `VERSION` file (the class of manual fix that PR #13 required).
  Added `CMAKE_CONFIGURE_DEPENDS` on `VERSION` so a bare `cmake --build`
  regenerates the resource instead of shipping a stale one.
- `scripts/check-version-sync.ps1` now also gates the version resource: it
  asserts the template stays parametric (no hardcoded numbers), verifies the
  generated `build/litepdf.rc` matches `VERSION` across all four version fields,
  fails loudly in CI when the generated resource is missing, and returns a
  deterministic exit code. Stays Windows PowerShell 5.1 compatible.

## [0.0.10-phase8.5] — 2026-05-05 — Print support

### Added
- **Print support** via `PrintDlgEx`: page range, copies, scale modes
  (fit / actual / custom %), and 90° auto-rotate. Bound to `Ctrl+P`.
- **Mid-job cancel**: shared `PrintAbortFlag` plus `SetAbortProc` callback so
  cancellation is observed sub-page even when the printer driver buffers.
- **Pre-flight scale picker** dialog (`PrintProgressDlg` config mode), then
  modeless progress dialog driven by `PrintJob` between pages.
- New `src/printing/` module (`PrintGeometry`, `PrintRange`, `PrintAbortFlag`,
  `PrintJob`, `PrintProgressDlg`) — ~345 LOC.
- 13 new unit tests for geometry composition, page-range parsing, and abort
  flag semantics.

### Changed
- File menu now contains a `Print...` entry, grayed out until a document is open
  (`Document::is_ready()`).
- `Document` exposes `clone_context()` so the print loop can render on a
  cloned `fz_context` (MuPDF cross-thread requirement).

### Fixed
- `EndPage` failure and dialog-creation OOM now route through the same error
  matrix as render failures (T9).

### Notes
- Spec: [docs/superpowers/specs/2026-05-03-print-support-design.md](docs/superpowers/specs/2026-05-03-print-support-design.md)
- Plan: [docs/superpowers/plans/2026-05-03-print-support-phase8.5.md](docs/superpowers/plans/2026-05-03-print-support-phase8.5.md)
- Win32 / MuPDF / driver pitfalls hit during the ship are captured in
  `memory/reference_litepdf_win32_print_gotchas.md`.

[Compare 0.0.9-phase8…0.0.10-phase8.5](https://github.com/jeffchen1981-fu/litepdf/compare/v0.0.9-phase8...v0.0.10-phase8.5)

## [0.0.9-phase8] — 2026-05-04 — Tier 3 completion

### Added
- **Encrypted PDFs**: modal password prompt with 3-attempt retry.
- **ePub / CBZ / XPS** opening via MuPDF's existing format support.
- **Invert colors** per-tab dark-mode toggle (`Ctrl+Shift+I`).
- **Two-page spread** book-style layout with cover-page rule (`Ctrl+Shift+D`).
- README refresh covering the full Phase 8 feature set, keyboard shortcuts,
  and smoke-test harness ([#9](https://github.com/jeffchen1981-fu/litepdf/pull/9)).

### Notes
- Tier 3 feature-complete per design §2; everything left for v1.0 is hardening.
- Plan: [docs/plans/2026-05-01-phase-8-tier3-completion.md](docs/plans/2026-05-01-phase-8-tier3-completion.md)
- Three-layer stack review log: [docs/plans/2026-05-01-phase-8-review-log.md](docs/plans/2026-05-01-phase-8-review-log.md)

[Compare 0.0.8-phase7…0.0.9-phase8](https://github.com/jeffchen1981-fu/litepdf/compare/v0.0.8-phase7...v0.0.9-phase8)

## [0.0.8-phase7] — 2026-04-27 — Thumbnails

### Added
- **Thumbnail pane** (owner-draw ListView, lazy render, F4 toggle), hidden by
  default. Mutually exclusive with the outline pane.
- `ThumbnailModel` (pure-logic geometry), `ThumbCache` (HBITMAP LRU), and
  `ThumbnailRenderer` (reuses `RenderEngine` + cancellable task drain).
- `RenderEngine::bypass_cache` flag for thumbnail-only renders that should not
  pollute the page LRU.
- DPI-aware thumb tile sizing (`set_tile_dip`) with `WM_DPICHANGED` plumbing.

### Changed
- `SplitterCore` extracted ahead of Phase 7 so vertical and horizontal
  splitters share the same drag/clamp logic ([#4](https://github.com/jeffchen1981-fu/litepdf/pull/4)).
- Outline pane (F5) and thumbnail pane (F4) are mutually exclusive — opening
  one closes the other.

### Fixed
- `toggle_outline` now populates from the active view before showing.
- `WM_DPICHANGED` dispatches to all tabs' thumb panes, not just the active one.
- `ThumbnailModel::set_tile_dip` clamps to ≥ 1 dip.
- Phase-7 polish round (5 reviewer follow-ups + first-F5 OutlinePane fix,
  [#5](https://github.com/jeffchen1981-fu/litepdf/pull/5)).

[Compare 0.0.7-phase6…0.0.8-phase7](https://github.com/jeffchen1981-fu/litepdf/compare/v0.0.7-phase6...v0.0.8-phase7)

## [0.0.7-phase6.1-indoc] — 2026-04-25

### Added
- In-document find polish: FindBar visibility against white PDF backgrounds
  (`WS_CLIPSIBLINGS` / `WS_CLIPCHILDREN`).
- ResultsPanel stale-count refresh when `page_hits` changes.

### Fixed
- `Document::Impl::doc_mutex` serializes `impl_->ctx` access (eliminates
  cross-thread MuPDF context race surfaced by cross-tab search).
- Open-failed `MessageBox` now includes the failing path.

## [0.0.7-phase6] — 2026-04-25 — Search

### Added
- **In-document find** (`Ctrl+F`) with hit highlighting + `F3` / `Shift+F3`
  next/previous.
- **Cross-tab search** (`Ctrl+Shift+F`) with a dockable ResultsPanel (`F6`).
- Click-to-navigate from search hits.

### Notes
- Plan: [docs/plans/2026-04-24-phase-6-search.md](docs/plans/2026-04-24-phase-6-search.md)
- Design: [docs/plans/2026-04-24-phase-6-search-design.md](docs/plans/2026-04-24-phase-6-search-design.md)

[Compare 0.0.6-phase5.4…0.0.7-phase6](https://github.com/jeffchen1981-fu/litepdf/compare/v0.0.6-phase5.4...v0.0.7-phase6)

## [0.0.6-phase5.4] — 2026-04-22 — Tab strip polish

### Changed
- Owner-draw tab strip with hover/active styling and a balanced close-button
  hit area ([#1](https://github.com/jeffchen1981-fu/litepdf/pull/1)).

## [0.0.6-phase5.3] — 2026-04-19

### Fixed
- `I-A` close-during-render race resolved (per-render context escrow lifetime
  documented in plan).

## [0.0.6-phase5.2] — 2026-04-19

### Notes
- Per-render escrow verification record (manual stress pass, no code change).

## [0.0.6-phase5.1] — 2026-04-18

### Notes
- Phase-5 follow-up: `Done-when` wording + `I-2` residual race documented in
  the plan.

## [0.0.6-phase5] — 2026-04-18 — Multi-tab

### Added
- **Multi-tab** support with `TabManager`, per-tab `Document` / `PageCache`,
  and single-instance IPC so a second `litepdf.exe` opens the file in the
  existing window.
- `Ctrl+Tab` / `Ctrl+Shift+Tab` cycling and `Ctrl+1..9` direct tab jumps.

### Changed
- MRU paths canonicalized before storage to dedupe cross-case Windows paths.
- About dialog and `VERSIONINFO` resource bumped to v0.0.6.

[Compare 0.0.5-phase4.1…0.0.6-phase5](https://github.com/jeffchen1981-fu/litepdf/compare/v0.0.5-phase4.1...v0.0.6-phase5)

## [0.0.5-phase4.1] — 2026-04-16

### Fixed
- MRU separator placement: separators now bracket entries dynamically so an
  empty MRU shows the right menu shape.

## [0.0.5-phase4] — 2026-04-16 — Outline + MRU

### Added
- **Outline / bookmarks** pane (F5) with click-to-jump navigation backed by
  `Document::outline()`.
- **MRU**: recent files in the File menu, persisted across runs.
- **Drag-and-drop** open from Explorer.
- Smoke test extended to cover `bookmarks.pdf` outline fixture.

[Compare 0.0.4-phase3.1…0.0.5-phase4](https://github.com/jeffchen1981-fu/litepdf/compare/v0.0.4-phase3.1...v0.0.5-phase4)

## [0.0.4-phase3.1] — 2026-04-16

### Fixed
- `std::once_flag` lifetime in window class registration.
- Long-path support in the open dialog.
- About-dialog text and zoom-doc wording.

## [0.0.4-phase3] — 2026-04-16 — Minimal viewer (Tier 1)

### Added
- **MainWindow + PdfCanvas** with Direct2D blit pipeline.
- Keyboard navigation (PgUp/PgDn/Home/End), zoom (`Ctrl+=` / `Ctrl+-` /
  `Ctrl+0`), and drag-drop file open.
- Cold-start budget verified ≤ 1 s on the HDD fixture.
- First demoable build: opens a PDF, renders pages, navigates, zooms.

[Compare 0.0.3-phase2…0.0.4-phase3](https://github.com/jeffchen1981-fu/litepdf/compare/v0.0.3-phase2...v0.0.4-phase3)

## [0.0.3-phase2] — 2026-04-16 — RenderEngine + PageCache

### Added
- **RenderEngine** thread pool with priority queue and cancellation support.
- **PageCache** two-level (L1 in-memory pixmap + L2 background-prefetch) with
  LRU eviction.
- `fz_pixmap` output target.
- Stress test: 1000 sequential page renders under the 25 MB RAM cap.

[Compare 0.0.2-phase1.1…0.0.3-phase2](https://github.com/jeffchen1981-fu/litepdf/compare/v0.0.2-phase1.1...v0.0.3-phase2)

## [0.0.2-phase1.1] — 2026-04-15

### Fixed
- Four post-tag cleanup commits addressing Phase 1 code review (refcount
  discipline, error-path tightening, Unicode-path test).

## [0.0.2-phase1] — 2026-04-15 — Document core

### Added
- **MuPDF-backed `Document`** wrapper with format detection (PDF / ePub / CBZ
  / XPS), page count, text extraction, and outline parsing.
- Headless CLI that opens fixtures and prints metadata.
- ≥ 80 % unit-test coverage on `core/Document.*`.

[Compare 0.0.1-phase0…0.0.2-phase1](https://github.com/jeffchen1981-fu/litepdf/compare/v0.0.1-phase0...v0.0.2-phase1)

## [0.0.1-phase0] — 2026-04-15 — Bootstrap

### Added
- CMake project scaffold + MuPDF submodule wiring.
- Hello-world Win32 window with `WIN32_LEAN_AND_MEAN` and DPI-aware manifest.
- GitHub Actions CI (configure + build + ctest) green on `windows-latest`.
- Cold-start ≤ 500 ms on an empty window.

[Compare initial commit…0.0.1-phase0](https://github.com/jeffchen1981-fu/litepdf/compare/v0.0.1-phase0)

[Unreleased]: https://github.com/jeffchen1981-fu/litepdf/compare/v0.0.10-phase8.5...HEAD
[0.0.10-phase8.5]: https://github.com/jeffchen1981-fu/litepdf/releases/tag/v0.0.10-phase8.5
[0.0.9-phase8]: https://github.com/jeffchen1981-fu/litepdf/releases/tag/v0.0.9-phase8
[0.0.8-phase7]: https://github.com/jeffchen1981-fu/litepdf/releases/tag/v0.0.8-phase7
[0.0.7-phase6.1-indoc]: https://github.com/jeffchen1981-fu/litepdf/releases/tag/v0.0.7-phase6.1-indoc
[0.0.7-phase6]: https://github.com/jeffchen1981-fu/litepdf/releases/tag/v0.0.7-phase6
[0.0.6-phase5.4]: https://github.com/jeffchen1981-fu/litepdf/releases/tag/v0.0.6-phase5.4
[0.0.6-phase5.3]: https://github.com/jeffchen1981-fu/litepdf/releases/tag/v0.0.6-phase5.3
[0.0.6-phase5.2]: https://github.com/jeffchen1981-fu/litepdf/releases/tag/v0.0.6-phase5.2
[0.0.6-phase5.1]: https://github.com/jeffchen1981-fu/litepdf/releases/tag/v0.0.6-phase5.1
[0.0.6-phase5]: https://github.com/jeffchen1981-fu/litepdf/releases/tag/v0.0.6-phase5
[0.0.5-phase4.1]: https://github.com/jeffchen1981-fu/litepdf/releases/tag/v0.0.5-phase4.1
[0.0.5-phase4]: https://github.com/jeffchen1981-fu/litepdf/releases/tag/v0.0.5-phase4
[0.0.4-phase3.1]: https://github.com/jeffchen1981-fu/litepdf/releases/tag/v0.0.4-phase3.1
[0.0.4-phase3]: https://github.com/jeffchen1981-fu/litepdf/releases/tag/v0.0.4-phase3
[0.0.3-phase2]: https://github.com/jeffchen1981-fu/litepdf/releases/tag/v0.0.3-phase2
[0.0.2-phase1.1]: https://github.com/jeffchen1981-fu/litepdf/releases/tag/v0.0.2-phase1.1
[0.0.2-phase1]: https://github.com/jeffchen1981-fu/litepdf/releases/tag/v0.0.2-phase1
[0.0.1-phase0]: https://github.com/jeffchen1981-fu/litepdf/releases/tag/v0.0.1-phase0
