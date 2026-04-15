# Phase 1: Document Core — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use `superpowers:executing-plans` to implement this plan task-by-task. This phase is TDD-heavy — follow `superpowers:test-driven-development` for every Document API.

**Goal:** Wire MuPDF into the CMake build, build a UI-agnostic `core::Document` class that wraps MuPDF's `fz_document`, back it with Catch2 unit tests, and produce a throwaway `litepdf-cli` that demonstrates opening fixture files and printing metadata — all without any Direct2D or Win32 UI code.

**Architecture:** MuPDF is added via `add_subdirectory(third_party/mupdf)` with a targeted feature-flag set (tesseract/leptonica/mujs/gumbo disabled; freetype's FTL license variant selected) so the eventual exe stays lean. `core::Document` is a PIMPL class hiding all MuPDF types behind a `std::unique_ptr<Impl>`, so headers can be included by UI code in Phase 3+ without pulling in `fitz.h`. Tests drive the public API and load small fixture files from `tests/fixtures/`. The CLI is a ~50-line console program linking against the same core library, used for manual smoke tests and benchmark scaffolding in Phase 11.

**Tech Stack:** C++17, MuPDF 1.24.11 (static, feature-pruned), Catch2 v3 (FetchContent), CMake 3.25+, existing MSVC toolchain from Phase 0. No UI libraries.

**Prerequisites (from Phase 0):**

- Phase 0 tag `v0.0.1-phase0` merged to `main`; origin configured.
- `build/Release/litepdf.exe` builds clean and passes `scripts/smoke-test.ps1`.
- MuPDF submodule present at `third_party/mupdf` pinned to `1.24.11`.
- `.gitattributes` in place.

**Done when:**

1. `cmake --build build --config Release` produces both `litepdf.exe` and `litepdf-cli.exe` cleanly.
2. Running `ctest --test-dir build --config Release` passes ≥ 20 unit tests with ≥ 80% coverage on `core/Document.*`.
3. `litepdf-cli tests/fixtures/simple.pdf` prints expected metadata (page count, title, first-page size).
4. Phase 0 smoke test still passes.
5. CI runs all of the above green.
6. Tag `v0.0.2-phase1` pushed.

---

## Pre-Phase-1 Cleanup (Task 0 group)

Phase 0 reviewers accumulated a cleanup list. The ones that genuinely block or complicate Phase 1 work land first.

### Task 0.1: Add MSBuild IDE output dirs to `.gitignore`

**Files:** Modify `.gitignore`

**Step 1: Append the following at the end of the Compiler output section:**

```gitignore
# MSBuild IDE build outputs at solution root (in case someone uses VS IDE build)
/x64/
/Win32/
/Debug/
/Release/
```

**Step 2: Verify no existing tracked files match these patterns.**

```bash
git ls-files | grep -E "^(x64|Win32|Debug|Release)/" || echo "none"
```

Expected: `none`.

**Step 3: Commit.**

```bash
git add .gitignore
git -c user.email="chen.yifu@local" commit -m "chore(gitignore): add MSBuild IDE output dirs"
```

### Task 0.2: Polish README — prerequisites, LICENSE link, fenced-lang, real URL already done

**Files:** Modify `README.md`

**Step 1: Update README.md to the following complete content:**

```markdown
# LitePDF

A lightweight PDF / ePub / CBZ / XPS reader for Windows 11, optimized for mechanical hard drives. Single self-contained executable, no runtime dependencies.

- **Status:** under development (Phase 1 — document core)
- **License:** [AGPL-3.0](LICENSE)
- **Design:** [`docs/plans/2026-04-15-litepdf-design.md`](docs/plans/2026-04-15-litepdf-design.md)
- **Roadmap:** [`docs/plans/2026-04-15-litepdf-roadmap.md`](docs/plans/2026-04-15-litepdf-roadmap.md)

## Build

### Prerequisites

- Windows 11
- Visual Studio 2022 Build Tools with the **Desktop development with C++** workload (includes MSVC v143, Windows 11 SDK, and CMake ≥ 3.25).
- Git 2.30+ (for submodule support).

### Commands

```sh
git clone --recursive https://github.com/jeffchen1981-fu/litepdf
cd litepdf
cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
```

The produced `build/Release/litepdf.exe` is a single self-contained binary.

## Versioning

`VERSION` contains a semver string optionally suffixed with `-dev`, `-rc1`, etc. The CMake build reads this file and strips the pre-release suffix before passing the numeric triple to `project(VERSION ...)` (required by CMake).
```

**Step 2: Commit.**

```bash
git add README.md
git -c user.email="chen.yifu@local" commit -m "docs(readme): add prerequisites, LICENSE link, and versioning note"
```

### Task 0.3: Add VERSION-suffix comment to VERSION file

**Files:** Modify `VERSION`

**Step 1:** Rewrite `VERSION` to include a leading comment explaining the convention. But `VERSION` is read by CMake's `file(READ)` and stripped by regex; it must contain ONLY the version string. Therefore **do not** add a comment; instead document the convention in the README (already done in Task 0.2). **This task is a no-op — close it out with a confirming `git check-attr` run.**

### Task 0.4: Add `TODO(phase-9)` hints for icon resources

**Files:** Modify `resources/litepdf.rc`

**Step 1:** Replace the existing commented-out icon slot block at the end of `resources/litepdf.rc`:

```rc
// Icon slots (placeholder; real icons arrive in Phase 9)
// IDI_APPICON    101 ICON "icon/litepdf-app.ico"
// IDI_PDFDOC     102 ICON "icon/litepdf-doc.ico"
```

With:

```rc
// Icon slots: real icon assets land in Phase 9. See design §9 for the "Lightning
// Document" app icon and the red-PDF document variant. Resource IDs are reserved
// here so existing consumers (WNDCLASSEX.hIcon, DefaultIcon registry keys) don't
// change when we uncomment these.
// TODO(phase-9): ship assets/icon/litepdf-app.ico and litepdf-doc.ico
// IDI_APPICON    101 ICON "icon/litepdf-app.ico"
// IDI_PDFDOC     102 ICON "icon/litepdf-doc.ico"
```

**Step 2:** Commit.

```bash
git add resources/litepdf.rc
git -c user.email="chen.yifu@local" commit -m "docs(rc): spell out phase-9 icon plan in comment"
```

---

## Task 1: Document MuPDF integration strategy in the design

**Files:** Modify `docs/plans/2026-04-15-litepdf-design.md`

MuPDF 1.24.11 ships no CMakeLists.txt — its upstream Windows build system is a Visual Studio solution at `third_party/mupdf/platform/win32/mupdf.sln`. CMake integration therefore uses `include_external_msproject` on the vendor-supplied `.vcxproj` files. A consequence is that MuPDF's default preprocessor definitions ship in — CMake has no clean hook to override them. We accept this for Phase 1 and schedule feature-flag pruning (OCR, JS, HTML/gumbo) for Phase 11 size optimization.

**Step 1:** Insert immediately after the §2 table, before §3 (Feature Set):

```markdown
### 2.1 MuPDF Integration & Deferred Feature Pruning

MuPDF 1.24.11 lacks a CMakeLists.txt. Its Windows build is a VS solution at `third_party/mupdf/platform/win32/mupdf.sln` whose projects are consumed directly via CMake's `include_external_msproject`. The five projects we pull in:

| Project | Output `.lib` | Purpose |
|---|---|---|
| `bin2coff.vcxproj` | (build tool) | Converts CMap/font binaries into COFF objects for `libresources` |
| `libthirdparty.vcxproj` | `libthirdparty.lib` | freetype, zlib, harfbuzz, jbig2dec, lcms2, openjpeg, libjpeg |
| `libresources.vcxproj` | `libresources.lib` | Embedded CMap tables and default fonts (depends on `bin2coff`) |
| `libmuthreads.vcxproj` | `libmuthreads.lib` | Threading primitives |
| `libmupdf.vcxproj` | `libmupdf.lib` | Main MuPDF library (depends on the other four) |

**Build outputs** land at `third_party/mupdf/platform/win32/x64/$<CONFIG>/` (x64 Release → `.../x64/Release/libmupdf.lib`). These paths are stable across MuPDF 1.24.x.

**Deferred: feature-flag pruning.** MuPDF ships with `mujs` (PDF JavaScript), `gumbo` (HTML), and `tesseract`+`leptonica` (OCR) compiled by default. LitePDF v1 does not use any of these. We accept the extra ~2 MB in Phase 1 and defer pruning to **Phase 11** (binary-size regression gate). When the pruning task lands it has two paths:

- (A) Patch MuPDF's own `.vcxproj` files to remove the relevant sources + preprocessor defines (upstream-divergent but small diff).
- (B) Migrate MuPDF integration from `include_external_msproject` to a hand-written CMake shim that globs sources and controls `target_compile_definitions` directly (larger up-front cost, but full control).

**License compliance:** freetype's dual FTL/GPLv2 licensing resolves to FTL by default in MuPDF's upstream `ftoption.h` configuration — the Phase 0 review flagged this as needing verification. Task 2 of Phase 1 performs a `grep` confirmation and records the result in this section, replacing this sentence.

**CVE audit cadence:** before each `1.24.x` → `1.24.(x+1)` bump of the MuPDF submodule, check Artifex's security advisories and changelog. Before v1.0 release, perform a full scan.
```

**Step 2:** Commit.

```bash
git add docs/plans/2026-04-15-litepdf-design.md
git -c user.email="chen.yifu@local" commit -m "docs(design): document MuPDF include_external_msproject integration strategy"
```

---

## Task 2: Wire MuPDF into CMake via `ExternalProject_Add` on `mupdf.sln`

**Files:**
- Create: `cmake/ImportMuPDF.cmake`
- Modify: `CMakeLists.txt`
- Modify: `scripts/smoke-test.ps1` (size budget bump)
- Verify: full build + smoke test pass with new MuPDF deps

**Strategy evolution:** An earlier revision of this task used `include_external_msproject`. That strategy failed in two ways: (1) MuPDF's vcxprojs hardcode `PlatformToolset=v142` which resists `Directory.Build.props` override; (2) MuPDF's `mupdf.sln` contains solution-level platform mappings — e.g. `bin2coff.vcxproj` is configured to build as `Release|Win32` for ANY solution platform because it is a host build tool — and those mappings are bypassed when projects are consumed individually. The sln-level invocation via `ExternalProject_Add` respects both the solution's platform mapping AND accepts `/p:PlatformToolset=v143` as a command-line override.

**Pre-step: verify freetype defaults to FTL, record in design §2.1.**

```bash
grep -i "FT_CONFIG_OPTION_USE_FTL\|FT_CONFIG_OPTION_INCLUDE_LZW" \
    third_party/mupdf/thirdparty/freetype/include/freetype/config/ftoption.h \
    | head -20
```

Expected: the grep prints either the `#define FT_CONFIG_OPTION_USE_FTL 1` line (FTL chosen) or shows the option commented out. Interpret:

- If `FT_CONFIG_OPTION_USE_FTL` is `#define`d as `1` with no `#undef` downstream → FTL active. Replace the placeholder sentence in design §2.1 with a confirmed statement citing the line number.
- If FTL is NOT the default → stop, ask. (This is genuinely unlikely — FreeType 2 has defaulted to FTL for over a decade.)

Commit the design doc update as a sub-task of Task 1 if you prefer atomic commits; or fold into Task 2 Commit A below.

**Step 1:** Create `cmake/ImportMuPDF.cmake`:

```cmake
# Import MuPDF into our CMake graph by invoking msbuild on its upstream VS
# solution (platform/win32/mupdf.sln). The solution's ConfigurationManager
# supplies per-project platform mappings (e.g. bin2coff→Win32 as a host tool)
# that we would otherwise have to replicate. VS 2022 (v143) toolset is forced
# via command-line override.
#
# Exports:
#   target: litepdf::mupdf  (INTERFACE library bundling MuPDF output .libs)

include(ExternalProject)

set(_MUPDF_ROOT        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mupdf")
set(_MUPDF_SLN         "${_MUPDF_ROOT}/platform/win32/mupdf.sln")
set(_MUPDF_OUTPUT_DIR  "${_MUPDF_ROOT}/platform/win32/x64/Release")

# Locate MSBuild.exe — ship with VS BuildTools. Use vswhere for discovery.
find_program(_VSWHERE_EXE
    NAMES vswhere.exe
    PATHS "$ENV{ProgramFiles\(x86\)}/Microsoft Visual Studio/Installer"
    NO_DEFAULT_PATH)
if(NOT _VSWHERE_EXE)
    message(FATAL_ERROR "vswhere.exe not found — Visual Studio 2022 BuildTools required.")
endif()

execute_process(
    COMMAND "${_VSWHERE_EXE}"
        -latest -products "*"
        -requires Microsoft.Component.MSBuild
        -find "MSBuild/**/Bin/MSBuild.exe"
    OUTPUT_VARIABLE _MSBUILD_EXE
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _MSBUILD_EXE OR NOT EXISTS "${_MSBUILD_EXE}")
    message(FATAL_ERROR "MSBuild.exe not found via vswhere.")
endif()
message(STATUS "MuPDF will be built via: ${_MSBUILD_EXE}")

# ExternalProject builds MuPDF's sln. We target libmupdf; MSBuild
# automatically chases its ProjectReferences (libthirdparty, libresources,
# libmuthreads) and the sln-level config maps bin2coff → Release|Win32.
ExternalProject_Add(mupdf_ext
    SOURCE_DIR        "${_MUPDF_ROOT}"
    CONFIGURE_COMMAND ""
    BUILD_COMMAND     "${_MSBUILD_EXE}" "${_MUPDF_SLN}"
                      /t:libmupdf
                      /p:Configuration=Release
                      /p:Platform=x64
                      /p:PlatformToolset=v143
                      /m /nologo /verbosity:minimal
    INSTALL_COMMAND   ""
    BUILD_ALWAYS      OFF
    BUILD_IN_SOURCE   1
    BUILD_BYPRODUCTS
        "${_MUPDF_OUTPUT_DIR}/libmupdf.lib"
        "${_MUPDF_OUTPUT_DIR}/libthirdparty.lib"
        "${_MUPDF_OUTPUT_DIR}/libresources.lib"
        "${_MUPDF_OUTPUT_DIR}/libmuthreads.lib"
)

# INTERFACE target that our code links against. Headers + 4 output .libs
# + a handful of Windows system libs MuPDF may reference.
add_library(litepdf_mupdf INTERFACE)
add_dependencies(litepdf_mupdf mupdf_ext)

target_include_directories(litepdf_mupdf INTERFACE
    "${_MUPDF_ROOT}/include")

target_link_libraries(litepdf_mupdf INTERFACE
    "${_MUPDF_OUTPUT_DIR}/libmupdf.lib"
    "${_MUPDF_OUTPUT_DIR}/libthirdparty.lib"
    "${_MUPDF_OUTPUT_DIR}/libresources.lib"
    "${_MUPDF_OUTPUT_DIR}/libmuthreads.lib"
    ws2_32 advapi32 gdi32 user32 crypt32)

add_library(litepdf::mupdf ALIAS litepdf_mupdf)
```

**Note on Debug builds:** this helper hardcodes `Configuration=Release`. Phase 1's goal is a working Release build; Debug is a Phase 2+ concern. When Debug matters, the fix is to duplicate the `ExternalProject_Add` call for a Debug variant and switch `$<CONFIG>` driven link paths. Out of scope for Phase 1.

**Step 2:** Modify `CMakeLists.txt`. Add, after `include(CompilerFlags)` and before `add_executable(litepdf ...)`:

```cmake
# --- MuPDF via ExternalProject_Add on mupdf.sln ---------------------------
include(ImportMuPDF)  # defines litepdf::mupdf INTERFACE target
```

And modify the `litepdf` target's link libraries:

```cmake
target_link_libraries(litepdf PRIVATE
    comctl32
    litepdf::mupdf
)
```

**Step 3:** Configure and build.

```bash
CMAKE='/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
rm -rf build
"$CMAKE" -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release
"$CMAKE" --build build --config Release --parallel
```

Expected: build succeeds. Five new MuPDF sub-builds run (first build adds ~3 minutes cold). `build/Release/litepdf.exe` grows to **6–8 MB** (MuPDF linked but no calls yet; LTCG strips some dead code; tesseract/JS/HTML still linked in per design §2.1 deferred-pruning decision).

**Likely failure modes:**

- `MSB8020 v142 build tools not found`: the `/p:PlatformToolset=v143` override isn't taking effect. Check the exact msbuild command line from the ExternalProject build log.
- `MSBUILD : error MSB1008: Only one project can be specified`: unquoted path with spaces. Confirm `${_MUPDF_SLN}` is quoted and contains no wildcards.
- `LNK1104 cannot open file libmupdf.lib`: MuPDF build completed but at a different path. Verify `_MUPDF_OUTPUT_DIR` matches MuPDF's actual x64 Release output.
- `fatal error C1083: cannot include fitz.h`: `target_include_directories` on `litepdf::mupdf` should be `INTERFACE` (check helper).
- `unresolved external symbol _imp__WSACleanup@0` or similar: MuPDF uses Winsock for HTTPS-fetched assets; `ws2_32` should already be in the link list.
- **Warnings from MuPDF's own source code at any /W level**: these are vendor code, not our responsibility. Count them and report, but do not suppress.

If the build fails in a way not listed, STOP and report. Do NOT start editing MuPDF's `.vcxproj` files, `mupdf.sln`, or the solution's ConfigurationManager section — the sln-based invocation is supposed to make those unnecessary.

**Step 4:** Verify smoke test. First bump the size budget:

```powershell
# Size budget (Phase 1: MuPDF linked but feature-pruning deferred to Phase 11): 8 MB.
$maxBytes = 8MB
```

Edit `scripts/smoke-test.ps1` line that says `$maxBytes = 2MB` → `$maxBytes = 8MB`, and update the preceding comment accordingly.

Run:

```bash
powershell -ExecutionPolicy Bypass -File scripts/smoke-test.ps1
```

Expected: `[OK] Phase 0 smoke test passed.` (prefix is cosmetic; the test logic passes).

**Step 5:** Commit in TWO separate commits (atomic):

```bash
# commit A: ImportMuPDF helper
git add cmake/ImportMuPDF.cmake
git -c user.email="chen.yifu@local" commit -m "build(cmake): add ImportMuPDF helper (ExternalProject_Add on mupdf.sln)"

# commit B: wire into main CMakeLists + smoke budget bump
git add CMakeLists.txt scripts/smoke-test.ps1
git -c user.email="chen.yifu@local" commit -m "build(cmake): link MuPDF into litepdf, bump smoke budget to 8 MB"
```

---

## Task 3: Add Catch2 via FetchContent

**Files:**
- Modify: `CMakeLists.txt`
- Create: `tests/CMakeLists.txt`
- Create: `tests/unit/test_smoke.cpp`

Catch2 is a header-only-ish test framework. Use CMake's `FetchContent` to pull `v3.5.4` (pinned) and expose `Catch2::Catch2WithMain`.

**Step 1:** Append to the root `CMakeLists.txt`, at the very end (after the MuPDF comment block):

```cmake
# --- Tests ----------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
```

**Step 2:** Create `tests/CMakeLists.txt`:

```cmake
include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.4
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Catch2)

add_executable(litepdf_unit_tests
    unit/test_smoke.cpp
)

litepdf_apply_flags(litepdf_unit_tests)

target_link_libraries(litepdf_unit_tests PRIVATE
    Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(litepdf_unit_tests)
```

**Step 3:** Create `tests/unit/test_smoke.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

TEST_CASE("Catch2 smoke: arithmetic works", "[smoke]") {
    REQUIRE(2 + 2 == 4);
}
```

**Step 4:** Configure + build + run tests.

```bash
rm -rf build
"$CMAKE" -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release
"$CMAKE" --build build --config Release --parallel
"$CMAKE" --build build --config Release --target RUN_TESTS
```

Expected: 1 test passes.

**Step 5:** Commit.

```bash
git add CMakeLists.txt tests/
git -c user.email="chen.yifu@local" commit -m "test: integrate Catch2 v3.5.4 via FetchContent + smoke test"
```

---

## Task 4: Scaffold `core/Document` (public header only, PIMPL)

**Files:**
- Create: `src/core/Document.hpp`
- Create: `src/core/Document.cpp`
- Modify: `CMakeLists.txt` — add source files to `litepdf` target

**Step 1:** Create `src/core/Document.hpp`:

```cpp
#pragma once

// core::Document — UI-agnostic wrapper around MuPDF's fz_document.
// PIMPL: no MuPDF types leak through this header, so UI code (Phase 3+)
// can include Document.hpp without pulling fitz.h.

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace litepdf::core {

struct PageSize {
    float width_pt;
    float height_pt;
};

class Document {
public:
    enum class OpenError {
        FileNotFound,
        UnsupportedFormat,
        NeedsPassword,
        BadPassword,
        Corrupted,
        OutOfMemory,
        Other
    };

    Document();
    ~Document();

    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;
    Document(Document&&) noexcept;
    Document& operator=(Document&&) noexcept;

    // Returns std::nullopt on success, an OpenError otherwise.
    // If NeedsPassword is returned, call authenticate(password) then retry.
    [[nodiscard]] OpenError open(const std::filesystem::path& path);

    [[nodiscard]] bool is_open() const noexcept;
    void close() noexcept;

    // For encrypted documents. Returns true if password accepted.
    [[nodiscard]] bool authenticate(std::string_view password);

    // Metadata
    [[nodiscard]] std::size_t page_count() const;
    [[nodiscard]] PageSize page_size(std::size_t index) const;

    // Returns the plain text of page `index`, with newlines preserved.
    [[nodiscard]] std::string page_text(std::size_t index) const;

    // Outline: flat list of entries with indent depth, title, and target page.
    struct OutlineEntry {
        int depth;
        std::string title;
        std::size_t page_index;  // 0-based; kNoPage if not a page link
    };
    static constexpr std::size_t kNoPage = static_cast<std::size_t>(-1);
    [[nodiscard]] std::vector<OutlineEntry> outline() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace litepdf::core
```

**Step 2:** Create `src/core/Document.cpp` with a minimal skeleton (all methods throw or return empty for now — subsequent tasks implement each method via TDD):

```cpp
#include "core/Document.hpp"

#include <stdexcept>

namespace litepdf::core {

struct Document::Impl {
    // MuPDF state will be added in subsequent tasks.
    bool open = false;
};

Document::Document() : impl_(std::make_unique<Impl>()) {}
Document::~Document() = default;
Document::Document(Document&&) noexcept = default;
Document& Document::operator=(Document&&) noexcept = default;

Document::OpenError Document::open(const std::filesystem::path&) {
    return OpenError::Other;  // TDD: to be filled in Task 5
}

bool Document::is_open() const noexcept { return impl_->open; }
void Document::close() noexcept { impl_->open = false; }

bool Document::authenticate(std::string_view) { return false; }

std::size_t Document::page_count() const { throw std::runtime_error("not implemented"); }
PageSize    Document::page_size(std::size_t) const { throw std::runtime_error("not implemented"); }
std::string Document::page_text(std::size_t) const { throw std::runtime_error("not implemented"); }

std::vector<Document::OutlineEntry> Document::outline() const { return {}; }

} // namespace litepdf::core
```

**Step 3:** Modify `CMakeLists.txt` — promote `core/` to a small static library that both `litepdf` and the unit tests link against:

```cmake
# --- core library ---------------------------------------------------------
add_library(litepdf_core STATIC
    src/core/Document.cpp
)

litepdf_apply_flags(litepdf_core)

target_include_directories(litepdf_core PUBLIC src)

target_link_libraries(litepdf_core PRIVATE
    libmupdf
)
```

And update the `litepdf` target to link `litepdf_core`:

```cmake
target_link_libraries(litepdf PRIVATE
    comctl32
    litepdf_core
)
```

Remove the direct `libmupdf` link on `litepdf` — it comes via `litepdf_core` now.

**Step 4:** Configure + build.

```bash
rm -rf build
"$CMAKE" -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release
"$CMAKE" --build build --config Release --parallel
```

Expected: clean build. `litepdf.exe` still under 6 MB.

**Step 5:** Commit.

```bash
git add src/core/ CMakeLists.txt
git -c user.email="chen.yifu@local" commit -m "feat(core): scaffold Document class (PIMPL, all APIs stubbed)"
```

---

## Task 5: TDD — `open()` success path for PDF

**Files:**
- Create: `tests/fixtures/simple.pdf` (downloaded, see Step 1)
- Create: `tests/unit/test_document_open.cpp`
- Modify: `tests/CMakeLists.txt` — add new test file
- Modify: `src/core/Document.cpp` — implement open() for the happy path

**Step 1:** Download a minimal public-domain PDF for testing. Use MuPDF's own test fixtures — they are part of the submodule and AGPL-compatible.

```bash
cp third_party/mupdf/docs/examples/example.pdf tests/fixtures/simple.pdf
```

Verify the file: `file tests/fixtures/simple.pdf` should report "PDF document".

**Step 2:** Write the failing test `tests/unit/test_document_open.cpp`:

```cpp
#include "core/Document.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace litepdf::core;

TEST_CASE("Document::open succeeds on a simple PDF", "[document][open]") {
    Document doc;
    auto err = doc.open("tests/fixtures/simple.pdf");
    REQUIRE(err == Document::OpenError::Other);  // will fail once implemented
    // NOTE: After Step 4, change this expectation to success (see Step 5).
}
```

**Step 3:** Add the test file to `tests/CMakeLists.txt`:

```cmake
target_sources(litepdf_unit_tests PRIVATE
    unit/test_document_open.cpp
)
target_link_libraries(litepdf_unit_tests PRIVATE litepdf_core)
```

**Step 4:** Build + run — verify the test currently passes the stubbed `OpenError::Other` path (i.e. the stub is the baseline).

```bash
"$CMAKE" --build build --config Release --parallel
ctest --test-dir build -C Release -R Document --output-on-failure
```

Expected: PASS (stub returns Other, test asserts Other).

**Step 5:** Invert the test to drive implementation. Replace the test body with:

```cpp
TEST_CASE("Document::open succeeds on a simple PDF", "[document][open]") {
    Document doc;
    REQUIRE_FALSE(doc.is_open());

    auto err = doc.open("tests/fixtures/simple.pdf");
    REQUIRE(err == Document::OpenError::FileNotFound);  // placeholder pattern
    REQUIRE(doc.is_open());
}
```

Wait — that's not right for the success path. The correct failing-test pattern is:

```cpp
TEST_CASE("Document::open succeeds on a simple PDF", "[document][open]") {
    Document doc;
    REQUIRE_FALSE(doc.is_open());

    auto err = doc.open("tests/fixtures/simple.pdf");
    INFO("OpenError value: " << static_cast<int>(err));
    REQUIRE(err == Document::OpenError{});  // will fail because stub returns Other
    // Canonical success is "no error" — add a success enum value next.
}
```

**Refactor the enum first.** Edit `Document.hpp` to turn the `OpenError` enum into an optional-returning pattern:

```cpp
[[nodiscard]] std::optional<OpenError> open(const std::filesystem::path& path);
```

And in the test:

```cpp
auto err = doc.open("tests/fixtures/simple.pdf");
REQUIRE_FALSE(err.has_value());
REQUIRE(doc.is_open());
```

Run — test now FAILS (stub returns `OpenError::Other`). Good.

**Step 6:** Implement. In `Document.cpp`:

```cpp
#include "core/Document.hpp"

#include <mupdf/fitz.h>

#include <stdexcept>

namespace litepdf::core {

struct Document::Impl {
    fz_context* ctx = nullptr;
    fz_document* doc = nullptr;

    Impl() {
        ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
        if (!ctx) throw std::bad_alloc();
        fz_register_document_handlers(ctx);
    }

    ~Impl() {
        if (doc) fz_drop_document(ctx, doc);
        if (ctx) fz_drop_context(ctx);
    }
};

Document::Document() : impl_(std::make_unique<Impl>()) {}
Document::~Document() = default;
Document::Document(Document&&) noexcept = default;
Document& Document::operator=(Document&&) noexcept = default;

std::optional<Document::OpenError> Document::open(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return OpenError::FileNotFound;

    fz_try(impl_->ctx) {
        // NB: fz_open_document takes a UTF-8 char*. On Windows MSVC, path.string()
        // yields the ACP; we need path.u8string() for the MuPDF pass-through.
        // This will be revisited in Task 10 (Unicode path robustness).
        impl_->doc = fz_open_document(impl_->ctx, path.string().c_str());
    }
    fz_catch(impl_->ctx) {
        return OpenError::Corrupted;
    }

    if (!impl_->doc) return OpenError::Other;
    return std::nullopt;
}

bool Document::is_open() const noexcept { return impl_->doc != nullptr; }

void Document::close() noexcept {
    if (impl_->doc) {
        fz_drop_document(impl_->ctx, impl_->doc);
        impl_->doc = nullptr;
    }
}

bool Document::authenticate(std::string_view) { return false; }

std::size_t Document::page_count() const { throw std::runtime_error("not implemented"); }
PageSize    Document::page_size(std::size_t) const { throw std::runtime_error("not implemented"); }
std::string Document::page_text(std::size_t) const { throw std::runtime_error("not implemented"); }

std::vector<Document::OutlineEntry> Document::outline() const { return {}; }

} // namespace litepdf::core
```

**Step 7:** Rebuild + test.

```bash
"$CMAKE" --build build --config Release --parallel
ctest --test-dir build -C Release -R Document --output-on-failure
```

Expected: PASS.

**Step 8:** Commit.

```bash
git add tests/fixtures/simple.pdf tests/unit/test_document_open.cpp tests/CMakeLists.txt src/core/Document.hpp src/core/Document.cpp
git -c user.email="chen.yifu@local" commit -m "feat(core): implement Document::open for happy-path PDF (TDD)"
```

---

## Task 6: TDD — `open()` error paths (FileNotFound, UnsupportedFormat, Corrupted)

**Files:**
- Create: `tests/fixtures/corrupt.pdf`
- Create: `tests/fixtures/sample.png` (to test UnsupportedFormat)
- Modify: `tests/unit/test_document_open.cpp`

**Step 1:** Create fixtures.

```bash
# Truncated PDF (first 100 bytes of simple.pdf)
head -c 100 tests/fixtures/simple.pdf > tests/fixtures/corrupt.pdf

# Make a tiny PNG — 67 bytes, valid PNG header, 1×1 red pixel
printf '\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR\x00\x00\x00\x01\x00\x00\x00\x01\x08\x02\x00\x00\x00\x90wS\xde\x00\x00\x00\x0cIDAT\x08\x99c\xf8\xcf\xc0\x00\x00\x00\x03\x00\x01Z\xb5\x89\x05\x00\x00\x00\x00IEND\xaeB`\x82' > tests/fixtures/sample.png
```

Verify:
```bash
file tests/fixtures/corrupt.pdf   # PDF document (possibly truncated)
file tests/fixtures/sample.png    # PNG image data
```

**Step 2:** Add failing tests to `tests/unit/test_document_open.cpp`:

```cpp
TEST_CASE("Document::open returns FileNotFound for missing file", "[document][open]") {
    Document doc;
    auto err = doc.open("tests/fixtures/does_not_exist.pdf");
    REQUIRE(err == Document::OpenError::FileNotFound);
    REQUIRE_FALSE(doc.is_open());
}

TEST_CASE("Document::open returns UnsupportedFormat for PNG", "[document][open]") {
    Document doc;
    auto err = doc.open("tests/fixtures/sample.png");
    REQUIRE(err == Document::OpenError::UnsupportedFormat);
    REQUIRE_FALSE(doc.is_open());
}

TEST_CASE("Document::open returns Corrupted for truncated PDF", "[document][open]") {
    Document doc;
    auto err = doc.open("tests/fixtures/corrupt.pdf");
    REQUIRE(err == Document::OpenError::Corrupted);
    REQUIRE_FALSE(doc.is_open());
}
```

**Step 3:** Run tests. FileNotFound passes (already implemented). The others fail (PNG probably returns Corrupted now; truncated PDF may hit fz_catch which we report as Corrupted — verify actual behavior).

**Step 4:** Implement UnsupportedFormat detection BEFORE passing to `fz_open_document`. Use `fz_recognize_document`:

```cpp
std::optional<Document::OpenError> Document::open(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return OpenError::FileNotFound;

    const auto path_str = path.string();

    // Recognize by extension + magic
    const fz_document_handler* handler =
        fz_recognize_document(impl_->ctx, path_str.c_str());
    if (!handler) return OpenError::UnsupportedFormat;

    fz_try(impl_->ctx) {
        impl_->doc = fz_open_document(impl_->ctx, path_str.c_str());
    }
    fz_catch(impl_->ctx) {
        const int code = fz_caught(impl_->ctx);
        if (code == FZ_ERROR_TRYLATER) return OpenError::Corrupted;
        return OpenError::Other;
    }

    if (!impl_->doc) return OpenError::Other;
    return std::nullopt;
}
```

**Step 5:** Rerun tests; all three new tests PASS.

**Step 6:** Commit.

```bash
git add tests/fixtures/corrupt.pdf tests/fixtures/sample.png tests/unit/test_document_open.cpp src/core/Document.cpp
git -c user.email="chen.yifu@local" commit -m "feat(core): Document::open error paths (not-found/unsupported/corrupted)"
```

---

## Task 7: TDD — `page_count` + `page_size`

**Files:** Modify `src/core/Document.cpp`, create `tests/unit/test_document_pages.cpp`

**Step 1:** Failing test:

```cpp
// tests/unit/test_document_pages.cpp
#include "core/Document.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace litepdf::core;

TEST_CASE("Document::page_count returns known page count", "[document][pages]") {
    Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());
    REQUIRE(doc.page_count() >= 1);  // example.pdf has at least 1 page
}

