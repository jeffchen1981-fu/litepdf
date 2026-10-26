#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// #176: reads the first bytes of every page entry of a CBZ ahead, at a queue
// depth above 1, so the per-page header reads that follow (CbzPageSizer) hit
// the file cache. Sizing a page reads three 4 KB blocks one at a time, so 300
// pages read cold took about 120-150 ms, against 100 ms for the strip (spec
// docs/superpowers/specs/2026-10-25-cbz-header-sizing-176-design.md, 3.8).
//
// A cache hint only: nothing here decides a page size, and any failure means
// the read-ahead is skipped. Win32 and the standard library, no MuPDF.

namespace litepdf::core::detail {

// The central directory an end-of-central-directory record points at.
struct ZipDirectory {
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    std::size_t count = 0;
};

// One central-directory record: the entry's raw name and where its local
// header starts.
struct ZipEntryRef {
    std::string name;
    std::uint64_t local_header_offset = 0;
};

// Bytes of the file to read ahead.
struct FileRange {
    std::uint64_t offset = 0;
    std::size_t length = 0;
};

// The directory the file's end-of-central-directory record describes, from
// `tail`, the file's last `n` bytes, which start at `tail_offset` (so the file
// is tail_offset + n bytes). Takes the last signature in the tail, as MuPDF
// does, and refuses Zip64, an empty or implausible directory, and one that
// does not end before the record. Reads only tail[0, n).
std::optional<ZipDirectory> find_zip_directory(const unsigned char* tail, std::size_t n,
                                               std::uint64_t tail_offset);

// The records of `dir`, in directory order, from `bytes`, which must be the
// directory's exact `dir.size` bytes. nullopt for a record that is malformed,
// runs past `n`, or puts a local header at or past the directory. Reads only
// bytes[0, n).
std::optional<std::vector<ZipEntryRef>> parse_zip_directory(const unsigned char* bytes,
                                                            std::size_t n, const ZipDirectory& dir);

// Bytes read ahead per entry: the budget spread over the entries, between the
// floor and the ceiling. The ceiling holds the 30-byte local header, a short
// name and extra field, and the first 64 KB of data (CbzPageSizer's second
// prefix step); the floor holds the header and its first 8 KB step.
inline constexpr std::size_t kReadAheadBudget = 32u * 1024 * 1024;
inline constexpr std::size_t kReadAheadMinSpan = 12u * 1024;
inline constexpr std::size_t kReadAheadMaxSpan = 68u * 1024;

// The ranges to read ahead for local headers at `offsets` in a file of
// `file_size` bytes: each starts at its offset, spans the per-entry share of
// kReadAheadBudget, and is clipped to the file; one at or past the end is
// dropped. Empty for no offsets.
std::vector<FileRange> plan_read_ahead(const std::vector<std::uint64_t>& offsets,
                                       std::uint64_t file_size);

// The central directory of the zip at `path`, read with ReadFile on a handle
// of its own. nullopt for anything find_zip_directory or parse_zip_directory
// refuses, or any I/O or allocation failure.
std::optional<std::vector<ZipEntryRef>> read_zip_directory(const std::wstring& path) noexcept;

// Hands the memory manager `ranges` of the file at `path` in one
// PrefetchVirtualMemory call, through a read-only mapping that is unmapped and
// closed before it returns; the view is never read. True when the call
// succeeded (it may still have prefetched only part). False for no ranges, an
// empty file, or any failure.
bool prefetch_file_ranges(const std::wstring& path, const std::vector<FileRange>& ranges) noexcept;

}  // namespace litepdf::core::detail
