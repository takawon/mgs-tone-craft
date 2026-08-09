#include "mgstc/engine/timbre_library.hpp"
#include "mgstc/engine/timbre_tags.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <sstream>
#include <utility>

namespace mgstc::engine {
namespace {

constexpr std::string_view kHeader{"MGSTC_TIMBRE_LIBRARY\t4"};

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

std::string encodeBytes(std::string_view bytes) {
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const unsigned char value : bytes) {
        result.push_back(hexDigit(static_cast<std::uint8_t>(value >> 4)));
        result.push_back(hexDigit(static_cast<std::uint8_t>(value & 0x0F)));
    }
    return result;
}

template <std::size_t Size>
std::string encodeBytes(const std::array<std::uint8_t, Size>& bytes) {
    return encodeBytes(std::string_view{
        reinterpret_cast<const char*>(bytes.data()), bytes.size()});
}

std::optional<std::string> decodeString(std::string_view text) {
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

void appendUint32(std::string& output, std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        output.push_back(static_cast<char>(
            (value >> (index * 8U)) & 0xFFU));
    }
}

[[nodiscard]] bool readUint32(
    std::string_view input,
    std::size_t& position,
    std::uint32_t& value) {
    if (position + 4 > input.size()) {
        return false;
    }
    value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(
                     static_cast<unsigned char>(
                         input[position + index]))
            << (index * 8U);
    }
    position += 4;
    return true;
}

[[nodiscard]] std::string encodeTagList(
    std::span<const std::string> tags) {
    std::string packed;
    appendUint32(
        packed, static_cast<std::uint32_t>(tags.size()));
    for (const auto& tag : tags) {
        appendUint32(
            packed, static_cast<std::uint32_t>(tag.size()));
        packed += tag;
    }
    return encodeBytes(packed);
}

[[nodiscard]] std::optional<std::vector<std::string>> decodeTagList(
    std::string_view text) {
    const auto packed = decodeString(text);
    if (!packed) {
        return std::nullopt;
    }
    std::size_t position{};
    std::uint32_t count{};
    if (!readUint32(*packed, position, count) || count > 100000) {
        return std::nullopt;
    }
    std::vector<std::string> tags;
    tags.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        std::uint32_t size{};
        if (!readUint32(*packed, position, size)
            || position + size > packed->size()) {
            return std::nullopt;
        }
        tags.emplace_back(packed->substr(position, size));
        position += size;
    }
    if (position != packed->size()) {
        return std::nullopt;
    }
    const auto normalized = serializeTimbreTags(tags);
    return parseTimbreTags(normalized);
}

template <std::size_t Size>
bool decodeArray(
    std::string_view text,
    std::array<std::uint8_t, Size>& destination) {
    if (text.size() != Size * 2) {
        return false;
    }
    for (std::size_t index = 0; index < Size; ++index) {
        const auto high = hexValue(text[index * 2]);
        const auto low = hexValue(text[index * 2 + 1]);
        if (!high || !low) {
            return false;
        }
        destination[index] =
            static_cast<std::uint8_t>((*high << 4) | *low);
    }
    return true;
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

void writeEntry(
    std::ostringstream& output,
    const TimbreLibraryEntry& entry) {
    output
        << (entry.category == TimbreCategory::Opll ? 'O' : 'S')
        << '\t' << entry.id
        << '\t' << entry.created_unix_seconds
        << '\t' << entry.updated_unix_seconds
        << '\t' << entry.last_used_unix_seconds
        << '\t' << entry.use_count
        << '\t' << (entry.favorite ? 1 : 0)
        << '\t' << entry.data_version
        << '\t' << entry.revision
        << '\t' << encodeBytes(entry.name)
        << '\t' << encodeTagList(entry.tags)
        << '\t' << encodeBytes(entry.memo)
        << '\t' << encodeBytes(entry.opll_registers)
        << '\t' << encodeBytes(entry.scc_waveform)
        << "\r\n";
}

}  // namespace

const std::vector<TimbreLibraryEntry>& TimbreLibrary::entries() const
    noexcept {
    return entries_;
}

const TimbreLibraryEntry* TimbreLibrary::find(std::uint64_t id) const
    noexcept {
    const auto found = std::find_if(
        entries_.begin(), entries_.end(),
        [id](const auto& entry) { return entry.id == id; });
    return found == entries_.end() ? nullptr : &*found;
}

TimbreLibraryEntry* TimbreLibrary::find(std::uint64_t id) noexcept {
    return const_cast<TimbreLibraryEntry*>(
        std::as_const(*this).find(id));
}

std::uint64_t TimbreLibrary::add(
    TimbreLibraryEntry entry,
    std::int64_t now_unix_seconds) {
    entry.id = next_id_++;
    entry.created_unix_seconds = now_unix_seconds;
    entry.updated_unix_seconds = now_unix_seconds;
    entry.last_used_unix_seconds = 0;
    entry.use_count = 0;
    entry.data_version = 1;
    entry.revision = 1;
    entries_.push_back(std::move(entry));
    return entries_.back().id;
}

bool TimbreLibrary::update(
    std::uint64_t id,
    const TimbreLibraryEntry& replacement,
    std::int64_t now_unix_seconds) {
    auto* current = find(id);
    if (!current || replacement.category != current->category) {
        return false;
    }
    auto updated = replacement;
    updated.id = current->id;
    updated.created_unix_seconds = current->created_unix_seconds;
    updated.updated_unix_seconds = now_unix_seconds;
    updated.last_used_unix_seconds =
        current->last_used_unix_seconds;
    updated.use_count = current->use_count;
    updated.data_version = 1;
    updated.revision = current->revision
        == std::numeric_limits<std::uint32_t>::max()
        ? current->revision
        : current->revision + 1;
    *current = std::move(updated);
    return true;
}

