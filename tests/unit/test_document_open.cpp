#include "core/Document.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace litepdf::core;

TEST_CASE("Document::open succeeds on a simple PDF", "[document][open]") {
    Document doc;
    REQUIRE_FALSE(doc.is_open());

    auto err = doc.open("tests/fixtures/simple.pdf");
    INFO("OpenError value: " << (err ? static_cast<int>(*err) : -1));
    REQUIRE_FALSE(err.has_value());
    REQUIRE(doc.is_open());
}
