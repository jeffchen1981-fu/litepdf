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

TEST_CASE("Document::open returns FileNotFound for missing file", "[document][open]") {
    Document doc;
    auto err = doc.open("tests/fixtures/does_not_exist.pdf");
    REQUIRE(err.has_value());
    REQUIRE(*err == Document::OpenError::FileNotFound);
    REQUIRE_FALSE(doc.is_open());
}

TEST_CASE("Document::open returns UnsupportedFormat for PNG", "[document][open]") {
    Document doc;
    auto err = doc.open("tests/fixtures/sample.png");
    REQUIRE(err.has_value());
    REQUIRE(*err == Document::OpenError::UnsupportedFormat);
    REQUIRE_FALSE(doc.is_open());
}

TEST_CASE("Document::open returns Corrupted for truncated PDF", "[document][open]") {
    Document doc;
    auto err = doc.open("tests/fixtures/corrupt.pdf");
    REQUIRE(err.has_value());
    REQUIRE(*err == Document::OpenError::Corrupted);
    REQUIRE_FALSE(doc.is_open());
}

TEST_CASE("Document is safe to use after being moved from", "[document][move]") {
    Document a;
    REQUIRE_FALSE(a.open("tests/fixtures/simple.pdf").has_value());

    Document b = std::move(a);
    REQUIRE(b.is_open());

    // a is moved-from; must be safe to query (returns false) and reusable.
    REQUIRE_FALSE(a.is_open());              // does NOT crash
    REQUIRE_FALSE(a.open("tests/fixtures/simple.pdf").has_value());  // reusable
    REQUIRE(a.is_open());
}
