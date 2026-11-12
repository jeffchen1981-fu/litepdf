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
# changed — e.g. V10 adds TOFU_CJK to drop the embedded CJK font), restore the
# tracked submodule file to pristine (dropping any older prepend so we never
# stack two), re-prepend the current set, and delete the stamp so it rebuilds.
set(_MUPDF_CONFIG_H "${_MUPDF_ROOT}/include/mupdf/fitz/config.h")
# V9 = the spike-proven durable set (spike findings lever table, 2026-06-09).
# V3-V8 were throwaway experiment rungs on spike/mupdf-1.27 and never landed;
# this is the first durable bump after V2, jumping straight to V9 so the marker
# matches the findings doc the team will reference. TOFU_EMOJI is a no-op in
# 1.27.2 (spike V8 = 0 bytes; no standalone consult, force-defined by TOFU_NOTO)
# but kept for spike-V9 parity — it is deliberately excluded from the assertion.
# V10 adds TOFU_CJK to drop the embedded CJK font (~4.84 MB SourceHanSerif block
# in font-table.h:281). CJK glyphs are now resolved at runtime from Windows system
# fonts via the DirectWrite loader (Task 2), so the embedded fallback is redundant.
set(_PRUNE_VER "LITEPDF_PRUNE_V10")  # bump when the injected define set changes
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
    "#define TOFU_CJK_LANG\n"
    "#define TOFU_CJK\n")
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
            "MuPDF config.h. Most likely your third_party/mupdf checkout is stale "
            "(run 'git submodule update --init --recursive'); otherwise the macro "
            "was renamed/removed upstream and the injected '#define ${_m} 0' is "
            "now a no-op — re-audit the V9 set against this MuPDF version.")
    endif()
endforeach()
# TOFU_* are consulted (bare #ifndef) in the FONT source, NOT config.h.
# font-table.h is the canonical consult site (font.c also references CJK_LANG).
# TOFU_EMOJI is intentionally NOT asserted: 1.27.2 has no standalone
# '#ifndef TOFU_EMOJI' consult; it is driven by TOFU_NOTO/NOTO_SMALL and
# measured 0 bytes in the spike (V8). It stays injected for spike-V9 parity.
file(READ "${_MUPDF_ROOT}/source/fitz/font-table.h" _ft_src)
foreach(_t TOFU_SYMBOL TOFU_NOTO TOFU_CJK_LANG TOFU_CJK)
    if(NOT _ft_src MATCHES "ifndef[ \t]+${_t}[^A-Za-z0-9_]")
        message(FATAL_ERROR
            "Prune drift: no live '#ifndef ${_t}' consult in "
            "source/fitz/font-table.h. The font-prune consult site moved or was "
            "renamed upstream; the injected '#define ${_t}' is now a no-op.")
    endif()
endforeach()
message(STATUS "MuPDF prune-effective assertion passed (12 FZ_ENABLE_* + 4 TOFU_*; TOFU_EMOJI exempt).")

