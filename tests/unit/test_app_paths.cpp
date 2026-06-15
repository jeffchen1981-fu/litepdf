#include <catch2/catch_test_macros.hpp>
#include "app/AppPaths.hpp"

using litepdf::app::compose_app_data_dir;
using litepdf::app::session_file_under;
using litepdf::app::crashes_dir_under;
using litepdf::app::running_marker_under;

TEST_CASE("compose_app_data_dir appends LitePDF", "[app][paths]") {
    auto root = std::filesystem::path(L"C:\\Users\\X\\AppData\\Local");
    REQUIRE(compose_app_data_dir(root) ==
            std::filesystem::path(L"C:\\Users\\X\\AppData\\Local\\LitePDF"));
}

TEST_CASE("sub-paths hang off the app data dir", "[app][paths]") {
    auto base = std::filesystem::path(L"D:\\appdata\\LitePDF");
    REQUIRE(session_file_under(base).filename() == L"session.json");
    REQUIRE(crashes_dir_under(base).filename() == L"crashes");
    REQUIRE(running_marker_under(base).filename() == L"running.lock");
    REQUIRE(session_file_under(base).parent_path() == base);
}
