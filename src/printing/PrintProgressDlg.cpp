// src/printing/PrintProgressDlg.cpp -- Phase 8.5 Task 4 (config mode)
// Task 5 will append progress-mode methods + dialog_proc handling.
#include "printing/PrintProgressDlg.hpp"

#include <commctrl.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace litepdf::printing {

namespace {

// ------------- DLGTEMPLATE byte-emit helpers (mirror PasswordDialog) --

void align_to_dword(std::vector<uint8_t>& v) {
    while ((v.size() % 4) != 0) v.push_back(0);
}

void emit_wstring(std::vector<uint8_t>& v, const std::wstring& s) {
    const auto* p = reinterpret_cast<const uint8_t*>(s.c_str());
    v.insert(v.end(), p, p + (s.size() + 1) * sizeof(wchar_t));
}

void emit_word(std::vector<uint8_t>& v, WORD w) {
    v.insert(v.end(),
             reinterpret_cast<const uint8_t*>(&w),
             reinterpret_cast<const uint8_t*>(&w) + sizeof(WORD));
}

void emit_bytes(std::vector<uint8_t>& v, const void* src, std::size_t n) {
    const auto* p = static_cast<const uint8_t*>(src);
    v.insert(v.end(), p, p + n);
}

// ------------- Config dialog control IDs ------------------------------
constexpr int IDC_RB_FIT       = 1001;
constexpr int IDC_RB_ACTUAL    = 1002;
constexpr int IDC_RB_CUSTOM    = 1003;
constexpr int IDC_EDIT_PCT     = 1004;
constexpr int IDC_LABEL_PCT    = 1005;

// Build an in-memory DLGTEMPLATE for the pre-flight scale picker.
// All sizes are dialog units (DLU). Layout: 180 x 110.
std::vector<uint8_t> build_config_template() {
    std::vector<uint8_t> out;
    out.reserve(512);

    DLGTEMPLATE hdr{};
    hdr.style = DS_MODALFRAME | DS_CENTER | DS_SETFONT |
                WS_POPUP | WS_CAPTION | WS_SYSMENU;
    hdr.dwExtendedStyle = 0;
    hdr.cdit = 8;  // group + 3 radios + edit + label + OK + Cancel
    hdr.x = 0; hdr.y = 0;
    hdr.cx = 180; hdr.cy = 110;
    emit_bytes(out, &hdr, sizeof(hdr));

    emit_word(out, 0x0000);  // no menu
    emit_word(out, 0x0000);  // default windowClass
    emit_wstring(out, std::wstring(L"Print"));
    emit_word(out, 9);                          // DS_SETFONT pointsize
    emit_wstring(out, std::wstring(L"Segoe UI"));

    auto emit_button_item = [&](LONG style, short x, short y, short cx, short cy,
                                WORD id, const std::wstring& text) {
        align_to_dword(out);
        DLGITEMTEMPLATE item{};
        item.style = WS_CHILD | WS_VISIBLE | style;
        item.x = x; item.y = y; item.cx = cx; item.cy = cy;
        item.id = id;
        emit_bytes(out, &item, sizeof(item));
        emit_word(out, 0xFFFF); emit_word(out, 0x0080);  // BUTTON class atom
        emit_wstring(out, text);
        emit_word(out, 0x0000);                          // creation data length
    };

    auto emit_static_item = [&](short x, short y, short cx, short cy,
                                WORD id, const std::wstring& text) {
        align_to_dword(out);
        DLGITEMTEMPLATE item{};
        item.style = WS_CHILD | WS_VISIBLE | SS_LEFT;
        item.x = x; item.y = y; item.cx = cx; item.cy = cy;
        item.id = id;
        emit_bytes(out, &item, sizeof(item));
        emit_word(out, 0xFFFF); emit_word(out, 0x0082);  // STATIC class atom
        emit_wstring(out, text);
        emit_word(out, 0x0000);
    };

    auto emit_edit_item = [&](LONG style, short x, short y, short cx, short cy,
                              WORD id) {
        align_to_dword(out);
        DLGITEMTEMPLATE item{};
        item.style = WS_CHILD | WS_VISIBLE | style;
        item.x = x; item.y = y; item.cx = cx; item.cy = cy;
        item.id = id;
        emit_bytes(out, &item, sizeof(item));
        emit_word(out, 0xFFFF); emit_word(out, 0x0081);  // EDIT class atom
        emit_wstring(out, std::wstring{});               // empty initial text
        emit_word(out, 0x0000);
    };

    // Item 0: Group "Print Scale" (BS_GROUPBOX)
    emit_button_item(BS_GROUPBOX, 5, 5, 170, 75, 0xFFFF, L"Print Scale");

    // Item 1: Radio "Fit to page" (starts radio group)
    emit_button_item(BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP,
                     10, 18, 110, 10, IDC_RB_FIT, L"Fit to page");

    // Item 2: Radio "Actual size"
    emit_button_item(BS_AUTORADIOBUTTON,
                     10, 33, 110, 10, IDC_RB_ACTUAL, L"Actual size");

    // Item 3: Radio "Custom %"
    emit_button_item(BS_AUTORADIOBUTTON,
                     10, 48, 70, 10, IDC_RB_CUSTOM, L"Custom %");

    // Item 4: Edit (numeric, custom percentage)
    emit_edit_item(ES_NUMBER | WS_BORDER | WS_TABSTOP,
                   85, 47, 30, 12, IDC_EDIT_PCT);

    // Item 5: Static "%" label after edit
    emit_static_item(120, 49, 10, 10, IDC_LABEL_PCT, L"%");

    // Item 6: OK (default push)
    emit_button_item(BS_DEFPUSHBUTTON | WS_TABSTOP,
                     70, 88, 50, 14, IDOK, L"OK");

    // Item 7: Cancel
    emit_button_item(BS_PUSHBUTTON | WS_TABSTOP,
                     125, 88, 50, 14, IDCANCEL, L"Cancel");

    return out;
}

INT_PTR CALLBACK config_dialog_proc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* result = reinterpret_cast<ScaleChoice*>(
        GetWindowLongPtrW(hDlg, DWLP_USER));
    switch (msg) {
        case WM_INITDIALOG: {
            SetWindowLongPtrW(hDlg, DWLP_USER, lp);
            // Default: Fit to page.
            CheckRadioButton(hDlg, IDC_RB_FIT, IDC_RB_CUSTOM, IDC_RB_FIT);
            SetDlgItemTextW(hDlg, IDC_EDIT_PCT, L"100");
            EnableWindow(GetDlgItem(hDlg, IDC_EDIT_PCT), FALSE);
            return TRUE;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            if (id == IDC_RB_FIT || id == IDC_RB_ACTUAL || id == IDC_RB_CUSTOM) {
                EnableWindow(GetDlgItem(hDlg, IDC_EDIT_PCT), id == IDC_RB_CUSTOM);
                return TRUE;
            }
            if (id == IDOK) {
                if (!result) {
                    EndDialog(hDlg, IDCANCEL);
                    return TRUE;
                }
                if (IsDlgButtonChecked(hDlg, IDC_RB_FIT) == BST_CHECKED) {
                    *result = { ScaleMode::FitToPage, 100.0f };
                } else if (IsDlgButtonChecked(hDlg, IDC_RB_ACTUAL) == BST_CHECKED) {
                    *result = { ScaleMode::ActualSize, 100.0f };
                } else {
                    BOOL ok = FALSE;
                    UINT pct = GetDlgItemInt(hDlg, IDC_EDIT_PCT, &ok, FALSE);
                    if (!ok || pct < 10 || pct > 400) {
                        MessageBoxW(hDlg,
                            L"Custom scale must be between 10 and 400 percent.",
                            L"Invalid scale", MB_OK | MB_ICONWARNING);
                        return TRUE;
                    }
                    *result = { ScaleMode::CustomPct, static_cast<float>(pct) };
                }
                EndDialog(hDlg, IDOK);
                return TRUE;
            }
            if (id == IDCANCEL) {
                EndDialog(hDlg, IDCANCEL);
                return TRUE;
            }
            break;
        }
    }
    return FALSE;
}

} // anonymous namespace

std::optional<ScaleChoice> PrintProgressDlg::show_config(HWND parent) {
    ScaleChoice chosen{ ScaleMode::FitToPage, 100.0f };

    auto tmpl = build_config_template();
    if (tmpl.empty()) return std::nullopt;

    INT_PTR rc = DialogBoxIndirectParamW(
        GetModuleHandleW(nullptr),
        reinterpret_cast<DLGTEMPLATE*>(tmpl.data()),
        parent,
        config_dialog_proc,
        reinterpret_cast<LPARAM>(&chosen));

    return rc == IDOK ? std::optional<ScaleChoice>{chosen} : std::nullopt;
}

// PrintProgressDlg constructor / destructor / set_progress / close
// implemented in Task 5.

} // namespace litepdf::printing
