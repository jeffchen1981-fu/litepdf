#include "core/Document.hpp"

#include <mupdf/fitz.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace litepdf::core {

struct Document::Impl {
    fz_context* ctx = nullptr;
    fz_document* doc = nullptr;
    bool needs_password = false;
    bool authenticated = false;

    Impl() {
        ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
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
    std::string ext = path.extension().string();
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
        // NOTE: fz_open_document takes a UTF-8 char*. path.string() yields ACP on
        // Windows MSVC; may fail for non-ASCII paths. Will be revisited in a
        // later task on Unicode path robustness.
        impl_->doc = fz_open_document(impl_->ctx, path.string().c_str());
    }
    fz_catch(impl_->ctx) {
        // File was recognized as a PDF but failed to parse → treat as corrupted.
        return OpenError::Corrupted;
    }

    if (!impl_->doc) return OpenError::Other;

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
}

bool Document::authenticate(std::string_view password) {
    if (!impl_->doc) return false;
    // fz_authenticate_password expects NUL-terminated; copy to std::string to ensure it.
    std::string pw(password);
    int ok = 0;
    fz_try(impl_->ctx) {
        ok = fz_authenticate_password(impl_->ctx, impl_->doc, pw.c_str());
    }
    fz_catch(impl_->ctx) {
        return false;
    }
    if (ok != 0) {
        impl_->authenticated = true;
        return true;
    }
    return false;
}

std::size_t Document::page_count() const {
    if (!impl_->doc) throw std::logic_error("page_count called on unopened document");
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

std::vector<Document::OutlineEntry> Document::outline() const {
    std::vector<OutlineEntry> result;
    if (!impl_->doc) return result;

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
