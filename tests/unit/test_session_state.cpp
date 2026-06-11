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
}
