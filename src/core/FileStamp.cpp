#include "core/FileStamp.hpp"

#include <windows.h>

namespace litepdf::core {

std::optional<FileStamp> file_stamp(const std::filesystem::path& path) noexcept {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        return std::nullopt;
    }
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return std::nullopt;
    FileStamp s;
    s.size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    s.write_time = (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                   data.ftLastWriteTime.dwLowDateTime;
    return s;
}

}  // namespace litepdf::core
