#include "core/SessionStore.hpp"

#include <windows.h>   // MoveFileExW
#include <fstream>
#include <sstream>
#include <system_error>

namespace litepdf::core {

bool save_session(const std::filesystem::path& file, const SessionState& s) {
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
