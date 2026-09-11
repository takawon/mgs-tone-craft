#include "mgstc/engine/gzip_inflate.hpp"
#include "tone_import_internal.hpp"

#include <unordered_set>

namespace mgstc::engine {
namespace {

std::uint32_t readU32(std::span<const std::uint8_t> bytes, std::size_t at) {
    return static_cast<std::uint32_t>(bytes[at])
        | (static_cast<std::uint32_t>(bytes[at + 1]) << 8U)
        | (static_cast<std::uint32_t>(bytes[at + 2]) << 16U)
        | (static_cast<std::uint32_t>(bytes[at + 3]) << 24U);
}

struct OpllWindow {
    bool active{};
    std::uint8_t mask{};
    std::array<std::uint8_t, 8> values{};
    std::uint32_t samples{};
    std::uint32_t commands{};
};

struct SccWindow {
    bool active{};
    std::uint32_t mask{};
    std::array<std::uint8_t, 32> values{};
    std::uint32_t samples{};
    std::uint32_t commands{};
};

void reset(OpllWindow& window) {
    window = {};
}

void reset(SccWindow& window) {
    window = {};
}

bool expired(std::uint32_t samples, std::uint32_t commands) {
    return samples > kVgmCandidateMaxSamples
        || commands > kVgmCandidateMaxCommands;
}

std::string hexKey(std::span<const std::uint8_t> bytes) {
    std::string key;
    key.resize(bytes.size() * 2);
    constexpr char digits[] = "0123456789ABCDEF";
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        key[index * 2] = digits[bytes[index] >> 4];
        key[index * 2 + 1] = digits[bytes[index] & 0x0F];
    }
    return key;
}

}  // namespace

bool looksLikeVgm(std::span<const std::uint8_t> bytes) {
    return bytes.size() >= 0x40
        && bytes[0] == 'V'
        && bytes[1] == 'g'
        && bytes[2] == 'm'
        && bytes[3] == ' ';
}

