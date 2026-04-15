#include "core/Document.hpp"

#include <stdexcept>

namespace litepdf::core {

struct Document::Impl {
    // MuPDF state will be added in subsequent tasks.
    bool open = false;
};

Document::Document() : impl_(std::make_unique<Impl>()) {}
Document::~Document() = default;
Document::Document(Document&&) noexcept = default;
Document& Document::operator=(Document&&) noexcept = default;

std::optional<Document::OpenError> Document::open(const std::filesystem::path&) {
    return OpenError::Other;  // TDD: to be filled in Task 5
}

bool Document::is_open() const noexcept { return impl_->open; }
void Document::close() noexcept { impl_->open = false; }

bool Document::authenticate(std::string_view) { return false; }

std::size_t Document::page_count() const { throw std::runtime_error("not implemented"); }
PageSize    Document::page_size(std::size_t) const { throw std::runtime_error("not implemented"); }
std::string Document::page_text(std::size_t) const { throw std::runtime_error("not implemented"); }

std::vector<Document::OutlineEntry> Document::outline() const { return {}; }

} // namespace litepdf::core
