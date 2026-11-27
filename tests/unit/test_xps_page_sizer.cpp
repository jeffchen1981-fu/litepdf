// #227: an XPS page's size comes from the root start tag of its FixedPage part,
// not from loading the page. Every size the root-tag route gives must be
// bitwise equal to fz_load_page + fz_bound_page on the same page.
//
// xps-sizing.xps and xps-escaped-name.xps come from
// scripts/generate-xps-sizing-fixture.py; each page and what it covers are
// listed there and in the spec,
// docs/superpowers/specs/2026-11-26-xps-header-sizing-227-design.md, section 5.1.
#include "core/Document.hpp"
#include "core/detail/XpsPageSizer.hpp"
#include "core/detail/ZipReadAhead.hpp"

#include "chapter_test_kit.hpp"   // utf8_of

#include <catch2/catch_test_macros.hpp>

#include <mupdf/fitz.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <windows.h>

using namespace litepdf::core;
namespace fs = std::filesystem;
using litepdf::test_kit::utf8_of;

namespace {

constexpr const char* kSizing = "tests/fixtures/xps-sizing.xps";
constexpr const char* kEscaped = "tests/fixtures/xps-escaped-name.xps";
constexpr int kSizingPages = 15;
constexpr int kEscapedPages = 3;
constexpr int kNoHeightPage = 9;   // p09: no Height, MuPDF's load throws
constexpr int kDuplicatePage = 13; // p13: two entries differing in case, MuPDF takes the first
constexpr const char* kUnhandled = "UNHANDLED EXCEPTION!";

using Route = detail::XpsPageSizer::Route;

// One xps-sizing.xps page. `direct`: the root-tag route sizes it once
// prepare_all has run (true), or declines it (false); before prepare_all a
// Direct row takes the Stream route.
struct XpsRow {
    int  page;     // 1-based, the pNN of the generator
    bool direct;
};

constexpr XpsRow kRows[kSizingPages] = {
    {1, true},  {2, true},  {3, true},  {4, true},  {5, true},
    {6, false}, {7, false}, {8, false}, {9, false}, {10, false},
    {11, true}, {12, false}, {13, true}, {14, true}, {15, true},
};

// The Width x Height of each page's root tag, in the units of the file (1/96 in).
struct XpsDims { int w; int h; };

constexpr XpsDims kSizingDims[kSizingPages] = {
    {816, 1056}, {612, 792}, {1000, 700}, {816, 500}, {500, 400},
    {640, 480},  {700, 900}, {720, 960},  {800, 0},   {610, 810},
    {620, 820},  {630, 830}, {300, 300},  {640, 840}, {650, 850},
};

constexpr XpsDims kEscapedDims[kEscapedPages] = {
    {300, 200}, {400, 300}, {500, 400},
};

// A MuPDF context and document of the test's own, to compute what loading the
// page gives. Collects the context's warnings.
struct OwnDocument {
    fz_context*  ctx = nullptr;
    fz_document* doc = nullptr;
    std::string  path;
    std::string  warnings;

