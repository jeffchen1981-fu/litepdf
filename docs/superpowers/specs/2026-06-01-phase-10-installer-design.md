# Phase 10 — Installer + GitHub Release Pipeline (Design Spec)

> **Status:** Approved 2026-06-01; hardened to v2 after a three-lens agent
> review (technical correctness / completeness / AGPL compliance) on 2026-06-01.
> Implementation plan to follow via `superpowers:writing-plans`.
>
> **Parent design:** This spec is the implementation-level authority for
> Phase 10. It inherits and refines [`docs/plans/2026-04-15-litepdf-design.md`](../../plans/2026-04-15-litepdf-design.md)
> §8.3–§8.5 (Distribution, Installer Design, License & Third-Party Notices Page)
> and the roadmap row in [`docs/plans/2026-04-15-litepdf-roadmap.md`](../../plans/2026-04-15-litepdf-roadmap.md).
> Where this spec and §8.x disagree, this spec wins — notably the license page
> uses `InfoBeforeFile=` instead of the radio-hiding hack in §8.5.4, and the
> §8.5.6 component inventory was expanded (this PR) to list all eight bundled
> third-party libraries.
>
> **Starting state:** `v0.0.11-phase9` shipped; `main` clean at `b78e7b5`;
> `VERSION = 0.0.12-dev`.

## 1. Goal

Ship the distribution layer for LitePDF: a per-user Inno Setup installer, a
portable zip, and an AGPL-compliant source tarball, all produced and published
automatically by a tag-triggered GitHub Actions release job. Phase 10 cuts the
project's first public, downloadable release (`v0.0.12-phase10`, a pre-1.0
milestone — v1.0 still gates on Phases 11–12; the GitHub Release is marked
**prerelease**).

## 2. Resolved Decisions

Four decisions left open by §8.x, resolved during brainstorming (2026-06-01):

| # | Decision | Choice | Rationale |
|---|----------|--------|-----------|
| D1 | Code signing | **Unsigned** | Personal OSS project; zero cost. README documents the SmartScreen "More info → Run anyway" step. `.iss` reserves a commented `SignTool` hook for later. |
| D2 | Release trigger | **Tag-triggered** (`on: push: tags: ['v*']`) | One `git tag && git push --tags` runs the whole release. Tag is the source of truth, sidestepping the squash-merge `git describe` ancestry break noted in prior checkpoints. Lives in a dedicated `release.yml`, leaving `ci.yml` untouched. |
| D3 | File associations | **Full capability registration + Settings deep-link** | Windows 10/11 hash-protects the `UserChoice` key; an installer cannot programmatically set the default handler. We register a ProgID (`.pdf` gets the red `IDI_PDFDOC` icon), add LitePDF to "Open with", and register Default-Programs capabilities. When the user ticks "set as default", the installer opens the Windows Default-apps settings page for the user to confirm. No `UserChoice` hash hacking. |
| D4 | Phase 10 scope | **Cut a real public pre-1.0 release now** | The roadmap exit criterion ("installs on a fresh VM") requires a real artifact. Exercising the full pipeline at low-stakes `0.0.12` surfaces release bugs before the high-stakes v1.0 moment, and gives the project its first downloadable build. |

## 3. Release Artifacts

Three assets attached to every GitHub Release. Version string is `VERSION`
with the `-dev` suffix stripped (e.g. `0.0.12`).

1. **Installer** — `litepdf-setup-<version>.exe` (Inno Setup, per-user default).
2. **Portable** — `litepdf-portable-<version>.zip` containing `litepdf.exe`,
   `LICENSE`, and `README.md`. The exe is genuinely self-contained: the Release
   build statically links the CRT (`/MT`), enforced by
   `cmake/CompilerFlags.cmake` (`MSVC_RUNTIME_LIBRARY MultiThreaded`) and
   `cmake/ImportMuPDF.cmake` (which rewrites MuPDF's vcxprojs from `/MD` to
   `/MT`). **No VC++ redistributable is bundled** — this is contingent on the
   `/MT` setting; see §5 step 2a.
3. **Source tarball** — `litepdf-<version>-source.tar.gz`: the exact tagged
   commit **including the pinned MuPDF submodule and its nested third-party
   submodules** (AGPL **§6** corresponding source; not §13 — §13 is the
   network-interaction clause and does not apply to a desktop app). `git
   archive` alone omits submodules, so the job uses `git-archive-all`
   (`pip install git-archive-all`, see §5 step 10).

**System requirements** (stated in README and the installer Welcome page):
Windows 10 version 1903 or later, 64-bit. The app uses Direct2D / DirectWrite
and the `ms-settings:` URI scheme; it will not run on Windows 7/8 or 32-bit
Windows.

## 4. Installer — `installer/litepdf.iss`

### 4.1 `[Setup]` identity and scope

