#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <vector>
#include <windows.h>

#include "core/SearchSession.hpp"

// Forward-decl so the header stays COM-free. ComPtr in .cpp only.
struct ID2D1Factory;
struct ID2D1HwndRenderTarget;
struct ID2D1Bitmap;
struct ID2D1SolidColorBrush;

// MuPDF forward decls — header stays mupdf-free; full types only in .cpp.
struct fz_context;
struct fz_pixmap;

namespace litepdf::core { class DocumentView; }

namespace litepdf::ui {

// Posted by render-done callback. WPARAM = fz_pixmap* (kept by worker),
// LPARAM = fz_context* escrow clone (also kept by worker). Both are null
// on cancel/fail. Canvas drops the pixmap through escrow, then drops
// escrow — staying on the pixmap's own MuPDF root even if the producing
// DocumentView has been swapped or destroyed.
// Must match the reservation in MainWindow.cpp (WM_USER + 3).
inline constexpr UINT WM_USER_RENDER_DONE = WM_USER + 3;

class PdfCanvas {
public:
    PdfCanvas(HINSTANCE hInstance, HWND parent);
    ~PdfCanvas();

    PdfCanvas(const PdfCanvas&)            = delete;
    PdfCanvas& operator=(const PdfCanvas&) = delete;

    HWND hwnd() const { return hwnd_; }

    // MainWindow owns the view; canvas holds a non-owning raw pointer
    // used to obtain fz_context* for fz_drop_pixmap on render-done.
    // Pass nullptr to clear.
    void set_view(litepdf::core::DocumentView* view);

    // Get/set the canvas pan offset (DIPs from the centered/fit origin).
    // Used by MainWindow to snapshot/restore per-tab scroll on tab switch.
    // Both are no-ops if called before the impl is ready.
    struct Pan { float x; float y; };
    Pan  pan() const;
    void set_pan(float x, float y);

    // Post WM_USER_RENDER_DONE to `target` for the (pixmap, ctx) pair,
    // where `ctx` is a clone-escrow made from `worker_ctx` so the UI
    // thread can drop the pixmap with the correct MuPDF root — even if
    // the producing DocumentView is torn down before the message lands.
    //
    // Called from the worker thread inside the render callback. Takes
    // an extra ref on the pixmap via fz_keep_pixmap, clones
    // worker_ctx, and on any failure (clone OOM, post FALSE) cleans up
    // both the kept pixmap and the escrow ctx. Returns true iff the
    // message was successfully posted.
    //
    // Callers: MainWindow::kick_render, resubmit_current_page,
    // on_key_down's page-change path, WM_MOUSEWHEEL zoom path.
    static bool post_render_done(HWND target,
                                 fz_pixmap* pix,
                                 fz_context* worker_ctx);

    // When true, on first real-bitmap paint the canvas calls
    // ColdStartTimer::emit_if_complete(true) so the line is mirrored to stderr.
    void set_log_timings(bool on) { log_timings_ = on; }

    // --- Phase 6 Task 9: search hit overlay ---

    // Source for overlay hits. PdfCanvas calls this each paint to fetch
    // the hits for the currently visible page. Non-owning; caller must
    // ensure the lambda stays valid while set (or call set_hits_source(nullptr)
    // before the lambda's captures become invalid).
    using HitsFn = std::function<std::vector<litepdf::core::SearchSession::Hit>(std::size_t page)>;
    void set_hits_source(HitsFn fn);

    // Current hit (if any). Drawn in orange (+ outline); others drawn in
    // yellow. Pass std::nullopt to clear current highlight.
    void set_current_hit(std::optional<litepdf::core::SearchSession::Hit> h);

    // Scroll / page-change such that `h`'s quad is visible with a 24 DIP
    // margin. If already visible, no scroll — only the invalidate. If
    // target page differs from current, page is switched via
    // DocumentView::set_current_page; caller (MainWindow) is responsible
    // for the subsequent kick_render. This method only handles pan and
    // invalidation.
    void scroll_into_view(const litepdf::core::SearchSession::Hit& h);

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    static void register_class_once(HINSTANCE hInstance);
    LRESULT handle_message(HWND, UINT, WPARAM, LPARAM);

    void create_render_target();
    void discard_render_target();
    void resubmit_current_page();
    void on_paint();
    void on_size(int width, int height);
    LRESULT on_key_down(WPARAM key);

    HWND hwnd_ = nullptr;
    bool log_timings_ = false;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace litepdf::ui
