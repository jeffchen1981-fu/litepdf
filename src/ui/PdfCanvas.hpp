#pragma once
#include <memory>
#include <windows.h>

// Forward-decl so the header stays COM-free. ComPtr in .cpp only.
struct ID2D1Factory;
struct ID2D1HwndRenderTarget;
struct ID2D1Bitmap;

namespace litepdf::core { class DocumentView; }

namespace litepdf::ui {

// Posted by render-done callback. WPARAM = fz_pixmap* (kept by worker),
// LPARAM = 0. Canvas owns the drop via view_->ui_ctx().
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

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    static void register_class_once(HINSTANCE hInstance);
    LRESULT handle_message(HWND, UINT, WPARAM, LPARAM);

    void create_render_target();
    void discard_render_target();
    void on_paint();
    void on_size(int width, int height);
    LRESULT on_key_down(WPARAM key);

    HWND hwnd_ = nullptr;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace litepdf::ui
