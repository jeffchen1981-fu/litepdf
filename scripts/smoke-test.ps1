#!/usr/bin/env pwsh
# Phase 0 smoke test: build, run briefly, verify window opens and exe is under budget.
# Exits non-zero on any failure; CI uses this as the final gate.

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

$exe = Join-Path $repoRoot "build/Release/litepdf.exe"

if (-not (Test-Path $exe)) {
    throw "exe not found at $exe — build first"
}

# Size budget (Phase 0): 2 MB
$maxBytes = 2MB
$size = (Get-Item $exe).Length
if ($size -gt $maxBytes) {
    throw "litepdf.exe is $size bytes, exceeds Phase 0 budget of $maxBytes"
}
Write-Host "[OK] exe size: $size bytes"

# Launch briefly, verify process starts and window appears, then kill.
$proc = Start-Process -FilePath $exe -PassThru
Start-Sleep -Milliseconds 800
if ($proc.HasExited) {
    throw "litepdf.exe exited immediately (exit code $($proc.ExitCode))"
}
if (-not $proc.MainWindowHandle -or $proc.MainWindowHandle -eq [IntPtr]::Zero) {
    Stop-Process -Id $proc.Id -Force
    throw "litepdf.exe did not create a main window within 800 ms"
}
Write-Host "[OK] main window handle: $($proc.MainWindowHandle)"

Stop-Process -Id $proc.Id -Force
Write-Host "[OK] Phase 0 smoke test passed."
