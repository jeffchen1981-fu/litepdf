# Build Identity (#60) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every non-release build says so in the title bar, every build carries a git-derived identity in About, `litepdf-cli --version` and crash-dump names, and the CI and release gates assert that identity on the built artifacts.

**Architecture:** A build-time CMake script (`cmake/CollectBuildFacts.cmake`) collects raw git facts into a generated header on every build (`copy_if_different`, so an unchanged commit recompiles nothing). A pure C++ function `classify()` in `litepdf_core` turns the facts into `{display_version, build_id, is_release}`; the UI, the CLI and the crash handler read the result. `check-version-sync.ps1` and `smoke-test.ps1` gain `-ExpectDev` / `-ExpectRelease` modes that assert the classification of the real binaries.

**Tech Stack:** C++20 / Win32, CMake 3.25 (Visual Studio 17 2022 generator, multi-config), Catch2 v3.5.4, PowerShell 5.1-compatible scripts, GitHub Actions (`windows-2022`).

**Spec:** `docs/superpowers/specs/2026-09-28-build-identity-design.md`

## Global Constraints

- Build and test **Release** only; Debug fails with LNK2038 against the prebuilt MuPDF libs.
- **Never run a bare `cmake`/`ctest`**: the ones on PATH are a MinGW/WinLibs CMake 4.3.3, not the VS BuildTools CMake 3.31 that configured `build/`. Every PowerShell session that runs a step first sets:
  `$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"; $ctest = Join-Path (Split-Path $cmake) "ctest.exe"`
  Build dir is `build/` (already configured).
