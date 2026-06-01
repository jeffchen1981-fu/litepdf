# Phase 10 — Installer + Release Pipeline Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship a per-user Inno Setup installer, a portable zip, and an AGPL source tarball, published automatically by a tag-triggered GitHub Actions release job, cutting LitePDF's first public (pre-1.0) release `v0.0.12-phase10`.

**Architecture:** A declarative Inno Setup script (`installer/litepdf.iss`) plus a hand-authored RTF license-disclosure page produce `litepdf-setup-<ver>.exe`. A new `.github/workflows/release.yml` (trigger `on: push: tags: ['v*']`, independent of `ci.yml`) builds Release, compiles the installer, zips the portable exe, archives the source incl. submodules, runs a build-back gate, and publishes a GitHub prerelease with all three assets. `VERSION` moves `0.0.12-dev → 0.0.12` for the release commit, then `→ 0.0.13-dev`.

**Tech Stack:** Inno Setup 6 (Pascal Script `[Code]`), GitHub Actions (`windows-latest`), Chocolatey, `git-archive-all` (pip), PowerShell 5.1-floor scripts, CMake + VS2022, MuPDF static-linked.

**Spec:** [`docs/superpowers/specs/2026-06-01-phase-10-installer-design.md`](../specs/2026-06-01-phase-10-installer-design.md) (v2).

**⚠️ This is a packaging / CI / docs phase, not a Catch2-testable code phase.** There is no `src/` logic to drive with unit tests. "Tests" here are: `ISCC.exe` compiles the script (exit 0), the release workflow's gates pass (static-CRT assert, version-sync, ctest, smoke, source build-back), and a **manual fresh-VM smoke checklist**. Each task states its concrete verification. Commit after every task.

**Branch:** `phase-10-installer` (already created; spec committed at `67a34f5`).

---

## File Structure

| File | Responsibility |
|---|---|
| `installer/litepdf.iss` | Inno Setup script: install scope, wizard, file-association registry, uninstall, `[Code]` shell-refresh. |
| `installer/LICENSE-DISPLAY.rtf` | Informational license + third-party notices page (zh-TW), shown via `InfoBeforeFile`. |
| `installer/lang/ChineseTraditional.isl` | Inno Setup Traditional Chinese UI strings (unofficial translation, vendored). |
| `.github/workflows/release.yml` | Tag-triggered release job: build → installer → zip → tarball → build-back → publish prerelease. |
| `VERSION` | `0.0.12-dev → 0.0.12` (release commit), then `→ 0.0.13-dev`. |
| `README.md` | Download / install section, system requirements, SmartScreen note, status-line bump. |
| `CHANGELOG.md` | `[0.0.12-phase10]` section (folds in the pending `[Unreleased]` items), compare link, footer ref. |
| `docs/plans/2026-04-15-litepdf-design.md` | §8.5.6 inventory — already expanded in this PR (`67a34f5`); no further edit. |

---

## Task 1: Vendor the Traditional Chinese language file

Inno Setup 6 does not ship Traditional Chinese in its official `Languages\`
folder; it is an "unofficial" translation. Vendor it into the repo so the build
is self-contained (CI's `choco install innosetup` won't fetch unofficial langs).

**Files:**
- Create: `installer/lang/ChineseTraditional.isl`

- [ ] **Step 1: Download the pinned translation**

Run (from repo root):

```pwsh
New-Item -ItemType Directory -Force installer\lang | Out-Null
$url = "https://raw.githubusercontent.com/jrsoftware/issrc/is-6_3_3/Files/Languages/Unofficial/ChineseTraditional.isl"
Invoke-WebRequest -Uri $url -OutFile installer\lang\ChineseTraditional.isl
```

- [ ] **Step 2: Verify it parsed as an .isl (has a LangOptions block)**

Run: `Select-String -Path installer\lang\ChineseTraditional.isl -Pattern 'LanguageName' | Select-Object -First 1`
Expected: a line like `LanguageName=...` (non-empty). The file is UTF-8/UTF-16 with BOM — leave its encoding untouched.

- [ ] **Step 3: Commit**

```bash
git add installer/lang/ChineseTraditional.isl
git commit -m "build: vendor Inno Setup Traditional Chinese language file (pinned 6.3.3)"
```

---

## Task 2: Author the license-disclosure RTF

The informational page content. Mirrors design §8.5.2 wording + the expanded
§8.5.6 inventory + the mandatory FreeType (FTL) and libjpeg (IJG) credit lines.

**Files:**
- Create: `installer/LICENSE-DISPLAY.rtf`

- [ ] **Step 1: Write the RTF**

Create `installer/LICENSE-DISPLAY.rtf` with exactly this content (it is a valid
minimal RTF; `\b`/`\b0` bold section headers, `\par` line breaks, `\u####?`
escapes for CJK so the file stays ASCII-safe and survives any editor):

