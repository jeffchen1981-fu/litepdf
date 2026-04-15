#include "core/Document.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

using namespace litepdf::core;

TEST_CASE("Document::source_path returns path passed to open",
          "[core][document][accessor]") {
    Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());
    REQUIRE(doc.source_path() == std::filesystem::path("tests/fixtures/simple.pdf"));
}

TEST_CASE("Document::source_path is empty before open",
          "[core][document][accessor]") {
    Document doc;
    REQUIRE(doc.source_path().empty());
}

TEST_CASE("Document::source_path clears on close",
          "[core][document][accessor]") {
    Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());
    REQUIRE_FALSE(doc.source_path().empty());
    doc.close();
    REQUIRE(doc.source_path().empty());
}
