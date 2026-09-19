#include "mgstc/engine/voice_allocator.hpp"

#include <algorithm>
#include <array>

namespace mgstc::engine {

SequentialVoiceAllocator::SequentialVoiceAllocator(
    std::uint8_t channel_count)
    : voices_(std::max<std::uint8_t>(channel_count, 1)) {}

VoiceAssignment SequentialVoiceAllocator::noteOn(std::uint8_t note) noexcept {
    for (std::size_t index = 0; index < voices_.size(); ++index) {
        if (voices_[index].note == note) {
            voices_[index].age = next_age_++;
            return {static_cast<std::uint8_t>(index), note};
        }
    }

    std::size_t selected = 0;
    if (polyphonic_) {
        const auto free = std::find_if(
            voices_.begin(), voices_.end(),
            [](const Voice& voice) { return !voice.note; });
        if (free != voices_.end()) {
            selected = static_cast<std::size_t>(
                std::distance(voices_.begin(), free));
        } else {
            selected = static_cast<std::size_t>(std::distance(
                voices_.begin(),
                std::min_element(
                    voices_.begin(), voices_.end(),
                    [](const Voice& left, const Voice& right) {
                        return left.age < right.age;
                    })));
        }
    }

    const auto stolen = voices_[selected].note;
    voices_[selected] = {.note = note, .age = next_age_++};
    return {
        .channel = static_cast<std::uint8_t>(selected),
        .stolen_note = stolen,
    };
}

std::optional<std::uint8_t> SequentialVoiceAllocator::noteOff(
    std::uint8_t note) noexcept {
    for (std::size_t index = 0; index < voices_.size(); ++index) {
        if (voices_[index].note != note) {
            continue;
        }
        voices_[index] = {};
        return static_cast<std::uint8_t>(index);
    }
    return std::nullopt;
}

std::size_t SequentialVoiceAllocator::allNotesOff(
    std::span<std::uint8_t> channels_out) noexcept {
    std::size_t written = 0;
    for (std::size_t index = 0; index < voices_.size(); ++index) {
        if (!voices_[index].note) {
            continue;
        }
        if (written < channels_out.size()) {
            channels_out[written] = static_cast<std::uint8_t>(index);
        }
        ++written;
        voices_[index] = {};
    }
    return written;
}

std::vector<std::uint8_t> SequentialVoiceAllocator::allNotesOff() {
    std::array<std::uint8_t, 16> channels{};
    const auto count = allNotesOff(channels);
    return {
        channels.begin(),
        channels.begin()
            + static_cast<std::ptrdiff_t>(
                std::min(count, channels.size())),
    };
}

void SequentialVoiceAllocator::setChannelCount(
    std::uint8_t channel_count) {
    std::array<std::uint8_t, 16> ignored{};
    static_cast<void>(allNotesOff(ignored));
    voices_.assign(std::max<std::uint8_t>(channel_count, 1), {});
}

void SequentialVoiceAllocator::setPolyphonic(bool polyphonic) {
    std::array<std::uint8_t, 16> ignored{};
    static_cast<void>(allNotesOff(ignored));
    polyphonic_ = polyphonic;
}

std::size_t SequentialVoiceAllocator::activeVoiceCount() const noexcept {
    return static_cast<std::size_t>(std::count_if(
        voices_.begin(), voices_.end(),
        [](const Voice& voice) { return voice.note.has_value(); }));
}

}  // namespace mgstc::engine
