#include "core/Document.hpp"

#include "core/MuPDFRoot.hpp"
#include "core/EscrowContext.hpp"
#include "core/detail/CbzPageSizer.hpp"
#include "core/detail/PageHitsAccess.hpp"
#include "core/detail/TextPageAccess.hpp"
#include "core/SearchQuery.hpp"
#include "core/SystemFonts.hpp"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cctype>
#include <climits>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

// SecureZeroMemory is a volatile-write loop the optimizer cannot elide,
// unlike std::memset which can be DCE'd when the buffer's lifetime ends
// without an observable use. Required by Phase 8 D3 hygiene.
#include <windows.h>

namespace litepdf::core {

struct Document::Impl {
    // Shared, not unique: every core::EscrowContext cloned from this Document's
    // family holds a copy, so the lock table AND the root context outlive the
    // last escrow even when this Document is destroyed first (#61). Declared
    // FIRST so it is destroyed LAST, after ~Impl has dropped the document
    // through it.
    std::shared_ptr<detail::MuPDFRoot> root;
    // The root's context, borrowed. Owned by `root`, not by this Impl.
    fz_context* ctx = nullptr;
    fz_document* doc = nullptr;
    bool needs_password = false;
    bool authenticated = false;
    std::filesystem::path path;
    // Read once, when the document becomes open (open() or authenticate()),
    // so page_count() never calls into MuPDF. fz_count_pages can throw after
    // a successful open -- a /Count MuPDF rejects, or a reflowable layout that
    // fails -- and page_count() runs inside window procedures (#119). Nothing
    // in LitePDF re-lays-out a document after it opens.
    std::size_t page_count = 0;

    // #176: a CBZ page's size from its image header. `cbz_path` is the
    // absolute UTF-8 path of an open .cbz (absolute, so a later change of the
    // working directory cannot point the probe at another file); the sizer is
    // created on the first page_size, so a CBZ that sizes no page pays nothing.
    // Declared after `root`, so the sizer drops its archive while the root
    // context is alive.
    std::string cbz_path;
    bool cbz_probed = false;
    std::unique_ptr<detail::CbzPageSizer> cbz;

    void reset_cbz() noexcept {
        cbz.reset();
        cbz_probed = false;
        cbz_path.clear();
    }

    // Serializes every method that touches impl_->ctx via fz_try/fz_catch.
    // MuPDF's exception handling uses a per-fz_context setjmp/longjmp error
    // stack that is NOT thread-safe — two threads in fz_try on the same ctx
    // corrupt the stack and crash. The MuPDFRoot lock table above covers the
    // allocator / font cache / freetype / glyph cache, but NOT the error
    // stack. Holding this mutex across each fz_try ... fz_catch block
    // serializes the error stack usage.
    //
    // It guards this Document's own context and document only. Search scans
    // run on a session-owned document and context, under the session's own
    // lock, so they never take this one (#130); render workers likewise use
    // their own documents. In practice only the UI thread takes it.
    mutable std::mutex doc_mutex;

    Impl() : root(detail::MuPDFRoot::create()) {
        ctx = root->ctx;
        fz_register_document_handlers(ctx);
        install_system_cjk_font_loader(ctx);  // cjk-system-font-loader: system CJK
    }

    // Drops only the document. The root context is dropped by ~MuPDFRoot, after
    // the last EscrowContext holding it has gone: MuPDF frees this family's ICC
    // profiles through the context that created them, so the root must be the
    // last context of the family to die (#61).
    ~Impl() {
        cbz.reset();
        if (doc) fz_drop_document(ctx, doc);
    }
};

Document::Document() : impl_(std::make_unique<Impl>()) {}
Document::~Document() = default;

// Move operations swap the unique_ptrs rather than moving ownership to null.
// This keeps both `this` and `other` with a valid Impl after any move, so
// moved-from Documents remain queryable (is_open() -> false) and reusable
// (open() can be called again) without risking nullptr dereferences in the
// public API, which accesses impl_-> unconditionally.
Document::Document(Document&& other) noexcept : impl_(std::make_unique<Impl>()) {
    std::swap(impl_, other.impl_);
}

Document& Document::operator=(Document&& other) noexcept {
    if (this != &other) {
        std::swap(impl_, other.impl_);
    }
    return *this;
}

namespace {

// LitePDF accepts a small allowlist of document formats that MuPDF supports:
// PDF, ePub, CBZ (comic book zip), XPS, FB2, and SVG. Recognize by extension
// plus a magic-number sanity check for the PDF/ZIP families, so MuPDF won't
// be handed unrelated files (e.g. PNG) that it would otherwise open.
bool looks_like_supported_document(const std::filesystem::path& path) {
    // Use u8string() for hygiene even though extensions are ASCII-only in
    // the allowlist; keeps string-handling uniform with the fz_open_document
    // call site below. C++17 returns std::string; C++20 returns std::u8string
    // (we reinterpret-cast the bytes to std::string for the compare).
#if defined(__cpp_lib_char8_t) && __cpp_lib_char8_t >= 201907L
    auto ext_u8 = path.extension().u8string();
    std::string ext(reinterpret_cast<const char*>(ext_u8.data()), ext_u8.size());
#else
    std::string ext = path.extension().u8string();
#endif
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    static constexpr std::string_view kAllowedExts[] = {
        ".pdf", ".epub", ".cbz", ".xps", ".fb2", ".svg"
    };
    bool ext_allowed = false;
    for (auto e : kAllowedExts) {
        if (ext == e) { ext_allowed = true; break; }
    }
    if (!ext_allowed) return false;

    if (ext == ".pdf") {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        char header[5] = {0};
        f.read(header, 4);
        return std::string_view(header, 4) == "%PDF";
    }
    if (ext == ".epub" || ext == ".cbz" || ext == ".xps") {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        char header[4] = {0};
        f.read(header, 4);
        return std::string_view(header, 4) == std::string_view("PK\x03\x04", 4);
    }
    // .fb2 and .svg are XML; no magic check.
    return true;
}

void flatten_outline(fz_context* ctx,
                     fz_outline* node,
                     int depth,
                     std::vector<Document::OutlineEntry>& out) {
    for (; node; node = node->next) {
        Document::OutlineEntry entry;
        entry.depth = depth;
        entry.title = node->title ? std::string(node->title) : std::string{};
        entry.page_index = Document::kNoPage;
        if (node->page.page >= 0) {
            entry.page_index = static_cast<std::size_t>(node->page.page);
        }
        out.push_back(std::move(entry));

        if (node->down) {
            flatten_outline(ctx, node->down, depth + 1, out);
        }
    }
}

// True for a path whose extension is .cbz, in any case.
bool is_cbz_path(const std::filesystem::path& path) {
    std::wstring ext = path.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](wchar_t c) { return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c; });
    return ext == L".cbz";
}

