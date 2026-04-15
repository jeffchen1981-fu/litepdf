#include "core/Document.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace litepdf::core;

TEST_CASE("Document::outline returns entries for bookmarks.pdf", "[document][outline]") {
    Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/bookmarks.pdf").has_value());

    auto entries = doc.outline();
    REQUIRE(entries.size() >= 3);

    // Expect at least one non-root-depth entry (nested item)
    bool has_nested = false;
    for (const auto& e : entries) {
        if (e.depth > 0) { has_nested = true; break; }
    }
    REQUIRE(has_nested);

    // Titles are non-empty
    for (const auto& e : entries) {
        REQUIRE(!e.title.empty());
    }
}

TEST_CASE("Document::outline returns empty for simple.pdf (no outline)", "[document][outline]") {
    Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());

    auto entries = doc.outline();
    // simple.pdf has no outline
    REQUIRE(entries.empty());
}
