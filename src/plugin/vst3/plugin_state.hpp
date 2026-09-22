// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "mgstc/engine/composite_timbre.hpp"

namespace mgstc::plugin {

// Plugin State schema is independent of tone-library SQLite user_version
// and of CompositeTimbre::kFormatVersion / kCompositeSoundPayloadVersion.
// v1 wraps the existing sound-only payload so DAW restore does not need
// tone_library.sqlite. Unknown schema versions are not read.
inline constexpr std::uint32_t kPluginStateSchemaVersion = 1;
inline constexpr char kPluginStateMagic[8] = {
    'M', 'G', 'S', 'T', 'C', 'P', 'S', '1'};

inline constexpr std::size_t kMaxPluginStateBytes = 4U * 1024U * 1024U;
inline constexpr std::size_t kMaxPluginSoundPayloadBytes = 2U * 1024U * 1024U;
inline constexpr std::size_t kMaxPluginStateNameBytes = 1024;
inline constexpr std::size_t kMaxPluginStateMemoBytes = 8192;
inline constexpr std::size_t kMaxPluginStateTagCount = 64;
inline constexpr std::size_t kMaxPluginStateTagBytes = 256;
inline constexpr std::uint32_t kMaxPluginStateLayers = 1024;

enum class PluginStateStatus : std::uint8_t {
    Ok = 0,
    Empty,
    Truncated,
    BadMagic,
    UnsupportedVersion,
    BrokenPayload,
    InvalidContent,
};

// Display-only. Not required to reproduce the sound.
struct PluginEditorState {
    bool has_selected_layer{false};
    std::uint32_t selected_layer{0};
    std::uint32_t editor_tab{0};

    friend bool operator==(
        const PluginEditorState&,
        const PluginEditorState&) = default;
};

// Sound body is the snapshot. name / memo / tags / favorite and library_id
// are metadata: library_id is never used to load tone_library.sqlite.
struct PluginStateDocument {
    PluginEditorState editor{};
    std::uint64_t library_id{0};
    mgstc::engine::CompositeTimbre sound{};
};

struct PluginStateParseResult {
    PluginStateStatus status{PluginStateStatus::Empty};
    PluginStateDocument document{};
};

[[nodiscard]] bool validatePluginSoundSnapshot(
    const mgstc::engine::CompositeTimbre& timbre);

[[nodiscard]] std::vector<std::uint8_t> serializePluginState(
    const PluginStateDocument& document);

// On any status other than Ok, `document` is empty and must not be applied.
[[nodiscard]] PluginStateParseResult parsePluginState(
    const void* data,
    std::size_t size);

}  // namespace mgstc::plugin
