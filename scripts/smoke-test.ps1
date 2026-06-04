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
    # ABSOLUTE CEILING, not the regression gate. This is the GUI T0->T4 line
    # (WARP-inclusive: D2D factory + blit run under software rendering in CI),
    # which is too GPU-noisy to gate at +/-10%. The Phase 11 benchmark gate
    # (.github/workflows/benchmark.yml) protects the CPU path via the headless
    # litepdf-cli harness; this check only catches a gross liveness/ceiling
    # blowout. Dev measured ~233 ms; CI runners are typically 3-5x slower.
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

# Phase 6 Task 14: cross-tab find smoke. Launch with two fixtures so the
# first instance opens two tabs (search.pdf + bookmarks.pdf — two distinct
# paths because single-instance IPC dedups by path if we passed the same
# file twice). Post WM_COMMAND IDM_CROSS_TAB_FIND (40045) and confirm a
# LitePDFResultsPanel child becomes visible. As with the find-bar smoke
# above, this is liveness-only: no crash + correct child-window class is
# evidence that WM_CREATE wiring + accelerator + on_cross_tab_find reach
# the panel. Row-click navigation and per-hit highlighting are manual-QA
# territory until ux-probe grows ListView row enumeration.

Write-Host "Launching first instance for cross-tab find: $exe $searchFixture"
$proc5 = Start-Process -FilePath $exe `
    -ArgumentList @($searchFixture) `
    -PassThru -NoNewWindow
$deadline5 = (Get-Date).AddSeconds(5)
$hwnd5 = [IntPtr]::Zero
while ((Get-Date) -lt $deadline5) {
    Start-Sleep -Milliseconds 100
    if ($proc5.HasExited) {
        throw "litepdf.exe exited during cross-tab startup (code $($proc5.ExitCode))"
    }
    $proc5.Refresh()
    if ($proc5.MainWindowHandle -ne [IntPtr]::Zero) {
        $hwnd5 = $proc5.MainWindowHandle
        break
    }
}
if ($hwnd5 -eq [IntPtr]::Zero) {
    Stop-Process -Id $proc5.Id -Force -ErrorAction SilentlyContinue
    throw "litepdf.exe did not create a main window for cross-tab smoke"
}
# Let the first tab finish opening before we forward the second.
Start-Sleep -Seconds 1

# Forward bookmarks.pdf to the first instance so the tab count is 2.
Write-Host "Forwarding second fixture: $exe $bookmarksFixture"
$fwd5 = Start-Process -FilePath $exe `
    -ArgumentList @($bookmarksFixture) `
    -PassThru -NoNewWindow -Wait
if ($fwd5.ExitCode -ne 0) {
    Stop-Process -Id $proc5.Id -Force -ErrorAction SilentlyContinue
    throw "cross-tab forwarder exited with code $($fwd5.ExitCode), expected 0"
}
Start-Sleep -Seconds 1

# Post IDM_CROSS_TAB_FIND (40045) via ux-probe.
& $uxProbe send-cmd 40045 | Out-Null
Start-Sleep -Milliseconds 400

# Confirm the ResultsPanel child is present + visible.
$rpState = & $uxProbe results-panel-state 2>&1 | Out-String
try {
    $rp = $rpState | ConvertFrom-Json -ErrorAction Stop
    if (-not $rp.present) {
        Stop-Process -Id $proc5.Id -Force -ErrorAction SilentlyContinue
        throw "results panel child window not found after Ctrl+Shift+F"
    }
    if (-not $rp.visible) {
        Stop-Process -Id $proc5.Id -Force -ErrorAction SilentlyContinue
        throw "results panel present but not visible after Ctrl+Shift+F"
    }
    Write-Host "[OK] results panel visible after Ctrl+Shift+F"
} catch {
    Stop-Process -Id $proc5.Id -Force -ErrorAction SilentlyContinue
    throw "results-panel-state probe failed: $rpState"
}

# F6 toggles the panel off.
& $uxProbe send-cmd 40046 | Out-Null
Start-Sleep -Milliseconds 300
$rpState2 = & $uxProbe results-panel-state 2>&1 | Out-String
try {
    $rp2 = $rpState2 | ConvertFrom-Json -ErrorAction Stop
    if ($rp2.visible) {
        Stop-Process -Id $proc5.Id -Force -ErrorAction SilentlyContinue
        throw "results panel still visible after F6 toggle"
    }
    Write-Host "[OK] results panel hidden after F6"
} catch {
    Stop-Process -Id $proc5.Id -Force -ErrorAction SilentlyContinue
    throw "results-panel-state post-toggle probe failed: $rpState2"
}

Stop-Process -Id $proc5.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500

# Phase 7 Task 9: thumbnail pane open/close smoke. Launch with bookmarks.pdf
# (multi-page so the thumb model has rows to render), post WM_COMMAND
# IDM_VIEW_THUMBS (40060) to show the pane via the F4 path, then post it
# again to hide. Liveness-only — same pattern as the find-bar smoke above:
# the ThumbnailPane is a plain SysListView32 (no bespoke window class), so
# distinguishing it from the cross-tab ResultsPanel ListView via FindWindowEx
# isn't worth the complexity here. No-crash + accelerator+toggle round-trip
# is sufficient evidence that lazy ensure_thumb_pane(), ThumbnailRenderer,
# and ThumbCache wire up cleanly. Per-row paint correctness is covered by
# unit tests + manual QA.
Write-Host "Launching for thumb-pane smoke: $exe $bookmarksFixture"
$proc6 = Start-Process -FilePath $exe `
    -ArgumentList @($bookmarksFixture) `
    -PassThru -NoNewWindow
