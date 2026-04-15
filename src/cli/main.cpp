// litepdf-cli — console demo + benchmark harness.
// Opens a document and prints metadata, first-page size, a text snippet,
// and the outline. Used manually during development and by Phase 11
// benchmarks. Not user-facing — the GUI is litepdf.exe.

#include "core/Document.hpp"

#include <cstdio>
#include <string>

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: %s <file>\n", argv[0]);
        return 2;
    }

    litepdf::core::Document doc;
    auto err = doc.open(argv[1]);
    if (err) {
        std::fprintf(stderr, "Open error: %d\n", static_cast<int>(*err));
        return 1;
    }

    const std::size_t n = doc.page_count();
    std::printf("File: %s\n", argv[1]);
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
