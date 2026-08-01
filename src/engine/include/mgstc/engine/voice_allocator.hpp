#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace mgstc::engine {

struct VoiceAssignment {
    std::uint8_t channel{};
    std::optional<std::uint8_t> stolen_note;
};

class SequentialVoiceAllocator {
public:
    explicit SequentialVoiceAllocator(std::uint8_t channel_count);

    [[nodiscard]] VoiceAssignment noteOn(std::uint8_t note);
    [[nodiscard]] std::optional<std::uint8_t>
    noteOff(std::uint8_t note) noexcept;
    [[nodiscard]] std::vector<std::uint8_t> allNotesOff();

    void setChannelCount(std::uint8_t channel_count);
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
    std::uint64_t next_age_{1};
    bool polyphonic_{true};
};

}  // namespace mgstc::engine
