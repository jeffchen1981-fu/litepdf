cmake_minimum_required(VERSION 3.25)
project(litepdf
    VERSION     0.1.0
    LANGUAGES   CXX
    DESCRIPTION "Lightweight PDF reader for Windows")

# Global settings
set(CMAKE_CXX_STANDARD          17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(CompilerFlags)

# --- litepdf executable ---------------------------------------------------
add_executable(litepdf WIN32
    src/main.cpp
    resources/litepdf.rc
)

litepdf_apply_flags(litepdf)

target_link_libraries(litepdf PRIVATE
    comctl32
)

set_target_properties(litepdf PROPERTIES
    OUTPUT_NAME           "litepdf"
    RUNTIME_OUTPUT_NAME   "litepdf"
)

# Resource compiler: ensure it can find manifest.xml relative to the .rc file
target_include_directories(litepdf PRIVATE
    "${CMAKE_SOURCE_DIR}/resources"
)

# MuPDF will be wired in during Phase 1 (Document core). Deliberately omitted
# here to keep Phase 0 build time small and isolate bootstrap failures.
