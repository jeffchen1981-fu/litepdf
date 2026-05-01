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

# Submodule init guard. ExternalProject's BUILD_BYPRODUCTS are not consulted on
# incremental rebuilds — only stamp files — so a wiped submodule with stale
# stamps silently no-ops MSBuild and link fails downstream with a confusing
# "cannot open libmupdf.lib" error. Fail fast at configure time using the .sln
# as a definitive witness file: it lives in tracked submodule content and is
# absent iff the submodule is uninitialized or has been emptied.
if(NOT EXISTS "${_MUPDF_SLN}")
    message(FATAL_ERROR
        "MuPDF submodule not initialized — '${_MUPDF_SLN}' not found.\n"
        "Run from repo root: git submodule update --init --recursive third_party/mupdf\n"
        "If the submodule was previously initialized, also purge stale ExternalProject stamps:\n"
        "  Remove-Item ${CMAKE_BINARY_DIR}/mupdf_ext-prefix/src/mupdf_ext-stamp/Release/* -Force")
endif()

# Locate MSBuild.exe — ship with VS BuildTools. Use vswhere for discovery.
find_program(_VSWHERE_EXE
    NAMES vswhere.exe
    PATHS
        "$ENV{ProgramFiles\(x86\)}/Microsoft Visual Studio/Installer"
        "$ENV{ProgramFiles}/Microsoft Visual Studio/Installer"
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

# Force MuPDF vcxprojs to use /MT (static CRT) so our output exe carries no
# msvcrt / vcruntime redistributable dependency. Upstream's vcxprojs hardcode
# <RuntimeLibrary>MultiThreadedDLL</RuntimeLibrary> (/MD) and msbuild's
# /p:RuntimeLibrary= and /p:ClCompile_RuntimeLibrary= command-line overrides
# do NOT take effect against an explicit project-level property — tested both,
# still produced MD_DynamicRelease .objs and 37 LNK2001 __imp_* errors when
# linking with Catch2 unit test exe.
#
# Patch strategy: in-place rewrite RuntimeLibrary values in all vcxproj files
# under platform/win32. Done at configure time; idempotent (second pass is a
# no-op because the MD form no longer matches). We mirror the v142→v143
# toolset override strategy of rewriting where command-line /p: is refused.
file(GLOB _MUPDF_VCXPROJS "${_MUPDF_ROOT}/platform/win32/*.vcxproj")
foreach(_vcx ${_MUPDF_VCXPROJS})
    file(READ "${_vcx}" _vcx_content)
    string(REPLACE
        "<RuntimeLibrary>MultiThreadedDLL</RuntimeLibrary>"
        "<RuntimeLibrary>MultiThreaded</RuntimeLibrary>"
        _vcx_content "${_vcx_content}")
    string(REPLACE
        "<RuntimeLibrary>MultiThreadedDebugDLL</RuntimeLibrary>"
        "<RuntimeLibrary>MultiThreadedDebug</RuntimeLibrary>"
        _vcx_content "${_vcx_content}")
    file(WRITE "${_vcx}" "${_vcx_content}")
endforeach()

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
