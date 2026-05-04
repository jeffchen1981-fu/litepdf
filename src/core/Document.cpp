#include "core/Document.hpp"

#include <mupdf/fitz.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>

// SecureZeroMemory is a volatile-write loop the optimizer cannot elide,
// unlike std::memset which can be DCE'd when the buffer's lifetime ends
// without an observable use. Required by Phase 8 D3 hygiene.
#include <windows.h>

namespace litepdf::core {

namespace {

// MuPDF requires a lock table (FZ_LOCK_MAX entries) to be supplied at
// fz_new_context time when we want fz_clone_context to succeed. Without
// locks, MuPDF refuses to clone because cloned contexts share the same
// underlying store / font cache and must serialize access to them.
//
// The lock table itself is heap-owned per fz_context (MuPDF keeps the
// fz_locks_context pointer, so the pointed-to storage must outlive the
// context). We stash it in the Impl.
struct MuPDFLocks {
    std::array<std::mutex, FZ_LOCK_MAX> mutexes;
    fz_locks_context fz;
};

void litepdf_lock(void* user, int lock) {
    auto* locks = static_cast<MuPDFLocks*>(user);
    locks->mutexes[static_cast<std::size_t>(lock)].lock();
}

void litepdf_unlock(void* user, int lock) {
    auto* locks = static_cast<MuPDFLocks*>(user);
    locks->mutexes[static_cast<std::size_t>(lock)].unlock();
}

} // namespace

struct Document::Impl {
    std::unique_ptr<MuPDFLocks> locks;
    fz_context* ctx = nullptr;
    fz_document* doc = nullptr;
    bool needs_password = false;
    bool authenticated = false;
    std::filesystem::path path;

    // Phase 6 Task 3 / ship-blocker fix: serialize every method that
    // touches impl_->ctx via fz_try/fz_catch. MuPDF's exception handling
    // uses a per-fz_context setjmp/longjmp error stack that is NOT
    // thread-safe — two threads in fz_try on the same ctx corrupt the
    // stack and crash. The MuPDFLocks table above covers the allocator /
    // font cache / freetype / glyph cache, but NOT the error stack.
    // SearchSession workers call page_hits() concurrently during eager
    // all-pages scans, and UI thread calls page_count() in set_query at
    // the same time — both race on this ctx. Holding this mutex across
    // each fz_try ... fz_catch block serializes the error stack usage.
    //
    // Performance impact: per-Document search parallelism drops to 1.
    // Acceptable for Phase 6 because (a) per-page search is ms-scale
    // and the 2-worker SearchDispatcher was already a soft serializer
    // for cross-document work, (b) cross-tab searches span multiple
    // Document instances and therefore multiple doc_mutexes, preserving
    // tab-level parallelism which §5.4 actually promises.
    mutable std::mutex doc_mutex;

    Impl() : locks(std::make_unique<MuPDFLocks>()) {
        locks->fz.user = locks.get();
        locks->fz.lock = &litepdf_lock;
        locks->fz.unlock = &litepdf_unlock;
        ctx = fz_new_context(nullptr, &locks->fz, FZ_STORE_DEFAULT);
        if (!ctx) throw std::bad_alloc();
        fz_register_document_handlers(ctx);
    }