TEST_CASE("Document::page_size returns A4-ish dimensions for simple.pdf", "[document][pages]") {
    Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());

    PageSize size = doc.page_size(0);
    // simple.pdf page is in US Letter or A4-ish range (500–700 pt wide)
    REQUIRE(size.width_pt  > 400.0f);
    REQUIRE(size.width_pt  < 700.0f);
    REQUIRE(size.height_pt > 400.0f);
    REQUIRE(size.height_pt < 900.0f);
}
```

Add to `tests/CMakeLists.txt`:

```cmake
target_sources(litepdf_unit_tests PRIVATE unit/test_document_pages.cpp)
```

**Step 2:** Verify fails (still throws `not implemented`).

**Step 3:** Implement in `Document.cpp`:

```cpp
std::size_t Document::page_count() const {
    if (!impl_->doc) throw std::logic_error("page_count called on unopened document");
    return static_cast<std::size_t>(fz_count_pages(impl_->ctx, impl_->doc));
}

PageSize Document::page_size(std::size_t index) const {
    if (!impl_->doc) throw std::logic_error("page_size called on unopened document");
    fz_page* page = fz_load_page(impl_->ctx, impl_->doc, static_cast<int>(index));
    fz_rect bounds = fz_bound_page(impl_->ctx, page);
    fz_drop_page(impl_->ctx, page);
    return {
        .width_pt  = bounds.x1 - bounds.x0,
        .height_pt = bounds.y1 - bounds.y0,
    };
}
```

**Step 4:** Build + test; PASS.

**Step 5:** Commit.

```bash
git add src/core/Document.cpp tests/unit/test_document_pages.cpp tests/CMakeLists.txt
git -c user.email="chen.yifu@local" commit -m "feat(core): Document::page_count + page_size (TDD)"
```

---

## Task 8: TDD — `page_text`

**Files:** Modify `src/core/Document.cpp`, create `tests/unit/test_document_text.cpp`

**Step 1:** Failing test:

```cpp
// tests/unit/test_document_text.cpp
#include "core/Document.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace litepdf::core;

