// #176: a CBZ page's size comes from its image header, not from loading the
// page (which reads the whole image out of the archive). Every size the header
// path gives must be bitwise equal to fz_load_page + fz_bound_page on the same
// page, and every page it declines falls back to that route.
//
// cbz-sizing.cbz comes from scripts/generate-cbz-sizing-fixture.py; its entries
// and what each one covers are listed there and in the spec,
// docs/superpowers/specs/2026-10-25-cbz-header-sizing-176-design.md, section 5.
#include "core/Document.hpp"
#include "core/detail/CbzPageSizer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <mupdf/fitz.h>

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>

#include <windows.h>

using namespace litepdf::core;
namespace fs = std::filesystem;

namespace {

constexpr const char* kSizing = "tests/fixtures/cbz-sizing.cbz";
constexpr const char* kUnhandled = "UNHANDLED EXCEPTION!";

// cbz-sizing.cbz pages the header path declines (MuPDF's page order): the JPEG
// whose SOS lies past 512 KB, the TIFF, and the JPEG MuPDF cannot decode.
const std::set<std::size_t> kSizingFallbacks = {4, 11, 12};
constexpr std::size_t kSizingPages = 16;
constexpr std::size_t kSizingLargeJpeg = 3;    // p04_large.jpg
constexpr std::size_t kSizingSosPast512k = 4;  // p05_sos_past_512k.jpg
constexpr std::size_t kSizingBroken = 12;      // p11_broken.jpg

std::string utf8(const fs::path& p) {
#if defined(__cpp_lib_char8_t) && __cpp_lib_char8_t >= 201907L
    const std::u8string u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
#else
    return p.u8string();
#endif
}

// A MuPDF context and document of the test's own, for the full-read sizes and
// for calling the sizer directly. Collects the context's warnings.
struct OwnDocument {
    fz_context*  ctx = nullptr;
    fz_document* doc = nullptr;
    std::string  path;
    std::string  warnings;

    explicit OwnDocument(std::string utf8_path) : path(std::move(utf8_path)) {
        ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
        REQUIRE(ctx != nullptr);
        fz_set_warning_callback(ctx, [](void* user, const char* message) {
            *static_cast<std::string*>(user) += std::string(message) + "\n";
        }, &warnings);
        fz_register_document_handlers(ctx);
        fz_try(ctx) { doc = fz_open_document(ctx, path.c_str()); }
        fz_catch(ctx) { fz_report_error(ctx); doc = nullptr; }
        REQUIRE(doc != nullptr);
    }
    ~OwnDocument() {
        fz_drop_document(ctx, doc);
        fz_drop_context(ctx);
    }
    OwnDocument(const OwnDocument&) = delete;
    OwnDocument& operator=(const OwnDocument&) = delete;

    std::size_t page_count() {
        int n = -1;
        fz_var(n);   // read after a catch that does not leave (#178)
        fz_try(ctx) { n = fz_count_pages(ctx, doc); }
        fz_catch(ctx) { fz_report_error(ctx); }
        REQUIRE(n >= 0);
        return static_cast<std::size_t>(n);
    }

    // fz_load_page + fz_bound_page, as Document::page_size computes it.
    PageSize loaded_size(std::size_t index) {
        fz_page* page = nullptr;
        fz_rect r{};
        bool ok = true;
        fz_var(page);
        fz_try(ctx) {
            page = fz_load_page(ctx, doc, static_cast<int>(index));
            r = fz_bound_page(ctx, page);
        }
        fz_always(ctx) { fz_drop_page(ctx, page); }
        fz_catch(ctx) { fz_report_error(ctx); ok = false; }
        REQUIRE(ok);
        PageSize s;
        s.width_pt = r.x1 - r.x0;
        s.height_pt = r.y1 - r.y0;
        return s;
    }

    std::unique_ptr<detail::CbzPageSizer> sizer(std::size_t page_count) {
        return detail::CbzPageSizer::create(ctx, doc, path.c_str(), page_count);
    }