```rtf
{\rtf1\ansi\deff0{\fonttbl{\f0\fnil\fcharset136 Microsoft JhengHei;}{\f1\fnil\fcharset0 Segoe UI;}}
\viewkind4\uc1\f0\fs20
\b LitePDF ╈0?❀2?♸1?❅4?\b0\par
\par
LitePDF ‸1? GNU Affero General Public License v3.0 (AGPL-3.0) 〳2? 6?。\par
♁2?♸1?❅4?⁄5?㡕6?⑴4?’7?下?❀2?℃3?:\par
•  ⅈ7?㌥8?　1?‵1?⦙2?♁2?ㄤ3?␳5? (⁉1?―4?┑0?Ↄ0?⦙2?〴2?ⅈ7?)\par
•  ⅈ7?ⅆ2?⑇1?⍃6?▗2?⅀7?⊘7?ゐ8?: https://github.com/jeffchen1981-fu/litepdf\par
•  ⅈ7?⁆2?░3? 6?₇7?▕5? 6?, 4?㡥6?➃9?⦙2?ぅ6?⅑6?╈0?❀2?\par
\par
㍐9?⑴4?⍕9?♁2?ㄤ3?␳5?㚇9?㚔2?㈗8?㘳5?╕2?‷9?☸1?℠9?,AGPL 㔠1?❱4?⑴4?₄4?㠨3?ぅ6?␡2?⅀7?⊘7?ゐ8? (AGPL §13)。\par
⍃6?▗2?㍒1?▙1?♸1?❅4?: https://www.gnu.org/licenses/agpl-3.0.html\par
\par
\b ♁2?ㄤ3?␳5?℥3?⅔7? 3?ㅓ2?三?☄1?₀3?‡4?\b0\par
\par
• MuPDF 1.24.11 — © Artifex Software, Inc. — AGPL-3.0\par
• FreeType 2.13.0 — The FreeType Project — FTL\par
• libjpeg 9e — Independent JPEG Group — IJG License\par
• OpenJPEG — UCLouvain / OpenJPEG contributors — BSD-2-Clause\par
• Little CMS (lcms2) — © Marti Maria — MIT\par
• MuJS — © Artifex Software, Inc. — ISC\par
• jbig2dec — © Artifex Software, Inc. — AGPL-3.0\par
• Gumbo (gumbo-parser) — © Google, Inc. — Apache-2.0\par
• zlib 1.2.13 — © Jean-loup Gailly & Mark Adler — zlib License\par
\par
Portions of this software are copyright © 2006-2024 The FreeType Project (www.freetype.org). All rights reserved.\par
This software is based in part on the work of the Independent JPEG Group.\par
\par
\b ₁3?㘁2?㊆2?☒6?\b0\par
\par
♁2?ㄤ3?␳5?┵3?「?⅀7?✗1?」?╕2?‷9?,不?⅔7?‡9?‰9?☒6?㄃4?┑0?䁦4?㄃4?⁄5?㕥7?。\par
‱6?㉷3?㌨7?㘁1?獻?㉷3?⍖5?‡9?‰9?‵1?⦙2?⑆0?♒4?不?㘀0?㘁2?‡9?。\par
}
```

> **Authoring note:** the `\u####?` sequences are decimal Unicode code points
> (the trailing `?` is the mandatory RTF fallback char). If you prefer, author
> the visible text in a real RTF editor (WordPad) using the **plain-text content
> shown in spec §8.5.2 plus the 9-line inventory above and the two English
> credit lines**, save as RTF — the rendered result must contain all 9
> components and both verbatim FreeType/libjpeg credit lines. The two English
> lines must appear verbatim (FTL §2 and IJG legal requirement).

- [ ] **Step 2: Verify the RTF opens and carries the mandatory credit lines**

Run:
```pwsh
(Get-Content installer\LICENSE-DISPLAY.rtf -Raw) -match 'Independent JPEG Group'
(Get-Content installer\LICENSE-DISPLAY.rtf -Raw) -match 'The FreeType Project'
```
Expected: both `True`. Open it in WordPad to eyeball the CJK renders correctly.

- [ ] **Step 3: Commit**

```bash
git add installer/LICENSE-DISPLAY.rtf
git commit -m "docs: add installer license + third-party notices page (zh-TW)"
```

---

## Task 3: Write the Inno Setup script

**Files:**
- Create: `installer/litepdf.iss`

- [ ] **Step 1: Write `installer/litepdf.iss`**