- Run the unit-test exe and `ctest` **from the repo root** (fixtures resolve relative to it).
- Do **not** bump `VERSION` (stays `1.3.0`).
- **No network access** is added to LitePDF, in any form.
- Release iff git proves it: `toplevel_match && exact_tag non-empty && !dirty`. Anything else is dev. A false "release" is the defect this work exists to prevent.
- Release `build_id` is the exact tag (`v1.3.0`), never the long describe form (`v1.3.0-0-g88513f2`).
- Fact strings are sanitized **as a whole value** to `^[A-Za-z0-9._-]+$`; a value with any other character becomes empty.
- Release title stays `LitePDF — x.pdf` / `LitePDF`; dev title is `LitePDF 1.3.0-dev — x.pdf` / `LitePDF 1.3.0-dev`. `kWindowTitle` stays `L"LitePDF"` (it is also the caption of MainWindow's MessageBoxes, About included).
- `.ps1` files: Windows PowerShell 5.1-compatible (no `?.`, `??`, ternary) and **ASCII-only outside comments**. The em dash is `[char]0x2014`, never a literal.
- Catch2 test names: ASCII, prefixed `Version:`, tag `[version]`.
- Test baseline before this work: re-measure with `ctest --test-dir build -C Release` at the start of Task 1 and record the number; do not quote an older one.
- Commits end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Branch and PR shape

One PR, branch `feat/60-build-identity` (the spec commit `79bc7a0` and this plan are already on it). Task order keeps CI green after every task: the gate stops reading the About literal (Task 4) before the literal is removed (Task 5).

## File Structure

| File | Responsibility |
|---|---|
| `src/core/Version.hpp` / `.cpp` (new) | `BuildFacts`, `BuildIdentity`, pure `classify()`, `build_identity()` (reads the generated header), `ascii_to_wide()` |
| `tests/unit/test_version.cpp` (new) | Catch2 cases for every `classify()` branch |
| `cmake/CollectBuildFacts.cmake` (new) | Build-time `cmake -P` script: git facts → `build/generated/litepdf_build_facts.h` |
| `scripts/test-collect-build-facts.ps1` (new) | ctest `version_script_selftest`: runs the collector against throwaway git repos |
| `CMakeLists.txt` | `find_package(Git)`, `litepdf_build_id` custom target, Version.cpp in core, two new ctests |
| `src/cli/main.cpp` | `--version` |
| `scripts/check-version-sync.ps1` | Drop About-literal check; artifact + classification assertions; `-SelfTest` |
| `src/ui/MainWindow.cpp` | Dev title prefix, About text |
| `src/app/CrashHandler.{hpp,cpp}`, `src/app/AppPaths.cpp` | Dump name carries the display version (+ comment updates) |
| `scripts/ux-probe.ps1` | Prefix title fallback |
| `scripts/smoke-test.ps1` | `-ExpectDev` / `-ExpectRelease` title assertion |
| `.github/workflows/ci.yml`, `release.yml` | Pass the modes |
| `README.md`, `CHANGELOG.md` | Versioning note, Unreleased entry |

---

## Task 1: Pure build-identity classification

**Files:**
- Create: `src/core/Version.hpp`, `src/core/Version.cpp`
- Create: `tests/unit/test_version.cpp`
- Modify: `CMakeLists.txt` (the `add_library(litepdf_core STATIC ...)` source list, starts at line 42)
- Modify: `tests/CMakeLists.txt` (the `target_sources(litepdf_unit_tests PRIVATE ...)` list)

**Interfaces:**
- Produces (in `namespace litepdf::core`, header `core/Version.hpp`):
  - `struct BuildFacts { bool toplevel_match = false; std::string_view exact_tag; std::string_view describe; std::string_view short_sha; bool dirty = false; };`
  - `struct BuildIdentity { std::string display_version; std::string build_id; bool is_release = false; };`
  - `BuildIdentity classify(const BuildFacts& facts, std::string_view version);`
  - `std::wstring ascii_to_wide(std::string_view ascii);`
  - (`build_identity()` is declared in Task 2, not here.)

- [ ] **Step 1: Record the test baseline**

Run from the repo root (PowerShell):
```powershell
$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ctest = Join-Path (Split-Path $cmake) "ctest.exe"
& $cmake --build build --config Release --parallel
& $ctest --test-dir build -C Release
```
Expected: all tests pass. Write the total (e.g. `375/375`) in the task report.

- [ ] **Step 2: Write the failing test**

Create `tests/unit/test_version.cpp`:
```cpp
// #60: pure-logic tests for the build-identity classification. The git facts
// are injected, so no repository is involved here; the fact collector itself
// is exercised by the version_script_selftest ctest.
#include "core/Version.hpp"

#include <catch2/catch_test_macros.hpp>

using litepdf::core::BuildFacts;
using litepdf::core::ascii_to_wide;
using litepdf::core::classify;

namespace {

BuildFacts facts(bool toplevel, const char* tag, const char* describe,
                 const char* sha, bool dirty) {
    BuildFacts f;
    f.toplevel_match = toplevel;
    f.exact_tag = tag;
    f.describe = describe;
    f.short_sha = sha;
    f.dirty = dirty;
    return f;
}

}  // namespace

TEST_CASE("Version: exact tag on a clean tree is a release", "[version]") {
    auto id = classify(facts(true, "v1.3.0", "v1.3.0-0-g88513f2", "88513f2", false), "1.3.0");
    REQUIRE(id.is_release);
    REQUIRE(id.display_version == "1.3.0");
    // The tag itself, never the long describe form: at a tag,
    // `git describe --tags --long` prints v1.3.0-0-g88513f2.
    REQUIRE(id.build_id == "v1.3.0");
}

TEST_CASE("Version: exact tag with a dirty tree is dev", "[version]") {
    auto id = classify(facts(true, "v1.3.0", "v1.3.0-0-g88513f2-dirty", "88513f2", true), "1.3.0");
    REQUIRE_FALSE(id.is_release);
    REQUIRE(id.display_version == "1.3.0-dev");
    REQUIRE(id.build_id == "v1.3.0-0-g88513f2-dirty");
}

TEST_CASE("Version: commits past a tag are dev with the describe id", "[version]") {
    auto id = classify(facts(true, "", "v1.3.0-18-gf7b1ed6", "f7b1ed6", false), "1.3.0");
    REQUIRE_FALSE(id.is_release);
    REQUIRE(id.display_version == "1.3.0-dev");
    REQUIRE(id.build_id == "v1.3.0-18-gf7b1ed6");
}

TEST_CASE("Version: no reachable tag falls back to the short sha", "[version]") {
    auto id = classify(facts(true, "", "", "f7b1ed6", false), "1.3.0");
    REQUIRE_FALSE(id.is_release);
    REQUIRE(id.display_version == "1.3.0-dev");
    REQUIRE(id.build_id == "gf7b1ed6");
}

TEST_CASE("Version: no reachable tag and a dirty tree marks the sha dirty", "[version]") {
    auto id = classify(facts(true, "", "", "f7b1ed6", true), "1.3.0");
    REQUIRE(id.build_id == "gf7b1ed6-dirty");
}

TEST_CASE("Version: git giving no answer is dev with an empty id", "[version]") {
    auto id = classify(facts(false, "", "", "", false), "1.3.0");
    REQUIRE_FALSE(id.is_release);
    REQUIRE(id.display_version == "1.3.0-dev");
    REQUIRE(id.build_id.empty());
}

TEST_CASE("Version: facts from a foreign repository are ignored", "[version]") {
    // release.yml's tarball build-back extracts inside the checkout, so git
    // run there answers for the parent repository. toplevel_match is false.
    auto id = classify(facts(false, "v1.3.0", "v1.3.0-0-g88513f2", "88513f2", false), "1.3.0");
    REQUIRE_FALSE(id.is_release);
    REQUIRE(id.display_version == "1.3.0-dev");
    REQUIRE(id.build_id.empty());
}

TEST_CASE("Version: a tag blanked by sanitizing is not a release", "[version]") {
    // A tag such as v1.3.0+meta fails the [A-Za-z0-9._-] rule, so the
    // collector records exact_tag and describe as empty.
    auto id = classify(facts(true, "", "", "88513f2", false), "1.3.0");
    REQUIRE_FALSE(id.is_release);
    REQUIRE(id.build_id == "g88513f2");
}

TEST_CASE("Version: ascii_to_wide widens byte for byte", "[version]") {
    REQUIRE(ascii_to_wide("1.3.0-dev") == L"1.3.0-dev");
    REQUIRE(ascii_to_wide("").empty());
}
```

In `tests/CMakeLists.txt`, add after the line `    unit/test_cli_bench_selection.cpp  # #73 --bench-selection harness`:
```cmake
    unit/test_version.cpp              # #60 build identity
```

- [ ] **Step 3: Run the build to verify it fails**

```powershell
& $cmake --build build --config Release --target litepdf_unit_tests
```
Expected: FAIL — `fatal error C1083: Cannot open include file: 'core/Version.hpp'`.

- [ ] **Step 4: Write the implementation**

Create `src/core/Version.hpp`:
```cpp
// LitePDF -- core::Version: which build is running (#60).
#pragma once

#include <string>
#include <string_view>

namespace litepdf::core {

// Raw git facts, collected at build time by cmake/CollectBuildFacts.cmake.
// Every string is either empty or matches [A-Za-z0-9._-]+.
struct BuildFacts {
    bool toplevel_match = false;  // git's top-level is this source tree
    std::string_view exact_tag;   // `git describe --tags --exact-match`
    std::string_view describe;    // `git describe --tags --long --dirty`
    std::string_view short_sha;   // `git rev-parse --short HEAD`
    bool dirty = false;           // tracked files modified
};

struct BuildIdentity {
    std::string display_version;  // "1.3.0" (release) or "1.3.0-dev"
    std::string build_id;         // "v1.3.0", "v1.3.0-18-gf7b1ed6", "gf7b1ed6", or ""
    bool is_release = false;
};

// A build is a release only when git proves it: this tree, exactly on a tag,
// clean. Everything else is dev -- a false "dev" costs a title suffix, a false
// "release" hides a dev build (the #60 failure). `version` is the numeric
// triple from VERSION.
BuildIdentity classify(const BuildFacts& facts, std::string_view version);

// Widens a string known to be ASCII (every BuildIdentity field is).
std::wstring ascii_to_wide(std::string_view ascii);

}  // namespace litepdf::core
```

Create `src/core/Version.cpp`:
```cpp
// LitePDF -- core::Version (#60).
#include "core/Version.hpp"

namespace litepdf::core {

BuildIdentity classify(const BuildFacts& facts, std::string_view version) {
    BuildIdentity id;
    if (facts.toplevel_match && !facts.exact_tag.empty() && !facts.dirty) {
        id.display_version = std::string(version);
        id.build_id = std::string(facts.exact_tag);
        id.is_release = true;
        return id;
    }
    id.display_version = std::string(version) + "-dev";
    // Facts about a repository that is not this source tree say nothing about
    // this build, so a foreign repo leaves the id empty.
    if (facts.toplevel_match) {
        if (!facts.describe.empty()) {
            id.build_id = std::string(facts.describe);
        } else if (!facts.short_sha.empty()) {
            id.build_id = "g" + std::string(facts.short_sha) + (facts.dirty ? "-dirty" : "");
        }
    }
    return id;
}

std::wstring ascii_to_wide(std::string_view ascii) {
    return std::wstring(ascii.begin(), ascii.end());
}

}  // namespace litepdf::core
```

In `CMakeLists.txt`, add to the `add_library(litepdf_core STATIC` list, right after `    src/core/Document.cpp`:
```cmake
    src/core/Version.cpp           # #60 build identity
```

- [ ] **Step 5: Run the tests to verify they pass**

```powershell
& $cmake --build build --config Release --target litepdf_unit_tests
& .\build\tests\Release\litepdf_unit_tests.exe "[version]"
```
Expected: `All tests passed (… assertions in 9 test cases)`.

- [ ] **Step 6: Commit**

```bash
git add src/core/Version.hpp src/core/Version.cpp tests/unit/test_version.cpp tests/CMakeLists.txt CMakeLists.txt
git commit -m "feat(core): classify a build as release or dev from git facts (#60)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

## Task 2: Collect git facts on every build

**Files:**
- Create: `cmake/CollectBuildFacts.cmake`
- Create: `scripts/test-collect-build-facts.ps1`
- Modify: `CMakeLists.txt` (after `include(ImportMuPDF)`; after the `litepdf_core` target; the `if(LITEPDF_PWSH)` ctest block near line 167)
- Modify: `src/core/Version.hpp`, `src/core/Version.cpp`

**Interfaces:**
- Consumes: `BuildFacts`, `BuildIdentity`, `classify()` from Task 1.
- Produces:
  - Generated header `build/generated/litepdf_build_facts.h` defining `LITEPDF_VERSION_TRIPLE` (string), `LITEPDF_FACT_TOPLEVEL_MATCH` (0/1), `LITEPDF_FACT_EXACT_TAG`, `LITEPDF_FACT_DESCRIBE`, `LITEPDF_FACT_SHORT_SHA` (strings), `LITEPDF_FACT_DIRTY` (0/1). Only `Version.cpp` includes it.
  - `const BuildIdentity& litepdf::core::build_identity();` — computed once, thread-safe (function-local static).
  - CMake target `litepdf_build_id`; `litepdf_core` depends on it.
  - ctest `version_script_selftest`.

- [ ] **Step 1: Write the failing self-test**

Create `scripts/test-collect-build-facts.ps1`:
```powershell
#!/usr/bin/env pwsh
#Requires -Version 5.1
# #60: runs cmake/CollectBuildFacts.cmake against throwaway git repositories,
# one per state the release path depends on. CI's checkout is shallow and
# tagless, so without this the exact-tag, dirty and toplevel-mismatch paths
# would first run inside the release workflow. Registered as the ctest
# version_script_selftest. 5.1-safe (no ?./??/ternary); ASCII only.
param(
    [Parameter(Mandatory = $true)][string]$CMake,
    [Parameter(Mandatory = $true)][string]$Git
)

# Under 5.1, native stderr plus "Stop" becomes a terminating NativeCommandError.
# Check exit codes explicitly instead.
$ErrorActionPreference = "Continue"

$script:ok = $true
$repoRoot  = Split-Path -Parent $PSScriptRoot
$collector = Join-Path $repoRoot "cmake/CollectBuildFacts.cmake"
$work      = Join-Path ([System.IO.Path]::GetTempPath()) ("litepdf-facts-" + [guid]::NewGuid().ToString("N"))

function Check([bool]$cond, [string]$label) {
    if ($cond) { Write-Host "[PASS] $label" }
    else { Write-Host "[FAIL] $label"; $script:ok = $false }
}

function Invoke-Git([string]$Dir, [string[]]$GitArgs) {
    & $Git -C $Dir @GitArgs 2>$null | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "git $($GitArgs -join ' ') failed in $Dir (exit $LASTEXITCODE)" }
}