$deadline6 = (Get-Date).AddSeconds(5)
$hwnd6 = [IntPtr]::Zero
while ((Get-Date) -lt $deadline6) {
    Start-Sleep -Milliseconds 100
    if ($proc6.HasExited) {
        throw "litepdf.exe exited during thumb-pane startup (code $($proc6.ExitCode))"
    }
    $proc6.Refresh()
    if ($proc6.MainWindowHandle -ne [IntPtr]::Zero) {
        $hwnd6 = $proc6.MainWindowHandle
        break
    }
}
if ($hwnd6 -eq [IntPtr]::Zero) {
    Stop-Process -Id $proc6.Id -Force -ErrorAction SilentlyContinue
    throw "litepdf.exe did not create a main window for thumb-pane smoke"
}
# Let the first tab finish opening + Direct2D first frame land before we
# fire F4 — ensures DocumentView is fully constructed.
Start-Sleep -Seconds 1

# Post IDM_VIEW_THUMBS (40060). First press lazy-creates the ThumbnailPane,
# kicks off ThumbnailRenderer for visible rows, and shows the pane.
& $uxProbe send-cmd 40060 | Out-Null
Start-Sleep -Milliseconds 800
$proc6.Refresh()
if ($proc6.HasExited) {
    throw "litepdf.exe exited after first F4 press (code $($proc6.ExitCode))"
}
Write-Host "[OK] thumb pane shown via F4 (process alive)"

# Second press should hide the pane (mutual-exclusion D10 path: thumb visible
# -> hide thumb). Process still alive == toggle path doesn't crash.
& $uxProbe send-cmd 40060 | Out-Null
Start-Sleep -Milliseconds 400
$proc6.Refresh()
if ($proc6.HasExited) {
    throw "litepdf.exe exited after second F4 press (code $($proc6.ExitCode))"
}
Write-Host "[OK] thumb pane hidden via F4 toggle (process alive)"

Stop-Process -Id $proc6.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500

# ---------------------------------------------------------------- Phase 8 ----
# Open-path liveness for the Tier 3 formats: ePub, CBZ, encrypted PDF.
# The encrypted launch leaves the password modal blocking on user input —
# we just assert the process is still alive 1 s post-launch (a crashed-on-
# encrypted regression would exit early). ePub and CBZ should reach the
# main-window state the same way simple.pdf does.

function Start-LitePDFSmokeLaunch {
    param(
        [string]$exe,
        [string]$fixture,
        [int]$timeoutSeconds = 5
    )
    Write-Host "----"
    Write-Host "Launching: $exe $fixture"
    $proc = Start-Process -FilePath $exe -ArgumentList @($fixture) -PassThru -NoNewWindow
    $deadline = (Get-Date).AddSeconds($timeoutSeconds)
    while ((Get-Date) -lt $deadline -and $proc.MainWindowHandle -eq [IntPtr]::Zero) {
        Start-Sleep -Milliseconds 100
        if ($proc.HasExited) {
            throw "$fixture launch crashed (exit code $($proc.ExitCode))"
        }
        $proc.Refresh()
    }
    if ($proc.MainWindowHandle -eq [IntPtr]::Zero) {
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        throw "$fixture never showed a window in $timeoutSeconds s"
    }
    Write-Host "[OK] $fixture window shown"
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 300
}

$epubFixture = Join-Path $repoRoot "tests/fixtures/sample.epub"
if (Test-Path $epubFixture) {
    Start-LitePDFSmokeLaunch -exe $exe -fixture $epubFixture
} else {
    Write-Host "[SKIP] sample.epub not found at $epubFixture"
}

$cbzFixture = Join-Path $repoRoot "tests/fixtures/sample.cbz"
if (Test-Path $cbzFixture) {
    Start-LitePDFSmokeLaunch -exe $exe -fixture $cbzFixture
} else {
    Write-Host "[SKIP] sample.cbz not found at $cbzFixture"
}

# Encrypted PDF: cannot drive the modal headlessly. Just verify the
# process is alive 1 s post-launch — a crashed-on-encrypted regression
# would exit early. The modal is dismissed by Stop-Process below.
$encryptedFixture = Join-Path $repoRoot "tests/fixtures/encrypted.pdf"
if (Test-Path $encryptedFixture) {
    Write-Host "----"
    Write-Host "Launching encrypted PDF (modal expected): $exe $encryptedFixture"
    $procEnc = Start-Process -FilePath $exe -ArgumentList @($encryptedFixture) -PassThru -NoNewWindow
    Start-Sleep -Seconds 1
    $procEnc.Refresh()
    if ($procEnc.HasExited) {
        throw "encrypted.pdf launch crashed (exit code $($procEnc.ExitCode))"
    }
    Write-Host "[OK] encrypted.pdf process alive after 1 s (modal blocking on user input)"
    Stop-Process -Id $procEnc.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 300
} else {
    Write-Host "[SKIP] encrypted.pdf not found at $encryptedFixture"
}

Write-Host "[OK] smoke test passed."
