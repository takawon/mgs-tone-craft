// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "mgstc/engine/composite_timbre.hpp"
#include "mgstc/engine/timbre_library.hpp"

namespace mgstc::engine {
struct OpllScopeFrame;
}

class SharedAudioHost;

namespace mgstc::app {

struct EnvelopeTimbreCatalogItem {
    std::uint64_t library_id{};
    juce::String name;
    std::uint32_t revision{1};
    bool favorite{};
    std::optional<std::uint8_t> assigned_number;
    std::vector<std::string> tags;
    juce::String memo;
    std::int64_t last_used_unix_seconds{};
    std::int64_t updated_unix_seconds{};
};

struct EnvelopeTimbreChoice {
    engine::TimbrePick pick{engine::TimbrePick::Library};
    std::uint64_t library_id{};
    std::uint8_t rom_number{};
};

[[nodiscard]] inline juce::String envelopeTimbreCatalogLabel(
    const EnvelopeTimbreCatalogItem& item) {
    auto label = item.name;
    if (item.favorite) {
        label = juce::String::fromUTF8("★ ") + label;
    }
    if (item.revision != 0) {
        label += " (r" + juce::String(static_cast<int>(item.revision)) + ")";
    }
    if (item.assigned_number) {
        label += "  #"
            + juce::String(static_cast<int>(*item.assigned_number));
    }
    return label;
}

struct LayerBaseTimbreAssign {
    std::optional<std::uint64_t> library_id;
    std::optional<std::uint8_t> opll_rom;
};

class CompositeTimeline final : public juce::Component {
public:
    using EditCallback = std::function<void(
        const engine::CompositeTimbre&, bool, bool)>;
    using StatusCallback = std::function<void(const juce::String&)>;
    using CatalogCallback = std::function<
        std::vector<EnvelopeTimbreCatalogItem>(engine::TimbreSource)>;
    using OpenTimbreCallback = std::function<void(
        const juce::String&, std::optional<std::uint64_t>)>;
    using AssignTimbreCallback = std::function<void(
        std::size_t, LayerBaseTimbreAssign)>;
    using TimbreNameCallback = std::function<juce::String(std::uint64_t)>;
    using TimbreLibraryCallback =
        std::function<const engine::TimbreLibrary*()>;
    using ManageTagsCallback = std::function<void()>;
    using MutateTimbreNameCallback =
        std::function<bool(std::uint64_t, juce::String)>;
    using MutateTimbreTagsCallback =
        std::function<bool(std::uint64_t, std::vector<std::string>)>;
    using MutateTimbreMemoCallback =
        std::function<bool(std::uint64_t, juce::String)>;
    using LfoSessionCallback =
        std::function<void(std::optional<std::size_t>, bool, bool)>;
    using LfoPollKeysCallback = std::function<void()>;

    CompositeTimeline();
    ~CompositeTimeline() override;

    CompositeTimeline(const CompositeTimeline&) = delete;
    CompositeTimeline& operator=(const CompositeTimeline&) = delete;

    void setEditCallback(EditCallback callback);
    void setStatusCallback(StatusCallback callback);
    void setTimbreCatalogCallback(CatalogCallback callback);
    void setOpenTimbreCallback(OpenTimbreCallback callback);
    void setAssignTimbreCallback(AssignTimbreCallback callback);
    void setTimbreNameCallback(TimbreNameCallback callback);
    void setTimbreLibraryCallback(TimbreLibraryCallback callback);
    void setManageTagsCallback(ManageTagsCallback callback);
    void setMutateTimbreNameCallback(MutateTimbreNameCallback callback);
    void setMutateTimbreTagsCallback(MutateTimbreTagsCallback callback);
    void setMutateTimbreMemoCallback(MutateTimbreMemoCallback callback);
    void setLfoSessionCallback(LfoSessionCallback callback);
    void setLfoPollKeysCallback(LfoPollKeysCallback callback);
    void setAudioService(SharedAudioHost* audio_service) noexcept;
    void setPlaybackTempo(int tempo_bpm);
    void setTimbre(
        const engine::CompositeTimbre& timbre,
        bool reset_scroll_extent = false);
    void refreshUiScaleFonts();
    [[nodiscard]] bool appendScopeFrame(
        const engine::OpllScopeFrame& frame,
        std::uint8_t midi_note);

    void resized() override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mgstc::app