```iss
; LitePDF installer — Phase 10. Per-user default, opt-in per-machine.
; Build:  ISCC.exe /DMyAppVersion=0.0.12 installer\litepdf.iss
; Output: installer\Output\litepdf-setup-0.0.12.exe

#ifndef MyAppVersion
  #error "MyAppVersion must be passed: ISCC /DMyAppVersion=x.y.z"
#endif

#define MyAppName "LitePDF"
#define MyAppExeName "litepdf.exe"
#define MyAppPublisher "LitePDF"
#define MyAppURL "https://github.com/jeffchen1981-fu/litepdf"

[Setup]
; AppId is the stable identity key — generated once, NEVER change it.
AppId={{62012304-133C-41C1-98E8-CCA248396FFF}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}/issues
AppUpdatesURL={#MyAppURL}/releases
DefaultDirName={localappdata}\Programs\LitePDF
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
; Per-user by default (no UAC); allow opt-in elevation to per-machine.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
UsePreviousPrivileges=yes
; 64-bit only (the exe is built -A x64).
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Win10 1903+ (Direct2D/DWrite + ms-settings: URI).
MinVersion=10.0.18362
; Offer to close a running instance before overwriting the in-use exe.
AppMutex=Local\LitePDF_SingleInstance_v1
CloseApplications=yes
RestartApplications=no
OutputDir=Output
OutputBaseFilename=litepdf-setup-{#MyAppVersion}
SetupIconFile=..\assets\icon\litepdf-app.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
; Informational disclosure page (NO agree/disagree radios by design).
InfoBeforeFile=LICENSE-DISPLAY.rtf
; --- Code signing hook (disabled — app is unsigned in Phase 10; see README
;     SmartScreen note). To enable later: configure a SignTool in Inno and
;     uncomment the next line.
; SignTool=mysigntool $f

[Languages]
Name: "zh_tw"; MessagesFile: "lang\ChineseTraditional.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "assocpdf"; Description: "設為 .pdf 預設程式 (稍後在 Windows 設定中確認)"; Flags: unchecked
Name: "assocothers"; Description: "關聯 .epub / .cbz / .xps"; Flags: unchecked
Name: "contextmenu"; Description: "新增「以 LitePDF 開啟」右鍵選單"; Flags: unchecked

[Files]
Source: "..\build\Release\litepdf.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
; HKA = HKEY_AUTO: resolves to HKCU (per-user) or HKLM (per-machine) by scope.
; --- PDF ProgID (red IDI_PDFDOC icon via negative resource id -102) ---
Root: HKA; Subkey: "Software\Classes\LitePDF.pdf"; ValueType: string; ValueName: ""; ValueData: "LitePDF PDF Document"; Flags: uninsdeletekey; Tasks: assocpdf
Root: HKA; Subkey: "Software\Classes\LitePDF.pdf\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExeName},-102"; Tasks: assocpdf
Root: HKA; Subkey: "Software\Classes\LitePDF.pdf\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""; Tasks: assocpdf
Root: HKA; Subkey: "Software\Classes\.pdf\OpenWithProgids"; ValueType: string; ValueName: "LitePDF.pdf"; ValueData: ""; Flags: uninsdeletevalue; Tasks: assocpdf
; --- ePub / CBZ / XPS ProgIDs (default app icon -101) ---
Root: HKA; Subkey: "Software\Classes\LitePDF.epub"; ValueType: string; ValueName: ""; ValueData: "LitePDF ePub Document"; Flags: uninsdeletekey; Tasks: assocothers
Root: HKA; Subkey: "Software\Classes\LitePDF.epub\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExeName},-101"; Tasks: assocothers
Root: HKA; Subkey: "Software\Classes\LitePDF.epub\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""; Tasks: assocothers
Root: HKA; Subkey: "Software\Classes\.epub\OpenWithProgids"; ValueType: string; ValueName: "LitePDF.epub"; ValueData: ""; Flags: uninsdeletevalue; Tasks: assocothers
Root: HKA; Subkey: "Software\Classes\LitePDF.cbz"; ValueType: string; ValueName: ""; ValueData: "LitePDF Comic Archive"; Flags: uninsdeletekey; Tasks: assocothers
Root: HKA; Subkey: "Software\Classes\LitePDF.cbz\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExeName},-101"; Tasks: assocothers
Root: HKA; Subkey: "Software\Classes\LitePDF.cbz\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""; Tasks: assocothers
Root: HKA; Subkey: "Software\Classes\.cbz\OpenWithProgids"; ValueType: string; ValueName: "LitePDF.cbz"; ValueData: ""; Flags: uninsdeletevalue; Tasks: assocothers
Root: HKA; Subkey: "Software\Classes\LitePDF.xps"; ValueType: string; ValueName: ""; ValueData: "LitePDF XPS Document"; Flags: uninsdeletekey; Tasks: assocothers
Root: HKA; Subkey: "Software\Classes\LitePDF.xps\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExeName},-101"; Tasks: assocothers
Root: HKA; Subkey: "Software\Classes\LitePDF.xps\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""; Tasks: assocothers
Root: HKA; Subkey: "Software\Classes\.xps\OpenWithProgids"; ValueType: string; ValueName: "LitePDF.xps"; ValueData: ""; Flags: uninsdeletevalue; Tasks: assocothers
; --- Default Programs capabilities (so LitePDF appears in Settings > Default apps) ---
Root: HKA; Subkey: "Software\LitePDF\Capabilities"; ValueType: string; ValueName: "ApplicationName"; ValueData: "LitePDF"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\LitePDF\Capabilities"; ValueType: string; ValueName: "ApplicationDescription"; ValueData: "Lightweight PDF / ePub / CBZ / XPS reader"
Root: HKA; Subkey: "Software\LitePDF\Capabilities\FileAssociations"; ValueType: string; ValueName: ".pdf"; ValueData: "LitePDF.pdf"; Tasks: assocpdf
Root: HKA; Subkey: "Software\LitePDF\Capabilities\FileAssociations"; ValueType: string; ValueName: ".epub"; ValueData: "LitePDF.epub"; Tasks: assocothers
Root: HKA; Subkey: "Software\LitePDF\Capabilities\FileAssociations"; ValueType: string; ValueName: ".cbz"; ValueData: "LitePDF.cbz"; Tasks: assocothers
Root: HKA; Subkey: "Software\LitePDF\Capabilities\FileAssociations"; ValueType: string; ValueName: ".xps"; ValueData: "LitePDF.xps"; Tasks: assocothers
Root: HKA; Subkey: "Software\RegisteredApplications"; ValueType: string; ValueName: "LitePDF"; ValueData: "Software\LitePDF\Capabilities"; Flags: uninsdeletevalue
; --- Context menu "Open with LitePDF" on the PDF ProgID ---
Root: HKA; Subkey: "Software\Classes\LitePDF.pdf\shell\openWithLitePDF"; ValueType: string; ValueName: ""; ValueData: "以 LitePDF 開啟"; Flags: uninsdeletekey; Tasks: contextmenu
Root: HKA; Subkey: "Software\Classes\LitePDF.pdf\shell\openWithLitePDF\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""; Tasks: contextmenu

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall skipifsilent
Filename: "ms-settings:defaultapps"; Description: "在 Windows 設定中將 LitePDF 設為預設"; Flags: shellexec postinstall skipifsilent; Tasks: assocpdf

[Code]
procedure SHChangeNotify(wEventId: Integer; uFlags: Cardinal; dwItem1, dwItem2: Cardinal);
  external 'SHChangeNotify@shell32.dll stdcall';

procedure CurStepChanged(CurStep: TSetupStep);
begin
  // SHCNE_ASSOCCHANGED = $08000000, SHCNF_IDLIST = $0000.
  // Refresh Explorer's icon/assoc cache so .pdf icons update without logoff.
  if CurStep = ssPostInstall then
    SHChangeNotify($08000000, $0000, 0, 0);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
    SHChangeNotify($08000000, $0000, 0, 0);
end;
```

