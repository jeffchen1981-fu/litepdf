#include "ui/PasswordDialog.hpp"
#include "ui/PasswordDialog_internal.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
#include <commctrl.h>  // EM_SETCUEBANNER

#pragma comment(lib, "comctl32.lib")

namespace litepdf::ui {

namespace detail {

namespace {

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

}  // namespace

std::vector<uint8_t> build_dialog_template(const std::wstring& basename,
                                           const std::wstring& status_text,
                                           UINT /*dpi*/) {
    std::vector<uint8_t> out;
    out.reserve(512);

    DLGTEMPLATE hdr{};
    hdr.style = DS_MODALFRAME | DS_CENTER | DS_SETFONT |
                WS_POPUP | WS_CAPTION | WS_SYSMENU;
    hdr.dwExtendedStyle = 0;
    hdr.cdit = 4;  // patched at end if status row is added
    hdr.x = 0; hdr.y = 0;
    hdr.cx = 200;
    hdr.cy = status_text.empty() ? 70 : 90;
    emit_bytes(out, &hdr, sizeof(hdr));

    // After header: menu, windowClass, title, then DS_SETFONT pointsize +
    // typeface. 0x0000 means "none" / "default class".
    emit_word(out, 0x0000);
    emit_word(out, 0x0000);
    emit_wstring(out, std::wstring(L"Password Required"));
    emit_word(out, 9);
    emit_wstring(out, std::wstring(L"Segoe UI"));

    // ---- Item 1: static label ----------------------------------------
    align_to_dword(out);
    {
        DLGITEMTEMPLATE item{};
        item.style = WS_CHILD | WS_VISIBLE | SS_LEFT;
        item.x = 7; item.y = 7; item.cx = 186; item.cy = 16;
        item.id = static_cast<WORD>(-1);
        emit_bytes(out, &item, sizeof(item));
        emit_word(out, 0xFFFF); emit_word(out, 0x0082);  // STATIC class atom
        std::wstring label = L"Enter password";
        if (!basename.empty()) {
            label += L" for ";
            label += basename;
        }
        label += L":";
        emit_wstring(out, label);
        emit_word(out, 0x0000);  // creation data length
    }

    // ---- Item 2: edit (ES_PASSWORD) ----------------------------------
    align_to_dword(out);
    {
        DLGITEMTEMPLATE item{};
        item.style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER |
                     ES_AUTOHSCROLL | ES_PASSWORD;
        item.x = 7; item.y = 25; item.cx = 186; item.cy = 14;
        item.id = kIdEdit;
        emit_bytes(out, &item, sizeof(item));
        emit_word(out, 0xFFFF); emit_word(out, 0x0081);  // EDIT class atom
        emit_wstring(out, std::wstring{});
        emit_word(out, 0x0000);
    }

    // ---- Optional item: status label ---------------------------------
    if (!status_text.empty()) {
        align_to_dword(out);
        DLGITEMTEMPLATE item{};
        item.style = WS_CHILD | WS_VISIBLE | SS_LEFT;
        item.x = 7; item.y = 43; item.cx = 186; item.cy = 14;
        item.id = kIdStatus;
        emit_bytes(out, &item, sizeof(item));
        emit_word(out, 0xFFFF); emit_word(out, 0x0082);  // STATIC class atom
        emit_wstring(out, status_text);
        emit_word(out, 0x0000);
    }

    // ---- Item: OK button ---------------------------------------------
    const int btn_y = status_text.empty() ? 47 : 65;
    align_to_dword(out);
    {
        DLGITEMTEMPLATE item{};
        item.style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON;
        item.x = 80; item.y = static_cast<short>(btn_y);
        item.cx = 50; item.cy = 14;
        item.id = IDOK;
        emit_bytes(out, &item, sizeof(item));
        emit_word(out, 0xFFFF); emit_word(out, 0x0080);  // BUTTON class atom
        emit_wstring(out, std::wstring(L"OK"));
        emit_word(out, 0x0000);
    }

    // ---- Item: Cancel button -----------------------------------------
    align_to_dword(out);
    {
        DLGITEMTEMPLATE item{};
        item.style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON;
        item.x = 140; item.y = static_cast<short>(btn_y);
        item.cx = 50; item.cy = 14;
        item.id = IDCANCEL;
        emit_bytes(out, &item, sizeof(item));
        emit_word(out, 0xFFFF); emit_word(out, 0x0080);  // BUTTON class atom
        emit_wstring(out, std::wstring(L"Cancel"));
        emit_word(out, 0x0000);
    }

    // Patch cdit to actual control count (4 without status, 5 with).
    auto* hdr_patch = reinterpret_cast<DLGTEMPLATE*>(out.data());
    hdr_patch->cdit = static_cast<WORD>(status_text.empty() ? 4 : 5);

    return out;
}

}  // namespace detail

namespace {

struct DialogState {
    std::wstring out_password;
    bool         accepted = false;
};

INT_PTR CALLBACK PasswordDlgProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    auto* st = reinterpret_cast<DialogState*>(
        GetWindowLongPtrW(hwnd, DWLP_USER));
    switch (msg) {
        case WM_INITDIALOG: {
            SetWindowLongPtrW(hwnd, DWLP_USER, l);
            HWND edit = GetDlgItem(hwnd, detail::kIdEdit);
            SetFocus(edit);
            // EM_SETCUEBANNER helps password managers identify the field
            // via accessible-name fallback and shows a hint until the
            // user types. The string MUST stay valid for the message
            // round-trip, but Edit_SetCueBannerText copies it.
            SendMessageW(edit, EM_SETCUEBANNER,
                         /*draw_when_focused=*/FALSE,
                         reinterpret_cast<LPARAM>(L"Password"));
            return FALSE;  // we set focus ourselves
        }
        case WM_COMMAND: {
            const int id = LOWORD(w);
            if (id == IDOK && st) {
                wchar_t buf[256] = {};
                GetDlgItemTextW(hwnd, detail::kIdEdit, buf, _countof(buf));
                st->out_password = buf;
                SecureZeroMemory(buf, sizeof(buf));
                st->accepted = true;
                EndDialog(hwnd, IDOK);
                return TRUE;
            }
            if (id == IDCANCEL) {
                if (st) st->accepted = false;
                EndDialog(hwnd, IDCANCEL);
                return TRUE;
            }
            break;
        }
        case WM_DESTROY:
            // Clear the edit control's text so its internal buffer does
            // not retain the password across the brief post-EndDialog
            // window before the HWND is freed.
            SetDlgItemTextW(hwnd, detail::kIdEdit, L"");
            return FALSE;
    }
    return FALSE;
}

}  // namespace

