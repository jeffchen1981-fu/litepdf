# Phase 10 — Installer + Release Pipeline Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship a per-user Inno Setup installer, a portable zip, and an AGPL source tarball, published automatically by a tag-triggered GitHub Actions release job, cutting LitePDF's first public (pre-1.0) release `v0.0.12-phase10`.

**Architecture:** A declarative Inno Setup script (`installer/litepdf.iss`) plus a hand-authored RTF license-disclosure page produce `litepdf-setup-<ver>.exe`. A new `.github/workflows/release.yml` (trigger `on: push: tags: ['v*']`, independent of `ci.yml`) builds Release, compiles the installer, zips the portable exe, archives the source incl. submodules, runs a build-back gate, and publishes a GitHub prerelease with all three assets. `VERSION` moves `0.0.12-dev → 0.0.12` for the release commit, then `→ 0.0.13-dev`.

**Tech Stack:** Inno Setup 6 (Pascal Script `[Code]`), GitHub Actions (`windows-latest`), Chocolatey, `git-archive-all` (pip), PowerShell 5.1-floor scripts, CMake + VS2022, MuPDF static-linked.

**Spec:** [`docs/superpowers/specs/2026-06-01-phase-10-installer-design.md`](../specs/2026-06-01-phase-10-installer-design.md) (v2). The zh-TW license wording originates in [`docs/plans/2026-04-15-litepdf-design.md`](../../plans/2026-04-15-litepdf-design.md) §8.5.2.

**⚠️ This is a packaging / CI / docs phase, not a Catch2-testable code phase.** There is no `src/` logic to drive with unit tests. "Tests" here are: `ISCC.exe` compiles the script (exit 0), the release workflow's gates pass (static-CRT assert, version-sync, ctest, smoke, source build-back), and a **manual fresh-VM smoke checklist**. Each task states its concrete verification. Commit after every task.

**Branch:** `phase-10-installer` (already created; spec committed at `67a34f5`).

**Shell convention:** code blocks labelled `pwsh` run in PowerShell; blocks labelled `bash` run in Git Bash. Do not mix.

---

## File Structure

| File | Responsibility |
|---|---|
| `installer/litepdf.iss` | Inno Setup script: install scope, wizard, file-association registry, uninstall (incl. keep-config prompt), `[Code]` shell-refresh. |
| `installer/LICENSE-DISPLAY.rtf` | Informational license + third-party notices page (zh-TW), shown via `InfoBeforeFile`. |
| `installer/lang/ChineseTraditional.isl` | Inno Setup Traditional Chinese UI strings (unofficial translation, vendored). |
| `.github/workflows/release.yml` | Tag-triggered release job: build → installer → zip → tarball → build-back → publish prerelease. |
| `VERSION` | `0.0.12-dev → 0.0.12` (release commit), then `→ 0.0.13-dev`. |
| `README.md` | Download / install section, system requirements, SmartScreen note, status-line bump. |
| `CHANGELOG.md` | `[0.0.12-phase10]` section (folds in the pending `[Unreleased]` items), compare link, footer refs. |
| `docs/plans/2026-04-15-litepdf-design.md` | §8.5.6 inventory — already expanded in this PR (`67a34f5`); no further edit. |

---

## Task 1: Vendor the Traditional Chinese language file