- [ ] **Step 2: Compile locally if Inno Setup is installed (otherwise defer to CI)**

If `ISCC.exe` is available locally:
```pwsh
& "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe" /DMyAppVersion=0.0.12 installer\litepdf.iss
```
Expected: `Successful compile` and `installer\Output\litepdf-setup-0.0.12.exe` exists. (Requires `build\Release\litepdf.exe` to exist — run a Release build first.) If Inno Setup is not installed locally, skip; the `release.yml` job (Task 5) compiles it in CI. Do **not** commit the `installer\Output\` artifact — add it to `.gitignore` in Step 3.

- [ ] **Step 3: Ignore the build output and commit**

```bash
printf '\n# Inno Setup build output\ninstaller/Output/\n' >> .gitignore
git add installer/litepdf.iss .gitignore
git commit -m "build: add Inno Setup installer script (per-user default, file assoc)"
```

---

## Task 4: Write the release workflow

**Files:**
- Create: `.github/workflows/release.yml`

- [ ] **Step 1: Write `.github/workflows/release.yml`**

```yaml
name: Release

on:
  push:
    tags: ['v*']

permissions:
  contents: write

concurrency:
  group: release-${{ github.ref }}
  cancel-in-progress: false

jobs:
  release:
    runs-on: windows-latest
    steps:
      - name: Checkout with submodules
        uses: actions/checkout@v5
        with:
          submodules: recursive
          fetch-depth: 0

      - name: Configure
        run: cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release

      - name: Build
        run: cmake --build build --config Release --parallel

      - name: Assert static CRT (portable zip must be self-contained)
        shell: pwsh
        run: |
          $dumpbin = (Get-ChildItem "${env:ProgramFiles}\Microsoft Visual Studio\2022\*\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe" -ErrorAction SilentlyContinue | Select-Object -First 1).FullName
          if (-not $dumpbin) { Write-Host "dumpbin not found; skipping CRT assertion"; exit 0 }
          $imports = & $dumpbin /imports build\Release\litepdf.exe
          if ($imports -match 'VCRUNTIME\d+\.dll' -or $imports -match 'MSVCP\d+\.dll') {
            Write-Error "litepdf.exe imports the dynamic VC++ runtime (/MD) — portable zip would not be self-contained. Expected /MT."
            exit 1
          }
          Write-Host "[OK] no dynamic VC++ runtime imports (static CRT confirmed)."

      - name: Version sync gate
        shell: pwsh
        run: ./scripts/check-version-sync.ps1

      - name: Unit tests
        run: ctest --test-dir build -C Release --output-on-failure

      - name: Smoke test
        shell: pwsh
        run: ./scripts/smoke-test.ps1

      - name: Derive + verify version
        id: ver
        shell: pwsh
        run: |
          $v = ((Get-Content VERSION -Raw).Trim()) -replace '-.*$',''
          $tagver = ("${{ github.ref_name }}" -replace '^v','') -replace '-.*$',''
          if ($v -ne $tagver) {
            Write-Error "VERSION ($v) does not match tag numeric ($tagver). Tag the 0.0.12 release commit, not the -dev follow-up."
            exit 1
          }
          "version=$v" | Out-File -FilePath $env:GITHUB_OUTPUT -Append
          Write-Host "Release version: $v"

      - name: Install Inno Setup (pinned)
        run: choco install innosetup --version=6.3.3 -y --no-progress

      - name: Compile installer
        shell: pwsh
        run: |
          & "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe" /DMyAppVersion=${{ steps.ver.outputs.version }} installer\litepdf.iss
          if (-not (Test-Path "installer\Output\litepdf-setup-${{ steps.ver.outputs.version }}.exe")) {
            Write-Error "Installer not produced."; exit 1
          }

      - name: Build portable zip
        shell: pwsh
        run: |
          $v = "${{ steps.ver.outputs.version }}"
          New-Item -ItemType Directory -Force portable | Out-Null
          Copy-Item build\Release\litepdf.exe portable\
          Copy-Item LICENSE portable\
          Copy-Item README.md portable\
          Compress-Archive -Path portable\* -DestinationPath "litepdf-portable-$v.zip" -Force

      - name: Build source tarball (incl. submodules)
        shell: pwsh
        run: |
          pip install git-archive-all
          $v = "${{ steps.ver.outputs.version }}"
          git-archive-all --prefix "litepdf-$v/" "litepdf-$v-source.tar.gz"

      - name: Source tarball build-back gate (AGPL corresponding-source proof)
        shell: pwsh
        run: |
          $v = "${{ steps.ver.outputs.version }}"
          New-Item -ItemType Directory -Force _backcheck | Out-Null
          tar -xzf "litepdf-$v-source.tar.gz" -C _backcheck
          $src = "_backcheck\litepdf-$v"
          if (-not (Test-Path "$src\third_party\mupdf\platform\win32\mupdf.sln")) {
            Write-Error "Tarball missing MuPDF submodule content — not complete corresponding source."; exit 1
          }
          cmake -S $src -B "$src\build" -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release
          cmake --build "$src\build" --config Release --parallel
          if (-not (Test-Path "$src\build\Release\litepdf.exe")) {
            Write-Error "Tarball did not build to litepdf.exe."; exit 1
          }
          Write-Host "[OK] source tarball is buildable corresponding source."

      - name: Extract changelog section for release notes
        shell: pwsh
        run: |
          $v = "${{ steps.ver.outputs.version }}"
          $cl = Get-Content CHANGELOG.md -Raw
          # Grab the block from "## [<v>-..." up to the next "## [" heading.
          $m = [regex]::Match($cl, "(?ms)^## \[$([regex]::Escape($v))-.*?(?=^## \[)")
          if ($m.Success) { $m.Value.Trim() | Out-File -Encoding utf8 notes.md }
          else { "LitePDF $v" | Out-File -Encoding utf8 notes.md }

      - name: Create draft prerelease
        env:
          GH_TOKEN: ${{ github.token }}
        shell: pwsh
        run: |
          $v = "${{ steps.ver.outputs.version }}"
          gh release create "${{ github.ref_name }}" `
            --draft --prerelease `
            --title "LitePDF ${{ github.ref_name }}" `
            --notes-file notes.md `
            "installer/Output/litepdf-setup-$v.exe" `
            "litepdf-portable-$v.zip" `
            "litepdf-$v-source.tar.gz"

      - name: Publish (un-draft) only after all assets uploaded
        env:
          GH_TOKEN: ${{ github.token }}
        run: gh release edit "${{ github.ref_name }}" --draft=false
