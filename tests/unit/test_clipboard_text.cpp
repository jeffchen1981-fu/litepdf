// #52: UTF-8 -> UTF-16 for CF_UNICODETEXT (ui/detail/ClipboardText.hpp).
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "ui/detail/ClipboardText.hpp"

TEST_CASE("ClipboardText utf8 to utf16 keeps CJK and CRLF and adds no terminator",
          "[ui][clipboard]") {
    // "ab", CRLF, U+4E2D U+6587 -- spelled as escapes so this file stays ASCII.
    const std::string utf8 = "ab\r\n\xE4\xB8\xAD\xE6\x96\x87";
    const std::wstring wide = litepdf::ui::utf8_to_utf16(utf8);
    REQUIRE(wide == std::wstring(L"ab\r\n\u4E2D\u6587"));
    REQUIRE(wide.size() == 6);
}

TEST_CASE("ClipboardText utf8 to utf16 of empty text is empty", "[ui][clipboard]") {
    REQUIRE(litepdf::ui::utf8_to_utf16(std::string{}).empty());
}