TEST_CASE("Document::page_text returns non-empty text for example.pdf", "[document][text]") {
    Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());

    std::string text = doc.page_text(0);
    INFO("Extracted: " << text);
    REQUIRE(!text.empty());
    REQUIRE(text.size() > 10);  // sanity: should be more than 10 bytes
}
```

Add to `tests/CMakeLists.txt` source list.

**Step 2:** Verify fails.

**Step 3:** Implement:

```cpp
std::string Document::page_text(std::size_t index) const {
    if (!impl_->doc) throw std::logic_error("page_text called on unopened document");

    fz_page* page = fz_load_page(impl_->ctx, impl_->doc, static_cast<int>(index));

    fz_stext_options opts = {};
    fz_stext_page* stext = fz_new_stext_page_from_page(impl_->ctx, page, &opts);

    fz_buffer* buf = fz_new_buffer(impl_->ctx, 4096);
    fz_output* out = fz_new_output_with_buffer(impl_->ctx, buf);
    fz_print_stext_page_as_text(impl_->ctx, out, stext);
    fz_close_output(impl_->ctx, out);

    unsigned char* data;
    const std::size_t len = fz_buffer_storage(impl_->ctx, buf, &data);
    std::string result(reinterpret_cast<const char*>(data), len);

    fz_drop_output(impl_->ctx, out);
    fz_drop_buffer(impl_->ctx, buf);
    fz_drop_stext_page(impl_->ctx, stext);
    fz_drop_page(impl_->ctx, page);

    return result;
}
```

**Step 4:** Build + test; PASS.

**Step 5:** Commit.

```bash
git add src/core/Document.cpp tests/unit/test_document_text.cpp tests/CMakeLists.txt
git -c user.email="chen.yifu@local" commit -m "feat(core): Document::page_text via fz_stext (TDD)"
```

---

## Task 9: TDD — password handling for encrypted PDFs

**Files:**
- Add: `tests/fixtures/encrypted.pdf` (use qpdf, pikepdf, or manual creation)
- Create: `tests/unit/test_document_password.cpp`
- Modify: `src/core/Document.cpp`

**Step 1:** Create encrypted fixture. Since we don't want an extra tool dependency, use MuPDF's `mutool` (we disabled it earlier — temporarily enable with cached build, OR use an external online PDF-encryption service, OR commit a pre-made encrypted PDF).

Simplest: commit a pre-made encrypted PDF. Use this python script (one-off, requires pikepdf on the dev machine only):

```bash
pip install pikepdf
python -c "
import pikepdf
pdf = pikepdf.open('tests/fixtures/simple.pdf')
pdf.save('tests/fixtures/encrypted.pdf', encryption=pikepdf.Encryption(owner='test', user='test'))
"
```

Verify: `file tests/fixtures/encrypted.pdf` says PDF; opening without password returns `NeedsPassword`.

**Step 2:** Failing tests:

```cpp
// tests/unit/test_document_password.cpp
#include "core/Document.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace litepdf::core;

