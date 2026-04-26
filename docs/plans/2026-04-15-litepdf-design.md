# LitePDF — Design Document

**Date**: 2026-04-15
**Status**: Approved, ready for implementation plan
**Author**: Design captured via brainstorming session

## 1. Goals & Constraints

LitePDF is a lightweight PDF / ePub / CBZ / XPS reader for Windows, targeted at modest hardware (Windows 11, ~8GB RAM, mechanical HDD). It is inspired by SumatraPDF's philosophy: a single self-contained executable that cold-starts fast on spinning disks.

**Non-goals (v1)**: PDF editing, annotation authoring, form filling, digital signatures, OCR.

### 1.1 Quantitative Targets

| Metric | Target |
|---|---|
| Executable size | ≤ 8 MB (single file, static CRT) |
| Installer size | ≤ 9 MB (Inno Setup) |
| Cold-start to visible window (HDD) | ≤ 500 ms |
| Cold-start to first page rendered (HDD) | ≤ 1 s |
| Idle RAM (one tab, one page) | ≤ 80 MB |
| External dependencies at runtime | None (no VC++ Redist required) |

### 1.2 License

AGPL-3.0, published on GitHub. MuPDF (also AGPL) is statically linked — compatible.

## 2. Tech Stack

| Layer | Choice | Rationale |
|---|---|---|
| Language | C++17 | Broad compiler support, MuPDF C API compatible |
| Compiler | MSVC 2022 (clang-cl acceptable) | Native Windows, best Direct2D support |
| Build system | CMake | Multi-generator, IDE-friendly, CI-friendly |
| PDF/ePub/CBZ/XPS engine | MuPDF (static link) | Smallest footprint, covers all target formats |
| Rendering | Direct2D + DirectWrite | Hardware accelerated, built into Win7+ |
| UI chrome | Win32 + Common Controls v6 | Zero runtime cost — comctl32.dll already loaded |
| C runtime | Static `/MT` | No redistributable required |
| Strings | UTF-16 (`wchar_t`) at Win32 boundary; UTF-8 internally | Win32 native; simpler MuPDF interop |

### 2.1 MuPDF Integration & Deferred Feature Pruning

MuPDF 1.24.11 lacks a CMakeLists.txt. Its Windows build is a VS solution at `third_party/mupdf/platform/win32/mupdf.sln` whose projects are consumed directly via CMake's `include_external_msproject`. The five projects we pull in:

| Project | Output `.lib` | Purpose |
|---|---|---|
| `bin2coff.vcxproj` | (build tool) | Converts CMap/font binaries into COFF objects for `libresources` |
| `libthirdparty.vcxproj` | `libthirdparty.lib` | freetype, zlib, harfbuzz, jbig2dec, lcms2, openjpeg, libjpeg |
| `libresources.vcxproj` | `libresources.lib` | Embedded CMap tables and default fonts (depends on `bin2coff`) |
| `libmuthreads.vcxproj` | `libmuthreads.lib` | Threading primitives |
| `libmupdf.vcxproj` | `libmupdf.lib` | Main MuPDF library (depends on the other four) |

**Build outputs** land at `third_party/mupdf/platform/win32/x64/$<CONFIG>/` (x64 Release → `.../x64/Release/libmupdf.lib`). These paths are stable across MuPDF 1.24.x.

**Deferred: feature-flag pruning.** MuPDF ships with `mujs` (PDF JavaScript), `gumbo` (HTML), and `tesseract`+`leptonica` (OCR) compiled by default. LitePDF v1 does not use any of these. We accept the extra ~2 MB in Phase 1 and defer pruning to **Phase 11** (binary-size regression gate). When the pruning task lands it has two paths:

- (A) Patch MuPDF's own `.vcxproj` files to remove the relevant sources + preprocessor defines (upstream-divergent but small diff).
- (B) Migrate MuPDF integration from `include_external_msproject` to a hand-written CMake shim that globs sources and controls `target_compile_definitions` directly (larger up-front cost, but full control).

**License compliance: freetype FTL.** FreeType 2 is dual-licensed under FTL (BSD-style, AGPL-compatible) and GPLv2-only (NOT AGPL-compatible). MuPDF ships freetype with FTL active by default — verified via inspection of `third_party/mupdf/thirdparty/freetype/include/freetype/config/ftoption.h` at MuPDF 1.24.11: the `FT_CONFIG_OPTION_USE_FTL` macro does not appear in the file (no `#define`, no `#undef`), and FreeType 2's compile-time default when the macro is absent is FTL. Record any change in this status during MuPDF version bumps.

