#include "core/detail/ZipReadAhead.hpp"

#include <algorithm>
#include <exception>
#include <utility>

#include <windows.h>

// Spec: docs/superpowers/specs/2026-10-25-cbz-header-sizing-176-design.md, 3.8.

namespace litepdf::core::detail {

namespace {

constexpr std::size_t kEocdSize = 22;
constexpr std::size_t kMaxCommentSize = 0xFFFF;
constexpr std::size_t kCentralRecordSize = 46;
constexpr std::uint64_t kMaxDirectorySize = 64ull * 1024 * 1024;

std::uint16_t le16(const unsigned char* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t le32(const unsigned char* p) {
    return std::uint32_t{p[0]} | (std::uint32_t{p[1]} << 8) | (std::uint32_t{p[2]} << 16) |
           (std::uint32_t{p[3]} << 24);
}

bool is_signature(const unsigned char* p, unsigned char a, unsigned char b) {
    return p[0] == 0x50 && p[1] == 0x4B && p[2] == a && p[3] == b;
}

// Closes a Win32 handle when it goes.
class Handle {
public:
    explicit Handle(HANDLE h) noexcept : h_(h) {}
    ~Handle() {
        if (valid()) CloseHandle(h_);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    bool valid() const noexcept { return h_ != nullptr && h_ != INVALID_HANDLE_VALUE; }
    HANDLE get() const noexcept { return h_; }

private:
    HANDLE h_;
};

// Unmaps a view when it goes.
class View {
public:
    explicit View(void* p) noexcept : p_(p) {}
    ~View() {
        if (p_) UnmapViewOfFile(p_);
    }
    View(const View&) = delete;
    View& operator=(const View&) = delete;
    unsigned char* get() const noexcept { return static_cast<unsigned char*>(p_); }

private:
    void* p_;
};

// A read-only handle that lets every other opener keep reading, writing or
// deleting the file, MuPDF's own handle included.
HANDLE open_shared(const std::wstring& path) {
    return CreateFileW(path.c_str(), GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

// Reads exactly `n` bytes at `offset` into `out`.
bool read_at(HANDLE h, std::uint64_t offset, std::size_t n, std::vector<unsigned char>& out) {
    out.resize(n);
    LARGE_INTEGER at{};
    at.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(h, at, nullptr, FILE_BEGIN)) return false;
    std::size_t done = 0;
    while (done < n) {
        const DWORD want = static_cast<DWORD>((std::min<std::size_t>)(n - done, 1u << 30));
        DWORD got = 0;
        if (!ReadFile(h, out.data() + done, want, &got, nullptr) || got == 0) return false;
        done += got;
    }
    return true;
}

}  // namespace

std::optional<ZipDirectory> find_zip_directory(const unsigned char* tail, std::size_t n,
                                               std::uint64_t tail_offset) {
    if (n < kEocdSize) return std::nullopt;
    // The last signature that leaves room for the record, as MuPDF's
    // ensure_zip_entries takes the last one it finds (unzip.c).
    for (std::size_t p = n - kEocdSize + 1; p-- > 0;) {
        const unsigned char* r = tail + p;
        if (!is_signature(r, 0x05, 0x06)) continue;
        const std::uint16_t count = le16(r + 10);
        const std::uint32_t size = le32(r + 12);
        const std::uint32_t offset = le32(r + 16);
        if (count == 0xFFFF || size == 0xFFFFFFFFu || offset == 0xFFFFFFFFu) return std::nullopt;
        if (count == 0) return std::nullopt;
        if (size < std::uint64_t{count} * kCentralRecordSize || size > kMaxDirectorySize)
            return std::nullopt;
        if (std::uint64_t{offset} + size > tail_offset + p) return std::nullopt;
        return ZipDirectory{offset, size, count};
    }
    return std::nullopt;
}

std::optional<std::vector<ZipEntryRef>> parse_zip_directory(const unsigned char* bytes,
                                                            std::size_t n, const ZipDirectory& dir) {
    if (n != dir.size) return std::nullopt;
    std::vector<ZipEntryRef> out;
    out.reserve(dir.count);
    std::size_t pos = 0;
    for (std::size_t i = 0; i < dir.count; ++i) {
        if (n - pos < kCentralRecordSize) return std::nullopt;
        const unsigned char* r = bytes + pos;
        if (!is_signature(r, 0x01, 0x02)) return std::nullopt;
        const std::size_t name_length = le16(r + 28);
        const std::size_t extra_length = le16(r + 30);
        const std::size_t comment_length = le16(r + 32);
        const std::uint32_t local = le32(r + 42);
        const std::size_t record = kCentralRecordSize + name_length + extra_length + comment_length;
        if (n - pos < record) return std::nullopt;
        if (local >= dir.offset) return std::nullopt;
        out.push_back(ZipEntryRef{
            std::string(reinterpret_cast<const char*>(r + kCentralRecordSize), name_length), local});
        pos += record;
    }
    return out;
}

std::vector<FileRange> plan_read_ahead(const std::vector<std::uint64_t>& offsets,
                                       std::uint64_t file_size) {
    std::vector<FileRange> out;
    if (offsets.empty()) return out;
    const std::size_t span = std::clamp(kReadAheadBudget / offsets.size(), kReadAheadMinSpan,
                                        kReadAheadMaxSpan);
    out.reserve(offsets.size());
    for (const std::uint64_t at : offsets) {
        if (at >= file_size) continue;
        const std::uint64_t left = file_size - at;
        out.push_back(FileRange{at, left < span ? static_cast<std::size_t>(left) : span});
    }
    return out;
}

std::optional<std::vector<ZipEntryRef>> read_zip_directory(const std::wstring& path) noexcept {
    try {
        const Handle file(open_shared(path));
        if (!file.valid()) return std::nullopt;
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file.get(), &size) || size.QuadPart <= 0) return std::nullopt;
        const auto file_size = static_cast<std::uint64_t>(size.QuadPart);

        const std::size_t tail_n = static_cast<std::size_t>(
            (std::min<std::uint64_t>)(file_size, kEocdSize + kMaxCommentSize));
        const std::uint64_t tail_offset = file_size - tail_n;
        std::vector<unsigned char> tail;
        if (!read_at(file.get(), tail_offset, tail_n, tail)) return std::nullopt;
        const auto dir = find_zip_directory(tail.data(), tail.size(), tail_offset);
        if (!dir) return std::nullopt;

        std::vector<unsigned char> bytes;
        if (!read_at(file.get(), dir->offset, static_cast<std::size_t>(dir->size), bytes))
            return std::nullopt;
        return parse_zip_directory(bytes.data(), bytes.size(), *dir);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

bool prefetch_file_ranges(const std::wstring& path, const std::vector<FileRange>& ranges) noexcept {
    try {
        // PrefetchVirtualMemory fails with ERROR_INVALID_PARAMETER for no entries.
        if (ranges.empty()) return false;
        const Handle file(open_shared(path));
        if (!file.valid()) return false;
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file.get(), &size) || size.QuadPart <= 0) return false;
        const auto file_size = static_cast<std::uint64_t>(size.QuadPart);

        const Handle mapping(CreateFileMappingW(file.get(), nullptr, PAGE_READONLY, 0, 0, nullptr));
        if (!mapping.valid()) return false;
        const View view(MapViewOfFile(mapping.get(), FILE_MAP_READ, 0, 0, 0));
        if (!view.get()) return false;

        // Addresses in the view; the view itself is never read.
        std::vector<WIN32_MEMORY_RANGE_ENTRY> entries;
        entries.reserve(ranges.size());
        for (const FileRange& r : ranges) {
            if (r.length == 0 || r.offset >= file_size || r.length > file_size - r.offset) continue;
            entries.push_back(WIN32_MEMORY_RANGE_ENTRY{view.get() + r.offset, r.length});
        }
        if (entries.empty()) return false;
        return PrefetchVirtualMemory(GetCurrentProcess(), entries.size(), entries.data(), 0) != 0;
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace litepdf::core::detail