```

- [ ] **Step 2: Lint the YAML**

Run (if `actionlint` available): `actionlint .github/workflows/release.yml`
Otherwise validate it parses: `pwsh -c "Get-Content .github/workflows/release.yml -Raw | Out-Null; 'syntax read ok'"` and eyeball indentation.
Expected: no errors. The workflow cannot be executed until a tag is pushed (Task 8).

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/release.yml
git commit -m "ci: add tag-triggered release workflow (installer + zip + source tarball)"
```

---

## Task 5: Update README (download / install / requirements / SmartScreen)

**Files:**
- Modify: `README.md` (status line ~5; add an "Install" section after the intro; update prerequisites min-OS)

- [ ] **Step 1: Bump the status line**

Replace line 5:
```markdown
- **Status:** under development (Phase 9 shipped — Tier 3 feature-complete + print support + app/document icons; tag `v0.0.11-phase9`)
```
with:
```markdown
- **Status:** under development (Phase 10 — installer + first public release `v0.0.12-phase10`)
```

- [ ] **Step 2: Add an Install section immediately before `## Features`**

```markdown
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

```

- [ ] **Step 3: Update build prerequisite min-OS**

In `### Prerequisites`, change `- Windows 11` to:
```markdown
- Windows 10 version 1903+ or Windows 11 (64-bit)
```