**CVE audit cadence:** before each `1.24.x` → `1.24.(x+1)` bump of the MuPDF submodule, check Artifex's security advisories and changelog. Before v1.0 release, perform a full scan.

## 3. Feature Set (Tier 3)

- Multi-tab interface, per-tab independent state
- PDF bookmark / outline tree in side panel
- Thumbnail side panel (**hidden by default**, toggle with F4)
- In-document search with highlighted hits
- **Cross-tab search**: query all open documents, results in a dockable panel
- Encrypted PDF: password prompt (max 3 attempts per session, password never persisted)
- Additional formats: ePub, CBZ, XPS, FB2, SVG (via MuPDF)
- Dark mode / invert colors
- Two-page spread display
- Recent files (MRU) stored in registry
- Session restore after crash

## 4. Architecture

### 4.1 Module Layout

```
main.cpp
  └─ AppController ── TabManager, MRU, IPC (single-instance)
      ├─ MainWindow (frame, menu, toolbar, statusbar, drag&drop, shortcuts)
      └─ DocumentView (per-tab)
          ├─ PdfCanvas (Direct2D render, scroll, zoom, selection)
          ├─ OutlinePane (TreeView for bookmarks)
          ├─ ThumbnailPane (ListView owner-draw)
          └─ Document (MuPDF wrapper)
              └─ RenderEngine (thread pool + PageCache)
```

| # | Module | Approx. LOC | Responsibility |
|---|---|---|---|
| 1 | `app/` | 400 | Lifecycle, tab orchestration, cross-tab search |
| 2 | `ui/MainWindow` | 600 | Frame, menu/toolbar/statusbar, shortcuts, drag&drop |
| 3 | `ui/TabManager` | 300 | TabCtrl wrapper, tab switching |
| 4 | `ui/DocumentView` | 500 | Per-doc layout (canvas + outline + thumbs) |
| 5 | `ui/PdfCanvas` | 900 | Direct2D paint, pan, zoom, text selection, search highlight |
| 6 | `ui/OutlinePane` | 250 | TreeView + click-to-navigate |
| 7 | `ui/ThumbnailPane` + `core/ThumbnailModel` + `core/ThumbCache` + `core/ThumbnailRenderer` + `ui/VerticalSplitter` | ~1275 | Lazy-rendered thumbnail pane (model, HBITMAP cache, renderer borrow of RenderEngine, vertical splitter, MainWindow integration) |
| 8 | `core/Document` | 400 | MuPDF abstraction, format detection, password, outline parse |
| 9 | `core/RenderEngine` + `PageCache` | 500 | Background rendering, LRU cache |
| 10 | `common/` | 300 | Utils, errors, config, log |
| | **Total** | **~4500** | |

### 4.2 Design Principles

- `Document` and `RenderEngine` are UI-agnostic — testable in a console harness.
- All MuPDF calls are encapsulated in `Document` + `RenderEngine`; swapping engines touches only two files.
- Tabs own independent `Document` and `PageCache` instances (no cross-tab pollution).
- Direct2D resources are lifetime-bound to HWND and rebuilt on `D2DERR_RECREATE_TARGET`.

## 5. Runtime Behavior

### 5.1 Cold-Start Sequence

```
t=0       OS loads exe (HDD seek + 8 MB read ≈ 80 ms)
t=90ms    WinMain: parse args, init COM + Direct2D factory (≈ 20 ms)
t=150ms   Window shown (empty) — user feedback
          PostMessage(WM_APP_OPEN_FIRST_DOC) — async
t=160ms   Worker: MuPDF open (mmap + xref parse ≈ 200 ms)
t=360ms   UI receives DOC_OPENED — tab label, menus, spinner
          Renderer starts on page 0
t=500ms   First page bitmap painted
```

**Critical rule**: Never open a document synchronously on `WM_CREATE`. The window must appear first.

### 5.2 Rendering & Caching

Three-tier cache per tab:

| Tier | Format | Size / page (A4 @ 150 dpi) | Purpose |
|---|---|---|---|
| L1 | `ID2D1Bitmap` | ≈ 4 MB | Ready to blit |
| L2 | MuPDF `fz_display_list` | ≈ 100–500 KB | Parsed commands, fast rasterize |
| L3 | OS mmap | 0 (kernel-managed) | Raw PDF bytes |

