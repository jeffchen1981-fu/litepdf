#include "core/SessionStore.hpp"

#include <windows.h>   // MoveFileExW
#include <fstream>
#include <sstream>
#include <system_error>

namespace litepdf::core {

namespace {
// True iff `file` currently holds a parseable v1 session document. Unparseable
// or absent files return false: there is nothing worth preserving, and the save
// must not be blocked by a corrupt predecessor.
bool existing_file_is_v1(const std::filesystem::path& file) {
    std::error_code ec;
    const auto sz = std::filesystem::file_size(file, ec);
    if (ec || sz > kMaxSessionBytes) return false;
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss; ss << in.rdbuf();
    const auto v = peek_version(ss.str());
    return v.has_value() && *v == 1;
}
}  // namespace

bool save_session(const std::filesystem::path& file, const SessionState& s) {
    // One-way door: once a v2 file exists, a rolled-back v1.2.0 binary rejects
    // it and the user loses their whole session. Keep the last v1 copy, and
    // FAIL CLOSED -- a save that destroys the only recoverable copy is exactly
    // the failure this guards against.
    if (existing_file_is_v1(file)) {
        std::filesystem::path bak = file;
        bak.replace_extension(L".v1.bak");
        std::error_code ec;
        std::filesystem::copy_file(
            file, bak, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) return false;
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
