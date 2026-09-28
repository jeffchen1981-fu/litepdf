#!/usr/bin/env pwsh
#Requires -Version 5.1
# #60: runs cmake/CollectBuildFacts.cmake against throwaway git repositories,
# one per state the release path depends on. CI's checkout is shallow and
# tagless, so without this the exact-tag, dirty and toplevel-mismatch paths
# would first run inside the release workflow. Registered as the ctest
# version_script_selftest. 5.1-safe (no ?./??/ternary); ASCII only.
param(
    [Parameter(Mandatory = $true)][string]$CMake,
    [Parameter(Mandatory = $true)][string]$Git
)

# Under 5.1, native stderr plus "Stop" becomes a terminating NativeCommandError.
# Check exit codes explicitly instead.
$ErrorActionPreference = "Continue"

$script:ok = $true
$repoRoot  = Split-Path -Parent $PSScriptRoot
$collector = Join-Path $repoRoot "cmake/CollectBuildFacts.cmake"
$work      = Join-Path ([System.IO.Path]::GetTempPath()) ("litepdf-facts-" + [guid]::NewGuid().ToString("N"))

function Check([bool]$cond, [string]$label) {
    if ($cond) { Write-Host "[PASS] $label" }
    else { Write-Host "[FAIL] $label"; $script:ok = $false }
}

function Invoke-Git([string]$Dir, [string[]]$GitArgs) {
    & $Git -C $Dir @GitArgs 2>$null | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "git $($GitArgs -join ' ') failed in $Dir (exit $LASTEXITCODE)" }
}

function New-Repo([string]$Name) {
    $dir = Join-Path $work $Name
    New-Item -ItemType Directory -Force (Join-Path $dir "sub") | Out-Null
    Invoke-Git $dir @("init", "-q")
    Invoke-Git $dir @("config", "user.name", "selftest")
    Invoke-Git $dir @("config", "user.email", "selftest@example.invalid")
    Invoke-Git $dir @("config", "core.autocrlf", "false")
    Set-Content -Path (Join-Path $dir "a.txt") -Value "one" -Encoding ascii
    Set-Content -Path (Join-Path $dir "sub/b.txt") -Value "two" -Encoding ascii
    Invoke-Git $dir @("add", ".")
    Invoke-Git $dir @("commit", "-q", "-m", "first")
    return $dir
}

function Invoke-Collector([string]$Src, [string]$GitExe, [string]$Out) {
    & $CMake "-DGIT_EXECUTABLE=$GitExe" "-DSRC=$Src" "-DOUT=$Out" "-DVERSION=9.9.9" -P $collector | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "CollectBuildFacts.cmake failed for $Src (exit $LASTEXITCODE)" }
}

function Read-Facts([string]$Path) {
    $facts = @{}
    foreach ($line in (Get-Content $Path)) {
        if ($line -match '^#define LITEPDF_(FACT_[A-Z_]+|VERSION_TRIPLE) (.*)$') {
            $facts[$Matches[1]] = ($Matches[2] -replace '^"(.*)"$', '$1')
        }
    }
    return $facts
}

function Get-Facts([string]$Src, [string]$GitExe) {
    $out = Join-Path $work ("facts-" + [guid]::NewGuid().ToString("N") + ".h")
    Invoke-Collector $Src $GitExe $out
    return (Read-Facts $out)
}

try {
    New-Item -ItemType Directory -Force $work | Out-Null

    # A: exactly on a tag, clean.
    $repo = New-Repo "tagged"
    Invoke-Git $repo @("tag", "v9.9.9")
    $f = Get-Facts $repo $Git
    Check ($f["VERSION_TRIPLE"] -eq "9.9.9") "A: version triple is passed through"
    Check ($f["FACT_TOPLEVEL_MATCH"] -eq "1") "A: toplevel matches (8.3 temp paths resolve)"
    Check ($f["FACT_EXACT_TAG"] -eq "v9.9.9") "A: exact tag recorded"
    Check ($f["FACT_DESCRIBE"] -like "v9.9.9-0-g*") "A: long describe recorded"
    Check ($f["FACT_SHORT_SHA"] -ne "") "A: short sha recorded"
    Check ($f["FACT_DIRTY"] -eq "0") "A: clean tree"

    # B: on the tag, tracked file modified.
    Set-Content -Path (Join-Path $repo "a.txt") -Value "changed" -Encoding ascii
    $f = Get-Facts $repo $Git
    Check ($f["FACT_EXACT_TAG"] -eq "v9.9.9") "B: exact tag still recorded when dirty"
    Check ($f["FACT_DIRTY"] -eq "1") "B: dirty detected"
    Check ($f["FACT_DESCRIBE"] -like "*-dirty") "B: describe carries -dirty"

    # C: one commit past the tag.
    Invoke-Git $repo @("commit", "-q", "-am", "second")
    $f = Get-Facts $repo $Git
    Check ($f["FACT_EXACT_TAG"] -eq "") "C: no exact tag past the tag"
    Check ($f["FACT_DESCRIBE"] -like "v9.9.9-1-g*") "C: describe counts one commit"
    Check ($f["FACT_DIRTY"] -eq "0") "C: clean after commit"

    # D: no tags at all (CI's shallow, tagless checkout).
    $plain = New-Repo "untagged"
    $f = Get-Facts $plain $Git
    Check ($f["FACT_TOPLEVEL_MATCH"] -eq "1") "D: toplevel matches"
    Check ($f["FACT_EXACT_TAG"] -eq "" -and $f["FACT_DESCRIBE"] -eq "") "D: no tag facts"
    Check ($f["FACT_SHORT_SHA"] -ne "") "D: short sha still recorded"

    # E: SRC is a subdirectory, i.e. git answers for a parent repository
    # (release.yml extracts the source tarball inside the checkout).
    $f = Get-Facts (Join-Path $plain "sub") $Git
    Check ($f["FACT_TOPLEVEL_MATCH"] -eq "0") "E: foreign toplevel detected"
    Check ($f["FACT_SHORT_SHA"] -eq "" -and $f["FACT_DESCRIBE"] -eq "") "E: no facts from a foreign repo"

    # F: git unusable.
    $f = Get-Facts $repo (Join-Path $work "no-such-git.exe")
    Check ($f["FACT_TOPLEVEL_MATCH"] -eq "0" -and $f["FACT_SHORT_SHA"] -eq "") "F: missing git records no answer"

    # G: a tag outside [A-Za-z0-9._-] is blanked as a whole value.
    $odd = New-Repo "oddtag"
    Invoke-Git $odd @("tag", "v9.9.9+meta")
    $f = Get-Facts $odd $Git
    Check ($f["FACT_EXACT_TAG"] -eq "") "G: illegal tag blanked"
    Check ($f["FACT_DESCRIBE"] -eq "") "G: illegal describe blanked"
    Check ($f["FACT_SHORT_SHA"] -ne "") "G: sha unaffected"

    # H: an unchanged state leaves the header untouched (no rebuild).
    $stable = Join-Path $work "stable.h"
    Invoke-Collector $repo $Git $stable
    $t1 = (Get-Item $stable).LastWriteTimeUtc
    Start-Sleep -Milliseconds 1100
    Invoke-Collector $repo $Git $stable
    $t2 = (Get-Item $stable).LastWriteTimeUtc
    Check ($t1 -eq $t2) "H: unchanged facts keep the header's mtime"
} finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}

if ($script:ok) {
    Write-Host "[OK] CollectBuildFacts self-test passed"
    exit 0
}
Write-Host "[FAIL] CollectBuildFacts self-test had failures"
exit 1
