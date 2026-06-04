#!/usr/bin/env pwsh
#Requires -Version 5.1
# Compares a PR benchmark result against its base (Phase 11 spec §3.3) and
# exits non-zero if any gated metric regresses beyond threshold. Also offers
# -SelfTest: seven synthetic-JSON assertions proving the gate logic with no real
# timing (the roadmap "regression correctly blocked" evidence, run as a CTest
# test). 5.1-safe syntax throughout — no ?./??/ternary, no [double]::IsFinite
# (absent on .NET Framework 4.x that backs PS 5.1).
[CmdletBinding(DefaultParameterSetName = "Compare")]
param(
    [Parameter(ParameterSetName = "Compare", Mandatory = $true)][string]$Base,
    [Parameter(ParameterSetName = "Compare", Mandatory = $true)][string]$Pr,
    [Parameter(ParameterSetName = "SelfTest", Mandatory = $true)][switch]$SelfTest
)

$ErrorActionPreference = "Stop"

# --- Gated-metric configuration (data-driven; finalized by PR1 floors) -----
# open_render_ms is the boundary-agnostic total and the primary gate. open_ms
# is added to $GatedTimingMetrics in PR2 ONLY if PR1's measure-only run shows
# it clears the noise floor (spec §3.3). simple.pdf is always informational.
$GatedFixture       = "large.pdf"
$GatedTimingMetrics = @("open_render_ms")

function Get-Thresholds {
    $path = Join-Path (Split-Path -Parent $PSScriptRoot) "benchmarks/thresholds.json"
    if (-not (Test-Path $path)) { Write-Host "[ERROR] thresholds.json not found at $path"; exit 2 }
    $t = (Get-Content $path -Raw) | ConvertFrom-Json
    # Fail-closed: a corrupt thresholds.json must error, not silently gate on a
    # missing/zero/non-finite bound. (Assert-PositiveFinite is defined below; it
    # exists by the time this function is first CALLED in the dispatch path.)
    [void](Assert-PositiveFinite $t.time_regression_pct   "thresholds.time_regression_pct")
    [void](Assert-PositiveFinite $t.time_min_delta_ms     "thresholds.time_min_delta_ms")
    [void](Assert-PositiveFinite $t.size_regression_bytes "thresholds.size_regression_bytes")
    return $t
}

# Validate a value is numeric, finite, and > 0; return it as a double. Throws a
# "BENCH_VALIDATION:" message (recognized by callers) on any failure, so a
# missing/zero/non-finite gated field becomes an error exit, never a 0 that
# divides or silently passes.
function Assert-PositiveFinite {
    param($Value, [string]$Label)
    $isNum = ($Value -is [int]) -or ($Value -is [long]) -or ($Value -is [double]) -or ($Value -is [decimal])
    if (-not $isNum) { throw "BENCH_VALIDATION: $Label is missing or non-numeric" }
    $d = [double]$Value
    if ([double]::IsNaN($d) -or [double]::IsInfinity($d)) { throw "BENCH_VALIDATION: $Label is not finite" }
    if ($d -le 0) { throw "BENCH_VALIDATION: $Label must be > 0 (got $d)" }
    return $d
}

# Core comparison. Returns [pscustomobject]@{ Failed; Rows }. Does NOT call
# exit, so -SelfTest can assert on the result. Throws "BENCH_VALIDATION:" on a
# bad gated field (assertion 6) or a missing/invalid exe_bytes (assertion 7).
function Invoke-BenchmarkCompare {
    param($BaseObj, $PrObj, $Thresholds)

    $rows = @()
    $failed = $false

    # exe_bytes — always gated, deterministic.
    $baseBytes = Assert-PositiveFinite $BaseObj.exe_bytes "base.exe_bytes"
    $prBytes   = Assert-PositiveFinite $PrObj.exe_bytes   "pr.exe_bytes"
    $byteDelta = $prBytes - $baseBytes
    $sizeFail  = ($byteDelta -gt $Thresholds.size_regression_bytes)
    if ($sizeFail) { $failed = $true }
    $rows += [pscustomobject]@{
        Metric = "exe_bytes"; Base = $baseBytes; Pr = $prBytes
        Delta = $byteDelta; Pct = ""; Fail = $sizeFail
    }

    # Gated timing metrics on the gated fixture.
    $baseFx = $BaseObj.fixtures.$GatedFixture
    $prFx   = $PrObj.fixtures.$GatedFixture
    if ($null -eq $baseFx) { throw "BENCH_VALIDATION: base.fixtures.$GatedFixture missing" }
    if ($null -eq $prFx)   { throw "BENCH_VALIDATION: pr.fixtures.$GatedFixture missing" }

    foreach ($metric in $GatedTimingMetrics) {
        $b = Assert-PositiveFinite $baseFx.$metric "base.$GatedFixture.$metric"
        $p = Assert-PositiveFinite $prFx.$metric   "pr.$GatedFixture.$metric"
        $absDelta = $p - $b
        $pct = ($absDelta / $b) * 100.0
        $fail = ($pct -gt $Thresholds.time_regression_pct) -and ($absDelta -gt $Thresholds.time_min_delta_ms)
        if ($fail) { $failed = $true }
        $rows += [pscustomobject]@{
            Metric = "$GatedFixture/$metric"; Base = $b; Pr = $p
            Delta = $absDelta; Pct = [math]::Round($pct, 2); Fail = $fail
        }
    }

    return [pscustomobject]@{ Failed = $failed; Rows = $rows }
}

