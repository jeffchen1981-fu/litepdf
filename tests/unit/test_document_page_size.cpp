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

namespace {

// #230: a fresh directory under %TEMP%, removed with everything in it when the
// guard goes. Declared before any document that opens a file in it, so it is
// destroyed after they have closed the file (MuPDF opens it without
// FILE_SHARE_DELETE). The error_code overload: a throwing destructor would
// terminate.
struct TempDir {
    std::filesystem::path path;
    TempDir() {
        path = std::filesystem::temp_directory_path() /
               ("litepdf-230-" + std::to_string(GetCurrentProcessId()) + "-" +
                std::to_string(GetTickCount64()));
        std::filesystem::create_directories(path);
    }
    ~TempDir() { std::error_code ec; std::filesystem::remove_all(path, ec); }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

std::string utf8_of(const std::filesystem::path& p) {
    const auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

// An ePub of `chapters` spine items, each one heading and one sentence (one
// page), written with MuPDF's zip writer (1.27.2 stores every entry). False
// when MuPDF throws.
bool write_many_chapter_epub(const std::filesystem::path& file, int chapters) {
    const std::string sentence =
        "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod "
        "tempor incididunt ut labore et dolore magna aliqua.";
    // Every string is built here, before the fz_try (#179).
    std::vector<std::pair<std::string, std::string>> entries;
    entries.emplace_back("mimetype", "application/epub+zip");
    entries.emplace_back(
        "META-INF/container.xml",
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
        "<rootfiles><rootfile full-path=\"OEBPS/content.opf\" "
        "media-type=\"application/oebps-package+xml\"/></rootfiles></container>\n");
    std::string manifest;
    std::string spine;
    for (int k = 1; k <= chapters; ++k) {
        const std::string id = "c" + std::to_string(k);
        manifest += "<item id=\"" + id + "\" href=\"" + id +
                    ".xhtml\" media-type=\"application/xhtml+xml\"/>\n";
        spine += "<itemref idref=\"" + id + "\"/>\n";
    }
    entries.emplace_back(
        "OEBPS/content.opf",
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\" unique-identifier=\"b\">\n"
        "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>t</dc:title>"
        "<dc:language>en</dc:language><dc:identifier id=\"b\">litepdf-230</dc:identifier></metadata>\n"
        "<manifest>\n" + manifest + "</manifest>\n<spine>\n" + spine + "</spine>\n</package>\n");
    for (int k = 1; k <= chapters; ++k) {
        const std::string n = std::to_string(k);
        entries.emplace_back(
            "OEBPS/c" + n + ".xhtml",
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<html xmlns=\"http://www.w3.org/1999/xhtml\">\n"
            "<head><title>Chapter " + n + "</title></head>\n<body>\n<h1>Chapter " + n +
            "</h1>\n<p>" + sentence + "</p>\n</body>\n</html>\n");
    }
    const std::string name = utf8_of(file);
    const auto* const data = entries.data();
    const std::size_t count = entries.size();

    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx) return false;
    fz_zip_writer* zip = nullptr;
    fz_buffer* buf = nullptr;
    bool ok = true;
    fz_var(zip);
    fz_var(buf);
    fz_try(ctx) {
        zip = fz_new_zip_writer(ctx, name.c_str());
        for (std::size_t i = 0; i < count; ++i) {
            buf = fz_new_buffer_from_copied_data(
                ctx, reinterpret_cast<const unsigned char*>(data[i].second.data()),
                data[i].second.size());
            fz_write_zip_entry(ctx, zip, data[i].first.c_str(), buf, 0);
            fz_drop_buffer(ctx, buf);
            buf = nullptr;
        }
        fz_close_zip_writer(ctx, zip);
    }
    fz_always(ctx) {
        fz_drop_buffer(ctx, buf);
        fz_drop_zip_writer(ctx, zip);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        ok = false;
    }
    fz_drop_context(ctx);
    return ok;
}

// The best of three runs of `run`, in ms.
double best_of_3_ms(const std::function<void()>& run) {
    double best = 1e300;
    for (int r = 0; r < 3; ++r) {
        const auto t0 = std::chrono::steady_clock::now();
        run();
        const auto t1 = std::chrono::steady_clock::now();
        best = std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    return best;
}

// Every page of `own`'s document, loaded by chapter location in chapter order,
// with `use` run on each. False when MuPDF throws.
bool for_each_page_by_location(OwnDocument& own, void (*use)(fz_context*, fz_page*)) {
    fz_context* const ctx = own.ctx;
    fz_document* const doc = own.doc;
    fz_page* page = nullptr;
    bool ok = true;
    fz_var(page);
    fz_try(ctx) {
        const int nc = fz_count_chapters(ctx, doc);
        for (int c = 0; c < nc; ++c) {
            const int m = fz_count_chapter_pages(ctx, doc, c);
            for (int k = 0; k < m; ++k) {
                page = fz_load_chapter_page(ctx, doc, c, k);
                use(ctx, page);
                fz_drop_page(ctx, page);
                page = nullptr;
            }
        }
    }
    fz_always(ctx) { fz_drop_page(ctx, page); }
    fz_catch(ctx) {
        fz_report_error(ctx);
        ok = false;
    }
    return ok;
}

void bound_only(fz_context* ctx, fz_page* page) { (void)fz_bound_page(ctx, page); }

void small_pixmap(fz_context* ctx, fz_page* page) {
    fz_drop_pixmap(ctx, fz_new_pixmap_from_page(ctx, page, fz_scale(0.05f, 0.05f),
                                                fz_device_bgr(ctx), 1));
}

}  // namespace

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
