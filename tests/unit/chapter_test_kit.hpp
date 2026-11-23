// Shared helpers for the tests of loading pages by chapter location (#230,
// #232): a MuPDF context and document of the test's own, an ePub writer for a
// many-chapter book, the token fixture's expected text, and the MuPDF-only
// cost routes the cost oracles compare against.
//
// Every helper obeys the product's MuPDF rules (#178, #179, #146): nothing
// with a destructor lives in a fz_try, and every fz_catch marks its error
// handled.
#pragma once

#include <catch2/catch_test_macros.hpp>

#include <mupdf/fitz.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <windows.h>

namespace litepdf::test_kit {

// A fresh directory under %TEMP%, removed with everything in it when the guard
// goes. Declared before any document that opens a file in it, so it is
// destroyed after they have closed the file (MuPDF opens it without
// FILE_SHARE_DELETE). The error_code overload: a throwing destructor would
// terminate.
struct TempDir {
    std::filesystem::path path;
    TempDir() {
        path = std::filesystem::temp_directory_path() /
               ("litepdf-chapters-" + std::to_string(GetCurrentProcessId()) + "-" +
                std::to_string(GetTickCount64()));
        std::filesystem::create_directories(path);
    }
    ~TempDir() { std::error_code ec; std::filesystem::remove_all(path, ec); }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

inline std::string utf8_of(const std::filesystem::path& p) {
    const auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

// A MuPDF context and document of the test's own, to compute what loading
// the page gives: fz_load_page + fz_bound_page, the route page_size used to
// take.
struct OwnDocument {
    fz_context*  ctx = nullptr;
    fz_document* doc = nullptr;

    explicit OwnDocument(const char* path) {
        ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
        REQUIRE(ctx != nullptr);
        fz_register_document_handlers(ctx);
        fz_try(ctx) { doc = fz_open_document(ctx, path); }
        fz_catch(ctx) { fz_report_error(ctx); doc = nullptr; }
        REQUIRE(doc != nullptr);
    }
    ~OwnDocument() {
        fz_drop_document(ctx, doc);
        fz_drop_context(ctx);
    }
    OwnDocument(const OwnDocument&) = delete;
    OwnDocument& operator=(const OwnDocument&) = delete;

    int page_count() {
        int n = -1;
        fz_var(n);
        fz_try(ctx) { n = fz_count_pages(ctx, doc); }
        fz_catch(ctx) { fz_report_error(ctx); }
        return n;
    }

    // False when the page does not load.
    bool loaded_bounds(int index, fz_rect& out) {
        fz_page* page = nullptr;
        bool ok = true;
        fz_var(page);
        fz_try(ctx) {
            page = fz_load_page(ctx, doc, index);
            out = fz_bound_page(ctx, page);
        }
        fz_always(ctx) { fz_drop_page(ctx, page); }
        fz_catch(ctx) { fz_report_error(ctx); ok = false; }
        return ok;
    }

    // #230: what Document::with_rendered_page hands its callback for
    // fz_load_page(index) at scale 1, no rotation: the same MuPDF calls in the
    // same order, then the same over-white compositing. False when any step
    // throws.
    bool rendered(int index, std::vector<std::uint8_t>& bgra, int& w, int& h) {
        fz_page* page = nullptr;
        fz_pixmap* pix = nullptr;
        fz_device* dev = nullptr;
        bool ok = true;
        fz_var(page);
        fz_var(pix);
        fz_var(dev);
        fz_try(ctx) {
            page = fz_load_page(ctx, doc, index);
            const fz_matrix m = fz_scale(1.0f, 1.0f);
            const fz_irect bbox = fz_round_rect(fz_transform_rect(fz_bound_page(ctx, page), m));
            pix = fz_new_pixmap_with_bbox(ctx, fz_device_bgr(ctx), bbox, nullptr, 1);
            fz_clear_pixmap_with_value(ctx, pix, 0xFF);
            dev = fz_new_draw_device(ctx, fz_identity, pix);
            fz_run_page(ctx, page, dev, m, nullptr);
            fz_close_device(ctx, dev);
        }
        fz_always(ctx) {
            if (dev) fz_drop_device(ctx, dev);
            if (page) fz_drop_page(ctx, page);
        }
        fz_catch(ctx) { fz_report_error(ctx); ok = false; }
        if (!ok) {
            if (pix) fz_drop_pixmap(ctx, pix);
            return false;
        }
        w = fz_pixmap_width(ctx, pix);
        h = fz_pixmap_height(ctx, pix);
        const std::uint8_t* samples = fz_pixmap_samples(ctx, pix);
        const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4;
        bgra.assign(samples, samples + n);
        fz_drop_pixmap(ctx, pix);
        for (std::size_t i = 0; i < n; i += 4) {
            std::uint8_t* p = bgra.data() + i;
            const int a = p[3];
            if (a == 255) continue;
            const int inv = 255 - a;
            p[0] = static_cast<std::uint8_t>(std::min(255, p[0] + inv));
            p[1] = static_cast<std::uint8_t>(std::min(255, p[1] + inv));
            p[2] = static_cast<std::uint8_t>(std::min(255, p[2] + inv));
            p[3] = 255;
        }
        return true;
    }

    // #232: the text of flat page `index`: fz_load_page, a default-option
    // structured text page, fz_print_stext_page_as_text. Empty when any step
    // throws. The bytes are copied out after the fz_try (#179), as in
    // Document::page_text.
    std::string text(int index) {
        fz_page* page = nullptr;
        fz_stext_page* stext = nullptr;
        fz_buffer* buf = nullptr;
        fz_output* out = nullptr;
        unsigned char* data = nullptr;
        std::size_t len = 0;
        bool ok = true;
        fz_var(page);
        fz_var(stext);
        fz_var(buf);
        fz_var(out);
        fz_try(ctx) {
            page = fz_load_page(ctx, doc, index);
            fz_stext_options opts = {};
            stext = fz_new_stext_page_from_page(ctx, page, &opts);
            buf = fz_new_buffer(ctx, 4096);
            out = fz_new_output_with_buffer(ctx, buf);
            fz_print_stext_page_as_text(ctx, out, stext);
            fz_close_output(ctx, out);   // flushes into buf before the storage read
            len = fz_buffer_storage(ctx, buf, &data);
        }
        fz_always(ctx) {
            if (out) fz_drop_output(ctx, out);
            if (stext) fz_drop_stext_page(ctx, stext);
            if (page) fz_drop_page(ctx, page);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
            ok = false;
        }
        std::string result;
        if (ok) result.assign(reinterpret_cast<const char*>(data), len);
        if (buf) fz_drop_buffer(ctx, buf);
        return result;
    }

    // #232: what the render worker draws for flat page `index` at `scale`
    // (RenderEngine.cpp worker_loop): fz_load_page, a display list, the
    // rounded bbox of the scaled list bounds, a BGR + alpha pixmap cleared to
    // 0xFF, a draw device at identity, fz_run_display_list. The bytes are
    // copied row by row through the pixmap stride; no compositing. False when
    // any step throws.
    bool rendered_via_list(int index, float scale, std::vector<std::uint8_t>& bgra, int& w, int& h) {
        fz_page* page = nullptr;
        fz_display_list* dl = nullptr;
        fz_pixmap* pix = nullptr;
        fz_device* dev = nullptr;
        bool ok = true;
        fz_var(page);
        fz_var(dl);
        fz_var(pix);
        fz_var(dev);
        fz_try(ctx) {
            page = fz_load_page(ctx, doc, index);
            dl = fz_new_display_list_from_page(ctx, page);
            const fz_matrix m = fz_scale(scale, scale);
            const fz_irect bbox = fz_round_rect(fz_transform_rect(fz_bound_display_list(ctx, dl), m));
            pix = fz_new_pixmap_with_bbox(ctx, fz_device_bgr(ctx), bbox, nullptr, 1);
            fz_clear_pixmap_with_value(ctx, pix, 0xFF);
            dev = fz_new_draw_device(ctx, fz_identity, pix);
            fz_run_display_list(ctx, dl, dev, m, fz_infinite_rect, nullptr);
            fz_close_device(ctx, dev);
        }
        fz_always(ctx) {
            if (dev) fz_drop_device(ctx, dev);
            if (dl) fz_drop_display_list(ctx, dl);
            if (page) fz_drop_page(ctx, page);
        }
        fz_catch(ctx) { fz_report_error(ctx); ok = false; }
        if (!ok) {
            if (pix) fz_drop_pixmap(ctx, pix);
            return false;
        }
        w = fz_pixmap_width(ctx, pix);
        h = fz_pixmap_height(ctx, pix);
        const std::uint8_t* samples = fz_pixmap_samples(ctx, pix);
        const std::size_t stride = static_cast<std::size_t>(fz_pixmap_stride(ctx, pix));
        const std::size_t row = static_cast<std::size_t>(w) * 4;
        bgra.resize(row * static_cast<std::size_t>(h));
        for (int y = 0; y < h; ++y) {
            const std::uint8_t* src = samples + static_cast<std::size_t>(y) * stride;
            std::copy(src, src + row, bgra.begin() + static_cast<std::ptrdiff_t>(row * static_cast<std::size_t>(y)));
        }
        fz_drop_pixmap(ctx, pix);
        return true;
    }
};

// An ePub of `chapters` spine items, each one heading and one sentence (one
// page), written with MuPDF's zip writer (1.27.2 stores every entry). False
// when MuPDF throws.
inline bool write_many_chapter_epub(const std::filesystem::path& file, int chapters) {
    const std::string sentence =
        "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod "
        "tempor incididunt ut labore et dolore magna aliqua.";
    // Every string is built here, before the fz_try (#179).
    std::vector<std::pair<std::string, std::string>> entries;
    entries.emplace_back("mimetype", "application/epub+zip");
    entries.emplace_back(
        "META-INF/container.xml",
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
        "<rootfiles><rootfile full-path=\"OEBPS/content.opf\" "
        "media-type=\"application/oebps-package+xml\"/></rootfiles></container>\n");
    std::string manifest;
    std::string spine;
    for (int k = 1; k <= chapters; ++k) {
        const std::string id = "c" + std::to_string(k);
        manifest += "<item id=\"" + id + "\" href=\"" + id +
                    ".xhtml\" media-type=\"application/xhtml+xml\"/>\n";
        spine += "<itemref idref=\"" + id + "\"/>\n";
    }
    entries.emplace_back(
        "OEBPS/content.opf",
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\" unique-identifier=\"b\">\n"
        "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>t</dc:title>"
        "<dc:language>en</dc:language><dc:identifier id=\"b\">litepdf-230</dc:identifier></metadata>\n"
        "<manifest>\n" + manifest + "</manifest>\n<spine>\n" + spine + "</spine>\n</package>\n");
    for (int k = 1; k <= chapters; ++k) {
        const std::string n = std::to_string(k);
        entries.emplace_back(
            "OEBPS/c" + n + ".xhtml",
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<html xmlns=\"http://www.w3.org/1999/xhtml\">\n"
            "<head><title>Chapter " + n + "</title></head>\n<body>\n<h1>Chapter " + n +
            "</h1>\n<p>" + sentence + "</p>\n</body>\n</html>\n");
    }
    const std::string name = utf8_of(file);
    const auto* const data = entries.data();
    const std::size_t count = entries.size();

    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx) return false;
    fz_zip_writer* zip = nullptr;
    fz_buffer* buf = nullptr;
    bool ok = true;
    fz_var(zip);
    fz_var(buf);
    fz_try(ctx) {
        zip = fz_new_zip_writer(ctx, name.c_str());
        for (std::size_t i = 0; i < count; ++i) {
            buf = fz_new_buffer_from_copied_data(
                ctx, reinterpret_cast<const unsigned char*>(data[i].second.data()),
                data[i].second.size());
            fz_write_zip_entry(ctx, zip, data[i].first.c_str(), buf, 0);
            fz_drop_buffer(ctx, buf);
            buf = nullptr;
        }
        fz_close_zip_writer(ctx, zip);
    }
    fz_always(ctx) {
        fz_drop_buffer(ctx, buf);
        fz_drop_zip_writer(ctx, zip);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        ok = false;
    }
    fz_drop_context(ctx);
    return ok;
}

// The best of three runs of `run`, in ms.
inline double best_of_3_ms(const std::function<void()>& run) {
    double best = 1e300;
    for (int r = 0; r < 3; ++r) {
        const auto t0 = std::chrono::steady_clock::now();
        run();
        const auto t1 = std::chrono::steady_clock::now();
        best = std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    return best;
}

// Every page of `own`'s document, loaded by chapter location in chapter order,
// with `use` run on each. False when MuPDF throws.
inline bool for_each_page_by_location(OwnDocument& own, void (*use)(fz_context*, fz_page*)) {
    fz_context* const ctx = own.ctx;
    fz_document* const doc = own.doc;
    fz_page* page = nullptr;
    bool ok = true;
    fz_var(page);
    fz_try(ctx) {
        const int nc = fz_count_chapters(ctx, doc);
        for (int c = 0; c < nc; ++c) {
            const int m = fz_count_chapter_pages(ctx, doc, c);
            for (int k = 0; k < m; ++k) {
                page = fz_load_chapter_page(ctx, doc, c, k);
                use(ctx, page);
                fz_drop_page(ctx, page);
                page = nullptr;
            }
        }
    }
    fz_always(ctx) { fz_drop_page(ctx, page); }
    fz_catch(ctx) {
        fz_report_error(ctx);
        ok = false;
    }
    return ok;
}

inline void bound_only(fz_context* ctx, fz_page* page) { (void)fz_bound_page(ctx, page); }

inline void small_pixmap(fz_context* ctx, fz_page* page) {
    fz_drop_pixmap(ctx, fz_new_pixmap_from_page(ctx, page, fz_scale(0.05f, 0.05f),
                                                fz_device_bgr(ctx), 1));
}

// --- the token fixture (tests/fixtures/chapter-tokens.epub, #232) ------------

inline constexpr const char* kTokensFixture = "tests/fixtures/chapter-tokens.epub";

// Pages per chapter of the token fixture: 12 chapters, 35 pages. Flat page i
// holds exactly the token tokCC_P for its chapter CC and page P within it.
inline constexpr int kTokenChapterPages[12] = {1, 4, 2, 5, 3, 1, 4, 2, 5, 3, 1, 4};

// The token flat page `flat_index` of the token fixture holds.
inline std::string expected_token(int flat_index) {
    int rest = flat_index;
    for (int c = 0; c < 12; ++c) {
        if (rest < kTokenChapterPages[c]) {
            char buf[16];
            std::snprintf(buf, sizeof buf, "tok%02d_%d", c, rest);
            return buf;
        }
        rest -= kTokenChapterPages[c];
    }
    return {};
}

// Every token ("tok" + 2 digits + "_" + 1 digit) in `text`, in order.
inline std::vector<std::string> tokens_in(std::string_view text) {
    std::vector<std::string> found;
    const auto digit = [](char ch) { return ch >= '0' && ch <= '9'; };
    for (std::size_t i = 0; i + 7 <= text.size(); ++i) {
        if (text.compare(i, 3, "tok") == 0 && digit(text[i + 3]) && digit(text[i + 4]) &&
            text[i + 5] == '_' && digit(text[i + 6])) {
            found.emplace_back(text.substr(i, 7));
            i += 6;
        }
    }
    return found;
}

}  // namespace litepdf::test_kit
