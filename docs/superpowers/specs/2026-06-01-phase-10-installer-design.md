# Phase 10 — Installer + GitHub Release Pipeline (Design Spec)

> **Status:** Approved 2026-06-01. Implementation plan to follow via
> `superpowers:writing-plans`.
>
> **Parent design:** This spec is the implementation-level authority for
> Phase 10. It inherits and refines [`docs/plans/2026-04-15-litepdf-design.md`](../../plans/2026-04-15-litepdf-design.md)
> §8.3–§8.5 (Distribution, Installer Design, License & Third-Party Notices Page)
> and the roadmap row in [`docs/plans/2026-04-15-litepdf-roadmap.md`](../../plans/2026-04-15-litepdf-roadmap.md).
> Where this spec and §8.x disagree, this spec wins.
>
> **Starting state:** `v0.0.11-phase9` shipped; `main` clean at `b78e7b5`;
> `VERSION = 0.0.12-dev`.

## 1. Goal

Ship the distribution layer for LitePDF: a per-user Inno Setup installer, a
portable zip, and an AGPL-compliant source tarball, all produced and published
automatically by a tag-triggered GitHub Actions release job. Phase 10 cuts the
project's first public, downloadable release (`v0.0.12-phase10`, a pre-1.0
milestone — v1.0 still gates on Phases 11–12).

## 2. Resolved Decisions

Four decisions left open by §8.x, resolved during brainstorming (2026-06-01):

| # | Decision | Choice | Rationale |
|---|----------|--------|-----------|
| D1 | Code signing | **Unsigned** | Personal OSS project; zero cost. README documents the SmartScreen "More info → Run anyway" step. `.iss` reserves a commented `SignTool` hook for later. |
| D2 | Release trigger | **Tag-triggered** (`on: push: tags: ['v*']`) | One `git tag && git push --tags` runs the whole release. Tag is the source of truth, sidestepping the squash-merge `git describe` ancestry break noted in prior checkpoints. Lives in a dedicated `release.yml`, leaving `ci.yml` untouched. |
| D3 | File associations | **Full capability registration + Settings deep-link** | Windows 10/11 hash-protects the `UserChoice` key; an installer cannot programmatically set the default handler. We register a ProgID (`.pdf` gets the red `IDI_PDFDOC` icon), add LitePDF to "Open with", and register Default-Programs capabilities. When the user ticks "set as default", the installer opens `ms-settings:defaultapps` at finish for one-click confirmation. No `UserChoice` hash hacking. |
| D4 | Phase 10 scope | **Cut a real public pre-1.0 release now** | The roadmap exit criterion ("installs on a fresh VM") requires a real artifact. Exercising the full pipeline at low-stakes `0.0.12` surfaces release bugs before the high-stakes v1.0 moment, and gives the project its first downloadable build. |

## 3. Release Artifacts

Three assets attached to every GitHub Release. Version string is `VERSION`
with the `-dev` suffix stripped (e.g. `0.0.12`).

1. **Installer** — `litepdf-setup-<version>.exe` (Inno Setup, per-user default).
2. **Portable** — `litepdf-portable-<version>.zip` containing `litepdf.exe`,
   `LICENSE`, and `README.md`.
3. **Source tarball** — `litepdf-<version>-source.tar.gz`: the exact tagged
   commit **including the pinned MuPDF submodule contents** (AGPL §13
   corresponding source). `git archive` alone does not include submodules; the
   job uses `git-archive-all` (or an equivalent that bundles submodule trees).

## 4. Installer — `installer/litepdf.iss`

### 4.1 Install type

- **Default: per-user.** Installs to `%LOCALAPPDATA%\Programs\LitePDF`, no UAC
  prompt. (`PrivilegesRequired=lowest`, `DefaultDirName={localappdata}\Programs\LitePDF`.)
- **Opt-in: per-machine.** Advanced users may elevate; installs to
  `Program Files`, triggers UAC. (Inno's `PrivilegesRequiredOverridesAllowed`
  to allow the dialog.)

### 4.2 Wizard flow (Traditional Chinese UI)

Per §8.4:

