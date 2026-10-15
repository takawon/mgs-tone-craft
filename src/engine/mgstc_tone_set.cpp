// SPDX-License-Identifier: AGPL-3.0-only

#include "mgstc/engine/mgstc_tone_set.hpp"

#include <algorithm>
#include <limits>
#include <sstream>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace mgstc::engine {
namespace {

constexpr std::string_view kTimbreHeader{"MGSTC_TIMBRE_LIBRARY\t4"};
constexpr std::string_view kCompositeHeader{
    "MGSTC_COMPOSITE_TIMBRE_LIBRARY\t3"};
constexpr std::string_view kToneSetHeader{"MGSTC_TONE_SET\t1"};

void setError(std::string* error, std::string_view message) {
    if (error != nullptr) {
        *error = message;
    }
}

std::string_view stripBom(std::string_view text) {
    if (text.size() >= 3
        && static_cast<unsigned char>(text[0]) == 0xEF
        && static_cast<unsigned char>(text[1]) == 0xBB
        && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3);
    }
    return text;
}

std::string_view firstLine(std::string_view text) {
    text = stripBom(text);
    const auto end = text.find('\n');
    auto line = end == std::string_view::npos
        ? text
        : text.substr(0, end);
    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }
    return line;
}

std::vector<std::string> splitLines(std::string_view text) {
    text = stripBom(text);
    std::vector<std::string> lines;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const auto end = text.find('\n', begin);
        auto line = text.substr(
            begin,
            end == std::string_view::npos
                ? text.size() - begin
                : end - begin);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        lines.emplace_back(line);
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    if (!lines.empty() && lines.back().empty()) {
        lines.pop_back();
    }
    return lines;
}

