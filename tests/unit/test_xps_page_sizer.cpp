// #227: an XPS page's size comes from the root start tag of its FixedPage part,
// not from loading the page. Every size the root-tag route gives must be
// bitwise equal to fz_load_page + fz_bound_page on the same page.
//
// xps-sizing.xps and xps-escaped-name.xps come from
// scripts/generate-xps-sizing-fixture.py; each page and what it covers are
// listed there and in the spec,
// docs/superpowers/specs/2026-11-26-xps-header-sizing-227-design.md, section 5.1.
#include "core/Document.hpp"

#include <catch2/catch_test_macros.hpp>

#include <mupdf/fitz.h>

#include <cstddef>
#include <cstring>
#include <string>

using namespace litepdf::core;

namespace {

constexpr const char* kSizing = "tests/fixtures/xps-sizing.xps";
constexpr const char* kEscaped = "tests/fixtures/xps-escaped-name.xps";
constexpr int kSizingPages = 15;
constexpr int kEscapedPages = 3;
constexpr int kNoHeightPage = 9;   // p09: no Height, MuPDF's load throws

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
