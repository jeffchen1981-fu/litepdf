#include "core/Document.hpp"

#include <mupdf/fitz.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <string>

namespace litepdf::core {

struct Document::Impl {
    fz_context* ctx = nullptr;
    fz_document* doc = nullptr;

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
Document::Document(Document&&) noexcept = default;
Document& Document::operator=(Document&&) noexcept = default;

namespace {

// LitePDF is scoped to PDF files only. Recognize format by extension +
// magic bytes before handing off to MuPDF (which would otherwise happily
// open images and other non-PDF formats it supports).
bool looks_like_pdf(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext != ".pdf") return false;

    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    char header[5] = {0};
    f.read(header, 4);
    return std::string(header, 4) == "%PDF";
}

} // namespace

std::optional<Document::OpenError> Document::open(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return OpenError::FileNotFound;

    if (!looks_like_pdf(path)) return OpenError::UnsupportedFormat;

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
    return std::nullopt;
}

bool Document::is_open() const noexcept { return impl_->doc != nullptr; }

void Document::close() noexcept {
    if (impl_->doc) {
        fz_drop_document(impl_->ctx, impl_->doc);
        impl_->doc = nullptr;
    }
}

bool Document::authenticate(std::string_view) { return false; }

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
std::string Document::page_text(std::size_t) const { throw std::runtime_error("not implemented"); }

std::vector<Document::OutlineEntry> Document::outline() const { return {}; }

} // namespace litepdf::core
