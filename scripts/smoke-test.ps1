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
        [string]$Step
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
    $st = Wait-StripState -Until { param($cs, $pb) $cs.present -and $cs.vscroll -and $cs.page -gt 0 }
    if ($null -eq $st.cs -or -not ($st.cs.vscroll -and $st.cs.page -gt 0)) {
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
        throw "the $Step launch shows no laid-out strip 5 s after opening $Fixture : $(Format-StripState $st)"
    }
    return $p
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

    # Row 4: Invert Colors, twice, keeps the strip.
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
    # at any window width. At the bottom the scroll position is the largest one
    # Win32 allows and the derived current page is the last (spec 4.6 rule 1).
    $procStrip = Start-StripLaunch -Fixture $largeFixture -Step "strip input"
    & $uxProbe canvas-wheel -32640 200 | Out-Null
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
    # process. This checks the model, not the pixels: no pixel-free signal says a
    # page's bitmap was accepted, so the final pages' paint is a manual check.
    $strip1000 = Join-Path $repoRoot "tests/fixtures/strip-1000.pdf"
    if (-not (Test-Path $strip1000)) { throw "fixture not found at $strip1000" }
    $procStrip = Start-StripLaunch -Fixture $strip1000 -Step "strip fling"
    & $uxProbe canvas-wheel -32640 400 | Out-Null
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
