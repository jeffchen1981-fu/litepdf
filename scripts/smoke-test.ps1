#!/usr/bin/env pwsh
#Requires -Version 5.1
# Smoke test: build artifact checks + cold-start render + timing budget.
# Exits non-zero on any failure; CI uses this as the final gate.
#
# #60: -ExpectDev / -ExpectRelease assert the window title's build marker.
# ci.yml passes -ExpectDev (title must carry "<VERSION>-dev"); release.yml
# passes -ExpectRelease (title must carry no version). With neither, only the
# document name is checked.
[CmdletBinding()]
param(
    [switch]$ExpectDev,
    [switch]$ExpectRelease
)

$ErrorActionPreference = "Stop"
if ($ExpectDev -and $ExpectRelease) { throw "-ExpectDev and -ExpectRelease are mutually exclusive" }
$repoRoot = Split-Path -Parent $PSScriptRoot

# Phase 12: this script force-kills each litepdf instance, which leaves the
# abnormal-exit marker behind, so the next launch would pop the crash-restore
# "restore previous tabs?" modal instead of opening the file we pass. Suppress
# restore for the whole smoke run; child processes inherit this env var.
$env:LITEPDF_NO_RESTORE = "1"

$exe     = Join-Path $repoRoot "build/Release/litepdf.exe"
$fixture = Join-Path $repoRoot "tests/fixtures/simple.pdf"

if (-not (Test-Path $exe)) {
    throw "exe not found at $exe - build first"
}
if (-not (Test-Path $fixture)) {
    throw "fixture not found at $fixture"
}

# cjk-system-font-loader: dropped the embedded CJK font (TOFU_CJK); CJK now
# renders from Windows system fonts. Release build ~7.25 MB. 12 MB leaves headroom.
$maxBytes = 12MB
$size = (Get-Item $exe).Length
if ($size -gt $maxBytes) {
    throw "litepdf.exe is $size bytes, exceeds budget of $maxBytes bytes"
}
Write-Host "[OK] exe size: $size bytes"

# #156: litepdf's crash handler writes a minidump to %LOCALAPPDATA%\LitePDF\crashes
# for each unhandled exception. A crash that a step's checks cannot see (#155),
# or one after they pass, would otherwise go unreported, so list the dumps before
# the first launch and fail if a new one appears. Compare by name, not by count:
# every launch that does not forward to a running instance prunes the folder to
# its newest 5 dumps, so older dumps, this run's included, can vanish.
$crashDir = Join-Path $env:LOCALAPPDATA "LitePDF\crashes"
function Get-CrashDumpNames {
    if (-not (Test-Path -LiteralPath $crashDir)) { return }
    Get-ChildItem -LiteralPath $crashDir -File |
        Where-Object { $_.Extension -eq ".dmp" } |
        ForEach-Object { $_.Name }
}
$crashBaseline = @(Get-CrashDumpNames)
$crashDumpsChecked = $false

# Returns the names of the dumps that were not there before the first launch.
# Copies each to build/smoke-crashes, which ci.yml uploads when the job fails,
# with the exe and pdb that produced it, so the dump can be read without
# rebuilding that commit.
function Get-NewCrashDumps {
    $new = @(Get-CrashDumpNames | Where-Object { $crashBaseline -notcontains $_ })
    if ($new.Count -gt 0) {
        $keep = Join-Path $repoRoot "build/smoke-crashes"
        New-Item -ItemType Directory -Force -Path $keep | Out-Null
        foreach ($name in $new) {
            # A later launch may have pruned it since the listing.
            Copy-Item -LiteralPath (Join-Path $crashDir $name) -Destination $keep -Force -ErrorAction SilentlyContinue
        }
        # Best effort: a failed copy must not replace the crash report.
        $pdb = [System.IO.Path]::ChangeExtension($exe, ".pdb")
        foreach ($file in @($exe, $pdb)) {
            Copy-Item -LiteralPath $file -Destination $keep -Force -ErrorAction SilentlyContinue
        }
    }
    return $new
}

function Write-NewCrashDumps {
    param([string[]]$Names)
    Write-Host "litepdf crashed during the smoke run; new crash dump(s) in ${crashDir}:"
    foreach ($name in $Names) { Write-Host "  $name" }
}

# A failing step throws, which skips the check at the end of the script, so list
# new dumps here too: a crash is often why the step failed. break rethrows the
# step's error.
trap {
    if (($null -ne $crashBaseline) -and -not $crashDumpsChecked) {
        try {
            $newDumps = @(Get-NewCrashDumps)
            if ($newDumps.Count -gt 0) { Write-NewCrashDumps -Names $newDumps }
        } catch { }
    }
    break
}

# Ends a step by killing its litepdf instance. Stop-Process says nothing when
# the process has already gone, so a crash after the step's checks passed went
# unreported (#156): fail with its exit code instead. Stop-Process also returns
# before the process is gone, and a launch that still finds the dying instance's
# single-instance mutex tries to forward to it and exits 1, so wait for the exit.
function Stop-LitePDF {
    param(
        [System.Diagnostics.Process]$Process,
        [string]$Step
    )
    if ($Process.HasExited) {
        throw "litepdf.exe exited before the end of the $Step step (exit code $($Process.ExitCode))"
    }
    Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
    if (-not $Process.WaitForExit(5000)) {
        throw "litepdf.exe did not exit within 5 s of being stopped at the end of the $Step step"
    }
}

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
# A -NoNewWindow process object keeps no handle unless one is read, and then
# ExitCode reads empty once the process has exited. Read it now, here and after
# every launch below, so a crash reports its exit code.
$null = $proc.Handle

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

Stop-LitePDF -Process $proc -Step "cold-start"
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
$null = $proc2.Handle

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

# Title is set in MainWindow WM_USER_OPEN_OK to "LitePDF[ <ver>-dev] <U+2014> <label>",
# so it takes a moment past the bare window creation for the title to update.
# The dash is built from its code point: this file has no BOM, and Windows
# PowerShell 5.1 on a non-UTF-8 code page would misread a literal em dash.
$dash = [string][char]0x2014
if ($ExpectDev) {
    $verTriple = ((Get-Content (Join-Path $repoRoot "VERSION") -Raw).Trim()) -replace '-.*$', ''
    $titlePattern = "^LitePDF " + [regex]::Escape($verTriple) + "-dev " + $dash + " bookmarks"
} elseif ($ExpectRelease) {
    $titlePattern = "^LitePDF " + $dash + " bookmarks"
} else {
    $titlePattern = "bookmarks"
}
# The worker posts WM_USER_OPEN_OK whenever it finishes, which on a slow CI
# runner can be after a fixed sleep has ended (#150), so poll the title until
# it matches or 5 s pass, and report the last title read.
$titleDeadline = (Get-Date).AddSeconds(5)
$title2 = ""
while ((Get-Date) -lt $titleDeadline) {
    Start-Sleep -Milliseconds 100
    if ($proc2.HasExited) {
        throw "litepdf.exe exited while waiting for the bookmarks.pdf title (exit code $($proc2.ExitCode))"
    }
    $proc2.Refresh()
    $title2 = $proc2.MainWindowTitle
    if ($title2 -match $titlePattern) { break }
}
if ($title2 -notmatch $titlePattern) {
    Stop-Process -Id $proc2.Id -Force -ErrorAction SilentlyContinue
    throw "bookmarks.pdf window title '$title2' did not match '$titlePattern'"
}
Write-Host "[OK] bookmarks.pdf window title: $title2"

Stop-LitePDF -Process $proc2 -Step "bookmarks.pdf"
Start-Sleep -Milliseconds 500

# Phase 5 multi-tab + single-instance smoke: launch simple.pdf, then a second
# litepdf.exe with bookmarks.pdf. The second invocation should forward via
# single-instance IPC (WM_COPYDATA) to the first, exit with code 0, and leave
# the first instance with 2 tabs (active = most-recently-forwarded, index 1).

Write-Host "Launching first instance: $exe $fixture (simple.pdf)"
$proc3 = Start-Process -FilePath $exe `
    -ArgumentList @($fixture) `
    -PassThru -NoNewWindow
$null = $proc3.Handle
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
$tabLabels = @()
$tabEnumError = $null
$pollDeadline = (Get-Date).AddSeconds(5)
while ((Get-Date) -lt $pollDeadline) {
    Start-Sleep -Milliseconds 200
    try {
        $raw = & $uxProbe tab-enum 2>&1 | Out-String
    } catch {
        # tab-enum throws only when it found the tab control but could not
        # read litepdf's memory, which a retry will not fix. Stop polling and
        # let the checks below report it.
        $tabEnumError = $_.Exception.Message
        break
    }
    try {
        $obj = $raw | ConvertFrom-Json -ErrorAction Stop
        $tabCount = [int]$obj.count
        $activeIdx = [int]$obj.active
        $tabLabels = @($obj.labels)
        if ($tabCount -ge 2) { break }
    } catch {
        # ux-probe may still be coming up or emit diagnostic lines; keep polling.
    }
}
# #155: tab-enum once crashed this instance on every run, after the count and
# active index were already read, so check that it is still alive and that the
# labels came back: they are the part a crashed read cannot fake.
if ($proc3.HasExited) {
    throw "first instance exited during tab-enum (exit code $($proc3.ExitCode))"
}
if ($null -ne $tabEnumError) {
    Stop-Process -Id $proc3.Id -Force -ErrorAction SilentlyContinue
    throw "tab-enum failed: $tabEnumError"
}
if ($tabCount -ne 2) {
    Stop-Process -Id $proc3.Id -Force -ErrorAction SilentlyContinue
    throw "expected 2 tabs after forwarder, got $tabCount"
}
if ($activeIdx -ne 1) {
    Stop-Process -Id $proc3.Id -Force -ErrorAction SilentlyContinue
    throw "expected active tab index 1 (most recent forward), got $activeIdx"
}
$expectedLabels = @("simple.pdf", "bookmarks.pdf")
if (($tabLabels -join "|") -ne ($expectedLabels -join "|")) {
    Stop-Process -Id $proc3.Id -Force -ErrorAction SilentlyContinue
    throw "expected tab labels [$($expectedLabels -join ', ')], got [$($tabLabels -join ', ')]"
}
Write-Host "[OK] first instance has 2 tabs, active=1, labels: $($tabLabels -join ', ')"

Stop-LitePDF -Process $proc3 -Step "multi-tab"
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
$null = $proc4.Handle
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

# A command posted with `ux-probe send-cmd` runs when litepdf's UI thread gets
# to it, which on a slow CI runner can be after a fixed sleep has ended (PR
# #148 saw the results panel still visible 300 ms after F6). Re-read a probe
# every 100 ms until $Until accepts the parsed state or $TimeoutSeconds
# expires, and return the last raw reading for the caller to check and report.
function Wait-UxProbeState {
    param(
        [string]$Probe,
        [scriptblock]$Until,
        [int]$TimeoutSeconds = 5
    )
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $raw = ""
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 100
        $raw = & $uxProbe $Probe 2>&1 | Out-String
        try {
            $state = $raw | ConvertFrom-Json -ErrorAction Stop
            if (& $Until $state) { break }
        } catch {
            # ux-probe may emit diagnostic lines; keep polling.
        }
    }
    return $raw
}

