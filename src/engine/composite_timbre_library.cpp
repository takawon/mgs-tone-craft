#include "mgstc/engine/composite_timbre_library.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <sstream>
#include <type_traits>
#include <utility>

namespace mgstc::engine {
namespace {

constexpr std::string_view kHeader{
    "MGSTC_COMPOSITE_TIMBRE_LIBRARY\t1"};
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
        string(value.tags);
        string(value.memo);
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
            unsignedInteger(layer.start_delay_counts, 4);
            unsignedInteger(layer.volume, 1);
            boolean(layer.base_timbre.has_value());
            if (layer.base_timbre) {
                reference(*layer.base_timbre);
            }
            envelope(layer.volume_envelope);
            envelope(layer.pitch_envelope);
            unsignedInteger(layer.timbre_automation.size(), 4);
            for (const auto& event_value :
                 layer.timbre_automation) {
                event(event_value);
            }
            timeline(layer.envelope_timeline);
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
        return enumeration(value.kind, 6)
            && integer(value.value, 4)
            && integer(value.secondary, 4)
            && integer(value.count, 4);
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
            || !integer(value.rate.release_rate, 1)
            || (format_version_ == 2
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
            || format_version_ < 1
            || format_version_ > CompositeTimbre::kFormatVersion
            || !string(value.name)
            || !string(value.tags)
            || !string(value.memo)
            || !integer(layer_count, 4)
            || layer_count > 1024) {
            return false;
        }
        value.layers.resize(layer_count);
        for (auto& layer : value.layers) {
            EnvelopeTimeline legacy_volume_timeline;
            EnvelopeTimeline legacy_pitch_timeline;
            EnvelopeTimeline legacy_timbre_timeline;
            bool has_reference{};
            std::uint8_t relative{};
            std::uint16_t detune{};
            std::uint32_t automation_count{};
            if (!string(layer.name)
                || !enumeration(layer.source, 2)
                || !integer(layer.channel, 1)
                || !boolean(layer.enabled)
                || !boolean(layer.muted)
                || !boolean(layer.solo)
                || !integer(relative, 1)
                || !integer(detune, 2)
                || !integer(layer.start_delay_counts, 4)
                || !integer(layer.volume, 1)
                || !boolean(has_reference)) {
                return false;
            }
            layer.relative_semitones =
                static_cast<std::int8_t>(relative);
            layer.detune = static_cast<std::int16_t>(detune);
            if (has_reference) {
                SavedTimbreReference reference_value;
                if (!reference(reference_value)) {
                    return false;
                }
                layer.base_timbre = std::move(reference_value);
            } else {
                layer.base_timbre.reset();
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
        }
        value.format_version = CompositeTimbre::kFormatVersion;
        return position_ == data_.size();
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
            << '\t' << encodeTimbre(entry.timbre)
            << "\r\n";
    }
    return output.str();
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
            if (fields.size() != 6 || fields[0] != "C"
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
                || library.find(entry.id)) {
                setError(
                    error,
                    "invalid composite timbre library metadata");
                return std::nullopt;
            }
            auto timbre = decodeTimbre(fields[5]);
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