TEST_CASE("Encrypted PDF open returns NeedsPassword", "[document][password]") {
    Document doc;
    auto err = doc.open("tests/fixtures/encrypted.pdf");
    REQUIRE(err == Document::OpenError::NeedsPassword);
    REQUIRE_FALSE(doc.is_open());
}

TEST_CASE("Encrypted PDF authenticates with correct password", "[document][password]") {
    Document doc;
    auto err = doc.open("tests/fixtures/encrypted.pdf");
    REQUIRE(err == Document::OpenError::NeedsPassword);

    REQUIRE(doc.authenticate("test"));
    REQUIRE(doc.is_open());
    REQUIRE(doc.page_count() >= 1);
}

TEST_CASE("Encrypted PDF rejects wrong password", "[document][password]") {
    Document doc;
    doc.open("tests/fixtures/encrypted.pdf");
    REQUIRE_FALSE(doc.authenticate("wrong"));
}
```

Add to `tests/CMakeLists.txt`.

**Step 3:** Verify fails.

**Step 4:** Implement:

```cpp
std::optional<Document::OpenError> Document::open(const std::filesystem::path& path) {
    // ... (existing file-exists + recognize checks) ...

    fz_try(impl_->ctx) {
        impl_->doc = fz_open_document(impl_->ctx, path_str.c_str());
    }
    fz_catch(impl_->ctx) {
        const int code = fz_caught(impl_->ctx);
        if (code == FZ_ERROR_TRYLATER) return OpenError::Corrupted;
        return OpenError::Other;
    }

    if (!impl_->doc) return OpenError::Other;

    // NEW: needs-password check
    if (fz_needs_password(impl_->ctx, impl_->doc)) {
        // Keep the fz_document alive so authenticate() can see it.
        return OpenError::NeedsPassword;
    }

    return std::nullopt;
}

