// #165: Document::page_size sizes a PDF page from its page object, without
// loading the page. Loading it also loads every annotation and scans the
// resources for transparency, one file read per object not yet loaded, and
// DocumentView sizes up to three pages on the UI thread per page turn.
//
// page-size.pdf comes from scripts/generate-page-size-fixture.py.
#include "core/Document.hpp"

#include <catch2/catch_test_macros.hpp>

#include <mupdf/fitz.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <windows.h>

using namespace litepdf::core;

namespace {

// A MuPDF context and document of the test's own, to compute what loading
// the page gives: fz_load_page + fz_bound_page, the route page_size used to
// take.
struct OwnDocument {
    fz_context*  ctx = nullptr;
    fz_document* doc = nullptr;

    explicit OwnDocument(const char* path) {
        ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
        REQUIRE(ctx != nullptr);
        fz_register_document_handlers(ctx);
        fz_try(ctx) { doc = fz_open_document(ctx, path); }
        fz_catch(ctx) { fz_report_error(ctx); doc = nullptr; }
        REQUIRE(doc != nullptr);
    }
    ~OwnDocument() {
        fz_drop_document(ctx, doc);
        fz_drop_context(ctx);
    }
    OwnDocument(const OwnDocument&) = delete;
    OwnDocument& operator=(const OwnDocument&) = delete;

    int page_count() {
        int n = -1;
        fz_var(n);
        fz_try(ctx) { n = fz_count_pages(ctx, doc); }
        fz_catch(ctx) { fz_report_error(ctx); }
        return n;
    }

    // False when the page does not load.
    bool loaded_bounds(int index, fz_rect& out) {
        fz_page* page = nullptr;
        bool ok = true;
        fz_var(page);
        fz_try(ctx) {
            page = fz_load_page(ctx, doc, index);
            out = fz_bound_page(ctx, page);
        }
        fz_always(ctx) { fz_drop_page(ctx, page); }
        fz_catch(ctx) { fz_report_error(ctx); ok = false; }
        return ok;
    }

    // #230: what Document::with_rendered_page hands its callback for
    // fz_load_page(index) at scale 1, no rotation: the same MuPDF calls in the
    // same order, then the same over-white compositing. False when any step
    // throws.
    bool rendered(int index, std::vector<std::uint8_t>& bgra, int& w, int& h) {
        fz_page* page = nullptr;
        fz_pixmap* pix = nullptr;
        fz_device* dev = nullptr;
        bool ok = true;
        fz_var(page);
        fz_var(pix);
        fz_var(dev);
        fz_try(ctx) {
            page = fz_load_page(ctx, doc, index);
            const fz_matrix m = fz_scale(1.0f, 1.0f);
            const fz_irect bbox = fz_round_rect(fz_transform_rect(fz_bound_page(ctx, page), m));
            pix = fz_new_pixmap_with_bbox(ctx, fz_device_bgr(ctx), bbox, nullptr, 1);
            fz_clear_pixmap_with_value(ctx, pix, 0xFF);
            dev = fz_new_draw_device(ctx, fz_identity, pix);
            fz_run_page(ctx, page, dev, m, nullptr);
            fz_close_device(ctx, dev);
        }
        fz_always(ctx) {
            if (dev) fz_drop_device(ctx, dev);
            if (page) fz_drop_page(ctx, page);
        }
        fz_catch(ctx) { fz_report_error(ctx); ok = false; }
        if (!ok) {
            if (pix) fz_drop_pixmap(ctx, pix);
            return false;
        }
        w = fz_pixmap_width(ctx, pix);
        h = fz_pixmap_height(ctx, pix);
        const std::uint8_t* samples = fz_pixmap_samples(ctx, pix);
        const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4;
        bgra.assign(samples, samples + n);
        fz_drop_pixmap(ctx, pix);
        for (std::size_t i = 0; i < n; i += 4) {
            std::uint8_t* p = bgra.data() + i;
            const int a = p[3];
            if (a == 255) continue;
            const int inv = 255 - a;
            p[0] = static_cast<std::uint8_t>(std::min(255, p[0] + inv));
            p[1] = static_cast<std::uint8_t>(std::min(255, p[1] + inv));
            p[2] = static_cast<std::uint8_t>(std::min(255, p[2] + inv));
            p[3] = 255;
        }
        return true;
    }
};

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
