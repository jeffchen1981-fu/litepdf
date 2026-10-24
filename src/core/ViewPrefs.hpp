#pragma once

// core::ViewPrefs — view preferences backed by the Windows registry under
// HKCU. Headless and unit-testable: the constructor accepts a registry
// sub-key path so tests can use a disposable key (same shape as MruList).
//
// Usage:
//   ViewPrefs prefs;     // uses default key SOFTWARE\LitePDF\Settings
//   prefs.load();        // read from registry; missing/invalid -> defaults
//   prefs.set_continuous_scroll(true);
//   prefs.save();        // write back
//
// Values:
//   ContinuousScroll (REG_DWORD)  0 = single/dual page view, non-zero = strip;
//                                 missing = strip (#55).

#include <string>

namespace litepdf::core {

class ViewPrefs {
public:
    static constexpr wchar_t kDefaultKey[] = L"SOFTWARE\\LitePDF\\Settings";
    static constexpr bool kContinuousScrollDefault = true;

    explicit ViewPrefs(const wchar_t* registry_subkey = kDefaultKey);

    // Read values from the registry. A missing key, missing value or value
    // of the wrong type leaves the default in place.
    void load();

    // Write values to the registry. Returns false if the key could not be
    // created or a value write failed.
    bool save() const;

    bool continuous_scroll() const noexcept { return continuous_scroll_; }
    void set_continuous_scroll(bool on) noexcept { continuous_scroll_ = on; }

private:
    std::wstring registry_subkey_;
    bool continuous_scroll_ = kContinuousScrollDefault;
};

}  // namespace litepdf::core
