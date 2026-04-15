#!/usr/bin/env pwsh
#Requires -Version 5.1
# Phase 0 smoke test: build, run briefly, verify window opens and exe is under budget.
# Exits non-zero on any failure; CI uses this as the final gate.

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot

$exe = Join-Path $repoRoot "build/Release/litepdf.exe"

if (-not (Test-Path $exe)) {
    throw "exe not found at $exe — build first"
}

# Size budget (Phase 1: MuPDF linked but feature-pruning deferred to Phase 11): 8 MB.
$maxBytes = 8MB
$size = (Get-Item $exe).Length
if ($size -gt $maxBytes) {
    throw "litepdf.exe is $size bytes, exceeds Phase 0 budget of $maxBytes"
}
Write-Host "[OK] exe size: $size bytes"

# Launch and poll for a valid MainWindowHandle. On slow HDDs / cold-cache CI runners
# the window may take longer than a single fixed sleep to appear; poll up to 5 s.
$proc = Start-Process -FilePath $exe -PassThru
$deadline = (Get-Date).AddSeconds(5)
$hwnd = [IntPtr]::Zero
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 100
    if ($proc.HasExited) {
        throw "litepdf.exe exited during startup (exit code $($proc.ExitCode))"
    }
    $proc.Refresh()
    if ($proc.MainWindowHandle -ne [IntPtr]::Zero) {
        $hwnd = $proc.MainWindowHandle
        break
    }
}
if ($hwnd -eq [IntPtr]::Zero) {
    Stop-Process -Id $proc.Id -Force
    throw "litepdf.exe did not create a main window within 5 s"
}
Write-Host "[OK] main window handle: $hwnd"

Stop-Process -Id $proc.Id -Force
Write-Host "[OK] Phase 0 smoke test passed."
