// #165: Document::page_size sizes a PDF page from its page object, without
// loading the page. Loading it also loads every annotation and scans the
// resources for transparency, one file read per object not yet loaded, and
// DocumentView sizes up to three pages on the UI thread per page turn.
//
// page-size.pdf comes from scripts/generate-page-size-fixture.py.
#include "chapter_test_kit.hpp"
#include "core/Document.hpp"

#include <catch2/catch_test_macros.hpp>

#include <mupdf/fitz.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <windows.h>

using namespace litepdf::core;
using namespace litepdf::test_kit;

namespace {

ULONGLONG read_operations() {
    IO_COUNTERS io{};
    REQUIRE(GetProcessIoCounters(GetCurrentProcess(), &io));
    return io.ReadOperationCount;
}

}  // namespace

TEST_CASE("Document::page_size applies inherited MediaBox, CropBox, UserUnit and Rotate",
          "[document][pages]") {
    Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/page-size.pdf").has_value());

    // CropBox 540 x 648 points, doubled by UserUnit 2, turned by Rotate 90.
    const PageSize size = doc.page_size(0);
    CHECK(size.width_pt == 1296.0f);
    CHECK(size.height_pt == 1080.0f);
}

TEST_CASE("Document::page_size equals the loaded page's bounds on every fixture",
          "[document][pages]") {
    // Every PDF fixture that opens without a password (the one with a
    // non-ASCII name is a copy of simple.pdf), the ePub, which still loads the
    // page, and the CBZ, sized from its image headers (#176). The same
    // arithmetic runs on the same floats, so the sizes must be equal, not just
    // close.
    const char* const fixtures[] = {
        "tests/fixtures/page-size.pdf",  "tests/fixtures/annot.pdf",
        "tests/fixtures/bookmarks.pdf",  "tests/fixtures/cjk-ja.pdf",
        "tests/fixtures/cjk-ko.pdf",     "tests/fixtures/cjk-zh-hant.pdf",
        "tests/fixtures/large.pdf",      "tests/fixtures/search.pdf",
        "tests/fixtures/selection.pdf",  "tests/fixtures/simple.pdf",
        "tests/fixtures/spread-unequal.pdf", "tests/fixtures/type3.pdf",
        "tests/fixtures/sample.epub",    "tests/fixtures/sample.cbz",
        // #230: 12 chapters of 12 page widths, loaded by chapter location.
        "tests/fixtures/chapter-sizes.epub",
    };
    for (const char* path : fixtures) {
        INFO(path);
        Document doc;
        REQUIRE_FALSE(doc.open(path).has_value());
        OwnDocument own(path);
        const int n = own.page_count();
        REQUIRE(n == static_cast<int>(doc.page_count()));
        for (int i = 0; i < n; ++i) {
            INFO("page " << i);
            fz_rect r{};
            REQUIRE(own.loaded_bounds(i, r));
            const PageSize size = doc.page_size(static_cast<std::size_t>(i));
            CHECK(size.width_pt == r.x1 - r.x0);
            CHECK(size.height_pt == r.y1 - r.y0);
        }
    }
}

TEST_CASE("Document::page_size reads nothing from the file", "[document][pages]") {
    const char* const path = "tests/fixtures/page-size.pdf";

    // Control: loading page 2 reads its annotations and image from the file.
    // If this stops reading, the fixture no longer tests anything. Page 1
    // loads first so that MuPDF's page-tree map, which a Document builds
    // while opening, is not what the reads come from.
    {
        OwnDocument own(path);
        fz_rect r{};
        REQUIRE(own.loaded_bounds(0, r));
        const ULONGLONG before = read_operations();
        REQUIRE(own.loaded_bounds(1, r));
        REQUIRE(read_operations() > before);
    }

    Document doc;
    REQUIRE_FALSE(doc.open(path).has_value());
    const ULONGLONG before = read_operations();
    const PageSize size = doc.page_size(1);
    CHECK(read_operations() == before);
    CHECK(size.width_pt == 595.0f);
    CHECK(size.height_pt == 842.0f);
}