# Post IDM_FIND (40042) via ux-probe send-cmd.
& $uxProbe send-cmd 40042 | Out-Null

# Confirm a LitePDFFindBar child is present + visible.
$findState = Wait-UxProbeState -Probe find-bar-state -Until { param($s) $s.present -and $s.visible }
if ($proc4.HasExited) {
    throw "litepdf.exe exited after Ctrl+F (exit code $($proc4.ExitCode))"
}
# Parse first and check outside the try, so each check's own message is the one
# reported instead of the catch's generic one (#153).
$fb = $null
try { $fb = $findState | ConvertFrom-Json -ErrorAction Stop } catch { }
if ($null -eq $fb) {
    Stop-Process -Id $proc4.Id -Force -ErrorAction SilentlyContinue
    throw "find-bar-state probe failed: $findState"
}
if (-not $fb.present) {
    Stop-Process -Id $proc4.Id -Force -ErrorAction SilentlyContinue
    throw "find bar child window not found after Ctrl+F: $findState"
}
if (-not $fb.visible) {
    Stop-Process -Id $proc4.Id -Force -ErrorAction SilentlyContinue
    throw "find bar present but not visible after Ctrl+F"
}
Write-Host "[OK] find bar child visible after Ctrl+F"

Stop-LitePDF -Process $proc4 -Step "find-bar"
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
$null = $proc5.Handle
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

# Confirm the ResultsPanel child is present + visible.
$rpState = Wait-UxProbeState -Probe results-panel-state -Until { param($s) $s.present -and $s.visible }
if ($proc5.HasExited) {
    throw "litepdf.exe exited after Ctrl+Shift+F (exit code $($proc5.ExitCode))"
}
$rp = $null
try { $rp = $rpState | ConvertFrom-Json -ErrorAction Stop } catch { }
if ($null -eq $rp) {
    Stop-Process -Id $proc5.Id -Force -ErrorAction SilentlyContinue
    throw "results-panel-state probe failed: $rpState"
}
if (-not $rp.present) {
    Stop-Process -Id $proc5.Id -Force -ErrorAction SilentlyContinue
    throw "results panel child window not found after Ctrl+Shift+F: $rpState"
}
if (-not $rp.visible) {
    Stop-Process -Id $proc5.Id -Force -ErrorAction SilentlyContinue
    throw "results panel present but not visible after Ctrl+Shift+F"
}
Write-Host "[OK] results panel visible after Ctrl+Shift+F"

# F6 toggles the panel off. on_toggle_results only hides the panel, so it must
# still be present: a gone main window also reads {"present":false,"visible":false},
# and checking only visible would pass when litepdf has crashed (#150).
& $uxProbe send-cmd 40046 | Out-Null
$rpState2 = Wait-UxProbeState -Probe results-panel-state -Until { param($s) -not $s.visible }
if ($proc5.HasExited) {
    throw "litepdf.exe exited after F6 (exit code $($proc5.ExitCode))"
}
# Parse first and check outside the try, so each check's own message is the one
# reported instead of the catch's generic one.
$rp2 = $null
try { $rp2 = $rpState2 | ConvertFrom-Json -ErrorAction Stop } catch { }
if ($null -eq $rp2) {
    Stop-Process -Id $proc5.Id -Force -ErrorAction SilentlyContinue
    throw "results-panel-state post-toggle probe failed: $rpState2"
}
if (-not $rp2.present) {
    Stop-Process -Id $proc5.Id -Force -ErrorAction SilentlyContinue
    throw "results panel child window not found after F6 toggle: $rpState2"
}
if ($rp2.visible) {
    Stop-Process -Id $proc5.Id -Force -ErrorAction SilentlyContinue
    throw "results panel still visible after F6 toggle"
}
Write-Host "[OK] results panel hidden after F6"

Stop-LitePDF -Process $proc5 -Step "cross-tab find"
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
$null = $proc6.Handle
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

Stop-LitePDF -Process $proc6 -Step "thumb-pane"
Start-Sleep -Milliseconds 500

# ------------------------------------------------------- #55 continuous strip ----
# With HKCU\SOFTWARE\LitePDF\Settings\ContinuousScroll = 1 a document opens as one
# vertical strip of pages, and the canvas has a vertical scroll bar whose range is
# the strip's height in DIPs (spec 6.2). The oracles are the scroll bar's range and
# the status-bar page box, read through ux-probe; no pixels. The preference is the
# user's own setting, so its prior value (or its absence) is put back in finally.
$largeFixture = Join-Path $repoRoot "tests/fixtures/large.pdf"
if (-not (Test-Path $largeFixture)) {
    throw "fixture not found at $largeFixture"
}
$stripKey = "HKCU:\SOFTWARE\LitePDF\Settings"
$stripHadKey = Test-Path $stripKey
$stripHadValue = $false
$stripPriorValue = $null
$stripPriorKind = $null
if ($stripHadKey) {
    $stripKeyItem = Get-Item $stripKey
    if ($stripKeyItem.GetValueNames() -contains "ContinuousScroll") {
        $stripHadValue = $true
        $stripPriorValue = $stripKeyItem.GetValue("ContinuousScroll")
        $stripPriorKind = $stripKeyItem.GetValueKind("ContinuousScroll")
    }
}

# One probe reading, parsed; $null when the probe printed something else.
function Read-UxProbeJson {
    param([string]$Probe)
    $raw = & $uxProbe $Probe 2>&1 | Out-String
    try { return ($raw | ConvertFrom-Json -ErrorAction Stop) } catch { return $null }
}

# Re-reads the canvas scroll bar and the page box every 100 ms until $Until
# (param($cs, $pb)) accepts both or $TimeoutSeconds expires. Returns the last
# pair as { cs, pb }; either is $null when its probe printed something else.
function Wait-StripState {
    param(
        [scriptblock]$Until,
        [int]$TimeoutSeconds = 5
    )
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $last = [pscustomobject]@{ cs = $null; pb = $null }
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 100
        $cs = Read-UxProbeJson -Probe canvas-scroll
        $pb = Read-UxProbeJson -Probe page-box
        $last = [pscustomobject]@{ cs = $cs; pb = $pb }
        if ($null -ne $cs -and $null -ne $pb -and (& $Until $cs $pb)) { break }
    }
    return $last
}

function Format-StripState {
    param($State)
    return "canvas-scroll $($State.cs | ConvertTo-Json -Compress), page-box $($State.pb | ConvertTo-Json -Compress)"
}

# Launches litepdf on $Fixture (ContinuousScroll is already 1) and waits until the
# canvas shows a laid-out strip: the scroll bar is there and its page is set, which
# paged mode never does. Returns the process; stops it before throwing.
function Start-StripLaunch {
    param(
        [string]$Fixture,
        [string]$Step,
        # ContinuousScroll is 0: wait for the paged first page instead (no scroll bar,
        # a page box reading 1).
        [switch]$Paged
    )
    Write-Host "Launching for continuous strip smoke ($Step): $exe $Fixture"
    $p = Start-Process -FilePath $exe -ArgumentList @($Fixture) -PassThru -NoNewWindow
    $null = $p.Handle
    $deadline = (Get-Date).AddSeconds(5)
    $hwnd = [IntPtr]::Zero
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 100
        if ($p.HasExited) {
            throw "litepdf.exe exited during the $Step startup (code $($p.ExitCode))"
        }
        $p.Refresh()
        if ($p.MainWindowHandle -ne [IntPtr]::Zero) {
            $hwnd = $p.MainWindowHandle
            break
        }
    }
    if ($hwnd -eq [IntPtr]::Zero) {
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
        throw "litepdf.exe did not create a main window for the $Step step"
    }
    if ($Paged) {
        $st = Wait-StripState -Until { param($cs, $pb) $cs.present -and -not $cs.vscroll -and $pb.text -eq "1" }
        if ($null -eq $st.cs -or $st.cs.vscroll -or $st.pb.text -ne "1") {
            Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
            throw "the $Step launch shows no paged first page 5 s after opening $Fixture : $(Format-StripState $st)"
        }
        return $p
    }
    $st = Wait-StripState -Until { param($cs, $pb) $cs.present -and $cs.vscroll -and $cs.page -gt 0 }
    if ($null -eq $st.cs -or -not ($st.cs.vscroll -and $st.cs.page -gt 0)) {
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
        throw "the $Step launch shows no laid-out strip 5 s after opening $Fixture : $(Format-StripState $st)"
    }
    return $p
}

# The ContinuousScroll value in the registry, as the menu item's write oracle; $null
# when the value is absent.
function Get-ContinuousPref {
    $v = Get-ItemProperty -Path $stripKey -Name ContinuousScroll -ErrorAction SilentlyContinue
    if ($null -eq $v) { return $null }
    return $v.ContinuousScroll
}

# Polls the registry value until it equals $Expected or $TimeoutSeconds expires;
# returns the last reading.
function Wait-ContinuousPref {
    param(
        [int]$Expected,
        [int]$TimeoutSeconds = 5
    )
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $v = Get-ContinuousPref
    while ((Get-Date) -lt $deadline -and $v -ne $Expected) {
        Start-Sleep -Milliseconds 100
        $v = Get-ContinuousPref
    }
    return $v
}

# One menu-state reading for the given command ids, parsed; $null when the probe
# printed something else. Items are read as $ms.items.'<id>'.
function Read-MenuState {
    param([string[]]$Ids)
    $raw = & $uxProbe menu-state @Ids 2>&1 | Out-String
    try { return ($raw | ConvertFrom-Json -ErrorAction Stop) } catch { return $null }
}

# Forwards $Path to the running instance as another tab (single-instance forwarding).
# The forwarder must exit 0 within 10 s; one still running at the deadline is killed,
# so no stray instance outlives the run.
function Send-ForwardedFile {
    param([string]$Path)
    $fwd = Start-Process -FilePath $exe -ArgumentList @($Path) -PassThru -NoNewWindow
    $null = $fwd.Handle
    if (-not $fwd.WaitForExit(10000)) {
        Stop-Process -Id $fwd.Id -Force -ErrorAction SilentlyContinue
        [void]$fwd.WaitForExit(5000)
        throw "the forwarder for $Path did not exit within 10 s (killed)"
    }
    if ($fwd.ExitCode -ne 0) { throw "the forwarder for $Path exited with code $($fwd.ExitCode), expected 0" }
}

# large.pdf is 500 US Letter pages (612 x 792 pt). At zoom z the strip is
# G + 500 x (792 z + G) DIPs tall, G = 8 (spec 4.2), and the scroll bar's
# nMax + 1 is that height rounded (spec 6.2).
function Get-StripExpectedHeight {
    param([double]$Zoom)
    return 8.0 + 500.0 * (792.0 * $Zoom + 8.0)
}

