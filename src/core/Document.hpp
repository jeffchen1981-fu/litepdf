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

// Forward-declare fz_context so this header can expose clone_context() without
// pulling in <mupdf/fitz.h>. The PIMPL stays MuPDF-free for public consumers.
// Must match MuPDF's declaration exactly: `typedef struct fz_context fz_context;`
struct fz_context;

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

    // Returns a freshly cloned fz_context* suitable for use on another
    // thread. MuPDF contexts are not thread-safe; RenderEngine's worker
    // threads each get their own clone via this method.
    //
    // Returns nullptr if:
    //  - Document is not yet open (no source fz_document to associate), OR
    //  - fz_clone_context itself fails (e.g., out-of-memory).
    // Caller MUST tolerate nullptr and treat it as a recoverable error
    // (not just "not opened" — OOM is possible too).
    //
    // Caller owns the returned pointer and MUST release it with
    // fz_drop_context() when done.
    //
    // Thread-safety: safe to call from multiple threads simultaneously.
    // fz_clone_context internally acquires FZ_LOCK_ALLOC when bumping
    // shared-context refcounts, and the lock table installed by the
    // Impl ctor uses std::mutex under the hood. The Document (and its
    // underlying fz_context) must not be destroyed while any call is
    // in progress.
    [[nodiscard]] fz_context* clone_context() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace litepdf::core