// The UTF-8 bytes of `path` made absolute, or "" when it cannot be.
std::string absolute_utf8(const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::path abs = std::filesystem::absolute(path, ec);
    if (ec) return {};
#if defined(__cpp_lib_char8_t) && __cpp_lib_char8_t >= 201907L
    const std::u8string u8 = abs.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
#else
    return abs.u8string();
#endif
}

// Counts the pages of a document that has just become open. False when MuPDF
// throws or answers a negative count; the caller then refuses the document.
//
// A PDF's count is its /Root/Pages/Count until MuPDF first builds its page-tree
// map, which rewrites a /Count larger than the tree to the real number of pages
// (pdf_load_page_tree_internal, pdf-page.c). That happens on the first page
// lookup, so look up page 0 before counting: cached any earlier, the count
// keeps pages that do not exist (#119). The app loads page 0 right after
// opening anyway, so this moves the work rather than adding to it.
bool read_page_count(fz_context* ctx, fz_document* doc, std::size_t& out) {
    int n = -1;
    fz_try(ctx) {
        n = fz_count_pages(ctx, doc);
        pdf_document* pdf = pdf_specifics(ctx, doc);
        if (pdf && n > 0) {
            (void)pdf_lookup_page_obj(ctx, pdf, 0);
            n = fz_count_pages(ctx, doc);
        }
    }
    fz_catch(ctx) {
        fz_report_error(ctx);   // also marks it handled (#146)
        return false;
    }
    if (n < 0) return false;
    out = static_cast<std::size_t>(n);
    return true;
}

} // namespace

std::optional<Document::OpenError> Document::open(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return OpenError::FileNotFound;

    if (!looks_like_supported_document(path)) return OpenError::UnsupportedFormat;

    impl_->reset_cbz();

    // UTF-8 conversion — fz_open_document takes UTF-8 char*. On Windows,
    // path.string() returns ACP (CP950/936/etc); path.u8string() returns
    // UTF-8. In C++17 path::u8string() returns std::string; in C++20+ it
    // returns std::u8string (we reinterpret-cast the bytes).
    //
    // Done BEFORE the fz_try, never inside it: fz_try is setjmp/longjmp, and
    // a longjmp out of a scope that holds an object with a destructor is
    // undefined ([csetjmp.syn]), while a C++ exception thrown inside it (a
    // bad_alloc from the conversion) leaves without popping MuPDF's try stack.
#if defined(__cpp_lib_char8_t) && __cpp_lib_char8_t >= 201907L
    const std::u8string u8 = path.u8string();
    const char* utf8_bytes = reinterpret_cast<const char*>(u8.c_str());
#else
    const std::string u8 = path.u8string();
    const char* utf8_bytes = u8.c_str();
#endif

    fz_try(impl_->ctx) {
        impl_->doc = fz_open_document(impl_->ctx, utf8_bytes);
    }
    fz_catch(impl_->ctx) {
        // File was recognized as a PDF but failed to parse → treat as corrupted.
        // Reporting also marks the error handled: with no document to drop,
        // the context's own drop would otherwise report it again as an
        // "UNHANDLED EXCEPTION!" (#146).
        fz_report_error(impl_->ctx);
        return OpenError::Corrupted;
    }

    if (!impl_->doc) return OpenError::Other;

    impl_->path = path;
    if (is_cbz_path(path)) impl_->cbz_path = absolute_utf8(path);
    impl_->needs_password = fz_needs_password(impl_->ctx, impl_->doc) != 0;
    impl_->authenticated = !impl_->needs_password;
    if (impl_->needs_password) {
        // Keep fz_document alive so authenticate() can complete the handshake.
        return OpenError::NeedsPassword;
    }

    if (!read_page_count(impl_->ctx, impl_->doc, impl_->page_count)) {
        close();
        return OpenError::Corrupted;
    }
    return std::nullopt;
}