**LRU capacity (v1)**: L1 keeps current ± 2 pages (5 pages ≈ 20 MB); L2 keeps current ± 5 pages (10 pages ≈ 3 MB). Total per tab ≈ 25 MB. Three tabs ≈ 75 MB — within the 80 MB target.

### 5.3 Thread Model

- **UI thread**: all Direct2D calls; consumes `fz_pixmap` from workers and uploads to `ID2D1Bitmap`.
- **Render pool (size = 2)**: fixed at 2 to avoid HDD seek thrash from concurrent IO.
- **Priority queue**: P0 = current page, P1 = ±1, P2 = ±2–5 (display list only).
- **Aggressive cancellation**: rapid paging (`PgDn` held down) cancels all but the latest target page ± 1. Required on HDD — a stale request costs a full seek.

### 5.4 Cross-Tab Search

- `Ctrl+Shift+F` opens a search dialog.
- `AppController` fans a search task out to each open `Document`. Search is pure CPU — tabs run in parallel.
- Results render in a **dockable panel at the bottom of the main window** (VS Code-style). Each row: file / page / 30-char context snippet. Click navigates + highlights.
- Per-`Document` result cache keyed by query string; repeat searches are instant.

## 6. Error Handling

### 6.1 Open Path

```
open_document(path):
  file exists?       → no: show "file not found", remove from MRU
  readable?          → no: "access denied" or "file in use"
  MuPDF recognized?  → no: "unsupported format" (show magic bytes in log)
  needs password?    → yes: modal dialog, 3-attempt cap, never persist
  corrupted?         → warn but continue (MuPDF is forgiving)
  success            → update tab title, outline, MRU, begin render
```

### 6.2 Runtime Errors

| Error | Strategy |
|---|---|
| Page-level MuPDF failure | Draw "cannot display this page" placeholder; keep document open |
| Direct2D device loss | Catch `D2DERR_RECREATE_TARGET`, rebuild resources, invalidate |
| Out of memory during render | Evict oldest L1 bitmap, retry once, else placeholder |
| Worker exception | Log, notify UI, worker continues |
| Slow HDD (>2 s to open) | Show cancelable spinner |

Non-fatal errors log to `%LOCALAPPDATA%\LitePDF\log.txt` (1 MB rotation).

### 6.3 Crash Protection

