// #232 cost oracles: every per-page load of a many-chapter ePub goes by its
// chapter location, so a loop over every page costs what loading the pages in
// chapter order costs, not what fz_load_page's walk from the first chapter
// costs. Each test compares against a MuPDF-only loop of the test's own, so the
// bound scales with the machine.
//
// The bound is measured <= kCostK * reference + kCostC. The constants are
// #230's. Planning probe P3 (1,500 chapters, best of 3, pinned to P-cores),
// every page, load + stext:
//   by location   28.76 ms     by flat number  796.63 ms
// load + dehyphenated stext (a scan):
//   by location   29.13 ms     by flat number  808.35 ms
// so the bound is about 140 ms and the flat route fails it by at least 5x,
// while the location route keeps 3x to spare (140.04 / 43.76 = 3.2x, with
// ~15 ms added by the product). A location-route reading with less than 2x
// headroom means k and c need re-deriving, not loosening here.
#include "chapter_test_kit.hpp"
#include "core/Document.hpp"

#include <catch2/catch_test_macros.hpp>

#include <mupdf/fitz.h>

#include <cstddef>
#include <filesystem>
#include <string>

using namespace litepdf::core;
using namespace litepdf::test_kit;

namespace {

constexpr double kCostK = 4.0;
constexpr double kCostC = 25.0;   // ms

// The reference loops' per-page work: the stext build the Document's path runs
// (default options for page_text and text_page, dehyphenated for page_hits).
void stext_with(fz_context* ctx, fz_page* page, int flags) {
    fz_stext_options so = {};
    so.flags = flags;
    fz_drop_stext_page(ctx, fz_new_stext_page_from_page(ctx, page, &so));
}
void stext_default(fz_context* ctx, fz_page* page) { stext_with(ctx, page, 0); }
// The reference for page_hits does the same work it does: the dehyphenated
// stext, then the search for the needle on it. The quad array is plain data and
// nothing here has a destructor, so it is safe inside the caller's fz_try (#179).
void stext_dehyphenate(fz_context* ctx, fz_page* page) {
    fz_stext_options so = {};
    so.flags = FZ_STEXT_DEHYPHENATE;
    fz_stext_page* stext = fz_new_stext_page_from_page(ctx, page, &so);
    fz_quad quads[256];
    (void)fz_search_stext_page(ctx, stext, "Lorem", nullptr, quads, 256);
    fz_drop_stext_page(ctx, stext);
}

}  // namespace

TEST_CASE("Document chapters cost: page_text of every page of a 1500-chapter ePub costs what loading by chapter costs",
          "[document][chapters][cost]") {
    const TempDir dir;   // first: destroyed after both documents
    const std::filesystem::path file = dir.path / "chapters-1500.epub";
    REQUIRE(write_many_chapter_epub(file, 1500));
    Document doc;
    REQUIRE_FALSE(doc.open(file).has_value());
    REQUIRE(doc.page_count() == 1500);
    const std::string file_utf8 = utf8_of(file);
    OwnDocument own(file_utf8.c_str());
    REQUIRE(own.page_count() == 1500);
    const std::size_t n = doc.page_count();

    bool own_ok = true;
    const double ref_ms = best_of_3_ms([&] {
        own_ok = for_each_page_by_location(own, stext_default) && own_ok;
    });
    REQUIRE(own_ok);
    const double ms = best_of_3_ms([&] {
        for (std::size_t i = 0; i < n; ++i) (void)doc.page_text(i);
    });

    INFO("page_text " << ms << " ms against chapter-order stext " << ref_ms << " ms");
    CHECK(ms <= kCostK * ref_ms + kCostC);
    WARN("page_text " << ms << " / " << ref_ms << " ms");
}

TEST_CASE("Document chapters cost: page_hits of every page of a 1500-chapter ePub costs what loading by chapter costs",
          "[document][chapters][cost]") {
    const TempDir dir;
    const std::filesystem::path file = dir.path / "chapters-1500.epub";
    REQUIRE(write_many_chapter_epub(file, 1500));
    Document doc;
    REQUIRE_FALSE(doc.open(file).has_value());
    REQUIRE(doc.page_count() == 1500);
    const std::string file_utf8 = utf8_of(file);
    OwnDocument own(file_utf8.c_str());
    REQUIRE(own.page_count() == 1500);
    const std::size_t n = doc.page_count();

    bool own_ok = true;
    const double ref_ms = best_of_3_ms([&] {
        own_ok = for_each_page_by_location(own, stext_dehyphenate) && own_ok;
    });
    REQUIRE(own_ok);
    const double ms = best_of_3_ms([&] {
        for (std::size_t i = 0; i < n; ++i) (void)doc.page_hits(i, "Lorem", {});
    });

    INFO("page_hits " << ms << " ms against chapter-order dehyphenated stext " << ref_ms << " ms");
    CHECK(ms <= kCostK * ref_ms + kCostC);
    WARN("page_hits " << ms << " / " << ref_ms << " ms");
}

TEST_CASE("Document chapters cost: text_page of every page of a 1500-chapter ePub costs what loading by chapter costs",
          "[document][chapters][cost]") {
    const TempDir dir;
    const std::filesystem::path file = dir.path / "chapters-1500.epub";
    REQUIRE(write_many_chapter_epub(file, 1500));
    Document doc;
    REQUIRE_FALSE(doc.open(file).has_value());
    REQUIRE(doc.page_count() == 1500);
    const std::string file_utf8 = utf8_of(file);
    OwnDocument own(file_utf8.c_str());
    REQUIRE(own.page_count() == 1500);
    const std::size_t n = doc.page_count();

    bool own_ok = true;
    const double ref_ms = best_of_3_ms([&] {
        own_ok = for_each_page_by_location(own, stext_default) && own_ok;
    });
    REQUIRE(own_ok);
    bool valid_ok = true;
    const double ms = best_of_3_ms([&] {
        for (std::size_t i = 0; i < n; ++i) valid_ok = doc.text_page(i).valid() && valid_ok;
    });
    REQUIRE(valid_ok);

    INFO("text_page " << ms << " ms against chapter-order stext " << ref_ms << " ms");
    CHECK(ms <= kCostK * ref_ms + kCostC);
    WARN("text_page " << ms << " / " << ref_ms << " ms");
}
