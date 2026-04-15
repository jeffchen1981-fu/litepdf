#include "core/Document.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace litepdf::core;

TEST_CASE("Encrypted PDF open returns NeedsPassword", "[document][password]") {
    Document doc;
    auto err = doc.open("tests/fixtures/encrypted.pdf");
    REQUIRE(err.has_value());
    REQUIRE(*err == Document::OpenError::NeedsPassword);
    REQUIRE_FALSE(doc.is_open());
}

TEST_CASE("Encrypted PDF authenticates with correct password", "[document][password]") {
    Document doc;
    auto err = doc.open("tests/fixtures/encrypted.pdf");
    REQUIRE(err.has_value());
    REQUIRE(*err == Document::OpenError::NeedsPassword);

    REQUIRE(doc.authenticate("test"));
    REQUIRE(doc.is_open());
    REQUIRE(doc.page_count() >= 1);
}

TEST_CASE("Encrypted PDF rejects wrong password", "[document][password]") {
    Document doc;
    (void)doc.open("tests/fixtures/encrypted.pdf");
    REQUIRE_FALSE(doc.authenticate("wrong"));
    REQUIRE_FALSE(doc.is_open());
}
