#!/usr/bin/env pwsh
#Requires -Version 5.1
# Runs under BOTH Windows PowerShell 5.1 (local dev -- no pwsh 7 here) and
# PowerShell 7 (CI invokes it via `shell: pwsh`). #Requires -Version 5.1 is a
# *minimum*, intentionally not an upper bound: do NOT add a PSEdition='Desktop'
# guard -- it would throw in CI. The rule is to author with 5.1-compatible
# syntax only (no ?./??/ternary), which then runs identically on both.
#
# Verifies that the version everywhere agrees with the canonical VERSION file,
# and that the BUILT binaries classify the way the caller expects:
#
#   1. The embedded Win32 VERSIONINFO resource. resources/litepdf.rc.in is a
#      configure_file() template filled by CMake from VERSION, so the .rc
#      cannot drift by construction. The template must stay parametric, and
#      the generated build/litepdf.rc must match VERSION when present.
#
#   2. The built artifacts (#60). `litepdf-cli --version` reports the build
#      identity computed from git at build time; its numeric triple must equal
#      VERSION and litepdf.exe's ProductVersion. With -ExpectRelease the build
#      must classify as a release whose id is the tag; with -ExpectDev it must
#      carry -dev and a non-empty id. Comparing numbers alone could only pass:
#      a dev build and a release carry the same triple.
#
# The About dialog no longer holds a version literal; it reads the build
# identity at run time, so there is no source literal to check.
#
# VERSION may carry a '-<suffix>'; comparisons use the stripped triple, the
# same normalization as CMakeLists.txt (REGEX REPLACE "-.*$" "").
#
# Exits 0 on match, 1 on divergence with diagnostics. CI calls this after the
# Build step so the binaries exist. -SelfTest proves the artifact assertions
# can fail (ctest version_sync_selftest).
[CmdletBinding(DefaultParameterSetName = "Local")]
param(
    [Parameter(ParameterSetName = "Release", Mandatory = $true)][switch]$ExpectRelease,
    [Parameter(ParameterSetName = "Release")][string]$Tag = "",
    [Parameter(ParameterSetName = "Dev", Mandatory = $true)][switch]$ExpectDev,
    [Parameter(ParameterSetName = "SelfTest", Mandatory = $true)][switch]$SelfTest
)

$ErrorActionPreference = "Stop"

# Returns an array of failure messages (empty = pass). Pure: no I/O, so the
# self-test can feed it synthetic cli output.
function Test-CliIdentity {
    param(
        [string[]]$CliOutput,       # lines printed by `litepdf-cli --version`
        [string]$Expected,          # VERSION triple, e.g. 1.3.0
        [string]$ProductVersion,    # litepdf.exe ProductVersion, e.g. 1.3.0.0
        [string]$Mode,              # Release | Dev | Local
        [string]$Tag                # required tag for Release
    )
    $failures = @()
    $kv = @{}
    foreach ($line in $CliOutput) {
        $t = "$line".Trim()
        $i = $t.IndexOf("=")
        if ($i -gt 0) { $kv[$t.Substring(0, $i)] = $t.Substring($i + 1) }
    }
    foreach ($key in @("version", "build", "release")) {
        if (-not $kv.ContainsKey($key)) { $failures += "litepdf-cli --version printed no '$key=' line" }
    }
    if ($failures.Count -gt 0) { return ,$failures }

    $numeric = $kv["version"] -replace '-.*$', ''
    if ($numeric -ne $Expected) {
        $failures += "litepdf-cli version '$($kv["version"])' does not match VERSION '$Expected'"
    }
    if ($ProductVersion -ne "$Expected.0") {
        $failures += "litepdf.exe ProductVersion '$ProductVersion' does not match '$Expected.0'"
    }
    if ($Mode -eq "Release") {
        if ($kv["release"] -ne "1") { $failures += "build is not classified as a release (release=$($kv["release"]))" }
        if ($kv["version"] -ne $Expected) { $failures += "release version must be the bare triple '$Expected', got '$($kv["version"])'" }
        if ($kv["build"] -ne $Tag) { $failures += "release build id '$($kv["build"])' does not equal the tag '$Tag'" }
    } elseif ($Mode -eq "Dev") {
        if ($kv["release"] -ne "0") { $failures += "build is not classified as dev (release=$($kv["release"]))" }
        if ($kv["version"] -ne "$Expected-dev") { $failures += "dev version must be '$Expected-dev', got '$($kv["version"])'" }
        if ($kv["build"] -eq "") { $failures += "dev build id is empty (no git provenance)" }
    }
    return ,$failures
}