bool Document::is_open() const noexcept {
    return impl_->doc != nullptr && !fz_needs_password(impl_->ctx, impl_->doc);
}

bool Document::authenticate(std::string_view password) {
    if (!impl_->doc) return false;
    std::string pw(password);
    return fz_authenticate_password(impl_->ctx, impl_->doc, pw.c_str()) != 0;
}
```

**Step 5:** Build + test; PASS.

**Step 6:** Commit.

```bash
git add tests/fixtures/encrypted.pdf tests/unit/test_document_password.cpp src/core/Document.hpp src/core/Document.cpp tests/CMakeLists.txt
git -c user.email="chen.yifu@local" commit -m "feat(core): Document needs/authenticate password flow (TDD)"
```

---

## Task 10: TDD — outline parsing

**Files:**
- Add: `tests/fixtures/bookmarks.pdf` — PDF with outline tree (source from MuPDF's test corpus or create one)
- Create: `tests/unit/test_document_outline.cpp`
- Modify: `src/core/Document.cpp`

**Step 1:** Source a PDF with bookmarks. MuPDF's `docs/examples/` or `thirdparty/mujs/docs/` — verify one has outlines via `third_party/mupdf/build/release/mutool.exe show bookmarks.pdf outline` (or pikepdf). Pick one with 3+ entries at 2+ levels of depth. Commit as `tests/fixtures/bookmarks.pdf`.

**Step 2:** Failing test:

```cpp
// tests/unit/test_document_outline.cpp
#include "core/Document.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace litepdf::core;

