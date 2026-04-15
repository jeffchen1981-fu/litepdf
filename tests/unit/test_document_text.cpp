#include "core/Document.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace litepdf::core;

TEST_CASE("Document::page_text returns non-empty text for simple.pdf", "[document][text]") {
    Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());

    std::string text = doc.page_text(0);
    INFO("Extracted: " << text);
    REQUIRE(!text.empty());
    REQUIRE(text.size() > 10);
}
