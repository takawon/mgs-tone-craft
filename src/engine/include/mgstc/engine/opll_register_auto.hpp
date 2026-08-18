#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "mgstc/engine/composite_timbre.hpp"
#include "mgstc/engine/timbre_library.hpp"

namespace mgstc::engine {

enum class OpllActiveTimbreKind : std::uint8_t {
    None = 0,
    Original = 1,
    Rom = 2,
};

struct OpllActiveTimbreAtCount {
    OpllActiveTimbreKind kind{OpllActiveTimbreKind::None};
    std::uint64_t library_id{};
    std::uint8_t rom_number{};
};

[[nodiscard]] std::uint8_t packOpllRegisterAutoByte(
    OpllRegisterAutoTarget target,
    std::uint8_t base_register_byte,
    std::uint8_t value) noexcept;

// Resolves original-timbre register dump for packing. Prefers the layer's
// base_timbre snapshot when ids match, otherwise the live library entry.
[[nodiscard]] std::optional<std::array<std::uint8_t, 8>>
opllOriginalRegistersForLibraryId(
    const CompositeLayer& layer,
    std::uint64_t library_id,
    const TimbreLibrary* library) noexcept;

// Active @ (or base) at the given count: ROM vs original library id.
[[nodiscard]] OpllActiveTimbreAtCount opllActiveTimbreAt(
    const CompositeLayer& layer,
    std::uint32_t count) noexcept;

// Original-tone register image (regs 0–7) at `count`: base / @-slide
// originals + manual y. When `include_manual_y_at_count` is false, y at
// exactly `count` are omitted (baseline before same-step manual y).
// When `include_register_auto` is true, TL/FB auto field values active at
// `count` are packed onto regs 2/3 (same packing as expand). Still excludes
// writing auto into the authoring model. nullopt when ROM is active or no
// original image.
[[nodiscard]] std::optional<std::array<std::uint8_t, 8>>
opllOriginalRegisterImageAt(
    const CompositeLayer& layer,
    std::uint32_t count,
    const TimbreLibrary* library = nullptr,
    bool include_manual_y_at_count = true,
    bool include_register_auto = false) noexcept;

[[nodiscard]] bool opllRegisterAutoAvailableAt(
    const CompositeLayer& layer,
    std::uint32_t count) noexcept;

// YM2413 original-tone registers 0–7 are chip-wide. Product rule: at most
// one OPLL layer may own TL auto, and at most one may own FB auto (they may
// be different layers). First active layer in authoring order wins.
[[nodiscard]] std::optional<std::size_t> opllRegisterAutoOwnerLayer(
    const CompositeTimbre& timbre,
    OpllRegisterAutoTarget target) noexcept;

[[nodiscard]] bool opllRegisterAutoOwnedByLayer(
    const CompositeTimbre& timbre,
    std::size_t layer_index,
    OpllRegisterAutoTarget target) noexcept;

// Clears duplicate active TL/FB autos so exclusivity holds. Returns true if
// any lane was forced Off.
bool enforceOpllRegisterAutoExclusivity(CompositeTimbre& timbre);

// Expands free-curve points to RegisterWrite events (yreg,data).
// Point i is written at start_count + i * coarseness.
// Packing uses a fixed base_register_byte; prefer
// expandOpllLayerRegisterAutos for product output.
[[nodiscard]] std::vector<EnvelopeEvent> expandOpllRegisterAuto(
    const OpllRegisterAutoLane& lane,
    OpllRegisterAutoTarget target,
    std::uint8_t base_register_byte,
    std::uint32_t length_counts);

// Expands TL/FB auto with per-count original register image:
// base / @-slide originals + prior manual y, field-only pack; skips ROM.
// When expand_tl / expand_fb is false, that lane is omitted (exclusivity /
// poly secondary-voice gating).
[[nodiscard]] std::vector<EnvelopeEvent> expandOpllLayerRegisterAutos(
    const CompositeLayer& layer,
    const TimbreLibrary* library = nullptr,
    bool expand_tl = true,
    bool expand_fb = true);

[[nodiscard]] std::optional<std::uint8_t> opllRegisterAutoValueAt(
    const OpllRegisterAutoLane& lane,
    OpllRegisterAutoTarget target,
    std::uint32_t count,
    std::uint32_t length_counts);

}  // namespace mgstc::engine
