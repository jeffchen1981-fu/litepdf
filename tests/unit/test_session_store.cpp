#include <catch2/catch_test_macros.hpp>
#include "core/SessionStore.hpp"
#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <windows.h>
#include <aclapi.h>
#pragma comment(lib, "advapi32.lib")

using namespace litepdf::core;

static std::filesystem::path unique_temp(const wchar_t* stem, const wchar_t* ext) {
    static std::atomic<unsigned> seq{0};
    std::wstring name = std::wstring(stem) + L"_" + std::to_wstring(GetCurrentProcessId())
                      + L"_" + std::to_wstring(seq.fetch_add(1)) + ext;
    auto p = std::filesystem::temp_directory_path() / name;
    std::error_code ec; std::filesystem::remove_all(p, ec);   // file OR directory
    return p;
}

static std::filesystem::path temp_session() {
    auto p = unique_temp(L"litepdf_test_session", L".json");
    // The backup guard mints a `.v1.bak` sibling that `unique_temp` never sees.
    // Clear it too: a case that aborts on a failed REQUIRE throws past its own
    // trailing cleanup, and because catch_discover_tests runs every TEST_CASE in
    // a fresh process the `seq` counter restarts at 0 -- so a later run whose PID
    // is recycled would inherit that orphan and fail for an unrelated reason.
    // `remove_all`, not `remove`: one case parks a *directory* on that path.
    auto bak = p;
    bak.replace_extension(L".v1.bak");
    std::error_code ec; std::filesystem::remove_all(bak, ec);
    return p;
}

TEST_CASE("save then load round-trips", "[core][session][store]") {
    auto file = temp_session();
    SessionState s;
    s.tabs.push_back({std::filesystem::path(L"C:\\x\\y.pdf"), 4, SessionZoom::FitPage, 1.0f});

    REQUIRE(save_session(file, s));
    auto r = load_session(file);
    REQUIRE(r.has_value());
    REQUIRE(r->tabs.size() == 1);
    REQUIRE(r->tabs[0].page == 4);

    std::error_code ec; std::filesystem::remove(file, ec);
}

TEST_CASE("load of a missing file is nullopt", "[core][session][store]") {
    auto file = std::filesystem::temp_directory_path() / L"litepdf_does_not_exist.json";
    std::error_code ec; std::filesystem::remove(file, ec);
    REQUIRE_FALSE(load_session(file).has_value());
}

TEST_CASE("load rejects an oversized file", "[core][session][store]") {
    auto file = temp_session();
    { std::ofstream o(file, std::ios::binary); std::string big(kMaxSessionBytes + 1, 'x'); o.write(big.data(), big.size()); }
    REQUIRE_FALSE(load_session(file).has_value());
    std::error_code ec; std::filesystem::remove(file, ec);
}

TEST_CASE("save leaves the prior file intact when it cannot replace", "[core][session][store]") {
    // Make the destination a directory so MoveFileExW fails; the existing
    // good content must survive (here: the directory stays, no data clobbered).
    auto dir = unique_temp(L"litepdf_locked", L".json");
    std::error_code ec; std::filesystem::create_directory(dir, ec);
    SessionState s;
    REQUIRE_FALSE(save_session(dir, s));   // cannot overwrite a directory
    REQUIRE(std::filesystem::is_directory(dir, ec));
    std::filesystem::remove(dir, ec);
}

TEST_CASE("SessionStore backup keeps a v1 copy on first v2 save",
          "[core][session][store][migration]") {
    auto file = temp_session();
    const std::string v1 =
        "{\"version\":1,\"window\":{\"flags\":0,\"show\":1,"
        "\"x\":0,\"y\":0,\"w\":800,\"h\":600},\"active\":0,\"tabs\":[]}";
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(v1.data(), (std::streamsize)v1.size());
    }

    SessionState s;   // defaults to the current version
    REQUIRE(save_session(file, s));

    auto bak = file;
    bak.replace_extension(L".v1.bak");
    REQUIRE(std::filesystem::exists(bak));

    {
        std::ifstream in(bak, std::ios::binary);
        std::ostringstream ss; ss << in.rdbuf();
        REQUIRE(ss.str() == v1);   // byte-identical to the original
    }

    std::error_code ec;
    std::filesystem::remove(file, ec);
    std::filesystem::remove(bak, ec);
}

