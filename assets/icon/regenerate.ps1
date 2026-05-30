# Phase 9 icon regeneration driver.
# Run from any directory with Windows PowerShell 5.1+ or PowerShell 7+:
#     powershell -File assets/icon/regenerate.ps1   # Windows PowerShell 5.1
#     pwsh assets/icon/regenerate.ps1               # PowerShell 7+
# See README.md for prerequisites.
$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $PSCommandPath
$pyScript  = Join-Path $scriptDir 'regenerate.py'
$reqFile   = Join-Path $scriptDir 'requirements.txt'

# Locate Python. Prefer `python` on PATH; fall back to the `py` launcher on
# Windows. Written with plain Get-Command + if (not the null-conditional `?.`)
# so the script parses under Windows PowerShell 5.1, not only PowerShell 7+.
$python = $null
$pythonCmd = Get-Command python -ErrorAction SilentlyContinue
if ($pythonCmd) { $python = $pythonCmd.Source }
if (-not $python) {
    $pyCmd = Get-Command py -ErrorAction SilentlyContinue
    if ($pyCmd) { $python = $pyCmd.Source }
}
if (-not $python) { throw "Python 3.10+ required (not on PATH). Install python.org or 'winget install Python.Python.3'." }

Write-Host "[regenerate.ps1] Using Python: $python"
& $python -m pip install --quiet --requirement $reqFile
if ($LASTEXITCODE -ne 0) { throw "pip install failed (exit $LASTEXITCODE)" }

& $python $pyScript
if ($LASTEXITCODE -ne 0) { throw "regenerate.py failed (exit $LASTEXITCODE)" }

Write-Host "[regenerate.ps1] Done. Inspect assets/icon/*.png and assets/icon/*.ico."
