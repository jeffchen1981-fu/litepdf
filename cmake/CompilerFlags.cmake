# Compiler flags shared across all LitePDF targets.
# Sets static CRT, warning level, Unicode, and C++17.

function(litepdf_apply_flags target)
    target_compile_features(${target} PRIVATE cxx_std_17)

    if(MSVC)
        # Static CRT: no VC++ Redistributable dependency at runtime.
        set_property(TARGET ${target} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

        target_compile_options(${target} PRIVATE
            /W4             # high warning level
            /permissive-    # strict conformance
            /Zc:__cplusplus # report correct __cplusplus value
            /utf-8          # source + execution charset UTF-8
            # Phase 12: emit CodeView debug info in Release so the linker can
            # produce a .pdb for crash-dump symbolization (a release build had
            # none, so shipped minidumps resolved only to module+offset). /Z7
            # (not /Zi) embeds the info in each .obj/.lib, sidestepping the
            # static-lib compiler-PDB path dance; the exe itself is unaffected —
            # the debug info lands in litepdf.pdb, never the binary.
            $<$<CONFIG:Release>:/Z7>
        )

        target_compile_definitions(${target} PRIVATE
            UNICODE _UNICODE             # Win32 W-APIs by default
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            _CRT_SECURE_NO_WARNINGS
        )

        # Phase 12: pair the /Z7 above with /DEBUG so Release links emit
        # litepdf.pdb, but re-assert the size optimizations /DEBUG otherwise
        # turns off (/OPT:REF drops unreferenced code, /OPT:ICF folds identical
        # COMDATs) so the shipped exe stays lean while gaining a symbol file.
        # No-op on litepdf_core (link options are ignored for static libs).
        target_link_options(${target} PRIVATE
            $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
    else()
        message(FATAL_ERROR
            "LitePDF is Windows/MSVC-only for v1. "
            "Detected CXX compiler: ${CMAKE_CXX_COMPILER_ID}")
    endif()
endfunction()
