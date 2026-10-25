// #176: the read-ahead of every CBZ page entry's first bytes
// (detail::ZipReadAhead, spec docs/superpowers/specs/
// 2026-10-25-cbz-header-sizing-176-design.md, sections 3.8 and 5.3). The
// parser runs on crafted buffers that end at a no-access page, so a read past
// the buffer faults the test instead of reading allocator slack.
#include "app/SearchDispatcher.hpp"
#include "core/Document.hpp"
#include "core/DocumentView.hpp"
#include "core/detail/ZipReadAhead.hpp"

#include <catch2/catch_test_macros.hpp>

#include <mupdf/fitz.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <windows.h>

using namespace litepdf::core;
using namespace litepdf::core::detail;
namespace fs = std::filesystem;

namespace {

using Bytes = std::vector<unsigned char>;

void put16(Bytes& b, std::size_t at, std::uint16_t v) {
    b[at] = static_cast<unsigned char>(v);
    b[at + 1] = static_cast<unsigned char>(v >> 8);
}

void put32(Bytes& b, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = static_cast<unsigned char>(v >> (8 * i));
}

void append(Bytes& to, const Bytes& from) {
    to.insert(to.end(), from.begin(), from.end());
}

// A 22-byte end-of-central-directory record.
Bytes eocd(std::uint16_t count, std::uint32_t size, std::uint32_t offset,
           std::uint16_t comment_length = 0) {
    Bytes b(22, 0);
    b[0] = 0x50; b[1] = 0x4B; b[2] = 0x05; b[3] = 0x06;
    put16(b, 8, count);
    put16(b, 10, count);
    put32(b, 12, size);
    put32(b, 16, offset);
    put16(b, 20, comment_length);
    return b;
}

// A central-directory record for `name` whose local header is at `offset`,
// with zero-filled extra field and comment of the given lengths. `name_length`
// may claim more than `name` holds.
Bytes record(const std::string& name, std::uint32_t offset, std::uint16_t extra = 0,
             std::uint16_t comment = 0, std::optional<std::uint16_t> name_length = std::nullopt) {
    Bytes b(46, 0);
    b[0] = 0x50; b[1] = 0x4B; b[2] = 0x01; b[3] = 0x02;
    put16(b, 28, name_length.value_or(static_cast<std::uint16_t>(name.size())));
    put16(b, 30, extra);
    put16(b, 32, comment);
    put32(b, 42, offset);
    b.insert(b.end(), name.begin(), name.end());
    b.insert(b.end(), static_cast<std::size_t>(extra) + comment, 0);
    return b;
}

// `data` copied so that its last byte is the last byte before a no-access page.
class GuardedBytes {
public:
    explicit GuardedBytes(const Bytes& data) : size_(data.size()) {
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        const std::size_t page = si.dwPageSize;
        const std::size_t data_pages = (data.size() + page - 1) / page + 1;
        total_ = (data_pages + 1) * page;
        base_ = static_cast<unsigned char*>(
            VirtualAlloc(nullptr, total_, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        REQUIRE(base_ != nullptr);
        DWORD old = 0;
        REQUIRE(VirtualProtect(base_ + data_pages * page, page, PAGE_NOACCESS, &old));
        ptr_ = base_ + data_pages * page - data.size();
        if (!data.empty()) std::memcpy(ptr_, data.data(), data.size());
    }
    ~GuardedBytes() { VirtualFree(base_, 0, MEM_RELEASE); }
    GuardedBytes(const GuardedBytes&) = delete;
    GuardedBytes& operator=(const GuardedBytes&) = delete;

    const unsigned char* data() const { return ptr_; }
    std::size_t size() const { return size_; }

private:
    unsigned char* base_ = nullptr;
    unsigned char* ptr_ = nullptr;
    std::size_t size_ = 0;
    std::size_t total_ = 0;
};

std::optional<ZipDirectory> find(const Bytes& tail, std::uint64_t tail_offset) {
    const GuardedBytes g(tail);
    return find_zip_directory(g.data(), g.size(), tail_offset);
}

std::optional<std::vector<ZipEntryRef>> parse(const Bytes& dir_bytes, ZipDirectory dir) {
    const GuardedBytes g(dir_bytes);
    return parse_zip_directory(g.data(), g.size(), dir);
}

// The archive MuPDF opens on `path`, for its entry list.
struct OwnArchive {
    fz_context* ctx = nullptr;
    fz_archive* arch = nullptr;

    explicit OwnArchive(const char* path) {
        ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
        REQUIRE(ctx != nullptr);
        fz_try(ctx) { arch = fz_open_archive(ctx, path); }
        fz_catch(ctx) { fz_report_error(ctx); arch = nullptr; }
        REQUIRE(arch != nullptr);
    }
    ~OwnArchive() {
        fz_drop_archive(ctx, arch);
        fz_drop_context(ctx);
    }
    OwnArchive(const OwnArchive&) = delete;
    OwnArchive& operator=(const OwnArchive&) = delete;

    std::vector<std::string> names() {
        std::vector<std::string> out;
        const int n = fz_count_archive_entries(ctx, arch);
        for (int i = 0; i < n; ++i) {
            const char* name = fz_list_archive_entry(ctx, arch, i);
            out.emplace_back(name ? name : "");
        }
        return out;
    }
};

Bytes file_bytes(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    REQUIRE(f);
    return Bytes(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

fs::path temp_copy(const char* fixture, const char* name) {
    const fs::path dir = fs::temp_directory_path() / "litepdf-zip-176";
    fs::create_directories(dir);
    const fs::path out = dir / name;
    fs::copy_file(fixture, out, fs::copy_options::overwrite_existing);
    return out;
}

DWORD handle_count() {
    DWORD n = 0;
    REQUIRE(GetProcessHandleCount(GetCurrentProcess(), &n));
    return n;
}

// No handle and no mapped view of `path` is left: an exclusive open succeeds,
// and so does setting the end of file, which fails with ERROR_USER_MAPPED_FILE
// while a view exists even after every handle is closed.
void require_released(const fs::path& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    INFO("exclusive open error " << GetLastError());
    REQUIRE(h != INVALID_HANDLE_VALUE);
    LARGE_INTEGER size{};
    const BOOL sized = GetFileSizeEx(h, &size);
    const BOOL moved = sized && SetFilePointerEx(h, size, nullptr, FILE_BEGIN);
    const BOOL ended = moved && SetEndOfFile(h);
    const DWORD err = GetLastError();
    CloseHandle(h);
    INFO("SetEndOfFile error " << err);
    REQUIRE(ended);
}

ULONGLONG read_operations() {
    IO_COUNTERS io{};
    REQUIRE(GetProcessIoCounters(GetCurrentProcess(), &io));
    return io.ReadOperationCount;
}

}  // namespace

TEST_CASE("ZipReadAhead: the directory matches MuPDF's entry list", "[core][zip]") {
    const char* const fixtures[] = {
        "tests/fixtures/cbz-sizing.cbz", "tests/fixtures/sample.cbz",
        "tests/fixtures/strip-images.cbz", "tests/fixtures/sample.epub",
    };
    for (const char* path : fixtures) {
        INFO(path);
        const auto entries = read_zip_directory(fs::path(path).wstring());
        REQUIRE(entries.has_value());
        OwnArchive own(path);
        const std::vector<std::string> names = own.names();
        REQUIRE(entries->size() == names.size());
        const Bytes file = file_bytes(path);
        for (std::size_t i = 0; i < names.size(); ++i) {
            const ZipEntryRef& e = (*entries)[i];
            INFO("entry " << i << " " << e.name);
            CHECK(e.name == names[i]);
            const std::uint64_t at = e.local_header_offset;
            REQUIRE(at + 30 + e.name.size() <= file.size());
            CHECK(file[at] == 0x50);
            CHECK(file[at + 1] == 0x4B);
            CHECK(file[at + 2] == 0x03);
            CHECK(file[at + 3] == 0x04);
            // The local header names the same entry.
            CHECK(std::memcmp(file.data() + at + 30, e.name.data(), e.name.size()) == 0);
        }
    }
}

TEST_CASE("ZipReadAhead: find_zip_directory refuses what it does not handle", "[core][zip]") {
    const Bytes zeros(200, 0);
    CHECK_FALSE(find({}, 0).has_value());
    CHECK_FALSE(find(Bytes(100, 0), 0).has_value());
    {
        Bytes bare(96, 0);
        append(bare, Bytes{0x50, 0x4B, 0x05, 0x06});   // no room for the record
        CHECK_FALSE(find(bare, 0).has_value());
    }
    const auto with = [&](const Bytes& rec, std::uint64_t tail_offset) {
        Bytes t = zeros;
        append(t, rec);
        return find(t, tail_offset);
    };
    CHECK_FALSE(with(eocd(0xFFFF, 92, 500), 1000).has_value());          // Zip64 count
    CHECK_FALSE(with(eocd(2, 0xFFFFFFFFu, 500), 1000).has_value());      // Zip64 size
    CHECK_FALSE(with(eocd(2, 92, 0xFFFFFFFFu), 1000).has_value());       // Zip64 offset
    CHECK_FALSE(with(eocd(0, 0, 500), 1000).has_value());                // no entries
    CHECK_FALSE(with(eocd(2, 91, 500), 1000).has_value());               // < 46 per entry
    CHECK_FALSE(with(eocd(2, 64u * 1024 * 1024 + 1, 10), 1000000000).has_value());  // > 64 MiB
    // The record sits at 1000 + 200 = 1200; a directory reaching 1242 overlaps it.
    CHECK_FALSE(with(eocd(2, 92, 1150), 1000).has_value());
}

TEST_CASE("ZipReadAhead: find_zip_directory takes the last record", "[core][zip]") {
    {
        // Bytes after the record are allowed, as MuPDF allows them.
        Bytes t(200, 0);
        append(t, eocd(2, 92, 500));
        append(t, Bytes(10, 0xAA));
        const auto dir = find(t, 1000);
        REQUIRE(dir.has_value());
        CHECK(dir->offset == 500);
        CHECK(dir->size == 92);
        CHECK(dir->count == 2);
    }
    {
        Bytes t(100, 0);
        append(t, eocd(1, 46, 300));
        append(t, Bytes(50, 0));
        append(t, eocd(2, 92, 500, 3));
        append(t, Bytes(3, 0x20));
        const auto dir = find(t, 1000);
        REQUIRE(dir.has_value());
        CHECK(dir->offset == 500);
        CHECK(dir->count == 2);
    }
}

TEST_CASE("ZipReadAhead: parse_zip_directory reads only well-formed records", "[core][zip]") {
    Bytes two = record("a.jpg", 0);
    append(two, record("b.png", 100, 4, 2));
    const ZipDirectory dir{1000, two.size(), 2};
    {
        const auto entries = parse(two, dir);
        REQUIRE(entries.has_value());
        REQUIRE(entries->size() == 2);
        CHECK((*entries)[0].name == "a.jpg");
        CHECK((*entries)[0].local_header_offset == 0);
        CHECK((*entries)[1].name == "b.png");
        CHECK((*entries)[1].local_header_offset == 100);
    }
    CHECK_FALSE(parse(two, ZipDirectory{1000, two.size() + 1, 2}).has_value());   // n != size
    {
        Bytes bad = two;
        bad[46 + 5] = 0x51;   // the second record's signature
        CHECK_FALSE(parse(bad, dir).has_value());
    }
    const auto one = [](const Bytes& rec) { return ZipDirectory{1000, rec.size(), 1}; };
    {
        // A name, extra field or comment that claims more than the directory holds.
        Bytes name_past = record("a.jpg", 0, 0, 0, std::uint16_t{9});
        CHECK_FALSE(parse(name_past, one(name_past)).has_value());
        Bytes extra_past = record("a.jpg", 0, 8);
        extra_past.resize(extra_past.size() - 1);
        CHECK_FALSE(parse(extra_past, one(extra_past)).has_value());
        Bytes comment_past = record("a.jpg", 0, 0, 8);
        comment_past.resize(comment_past.size() - 1);
        CHECK_FALSE(parse(comment_past, one(comment_past)).has_value());
        Bytes short_header(40, 0);
        CHECK_FALSE(parse(short_header, one(short_header)).has_value());
    }
    {
        Bytes at_dir = record("a.jpg", 1000);
        CHECK_FALSE(parse(at_dir, one(at_dir)).has_value());
        Bytes past_dir = record("a.jpg", 1001);
        CHECK_FALSE(parse(past_dir, one(past_dir)).has_value());
    }
}

TEST_CASE("ZipReadAhead: plan_read_ahead spreads the budget", "[core][zip]") {
    const std::uint64_t big = 1ull << 40;
    CHECK(plan_read_ahead({}, big).empty());

    const auto spans = [&](std::size_t k) {
        std::vector<std::uint64_t> offsets(k);
        for (std::size_t i = 0; i < k; ++i) offsets[i] = i * 100000ull;
        return plan_read_ahead(offsets, big);
    };
    const auto check_all = [](const std::vector<FileRange>& r, std::size_t k, std::size_t span) {
        REQUIRE(r.size() == k);
        for (const FileRange& x : r) CHECK(x.length == span);
    };
    check_all(spans(1), 1, kReadAheadMaxSpan);
    check_all(spans(481), 481, kReadAheadMaxSpan);
    check_all(spans(482), 482, kReadAheadBudget / 482);
    check_all(spans(2730), 2730, kReadAheadBudget / 2730);
    check_all(spans(2731), 2731, kReadAheadMinSpan);

    const auto clipped = plan_read_ahead({0, 1000, 1900, 2000, 5000}, 2000);
    REQUIRE(clipped.size() == 3);   // 2000 and 5000 start at or past the end
    CHECK(clipped[0].length == 2000);
    CHECK(clipped[1].offset == 1000);
    CHECK(clipped[1].length == 1000);
    CHECK(clipped[2].offset == 1900);
    CHECK(clipped[2].length == 100);
}

TEST_CASE("ZipReadAhead: prefetch_file_ranges releases the file", "[core][zip]") {
    const fs::path copy = temp_copy("tests/fixtures/cbz-sizing.cbz", "prefetch.cbz");
    const auto entries = read_zip_directory(copy.wstring());
    REQUIRE(entries.has_value());
    std::vector<std::uint64_t> offsets;
    for (const ZipEntryRef& e : *entries) offsets.push_back(e.local_header_offset);
    const auto ranges = plan_read_ahead(offsets, fs::file_size(copy));
    REQUIRE_FALSE(ranges.empty());

    {
        const DWORD before = handle_count();
        CHECK(prefetch_file_ranges(copy.wstring(), ranges));
        CHECK(handle_count() == before);
        require_released(copy);
    }
    {
        const DWORD before = handle_count();
        CHECK_FALSE(prefetch_file_ranges(copy.wstring(), {}));
        CHECK(handle_count() == before);
        require_released(copy);
    }
    {
        const fs::path empty = fs::temp_directory_path() / "litepdf-zip-176" / "empty.cbz";
        { std::ofstream f(empty, std::ios::binary | std::ios::trunc); }
        const DWORD before = handle_count();
        CHECK_FALSE(prefetch_file_ranges(empty.wstring(), {FileRange{0, 10}}));
        CHECK(handle_count() == before);
        require_released(empty);
    }
    CHECK_FALSE(prefetch_file_ranges(
        (fs::temp_directory_path() / "litepdf-zip-176" / "missing.cbz").wstring(), ranges));
    CHECK_FALSE(read_zip_directory(
        (fs::temp_directory_path() / "litepdf-zip-176" / "missing.cbz").wstring()).has_value());
}

TEST_CASE("ZipReadAhead: Document reads ahead once, and page_sizes asks for it", "[core][zip]") {
    const fs::path copy = temp_copy("tests/fixtures/cbz-sizing.cbz", "document.cbz");
    {
        Document doc;
        REQUIRE_FALSE(doc.open(copy).has_value());
        (void)doc.page_size(0);   // the sizer exists; no read-ahead yet
        const ULONGLONG before = read_operations();
        CHECK(doc.read_ahead_page_sizes());
        CHECK(read_operations() - before >= 2);   // the EOCD tail and the directory
        const ULONGLONG again = read_operations();
        CHECK_FALSE(doc.read_ahead_page_sizes());
        CHECK(read_operations() == again);
    }
    {
        litepdf::app::InlineDispatcher disp;
        Document doc;
        REQUIRE_FALSE(doc.open(copy).has_value());
        DocumentView view(std::move(doc), disp);
        (void)view.page_sizes();
        CHECK_FALSE(view.document().read_ahead_page_sizes());   // page_sizes already did
    }
    {
        // Not a CBZ: nothing to read ahead.
        Document doc;
        REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());
        CHECK_FALSE(doc.read_ahead_page_sizes());
    }
}
