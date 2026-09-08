#include "core/SessionStore.hpp"

#include <windows.h>   // MoveFileExW
#include <fstream>
#include <sstream>
#include <system_error>

namespace litepdf::core {

namespace {
// Three-way answer to "does `file` currently hold a parseable v1 session
// document?". The third state is the point: "I could not read it" is NOT
// "there is nothing worth preserving", because MoveFileExW's replace only
// needs DELETE -- granted by the parent directory's FILE_DELETE_CHILD, which
// a user-owned %LOCALAPPDATA% always gives -- so a file this function cannot
// read is still a file the save can destroy. Collapsing the two into `false`
// would fail OPEN and take the user's last v1 copy with it.
enum class V1Probe {
    No,        // demonstrably nothing to preserve; the save may proceed
    Yes,       // a parseable v1 document; back it up before replacing it
    Unknown,   // it exists but could not be read; the save must NOT proceed
};

V1Probe probe_existing_v1(const std::filesystem::path& file) {
    // `status()`, not `file_size()`, decides absent vs unreadable. `file_size`
    // sets `ec` for both, and mistaking "unreadable" for "absent" is the whole
    // defect; `status()` reports a missing file as `not_found` with `ec`
    // *cleared*, and sets `ec` only for a real query failure (a sharing
    // violation, say), which means the file is there and we cannot see it.
    std::error_code ec;
    const auto st = std::filesystem::status(file, ec);
    if (st.type() == std::filesystem::file_type::not_found)
        return V1Probe::No;                              // absent: first-ever save
    if (ec || !std::filesystem::status_known(st))
        return V1Probe::Unknown;                         // present but unquery-able
    if (!std::filesystem::is_regular_file(st))
        return V1Probe::No;                              // a directory, a device: not a session

    const auto sz = std::filesystem::file_size(file, ec);
    if (ec) return V1Probe::Unknown;                     // exists, but cannot be sized
    if (sz > kMaxSessionBytes) return V1Probe::No;
    // Unchanged on purpose: v1.2.0's SessionStore.hpp carries the identical cap,
    // so a file this large was already unloadable on the downgrade target. There
    // is nothing a backup could restore.

    std::ifstream in(file, std::ios::binary);
    if (!in) return V1Probe::Unknown;                    // DACL denies read, or a share-mode lock

    std::ostringstream ss;
    ss << in.rdbuf();
    if (in.bad() || ss.bad()) return V1Probe::Unknown;
    const std::string bytes = ss.str();
    // `ss << in.rdbuf()` cannot tell "the stream ended" from "the read failed"
    // -- a mid-file I/O error just stops early and looks like a short document,
    // which would then parse as garbage and be discarded as worthless. Compare
    // against the size we already measured instead. (`ss.fail()` is not usable
    // here: the inserter sets failbit for a legitimately empty file too.)
    if (bytes.size() != (std::size_t)sz) return V1Probe::Unknown;

    const auto v = peek_version(bytes);
    if (!v) return V1Probe::No;   // read in full and it is not JSON we understand:
                                  // a corrupt predecessor must not block saving forever
    return *v == 1 ? V1Probe::Yes : V1Probe::No;
}
}  // namespace

bool save_session(const std::filesystem::path& file, const SessionState& s) {
    // One-way door: once a v2 file exists, a rolled-back v1.2.0 binary rejects
    // it and the user loses their whole session. Keep the last v1 copy, and
    // FAIL CLOSED -- a save that destroys the only recoverable copy is exactly
    // the failure this guards against.
    switch (probe_existing_v1(file)) {
    case V1Probe::Unknown:
        return false;   // cannot inspect it => must not replace it
    case V1Probe::Yes: {
        std::filesystem::path bak = file;
        bak.replace_extension(L".v1.bak");
        std::error_code ec;
        std::filesystem::copy_file(
            file, bak, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) return false;
        break;
    }
    case V1Probe::No:
        break;
    }

    std::filesystem::path tmp = file;
    tmp += L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        std::string json = to_json(s);
        out.write(json.data(), (std::streamsize)json.size());
        out.flush();   // surface a disk-full/quota failure NOW, not silently at dtor close
        if (!out) { out.close(); std::error_code ec; std::filesystem::remove(tmp, ec); return false; }
    }
    // Atomic replace. On failure, the original file is untouched; drop the tmp.
    if (!MoveFileExW(tmp.c_str(), file.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ec; std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

std::optional<SessionState> load_session(const std::filesystem::path& file) {
    std::error_code ec;
    auto sz = std::filesystem::file_size(file, ec);
    if (ec) return std::nullopt;                 // missing / unstat-able
    if (sz > kMaxSessionBytes) return std::nullopt;
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return from_json(ss.str());
}

}  // namespace litepdf::core