# --- #229: local source patch, one-pass outline resolve ---------------------
# MuPDF 1.27.2 resolves an HTML-engine document's outline (FB2: one entry per
# section title) with one walk of the whole box tree per entry, after every
# layout: O(N^2) in the titles. third_party/mupdf-patches/ carries the fix
# until upstream has one; its header says what it changes and how to drop it.
#
# Same shape as the prune block: the patched file carries a versioned marker.
# Without the current marker (fresh checkout, or the patch changed and the
# marker was bumped), restore the tracked file to pristine, apply the patch,
# check the marker landed, and delete the stamp so MSBuild rebuilds. A tree
# with no .git (the release source tarball) already carries the marker, so it
# never reaches the git calls. The marker version and the symbol suffix move
# together (src/core/MuPdfPatchNotice.cpp): a library built from another patch
# version then fails litepdf's link instead of linking silently.
set(_LITEPDF_PATCH_VER "LITEPDF_PATCH_HTDOC_OUTLINE_V1")
set(_LITEPDF_PATCH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mupdf-patches/0001-htdoc-one-pass-outline.patch")
set(_HTML_DOC_C "${_MUPDF_ROOT}/source/html/html-doc.c")
set(_HTML_OUTLINE_C "${_MUPDF_ROOT}/source/html/html-outline.c")
file(READ "${_HTML_DOC_C}" _html_doc)
if(NOT _html_doc MATCHES "${_LITEPDF_PATCH_VER}")
    execute_process(
        COMMAND git -C "${_MUPDF_ROOT}" checkout -- source/html/html-doc.c
        RESULT_VARIABLE _gitrc)
    if(NOT _gitrc EQUAL 0)
        message(FATAL_ERROR
            "#229 patch: MuPDF source/html/html-doc.c lacks the ${_LITEPDF_PATCH_VER} "
            "marker and could not be restored to pristine (git rc=${_gitrc}). "
            "A tree without .git (an extracted source tarball) must already carry "
            "the patched file; otherwise run 'git submodule update --init "
            "third_party/mupdf' and configure again.")
    endif()
    execute_process(
        COMMAND git -C "${_MUPDF_ROOT}" apply --whitespace=nowarn "${_LITEPDF_PATCH}"
        RESULT_VARIABLE _gitrc)
    if(NOT _gitrc EQUAL 0)
        message(FATAL_ERROR
            "#229 patch: ${_LITEPDF_PATCH} no longer applies to MuPDF's "
            "source/html/html-doc.c (git rc=${_gitrc}). Most likely MuPDF was "
            "bumped: re-base the patch on the new html-doc.c, or drop it if "
            "upstream now resolves the outline in one pass (see the patch header).")
    endif()
    file(READ "${_HTML_DOC_C}" _html_doc)
    if(NOT _html_doc MATCHES "${_LITEPDF_PATCH_VER}")
        message(FATAL_ERROR
            "#229 patch: git apply succeeded but ${_HTML_DOC_C} still lacks the "
            "${_LITEPDF_PATCH_VER} marker. The patch file and the marker version "
            "in ImportMuPDF.cmake disagree.")
    endif()
    message(STATUS "MuPDF html-doc.c patched (${_LITEPDF_PATCH_VER}).")
    file(REMOVE
        "${CMAKE_BINARY_DIR}/mupdf_ext-prefix/src/mupdf_ext-stamp/Release/mupdf_ext-build")
endif()

# Every configure: the patch copies find_first_content and the value rules of
# find_box_target / find_flow_target from html-outline.c, a file it does not
# touch. If a MuPDF bump changes that file, html-doc.c may still patch cleanly
# while the copied rules go stale (upstream master already changed
# find_flow_target). Line endings are normalised so CRLF and LF checkouts agree.
set(_HTML_OUTLINE_SHA256_1272 "25a76099a70f4daefc9ad2f94d8141d8705c5b764fd04950ca12ed9a12c38dc4")
file(READ "${_HTML_OUTLINE_C}" _html_outline)
string(REPLACE "\r\n" "\n" _html_outline "${_html_outline}")
string(SHA256 _html_outline_sha "${_html_outline}")
if(NOT _html_outline_sha STREQUAL _HTML_OUTLINE_SHA256_1272)
    message(FATAL_ERROR
        "#229 patch: MuPDF source/html/html-outline.c changed (SHA-256 "
        "${_html_outline_sha}, recorded ${_HTML_OUTLINE_SHA256_1272}). The patch "
        "in third_party/mupdf-patches/ copies find_first_content and the value "
        "rules of find_box_target / find_flow_target from it: re-check those "
        "copies against the new file, then update _HTML_OUTLINE_SHA256_1272 here.")
endif()

# A changed patch, a reverted html-doc.c or a changed html-outline.c re-runs
# configure, and with it the apply and the hash check, on the next build.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_LITEPDF_PATCH}" "${_HTML_DOC_C}" "${_HTML_OUTLINE_C}")

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