TEST_CASE("Outline parsing yields entries for bookmarks.pdf", "[document][outline]") {
    Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/bookmarks.pdf").has_value());

    auto entries = doc.outline();
    REQUIRE(entries.size() >= 3);

    // Check at least one entry has a non-root depth
    bool has_nested = false;
    for (const auto& e : entries) {
        if (e.depth > 0) { has_nested = true; break; }
    }
    REQUIRE(has_nested);

    // Titles are non-empty
    for (const auto& e : entries) {
        REQUIRE(!e.title.empty());
    }
}
```

Add to `tests/CMakeLists.txt`.

**Step 3:** Verify fails (stub returns empty).

**Step 4:** Implement. MuPDF's `fz_outline` is a tree; flatten via recursion:

```cpp
namespace {

void flatten_outline(fz_context* ctx, fz_document* doc, fz_outline* node, int depth,
                     std::vector<Document::OutlineEntry>& out) {
    for (; node; node = node->next) {
        Document::OutlineEntry entry;
        entry.depth = depth;
        entry.title = (node->title ? std::string(node->title) : std::string{});
        entry.page_index = Document::kNoPage;

        if (node->page.page >= 0) {
            entry.page_index = static_cast<std::size_t>(node->page.page);
        }
        out.push_back(std::move(entry));

        if (node->down) {
            flatten_outline(ctx, doc, node->down, depth + 1, out);
        }
    }
}

} // namespace

