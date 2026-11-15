#pragma once

// #229: what a file looked like when it was read: its size and last-write
// time. A later read of the same name compares stamps to tell whether it would
// get the same bytes. Not proof (a rewrite that keeps both is missed), but
// NTFS keeps the write time in 100 ns steps and every ordinary save moves it.

#include <cstdint>
#include <filesystem>
#include <optional>

namespace litepdf::core {

struct FileStamp {
    std::uint64_t size       = 0;
    std::uint64_t write_time = 0;  // FILETIME, 100 ns since 1601 (UTC)

    friend bool operator==(const FileStamp& a, const FileStamp& b) noexcept {
        return a.size == b.size && a.write_time == b.write_time;
    }
    friend bool operator!=(const FileStamp& a, const FileStamp& b) noexcept {
        return !(a == b);
    }
};

// The stamp of `path` now, or nothing when its attributes cannot be read (gone,
// a path that does not resolve) or it is a directory. Needs no read access to
// the file itself.
[[nodiscard]] std::optional<FileStamp> file_stamp(const std::filesystem::path& path) noexcept;

}  // namespace litepdf::core
