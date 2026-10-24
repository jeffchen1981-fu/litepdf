#include "core/ViewPrefs.hpp"

#include <catch2/catch_test_macros.hpp>
#include <windows.h>

using namespace litepdf::core;

// Disposable registry key so tests never touch the real Settings key.
static constexpr wchar_t kTestKey[] = L"SOFTWARE\\LitePDF\\TestSettings";

static void delete_test_key() {
    RegDeleteKeyW(HKEY_CURRENT_USER, kTestKey);
}

TEST_CASE("ViewPrefs: a missing value gives the default", "[view_prefs]") {
    delete_test_key();
    ViewPrefs prefs(kTestKey);
    prefs.load();
    REQUIRE(prefs.continuous_scroll() == ViewPrefs::kContinuousScrollDefault);
    REQUIRE(prefs.continuous_scroll());
    delete_test_key();
}

TEST_CASE("ViewPrefs: save and load round trip", "[view_prefs]") {
    delete_test_key();
    {
        ViewPrefs prefs(kTestKey);
        prefs.set_continuous_scroll(true);
        REQUIRE(prefs.save());
    }
    {
        ViewPrefs prefs(kTestKey);
        prefs.load();
        REQUIRE(prefs.continuous_scroll());
        prefs.set_continuous_scroll(false);
        REQUIRE(prefs.save());
    }
    {
        ViewPrefs prefs(kTestKey);
        prefs.set_continuous_scroll(true);  // load must overwrite this
        prefs.load();
        REQUIRE_FALSE(prefs.continuous_scroll());
    }
    delete_test_key();
}

TEST_CASE("ViewPrefs: a value of the wrong type is ignored", "[view_prefs]") {
    delete_test_key();
    HKEY hkey = nullptr;
    REQUIRE(RegCreateKeyExW(HKEY_CURRENT_USER, kTestKey, 0, nullptr,
                            REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
                            &hkey, nullptr) == ERROR_SUCCESS);
    const wchar_t one[] = L"1";
    REQUIRE(RegSetValueExW(hkey, L"ContinuousScroll", 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(one), sizeof(one))
            == ERROR_SUCCESS);
    RegCloseKey(hkey);

    ViewPrefs prefs(kTestKey);
    prefs.load();
    REQUIRE(prefs.continuous_scroll() == ViewPrefs::kContinuousScrollDefault);
    delete_test_key();
}

// With the default on, a REG_SZ that misreads as non-zero cannot tell an ignored
// value from a misread one, so this string's four bytes are all zero: read as a
// DWORD it would turn the preference off.
TEST_CASE("ViewPrefs: a string value that reads as zero is ignored", "[view_prefs]") {
    delete_test_key();
    HKEY hkey = nullptr;
    REQUIRE(RegCreateKeyExW(HKEY_CURRENT_USER, kTestKey, 0, nullptr,
                            REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
                            &hkey, nullptr) == ERROR_SUCCESS);
    const wchar_t zeros[2] = {L'\0', L'\0'};
    static_assert(sizeof(zeros) == sizeof(DWORD), "must pass the size check");
    REQUIRE(RegSetValueExW(hkey, L"ContinuousScroll", 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(zeros), sizeof(zeros))
            == ERROR_SUCCESS);
    RegCloseKey(hkey);

    ViewPrefs prefs(kTestKey);
    prefs.load();
    REQUIRE(prefs.continuous_scroll());
    delete_test_key();
}
