#include "core/Document.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace litepdf::core;

TEST_CASE("Document::page_count returns >=1 for simple.pdf", "[document][pages]") {
    Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());
    REQUIRE(doc.page_count() >= 1);
}

TEST_CASE("Document::page_size returns sane dimensions for simple.pdf", "[document][pages]") {
    Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());

    PageSize size = doc.page_size(0);
    // simple.pdf (agstat.pdf) is a standard-size PDF — width 400-700pt, height 400-900pt
    REQUIRE(size.width_pt  > 400.0f);
    REQUIRE(size.width_pt  < 700.0f);
    REQUIRE(size.height_pt > 400.0f);
    REQUIRE(size.height_pt < 900.0f);
}
