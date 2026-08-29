#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace mgstc::engine {

using SccWaveform = std::array<std::int8_t, 32>;

enum class SccWavePreset {
    Sine,
    Square,
    Triangle,
    Saw,
    Pulse25,
    Pulse12_5,
};

enum class SccHarmonic {
    One,
    OneAndHalf,
    Two,
    Three,
    Four,
};

enum class SccApplyRange {
    All,
    LeftHalf,
    RightHalf,
};

struct SccMergeOptions {
    double amount{0.5};
    bool auto_phase{true};
    bool allow_polarity_inversion{true};
    bool preserve_volume{true};

    friend bool operator==(
        const SccMergeOptions&,
        const SccMergeOptions&) = default;
};

struct SccMergeResult {
    SccWaveform waveform{};
    std::size_t circular_shift{};
    bool polarity_inverted{};
};

inline constexpr std::size_t kSccRandomPresetMaximumStages = 4;

struct SccRandomPresetStage {
    SccWavePreset preset{SccWavePreset::Sine};
    SccHarmonic harmonic{SccHarmonic::One};
    bool flip_horizontal{};
    bool flip_vertical{};
    SccMergeOptions merge;

    friend bool operator==(
        const SccRandomPresetStage&,
        const SccRandomPresetStage&) = default;
};

struct SccRandomPresetRecipe {
    std::uint8_t stage_count{2};
    std::array<
        SccRandomPresetStage,
        kSccRandomPresetMaximumStages> stages{};
    SccApplyRange apply_range{SccApplyRange::All};

    friend bool operator==(
        const SccRandomPresetRecipe&,
        const SccRandomPresetRecipe&) = default;
};

[[nodiscard]] SccWaveform generateSccPreset(
    SccWavePreset preset,
    SccHarmonic harmonic) noexcept;

[[nodiscard]] SccRandomPresetRecipe makeSccRandomPresetRecipe(
    std::uint32_t seed) noexcept;

[[nodiscard]] SccWaveform generateSccRandomPreset(
    const SccRandomPresetRecipe& recipe) noexcept;

[[nodiscard]] SccWaveform applySccRandomPreset(
    const SccWaveform& current,
    const SccRandomPresetRecipe& recipe) noexcept;

[[nodiscard]] SccMergeResult mergeSccWaveforms(
    const SccWaveform& current,
    const SccWaveform& added,
    SccMergeOptions options = {}) noexcept;

[[nodiscard]] SccWaveform averageSccWaveform(
    const SccWaveform& waveform) noexcept;

[[nodiscard]] SccWaveform normalizeSccWaveform(
    const SccWaveform& waveform) noexcept;

[[nodiscard]] SccWaveform invertSccWaveform(
    const SccWaveform& waveform) noexcept;

[[nodiscard]] SccWaveform mirrorSccWaveform(
    const SccWaveform& waveform) noexcept;

[[nodiscard]] SccWaveform rotateSccWaveform(
    const SccWaveform& waveform,
    int samples) noexcept;

[[nodiscard]] SccWaveform shiftSccWaveformVertically(
    const SccWaveform& waveform,
    int amount) noexcept;

[[nodiscard]] SccWaveform scaleSccWaveformVertically(
    const SccWaveform& waveform,
    int percent) noexcept;

[[nodiscard]] SccWaveform applySccWaveformRange(
    const SccWaveform& current,
    const SccWaveform& candidate,
    SccApplyRange range) noexcept;

}  // namespace mgstc::engine
