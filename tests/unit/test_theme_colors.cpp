// #83 / #86 / #90: High Contrast palette rules shared by the custom-painted
// components -- when a theme message rebuilds a palette, and which colour
// pair each owner-drawn button state gets.
#include <catch2/catch_test_macros.hpp>

#include "ui/detail/ButtonColors.hpp"
#include "ui/detail/HighContrast.hpp"
#include "ui/detail/SplitterCore.hpp"
#include "ui/detail/ThumbnailPalette.hpp"

using litepdf::ui::detail::button_colors;
using litepdf::ui::detail::theme_needs_rebuild;

namespace {

// Every field distinct, so a test can tell which one was picked.
struct FakePalette {
    COLORREF btn_normal     = RGB(1, 0, 0);
    COLORREF btn_fg         = RGB(2, 0, 0);
    COLORREF btn_hover      = RGB(3, 0, 0);
    COLORREF btn_hover_fg   = RGB(4, 0, 0);
    COLORREF btn_pressed    = RGB(5, 0, 0);
    COLORREF btn_pressed_fg = RGB(6, 0, 0);
    COLORREF close_hover_bg = RGB(7, 0, 0);
    COLORREF close_hover_fg = RGB(8, 0, 0);
};

bool same(litepdf::ui::detail::ButtonColors c, COLORREF bg, COLORREF fg) {
    return c.bg == bg && c.fg == fg;
}

}  // namespace

TEST_CASE("Theme theme_needs_rebuild is a no-op when nothing changed outside High Contrast", "[theme]") {
    REQUIRE_FALSE(theme_needs_rebuild(false, false, false, false));
    REQUIRE_FALSE(theme_needs_rebuild(true, false, true, false));
}

TEST_CASE("Theme theme_needs_rebuild rebuilds on a dark or light flip", "[theme]") {
    REQUIRE(theme_needs_rebuild(false, false, true, false));
    REQUIRE(theme_needs_rebuild(true, false, false, false));
}

TEST_CASE("Theme theme_needs_rebuild rebuilds on entering or leaving High Contrast", "[theme]") {
    REQUIRE(theme_needs_rebuild(false, false, false, true));
    REQUIRE(theme_needs_rebuild(true, true, true, false));
}

TEST_CASE("Theme theme_needs_rebuild always rebuilds while High Contrast stays on", "[theme]") {
    // Switching between two contrast themes changes the system colours the
    // palette snapshot holds without changing either flag.
    REQUIRE(theme_needs_rebuild(false, true, false, true));
    REQUIRE(theme_needs_rebuild(true, true, true, true));
}

TEST_CASE("Theme button_colors pairs each state's background with its own foreground", "[theme]") {
    const FakePalette p;
    REQUIRE(same(button_colors(p, false, false, false, false), p.btn_normal, p.btn_fg));
    REQUIRE(same(button_colors(p, false, true,  false, false), p.btn_hover, p.btn_hover_fg));
    REQUIRE(same(button_colors(p, false, false, true,  false), p.btn_pressed, p.btn_pressed_fg));
    REQUIRE(same(button_colors(p, false, false, false, true),  p.btn_pressed, p.btn_pressed_fg));
}

TEST_CASE("Theme button_colors keeps a latched toggle latched under hover", "[theme]") {
    // A hovered latch must still read as ON, not as a hovered OFF.
    const FakePalette p;
    REQUIRE(same(button_colors(p, false, true, false, true), p.btn_pressed, p.btn_pressed_fg));
}

TEST_CASE("Theme button_colors gives a hovered close button its own pair, even pressed", "[theme]") {
    const FakePalette p;
    REQUIRE(same(button_colors(p, true, true, false, false), p.close_hover_bg, p.close_hover_fg));
    REQUIRE(same(button_colors(p, true, true, true,  false), p.close_hover_bg, p.close_hover_fg));
    REQUIRE(same(button_colors(p, true, false, false, false), p.btn_normal, p.btn_fg));
}

TEST_CASE("Theme splitter palette uses system colours under High Contrast", "[theme]") {
    using litepdf::ui::detail::make_palette;
    for (bool dark : {false, true}) {
        const auto pal = make_palette(dark, /*high_contrast=*/true);
        REQUIRE(pal.bar_bg    == GetSysColor(COLOR_BTNFACE));
        REQUIRE(pal.bar_hover == GetSysColor(COLOR_HIGHLIGHT));
    }
    // Outside High Contrast the fixed palettes are untouched.
    REQUIRE(make_palette(true,  false).bar_bg == RGB(0x3A, 0x3A, 0x3A));
    REQUIRE(make_palette(false, false).bar_bg == RGB(0xD8, 0xD8, 0xD8));
}

TEST_CASE("Theme thumbnail palette uses system colours under High Contrast", "[theme]") {
    using litepdf::ui::detail::make_thumbnail_palette;
    for (bool dark : {false, true}) {
        const auto pal = make_thumbnail_palette(dark, /*high_contrast=*/true);
        REQUIRE(pal.pane_bg        == GetSysColor(COLOR_WINDOW));
        REQUIRE(pal.tile_fill      == GetSysColor(COLOR_WINDOW));
        REQUIRE(pal.tile_border    == GetSysColor(COLOR_WINDOWTEXT));
        REQUIRE(pal.text           == GetSysColor(COLOR_WINDOWTEXT));
        REQUIRE(pal.current_border == GetSysColor(COLOR_HIGHLIGHT));
    }
    // Outside High Contrast the fixed palettes are untouched.
    REQUIRE(make_thumbnail_palette(true,  false).pane_bg == RGB(0x1F, 0x1F, 0x1F));
    REQUIRE(make_thumbnail_palette(false, false).pane_bg == RGB(0xF5, 0xF5, 0xF5));
}

TEST_CASE("Theme thumbnail palette leaves the accent to paint time outside High Contrast", "[theme]") {
    // #90: DWM reports black under High Contrast, so an accent frozen into the
    // palette would stay black. The sentinel makes the pane ask DWM per paint.
    using litepdf::ui::detail::make_thumbnail_palette;
    REQUIRE(make_thumbnail_palette(false, false).current_border == CLR_INVALID);
    REQUIRE(make_thumbnail_palette(true,  false).current_border == CLR_INVALID);
}
