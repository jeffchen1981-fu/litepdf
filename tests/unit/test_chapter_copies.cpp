// #232: loading pages by chapter location in every copy of a document. The
// token fixture (chapter-tokens.epub, from scripts/generate-strip-fixtures.py)
// holds one token per page, so a page loaded from the wrong chapter or the
// wrong place within it shows the wrong token.
#include "app/SearchDispatcher.hpp"
#include "chapter_test_kit.hpp"
#include "core/Document.hpp"
#include "core/DocumentShare.hpp"
#include "core/RenderEngine.hpp"
#include "core/SearchSession.hpp"
#include "core/detail/ChapterPagesAccess.hpp"
#include "core/detail/PageHitsAccess.hpp"
#include "core/detail/PageLoad.hpp"

#include <catch2/catch_test_macros.hpp>

#include <mupdf/fitz.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace litepdf::test_kit;
using litepdf::app::InlineDispatcher;
using litepdf::core::Document;
using litepdf::core::DocumentShare;
using litepdf::core::RenderEngine;
using litepdf::core::SearchSession;
using litepdf::core::SelPoint;
using litepdf::core::detail::ChapterPages;
using litepdf::core::detail::ChapterPagesAccess;
using litepdf::core::detail::PageAt;
using litepdf::core::detail::load_page_at;
using litepdf::core::detail::page_at;
using litepdf::core::detail::page_hits_on;
using litepdf::core::detail::table_for_copy;

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

// --- a table for a copy (Task 4) ---------------------------------------------

namespace {

// A second fz_document of `path` on a clone of `doc`'s context family, as a
// render worker or a search session opens its own. The copy is dropped before
// its context. fz_open_document sits in a fz_try with nothing that has a
// destructor (#179); the catch reports (#146).
struct CopyDocument {
    fz_context*  ctx  = nullptr;
    fz_document* copy = nullptr;

    CopyDocument(const Document& doc, const char* path) {
        ctx = doc.clone_context();
        REQUIRE(ctx != nullptr);
        fz_try(ctx) { copy = fz_open_document(ctx, path); }
        fz_catch(ctx) { fz_report_error(ctx); copy = nullptr; }
        REQUIRE(copy != nullptr);
    }
    ~CopyDocument() {
        fz_drop_document(ctx, copy);
        fz_drop_context(ctx);
    }
    CopyDocument(const CopyDocument&) = delete;
    CopyDocument& operator=(const CopyDocument&) = delete;

    // The copy's own chapter counts; false when MuPDF throws.
    bool chapter_counts(std::vector<int>& counts) {
        int nc = -1;
        fz_var(nc);
        fz_try(ctx) { nc = fz_count_chapters(ctx, copy); }
        fz_catch(ctx) { fz_report_error(ctx); nc = -1; }
        if (nc < 0) return false;
        counts.assign(static_cast<std::size_t>(nc), 0);
        for (int c = 0; c < nc; ++c) {
            int m = -1;
            fz_var(m);
            fz_try(ctx) { m = fz_count_chapter_pages(ctx, copy, c); }
            fz_catch(ctx) { fz_report_error(ctx); m = -1; }
            if (m < 0) return false;
            counts[static_cast<std::size_t>(c)] = m;
        }
        return true;
    }
};

constexpr const char* kStripEpub = "tests/fixtures/strip-chapters.epub";
constexpr const char* kSizesEpub = "tests/fixtures/chapter-sizes.epub";
constexpr const char* kSearchPdf = "tests/fixtures/search.pdf";
constexpr const char* kStripFb2  = "tests/fixtures/strip-chapters.fb2";

}  // namespace

TEST_CASE("ChapterPagesAccess: a copy of the same file gets the Document's table",
          "[document][chapters]") {
    Document doc;
    REQUIRE_FALSE(doc.open(kTokensFixture).has_value());
    const auto t = ChapterPagesAccess::table(doc);
    REQUIRE(t != nullptr);
    CopyDocument c(doc, kTokensFixture);
    CHECK(table_for_copy(c.ctx, c.copy, t).get() == t.get());
}

