#include "tone_import_internal.hpp"

#include <algorithm>
#include <cctype>

namespace mgstc::engine {
namespace {

constexpr std::size_t kInstrumentSize = 40;
constexpr std::size_t kMinInstruments = 1;
constexpr std::size_t kMaxInstruments = 32;

bool printableName(std::span<const std::uint8_t> name) {
    int printable = 0;
    for (const auto byte : name) {
        if (byte == 0) {
            continue;
        }
        if (byte < 0x20 && byte != 0) {
            return false;
        }
        if (byte >= 0x20 && byte < 0x7F) {
            ++printable;
        } else if (byte >= 0x80) {
            ++printable;
        }
    }
    return printable >= 2;
}

std::span<const std::uint8_t> instrumentTable(
    std::span<const std::uint8_t> bytes) {
    if (bytes.size() >= 7 && bytes[0] == 0xFE) {
        return bytes.subspan(7);
    }
    return bytes;
}

}  // namespace

bool looksLikeSccMusixxSng(std::span<const std::uint8_t> bytes) {
    const auto table = instrumentTable(bytes);
    if (table.size() < kInstrumentSize
        || table.size() % kInstrumentSize != 0) {
        return false;
    }
    const auto count = table.size() / kInstrumentSize;
    if (count < kMinInstruments || count > kMaxInstruments) {
        return false;
    }
    int named = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const auto name = table.subspan(index * kInstrumentSize, 8);
        if (printableName(name)) {
            ++named;
        }
    }
    return named > 0;
}

ToneImportResult importSccMusixxSng(std::span<const std::uint8_t> bytes) {
    ToneImportResult result;
    result.format = ToneImportFormat::SccMusixxSng;
    if (!looksLikeSccMusixxSng(bytes)) {
        result.errors.emplace_back("not an SCC-Musixx instrument table");
        return result;
    }
    const auto table = instrumentTable(bytes);
    const auto count = table.size() / kInstrumentSize;
    for (std::size_t index = 0; index < count; ++index) {
        const auto record = table.subspan(index * kInstrumentSize, kInstrumentSize);
        const auto name_bytes = record.subspan(0, 8);
        const auto wave_bytes = record.subspan(8, 32);
        if (tone_import_detail::decodeCp932Name(name_bytes).empty()
            && tone_import_detail::allZero(wave_bytes)) {
            continue;
        }
        std::array<std::uint8_t, 32> wave{};
        std::copy(wave_bytes.begin(), wave_bytes.end(), wave.begin());
        tone_import_detail::addCandidate(
            result.candidates,
            tone_import_detail::makeSccCandidate(
                sccWaveformFromBytes(wave),
                tone_import_detail::decodeCp932Name(name_bytes),
                "SNG"));
    }
    return result;
}

}  // namespace mgstc::engine