# core/detail/ZoomMath.hpp's kZoomPresets: Zoom In goes to the first one above
# the current zoom (next_preset, with its 1e-4 tolerance).
$stripZoomPresets = @(0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0, 4.0, 6.0, 8.0)

# The wheel rows derive one notch's step from the system's lines-per-notch setting,
# as the canvas does: 16 DIP per line (ScrollMath.hpp kWheelLineDip), so 48 DIP at
# the default 3 lines. 0 turns the wheel off and 0xFFFFFFFF scrolls a screen per
# notch; neither has a fixed step, so they are refused here, before anything is
# launched or written, rather than failing later with a misleading delta.
$stripProbe = Read-UxProbeJson -Probe canvas-scroll
if ($null -eq $stripProbe -or $null -eq $stripProbe.wheel_lines) {
    throw "canvas-scroll probe reported no wheel_lines: $($stripProbe | ConvertTo-Json -Compress)"
}
$stripWheelLines = [int64]$stripProbe.wheel_lines
if ($stripWheelLines -le 0 -or $stripWheelLines -eq 4294967295) {
    throw "smoke strip rows need a lines-per-notch wheel setting (SPI_GETWHEELSCROLLLINES = $stripWheelLines; 0 is off, 4294967295 is one screen at a time)"
}
$stripNotchDip = [double]($stripWheelLines * 16)
Write-Host "continuous strip: wheel scroll lines $stripWheelLines, $stripNotchDip DIP per notch"

