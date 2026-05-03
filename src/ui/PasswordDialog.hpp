#pragma once

// ui::PasswordDialog — modal Win32 password prompt for encrypted PDF.
//
// In-memory DLGTEMPLATE (Phase 8 D1); no .rc resource. The caller invokes
// prompt() from the open path on Document::OpenError::NeedsPassword.
// Returns the entered password (UTF-8) on accept, std::nullopt on cancel.
//
// Verification (Document::authenticate) is the caller's responsibility.
// The dialog does NOT loop; loop+attempt-counting lives in
// ui::try_authenticate_with_retry (password_retry.hpp) so the loop stays
// unit-testable without Win32.
//
// Memory hygiene (Phase 8 D3): the dialog wipes its on-stack edit buffer
// and the wide-character intermediate before returning. The returned
// std::string holds the password in UTF-8; the caller is responsible for
// SecureZeroMemory'ing it after Document::authenticate consumes it.

#include <optional>
#include <string>
#include <windows.h>

namespace litepdf::ui {

class PasswordDialog {
public:
    // Show the modal. Blocks until OK or Cancel.
    //
    // @param parent       The window the dialog is modal to (typically
    //                     the MainWindow HWND). May be nullptr.
    // @param basename     Human-readable file name (UTF-16) shown above
    //                     the input. Empty allowed (just shows generic
    //                     prompt).
    // @param status_text  Text shown next to the input. Use to display
    //                     "Incorrect password (N attempts remaining)"
    //                     between retries. Empty hides the status row.
    static std::optional<std::string> prompt(HWND parent,
                                             const std::wstring& basename,
                                             const std::wstring& status_text);

private:
    PasswordDialog() = delete;
};

}  // namespace litepdf::ui
