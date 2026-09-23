// SPDX-License-Identifier: AGPL-3.0-only

#include "plugin_state.hpp"

#include <cstring>
#include <optional>
#include <string_view>

#include "mgstc/engine/composite_timbre_library.hpp"

namespace mgstc::plugin {
namespace {

constexpr std::uint32_t kFlagSelectedLayer = 1U << 0U;
constexpr std::uint32_t kFlagFavorite = 1U << 1U;

class ByteWriter {
public:
    void u8(std::uint8_t value) { bytes_.push_back(value); }

    void u32(std::uint32_t value) {
        u8(static_cast<std::uint8_t>(value & 0xFFU));
        u8(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
        u8(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
        u8(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
    }

    void u64(std::uint64_t value) {
        u32(static_cast<std::uint32_t>(value & 0xFFFFFFFFULL));
        u32(static_cast<std::uint32_t>(value >> 32U));
    }

    void bytes(std::string_view value) {
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }

    void text(std::string_view value) {
        u32(static_cast<std::uint32_t>(value.size()));
        bytes(value);
    }

    [[nodiscard]] std::vector<std::uint8_t>& data() noexcept { return bytes_; }

private:
    std::vector<std::uint8_t> bytes_;
};

class ByteReader {
public:
    ByteReader(const std::uint8_t* data, std::size_t size)
        : data_(data), size_(size) {}

    bool truncated{false};

    [[nodiscard]] bool need(std::size_t count) {
        if (count > size_ - pos_) {
            truncated = true;
            return false;
        }
        return true;
    }

    [[nodiscard]] std::optional<std::uint8_t> u8() {
        if (!need(1)) {
            return std::nullopt;
        }
        return data_[pos_++];
    }

    [[nodiscard]] std::optional<std::uint32_t> u32() {
        if (!need(4)) {
            return std::nullopt;
        }
        const auto value = static_cast<std::uint32_t>(data_[pos_])
            | (static_cast<std::uint32_t>(data_[pos_ + 1]) << 8U)
            | (static_cast<std::uint32_t>(data_[pos_ + 2]) << 16U)
            | (static_cast<std::uint32_t>(data_[pos_ + 3]) << 24U);
        pos_ += 4;
        return value;
    }

    [[nodiscard]] std::optional<std::uint64_t> u64() {
        const auto low = u32();
        const auto high = u32();
        if (!low || !high) {
            return std::nullopt;
        }
        return static_cast<std::uint64_t>(*low)
            | (static_cast<std::uint64_t>(*high) << 32U);
    }

    [[nodiscard]] std::optional<std::string> text(std::size_t max_bytes) {
        const auto size = u32();
        if (!size) {
            return std::nullopt;
        }
        if (*size > max_bytes) {
            overflow_ = true;
            return std::nullopt;
        }
        if (!need(*size)) {
            return std::nullopt;
        }
        std::string value(
            reinterpret_cast<const char*>(data_ + pos_),
            *size);
        pos_ += *size;
        return value;
    }

    [[nodiscard]] bool overflow() const noexcept { return overflow_; }
    [[nodiscard]] bool finished() const noexcept { return pos_ == size_; }
    [[nodiscard]] std::size_t position() const noexcept { return pos_; }

private:
    const std::uint8_t* data_{};
    std::size_t size_{};
    std::size_t pos_{};
    bool overflow_{false};
};

[[nodiscard]] PluginStateParseResult fail(PluginStateStatus status) {
    PluginStateParseResult result;
    result.status = status;
    return result;
}

[[nodiscard]] bool metadataFits(const mgstc::engine::CompositeTimbre& timbre) {
    if (timbre.name.size() > kMaxPluginStateNameBytes
        || timbre.memo.size() > kMaxPluginStateMemoBytes
        || timbre.tags.size() > kMaxPluginStateTagCount) {
        return false;
    }
    for (const auto& tag : timbre.tags) {
        if (tag.size() > kMaxPluginStateTagBytes) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::uint8_t channelCapacity(
    mgstc::engine::TimbreSource source) noexcept {
    switch (source) {
    case mgstc::engine::TimbreSource::Psg:
        return 3;
    case mgstc::engine::TimbreSource::Scc:
        return 5;
    case mgstc::engine::TimbreSource::Opll:
        return 9;
    default:
        return 0;
    }
}

}  // namespace

bool validatePluginSoundSnapshot(const mgstc::engine::CompositeTimbre& timbre) {
    if (!metadataFits(timbre) || timbre.layers.size() > kMaxPluginStateLayers
        || timbre.embedded_timbres.size() > kMaxPluginStateLayers) {
        return false;
    }
    for (const auto& layer : timbre.layers) {
        const auto capacity = channelCapacity(layer.source);
        if (capacity == 0 || layer.channel >= capacity) {
            return false;
        }
        if (layer.start_delay_form != mgstc::engine::StartDelayForm::NoteLength
            && layer.start_delay_form
                != mgstc::engine::StartDelayForm::AbsoluteTicks) {
            return false;
        }
        if (layer.base_opll_rom && *layer.base_opll_rom > 14) {
            return false;
        }
    }
    return true;
}

std::vector<std::uint8_t> serializePluginState(
    const PluginStateDocument& document) {
    if (!validatePluginSoundSnapshot(document.sound)
        || document.master_volume_percent > 100) {
        return {};
    }
    const auto payload = mgstc::engine::serializeCompositeSoundPayload(
        document.sound);
    if (payload.size() > kMaxPluginSoundPayloadBytes) {
        return {};
    }

    ByteWriter writer;
    writer.bytes(std::string_view(kPluginStateMagic, 8));
    writer.u32(kPluginStateSchemaVersion);
    std::uint32_t flags = 0;
    if (document.editor.has_selected_layer) {
        flags |= kFlagSelectedLayer;
    }
    if (document.sound.favorite) {
        flags |= kFlagFavorite;
    }
    writer.u32(flags);
    writer.u32(document.editor.selected_layer);
    writer.u32(document.editor.editor_tab);
    writer.u64(document.library_id);
    writer.u32(document.master_volume_percent);
    writer.text(document.sound.name);
    writer.text(document.sound.memo);
    writer.u32(static_cast<std::uint32_t>(document.sound.tags.size()));
    for (const auto& tag : document.sound.tags) {
        writer.text(tag);
    }
    writer.u32(mgstc::engine::kCompositeSoundPayloadVersion);
    writer.u32(static_cast<std::uint32_t>(payload.size()));
    writer.bytes(payload);
    return std::move(writer.data());
}

PluginStateParseResult parsePluginState(const void* data, std::size_t size) {
    if (data == nullptr || size == 0) {
        return fail(PluginStateStatus::Empty);
    }
    if (size > kMaxPluginStateBytes) {
        return fail(PluginStateStatus::InvalidContent);
    }
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    if (size < 8) {
        return fail(PluginStateStatus::Truncated);
    }
    if (std::memcmp(bytes, kPluginStateMagic, 8) != 0) {
        return fail(PluginStateStatus::BadMagic);
    }
    ByteReader body(bytes + 8, size - 8);
    const auto version = body.u32();
    if (!version) {
        return fail(PluginStateStatus::Truncated);
    }
    if (*version != kPluginStateSchemaVersion
        && *version != kPluginStateSchemaVersionV1) {
        return fail(PluginStateStatus::UnsupportedVersion);
    }

    const auto flags = body.u32();
    const auto selected_layer = body.u32();
    const auto editor_tab = body.u32();
    const auto library_id = body.u64();
    if (!flags || !selected_layer || !editor_tab || !library_id) {
        return fail(
            body.overflow() ? PluginStateStatus::InvalidContent
                            : PluginStateStatus::Truncated);
    }

    std::uint32_t master_volume_percent = 100;
    if (*version >= kPluginStateSchemaVersion) {
        const auto master = body.u32();
        if (!master) {
            return fail(
                body.overflow() ? PluginStateStatus::InvalidContent
                                : PluginStateStatus::Truncated);
        }
        if (*master > 100) {
            return fail(PluginStateStatus::InvalidContent);
        }
        master_volume_percent = *master;
    }

    auto name = body.text(kMaxPluginStateNameBytes);
    auto memo = body.text(kMaxPluginStateMemoBytes);
    if (!name || !memo) {
        return fail(
            body.overflow() ? PluginStateStatus::InvalidContent
                            : PluginStateStatus::Truncated);
    }
    const auto tag_count = body.u32();
    if (!tag_count) {
        return fail(
            body.overflow() ? PluginStateStatus::InvalidContent
                            : PluginStateStatus::Truncated);
    }
    if (*tag_count > kMaxPluginStateTagCount) {
        return fail(PluginStateStatus::InvalidContent);
    }
    std::vector<std::string> tags;
    tags.reserve(*tag_count);
    for (std::uint32_t index = 0; index < *tag_count; ++index) {
        auto tag = body.text(kMaxPluginStateTagBytes);
        if (!tag) {
            return fail(
                body.overflow() ? PluginStateStatus::InvalidContent
                                : PluginStateStatus::Truncated);
        }
        tags.push_back(std::move(*tag));
    }

    const auto payload_version = body.u32();
    const auto payload_size = body.u32();
    if (!payload_version || !payload_size) {
        return fail(PluginStateStatus::Truncated);
    }
    if (*payload_version != mgstc::engine::kCompositeSoundPayloadVersion) {
        return fail(PluginStateStatus::UnsupportedVersion);
    }
    if (*payload_size > kMaxPluginSoundPayloadBytes) {
        return fail(PluginStateStatus::InvalidContent);
    }
    if (!body.need(*payload_size)) {
        return fail(PluginStateStatus::Truncated);
    }
    const auto payload_offset = body.position();
    std::string payload(
        reinterpret_cast<const char*>(bytes + 8 + payload_offset),
        *payload_size);
    // need() does not advance. Consume the payload then require no trailer.
    for (std::uint32_t index = 0; index < *payload_size; ++index) {
        static_cast<void>(body.u8());
    }
    if (body.truncated) {
        return fail(PluginStateStatus::Truncated);
    }
    if (!body.finished()) {
        return fail(PluginStateStatus::BrokenPayload);
    }

    auto sound = mgstc::engine::deserializeCompositeSoundPayload(
        payload,
        *payload_version,
        nullptr);
    if (!sound) {
        return fail(PluginStateStatus::BrokenPayload);
    }
    sound->name = std::move(*name);
    sound->memo = std::move(*memo);
    sound->tags = std::move(tags);
    sound->favorite = (*flags & kFlagFavorite) != 0;
    if (!validatePluginSoundSnapshot(*sound)) {
        return fail(PluginStateStatus::InvalidContent);
    }

    PluginStateParseResult result;
    result.status = PluginStateStatus::Ok;
    result.document.editor.has_selected_layer =
        (*flags & kFlagSelectedLayer) != 0;
    result.document.editor.selected_layer = *selected_layer;
    result.document.editor.editor_tab = *editor_tab;
    result.document.library_id = *library_id;
    result.document.master_volume_percent = master_volume_percent;
    result.document.sound = std::move(*sound);
    return result;
}

}  // namespace mgstc::plugin
