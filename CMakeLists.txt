cmake_minimum_required(VERSION 3.25)

# Read version from VERSION file (single source of truth).
# Strip any pre-release suffix (e.g. "-dev", "-rc1") because CMake's
# project(VERSION ...) only accepts numeric major.minor.patch[.tweak].
# Re-run CMake configure whenever VERSION changes, so the generated
# litepdf.rc (below) and project version never go stale on an incremental
# local build — a bare `cmake --build` would otherwise skip reconfigure.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" LITEPDF_VERSION_RAW)
string(STRIP "${LITEPDF_VERSION_RAW}" LITEPDF_VERSION_RAW)
string(REGEX REPLACE "-.*$" "" LITEPDF_VERSION "${LITEPDF_VERSION_RAW}")

project(litepdf
    VERSION     ${LITEPDF_VERSION}
    LANGUAGES   CXX
    DESCRIPTION "Lightweight PDF reader for Windows")

# --- Embedded Win32 version resource (VERSIONINFO) ------------------------
# Derive the .rc version fields from PROJECT_VERSION (itself read from the
# canonical VERSION file above) so litepdf.exe's embedded FILEVERSION /
# ProductVersion can never drift from VERSION. The template lives at
# resources/litepdf.rc.in; the configured copy is generated into the build
# tree and consumed by the litepdf target below. scripts/check-version-sync.ps1
# verifies both that the template stays parametric and that the generated
# output matches VERSION.
set(LITEPDF_VERSION_COMMA  "${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0")
set(LITEPDF_VERSION_DOTTED "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}.0")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/litepdf.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/litepdf.rc"
    @ONLY)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerFlags)

# --- MuPDF via ExternalProject_Add on mupdf.sln ---------------------------
include(ImportMuPDF)  # defines litepdf::mupdf INTERFACE target

# --- core library ---------------------------------------------------------
add_library(litepdf_core STATIC
    src/core/Document.cpp
    src/core/DocumentView.cpp
    src/core/MruList.cpp
    src/core/PageCache.cpp
    src/core/RenderEngine.cpp
    src/core/TabList.cpp
    src/core/SearchSession.cpp     # Phase 6 Task 5
    src/app/SearchDispatcher.cpp   # Phase 6 Task 4
    src/app/CrossTabSearch.cpp     # Phase 6 Task 11
    src/core/ThumbnailModel.cpp    # Phase 7 Task 1
    src/core/ThumbCache.cpp        # Phase 7 Task 2
    src/core/ThumbnailRenderer.cpp # Phase 7 Task 3
    # Phase 7 Task 8: DocumentView owns one ThumbnailPane per tab
    # (D11), so DocumentView.cpp's translation unit references
    # ThumbnailPane symbols. Linking ThumbnailPane.cpp into litepdf_core
    # keeps the static library self-contained and lets unit tests link
    # litepdf_core without pulling in the entire litepdf.exe target.
    # ThumbnailPane is Win32-aware; the rest of litepdf_core's headers
    # already include <windows.h> (ThumbCache, ThumbnailRenderer,
    # ThumbnailModel) so this isn't a new layering breach.
    src/ui/ThumbnailPane.cpp       # Phase 7 Task 5 + 6
    # Phase 8 Task 1: PasswordDialog (Win32 modal) + the pure-logic
    # 3-attempt retry loop. Both live in litepdf_core so the unit tests
    # can link them without pulling in the full litepdf.exe target —
    # same rationale as ThumbnailPane.cpp above.
    src/ui/PasswordDialog.cpp
    src/ui/password_retry.cpp
    # Phase 8.5: Print support (T2). Header-only files
    # (PrintGeometry.hpp, PrintAbortFlag.hpp) are #included directly
    # by consumers and not listed here.
    src/printing/PrintRange.cpp
    src/printing/PrintProgressDlg.cpp
    src/printing/PrintJob.cpp
)

litepdf_apply_flags(litepdf_core)

target_include_directories(litepdf_core PUBLIC src)

target_link_libraries(litepdf_core PRIVATE
    litepdf::mupdf
)

# --- litepdf executable ---------------------------------------------------
add_executable(litepdf WIN32
    src/main.cpp
    src/app/SingleInstance.cpp
    src/ui/ColdStartTimer.cpp
    src/ui/FindBar.cpp           # Phase 6 Task 8
    src/ui/MainWindow.cpp
    src/ui/OutlinePane.cpp
    src/ui/PdfCanvas.cpp
    src/ui/ResultsPanel.cpp      # Phase 6 Task 13
    src/ui/Splitter.cpp          # Phase 6 Task 12
    src/ui/TabManager.cpp
    src/ui/VerticalSplitter.cpp  # Phase 7 Task 4b
    # Version resource generated from resources/litepdf.rc.in by the
    # configure_file() call above (version derived from VERSION at build time).
    "${CMAKE_CURRENT_BINARY_DIR}/litepdf.rc"
)

litepdf_apply_flags(litepdf)

target_link_libraries(litepdf PRIVATE
    comctl32
    litepdf_core
)

set_target_properties(litepdf PROPERTIES
    OUTPUT_NAME           "litepdf"
)

# Resource compiler include paths. The generated litepdf.rc lives in the build
# tree (configure_file above), so EVERY file it references resolves through
# these dirs, not "next to the .rc": MainMenu.rc.h + manifest.xml via the
# resources/ dir, and assets/icon/*.ico via the source root (Phase 9).
target_include_directories(litepdf PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

# Tell the MSVC linker NOT to auto-generate/embed a manifest — the generated
# litepdf.rc (from resources/litepdf.rc.in) already supplies it via
# CREATEPROCESS_MANIFEST_RESOURCE_ID RT_MANIFEST. Having both produces CVT1100
# "duplicate resource" at link time.
if(MSVC)
    target_link_options(litepdf PRIVATE "/MANIFEST:NO")
endif()

# MuPDF will be wired in during Phase 1 (Document core). Deliberately omitted
# here to keep Phase 0 build time small and isolate bootstrap failures.

# --- litepdf-cli (console demo / benchmark harness) -----------------------
add_executable(litepdf-cli
    src/cli/main.cpp
    src/cli/render_to_ppm.cpp
    src/cli/bench_iteration.cpp)
litepdf_apply_flags(litepdf-cli)
target_link_libraries(litepdf-cli PRIVATE litepdf_core litepdf::mupdf)

# --- Tests ----------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)

    # Phase 11: deterministic compare-logic self-test. Use whichever PowerShell
    # is present — pwsh on CI, Windows PowerShell 5.1 on the dev box (which has
    # no pwsh; see reference_litepdf_powershell_51_only). The script is authored
    # 5.1-safe, so either interpreter is correct. $PSScriptRoot-relative
    # threshold resolution + WORKING_DIRECTORY keep the test location-independent.
    find_program(LITEPDF_PWSH NAMES pwsh powershell.exe powershell
                 DOC "PowerShell interpreter for the benchmark self-test")
    if(LITEPDF_PWSH)
        add_test(NAME benchmark_selftest
                 COMMAND "${LITEPDF_PWSH}" -NoProfile -File
                         "${CMAKE_SOURCE_DIR}/scripts/check-benchmark-regression.ps1" -SelfTest)
        set_tests_properties(benchmark_selftest PROPERTIES
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    else()
        message(WARNING "No PowerShell interpreter found; benchmark_selftest not registered.")
    endif()
endif()
