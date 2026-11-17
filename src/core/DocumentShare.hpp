#pragma once

// core::DocumentShare -- whether a render engine or a search session opens the
// tab's file again or borrows the tab Document's own document (#229).
//
//   Own     opens the file again by name, on its own context: one copy per
//           render worker and one per search session. Pages interpret in
//           parallel, with no lock between the copies. Every format but FB2.
//   Shared  opens nothing. Every use of the Document's document goes through
//           detail::SharedDocumentAccess, under the Document's gate (Render
//           for the worker, Scan for the search), so the tab keeps one parsed
//           and laid-out copy of the book. An FB2 tab with no password
//           (ui::make_document_view). Spec §3.2-3.5.

namespace litepdf::core {

enum class DocumentShare { Own, Shared };

}  // namespace litepdf::core
