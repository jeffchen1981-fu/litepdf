#include "core/MruList.hpp"

#include <windows.h>
#include <algorithm>

namespace litepdf::core {

MruList::MruList(const wchar_t* registry_subkey)
    : registry_subkey_(registry_subkey) {}

void MruList::load() {
    entries_.clear();
    HKEY hkey = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, registry_subkey_.c_str(),
                            0, KEY_READ, &hkey);
    if (rc != ERROR_SUCCESS) return;  // key doesn't exist yet → empty

    DWORD count = 0;
    DWORD cb = sizeof(count);
    rc = RegQueryValueExW(hkey, L"Count", nullptr, nullptr,
                          reinterpret_cast<BYTE*>(&count), &cb);
    if (rc != ERROR_SUCCESS || count == 0) {
        RegCloseKey(hkey);
        return;
    }
    if (count > kMaxEntries) count = static_cast<DWORD>(kMaxEntries);

    for (DWORD i = 0; i < count; ++i) {
        wchar_t name[16];
        wsprintfW(name, L"Entry%u", i);

        wchar_t buf[1024] = {};
        DWORD buf_cb = sizeof(buf);
        rc = RegQueryValueExW(hkey, name, nullptr, nullptr,
                              reinterpret_cast<BYTE*>(buf), &buf_cb);
        if (rc == ERROR_SUCCESS) {
            buf[sizeof(buf)/sizeof(wchar_t) - 1] = L'\0';  // defensive: REG_SZ may not be null-terminated
            if (buf[0] != L'\0') entries_.emplace_back(buf);
        }
    }
    RegCloseKey(hkey);
}

bool MruList::save() const {
    HKEY hkey = nullptr;
    DWORD disp = 0;
    LONG rc = RegCreateKeyExW(HKEY_CURRENT_USER, registry_subkey_.c_str(),
                              0, nullptr, REG_OPTION_NON_VOLATILE,
                              KEY_WRITE, nullptr, &hkey, &disp);
    if (rc != ERROR_SUCCESS) return false;

    bool ok = true;
    DWORD count = static_cast<DWORD>(entries_.size());
    if (RegSetValueExW(hkey, L"Count", 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&count), sizeof(count))
        != ERROR_SUCCESS) {
        ok = false;
    }

    for (DWORD i = 0; i < count; ++i) {
        wchar_t name[16];
        wsprintfW(name, L"Entry%u", i);
        const auto& e = entries_[i];
        if (RegSetValueExW(hkey, name, 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(e.c_str()),
                           static_cast<DWORD>((e.size() + 1) * sizeof(wchar_t)))
            != ERROR_SUCCESS) {
            ok = false;
        }
    }
    // Clean up stale entries beyond current count (from a previous
    // session that may have had more items). RegDeleteValueW errors are
    // ignored here — stale values are cosmetic, not a correctness issue.
    for (DWORD i = count; i < static_cast<DWORD>(kMaxEntries); ++i) {
        wchar_t name[16];
        wsprintfW(name, L"Entry%u", i);
        RegDeleteValueW(hkey, name);
    }
    RegCloseKey(hkey);
    return ok;
}

void MruList::push(const std::wstring& path) {
    // Remove existing duplicate (case-insensitive on Windows).
    entries_.erase(
        std::remove_if(entries_.begin(), entries_.end(),
            [&](const std::wstring& e) {
                return _wcsicmp(e.c_str(), path.c_str()) == 0;
            }),
        entries_.end());

    entries_.insert(entries_.begin(), path);

    if (entries_.size() > kMaxEntries)
        entries_.resize(kMaxEntries);
}

void MruList::remove(std::size_t index) {
    if (index < entries_.size())
        entries_.erase(entries_.begin() + static_cast<ptrdiff_t>(index));
}

}  // namespace litepdf::core