- [ ] **Step 4: Commit**

```bash
git add README.md
git commit -m "docs: add README install section, system requirements, SmartScreen note"
```

---

## Task 6: Update CHANGELOG

The `[Unreleased]` section currently holds the PR #14 VERSIONINFO-derive items,
which have not shipped in any tagged release. Fold them into `[0.0.12-phase10]`
along with the new installer work, and reset `[Unreleased]`.

**Files:**
- Modify: `CHANGELOG.md`

- [ ] **Step 1: Replace the `## [Unreleased]` block and insert the new release section**

Replace the current `## [Unreleased]` section (lines 9–23) with an empty
`[Unreleased]` followed by the new release section:

```markdown
## [Unreleased]

## [0.0.12-phase10] — 2026-06-01 — Installer

### Added
- **Inno Setup installer** (`litepdf-setup-<version>.exe`): per-user by default
  (no UAC), opt-in per-machine. Optional file associations for `.pdf` (red
  document icon) / `.epub` / `.cbz` / `.xps`, an "Open with LitePDF" context
  menu entry, and Default-Programs capability registration. License page is an
  informational AGPL + third-party disclosure (no agree/disagree gate).
- **Portable zip** (`litepdf-portable-<version>.zip`): the self-contained
  `litepdf.exe` plus `LICENSE` and `README.md`.
- **Source tarball** (`litepdf-<version>-source.tar.gz`): complete AGPL
  corresponding source including the MuPDF submodule.
- **Tag-triggered release workflow** (`.github/workflows/release.yml`): builds
  Release, asserts static CRT, runs the version-sync gate + unit tests + smoke
  test, compiles the installer, archives source, verifies the tarball is
  buildable, and publishes a GitHub prerelease with all three assets.
- Expanded the §8.5.6 third-party license inventory to all eight statically
  linked libraries (FreeType, libjpeg, OpenJPEG, lcms2, MuJS, jbig2dec, Gumbo,
  zlib) with the mandatory FreeType (FTL) and libjpeg (IJG) attribution lines.

### Changed
- **Embedded Win32 version resource now derives from `VERSION` at build time.**
  `resources/litepdf.rc` became a `configure_file` template (`litepdf.rc.in`);
  CMake fills `FILEVERSION` / `PRODUCTVERSION` / `FileVersion` / `ProductVersion`
  from `PROJECT_VERSION`, so `litepdf.exe`'s embedded version can no longer drift
  from the canonical `VERSION` file. Added `CMAKE_CONFIGURE_DEPENDS` on `VERSION`
  so a bare `cmake --build` regenerates the resource instead of shipping a stale
  one.
- `scripts/check-version-sync.ps1` now also gates the version resource: it
  asserts the template stays parametric, verifies the generated `build/litepdf.rc`
  matches `VERSION` across all four version fields, fails loudly in CI when the
  generated resource is missing, and returns a deterministic exit code. Stays
  Windows PowerShell 5.1 compatible.

### Notes
- Spec: [docs/superpowers/specs/2026-06-01-phase-10-installer-design.md](docs/superpowers/specs/2026-06-01-phase-10-installer-design.md)
- Plan: [docs/superpowers/plans/2026-06-01-phase-10-installer.md](docs/superpowers/plans/2026-06-01-phase-10-installer.md)
- First public, downloadable release (pre-1.0; marked prerelease on GitHub).

[Compare 0.0.11-phase9…0.0.12-phase10](https://github.com/jeffchen1981-fu/litepdf/compare/v0.0.11-phase9...v0.0.12-phase10)
```