TEST_CASE("ChapterPagesAccess: a copy with another chapter count gets no table",
          "[document][chapters]") {
    Document doc;
    REQUIRE_FALSE(doc.open(kTokensFixture).has_value());
    const auto t = ChapterPagesAccess::table(doc);
    REQUIRE(t != nullptr);
    {
        CopyDocument c(doc, kStripEpub);
        CHECK(table_for_copy(c.ctx, c.copy, t) == nullptr);
    }
    {
        CopyDocument c(doc, kSearchPdf);
        CHECK(table_for_copy(c.ctx, c.copy, t) == nullptr);
    }
}

TEST_CASE("ChapterPagesAccess: no table in, no table out", "[document][chapters]") {
    Document doc;
    REQUIRE_FALSE(doc.open(kTokensFixture).has_value());
    CopyDocument c(doc, kTokensFixture);
    CHECK(table_for_copy(c.ctx, c.copy, nullptr) == nullptr);
}

TEST_CASE("ChapterPagesAccess: a non-ePub Document has no table", "[document][chapters]") {
    for (const char* path : {kSearchPdf, kStripFb2}) {
        INFO(path);
        Document doc;
        REQUIRE_FALSE(doc.open(path).has_value());
        CHECK(ChapterPagesAccess::table(doc) == nullptr);
    }
}

TEST_CASE("ChapterPagesAccess: the table outlives the Document's close", "[document][chapters]") {
    Document doc;
    REQUIRE_FALSE(doc.open(kTokensFixture).has_value());
    const auto t = ChapterPagesAccess::table(doc);
    REQUIRE(t != nullptr);
    doc.close();
    CHECK(ChapterPagesAccess::table(doc) == nullptr);
    CHECK(t->page_count() == 35);
    const auto at = t->locate(34);
    REQUIRE(at.has_value());
    CHECK(at->chapter == 11);
    CHECK(at->page == 3);
}

TEST_CASE("ChapterPagesAccess: a copy's own chapter counts give the Document's table",
          "[document][chapters]") {
    for (const char* path : {kTokensFixture, kSizesEpub, kStripEpub}) {
        INFO(path);
        Document doc;
        REQUIRE_FALSE(doc.open(path).has_value());
        const auto t = ChapterPagesAccess::table(doc);
        REQUIRE(t != nullptr);
        CopyDocument c(doc, path);
        std::vector<int> counts;
        REQUIRE(c.chapter_counts(counts));
        const auto own = ChapterPages::from_counts(counts, doc.page_count());
        REQUIRE(own.has_value());
        CHECK(own->chapter_count() == t->chapter_count());
        CHECK(own->page_count() == doc.page_count());
        CHECK(t->page_count() == doc.page_count());
        for (std::size_t i = 0; i < doc.page_count(); ++i) {
            const auto a = own->locate(i);
            const auto b = t->locate(i);
            REQUIRE(a.has_value());
            REQUIRE(b.has_value());
            CHECK(a->chapter == b->chapter);
            CHECK(a->page == b->page);
        }
    }
}

// --- the render workers (Task 5) ---------------------------------------------