TEST_CASE("SessionStore backup is not rewritten once the file is v2",
          "[core][session][store][migration]") {
    auto file = temp_session();
    SessionState s;
    REQUIRE(save_session(file, s));       // creates a v2 file, no backup needed
    auto bak = file;
    bak.replace_extension(L".v1.bak");
    REQUIRE_FALSE(std::filesystem::exists(bak));
    REQUIRE(save_session(file, s));       // and still none on a second save
    REQUIRE_FALSE(std::filesystem::exists(bak));

    std::error_code ec;
    std::filesystem::remove(file, ec);
}

// Denies FILE_READ_DATA to the caller's own SID for the lifetime of the object
// and restores the original DACL in the destructor -- including when a failed
// Catch2 assertion throws past the end of the scope, so no unreadable artifact
// can survive the test.
//
// Why a DACL and not a share-mode lock: a lock that blocks readers also blocks
// MoveFileExW's replace (measured on this machine: every dwShareMode that denies
// FILE_SHARE_READ makes the replace fail with ERROR_ACCESS_DENIED), so the
// destructive step could never run and the case would pass with or without the
// fix. A deny-read ACE leaves DELETE alone -- it is granted by the parent
// directory's FILE_DELETE_CHILD, not by the file's own DACL -- so the replace
// still succeeds and the fail-open is reachable.
//
// Only FILE_READ_DATA is denied, so FILE_READ_ATTRIBUTES still works: the case
// then exercises the `ifstream`-open leg specifically rather than short-
// circuiting at the earlier existence/size probe.
class DenyReadData {
public:
    explicit DenyReadData(const std::filesystem::path& p) : path_(p.wstring()) {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return;
        DWORD need = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &need);
        std::vector<BYTE> buf(need);
        const BOOL got = GetTokenInformation(token, TokenUser, buf.data(), need, &need);
        CloseHandle(token);
        if (!got) return;

        if (GetNamedSecurityInfoW(path_.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                  nullptr, nullptr, &old_dacl_, nullptr, &sd_) != ERROR_SUCCESS)
            return;

        EXPLICIT_ACCESS_W ea{};
        ea.grfAccessPermissions = FILE_READ_DATA;
        ea.grfAccessMode        = DENY_ACCESS;
        ea.grfInheritance       = NO_INHERITANCE;
        ea.Trustee.TrusteeForm  = TRUSTEE_IS_SID;
        ea.Trustee.TrusteeType  = TRUSTEE_IS_USER;
        ea.Trustee.ptstrName    = (LPWCH)reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid;
        if (SetEntriesInAclW(1, &ea, old_dacl_, &new_dacl_) != ERROR_SUCCESS) return;

        applied_ = SetNamedSecurityInfoW(&path_[0], SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                         nullptr, nullptr, new_dacl_, nullptr) == ERROR_SUCCESS;
    }
    ~DenyReadData() {
        if (applied_)
            SetNamedSecurityInfoW(&path_[0], SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                  nullptr, nullptr, old_dacl_, nullptr);
        if (new_dacl_) LocalFree(new_dacl_);
        if (sd_) LocalFree(sd_);
    }
    DenyReadData(const DenyReadData&) = delete;
    DenyReadData& operator=(const DenyReadData&) = delete;

    bool applied() const { return applied_; }

private:
    std::wstring path_;
    PSECURITY_DESCRIPTOR sd_ = nullptr;
    PACL old_dacl_ = nullptr;
    PACL new_dacl_ = nullptr;
    bool applied_ = false;
};