$stripErr = Join-Path $repoRoot "timings-strip.log"
if (Test-Path $stripErr) { Remove-Item $stripErr -Force }
$procStrip = $null
try {
    if (-not $stripHadKey) { New-Item -Path $stripKey -Force | Out-Null }
    Set-ItemProperty -Path $stripKey -Name ContinuousScroll -Value 1 -Type DWord

    # Row 1: cold start in continuous mode. The scroll bar appears only while the
    # canvas shows the strip, so polling for it proves this run really is the strip.
    Write-Host "Launching for continuous strip smoke: $exe $largeFixture --log-timings"
    $procStrip = Start-Process -FilePath $exe `
        -ArgumentList @($largeFixture, "--log-timings") `
        -RedirectStandardError $stripErr `
        -PassThru -NoNewWindow
    $null = $procStrip.Handle
    $deadlineStrip = (Get-Date).AddSeconds(5)
    $hwndStrip = [IntPtr]::Zero
    while ((Get-Date) -lt $deadlineStrip) {
        Start-Sleep -Milliseconds 100
        if ($procStrip.HasExited) {
            throw "litepdf.exe exited during continuous strip startup (code $($procStrip.ExitCode))"
        }
        $procStrip.Refresh()
        if ($procStrip.MainWindowHandle -ne [IntPtr]::Zero) {
            $hwndStrip = $procStrip.MainWindowHandle
            break
        }
    }
    if ($hwndStrip -eq [IntPtr]::Zero) {
        throw "litepdf.exe did not create a main window for the continuous strip smoke"
    }
    $csRaw = Wait-UxProbeState -Probe canvas-scroll -Until { param($s) $s.present -and $s.vscroll }
    $cs = $null
    try { $cs = $csRaw | ConvertFrom-Json -ErrorAction Stop } catch { }
    if ($null -eq $cs) { throw "canvas-scroll probe failed: $csRaw" }
    if (-not $cs.vscroll) {
        throw "the canvas has no vertical scroll bar 5 s after opening large.pdf with ContinuousScroll = 1: $csRaw"
    }
    Write-Host "[OK] continuous strip: canvas scroll bar shown"

    # Row 2: the scroll bar shows the strip's real range (spec 9.3's probe
    # self-check). FitWidth on Letter: z = viewport width in DIPs / 612. The
    # reading is polled until it matches, since the range is set by the open's
    # render kick, which may run after the scroll bar style appears.
    $stripZoom = 0.0
    $stripExpected = 0.0
    $csRaw = Wait-UxProbeState -Probe canvas-scroll -Until {
        param($s)
        if (-not ($s.present -and $s.vscroll -and $s.dpi -gt 0)) { return $false }
        $z = ($s.client_w * 96.0 / $s.dpi) / 612.0
        return [Math]::Abs(($s.max + 1) - (Get-StripExpectedHeight -Zoom $z)) -le 2
    }
    $cs = $null
    try { $cs = $csRaw | ConvertFrom-Json -ErrorAction Stop } catch { }
    if ($null -eq $cs -or $cs.dpi -le 0) { throw "canvas-scroll probe failed: $csRaw" }
    $stripZoom = ($cs.client_w * 96.0 / $cs.dpi) / 612.0
    $stripExpected = Get-StripExpectedHeight -Zoom $stripZoom
    if ([Math]::Abs(($cs.max + 1) - $stripExpected) -gt 2) {
        throw "strip scroll range: max + 1 = $($cs.max + 1), expected $stripExpected (+/- 2) at zoom $stripZoom : $csRaw"
    }
    $vpH = [Math]::Round($cs.client_h * 96.0 / $cs.dpi)
    if ([Math]::Abs($cs.page - $vpH) -gt 1) {
        throw "strip scroll page: $($cs.page), expected the viewport height $vpH (+/- 1) DIPs: $csRaw"
    }
    Write-Host "[OK] continuous strip: scroll range $($cs.max + 1) DIPs (expected $stripExpected), page $($cs.page)"

    # Row 3: Zoom In re-lays out the strip at the next preset.
    $maxBefore = $cs.max
    & $uxProbe send-cmd 40010 | Out-Null   # IDM_ZOOM_IN
    $csRaw = Wait-UxProbeState -Probe canvas-scroll -Until { param($s) $s.present -and $s.max -ne $maxBefore }
    $cs = $null
    try { $cs = $csRaw | ConvertFrom-Json -ErrorAction Stop } catch { }
    if ($null -eq $cs) { throw "canvas-scroll probe failed after Zoom In: $csRaw" }
    if ($cs.max -eq $maxBefore) { throw "the strip's scroll range did not change after Zoom In: $csRaw" }
    $stripZoom2 = $null
    foreach ($preset in $stripZoomPresets) {
        if ($preset -gt $stripZoom + 1e-4) { $stripZoom2 = $preset; break }
    }
    if ($null -eq $stripZoom2) { throw "no zoom preset above $stripZoom" }
    $stripExpected2 = Get-StripExpectedHeight -Zoom $stripZoom2
    if ([Math]::Abs(($cs.max + 1) - $stripExpected2) -gt 2) {
        throw "strip scroll range after Zoom In: max + 1 = $($cs.max + 1), expected $stripExpected2 (+/- 2) at zoom $stripZoom2 : $csRaw"
    }
    Write-Host "[OK] continuous strip: Zoom In re-laid out to $($cs.max + 1) DIPs (zoom $stripZoom2)"

    # Row 4: Invert Colors, twice, keeps the strip. A liveness row: no probe reads
    # the polarity, and two toggles end where they started, so there is no state to
    # poll for. Give the posted commands time to run, then check that the process
    # is alive and the strip's scroll bar is still there.
    & $uxProbe send-cmd 40061 2 | Out-Null   # IDM_VIEW_INVERT x2
    Start-Sleep -Milliseconds 500
    $procStrip.Refresh()
    if ($procStrip.HasExited) {
        throw "litepdf.exe exited after Invert Colors in the strip (exit code $($procStrip.ExitCode))"
    }
    $cs = Read-UxProbeJson -Probe canvas-scroll
    if ($null -eq $cs -or -not $cs.vscroll) {
        throw "the canvas lost its scroll bar after Invert Colors twice"
    }
    Write-Host "[OK] continuous strip: Invert Colors twice keeps the strip"

    # Row 5: closing the last tab removes the scroll bar. Precondition first: no
    # scroll bar is also the paged state, so the bar must be there before the close.
    $cs = Read-UxProbeJson -Probe canvas-scroll
    if ($null -eq $cs -or -not ($cs.present -and $cs.vscroll)) {
        throw "precondition: the strip's scroll bar is not shown before the tab close"
    }
    & $uxProbe send-cmd 40030 | Out-Null   # IDM_TAB_CLOSE
    $csRaw = Wait-UxProbeState -Probe canvas-scroll -Until { param($s) $s.present -and -not $s.vscroll }
    $cs = $null
    try { $cs = $csRaw | ConvertFrom-Json -ErrorAction Stop } catch { }
    if ($null -eq $cs -or -not $cs.present) { throw "canvas-scroll probe failed after closing the last tab: $csRaw" }
    if ($cs.vscroll) { throw "the canvas still has a scroll bar after closing the last tab: $csRaw" }
    $pbRaw = Wait-UxProbeState -Probe page-box -Until { param($s) $s.present -and $s.text -eq "" }
    $pb = $null
    try { $pb = $pbRaw | ConvertFrom-Json -ErrorAction Stop } catch { }
    if ($null -eq $pb -or -not $pb.present) { throw "page-box probe failed after closing the last tab: $pbRaw" }
    if ($pb.text -ne "") { throw "the page box still reads '$($pb.text)' after closing the last tab" }
    $procStrip.Refresh()
    if ($procStrip.HasExited) {
        throw "litepdf.exe exited after closing the last strip tab (exit code $($procStrip.ExitCode))"
    }
    Write-Host "[OK] continuous strip: closing the last tab removes the scroll bar"

    Stop-LitePDF -Process $procStrip -Step "continuous strip"
    Start-Sleep -Milliseconds 500  # allow stderr buffers to flush

    # Row 1, continued: the strip marks the cold-start timer's bitmap points too.
    if (-not (Test-Path $stripErr)) { throw "no stderr log produced at $stripErr" }
    $stripLines = Get-Content $stripErr
    $stripTiming = $stripLines | Where-Object { $_ -match "LitePDF cold-start:" } | Select-Object -First 1
    if (-not $stripTiming) {
        Write-Host "stderr contents:"
        $stripLines | ForEach-Object { Write-Host "  $_" }
        throw "no 'LitePDF cold-start:' line found in the continuous strip run's stderr"
    }
    Write-Host "[OK] continuous strip: timing line: $stripTiming"

    # Rows 6-9: input in the strip (spec 6.1, 6.2, 4.6). Each fixture gets a fresh
    # launch; the wheel and the keys are posted to the canvas by ux-probe.

    # Row 6: wheel to the bottom (spec 9.3). 272 notches per message is about
    # 13,000 DIPs at the default 3 lines, so 200 messages cover large.pdf's strip
    # at any window width; a smaller lines setting sends proportionally more. At the
    # bottom the scroll position is the largest one Win32 allows and the derived
    # current page is the last (spec 4.6 rule 1).
    $procStrip = Start-StripLaunch -Fixture $largeFixture -Step "strip input"
    $bottomMessages = [int][Math]::Ceiling(200 * 48.0 / $stripNotchDip)
    & $uxProbe canvas-wheel -32640 $bottomMessages | Out-Null
    $st = Wait-StripState -TimeoutSeconds 10 -Until {
        param($cs, $pb)
        $cs.pos -eq ($cs.max - $cs.page + 1) -and $pb.text -eq "500"
    }
    if ($null -eq $st.cs -or $st.cs.pos -ne ($st.cs.max - $st.cs.page + 1)) {
        throw "wheel to the bottom: the scroll position is not max - page + 1: $(Format-StripState $st)"
    }
    if ($st.pb.text -ne "500") {
        throw "wheel to the bottom: the page box reads '$($st.pb.text)', expected 500: $(Format-StripState $st)"
    }
    Write-Host "[OK] continuous strip: wheel to the bottom: pos $($st.cs.pos) = max - page + 1, page box 500"

    # Row 7: Home, then PgDn. Precondition first: in paged mode pos and page are both
    # 0, so the checks below would pass there too.
    $cs = Read-UxProbeJson -Probe canvas-scroll
    if ($null -eq $cs -or -not ($cs.present -and $cs.vscroll -and $cs.page -gt 0)) {
        throw "precondition: the strip is not shown before Home: $($cs | ConvertTo-Json -Compress)"
    }
    & $uxProbe canvas-key 0x24 | Out-Null   # VK_HOME
    $st = Wait-StripState -Until { param($cs, $pb) $cs.pos -eq 0 -and $pb.text -eq "1" }
    if ($null -eq $st.cs -or $st.cs.pos -ne 0 -or $st.pb.text -ne "1") {
        throw "Home in the strip: expected pos 0 and page box 1: $(Format-StripState $st)"
    }
    & $uxProbe canvas-key 0x22 | Out-Null   # VK_NEXT (PgDn)
    $st = Wait-StripState -Until {
        param($cs, $pb)
        [Math]::Abs($cs.pos - [Math]::Round(0.9 * $cs.page)) -le 1
    }
    if ($null -eq $st.cs -or [Math]::Abs($st.cs.pos - [Math]::Round(0.9 * $st.cs.page)) -gt 1) {
        throw "PgDn in the strip: expected pos = 0.9 x page (+/- 1): $(Format-StripState $st)"
    }
    Write-Host "[OK] continuous strip: Home -> pos 0, page box 1; PgDn -> pos $($st.cs.pos) (page $($st.cs.page))"
    Stop-LitePDF -Process $procStrip -Step "strip input"
    Start-Sleep -Milliseconds 500

    # Row 8: a fling through 1000 pages, 400 wheel messages back to back from one
    # process at the default 3 lines; like Row 6, a smaller lines setting sends
    # proportionally more. This checks the model, not the pixels: no pixel-free
    # signal says a page's bitmap was accepted, so the final pages' paint is a
    # manual check.
    $strip1000 = Join-Path $repoRoot "tests/fixtures/strip-1000.pdf"
    if (-not (Test-Path $strip1000)) { throw "fixture not found at $strip1000" }
    $procStrip = Start-StripLaunch -Fixture $strip1000 -Step "strip fling"
    $flingMessages = [int][Math]::Ceiling(400 * 48.0 / $stripNotchDip)
    & $uxProbe canvas-wheel -32640 $flingMessages | Out-Null
    $st = Wait-StripState -TimeoutSeconds 10 -Until {
        param($cs, $pb)
        $pb.text -eq "1000" -and $cs.pos -eq ($cs.max - $cs.page + 1)
    }
    if ($null -eq $st.cs -or $st.pb.text -ne "1000" -or $st.cs.pos -ne ($st.cs.max - $st.cs.page + 1)) {
        throw "fling through strip-1000.pdf: expected page box 1000 and pos = max - page + 1 within 10 s: $(Format-StripState $st)"
    }
    $procStrip.Refresh()
    if ($procStrip.HasExited) {
        throw "litepdf.exe exited after the fling (exit code $($procStrip.ExitCode))"
    }
    Write-Host "[OK] continuous strip: fling through 1000 pages: page box 1000, pos $($st.cs.pos) = max - page + 1"
    Stop-LitePDF -Process $procStrip -Step "strip fling"
    Start-Sleep -Milliseconds 500

    # Row 9: a one-page document. sample.svg is 200 x 100 pt, so at FitWidth the
    # strip is vp_w / 2 + 16 DIPs tall, shorter than the canvas: the bar stays with a
    # disabled range (Windows stores page clamped to max + 1), and neither the wheel
    # nor End may move anything.
    $svgFixture = Join-Path $repoRoot "tests/fixtures/sample.svg"
    if (-not (Test-Path $svgFixture)) { throw "fixture not found at $svgFixture" }
    $procStrip = Start-StripLaunch -Fixture $svgFixture -Step "strip one page"
    $st = Wait-StripState -Until { param($cs, $pb) $pb.text -eq "1" }
    if ($null -eq $st.cs -or -not $st.cs.vscroll -or $st.cs.page -le $st.cs.max -or $st.pb.text -ne "1") {
        throw "one-page strip: expected vscroll, page > max and page box 1: $(Format-StripState $st)"
    }
    & $uxProbe canvas-wheel -120 3 | Out-Null
    & $uxProbe canvas-key 0x23 | Out-Null   # VK_END
    # Nothing is expected to change, so there is no state to poll for: give the
    # posted messages time to run, then read.
    Start-Sleep -Milliseconds 500
    $cs = Read-UxProbeJson -Probe canvas-scroll
    $pb = Read-UxProbeJson -Probe page-box
    $st = [pscustomobject]@{ cs = $cs; pb = $pb }
    if ($null -eq $cs -or $null -eq $pb -or $cs.pos -ne 0 -or $pb.text -ne "1") {
        throw "one-page strip after the wheel and End: expected pos 0 and page box 1: $(Format-StripState $st)"
    }
    $procStrip.Refresh()
    if ($procStrip.HasExited) {
        throw "litepdf.exe exited after input on a one-page strip (exit code $($procStrip.ExitCode))"
    }
    Write-Host "[OK] continuous strip: one-page document: page $($cs.page) > max $($cs.max), wheel and End keep pos 0 and page box 1"
    Stop-LitePDF -Process $procStrip -Step "strip one page"
    Start-Sleep -Milliseconds 500

    # Rows 10-12: navigation in strip coordinates (spec 6.3, 4.5). A fresh large.pdf
    # launch at FitWidth, so z = viewport width in DIPs / 612, every page is
    # h = 792 z DIPs tall and page i's scroll anchor is i x (h + 8) (spec 4.2).
    $procStrip = Start-StripLaunch -Fixture $largeFixture -Step "strip navigation"
    $stripZoomOf = { param($s) ($s.client_w * 96.0 / $s.dpi) / 612.0 }
    $stripAnchor = { param($i, $z) $i * (792.0 * $z + 8.0) }

    # Row 10: Ctrl+G 30 (spec 9.3). The page box is the Ctrl+G target, and both run
    # navigate_click. The strip scrolls to page index 29's anchor at once.
    & $uxProbe page-box-go 30 | Out-Null
    $st = Wait-StripState -Until {
        param($cs, $pb)
        $pb.text -eq "30" -and
            [Math]::Abs($cs.pos - [Math]::Round((& $stripAnchor 29 (& $stripZoomOf $cs)))) -le 1
    }
    if ($null -eq $st.cs -or $null -eq $st.pb) { throw "Ctrl+G 30 in the strip: probe failed: $(Format-StripState $st)" }
    $navAnchor = [Math]::Round((& $stripAnchor 29 (& $stripZoomOf $st.cs)))
    if ($st.pb.text -ne "30" -or [Math]::Abs($st.cs.pos - $navAnchor) -gt 1) {
        throw "Ctrl+G 30 in the strip: expected page box 30 and pos $navAnchor (+/- 1): $(Format-StripState $st)"
    }
    Write-Host "[OK] continuous strip: Ctrl+G 30 -> page box 30, pos $($st.cs.pos) = anchor(29) $navAnchor"

    # Row 11: Ctrl+G to the current page while mid-page (spec 9.3, 3.4). Two wheel
    # notches move 2 x $stripNotchDip DIPs (96 at the default 3 lines) and leave
    # page 30 current; going to page 30 again must scroll back to its anchor, which a
    # paged-style "same page, nothing to do" early return would not.
    $posBefore = $st.cs.pos
    & $uxProbe canvas-wheel -120 2 | Out-Null
    $twoNotchPos = $posBefore + 2 * $stripNotchDip
    $st = Wait-StripState -Until { param($cs, $pb) [Math]::Abs($cs.pos - $twoNotchPos) -le 1 }
    if ($null -eq $st.cs -or [Math]::Abs($st.cs.pos - $twoNotchPos) -gt 1 -or $st.pb.text -ne "30") {
        throw "two wheel notches from anchor(29): expected pos $twoNotchPos (+/- 1) and page box 30: $(Format-StripState $st)"
    }
    & $uxProbe page-box-go 30 | Out-Null
    $st = Wait-StripState -Until { param($cs, $pb) [Math]::Abs($cs.pos - $navAnchor) -le 1 -and $pb.text -eq "30" }
    if ($null -eq $st.cs -or [Math]::Abs($st.cs.pos - $navAnchor) -gt 1 -or $st.pb.text -ne "30") {
        throw "Ctrl+G to the current page mid-page: expected pos $navAnchor (+/- 1) and page box 30: $(Format-StripState $st)"
    }
    Write-Host "[OK] continuous strip: Ctrl+G 30 again from mid-page -> pos $($st.cs.pos) = anchor(29)"

    # Row 12: a resize keeps the viewport-top point (spec 4.5). Two more notches put
    # the viewport top mid-page, d0 DIPs below page 29's top, which no anchor is: a
    # "re-navigate to the current page" implementation would also pass from an
    # anchor. After a narrower window refits FitWidth to z1, the same point of
    # page 29 is at the top: pos = anchor1(29) + 8 + d0 x z1 / z0.
    $posBefore = $st.cs.pos
    & $uxProbe canvas-wheel -120 2 | Out-Null
    $twoNotchPos = $posBefore + 2 * $stripNotchDip
    $st = Wait-StripState -Until { param($cs, $pb) [Math]::Abs($cs.pos - $twoNotchPos) -le 1 }
    if ($null -eq $st.cs -or [Math]::Abs($st.cs.pos - $twoNotchPos) -gt 1 -or $st.pb.text -ne "30") {
        throw "two more wheel notches: expected pos $twoNotchPos (+/- 1) and page box 30: $(Format-StripState $st)"
    }
    $pos0 = [double]$st.cs.pos
    $z0 = & $stripZoomOf $st.cs
    $cw0 = $st.cs.client_w
    $d0 = $pos0 - ((& $stripAnchor 29 $z0) + 8.0)
    & $uxProbe resize -100 0 | Out-Null
    $resizeExpected = { param($s) $z1 = & $stripZoomOf $s; (& $stripAnchor 29 $z1) + 8.0 + $d0 * $z1 / $z0 }
    $st = Wait-StripState -Until {
        param($cs, $pb)
        $cs.client_w -ne $cw0 -and $pb.text -eq "30" -and
            [Math]::Abs($cs.pos - [Math]::Round((& $resizeExpected $cs))) -le 2
    }
    if ($null -eq $st.cs -or $st.cs.client_w -eq $cw0) {
        throw "resize -100 0: the canvas width did not change: $(Format-StripState $st)"
    }
    $posExpected = [Math]::Round((& $resizeExpected $st.cs))
    if ($st.pb.text -ne "30" -or [Math]::Abs($st.cs.pos - $posExpected) -gt 2) {
        throw "resize keeps the viewport-top point: expected page box 30 and pos $posExpected (+/- 2) (pos0 $pos0, z0 $z0, d0 $d0): $(Format-StripState $st)"
    }
    Write-Host "[OK] continuous strip: resize keeps the viewport-top point: pos $pos0 -> $($st.cs.pos) (expected $posExpected), page box 30"
    Stop-LitePDF -Process $procStrip -Step "strip navigation"
    Start-Sleep -Milliseconds 500

    # Rows 13-17: layout transitions and per-tab strip state (spec 7.3, 7.4, 9.3).
    # One fresh large.pdf launch; the rows run in order, each starting from the
    # state the previous one left. IDM_VIEW_DUAL_PAGE = 40062, IDM_ZOOM_IN = 40010,
    # IDM_TAB_GOTO_1 = 40033, IDM_TAB_GOTO_2 = 40034.
    $procStrip = Start-StripLaunch -Fixture $largeFixture -Step "strip transitions"

    # Row 13: two-page on, then off, from mid-page (spec 9.3, deviation 6). At a
    # Custom zoom two presets above FitWidth the spread overflows the viewport, so
    # the in-page offset has somewhere to go; page 30 (index 29) is a spread's left
    # page. Three wheel notches (3 x $stripNotchDip DIPs, 144 at the default 3
    # lines) put the viewport top mid-page.
    $cs = Read-UxProbeJson -Probe canvas-scroll
    if ($null -eq $cs) { throw "precondition: canvas-scroll probe failed before Zoom In" }
    $zFit = ($cs.client_w * 96.0 / $cs.dpi) / 612.0
    $zCustom = $null
    $above = 0
    foreach ($preset in $stripZoomPresets) {
        if ($preset -gt $zFit + 1e-4) {
            $above++
            if ($above -eq 2) { $zCustom = $preset; break }
        }
    }
    if ($null -eq $zCustom) { throw "no second zoom preset above $zFit" }
    & $uxProbe send-cmd 40010 2 | Out-Null
    $zoomedMax = Get-StripExpectedHeight -Zoom $zCustom
    $st = Wait-StripState -Until { param($cs, $pb) [Math]::Abs(($cs.max + 1) - $zoomedMax) -le 2 }
    if ($null -eq $st.cs -or [Math]::Abs(($st.cs.max + 1) - $zoomedMax) -gt 2) {
        throw "Zoom In twice: expected max + 1 = $zoomedMax (+/- 2) at zoom $zCustom : $(Format-StripState $st)"
    }
    $customAnchor = [Math]::Round(29 * (792.0 * $zCustom + 8.0))
    & $uxProbe page-box-go 30 | Out-Null
    $st = Wait-StripState -Until { param($cs, $pb) $pb.text -eq "30" -and [Math]::Abs($cs.pos - $customAnchor) -le 1 }
    if ($null -eq $st.cs -or $st.pb.text -ne "30" -or [Math]::Abs($st.cs.pos - $customAnchor) -gt 1) {
        throw "Ctrl+G 30 at zoom $zCustom : expected page box 30 and pos $customAnchor (+/- 1): $(Format-StripState $st)"
    }
    & $uxProbe canvas-wheel -120 3 | Out-Null
    $threeNotchPos = $customAnchor + 3 * $stripNotchDip
    $st = Wait-StripState -Until { param($cs, $pb) [Math]::Abs($cs.pos - $threeNotchPos) -le 1 }
    if ($null -eq $st.cs -or [Math]::Abs($st.cs.pos - $threeNotchPos) -gt 1 -or $st.pb.text -ne "30") {
        throw "three wheel notches from anchor(29): expected pos $threeNotchPos (+/- 1) and page box 30: $(Format-StripState $st)"
    }
    $midPos = $st.cs.pos
    & $uxProbe send-cmd 40062 | Out-Null
    $st = Wait-StripState -Until { param($cs, $pb) -not $cs.vscroll }
    if ($null -eq $st.cs -or $st.cs.vscroll) { throw "two-page on: the scroll bar stayed: $(Format-StripState $st)" }
    & $uxProbe send-cmd 40062 | Out-Null
    $st = Wait-StripState -Until { param($cs, $pb) $cs.vscroll -and $pb.text -eq "30" -and [Math]::Abs($cs.pos - $midPos) -le 1 }
    if ($null -eq $st.cs -or -not $st.cs.vscroll -or $st.pb.text -ne "30" -or [Math]::Abs($st.cs.pos - $midPos) -gt 1) {
        throw "two-page on, then off, from mid-page: expected the strip, page box 30 and pos $midPos (+/- 1): $(Format-StripState $st)"
    }
    Write-Host "[OK] continuous strip: two-page on, then off, from mid-page: page box 30, pos $midPos -> $($st.cs.pos)"

    # Row 14: rapid toggling (Review Focus 1). From the spread, an odd burst of
    # toggles posted back to back from one process must end in the strip -- the
    # opposite of where it starts, so a burst that was never processed cannot pass.
    # A wheel notch posted after the burst is a FIFO marker: posted messages to one
    # thread are dequeued in order, so once the strip has moved by one notch
    # ($stripNotchDip DIPs, 48 at the default 3 lines) from the carried position,
    # the whole burst has run. Then nothing late -- a strip completion, a paged
    # completion, a reschedule -- may flip the state or the range. One more toggle
    # leaves the strip, and nothing may bring the bar back.
    & $uxProbe send-cmd 40062 | Out-Null
    $st = Wait-StripState -Until { param($cs, $pb) -not $cs.vscroll }
    if ($null -eq $st.cs -or $st.cs.vscroll) { throw "precondition: two-page on did not leave the strip: $(Format-StripState $st)" }
    $toggleBurst = 11
    & $uxProbe send-cmd 40062 $toggleBurst | Out-Null
    & $uxProbe canvas-wheel -120 1 | Out-Null
    $markerPos = $midPos + $stripNotchDip
    $st = Wait-StripState -Until { param($cs, $pb) $cs.vscroll -and [Math]::Abs($cs.pos - $markerPos) -le 1 }
    if ($null -eq $st.cs -or -not $st.cs.vscroll -or [Math]::Abs($st.cs.pos - $markerPos) -gt 1 -or $st.pb.text -ne "30") {
        throw "$toggleBurst two-page toggles from the spread, then a wheel notch: expected the strip, page box 30 and pos $markerPos (+/- 1) within 5 s: $(Format-StripState $st)"
    }
    $toggleMax = $st.cs.max
    for ($i = 0; $i -lt 5; $i++) {
        Start-Sleep -Milliseconds 100
        $cs = Read-UxProbeJson -Probe canvas-scroll
        if ($null -eq $cs -or -not $cs.vscroll -or $cs.max -ne $toggleMax) {
            throw "after $toggleBurst two-page toggles the strip changed (reading $($i + 1) of 5, max was $toggleMax): $($cs | ConvertTo-Json -Compress)"
        }
    }
    $procStrip.Refresh()
    if ($procStrip.HasExited) { throw "litepdf.exe exited after $toggleBurst two-page toggles (exit code $($procStrip.ExitCode))" }
    $toggleDumps = @(Get-NewCrashDumps)
    if ($toggleDumps.Count -gt 0) { throw "$toggleBurst two-page toggles left $($toggleDumps.Count) new crash dump(s)" }
    & $uxProbe send-cmd 40062 | Out-Null
    $st = Wait-StripState -Until { param($cs, $pb) -not $cs.vscroll }
    if ($null -eq $st.cs -or $st.cs.vscroll) { throw "one more toggle did not leave the strip: $(Format-StripState $st)" }
    for ($i = 0; $i -lt 5; $i++) {
        Start-Sleep -Milliseconds 100
        $cs = Read-UxProbeJson -Probe canvas-scroll
        if ($null -eq $cs -or $cs.vscroll) {
            throw "the scroll bar came back after leaving the strip (reading $($i + 1) of 5): $($cs | ConvertTo-Json -Compress)"
        }
    }
    Write-Host "[OK] continuous strip: $toggleBurst rapid two-page toggles from the spread end in the strip (marker pos $markerPos, max $toggleMax, stable); one more leaves it for good"

    # Row 15: a tab with two-page on has no scroll bar (spec 9.3). Tab 1 is in the
    # spread now: back to the strip first. Then bookmarks.pdf is forwarded to the
    # running instance as a second tab, which must enter the strip -- without that
    # the row would pass in paged mode -- and two-page on removes the bar.
    & $uxProbe send-cmd 40062 | Out-Null
    $st = Wait-StripState -Until { param($cs, $pb) $cs.vscroll }
    if ($null -eq $st.cs -or -not $st.cs.vscroll) { throw "precondition: tab 1 did not return to the strip: $(Format-StripState $st)" }
    $bookmarksStrip = Join-Path $repoRoot "tests/fixtures/bookmarks.pdf"
    # The forwarder must exit 0, as the earlier forwarding steps check. Start-Process
    # -Wait has no timeout, so the wait is bounded here instead, and a forwarder
    # still running at the deadline is killed: no stray instance may outlive the
    # run (the .Handle read makes ExitCode readable under -NoNewWindow).
    $fwd = Start-Process -FilePath $exe -ArgumentList @($bookmarksStrip) -PassThru -NoNewWindow
    $null = $fwd.Handle
    if (-not $fwd.WaitForExit(10000)) {
        Stop-Process -Id $fwd.Id -Force -ErrorAction SilentlyContinue
        [void]$fwd.WaitForExit(5000)
        throw "the forwarder for bookmarks.pdf did not exit within 10 s (killed)"
    }
    if ($fwd.ExitCode -ne 0) { throw "the forwarder for bookmarks.pdf exited with code $($fwd.ExitCode), expected 0" }
    $deadline = (Get-Date).AddSeconds(5)
    $tabs = $null
    $cs = $null
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 100
        $tabs = Read-UxProbeJson -Probe tab-enum
        $cs = Read-UxProbeJson -Probe canvas-scroll
        if ($null -ne $tabs -and $tabs.count -eq 2 -and $tabs.active -eq 1 -and $null -ne $cs -and $cs.vscroll) { break }
    }
    if ($null -eq $tabs -or $tabs.count -ne 2 -or $tabs.active -ne 1 -or $null -eq $cs -or -not $cs.vscroll) {
        throw "forwarded bookmarks.pdf: expected 2 tabs, the second active, and the strip within 5 s: tab-enum $($tabs | ConvertTo-Json -Compress), canvas-scroll $($cs | ConvertTo-Json -Compress)"
    }
    & $uxProbe send-cmd 40062 | Out-Null
    $st = Wait-StripState -Until { param($cs, $pb) -not $cs.vscroll }
    if ($null -eq $st.cs -or $st.cs.vscroll) { throw "two-page on in the second tab: the scroll bar stayed: $(Format-StripState $st)" }
    Write-Host "[OK] continuous strip: the forwarded tab is active and entered the strip; two-page on removes the scroll bar"

    # Row 16: switching back to a strip tab restores the scroll bar (spec 9.3), and
    # tab 1's own position: row 14's marker position on page 30, which every carry
    # since kept at the unchanged Custom zoom.
    & $uxProbe send-cmd 40033 | Out-Null
    $st = Wait-StripState -Until { param($cs, $pb) $cs.vscroll -and $pb.text -eq "30" -and [Math]::Abs($cs.pos - $markerPos) -le 1 }
    if ($null -eq $st.cs -or -not $st.cs.vscroll -or $st.pb.text -ne "30" -or [Math]::Abs($st.cs.pos - $markerPos) -gt 1) {
        throw "switch back to the strip tab: expected the scroll bar, page box 30 and pos $markerPos (+/- 1): $(Format-StripState $st)"
    }
    Write-Host "[OK] continuous strip: switching back to the strip tab: scroll bar, page box 30, pos $($st.cs.pos)"

    # Row 17: strip tab A at page 20, strip tab B keeps its own page (spec 9.3). Tab
    # 2 is still in the spread: back to the strip, go to page 3 and record the
    # position. Tab 1 goes to page 20; back on tab 2 without scrolling, both the
    # page box and the position are tab 2's own. The page box alone follows
    # DocumentView::current_page, which survives a lost scroll position.
    & $uxProbe send-cmd 40034 | Out-Null
    $st = Wait-StripState -Until { param($cs, $pb) -not $cs.vscroll }
    if ($null -eq $st.cs -or $st.cs.vscroll) { throw "precondition: tab 2 is not in the spread: $(Format-StripState $st)" }
    & $uxProbe send-cmd 40062 | Out-Null
    $st = Wait-StripState -Until { param($cs, $pb) $cs.vscroll }
    if ($null -eq $st.cs -or -not $st.cs.vscroll) { throw "precondition: tab 2 did not enter the strip: $(Format-StripState $st)" }
    & $uxProbe page-box-go 3 | Out-Null
    $st = Wait-StripState -Until { param($cs, $pb) $pb.text -eq "3" }
    if ($null -eq $st.cs -or $st.pb.text -ne "3") { throw "Ctrl+G 3 in tab 2: expected page box 3: $(Format-StripState $st)" }
    $tab2Pos = $st.cs.pos
    & $uxProbe send-cmd 40033 | Out-Null
    # Tab 2 showed page 3 in the strip too: the switch has landed once the page box
    # shows something else.
    $st = Wait-StripState -Until { param($cs, $pb) $cs.vscroll -and $pb.text -ne "3" }
    if ($null -eq $st.cs -or -not $st.cs.vscroll -or $st.pb.text -eq "3") {
        throw "precondition: tab 1 is not shown in the strip: $(Format-StripState $st)"
    }
    & $uxProbe page-box-go 20 | Out-Null
    $st = Wait-StripState -Until { param($cs, $pb) $pb.text -eq "20" }
    if ($null -eq $st.cs -or $st.pb.text -ne "20") { throw "Ctrl+G 20 in tab 1: expected page box 20: $(Format-StripState $st)" }
    & $uxProbe send-cmd 40034 | Out-Null
    $st = Wait-StripState -Until { param($cs, $pb) $cs.vscroll -and $pb.text -eq "3" -and [Math]::Abs($cs.pos - $tab2Pos) -le 1 }
    if ($null -eq $st.cs -or -not $st.cs.vscroll -or $st.pb.text -ne "3" -or [Math]::Abs($st.cs.pos - $tab2Pos) -gt 1) {
        throw "strip tab B after tab A went to page 20: expected page box 3 (not 20) and pos $tab2Pos (+/- 1): $(Format-StripState $st)"
    }
    Write-Host "[OK] continuous strip: tab A at page 20, tab B keeps page 3 and pos $($st.cs.pos) (recorded $tab2Pos)"
    Stop-LitePDF -Process $procStrip -Step "strip transitions"
    Start-Sleep -Milliseconds 500

    # Row 18: the carry is scaled by the zoom ratio (spec 7.3, deviation 1). At
    # FitWidth the strip fits the full width (z0) and the spread a half-width slot
    # (z1), so a toggle changes the zoom. From mid-page 30 (three notches below
    # anchor(29)), the offset below page 29's top scales by z1 / z0 into the spread,
    # where the pan clamps it to the page's overflow (h1 - vp_h), and scales back by
    # z2 / z1 into the strip. Everything is derived from the probe's client sizes:
    # the spread's FitWidth is ((client_w DIPs - 8) / 2) / 612 (PdfCanvas::
    # apply_viewport's 8 DIP gutter). Unscaled carries land elsewhere.
    #
    # The round trip only tells a scaled carry from an unscaled one when the
    # spread's pan clamps the offset: unclamped, the two scalings cancel and an
    # unscaled build lands on the same pos. Whether the spread's page overflows at
    # all depends on the window's aspect (a 1024x768 window at 100 % fits it with
    # 5 DIPs to spare), so the row sets the window height first: the spread's
    # overflow becomes about 0.35 of the offset the carry brings into it (the
    # spread is a scroll bar wider than the strip, which only adds a few DIPs), and
    # the clamp is asserted once the spread is shown.
    $procStrip = Start-StripLaunch -Fixture $largeFixture -Step "strip carry at FitWidth"
    $dipOf = { param($px, $s) $px * 96.0 / $s.dpi }
    $cs = Read-UxProbeJson -Probe canvas-scroll
    if ($null -eq $cs) { throw "precondition: canvas-scroll probe failed at FitWidth" }
    $estZ0 = (& $dipOf $cs.client_w $cs) / 612.0
    $estZ1 = (((& $dipOf $cs.client_w $cs) - 8.0) / 2.0) / 612.0
    $estSpreadOffset = (3 * $stripNotchDip - 8.0) * $estZ1 / $estZ0
    $targetH = 792.0 * $estZ1 - 0.35 * $estSpreadOffset
    $deltaPx = [int][Math]::Round(($targetH - (& $dipOf $cs.client_h $cs)) * $cs.dpi / 96.0)
    if ([Math]::Abs($deltaPx) -ge 2) {
        # A unique name: Wait-StripState's own $cs would shadow $cs in the block.
        $preResizeClientH = $cs.client_h
        & $uxProbe resize 0 $deltaPx | Out-Null
        $st = Wait-StripState -Until { param($c, $pb) $c.vscroll -and $c.client_h -ne $preResizeClientH }
        if ($null -eq $st.cs -or -not $st.cs.vscroll -or $st.cs.client_h -eq $preResizeClientH) {
            throw "precondition: resizing the window by $deltaPx px did not change the canvas height: $(Format-StripState $st)"
        }
        $cs = $st.cs
    }
    $z0 = (& $dipOf $cs.client_w $cs) / 612.0
    $fitAnchor0 = 29 * (792.0 * $z0 + 8.0)
    & $uxProbe page-box-go 30 | Out-Null
    $st = Wait-StripState -Until { param($cs, $pb) $pb.text -eq "30" -and [Math]::Abs($cs.pos - [Math]::Round($fitAnchor0)) -le 1 }
    if ($null -eq $st.cs -or $st.pb.text -ne "30" -or [Math]::Abs($st.cs.pos - [Math]::Round($fitAnchor0)) -gt 1) {
        throw "Ctrl+G 30 at FitWidth: expected page box 30 and pos $([Math]::Round($fitAnchor0)) (+/- 1): $(Format-StripState $st)"
    }
    & $uxProbe canvas-wheel -120 3 | Out-Null
    $fitNotchPos = [Math]::Round($fitAnchor0) + 3 * $stripNotchDip
    $st = Wait-StripState -Until { param($cs, $pb) [Math]::Abs($cs.pos - $fitNotchPos) -le 1 }
    if ($null -eq $st.cs -or [Math]::Abs($st.cs.pos - $fitNotchPos) -gt 1 -or $st.pb.text -ne "30") {
        throw "three wheel notches at FitWidth: expected pos $fitNotchPos (+/- 1) and page box 30: $(Format-StripState $st)"
    }
    $fitPos0 = [double]$st.cs.pos
    $offset0 = $fitPos0 - ($fitAnchor0 + 8.0)
    & $uxProbe send-cmd 40062 | Out-Null
    $st = Wait-StripState -Until { param($cs, $pb) -not $cs.vscroll }
    if ($null -eq $st.cs -or $st.cs.vscroll) { throw "two-page on at FitWidth: the scroll bar stayed: $(Format-StripState $st)" }
    $z1 = (((& $dipOf $st.cs.client_w $st.cs) - 8.0) / 2.0) / 612.0
    $vpH = & $dipOf $st.cs.client_h $st.cs
    $h1 = 792.0 * $z1
    if ($h1 -le $vpH) { throw "precondition: the spread's page ($h1 DIPs at zoom $z1) fits the $vpH DIP viewport, so the carry has no offset to scale" }
    if ($offset0 * $z1 / $z0 -le ($h1 - $vpH) + 2.0) {
        throw "precondition: the carried offset ($($offset0 * $z1 / $z0) DIPs) does not clamp at the spread's overflow ($($h1 - $vpH) DIPs), so an unscaled carry would land on the same pos"
    }
    $offsetSpread = [Math]::Min($offset0 * $z1 / $z0, $h1 - $vpH)
    & $uxProbe send-cmd 40062 | Out-Null
    $st = Wait-StripState -Until { param($cs, $pb) $cs.vscroll }
    if ($null -eq $st.cs -or -not $st.cs.vscroll) { throw "two-page off at FitWidth: the strip did not return: $(Format-StripState $st)" }
    $fitExpected = { param($s)
        $z2 = (& $dipOf $s.client_w $s) / 612.0
        29 * (792.0 * $z2 + 8.0) + 8.0 + $offsetSpread * $z2 / $z1
    }
    $st = Wait-StripState -Until { param($cs, $pb) $cs.vscroll -and $pb.text -eq "30" -and [Math]::Abs($cs.pos - [Math]::Round((& $fitExpected $cs))) -le 1 }
    $fitPredicted = [Math]::Round((& $fitExpected $st.cs))
    if ($null -eq $st.cs -or $st.pb.text -ne "30" -or [Math]::Abs($st.cs.pos - $fitPredicted) -gt 1) {
        throw "two-page on, then off, at FitWidth: expected page box 30 and pos $fitPredicted (+/- 1) (pos0 $fitPos0, z0 $z0, z1 $z1, offset in the spread $offsetSpread): $(Format-StripState $st)"
    }
    Write-Host "[OK] continuous strip: FitWidth two-page round trip scales the carry: pos $fitPos0 -> $($st.cs.pos) (predicted $fitPredicted; zoom $z0 -> $z1 -> back)"
    Stop-LitePDF -Process $procStrip -Step "strip carry at FitWidth"
    Start-Sleep -Milliseconds 500

    # Rows 19-20: Select All and Copy in the strip (spec 7.1). IDM_EDIT_COPY = 40071,
    # IDM_EDIT_SELECT_ALL = 40072. Both go through the clipboard, which is the user's:
    # its text is saved first and put back in finally.
    $stripClipSaved = Get-Clipboard -Raw
    try {
        # Row 19: Select All selects the current page -- the one the page box shows --
        # and Copy copies that page's text only. strip-1000.pdf's page n holds
        # "Strip page n of 1000.", so the next page's text is told apart.
        $procStrip = Start-StripLaunch -Fixture $strip1000 -Step "strip select all"
        $clipMarker = "litepdf-smoke-marker-$([guid]::NewGuid().ToString('N'))"
        Set-Clipboard -Value $clipMarker
        & $uxProbe page-box-go 30 | Out-Null
        $st = Wait-StripState -Until { param($cs, $pb) $cs.vscroll -and $pb.text -eq "30" }
        if ($null -eq $st.cs -or -not $st.cs.vscroll -or $st.pb.text -ne "30") {
            throw "Ctrl+G 30 in strip-1000.pdf: expected the strip and page box 30: $(Format-StripState $st)"
        }
        & $uxProbe send-cmd 40072 | Out-Null
        & $uxProbe send-cmd 40071 | Out-Null
        $clip = $null
        $deadline = (Get-Date).AddSeconds(5)
        while ((Get-Date) -lt $deadline) {
            Start-Sleep -Milliseconds 100
            $clip = Get-Clipboard -Raw
            if ($null -ne $clip -and $clip.Contains("Strip page 30 of 1000.")) { break }
        }
        if ($null -eq $clip -or -not $clip.Contains("Strip page 30 of 1000.")) {
            throw "Select All + Copy on strip page 30: the clipboard does not hold 'Strip page 30 of 1000.' within 5 s: '$clip'"
        }
        if ($clip.Contains("Strip page 31")) {
            throw "Select All + Copy on strip page 30: the clipboard holds page 31's text too: '$clip'"
        }
        Write-Host "[OK] continuous strip: Select All + Copy on page 30 copies 'Strip page 30 of 1000.' and nothing of page 31"
        Stop-LitePDF -Process $procStrip -Step "strip select all"
        Start-Sleep -Milliseconds 500

        # Row 20: Select All on a page with no text (a guard, not a fail-first row).
        # strip-images.cbz's pages are images: Select All must build no selection,
        # so Copy stays grayed and leaves the clipboard alone. Nothing is expected to
        # change, so a FIFO barrier (Status Bar, 40064: the canvas height changes)
        # shows that both commands have run before the oracle is read.
        $stripImages = Join-Path $repoRoot "tests/fixtures/strip-images.cbz"
        if (-not (Test-Path $stripImages)) { throw "fixture not found at $stripImages" }
        $procStrip = Start-StripLaunch -Fixture $stripImages -Step "strip select all, no text"
        $clipMarker = "litepdf-smoke-marker-$([guid]::NewGuid().ToString('N'))"
        Set-Clipboard -Value $clipMarker
        $cs = Read-UxProbeJson -Probe canvas-scroll
        if ($null -eq $cs -or -not $cs.vscroll) { throw "precondition: strip-images.cbz is not in the strip: $($cs | ConvertTo-Json -Compress)" }
        $barrierH = $cs.client_h
        & $uxProbe send-cmd 40072 | Out-Null
        & $uxProbe send-cmd 40071 | Out-Null
        & $uxProbe send-cmd 40064 | Out-Null
        $st = Wait-StripState -Until { param($c, $pb) $c.client_h -ne $barrierH }
        if ($null -eq $st.cs -or $st.cs.client_h -eq $barrierH) {
            throw "FIFO barrier: the canvas height did not change after Status Bar: $(Format-StripState $st)"
        }
        $ms = (& $uxProbe menu-state 40071 2>&1 | Out-String)
        $msObj = $null
        try { $msObj = $ms | ConvertFrom-Json -ErrorAction Stop } catch { $msObj = $null }
        $clip = Get-Clipboard -Raw
        if ($null -eq $msObj -or -not $msObj.items.'40071'.found -or -not $msObj.items.'40071'.grayed) {
            throw "Select All on a page with no text: Copy is not grayed: $ms"
        }
        if ($clip -ne $clipMarker) {
            throw "Select All + Copy on a page with no text changed the clipboard: '$clip'"
        }
        & $uxProbe send-cmd 40064 | Out-Null
        $st = Wait-StripState -Until { param($c, $pb) $c.client_h -eq $barrierH }
        if ($null -eq $st.cs -or $st.cs.client_h -ne $barrierH) {
            throw "FIFO barrier: the status bar did not come back: $(Format-StripState $st)"
        }
        Write-Host "[OK] continuous strip: Select All + Copy on a page with no text: Copy grayed, clipboard unchanged"
        Stop-LitePDF -Process $procStrip -Step "strip select all, no text"
    } finally {
        if ($null -ne $stripClipSaved) { Set-Clipboard -Value $stripClipSaved } else { Set-Clipboard -Value $null }
    }
    Start-Sleep -Milliseconds 500

    # Rows 21-23: the View > Continuous Scroll menu item (spec 7.2, 7.3, 9.3).
    # IDM_VIEW_CONTINUOUS = 40066, IDM_VIEW_DUAL_PAGE = 40062, IDM_ZOOM_IN = 40010,
    # IDM_TAB_CLOSE = 40030. One large.pdf launch with ContinuousScroll = 1; each row
    # starts from the state the previous one left. The registry value is the oracle
    # for the preference write, read with Get-ItemProperty.
    Set-ItemProperty -Path $stripKey -Name ContinuousScroll -Value 1 -Type DWord
    $procStrip = Start-StripLaunch -Fixture $largeFixture -Step "continuous menu item"

    # Row 21: the transition through the menu item (spec 9.3). Zoom In twice puts the
    # zoom two presets above FitWidth, so the page overflows the viewport and the
    # in-page offset has somewhere to go (as in row 13); three wheel notches put the
    # viewport top mid-page 30. The item turns the preference off (the paged layout)
    # and on again (the strip), and the position comes back.
    $cs = Read-UxProbeJson -Probe canvas-scroll
    if ($null -eq $cs) { throw "precondition: canvas-scroll probe failed before Zoom In" }
    $zFit = ($cs.client_w * 96.0 / $cs.dpi) / 612.0
    $zMenu = $null
    $above = 0
    foreach ($preset in $stripZoomPresets) {
        if ($preset -gt $zFit + 1e-4) {
            $above++
            if ($above -eq 2) { $zMenu = $preset; break }
        }
    }
    if ($null -eq $zMenu) { throw "no second zoom preset above $zFit" }
    & $uxProbe send-cmd 40010 2 | Out-Null
    $menuMax = Get-StripExpectedHeight -Zoom $zMenu
    $st = Wait-StripState -Until { param($cs, $pb) [Math]::Abs(($cs.max + 1) - $menuMax) -le 2 }
    if ($null -eq $st.cs -or [Math]::Abs(($st.cs.max + 1) - $menuMax) -gt 2) {
        throw "Zoom In twice: expected max + 1 = $menuMax (+/- 2) at zoom $zMenu : $(Format-StripState $st)"
    }
    $menuAnchor = [Math]::Round(29 * (792.0 * $zMenu + 8.0))
    & $uxProbe page-box-go 30 | Out-Null
    $st = Wait-StripState -Until { param($cs, $pb) $pb.text -eq "30" -and [Math]::Abs($cs.pos - $menuAnchor) -le 1 }
    if ($null -eq $st.cs -or $st.pb.text -ne "30" -or [Math]::Abs($st.cs.pos - $menuAnchor) -gt 1) {
        throw "Ctrl+G 30 at zoom $zMenu : expected page box 30 and pos $menuAnchor (+/- 1): $(Format-StripState $st)"
    }
    & $uxProbe canvas-wheel -120 3 | Out-Null
    $menuNotchPos = $menuAnchor + 3 * $stripNotchDip
    $st = Wait-StripState -Until { param($cs, $pb) [Math]::Abs($cs.pos - $menuNotchPos) -le 1 }
    if ($null -eq $st.cs -or [Math]::Abs($st.cs.pos - $menuNotchPos) -gt 1 -or $st.pb.text -ne "30") {
        throw "three wheel notches from anchor(29): expected pos $menuNotchPos (+/- 1) and page box 30: $(Format-StripState $st)"
    }
    $menuPos = $st.cs.pos
    & $uxProbe send-cmd 40066 | Out-Null
    $st = Wait-StripState -Until { param($cs, $pb) -not $cs.vscroll }
    if ($null -eq $st.cs -or $st.cs.vscroll) { throw "Continuous Scroll off: the scroll bar stayed: $(Format-StripState $st)" }
    $pref = Wait-ContinuousPref -Expected 0
    if ($pref -ne 0) { throw "Continuous Scroll off: the registry value is '$pref', expected 0" }
    $ms = Read-MenuState -Ids @("40066")
    if ($null -eq $ms -or -not $ms.items.'40066'.found -or $ms.items.'40066'.checked) {
        throw "Continuous Scroll off: expected the item present and not checked: $($ms | ConvertTo-Json -Compress -Depth 5)"
    }
    & $uxProbe send-cmd 40066 | Out-Null
    $st = Wait-StripState -Until { param($cs, $pb) $cs.vscroll -and $pb.text -eq "30" -and [Math]::Abs($cs.pos - $menuPos) -le 1 }
    if ($null -eq $st.cs -or -not $st.cs.vscroll -or $st.pb.text -ne "30" -or [Math]::Abs($st.cs.pos - $menuPos) -gt 1) {
        throw "Continuous Scroll off, then on, from mid-page: expected the strip, page box 30 and pos $menuPos (+/- 1): $(Format-StripState $st)"
    }
    $pref = Wait-ContinuousPref -Expected 1
    if ($pref -ne 1) { throw "Continuous Scroll on: the registry value is '$pref', expected 1" }
    Write-Host "[OK] continuous strip: the menu item off, then on, from mid-page: registry 0 then 1, page box 30, pos $menuPos -> $($st.cs.pos)"

    # Row 22: from a two-page tab (Review Focus 2). Two-page on leaves the strip (the
    # spread wins over the preference); the item then turns the spread off and keeps
    # the preference on, so the strip returns, the preference stays 1, and the item
    # reads checked while Two-Page Spread reads unchecked.
    & $uxProbe send-cmd 40062 | Out-Null
    $st = Wait-StripState -Until { param($cs, $pb) -not $cs.vscroll }
    if ($null -eq $st.cs -or $st.cs.vscroll) { throw "precondition: two-page on did not leave the strip: $(Format-StripState $st)" }
    & $uxProbe send-cmd 40066 | Out-Null
    $st = Wait-StripState -Until { param($cs, $pb) $cs.vscroll -and $pb.text -eq "30" }
    if ($null -eq $st.cs -or -not $st.cs.vscroll -or $st.pb.text -ne "30") {
        throw "Continuous Scroll from a two-page tab: expected the strip and page box 30: $(Format-StripState $st)"
    }
    $pref = Get-ContinuousPref
    if ($pref -ne 1) { throw "Continuous Scroll from a two-page tab: the registry value is '$pref', expected 1 (the item must not turn the preference off)" }
    $ms = Read-MenuState -Ids @("40062", "40066")
    if ($null -eq $ms -or -not $ms.items.'40062'.found -or $ms.items.'40062'.checked -or
        -not $ms.items.'40066'.found -or -not $ms.items.'40066'.checked) {
        throw "Continuous Scroll from a two-page tab: expected Two-Page Spread unchecked and Continuous Scroll checked: $($ms | ConvertTo-Json -Compress -Depth 5)"
    }
    Write-Host "[OK] continuous strip: the item from a two-page tab: the strip, page box 30, preference still 1, Two-Page unchecked, item checked"

    # Row 23: no document greys the item and the handler refuses (the one enable
    # predicate). The format half of the predicate has no fixture and is covered by
    # the unit tests. A refused command changes nothing, so a FIFO barrier (Status
    # Bar, 40064: the canvas height changes) shows that it ran before the oracle is
    # read.
    & $uxProbe send-cmd 40030 | Out-Null   # IDM_TAB_CLOSE
    $pbRaw = Wait-UxProbeState -Probe page-box -Until { param($s) $s.present -and $s.text -eq "" }
    $pb = $null
    try { $pb = $pbRaw | ConvertFrom-Json -ErrorAction Stop } catch { }
    if ($null -eq $pb -or -not $pb.present -or $pb.text -ne "") { throw "closing the last tab: the page box did not empty: $pbRaw" }
    $ms = Read-MenuState -Ids @("40066")
    if ($null -eq $ms -or -not $ms.items.'40066'.found -or -not $ms.items.'40066'.grayed) {
        throw "no document: expected Continuous Scroll present and grayed: $($ms | ConvertTo-Json -Compress -Depth 5)"
    }
    $cs = Read-UxProbeJson -Probe canvas-scroll
    if ($null -eq $cs) { throw "precondition: canvas-scroll probe failed with no document" }
    $barrierH = $cs.client_h
    & $uxProbe send-cmd 40066 | Out-Null
    & $uxProbe send-cmd 40064 | Out-Null
    $st = Wait-StripState -Until { param($c, $pb) $c.client_h -ne $barrierH }
    if ($null -eq $st.cs -or $st.cs.client_h -eq $barrierH) {
        throw "FIFO barrier: the canvas height did not change after Status Bar: $(Format-StripState $st)"
    }
    $pref = Get-ContinuousPref
    if ($pref -ne 1) { throw "no document: the refused command changed the registry value to '$pref', expected 1" }
    & $uxProbe send-cmd 40064 | Out-Null
    $st = Wait-StripState -Until { param($c, $pb) $c.client_h -eq $barrierH }
    if ($null -eq $st.cs -or $st.cs.client_h -ne $barrierH) {
        throw "FIFO barrier: the status bar did not come back: $(Format-StripState $st)"
    }
    $procStrip.Refresh()
    if ($procStrip.HasExited) { throw "litepdf.exe exited after the refused Continuous Scroll command (exit code $($procStrip.ExitCode))" }
    Write-Host "[OK] continuous strip: with no document the item is grayed and the command changes nothing (registry 1)"
    Stop-LitePDF -Process $procStrip -Step "continuous menu item"
    Start-Sleep -Milliseconds 500

    # Row 24: a background paged tab keeps its page when the preference turns on
    # (Review Focus 3). A fresh launch with ContinuousScroll = 0: tab 1 is large.pdf
    # at page 30, and simple.pdf is forwarded as tab 2 (the active one). The item
    # turns the preference on: tab 2, the shown tab, enters the strip. Switching to
    # tab 1, which has been paged and in the background, builds its strip at its
    # current page 30: the scroll position is anchor(29) at FitWidth, not the top.
    # Turning the preference off again and switching to tab 2 (last shown in the
    # strip) lands in the paged layout on its page 1.
    Set-ItemProperty -Path $stripKey -Name ContinuousScroll -Value 0 -Type DWord
    $procStrip = Start-StripLaunch -Fixture $largeFixture -Step "continuous menu item, background tab" -Paged
    & $uxProbe page-box-go 30 | Out-Null
    $st = Wait-StripState -Until { param($cs, $pb) $pb.text -eq "30" }
    if ($null -eq $st.cs -or $st.pb.text -ne "30") { throw "Ctrl+G 30 in the paged tab: expected page box 30: $(Format-StripState $st)" }
    $bgSimple = Join-Path $repoRoot "tests/fixtures/simple.pdf"
    Send-ForwardedFile -Path $bgSimple
    $deadline = (Get-Date).AddSeconds(5)
    $tabs = $null
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 100
        $tabs = Read-UxProbeJson -Probe tab-enum
        if ($null -ne $tabs -and $tabs.count -eq 2 -and $tabs.active -eq 1) { break }
    }
    if ($null -eq $tabs -or $tabs.count -ne 2 -or $tabs.active -ne 1) {
        throw "forwarded simple.pdf: expected 2 tabs and the second active within 5 s: tab-enum $($tabs | ConvertTo-Json -Compress)"
    }
    & $uxProbe send-cmd 40066 | Out-Null
    $st = Wait-StripState -Until { param($cs, $pb) $cs.vscroll }
    if ($null -eq $st.cs -or -not $st.cs.vscroll) { throw "Continuous Scroll on: tab 2 did not enter the strip: $(Format-StripState $st)" }
    $pref = Wait-ContinuousPref -Expected 1
    if ($pref -ne 1) { throw "Continuous Scroll on: the registry value is '$pref', expected 1" }
    & $uxProbe send-cmd 40033 | Out-Null
    $bgExpected = { param($s)
        $z = ($s.client_w * 96.0 / $s.dpi) / 612.0
        29 * (792.0 * $z + 8.0)
    }
    $st = Wait-StripState -Until { param($cs, $pb) $cs.vscroll -and $pb.text -eq "30" -and [Math]::Abs($cs.pos - [Math]::Round((& $bgExpected $cs))) -le 1 }
    if ($null -eq $st.cs) { throw "switch to the background tab: canvas-scroll probe failed: $(Format-StripState $st)" }
    $bgPredicted = [Math]::Round((& $bgExpected $st.cs))
    if (-not $st.cs.vscroll -or $st.pb.text -ne "30" -or [Math]::Abs($st.cs.pos - $bgPredicted) -gt 1) {
        throw "the background paged tab entering the strip: expected the strip, page box 30 and pos $bgPredicted (+/- 1; anchor(29) at FitWidth): $(Format-StripState $st)"
    }
    Write-Host "[OK] continuous strip: the background paged tab entered the strip at its page: page box 30, pos $($st.cs.pos) (anchor(29) $bgPredicted)"
    & $uxProbe send-cmd 40066 | Out-Null
    $st = Wait-StripState -Until { param($cs, $pb) -not $cs.vscroll }
    if ($null -eq $st.cs -or $st.cs.vscroll) { throw "Continuous Scroll off: tab 1 stayed in the strip: $(Format-StripState $st)" }
    $pref = Wait-ContinuousPref -Expected 0
    if ($pref -ne 0) { throw "Continuous Scroll off: the registry value is '$pref', expected 0" }
    & $uxProbe send-cmd 40034 | Out-Null
    $st = Wait-StripState -Until { param($cs, $pb) -not $cs.vscroll -and $pb.text -eq "1" }
    if ($null -eq $st.cs -or $st.cs.vscroll -or $st.pb.text -ne "1") {
        throw "tab 2, last shown in the strip, after the preference turned off: expected paged and page box 1: $(Format-StripState $st)"
    }
    $procStrip.Refresh()
    if ($procStrip.HasExited) { throw "litepdf.exe exited during the background-tab row (exit code $($procStrip.ExitCode))" }
    Write-Host "[OK] continuous strip: preference off, then tab 2 (last shown in the strip): paged, page box 1, process alive"
    Stop-LitePDF -Process $procStrip -Step "continuous menu item, background tab"
} finally {
    if ($null -ne $procStrip -and -not $procStrip.HasExited) {
        Stop-Process -Id $procStrip.Id -Force -ErrorAction SilentlyContinue
        [void]$procStrip.WaitForExit(5000)
    }
    if ($stripHadValue) {
        $stripRestoreKind = "DWord"
        if ($null -ne $stripPriorKind) { $stripRestoreKind = $stripPriorKind.ToString() }
        Set-ItemProperty -Path $stripKey -Name ContinuousScroll -Value $stripPriorValue -Type $stripRestoreKind
    } elseif (Test-Path $stripKey) {
        Remove-ItemProperty -Path $stripKey -Name ContinuousScroll -ErrorAction SilentlyContinue
        if (-not $stripHadKey) {
            $stripKeyItem = Get-Item $stripKey
            if ($stripKeyItem.ValueCount -eq 0 -and $stripKeyItem.SubKeyCount -eq 0) {
                Remove-Item -Path $stripKey -Force
            }
        }
    }
    Remove-Item $stripErr -Force -ErrorAction SilentlyContinue
}
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
    $null = $proc.Handle
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
    Stop-LitePDF -Process $proc -Step (Split-Path -Leaf $fixture)
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
# would exit early. The modal is dismissed by Stop-LitePDF below.
$encryptedFixture = Join-Path $repoRoot "tests/fixtures/encrypted.pdf"
if (Test-Path $encryptedFixture) {
    Write-Host "----"
    Write-Host "Launching encrypted PDF (modal expected): $exe $encryptedFixture"
    $procEnc = Start-Process -FilePath $exe -ArgumentList @($encryptedFixture) -PassThru -NoNewWindow
    $null = $procEnc.Handle
    Start-Sleep -Seconds 1
    $procEnc.Refresh()
    if ($procEnc.HasExited) {
        throw "encrypted.pdf launch crashed (exit code $($procEnc.ExitCode))"
    }
    Write-Host "[OK] encrypted.pdf process alive after 1 s (modal blocking on user input)"
    Stop-LitePDF -Process $procEnc -Step "encrypted.pdf"
    Start-Sleep -Milliseconds 300
} else {
    Write-Host "[SKIP] encrypted.pdf not found at $encryptedFixture"
}

# #156: fail if any litepdf instance crashed during the run, including one that
# crashed after its step's checks had passed.
$crashDumpsChecked = $true
$newDumps = @(Get-NewCrashDumps)
if ($newDumps.Count -gt 0) {
    Write-NewCrashDumps -Names $newDumps
    throw "litepdf crashed during the smoke run: $($newDumps.Count) new crash dump(s), listed above"
}
Write-Host "[OK] no new crash dumps in $crashDir"

Write-Host "[OK] smoke test passed."
