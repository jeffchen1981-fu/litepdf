#pragma once

// #229: which file a name led to, and what it looked like, when it was read:
// the file's identity (volume serial and file index) and its size and
// last-write time. A later read of the same name compares stamps to tell
// whether it would get the same bytes. Not proof: a rewrite in place that keeps
// the size and puts back the same write time, to the 100 ns NTFS keeps, is
// missed. Every ordinary save moves the write time, and another file moved or
// copied into the name's place has another identity.

#include <cstdint>
#include <filesystem>
#include <optional>

namespace litepdf::core {

struct FileStamp {
    std::uint64_t size       = 0;
    std::uint64_t write_time = 0;  // FILETIME, 100 ns since 1601 (UTC)
    std::uint32_t volume     = 0;  // volume serial number
    std::uint64_t file_index = 0;  // the file's index on that volume

    friend bool operator==(const FileStamp& a, const FileStamp& b) noexcept {
        return a.size == b.size && a.write_time == b.write_time && a.volume == b.volume &&
               a.file_index == b.file_index;
    }
    friend bool operator!=(const FileStamp& a, const FileStamp& b) noexcept {
        return !(a == b);
    }
};

// The stamp of the file `path` leads to now, following a symbolic link to its
// target as a read would; nothing when that file cannot be opened for its
// attributes (gone, a path that does not resolve) or is a directory. Needs no
// read access to the file's contents.
[[nodiscard]] std::optional<FileStamp> file_stamp(const std::filesystem::path& path) noexcept;

}  // namespace litepdf::core
