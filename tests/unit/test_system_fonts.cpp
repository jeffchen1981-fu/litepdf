#include "core/SystemFonts.hpp"
#include "core/Document.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using litepdf::core::cjk_family_candidates;
using WVec = std::vector<std::wstring>;

TEST_CASE("cjk_family_candidates: maps ordering+serif to Windows families",
          "[core][fonts][cjk]") {
    // FZ_ADOBE_CNS=0 (Traditional Chinese)
    REQUIRE(cjk_family_candidates(0, 1) == WVec{L"PMingLiU", L"MingLiU"});
    REQUIRE(cjk_family_candidates(0, 0) ==
            WVec{L"Microsoft JhengHei", L"Microsoft JhengHei UI"});
    // FZ_ADOBE_GB=1 (Simplified Chinese)
    REQUIRE(cjk_family_candidates(1, 1) == WVec{L"SimSun", L"NSimSun"});
    REQUIRE(cjk_family_candidates(1, 0) ==
            WVec{L"Microsoft YaHei", L"Microsoft YaHei UI"});
    // FZ_ADOBE_JAPAN=2
    REQUIRE(cjk_family_candidates(2, 1) == WVec{L"MS Mincho", L"MS PMincho"});
    REQUIRE(cjk_family_candidates(2, 0) == WVec{L"Yu Gothic", L"MS Gothic", L"Meiryo"});
    // FZ_ADOBE_KOREA=3
    REQUIRE(cjk_family_candidates(3, 1) == WVec{L"Batang", L"BatangChe"});
    REQUIRE(cjk_family_candidates(3, 0) == WVec{L"Malgun Gothic", L"Gulim"});
}

TEST_CASE("cjk_family_candidates: unknown ordering is empty",
          "[core][fonts][cjk]") {
    REQUIRE(cjk_family_candidates(99, 1).empty());
    REQUIRE(cjk_family_candidates(-1, 0).empty());
}

// Forward-declare the fz ABI bits the test drops. litepdf_core links mupdf
// PRIVATE, so the test target has no mupdf include path — local extern-C decls
// are enough (same pattern as test_document_clone_context.cpp).
extern "C" {
struct fz_context;
struct fz_font;
void fz_drop_context(fz_context* ctx);
void fz_drop_font(fz_context* ctx, fz_font* font);
}

using litepdf::core::Document;
using litepdf::core::resolve_cjk_system_font;

// Test-case NAME must stay ASCII: catch_discover_tests round-trips it through
// ctest as a --filter, and a non-ASCII char (em-dash) gets mangled on Windows,
// so ctest finds "No test cases matched" and the test fails (CI-only). ASCII '-'.
TEST_CASE("resolve_cjk_system_font: never returns NULL - base14 last resort (D6)",
          "[core][fonts][cjk][d6]") {
    Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());
    fz_context* ctx = doc.clone_context();
    REQUIRE(ctx != nullptr);

    // Ordering 99 matches no candidate family, so DirectWrite resolution is
    // skipped entirely and the loader must fall through to the base14 Helvetica
    // last-resort — deterministic on any machine, even one with CJK fonts.
    fz_font* font = resolve_cjk_system_font(ctx, "SourceHanSerif", 99, 1);
    REQUIRE(font != nullptr);

    fz_drop_font(ctx, font);
    fz_drop_context(ctx);
}