- [ ] **Step 2: Update the footer link references**

At the bottom of the file, change the `[Unreleased]` compare link and add the
new release tag link. Replace:
```markdown
[Unreleased]: https://github.com/jeffchen1981-fu/litepdf/compare/v0.0.10-phase8.5...HEAD
```
with:
```markdown
[Unreleased]: https://github.com/jeffchen1981-fu/litepdf/compare/v0.0.12-phase10...HEAD
[0.0.12-phase10]: https://github.com/jeffchen1981-fu/litepdf/releases/tag/v0.0.12-phase10
```

> Note: the existing `[Unreleased]` compare base was stale at `v0.0.10-phase8.5`
> (never advanced after 0.0.11). Correcting it to `v0.0.12-phase10` is intended.

- [ ] **Step 3: Commit**

```bash
git add CHANGELOG.md
git commit -m "docs: add CHANGELOG 0.0.12-phase10 section (installer)"
```

---

## Task 7: Bump VERSION to the release value and verify the gate

**Files:**
- Modify: `VERSION` (`0.0.12-dev` → `0.0.12`)

- [ ] **Step 1: Write the release version**

```pwsh
"0.0.12" | Out-File -FilePath VERSION -Encoding ascii -NoNewline
```
(The About-dialog literal in `src/ui/MainWindow.cpp:1077` already reads
`LitePDF v0.0.12`, so no source change is needed — the gate normalizes `-dev`.)

- [ ] **Step 2: Regenerate the resource and run the version-sync gate**

```pwsh
cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release
./scripts/check-version-sync.ps1
```
Expected: `[OK] version sync: VERSION=0.0.12` with About `v0.0.12` and
`VERSIONINFO ... 0,0,12,0`. Exit 0.

- [ ] **Step 3: Commit (this is the release commit the tag will point at)**

```bash
git add VERSION
git commit -m "release: LitePDF 0.0.12 (Phase 10 installer)"
```

---

## Task 8: Land the branch, then cut the release tag

This is the release-cutting step. The tag MUST point at the `0.0.12` release
commit (Task 7), before the `-dev` follow-up (Task 9) exists.

- [ ] **Step 1: Open the PR and land it on `main`**

```bash
git push -u origin phase-10-installer
gh pr create --title "Phase 10: Installer + release pipeline" \
  --body "Implements docs/superpowers/specs/2026-06-01-phase-10-installer-design.md (v2). Adds Inno Setup installer, portable zip, AGPL source tarball, and tag-triggered release.yml. Cuts the first public pre-1.0 release v0.0.12-phase10. VERSION 0.0.12-dev -> 0.0.12; CHANGELOG + README updated." \
  --base main
```
Wait for CI (`ci.yml`) to pass, then squash-merge:
```bash
gh pr merge --squash --delete-branch
```

- [ ] **Step 2: Tag the merge commit on `main` and push the tag**

```bash
git checkout main && git pull --ff-only
# Confirm VERSION reads 0.0.12 at HEAD before tagging:
git show HEAD:VERSION   # expect: 0.0.12
git tag v0.0.12-phase10
git push origin v0.0.12-phase10
```

- [ ] **Step 3: Watch the release workflow and verify the published prerelease**

```bash
gh run watch
gh release view v0.0.12-phase10
```
Expected: the run succeeds; the release is marked **Pre-release** and lists all
three assets (`litepdf-setup-0.0.12.exe`, `litepdf-portable-0.0.12.zip`,
`litepdf-0.0.12-source.tar.gz`). If the run fails, fix the cause on `main`, then
delete and re-push the tag: `git push --delete origin v0.0.12-phase10 && git tag -d v0.0.12-phase10 && git tag v0.0.12-phase10 <fixed-commit> && git push origin v0.0.12-phase10` (the `concurrency` guard + `--draft`-first publish make a re-run safe; delete any leftover draft release first with `gh release delete v0.0.12-phase10 --yes` if it exists).

