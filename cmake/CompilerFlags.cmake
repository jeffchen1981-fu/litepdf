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
        )

        target_compile_definitions(${target} PRIVATE
            UNICODE _UNICODE             # Win32 W-APIs by default
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            _CRT_SECURE_NO_WARNINGS
        )
    endif()
endfunction()
