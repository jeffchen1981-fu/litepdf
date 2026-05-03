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
    unit/test_document_search.cpp   # Phase 6 Task 3
    unit/test_render_engine_lifecycle.cpp
    unit/test_render_engine_submit.cpp
    unit/test_render_engine_priority.cpp
    unit/test_render_engine_cancel.cpp
    unit/test_render_engine_output.cpp
    unit/test_page_cache_l1.cpp
    unit/test_page_cache_l2.cpp
    unit/test_page_cache_peek.cpp   # Phase 6 Task 1
    unit/test_render_engine_cache.cpp
    unit/test_render_stress.cpp
    unit/test_document_view.cpp
    unit/test_mru_list.cpp
    unit/test_tab_list.cpp
    unit/test_search_dispatcher.cpp  # Phase 6 Task 4
    unit/test_search_session.cpp     # Phase 6 Task 5
    unit/test_cross_tab_search.cpp   # Phase 6 Task 11
    unit/test_crosstab_ctrlw_race.cpp  # Phase 6 ship-gate repro
    unit/test_splitter_math.cpp      # Phase 7 Task 4a
    unit/test_vertical_splitter.cpp  # Phase 7 Task 4b
    unit/test_thumbnail_model.cpp      # Phase 7 Task 1
    unit/test_thumb_cache.cpp          # Phase 7 Task 2
    unit/test_thumbnail_renderer.cpp   # Phase 7 Task 3
    unit/test_password_dialog.cpp      # Phase 8 Task 1
    unit/test_password_retry.cpp       # Phase 8 Task 1
    unit/test_render_engine_invert.cpp # Phase 8 Task 3
    unit/test_dual_page_layout.cpp     # Phase 8 Task 4
)
target_link_libraries(litepdf_unit_tests PRIVATE litepdf_core)

litepdf_apply_flags(litepdf_unit_tests)

target_link_libraries(litepdf_unit_tests PRIVATE
    Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(litepdf_unit_tests WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