1. Welcome (icon + version)
2. License & third-party notices (informational — see §4.3)
3. Install location
4. Component / task selection:
   - [✓] Start menu shortcut
   - [✓] Desktop shortcut
   - [ ] Set as default for `.pdf` *(opt-in; behavior per §4.4)*
   - [ ] Associate `.epub` / `.cbz` / `.xps` *(opt-in)*
   - [ ] Add "Open with LitePDF" context menu entry *(opt-in)*
5. Confirmation summary
6. Install progress
7. Finish (optional "launch now")

### 4.3 License & third-party notices page (§8.5, informational)

- Source: hand-authored `installer/LICENSE-DISPLAY.rtf` (not raw AGPL text),
  section headers in bold. Content is the Traditional Chinese wording in design
  §8.5.2 plus the third-party components table (§8.5.6: MuPDF 1.24.x, Artifex
  Software, AGPL-3.0).
- `[Setup] LicenseFile=` points at the RTF.
- **No "I agree / I do not agree" radios.** Inno's default agreement radios are
  hidden via `[Code]`: on the license page's activation, set
  `WizardForm.LicenseNotAcceptedRadio.Visible := False` and auto-select the
  accepted radio so Next is always enabled.
- `[Messages]` overrides replace agreement language with acknowledgement
  language. Proceed button reads **「我已閱讀並了解」** ("I have read and
  understood").
- Rationale: AGPL governs distribution and modification, not use, so an
  agreement gate would misrepresent the license. This page is disclosure, not a
  contract (§8.5.1).

### 4.4 File-association mechanics (D3)

Registry hive follows install scope: `HKCU\Software\Classes` for per-user,
`HKLM\Software\Classes` for per-machine. The installer always registers
*capability* (so LitePDF is selectable); it never force-writes `UserChoice`.

For each opted-in extension:

1. **ProgID** — e.g. `LitePDF.pdf` with `DefaultIcon` → `IDI_PDFDOC` (resource
   in `litepdf.exe`) and `shell\open\command` → `"litepdf.exe" "%1"`. ePub/CBZ/
   XPS reuse the same pattern (default app icon for non-PDF).
2. **OpenWithProgids** — add the ProgID under the extension's
   `OpenWithProgids` so LitePDF appears in "Open with".
3. **Default Programs capabilities** — register under
   `Software\Clients` / `RegisteredApplications` (or the `Capabilities`
   subkey pattern) so LitePDF shows in Settings → Default apps.
4. **Context menu** (separate opt-in) — `shell\Open with LitePDF\command` on
   the relevant classes.

If "set as default for `.pdf`" is ticked, the **Finish** step launches
`ms-settings:defaultapps` (via `Exec`/`ShellExec` on the URI) so the user
confirms the default in Windows Settings with one click. We do not claim the
default was set automatically.

### 4.5 Uninstaller (`unins000.exe`, auto-generated)

- Removes installed files, shortcuts, registered associations, ProgIDs, and the
  context-menu entry.
- Prompts whether to keep user config at `%LOCALAPPDATA%\LitePDF\`. **Default:
  keep.**
- Registered under `HKCU\...\Uninstall\LitePDF` (per-user) or `HKLM\...` (per-machine).

### 4.6 Signing (D1)

Not signed in Phase 10. The `.iss` carries a commented `SignTool` directive and
a one-line note pointing at the README SmartScreen section, so enabling signing
later is a config change, not a redesign.

## 5. Release Pipeline — `.github/workflows/release.yml`

New workflow, independent of `ci.yml` (which is unchanged).

- **Trigger:** `on: push: tags: ['v*']`.
- **Runner:** `windows-latest`.
- **Steps:**
  1. Checkout with `submodules: recursive`.
  2. Configure + build Release (`cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release`; `cmake --build build --config Release --parallel`).
  3. Version-sync gate (`scripts/check-version-sync.ps1`).
  4. Unit tests (`ctest --test-dir build -C Release --output-on-failure`).
  5. Smoke test (`scripts/smoke-test.ps1`).
  6. Derive version: read `VERSION`, strip `-dev` → `<version>`.
  7. Install Inno Setup (`choco install innosetup -y` — not preinstalled on the runner).
  8. Compile installer: `iscc /DMyAppVersion=<version> installer\litepdf.iss`.
  9. Build portable zip (`litepdf.exe` + `LICENSE` + `README.md`).
  10. Build source tarball (`git-archive-all` incl. submodules).
  11. (Optional gate) Tarball build-back check: extract to a clean dir,
      configure+build, assert `litepdf.exe` produced — proves §13 source is
      buildable.
  12. `gh release create <tag>` attaching the three assets, with
      `if-no-files-found: error` semantics (fail the job if any asset is missing).

## 6. VERSION & Release Semantics (D4)

The repo bumps `VERSION` only at release boundaries (PR #14 precedent); gstack's
4-digit bumper is incompatible with this repo's `3-digit + -dev` format and is
not used.

1. Release commit: `VERSION` `0.0.12-dev` → `0.0.12`. `check-version-sync.ps1`
   stays green (About `v0.0.12`, VERSIONINFO `0,0,12,0`; the gate already
   normalizes the `-dev` suffix).
2. Tag `v0.0.12-phase10` (matches the `v*` trigger and the per-phase tag
   convention: `v0.0.11-phase9`, `v0.0.10-phase8.5`, ...).
3. Push tag → `release.yml` publishes the GitHub Release.
4. Follow-up commit: `VERSION` `0.0.12` → `0.0.13-dev` to resume development.

The artifact version string is `VERSION` minus `-dev` (`0.0.12`), independent of
the `-phase10` tag suffix. `VERSION` remains the single source of truth, shared
with the Win32 VERSIONINFO derivation (commit `d60b524`).

## 7. Verification

- **CI gate (automated):** `release.yml` must compile the `.iss` (`iscc` exit 0)
  and produce all three artifacts, or the job fails.
- **Source-tarball build-back (automated or manual):** the extracted tarball
  must configure + build to a working `litepdf.exe`.
- **Manual fresh-VM smoke checklist** (roadmap exit criterion; installer logic
  cannot be Catch2-tested):
  1. Per-user install completes with **no UAC prompt**.
  2. App launches from Start-menu and Desktop shortcuts.
  3. Opening a `.pdf` works; after choosing LitePDF as default in Settings, the
     `.pdf` file shows the red `IDI_PDFDOC` icon.
  4. License page shows the acknowledgement button, **no** agree/disagree radios.
  5. Uninstall removes files + associations and prompts to keep config (default
     keep); declining keep-config removes `%LOCALAPPDATA%\LitePDF\`.
  6. Re-install over an existing install upgrades in place cleanly.
  7. (Spot check) Per-machine install path triggers UAC and lands in
     `Program Files`.

## 8. File Manifest

**New:**
- `installer/litepdf.iss`
- `installer/LICENSE-DISPLAY.rtf`
- `.github/workflows/release.yml`
- `docs/superpowers/specs/2026-06-01-phase-10-installer-design.md` (this spec)
- `scripts/make-source-tarball.ps1` *(optional — may be inlined in the workflow)*

**Edited:**
- `VERSION` (→ `0.0.12` for the release commit, → `0.0.13-dev` after)
- `README.md` (download/install section + SmartScreen "Run anyway" note)
- `CHANGELOG.md` (`[0.0.12-phase10] — Installer` section)

**Unchanged:** `ci.yml`; `resources/manifest.xml` `assemblyIdentity version`
(frozen since Phase 0, does not track VERSION).

## 9. Out of Scope (YAGNI)

Code signing (deferred, hook reserved); auto-update / update checker;
per-machine as the *default* (stays opt-in); MSI / winget / Microsoft Store
packaging; installer languages beyond Traditional Chinese; touching
`manifest.xml` SxS identity.

## 10. Acceptance Criteria

- `iscc installer/litepdf.iss` compiles clean in CI.
- Pushing tag `v0.0.12-phase10` publishes a GitHub Release with all three assets.
- Manual smoke checklist (§7) passes on a fresh Windows VM.
- `VERSION` returns to `0.0.13-dev` post-release; `check-version-sync.ps1` green.
- README documents download, install, and the SmartScreen step.
- CHANGELOG has the `[0.0.12-phase10]` section with a compare link.