function New-Repo([string]$Name) {
    $dir = Join-Path $work $Name
    New-Item -ItemType Directory -Force (Join-Path $dir "sub") | Out-Null
    Invoke-Git $dir @("init", "-q")
    Invoke-Git $dir @("config", "user.name", "selftest")
    Invoke-Git $dir @("config", "user.email", "selftest@example.invalid")
    Invoke-Git $dir @("config", "core.autocrlf", "false")
    Set-Content -Path (Join-Path $dir "a.txt") -Value "one" -Encoding ascii
    Set-Content -Path (Join-Path $dir "sub/b.txt") -Value "two" -Encoding ascii
    Invoke-Git $dir @("add", ".")
    Invoke-Git $dir @("commit", "-q", "-m", "first")
    return $dir
}

function Invoke-Collector([string]$Src, [string]$GitExe, [string]$Out) {
    & $CMake "-DGIT_EXECUTABLE=$GitExe" "-DSRC=$Src" "-DOUT=$Out" "-DVERSION=9.9.9" -P $collector | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "CollectBuildFacts.cmake failed for $Src (exit $LASTEXITCODE)" }
}

function Read-Facts([string]$Path) {
    $facts = @{}
    foreach ($line in (Get-Content $Path)) {
        if ($line -match '^#define LITEPDF_(FACT_[A-Z_]+|VERSION_TRIPLE) (.*)$') {
            $facts[$Matches[1]] = ($Matches[2] -replace '^"(.*)"$', '$1')
        }
    }
    return $facts
}

function Get-Facts([string]$Src, [string]$GitExe) {
    $out = Join-Path $work ("facts-" + [guid]::NewGuid().ToString("N") + ".h")
    Invoke-Collector $Src $GitExe $out
    return (Read-Facts $out)
}

try {
    New-Item -ItemType Directory -Force $work | Out-Null

    # A: exactly on a tag, clean.
    $repo = New-Repo "tagged"
    Invoke-Git $repo @("tag", "v9.9.9")
    $f = Get-Facts $repo $Git
    Check ($f["VERSION_TRIPLE"] -eq "9.9.9") "A: version triple is passed through"
    Check ($f["FACT_TOPLEVEL_MATCH"] -eq "1") "A: toplevel matches (8.3 temp paths resolve)"
    Check ($f["FACT_EXACT_TAG"] -eq "v9.9.9") "A: exact tag recorded"
    Check ($f["FACT_DESCRIBE"] -like "v9.9.9-0-g*") "A: long describe recorded"
    Check ($f["FACT_SHORT_SHA"] -ne "") "A: short sha recorded"
    Check ($f["FACT_DIRTY"] -eq "0") "A: clean tree"

    # B: on the tag, tracked file modified.
    Set-Content -Path (Join-Path $repo "a.txt") -Value "changed" -Encoding ascii
    $f = Get-Facts $repo $Git
    Check ($f["FACT_EXACT_TAG"] -eq "v9.9.9") "B: exact tag still recorded when dirty"
    Check ($f["FACT_DIRTY"] -eq "1") "B: dirty detected"
    Check ($f["FACT_DESCRIBE"] -like "*-dirty") "B: describe carries -dirty"

    # C: one commit past the tag.
    Invoke-Git $repo @("commit", "-q", "-am", "second")
    $f = Get-Facts $repo $Git
    Check ($f["FACT_EXACT_TAG"] -eq "") "C: no exact tag past the tag"
    Check ($f["FACT_DESCRIBE"] -like "v9.9.9-1-g*") "C: describe counts one commit"
    Check ($f["FACT_DIRTY"] -eq "0") "C: clean after commit"

    # D: no tags at all (CI's shallow, tagless checkout).
    $plain = New-Repo "untagged"
    $f = Get-Facts $plain $Git
    Check ($f["FACT_TOPLEVEL_MATCH"] -eq "1") "D: toplevel matches"
    Check ($f["FACT_EXACT_TAG"] -eq "" -and $f["FACT_DESCRIBE"] -eq "") "D: no tag facts"
    Check ($f["FACT_SHORT_SHA"] -ne "") "D: short sha still recorded"

    # E: SRC is a subdirectory, i.e. git answers for a parent repository
    # (release.yml extracts the source tarball inside the checkout).
    $f = Get-Facts (Join-Path $plain "sub") $Git
    Check ($f["FACT_TOPLEVEL_MATCH"] -eq "0") "E: foreign toplevel detected"
    Check ($f["FACT_SHORT_SHA"] -eq "" -and $f["FACT_DESCRIBE"] -eq "") "E: no facts from a foreign repo"

    # F: git unusable.
    $f = Get-Facts $repo (Join-Path $work "no-such-git.exe")
    Check ($f["FACT_TOPLEVEL_MATCH"] -eq "0" -and $f["FACT_SHORT_SHA"] -eq "") "F: missing git records no answer"

    # G: a tag outside [A-Za-z0-9._-] is blanked as a whole value.
    $odd = New-Repo "oddtag"
    Invoke-Git $odd @("tag", "v9.9.9+meta")
    $f = Get-Facts $odd $Git
    Check ($f["FACT_EXACT_TAG"] -eq "") "G: illegal tag blanked"
    Check ($f["FACT_DESCRIBE"] -eq "") "G: illegal describe blanked"
    Check ($f["FACT_SHORT_SHA"] -ne "") "G: sha unaffected"

    # I: the tag lookup answers but the dirty probe fails (corrupt index).
    # Unknown dirtiness must fail toward dev.
    $broken = New-Repo "brokenindex"
    Invoke-Git $broken @("tag", "v9.9.9")
    [System.IO.File]::WriteAllBytes((Join-Path $broken ".git/index"), [byte[]](1, 2, 3))
    $f = Get-Facts $broken $Git
    Check ($f["FACT_EXACT_TAG"] -eq "v9.9.9") "I: tag lookup still answers"
    Check ($f["FACT_DIRTY"] -eq "1") "I: a failed dirty probe records dirty"

    # H: an unchanged state leaves the header untouched (no rebuild).
    $stable = Join-Path $work "stable.h"
    Invoke-Collector $repo $Git $stable
    $t1 = (Get-Item $stable).LastWriteTimeUtc
    Start-Sleep -Milliseconds 1100
    Invoke-Collector $repo $Git $stable
    $t2 = (Get-Item $stable).LastWriteTimeUtc
    Check ($t1 -eq $t2) "H: unchanged facts keep the header's mtime"
} finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}

if ($script:ok) {
    Write-Host "[OK] CollectBuildFacts self-test passed"
    exit 0
}
Write-Host "[FAIL] CollectBuildFacts self-test had failures"
exit 1
```

- [ ] **Step 2: Run it to verify it fails**

```powershell
powershell -NoProfile -File scripts\test-collect-build-facts.ps1 -CMake $cmake -Git (Get-Command git).Source
```
Expected: FAIL — `CollectBuildFacts.cmake failed for …` (the script does not exist yet; cmake exits non-zero).

- [ ] **Step 3: Write the collector**

Create `cmake/CollectBuildFacts.cmake`:
```cmake
# #60: collects raw git facts about the source tree into a generated header.
# Runs at BUILD time (target litepdf_build_id), not configure time: CMake only
# re-configures when VERSION changes, so a configure-time SHA would go stale.
#
# It collects and never classifies -- src/core/Version.cpp decides release vs
# dev. Every git failure records "no answer" and the build continues; the
# classifier treats "no answer" as dev.
#
# Inputs (-D): GIT_EXECUTABLE (may be empty or *-NOTFOUND), SRC, OUT, VERSION.
cmake_minimum_required(VERSION 3.25)

foreach(_var SRC OUT VERSION)
    if(NOT DEFINED ${_var} OR "${${_var}}" STREQUAL "")
        message(FATAL_ERROR "CollectBuildFacts: -D${_var}= is required")
    endif()
endforeach()

