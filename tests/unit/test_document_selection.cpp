// Text selection, engine layer (#52): Document::TextPage against
// tests/fixtures/selection.pdf (scripts/generate-selection-fixture.py documents
// what each page pins) and the existing multi-format fixtures.
#include "core/Document.hpp"
#include "core/EscrowContext.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

using litepdf::core::Document;
using litepdf::core::SelectMode;
using litepdf::core::SelPoint;

namespace {

constexpr std::size_t kTwoColumns = 0;
constexpr std::size_t kWideGap    = 1;
constexpr std::size_t kBlank      = 2;
constexpr std::size_t kWords      = 3;
constexpr std::size_t kManyRuns   = 4;
constexpr std::size_t kCropOffset = 5;

Document open_fixture(const char* path) {
    Document doc;
    REQUIRE_FALSE(doc.open(std::filesystem::path(path)).has_value());
    return doc;
}

Document open_selection_fixture() {
    return open_fixture("tests/fixtures/selection.pdf");
}

// A selection copy and page_text() lay out line breaks differently -- CRLF
// versus LF, and page_text adds a blank line after every block -- but must agree
// on every character. Compare with whitespace removed.
std::string without_whitespace(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c != ' ' && c != '\r' && c != '\n' && c != '\t') out.push_back(c);
    }
    return out;
}

// Centre of the single whole-word hit for `word` on `page`.
SelPoint center_of_word(const Document& doc, std::size_t page, const char* word) {
    Document::SearchFlags flags;
    flags.match_case = true;
    flags.whole_word = true;
    const auto hits = doc.page_hits(page, word, flags);
    REQUIRE(hits.size() == 1);
    const auto& h = hits[0];
    return SelPoint{ (h.ul_x + h.ur_x + h.ll_x + h.lr_x) * 0.25f,
                     (h.ul_y + h.ur_y + h.ll_y + h.lr_y) * 0.25f };
}

std::string copy_snapped(const Document::TextPage& text, SelPoint a, SelPoint b,
                         SelectMode mode) {
    const auto s = text.snap(a, b, mode);
    return text.copy(s.a, s.b);
}

// Select All must copy exactly the characters page_text() extracts. On a page
// with no text there is no range, and page_text must agree that it is empty.
void require_select_all_matches_page_text(const Document& doc, std::size_t page) {
    const auto text = doc.text_page(page);
    REQUIRE(text.valid());
    SelPoint first, last;
    if (text.full_range(first, last)) {
        REQUIRE(without_whitespace(text.copy(first, last))
                == without_whitespace(doc.page_text(page)));
    } else {
        REQUIRE(without_whitespace(doc.page_text(page)).empty());
    }
}

}  // namespace

TEST_CASE("DocumentSelection text page is empty when there is nothing to extract",
          "[core][selection]") {
    Document unopened;
    REQUIRE_FALSE(unopened.text_page(0).valid());

    const Document doc = open_selection_fixture();
    REQUIRE_FALSE(doc.text_page(6).valid());      // one past the last page
    REQUIRE_FALSE(doc.text_page(99999).valid());

    const Document::TextPage empty;
    REQUIRE_FALSE(empty.valid());
    REQUIRE(empty.page() == -1);
    SelPoint a, b;
    REQUIRE_FALSE(empty.full_range(a, b));
    REQUIRE(empty.copy(SelPoint{0, 0}, SelPoint{100, 100}).empty());
    REQUIRE(empty.highlight(SelPoint{0, 0}, SelPoint{100, 100}).empty());
}

TEST_CASE("DocumentSelection select all copies both columns and the final character",
          "[core][selection]") {
    const Document doc = open_selection_fixture();
    const auto text = doc.text_page(kTwoColumns);
    REQUIRE(text.valid());
    REQUIRE(text.page() == static_cast<int>(kTwoColumns));

    SelPoint first, last;
    REQUIRE(text.full_range(first, last));
    const std::string copied = without_whitespace(text.copy(first, last));

    REQUIRE(copied == without_whitespace(doc.page_text(kTwoColumns)));
    // The corner-points approach drops the shorter column; the origins approach
    // drops the last character. Name both failures directly.
    REQUIRE(copied.find("Rightcolumnline1") != std::string::npos);
    REQUIRE(copied.substr(copied.size() - 16) == "Rightcolumnline4");
}

TEST_CASE("DocumentSelection select all matches page text across existing fixtures",
          "[core][selection]") {
    for (const char* path : { "tests/fixtures/search.pdf", "tests/fixtures/simple.pdf" }) {
        const Document doc = open_fixture(path);
        for (std::size_t page = 0; page < doc.page_count(); ++page) {
            INFO(path << " page " << page);
            require_select_all_matches_page_text(doc, page);
        }
    }
}

