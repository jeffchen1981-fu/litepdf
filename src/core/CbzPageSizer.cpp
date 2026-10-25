#include "core/detail/CbzPageSizer.hpp"
#include "core/detail/ZipReadAhead.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <new>
#include <system_error>
#include <utility>

#include <windows.h>

// Spec: docs/superpowers/specs/2026-10-25-cbz-header-sizing-176-design.md.
//
// MuPDF error handling (#146): every fz_catch here is an expected-failure
// probe, so it takes the error with fz_convert_error and discards it, and no
// error is reported per page. MuPDF's own warnings still are (a BMP colour
// profile past the prefix, libjpeg's notes on corrupt data). No C++ code that
// can throw runs inside a fz_try.

namespace litepdf::core::detail {

namespace {

// cbz_ext_list, mucbz.c: the entries MuPDF's CBZ handler takes as pages.
const char* const kPageExtensions[] = {
    ".bmp", ".gif", ".hdp", ".j2k", ".jb2", ".jbig2", ".jp2", ".jpeg", ".jpg",
    ".jpx", ".jxr", ".pam", ".pbm", ".pgm", ".pkm", ".png", ".pnm", ".ppm",
    ".tif", ".tiff", ".wdp", nullptr,
};

// cbz_strnatcmp and its helpers, mucbz.c, unchanged: the page order must be
// MuPDF's, ties included, so the same comparator goes to the same qsort.
inline int cbz_isdigit(int c) {
    return c >= '0' && c <= '9';
}

inline int cbz_toupper(int c) {
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 'A';
    return c;
}

int cbz_strnatcmp(const char* a, const char* b) {
    int x, y;

    while (*a || *b) {
        if (cbz_isdigit(*a) && cbz_isdigit(*b)) {
            x = *a++ - '0';
            while (cbz_isdigit(*a))
                x = x * 10 + *a++ - '0';
            y = *b++ - '0';
            while (cbz_isdigit(*b))
                y = y * 10 + *b++ - '0';
        } else {
            x = cbz_toupper(*a++);
            y = cbz_toupper(*b++);
        }
        if (x < y)
            return -1;
        if (x > y)
            return 1;
    }

    return 0;
}

int compare_page_names(const void* a, const void* b) {
    return cbz_strnatcmp(*static_cast<const char* const*>(a),
                         *static_cast<const char* const*>(b));
}

bool is_page_name(const char* name) {
    const char* ext = name ? std::strrchr(name, '.') : nullptr;
    if (!ext) return false;
    for (int k = 0; kPageExtensions[k]; ++k) {
        if (!fz_strcasecmp(ext, kPageExtensions[k])) return true;
    }
    return false;
}

// The prefix grows through these sizes; most JPEG and PNG headers fit the
// first. Past the last one the page is loaded instead.
constexpr std::size_t kPrefixSteps[] = {8 * 1024, 64 * 1024, 512 * 1024};

// An empty IEND chunk: length 0, type, CRC.
constexpr unsigned char kPngIend[] = {0, 0, 0, 0, 'I', 'E', 'N', 'D', 0xAE, 0x42, 0x60, 0x82};

// cbz_bound_page's DPI, mucbz.c.
constexpr float kCbzDpi = 72.0f;

// Reads `stm` into `buf` until `buf` holds `limit` bytes. True when the entry
// ended first.
bool read_up_to(fz_context* ctx, fz_stream* stm, fz_buffer* buf, std::size_t limit) {
    if (buf->cap < limit) fz_resize_buffer(ctx, buf, limit);
    while (buf->len < limit) {
        const std::size_t n = fz_read(ctx, stm, buf->data + buf->len, limit - buf->len);
        if (n == 0) return true;
        buf->len += n;
    }
    return false;
}

// MuPDF's own header reading on `buf`, or nullptr when it cannot read a
// header from these bytes.
fz_image* try_image(fz_context* ctx, fz_buffer* buf) {
    fz_image* image = nullptr;
    fz_try(ctx) {
        image = fz_new_image_from_buffer(ctx, buf);
    }
    fz_catch(ctx) {
        fz_convert_error(ctx, nullptr);
        image = nullptr;
    }
    return image;
}

// Where the PNG chunk walk over the prefix stopped.
struct PngScan {
    bool idat = false;          // the first IDAT starts at `idat_at`
    bool iend = false;          // IEND came before any IDAT
    std::size_t idat_at = 0;
};

// Walks the chunk headers of the PNG in `buf` from after the signature. A
// chunk header that does not fit in `buf` leaves both flags false.
PngScan scan_png(const fz_buffer* buf) {
    PngScan s;
    std::size_t pos = 8;
    while (pos + 8 <= buf->len) {
        const unsigned char* p = buf->data + pos;
        const std::uint32_t length = (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) |
                                     (std::uint32_t{p[2]} << 8) | std::uint32_t{p[3]};
        if (!std::memcmp(p + 4, "IDAT", 4)) {
            s.idat = true;
            s.idat_at = pos;
            return s;
        }
        if (!std::memcmp(p + 4, "IEND", 4)) {
            s.iend = true;
            return s;
        }
        pos += 12 + static_cast<std::size_t>(length);
    }
    return s;
}

// cbz_bound_page, mucbz.c, with the same types, so the size is the same bits.
PageSize image_page_size(fz_context* ctx, fz_image* image) {
    int xres, yres;
    fz_image_resolution(image, &xres, &yres);
    const std::uint8_t orientation = fz_image_orientation(ctx, image);
    PageSize s;
    if (orientation == 0 || (orientation & 1) == 1) {
        s.width_pt = image->w * kCbzDpi / xres;
        s.height_pt = image->h * kCbzDpi / yres;
    } else {
        s.height_pt = image->w * kCbzDpi / xres;
        s.width_pt = image->h * kCbzDpi / yres;
    }
    return s;
}

}  // namespace

std::unique_ptr<CbzPageSizer> CbzPageSizer::create(fz_context* ctx, fz_document* doc,
                                                   const char* utf8_path,
                                                   std::size_t page_count) noexcept {
    if (!ctx || !doc || !utf8_path) return nullptr;

    fz_archive* arch = nullptr;
    const char** names = nullptr;   // point into `arch`
    int count = 0;
    bool same_format = false;
    char doc_format[32] = "";
    fz_var(arch);
    fz_var(names);
    fz_var(count);
    fz_var(same_format);

    fz_try(ctx) {
        // The same path the Document opened, so the same archive type and
        // entry order as the CBZ handler's own archive (cbz_open_document).
        arch = fz_open_archive(ctx, utf8_path);
        // cbz_lookup_metadata answers the archive's format; another handler
        // that took this file answers its own ("EPUB", "XPS").
        if (fz_lookup_metadata(ctx, doc, FZ_META_FORMAT, doc_format, sizeof doc_format) > 0 &&
            !std::strcmp(doc_format, fz_archive_format(ctx, arch))) {
            // cbz_create_page_list, mucbz.c.
            const int n = fz_count_archive_entries(ctx, arch);
            names = fz_malloc_array(ctx, n, const char*);
            for (int i = 0; i < n; ++i) {
                const char* name = fz_list_archive_entry(ctx, arch, i);
                if (is_page_name(name)) names[count++] = name;
            }
            std::qsort(names, static_cast<std::size_t>(count), sizeof *names, compare_page_names);
            same_format = true;
        }
    }
    fz_catch(ctx) {
        fz_convert_error(ctx, nullptr);
        fz_free(ctx, names);
        fz_drop_archive(ctx, arch);
        return nullptr;
    }

    std::unique_ptr<CbzPageSizer> sizer;
    if (same_format && static_cast<std::size_t>(count) == page_count) {
        try {
            std::vector<std::string> list(names, names + count);
            sizer.reset(new CbzPageSizer(ctx, arch, std::move(list), utf8_path));
            arch = nullptr;   // the sizer owns it now
        } catch (const std::bad_alloc&) {
            sizer.reset();
        }
    }
    fz_free(ctx, names);
    if (arch) fz_drop_archive(ctx, arch);
    return sizer;
}

CbzPageSizer::CbzPageSizer(fz_context* ctx, fz_archive* arch, std::vector<std::string> names,
                           std::string utf8_path) noexcept
    : ctx_(ctx), arch_(arch), names_(std::move(names)), utf8_path_(std::move(utf8_path)) {}

CbzPageSizer::~CbzPageSizer() {
    fz_drop_archive(ctx_, arch_);
}

bool CbzPageSizer::read_ahead() noexcept {
    if (read_ahead_done_) return false;
    read_ahead_done_ = true;
    try {
        const int wide = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_path_.data(),
                                             static_cast<int>(utf8_path_.size()), nullptr, 0);
        if (wide <= 0) return true;
        std::wstring path(static_cast<std::size_t>(wide), L'\0');
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_path_.data(),
                                static_cast<int>(utf8_path_.size()), path.data(), wide) != wide)
            return true;

        // The directory's raw names: for an entry MuPDF renames (no UTF-8 flag,
        // or an Info-ZIP Unicode Path field) the kept set can differ from the
        // page list, which only changes what is cached.
        const auto entries = read_zip_directory(path);
        if (!entries) return true;
        std::vector<std::uint64_t> offsets;
        for (const ZipEntryRef& e : *entries) {
            if (is_page_name(e.name.c_str())) offsets.push_back(e.local_header_offset);
        }
        if (offsets.empty()) return true;
        std::error_code ec;
        const std::uintmax_t size = std::filesystem::file_size(path, ec);
        if (ec) return true;
        (void)prefetch_file_ranges(path, plan_read_ahead(offsets, size));
    } catch (const std::exception&) {
        // Skipped: a cache hint only.
    }
    return true;
}

