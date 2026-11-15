#pragma once

// #229: how a tab's DocumentView is built. Both MainWindow construction sites
// (the open thread and the password prompt) call this, so the per-format worker
// count is decided in one place and a unit test reaches it.

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
    const std::size_t workers = one_render_worker(doc.source_path())
                                    ? 1
                                    : litepdf::core::RenderEngine::kDefaultWorkers;
    return std::make_unique<litepdf::core::DocumentView>(std::move(doc), dispatcher, password,
                                                         workers);
}

}  // namespace litepdf::ui