function New-SyntheticResult {
    param([double]$OpenRender, [double]$ExeBytes)
    return [pscustomobject]@{
        exe_bytes = $ExeBytes
        fixtures  = [pscustomobject]@{
            "large.pdf" = [pscustomobject]@{ open_render_ms = $OpenRender }
        }
    }
}

function Invoke-SelfTest {
    $thr = Get-Thresholds
    $script:selfTestOk = $true

    function Check([bool]$cond, [string]$label) {
        if ($cond) { Write-Host "[PASS] $label" }
        else { Write-Host "[FAIL] $label"; $script:selfTestOk = $false }
    }

    # 1: +15% delta, above the absolute min-delta -> blocked.
    $r = Invoke-BenchmarkCompare (New-SyntheticResult 100 40000000) (New-SyntheticResult 115 40000000) $thr
    Check ($r.Failed -eq $true) "1: +15% (> min-delta) blocks"

    # 2: +5% delta -> allowed (under the pct threshold).
    $r = Invoke-BenchmarkCompare (New-SyntheticResult 100 40000000) (New-SyntheticResult 105 40000000) $thr
    Check ($r.Failed -eq $false) "2: +5% allowed"

    # 3: +50% but tiny absolute (4 ms < 5 ms floor) -> allowed.
    $r = Invoke-BenchmarkCompare (New-SyntheticResult 8 40000000) (New-SyntheticResult 12 40000000) $thr
    Check ($r.Failed -eq $false) "3: +50% but < min-delta allowed"

    # 4: exe growth beyond the 256 KB tolerance -> blocked.
    $r = Invoke-BenchmarkCompare (New-SyntheticResult 100 40000000) (New-SyntheticResult 100 40300000) $thr
    Check ($r.Failed -eq $true) "4: exe +300000 B blocks"

    # 5: exe growth within tolerance -> allowed.
    $r = Invoke-BenchmarkCompare (New-SyntheticResult 100 40000000) (New-SyntheticResult 100 40100000) $thr
    Check ($r.Failed -eq $false) "5: exe +100000 B allowed"

    # 6: a zero gated field -> error (throws BENCH_VALIDATION).
    $threw = $false
    try {
        $bad = [pscustomobject]@{
            exe_bytes = 40000000
            fixtures  = [pscustomobject]@{ "large.pdf" = [pscustomobject]@{ open_render_ms = 0 } }
        }
        Invoke-BenchmarkCompare (New-SyntheticResult 100 40000000) $bad $thr | Out-Null
    } catch {
        if ($_.Exception.Message -like "BENCH_VALIDATION*") { $threw = $true }
    }
    Check $threw "6: zero gated field errors"

    # 7: missing base exe_bytes -> error (throws BENCH_VALIDATION). Guards the
    # exe_bytes validation path (Invoke-BenchmarkCompare's first check), which
    # assertion 6 (a zero timing field) never reaches.
    $threw = $false
    try {
        $noBytes = [pscustomobject]@{
            fixtures = [pscustomobject]@{ "large.pdf" = [pscustomobject]@{ open_render_ms = 100 } }
        }
        Invoke-BenchmarkCompare $noBytes (New-SyntheticResult 100 40000000) $thr | Out-Null
    } catch {
        if ($_.Exception.Message -like "BENCH_VALIDATION*") { $threw = $true }
    }
    Check $threw "7: missing exe_bytes errors"

    if ($script:selfTestOk) {
        Write-Host "[OK] benchmark self-test: 7/7 passed"
        exit 0
    }
    Write-Host "[FAIL] benchmark self-test had failures"
    exit 1
}

# ---------------------------------------------------------------- dispatch ---
if ($SelfTest) {
    Invoke-SelfTest   # exits
}

if (-not (Test-Path $Base)) { Write-Host "[ERROR] Base result not found: $Base"; exit 2 }
if (-not (Test-Path $Pr))   { Write-Host "[ERROR] PR result not found: $Pr"; exit 2 }
$thr = Get-Thresholds

try {
    $baseObj = (Get-Content $Base -Raw) | ConvertFrom-Json -ErrorAction Stop
    $prObj   = (Get-Content $Pr   -Raw) | ConvertFrom-Json -ErrorAction Stop
} catch {
    Write-Host "[ERROR] malformed benchmark JSON: $($_.Exception.Message)"
    exit 2
}

try {
    $cmp = Invoke-BenchmarkCompare $baseObj $prObj $thr
} catch {
    if ($_.Exception.Message -like "BENCH_VALIDATION*") {
        Write-Host "[ERROR] $($_.Exception.Message)"
        exit 2
    }
    throw
}

Write-Host ""
Write-Host ("{0,-28} {1,16} {2,16} {3,14} {4,8} {5}" -f "Metric", "Base", "Pr", "Delta", "Pct%", "Result")
foreach ($row in $cmp.Rows) {
    $status = "ok"
    if ($row.Fail) { $status = "FAIL" }
    Write-Host ("{0,-28} {1,16} {2,16} {3,14} {4,8} {5}" -f $row.Metric, $row.Base, $row.Pr, $row.Delta, $row.Pct, $status)
}
Write-Host ""

if ($cmp.Failed) {
    Write-Host "[FAIL] benchmark regression gate: one or more gated metrics regressed."
    exit 1
}
Write-Host "[OK] benchmark regression gate: all gated metrics within threshold."
exit 0
