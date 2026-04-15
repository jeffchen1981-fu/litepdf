#include "core/Document.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace litepdf::core;

TEST_CASE("ePub opens and has pages", "[document][formats]") {
    Document doc;
    auto err = doc.open("tests/fixtures/sample.epub");
    REQUIRE_FALSE(err.has_value());
    REQUIRE(doc.page_count() >= 1);
}

TEST_CASE("CBZ opens and has pages", "[document][formats]") {
    Document doc;
    auto err = doc.open("tests/fixtures/sample.cbz");
    REQUIRE_FALSE(err.has_value());
    REQUIRE(doc.page_count() >= 1);
}