bool TimbreLibrary::erase(std::uint64_t id) {
    const auto old_size = entries_.size();
    std::erase_if(
        entries_, [id](const auto& entry) { return entry.id == id; });
    return entries_.size() != old_size;
}

bool TimbreLibrary::touch(
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

std::size_t TimbreLibrary::rewriteTag(
    std::string_view source,
    std::string_view replacement,
    std::int64_t now_unix_seconds) {
    std::size_t changed{};
    for (auto& entry : entries_) {
        if (rewriteTimbreTag(entry.tags, source, replacement)) {
            entry.updated_unix_seconds = now_unix_seconds;
            ++changed;
        }
    }
    return changed;
}

std::string TimbreLibrary::serialize() const {
    std::ostringstream output;
    output << kHeader << "\r\n";
    for (const auto& entry : entries_) {
        writeEntry(output, entry);
    }
    return output.str();
}

std::optional<std::string> TimbreLibrary::serializeEntry(
    std::uint64_t id) const {
    const auto* entry = find(id);
    if (!entry) {
        return std::nullopt;
    }
    std::ostringstream output;
    output << kHeader << "\r\n";
    writeEntry(output, *entry);
    return output.str();
}

std::string TimbreLibrary::uniqueName(
    TimbreCategory category,
    std::string_view requested_name) const {
    const std::string base = requested_name.empty()
        ? std::string("Imported Timbre")
        : std::string(requested_name);
    const auto available = [&](std::string_view candidate) {
        return std::none_of(
            entries_.begin(),
            entries_.end(),
            [&](const auto& entry) {
                return entry.category == category
                    && entry.name == candidate;
            });
    };
    if (available(base)) {
        return base;
    }
    for (std::uint64_t suffix = 1;
         suffix < std::numeric_limits<std::uint64_t>::max();
         ++suffix) {
        std::string candidate =
            base + '(' + std::to_string(suffix) + ')';
        if (available(candidate)) {
            return candidate;
        }
    }
    return base;
}

std::optional<std::vector<std::uint64_t>>
TimbreLibrary::importSerialized(
    std::string_view text,
    std::int64_t now_unix_seconds,
    std::string* error) {
    auto imported = deserialize(text, error);
    if (!imported) {
        return std::nullopt;
    }
    std::vector<std::uint64_t> ids;
    ids.reserve(imported->entries_.size());
    for (auto entry : imported->entries_) {
        entry.name = uniqueName(entry.category, entry.name);
        ids.push_back(add(std::move(entry), now_unix_seconds));
    }
    return ids;
}

std::optional<TimbreLibrary> TimbreLibrary::deserialize(
    std::string_view text,
    std::string* error) {
    TimbreLibrary library;
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
                setError(error, "unsupported timbre library header");
                return std::nullopt;
            }
        } else if (!line.empty()) {
            const auto fields = splitTabs(line);
            if (fields.size() != 14
                || (fields[0] != "O" && fields[0] != "S")) {
                setError(error, "invalid timbre library record");
                return std::nullopt;
            }
            TimbreLibraryEntry entry;
            entry.category = fields[0] == "O"
                ? TimbreCategory::Opll
                : TimbreCategory::Scc;
            unsigned int favorite = 0;
            if (!parseInteger(fields[1], entry.id)
                || entry.id == 0
                || entry.id == std::numeric_limits<std::uint64_t>::max()
                || !parseInteger(fields[2], entry.created_unix_seconds)
                || !parseInteger(fields[3], entry.updated_unix_seconds)
                || !parseInteger(
                    fields[4], entry.last_used_unix_seconds)
                || !parseInteger(fields[5], entry.use_count)
                || !parseInteger(fields[6], favorite)
                || favorite > 1
                || !parseInteger(fields[7], entry.data_version)
                || entry.data_version != 1
                || !parseInteger(fields[8], entry.revision)
                || entry.revision == 0) {
                setError(error, "invalid timbre library metadata");
                return std::nullopt;
            }
            constexpr std::size_t payload_offset = 9;
            const auto name =
                decodeString(fields[payload_offset]);
            const auto tags =
                decodeTagList(fields[payload_offset + 1]);
            const auto memo =
                decodeString(fields[payload_offset + 2]);
            if (!name || !tags || !memo
                || !decodeArray(
                    fields[payload_offset + 3],
                    entry.opll_registers)
                || !decodeArray(
                    fields[payload_offset + 4],
                    entry.scc_waveform)
                || library.find(entry.id)) {
                setError(error, "invalid timbre library payload");
                return std::nullopt;
            }
            entry.favorite = favorite != 0;
            entry.name = *name;
            entry.tags = *tags;
            entry.memo = *memo;
            library.next_id_ = std::max(
                library.next_id_,
                entry.id + 1);
            library.entries_.push_back(std::move(entry));
        }
        if (line_end == std::string_view::npos) {
            break;
        }
        line_begin = line_end + 1;
    }
    if (line_number == 0) {
        setError(error, "empty timbre library");
        return std::nullopt;
    }
    return library;
}

}  // namespace mgstc::engine
