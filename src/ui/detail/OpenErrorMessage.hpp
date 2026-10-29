#pragma once
// ui::detail::unconvertible_path_message -- the open dialog's text for
// Document::OpenError::UnconvertiblePath.
//
// Document::open converts the WHOLE path to UTF-8, so the error says only that
// some component holds an unpaired UTF-16 surrogate. A good file inside such a
// folder gets it too, and "Rename the file" does not help there (#184). The
// dialog has the path, so it works out which component to name: the file, the
// first folder that does not convert, or both.

#include <filesystem>
#include <string>
#include <system_error>

namespace litepdf::ui::detail {

// path::u8string() throws std::system_error for a component with no UTF-8 form
// (MSVC's STL converts with WC_ERR_INVALID_CHARS) -- the same test the open
// failed, so the verdict here matches it.
inline bool has_utf8_form(const std::filesystem::path& p) {
    try {
        (void)p.u8string();
        return true;
    } catch (const std::system_error&) {
        return false;
    }
}

// `path` is the path the open failed on, or empty when the worker posted none.
inline std::wstring unconvertible_path_message(const std::filesystem::path& path) {
    const bool name_bad = !path.filename().empty() && !has_utf8_form(path.filename());

    // The first (outermost) bad folder, if there are several. relative_path()
    // leaves out the drive and root.
    std::wstring folder;
    for (const auto& part : path.parent_path().relative_path()) {
        if (!has_utf8_form(part)) {
            folder = part.wstring();
            break;
        }
    }

    if (name_bad && folder.empty()) {
        return L"The file name contains an invalid Unicode character. "
               L"Rename the file and try again.";
    }
    if (!folder.empty() && !name_bad) {
        return L"The folder \"" + folder + L"\" in the file's path contains an "
               L"invalid Unicode character. Rename the folder or move the file "
               L"to another folder, and try again.";
    }
    if (!folder.empty()) {
        return L"The file name and the folder \"" + folder + L"\" in its path "
               L"contain invalid Unicode characters. Rename both and try again.";
    }
    // No path, or one that converts: nothing to name.
    return L"The file's path contains an invalid Unicode character. "
           L"Rename the file or the folders holding it, and try again.";
}

}  // namespace litepdf::ui::detail