function Invoke-SelfTest {
    $script:selfTestOk = $true
    function Check([bool]$cond, [string]$label) {
        if ($cond) { Write-Host "[PASS] $label" }
        else { Write-Host "[FAIL] $label"; $script:selfTestOk = $false }
    }
    $rel = @("version=1.3.0", "build=v1.3.0", "release=1")
    $dev = @("version=1.3.0-dev", "build=v1.3.0-18-gf7b1ed6", "release=0")

    $f = Test-CliIdentity $rel "1.3.0" "1.3.0.0" "Release" "v1.3.0"
    Check ($f.Count -eq 0) "1: release build passes -ExpectRelease"
    $f = Test-CliIdentity $dev "1.3.0" "1.3.0.0" "Release" "v1.3.0"
    Check ($f.Count -gt 0) "2: dev build fails -ExpectRelease"
    $f = Test-CliIdentity @("version=1.3.0", "build=v1.3.0-0-g88513f2", "release=1") "1.3.0" "1.3.0.0" "Release" "v1.3.0"
    Check ($f.Count -gt 0) "3: long describe id fails -ExpectRelease"
    $f = Test-CliIdentity $dev "1.3.0" "1.3.0.0" "Dev" ""
    Check ($f.Count -eq 0) "4: dev build passes -ExpectDev"
    $f = Test-CliIdentity @("version=1.3.0-dev", "build=", "release=0") "1.3.0" "1.3.0.0" "Dev" ""
    Check ($f.Count -gt 0) "5: empty dev id fails -ExpectDev"
    $f = Test-CliIdentity $rel "1.3.0" "1.3.0.0" "Dev" ""
    Check ($f.Count -gt 0) "6: release build fails -ExpectDev"
    $f = Test-CliIdentity @("version=1.2.0-dev", "build=gabc1234", "release=0") "1.3.0" "1.3.0.0" "Local" ""
    Check ($f.Count -gt 0) "7: numeric mismatch fails"
    $f = Test-CliIdentity $dev "1.3.0" "1.2.0.0" "Local" ""
    Check ($f.Count -gt 0) "8: ProductVersion mismatch fails"
    $f = Test-CliIdentity @("version=1.3.0-dev", "release=0") "1.3.0" "1.3.0.0" "Local" ""
    Check ($f.Count -gt 0) "9: missing build= line fails"

    if ($script:selfTestOk) { Write-Host "[OK] version-sync self-test: 9/9 passed"; exit 0 }
    Write-Host "[FAIL] version-sync self-test had failures"
    exit 1
}

if ($SelfTest) {
    Invoke-SelfTest   # exits
}
$repoRoot = Split-Path -Parent $PSScriptRoot

$versionFile = Join-Path $repoRoot "VERSION"
$rcTemplate  = Join-Path $repoRoot "resources/litepdf.rc.in"
$rcGenerated = Join-Path $repoRoot "build/litepdf.rc"

if (-not (Test-Path $versionFile)) { throw "VERSION file not found at $versionFile" }
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

# --- Surface 1a: the .rc template must stay parametric --------------------
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

# --- Surface 1b: the generated .rc must match VERSION ---------------------
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

# --- Surface 2: the built binaries (#60) ----------------------------------
$mode = "Local"
if ($ExpectRelease) { $mode = "Release" }
if ($ExpectDev)     { $mode = "Dev" }
if ($mode -eq "Release" -and $Tag -eq "") { $Tag = "v$expected" }

$cliExe = Join-Path $repoRoot "build/Release/litepdf-cli.exe"
$appExe = Join-Path $repoRoot "build/Release/litepdf.exe"
if ((Test-Path $cliExe) -and (Test-Path $appExe)) {
    $cliOut = & $cliExe --version
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[FAIL] litepdf-cli --version exited $LASTEXITCODE"
        exit 1
    }
    $productVersion = (Get-Item $appExe).VersionInfo.ProductVersion
    $failures = Test-CliIdentity $cliOut $expected $productVersion $mode $Tag
    if ($failures.Count -gt 0) {
        Write-Host "[FAIL] built binaries ($mode):"
        foreach ($f in $failures) { Write-Host "  $f" }
        Write-Host "  litepdf-cli --version printed:"
        foreach ($line in $cliOut) { Write-Host "    $line" }
        exit 1
    }
    $artifactStatus = "$mode OK: " + (($cliOut | ForEach-Object { "$_".Trim() }) -join ", ")
    if ($mode -eq "Local") {
        $artifactStatus += " (classification not asserted; pass -ExpectDev or -ExpectRelease)"
    }
} elseif ($env:CI -eq "true" -or $mode -ne "Local") {
    Write-Host "[FAIL] build/Release/litepdf-cli.exe or litepdf.exe not found."
    Write-Host "  This gate asserts the built binaries; run it after the Build step."
    exit 1
} else {
    $artifactStatus = "skipped (no build/Release binaries; build first)"
}

Write-Host "[OK] version sync: VERSION=$versionRaw"
Write-Host "       VERSIONINFO  : $rcStatus"
Write-Host "       binaries     : $artifactStatus"

# Explicit success code so $LASTEXITCODE is deterministic for any caller,
# regardless of prior command state (a script that just falls off the end
# leaves $LASTEXITCODE untouched).
exit 0