    explicit OwnDocument(std::string utf8_path) : path(std::move(utf8_path)) {
        ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
        REQUIRE(ctx != nullptr);
        // noexcept: MuPDF calls it from its C frames (#179).
        fz_set_warning_callback(ctx, [](void* user, const char* message) noexcept {
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

    std::unique_ptr<detail::XpsPageSizer> sizer(std::size_t page_count) {
        return detail::XpsPageSizer::create(ctx, doc, path.c_str(), page_count);
    }
};

// fz_load_page + fz_bound_page on the test's own context, as Document::page_size
// computes it. `*threw` is set when MuPDF throws (the size is then zero).
PageSize loaded_size(OwnDocument& own, int index, bool* threw) {
    fz_context* ctx = own.ctx;
    fz_page* page = nullptr;
    fz_rect r{};
    bool ok = true;
    fz_var(page);
    fz_try(ctx) {
        page = fz_load_page(ctx, own.doc, index);
        r = fz_bound_page(ctx, page);
    }
    fz_always(ctx) { fz_drop_page(ctx, page); }
    fz_catch(ctx) {
        // Marks the error handled; a later throw on this context starts clean.
        fz_convert_error(ctx, nullptr);
        ok = false;
    }
    *threw = !ok;
    PageSize s{0.0f, 0.0f};
    if (ok) {
        s.width_pt = r.x1 - r.x0;
        s.height_pt = r.y1 - r.y0;
    }
    return s;
}

bool same_bits(const PageSize& a, const PageSize& b) {
    return std::memcmp(&a.width_pt, &b.width_pt, sizeof(float)) == 0 &&
           std::memcmp(&a.height_pt, &b.height_pt, sizeof(float)) == 0;
}

// The size the spec's arithmetic gives for a root tag's Width and Height.
PageSize table_size(const XpsDims& d) {
    return PageSize{d.w * 72.0f / 96.0f - 0.0f, d.h * 72.0f / 96.0f - 0.0f};
}

// A FixedPage part whose root carries `quoted` attributes holding a '>' before
// Width, so each of the first `quoted` cuts ends inside a value and throws.
std::string page_with_quoted_gt(int quoted, const XpsDims& d) {
    std::string s = "<FixedPage xmlns=\"http://schemas.microsoft.com/xps/2005/06\"";
    for (int k = 1; k <= quoted; ++k) s += " q" + std::to_string(k) + "=\"x>y\"";
    s += " Width=\"" + std::to_string(d.w) + "\" Height=\"" + std::to_string(d.h) +
         "\"></FixedPage>";
    return s;
}

// Writes an XPS of one FixedDocument with `pages` (whole FixedPage parts, as
// Documents/1/Pages/<n>.fpage) to `utf8_path`, with MuPDF's zip writer. The
// parts are built before the fz_try: no object with a destructor inside it (#179).
void write_xps(const std::string& utf8_path, const std::vector<std::string>& pages) {
    std::vector<std::pair<std::string, std::string>> parts;
    parts.emplace_back("_rels/.rels",
                       "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                       "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/"
                       "relationships\"><Relationship Id=\"R1\" Type=\"http://schemas.microsoft"
                       ".com/xps/2005/06/fixedrepresentation\" "
                       "Target=\"/FixedDocumentSequence.fdseq\"/></Relationships>");
    parts.emplace_back("FixedDocumentSequence.fdseq",
                       "<FixedDocumentSequence xmlns=\"http://schemas.microsoft.com/xps/2005/06\">"
                       "<DocumentReference Source=\"/Documents/1/FixedDocument.fdoc\"/>"
                       "</FixedDocumentSequence>");
    std::string fdoc = "<FixedDocument xmlns=\"http://schemas.microsoft.com/xps/2005/06\">";
    for (std::size_t i = 0; i < pages.size(); ++i) {
        fdoc += "<PageContent Source=\"/Documents/1/Pages/" + std::to_string(i + 1) +
                ".fpage\"/>";
    }
    fdoc += "</FixedDocument>";
    parts.emplace_back("Documents/1/FixedDocument.fdoc", fdoc);
    for (std::size_t i = 0; i < pages.size(); ++i) {
        parts.emplace_back("Documents/1/Pages/" + std::to_string(i + 1) + ".fpage", pages[i]);
    }
    const std::pair<std::string, std::string>* const part = parts.data();
    const std::size_t count = parts.size();

    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    REQUIRE(ctx != nullptr);
    fz_zip_writer* zip = nullptr;
    fz_buffer* buf = nullptr;
    bool ok = true;
    fz_var(zip);
    fz_var(buf);
    fz_try(ctx) {
        zip = fz_new_zip_writer(ctx, utf8_path.c_str());
        for (std::size_t i = 0; i < count; ++i) {
            buf = fz_new_buffer_from_copied_data(
                ctx, reinterpret_cast<const unsigned char*>(part[i].second.data()),
                part[i].second.size());
            fz_write_zip_entry(ctx, zip, part[i].first.c_str(), buf, 1);
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
    REQUIRE(ok);
}

// Removes the file at `path` when it goes out of scope.
struct TempFile {
    fs::path path;
    ~TempFile() {
        std::error_code ec;
        fs::remove(path, ec);
    }
};

}  // namespace

TEST_CASE("XpsPageSizer: the fixtures load as the table says", "[core][xps]") {
    SECTION("xps-sizing.xps") {
        OwnDocument own(kSizing);
        REQUIRE(own.page_count() == static_cast<std::size_t>(kSizingPages));
        for (const XpsRow& row : kRows) {
            CAPTURE(row.page);
            bool threw = false;
            const PageSize got = loaded_size(own, row.page - 1, &threw);
            if (row.page == kNoHeightPage) {
                CHECK(threw);
                own.throw_on_context();
                continue;
            }
            REQUIRE_FALSE(threw);
            const PageSize want = table_size(kSizingDims[row.page - 1]);
            CHECK(same_bits(got, want));
        }
    }
    SECTION("xps-escaped-name.xps") {
        OwnDocument own(kEscaped);
        REQUIRE(own.page_count() == static_cast<std::size_t>(kEscapedPages));
        for (int i = 0; i < kEscapedPages; ++i) {
            CAPTURE(i);
            bool threw = false;
            const PageSize got = loaded_size(own, i, &threw);
            REQUIRE_FALSE(threw);
            CHECK(same_bits(got, table_size(kEscapedDims[i])));
        }
    }
}

TEST_CASE("XpsPageSizer: create applies only to an XPS with a matching page list",
          "[core][xps]") {
    SECTION("xps-sizing.xps") {
        OwnDocument own(kSizing);
        CHECK(own.sizer(kSizingPages) != nullptr);
        CHECK(own.sizer(kSizingPages - 1) == nullptr);
        CHECK(own.sizer(kSizingPages + 1) == nullptr);
    }
    SECTION("xps-escaped-name.xps") {
        OwnDocument own(kEscaped);
        CHECK(own.sizer(kEscapedPages) != nullptr);
    }
    SECTION("simple.pdf, another handler") {
        OwnDocument own("tests/fixtures/simple.pdf");
        CHECK(own.sizer(own.page_count()) == nullptr);
    }
}

TEST_CASE("XpsPageSizer: before prepare_all every page takes the stream route or none",
          "[core][xps]") {
    SECTION("xps-sizing.xps") {
        OwnDocument own(kSizing);
        auto sizer = own.sizer(kSizingPages);
        REQUIRE(sizer != nullptr);
        for (const XpsRow& row : kRows) {
            CAPTURE(row.page);
            Route route = Route::Direct;   // anything but what is expected
            const std::optional<PageSize> got =
                sizer->header_size(static_cast<std::size_t>(row.page - 1), &route);
            if (!row.direct) {
                CHECK_FALSE(got.has_value());
                CHECK(route == Route::None);
                continue;
            }
            CHECK(route == Route::Stream);
            REQUIRE(got.has_value());
            bool threw = false;
            const PageSize want = loaded_size(own, row.page - 1, &threw);
            REQUIRE_FALSE(threw);
            CHECK(same_bits(*got, want));
            if (row.page == kDuplicatePage) {
                CHECK(same_bits(*got, table_size(XpsDims{300, 300})));
            }
        }
        Route past = Route::Stream;
        CHECK_FALSE(sizer->header_size(kSizingPages, &past).has_value());
        CHECK(past == Route::None);
    }
    SECTION("xps-escaped-name.xps") {
        OwnDocument own(kEscaped);
        auto sizer = own.sizer(kEscapedPages);
        REQUIRE(sizer != nullptr);
        for (int i = 0; i < kEscapedPages; ++i) {
            CAPTURE(i);
            Route route = Route::None;
            const std::optional<PageSize> got =
                sizer->header_size(static_cast<std::size_t>(i), &route);
            CHECK(route == Route::Stream);
            REQUIRE(got.has_value());
            bool threw = false;
            const PageSize want = loaded_size(own, i, &threw);
            REQUIRE_FALSE(threw);
            CHECK(same_bits(*got, want));
        }
    }
}

TEST_CASE("XpsPageSizer: declined pages leave no MuPDF error behind", "[core][xps]") {
    OwnDocument own(kSizing);
    auto sizer = own.sizer(kSizingPages);
    REQUIRE(sizer != nullptr);

    // p15's first cut throws in the parser; p06-p08, p10 and p12 are declined
    // (p12's entry is not there, so opening it throws).
    for (const int page : {15, 6, 7, 8, 10, 12}) {
        CAPTURE(page);
        (void)sizer->header_size(static_cast<std::size_t>(page - 1));
        own.throw_on_context();
    }

    INFO("warnings:\n" << own.warnings);
    CHECK(own.warnings.find(kUnhandled) == std::string::npos);
}

TEST_CASE("XpsPageSizer: at most 16 cuts, and a throwing last cut leaves no MuPDF error",
          "[core][xps]") {
    // Page 1: fifteen cuts throw, the sixteenth ends the root tag. Page 2: all
    // sixteen throw and the search stops there, so the last MuPDF call of the
    // sizer is a parse that threw (no fixture page ends that way: a later
    // fz_try resets the error code, error.c fz_do_catch).
    const XpsDims fits{660, 860};
    const XpsDims past{670, 870};
    TempFile file{fs::temp_directory_path() /
                  ("litepdf-xps-227-cuts-" + std::to_string(GetCurrentProcessId()) + ".xps")};
    write_xps(utf8_of(file.path), {page_with_quoted_gt(15, fits), page_with_quoted_gt(16, past)});

    OwnDocument own(utf8_of(file.path));
    REQUIRE(own.page_count() == 2);
    auto sizer = own.sizer(2);
    REQUIRE(sizer != nullptr);

    Route route = Route::None;
    const std::optional<PageSize> first = sizer->header_size(0, &route);
    CHECK(route == Route::Stream);
    REQUIRE(first.has_value());
    CHECK(same_bits(*first, table_size(fits)));

    route = Route::Stream;
    CHECK_FALSE(sizer->header_size(1, &route).has_value());
    CHECK(route == Route::None);
    own.throw_on_context();

    // Both pages load in full: only the cut limit declined page 2.
    for (int i = 0; i < 2; ++i) {
        CAPTURE(i);
        bool threw = false;
        const PageSize loaded = loaded_size(own, i, &threw);
        REQUIRE_FALSE(threw);
        CHECK(same_bits(loaded, table_size(i == 0 ? fits : past)));
    }

    INFO("warnings:" << '\n' << own.warnings);
    CHECK(own.warnings.find(kUnhandled) == std::string::npos);
}

TEST_CASE("XpsPageSizer: a part that fails to inflate leaves no MuPDF error", "[core][xps]") {
    // A copy of xps-sizing.xps whose p14 (deflated) has every compressed byte
    // overwritten with 0xFF, file length and offsets unchanged: the first
    // byte is a reserved deflate block type, so the flate filter throws
    // "zlib error" on the first read. fz_read does not pass that on:
    // fz_available (stream.h) catches every error but FZ_ERROR_TRYLATER,
    // reports it through the error callback, warns and ends the stream. So the
    // page is declined as a stream that ended without a root.
    constexpr int kDamagedPage = 14;
    const char* const kDamagedName = "Documents/1/Pages/p14.fpage";
    TempFile file{fs::temp_directory_path() /
                  ("litepdf-xps-227-damaged-" + std::to_string(GetCurrentProcessId()) + ".xps")};
    REQUIRE(fs::copy_file(kSizing, file.path, fs::copy_options::overwrite_existing));

    const auto entries = detail::read_zip_directory(file.path.wstring());
    REQUIRE(entries.has_value());
    const detail::ZipEntryRef* entry = nullptr;
    for (const detail::ZipEntryRef& e : *entries) {
        if (e.name == kDamagedName) entry = &e;
    }
    REQUIRE(entry != nullptr);

    std::vector<unsigned char> bytes;
    {
        std::ifstream in(file.path, std::ios::binary);
        REQUIRE(in);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const std::size_t lho = static_cast<std::size_t>(entry->local_header_offset);
    REQUIRE(lho + 30 <= bytes.size());
    const unsigned char* const local = bytes.data() + lho;
    REQUIRE(std::memcmp(local, "PK\x03\x04", 4) == 0);
    const unsigned method = local[8] | (local[9] << 8);
    REQUIRE(method == 8);
    const std::size_t name_len = local[26] | (local[27] << 8);
    const std::size_t extra_len = local[28] | (local[29] << 8);
    const std::size_t data = lho + 30 + name_len + extra_len;
    REQUIRE(entry->compressed_size > 1);
    REQUIRE(data + entry->compressed_size <= bytes.size());
    std::fill(bytes.begin() + static_cast<std::ptrdiff_t>(data),
              bytes.begin() + static_cast<std::ptrdiff_t>(data + entry->compressed_size),
              static_cast<unsigned char>(0xFF));
    {
        std::ofstream out(file.path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        REQUIRE(out);
    }

    {
        OwnDocument own(utf8_of(file.path));
        std::string errors;   // MuPDF's report of the zlib error, kept off stderr
        fz_set_error_callback(own.ctx, [](void* user, const char* message) noexcept {
            *static_cast<std::string*>(user) += std::string(message) + "\n";
        }, &errors);
        REQUIRE(own.page_count() == static_cast<std::size_t>(kSizingPages));
        auto sizer = own.sizer(kSizingPages);
        REQUIRE(sizer != nullptr);

        Route route = Route::Stream;
        CHECK_FALSE(sizer->header_size(kDamagedPage - 1, &route).has_value());
        CHECK(route == Route::None);
        own.throw_on_context();

        INFO("warnings:" << '\n' << own.warnings << "errors:" << '\n' << errors);
        // The damage was read (not, say, the entry missed): MuPDF's stream
        // layer reported it and ended the stream.
        CHECK(errors.find("zlib error") != std::string::npos);
        CHECK(own.warnings.find("read error; treating as end of file") != std::string::npos);
        CHECK(own.warnings.find(kUnhandled) == std::string::npos);
    }
}
