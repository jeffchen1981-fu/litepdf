// Tests for Document::page_hits — stateless MuPDF search bridge (Phase 6 Task 3).
//
// The search.pdf fixture (Task 2) contains:
//   - page 0 : "Lorem" x 12 (among other text)
//   - page 2 : a CJK phrase (4 codepoints, U+4E2D U+6587 U+6E2C U+8A66)
//              present at least once
//   - page 4 : no "Lorem"
//
// MuPDF 1.24.11 ships only fz_search_page (case-insensitive). The case-
// sensitive test is marked [!mayfail] so it does not fail the suite: it
// is expected to fail until we upgrade MuPDF (Phase 11) or swap to
// stext+custom matcher.
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

// Revisit trigger: the Phase 11 MuPDF upgrade (FZ_SEARCH_EXACT /
// fz_search_page2) is expected to make this test pass. When that happens,
// Catch2's [!shouldfail] will flip the test RED to force a manual re-audit:
// remove the tag, adjust the case-sensitivity code path in Document.cpp, and
// confirm the semantics still match what callers expect.
TEST_CASE("page_hits: case-sensitive does not match mismatched case", "[document][search][!shouldfail]") {
    // MuPDF 1.24.11's fz_search_page is always case-insensitive (canon() in
    // stext-search.c uppercases every codepoint before matching). This test
    // therefore fails on 1.24.x: lowercase "lorem" still matches "Lorem".
    // Marked [!shouldfail] so Catch2 inverts the expectation — the failing
    // REQUIRE below counts as a pass today, and if a future MuPDF upgrade
    // makes it actually pass, Catch2 will flip it RED so we revisit.
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
