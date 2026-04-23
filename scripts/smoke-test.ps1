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
Start-Sleep -Milliseconds 500

# Phase 5 multi-tab + single-instance smoke: launch simple.pdf, then a second
# litepdf.exe with bookmarks.pdf. The second invocation should forward via
# single-instance IPC (WM_COPYDATA) to the first, exit with code 0, and leave
# the first instance with 2 tabs (active = most-recently-forwarded, index 1).

Write-Host "Launching first instance: $exe $fixture (simple.pdf)"
$proc3 = Start-Process -FilePath $exe `
    -ArgumentList @($fixture) `
    -PassThru -NoNewWindow
$deadline3 = (Get-Date).AddSeconds(5)
$hwnd3 = [IntPtr]::Zero
while ((Get-Date) -lt $deadline3) {
    Start-Sleep -Milliseconds 100
    if ($proc3.HasExited) { throw "first instance exited during startup (code $($proc3.ExitCode))" }
    $proc3.Refresh()
    if ($proc3.MainWindowHandle -ne [IntPtr]::Zero) { $hwnd3 = $proc3.MainWindowHandle; break }
}
if ($hwnd3 -eq [IntPtr]::Zero) {
    Stop-Process -Id $proc3.Id -Force -ErrorAction SilentlyContinue
    throw "first instance did not create a main window within 5 s"
}
Write-Host "[OK] first instance main window: $hwnd3"

# Let WM_USER_OPEN_OK land so the first tab is actually registered before the
# forwarder races in with a second WM_COPYDATA.
Start-Sleep -Seconds 1

Write-Host "Launching second instance (forwarder): $exe $bookmarksFixture"
$fwd = Start-Process -FilePath $exe `
    -ArgumentList @($bookmarksFixture) `
    -PassThru -NoNewWindow -Wait
if ($fwd.ExitCode -ne 0) {
    Stop-Process -Id $proc3.Id -Force -ErrorAction SilentlyContinue
    throw "forwarder exited with code $($fwd.ExitCode), expected 0"
}
Write-Host "[OK] forwarder exited with code 0"

# Poll the first instance's tab count via ux-probe tab-enum.
$uxProbe = Join-Path $PSScriptRoot "ux-probe.ps1"
$tabCount = -1
$activeIdx = -1
$pollDeadline = (Get-Date).AddSeconds(5)
while ((Get-Date) -lt $pollDeadline) {
    Start-Sleep -Milliseconds 200
    $raw = & $uxProbe tab-enum 2>&1 | Out-String
    try {
        $obj = $raw | ConvertFrom-Json -ErrorAction Stop
        $tabCount = [int]$obj.count
        $activeIdx = [int]$obj.active
        if ($tabCount -ge 2) { break }
    } catch {
        # ux-probe may still be coming up or emit diagnostic lines; keep polling.
    }
}
if ($tabCount -ne 2) {
    Stop-Process -Id $proc3.Id -Force -ErrorAction SilentlyContinue
    throw "expected 2 tabs after forwarder, got $tabCount"
}
if ($activeIdx -ne 1) {
    Stop-Process -Id $proc3.Id -Force -ErrorAction SilentlyContinue
    throw "expected active tab index 1 (most recent forward), got $activeIdx"
}
Write-Host "[OK] first instance has 2 tabs, active=1"

Stop-Process -Id $proc3.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500

# Phase 6 Task 10: in-doc find bar smoke. Launch search.pdf, post WM_COMMAND
# IDM_FIND (40042) to show the find bar, then confirm a child window with the
# "LitePDFFindBar" class becomes visible. Counter polling requires per-child
# text reads that ux-probe doesn't expose yet; the full m/n assertion is a
# Phase 11 UX-automation follow-up. This step is liveness-only — no crash is
# sufficient evidence that the accelerator and child-creation plumbing works.

$searchFixture = Join-Path $repoRoot "tests/fixtures/search.pdf"
if (-not (Test-Path $searchFixture)) {
    throw "fixture not found at $searchFixture"
}

Write-Host "Launching for find-bar smoke: $exe $searchFixture"
$proc4 = Start-Process -FilePath $exe `
    -ArgumentList @($searchFixture) `
    -PassThru -NoNewWindow
$deadline4 = (Get-Date).AddSeconds(5)
$hwnd4 = [IntPtr]::Zero
while ((Get-Date) -lt $deadline4) {
    Start-Sleep -Milliseconds 100
    if ($proc4.HasExited) {
        throw "litepdf.exe exited during search.pdf startup (code $($proc4.ExitCode))"
    }
    $proc4.Refresh()
    if ($proc4.MainWindowHandle -ne [IntPtr]::Zero) {
        $hwnd4 = $proc4.MainWindowHandle
        break
    }
}
if ($hwnd4 -eq [IntPtr]::Zero) {
    Stop-Process -Id $proc4.Id -Force -ErrorAction SilentlyContinue
    throw "litepdf.exe did not create a main window for search.pdf"
}
# Let first tab finish opening before we fire Ctrl+F.
Start-Sleep -Seconds 1

# Post IDM_FIND (40042) via ux-probe send-cmd.
& $uxProbe send-cmd 40042 | Out-Null
Start-Sleep -Milliseconds 300

# Confirm a LitePDFFindBar child is present + visible.
$findState = & $uxProbe find-bar-state 2>&1 | Out-String
try {
    $fb = $findState | ConvertFrom-Json -ErrorAction Stop
    if (-not $fb.present) {
        Stop-Process -Id $proc4.Id -Force -ErrorAction SilentlyContinue
        throw "find bar child window not found after Ctrl+F"
    }
    if (-not $fb.visible) {
        Stop-Process -Id $proc4.Id -Force -ErrorAction SilentlyContinue
        throw "find bar present but not visible after Ctrl+F"
    }
    Write-Host "[OK] find bar child visible after Ctrl+F"
} catch {
    Stop-Process -Id $proc4.Id -Force -ErrorAction SilentlyContinue
    throw "find-bar-state probe failed: $findState"
}

Stop-Process -Id $proc4.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500

Write-Host "[OK] smoke test passed."