bool Document::is_open() const noexcept {
    if (!impl_->doc) return false;
    // A document that needs password but hasn't been authenticated is not "open".
    return impl_->authenticated;
}

void Document::close() noexcept {
    impl_->reset_cbz();
    if (impl_->doc) {
        fz_drop_document(impl_->ctx, impl_->doc);
        impl_->doc = nullptr;
    }
    impl_->needs_password = false;
    impl_->authenticated = false;
    impl_->path.clear();
    impl_->page_count = 0;
}

const std::filesystem::path& Document::source_path() const noexcept {
    return impl_->path;
}

bool Document::with_rendered_page(
    std::size_t page_idx,
    float scale_x_px_per_pt,
    float scale_y_px_per_pt,
    bool  rotate_90,
    const std::function<void(int width_px,
                             int height_px,
                             const std::uint8_t* bgra_top_down)>& callback) const
{
    if (!impl_->doc || !impl_->ctx) return false;

    // Use this Document's own fz_context + fz_document so authentication
    // (encrypted PDFs, Phase 8) and reflowable layout state (ePub, CBZ)
    // are preserved. Cloning would lose both. doc_mutex serializes with
    // every other user of this context; see the Impl comment.
    std::lock_guard<std::mutex> lk(impl_->doc_mutex);

    fz_context*  ctx  = impl_->ctx;
    fz_document* doc  = impl_->doc;
    fz_page*     page = nullptr;
    fz_pixmap*   pix  = nullptr;
    bool         ok   = false;

    fz_device* dev = nullptr;
    fz_var(page);
    fz_var(pix);
    fz_var(dev);
    fz_try(ctx) {
        page = fz_load_page(ctx, doc, static_cast<int>(page_idx));
        if (page) {
            fz_matrix m = fz_scale(scale_x_px_per_pt, scale_y_px_per_pt);
            if (rotate_90) m = fz_pre_rotate(m, 90.0f);
            // Render onto an OPAQUE WHITE background. fz_new_pixmap_from_page
            // with alpha=1 clears to TRANSPARENT, which a downstream consumer
            // that ignores alpha (printer DC BitBlt, D2D ALPHA_MODE_IGNORE)
            // paints as opaque BLACK for any unfilled page area — so pages
            // without their own white fill printed/rendered black. Keep the
            // 4-channel BGRA buffer but clear it to white, then run the page.
            fz_irect bbox = fz_round_rect(
                fz_transform_rect(fz_bound_page(ctx, page), m));
            pix = fz_new_pixmap_with_bbox(ctx, fz_device_bgr(ctx), bbox,
                                          nullptr, /*alpha*/ 1);
            fz_clear_pixmap_with_value(ctx, pix, 0xFF);  // opaque white
            dev = fz_new_draw_device(ctx, fz_identity, pix);
            fz_run_page(ctx, page, dev, m, nullptr);
            fz_close_device(ctx, dev);
        }
    } fz_always(ctx) {
        if (dev) fz_drop_device(ctx, dev);
    } fz_catch(ctx) {
        fz_report_error(ctx);   // also marks it handled (#146)
        if (pix)  { fz_drop_pixmap(ctx, pix);   pix  = nullptr; }
        if (page) { fz_drop_page(ctx, page);    page = nullptr; }
        return false;
    }

    if (page) fz_drop_page(ctx, page);
    if (!pix) return false;

    int w = 0, h = 0;
    const std::uint8_t* samples = nullptr;
    fz_try(ctx) {
        w       = fz_pixmap_width(ctx, pix);
        h       = fz_pixmap_height(ctx, pix);
        samples = fz_pixmap_samples(ctx, pix);
    } fz_catch(ctx) {
        fz_report_error(ctx);   // also marks it handled (#146)
        fz_drop_pixmap(ctx, pix);
        return false;
    }

    if (samples && w > 0 && h > 0) {
        // MuPDF with alpha=1 initializes the pixmap to TRANSPARENT
        // (0,0,0,0), not white. Pages with no explicit white-background
        // drawing (ePub, some encrypted PDFs) leave the un-covered area
        // transparent. StretchDIBits with BI_RGB ignores the alpha
        // channel, so those pixels would be blitted as BGR=(0,0,0)
        // -- solid black. Composite each pixel over white in-place so
        // the resulting BGRA can be safely SRCCOPY'd.
        //
        // MuPDF pixmaps use PREMULTIPLIED alpha during rendering
        // (mupdf/fitz/pixmap.h:37). For premultiplied source over a
        // solid white background:
        //   out_color = src_color + (255 - alpha)  (clamped)
        // -- because src_color already encodes color * alpha. Using the
        // straight-alpha formula (src_color * alpha + 255 * inv) on a
        // premultiplied buffer produces fringe-darkening on partial-alpha
        // edges (anti-aliased glyph borders show ~25% too dark for
        // saturated colors).
        // The const_cast is safe: this Document owns the pixmap (about to
        // drop it after the callback) and doc_mutex serializes all access.
        std::uint8_t* mut = const_cast<std::uint8_t*>(samples);
        const std::size_t n_pixels = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
        for (std::size_t i = 0; i < n_pixels; ++i) {
            std::uint8_t* p = mut + i * 4;  // BGRA layout
            const int a = p[3];
            if (a == 255) continue;  // fully opaque -- common case, no work
            const int inv = 255 - a;
            p[0] = static_cast<std::uint8_t>(std::min(255, p[0] + inv));
            p[1] = static_cast<std::uint8_t>(std::min(255, p[1] + inv));
            p[2] = static_cast<std::uint8_t>(std::min(255, p[2] + inv));
            p[3] = 255;
        }
        callback(w, h, samples);
        ok = true;
    }

    fz_drop_pixmap(ctx, pix);
    return ok;
}

