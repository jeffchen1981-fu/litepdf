#!/usr/bin/env pwsh
#Requires -Version 5.1
# Runs the litepdf-cli benchmark harness over the gated fixtures, records the
# GUI exe size, and writes one combined result JSON (Phase 11 spec §3.2).
# Authored 5.1-safe (no ?./??/ternary) so it runs identically under Windows
# PowerShell 5.1 (local) and pwsh 7 (CI). See reference_litepdf_powershell_51_only.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$CliExe,
    [Parameter(Mandatory = $true)][string]$GuiExe,
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$Iterations = 5,
    [string]$GitSha = ""
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot

# --- Validate the two binaries. Filename-checked so the size gate can never
#     accidentally record the CLI exe, and timings can never come from the GUI.
if (-not (Test-Path $CliExe)) { throw "CliExe not found: $CliExe" }
if (-not (Test-Path $GuiExe)) { throw "GuiExe not found: $GuiExe" }
$cliLeaf = Split-Path -Leaf $CliExe
$guiLeaf = Split-Path -Leaf $GuiExe
if ($cliLeaf -ne "litepdf-cli.exe") { throw "CliExe filename must be litepdf-cli.exe, got '$cliLeaf'" }
if ($guiLeaf -ne "litepdf.exe")     { throw "GuiExe filename must be litepdf.exe, got '$guiLeaf'" }

# --- Run the harness over each gated fixture ------------------------------
$fixtureNames = @("large.pdf", "simple.pdf")
$fixtures = [ordered]@{}
foreach ($name in $fixtureNames) {
    $fixturePath = Join-Path $repoRoot (Join-Path "tests/fixtures" $name)
    if (-not (Test-Path $fixturePath)) { throw "fixture not found: $fixturePath" }

    $raw = & $CliExe $fixturePath --benchmark --iterations $Iterations --json
    if ($LASTEXITCODE -ne 0) {
        throw "harness failed (exit $LASTEXITCODE) on $name"
    }
    $rawText = ($raw | Out-String).Trim()
    try {
        $obj = $rawText | ConvertFrom-Json -ErrorAction Stop
    } catch {
        throw "harness emitted non-JSON for ${name}: $rawText"
    }
    $fixtures[$name] = $obj
}

# --- Provenance -----------------------------------------------------------
# Default to the checkout this script lives in (correct for PR / local). CI
# passes -GitSha explicitly for base.json, whose source is a separate worktree.
if ([string]::IsNullOrEmpty($GitSha)) {
    $GitSha = (& git -C $repoRoot rev-parse HEAD 2>$null | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrEmpty($GitSha)) {
        throw "git rev-parse HEAD failed in $repoRoot; pass -GitSha explicitly"
    }
}
$runnerOs = $env:ImageOS
if ([string]::IsNullOrEmpty($runnerOs)) { $runnerOs = "local" }

$exeBytes = (Get-Item $GuiExe).Length

$result = [ordered]@{
    schema_version = 1
    git_sha        = $GitSha
    config         = "Release"
    runner_os      = $runnerOs
    cli_exe_path   = $CliExe
    gui_exe_path   = $GuiExe
    exe_bytes      = $exeBytes
    fixtures       = $fixtures
}

# Depth 10: result -> fixtures -> <name> -> samples[] -> elements; well above
# the PS 5.1 default of 2 so the samples array never collapses to a type name.
$json = $result | ConvertTo-Json -Depth 10
# BOM-less UTF-8. PS 5.1's Set-Content -Encoding UTF8 prepends a BOM; write
# BOM-less so the file is clean for every consumer. Resolve $Out via the PS
# provider so a relative path roots at the PS location, not the process CWD
# that [IO.File] would otherwise use.
$outFull = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Out)
[System.IO.File]::WriteAllText($outFull, $json, (New-Object System.Text.UTF8Encoding($false)))
Write-Host "[OK] wrote $Out (exe_bytes=$exeBytes, git_sha=$GitSha)"