ToneImportResult importVgm(std::span<const std::uint8_t> bytes) {
    ToneImportResult result;
    result.format = ToneImportFormat::Vgm;
    if (!looksLikeVgm(bytes)) {
        result.errors.emplace_back("not a VGM file");
        return result;
    }
    const auto version = readU32(bytes, 0x08);
    const auto relative = readU32(bytes, 0x34);
    std::size_t position = version < 0x150
        ? 0x40
        : static_cast<std::size_t>(0x34 + relative);
    if (position >= bytes.size()) {
        result.errors.emplace_back("invalid VGM data offset");
        return result;
    }

    OpllWindow opll;
    std::array<SccWindow, 5> scc{};
    std::unordered_set<std::string> seen_opll;
    std::unordered_set<std::string> seen_scc;

    const auto tickWait = [&](std::uint32_t samples) {
        if (opll.active) {
            opll.samples += samples;
            if (expired(opll.samples, opll.commands)) {
                reset(opll);
            }
        }
        for (auto& window : scc) {
            if (!window.active) {
                continue;
            }
            window.samples += samples;
            if (expired(window.samples, window.commands)) {
                reset(window);
            }
        }
    };

    const auto bumpCommands = [&] {
        if (opll.active) {
            ++opll.commands;
            if (expired(opll.samples, opll.commands)) {
                reset(opll);
            }
        }
        for (auto& window : scc) {
            if (!window.active) {
                continue;
            }
            ++window.commands;
            if (expired(window.samples, window.commands)) {
                reset(window);
            }
        }
    };

    const auto completeOpll = [&] {
        if (opll.mask != 0xFF) {
            return;
        }
        const auto key = hexKey(opll.values);
        if (seen_opll.insert(key).second) {
            tone_import_detail::addCandidate(
                result.candidates,
                tone_import_detail::makeOpllCandidate(
                    decodeOpllPatch(opll.values), {}, "VGM"));
        }
        reset(opll);
    };

    const auto completeScc = [&](SccWindow& window) {
        if (window.mask != 0xFFFFFFFFU) {
            return;
        }
        const auto key = hexKey(window.values);
        if (seen_scc.insert(key).second) {
            tone_import_detail::addCandidate(
                result.candidates,
                tone_import_detail::makeSccCandidate(
                    sccWaveformFromBytes(window.values), {}, "VGM"));
        }
        reset(window);
    };

    const auto writeOpll = [&](std::uint8_t reg, std::uint8_t value) {
        if (reg > 7) {
            bumpCommands();
            return;
        }
        if (!opll.active) {
            reset(opll);
            opll.active = true;
        }
        opll.values[reg] = value;
        opll.mask = static_cast<std::uint8_t>(opll.mask | (1U << reg));
        ++opll.commands;
        if (expired(opll.samples, opll.commands)) {
            reset(opll);
            return;
        }
        completeOpll();
    };

    const auto writeSccWave = [&](int channel, std::uint8_t offset, std::uint8_t value) {
        if (channel < 0 || channel > 4 || offset > 31) {
            bumpCommands();
            return;
        }
        auto& window = scc[static_cast<std::size_t>(channel)];
        if (!window.active) {
            reset(window);
            window.active = true;
        }
        window.values[offset] = value;
        window.mask |= (1U << offset);
        ++window.commands;
        if (expired(window.samples, window.commands)) {
            reset(window);
            return;
        }
        completeScc(window);
    };

    while (position < bytes.size()) {
        const auto opcode = bytes[position++];
        if (opcode == 0x66) {
            break;
        }
        if (opcode == 0x61) {
            if (position + 1 >= bytes.size()) {
                break;
            }
            const auto samples = static_cast<std::uint32_t>(
                bytes[position]
                | (static_cast<std::uint16_t>(bytes[position + 1]) << 8U));
            position += 2;
            tickWait(samples);
            continue;
        }
        if (opcode == 0x62) {
            tickWait(735);
            continue;
        }
        if (opcode == 0x63) {
            tickWait(882);
            continue;
        }
        if (opcode >= 0x70 && opcode <= 0x7F) {
            tickWait((opcode & 0x0F) + 1);
            continue;
        }
        if (opcode >= 0x80 && opcode <= 0x8F) {
            tickWait(opcode & 0x0F);
            if (position < bytes.size()) {
                ++position;
            }
            continue;
        }
        if (opcode == 0x51 || opcode == 0xA1) {
            if (position + 1 >= bytes.size()) {
                break;
            }
            const auto reg = bytes[position++];
            const auto value = bytes[position++];
            writeOpll(reg, value);
            continue;
        }
        if (opcode == 0xD2) {
            if (position + 2 >= bytes.size()) {
                break;
            }
            const auto port = bytes[position++];
            const auto offset = bytes[position++];
            const auto value = bytes[position++];
            static_cast<void>(port);
            int channel = -1;
            std::uint8_t wave_offset = 0;
            if (offset < 0x80) {
                channel = offset / 32;
                wave_offset = static_cast<std::uint8_t>(offset % 32);
            } else if (offset >= 0xA0 && offset < 0xC0) {
                channel = 4;
                wave_offset = static_cast<std::uint8_t>(offset - 0xA0);
            }
            if (channel >= 0) {
                writeSccWave(channel, wave_offset, value);
            } else {
                bumpCommands();
            }
            continue;
        }
        if (opcode == 0x67) {
            if (position + 6 > bytes.size()) {
                break;
            }
            const auto size = readU32(bytes, position + 2);
            position += 6 + size;
            bumpCommands();
            continue;
        }
        std::size_t skip = 0;
        if (opcode == 0x4F || opcode == 0x50 || opcode == 0x30 || opcode == 0x31) {
            skip = 1;
        } else if ((opcode >= 0x40 && opcode <= 0x5F)
            || (opcode >= 0xA0 && opcode <= 0xBF)) {
            skip = 2;
        } else if (opcode >= 0xC0 && opcode <= 0xDF) {
            skip = 3;
        } else if (opcode >= 0xE0) {
            skip = 4;
        }
        if (position + skip > bytes.size()) {
            break;
        }
        position += skip;
        bumpCommands();
    }
    return result;
}

}  // namespace mgstc::engine