- `SetUnhandledExceptionFilter` writes minidump to `%LOCALAPPDATA%\LitePDF\crashes\`.
- Session state (open tabs, page numbers, zoom) periodically serialized to `session.json`; restored after abnormal exit.
- No automatic telemetry — crash reports stay local.

## 7. Testing

| Layer | Approach | Tool |
|---|---|---|
| `Document` (MuPDF wrapper) | Unit tests, console harness | Catch2 |
| `RenderEngine` + `PageCache` | Unit tests + stress (1000 sequential renders) | Catch2 |
| Search | Unit tests against fixture PDFs | Catch2 |
| UI (Canvas, Outline, Tab) | Manual smoke checklist | — |
| Cold-start time | Batch benchmark, regression gate (+10% fails CI) | PowerShell + exe args |

**Fixtures** (`tests/fixtures/`):

- `simple.pdf` — 5 text pages
- `encrypted.pdf` — password `test`
- `large.pdf` — 500-page scanned PDF (memory stress)
- `corrupt.pdf` — deliberately truncated
- `bookmarks.pdf` — multi-level outline
- `search.pdf` — known keyword positions
- `sample.epub`, `sample.cbz` — non-PDF formats

**CI**: GitHub Actions on Windows runners; unit tests + benchmark gate on every PR.

**Manual smoke list** (pre-release):

- [ ] Double-click a `.pdf` in Explorer opens correctly
- [ ] Drag-and-drop a PDF onto the window
- [ ] Open multiple tabs, close, no memory leak (Task Manager)
- [ ] Cross-tab search and click-to-navigate
- [ ] Zoom, scroll, keyboard nav
- [ ] Cold-start on HDD machine < 1 s to first page

## 8. Build & Distribution

### 8.1 Directory Layout

```
litepdf/
├─ CMakeLists.txt
├─ cmake/
│   ├─ FetchMuPDF.cmake
│   └─ CompilerFlags.cmake
├─ src/
│   ├─ main.cpp
│   ├─ app/
│   ├─ ui/
│   ├─ core/
│   └─ common/
├─ tests/
│   ├─ CMakeLists.txt
│   ├─ fixtures/
│   └─ unit/
├─ third_party/
│   └─ mupdf/               # git submodule, tag-pinned
├─ resources/
│   ├─ litepdf.rc
│   ├─ manifest.xml
│   └─ icons/
├─ assets/
│   └─ icon/                # SVG source + per-size PNGs + .ico
├─ installer/
│   └─ litepdf.iss          # Inno Setup script
├─ docs/
│   └─ plans/
├─ .github/workflows/
│   └─ ci.yml
├─ LICENSE                  # AGPL-3.0
├─ VERSION
├─ README.md
└─ CHANGELOG.md
```

### 8.2 Build Commands

```
git clone --recursive https://github.com/<user>/litepdf
cd litepdf
cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
# Output: build/Release/litepdf.exe  (~8 MB, standalone)
```

### 8.3 Distribution

Two artifacts per release on GitHub Releases:

1. **Portable**: `litepdf-portable-<version>.zip` containing `litepdf.exe`.
2. **Installer**: `litepdf-setup-<version>.exe` (Inno Setup).

### 8.4 Installer Design (Inno Setup)

**Type**: Per-user install by default (writes to `%LOCALAPPDATA%\Programs\LitePDF`, no UAC prompt). Advanced users may opt into per-machine install (triggers UAC, writes to `Program Files`).

**Wizard flow** (Traditional Chinese UI):

1. Welcome (icon + version)
2. **License & third-party notices** (informational — see §8.5 for exact text and rationale)
3. Install location
4. Component selection:
   - [✓] Start menu shortcut
   - [✓] Desktop shortcut
   - [ ] Set as default for `.pdf` *(unchecked — user opts in)*
   - [ ] Associate `.epub` / `.cbz` / `.xps` *(unchecked)*
   - [ ] Add "Open with LitePDF" context menu entry *(unchecked)*
5. Confirmation summary
6. Install progress
7. Finish (optional "launch now")

**Uninstaller** (`unins000.exe`, auto-generated):

- Removes installed files, shortcuts, associations, context menu entry.
- On uninstall, prompts whether to keep user config (`%LOCALAPPDATA%\LitePDF\`). Default: keep.
- Registered under `HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\LitePDF` for per-user, `HKLM` for per-machine.

**CI**: GitHub Actions job runs `iscc installer/litepdf.iss` after Release build and uploads both artifacts.

### 8.5 License & Third-Party Notices Page (installer step 2)

#### 8.5.1 Rationale

The AGPL-3.0 does not require end-user agreement as a precondition to *running* the program — the license governs distribution and modification, not use. ([FSF GPL FAQ: "Does the GPL require that source code of modified versions be posted to the public?"](https://www.gnu.org/licenses/gpl-faq.html#DoesTheGPLRequireAvailabilityToPublic)). Consequently this installer page is **informational disclosure**, not an agreement gate. Using "I agree / I do not agree" radio buttons — as in Inno Setup's default template — misrepresents the license by implying use is conditional on assent.

The page still exists because AGPL §7 requires reasonable attribution of third-party components under the same license (MuPDF), and because informing users of their AGPL rights (source availability, redistribution freedom, network-use clause) is good practice and a legal courtesy from the distributor.

#### 8.5.2 Wording

Shown in Traditional Chinese with an English toggle. Reference text (Traditional Chinese):

```
─────────────────────────────────────────────
LitePDF 授權條款
─────────────────────────────────────────────

LitePDF 依 GNU Affero General Public License v3.0 (AGPL-3.0) 發佈。
本條款保障您以下權利:
  • 可自由使用本程式 (個人或商業用途均可)
  • 可取得完整原始碼: https://github.com/<user>/litepdf
  • 可修改並再散佈本程式,但需沿用相同授權

若您將本程式或其衍生作品透過網路提供服務,
AGPL 要求您同樣需將您的原始碼公開 (AGPL §13)。

完整英文條款: https://www.gnu.org/licenses/agpl-3.0.html

─────────────────────────────────────────────
本程式包含之第三方元件
─────────────────────────────────────────────

