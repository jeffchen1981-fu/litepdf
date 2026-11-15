#pragma once

// #229: how a tab's DocumentView is built. Both MainWindow construction sites
// (the open thread and the password prompt) call this, so the per-format worker
// count and search open are decided in one place and a unit test reaches them.

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
    using litepdf::core::DocumentView;
    using litepdf::core::SearchOpen;
    // An FB2 lays the whole book out per copy and parses it at open (#229):
    // one render worker, and a search copy parsed only when the tab is first
    // searched.
    const bool fb2 = one_render_worker(doc.source_path());
    const std::size_t workers = fb2 ? 1 : litepdf::core::RenderEngine::kDefaultWorkers;
    const SearchOpen search_open = fb2 ? SearchOpen::AtFirstScan : SearchOpen::AtConstruction;
    return std::make_unique<DocumentView>(std::move(doc), dispatcher, password, workers,
                                          DocumentView::kDefaultL1Capacity,
                                          DocumentView::kDefaultL2Capacity,
                                          litepdf::core::kDefaultL1BudgetBytes, search_open);
}

}  // namespace litepdf::ui