namespace {

// What one request delivered. Shared with the request's callbacks, so the worker
// can still be inside a promise's set_value after the test has seen its future.
struct Delivery {
    std::vector<std::uint8_t> bgra;
    int w = 0;
    int h = 0;
    bool got_pixmap = false;
    std::string text;
    std::promise<void> completed;
    std::promise<void> texted;
};

// Renders `page` at scale 1 on `engine` and takes its prefetched text: one
// request (priority 0, bypass_cache) whose on_complete copies the pixmap's rows
// through its stride and drops it, and whose on_text stores the full-range copy.
// Waits for both callbacks (10 s each). False when either does not arrive or the
// pixmap is null. One request at a time, so no skip rule drops the text.
bool render_and_text(RenderEngine& engine, int page, std::vector<std::uint8_t>& bgra,
                     int& w, int& h, std::string& text) {
    auto d = std::make_shared<Delivery>();
    auto completed = d->completed.get_future();
    auto texted = d->texted.get_future();
    RenderEngine::RenderRequest r{page, 0, 1.0f, [d](fz_pixmap* pix, fz_context* ctx) {
        if (pix) {
            d->got_pixmap = true;
            d->w = fz_pixmap_width(ctx, pix);
            d->h = fz_pixmap_height(ctx, pix);
            const std::uint8_t* samples = fz_pixmap_samples(ctx, pix);
            const std::size_t stride = static_cast<std::size_t>(fz_pixmap_stride(ctx, pix));
            const std::size_t row = static_cast<std::size_t>(d->w) * 4;
            d->bgra.resize(row * static_cast<std::size_t>(d->h));
            for (int y = 0; y < d->h; ++y) {
                const std::uint8_t* src = samples + static_cast<std::size_t>(y) * stride;
                std::copy(src, src + row,
                          d->bgra.begin() +
                              static_cast<std::ptrdiff_t>(row * static_cast<std::size_t>(y)));
            }
            fz_drop_pixmap(ctx, pix);
        }
        d->completed.set_value();
    }};
    r.bypass_cache = true;
    r.on_text = [d](Document::TextPage tp) {
        SelPoint a, b;
        if (tp.full_range(a, b)) d->text = tp.copy(a, b);
        d->texted.set_value();
    };
    r.text_wanted = std::make_shared<const std::atomic<int>>(page);
    engine.submit(std::move(r));
    constexpr auto kWait = std::chrono::seconds(10);
    if (completed.wait_for(kWait) != std::future_status::ready) return false;
    if (!d->got_pixmap) return false;
    if (texted.wait_for(kWait) != std::future_status::ready) return false;
    bgra = std::move(d->bgra);
    w = d->w;
    h = d->h;
    text = std::move(d->text);
    return true;
}

// Every page of the token fixture from `engine` against `own`'s flat load
// through the worker's raster steps, and its prefetched text's tokens against
// `own`'s.
void check_every_token_page(RenderEngine& engine, OwnDocument& own) {
    for (int i = 0; i < 35; ++i) {
        INFO("page " << i);
        std::vector<std::uint8_t> got, want;
        int gw = 0, gh = 0, ww = 0, wh = 0;
        std::string text;
        REQUIRE(render_and_text(engine, i, got, gw, gh, text));
        REQUIRE(own.rendered_via_list(i, 1.0f, want, ww, wh));
        CHECK(gw == ww);
        CHECK(gh == wh);
        REQUIRE(got.size() == want.size());
        const auto diff = std::mismatch(got.begin(), got.end(), want.begin());
        INFO("first differing byte " << (diff.first - got.begin()) << " of " << got.size());
        CHECK(diff.first == got.end());
        CHECK(tokens_in(text) == tokens_in(own.text(i)));
    }
}

}  // namespace

TEST_CASE("RenderEngine chapters: two own workers render and prefetch each page as a flat load does",
          "[core][render][chapters]") {
    OwnDocument own(kTokensFixture);
    Document doc;
    REQUIRE_FALSE(doc.open(kTokensFixture).has_value());
    REQUIRE(doc.page_count() == 35);
    RenderEngine engine(doc, 2, nullptr);
    check_every_token_page(engine, own);
}

TEST_CASE("RenderEngine chapters: a shared worker renders and prefetches each page as a flat load does",
          "[core][render][chapters]") {
    // The product never builds a Shared ePub (spec 4.5); the constructor accepts one.
    OwnDocument own(kTokensFixture);
    Document doc;
    REQUIRE_FALSE(doc.open(kTokensFixture).has_value());
    REQUIRE(doc.page_count() == 35);
    RenderEngine engine(doc, 1, nullptr, std::nullopt, DocumentShare::Shared);
    check_every_token_page(engine, own);
}

TEST_CASE("RenderEngine chapters: a copy of a file replaced after open renders every page by flat number",
          "[core][render][chapters]") {
    const TempDir dir;   // first: destroyed after every document that opens the file
    const std::filesystem::path book = dir.path / "book.epub";
    std::filesystem::copy_file(kTokensFixture, book);
    Document doc;
    REQUIRE_FALSE(doc.open(book).has_value());
    REQUIRE(doc.page_count() == 35);
    // Another chapter count under the open Document (planning finding P2: a file
    // MuPDF holds open can be overwritten), so each worker's check refuses the table.
    std::filesystem::copy_file(kStripEpub, book,
                               std::filesystem::copy_options::overwrite_existing);
    RenderEngine engine(doc, 2, nullptr);
    OwnDocument fresh(utf8_of(book).c_str());
    for (int i = 0; i < static_cast<int>(doc.page_count()); ++i) {
        INFO("page " << i);
        std::vector<std::uint8_t> got, want;
        int gw = 0, gh = 0, ww = 0, wh = 0;
        std::string text;
        REQUIRE(render_and_text(engine, i, got, gw, gh, text));
        REQUIRE(fresh.rendered_via_list(i, 1.0f, want, ww, wh));
        CHECK(gw == ww);
        CHECK(gh == wh);
        REQUIRE(got.size() == want.size());
        const auto diff = std::mismatch(got.begin(), got.end(), want.begin());
        INFO("first differing byte " << (diff.first - got.begin()) << " of " << got.size());
        CHECK(diff.first == got.end());
    }
}

