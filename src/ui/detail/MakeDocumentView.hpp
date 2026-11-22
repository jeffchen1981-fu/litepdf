#pragma once

// #229: how a tab's DocumentView is built. Both MainWindow construction sites
// (the open thread and the password prompt) call this, so the per-format worker
// count and document sharing are decided in one place and a unit test reaches
// them.

#include <memory>
#include <optional>
#include <string_view>

#include "core/Document.hpp"
#include "core/DocumentView.hpp"
#include "core/RenderEngine.hpp"
#include "ui/detail/EffectiveLayout.hpp"

namespace litepdf::app {
class ISearchDispatcher;
}

namespace litepdf::ui {

// `password` is the one `doc` was authenticated with, or none (DocumentView).
// Throws whatever the DocumentView constructor throws.
inline std::unique_ptr<litepdf::core::DocumentView> make_document_view(
    litepdf::core::Document doc, litepdf::app::ISearchDispatcher& dispatcher,
    std::optional<std::string_view> password = std::nullopt) {
    using litepdf::core::DocumentShare;
    using litepdf::core::DocumentView;
    // An FB2 parses the whole book at open and lays it out per copy (#229), so
    // the tab keeps one copy: the render worker and the search session borrow
    // the Document's own document under its gate. Two predicates, on purpose:
    //   - Sharing follows what MuPDF opened (Document::is_fb2), not the name.
    //     A PDF named .fb2 opens as a PDF, whose page interpretation is a long
    //     hold, so it keeps documents of its own; FB2 content named .svg (no
    //     magic check, and MuPDF scores content above the extension) is FB2
    //     and is shared. With a password nothing is shared, so the password is
    //     never kept past the constructors (Phase 8 D3); an FB2 never has one.
    //   - One render worker for a shared view (a second would mostly wait on
    //     the gate: only rasterising runs outside it), and for anything named
    //     .fb2 (#235: one_render_worker, which decides only the worker count).
    const bool shared = doc.is_fb2() && !password;
    const std::size_t workers = shared || one_render_worker(doc.source_path())
                                    ? 1
                                    : litepdf::core::RenderEngine::kDefaultWorkers;
    return std::make_unique<DocumentView>(std::move(doc), dispatcher, password, workers,
                                          DocumentView::kDefaultL1Capacity,
                                          DocumentView::kDefaultL2Capacity,
                                          litepdf::core::kDefaultL1BudgetBytes,
                                          shared ? DocumentShare::Shared : DocumentShare::Own);
}

}  // namespace litepdf::ui
