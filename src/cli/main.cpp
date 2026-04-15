// litepdf-cli — console demo + benchmark harness.
// Opens a document and prints metadata, first-page size, a text snippet,
// and the outline. Used manually during development and by Phase 11
// benchmarks. Not user-facing — the GUI is litepdf.exe.
//
// Also supports `--render N` to render page N via RenderEngine and emit
// a binary PPM (P6) image to stdout. Used for manual smoke-checking the
// render path during Phase 2+ development.

#include "core/Document.hpp"
#include "core/RenderEngine.hpp"

#include <mupdf/fitz.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <file> [--render N]\n", argv[0]);
        return 2;
    }

    const char* path = argv[1];
    int render_page = -1;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--render") == 0 && i + 1 < argc) {
            render_page = std::atoi(argv[++i]);
        }
    }

    litepdf::core::Document doc;
    auto err = doc.open(path);
    if (err) {
        std::fprintf(stderr, "Open error: %d\n", static_cast<int>(*err));
        return 1;
    }

    if (render_page >= 0) {
#ifdef _WIN32
        // Ensure stdout emits raw bytes (no CRLF translation) on Windows.
        std::fflush(stdout);
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        litepdf::core::RenderEngine engine(doc, 1);

        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        int exit_code = 0;

        engine.submit({
            render_page,
            0,
            1.0f,
            [&](fz_pixmap* pix, fz_context* ctx) {
                if (!pix) {
                    std::lock_guard<std::mutex> g(m);
                    exit_code = 2;
                    done = true;
                    cv.notify_all();
                    return;
                }
                int w = fz_pixmap_width(ctx, pix);
                int h = fz_pixmap_height(ctx, pix);
                int stride = fz_pixmap_stride(ctx, pix);
                unsigned char* samples = fz_pixmap_samples(ctx, pix);

                // Task 0.3 (Phase 3): RenderEngine now produces BGRA
                // (fz_device_bgr + alpha=1) so Direct2D can upload the
                // buffer without a per-pixel channel swap. PPM's P6 format
                // is RGB with no alpha, so we swap BGRA → RGB at write time.
                // BGRA memory layout: byte[0]=B, byte[1]=G, byte[2]=R, byte[3]=A.
                std::fprintf(stdout, "P6\n%d %d\n255\n", w, h);
                for (int y = 0; y < h; ++y) {
                    const unsigned char* row =
                        samples + static_cast<std::size_t>(y) * stride;
                    for (int x = 0; x < w; ++x) {
                        const unsigned char* px = row + static_cast<std::size_t>(x) * 4;
                        const unsigned char rgb[3] = {
                            px[2],  // R
                            px[1],  // G
                            px[0],  // B
                        };
                        std::fwrite(rgb, 1, 3, stdout);
                    }
                }
                std::fflush(stdout);
                fz_drop_pixmap(ctx, pix);

                std::lock_guard<std::mutex> g(m);
                done = true;
                cv.notify_all();
            }
        });

        {
            std::unique_lock<std::mutex> lk(m);
            cv.wait_for(lk, std::chrono::seconds(10), [&] { return done; });
            if (!done) {
                std::fprintf(stderr, "Render timed out\n");
                exit_code = 3;
            }
        }
        // engine destructor joins workers.
        return exit_code;
    }

    const std::size_t n = doc.page_count();
    std::printf("File: %s\n", path);
    std::printf("Pages: %zu\n", n);

    if (n > 0) {
        auto size = doc.page_size(0);
        std::printf("First page: %.1f x %.1f pt\n", size.width_pt, size.height_pt);

        std::string text = doc.page_text(0);
        if (text.size() > 200) text.resize(200);
        std::printf("First-page text snippet:\n%s\n", text.c_str());
    }

    const auto outline = doc.outline();
    if (!outline.empty()) {
        std::printf("Outline (%zu entries):\n", outline.size());
        for (const auto& e : outline) {
            std::size_t page_display = (e.page_index == litepdf::core::Document::kNoPage)
                                           ? 0
                                           : e.page_index + 1;
            std::printf("  %*s- %s (page %zu)\n",
                        e.depth * 2, "", e.title.c_str(), page_display);
        }
    }

    return 0;
}
