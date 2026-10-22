#include "core/ViewPrefs.hpp"

#include <windows.h>

namespace litepdf::core {

namespace {
constexpr wchar_t kContinuousScrollValue[] = L"ContinuousScroll";
}  // namespace

ViewPrefs::ViewPrefs(const wchar_t* registry_subkey)
    : registry_subkey_(registry_subkey) {}

void ViewPrefs::load() {
    continuous_scroll_ = kContinuousScrollDefault;

    HKEY hkey = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, registry_subkey_.c_str(),
                            0, KEY_READ, &hkey);
    if (rc != ERROR_SUCCESS) return;  // key doesn't exist yet -> defaults

    DWORD value = 0;
    DWORD type = 0;
    DWORD cb = sizeof(value);
    rc = RegQueryValueExW(hkey, kContinuousScrollValue, nullptr, &type,
                          reinterpret_cast<BYTE*>(&value), &cb);
    RegCloseKey(hkey);
    if (rc == ERROR_SUCCESS && type == REG_DWORD && cb == sizeof(value)) {
        continuous_scroll_ = (value != 0);
    }
}

bool ViewPrefs::save() const {
    HKEY hkey = nullptr;
    LONG rc = RegCreateKeyExW(HKEY_CURRENT_USER, registry_subkey_.c_str(),
                              0, nullptr, REG_OPTION_NON_VOLATILE,
                              KEY_WRITE, nullptr, &hkey, nullptr);
    if (rc != ERROR_SUCCESS) return false;

    const DWORD value = continuous_scroll_ ? 1 : 0;
    rc = RegSetValueExW(hkey, kContinuousScrollValue, 0, REG_DWORD,
                        reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(hkey);
    return rc == ERROR_SUCCESS;
}

}  // namespace litepdf::core
