// #55 PR 1 Task 10: the strip fixtures behind the page-sizing gate (spec
// section 9.4), written by scripts/generate-strip-fixtures.py. Pins the shape
// the gate's readings depend on, so a regenerated fixture that drifts in page
// count or layout fails here rather than as a quietly cheaper benchmark.
// strip-images.cbz cannot show a CBZ miss (its images are under 1 KB, and
// sizing a CBZ page reads its whole image): CBZ stays paged, #176.

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
}
