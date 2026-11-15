#include "core/FileStamp.hpp"

#include <windows.h>

namespace litepdf::core {

std::optional<FileStamp> file_stamp(const std::filesystem::path& path) noexcept {
    // A handle, not GetFileAttributesExW on the name: CreateFileW follows a
    // symbolic link (or other name-surrogate reparse point) to the file MuPDF
    // will read, where the name-based call describes the link itself. Opened
    // for attributes only and sharing everything, so it never blocks a writer
    // or a delete. Without FILE_FLAG_BACKUP_SEMANTICS a directory does not
    // open, which is the answer for one.
    HANDLE h = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::nullopt;
    BY_HANDLE_FILE_INFORMATION info{};
    const BOOL ok = GetFileInformationByHandle(h, &info);
    CloseHandle(h);
    if (!ok || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return std::nullopt;

    FileStamp s;
    s.size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    s.write_time = (static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) |
                   info.ftLastWriteTime.dwLowDateTime;
    s.volume = info.dwVolumeSerialNumber;
    s.file_index = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    return s;
}

}  // namespace litepdf::core
