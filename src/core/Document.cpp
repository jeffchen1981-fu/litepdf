#include "core/Document.hpp"

#include <mupdf/fitz.h>

#include <stdexcept>

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

std::optional<Document::OpenError> Document::open(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return OpenError::FileNotFound;

    fz_try(impl_->ctx) {
        // NOTE: fz_open_document takes a UTF-8 char*. path.string() yields ACP on
        // Windows MSVC; may fail for non-ASCII paths. Will be revisited in a
        // later task on Unicode path robustness.
        impl_->doc = fz_open_document(impl_->ctx, path.string().c_str());
    }
    fz_catch(impl_->ctx) {
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

std::size_t Document::page_count() const { throw std::runtime_error("not implemented"); }
PageSize    Document::page_size(std::size_t) const { throw std::runtime_error("not implemented"); }
std::string Document::page_text(std::size_t) const { throw std::runtime_error("not implemented"); }

std::vector<Document::OutlineEntry> Document::outline() const { return {}; }

} // namespace litepdf::core
