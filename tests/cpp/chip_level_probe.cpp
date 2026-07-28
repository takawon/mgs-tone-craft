#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string_view>

#include "mgstc/engine/chip_rack.hpp"
#include "mgstc/engine/register_write.hpp"

namespace {

using mgstc::engine::ChipId;
using mgstc::engine::ChipRack;
using mgstc::engine::RegisterWrite;
using mgstc::engine::WriteReason;

struct Level {
    double peak{};
    double rms{};
};

template <typename Selector>
Level measure(ChipRack& rack, Selector select) {
    constexpr int kWarmupFrames = 4'800;
    constexpr int kMeasureFrames = 48'000;
    for (int frame = 0; frame < kWarmupFrames; ++frame) {
        static_cast<void>(rack.renderSample());
    }

    double peak = 0.0;
    double square_sum = 0.0;
    for (int frame = 0; frame < kMeasureFrames; ++frame) {
        const auto sample = static_cast<double>(select(rack.renderSample()));
        peak = std::max(peak, std::abs(sample));
        square_sum += sample * sample;
    }
    return {peak, std::sqrt(square_sum / kMeasureFrames)};
}

void print(std::string_view name, Level level) {
    std::cout << std::left << std::setw(8) << name
              << " peak=" << std::fixed << std::setprecision(8) << level.peak
              << " rms=" << level.rms << '\n';
}

}  // namespace

int main() {
    ChipRack psg;
    const std::array<RegisterWrite, 4> psg_setup{{
        {0, 0, ChipId::Psg, 0, 0, 0xAB, 0, WriteReason::Frequency},
        {0, 1, ChipId::Psg, 0, 1, 0x01, 0, WriteReason::Frequency},
        {0, 2, ChipId::Psg, 0, 7, 0xBE, 0, WriteReason::Patch},
        {0, 3, ChipId::Psg, 0, 8, 0x0F, 0, WriteReason::Volume},
    }};
    if (!psg.apply(psg_setup)) {
        return 1;
    }

    ChipRack scc;
    std::uint32_t sequence = 0;
    for (std::uint8_t index = 0; index < 32; ++index) {
        if (!scc.apply({
                0,
                sequence++,
                ChipId::Scc,
                0,
                index,
                static_cast<std::uint8_t>(index < 16 ? 0x7F : 0x80),
                3,
                WriteReason::Patch,
            })) {
            return 1;
        }
    }
    const std::array<RegisterWrite, 4> scc_setup{{
        {0, sequence++, ChipId::Scc, 1, 0, 0xAB, 3, WriteReason::Frequency},
        {0, sequence++, ChipId::Scc, 1, 1, 0x01, 3, WriteReason::Frequency},
        {0, sequence++, ChipId::Scc, 2, 0, 0x0F, 3, WriteReason::Volume},
        {0, sequence++, ChipId::Scc, 3, 0, 0x01, 3, WriteReason::KeyOn},
    }};
    if (!scc.apply(scc_setup)) {
        return 1;
    }

    ChipRack opll;
    const std::array<std::uint8_t, 8> original_patch{
        0x21, 0x21, 0x00, 0x00, 0xF0, 0xF0, 0x00, 0x00,
    };
    for (std::uint8_t address = 0; address < original_patch.size(); ++address) {
        if (!opll.apply({
                0,
                address,
                ChipId::Opll,
                0,
                address,
                original_patch[address],
                8,
                WriteReason::Patch,
            })) {
            return 1;
        }
    }
    const std::array<RegisterWrite, 3> opll_setup{{
        {0, 8, ChipId::Opll, 0, 0x30, 0x00, 8, WriteReason::Patch},
        {0, 9, ChipId::Opll, 0, 0x20, 0x16, 8, WriteReason::KeyOn},
        {0, 10, ChipId::Opll, 0, 0x10, 0xAC, 8, WriteReason::Frequency},
    }};
    if (!opll.apply(opll_setup)) {
        return 1;
    }

    print("PSG", measure(psg, [](const auto& sample) { return sample.psg; }));
    print("SCC", measure(scc, [](const auto& sample) { return sample.scc; }));
    print("OPLL", measure(opll, [](const auto& sample) { return sample.opll; }));
    return 0;
}