# Runs git in SRC; sets out_var to trimmed stdout, or "" on any failure.
function(_litepdf_git out_var)
    set(${out_var} "" PARENT_SCOPE)
    if("${GIT_EXECUTABLE}" STREQUAL "" OR NOT EXISTS "${GIT_EXECUTABLE}")
        return()
    endif()
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${SRC}" ${ARGN}
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_rc EQUAL 0)
        set(${out_var} "${_out}" PARENT_SCOPE)
    endif()
endfunction()

# Whole-value sanitize: anything outside [A-Za-z0-9._-] blanks the value, so a
# generated C++ string literal can never be broken by a tag name.
function(_litepdf_sanitize var)
    if(NOT "${${var}}" MATCHES "^[A-Za-z0-9._-]+$")
        set(${var} "" PARENT_SCOPE)
    endif()
endfunction()

set(_toplevel_match 0)
set(_exact_tag "")
set(_describe "")
set(_short_sha "")
set(_dirty 0)

# Only trust git when its top-level IS this source tree. release.yml extracts
# the source tarball inside the checkout, where git would answer for the parent
# repository. REALPATH + lower-case absorbs 8.3 names, junctions and
# drive-letter case.
_litepdf_git(_toplevel rev-parse --show-toplevel)
if(NOT _toplevel STREQUAL "")
    get_filename_component(_top_real "${_toplevel}" REALPATH)
    get_filename_component(_src_real "${SRC}" REALPATH)
    string(TOLOWER "${_top_real}" _top_real)
    string(TOLOWER "${_src_real}" _src_real)
    if(_top_real STREQUAL _src_real)
        set(_toplevel_match 1)
    endif()
endif()

if(_toplevel_match)
    # Only release-shaped tags count: a local tag such as `wip` must never
    # make a release.
    _litepdf_git(_exact_tag describe --tags --exact-match --match "v[0-9]*")
    _litepdf_git(_describe describe --tags --long --dirty --match "v[0-9]*")
    _litepdf_git(_short_sha rev-parse --short HEAD)
    # Same dirty mechanism as _describe, and it works with no tags. It honours
    # .gitmodules `ignore = dirty`, so the MuPDF prune edits do not count.
    # Only tracked files count; untracked files never make a tree dirty.
    # No answer counts as dirty: this is the one fact whose unknown value must
    # be the dev value (a corrupt index can fail this probe while the tag
    # lookup still succeeds).
    # ":dirty" cannot occur in a ref name, so a tag such as v1.3.0-dirty can
    # never be mistaken for the dirty marker.
    _litepdf_git(_always describe --always --dirty=:dirty)
    if(_always STREQUAL "" OR _always MATCHES ":dirty$")
        set(_dirty 1)
    endif()
endif()

_litepdf_sanitize(_exact_tag)
_litepdf_sanitize(_describe)
_litepdf_sanitize(_short_sha)

