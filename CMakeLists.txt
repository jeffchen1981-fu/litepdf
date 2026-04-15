cmake_minimum_required(VERSION 3.25)

# Read version from VERSION file (single source of truth).
# Strip any pre-release suffix (e.g. "-dev", "-rc1") because CMake's
# project(VERSION ...) only accepts numeric major.minor.patch[.tweak].
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" LITEPDF_VERSION_RAW)
string(STRIP "${LITEPDF_VERSION_RAW}" LITEPDF_VERSION_RAW)
string(REGEX REPLACE "-.*$" "" LITEPDF_VERSION "${LITEPDF_VERSION_RAW}")

project(litepdf
    VERSION     ${LITEPDF_VERSION}
    LANGUAGES   CXX
    DESCRIPTION "Lightweight PDF reader for Windows")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerFlags)

# --- MuPDF via ExternalProject_Add on mupdf.sln ---------------------------
include(ImportMuPDF)  # defines litepdf::mupdf INTERFACE target

# --- core library ---------------------------------------------------------
add_library(litepdf_core STATIC
    src/core/Document.cpp
    src/core/PageCache.cpp
    src/core/RenderEngine.cpp
)

litepdf_apply_flags(litepdf_core)

target_include_directories(litepdf_core PUBLIC src)

target_link_libraries(litepdf_core PRIVATE
    litepdf::mupdf
)

# --- litepdf executable ---------------------------------------------------
add_executable(litepdf WIN32
    src/main.cpp
    src/ui/MainWindow.cpp
    resources/litepdf.rc
)

litepdf_apply_flags(litepdf)

target_link_libraries(litepdf PRIVATE
    comctl32
    litepdf_core
)

set_target_properties(litepdf PROPERTIES
    OUTPUT_NAME           "litepdf"
)

# Resource compiler: ensure it can find manifest.xml relative to the .rc file
target_include_directories(litepdf PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/resources"
)

# Tell the MSVC linker NOT to auto-generate/embed a manifest — resources/litepdf.rc
# already supplies it via CREATEPROCESS_MANIFEST_RESOURCE_ID RT_MANIFEST. Having both
# produces CVT1100 "duplicate resource" at link time.
if(MSVC)
    target_link_options(litepdf PRIVATE "/MANIFEST:NO")
endif()

# MuPDF will be wired in during Phase 1 (Document core). Deliberately omitted
# here to keep Phase 0 build time small and isolate bootstrap failures.

# --- litepdf-cli (console demo / benchmark harness) -----------------------
add_executable(litepdf-cli src/cli/main.cpp)
litepdf_apply_flags(litepdf-cli)
target_link_libraries(litepdf-cli PRIVATE litepdf_core litepdf::mupdf)

# --- Tests ----------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
