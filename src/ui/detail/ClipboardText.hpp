#pragma once

// #52: UTF-8 -> UTF-16 for CF_UNICODETEXT. Header-only so it is unit-testable;
// the clipboard calls themselves live in ui/Clipboard.cpp.

#include <windows.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace litepdf::ui {

// An explicit byte length, so the result holds no terminator -- the same
// convention as OutlinePane and ResultsPanel. Invalid sequences become U+FFFD
// (no MB_ERR_INVALID_CHARS). Selection text is far below INT_MAX bytes.
inline std::wstring utf8_to_utf16(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int len  = static_cast<int>(utf8.size());
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), len, nullptr, 0);
    if (wlen <= 0) return {};
    std::wstring out(static_cast<std::size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), len, out.data(), wlen);
    return out;
}

}  // namespace litepdf::ui