TEST_CASE("DocumentSelection a wide gap on one baseline yields separate quads",
          "[core][selection]") {
    const Document doc = open_selection_fixture();
    const auto text = doc.text_page(kWideGap);
    SelPoint first, last;
    REQUIRE(text.full_range(first, last));
    // One quad would be a highlight bar painted straight across ~270 pt of
    // whitespace -- what a hand-rolled one-quad-per-line walk produces.
    REQUIRE(text.highlight(first, last).size() >= 2);
}

TEST_CASE("DocumentSelection a page with no text has no range and copies nothing",
          "[core][selection]") {
    const Document doc = open_selection_fixture();
    const auto text = doc.text_page(kBlank);
    REQUIRE(text.valid());
    SelPoint first, last;
    REQUIRE_FALSE(text.full_range(first, last));
    REQUIRE(text.highlight(SelPoint{0, 0}, SelPoint{612, 792}).empty());
    REQUIRE(text.copy(SelPoint{0, 0}, SelPoint{612, 792}).empty());
}

TEST_CASE("DocumentSelection highlight grows past its initial quad buffer",
          "[core][selection]") {
    const Document doc = open_selection_fixture();
    const auto text = doc.text_page(kManyRuns);
    SelPoint first, last;
    REQUIRE(text.full_range(first, last));
    // The page has 300 separate runs. A buffer that never grows returns 256.
    REQUIRE(text.highlight(first, last).size() > 256);
}

TEST_CASE("DocumentSelection coordinates are page box relative on a CropBox offset page",
          "[core][selection]") {
    const Document doc = open_selection_fixture();

    // CropBox [36 36 576 756]: the page is 540 x 720 and its corner is page
    // space (0, 0). "ORIGIN" was drawn at user space (108, 684), i.e. 72 pt in
    // from the crop box's left edge, baseline 72 pt below its top.
    const auto size = doc.page_size(kCropOffset);
    REQUIRE(size.width_pt  == Catch::Approx(540.0f).margin(0.01f));
    REQUIRE(size.height_pt == Catch::Approx(720.0f).margin(0.01f));

    Document::SearchFlags flags;
    const auto hits = doc.page_hits(kCropOffset, "ORIGIN", flags);
    REQUIRE(hits.size() == 1);
    REQUIRE(hits[0].ll_x == Catch::Approx(72.0f).margin(1.0f));   // 108 if untranslated
    REQUIRE(hits[0].ul_y < 72.0f);                                 // glyph top above the baseline
    REQUIRE(hits[0].ll_y > 72.0f);                                 // descender below it
    REQUIRE(hits[0].ll_y < 80.0f);

    const auto text = doc.text_page(kCropOffset);
    SelPoint first, last;
    REQUIRE(text.full_range(first, last));
    const auto quads = text.highlight(first, last);
    REQUIRE(quads.size() == 1);
    REQUIRE(quads[0].ll_x == Catch::Approx(72.0f).margin(1.0f));
    REQUIRE(text.copy(first, last) == "ORIGIN");
}

TEST_CASE("DocumentSelection chars mode passes points through unsnapped",
          "[core][selection]") {
    const Document doc = open_selection_fixture();
    const auto text = doc.text_page(kWords);
    const SelPoint a{ 80.0f, 70.0f };
    const SelPoint b{ 150.0f, 90.0f };
    const auto s = text.snap(a, b, SelectMode::Chars);
    REQUIRE(s.a.x == a.x);
    REQUIRE(s.a.y == a.y);
    REQUIRE(s.b.x == b.x);
    REQUIRE(s.b.y == b.y);
}

TEST_CASE("DocumentSelection a stationary double click selects one word",
          "[core][selection]") {
    const Document doc = open_selection_fixture();
    const auto text = doc.text_page(kWords);
    const SelPoint beta = center_of_word(doc, kWords, "beta");
    REQUIRE(copy_snapped(text, beta, beta, SelectMode::Words) == "beta");
}

TEST_CASE("DocumentSelection a triple click selects the whole line",
          "[core][selection]") {
    const Document doc = open_selection_fixture();
    const auto text = doc.text_page(kWords);
    const SelPoint beta = center_of_word(doc, kWords, "beta");
    REQUIRE(copy_snapped(text, beta, beta, SelectMode::Lines) == "alpha beta gamma");
}

