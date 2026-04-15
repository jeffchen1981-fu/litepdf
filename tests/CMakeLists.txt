include(FetchContent)

# Catch2 must use the same CRT as the rest of LitePDF (static /MT[d]).
# Without this, Catch2 builds with default /MD and the test exe gets
# LNK2019 unresolved externals (nextafter, _time64, _gmtime64_s).
# CMP0091 (NEW) plus this policy variable propagates MSVC_RUNTIME_LIBRARY
# into FetchContent subprojects.
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY
    "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)

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

target_sources(litepdf_unit_tests PRIVATE
    unit/test_document_open.cpp
    unit/test_document_pages.cpp
    unit/test_document_text.cpp
    unit/test_document_password.cpp
    unit/test_document_outline.cpp
    unit/test_document_formats.cpp
    unit/test_document_clone_context.cpp
    unit/test_document_accessors.cpp
    unit/test_document_unicode_path.cpp
    unit/test_render_engine_lifecycle.cpp
    unit/test_render_engine_submit.cpp
    unit/test_render_engine_priority.cpp
    unit/test_render_engine_cancel.cpp
    unit/test_render_engine_output.cpp
    unit/test_page_cache_l1.cpp
    unit/test_page_cache_l2.cpp
    unit/test_render_engine_cache.cpp
    unit/test_render_stress.cpp
    unit/test_document_view.cpp
)
target_link_libraries(litepdf_unit_tests PRIVATE litepdf_core)

litepdf_apply_flags(litepdf_unit_tests)

target_link_libraries(litepdf_unit_tests PRIVATE
    Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(litepdf_unit_tests WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