bool Document::authenticate(std::string_view password) {
    if (!impl_->doc) return false;
    // fz_authenticate_password expects NUL-terminated; copy to std::string to
    // ensure it. The copy is wiped before going out of scope so the password
    // bytes do not linger in freed heap memory (Phase 8 D3 — closes the last
    // hop in the edit-control → wide buffer → UTF-8 → MuPDF chain).
    std::string pw(password);
    int ok = 0;
    fz_try(impl_->ctx) {
        ok = fz_authenticate_password(impl_->ctx, impl_->doc, pw.c_str());
    }
    fz_catch(impl_->ctx) {
        fz_report_error(impl_->ctx);   // also marks it handled (#146)
        SecureZeroMemory(pw.data(), pw.size());
        return false;
    }
    SecureZeroMemory(pw.data(), pw.size());
    if (ok != 0) {
        // The password was right even if the pages cannot be counted, so
        // report it accepted and close the document: a false here would read
        // as a wrong password and burn the user's retries. Callers check
        // is_open() (DocumentView refuses a closed document).
        if (read_page_count(impl_->ctx, impl_->doc, impl_->page_count)) {
            impl_->authenticated = true;
        } else {
            close();
        }
        return true;
    }
    return false;
}

std::size_t Document::page_count() const {
    if (!is_open()) throw std::logic_error("page_count called on unopened document");
    return impl_->page_count;
}

PageSize Document::page_size(std::size_t index) const {
    if (!impl_->doc) throw std::logic_error("page_size called on unopened document");

    // Defense in depth: protect impl_->ctx error stack from any concurrent
    // call into this Document's context. See Impl::doc_mutex.
    std::lock_guard<std::mutex> lk(impl_->doc_mutex);

    // A CBZ page's size from its image header (#176): loading the page reads
    // the whole image out of the archive first. Outside the fz_try below,
    // which must not be returned from. A page the header path declines is
    // loaded as before.
    if (!impl_->cbz_path.empty()) {
        if (!impl_->cbz_probed) {
            impl_->cbz_probed = true;
            impl_->cbz = detail::CbzPageSizer::create(impl_->ctx, impl_->doc,
                                                      impl_->cbz_path.c_str(),
                                                      impl_->page_count);
        }
        if (impl_->cbz) {
            if (const auto size = impl_->cbz->header_size(index)) return *size;
        }
    }

    fz_page* page = nullptr;
    fz_rect bounds = {};
    fz_var(page);
    fz_try(impl_->ctx) {
        // A PDF page's bounds come from its page object alone (MediaBox,
        // CropBox, Rotate, UserUnit): this is what fz_bound_page computes
        // for a loaded PDF page (pdf_bound_page, pdf-page.c). fz_load_page
        // would also load every annotation and scan the resources for
        // transparency, one file read per object not yet loaded, and this
        // runs on the UI thread (#165). Other formats load the page.
        pdf_document* pdf = pdf_specifics(impl_->ctx, impl_->doc);
        if (pdf) {
            pdf_obj* obj = pdf_lookup_page_obj(impl_->ctx, pdf, static_cast<int>(index));
            fz_rect box;
            fz_matrix ctm;
            pdf_page_obj_transform(impl_->ctx, obj, &box, &ctm);
            bounds = fz_transform_rect(box, ctm);
        } else {
            page = fz_load_page(impl_->ctx, impl_->doc, static_cast<int>(index));
            bounds = fz_bound_page(impl_->ctx, page);
        }
    }
    fz_always(impl_->ctx) {
        if (page) fz_drop_page(impl_->ctx, page);
    }
    fz_catch(impl_->ctx) {
        fz_report_error(impl_->ctx);   // also marks it handled (#146)
        throw std::runtime_error("page_size: page bounds not readable");
    }

    PageSize result;
    result.width_pt  = bounds.x1 - bounds.x0;
    result.height_pt = bounds.y1 - bounds.y0;
    return result;
}
bool Document::read_ahead_page_sizes() const {
    if (!impl_->doc || impl_->cbz_path.empty()) return false;
    std::lock_guard<std::mutex> lk(impl_->doc_mutex);
    if (!impl_->cbz_probed) {
        impl_->cbz_probed = true;
        impl_->cbz = detail::CbzPageSizer::create(impl_->ctx, impl_->doc, impl_->cbz_path.c_str(),
                                                  impl_->page_count);
    }
    return impl_->cbz && impl_->cbz->read_ahead();
}

