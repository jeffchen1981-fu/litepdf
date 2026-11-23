// #232: loading pages by chapter location in every copy of a document. The
// token fixture (chapter-tokens.epub, from scripts/generate-strip-fixtures.py)
// holds one token per page, so a page loaded from the wrong chapter or the
// wrong place within it shows the wrong token.
#include "chapter_test_kit.hpp"

#include <catch2/catch_test_macros.hpp>

#include <mupdf/fitz.h>

#include <string>
#include <vector>

using namespace litepdf::test_kit;

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
