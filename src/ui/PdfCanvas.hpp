#pragma once
#include <memory>
#include <windows.h>

// Forward-decl so the header stays COM-free. ComPtr in .cpp only.
struct ID2D1Factory;
struct ID2D1HwndRenderTarget;
struct ID2D1Bitmap;

namespace litepdf::ui {

class PdfCanvas {
public:
    PdfCanvas(HINSTANCE hInstance, HWND parent);
    ~PdfCanvas();

    PdfCanvas(const PdfCanvas&)            = delete;
    PdfCanvas& operator=(const PdfCanvas&) = delete;

    HWND hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    static void register_class_once(HINSTANCE hInstance);
    LRESULT handle_message(HWND, UINT, WPARAM, LPARAM);

    void create_render_target();
    void discard_render_target();
    void on_paint();
    void on_size(int width, int height);

    HWND hwnd_ = nullptr;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace litepdf::ui