---

## Task 9: Bump VERSION back to dev

**Files:**
- Modify: `VERSION` (`0.0.12` → `0.0.13-dev`)

- [ ] **Step 1: Write the dev version on a fresh branch**

```bash
git checkout -b chore-bump-0.0.13-dev main
```
```pwsh
"0.0.13-dev" | Out-File -FilePath VERSION -Encoding ascii -NoNewline
```

- [ ] **Step 2: Update the About-dialog literal to match**

`scripts/check-version-sync.ps1` compares `src/ui/MainWindow.cpp`'s
`LitePDF v<x.y.z>` literal (currently `v0.0.12`) against the normalized VERSION
(`0.0.13`). Update line ~1077:
```cpp
                        L"LitePDF v0.0.13\n\n"
```

- [ ] **Step 3: Regenerate + verify the gate, then commit and PR**

```pwsh
cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release
./scripts/check-version-sync.ps1
```
Expected: `[OK] version sync: VERSION=0.0.13-dev`, About `v0.0.13`. Exit 0.
```bash
git add VERSION src/ui/MainWindow.cpp
git commit -m "chore: resume development at 0.0.13-dev"
git push -u origin chore-bump-0.0.13-dev
gh pr create --title "chore: bump VERSION to 0.0.13-dev" --body "Post-release dev bump after v0.0.12-phase10." --base main
gh pr merge --squash --delete-branch
```

---

## Task 10: Manual fresh-VM smoke checklist

The installer cannot be Catch2-tested; this is the roadmap exit criterion. Run
on a clean Windows 10/11 x64 VM using the published `litepdf-setup-0.0.12.exe`.

- [ ] Per-user install completes with **no UAC prompt**.
- [ ] App launches from the Start-menu and Desktop shortcuts.
- [ ] License page shows Back/Next only — **no** agree/disagree radios.
- [ ] Opening a `.pdf` works; after choosing LitePDF as default in Settings, the
      `.pdf` file shows the red icon **without a logoff** (proves `SHChangeNotify`).
- [ ] LitePDF appears in the `.pdf` "Open with" list.
- [ ] Uninstall removes files + all association/ProgID/OpenWithProgids/context
      keys (LitePDF no longer in "Open with") and prompts to keep config
      (default keep); declining removes `%LOCALAPPDATA%\LitePDF\`.
- [ ] Re-install over an existing install upgrades in place (single "Add or
      Remove Programs" entry) — test in **both** per-user and per-machine scope.
- [ ] Per-machine install path triggers UAC and lands in real `Program Files`.

Record results (and any fixes) in the PR or a follow-up note. If a fix is
needed, it lands as a normal commit on `main` and a re-tagged patch release
(e.g. `v0.0.12.1` or a fresh `-phase10` re-tag per Task 8 Step 3).

---

## Self-Review (completed by plan author)

**Spec coverage:** D1 unsigned + SignTool hook → Task 3 `[Setup]`. D2 tag
trigger → Task 4. D3 associations + ms-settings → Task 3 `[Registry]`/`[Run]`.
D4 real prerelease + VERSION flow → Tasks 7–9. Artifacts ×3 → Task 4. InfoBeforeFile
license page → Tasks 2–3. AppId/AppMutex/Architectures/SHChangeNotify/DefaultIcon
-102/uninstall flags → Task 3. permissions/concurrency/static-CRT/build-back/
prerelease/draft/notes-file/choco-pin → Task 4. §6 source tarball → Task 4.
Expanded inventory → already in `67a34f5` + surfaced in RTF (Task 2) and CHANGELOG
(Task 6). README + system requirements → Task 5. Smoke checklist → Task 10. All
spec §10 acceptance criteria map to a task.

**Placeholders:** none — `.iss`, `release.yml`, RTF, CHANGELOG, and README blocks
are given in full. The one fetched file (ChineseTraditional.isl, Task 1) is a real
pinned download, not a placeholder.

**Consistency:** AppId `{62012304-133C-41C1-98E8-CCA248396FFF}`, mutex
`Local\LitePDF_SingleInstance_v1`, ProgIDs `LitePDF.<ext>`, icon ids `-102`
(PDF) / `-101` (app), version string `0.0.12`, tag `v0.0.12-phase10`, and the
`installer\Output\litepdf-setup-0.0.12.exe` path are used identically across the
`.iss`, the workflow, and the release step.