TEST_CASE("Document::page_size past the last page of a multi-chapter ePub throws",
          "[document][pages]") {
    // #230: an index the chapter table does not hold falls back to
    // fz_load_page, which refuses it as before.
    Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/chapter-sizes.epub").has_value());
    CHECK_THROWS_AS(doc.page_size(doc.page_count()), std::runtime_error);
}

TEST_CASE("Document::with_rendered_page draws each page of a multi-chapter ePub as fz_load_page does",
          "[document][pages]") {
    // #230: printing loads a page by its chapter location. The bytes, not
    // only the size: a page from the right chapter but the wrong page within
    // it has the right size and different text.
    const char* const path = "tests/fixtures/chapter-sizes.epub";
    Document doc;
    REQUIRE_FALSE(doc.open(path).has_value());
    OwnDocument own(path);
    const int n = own.page_count();
    REQUIRE(n == static_cast<int>(doc.page_count()));
    for (int i = 0; i < n; ++i) {
        INFO("page " << i);
        std::vector<std::uint8_t> want;
        int want_w = 0, want_h = 0;
        REQUIRE(own.rendered(i, want, want_w, want_h));

        std::vector<std::uint8_t> got;
        int got_w = -1, got_h = -1;
        REQUIRE(doc.with_rendered_page(
            static_cast<std::size_t>(i), 1.0f, 1.0f, false,
            [&](int w, int h, const std::uint8_t* bgra) {
                got_w = w;
                got_h = h;
                got.assign(bgra, bgra + static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
            }));
        REQUIRE(got_w == want_w);
        REQUIRE(got_h == want_h);
        const auto diff = std::mismatch(got.begin(), got.end(), want.begin());
        INFO("first differing byte " << (diff.first - got.begin()) << " of " << got.size());
        CHECK(diff.first == got.end());
    }
}

TEST_CASE("Document on a 1500-chapter ePub: sizing and printing every page cost what loading by chapter costs",
          "[document][pages]") {
    // #230: fz_load_page walks the chapters from the first for every page,
    // and an ePub's chapter page count walks the spine from its head, so
    // sizing or printing every page by flat number grows with the cube of the
    // chapter count (about 1 s here). By chapter location it costs what the
    // test's own chapter-order loop costs. The bound is relative, so it scales
    // with the machine.
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
    const double ref_size_ms = best_of_3_ms([&] {
        own_ok = for_each_page_by_location(own, bound_only) && own_ok;
    });
    const double ref_render_ms = best_of_3_ms([&] {
        own_ok = for_each_page_by_location(own, small_pixmap) && own_ok;
    });
    REQUIRE(own_ok);

    const double size_ms = best_of_3_ms([&] {
        for (std::size_t i = 0; i < n; ++i) (void)doc.page_size(i);
    });
    bool rendered_ok = true;
    const double render_ms = best_of_3_ms([&] {
        for (std::size_t i = 0; i < n; ++i) {
            rendered_ok = doc.with_rendered_page(i, 0.05f, 0.05f, false,
                                                 [](int, int, const std::uint8_t*) {}) &&
                          rendered_ok;
        }
    });
    REQUIRE(rendered_ok);

    INFO("page_size " << size_ms << " ms against chapter-order bounds " << ref_size_ms << " ms");
    INFO("with_rendered_page " << render_ms << " ms against chapter-order render " << ref_render_ms
                               << " ms");
    CHECK(size_ms < 4.0 * ref_size_ms + 25.0);
    CHECK(render_ms < 4.0 * ref_render_ms + 25.0);
    // The readings, for the record, also when everything passes.
    WARN("page_size " << size_ms << " / " << ref_size_ms << " ms; with_rendered_page "
                      << render_ms << " / " << ref_render_ms << " ms");
}