std::string Document::page_text(std::size_t index) const {
    if (!impl_->doc) throw std::logic_error("page_text called on unopened document");

    // Defense in depth: serialize with any concurrent call into this
    // Document's context. See Impl::doc_mutex.
    std::lock_guard<std::mutex> lk(impl_->doc_mutex);
    fz_page*       page  = nullptr;
    fz_stext_page* stext = nullptr;
    fz_buffer*     buf   = nullptr;
    fz_output*     out   = nullptr;
    unsigned char* data  = nullptr;
    std::size_t    len   = 0;
    fz_var(page);
    fz_var(stext);
    fz_var(buf);
    fz_var(out);

    fz_try(impl_->ctx) {
        page  = fz_load_page(impl_->ctx, impl_->doc, static_cast<int>(index));
        fz_stext_options opts = {};
        stext = fz_new_stext_page_from_page(impl_->ctx, page, &opts);

        buf = fz_new_buffer(impl_->ctx, 4096);
        out = fz_new_output_with_buffer(impl_->ctx, buf);
        fz_print_stext_page_as_text(impl_->ctx, out, stext);
        // IMPORTANT: fz_close_output must run before fz_buffer_storage to flush
        // all pending bytes into the buffer.
        fz_close_output(impl_->ctx, out);
        len = fz_buffer_storage(impl_->ctx, buf, &data);
    }
    fz_always(impl_->ctx) {
        if (out)   fz_drop_output(impl_->ctx, out);
        if (stext) fz_drop_stext_page(impl_->ctx, stext);
        if (page)  fz_drop_page(impl_->ctx, page);
    }
    fz_catch(impl_->ctx) {
        if (buf) fz_drop_buffer(impl_->ctx, buf);
        fz_report_error(impl_->ctx);   // also marks it handled (#146)
        throw std::runtime_error("page_text extraction failed");
    }

    // Copied out of the buffer AFTER the fz_try, as in TextPage::copy: the copy
    // can throw std::bad_alloc, and a C++ exception thrown inside a fz_try
    // leaves without popping MuPDF's try stack. So `buf` outlives the block.
    std::string result;
    try {
        result.assign(reinterpret_cast<const char*>(data), len);
    } catch (...) {
        fz_drop_buffer(impl_->ctx, buf);
        throw;
    }
    fz_drop_buffer(impl_->ctx, buf);
    return result;
}

fz_context* Document::clone_context() const {
    // Swap-based move ctor/assign guarantees impl_ is never null for a live
    // Document, so we assert rather than branch on it.
    assert(impl_);
    // Only hand out clones once a document has actually been opened. This
    // mirrors the rest of the API (page_count, page_size, etc. require a
    // live fz_document) and gives workers a clear "no doc, no context"
    // contract.
    if (!impl_->doc) return nullptr;
    // fz_clone_context is thread-safe w.r.t. the source context's internal
    // locking (which the Impl ctor installed via fz_locks_context); safe to
    // call while other threads hold clones of the same original context.
    // Caller takes ownership and must fz_drop_context().
    //
    // NOTE: fz_clone_context itself may return nullptr on OOM. Callers
    // must treat the nullptr return as a recoverable error, not just as
    // "document not opened".
    return fz_clone_context(impl_->ctx);
}