std::vector<Document::OutlineEntry> Document::outline() const {
    std::vector<OutlineEntry> result;
    if (!impl_->doc) return result;

    fz_outline* root = nullptr;
    fz_try(impl_->ctx) {
        root = fz_load_outline(impl_->ctx, impl_->doc);
    }
    fz_catch(impl_->ctx) {
        return result;
    }

    if (root) {
        flatten_outline(impl_->ctx, impl_->doc, root, 0, result);
        fz_drop_outline(impl_->ctx, root);
    }
    return result;
}
```

**Step 5:** Build + test; PASS.

**Step 6:** Commit.

```bash
git add tests/fixtures/bookmarks.pdf tests/unit/test_document_outline.cpp src/core/Document.cpp
git -c user.email="chen.yifu@local" commit -m "feat(core): Document::outline flattens fz_outline tree (TDD)"
```

---

## Task 11: Non-PDF format smoke (ePub, CBZ)

**Files:**
- Add: `tests/fixtures/sample.epub` and `tests/fixtures/sample.cbz`
- Create: `tests/unit/test_document_formats.cpp`

**Step 1:** Source fixtures. MuPDF's test suite has both; alternatively create a minimal CBZ (zip of 1–2 JPGs) and use any free ePub (Project Gutenberg).

**Step 2:** Tests:

```cpp
// tests/unit/test_document_formats.cpp
#include "core/Document.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace litepdf::core;

