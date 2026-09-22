// SPDX-License-Identifier: AGPL-3.0-only
// Compiles the shared editor without SharedAudioService, SharedMidiInputService,
// or RealtimeEngineHost. Standalone wires those behind StandaloneEditorContext.

#include "mgstc_editor_view.hpp"

namespace mgstc::app {
int mgstcSharedEditorTranslationUnit() {
    return static_cast<int>(sizeof(CompositeEditorComponent));
}
}  // namespace mgstc::app
