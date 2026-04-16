#!/usr/bin/env pwsh
#Requires -Version 5.1
# Smoke test: build artifact checks + cold-start render + timing budget.
# Exits non-zero on any failure; CI uses this as the final gate.

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot

$exe     = Join-Path $repoRoot "build/Release/litepdf.exe"
$fixture = Join-Path $repoRoot "tests/fixtures/simple.pdf"

if (-not (Test-Path $exe)) {
    throw "exe not found at $exe - build first"
}
if (-not (Test-Path $fixture)) {
    throw "fixture not found at $fixture"
}

# Size budget. MuPDF is linked in full; feature-pruning deferred to Phase 11.
# Current Release build ~39 MB; 50 MB leaves headroom for Phase 3 additions.
$maxBytes = 50MB
$size = (Get-Item $exe).Length
if ($size -gt $maxBytes) {
    throw "litepdf.exe is $size bytes, exceeds budget of $maxBytes bytes"
}
Write-Host "[OK] exe size: $size bytes"

# Phase 3 smoke: launch with a fixture + --log-timings, confirm the window
# opens (proxy for first-page render), and assert the cold-start timing line
# is within budget. If the window never shows, the renderer never ran, so we
# wouldn't see the timing line either - window-open check from Phase 0 is
# subsumed by this phase.

$errFile = Join-Path $repoRoot "timings.log"
if (Test-Path $errFile) { Remove-Item $errFile -Force }

Write-Host "Launching: $exe $fixture --log-timings"
$proc = Start-Process -FilePath $exe `
    -ArgumentList @($fixture, "--log-timings") `
    -RedirectStandardError $errFile `
    -PassThru -NoNewWindow

# Poll for MainWindowHandle up to 5 s (slow HDDs / cold-cache CI).
$deadline = (Get-Date).AddSeconds(5)
$hwnd = [IntPtr]::Zero
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 100
    if ($proc.HasExited) {
        if (Test-Path $errFile) {
            Write-Host "stderr:"
            Get-Content $errFile | ForEach-Object { Write-Host "  $_" }
        }
        throw "litepdf.exe exited during startup (exit code $($proc.ExitCode))"
    }
    $proc.Refresh()
    if ($proc.MainWindowHandle -ne [IntPtr]::Zero) {
        $hwnd = $proc.MainWindowHandle
        break
    }
}
if ($hwnd -eq [IntPtr]::Zero) {
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    throw "litepdf.exe did not create a main window within 5 s"
}
Write-Host "[OK] main window handle: $hwnd"

# Let the first-frame render + timing log land.
Start-Sleep -Seconds 2

Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500  # allow stderr buffers to flush

if (-not (Test-Path $errFile)) {
    throw "no timings.log produced at $errFile"
}
$lines = Get-Content $errFile
$timingLine = $lines | Where-Object { $_ -match "LitePDF cold-start:" } | Select-Object -First 1
if (-not $timingLine) {
    Write-Host "stderr contents:"
    $lines | ForEach-Object { Write-Host "  $_" }
    throw "no 'LitePDF cold-start:' line found in stderr"
}

Write-Host "[OK] timing line: $timingLine"

if ($timingLine -match "T0->T4=(\d+)\s*ms") {
    $t4 = [int]$Matches[1]
    # Loose CI budget. Dev measured ~233 ms; CI runners are typically 3-5x slower.
    $budget = 1500
    if ($t4 -gt $budget) {
        throw "cold-start T0->T4 = $t4 ms exceeds budget $budget ms"
    }
    Write-Host "[OK] cold-start T0->T4 = $t4 ms (budget $budget ms)"
} else {
    throw "could not parse T0->T4 from timing line: $timingLine"
}

Remove-Item $errFile -Force -ErrorAction SilentlyContinue

# Phase 4 smoke: launch with bookmarks.pdf to exercise the outline-pane path.
# Confirms the MainWindow + OutlinePane + MRU stack comes up cleanly on a
# document with bookmark entries. Liveness-only - no timing assertion here,
# since the cold-start budget is already covered by the simple.pdf run above.

$bookmarksFixture = Join-Path $repoRoot "tests/fixtures/bookmarks.pdf"
if (-not (Test-Path $bookmarksFixture)) {
    throw "fixture not found at $bookmarksFixture"
}

Write-Host "Launching: $exe $bookmarksFixture"
$proc2 = Start-Process -FilePath $exe `
    -ArgumentList @($bookmarksFixture) `
    -PassThru -NoNewWindow

$deadline2 = (Get-Date).AddSeconds(5)
$hwnd2 = [IntPtr]::Zero
while ((Get-Date) -lt $deadline2) {
    Start-Sleep -Milliseconds 100
    if ($proc2.HasExited) {
        throw "litepdf.exe exited during bookmarks.pdf startup (exit code $($proc2.ExitCode))"
    }
    $proc2.Refresh()
    if ($proc2.MainWindowHandle -ne [IntPtr]::Zero) {
        $hwnd2 = $proc2.MainWindowHandle
        break
    }
}
if ($hwnd2 -eq [IntPtr]::Zero) {
    Stop-Process -Id $proc2.Id -Force -ErrorAction SilentlyContinue
    throw "litepdf.exe did not create a main window within 5 s for bookmarks.pdf"
}
Write-Host "[OK] bookmarks.pdf main window handle: $hwnd2"

# Title is set in MainWindow WM_USER_OPEN_OK to "LitePDF - <filename>", so it
# takes a moment past the bare window creation for the title to update.
Start-Sleep -Seconds 1
$proc2.Refresh()
$title2 = $proc2.MainWindowTitle
if ($title2 -notmatch "bookmarks") {
    Stop-Process -Id $proc2.Id -Force -ErrorAction SilentlyContinue
    throw "bookmarks.pdf window title did not contain 'bookmarks': '$title2'"
}
Write-Host "[OK] bookmarks.pdf window title: $title2"

Stop-Process -Id $proc2.Id -Force -ErrorAction SilentlyContinue

Write-Host "[OK] smoke test passed."
