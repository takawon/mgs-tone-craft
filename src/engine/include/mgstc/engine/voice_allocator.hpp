#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace mgstc::engine {

struct VoiceAssignment {
    std::uint8_t channel{};
    std::optional<std::uint8_t> stolen_note;
};

class SequentialVoiceAllocator {
public:
    explicit SequentialVoiceAllocator(std::uint8_t channel_count);

    [[nodiscard]] VoiceAssignment noteOn(std::uint8_t note) noexcept;
    [[nodiscard]] std::optional<std::uint8_t>
    noteOff(std::uint8_t note) noexcept;
    [[nodiscard]] std::vector<std::uint8_t> allNotesOff();
    // Audio-thread safe: writes cleared voice indices into `channels_out`
    // (truncated if the span is short) and returns the number of voices
    // that were active.
    [[nodiscard]] std::size_t allNotesOff(
        std::span<std::uint8_t> channels_out) noexcept;

    void setChannelCount(std::uint8_t channel_count);
    // Limits note allocation without resizing. `channel_count` must already
    // fit in the vector allocated by the constructor or setChannelCount.
    void setActiveChannelCount(std::uint8_t channel_count) noexcept;
    void setPolyphonic(bool polyphonic);
    [[nodiscard]] bool polyphonic() const noexcept {
        return polyphonic_;
    }
    [[nodiscard]] std::size_t activeVoiceCount() const noexcept;

private:
    struct Voice {
        std::optional<std::uint8_t> note;
        std::uint64_t age{};
    };

    std::vector<Voice> voices_;
    std::uint8_t active_count_{1};
    std::uint64_t next_age_{1};
    bool polyphonic_{true};
};

}  // namespace mgstc::engine