TEST_CASE("SessionStore backup aborts the save when the existing file cannot be read",
          "[core][session][store][migration]") {
    auto file = temp_session();
    const std::string v1 =
        "{\"version\":1,\"window\":{\"flags\":0,\"show\":1,"
        "\"x\":0,\"y\":0,\"w\":800,\"h\":600},\"active\":0,\"tabs\":[]}";
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(v1.data(), (std::streamsize)v1.size());
    }

    SessionState s;
    bool saved = true;
    {
        DenyReadData deny(file);
        REQUIRE(deny.applied());
        saved = save_session(file, s);
    }   // DACL restored here, so the read-back below can see the file again

    // Read the bytes back before asserting anything, so a regression reports the
    // data loss itself and not just the return value.
    std::string on_disk;
    {
        std::ifstream in(file, std::ios::binary);
        std::ostringstream ss; ss << in.rdbuf();
        on_disk = ss.str();
    }
    REQUIRE(on_disk == v1);     // the only v1 copy survived
    REQUIRE_FALSE(saved);       // and the save reported that it could not proceed

    auto bak = file;
    bak.replace_extension(L".v1.bak");
    std::error_code ec;
    std::filesystem::remove(file, ec);
    std::filesystem::remove_all(bak, ec);
}

// The misfire direction of the guard above: "I read it and it is worthless" must
// stay distinct from "I could not read it", or a corrupt predecessor blocks
// saving forever. None of these legs may abort the save or write a backup.
TEST_CASE("SessionStore backup proceeds when the existing file holds nothing restorable",
          "[core][session][store][migration]") {
    SessionState s;
    auto backup_of = [](std::filesystem::path p) {
        p.replace_extension(L".v1.bak");
        return p;
    };

    SECTION("bytes that are not JSON at all") {
        auto file = temp_session();
        { std::ofstream out(file, std::ios::binary | std::ios::trunc); out << "not json at all"; }
        REQUIRE(save_session(file, s));
        REQUIRE_FALSE(std::filesystem::exists(backup_of(file)));
        std::error_code ec; std::filesystem::remove(file, ec);
    }
    SECTION("a zero-byte file") {
        // Also pins the short-read check: `ss << in.rdbuf()` sets failbit when it
        // inserts nothing, which is exactly what an empty file does -- that alone
        // must not read as an I/O failure.
        auto file = temp_session();
        { std::ofstream out(file, std::ios::binary | std::ios::trunc); }
        REQUIRE(save_session(file, s));
        REQUIRE_FALSE(std::filesystem::exists(backup_of(file)));
        std::error_code ec; std::filesystem::remove(file, ec);
    }
    SECTION("a file past the size cap") {
        // v1.2.0's SessionStore.hpp carries the identical cap, so this file was
        // already unloadable on the downgrade target: no backup, save proceeds.
        auto file = temp_session();
        {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            std::string big(kMaxSessionBytes + 1, 'x');
            out.write(big.data(), (std::streamsize)big.size());
        }
        REQUIRE(save_session(file, s));
        REQUIRE_FALSE(std::filesystem::exists(backup_of(file)));
        std::error_code ec; std::filesystem::remove(file, ec);
    }
}

TEST_CASE("SessionStore backup failure aborts the save",
          "[core][session][store][migration]") {
    auto file = temp_session();
    const std::string v1 =
        "{\"version\":1,\"window\":{\"flags\":0,\"show\":1,"
        "\"x\":0,\"y\":0,\"w\":800,\"h\":600},\"active\":0,\"tabs\":[]}";
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(v1.data(), (std::streamsize)v1.size());
    }
    // Occupy the backup path with a DIRECTORY so the copy cannot succeed.
    auto bak = file;
    bak.replace_extension(L".v1.bak");
    std::filesystem::create_directory(bak);

    SessionState s;
    REQUIRE_FALSE(save_session(file, s));

    {
        std::ifstream in(file, std::ios::binary);
        std::ostringstream ss; ss << in.rdbuf();
        REQUIRE(ss.str() == v1);   // untouched
    }

    std::error_code ec;
    std::filesystem::remove(file, ec);
    std::filesystem::remove(bak, ec);
}
