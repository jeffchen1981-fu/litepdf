#!/usr/bin/env pwsh
#Requires -Version 5.1
# Verify that the version literal in the About dialog matches the canonical
# VERSION file. Catches the class of regression observed during Phase 6 ship,
# where MainWindow.cpp's About string lagged at v0.0.6 while VERSION had
# already moved to 0.0.7 — undetected until Phase 7 ship pass.
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

if (-not (Test-Path $versionFile)) { throw "VERSION file not found at $versionFile" }
if (-not (Test-Path $mainWindow))  { throw "MainWindow.cpp not found at $mainWindow" }

$versionRaw = (Get-Content $versionFile -Raw).Trim()
$expected   = $versionRaw -replace '-[A-Za-z0-9]+$', ''

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

Write-Host "[OK] version sync: VERSION=$versionRaw -> About dialog v$actual"
