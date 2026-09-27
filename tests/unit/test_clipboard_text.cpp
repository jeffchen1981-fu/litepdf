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

TEST_CASE("ClipboardText utf8 to utf16 replaces an invalid byte and keeps the rest",
          "[ui][clipboard]") {
    // 0xFF never occurs in UTF-8. The header promises U+FFFD, not an empty
    // result: with MB_ERR_INVALID_CHARS the conversion fails outright, and one
    // bad byte on a page would copy nothing at all.
    const std::wstring wide = litepdf::ui::utf8_to_utf16("a\xFF" "b");
    REQUIRE(wide == std::wstring(L"a�b"));
}

TEST_CASE("ClipboardText utf8 to utf16 encodes an astral character as a surrogate pair",
          "[ui][clipboard]") {
    // U+1F600, four UTF-8 bytes, two UTF-16 units.
    const std::wstring wide = litepdf::ui::utf8_to_utf16("\xF0\x9F\x98\x80" "z");
    REQUIRE(wide == std::wstring{ wchar_t(0xD83D), wchar_t(0xDE00), L'z' });
}
