# LitePDF

A lightweight PDF / ePub / CBZ / XPS reader for Windows 11, optimized for mechanical hard drives. Single self-contained executable, no runtime dependencies.

- **Status:** under development (Phase 10 — installer + first public release `v0.0.12-phase10`)
- **License:** [AGPL-3.0](LICENSE)
- **Design:** [`docs/plans/2026-04-15-litepdf-design.md`](docs/plans/2026-04-15-litepdf-design.md)
- **Roadmap:** [`docs/plans/2026-04-15-litepdf-roadmap.md`](docs/plans/2026-04-15-litepdf-roadmap.md)
- **Changelog:** [`CHANGELOG.md`](CHANGELOG.md)

## Install

Download the latest release from
[GitHub Releases](https://github.com/jeffchen1981-fu/litepdf/releases):

- **`litepdf-setup-<version>.exe`** — installer (per-user by default, no admin
  needed; advanced users can opt into a per-machine install). Optional file
  associations for `.pdf` / `.epub` / `.cbz` / `.xps`.
- **`litepdf-portable-<version>.zip`** — just `litepdf.exe`, no install. Unzip
  and run.
- **`litepdf-<version>-source.tar.gz`** — complete corresponding source
  (AGPL-3.0), MuPDF submodule included.

**System requirements:** Windows 10 version 1903 or later, 64-bit.

> **SmartScreen note:** LitePDF is not code-signed, so Windows SmartScreen may
> show "Windows protected your PC". Click **More info → Run anyway** to proceed.
> The binary is the one built by the [release workflow](.github/workflows/release.yml)
> from the tagged source.

## Features (v1.1.0)

Open and read PDFs, ePub, CBZ, and XPS via MuPDF. Multi-tab interface, per-tab independent state. Cold-start budget under 1 s on SSD; tuned for HDD-friendly I/O patterns.

- **Encrypted PDFs** — modal password prompt with 3-attempt retry (Phase 8)
- **Search** — in-document find with highlighted hits + cross-tab search panel; case-sensitive, regex (ECMAScript), and whole-word toggles, with regex run on Enter (v1.1.0)
- **Outline / bookmarks** — click-to-navigate side pane (F5)
- **Thumbnails** — lazy-rendered side pane (F4)
- **Invert colors** — per-tab dark-mode toggle (Ctrl+Shift+I, Phase 8)
- **Two-page spread** — book-style side-by-side layout with cover-page rule (Ctrl+Shift+D, Phase 8)
- **Print** — standard `PrintDlgEx` with page range, copies, scale modes (fit / actual / custom %), auto-rotate, and mid-job cancel (Ctrl+P, Phase 8.5)
- **MRU** — recent files in File menu, persisted across runs

## Keyboard shortcuts

| Key                | Action                              |
|--------------------|-------------------------------------|
| Ctrl+O             | Open file                           |
| Ctrl+P             | Print active document               |
| Ctrl+W             | Close active tab                    |
| Ctrl+Tab / Ctrl+Shift+Tab | Cycle tabs                   |
| Ctrl+1..9          | Jump to tab N                       |
| Ctrl+F             | Find in document                    |
| F3 / Shift+F3      | Find next / previous                |
| Ctrl+Shift+F       | Cross-tab search                    |
| F6                 | Toggle search results panel         |
| F4                 | Toggle thumbnails pane              |
| F5                 | Toggle outline pane                 |
| Ctrl+Shift+I       | Invert colors (per tab)             |
| Ctrl+Shift+D       | Two-page spread (per tab)           |
| Ctrl+= / Ctrl+-    | Zoom in / out                       |
| Ctrl+0             | Reset zoom                          |
| PgDn / PgUp        | Next / previous page (or pair in spread mode) |

## Build

### Prerequisites

- Windows 10 version 1903+ or Windows 11 (64-bit)
- Visual Studio 2022 Build Tools with the **Desktop development with C++** workload (includes MSVC v143, Windows 11 SDK, and CMake ≥ 3.25).
- Git 2.30+ (for submodule support).

### Commands

```sh
git clone --recursive https://github.com/jeffchen1981-fu/litepdf
cd litepdf
cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
```

The produced `build/Release/litepdf.exe` is a single self-contained binary.

## Tests

```sh
ctest --test-dir build -C Release --output-on-failure
```

162 unit tests at `v0.0.11-phase9` (the `src/printing/` suite added in Phase 8.5 covers geometry, range parser, and abort flag; Phase 9 added icons only, no new unit tests). The CI workflow (`.github/workflows/ci.yml`) runs configure + build + version-sync gate + ctest on `windows-latest` for every push and pull request.

The PowerShell smoke harness exercises the full app via launch-and-poll:

```pwsh
pwsh scripts/smoke-test.ps1
```

It opens `simple.pdf`, `bookmarks.pdf`, `search.pdf`, `sample.epub`, `sample.cbz`, and `encrypted.pdf` (modal expected), and exercises Ctrl+F / Ctrl+Shift+F / F4 toggles via a small ux-probe helper.

## Versioning

`VERSION` contains a semver string optionally suffixed with `-dev`, `-rc1`, etc. The CMake build reads this file and strips the pre-release suffix before passing the numeric triple to `project(VERSION ...)` (required by CMake).