std::vector<std::string_view> splitTabs(std::string_view line) {
    std::vector<std::string_view> fields;
    std::size_t begin = 0;
    while (begin <= line.size()) {
        const auto end = line.find('\t', begin);
        fields.push_back(line.substr(
            begin,
            end == std::string_view::npos
                ? line.size() - begin
                : end - begin));
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    return fields;
}

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

std::optional<std::string> decodeBytes(std::string_view text) {
    if ((text.size() % 2) != 0) {
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

std::optional<std::string> recordLine(
    std::string serialized,
    std::string* error) {
    const auto split = serialized.find('\n');
    if (split == std::string::npos) {
        setError(error, "timbre record is missing");
        return std::nullopt;
    }
    serialized.erase(0, split + 1);
    while (!serialized.empty()
           && (serialized.back() == '\n' || serialized.back() == '\r')) {
        serialized.pop_back();
    }
    if (serialized.empty() || serialized.find('\n') != std::string::npos) {
        setError(error, "timbre record is missing");
        return std::nullopt;
    }
    return serialized;
}

std::optional<std::string> timbreRecordLine(
    const TimbreLibraryEntry& entry,
    std::string* error) {
    TimbreLibrary library;
    library.installEntries({entry});
    return recordLine(library.serialize(), error);
}

std::optional<std::string> compositeRecordLine(
    const CompositeTimbreLibraryEntry& entry,
    std::string* error) {
    CompositeTimbreLibrary library;
    library.installEntries({entry});
    return recordLine(library.serialize(), error);
}

std::optional<TimbreLibraryEntry> parseTimbreLine(
    std::string_view line,
    std::string* error) {
    std::string wrapped;
    wrapped.reserve(kTimbreHeader.size() + line.size() + 4);
    wrapped.append(kTimbreHeader);
    wrapped.append("\r\n");
    wrapped.append(line);
    wrapped.append("\r\n");
    auto library = TimbreLibrary::deserialize(wrapped, error);
    if (!library || library->entries().size() != 1) {
        if (library) {
            setError(error, "invalid timbre record");
        }
        return std::nullopt;
    }
    return library->entries().front();
}

std::optional<CompositeTimbreLibraryEntry> parseCompositeLine(
    std::string_view line,
    std::string* error) {
    std::string wrapped;
    wrapped.reserve(kCompositeHeader.size() + line.size() + 4);
    wrapped.append(kCompositeHeader);
    wrapped.append("\r\n");
    wrapped.append(line);
    wrapped.append("\r\n");
    auto library = CompositeTimbreLibrary::deserialize(wrapped, error);
    if (!library || library->entries().size() != 1) {
        if (library) {
            setError(error, "invalid composite record");
        }
        return std::nullopt;
    }
    return library->entries().front();
}

std::string uniqueSetName(
    const std::vector<ToneSetRecord>& sets,
    std::string base) {
    const auto taken = [&](const std::string& candidate) {
        return std::any_of(
            sets.begin(),
            sets.end(),
            [&](const ToneSetRecord& set) {
                return set.name == candidate;
            });
    };
    if (!taken(base)) {
        return base;
    }
    for (int index = 2; index < 1000; ++index) {
        const auto candidate = base + " (" + std::to_string(index) + ")";
        if (!taken(candidate)) {
            return candidate;
        }
    }
    return base + " (1000)";
}

std::uint64_t nextSetId(const std::vector<ToneSetRecord>& sets) {
    std::uint64_t next = 1;
    for (const auto& set : sets) {
        if (set.id >= next
            && set.id != std::numeric_limits<std::uint64_t>::max()) {
            next = set.id + 1;
        }
    }
    return next;
}

}  // namespace

MgstcPortableKind detectMgstcFileKind(std::string_view text) {
    const auto line = firstLine(text);
    if (line == kTimbreHeader) {
        return MgstcPortableKind::TimbreLibrary;
    }
    if (line == kCompositeHeader) {
        return MgstcPortableKind::CompositeLibrary;
    }
    if (line == kToneSetHeader) {
        return MgstcPortableKind::ToneSet;
    }
    return MgstcPortableKind::Unknown;
}

std::optional<std::string> serializeMgstcToneSet(
    const MgstcToneSetDocument& document,
    std::string* error) {
    if (document.name.empty()) {
        setError(error, "tone set name is empty");
        return std::nullopt;
    }
    std::ostringstream output;
    output << kToneSetHeader << "\r\n"
           << "SET\t" << encodeBytes(document.name)
           << '\t' << encodeBytes(document.memo) << "\r\n";
    for (const auto& member : document.members) {
        std::optional<std::string> line;
        if (const auto* timbre = std::get_if<TimbreLibraryEntry>(&member)) {
            line = timbreRecordLine(*timbre, error);
        } else if (
            const auto* composite =
                std::get_if<CompositeTimbreLibraryEntry>(&member)) {
            line = compositeRecordLine(*composite, error);
        }
        if (!line) {
            return std::nullopt;
        }
        output << *line << "\r\n";
    }
    return output.str();
}

std::optional<MgstcToneSetDocument> deserializeMgstcToneSet(
    std::string_view text,
    std::string* error) {
    const auto lines = splitLines(text);
    if (lines.size() < 2 || lines[0] != kToneSetHeader) {
        setError(error, "unsupported tone set header");
        return std::nullopt;
    }
    const auto fields = splitTabs(lines[1]);
    if (fields.size() != 3 || fields[0] != "SET") {
        setError(error, "invalid tone set header");
        return std::nullopt;
    }
    const auto name = decodeBytes(fields[1]);
    const auto memo = decodeBytes(fields[2]);
    if (!name || name->empty() || !memo) {
        setError(error, "invalid tone set name");
        return std::nullopt;
    }
    MgstcToneSetDocument document;
    document.name = *name;
    document.memo = *memo;
    for (std::size_t index = 2; index < lines.size(); ++index) {
        const auto& line = lines[index];
        if (line.empty()) {
            setError(error, "invalid tone set record");
            return std::nullopt;
        }
        if (line.front() == 'O' || line.front() == 'S') {
            auto entry = parseTimbreLine(line, error);
            if (!entry) {
                return std::nullopt;
            }
            document.members.emplace_back(std::move(*entry));
        } else if (line.front() == 'C') {
            auto entry = parseCompositeLine(line, error);
            if (!entry) {
                return std::nullopt;
            }
            document.members.emplace_back(std::move(*entry));
        } else {
            setError(error, "invalid tone set record");
            return std::nullopt;
        }
    }
    return document;
}

std::optional<MgstcToneSetImportResult> importMgstcToneSet(
    const MgstcToneSetDocument& document,
    TimbreLibrary& timbres,
    CompositeTimbreLibrary& composites,
    std::vector<ToneSetRecord>& sets,
    std::vector<ToneSetMemberRecord>& members,
    std::int64_t now_unix_seconds,
    std::string* error) {
    if (document.name.empty()) {
        setError(error, "tone set name is empty");
        return std::nullopt;
    }
    const auto set_id = nextSetId(sets);
    if (set_id == 0) {
        setError(error, "tone set id space is exhausted");
        return std::nullopt;
    }
    MgstcToneSetImportResult result;
    result.set_id = set_id;
    result.set_name = uniqueSetName(sets, document.name);
    std::vector<ToneSetMemberRecord> added;
    added.reserve(document.members.size());
    int position = 0;
    for (const auto& member : document.members) {
        ToneSetMemberRecord row;
        row.set_id = set_id;
        row.position = position++;
        if (const auto* timbre = std::get_if<TimbreLibraryEntry>(&member)) {
            auto entry = *timbre;
            entry.name = timbres.uniqueName(entry.category, entry.name);
            row.timbre_id = timbres.add(std::move(entry), now_unix_seconds);
            ++result.added_timbres;
        } else if (
            const auto* composite =
                std::get_if<CompositeTimbreLibraryEntry>(&member)) {
            auto program = composite->timbre;
            program.name = composites.uniqueName(program.name);
            row.composite_id =
                composites.add(std::move(program), now_unix_seconds);
            ++result.added_composites;
        }
        added.push_back(row);
    }
    ToneSetRecord set;
    set.id = set_id;
    set.name = result.set_name;
    set.memo = document.memo;
    set.created_unix_seconds = now_unix_seconds;
    set.updated_unix_seconds = now_unix_seconds;
    set.revision = 1;
    sets.push_back(std::move(set));
    members.insert(members.end(), added.begin(), added.end());
    return result;
}

bool replaceFileBytesAtomically(
    const std::filesystem::path& destination,
    std::string_view bytes,
    std::string* error) {
    std::error_code exists_error;
    if (destination.empty()
        || !std::filesystem::is_directory(
            destination.parent_path(), exists_error)
        || exists_error) {
        setError(error, "export folder is missing");
        return false;
    }
    const auto temporary =
        destination.parent_path()
        / (destination.filename().wstring() + L".writing");
    const auto temporary_wide = temporary.wstring();
    const auto destination_wide = destination.wstring();
    const HANDLE file = CreateFileW(
        temporary_wide.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        setError(error, "export temporary file could not be created");
        return false;
    }
    std::size_t written_total = 0;
    while (written_total < bytes.size()) {
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(
            bytes.size() - written_total,
            static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        DWORD written = 0;
        if (!WriteFile(
                file,
                bytes.data() + written_total,
                chunk,
                &written,
                nullptr)
            || written != chunk) {
            CloseHandle(file);
            DeleteFileW(temporary_wide.c_str());
            setError(error, "export temporary file could not be written");
            return false;
        }
        written_total += written;
    }
    if (!FlushFileBuffers(file)) {
        CloseHandle(file);
        DeleteFileW(temporary_wide.c_str());
        setError(error, "export temporary file could not be written");
        return false;
    }
    CloseHandle(file);
    if (!MoveFileExW(
            temporary_wide.c_str(),
            destination_wide.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary_wide.c_str());
        setError(error, "export file could not be replaced");
        return false;
    }
    return true;
}

bool writeMgstcToneSetFile(
    const std::filesystem::path& destination,
    const MgstcToneSetDocument& document,
    std::string* error) {
    const auto text = serializeMgstcToneSet(document, error);
    if (!text) {
        return false;
    }
    return replaceFileBytesAtomically(destination, *text, error);
}

bool toneVisibleInSetFilter(
    const std::uint64_t timbre_id,
    const std::uint64_t composite_id,
    const ToneSetListFilter filter,
    const std::uint64_t set_id,
    const std::vector<ToneSetMemberRecord>& members) {
    if (filter == ToneSetListFilter::All) {
        return true;
    }
    const auto same_tone = [&](const ToneSetMemberRecord& member) {
        return timbre_id != 0
            ? member.timbre_id == timbre_id && member.composite_id == 0
            : member.composite_id == composite_id && member.timbre_id == 0;
    };
    if (filter == ToneSetListFilter::Unassigned) {
        return std::none_of(members.begin(), members.end(), same_tone);
    }
    return std::any_of(
        members.begin(),
        members.end(),
        [&](const ToneSetMemberRecord& member) {
            return member.set_id == set_id && same_tone(member);
        });
}

ToneSetMembershipAppend appendToneSetMember(
    std::vector<ToneSetMemberRecord>& members,
    const std::uint64_t set_id,
    const std::uint64_t timbre_id,
    const std::uint64_t composite_id) {
    if (set_id == 0 || (timbre_id == 0) == (composite_id == 0)) {
        return ToneSetMembershipAppend::Rejected;
    }
    for (const auto& member : members) {
        if (member.set_id == set_id
            && member.timbre_id == timbre_id
            && member.composite_id == composite_id) {
            return ToneSetMembershipAppend::AlreadyPresent;
        }
    }
    int position = 0;
    for (const auto& member : members) {
        if (member.set_id != set_id) {
            continue;
        }
        if (member.position == std::numeric_limits<int>::max()) {
            return ToneSetMembershipAppend::Rejected;
        }
        position = std::max(position, member.position + 1);
    }
    members.push_back(ToneSetMemberRecord{
        set_id, timbre_id, composite_id, position});
    return ToneSetMembershipAppend::Added;
}

}  // namespace mgstc::engine
