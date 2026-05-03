// Phase 8 Task 1: cover the in-memory DLGTEMPLATE byte builder. The
// modal itself cannot be driven from a headless test; integration is
// covered by the manual smoke at scripts/smoke-test.ps1.

#include "ui/PasswordDialog_internal.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using litepdf::ui::detail::build_dialog_template;

TEST_CASE("PasswordDialog template: header has DS_MODALFRAME + WS_POPUP",
          "[password_dialog]") {
    std::vector<uint8_t> tmpl = build_dialog_template(L"foo.pdf", L"", /*dpi=*/96);
    REQUIRE(tmpl.size() >= sizeof(DLGTEMPLATE));
    auto* hdr = reinterpret_cast<const DLGTEMPLATE*>(tmpl.data());
    REQUIRE((hdr->style & DS_MODALFRAME) != 0);
    REQUIRE((hdr->style & WS_POPUP)      != 0);
    REQUIRE(hdr->cdit == 4);  // label, edit, OK, Cancel
}

TEST_CASE("PasswordDialog template: empty basename still produces valid bytes",
          "[password_dialog]") {
    std::vector<uint8_t> tmpl = build_dialog_template(L"", L"", 96);
    REQUIRE(tmpl.size() > sizeof(DLGTEMPLATE));
}

TEST_CASE("PasswordDialog template: status text adds a 5th item",
          "[password_dialog]") {
    auto with    = build_dialog_template(L"x.pdf", L"Try again (2 attempts remaining.)", 96);
    auto without = build_dialog_template(L"x.pdf", L"",                                   96);
    REQUIRE(with.size() > without.size());
    auto* hdr_with    = reinterpret_cast<const DLGTEMPLATE*>(with.data());
    auto* hdr_without = reinterpret_cast<const DLGTEMPLATE*>(without.data());
    REQUIRE(hdr_with->cdit == 5);
    REQUIRE(hdr_without->cdit == 4);
}