- **Stable `AppId`** — a fixed GUID generated once and **never changed across
  versions**. Mandatory: it is the identity key Inno Setup uses to detect an
  existing install, upgrade in place, and locate the uninstaller. Without it,
  reinstalls leave a parallel install + a second "Add or Remove Programs" entry.
- **Per-user default** — `PrivilegesRequired=lowest`,
  `DefaultDirName={localappdata}\Programs\LitePDF`. No UAC prompt.
- **Opt-in per-machine** — `PrivilegesRequiredOverridesAllowed=dialog` so
  advanced users may elevate; per-machine installs to `Program Files`, triggers
  UAC. `UsePreviousPrivileges=yes` (default) makes an upgrade reuse the prior
  scope (relies on the stable `AppId`).
- **64-bit only** — `ArchitecturesAllowed=x64compatible` and
  `ArchitecturesInstallIn64BitMode=x64compatible`. The binary is built `-A x64`;
  these directives block install on 32-bit Windows and place the app in the real
  `Program Files` (not WoW64) for per-machine installs.
- **Running-instance handling** — `AppMutex=Local\LitePDF_SingleInstance_v1`
  (the exact named mutex the app holds; see `src/app/SingleInstance.cpp`) plus
  `CloseApplications=yes`. Without this, copying over an in-use, memory-mapped
  `litepdf.exe` fails with an "access denied" error mid-install.
- **Output naming** — `OutputBaseFilename=litepdf-setup-{#MyAppVersion}` and a
  known `OutputDir` (e.g. `installer\Output`) so the `release.yml` asset path is
  deterministic. `MyAppVersion` is passed in via `iscc /DMyAppVersion=<version>`.

### 4.2 Wizard flow (Traditional Chinese UI)

Per §8.4:

1. Welcome (icon + version + min-OS line)
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

- **Mechanism:** use Inno Setup's `InfoBeforeFile=installer\LICENSE-DISPLAY.rtf`,
  which renders an informational page with **only Back/Next** and **no
  agree/disagree radios by design**. This supersedes design §8.5.4's approach of
  hiding `LicenseNotAcceptedRadio` via `[Code]` — `InfoBeforeFile` is the
  supported, honest way to show pure disclosure and matches the "disclosure, not
  a contract" rationale (§8.5.1) without mutating internal wizard controls.
- **Content:** hand-authored `installer/LICENSE-DISPLAY.rtf` (not raw AGPL text),
  section headers in bold. Body = the Traditional Chinese wording in design
  §8.5.2, plus the **full third-party components table from the expanded
  §8.5.6** (MuPDF + FreeType + libjpeg + OpenJPEG + lcms2 + MuJS + jbig2dec +
  Gumbo + zlib), and the **mandatory verbatim attribution lines** for FreeType
  (FTL) and libjpeg (IJG) required by those licenses independent of AGPL.
- Rationale: AGPL governs distribution and modification, not use, so an
  agreement gate would misrepresent the license (§8.5.1).

### 4.4 File-association mechanics (D3)

Registry hive follows install scope: `HKCU\Software\Classes` for per-user,
`HKLM\Software\Classes` for per-machine. The installer always registers
*capability* (so LitePDF is selectable); it never force-writes `UserChoice`.

For each opted-in extension:

