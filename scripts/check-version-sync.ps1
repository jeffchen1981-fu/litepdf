#!/usr/bin/env pwsh
#Requires -Version 5.1
# Runs under BOTH Windows PowerShell 5.1 (local dev — no pwsh 7 here) and
# PowerShell 7 (CI invokes it via `shell: pwsh`). #Requires -Version 5.1 is a
# *minimum*, intentionally not an upper bound: do NOT add a PSEdition='Desktop'
# guard — it would throw in CI. The rule is to author with 5.1-compatible
# syntax only (no ?./??/ternary), which then runs identically on both.
#
# Verify that every place the version appears stays in sync with the canonical
# VERSION file. Two surfaces are covered:
#
#   1. The About dialog literal in src/ui/MainWindow.cpp. Catches the class of
#      regression observed during Phase 6 ship, where MainWindow.cpp's About
#      string lagged at v0.0.6 while VERSION had already moved to 0.0.7 —
#      undetected until Phase 7 ship pass.
#
#   2. The embedded Win32 VERSIONINFO resource. resources/litepdf.rc.in is a
#      configure_file() template whose version fields are filled by CMake from
#      VERSION at build time, so the .rc *cannot* drift by construction. This
#      gate enforces that guarantee from both ends: the template must stay
#      parametric (no hardcoded numbers sneaking back in), and the generated
#      build/litepdf.rc must match VERSION when present (CI configures before
#      this gate runs, so the generated check always fires in CI).
#
# VERSION may carry a '-dev' suffix during in-flight development; the About
# dialog displays only the public triple (no suffix). This script normalizes
# by stripping any trailing '-<word>' before comparing.
#
# Exits 0 on match, 1 on divergence with diagnostics. CI calls this in the
# build job before tests so version drift fails fast and locally-runnable.

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot

$versionFile = Join-Path $repoRoot "VERSION"
$mainWindow  = Join-Path $repoRoot "src/ui/MainWindow.cpp"
$rcTemplate  = Join-Path $repoRoot "resources/litepdf.rc.in"
$rcGenerated = Join-Path $repoRoot "build/litepdf.rc"

if (-not (Test-Path $versionFile)) { throw "VERSION file not found at $versionFile" }
if (-not (Test-Path $mainWindow))  { throw "MainWindow.cpp not found at $mainWindow" }
if (-not (Test-Path $rcTemplate))  { throw "RC template not found at $rcTemplate" }

$versionRaw = (Get-Content $versionFile -Raw).Trim()
# Validate format up front so a malformed VERSION produces a clear error here
# rather than a misleading "version mismatch" downstream.
if ($versionRaw -notmatch '^\d+\.\d+\.\d+(-.+)?$') {
    throw "VERSION file has unexpected format: '$versionRaw' (expected major.minor.patch[-prerelease])"
}
# Strip any pre-release suffix. This MUST match CMakeLists.txt's normalization
# (REGEX REPLACE "-.*$" "") so the script and the generated .rc agree for any
# suffix shape (-dev, -rc1, -rc.1, ...), not just the current -dev convention.
$expected   = $versionRaw -replace '-.*$', ''

$aboutLine = Select-String -Path $mainWindow -Pattern 'LitePDF v(\d+\.\d+\.\d+)' | Select-Object -First 1
if (-not $aboutLine) {
    throw "Could not locate 'LitePDF v<x.y.z>' literal in $mainWindow"
}
$actual = $aboutLine.Matches[0].Groups[1].Value

if ($actual -ne $expected) {
    Write-Host "[FAIL] version mismatch:"
    Write-Host "  VERSION file        : $versionRaw (normalized: $expected)"
    Write-Host "  About dialog literal: v$actual ($($mainWindow):$($aboutLine.LineNumber))"
    Write-Host ""
    Write-Host "Update the About dialog string to match VERSION (or vice versa) before tagging."
    exit 1
}

# --- Surface 2a: the .rc template must stay parametric --------------------
# The version fields must reference the CMake placeholders, never literal
# numbers. A hardcoded number here would survive configure_file() unchanged
# and silently reintroduce the drift this template exists to prevent.
$rcTemplateRaw = (Get-Content $rcTemplate -Raw)