• MuPDF 1.24.x  — © Artifex Software, Inc. — AGPL-3.0
  https://mupdf.com

─────────────────────────────────────────────
免責聲明
─────────────────────────────────────────────

本程式按「原樣」提供,不含任何明示或默示保證。
作者與貢獻者對任何使用後果不負責任。
```

#### 8.5.3 Buttons & Flow

- **No "I agree / I do not agree" radio buttons.** The Inno Setup default radios are replaced with a single "Next" (「下一步」) and the standard "Cancel" (「取消」).
- Proceed button label: **「我已閱讀並了解」** ("I have read and understood"). The wizard advances on click.
- No checkbox is required. A user who reaches this page and clicks Next is acknowledging they saw the notice, which is all that is legally meaningful under AGPL.

#### 8.5.4 Inno Setup Implementation

The license file is a hand-authored RTF at `installer/LICENSE-DISPLAY.rtf` (not the raw AGPL text), laid out with section headers in bold. The `[Setup]` section sets `LicenseFile=` to this RTF. Default strings are overridden in the `[Messages]` section to replace agreement language with acknowledgement language, and the "I do not accept" radio is hidden via `[Code]` (Pascal Script) by setting `WizardForm.LicenseNotAcceptedRadio.Visible := False` and auto-selecting the accepted radio on page activation. See Phase 10 implementation plan for exact script.

#### 8.5.5 Maintenance Rules (normative)

These rules are binding on every PR that changes the set of components shipped in `litepdf.exe` or in the installer bundle.

**Rule 1 — When to update this page.** Update `installer/LICENSE-DISPLAY.rtf` and this section of the design doc whenever any of the following is true:

1. A new library, font, icon, or data file is statically linked, dynamically linked, or bundled into the installer.
2. An existing bundled component's license changes (e.g. MuPDF relicenses, or we upgrade to a version with different terms).
3. An existing bundled component's version changes in a way that affects the notice (e.g. version bump that matters for CVE disclosure).

**Rule 2 — When components are exempt.** Do *not* list:

- Build-time-only tooling (CMake, MSVC, Inno Setup compiler itself, ImageMagick, rsvg-convert). These are not "distributed" with the product.
- Test-only dependencies (Catch2 linked only into the test harness and excluded from Release). If a test helper ships in the Release exe, it is no longer test-only and must be listed.
- System libraries provided by Windows (`comctl32`, `d2d1`, `dwrite`, `gdi32`, `user32`, etc.). Windows components are not redistributed by us.

**Rule 3 — License compatibility gate.** Before adding a component, verify its license is compatible with AGPL-3.0:

- Compatible: AGPL-3.0, GPL-3.0, LGPL-3.0, MIT, BSD-2/3, Apache-2.0, ISC, Zlib, Public Domain / CC0.
- **Incompatible and must be rejected**: any license that forbids copyleft (e.g. some source-available licenses), or GPL-2.0-only (without "or later" clause — incompatible with AGPL-3.0).
- Ambiguous cases (custom licenses, dual-licensed components) require documenting the choice and the reasoning in the PR description.

**Rule 4 — Required notice fields.** Each bullet in "本程式包含之第三方元件 / Third-Party Components" must contain:

1. Component name and version (major.minor acceptable; patch level optional unless needed for CVE context).
2. Copyright holder as stated in the component's own LICENSE file.
3. License identifier (SPDX short form: `AGPL-3.0`, `MIT`, etc.).
4. Project URL (preferred: the official homepage; fallback: the source repository).

**Rule 5 — PR checklist for component changes.** The PR description must include, and the reviewer must verify:

- [ ] Component name, version, license, copyright, URL gathered.
- [ ] License compatibility confirmed against Rule 3.
- [ ] `installer/LICENSE-DISPLAY.rtf` updated.
- [ ] This section (§8.5) of the design doc updated to match.
- [ ] A copy of the component's upstream `LICENSE` file is checked into `third_party/<component>/LICENSE` or equivalent path required by that license.
- [ ] If the component's license requires a `NOTICE` file or equivalent (e.g. Apache-2.0), that file is also preserved and bundled.

**Rule 6 — Source availability promise.** Because AGPL requires offering source to anyone who receives the binary, every GitHub Release must attach the exact source tarball (git archive of the tagged commit, submodules included) alongside `litepdf-setup.exe` and `litepdf-portable.zip`. The CI release job is the enforcement point; this check must not be skipped.

#### 8.5.6 Current Components Inventory

| Name | Version | License | Copyright | URL |
|---|---|---|---|---|
| MuPDF | 1.24.x (pinned per release) | AGPL-3.0 | Artifex Software, Inc. | https://mupdf.com |

This table is the single source of truth. `installer/LICENSE-DISPLAY.rtf` and Rule 5 PR checklists must be kept consistent with it.

## 9. Application Icon

### 9.1 Primary Icon — "Lightning Document"

A white sheet of paper with a folded top-right corner; a blue lightning bolt overlaid across the sheet.

- Paper: `#F8F9FA`; fold shadow: `#DEE2E6`
- Lightning: `#0D6EFD` (high contrast on both light and dark taskbars)
- Background: transparent
- Meaning: lightning = fast/lightweight; paper = document