1. **ProgID** — e.g. `LitePDF.pdf` with `DefaultIcon` →
   `"<InstallDir>\litepdf.exe,-102"`. The `-102` is the **negative resource-ID**
   form referencing `IDI_PDFDOC` (resilient to icon reordering); it is
   contractually pinned by `resources/MainMenu.rc.h` ("do NOT renumber — Phase
   10 installer references IDI_PDFDOC by numeric value (-102)"). Do **not** use a
   positional index (`,1`). `shell\open\command` → `"<InstallDir>\litepdf.exe" "%1"`.
   ePub/CBZ/XPS reuse the same pattern (default app icon for non-PDF).
2. **OpenWithProgids** — add the ProgID under the extension's
   `OpenWithProgids` so LitePDF appears in "Open with".
3. **Default Programs capabilities** — register under
   `Software\Clients` / `RegisteredApplications` (the `Capabilities` subkey
   pattern) so LitePDF shows in Settings → Default apps.
4. **Context menu** (separate opt-in) — `shell\Open with LitePDF\command` on
   the relevant classes.
5. **Refresh the shell** — after writing associations, call
   `SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, 0, 0)` from `[Code]`
   (`CurStepChanged(ssPostInstall)`). Without it, Explorer's icon/association
   cache does not refresh until the next logon and `.pdf` files keep showing the
   old icon.

If "set as default for `.pdf`" is ticked, the **Finish** step opens the Windows
**Default apps settings page** (`ms-settings:defaultapps`; best-effort
`?registeredAppUser=LitePDF` to deep-link to the app where supported) so the
user confirms the default in Settings. This is *not* one-click on Windows 11
(the URI opens the general/per-app page, not a per-extension chooser); we do not
claim the default was set automatically.

### 4.5 Uninstaller (`unins000.exe`, auto-generated)

Inno's auto-generated uninstaller removes only keys created via explicit
`[Registry]` entries flagged for deletion, so every association key must carry
the right flag:

- ProgID keys (`LitePDF.pdf`, `LitePDF.epub`, …): `Flags: uninsdeletekey`.
- Extension root keys touched only to add `OpenWithProgids`:
  `Flags: uninsdeletevalue` on the specific value (do not delete the whole
  extension key, which other apps share) — explicitly remove the LitePDF
  `OpenWithProgids` entry so it does not orphan in "Open with".
- Capabilities / `RegisteredApplications` entries and the context-menu
  `shell\Open with LitePDF` key: `uninsdeletekey` / `uninsdeletekeyifempty`.
- Files, shortcuts: standard `[Files]`/`[Icons]` removal.
- On uninstall, prompt whether to keep user config at `%LOCALAPPDATA%\LitePDF\`.
  **Default: keep.** Declining removes the folder.
- Registered under `HKCU\...\Uninstall\LitePDF` (per-user) or `HKLM\...`
  (per-machine).
- Re-fire `SHChangeNotify` after removing associations.

### 4.6 Signing (D1)

Not signed in Phase 10. The `.iss` carries a commented `SignTool` directive and
a one-line note pointing at the README SmartScreen section, so enabling signing
later is a config change, not a redesign.

## 5. Release Pipeline — `.github/workflows/release.yml`

New workflow, independent of `ci.yml` (which is unchanged).

- **Trigger:** `on: push: tags: ['v*']`.
- **Permissions:** `permissions: contents: write` at the job (or workflow)
  level — **required** or `gh release create` fails with HTTP 403 on repos whose
  default `GITHUB_TOKEN` is read-only.
- **Concurrency:** `concurrency: { group: release-${{ github.ref }},
  cancel-in-progress: false }` so a re-pushed tag does not race a running job.
- **Runner:** `windows-latest`.
- **Steps:**
  1. Checkout with `submodules: recursive`.
  2. Configure + build Release (`cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release`; `cmake --build build --config Release --parallel`).
  2a. Assert static CRT: confirm `cmake/CompilerFlags.cmake` still sets
      `MSVC_RUNTIME_LIBRARY MultiThreaded` (and optionally `dumpbin /imports build\Release\litepdf.exe`
      shows no `VCRUNTIME*.dll` / `MSVCP*.dll`). A `/MD` build would require
      bundling the VC++ redistributable — out of scope; fail the release instead.
  3. Version-sync gate (`scripts/check-version-sync.ps1`).
  4. Unit tests (`ctest --test-dir build -C Release --output-on-failure`).
  5. Smoke test (`scripts/smoke-test.ps1`).
  6. Derive version: read `VERSION`, strip `-dev` → `<version>`. Assert it equals
     the tag's `vX.Y.Z` numeric portion (defends against the tag pointing at the
     wrong commit — see §6).
  7. Install Inno Setup, version-pinned: `choco install innosetup --version=<pinned> -y`
     (not preinstalled on the runner). Invoke the compiler by absolute path
     (`& "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"`) rather than relying
     on `iscc` being on PATH in the same step.
  8. Compile installer: `ISCC.exe /DMyAppVersion=<version> installer\litepdf.iss`
     (`OutputBaseFilename`/`OutputDir` per §4.1 make the output path deterministic).
  9. Build portable zip (`litepdf.exe` + `LICENSE` + `README.md`).
  10. Install and run the source-tarball tool: `pip install git-archive-all`,
      then produce `litepdf-<version>-source.tar.gz` including nested submodules.
  11. **Source-tarball build-back gate (required, not optional):** extract the
      tarball to a clean dir, `git submodule status --recursive`-equivalent
      sanity check that MuPDF + nested third-party trees are present, configure +
      build, assert `litepdf.exe` is produced. This is the only proof the
      shipped tarball is genuinely buildable AGPL §6 Corresponding Source.
  12. Publish safely: `gh release create <tag> --draft --prerelease
      --notes-file <changelog-section.md>` (extract the `[0.0.12-phase10]`
      section from `CHANGELOG.md`; fall back to `--generate-notes`), upload all
      three assets, then `gh release edit <tag> --draft=false` only after all
      uploads succeed. Drafting first prevents a publicly visible half-uploaded
      release; `--prerelease` flags the pre-1.0 build (drop it at v1.0). The job
      fails if any asset is missing (no silent partial release).

## 6. VERSION & Release Semantics (D4)

The repo bumps `VERSION` only at release boundaries (PR #14 precedent); gstack's
4-digit bumper is incompatible with this repo's `3-digit + -dev` format and is
not used.

1. **Release commit:** `VERSION` `0.0.12-dev` → `0.0.12`.
   `check-version-sync.ps1` stays green (it strips the suffix via
   `-replace '-.*$',''` at line ~53, matching `CMakeLists.txt`'s normalization;
   About `v0.0.12`, VERSIONINFO `0,0,12,0`).
2. **Tag while HEAD is the release commit** — `git tag v0.0.12-phase10 && git
   push origin v0.0.12-phase10`. The tag MUST point at the `0.0.12` commit,
   **before** the follow-up `-dev` commit exists. (Tag matches the `v*` trigger
   and the per-phase convention: `v0.0.11-phase9`, `v0.0.10-phase8.5`, …)
3. Push tag → `release.yml` publishes the prerelease. Step 6's tag-vs-VERSION
   assertion catches any ordering mistake.
4. **Follow-up commit:** `VERSION` `0.0.12` → `0.0.13-dev` to resume development.

The artifact version string is `VERSION` minus `-dev` (`0.0.12`), independent of
the `-phase10` tag suffix. `VERSION` remains the single source of truth, shared
with the Win32 VERSIONINFO derivation (commit `d60b524`).

## 7. Verification

- **CI gate (automated):** `release.yml` must compile the `.iss` (ISCC exit 0),
  pass the static-CRT assertion (step 2a), pass the source-tarball build-back
  gate (step 11), and produce all three artifacts, or the job fails.
- **Manual fresh-VM smoke checklist** (roadmap exit criterion; installer logic
  cannot be Catch2-tested):
  1. Per-user install completes with **no UAC prompt**.
  2. App launches from Start-menu and Desktop shortcuts.
  3. License page shows Back/Next only, **no** agree/disagree radios.
  4. Opening a `.pdf` works; after choosing LitePDF as default in Settings, the
     `.pdf` file shows the red `IDI_PDFDOC` icon **without a logoff** (proves
     `SHChangeNotify` fired).
  5. LitePDF appears in the `.pdf` "Open with" list.
  6. Uninstall removes files + all association/ProgID/OpenWithProgids/context-menu
     keys (verify "Open with" no longer lists LitePDF) and prompts to keep config
     (default keep); declining removes `%LOCALAPPDATA%\LitePDF\`.
  7. Re-install over an existing install upgrades in place cleanly (single ARP
     entry) — test in **both** per-user and per-machine scope.
  8. Per-machine install path triggers UAC and lands in real `Program Files`.

## 8. File Manifest

**New:**
- `installer/litepdf.iss`
- `installer/LICENSE-DISPLAY.rtf`
- `.github/workflows/release.yml`
- `docs/superpowers/specs/2026-06-01-phase-10-installer-design.md` (this spec)
- `scripts/make-source-tarball.ps1` *(optional — may be inlined in the workflow)*

**Edited:**
- `VERSION` (→ `0.0.12` for the release commit, → `0.0.13-dev` after)
- `README.md` (download/install section, system requirements, SmartScreen "Run
  anyway" note)
- `CHANGELOG.md` (`[0.0.12-phase10] — Installer` section with compare link)
- `docs/plans/2026-04-15-litepdf-design.md` §8.5.6 (expanded inventory — done in
  this PR)

**Unchanged:** `ci.yml`; `resources/manifest.xml` `assemblyIdentity version`
(frozen since Phase 0, does not track VERSION).

## 9. Out of Scope (YAGNI)

Code signing (deferred, hook reserved); auto-update / update checker;
per-machine as the *default* (stays opt-in); MSI / winget / Microsoft Store
packaging; installer languages beyond Traditional Chinese; touching
`manifest.xml` SxS identity; bundling a VC++ redistributable (not needed while
the build is `/MT`).

## 10. Acceptance Criteria

- `ISCC.exe installer/litepdf.iss` compiles clean in CI.
- `release.yml` declares `permissions: contents: write`.
- Pushing tag `v0.0.12-phase10` publishes a GitHub **prerelease** with all three
  assets; the release is created `--draft` and promoted only after all uploads.
- `installer/LICENSE-DISPLAY.rtf` lists all nine inventory components from the
  expanded §8.5.6 and includes the verbatim FreeType (FTL) and libjpeg (IJG)
  attribution lines.
- Source-tarball build-back gate passes (tarball is buildable Corresponding
  Source incl. nested submodules).
- Static-CRT assertion passes (portable zip is self-contained).
- Manual smoke checklist (§7) passes on a fresh Windows VM, including no-logoff
  icon refresh and both-scope upgrade.
- `VERSION` returns to `0.0.13-dev` post-release; `check-version-sync.ps1` green.
- README documents download, install, system requirements, and the SmartScreen
  step.
- CHANGELOG has the `[0.0.12-phase10]` section with a compare link.