Inno Setup 6 does not ship Traditional Chinese in its official `Languages\`
folder; it is an "unofficial" translation. Vendor it into the repo so the build
is self-contained (CI's `choco install innosetup` won't fetch unofficial langs).

**Files:**
- Create: `installer/lang/ChineseTraditional.isl`

- [ ] **Step 1: Download the pinned translation** (`pwsh`, from repo root)

```pwsh
New-Item -ItemType Directory -Force installer\lang | Out-Null
$url = "https://raw.githubusercontent.com/jrsoftware/issrc/is-6_3_3/Files/Languages/Unofficial/ChineseTraditional.isl"
Invoke-WebRequest -Uri $url -OutFile installer\lang\ChineseTraditional.isl
```

- [ ] **Step 2: Verify it parsed as an .isl (has a LanguageName line)**

Run: `Select-String -Path installer\lang\ChineseTraditional.isl -Pattern 'LanguageName' | Select-Object -First 1`
Expected: `LanguageName=繁體中文` (non-empty). The file is **UTF-8 with BOM and
CRLF line endings** (~20 KB) as shipped upstream — DO NOT re-encode or reformat.

- [ ] **Step 3: Keep it byte-exact in git** (the repo's `.gitattributes` has
  `* text=auto eol=lf`, which would strip the CRLFs from this vendored file).
  Mark `*.isl` binary so git stores the upstream bytes verbatim:

Append to `.gitattributes` (under the binary section):
```gitattributes
# Vendored Inno Setup language files: keep byte-exact (UTF-8 BOM + CRLF).
*.isl     binary
```

- [ ] **Step 4: Commit** (`bash`)

```bash
git add .gitattributes
git add --renormalize installer/lang/ChineseTraditional.isl
git commit -m "build: vendor Inno Setup Traditional Chinese language file (pinned 6.3.3)"
```
Verify the stored blob is byte-exact: `git cat-file -s :installer/lang/ChineseTraditional.isl`
should be ~20022 (not the LF-shrunk ~19639).

---

## Task 2: Author the license-disclosure RTF

The informational page content (user-facing installer UI copy; Traditional
Chinese is the intended locale). RTF cannot hold raw UTF-8 CJK reliably and
headless agents can't drive WordPad, so the RTF is **generated** by
`installer/make-license-rtf.py`, which embeds the canonical text and emits
`\uNNNN?` escapes at runtime (no hand-typed codepoints → no mojibake). The
output is pure ASCII.

**Files:**
- Create: `installer/make-license-rtf.py` (generator), `installer/LICENSE-DISPLAY.rtf` (output)

- [ ] **Step 1: Write the generator `installer/make-license-rtf.py`**

The script embeds the content below verbatim (zh-TW wording from design §8.5.2;
the 9-component inventory and the two English credit lines are mandatory — FTL §2
and the IJG license require them verbatim for binary distribution) and converts
each char: ASCII passes through, `\\`/`{`/`}` are escaped, newline → `\par`,
non-ASCII → `\uNNNN?`. RTF header:
`{\rtf1\ansi\ansicpg950\deff0{\fonttbl{\f0\fnil\fcharset136 Microsoft JhengHei;}}` +
`\viewkind4\uc1\f0\fs20`. Canonical content:

```text
─────────────────────────────────────────────
LitePDF 授權條款
─────────────────────────────────────────────

LitePDF 依 GNU Affero General Public License v3.0 (AGPL-3.0) 發佈。
本條款保障您以下權利:
  • 可自由使用本程式 (個人或商業用途均可)
  • 可取得完整原始碼: https://github.com/jeffchen1981-fu/litepdf
  • 可修改並再散佈本程式,但需沿用相同授權

若您將本程式或其衍生作品透過網路提供服務,
AGPL 要求您同樣需將您的原始碼公開 (AGPL §13)。

完整英文條款: https://www.gnu.org/licenses/agpl-3.0.html

─────────────────────────────────────────────
本程式包含之第三方元件
─────────────────────────────────────────────

• MuPDF 1.24.11 — © Artifex Software, Inc. — AGPL-3.0
• FreeType 2.13.0 — The FreeType Project — FTL
• libjpeg 9e — Independent JPEG Group — IJG License
• OpenJPEG — UCLouvain / OpenJPEG contributors — BSD-2-Clause
• Little CMS (lcms2) — © Marti Maria — MIT
• MuJS — © Artifex Software, Inc. — ISC
• jbig2dec — © Artifex Software, Inc. — AGPL-3.0
• Gumbo (gumbo-parser) — © Google, Inc. — Apache-2.0
• zlib 1.2.13 — © Jean-loup Gailly & Mark Adler — zlib License

Portions of this software are copyright © 2006-2024 The FreeType Project (www.freetype.org). All rights reserved.
This software is based in part on the work of the Independent JPEG Group.

─────────────────────────────────────────────
免責聲明
─────────────────────────────────────────────

本程式按「原樣」提供,不含任何明示或默示保證。
作者與貢獻者對任何使用後果不負責任。
```

- [ ] **Step 2: Run the generator** (`pwsh`): `python installer\make-license-rtf.py`
  (fallback `py installer\make-license-rtf.py`). Expected: `wrote ...LICENSE-DISPLAY.rtf <N> bytes`.

- [ ] **Step 3: Verify the RTF is valid pure-ASCII with escaped CJK + required strings**

Run (`pwsh`):
```pwsh
$raw = Get-Content installer\LICENSE-DISPLAY.rtf -Raw
if (-not ($raw -match '^\{\\rtf')) { Write-Error "Not RTF (missing {\rtf header)"; exit 1 }
@('Independent JPEG Group','The FreeType Project','MuPDF 1.24.11','zlib','OpenJPEG','jbig2dec') | ForEach-Object {
  if ($raw -notmatch [regex]::Escape($_)) { Write-Error "RTF missing: $_"; exit 1 }
}
$bytes = [System.IO.File]::ReadAllBytes("installer\LICENSE-DISPLAY.rtf")
if ($bytes | Where-Object { $_ -gt 127 }) { Write-Error "RTF has non-ASCII bytes (escaping failed)"; exit 1 }
if ($raw -notmatch '\\u25480\?') { Write-Error "expected 授 (╈0?) escape not found"; exit 1 }
"OK: valid pure-ASCII RTF with escaped CJK and all required strings"
```

- [ ] **Step 4: Commit** (`bash`)

```bash
git add installer/make-license-rtf.py installer/LICENSE-DISPLAY.rtf
git commit -m "docs: add installer license + third-party notices page (zh-TW, generated)"
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
; Own the whole Software\LitePDF subtree; remove it wholesale on uninstall.
; (uninsdeletekeyifempty would no-op here: LIFO uninstall evaluates the parent
;  before its Capabilities child is removed, so it's never empty at that point.)
Root: HKA; Subkey: "Software\LitePDF"; Flags: uninsdeletekey
; --- Context menu "Open with LitePDF" on the PDF ProgID ---
Root: HKA; Subkey: "Software\Classes\LitePDF.pdf\shell\openWithLitePDF"; ValueType: string; ValueName: ""; ValueData: "以 LitePDF 開啟"; Flags: uninsdeletekey; Tasks: contextmenu
Root: HKA; Subkey: "Software\Classes\LitePDF.pdf\shell\openWithLitePDF\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""; Tasks: contextmenu

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall skipifsilent
; runasoriginaluser: in a per-machine (elevated) install, open Settings in the
; invoking user's session, not the admin context. Best-effort deep-link to LitePDF.
Filename: "ms-settings:defaultapps?registeredAppUser=LitePDF"; Description: "在 Windows 設定中將 LitePDF 設為預設"; Flags: shellexec runasoriginaluser postinstall skipifsilent; Tasks: assocpdf

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
var
  ConfigDir: String;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    // Prompt whether to keep user config. Default = No (keep): MB_DEFBUTTON2.
    ConfigDir := ExpandConstant('{localappdata}\LitePDF');
    if DirExists(ConfigDir) then
    begin
      if MsgBox('要一併刪除 LitePDF 的設定資料嗎?' + #13#10 + ConfigDir + #13#10#13#10 +
                '選「否」會保留您的設定 (預設)。', mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
        DelTree(ConfigDir, True, True, True);
    end;
    // Refresh the shell after removing associations.
    SHChangeNotify($08000000, $0000, 0, 0);
  end;
end;
```

- [ ] **Step 1a: Save the `.iss` with a UTF-8 BOM** — CRITICAL. ISCC reads a
  `.iss` as the system ANSI code page UNLESS it begins with a UTF-8 BOM; without
  it, the literal Traditional Chinese in `[Tasks]`/`[Run]`/`[Code]` is mojibake
  on an en-US build runner. Most editors/`Write` tools omit the BOM, so prepend
  it at byte level (`bash`):
```bash
head -c3 installer/litepdf.iss | grep -q $'\xEF\xBB\xBF' || { printf '\xEF\xBB\xBF' | cat - installer/litepdf.iss > installer/litepdf.iss.tmp && mv installer/litepdf.iss.tmp installer/litepdf.iss; }
```
  Verify: `xxd installer/litepdf.iss | head -1` shows `efbb bf...`. (Do NOT
  round-trip through PowerShell 5.1 `Get-Content` — it reads a BOM-less UTF-8
  file as ANSI and corrupts the Chinese.)

- [ ] **Step 2: Compile locally if Inno Setup is installed (otherwise defer to CI)**

**Prerequisite:** `build\Release\litepdf.exe` must exist (the `[Files]` step
copies it). If it does not, build it first (`pwsh`):
```pwsh
cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
```
Then, if `ISCC.exe` is available locally (`pwsh`):
```pwsh
& "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe" /DMyAppVersion=0.0.12 installer\litepdf.iss
```
Expected: `Successful compile` and `installer\Output\litepdf-setup-0.0.12.exe`
exists. If Inno Setup is not installed locally, skip — the `release.yml` job
(Task 4) compiles it in CI. Do **not** commit `installer\Output\`.

- [ ] **Step 3: Ignore the build output and commit** (`pwsh` then `bash`)

```pwsh
Add-Content -Path .gitignore -Value "`n# Inno Setup build output`ninstaller/Output/"
```
```bash
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
    timeout-minutes: 120
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
          $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
          # -products "*" so Build-Tools-only hosts also match (mirrors cmake/ImportMuPDF.cmake).
          $msvc = & $vswhere -latest -products "*" -find "VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe" | Select-Object -First 1
          if (-not $msvc -or -not (Test-Path $msvc)) { Write-Error "dumpbin not found via vswhere"; exit 1 }
          $imports = & $msvc /imports build\Release\litepdf.exe
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
          $raw = (Get-Content VERSION -Raw).Trim()
          if ($raw -match '-') {
            Write-Error "VERSION still carries a pre-release suffix ($raw). Tag the release commit where VERSION has no -dev (Task 7), not the dev tree."
            exit 1
          }
          $v = $raw -replace '-.*$',''
          $tagver = ("${{ github.ref_name }}" -replace '^v','') -replace '-.*$',''
          if ($v -ne $tagver) {
            Write-Error "VERSION ($v) does not match tag numeric ($tagver)."
            exit 1
          }
          "version=$v" | Add-Content -Path $env:GITHUB_OUTPUT -Encoding utf8
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
          python -m pip install --upgrade pip
          python -m pip install git-archive-all
          # git-archive-all installs as a console script; ensure its dir is on PATH.
          $scripts = & python -c "import sysconfig; print(sysconfig.get_path('scripts'))"
          $env:PATH = "$scripts;$env:PATH"
          $v = "${{ steps.ver.outputs.version }}"
          git-archive-all --prefix "litepdf-$v/" "litepdf-$v-source.tar.gz"
          if (-not (Test-Path "litepdf-$v-source.tar.gz")) { Write-Error "tarball not produced"; exit 1 }

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
          $m = [regex]::Match($cl, "(?ms)^## \[$([regex]::Escape($v))-.*?(?=^## \[|\z)")
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
Otherwise validate it parses (`pwsh`): `Get-Content .github/workflows/release.yml -Raw | Out-Null; 'syntax read ok'` and eyeball indentation.
Expected: no errors. The workflow cannot execute until a tag is pushed (Task 8).

- [ ] **Step 3: Commit** (`bash`)

```bash
git add .github/workflows/release.yml
git commit -m "ci: add tag-triggered release workflow (installer + zip + source tarball)"
```

---

## Task 5: Update README (download / install / requirements / SmartScreen)

**Files:**
- Modify: `README.md` (status line ~5; add an "Install" section before `## Features`; update prerequisites min-OS)

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

- [ ] **Step 4: Commit** (`bash`)

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
`[Unreleased]` followed by the new release section. **Leave the existing
`## [0.0.11-phase9]` section and everything below it unchanged:**

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
- Expanded the §8.5.6 third-party license inventory to all nine bundled
  libraries (MuPDF + eight static deps: FreeType, libjpeg, OpenJPEG, lcms2,
  MuJS, jbig2dec, Gumbo, zlib) with the mandatory FreeType (FTL) and libjpeg
  (IJG) attribution lines.

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

At the bottom of the file, replace **only the first footer reference line** (the
`[Unreleased]:` line). Leave every other `[x.y.z-...]:` tag-reference line below
it untouched. Replace:
```markdown
[Unreleased]: https://github.com/jeffchen1981-fu/litepdf/compare/v0.0.10-phase8.5...HEAD
```
with (this also corrects the stale `[Unreleased]` base and adds the missing
`[0.0.11-phase9]` footer reference, which the current file lacks):
```markdown
[Unreleased]: https://github.com/jeffchen1981-fu/litepdf/compare/v0.0.12-phase10...HEAD
[0.0.12-phase10]: https://github.com/jeffchen1981-fu/litepdf/releases/tag/v0.0.12-phase10
[0.0.11-phase9]: https://github.com/jeffchen1981-fu/litepdf/releases/tag/v0.0.11-phase9
```

- [ ] **Step 3: Commit** (`bash`)

```bash
git add CHANGELOG.md
git commit -m "docs: add CHANGELOG 0.0.12-phase10 section (installer)"
```

---

## Task 7: Bump VERSION to the release value and verify the gate

**Files:**
- Modify: `VERSION` (`0.0.12-dev` → `0.0.12`)

- [ ] **Step 1: Write the release version** (`pwsh`)

```pwsh
"0.0.12" | Out-File -FilePath VERSION -Encoding ascii -NoNewline
```
(The About-dialog literal in `src/ui/MainWindow.cpp:1077` already reads
`LitePDF v0.0.12`, so no source change is needed — the gate normalizes `-dev`.)

- [ ] **Step 2: Regenerate the resource and run the version-sync gate** (`pwsh`)

```pwsh
cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release
./scripts/check-version-sync.ps1
```
This only re-runs cmake **configure** to regenerate `build/litepdf.rc` from the
updated VERSION; a full `cmake --build` is not required here.
Expected: `[OK] version sync: VERSION=0.0.12`, About `v0.0.12`, VERSIONINFO
`0,0,12,0`. Exit 0.

- [ ] **Step 3: Commit (this is the release commit the tag will point at)** (`bash`)

```bash
git add VERSION
git commit -m "release: LitePDF 0.0.12 (Phase 10 installer)"
```

---

## Task 8: Land the branch, then cut the release tag

This is the release-cutting step. The tag MUST point at the `0.0.12` release
commit (Task 7), before the `-dev` follow-up (Task 9) exists.

- [ ] **Step 1: Open the PR and land it on `main`** (`bash`)

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

- [ ] **Step 2: Tag the merge commit on `main` and push the tag** (`bash`)

```bash
git fetch origin
git checkout main && git pull --ff-only
# Confirm VERSION reads 0.0.12 at HEAD before tagging:
git show HEAD:VERSION   # expect: 0.0.12
git tag v0.0.12-phase10
git push origin v0.0.12-phase10
```

- [ ] **Step 3: Watch the release workflow and verify the published prerelease** (`bash`)

```bash
gh run watch
gh release view v0.0.12-phase10
```
Expected: the run succeeds; the release is marked **Pre-release** and lists all
three assets (`litepdf-setup-0.0.12.exe`, `litepdf-portable-0.0.12.zip`,
`litepdf-0.0.12-source.tar.gz`). If the run fails, fix the cause on `main`, then
delete any leftover draft and re-tag:
```bash
gh release delete v0.0.12-phase10 --yes   # remove the leftover draft, if any
git push --delete origin v0.0.12-phase10
git tag -d v0.0.12-phase10
git tag v0.0.12-phase10 <fixed-commit>
git push origin v0.0.12-phase10
```
(The `concurrency` guard + `--draft`-first publish make a re-run safe.)

---

## Task 9: Bump VERSION back to dev

**Files:**
- Modify: `VERSION` (`0.0.12` → `0.0.13-dev`), `src/ui/MainWindow.cpp:1077`

- [ ] **Step 1: Write the dev version on a fresh branch** (`bash` then `pwsh`)

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

- [ ] **Step 3: Regenerate + verify the gate, then commit and PR** (`pwsh` then `bash`)

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
```
Wait for CI to pass, then:
```bash
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
      keys (LitePDF no longer in "Open with"), and **prompts** to keep config
      (default keep); declining removes `%LOCALAPPDATA%\LitePDF\`.
- [ ] Re-install over an existing install upgrades in place (single "Add or
      Remove Programs" entry) — test in **both** per-user and per-machine scope.
- [ ] Per-machine install path triggers UAC and lands in real `Program Files`.

Record results (and any fixes) in the PR or a follow-up note. If a fix is
needed, it lands as a normal commit on `main` and a re-tagged release
(per Task 8 Step 3).

---

## Self-Review (completed by plan author; updated after Round-1 agent review)

**Spec coverage:** D1 unsigned + SignTool hook → Task 3 `[Setup]`. D2 tag
trigger → Task 4. D3 associations + ms-settings deep-link → Task 3
`[Registry]`/`[Run]`. D4 real prerelease + VERSION flow → Tasks 7–9. Artifacts
×3 → Task 4. InfoBeforeFile license page → Tasks 2–3. AppId / AppMutex /
Architectures / SHChangeNotify / DefaultIcon -102 / uninstall flags /
keep-config prompt → Task 3. permissions / concurrency / static-CRT / build-back
/ prerelease / draft / notes-file / choco-pin / git-archive-all PATH →
Task 4. §6 source tarball → Task 4. Expanded inventory → already in `67a34f5`,
surfaced in RTF (Task 2) and CHANGELOG (Task 6). README + system requirements →
Task 5. Smoke checklist → Task 10. All spec §10 acceptance criteria map to a task.

**Round-1 fixes applied:** RTF authored as readable zh-TW (no mojibake) via
WordPad with §8.5.2 citation; uninstaller keep-config prompt added to `[Code]`;
`git-archive-all` PATH hardened (`python -m pip` + Scripts dir on PATH);
CHANGELOG adds the missing `[0.0.11-phase9]` footer ref; Task 3 Step 2 states
the Release-build prerequisite; ms-settings gains `runasoriginaluser` +
`?registeredAppUser`; `Software\LitePDF` parent-key cleanup row; `.gitignore`
via PowerShell `Add-Content`; `GITHUB_OUTPUT` via `Add-Content -Encoding utf8`;
`timeout-minutes: 120`; raw-VERSION no-suffix assertion; `dumpbin` located via
`vswhere` (hard fail if absent); Task 8 `git fetch` before checkout; Task 9
waits for CI before merge.

**Placeholders:** none — `.iss`, `release.yml`, RTF content, CHANGELOG, and
README blocks are given in full. The one fetched file (ChineseTraditional.isl,
Task 1) is a real pinned download.

**Consistency:** AppId `{62012304-133C-41C1-98E8-CCA248396FFF}`, mutex
`Local\LitePDF_SingleInstance_v1`, ProgIDs `LitePDF.<ext>`, icon ids `-102`
(PDF) / `-101` (app/others), version string `0.0.12`, tag `v0.0.12-phase10`, and
`installer\Output\litepdf-setup-0.0.12.exe` are used identically across the
`.iss`, the workflow, and the release step.
