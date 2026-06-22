#include "core/SystemFonts.hpp"

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
