#include <catch2/catch_test_macros.hpp>
#include "core/SearchQuery.hpp"
using namespace litepdf::core;

TEST_CASE("search_options_value", "[search][query]") {
    REQUIRE(search_options_value(false, false) == 1);  // ignore-case literal
    REQUIRE(search_options_value(true,  false) == 0);  // exact literal
    REQUIRE(search_options_value(false, true)  == 5);  // ignore-case regex
    REQUIRE(search_options_value(true,  true)  == 4);  // exact regex
}
TEST_CASE("regex_escape", "[search][query]") {
    REQUIRE(regex_escape("a.b*c") == "a\\.b\\*c");
    REQUIRE(regex_escape("(x)+")  == "\\(x\\)\\+");
    REQUIRE(regex_escape("plain") == "plain");
    REQUIRE(regex_escape("a\\b")  == "a\\\\b");   // lone backslash gets escaped
}
TEST_CASE("build_search_needle", "[search][query]") {
    REQUIRE(build_search_needle("cat",  false, false) == "cat");
    REQUIRE(build_search_needle("cat",  false, true)  == "cat");
    REQUIRE(build_search_needle("a.b",  true,  false) == "\\ba\\.b\\b");
    REQUIRE(build_search_needle("ca|t", true,  true)  == "\\b(?:ca|t)\\b");
}