if ($rcTemplateRaw -notmatch '(?m)^\s*FILEVERSION\s+@LITEPDF_VERSION_COMMA@\s*$' -or
    $rcTemplateRaw -notmatch '(?m)^\s*PRODUCTVERSION\s+@LITEPDF_VERSION_COMMA@\s*$') {
    Write-Host "[FAIL] resources/litepdf.rc.in: FILEVERSION/PRODUCTVERSION not parametric"
    Write-Host "  Expected both to read '@LITEPDF_VERSION_COMMA@' so CMake fills them"
    Write-Host "  from VERSION at build time. A hardcoded number reintroduces drift."
    exit 1
}
if ($rcTemplateRaw -notmatch 'VALUE\s+"FileVersion",\s+"@LITEPDF_VERSION_DOTTED@"' -or
    $rcTemplateRaw -notmatch 'VALUE\s+"ProductVersion",\s+"@LITEPDF_VERSION_DOTTED@"') {
    Write-Host "[FAIL] resources/litepdf.rc.in: FileVersion/ProductVersion string not parametric"
    Write-Host "  Expected both VALUE strings to read '@LITEPDF_VERSION_DOTTED@'."
    exit 1
}
# Belt-and-suspenders: no literal numeric version in any version field.
if ($rcTemplateRaw -match '(?m)^\s*(FILEVERSION|PRODUCTVERSION)\s+[0-9]' -or
    $rcTemplateRaw -match 'VALUE\s+"(File|Product)Version",\s+"[0-9]') {
    Write-Host "[FAIL] resources/litepdf.rc.in: found a hardcoded numeric version field"
    Write-Host "  Replace it with the @LITEPDF_VERSION_*@ placeholder."
    exit 1
}

# --- Surface 2b: the generated .rc must match VERSION ---------------------
# Present whenever the project has been configured. CI configures before this
# gate (see .github/workflows/ci.yml), so this end-to-end check always fires
# there; on an unconfigured local checkout it is skipped (the template check
# above still ran). NOTE: $rcGenerated is coupled to a 'build/' binary dir; CI
# uses `-B build`. If that ever changes, update $rcGenerated above.
$expectedComma  = ($expected -replace '\.', ',') + ',0'   # 0.0.12 -> 0,0,12,0
$expectedDotted = "$expected.0"                           # 0.0.12 -> 0.0.12.0

if (Test-Path $rcGenerated) {
    $rcGenRaw = (Get-Content $rcGenerated -Raw)

    # Every version field in the generated .rc, with its expected value. A
    # missing match is a failure (not a skip): an absent/unparseable field
    # means configure_file didn't substitute, e.g. a placeholder leaked through
    # because a CMake var was unset — exactly the case that must fail the gate.
    $rcChecks = @(
        @{ Label = 'FILEVERSION';           Pattern = '(?m)^\s*FILEVERSION\s+([0-9,]+)\s*$';        Expected = $expectedComma  },
        @{ Label = 'PRODUCTVERSION';        Pattern = '(?m)^\s*PRODUCTVERSION\s+([0-9,]+)\s*$';     Expected = $expectedComma  },
        @{ Label = 'FileVersion string';    Pattern = 'VALUE\s+"FileVersion",\s+"([0-9.]+)"';       Expected = $expectedDotted },
        @{ Label = 'ProductVersion string'; Pattern = 'VALUE\s+"ProductVersion",\s+"([0-9.]+)"';    Expected = $expectedDotted }
    )
    foreach ($check in $rcChecks) {
        $m = [regex]::Match($rcGenRaw, $check.Pattern)
        if (-not $m.Success) {
            throw "Could not locate $($check.Label) in $rcGenerated (was the .rc fully configured from VERSION?)"
        }
        $got = ($m.Groups[1].Value -replace '\s', '')
        if ($got -ne $check.Expected) {
            Write-Host "[FAIL] generated build/litepdf.rc $($check.Label) mismatch:"
            Write-Host "  VERSION (normalized): $expected -> expected $($check.Expected)"
            Write-Host "  build/litepdf.rc     : $got"
            Write-Host ""
            Write-Host "Re-run 'cmake -B build' so the .rc regenerates from VERSION."
            exit 1
        }
    }
    $rcStatus = "build/litepdf.rc all 4 version fields match ($expectedComma)"
} elseif ($env:CI -eq 'true') {
    # In CI the Configure step runs before this gate, so the generated .rc must
    # exist. If it doesn't, the gate would otherwise verify nothing about the
    # embedded version and still pass — fail loudly instead.
    Write-Host "[FAIL] build/litepdf.rc not found while running in CI."
    Write-Host "  The Configure step must generate it before this gate fires"
    Write-Host "  (.github/workflows/ci.yml). If the CMake binary dir is not 'build/',"
    Write-Host "  update this script's `$rcGenerated path."
    exit 1
} else {
    $rcStatus = "template parametric (build/litepdf.rc not generated; run cmake -B build)"
}

Write-Host "[OK] version sync: VERSION=$versionRaw"
Write-Host "       About dialog : v$actual"
Write-Host "       VERSIONINFO  : $rcStatus"

# Explicit success code so $LASTEXITCODE is deterministic for any caller,
# regardless of prior command state (a script that just falls off the end
# leaves $LASTEXITCODE untouched).
exit 0
