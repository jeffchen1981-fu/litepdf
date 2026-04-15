#pragma once

// core::Document — UI-agnostic wrapper around MuPDF's fz_document.
// PIMPL: no MuPDF types leak through this header, so UI code (Phase 3+)
// can include Document.hpp without pulling fitz.h.

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace litepdf::core {

struct PageSize {
    float width_pt;
    float height_pt;
};

class Document {
public:
    enum class OpenError {
        FileNotFound,
        UnsupportedFormat,
        NeedsPassword,
        BadPassword,
        Corrupted,
        OutOfMemory,
        Other
    };

    Document();
    ~Document();

    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;
    Document(Document&&) noexcept;
    Document& operator=(Document&&) noexcept;

    // Returns std::nullopt on success, an OpenError otherwise.
    // If NeedsPassword is returned, call authenticate(password) then retry.
    [[nodiscard]] std::optional<OpenError> open(const std::filesystem::path& path);

    [[nodiscard]] bool is_open() const noexcept;
    void close() noexcept;

    // For encrypted documents. Returns true if password accepted.
    [[nodiscard]] bool authenticate(std::string_view password);

    // Metadata
    [[nodiscard]] std::size_t page_count() const;
    [[nodiscard]] PageSize page_size(std::size_t index) const;

    // Returns the plain text of page `index`, with newlines preserved.
    [[nodiscard]] std::string page_text(std::size_t index) const;

    // Outline: flat list of entries with indent depth, title, and target page.
    struct OutlineEntry {
        int depth;
        std::string title;
        std::size_t page_index;  // 0-based; kNoPage if not a page link
    };
    static constexpr std::size_t kNoPage = static_cast<std::size_t>(-1);
    [[nodiscard]] std::vector<OutlineEntry> outline() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace litepdf::core
