#include "mgstc/engine/composite_timbre_library.hpp"
#include "mgstc/engine/timbre_tags.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <sstream>
#include <type_traits>
#include <utility>

namespace mgstc::engine {
namespace {

constexpr std::string_view kHeader{
    "MGSTC_COMPOSITE_TIMBRE_LIBRARY\t3"};
constexpr std::uint32_t kMaximumCollectionSize = 100000;
constexpr std::uint32_t kMaximumStringSize = 1024 * 1024;

char hexDigit(std::uint8_t value) {
    return value < 10
        ? static_cast<char>('0' + value)
        : static_cast<char>('A' + value - 10);
}

std::optional<std::uint8_t> hexValue(char value) {
    if (value >= '0' && value <= '9') {
        return static_cast<std::uint8_t>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
        return static_cast<std::uint8_t>(value - 'a' + 10);
    }
    if (value >= 'A' && value <= 'F') {
        return static_cast<std::uint8_t>(value - 'A' + 10);
    }
    return std::nullopt;
}

std::string encodeHex(std::string_view bytes) {
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const unsigned char value : bytes) {
        result.push_back(hexDigit(
            static_cast<std::uint8_t>(value >> 4)));
        result.push_back(hexDigit(
            static_cast<std::uint8_t>(value & 0x0F)));
    }
    return result;
}

std::optional<std::string> decodeHex(std::string_view text) {
    if ((text.size() & 1U) != 0U) {
        return std::nullopt;
    }
    std::string result;
    result.reserve(text.size() / 2);
    for (std::size_t index = 0; index < text.size(); index += 2) {
        const auto high = hexValue(text[index]);
        const auto low = hexValue(text[index + 1]);
        if (!high || !low) {
            return std::nullopt;
        }
        result.push_back(static_cast<char>((*high << 4) | *low));
    }
    return result;
}

template <typename Integer>
bool parseInteger(std::string_view text, Integer& value) {
    const auto result = std::from_chars(
        text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{}
        && result.ptr == text.data() + text.size();
}

std::vector<std::string_view> splitTabs(std::string_view line) {
    std::vector<std::string_view> fields;
    std::size_t begin = 0;
    while (begin <= line.size()) {
        const auto end = line.find('\t', begin);
        if (end == std::string_view::npos) {
            fields.push_back(line.substr(begin));
            break;
        }
        fields.push_back(line.substr(begin, end - begin));
        begin = end + 1;
    }
    return fields;
}

void setError(std::string* error, std::string_view message) {
    if (error) {
        *error = message;
    }
}

class BinaryWriter {
public:
    void unsignedInteger(std::uint64_t value, std::size_t bytes) {
        for (std::size_t index = 0; index < bytes; ++index) {
            data_.push_back(static_cast<char>(
                (value >> (index * 8U)) & 0xFFU));
        }
    }

    template <typename Enum>
    void enumeration(Enum value) {
        static_assert(std::is_enum_v<Enum>);
        unsignedInteger(
            static_cast<std::uint8_t>(value), 1);
    }

    void boolean(bool value) {
        unsignedInteger(value ? 1 : 0, 1);
    }

    void string(std::string_view value) {
        unsignedInteger(value.size(), 4);
        data_.append(value);
    }

    void event(const EnvelopeEvent& value) {
        enumeration(value.kind);
        unsignedInteger(
            static_cast<std::uint32_t>(value.value), 4);
        unsignedInteger(
            static_cast<std::uint32_t>(value.secondary), 4);
        unsignedInteger(value.count, 4);
        unsignedInteger(value.target_library_id, 8);
        enumeration(value.timbre_pick);
        boolean(value.after_loop_start);
        boolean(value.automatic);
        boolean(value.precise);
    }

    void timeline(const EnvelopeTimeline& value) {
        unsignedInteger(value.length_counts, 4);
        boolean(value.loop_start_count.has_value());
        unsignedInteger(value.loop_start_count.value_or(0), 4);
        boolean(value.loop_end_count.has_value());
        unsignedInteger(value.loop_end_count.value_or(0), 4);
    }

    void envelope(const SoftwareEnvelope& value) {
        enumeration(value.kind);
        unsignedInteger(value.rate.attack_level, 1);
        unsignedInteger(value.rate.attack_rate, 1);
        unsignedInteger(value.rate.decay_rate, 1);
        unsignedInteger(value.rate.sustain_level, 1);
        unsignedInteger(value.rate.sustain_rate, 1);
        unsignedInteger(value.rate.release_rate, 1);
        unsignedInteger(value.rate.tone_mode, 1);
        unsignedInteger(value.rate.noise, 1);
        unsignedInteger(value.events.size(), 4);
        for (const auto& event_value : value.events) {
            event(event_value);
        }
    }

    void reference(const SavedTimbreReference& value) {
        unsignedInteger(value.library_id, 8);
        unsignedInteger(value.revision, 4);
        string(value.name);
        enumeration(value.source);
        enumeration(value.number_mode);
        boolean(value.manual_number.has_value());
        unsignedInteger(value.manual_number.value_or(0), 1);
        for (const auto byte : value.opll_registers) {
            unsignedInteger(byte, 1);
        }
        for (const auto byte : value.scc_waveform) {
            unsignedInteger(byte, 1);
        }
    }

    void timbre(const CompositeTimbre& value) {
        unsignedInteger(CompositeTimbre::kFormatVersion, 4);
        string(value.name);
        unsignedInteger(value.tags.size(), 4);
        for (const auto& tag : value.tags) {
            string(tag);
        }
        string(value.memo);
        boolean(value.favorite);
        unsignedInteger(
            static_cast<std::uint16_t>(std::clamp(
                value.playback_tempo, kMgscTempoMin, kMgscTempoMax)),
            2);
        unsignedInteger(value.layers.size(), 4);
        for (const auto& layer : value.layers) {
            string(layer.name);
            enumeration(layer.source);
            unsignedInteger(layer.channel, 1);
            boolean(layer.enabled);
            boolean(layer.muted);
            boolean(layer.solo);
            unsignedInteger(
                static_cast<std::uint8_t>(
                    layer.relative_semitones),
                1);
            unsignedInteger(
                static_cast<std::uint16_t>(layer.detune),
                2);
            unsignedInteger(
                static_cast<std::uint32_t>(layer.micro_detune),
                4);
            enumeration(layer.start_delay_form);
            unsignedInteger(layer.start_delay_value, 4);
            unsignedInteger(layer.volume, 1);
            if (layer.base_opll_rom) {
                unsignedInteger(2, 1); // base kind: OPLL ROM
                unsignedInteger(*layer.base_opll_rom, 1);
            } else if (layer.base_timbre) {
                unsignedInteger(1, 1); // base kind: library
                reference(*layer.base_timbre);
            } else {
                unsignedInteger(0, 1); // base kind: none
            }
            envelope(layer.volume_envelope);
            envelope(layer.pitch_envelope);
            unsignedInteger(layer.timbre_automation.size(), 4);
            for (const auto& event_value :
                 layer.timbre_automation) {
                event(event_value);
            }
            timeline(layer.envelope_timeline);
            registerAuto(layer.opll_tl_auto);
            registerAuto(layer.opll_fb_auto);
            boolean(layer.software_lfo.enabled);
            unsignedInteger(layer.software_lfo.delay, 1);
            unsignedInteger(layer.software_lfo.depth, 1);
            unsignedInteger(layer.software_lfo.speed, 1);
            unsignedInteger(
                static_cast<std::uint8_t>(layer.software_lfo.roughness), 1);
            unsignedInteger(
                static_cast<std::uint32_t>(layer.software_lfo.extra_roughness),
                4);
            unsignedInteger(layer.envelope_number, 1);
            unsignedInteger(layer.key_off_hang, 1);
            boolean(layer.pitch_sweep.enabled);
            unsignedInteger(layer.pitch_sweep.value, 1);
            boolean(layer.opll_sustain);
        }
    }

    void registerAuto(const OpllRegisterAutoLane& value) {
        enumeration(value.mode);
        unsignedInteger(value.start_count, 4);
        unsignedInteger(value.depth, 1);
        unsignedInteger(value.change_speed, 1);
        unsignedInteger(value.coarseness, 1);
        unsignedInteger(value.stop_position, 1);
        unsignedInteger(value.free_curve.size(), 4);
        for (const auto point : value.free_curve) {
            unsignedInteger(point, 1);
        }
    }

    [[nodiscard]] const std::string& data() const noexcept {
        return data_;
    }

private:
    std::string data_;
};

class BinaryReader {
public:
    explicit BinaryReader(std::string_view data) : data_(data) {}

    bool unsignedInteger(
        std::uint64_t& value,
        std::size_t bytes) {
        if (bytes > 8 || position_ + bytes > data_.size()) {
            return false;
        }
        value = 0;
        for (std::size_t index = 0; index < bytes; ++index) {
            value |= static_cast<std::uint64_t>(
                         static_cast<unsigned char>(
                             data_[position_ + index]))
                << (index * 8U);
        }
        position_ += bytes;
        return true;
    }

    template <typename Integer>
    bool integer(Integer& value, std::size_t bytes) {
        std::uint64_t parsed{};
        if (!unsignedInteger(parsed, bytes)) {
            return false;
        }
        value = static_cast<Integer>(parsed);
        return true;
    }

    bool boolean(bool& value) {
        std::uint8_t parsed{};
        if (!integer(parsed, 1) || parsed > 1) {
            return false;
        }
        value = parsed != 0;
        return true;
    }

    template <typename Enum>
    bool enumeration(Enum& value, std::uint8_t maximum) {
        std::uint8_t parsed{};
        if (!integer(parsed, 1) || parsed > maximum) {
            return false;
        }
        value = static_cast<Enum>(parsed);
        return true;
    }

    bool string(std::string& value) {
        std::uint32_t size{};
        if (!integer(size, 4)
            || size > kMaximumStringSize
            || position_ + size > data_.size()) {
            return false;
        }
        value.assign(data_.substr(position_, size));
        position_ += size;
        return true;
    }

    bool event(EnvelopeEvent& value) {
        if (!enumeration(value.kind, 6)
            || !integer(value.value, 4)
            || !integer(value.secondary, 4)
            || !integer(value.count, 4)) {
            return false;
        }
        if (format_version_ < 6) {
            value.target_library_id = 0;
            value.timbre_pick = TimbrePick::Library;
            value.after_loop_start = false;
            return true;
        }
        std::uint8_t pick{};
        if (!integer(value.target_library_id, 8)
            || !integer(pick, 1)
            || pick > 1) {
            return false;
        }
        value.timbre_pick = static_cast<TimbrePick>(pick);
        if (format_version_ >= 13) {
            if (!boolean(value.after_loop_start)) {
                return false;
            }
        } else {
            value.after_loop_start = false;
        }
        if (format_version_ >= 16) {
            if (!boolean(value.automatic)) {
                return false;
            }
        } else {
            value.automatic = false;
        }
        if (format_version_ >= 17) {
            if (!boolean(value.precise)) {
                return false;
            }
        } else {
            value.precise = false;
        }
        if (!value.automatic) {
            value.precise = false;
        }
        return true;
    }

    bool timeline(EnvelopeTimeline& value) {
        bool has_loop_start{};
        bool has_loop_end{};
        std::uint32_t loop_start{};
        std::uint32_t loop_end{};
        if (!integer(value.length_counts, 4)
            || value.length_counts == 0
            || value.length_counts
                > EnvelopeTimeline::kMaximumLengthCounts
            || !boolean(has_loop_start)
            || !integer(loop_start, 4)
            || !boolean(has_loop_end)
            || !integer(loop_end, 4)) {
            return false;
        }
        value.loop_start_count = has_loop_start
            ? std::optional<std::uint32_t>{loop_start}
            : std::nullopt;
        value.loop_end_count = has_loop_end
            ? std::optional<std::uint32_t>{loop_end}
            : std::nullopt;
        return (!value.loop_start_count
                || *value.loop_start_count <= value.length_counts)
            && (!value.loop_end_count
                || *value.loop_end_count <= value.length_counts)
            && (!value.loop_start_count || !value.loop_end_count
                || *value.loop_start_count <= *value.loop_end_count);
    }

    bool envelope(
        SoftwareEnvelope& value,
        EnvelopeTimeline* legacy_timeline) {
        std::uint32_t count{};
        if (!enumeration(value.kind, 1)
            || !integer(value.rate.attack_level, 1)
            || !integer(value.rate.attack_rate, 1)
            || !integer(value.rate.decay_rate, 1)
            || !integer(value.rate.sustain_level, 1)
            || !integer(value.rate.sustain_rate, 1)
            || !integer(value.rate.release_rate, 1)) {
            return false;
        }
        if (format_version_ >= 18) {
            if (!integer(value.rate.tone_mode, 1)
                || !integer(value.rate.noise, 1)) {
                return false;
            }
            value.rate = clampRateEnvelope(value.rate);
        } else {
            value.rate.tone_mode = 0;
            value.rate.noise = 0;
        }
        if ((format_version_ == 2
                && (legacy_timeline == nullptr
                    || !timeline(*legacy_timeline)))
            || !integer(count, 4)
            || count > kMaximumCollectionSize) {
            return false;
        }
        value.events.resize(count);
        return std::all_of(
            value.events.begin(),
            value.events.end(),
            [this](auto& event_value) {
                return event(event_value);
            });
    }

    bool reference(SavedTimbreReference& value) {
        bool has_manual{};
        std::uint8_t manual{};
        if (!integer(value.library_id, 8)
            || value.library_id == 0
            || !integer(value.revision, 4)
            || value.revision == 0
            || !string(value.name)
            || !enumeration(value.source, 2)
            || !enumeration(value.number_mode, 1)
            || !boolean(has_manual)
            || !integer(manual, 1)) {
            return false;
        }
        value.manual_number = has_manual
            ? std::optional<std::uint8_t>{manual}
            : std::nullopt;
        for (auto& byte : value.opll_registers) {
            if (!integer(byte, 1)) {
                return false;
            }
        }
        for (auto& byte : value.scc_waveform) {
            if (!integer(byte, 1)) {
                return false;
            }
        }
        return true;
    }

    bool timbre(CompositeTimbre& value) {
        std::uint32_t layer_count{};
        if (!integer(format_version_, 4)
            || format_version_
                < CompositeTimbre::kMinimumReadableFormatVersion
            || format_version_ > CompositeTimbre::kFormatVersion
            || !string(value.name)) {
            return false;
        }
        std::uint32_t tag_count{};
        if (!integer(tag_count, 4)
            || tag_count > kMaximumCollectionSize) {
            return false;
        }
        value.tags.resize(tag_count);
        for (auto& tag : value.tags) {
            if (!string(tag)) {
                return false;
            }
        }
        value.tags = parseTimbreTags(
            serializeTimbreTags(value.tags));
        if (!string(value.memo)
            || !boolean(value.favorite)) {
            return false;
        }
        if (format_version_ >= 10) {
            std::uint16_t tempo{};
            if (!integer(tempo, 2)) {
                return false;
            }
            value.playback_tempo = std::clamp(
                static_cast<int>(tempo), kMgscTempoMin, kMgscTempoMax);
        } else {
            value.playback_tempo = kMgscDefaultTempo;
        }
        if (!integer(layer_count, 4)
            || layer_count > 1024) {
            return false;
        }
        value.layers.resize(layer_count);
        for (auto& layer : value.layers) {
            EnvelopeTimeline legacy_volume_timeline;
            EnvelopeTimeline legacy_pitch_timeline;
            EnvelopeTimeline legacy_timbre_timeline;
            std::uint8_t relative{};
            std::uint16_t detune{};
            std::uint32_t automation_count{};
            std::uint32_t legacy_delay_counts{};
            if (!string(layer.name)
                || !enumeration(layer.source, 2)
                || !integer(layer.channel, 1)
                || !boolean(layer.enabled)
                || !boolean(layer.muted)
                || !boolean(layer.solo)
                || !integer(relative, 1)
                || !integer(detune, 2)) {
                return false;
            }
            layer.relative_semitones =
                static_cast<std::int8_t>(relative);
            layer.detune = static_cast<std::int16_t>(detune);
            if (format_version_ >= 11) {
                std::uint32_t micro{};
                if (!integer(micro, 4)) {
                    return false;
                }
                layer.micro_detune = static_cast<std::int32_t>(micro);
            } else {
                layer.micro_detune = 0;
            }
            if (format_version_ >= 9) {
                if (!enumeration(layer.start_delay_form, 1)
                    || !integer(layer.start_delay_value, 4)) {
                    return false;
                }
            } else if (!integer(legacy_delay_counts, 4)) {
                return false;
            } else {
                layer.start_delay_form =
                    StartDelayForm::AbsoluteTicks;
                layer.start_delay_value = legacy_delay_counts;
            }
            if (!integer(layer.volume, 1)) {
                return false;
            }
            layer.base_timbre.reset();
            layer.base_opll_rom.reset();
            if (format_version_ >= 12) {
                std::uint8_t base_kind{};
                if (!integer(base_kind, 1)) {
                    return false;
                }
                if (base_kind == 1) {
                    SavedTimbreReference reference_value;
                    if (!reference(reference_value)) {
                        return false;
                    }
                    layer.base_timbre = std::move(reference_value);
                } else if (base_kind == 2) {
                    std::uint8_t rom{};
                    if (!integer(rom, 1) || rom > 14) {
                        return false;
                    }
                    layer.base_opll_rom = rom;
                } else if (base_kind != 0) {
                    return false;
                }
            } else {
                bool has_reference{};
                if (!boolean(has_reference)) {
                    return false;
                }
                if (has_reference) {
                    SavedTimbreReference reference_value;
                    if (!reference(reference_value)) {
                        return false;
                    }
                    layer.base_timbre = std::move(reference_value);
                }
            }
            if (!envelope(
                    layer.volume_envelope,
                    &legacy_volume_timeline)
                || !envelope(
                    layer.pitch_envelope,
                    &legacy_pitch_timeline)
                || !integer(automation_count, 4)
                || automation_count > kMaximumCollectionSize) {
                return false;
            }
            layer.timbre_automation.resize(automation_count);
            if (!std::all_of(
                    layer.timbre_automation.begin(),
                    layer.timbre_automation.end(),
                    [this](auto& event_value) {
                        return event(event_value);
                    })) {
                return false;
            }
            if (format_version_ == 2) {
                if (!timeline(legacy_timbre_timeline)) {
                    return false;
                }
                layer.envelope_timeline = legacy_volume_timeline;
            } else if (format_version_ >= 3
                       && !timeline(layer.envelope_timeline)) {
                return false;
            }
            if (format_version_ >= 7) {
                if (!registerAuto(layer.opll_tl_auto)
                    || !registerAuto(layer.opll_fb_auto)) {
                    return false;
                }
            } else {
                layer.opll_tl_auto = {};
                layer.opll_fb_auto = {};
            }
            layer.software_lfo = {};
            if (format_version_ >= 14) {
                std::uint8_t delay{};
                std::uint8_t depth{};
                std::uint8_t speed{};
                std::uint8_t roughness{};
                std::uint32_t extra{};
                if (!boolean(layer.software_lfo.enabled)
                    || !integer(delay, 1)
                    || !integer(depth, 1)
                    || !integer(speed, 1)
                    || !integer(roughness, 1)
                    || !integer(extra, 4)) {
                    return false;
                }
                layer.software_lfo.delay = delay;
                layer.software_lfo.depth = depth;
                layer.software_lfo.speed = speed;
                layer.software_lfo.roughness =
                    static_cast<std::int8_t>(roughness);
                layer.software_lfo.extra_roughness =
                    static_cast<std::int32_t>(extra);
                layer.software_lfo = clampSoftwareLfo(
                    layer.software_lfo,
                    layer.source != TimbreSource::Opll);
            }
            layer.envelope_number = 0;
            layer.key_off_hang = 0;
            layer.pitch_sweep = {};
            layer.opll_sustain = false;
            if (format_version_ >= 15) {
                std::uint8_t envelope_number{};
                std::uint8_t key_off_hang{};
                std::uint8_t pitch_sweep_value{};
                bool pitch_sweep_enabled{};
                bool opll_sustain{};
                if (!integer(envelope_number, 1)
                    || !integer(key_off_hang, 1)
                    || !boolean(pitch_sweep_enabled)
                    || !integer(pitch_sweep_value, 1)
                    || !boolean(opll_sustain)) {
                    return false;
                }
                layer.envelope_number = std::min<std::uint8_t>(
                    envelope_number, 31);
                layer.key_off_hang = key_off_hang;
                layer.pitch_sweep.enabled =
                    pitch_sweep_enabled
                    && layer.source != TimbreSource::Opll;
                layer.pitch_sweep.value = pitch_sweep_value;
                layer.opll_sustain =
                    opll_sustain && layer.source == TimbreSource::Opll;
                if (layer.pitch_sweep.enabled
                    && layer.software_lfo.enabled) {
                    layer.software_lfo.enabled = false;
                }
            }
        }
        if (format_version_ < 15) {
            for (std::size_t index = 0; index < value.layers.size(); ++index) {
                value.layers[index].envelope_number =
                    static_cast<std::uint8_t>(std::min<std::size_t>(index, 31));
            }
        }
        value.format_version = CompositeTimbre::kFormatVersion;
        return position_ == data_.size();
    }

    bool registerAuto(OpllRegisterAutoLane& value) {
        std::uint32_t curve_size{};
        if (!enumeration(value.mode, 4)
            || !integer(value.start_count, 4)
            || !integer(value.depth, 1)
            || !integer(value.change_speed, 1)
            || !integer(value.coarseness, 1)
            || !integer(value.stop_position, 1)
            || !integer(curve_size, 4)
            || curve_size > kMaximumCollectionSize) {
            return false;
        }
        if (value.coarseness == 0) {
            value.coarseness = 1;
        }
        if (value.change_speed == 0) {
            value.change_speed = 1;
        }
        value.free_curve.resize(curve_size);
        for (auto& point : value.free_curve) {
            if (!integer(point, 1)) {
                return false;
            }
        }
        return true;
    }

private:
    std::string_view data_;
    std::size_t position_{};
    std::uint32_t format_version_{};
};

std::string encodeTimbre(const CompositeTimbre& timbre) {
    BinaryWriter writer;
    writer.timbre(timbre);
    return encodeHex(writer.data());
}

std::optional<CompositeTimbre> decodeTimbre(
    std::string_view text) {
    auto bytes = decodeHex(text);
    if (!bytes) {
        return std::nullopt;
    }
    CompositeTimbre timbre;
    BinaryReader reader(*bytes);
    if (!reader.timbre(timbre)) {
        return std::nullopt;
    }
    return timbre;
}

}  // namespace

const std::vector<CompositeTimbreLibraryEntry>&
CompositeTimbreLibrary::entries() const noexcept {
    return entries_;
}

const CompositeTimbreLibraryEntry* CompositeTimbreLibrary::find(
    std::uint64_t id) const noexcept {
    const auto found = std::find_if(
        entries_.begin(),
        entries_.end(),
        [id](const auto& entry) { return entry.id == id; });
    return found == entries_.end() ? nullptr : &*found;
}

CompositeTimbreLibraryEntry* CompositeTimbreLibrary::find(
    std::uint64_t id) noexcept {
    return const_cast<CompositeTimbreLibraryEntry*>(
        std::as_const(*this).find(id));
}

std::uint64_t CompositeTimbreLibrary::add(
    CompositeTimbre timbre,
    std::int64_t now_unix_seconds) {
    CompositeTimbreLibraryEntry entry;
    entry.id = next_id_++;
    entry.created_unix_seconds = now_unix_seconds;
    entry.updated_unix_seconds = now_unix_seconds;
    entry.timbre = std::move(timbre);
    entries_.push_back(std::move(entry));
    return entries_.back().id;
}

bool CompositeTimbreLibrary::update(
    std::uint64_t id,
    const CompositeTimbre& replacement,
    std::int64_t now_unix_seconds) {
    auto* current = find(id);
    if (!current) {
        return false;
    }
    current->timbre = replacement;
    current->updated_unix_seconds = now_unix_seconds;
    if (current->revision
        != std::numeric_limits<std::uint32_t>::max()) {
        ++current->revision;
    }
    return true;
}

bool CompositeTimbreLibrary::erase(std::uint64_t id) {
    const auto old_size = entries_.size();
    std::erase_if(
        entries_, [id](const auto& entry) { return entry.id == id; });
    return entries_.size() != old_size;
}

bool CompositeTimbreLibrary::touch(
    std::uint64_t id,
    std::int64_t now_unix_seconds) {
    auto* entry = find(id);
    if (!entry) {
        return false;
    }
    entry->last_used_unix_seconds = now_unix_seconds;
    if (entry->use_count
        != std::numeric_limits<std::uint32_t>::max()) {
        ++entry->use_count;
    }
    return true;
}

std::size_t CompositeTimbreLibrary::rewriteTag(
    std::string_view source,
    std::string_view replacement,
    std::int64_t now_unix_seconds) {
    std::size_t changed{};
    for (auto& entry : entries_) {
        if (rewriteTimbreTag(
                entry.timbre.tags, source, replacement)) {
            entry.updated_unix_seconds = now_unix_seconds;
            ++changed;
        }
    }
    return changed;
}

std::string CompositeTimbreLibrary::uniqueName(
    std::string_view requested_name) const {
    const std::string base = requested_name.empty()
        ? std::string("New Composite Timbre")
        : std::string(requested_name);
    const auto available = [&](std::string_view candidate) {
        return std::none_of(
            entries_.begin(),
            entries_.end(),
            [&](const auto& entry) {
                return entry.timbre.name == candidate;
            });
    };
    if (available(base)) {
        return base;
    }
    for (std::uint64_t suffix = 1;
         suffix < std::numeric_limits<std::uint64_t>::max();
         ++suffix) {
        const auto candidate =
            base + '(' + std::to_string(suffix) + ')';
        if (available(candidate)) {
            return candidate;
        }
    }
    return base;
}

std::vector<TimbreUse> CompositeTimbreLibrary::findTimbreUses(
    std::uint64_t timbre_library_id) const {
    std::vector<CompositeTimbre> composites;
    composites.reserve(entries_.size());
    for (const auto& entry : entries_) {
        composites.push_back(entry.timbre);
    }
    return mgstc::engine::findTimbreUses(
        composites, timbre_library_id);
}

std::size_t CompositeTimbreLibrary::updateTimbreReferences(
    const TimbreLibraryEntry& timbre,
    std::int64_t now_unix_seconds) {
    std::size_t updated_references{};
    for (auto& entry : entries_) {
        const auto updated = mgstc::engine::updateTimbreReferences(
            std::span<CompositeTimbre>{&entry.timbre, 1},
            timbre);
        if (updated == 0) {
            continue;
        }
        updated_references += updated;
        entry.updated_unix_seconds = now_unix_seconds;
        if (entry.revision
            != std::numeric_limits<std::uint32_t>::max()) {
            ++entry.revision;
        }
    }
    return updated_references;
}

std::string CompositeTimbreLibrary::serialize() const {
    std::ostringstream output;
    output << kHeader << "\r\n";
    for (const auto& entry : entries_) {
        output
            << "C\t" << entry.id
            << '\t' << entry.created_unix_seconds
            << '\t' << entry.updated_unix_seconds
            << '\t' << entry.revision
            << '\t' << entry.last_used_unix_seconds
            << '\t' << entry.use_count
            << '\t' << encodeTimbre(entry.timbre)
            << "\r\n";
    }
    return output.str();
}

std::string CompositeTimbreLibrary::serializeTimbreFile(
    const CompositeTimbre& timbre,
    std::int64_t now_unix_seconds) {
    CompositeTimbreLibrary library;
    library.add(timbre, now_unix_seconds);
    return library.serialize();
}

std::optional<CompositeTimbre>
CompositeTimbreLibrary::deserializeTimbreFile(
    std::string_view text,
    std::string* error) {
    auto library = deserialize(text, error);
    if (!library || library->entries().empty()) {
        if (library && library->entries().empty()) {
            setError(error, "empty composite timbre file");
        }
        return std::nullopt;
    }
    return library->entries().front().timbre;
}

std::optional<CompositeTimbreLibrary>
CompositeTimbreLibrary::deserialize(
    std::string_view text,
    std::string* error) {
    CompositeTimbreLibrary library;
    std::size_t line_begin = 0;
    std::size_t line_number = 0;
    while (line_begin <= text.size()) {
        const auto line_end = text.find('\n', line_begin);
        auto line = text.substr(
            line_begin,
            line_end == std::string_view::npos
                ? text.size() - line_begin
                : line_end - line_begin);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        ++line_number;
        if (line_number == 1) {
            if (line != kHeader) {
                setError(
                    error,
                    "unsupported composite timbre library header");
                return std::nullopt;
            }
        } else if (!line.empty()) {
            const auto fields = splitTabs(line);
            CompositeTimbreLibraryEntry entry;
            if (fields.size() != 8 || fields[0] != "C"
                || !parseInteger(fields[1], entry.id)
                || entry.id == 0
                || entry.id
                    == std::numeric_limits<std::uint64_t>::max()
                || !parseInteger(
                    fields[2], entry.created_unix_seconds)
                || !parseInteger(
                    fields[3], entry.updated_unix_seconds)
                || !parseInteger(fields[4], entry.revision)
                || entry.revision == 0
                || !parseInteger(
                    fields[5], entry.last_used_unix_seconds)
                || !parseInteger(fields[6], entry.use_count)
                || library.find(entry.id)) {
                setError(
                    error,
                    "invalid composite timbre library metadata");
                return std::nullopt;
            }
            auto timbre = decodeTimbre(fields[7]);
            if (!timbre) {
                setError(
                    error,
                    "invalid composite timbre library payload");
                return std::nullopt;
            }
            entry.timbre = std::move(*timbre);
            library.next_id_ = std::max(
                library.next_id_, entry.id + 1);
            library.entries_.push_back(std::move(entry));
        }
        if (line_end == std::string_view::npos) {
            break;
        }
        line_begin = line_end + 1;
    }
    return library;
}

}  // namespace mgstc::engine
