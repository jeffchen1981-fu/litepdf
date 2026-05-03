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

// Phase 8 Task 2 (D5): no XPS fixture in v1, but the allowlist code
// path must keep accepting `.xps`. We assert this by attempting to open
// a nonexistent .xps and observing FileNotFound — NOT UnsupportedFormat.
// If a future refactor drops .xps from the allowlist, this assertion
// fires loudly.
TEST_CASE("XPS extension stays in allowlist (no fixture, FileNotFound expected)",
          "[document][formats]") {
    Document d;
    auto err = d.open("tests/fixtures/does-not-exist.xps");
    REQUIRE(err.has_value());
    REQUIRE(*err == Document::OpenError::FileNotFound);
}

// Phase 8 Task 2 (D4 negative case): non-allowlisted extensions must be
// rejected before MuPDF is invoked — sample.png is a deliberately-shipped
// negative fixture for this check.
TEST_CASE("Document rejects non-allowlisted extension (sample.png)",
          "[document][formats]") {
    Document d;
    auto err = d.open("tests/fixtures/sample.png");
    REQUIRE(err.has_value());
    REQUIRE(*err == Document::OpenError::UnsupportedFormat);
}