### 9.2 Document File Icon Variant (for `.pdf` association, v1)

Same paper silhouette, with red "PDF" wordmark in place of the lightning. Distinct from the app icon so users distinguish "the app" from "a document":

- Paper: `#F8F9FA`; fold shadow: `#DEE2E6`
- Text "PDF": `#D32F2F`, bold sans-serif, center
- Used when the installer registers file association; stored as `IDI_PDFDOC`, referenced by `DefaultIcon` registry key on associated extensions.

### 9.3 Required Sizes

| Size | Use | Notes |
|---|---|---|
| 16×16 | Toolbar, tab icon | Hand-tuned pixel art — lightning becomes a 3-pixel polyline |
| 20×20 | Win11 taskbar (small) | Same, more detail |
| 24×24 | Explorer details view | Fold returns; bolt still hinted |
| 32×32 | Alt+Tab, file default | Standard detail |
| 48×48 | Desktop, Start tile | Full detail |
| 64×64 | High-DPI file | Scaled from 48 |
| 256×256 | Large icon view, installer welcome | Full detail + subtle shadow |

### 9.4 Asset Pipeline

```
assets/icon/
├─ litepdf-app.svg         # vector source, 256px master
├─ litepdf-doc.svg         # document variant
├─ app-<size>.png          # per-size rasterizations (16/20/24/32/48/64/256)
├─ doc-<size>.png
├─ litepdf-app.ico         # multi-resolution, embedded in exe as IDI_APPICON
└─ litepdf-doc.ico         # embedded as IDI_PDFDOC
```

Generation (CMake custom target):

```
rsvg-convert -w <N> -h <N> litepdf-app.svg -o app-<N>.png   # for each size
magick app-16.png app-20.png ... app-256.png litepdf-app.ico
# same for doc variant
```

Embedded via `resources/litepdf.rc`:

```
IDI_APPICON ICON "icon/litepdf-app.ico"
IDI_PDFDOC  ICON "icon/litepdf-doc.ico"
```

## 10. Out of Scope (v1)

Deferred to later versions:

- Annotation authoring (highlight / note / draw)
- Form filling (AcroForm widgets)
- Digital signature verification
- Printing presets beyond the Windows default dialog
- Accessibility (screen reader, high-contrast themes beyond invert)
- Automatic updates
- Telemetry / analytics
- Localization beyond Traditional Chinese and English

## Summary

| Dimension | Decision |
|---|---|
| Target | Windows 11 on HDD-based hardware, ≤ 8 GB RAM |
| Stack | C++17, MSVC, CMake, Direct2D, Win32, MuPDF (static) |
| License | AGPL-3.0 |
| Size budget | exe ≤ 8 MB, installer ≤ 9 MB, RAM ≤ 80 MB, cold start ≤ 1 s |
| Features | Tier 3 (tabs, outline, thumbs, cross-tab search, encrypted, multi-format, dark, dual-page) |
| Architecture | 10 modules, ≈ 4500 LOC, UI/core decoupled, 2-thread render pool |
| Cache | Per-tab L1 (±2 bitmaps) + L2 (±5 display lists) + L3 (OS mmap) |
| Distribution | Portable zip + per-user Inno Setup installer |
| Icon | "Lightning Document" (app) + red "PDF" variant (document) |
| Testing | Catch2 + benchmark regression gate + manual smoke + installer install/uninstall/upgrade tests |

Implementation plan (milestones, sequencing, dependencies) will be produced by the `writing-plans` skill in a follow-up document.
