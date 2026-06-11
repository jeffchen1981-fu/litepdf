#include <catch2/catch_test_macros.hpp>
#include "core/SessionState.hpp"

using namespace litepdf::core;

TEST_CASE("to_json emits a backslash-escaped Windows path", "[core][session][json]") {
    SessionState s;
    s.window = {0, 1, 100, 80, 1024, 768};
    s.active_tab = 0;
    s.tabs.push_back({std::filesystem::path(L"C:\\docs\\a b.pdf"), 3,
                      SessionZoom::Custom, 1.25f});

    std::string j = to_json(s);
    REQUIRE(j.find("C:\\\\docs\\\\a b.pdf") != std::string::npos);
    REQUIRE(j.find("\"page\":3") != std::string::npos);
    REQUIRE(j.find("\"zoom_mode\":\"custom\"") != std::string::npos);
    // 1.25f is a power-of-two-divisible float, but assert it round-trips
    // exactly through %.9g (no trailing 1.2500000x noise).
    REQUIRE(j.find("\"zoom_scale\":1.25") != std::string::npos);
}

TEST_CASE("to_json emits empty tabs array", "[core][session][json]") {
    SessionState s;  // default-constructed: no tabs
    std::string j = to_json(s);
    REQUIRE(j.find("\"tabs\":[]") != std::string::npos);
}

TEST_CASE("to_json maps each fit zoom mode to its string", "[core][session][json]") {
    SessionState s;
    s.tabs.push_back({std::filesystem::path(L"a.pdf"), 0,
                      SessionZoom::FitWidth, 1.0f});
    s.tabs.push_back({std::filesystem::path(L"b.pdf"), 0,
                      SessionZoom::FitPage, 1.0f});

    std::string j = to_json(s);
    REQUIRE(j.find("\"zoom_mode\":\"fit_width\"") != std::string::npos);
    REQUIRE(j.find("\"zoom_mode\":\"fit_page\"") != std::string::npos);
}
