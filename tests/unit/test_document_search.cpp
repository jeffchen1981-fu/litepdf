// Tests for Document::page_hits — stateless MuPDF search bridge (Phase 6 Task 3).
//
// The search.pdf fixture (Task 2) contains:
//   - page 0 : "Lorem" x 12 (among other text)
//   - page 2 : a CJK phrase (4 codepoints, U+4E2D U+6587 U+6E2C U+8A66)
//              present at least once
//   - page 4 : no "Lorem"
//
// MuPDF 1.27.2's incremental fz_search matcher honors case-sensitivity,
// whole-word, and regex. The case-sensitive test that was previously tagged
// [!shouldfail] (documenting the legacy always-case-insensitive fz_search_page
// no-op) now passes as a normal case.
#include "core/Document.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <string>

using namespace litepdf::core;

namespace {
Document open_search_fixture() {
    Document doc;
    auto err = doc.open(std::filesystem::path("tests/fixtures/search.pdf"));
    REQUIRE_FALSE(err.has_value());
    return doc;
}
}  // namespace

TEST_CASE("page_hits: page 0 has 12 Lorem hits", "[document][search]") {
    Document doc = open_search_fixture();
    auto hits = doc.page_hits(0, "Lorem", Document::SearchFlags{});
    REQUIRE(hits.size() == 12);
    REQUIRE(hits[0].snippet_utf8.find("Lorem") != std::string::npos);
}

TEST_CASE("page_hits: page 4 has zero Lorem hits", "[document][search]") {
    Document doc = open_search_fixture();
    auto hits = doc.page_hits(4, "Lorem", Document::SearchFlags{});
    REQUIRE(hits.empty());
}

TEST_CASE("page_hits: case-insensitive matches lowercase needle", "[document][search]") {
    Document doc = open_search_fixture();
    Document::SearchFlags f{false};  // match_case=false
    auto hits = doc.page_hits(0, "lorem", f);
    REQUIRE(hits.size() == 12);
}

// Case-sensitive search now excludes wrong-case matches. On MuPDF 1.27.2 the
// incremental fz_search matcher honors FZ_SEARCH_EXACT, so a lowercase needle
// no longer matches the capitalized "Lorem" in the fixture. (Previously this
// was tagged [!shouldfail] to document the legacy always-case-insensitive
// fz_search_page no-op.)
TEST_CASE("page_hits: case-sensitive does not match mismatched case", "[document][search]") {
    Document doc = open_search_fixture();
    Document::SearchFlags f{true};  // match_case=true
    auto hits = doc.page_hits(0, "lorem", f);  // lowercase; fixture uses "Lorem"
    REQUIRE(hits.empty());
}

TEST_CASE("page_hits: Unicode needle matches CJK text", "[document][search][unicode]") {
    Document doc = open_search_fixture();
    // UTF-8 encoding of the Chinese phrase for "Chinese test" (4 codepoints).
    // Stored as hex-escaped bytes so this source file remains pure ASCII and
    // survives ACP-based toolchains cleanly.
    const std::string needle_utf8 = "\xE4\xB8\xAD\xE6\x96\x87\xE6\xB8\xAC\xE8\xA9\xA6";
    auto hits = doc.page_hits(2, needle_utf8, Document::SearchFlags{});
    REQUIRE(hits.size() >= 1);
}

TEST_CASE("page_hits: no-hit query returns empty", "[document][search]") {
    Document doc = open_search_fixture();
    auto hits = doc.page_hits(0, "XYZABC123", Document::SearchFlags{});
    REQUIRE(hits.empty());
}

TEST_CASE("page_hits: quads lie in PDF coord space", "[document][search]") {
    Document doc = open_search_fixture();
    auto hits = doc.page_hits(0, "Lorem", Document::SearchFlags{});
    REQUIRE_FALSE(hits.empty());
    // Letter size is ~612x792 pt. Reject obviously-out-of-range quads.
    const auto& h = hits[0];
    REQUIRE(h.ul_x >= 0.0f);  REQUIRE(h.ul_x < 1000.0f);
    REQUIRE(h.ul_y >= 0.0f);  REQUIRE(h.ul_y < 1500.0f);
}

// --- MuPDF 1.27.2 incremental matcher: case / regex / whole-word / cancel ---

TEST_CASE("page_hits parity: ignore-case literal count", "[document][search]") {
    Document d; REQUIRE_FALSE(d.open("tests/fixtures/search.pdf").has_value());
    auto h = d.page_hits(0, "lorem", {false, false, false}, nullptr);
    REQUIRE(h.size() == 12);   // search.pdf page 0 "Lorem" count (fixture header)
}

TEST_CASE("page_hits honors match_case", "[document][search]") {
    Document d; REQUIRE_FALSE(d.open("tests/fixtures/search.pdf").has_value());
    auto ci = d.page_hits(0, "Lorem", {false, false, false}, nullptr);
    auto cs = d.page_hits(0, "lorem", {true,  false, false}, nullptr);
    REQUIRE(ci.size() == 12);  // same count as the ignore-case parity case
    REQUIRE(cs.size() == 0);   // lowercase needle, capitalized text
}

TEST_CASE("page_hits regex + whole_word", "[document][search]") {
    Document d; REQUIRE_FALSE(d.open("tests/fixtures/search.pdf").has_value());
    REQUIRE(d.page_hits(0, "lo.em", {false, false, true}).size() > 0);  // '.'='r'
    REQUIRE(d.page_hits(0, "lorem", {false, true,  false}).size() > 0); // whole word
}

TEST_CASE("query_compiles rejects bad regex", "[document][search]") {
    Document d; REQUIRE_FALSE(d.open("tests/fixtures/search.pdf").has_value());
    REQUIRE(d.query_compiles("lo.em", {false, false, true}));
    REQUIRE_FALSE(d.query_compiles("foo(", {false, false, true}));
    REQUIRE(d.query_compiles("", {false, false, true}));
}

TEST_CASE("page_hits abort_flag + leak stress", "[document][search]") {
    Document d; REQUIRE_FALSE(d.open("tests/fixtures/large.pdf").has_value());
    std::atomic<int> stop{1};
    REQUIRE(d.page_hits(0, "the", {false, false, false}, &stop).size() == 0);  // pre-aborted
    for (int i = 0; i < 200; ++i) {
        std::atomic<int> s{0};
        (void)d.page_hits(0, "the", {false, false, false}, &s);
        (void)d.page_hits(0, "foo(", {false, false, true}, nullptr);  // throws -> caught -> empty
    }
    REQUIRE(d.page_hits(0, "the", {false, false, false}, nullptr).size() > 0);  // still works
}
