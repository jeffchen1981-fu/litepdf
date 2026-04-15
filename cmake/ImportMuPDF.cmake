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
)

# INTERFACE target that our code links against. Headers + 3 output .libs
# (libmuthreads.vcxproj is not a ProjectReference of libmupdf — MuPDF uses
# Win32 threading APIs directly; threading helper lib is opt-in and unused)
# + a handful of Windows system libs MuPDF may reference.
add_library(litepdf_mupdf INTERFACE)
add_dependencies(litepdf_mupdf mupdf_ext)

target_include_directories(litepdf_mupdf INTERFACE
    "${_MUPDF_ROOT}/include")

target_link_libraries(litepdf_mupdf INTERFACE
    "${_MUPDF_OUTPUT_DIR}/libmupdf.lib"
    "${_MUPDF_OUTPUT_DIR}/libthirdparty.lib"
    "${_MUPDF_OUTPUT_DIR}/libresources.lib"
    ws2_32 advapi32 gdi32 user32 crypt32)

add_library(litepdf::mupdf ALIAS litepdf_mupdf)