// --- the search sessions (Task 6) --------------------------------------------

namespace {

// The token as set_query takes it. The tokens are ASCII.
std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// Each page's token, searched by `session` (on an InlineDispatcher, so each
// query's scan is done when set_query returns), hits its own page and no other.
void check_every_token_search(SearchSession& session) {
    for (int i = 0; i < 35; ++i) {
        session.set_query(widen(expected_token(i)), {});
        REQUIRE(session.scan_complete());
        for (int j = 0; j < 35; ++j) {
            INFO("token of page " << i << " searched on page " << j);
            CHECK(session.hits_for_page(static_cast<std::size_t>(j)).size() ==
                  (i == j ? 1u : 0u));
        }
    }
}

}  // namespace

TEST_CASE("SearchSession chapters: an own session finds each token on its own page",
          "[search][chapters]") {
    Document doc;
    REQUIRE_FALSE(doc.open(kTokensFixture).has_value());
    REQUIRE(doc.page_count() == 35);
    InlineDispatcher disp;
    SearchSession session(doc, disp, std::nullopt);
    check_every_token_search(session);
}

TEST_CASE("SearchSession chapters: a shared session finds each token on its own page",
          "[search][chapters]") {
    // The product never builds a Shared ePub (spec 4.5); the constructor accepts one.
    Document doc;
    REQUIRE_FALSE(doc.open(kTokensFixture).has_value());
    REQUIRE(doc.page_count() == 35);
    InlineDispatcher disp;
    SearchSession session(doc, disp, std::nullopt, DocumentShare::Shared);
    check_every_token_search(session);
}

TEST_CASE("SearchSession chapters: a session on a file replaced after open finds hits by flat number",
          "[search][chapters]") {
    const TempDir dir;   // first: destroyed after every document that opens the file
    const std::filesystem::path book = dir.path / "book.epub";
    std::filesystem::copy_file(kTokensFixture, book);
    Document doc;
    REQUIRE_FALSE(doc.open(book).has_value());
    REQUIRE(doc.page_count() == 35);
    // Another chapter count under the open Document (planning finding P2), so the
    // session's check refuses the table. "Chapter" is on the first page of each
    // strip-chapters.epub chapter only, so a location from the old table would
    // land on the wrong page.
    std::filesystem::copy_file(kStripEpub, book,
                               std::filesystem::copy_options::overwrite_existing);
    InlineDispatcher disp;
    SearchSession session(doc, disp, std::nullopt);
    session.set_query(L"Chapter", {});
    REQUIRE(session.scan_complete());

    // The oracle: the new file loaded by flat number on the test's own document.
    OwnDocument fresh(utf8_of(book).c_str());
    std::size_t oracle_total = 0;
    for (std::size_t i = 0; i < 35; ++i) {
        INFO("page " << i);
        std::vector<Document::PageHit> want =
            page_hits_on(fresh.ctx, fresh.doc, nullptr, i, "Chapter", {}, nullptr);
        // The session orders a page's hits top to bottom, stably (#139).
        std::stable_sort(want.begin(), want.end(),
                         [](const Document::PageHit& a, const Document::PageHit& b) {
                             return a.ul_y < b.ul_y;
                         });
        oracle_total += want.size();
        const std::vector<SearchSession::Hit> got = session.hits_for_page(i);
        REQUIRE(got.size() == want.size());
        for (std::size_t k = 0; k < got.size(); ++k) {
            CHECK(got[k].page == i);
            CHECK(got[k].geom.ul_x == want[k].ul_x);
            CHECK(got[k].geom.ul_y == want[k].ul_y);
        }
    }
    CHECK(oracle_total > 0);   // the comparison is not vacuous
}
