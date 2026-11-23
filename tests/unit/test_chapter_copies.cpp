// #232: loading pages by chapter location in every copy of a document. The
// token fixture (chapter-tokens.epub, from scripts/generate-strip-fixtures.py)
// holds one token per page, so a page loaded from the wrong chapter or the
// wrong place within it shows the wrong token.
#include "chapter_test_kit.hpp"
#include "core/Document.hpp"
#include "core/detail/PageLoad.hpp"

#include <catch2/catch_test_macros.hpp>

#include <mupdf/fitz.h>

#include <cstddef>
#include <iterator>
#include <string>
#include <vector>

using namespace litepdf::test_kit;
using litepdf::core::Document;
using litepdf::core::SelPoint;
using litepdf::core::detail::ChapterPages;
using litepdf::core::detail::PageAt;
using litepdf::core::detail::load_page_at;
using litepdf::core::detail::page_at;

namespace {

// The text of the page load_page_at gives for `at` and flat `index`, as
// OwnDocument::text builds it. False when the fz_try throws. The bytes are
// copied out after the fz_try (#179).
bool text_by_page_at(OwnDocument& own, PageAt at, int index, std::string& text) {
    fz_context* const ctx = own.ctx;
    fz_document* const doc = own.doc;
    fz_page* page = nullptr;
    fz_stext_page* stext = nullptr;
    fz_buffer* buf = nullptr;
    fz_output* out = nullptr;
    unsigned char* data = nullptr;
    std::size_t len = 0;
    bool threw = false;
    fz_var(page);
    fz_var(stext);
    fz_var(buf);
    fz_var(out);
    fz_try(ctx) {
        page = load_page_at(ctx, doc, at, index);
        fz_stext_options opts = {};
        stext = fz_new_stext_page_from_page(ctx, page, &opts);
        buf = fz_new_buffer(ctx, 4096);
        out = fz_new_output_with_buffer(ctx, buf);
        fz_print_stext_page_as_text(ctx, out, stext);
        fz_close_output(ctx, out);   // flushes into buf before the storage read
        len = fz_buffer_storage(ctx, buf, &data);
    }
    fz_always(ctx) {
        if (out) fz_drop_output(ctx, out);
        if (stext) fz_drop_stext_page(ctx, stext);
        if (page) fz_drop_page(ctx, page);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);   // also marks it handled (#146)
        threw = true;
    }
    if (!threw) text.assign(reinterpret_cast<const char*>(data), len);
    if (buf) fz_drop_buffer(ctx, buf);
    return !threw;
}

void check_at(PageAt got, int chapter, int page) {
    CHECK(got.by_location);
    CHECK(got.chapter == chapter);
    CHECK(got.page == page);
}

}  // namespace

TEST_CASE("ChapterTokens: each page of the fixture holds its own token, by flat load",
          "[document][chapters]") {
    OwnDocument own(kTokensFixture);
    REQUIRE(own.page_count() == 35);
    int nc = -1;
    fz_var(nc);   // assigned in the try, read after a catch that falls through (#178)
    fz_try(own.ctx) { nc = fz_count_chapters(own.ctx, own.doc); }
    fz_catch(own.ctx) { fz_report_error(own.ctx); }
    REQUIRE(nc == 12);
    for (int c = 0; c < 12; ++c) {
        int m = -1;
        fz_var(m);
        fz_try(own.ctx) { m = fz_count_chapter_pages(own.ctx, own.doc, c); }
        fz_catch(own.ctx) { fz_report_error(own.ctx); }
        CHECK(m == kTokenChapterPages[c]);
    }
    for (int i = 0; i < 35; ++i) {
        INFO("page " << i);
        CHECK(tokens_in(own.text(i)) == std::vector<std::string>{expected_token(i)});
    }
}

TEST_CASE("PageLoad: page_at locates an index from the table", "[document][chapters]") {
    const auto table = ChapterPages::from_counts({1, 4, 2}, 7);
    REQUIRE(table);
    check_at(page_at(&*table, 0), 0, 0);
    check_at(page_at(&*table, 1), 1, 0);
    check_at(page_at(&*table, 4), 1, 3);
    check_at(page_at(&*table, 5), 2, 0);
    check_at(page_at(&*table, 6), 2, 1);
}

TEST_CASE("PageLoad: page_at loads by flat number without a table or past it",
          "[document][chapters]") {
    CHECK_FALSE(page_at(nullptr, 0).by_location);
    const auto table = ChapterPages::from_counts({1, 4, 2}, 7);
    REQUIRE(table);
    CHECK_FALSE(page_at(&*table, 7).by_location);
    CHECK_FALSE(page_at(&*table, static_cast<std::size_t>(-1)).by_location);
}

TEST_CASE("PageLoad: load_page_at loads the located page and throws past the last",
          "[document][chapters]") {
    OwnDocument own(kTokensFixture);
    REQUIRE(own.page_count() == 35);
    const auto table = ChapterPages::from_counts(
        std::vector<int>(std::begin(kTokenChapterPages), std::end(kTokenChapterPages)), 35);
    REQUIRE(table);
    for (int i = 0; i < 35; ++i) {
        INFO("page " << i);
        const PageAt at = page_at(&*table, static_cast<std::size_t>(i));
        CHECK(at.by_location);
        std::string text;
        REQUIRE(text_by_page_at(own, at, i, text));
        CHECK(tokens_in(text) == std::vector<std::string>{expected_token(i)});
    }
    const PageAt past = page_at(&*table, 35);
    CHECK_FALSE(past.by_location);
    std::string text;
    CHECK_FALSE(text_by_page_at(own, past, 35, text));
}

// --- the Document's own paths (Task 3) ---------------------------------------

TEST_CASE("Document chapters: page_text holds each page's own token", "[document][chapters]") {
    OwnDocument own(kTokensFixture);
    Document doc;
    REQUIRE_FALSE(doc.open(kTokensFixture).has_value());
    REQUIRE(doc.page_count() == 35);
    for (int i = 0; i < 35; ++i) {
        INFO("page " << i);
        CHECK(tokens_in(doc.page_text(static_cast<std::size_t>(i))) == tokens_in(own.text(i)));
    }
}

TEST_CASE("Document chapters: text_page copies each page's own token", "[document][chapters]") {
    OwnDocument own(kTokensFixture);
    Document doc;
    REQUIRE_FALSE(doc.open(kTokensFixture).has_value());
    REQUIRE(doc.page_count() == 35);
    for (int i = 0; i < 35; ++i) {
        INFO("page " << i);
        const Document::TextPage tp = doc.text_page(static_cast<std::size_t>(i));
        REQUIRE(tp.valid());
        SelPoint a, b;
        REQUIRE(tp.full_range(a, b));
        CHECK(tokens_in(tp.copy(a, b)) == tokens_in(own.text(i)));
    }
    CHECK_FALSE(doc.text_page(35).valid());
}

TEST_CASE("Document chapters: page_hits finds each token on its own page only",
          "[document][chapters]") {
    Document doc;
    REQUIRE_FALSE(doc.open(kTokensFixture).has_value());
    REQUIRE(doc.page_count() == 35);
    for (int i = 0; i < 35; ++i) {
        const std::string token = expected_token(i);
        for (int j = 0; j < 35; ++j) {
            INFO("token of page " << i << " searched on page " << j);
            CHECK(doc.page_hits(static_cast<std::size_t>(j), token, {}).size() ==
                  (i == j ? 1u : 0u));
        }
    }
}
