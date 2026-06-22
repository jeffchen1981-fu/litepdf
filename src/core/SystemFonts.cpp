// src/core/SystemFonts.cpp
#include "core/SystemFonts.hpp"

namespace litepdf::core {

std::vector<std::wstring> cjk_family_candidates(int ordering, int serif) {
    // FZ_ADOBE_CNS=0, FZ_ADOBE_GB=1, FZ_ADOBE_JAPAN=2, FZ_ADOBE_KOREA=3.
    switch (ordering) {
        case 0:  // Traditional Chinese
            return serif ? std::vector<std::wstring>{L"PMingLiU", L"MingLiU"}
                         : std::vector<std::wstring>{L"Microsoft JhengHei",
                                                     L"Microsoft JhengHei UI"};
        case 1:  // Simplified Chinese
            return serif ? std::vector<std::wstring>{L"SimSun", L"NSimSun"}
                         : std::vector<std::wstring>{L"Microsoft YaHei",
                                                     L"Microsoft YaHei UI"};
        case 2:  // Japanese
            return serif ? std::vector<std::wstring>{L"MS Mincho", L"MS PMincho"}
                         : std::vector<std::wstring>{L"Yu Gothic", L"MS Gothic",
                                                     L"Meiryo"};
        case 3:  // Korean
            return serif ? std::vector<std::wstring>{L"Batang", L"BatangChe"}
                         : std::vector<std::wstring>{L"Malgun Gothic", L"Gulim"};
        default:
            return {};
    }
}

// resolve_cjk_system_font + install_system_cjk_font_loader are implemented in
// Task 2. Temporary stubs so the unit-test target links for the Task-1 mapping
// test. (fz_font is forward-declared; returning nullptr is valid here.)
fz_font* resolve_cjk_system_font(fz_context*, const char*, int, int) noexcept {
    return nullptr;
}
void install_system_cjk_font_loader(fz_context*) noexcept {}

}  // namespace litepdf::core
