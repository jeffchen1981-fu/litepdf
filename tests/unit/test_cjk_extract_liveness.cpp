// Liveness: the three CJK fixtures still open and extract text after the embedded
// CJK font is dropped (TOFU_CJK) and CJK is system-resolved. Extraction is
// ToUnicode/CMap-based (font-independent), so this stays green on any runner
// regardless of which CJK fonts it has. Glyph-rendering fidelity is checked via
// the CLI render + manual visual vs the reference PNGs (Step 4), not in CI.
#include "core/Document.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

using litepdf::core::Document;

namespace {
void require_opens_and_extracts(const char* fixture) {
    Document doc;
    REQUIRE_FALSE(doc.open(std::filesystem::path(fixture)).has_value());
    REQUIRE(doc.page_count() >= 1);
    std::string text = doc.page_text(0);  // must not throw post-prune
    (void)text;                            // content varies across fixtures
    SUCCEED();
}
}  // namespace

TEST_CASE("CJK fixtures open + extract without crashing (post-TOFU_CJK)",
          "[core][fonts][cjk][liveness]") {
    require_opens_and_extracts("tests/fixtures/cjk-zh-hant.pdf");
    require_opens_and_extracts("tests/fixtures/cjk-ja.pdf");
    require_opens_and_extracts("tests/fixtures/cjk-ko.pdf");
}
