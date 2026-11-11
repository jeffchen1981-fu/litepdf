// #55 PR 1 Task 10: the strip fixtures behind the page-sizing gate (spec
// section 9.4), written by scripts/generate-strip-fixtures.py. Pins the shape
// the gate's readings depend on, so a regenerated fixture that drifts in page
// count or layout fails here rather than as a quietly cheaper benchmark.
// strip-images.cbz cannot show what a real-size CBZ costs (its images are
// under 1 KB); the CBZ gate is measured on local archives (#176).
// strip-chapters.fb2 was added by #169; there is no XPS fixture (XPS missed the
// gate on a local WPF-written document, spec section 4.4).

#include <catch2/catch_test_macros.hpp>

#include "core/Document.hpp"

using litepdf::core::Document;

TEST_CASE("StripFixtures: the strip fixtures open with the expected page counts",
          "[fixtures][strip]") {
    SECTION("strip-1000.pdf: 1000 pages, page 9 landscape") {
        Document doc;
        REQUIRE_FALSE(doc.open("tests/fixtures/strip-1000.pdf").has_value());
        REQUIRE(doc.page_count() == 1000);
        const auto portrait = doc.page_size(0);
        CHECK(portrait.width_pt == 612.0f);
        CHECK(portrait.height_pt == 792.0f);
        const auto landscape = doc.page_size(9);   // 0-based i % 10 == 9
        CHECK(landscape.width_pt == 792.0f);
        CHECK(landscape.height_pt == 612.0f);
    }
    SECTION("strip-chapters.epub: more pages than its 200 chapters") {
        Document doc;
        REQUIRE_FALSE(doc.open("tests/fixtures/strip-chapters.epub").has_value());
        CHECK(doc.page_count() > 200);
    }
    SECTION("strip-images.cbz: 300 pages") {
        Document doc;
        REQUIRE_FALSE(doc.open("tests/fixtures/strip-images.cbz").has_value());
        CHECK(doc.page_count() == 300);
    }
    SECTION("strip-chapters.fb2: more pages than its 200 sections, all one size") {
        // #169: FB2 is laid out when it opens, so every page has the layout's
        // size and sizing them reads nothing.
        Document doc;
        REQUIRE_FALSE(doc.open("tests/fixtures/strip-chapters.fb2").has_value());
        REQUIRE(doc.page_count() > 200);
        const auto first = doc.page_size(0);
        CHECK(first.width_pt > 0.0f);
        CHECK(first.height_pt > 0.0f);
        const auto last = doc.page_size(doc.page_count() - 1);
        CHECK(last.width_pt == first.width_pt);
        CHECK(last.height_pt == first.height_pt);
    }
}