set(_content "// Generated by cmake/CollectBuildFacts.cmake on every build. Do not edit.
#pragma once
#define LITEPDF_VERSION_TRIPLE \"${VERSION}\"
#define LITEPDF_FACT_TOPLEVEL_MATCH ${_toplevel_match}
#define LITEPDF_FACT_EXACT_TAG \"${_exact_tag}\"
#define LITEPDF_FACT_DESCRIBE \"${_describe}\"
#define LITEPDF_FACT_SHORT_SHA \"${_short_sha}\"
#define LITEPDF_FACT_DIRTY ${_dirty}
")

# Write to a temp file and copy only if different: an unchanged commit keeps
# the header's mtime, so nothing recompiles.
cmake_path(GET OUT PARENT_PATH _out_dir)
file(MAKE_DIRECTORY "${_out_dir}")
file(WRITE "${OUT}.tmp" "${_content}")
file(COPY_FILE "${OUT}.tmp" "${OUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUT}.tmp")
```

- [ ] **Step 4: Run the self-test to verify it passes**

```powershell
powershell -NoProfile -File scripts\test-collect-build-facts.ps1 -CMake $cmake -Git (Get-Command git).Source
```
Expected: every line `[PASS]`, final `[OK] CollectBuildFacts self-test passed`, exit 0. If `A: toplevel matches` fails, the REAL_PATH normalization is not resolving the temp path — fix the collector, do not weaken the check.

- [ ] **Step 5: Wire the target, the header and `build_identity()`**

In `CMakeLists.txt`, after the line `include(ImportMuPDF)  # defines litepdf::mupdf INTERFACE target`, add:
```cmake

# --- Build identity (#60) -------------------------------------------------
# Runs on every build (no OUTPUT => always out of date). The script rewrites
# the header only when a fact changed, so an unchanged commit recompiles
# nothing and a new commit recompiles only src/core/Version.cpp.
find_package(Git QUIET)
set(LITEPDF_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_custom_target(litepdf_build_id
    COMMAND "${CMAKE_COMMAND}"
            "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
            "-DSRC=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DOUT=${LITEPDF_GENERATED_DIR}/litepdf_build_facts.h"
            "-DVERSION=${PROJECT_VERSION}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CollectBuildFacts.cmake"
    COMMENT "Collecting build identity from git"
    VERBATIM)
```

After the line `target_include_directories(litepdf_core PUBLIC src)`, add:
```cmake
# #60: only src/core/Version.cpp includes the generated facts header.
target_include_directories(litepdf_core PRIVATE "${LITEPDF_GENERATED_DIR}")
add_dependencies(litepdf_core litepdf_build_id)
```

Inside the existing `if(LITEPDF_PWSH)` block, after the `set_tests_properties(benchmark_selftest ...)` call, add:
```cmake
        # #60: the build-time git fact collector, against throwaway repos.
        if(GIT_FOUND)
            add_test(NAME version_script_selftest
                     COMMAND "${LITEPDF_PWSH}" -NoProfile -File
                             "${CMAKE_SOURCE_DIR}/scripts/test-collect-build-facts.ps1"
                             -CMake "${CMAKE_COMMAND}" -Git "${GIT_EXECUTABLE}")
            set_tests_properties(version_script_selftest PROPERTIES
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        else()
            message(WARNING "git not found; version_script_selftest not registered.")
        endif()
```

In `src/core/Version.hpp`, add before the `ascii_to_wide` declaration:
```cpp
// This build's identity, from the facts collected when it was built.
// Computed once; safe to call from any thread.
const BuildIdentity& build_identity();

```

In `src/core/Version.cpp`, add `#include "litepdf_build_facts.h"  // generated, see cmake/CollectBuildFacts.cmake` after `#include "core/Version.hpp"`, and add before `std::wstring ascii_to_wide`:
```cpp
const BuildIdentity& build_identity() {
    static const BuildIdentity id = [] {
        BuildFacts f;
        f.toplevel_match = LITEPDF_FACT_TOPLEVEL_MATCH != 0;
        f.exact_tag = LITEPDF_FACT_EXACT_TAG;
        f.describe = LITEPDF_FACT_DESCRIBE;
        f.short_sha = LITEPDF_FACT_SHORT_SHA;
        f.dirty = LITEPDF_FACT_DIRTY != 0;
        return classify(f, LITEPDF_VERSION_TRIPLE);
    }();
    return id;
}

```

Add one test to `tests/unit/test_version.cpp` (end of file):
```cpp
TEST_CASE("Version: this build's identity is well formed", "[version]") {
    const auto& id = litepdf::core::build_identity();
    REQUIRE_FALSE(id.display_version.empty());
    if (id.is_release) {
        REQUIRE(id.display_version.find("-dev") == std::string::npos);
        REQUIRE_FALSE(id.build_id.empty());
    } else {
        REQUIRE(id.display_version.size() > 4);
        REQUIRE(id.display_version.substr(id.display_version.size() - 4) == "-dev");
    }
}
```

- [ ] **Step 6: Reconfigure, build and run the tests**

```powershell
& $cmake -B build
& $cmake --build build --config Release --parallel
& .\build\tests\Release\litepdf_unit_tests.exe "[version]"
& $ctest --test-dir build -C Release -R "version_script_selftest|Version:" --output-on-failure
Get-Content build\generated\litepdf_build_facts.h
```
Expected: `[version]` 10 test cases pass; the ctest run passes; the header shows `LITEPDF_FACT_TOPLEVEL_MATCH 1`, an empty `EXACT_TAG` and a `v1.3.0-…` describe (this branch is past the tag).

- [ ] **Step 7: Verify the no-rebuild behaviour**

```powershell
& $cmake --build build --config Release --target litepdf_core -- /v:n | Select-String '^\s+[\w.-]+\.cpp\s*$'
```
Expected: no output — a second build with no new commit compiles nothing. (The recompile-on-new-commit half is checked in Task 3 Step 1, after this task's commit exists.)

- [ ] **Step 8: Run the full suite and commit**

```powershell
& $ctest --test-dir build -C Release
```
Expected: baseline + 11 (10 `Version:` cases + `version_script_selftest`), all pass.
```bash
git add cmake/CollectBuildFacts.cmake scripts/test-collect-build-facts.ps1 CMakeLists.txt src/core/Version.hpp src/core/Version.cpp tests/unit/test_version.cpp
git commit -m "build: collect git build facts on every build (#60)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

## Task 3: `litepdf-cli --version`

**Files:**
- Modify: `src/cli/main.cpp` (includes at lines 13-27; `int main` at line 229)

**Interfaces:**
- Consumes: `litepdf::core::build_identity()` from Task 2 (`#include "core/Version.hpp"`).
- Produces: `litepdf-cli --version` prints exactly three lines to stdout and exits 0:
  ```
  version=<display_version>
  build=<build_id>
  release=<0|1>
  ```
  Task 4's gate parses these keys.

- [ ] **Step 1: Verify the new commit recompiles only Version.cpp**

Task 2's commit changed HEAD, so the facts header must change:
```powershell
& $cmake --build build --config Release --target litepdf_core -- /v:n | Select-String '^\s+[\w.-]+\.cpp\s*$'
```
Expected: exactly one line, `Version.cpp` (the pattern matches MSBuild's bare per-file lines, not the `cl.exe` command line). Record the output in the task report. If other TUs recompile, stop and report — the header dependency is wider than designed.

- [ ] **Step 2: Show the current (failing) behaviour**

```powershell
& $cmake --build build --config Release --target litepdf-cli
.\build\Release\litepdf-cli.exe --version; "exit=$LASTEXITCODE"
```
Expected: FAIL — `--version` is treated as a file path; an open error and a non-zero exit.

- [ ] **Step 3: Implement**

In `src/cli/main.cpp`, add `#include "core/Version.hpp"` after `#include "core/Document.hpp"`. At the top of `int main(int argc, char* argv[])`, before `if (argc < 2) {`, add:
```cpp
    // #60: build identity for gates and bug reports. key=value lines so
    // scripts/check-version-sync.ps1 never has to guess the format.
    if (argc >= 2 && std::strcmp(argv[1], "--version") == 0) {
        const auto& id = litepdf::core::build_identity();
        std::printf("version=%s\nbuild=%s\nrelease=%d\n",
                    id.display_version.c_str(), id.build_id.c_str(),
                    id.is_release ? 1 : 0);
        return 0;
    }
```
In the usage string, change `"Usage: %s <file> [--render N | --benchmark [--iterations N] [--json]\n"` to:
```cpp
            "Usage: %s --version\n"
            "       %s <file> [--render N | --benchmark [--iterations N] [--json]\n"
```
and pass `argv[0]` twice to that `fprintf` (`argv[0], argv[0]);`).

- [ ] **Step 4: Verify**

```powershell
& $cmake --build build --config Release --target litepdf-cli
.\build\Release\litepdf-cli.exe --version; "exit=$LASTEXITCODE"
.\build\Release\litepdf-cli.exe; "exit=$LASTEXITCODE"
```
Expected: first command prints `version=1.3.0-dev`, `build=v1.3.0-<n>-g<sha>-dirty` (this task's edit is not committed yet, so the tree is dirty), `release=0`, `exit=0`. Second prints both usage lines and `exit=2`.

- [ ] **Step 5: Commit**

```bash
git add src/cli/main.cpp
git commit -m "feat(cli): --version prints the build identity (#60)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

## Task 4: Version-sync gate asserts the built artifacts

**Files:**
- Modify: `scripts/check-version-sync.ps1`
- Modify: `CMakeLists.txt` (the `if(LITEPDF_PWSH)` ctest block)
- Modify: `.github/workflows/ci.yml:32`, `.github/workflows/release.yml:61`

**Interfaces:**
- Consumes: `litepdf-cli --version` output format from Task 3; exe `ProductVersion` from the existing `.rc`.
- Produces: `check-version-sync.ps1 [-ExpectRelease [-Tag <name>] | -ExpectDev | -SelfTest]`; ctest `version_sync_selftest`.

- [ ] **Step 1: Show the gate cannot fail today on a misclassified build**

```powershell
powershell -NoProfile -File scripts\check-version-sync.ps1 -ExpectRelease; "exit=$LASTEXITCODE"
```
Expected: exit 0 with `[OK]`. The script has no `param` block, so `-ExpectRelease` is silently ignored — that silence is the point: the gate cannot assert classification yet. Record that it exits 0.

- [ ] **Step 2: Replace the header comment and add the parameters**

Replace everything from line 3 (`# Runs under BOTH Windows PowerShell 5.1 …`) through the line `$ErrorActionPreference = "Stop"` with:
```powershell
# Runs under BOTH Windows PowerShell 5.1 (local dev -- no pwsh 7 here) and
# PowerShell 7 (CI invokes it via `shell: pwsh`). #Requires -Version 5.1 is a
# *minimum*, intentionally not an upper bound: do NOT add a PSEdition='Desktop'
# guard -- it would throw in CI. The rule is to author with 5.1-compatible
# syntax only (no ?./??/ternary), which then runs identically on both.
#
# Verifies that the version everywhere agrees with the canonical VERSION file,
# and that the BUILT binaries classify the way the caller expects:
#
#   1. The embedded Win32 VERSIONINFO resource. resources/litepdf.rc.in is a
#      configure_file() template filled by CMake from VERSION, so the .rc
#      cannot drift by construction. The template must stay parametric, and
#      the generated build/litepdf.rc must match VERSION when present.
#
#   2. The built artifacts (#60). `litepdf-cli --version` reports the build
#      identity computed from git at build time; its numeric triple must equal
#      VERSION and litepdf.exe's ProductVersion. With -ExpectRelease the build
#      must classify as a release whose id is the tag; with -ExpectDev it must
#      carry -dev and a non-empty id. Comparing numbers alone could only pass:
#      a dev build and a release carry the same triple.
#
# The About dialog no longer holds a version literal; it reads the build
# identity at run time, so there is no source literal to check.
#
# VERSION may carry a '-<suffix>'; comparisons use the stripped triple, the
# same normalization as CMakeLists.txt (REGEX REPLACE "-.*$" "").
#
# Exits 0 on match, 1 on divergence with diagnostics. CI calls this after the
# Build step so the binaries exist. -SelfTest proves the artifact assertions
# can fail (ctest version_sync_selftest).
[CmdletBinding(DefaultParameterSetName = "Local")]
param(
    [Parameter(ParameterSetName = "Release", Mandatory = $true)][switch]$ExpectRelease,
    [Parameter(ParameterSetName = "Release")][string]$Tag = "",
    [Parameter(ParameterSetName = "Dev", Mandatory = $true)][switch]$ExpectDev,
    [Parameter(ParameterSetName = "SelfTest", Mandatory = $true)][switch]$SelfTest
)

$ErrorActionPreference = "Stop"

# Returns an array of failure messages (empty = pass). Pure: no I/O, so the
# self-test can feed it synthetic cli output.
function Test-CliIdentity {
    param(
        [string[]]$CliOutput,       # lines printed by `litepdf-cli --version`
        [string]$Expected,          # VERSION triple, e.g. 1.3.0
        [string]$ProductVersion,    # litepdf.exe ProductVersion, e.g. 1.3.0.0
        [string]$Mode,              # Release | Dev | Local
        [string]$Tag                # required tag for Release
    )
    $failures = @()
    $kv = @{}
    foreach ($line in $CliOutput) {
        $t = "$line".Trim()
        $i = $t.IndexOf("=")
        if ($i -gt 0) { $kv[$t.Substring(0, $i)] = $t.Substring($i + 1) }
    }
    foreach ($key in @("version", "build", "release")) {
        if (-not $kv.ContainsKey($key)) { $failures += "litepdf-cli --version printed no '$key=' line" }
    }
    if ($failures.Count -gt 0) { return ,$failures }

    $numeric = $kv["version"] -replace '-.*$', ''
    if ($numeric -ne $Expected) {
        $failures += "litepdf-cli version '$($kv["version"])' does not match VERSION '$Expected'"
    }
    if ($ProductVersion -ne "$Expected.0") {
        $failures += "litepdf.exe ProductVersion '$ProductVersion' does not match '$Expected.0'"
    }
    if ($Mode -eq "Release") {
        if ($kv["release"] -ne "1") { $failures += "build is not classified as a release (release=$($kv["release"]))" }
        if ($kv["version"] -ne $Expected) { $failures += "release version must be the bare triple '$Expected', got '$($kv["version"])'" }
        if ($kv["build"] -ne $Tag) { $failures += "release build id '$($kv["build"])' does not equal the tag '$Tag'" }
    } elseif ($Mode -eq "Dev") {
        if ($kv["release"] -ne "0") { $failures += "build is not classified as dev (release=$($kv["release"]))" }
        if ($kv["version"] -ne "$Expected-dev") { $failures += "dev version must be '$Expected-dev', got '$($kv["version"])'" }
        if ($kv["build"] -eq "") { $failures += "dev build id is empty (no git provenance)" }
    }
    return ,$failures
}

function Invoke-SelfTest {
    $script:selfTestOk = $true
    function Check([bool]$cond, [string]$label) {
        if ($cond) { Write-Host "[PASS] $label" }
        else { Write-Host "[FAIL] $label"; $script:selfTestOk = $false }
    }
    $rel = @("version=1.3.0", "build=v1.3.0", "release=1")
    $dev = @("version=1.3.0-dev", "build=v1.3.0-18-gf7b1ed6", "release=0")

    $f = Test-CliIdentity $rel "1.3.0" "1.3.0.0" "Release" "v1.3.0"
    Check ($f.Count -eq 0) "1: release build passes -ExpectRelease"
    $f = Test-CliIdentity $dev "1.3.0" "1.3.0.0" "Release" "v1.3.0"
    Check ($f.Count -gt 0) "2: dev build fails -ExpectRelease"
    $f = Test-CliIdentity @("version=1.3.0", "build=v1.3.0-0-g88513f2", "release=1") "1.3.0" "1.3.0.0" "Release" "v1.3.0"
    Check ($f.Count -gt 0) "3: long describe id fails -ExpectRelease"
    $f = Test-CliIdentity $dev "1.3.0" "1.3.0.0" "Dev" ""
    Check ($f.Count -eq 0) "4: dev build passes -ExpectDev"
    $f = Test-CliIdentity @("version=1.3.0-dev", "build=", "release=0") "1.3.0" "1.3.0.0" "Dev" ""
    Check ($f.Count -gt 0) "5: empty dev id fails -ExpectDev"
    $f = Test-CliIdentity $rel "1.3.0" "1.3.0.0" "Dev" ""
    Check ($f.Count -gt 0) "6: release build fails -ExpectDev"
    $f = Test-CliIdentity @("version=1.2.0-dev", "build=gabc1234", "release=0") "1.3.0" "1.3.0.0" "Local" ""
    Check ($f.Count -gt 0) "7: numeric mismatch fails"
    $f = Test-CliIdentity $dev "1.3.0" "1.2.0.0" "Local" ""
    Check ($f.Count -gt 0) "8: ProductVersion mismatch fails"
    $f = Test-CliIdentity @("version=1.3.0-dev", "release=0") "1.3.0" "1.3.0.0" "Local" ""
    Check ($f.Count -gt 0) "9: missing build= line fails"

    if ($script:selfTestOk) { Write-Host "[OK] version-sync self-test: 9/9 passed"; exit 0 }
    Write-Host "[FAIL] version-sync self-test had failures"
    exit 1
}

if ($SelfTest) {
    Invoke-SelfTest   # exits
}
```

- [ ] **Step 3: Remove the About-literal check**

Delete the line `$mainWindow  = Join-Path $repoRoot "src/ui/MainWindow.cpp"` and the line `if (-not (Test-Path $mainWindow))  { throw "MainWindow.cpp not found at $mainWindow" }`.

Delete the whole block from `$aboutLine = Select-String -Path $mainWindow -Pattern 'LitePDF v(\d+\.\d+\.\d+)' | Select-Object -First 1` through the closing `}` of `if ($actual -ne $expected) { … exit 1 }` (it ends with `Write-Host "Update the About dialog string to match VERSION (or vice versa) before tagging."` / `exit 1` / `}`).

In the header of the `.rc` section, the comment `# --- Surface 2a: the .rc template must stay parametric` and `# --- Surface 2b: …` keep working; renumber them to `Surface 1a` / `Surface 1b` to match the new header.

- [ ] **Step 4: Add the artifact check and replace the final report**

Replace the three lines
```powershell
Write-Host "[OK] version sync: VERSION=$versionRaw"
Write-Host "       About dialog : v$actual"
Write-Host "       VERSIONINFO  : $rcStatus"
```
with:
```powershell
# --- Surface 2: the built binaries (#60) ----------------------------------
$mode = "Local"
if ($ExpectRelease) { $mode = "Release" }
if ($ExpectDev)     { $mode = "Dev" }
if ($mode -eq "Release" -and $Tag -eq "") { $Tag = "v$expected" }

$cliExe = Join-Path $repoRoot "build/Release/litepdf-cli.exe"
$appExe = Join-Path $repoRoot "build/Release/litepdf.exe"
if ((Test-Path $cliExe) -and (Test-Path $appExe)) {
    $cliOut = & $cliExe --version
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[FAIL] litepdf-cli --version exited $LASTEXITCODE"
        exit 1
    }
    $productVersion = (Get-Item $appExe).VersionInfo.ProductVersion
    $failures = Test-CliIdentity $cliOut $expected $productVersion $mode $Tag
    if ($failures.Count -gt 0) {
        Write-Host "[FAIL] built binaries ($mode):"
        foreach ($f in $failures) { Write-Host "  $f" }
        Write-Host "  litepdf-cli --version printed:"
        foreach ($line in $cliOut) { Write-Host "    $line" }
        exit 1
    }
    $artifactStatus = "$mode OK: " + (($cliOut | ForEach-Object { "$_".Trim() }) -join ", ")
    if ($mode -eq "Local") {
        $artifactStatus += " (classification not asserted; pass -ExpectDev or -ExpectRelease)"
    }
} elseif ($env:CI -eq "true" -or $mode -ne "Local") {
    Write-Host "[FAIL] build/Release/litepdf-cli.exe or litepdf.exe not found."
    Write-Host "  This gate asserts the built binaries; run it after the Build step."
    exit 1
} else {
    $artifactStatus = "skipped (no build/Release binaries; build first)"
}

Write-Host "[OK] version sync: VERSION=$versionRaw"
Write-Host "       VERSIONINFO  : $rcStatus"
Write-Host "       binaries     : $artifactStatus"
```

- [ ] **Step 5: Register the self-test**

In `CMakeLists.txt`, inside the `if(LITEPDF_PWSH)` block, after the `set_tests_properties(benchmark_selftest ...)` call, add:
```cmake
        # #60: proves the version-sync gate's artifact assertions can fail.
        add_test(NAME version_sync_selftest
                 COMMAND "${LITEPDF_PWSH}" -NoProfile -File
                         "${CMAKE_SOURCE_DIR}/scripts/check-version-sync.ps1" -SelfTest)
        set_tests_properties(version_sync_selftest PROPERTIES
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
```

- [ ] **Step 6: Wire the workflows**

`.github/workflows/ci.yml` line 32: `run: ./scripts/check-version-sync.ps1` → `run: ./scripts/check-version-sync.ps1 -ExpectDev`

`.github/workflows/release.yml` line 61: `run: ./scripts/check-version-sync.ps1` → `run: ./scripts/check-version-sync.ps1 -ExpectRelease -Tag $env:GITHUB_REF_NAME`

- [ ] **Step 7: Verify the gate passes and fails where it should**

```powershell
powershell -NoProfile -File scripts\check-version-sync.ps1 -SelfTest; "exit=$LASTEXITCODE"
powershell -NoProfile -File scripts\check-version-sync.ps1 -ExpectDev; "exit=$LASTEXITCODE"
powershell -NoProfile -File scripts\check-version-sync.ps1 -ExpectRelease; "exit=$LASTEXITCODE"
powershell -NoProfile -File scripts\check-version-sync.ps1; "exit=$LASTEXITCODE"
& $cmake -B build
& $ctest --test-dir build -C Release -R version_sync_selftest --output-on-failure
```
Expected, in order: `9/9 passed` exit 0; `[OK]` with `binaries : Dev OK: …` exit 0; `[FAIL] built binaries (Release)` listing "not classified as a release" exit 1 (this branch is a dev build — the gate failing here is the point); `[OK]` with `Local OK` exit 0; ctest passes.

- [ ] **Step 8: Commit**

```bash
git add scripts/check-version-sync.ps1 CMakeLists.txt .github/workflows/ci.yml .github/workflows/release.yml
git commit -m "ci: version-sync gate asserts the built binaries' identity (#60)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

## Task 5: Title bar, About and crash-dump name

**Files:**
- Modify: `src/ui/MainWindow.cpp` (includes ~line 9-18; anonymous-namespace constants at lines 40-41; `update_window_title` at 205-215; `IDM_HELP_ABOUT` at ~1695-1704; `CreateWindowExW` at ~2392-2396)
- Modify: `src/ui/MainWindow.hpp:125` (comment only)
- Modify: `src/app/CrashHandler.cpp`, `src/app/CrashHandler.hpp:8`, `src/app/AppPaths.cpp:55`
- Modify: `scripts/ux-probe.ps1:57`

**Interfaces:**
- Consumes: `litepdf::core::build_identity()`, `litepdf::core::ascii_to_wide()` from Tasks 1-2.
- Produces: window titles `LitePDF 1.3.0-dev` / `LitePDF 1.3.0-dev — <label>` on dev builds (Task 6's smoke test asserts them).

- [ ] **Step 1: Record the current title and About text**

Create `build\gui\about-probe.ps1` (scratch, not committed):
```powershell
# Scratch probe for #60 Task 5: prints the main window title and the About text.
# Run from the repo root in Windows PowerShell 5.1 with no other litepdf running.
Add-Type @"
using System; using System.Runtime.InteropServices; using System.Text;
public class P {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
}
"@
$env:LITEPDF_NO_RESTORE = "1"
$p = Start-Process -FilePath "build\Release\litepdf.exe" -ArgumentList @("tests\fixtures\bookmarks.pdf") -PassThru
Start-Sleep -Seconds 3
$p.Refresh()
"TITLE: " + $p.MainWindowTitle
[P]::PostMessage($p.MainWindowHandle, 0x0111, [IntPtr]40003, [IntPtr]::Zero) | Out-Null   # WM_COMMAND IDM_HELP_ABOUT
Start-Sleep -Seconds 1
$dlg = [P]::FindWindow("#32770", "LitePDF")
$sb = New-Object System.Text.StringBuilder 1024
[P]::GetWindowText([P]::GetDlgItem($dlg, 0xFFFF), $sb, 1024) | Out-Null   # MessageBox text control
"ABOUT:"; $sb.ToString()
Stop-Process -Id $p.Id -Force
```
Run: `powershell -NoProfile -File build\gui\about-probe.ps1`
Expected (before the change): `TITLE: LitePDF — bookmarks.pdf` and About starting `LitePDF v1.3.0`.

- [ ] **Step 2: Implement the title and About**

In `src/ui/MainWindow.cpp`, add `#include "core/Version.hpp"          // #60 build identity` after `#include "core/TabList.hpp"`.

After the line `constexpr wchar_t kWindowTitle[]     = L"LitePDF";`, add:
```cpp

// #60: a non-release build names its version in every title, so a dev build
// can never pass for a release. Release builds keep the bare product name.
// kWindowTitle itself stays "LitePDF": it is also every MessageBox caption.
std::wstring title_base() {
    const auto& id = litepdf::core::build_identity();
    std::wstring t = kWindowTitle;
    if (!id.is_release) {
        t += L' ';
        t += litepdf::core::ascii_to_wide(id.display_version);
    }
    return t;
}
```

In `src/ui/MainWindow.hpp`, change the comment `// Rewrite window title based on the active tab (or reset to "LitePDF").` to `// Rewrite window title based on the active tab (or reset to the bare title; dev builds add "<version>-dev").`

Replace the body of `MainWindow::update_window_title()`:
```cpp
void MainWindow::update_window_title() {
    if (!hwnd_) return;
    auto* t = tabs_ ? tabs_->active_tab() : nullptr;
    std::wstring title = title_base();
    if (t) {
        title += L" \u2014 ";
        title += t->label;
    }
    SetWindowTextW(hwnd_, title.c_str());
}
```

In the `CreateWindowExW` call, change `0, kWindowClassName, kWindowTitle,` to `0, kWindowClassName, title_base().c_str(),`.

Replace the `case IDM_HELP_ABOUT:` block (the `MessageBoxW` with the `L"LitePDF v1.3.0\n\n"` literal and its `return 0;`) with:
```cpp
                case IDM_HELP_ABOUT: {
                    // #60: version and build come from the build identity, so
                    // there is no version literal to keep in sync.
                    const auto& id = litepdf::core::build_identity();
                    std::wstring text = L"LitePDF ";
                    text += litepdf::core::ascii_to_wide(id.display_version);
                    text += L"\n";
                    if (!id.build_id.empty()) {
                        text += L"Build: ";
                        text += litepdf::core::ascii_to_wide(id.build_id);
                        text += L"\n";
                    }
                    text += L"\nA lightweight PDF / ePub / CBZ / XPS viewer for Windows.\n\n"
                            L"License: AGPL-3.0\n"
                            // Source of truth: third_party/mupdf FZ_VERSION; update on bumps.
                            L"Engine: MuPDF 1.27.2\n"
                            L"Rendering: Direct2D\n\n"
                            L"Releases: https://github.com/jeffchen1981-fu/litepdf/releases";
                    MessageBoxW(hwnd, text.c_str(), kWindowTitle, MB_ICONINFORMATION);
                    return 0;
                }
```

- [ ] **Step 3: Implement the crash-dump name**

In `src/app/CrashHandler.cpp`, add `#include "core/Version.hpp"` after `#include "app/CrashHandler.hpp"`.

Replace the comment above `g_prefix`:
```cpp
// Prebuilt at install time: "<crashes_dir>\litepdf-<display_version>-" in a
// fixed buffer, so the filter only appends "<pid>-<tick>.dmp" and never
// formats the directory or the version.
```
In `on_unhandled`, change the format string `L"%slitepdf-%lu-%lu.dmp"` to `L"%s%lu-%lu.dmp"`.

In `install_crash_handler`, replace
```cpp
    if (!p.empty() && p.back() != L'\\') p.push_back(L'\\');
    StringCchCopyW(g_prefix, ARRAYSIZE(g_prefix), p.c_str());  // truncates safely if huge
```
with
```cpp
    if (!p.empty() && p.back() != L'\\') p.push_back(L'\\');
    // #60: the dump name carries the build's version, so a dump sent in
    // without context still says which build (and which pdb) produced it.
    p += L"litepdf-";
    p += litepdf::core::ascii_to_wide(litepdf::core::build_identity().display_version);
    p += L'-';
    StringCchCopyW(g_prefix, ARRAYSIZE(g_prefix), p.c_str());  // truncates safely if huge
```

`src/app/CrashHandler.hpp:8`: `// minidump to <crashes_dir>\litepdf-<pid>-<ticks>.dmp, then returns` → `// minidump to <crashes_dir>\litepdf-<version>-<pid>-<ticks>.dmp, then returns`

`src/app/AppPaths.cpp:55`: `// lexical filename sort is wrong: names are litepdf-<pid>-<tick>.dmp, so it` → `// lexical filename sort is wrong: names are litepdf-<version>-<pid>-<tick>.dmp, so it`

- [ ] **Step 4: Make ux-probe's title fallback match a prefix**

`scripts/ux-probe.ps1` line 57, replace
```powershell
  if ($hwnd -eq [IntPtr]::Zero) { $hwnd = [W.U32]::FindWindow($null, 'LitePDF') }
```
with
```powershell
  if ($hwnd -eq [IntPtr]::Zero) {
    # #60: dev builds title the window "LitePDF <version>-dev", so match by prefix.
    $p = Get-Process litepdf -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowTitle -like 'LitePDF*' } | Select-Object -First 1
    if ($p) { $hwnd = $p.MainWindowHandle }
  }
```

- [ ] **Step 5: Build and verify**

```powershell
& $cmake --build build --config Release --parallel
powershell -NoProfile -File build\gui\about-probe.ps1
Select-String -Path src\ui\MainWindow.cpp -Pattern 'LitePDF v\d'
```
(This text probe stands in for the spec §4 "screenshot About" item: it checks the same content, reproducibly.)
Expected: `TITLE: LitePDF 1.3.0-dev — bookmarks.pdf`; About text starts `LitePDF 1.3.0-dev`, then `Build: v1.3.0-<n>-g<sha>-dirty` (uncommitted task edits), and ends with the `Releases:` line; the `Select-String` prints nothing (the literal is gone).

- [ ] **Step 6: Run the gates and the full suite**

```powershell
powershell -NoProfile -File scripts\check-version-sync.ps1 -ExpectDev; "exit=$LASTEXITCODE"
& $ctest --test-dir build -C Release
```
Expected: gate exit 0; all tests pass (same count as after Task 4).

- [ ] **Step 7: Commit**

```bash
git add src/ui/MainWindow.cpp src/ui/MainWindow.hpp src/app/CrashHandler.cpp src/app/CrashHandler.hpp src/app/AppPaths.cpp scripts/ux-probe.ps1
git commit -m "feat(ui): dev builds name their version in the title; About shows the build (#60)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

## Task 6: Smoke test asserts the title; docs

**Files:**
- Modify: `scripts/smoke-test.ps1` (header lines 1-6; the bookmarks title check at lines 142-154)
- Modify: `.github/workflows/ci.yml:39`, `.github/workflows/release.yml:68`
- Modify: `README.md` (`## Versioning`, line 111)
- Modify: `CHANGELOG.md` (`## [Unreleased]` → `### Added`)

**Interfaces:**
- Consumes: title format from Task 5; `-ExpectDev` / `-ExpectRelease` naming from Task 4.
- Produces: `smoke-test.ps1 [-ExpectDev | -ExpectRelease]`.

- [ ] **Step 1: Show the current smoke test accepts any title**

```powershell
powershell -NoProfile -File scripts\smoke-test.ps1 -ExpectRelease; "exit=$LASTEXITCODE"
```
Expected: the script ignores the unknown switch (no `param` block) and passes on a dev build — i.e. it cannot tell a dev exe from a release exe. Record that it exits 0.

- [ ] **Step 2: Add the parameters**

In `scripts/smoke-test.ps1`, after the line `# Exits non-zero on any failure; CI uses this as the final gate.`, insert:
```powershell
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
```
After the line `$ErrorActionPreference = "Stop"`, insert:
```powershell
if ($ExpectDev -and $ExpectRelease) { throw "-ExpectDev and -ExpectRelease are mutually exclusive" }
```

- [ ] **Step 3: Replace the title assertion**

Replace
```powershell
# Title is set in MainWindow WM_USER_OPEN_OK to "LitePDF - <filename>", so it
# takes a moment past the bare window creation for the title to update.
Start-Sleep -Seconds 1
$proc2.Refresh()
$title2 = $proc2.MainWindowTitle
if ($title2 -notmatch "bookmarks") {
    Stop-Process -Id $proc2.Id -Force -ErrorAction SilentlyContinue
    throw "bookmarks.pdf window title did not contain 'bookmarks': '$title2'"
}
```
with
```powershell
# Title is set in MainWindow WM_USER_OPEN_OK to "LitePDF[ <ver>-dev] <U+2014> <label>",
# so it takes a moment past the bare window creation for the title to update.
# The dash is built from its code point: this file has no BOM, and Windows
# PowerShell 5.1 on a non-UTF-8 code page would misread a literal em dash.
Start-Sleep -Seconds 1
$proc2.Refresh()
$title2 = $proc2.MainWindowTitle
$dash = [string][char]0x2014
if ($ExpectDev) {
    $verTriple = ((Get-Content (Join-Path $repoRoot "VERSION") -Raw).Trim()) -replace '-.*$', ''
    $titlePattern = "^LitePDF " + [regex]::Escape($verTriple) + "-dev " + $dash + " bookmarks"
} elseif ($ExpectRelease) {
    $titlePattern = "^LitePDF " + $dash + " bookmarks"
} else {
    $titlePattern = "bookmarks"
}
if ($title2 -notmatch $titlePattern) {
    Stop-Process -Id $proc2.Id -Force -ErrorAction SilentlyContinue
    throw "bookmarks.pdf window title '$title2' did not match '$titlePattern'"
}
```

- [ ] **Step 4: Wire the workflows**

`.github/workflows/ci.yml` line 39: `run: ./scripts/smoke-test.ps1` → `run: ./scripts/smoke-test.ps1 -ExpectDev`

`.github/workflows/release.yml` line 68: `run: ./scripts/smoke-test.ps1` → `run: ./scripts/smoke-test.ps1 -ExpectRelease`

- [ ] **Step 5: Verify both directions**

```powershell
powershell -NoProfile -File scripts\smoke-test.ps1 -ExpectDev; "exit=$LASTEXITCODE"
powershell -NoProfile -File scripts\smoke-test.ps1 -ExpectRelease; "exit=$LASTEXITCODE"
powershell -NoProfile -File scripts\smoke-test.ps1; "exit=$LASTEXITCODE"
```
Expected: `-ExpectDev` passes (exit 0, `[OK] bookmarks.pdf window title: LitePDF 1.3.0-dev — bookmarks.pdf`); `-ExpectRelease` FAILS with `did not match '^LitePDF — bookmarks'` (this is a dev build — proof the check can fail); no switch passes. Then confirm no `litepdf` process is left: `Get-Process litepdf -ErrorAction SilentlyContinue` prints nothing.

Also confirm ASCII-only code lines in both edited scripts:
```bash
LC_ALL=C grep -n '[^[:print:][:space:]]' scripts/smoke-test.ps1 scripts/check-version-sync.ps1 scripts/test-collect-build-facts.ps1 | grep -v '^[^:]*:[0-9]*:[[:space:]]*#'
```
Expected: no output.

- [ ] **Step 6: Docs**

`README.md`, replace the paragraph under `## Versioning` with:
```markdown
`VERSION` holds the numeric release triple (it may carry a `-dev` or `-rc1` suffix; the CMake build strips it before `project(VERSION ...)`, which requires numbers).

Which build is running is decided from git on every build (#60). A build is a **release** only when the source tree is exactly on a tag with no local modifications; everything else is a **dev** build. Dev builds show the version in the title bar (`LitePDF 1.3.0-dev — file.pdf`); release builds do not. Help → About always shows the version and, when git knows it, the build (`Build: v1.3.0-18-gf7b1ed6`). `litepdf-cli --version` prints the same, as `key=value` lines. A build from a source archive without `.git` is a dev build with no build id.
```

`CHANGELOG.md`, add as the first bullet under `## [Unreleased]` → `### Added`:
```markdown
- The running build is identifiable (#60). Development builds name their version
  in the title bar (`LitePDF 1.3.0-dev — file.pdf`); release builds are unchanged.
  Help → About shows the build (for example `Build: v1.3.0-18-gf7b1ed6`) and a
  link to the Releases page, `litepdf-cli --version` prints the same, and crash
  dumps carry the version in their file name.
```

- [ ] **Step 7: Full verification and commit**

```powershell
& $ctest --test-dir build -C Release
powershell -NoProfile -File scripts\check-version-sync.ps1 -ExpectDev; "exit=$LASTEXITCODE"
```
Expected: all tests pass (baseline + 12: 10 `Version:` cases, `version_script_selftest`, `version_sync_selftest`); gate exit 0.
```bash
git add scripts/smoke-test.ps1 .github/workflows/ci.yml .github/workflows/release.yml README.md CHANGELOG.md
git commit -m "test(smoke): assert the title's build marker in CI and release (#60)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```
