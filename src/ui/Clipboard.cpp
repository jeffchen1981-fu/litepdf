#include "ui/Clipboard.hpp"

#include "ui/detail/ClipboardText.hpp"

#include <cstddef>
#include <cstring>
#include <string>

namespace litepdf::ui {

namespace {

// Every path past a successful OpenClipboard must reach CloseClipboard. A
// clipboard left open stays held for the rest of the process's life and breaks
// copy and paste in every other application. A guard, not a sequence of early
// returns that each have to remember.
class ClipboardSession {
public:
    explicit ClipboardSession(HWND owner) noexcept
        : open_(OpenClipboard(owner) != FALSE) {}
    ~ClipboardSession() {
        if (open_) CloseClipboard();
    }
    ClipboardSession(const ClipboardSession&)            = delete;
    ClipboardSession& operator=(const ClipboardSession&) = delete;

    bool is_open() const noexcept { return open_; }

private:
    bool open_;
};

}  // namespace

bool set_clipboard_text(HWND owner, std::string_view utf8) {
    if (utf8.empty()) return false;
    const std::wstring wide = utf8_to_utf16(utf8);
    if (wide.empty()) return false;

    // Build the handle BEFORE opening the clipboard, so it is held open for as
    // short a time as possible.
    const std::size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!h) return false;
    auto* dst = static_cast<wchar_t*>(GlobalLock(h));
    if (!dst) {
        GlobalFree(h);
        return false;
    }
    std::memcpy(dst, wide.c_str(), bytes);   // c_str() includes the terminator
    GlobalUnlock(h);

    ClipboardSession clip(owner);
    if (!clip.is_open()) {
        GlobalFree(h);
        return false;
    }
    if (!EmptyClipboard()) {
        GlobalFree(h);
        return false;
    }
    if (!SetClipboardData(CF_UNICODETEXT, h)) {
        GlobalFree(h);   // the call failed, so we still own the handle
        return false;
    }
    return true;         // the system owns `h` now -- never free it
}

}  // namespace litepdf::ui
