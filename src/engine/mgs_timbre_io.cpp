#include "mgstc/engine/mgs_timbre_io.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <vector>

namespace mgstc::engine {
namespace {

struct DefinitionBody {
    std::uint16_t number{};
    std::string body;
};

std::string withoutComments(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    bool comment{};
    for (const char character : text) {
        if (comment) {
            if (character == '\r' || character == '\n') {
                comment = false;
                result.push_back(character);
            }
            continue;
        }
        if (character == ';') {
            comment = true;
            continue;
        }
        result.push_back(character);
    }
    return result;
}

std::optional<DefinitionBody> findDefinition(
    std::string_view text,
    char kind) {
    const std::string cleaned = withoutComments(text);
    for (std::size_t start = 0; start + 2 < cleaned.size(); ++start) {
        if (cleaned[start] != '@'
            || std::tolower(
                static_cast<unsigned char>(cleaned[start + 1]))
                != kind) {
            continue;
        }
        std::size_t position = start + 2;
        const std::size_t number_start = position;
        while (position < cleaned.size()
               && std::isdigit(
                   static_cast<unsigned char>(cleaned[position]))) {
            ++position;
        }
        if (position == number_start) {
            continue;
        }
        unsigned long number{};
        try {
            number = std::stoul(
                cleaned.substr(number_start, position - number_start));
        } catch (...) {
            continue;
        }
        if (number > 65535) {
            continue;
        }
        while (position < cleaned.size()
               && std::isspace(
                   static_cast<unsigned char>(cleaned[position]))) {
            ++position;
        }
        if (position >= cleaned.size() || cleaned[position] != '=') {
            continue;
        }
        ++position;
        while (position < cleaned.size()
               && std::isspace(
                   static_cast<unsigned char>(cleaned[position]))) {
            ++position;
        }
        if (position >= cleaned.size() || cleaned[position] != '{') {
            continue;
        }
        const std::size_t body_start = ++position;
        const std::size_t body_end = cleaned.find('}', body_start);
        if (body_end == std::string::npos) {
            return std::nullopt;
        }
        return DefinitionBody{
            .number = static_cast<std::uint16_t>(number),
            .body = cleaned.substr(body_start, body_end - body_start),
        };
    }
    return std::nullopt;
}

std::vector<std::string> tokens(std::string body) {
    std::replace(body.begin(), body.end(), ',', ' ');
    std::istringstream stream(body);
    std::vector<std::string> result;
    std::string token;
    while (stream >> token) {
        result.push_back(token);
    }
    return result;
}

bool parseDecimal(
    std::string_view token,
    int minimum,
    int maximum,
    std::uint8_t& value) {
    if (token.empty()
        || !std::all_of(
            token.begin(),
            token.end(),
            [](char character) {
                return std::isdigit(
                    static_cast<unsigned char>(character));
            })) {
        return false;
    }
    unsigned long parsed{};
    try {
        parsed = std::stoul(std::string(token));
    } catch (...) {
        return false;
    }
    if (parsed < static_cast<unsigned long>(minimum)
        || parsed > static_cast<unsigned long>(maximum)) {
        return false;
    }
    value = static_cast<std::uint8_t>(parsed);
    return true;
}

bool parseHexByte(
    std::string_view token,
    std::uint8_t& value) {
    if (token.empty() || token.size() > 2
        || !std::all_of(
            token.begin(),
            token.end(),
            [](char character) {
                return std::isxdigit(
                    static_cast<unsigned char>(character));
            })) {
        return false;
    }
    unsigned long parsed{};
    try {
        parsed = std::stoul(std::string(token), nullptr, 16);
    } catch (...) {
        return false;
    }
    if (parsed > 0xFF) {
        return false;
    }
    value = static_cast<std::uint8_t>(parsed);
    return true;
}

std::string singleLineComment(std::string_view text) {
    std::string result(text);
    std::replace(result.begin(), result.end(), '\r', ' ');
    std::replace(result.begin(), result.end(), '\n', ' ');
    return result;
}

void appendOperatorLine(
    std::ostringstream& output,
    const OpllOperatorParameters& parameters) {
    const std::array<int, 11> values{
        parameters.attack_rate,
        parameters.decay_rate,
        parameters.sustain_level,
        parameters.release_rate,
        parameters.key_scale_level,
        parameters.multiplier,
        parameters.amplitude_modulation ? 1 : 0,
        parameters.pitch_modulation ? 1 : 0,
        parameters.sustained_tone ? 1 : 0,
        parameters.key_rate_scaling ? 1 : 0,
        parameters.waveform ? 1 : 0,
    };
    output << "  ";
    for (std::size_t index = 0; index < values.size(); ++index) {
        output << std::setw(2) << values[index];
        if (index + 1 < values.size()) {
            output << ",";
        }
    }
}

}  // namespace

std::string formatMgsOpllDefinition(
    const OpllPatchParameters& patch,
    std::uint16_t number,
    std::string_view voice_name) {
    std::ostringstream output;
    output << "@v" << number << " = {";
    const auto comment = singleLineComment(voice_name);
    if (!comment.empty()) {
        output << " ; " << comment;
    }
    output << "\r\n; TL FB\r\n  "
           << std::setw(2)
           << static_cast<int>(patch.modulator.total_level)
           << ","
           << std::setw(2)
           << static_cast<int>(patch.feedback)
           << ",\r\n; AR DR SL RR KL MT AM VB EG KR DT\r\n";
    appendOperatorLine(output, patch.modulator);
    output << ",\r\n";
    appendOperatorLine(output, patch.carrier);
    output << " }\r\n";
    return output.str();
}

std::string formatMgsSccDefinition(
    const SccWaveform& waveform,
    std::uint16_t number,
    std::string_view voice_name) {
    std::ostringstream output;
    output << "@s" << number << " = {";
    const auto comment = singleLineComment(voice_name);
    if (!comment.empty()) {
        output << " ; " << comment;
    }
    output << "\r\n";
    output << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < waveform.size(); ++index) {
        if (index % 8 == 0) {
            output << "  ";
        }
        output << std::setw(2)
               << static_cast<int>(
                   static_cast<std::uint8_t>(waveform[index]));
        output << (index % 8 == 7 ? "\r\n" : " ");
    }
    output << "}\r\n";
    return output.str();
}

