#include <catch2/catch_test_macros.hpp>
#include "core/Document.hpp"
#include <filesystem>

TEST_CASE("Document opens a PDF with a non-ASCII (CJK) path", "[core][document][unicode]") {
    litepdf::core::Document doc;
    // Using u8path to ensure we hand std::filesystem a UTF-8 encoded source
    // regardless of the compiler's source charset. The escape \u6e2c\u8a66
    // encodes the CJK characters used in the fixture filename.
    std::filesystem::path p = std::filesystem::u8path(u8"tests/fixtures/\u6e2c\u8a66.pdf");
    auto err = doc.open(p);
    REQUIRE(!err.has_value());
    REQUIRE(doc.page_count() > 0);
}