TEST_CASE("DocumentSelection a backward word drag selects the same text as a forward one",
          "[core][selection]") {
    const Document doc = open_selection_fixture();
    const auto text = doc.text_page(kWords);
    const SelPoint alpha = center_of_word(doc, kWords, "alpha");
    const SelPoint gamma = center_of_word(doc, kWords, "gamma");

    REQUIRE(copy_snapped(text, alpha, gamma, SelectMode::Words) == "alpha beta gamma");
    REQUIRE(copy_snapped(text, gamma, alpha, SelectMode::Words) == "alpha beta gamma");

    // snap returns the ends in reading order -- which is exactly why a caller
    // must never write the result back over its raw anchor (spec §2).
    const auto s = text.snap(gamma, alpha, SelectMode::Words);
    REQUIRE(s.a.x < s.b.x);
}

TEST_CASE("DocumentSelection a word drag from below the text keeps its far end",
          "[core][selection]") {
    // Plan correction C4. A point below every line resolves to the end of the
    // text, and fz_snap_selection never writes an end that lies past the last
    // character: the caller's raw second point stays in place. Dragging BACKWARD
    // from there, that point is the earlier one, and without the fix the
    // selection collapses to part of "beta".
    const Document doc = open_selection_fixture();
    const auto text = doc.text_page(kWords);
    const SelPoint below{ 300.0f, 600.0f };
    const SelPoint beta = center_of_word(doc, kWords, "beta");

    REQUIRE(copy_snapped(text, below, beta, SelectMode::Words)
            == "beta gamma\r\ndelta epsilon");
    REQUIRE(copy_snapped(text, beta, below, SelectMode::Words)
            == "beta gamma\r\ndelta epsilon");
    REQUIRE(copy_snapped(text, below, beta, SelectMode::Lines)
            == "alpha beta gamma\r\ndelta epsilon");
}

TEST_CASE("DocumentSelection copied text uses CRLF line endings",
          "[core][selection]") {
    const Document doc = open_selection_fixture();
    const auto text = doc.text_page(kWords);
    SelPoint first, last;
    REQUIRE(text.full_range(first, last));
    REQUIRE(text.copy(first, last) == "alpha beta gamma\r\ndelta epsilon");
}

TEST_CASE("DocumentSelection a text page outlives its Document",
          "[core][selection]") {
    const std::size_t tables_before = litepdf::core::detail::live_lock_tables();
    Document::TextPage text;
    {
        const Document doc = open_selection_fixture();
        text = doc.text_page(kWords);
        REQUIRE(text.valid());
    }
    // The Document -- and the context the page was extracted on -- is gone.
    // Closing a tab mid-drag produces exactly this state (spec §3.3).
    REQUIRE(litepdf::core::detail::live_lock_tables() == tables_before + 1);

    SelPoint first, last;
    REQUIRE(text.full_range(first, last));
    REQUIRE(text.copy(first, last) == "alpha beta gamma\r\ndelta epsilon");

    text = Document::TextPage{};
    REQUIRE(litepdf::core::detail::live_lock_tables() == tables_before);
}

TEST_CASE("DocumentSelection CJK text round trips through copy",
          "[core][selection]") {
    const Document doc = open_fixture("tests/fixtures/cjk-zh-hant.pdf");
    const auto text = doc.text_page(0);
    SelPoint first, last;
    REQUIRE(text.full_range(first, last));
    REQUIRE_FALSE(without_whitespace(text.copy(first, last)).empty());
    require_select_all_matches_page_text(doc, 0);
}

TEST_CASE("DocumentSelection a text page is available only after authenticate",
          "[core][selection]") {
    // What this proves, and what it does not. encrypted.pdf's only page has NO
    // text (MuPDF 1.27.2 extracts ''), so no selection query runs on decrypted
    // content here -- the fixture cannot exercise that, and the test does not
    // pretend to. It pins the acquisition contract only: no handle before
    // authenticate (is_open() is false), a valid handle after it, and an empty
    // range that agrees with page_text.
    Document doc;
    const auto err = doc.open("tests/fixtures/encrypted.pdf");
    REQUIRE(err.has_value());
    REQUIRE(*err == Document::OpenError::NeedsPassword);
    REQUIRE_FALSE(doc.text_page(0).valid());   // not open until authenticated
    REQUIRE(doc.authenticate("test"));
    const auto text = doc.text_page(0);
    REQUIRE(text.valid());
    SelPoint first, last;
    REQUIRE_FALSE(text.full_range(first, last));        // the fixture has no text
    REQUIRE(without_whitespace(doc.page_text(0)).empty());
}

TEST_CASE("DocumentSelection works on a reflowable epub page",
          "[core][selection]") {
    const Document doc = open_fixture("tests/fixtures/sample.epub");
    const auto text = doc.text_page(0);
    SelPoint first, last;
    REQUIRE(text.full_range(first, last));
    REQUIRE(without_whitespace(text.copy(first, last)).find("HellofromLitePDFtestePub.")
            != std::string::npos);
    require_select_all_matches_page_text(doc, 0);
}