std::optional<MgsOpllDefinition>
parseMgsOpllDefinition(std::string_view text) {
    const auto definition = findDefinition(text, 'v');
    if (!definition || definition->number > 31) {
        return std::nullopt;
    }
    const auto values = tokens(definition->body);
    if (values.size() != 24) {
        return std::nullopt;
    }
    std::array<std::uint8_t, 24> parsed{};
    constexpr std::array<int, 24> maximums{
        63, 7,
        15, 15, 15, 15, 3, 15, 1, 1, 1, 1, 1,
        15, 15, 15, 15, 3, 15, 1, 1, 1, 1, 1};
    for (std::size_t index = 0; index < parsed.size(); ++index) {
        if (!parseDecimal(
                values[index],
                0,
                maximums[index],
                parsed[index])) {
            return std::nullopt;
        }
    }
    const auto decode_operator = [&parsed](std::size_t offset) {
        return OpllOperatorParameters{
            .amplitude_modulation = parsed[offset + 6] != 0,
            .pitch_modulation = parsed[offset + 7] != 0,
            .sustained_tone = parsed[offset + 8] != 0,
            .key_rate_scaling = parsed[offset + 9] != 0,
            .multiplier = parsed[offset + 5],
            .key_scale_level = parsed[offset + 4],
            .total_level = 0,
            .waveform = parsed[offset + 10] != 0,
            .attack_rate = parsed[offset],
            .decay_rate = parsed[offset + 1],
            .sustain_level = parsed[offset + 2],
            .release_rate = parsed[offset + 3],
        };
    };
    auto modulator = decode_operator(2);
    auto carrier = decode_operator(13);
    modulator.total_level = parsed[0];
    return MgsOpllDefinition{
        .number = definition->number,
        .patch = {
            .modulator = modulator,
            .carrier = carrier,
            .feedback = parsed[1],
        },
    };
}

std::optional<MgsSccDefinition>
parseMgsSccDefinition(std::string_view text) {
    const auto definition = findDefinition(text, 's');
    if (!definition || definition->number > 31) {
        return std::nullopt;
    }
    const auto values = tokens(definition->body);
    std::vector<std::uint8_t> bytes;
    bytes.reserve(32);
    for (const auto& value : values) {
        if (value.empty() || value.size() % 2 != 0) {
            return std::nullopt;
        }
        for (std::size_t offset = 0;
             offset < value.size();
             offset += 2) {
            std::uint8_t parsed{};
            if (!parseHexByte(
                    std::string_view(value).substr(offset, 2),
                    parsed)) {
                return std::nullopt;
            }
            bytes.push_back(parsed);
        }
    }
    if (bytes.size() != 32) {
        return std::nullopt;
    }
    SccWaveform waveform{};
    for (std::size_t index = 0; index < waveform.size(); ++index) {
        waveform[index] = static_cast<std::int8_t>(bytes[index]);
    }
    return MgsSccDefinition{
        .number = definition->number,
        .waveform = waveform,
    };
}

}  // namespace mgstc::engine