std::vector<Document::PageHit> detail::page_hits_on(
    fz_context* ctx, fz_document* doc, std::size_t page,
    std::string_view needle_utf8, Document::SearchFlags flags, fz_cookie* cookie)
{
    std::vector<Document::PageHit> out;
    if (!ctx || !doc || needle_utf8.empty()) return out;

    const std::string needle =
        build_search_needle(needle_utf8, flags.whole_word, flags.regex);
    const auto options = static_cast<fz_search_options>(
        search_options_value(flags.match_case, flags.regex || flags.whole_word));

    fz_page* fzpage = nullptr;
    fz_stext_page* stext = nullptr;
    fz_search* search = nullptr;
    constexpr int kMaxQuads = 256;
    fz_quad quads[kMaxQuads] = {};
    int n = 0;
    bool fed = false;
    // Each of these is modified inside fz_try; fz_var them so their values are
    // well-defined on the (setjmp/longjmp) exception path that fz_always and
    // fz_catch read them on (context.h).
    fz_var(fzpage); fz_var(stext); fz_var(search); fz_var(n); fz_var(fed);

    fz_try(ctx) {
        fz_stext_options so = { FZ_STEXT_DEHYPHENATE };   // match legacy fz_search_page
        fzpage = fz_load_page(ctx, doc, static_cast<int>(page));
        // An abort set during the build stops it early WITHOUT throwing and
        // leaves a partial page; the match loop's abort check below runs before
        // anything searches it (spec §3.4).
        stext = fz_new_stext_page_from_page_with_cookie(ctx, fzpage, &so, cookie);
        search = fz_new_search(ctx);
        fz_search_set_options(ctx, search, options, needle.c_str());  // throws on bad regex
        for (;;) {
            if (cookie && abort_requested(*cookie)) break;            // mid-page cancel
            fz_search_result r = fz_search_forwards(ctx, search);
            if (r.reason == FZ_SEARCH_MORE_INPUT) {
                // Single page: feed it once (search keeps its OWN ref); later
                // requests get NULL = end-of-doc.
                fz_feed_search(ctx, search,
                               fed ? nullptr : fz_keep_stext_page(ctx, stext),
                               r.u.more_input.seq_needed);
                fed = true;
            } else if (r.reason == FZ_SEARCH_MATCH) {
                fz_search_result_details* d = r.u.match.result;
                for (int i = 0; i < d->num_quads && n < kMaxQuads; ++i)
                    quads[n++] = d->quads[i].quad;
                if (n >= kMaxQuads) break;
            } else {  // FZ_SEARCH_COMPLETE
                break;
            }
        }
    }
    fz_always(ctx) {
        if (search) fz_drop_search(ctx, search);     // releases the fed (kept) ref
        if (stext)  fz_drop_stext_page(ctx, stext);  // releases our own ref
        if (fzpage) fz_drop_page(ctx, fzpage);
    }
    fz_catch(ctx) {
        // fz_convert_error, not fz_caught_message: it also marks the error
        // handled, so a later throw on this context, or its drop, does not
        // report it again as an "UNHANDLED EXCEPTION!" (#146). A cookie abort
        // never lands here: the build stops without throwing (above) and
        // fz_run_page_contents swallows an FZ_ERROR_ABORT itself.
        std::fprintf(stderr, "litepdf: page_hits failed on page %zu: %s\n",
                     page, fz_convert_error(ctx, nullptr));
        return out;   // invalid regex / load failure → empty (UI gates via query_compiles)
    }

    // D15: log when the per-page hit cap is reached so ops can see truncation.
    // Placed before the extraction loop so the log fires even if reserve() or
    // push_back() throws. TODO(phase-6.x): surface hit_limit_reached flag to
    // SearchSession per design D15 (may require changing return type to
    // include metadata).
    if (n == kMaxQuads) {
        std::fprintf(stderr,
            "litepdf: page_hits: hit cap %d reached on page %zu — tail dropped\n",
            kMaxQuads, page);
    }

    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        Document::PageHit h{};
        h.ul_x = quads[i].ul.x; h.ul_y = quads[i].ul.y;
        h.ur_x = quads[i].ur.x; h.ur_y = quads[i].ur.y;
        h.ll_x = quads[i].ll.x; h.ll_y = quads[i].ll.y;
        h.lr_x = quads[i].lr.x; h.lr_y = quads[i].lr.y;
        // TODO(phase-6.2): replace with a centered ~30-char snippet once
        // stext-backed matching lands. v1 returns just the needle itself.
        h.snippet_utf8 = std::string(needle_utf8);
        out.push_back(std::move(h));
    }
    return out;
}

std::vector<Document::PageHit> Document::page_hits(
    std::size_t page,
    std::string_view needle_utf8,
    SearchFlags flags) const
{
    if (!is_open() || needle_utf8.empty()) return {};

    // Serialize access to impl_->ctx. See Impl::doc_mutex rationale.
    // Must cover the whole helper, since every MuPDF call in it pushes and
    // pops frames on the per-ctx error stack.
    std::lock_guard<std::mutex> lk(impl_->doc_mutex);
    return detail::page_hits_on(impl_->ctx, impl_->doc, page, needle_utf8, flags,
                                nullptr);
}

EscrowContext Document::clone_escrow_context() const {
    if (!is_open()) return EscrowContext{};
    // No doc_mutex: fz_clone_context locks through the family's lock table
    // (see clone_context()).
    return EscrowContext::clone_from(impl_->ctx);
}

bool Document::query_compiles(std::string_view needle_utf8, SearchFlags flags) const {
    if (!is_open() || needle_utf8.empty()) return true;
    if (!flags.regex && !flags.whole_word) return true;  // plain literal (no \b wrap) always compiles
    const std::string needle =
        build_search_needle(needle_utf8, flags.whole_word, flags.regex);
    const auto options = static_cast<fz_search_options>(
        search_options_value(flags.match_case, true));
    std::lock_guard<std::mutex> lk(impl_->doc_mutex);
    fz_context* ctx = impl_->ctx;
    fz_search* search = nullptr;
    bool ok = true;
    fz_var(search); fz_var(ok);
    fz_try(ctx) {
        search = fz_new_search(ctx);
        fz_search_set_options(ctx, search, options, needle.c_str());  // compiles regex
    }
    fz_always(ctx) { if (search) fz_drop_search(ctx, search); }
    fz_catch(ctx) {
        // A pattern that does not compile is this function's answer, not a
        // failure: mark it handled without printing it (#146).
        fz_convert_error(ctx, nullptr);
        ok = false;
    }
    return ok;
}

std::vector<Document::OutlineEntry> Document::outline() const {
    std::vector<OutlineEntry> result;
    if (!impl_->doc) return result;

    // Defense in depth: serialize with any concurrent call into this
    // Document's context. See Impl::doc_mutex.
    std::lock_guard<std::mutex> lk(impl_->doc_mutex);
    fz_outline* root = nullptr;
    fz_try(impl_->ctx) {
        root = fz_load_outline(impl_->ctx, impl_->doc);
    }
    fz_catch(impl_->ctx) {
        // Marks the error handled, as in page_hits_on (#146).
        std::fprintf(stderr, "litepdf: fz_load_outline failed: %s\n",
                     fz_convert_error(impl_->ctx, nullptr));
        return result;
    }

    if (root) {
        flatten_outline(impl_->ctx, root, 0, result);
        fz_drop_outline(impl_->ctx, root);
    }
    return result;
}