    // Throws and catches a fresh error: a code a call before it left set is
    // reported as "UNHANDLED EXCEPTION!" (#146).
    void throw_on_context() {
        bool caught = false;
        fz_try(ctx) { fz_throw(ctx, FZ_ERROR_GENERIC, "#146 probe"); }
        fz_catch(ctx) {
            caught = true;
            fz_convert_error(ctx, nullptr);
        }
        REQUIRE(caught);
    }
};

bool same_bits(const PageSize& a, const PageSize& b) {
    return std::memcmp(&a.width_pt, &b.width_pt, sizeof(float)) == 0 &&
           std::memcmp(&a.height_pt, &b.height_pt, sizeof(float)) == 0;
}

// A copy of `fixture` under the temp directory, at `name`.
fs::path temp_copy(const char* fixture, const fs::path& dir_name, const char* name) {
    const fs::path dir = fs::temp_directory_path() / dir_name;
    fs::create_directories(dir);
    const fs::path out = dir / name;
    fs::copy_file(fixture, out, fs::copy_options::overwrite_existing);
    return out;
}

ULONGLONG read_operations() {
    IO_COUNTERS io{};
    REQUIRE(GetProcessIoCounters(GetCurrentProcess(), &io));
    return io.ReadOperationCount;
}

}  // namespace

TEST_CASE("CbzPageSizer: create applies only to a CBZ with a matching page list",
          "[core][cbz]") {
    {
        OwnDocument own(kSizing);
        REQUIRE(own.page_count() == kSizingPages);
        CHECK(own.sizer(kSizingPages) != nullptr);
        // Guard 2 alone: the list matches the archive, the count does not.
        CHECK(own.sizer(kSizingPages + 1) == nullptr);
    }
    {
        OwnDocument own("tests/fixtures/sample.cbz");
        CHECK(own.sizer(own.page_count()) != nullptr);
    }
    {
        // No archive to open.
        OwnDocument own("tests/fixtures/simple.pdf");
        CHECK(own.sizer(own.page_count()) == nullptr);
    }
    {
        // Guard 1 alone: MuPDF opens the renamed EPUB with its EPUB handler; the
        // EPUB has no image entries, so a page count of 0 matches the rebuilt list.
        const fs::path renamed = temp_copy("tests/fixtures/sample.epub",
                                           "litepdf-cbz-176", "renamed.cbz");
        OwnDocument own(utf8(renamed));
        CHECK(own.sizer(0) == nullptr);
    }
}

TEST_CASE("CbzPageSizer: header sizes equal the loaded page's bounds bit for bit",
          "[core][cbz]") {
    struct Case { const char* path; std::set<std::size_t> fallbacks; };
    const Case cases[] = {
        {kSizing, kSizingFallbacks},
        {"tests/fixtures/sample.cbz", {}},
        {"tests/fixtures/strip-images.cbz", {}},
    };
    for (const Case& c : cases) {
        INFO(c.path);
        OwnDocument own(c.path);
        const std::size_t n = own.page_count();
        auto sizer = own.sizer(n);
        REQUIRE(sizer != nullptr);
        for (std::size_t i = 0; i < n; ++i) {
            INFO("page " << i);
            const std::optional<PageSize> header = sizer->header_size(i);
            if (c.fallbacks.count(i)) {
                CHECK_FALSE(header.has_value());
            } else {
                REQUIRE(header.has_value());
                const PageSize loaded = own.loaded_size(i);
                INFO("header " << header->width_pt << " x " << header->height_pt
                     << ", loaded " << loaded.width_pt << " x " << loaded.height_pt);
                CHECK(same_bits(*header, loaded));
            }
        }
    }
}

TEST_CASE("CbzPageSizer: Document::page_size equals the loaded page's bounds",
          "[core][cbz]") {
    const fs::path renamed = temp_copy("tests/fixtures/sample.epub",
                                       "litepdf-cbz-176", "renamed.cbz");
    const std::string paths[] = {
        kSizing, "tests/fixtures/sample.cbz", "tests/fixtures/strip-images.cbz",
        utf8(renamed),
    };
    for (const std::string& path : paths) {
        INFO(path);
        Document doc;
        REQUIRE_FALSE(doc.open(fs::u8path(path)).has_value());
        OwnDocument own(path);
        REQUIRE(own.page_count() == doc.page_count());
        // As the strip sizes them: after the read-ahead (#176, spec 3.8).
        (void)doc.read_ahead_page_sizes();
        for (std::size_t i = 0; i < doc.page_count(); ++i) {
            INFO("page " << i);
            CHECK(same_bits(doc.page_size(i), own.loaded_size(i)));
        }
    }
}

