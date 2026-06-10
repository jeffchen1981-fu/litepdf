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

# --- Phase 11.5: prune unused MuPDF features + fonts to shrink the exe ----
# Prepend pruning #defines above config.h's `#ifndef FZ_CONFIG_H` include guard.
# FZ_ENABLE_* are #ifndef / #if-!defined-guarded in config.h so an injected 0
# wins; the TOFU_* macros are tested with bare #ifndef in the font source
# (source/fitz/font-table.h), so an injected #define equally takes effect. Exact
# line numbers vary per MuPDF version — the prune-effective assertion below
# verifies each guard/consult is still live.
#
# Idempotent AND re-appliable: the _PRUNE_VER marker version-stamps the injected
# set. If config.h lacks the CURRENT marker (fresh checkout, OR the define set
# changed — e.g. Task 6 adds TOFU_CJK_LANG and bumps the version), restore the
# tracked submodule file to pristine (dropping any older prepend so we never
# stack two), re-prepend the current set, and delete the stamp so it rebuilds.
set(_MUPDF_CONFIG_H "${_MUPDF_ROOT}/include/mupdf/fitz/config.h")
# V9 = the spike-proven durable set (spike findings lever table, 2026-06-09).
# V3-V8 were throwaway experiment rungs on spike/mupdf-1.27 and never landed;
# this is the first durable bump after V2, jumping straight to V9 so the marker
# matches the findings doc the team will reference. TOFU_EMOJI is a no-op in
# 1.27.2 (spike V8 = 0 bytes; no standalone consult, force-defined by TOFU_NOTO)
# but kept for spike-V9 parity — it is deliberately excluded from the assertion.
set(_PRUNE_VER "LITEPDF_PRUNE_V9")   # bump when the injected define set changes
set(_prune
    "/* ${_PRUNE_VER} — injected by ImportMuPDF.cmake (do not edit) */\n"
    "#define FZ_ENABLE_JS 0\n"
    "#define FZ_ENABLE_OCR_OUTPUT 0\n"
    "#define FZ_ENABLE_DOCX_OUTPUT 0\n"
    "#define FZ_ENABLE_BARCODE 0\n"
    "#define FZ_ENABLE_BROTLI 0\n"
    "#define FZ_ENABLE_HYPHEN 0\n"
    "#define FZ_ENABLE_HYPHEN_ALL 0\n"
    "#define FZ_ENABLE_OFFICE 0\n"
    "#define FZ_ENABLE_MOBI 0\n"
    "#define FZ_ENABLE_HTML 0\n"
    "#define FZ_ENABLE_TXT 0\n"
    "#define FZ_ENABLE_IMG 0\n"
    "#define TOFU_SYMBOL\n"
    "#define TOFU_EMOJI\n"
    "#define TOFU_NOTO\n"
    "#define TOFU_CJK_LANG\n")
string(JOIN "" _prune_str ${_prune})
file(READ "${_MUPDF_CONFIG_H}" _cfg)
if(NOT _cfg MATCHES "${_PRUNE_VER}")
    execute_process(
        COMMAND git -C "${_MUPDF_ROOT}" checkout -- include/mupdf/fitz/config.h
        RESULT_VARIABLE _gitrc)
    if(NOT _gitrc EQUAL 0)
        message(FATAL_ERROR
            "Phase 11.5: failed to restore MuPDF config.h to pristine (git rc=${_gitrc}). "
            "A silent restore failure would stack two prune layers and break the "
            "single-clean-layer / exactly-one-rebuild guarantee. Aborting.")
    endif()
    file(READ "${_MUPDF_CONFIG_H}" _cfg_pristine)
    file(WRITE "${_MUPDF_CONFIG_H}" "${_prune_str}${_cfg_pristine}")
    message(STATUS "MuPDF config.h pruned (${_PRUNE_VER}).")
    # ExternalProject BUILD_ALWAYS OFF re-runs MSBuild only when its stamp is
    # absent — not when config.h changes. Delete the stamp so the prune rebuilds.
    file(REMOVE
        "${CMAKE_BINARY_DIR}/mupdf_ext-prefix/src/mupdf_ext-stamp/Release/mupdf_ext-build")
endif()

# --- Prune-effective assertion (spec section 5; Opus F6 + Codex #5) ----------
# Runs on EVERY configure (outside the marker-guarded prune block) so a renamed
# or removed macro after a future MuPDF bump fails loudly HERE, not as a late,
# ambiguous size regression. Anchored to the actual guard/consult FORM (not a
# bare token) so a substring (FZ_ENABLE_HYPHEN inside FZ_ENABLE_HYPHEN_ALL;
# FZ_ENABLE_HTML inside FZ_ENABLE_HTML_ENGINE) or a comment-only opt-in line
# cannot mask a real removal. CMake regex has no word boundary, so we require a
# non-identifier char after the token.
file(READ "${_MUPDF_CONFIG_H}" _cfg_live)
foreach(_m FZ_ENABLE_JS FZ_ENABLE_OCR_OUTPUT FZ_ENABLE_DOCX_OUTPUT
           FZ_ENABLE_BARCODE FZ_ENABLE_BROTLI FZ_ENABLE_HYPHEN
           FZ_ENABLE_HYPHEN_ALL FZ_ENABLE_OFFICE FZ_ENABLE_MOBI
           FZ_ENABLE_HTML FZ_ENABLE_TXT FZ_ENABLE_IMG)
    # Accept #ifndef X  OR  #if !defined(X)  (BARCODE uses the latter).
    if(NOT _cfg_live MATCHES "ifndef[ \t]+${_m}[^A-Za-z0-9_]"
       AND NOT _cfg_live MATCHES "defined[ \t]*\\([ \t]*${_m}[ \t]*\\)")
        message(FATAL_ERROR
            "Prune drift: no live #ifndef / #if-!defined guard for ${_m} in "
            "MuPDF config.h. It was renamed/removed upstream and the injected "
            "'#define ${_m} 0' is now a no-op. Re-audit the V9 set against this "
            "MuPDF version before proceeding.")
    endif()
endforeach()
# TOFU_* are consulted (bare #ifndef) in the FONT source, NOT config.h.
# font-table.h is the canonical consult site (font.c also references CJK_LANG).
# TOFU_EMOJI is intentionally NOT asserted: 1.27.2 has no standalone
# '#ifndef TOFU_EMOJI' consult; it is driven by TOFU_NOTO/NOTO_SMALL and
# measured 0 bytes in the spike (V8). It stays injected for spike-V9 parity.
file(READ "${_MUPDF_ROOT}/source/fitz/font-table.h" _ft_src)
foreach(_t TOFU_SYMBOL TOFU_NOTO TOFU_CJK_LANG)
    if(NOT _ft_src MATCHES "ifndef[ \t]+${_t}[^A-Za-z0-9_]")
        message(FATAL_ERROR
            "Prune drift: no live '#ifndef ${_t}' consult in "
            "source/fitz/font-table.h. The font-prune consult site moved or was "
            "renamed upstream; the injected '#define ${_t}' is now a no-op.")
    endif()
endforeach()
message(STATUS "MuPDF prune-effective assertion passed (12 FZ_ENABLE_* + 3 TOFU_*; TOFU_EMOJI exempt).")

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
