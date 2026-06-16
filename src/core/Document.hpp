#pragma once

// core::Document — UI-agnostic wrapper around MuPDF's fz_document.
// PIMPL: no MuPDF types leak through this header, so UI code (Phase 3+)
// can include Document.hpp without pulling fitz.h.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Forward-declare fz_context so this header can expose clone_context() without
// pulling in <mupdf/fitz.h>. The PIMPL stays MuPDF-free for public consumers.
// In C++ a bare struct-tag forward declaration is compatible with MuPDF's
// `typedef struct fz_context fz_context;` — the struct tag and typedef name
// can coexist in the same TU without conflict. We use this simpler form so
// Document.hpp stays free of MuPDF headers (PIMPL discipline).
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
    // The internal NUL-terminated copy of the password is wiped before
    // this method returns (Phase 8 D3). Callers are still responsible
    // for zeroing the buffer they own; this method only guarantees the
    // copy MuPDF sees is not retained in heap memory.
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

    // ------------------------------------------------------------------
    // Search (Phase 6)
    // ------------------------------------------------------------------
    // Stateless per-page search. Callers (core::SearchSession, Task 5)
    // drive the cursor/cache and epoch-based cancellation on top of this.
    //
    // Search options. On MuPDF 1.27.2 all three are honored via the
    // incremental fz_search matcher; see SearchQuery.hpp for the
    // flag->needle/options translation.
    struct SearchFlags {
        bool match_case = false;
        bool whole_word = false;
        bool regex      = false;
    };

    // One search hit on a page. Coordinates are in PDF user space (points),
    // same convention as page_size(). The quad corners are stored as plain
    // floats to keep this header MuPDF-free.
    struct PageHit {
        float ul_x = 0, ul_y = 0;  // upper-left
        float ur_x = 0, ur_y = 0;  // upper-right
        float ll_x = 0, ll_y = 0;  // lower-left
        float lr_x = 0, lr_y = 0;  // lower-right
        // Snippet string for the cross-tab results panel. v1 returns just
        // the needle itself; a centered 30-char snippet is a Phase 6.2
        // upgrade once stext-backed matching lands.
        std::string snippet_utf8;
    };

    // Returns all hits for `needle_utf8` on `page`. Empty needle → empty
    // result (matches MuPDF's own behavior in fz_search_stext_page).
    //
    // @param page        0-based page index.
    // @param needle_utf8 UTF-8 query. Empty returns {}.
    // @param flags       Case sensitivity, whole-word, and regex. ALL honored
    //                    on MuPDF 1.27.2 via the incremental fz_search matcher
    //                    (see SearchQuery.hpp for the flag->needle/options
    //                    translation). An invalid regex throws inside MuPDF,
    //                    is caught, and yields an empty result — callers
    //                    pre-validate via query_compiles().
    // @param abort_flag  Honored on MuPDF 1.27.2: checked at the top of every
    //                    iteration of the incremental search loop, giving
    //                    prompt mid-page cancellation. A non-zero value before
    //                    or during the search yields whatever hits were already
    //                    collected (empty if cancelled before the first match).
    //                    Cross-page cancellation remains SearchSession's
    //                    responsibility (epoch bump).
    //
    // Thread-safety: SAFE to call concurrently from multiple threads on
    // the same Document instance. An internal std::mutex serializes every
    // method that touches impl_->ctx (page_hits, page_count, page_text,
    // page_size, outline) — MuPDF's fz_try/fz_catch uses a per-ctx error
    // stack that is not thread-safe, so this serialization is mandatory.
    // Consequence: per-Document search parallelism is bounded to 1;
    // cross-tab searches still parallelize across distinct Documents.
    // SearchSession workers (Phase 6 Task 5) rely on this guarantee.
    [[nodiscard]] std::vector<PageHit> page_hits(
        std::size_t page,
        std::string_view needle_utf8,
        SearchFlags flags,
        std::atomic<int>* abort_flag = nullptr) const;

    // One-shot validity check: compiles the regex/whole-word needle under
    // `flags`. Returns false iff the pattern fails to compile. Empty needle or
    // a pure-literal query → true (no compile needed). Returns true when no
    // document is open (can't validate; the UI only calls this with an active
    // doc). Cheap: no page is loaded/searched.
    [[nodiscard]] bool query_compiles(std::string_view needle_utf8, SearchFlags flags) const;

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

    // Returns the path that was last successfully passed to open(). If the
    // document is not open (or was never opened), returns a reference to an
    // empty path. RenderEngine workers use this to re-open the file on their
    // own fz_context (MuPDF forbids sharing fz_document across contexts).
    [[nodiscard]] const std::filesystem::path& source_path() const noexcept;

    // ------------------------------------------------------------------
    // Synchronous render escape hatch (Phase 8.5)
    // ------------------------------------------------------------------
    // Renders a page to a BGRA pixmap on this Document's internal
    // fz_context, invokes the callback with the pixel data, then drops
    // the pixmap. Used by PrintJob (UI-thread, synchronous, modal) so
    // that authentication state on encrypted PDFs and layout state on
    // reflowable formats (ePub, CBZ) are preserved -- a cloned context
    // would lose both.
    //
    // Caller MUST consume `bgra` only inside the callback; the pointer
    // is invalidated when the callback returns. Pixel order is BGRA
    // (4 bytes per pixel) top-down (row 0 = top of page).
    //
    // scale_x_px_per_pt, scale_y_px_per_pt: feed directly into fz_scale().
    // rotate_90: if true, the matrix prepends a 90 degree pre-rotation
    // (auto-rotate logic, computed by the caller).
    //
    // Returns false if page_idx is out of range, document is not open,
    // or rendering fails. On false, callback is not invoked.
    //
    // Thread-safety: SAFE concurrently with other Document methods on
    // the same instance -- internal doc_mutex serializes all callers.
    // Holds the mutex for the entire render duration (~50-300ms at
    // print DPI). RenderEngine worker threads hitting this Document
    // will block; acceptable because print is a rare modal operation.
    [[nodiscard]] bool with_rendered_page(
        std::size_t page_idx,
        float scale_x_px_per_pt,
        float scale_y_px_per_pt,
        bool  rotate_90,
        const std::function<void(int width_px,
                                 int height_px,
                                 const std::uint8_t* bgra_top_down)>& callback) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace litepdf::core