TEST_CASE("CbzPageSizer: a header size reads a few blocks, not the whole image",
          "[core][cbz]") {
    // Control: loading p04_large.jpg (>= 512 KB, stored) reads the whole entry.
    // If this stops reading, the fixture no longer tests anything.
    {
        OwnDocument own(kSizing);
        (void)own.loaded_size(0);
        const ULONGLONG before = read_operations();
        (void)own.loaded_size(kSizingLargeJpeg);
        CHECK(read_operations() - before >= 60);
    }

    Document doc;
    REQUIRE_FALSE(doc.open(kSizing).has_value());
    (void)doc.page_size(0);   // the probe opens the archive here
    const ULONGLONG before = read_operations();
    (void)doc.page_size(kSizingLargeJpeg);
    CHECK(read_operations() - before <= 20);
}

TEST_CASE("CbzPageSizer: a UTF-8 path with CJK characters", "[core][cbz]") {
    const fs::path copy = temp_copy(kSizing, fs::path(L"litepdf-cbz-\u6e2c\u8a66"),
                                    "cbz-sizing.cbz");
    OwnDocument own(utf8(copy));
    auto sizer = own.sizer(own.page_count());
    REQUIRE(sizer != nullptr);
    const std::optional<PageSize> header = sizer->header_size(0);
    REQUIRE(header.has_value());
    CHECK(same_bits(*header, own.loaded_size(0)));

    Document doc;
    REQUIRE_FALSE(doc.open(copy).has_value());
    CHECK(same_bits(doc.page_size(0), own.loaded_size(0)));
}

TEST_CASE("CbzPageSizer: declined pages leave no MuPDF error behind", "[core][cbz]") {
    OwnDocument own(kSizing);
    auto sizer = own.sizer(own.page_count());
    REQUIRE(sizer != nullptr);

    CHECK_FALSE(sizer->header_size(kSizingBroken).has_value());
    own.throw_on_context();
    CHECK_FALSE(sizer->header_size(kSizingSosPast512k).has_value());
    own.throw_on_context();
    {
        // The archive open fails inside create; checked on this context.
        auto none = detail::CbzPageSizer::create(own.ctx, own.doc,
                                                 "tests/fixtures/simple.pdf", 1);
        CHECK(none == nullptr);
    }
    own.throw_on_context();

    INFO("warnings:\n" << own.warnings);
    CHECK(own.warnings.find(kUnhandled) == std::string::npos);
}

TEST_CASE("CbzPageSizer: close and open size the new document", "[core][cbz]") {
    Document doc;
    REQUIRE_FALSE(doc.open(kSizing).has_value());
    (void)doc.page_size(0);
    doc.close();

    REQUIRE_FALSE(doc.open("tests/fixtures/sample.cbz").has_value());
    OwnDocument own("tests/fixtures/sample.cbz");
    REQUIRE(own.page_count() == doc.page_count());
    for (std::size_t i = 0; i < doc.page_count(); ++i) {
        INFO("page " << i);
        CHECK(same_bits(doc.page_size(i), own.loaded_size(i)));
    }
}

TEST_CASE("CbzPageSizer: close releases the archive's file", "[core][cbz]") {
    // MuPDF opens files without FILE_SHARE_DELETE, so the copy cannot be
    // removed while the sizer's archive still holds it.
    const fs::path copy = temp_copy(kSizing, "litepdf-cbz-176-close", "close.cbz");
    {
        Document doc;
        REQUIRE_FALSE(doc.open(copy).has_value());
        (void)doc.page_size(0);
        doc.close();
        std::error_code ec;
        CHECK(fs::remove(copy, ec));
        INFO(ec.message());
        CHECK_FALSE(ec);
    }
    std::error_code ignored;
    fs::remove(copy, ignored);
}

TEST_CASE("CbzPageSizer: a page past the end", "[core][cbz]") {
    OwnDocument own(kSizing);
    auto sizer = own.sizer(kSizingPages);
    REQUIRE(sizer != nullptr);
    CHECK_FALSE(sizer->header_size(kSizingPages).has_value());

    Document doc;
    REQUIRE_FALSE(doc.open(kSizing).has_value());
    CHECK_THROWS_AS(doc.page_size(kSizingPages), std::runtime_error);
}