    ~Impl() {
        if (doc) fz_drop_document(ctx, doc);
        if (ctx) fz_drop_context(ctx);
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

} // namespace

std::optional<Document::OpenError> Document::open(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return OpenError::FileNotFound;

    if (!looks_like_supported_document(path)) return OpenError::UnsupportedFormat;

    fz_try(impl_->ctx) {
        // UTF-8 conversion — fz_open_document takes UTF-8 char*. On Windows,
        // path.string() returns ACP (CP950/936/etc); path.u8string() returns
        // UTF-8. In C++17 path::u8string() returns std::string; in C++20+ it
        // returns std::u8string (we reinterpret-cast the bytes).
#if defined(__cpp_lib_char8_t) && __cpp_lib_char8_t >= 201907L
        const std::u8string u8 = path.u8string();
        const char* utf8_bytes = reinterpret_cast<const char*>(u8.c_str());
#else
        const std::string u8 = path.u8string();
        const char* utf8_bytes = u8.c_str();
#endif
        impl_->doc = fz_open_document(impl_->ctx, utf8_bytes);
    }
    fz_catch(impl_->ctx) {
        // File was recognized as a PDF but failed to parse → treat as corrupted.
        return OpenError::Corrupted;
    }

    if (!impl_->doc) return OpenError::Other;

    impl_->path = path;
    impl_->needs_password = fz_needs_password(impl_->ctx, impl_->doc) != 0;
    impl_->authenticated = !impl_->needs_password;
    if (impl_->needs_password) {
        // Keep fz_document alive so authenticate() can complete the handshake.
        return OpenError::NeedsPassword;
    }

    return std::nullopt;
}

bool Document::is_open() const noexcept {
    if (!impl_->doc) return false;
    // A document that needs password but hasn't been authenticated is not "open".
    return impl_->authenticated;
}

void Document::close() noexcept {
    if (impl_->doc) {
        fz_drop_document(impl_->ctx, impl_->doc);
        impl_->doc = nullptr;
    }
    impl_->needs_password = false;
    impl_->authenticated = false;
    impl_->path.clear();
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
    // search workers et al per the Impl comment.
    std::lock_guard<std::mutex> lk(impl_->doc_mutex);

    fz_context*  ctx  = impl_->ctx;
    fz_document* doc  = impl_->doc;
    fz_page*     page = nullptr;
    fz_pixmap*   pix  = nullptr;
    bool         ok   = false;

    fz_try(ctx) {
        page = fz_load_page(ctx, doc, static_cast<int>(page_idx));
        if (page) {
            fz_matrix m = fz_scale(scale_x_px_per_pt, scale_y_px_per_pt);
            if (rotate_90) m = fz_pre_rotate(m, 90.0f);
            pix = fz_new_pixmap_from_page(
                ctx, page, m, fz_device_bgr(ctx), /*alpha*/1);
        }
    } fz_catch(ctx) {
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
        std::uint8_t* mut = const_cast<std::uint8_t*>(samples);
        const std::size_t n_pixels = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
        for (std::size_t i = 0; i < n_pixels; ++i) {
            std::uint8_t* p = mut + i * 4;  // BGRA layout
            const int a = p[3];
            if (a == 255) continue;  // fully opaque -- common case, no work
            const int inv = 255 - a;
            p[0] = static_cast<std::uint8_t>((p[0] * a + 255 * inv + 127) / 255);
            p[1] = static_cast<std::uint8_t>((p[1] * a + 255 * inv + 127) / 255);
            p[2] = static_cast<std::uint8_t>((p[2] * a + 255 * inv + 127) / 255);
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
        SecureZeroMemory(pw.data(), pw.size());
        return false;
    }
    SecureZeroMemory(pw.data(), pw.size());
    if (ok != 0) {
        impl_->authenticated = true;
        return true;
    }
    return false;
}

std::size_t Document::page_count() const {
    if (!impl_->doc) throw std::logic_error("page_count called on unopened document");
    // Serialize with page_hits (worker threads). See Impl::doc_mutex.
    std::lock_guard<std::mutex> lk(impl_->doc_mutex);
    int n = 0;
    fz_try(impl_->ctx) {
        n = fz_count_pages(impl_->ctx, impl_->doc);
    }
    fz_catch(impl_->ctx) {
        throw std::runtime_error("fz_count_pages failed");
    }
    return static_cast<std::size_t>(n);
}

PageSize Document::page_size(std::size_t index) const {
    if (!impl_->doc) throw std::logic_error("page_size called on unopened document");

    // Defense in depth: protect impl_->ctx error stack from any concurrent
    // worker call (page_hits). See Impl::doc_mutex.
    std::lock_guard<std::mutex> lk(impl_->doc_mutex);
    fz_page* page = nullptr;
    fz_rect bounds = {};
    fz_try(impl_->ctx) {
        page = fz_load_page(impl_->ctx, impl_->doc, static_cast<int>(index));
        bounds = fz_bound_page(impl_->ctx, page);
    }
    fz_always(impl_->ctx) {
        if (page) fz_drop_page(impl_->ctx, page);
    }
    fz_catch(impl_->ctx) {
        throw std::runtime_error("fz_load_page / fz_bound_page failed");
    }

    PageSize result;
    result.width_pt  = bounds.x1 - bounds.x0;
    result.height_pt = bounds.y1 - bounds.y0;
    return result;
}
std::string Document::page_text(std::size_t index) const {
    if (!impl_->doc) throw std::logic_error("page_text called on unopened document");

    // Defense in depth: serialize with any concurrent page_hits() call
    // from a worker thread. See Impl::doc_mutex.
    std::lock_guard<std::mutex> lk(impl_->doc_mutex);
    fz_page*       page  = nullptr;
    fz_stext_page* stext = nullptr;
    fz_buffer*     buf   = nullptr;
    fz_output*     out   = nullptr;
    std::string    result;

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

        unsigned char* data = nullptr;
        const std::size_t len = fz_buffer_storage(impl_->ctx, buf, &data);
        result.assign(reinterpret_cast<const char*>(data), len);
    }
    fz_always(impl_->ctx) {
        if (out)   fz_drop_output(impl_->ctx, out);
        if (buf)   fz_drop_buffer(impl_->ctx, buf);
        if (stext) fz_drop_stext_page(impl_->ctx, stext);
        if (page)  fz_drop_page(impl_->ctx, page);
    }
    fz_catch(impl_->ctx) {
        throw std::runtime_error("page_text extraction failed");
    }

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

std::vector<Document::PageHit> Document::page_hits(
    std::size_t page,
    std::string_view needle_utf8,
    SearchFlags flags,
    std::atomic<int>* abort_flag) const
{
    std::vector<PageHit> out;
    if (!is_open() || needle_utf8.empty()) return out;

    // Serialize access to impl_->ctx. See Impl::doc_mutex rationale.
    // Must cover the entire fz_try ... fz_catch block plus fz_load_page
    // below, since all of them push/pop frames on the per-ctx error
    // stack. Also protects against concurrent page_count() from the UI
    // thread's set_query path.
    std::lock_guard<std::mutex> lk(impl_->doc_mutex);

    // match_case is intentionally ignored on MuPDF 1.24.x — see the header
    // comment on SearchFlags. Silence the unused-parameter warning.
    (void)flags;

    // fz_search_page / fz_search_stext_page expect a NUL-terminated char*.
    const std::string needle(needle_utf8);

    fz_context* ctx = impl_->ctx;
    fz_page* pg = nullptr;

    // abort_flag is accepted for API forward-compatibility but not honored
    // by MuPDF 1.24.11 — fz_search_page takes no fz_cookie. See the header
    // comment on page_hits for the phase-11 upgrade path.
    (void)abort_flag;

    // Cap per-page hits. Larger than typical query hit counts; if a page
    // exceeds this we simply drop the tail — acceptable for v1 search UI.
    constexpr int kMaxQuads = 256;
    fz_quad quads[kMaxQuads] = {};
    int marks[kMaxQuads] = {};
    int n = 0;

    fz_try(ctx) {
        pg = fz_load_page(ctx, impl_->doc, static_cast<int>(page));
        // MuPDF 1.24.11 only exposes fz_search_page (always case-insensitive).
        // When we upgrade to a MuPDF with fz_search_page2 + FZ_SEARCH_EXACT,
        // switch on flags.match_case here.
        n = fz_search_page(ctx, pg, needle.c_str(), marks, quads, kMaxQuads);
    }
    fz_always(ctx) {
        if (pg) fz_drop_page(ctx, pg);
    }
    fz_catch(ctx) {
        std::fprintf(stderr, "litepdf: page_hits failed on page %zu: %s\n",
                     page, fz_caught_message(ctx));
        return out;
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
        PageHit h{};
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

std::vector<Document::OutlineEntry> Document::outline() const {
    std::vector<OutlineEntry> result;
    if (!impl_->doc) return result;

    // Defense in depth: serialize with page_hits() worker calls.
    // See Impl::doc_mutex.
    std::lock_guard<std::mutex> lk(impl_->doc_mutex);
    fz_outline* root = nullptr;
    fz_try(impl_->ctx) {
        root = fz_load_outline(impl_->ctx, impl_->doc);
    }
    fz_catch(impl_->ctx) {
        std::fprintf(stderr, "litepdf: fz_load_outline failed: %s\n",
                     fz_caught_message(impl_->ctx));
        return result;
    }

    if (root) {
        flatten_outline(impl_->ctx, root, 0, result);
        fz_drop_outline(impl_->ctx, root);
    }
    return result;
}

} // namespace litepdf::core