std::optional<std::string> PasswordDialog::prompt(HWND parent,
                                                  const std::wstring& basename,
                                                  const std::wstring& status_text) {
    UINT dpi = GetDpiForWindow(parent ? parent : GetDesktopWindow());
    auto tmpl = detail::build_dialog_template(basename, status_text, dpi);

    DialogState st{};
    INT_PTR r = DialogBoxIndirectParamW(
        GetModuleHandleW(nullptr),
        reinterpret_cast<DLGTEMPLATE*>(tmpl.data()),
        parent,
        &PasswordDlgProc,
        reinterpret_cast<LPARAM>(&st));

    if (r != IDOK || !st.accepted) {
        if (!st.out_password.empty()) {
            SecureZeroMemory(st.out_password.data(),
                             st.out_password.size() * sizeof(wchar_t));
        }
        return std::nullopt;
    }

    int n = WideCharToMultiByte(CP_UTF8, 0,
                                st.out_password.c_str(), -1,
                                nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? static_cast<size_t>(n - 1) : 0, '\0');
    if (n > 0) {
        WideCharToMultiByte(CP_UTF8, 0,
                            st.out_password.c_str(), -1,
                            out.data(), n, nullptr, nullptr);
    }
    if (!st.out_password.empty()) {
        SecureZeroMemory(st.out_password.data(),
                         st.out_password.size() * sizeof(wchar_t));
    }
    return out;
}

}  // namespace litepdf::ui