std::optional<PageSize> CbzPageSizer::header_size(std::size_t index) noexcept {
    if (index >= names_.size()) return std::nullopt;
    fz_context* const ctx = ctx_;
    const char* const name = names_[index].c_str();

    fz_stream* stm = nullptr;
    fz_buffer* buf = nullptr;
    fz_buffer* cut = nullptr;
    fz_image* image = nullptr;
    fz_var(stm);
    fz_var(buf);
    fz_var(cut);
    fz_var(image);

    fz_try(ctx) {
        stm = fz_open_archive_entry(ctx, arch_, name);
        buf = fz_new_buffer(ctx, kPrefixSteps[0]);
        for (const std::size_t step : kPrefixSteps) {
            const bool ended = read_up_to(ctx, stm, buf, step);
            if (buf->len < 8) break;
            bool done = false;
            switch (fz_recognize_image_format(ctx, buf->data)) {
            case FZ_IMAGE_JPEG:
            case FZ_IMAGE_GIF:
            case FZ_IMAGE_BMP:
                // These info readers stop at the end of the header; JPEG's
                // fails unless the prefix holds the real SOS marker.
                image = try_image(ctx, buf);
                done = image != nullptr || ended;
                break;
            case FZ_IMAGE_PNG: {
                // MuPDF's PNG info reader walks every chunk to IEND, so hand
                // it the chunks before the first IDAT and an IEND.
                const PngScan scan = scan_png(buf);
                if (scan.idat) {
                    cut = fz_new_buffer_from_copied_data(ctx, buf->data, scan.idat_at);
                    fz_append_data(ctx, cut, kPngIend, sizeof kPngIend);
                    image = try_image(ctx, cut);
                    done = true;
                } else {
                    done = scan.iend || ended;
                }
                break;
            }
            default:
                // TIFF (its IFD may sit at the end), JPX, JXR, PNM, PSD, JBIG2.
                done = true;
                break;
            }
            if (done) break;
        }
    }
    fz_always(ctx) {
        fz_drop_buffer(ctx, cut);
        fz_drop_buffer(ctx, buf);
        fz_drop_stream(ctx, stm);
    }
    fz_catch(ctx) {
        fz_convert_error(ctx, nullptr);
        fz_drop_image(ctx, image);
        return std::nullopt;
    }

    if (!image) return std::nullopt;
    const PageSize size = image_page_size(ctx, image);
    fz_drop_image(ctx, image);
    return size;
}

}  // namespace litepdf::core::detail
