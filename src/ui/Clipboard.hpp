#pragma once

// #52: write text to the Windows clipboard.

#include <windows.h>

#include <string_view>

namespace litepdf::ui {

// Replace the clipboard contents with `utf8` as CF_UNICODETEXT. Returns true iff
// the clipboard now holds it.
//
// Empty text touches nothing -- opening and emptying the clipboard to write
// nothing would destroy whatever the user had there. A clipboard held by another
// process (Explorer, Office -- usually transient) is a silent failure: no retry,
// no message box. Ctrl+C doing nothing once beats a modal interrupting a copy.
bool set_clipboard_text(HWND owner, std::string_view utf8);

}  // namespace litepdf::ui