// ----------------------------------------------------------------------
// Text selection (#52)
// ----------------------------------------------------------------------

namespace {

constexpr int kSelectionQuadsInitial = 256;
// A memory bound, not a correctness limit: past it the HIGHLIGHT is truncated
// (copy() is unaffected) and highlight() logs it. 65,536 disjoint quads on one
// page is far beyond real documents; recorded as a known limitation.
constexpr int kSelectionQuadsMax     = 1 << 16;

fz_point to_fz(SelPoint p) noexcept { return fz_make_point(p.x, p.y); }
SelPoint from_fz(fz_point p) noexcept { return SelPoint{ p.x, p.y }; }

fz_point midpoint(fz_point a, fz_point b) noexcept {
    return fz_make_point((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
}

// True when `p` resolves to the end of the page's text: nothing lies between it
// and the trailing edge of the last character. One false positive, accepted: a
// trailing run of characters that on_highlight_char skips as "zero-extent" also
// reads as the end. That test is MuPDF's same_point, which truncates to int
// before comparing with 0.1 -- so it skips glyphs narrower than 1 pt, not only
// zero-width ones. Treating such a run as the end extends a copy by characters
// too small to see; it never moves the visible highlight.
bool at_text_end(fz_context* ctx, fz_stext_page* stext, SelPoint p,
                 SelPoint text_end) noexcept {
    fz_quad probe;
    return fz_highlight_selection(ctx, stext, to_fz(p), to_fz(text_end), &probe, 1) == 0;
}

}  // namespace

Document::TextPage::TextPage() noexcept = default;
Document::TextPage::~TextPage() = default;
Document::TextPage::TextPage(TextPage&&) noexcept = default;
Document::TextPage& Document::TextPage::operator=(TextPage&&) noexcept = default;

bool Document::TextPage::valid() const noexcept {
    return impl_ && impl_->stext;
}

int Document::TextPage::page() const noexcept {
    return valid() ? impl_->page : -1;
}

Document::TextPage Document::TextPage::share() const noexcept {
    TextPage out;
    out.impl_ = impl_;   // shared_ptr copy: never throws
    return out;
}

bool Document::TextPage::full_range(SelPoint& first, SelPoint& last) const noexcept {
    if (!valid()) return false;
    if (impl_->has_range) {
        first = impl_->range_first;
        last  = impl_->range_last;
        return true;
    }
    const fz_stext_char* head = nullptr;
    const fz_stext_char* tail = nullptr;
    // Top-level text blocks only, in list order: exactly the walk
    // fz_enumerate_selection makes, so "first" and "last" name the same
    // characters to it.
    for (fz_stext_block* block = impl_->stext->first_block; block; block = block->next) {
        if (block->type != FZ_STEXT_BLOCK_TEXT) continue;
        for (fz_stext_line* line = block->u.t.first_line; line; line = line->next) {
            for (fz_stext_char* ch = line->first_char; ch; ch = ch->next) {
                if (!head) head = ch;
                tail = ch;
            }
        }
    }
    if (!head) return false;
    // Edges, NOT origins. MuPDF resolves a point to the nearest character
    // BOUNDARY and selects the half-open range [start, end). A character's
    // origin sits on its leading edge, so the last character's origin resolves
    // to the boundary BEFORE it and would drop the page's final character. The
    // trailing edge resolves to the boundary after it.
    first = from_fz(midpoint(head->quad.ll, head->quad.ul));
    last  = from_fz(midpoint(tail->quad.lr, tail->quad.ur));
    impl_->range_first = first;
    impl_->range_last  = last;
    impl_->has_range   = true;
    return true;
}

Document::TextPage::Snapped Document::TextPage::snap(SelPoint a, SelPoint b,
                                                     SelectMode mode) const noexcept {
    if (!valid() || mode == SelectMode::Chars) return Snapped{ a, b };

    fz_context* ctx = impl_->escrow.get();
    fz_point pa = to_fz(a);
    fz_point pb = to_fz(b);
    // Cannot throw: walks the page, never allocates.
    fz_snap_selection(ctx, impl_->stext, &pa, &pb,
                      mode == SelectMode::Words ? FZ_SELECT_WORDS : FZ_SELECT_LINES);

    // fz_snap_selection writes the far end only when it finds a character at or
    // after it. When the later point lies past the page's last character there is
    // none, so the caller's raw SECOND point is left in place -- and on a
    // backward drag that is the EARLIER point, collapsing the selection to part
    // of one word. Put the end back.
    SelPoint text_start, text_end;
    if (full_range(text_start, text_end)
        && (at_text_end(ctx, impl_->stext, a, text_end)
            || at_text_end(ctx, impl_->stext, b, text_end))) {
        pb = to_fz(text_end);
    }
    return Snapped{ from_fz(pa), from_fz(pb) };
}

std::vector<Quad> Document::TextPage::highlight(SelPoint a, SelPoint b) const {
    std::vector<Quad> out;
    if (!valid()) return out;

    // fz_highlight_selection fills a caller-owned buffer and silently drops what
    // does not fit. It merges into the last quad BEFORE its capacity test and
    // drops only at len == cap, so "returned count == capacity" is an exact
    // signal to grow and retry (spec §3.1).
    std::vector<fz_quad> buf(static_cast<std::size_t>(kSelectionQuadsInitial));
    int n = 0;
    for (;;) {
        n = fz_highlight_selection(impl_->escrow.get(), impl_->stext, to_fz(a), to_fz(b),
                                   buf.data(), static_cast<int>(buf.size()));
        if (n < static_cast<int>(buf.size())
            || buf.size() >= static_cast<std::size_t>(kSelectionQuadsMax)) {
            break;
        }
        buf.resize(buf.size() * 2);
    }

    // Same policy as page_hits' D15 log: a capped result must be visible. At the
    // cap an exact fit and a dropped tail both return n == cap, so this may fire
    // on a complete highlight -- hence "may be".
    if (n >= kSelectionQuadsMax && !impl_->quad_cap_logged) {
        impl_->quad_cap_logged = true;
        std::fprintf(stderr,
            "litepdf: highlight: quad cap %d reached on page %d — tail may be dropped\n",
            kSelectionQuadsMax, impl_->page);
    }

    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const fz_quad& q = buf[static_cast<std::size_t>(i)];
        out.push_back(Quad{ q.ul.x, q.ul.y, q.ur.x, q.ur.y,
                            q.ll.x, q.ll.y, q.lr.x, q.lr.y });
    }
    return out;
}

std::string Document::TextPage::copy(SelPoint a, SelPoint b) const {
    std::string out;
    if (!valid()) return out;

    fz_context* ctx = impl_->escrow.get();
    char* text = nullptr;
    fz_var(text);
    fz_try(ctx) {
        text = fz_copy_selection(ctx, impl_->stext, to_fz(a), to_fz(b), /*crlf*/ 1);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        return out;   // a MuPDF allocation failure becomes an empty copy, not a MuPDF error
    }
    if (!text) return out;
    try {
        out.assign(text);
    } catch (...) {
        fz_free(ctx, text);
        throw;
    }
    fz_free(ctx, text);
    return out;
}

Document::TextPage detail::TextPageAccess::build(fz_context* ctx, fz_document* doc,
                                                 int index) noexcept {
    Document::TextPage result;
    if (!ctx || !doc || index < 0) return result;

    std::shared_ptr<Document::TextPage::Impl> handle;
    try {
        handle = std::make_shared<Document::TextPage::Impl>();
    } catch (...) {
        return result;
    }

    handle->escrow = EscrowContext::clone_from(ctx);
    if (!handle->escrow.valid()) return result;

    fz_page*       page  = nullptr;
    fz_stext_page* stext = nullptr;
    fz_var(page);
    fz_var(stext);
    fz_try(ctx) {
        if (index < fz_count_pages(ctx, doc)) {
            page = fz_load_page(ctx, doc, index);
            // Pinned to default options, the same as page_text. Two flags must
            // NOT be added (spec §3.1): FZ_STEXT_COLLECT_STRUCTURE nests text
            // under struct blocks the selection walkers skip, and
            // FZ_STEXT_DEHYPHENATE (page_hits) changes line joining so a copy
            // would disagree with page_text.
            fz_stext_options opts = {};
            stext = fz_new_stext_page_from_page(ctx, page, &opts);
        }
    }
    fz_always(ctx) {
        if (page) fz_drop_page(ctx, page);
    }
    fz_catch(ctx) {
        // fz_new_stext_page_from_page drops its partial page before rethrowing.
        fz_report_error(ctx);
        return result;
    }
    if (!stext) return result;

    handle->stext = stext;
    handle->page  = index;
    result.impl_  = std::move(handle);
    // Warm the cached range on the acquisition that already costs a page walk,
    // so no drag move pays for it.
    SelPoint warm_first, warm_last;
    (void)result.full_range(warm_first, warm_last);
    return result;
}

Document::TextPage Document::text_page(std::size_t index) const noexcept {
    if (!is_open() || index >= static_cast<std::size_t>(INT_MAX)) return TextPage{};

    // Only ACQUISITION serialises with the other users of this Document's
    // context: building the stext page loads a page from impl_->doc. Everything
    // afterwards runs on the handle's escrow, lock-free (spec §3.2).
    //
    // Taken through a deferred unique_lock rather than a lock_guard because this
    // function is noexcept: std::mutex::lock() throws std::system_error when the
    // calling thread already holds the mutex (MSVC raises
    // resource_deadlock_would_occur), and a throw out of a noexcept function is
    // std::terminate, not the empty handle this contract promises. No caller
    // holds doc_mutex today -- and none may: the empty handle is SILENT in every
    // configuration this project can build (all of them define NDEBUG; Debug
    // does not link against the Release-only MuPDF libs), so a re-entrant caller
    // would just see a selection that never starts (#70).
    std::unique_lock<std::mutex> lk(impl_->doc_mutex, std::defer_lock);
    try {
        lk.lock();
    } catch (...) {
        return TextPage{};
    }

    return detail::TextPageAccess::build(impl_->ctx, impl_->doc, static_cast<int>(index));
}

} // namespace litepdf::core
