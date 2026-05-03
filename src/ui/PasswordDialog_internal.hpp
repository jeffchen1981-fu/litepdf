#pragma once

// Private header for ui::PasswordDialog — exposes the in-memory
// DLGTEMPLATE byte builder for unit testing. Production code must use
// PasswordDialog::prompt() from PasswordDialog.hpp.

#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>

namespace litepdf::ui::detail {

// Build an in-memory DLGTEMPLATE + DLGITEMTEMPLATE entries packaged as a
// flat byte vector suitable for DialogBoxIndirectParamW. WORD-aligned
// per MSDN. status_text non-empty adds a 5th status item; empty produces
// 4 items (label, edit, OK, Cancel).
std::vector<uint8_t> build_dialog_template(const std::wstring& basename,
                                           const std::wstring& status_text,
                                           UINT dpi);

// Numeric IDs of the controls in the dialog template above. Exposed so
// the dialog procedure (and tests) can refer to them by name.
constexpr WORD kIdEdit   = 1001;
constexpr WORD kIdStatus = 1003;

}  // namespace litepdf::ui::detail
