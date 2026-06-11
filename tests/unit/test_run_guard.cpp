#include <catch2/catch_test_macros.hpp>
#include "app/AppRunGuard.hpp"
#include <filesystem>
#include <fstream>

using litepdf::app::AppRunGuard;

static std::filesystem::path marker_path() {
    auto p = std::filesystem::temp_directory_path() / L"litepdf_test_run.lock";
    std::error_code ec; std::filesystem::remove(p, ec);
    return p;
}

TEST_CASE("fresh start is not abnormal; clean exit removes marker", "[app][runguard]") {
    auto m = marker_path();
    AppRunGuard g(m);
    REQUIRE_FALSE(g.previous_exit_was_abnormal());
    REQUIRE(std::filesystem::exists(m));
    g.mark_clean_exit();
    REQUIRE_FALSE(std::filesystem::exists(m));
    g.mark_clean_exit();   // idempotent
}

TEST_CASE("leftover marker => previous exit abnormal", "[app][runguard]") {
    auto m = marker_path();
    { std::ofstream(m) << "x"; }   // simulate a crash leaving the marker
    AppRunGuard g(m);
    REQUIRE(g.previous_exit_was_abnormal());
    std::error_code ec; std::filesystem::remove(m, ec);
}

TEST_CASE("empty marker path => no file, never abnormal", "[app][runguard]") {
    AppRunGuard g(std::filesystem::path{});   // persistence-disabled construction
    REQUIRE_FALSE(g.previous_exit_was_abnormal());
    g.mark_clean_exit();   // must not throw or create a "" file
}