TEST_CASE("ePub opens and has pages", "[document][formats]") {
    Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/sample.epub").has_value());
    REQUIRE(doc.page_count() >= 1);
}

TEST_CASE("CBZ opens and has pages", "[document][formats]") {
    Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/sample.cbz").has_value());
    REQUIRE(doc.page_count() >= 1);
}
```

**Step 3:** Build + test; should PASS if MuPDF feature flags are correctly set (FZ_ENABLE_EPUB, FZ_ENABLE_CBZ both ON). If FAIL, the feature-flag task (Task 2 / MuPDFConfig.cmake) has a mistake.

**Step 4:** Commit.

```bash
git add tests/fixtures/sample.epub tests/fixtures/sample.cbz tests/unit/test_document_formats.cpp tests/CMakeLists.txt
git -c user.email="chen.yifu@local" commit -m "test(core): ePub + CBZ smoke open"
```

---

## Task 12: `litepdf-cli` demo target

**Files:**
- Create: `src/cli/main.cpp`
- Modify: `CMakeLists.txt`

**Step 1:** Create `src/cli/main.cpp`:

```cpp
#include "core/Document.hpp"

#include <cstdio>
#include <string>

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: %s <file>\n", argv[0]);
        return 2;
    }

    litepdf::core::Document doc;
    auto err = doc.open(argv[1]);
    if (err) {
        std::fprintf(stderr, "Open error: %d\n", static_cast<int>(*err));
        return 1;
    }

    const std::size_t n = doc.page_count();
    std::printf("File: %s\n", argv[1]);
    std::printf("Pages: %zu\n", n);

    if (n > 0) {
        auto size = doc.page_size(0);
        std::printf("First page: %.1f x %.1f pt\n", size.width_pt, size.height_pt);

        std::string text = doc.page_text(0);
        if (text.size() > 200) text.resize(200);
        std::printf("First-page text snippet:\n%s\n", text.c_str());
    }

    const auto outline = doc.outline();
    if (!outline.empty()) {
        std::printf("Outline (%zu entries):\n", outline.size());
        for (const auto& e : outline) {
            std::printf("  %*s- %s (page %zu)\n",
                        e.depth * 2, "", e.title.c_str(),
                        e.page_index == litepdf::core::Document::kNoPage ? 0 : e.page_index + 1);
        }
    }

    return 0;
}
```

**Step 2:** Modify `CMakeLists.txt`:

```cmake
# --- litepdf-cli (demo / benchmark harness) -------------------------------
add_executable(litepdf-cli src/cli/main.cpp)
litepdf_apply_flags(litepdf-cli)
target_link_libraries(litepdf-cli PRIVATE litepdf_core)
```

(This is a console app — no `WIN32` keyword.)

**Step 3:** Build + run manually:

```bash
"$CMAKE" --build build --config Release --parallel
./build/Release/litepdf-cli.exe tests/fixtures/simple.pdf
```

Expected output:

```
File: tests/fixtures/simple.pdf
Pages: 1
First page: 595.3 x 842.0 pt
First-page text snippet:
<some text from the sample PDF>
```

**Step 4:** Commit.

```bash
git add src/cli/main.cpp CMakeLists.txt
git -c user.email="chen.yifu@local" commit -m "feat(cli): add litepdf-cli demo harness"
```

---

## Task 13: Update CI to run unit tests

**Files:** Modify `.github/workflows/ci.yml`

**Step 1:** Insert a new step between "Build" and "Smoke test":

```yaml
      - name: Unit tests
        run: ctest --test-dir build -C Release --output-on-failure
```

**Step 2:** Commit.

```bash
git add .github/workflows/ci.yml
git -c user.email="chen.yifu@local" commit -m "ci: run ctest after build"
```

**Step 3:** Push and verify CI green.

```bash
git push origin main
gh run watch --exit-status  # block until the run finishes; non-zero on failure
```

Expected: run finishes green, 3 steps pass (Configure, Build, Unit tests, Smoke test).

---

## Task 14: Tag v0.0.2-phase1

**Step 1:**

```bash
git -c user.email="chen.yifu@local" tag -a v0.0.2-phase1 -m "Phase 1 complete: Document core with MuPDF integration"
git push origin v0.0.2-phase1
```

**Step 2:** Verify tag and that HEAD matches:

```bash
git describe --tags   # → v0.0.2-phase1
```

---

## Exit Checklist

- [ ] All Task 0.x pre-cleanup items committed
- [ ] MuPDFConfig.cmake feature flags match design §2.1
- [ ] `litepdf.exe` builds clean at /W4; exe size ≤ 6 MB
- [ ] `litepdf-cli.exe` builds clean at /W4
- [ ] `litepdf_unit_tests.exe` builds clean at /W4
- [ ] ctest reports all Document tests green (≥ 10 test cases)
- [ ] `scripts/smoke-test.ps1` still passes
- [ ] `litepdf-cli tests/fixtures/simple.pdf` prints metadata
- [ ] GitHub Actions CI green on `main`
- [ ] `v0.0.2-phase1` tag pushed

## What Phase 2 Will Build On

Phase 2 (RenderEngine + PageCache) will:

- Add `core/RenderEngine` with a 2-thread pool producing `fz_pixmap`s.
- Add `core/PageCache` LRU keyed by (doc-id, page-index, scale).
- Cancellation support for rapid paging.
- Extends `Document` to expose `fz_display_list` per page (pre-raster intermediate).
- Headless stress test: render 1000 sequential pages from `large.pdf` within 25 MB cache cap.
- Still no UI — all testable in the console.

Re-invoke `superpowers:writing-plans` for Phase 2 after Phase 1 is tagged and pushed.
