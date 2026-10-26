// SPDX-License-Identifier: AGPL-3.0-only
#include "../../src/app/scc_numeric_wave.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace mgstc::app;

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

SccNumericWave parse(std::string_view text, int resolution = 256,
    bool allow_shared = false) {
    auto result = parseSccNumericWave(text, resolution, allow_shared);
    require(result.value.has_value() && result.error == SccNumericError::None,
        "valid numeric wave rejected");
    return *result.value;
}

void testExpansion() {
    const auto low = parse("0", 16).waveform();
    const auto high = parse("15", 16).waveform();
    for (std::size_t i = 0; i < low.size(); ++i) {
        require(low[i] == -128, "zero must be bottom at all 32 samples");
        require(high[i] == 127, "maximum must be top at all 32 samples");
    }
    const auto half = parse("0 15", 16).waveform();
    const auto quarter = parse("0 15 15 15", 16).waveform();
    const auto alternating = parse("0 15 0 15", 16).waveform();
    for (std::size_t i = 0; i < half.size(); ++i) {
        require(half[i] == (i < 16 ? -128 : 127), "half hold distribution");
        require(quarter[i] == (i < 8 ? -128 : 127), "quarter hold distribution");
        require(alternating[i] == ((i / 8) % 2 == 0 ? -128 : 127),
            "alternating hold distribution");
    }
    for (const auto count : {3, 5, 17, 32}) {
        std::string text;
        for (int i = 0; i < count; ++i) {
            if (!text.empty()) text += ' ';
            text += std::to_string(i % 16);
        }
        const auto waveform = parse(text, 16).waveform();
        for (std::size_t i = 0; i < waveform.size(); ++i) {
            const auto input_index = static_cast<int>(i) * count / 32;
            require(waveform[i] == 17 * (input_index % 16) - 128,
                "uneven count must use floor(i * N / 32), with no interpolation");
        }
    }
    std::string text;
    for (int i = 0; i < 32; ++i) {
        if (!text.empty()) text += ' ';
        text += std::to_string(i * 8 + 7);
    }
    const auto identity = parse(text).waveform();
    for (std::size_t i = 0; i < identity.size(); ++i)
        require(identity[i] == static_cast<int>(i) * 8 + 7 - 128,
            "256-resolution 32-value mapping must be exact");
}

void testResolutionAndRounding() {
    const auto source = parse("0 8 15", 16);
    const auto expanded = source.requantized(256);
    require(expanded.text() == "0 136 255" && expanded.resolution == 256,
        "16 to 256 scaling");
    require(expanded.waveform() == source.waveform(), "exact expansion changed wave");
    require(expanded.requantized(16) == source, "16 to 256 round trip");
    require(parse("0 128 255").requantized(16).text() == "0 8 15",
        "256 to 16 quantization");

    for (const auto resolution : kSccNumericResolutions) {
        for (int value = 0; value < resolution; ++value) {
            const auto input = parse(std::to_string(value), resolution);
            const auto sample = static_cast<int>(std::round(
                255.0 * value / (resolution - 1))) - 128;
            for (const auto actual : input.waveform())
                require(actual == sample, "numeric conversion rounding");
            for (const auto target : kSccNumericResolutions) {
                const auto converted = input.requantized(target);
                const auto expected = static_cast<int>(std::round(
                    static_cast<double>(value) * (target - 1) / (resolution - 1)));
                require(converted.values.size() == 1 && converted.values[0] == expected,
                    "resolution conversion rounding or input count");
            }
        }
    }
    require(source.requantized(17) == source, "invalid target resolution changed data");
}

void testSharingAndExternalSync() {
    for (const auto resolution : kSccNumericResolutions) {
        const auto original = parse("0 " + std::to_string(resolution - 1), resolution);
        const auto shared = original.sharedText();
        require(shared == "SCCWAV L=" + std::to_string(resolution)
            + " 0 " + std::to_string(resolution - 1), "share text format");
        const auto pasted = parseSccNumericWave(shared, 256, true);
        require(pasted.shared && pasted.value && pasted.value->values == original.values && pasted.value->resolution == original.resolution,
            "share paste did not restore original resolution and two input values");
        require(pasted.value->waveform() == original.waveform(),
            "share round trip changed waveform");
    }
    require(parse("  000  015  ", 16).text() == "  000  015  ", "spaces/leading zeros");
    require(parse("0 15", 16, true).resolution == 16,
        "plain paste must use selected resolution");
    require(parse("SCCWAV   L=16   0 15 ", 256, true).values.size() == 2,
        "sharing whitespace rejected");
    for (int first = -128; first <= 96; first += 32) {
        mgstc::engine::SccWaveform waveform;
        for (std::size_t i = 0; i < waveform.size(); ++i)
            waveform[i] = static_cast<std::int8_t>(first + static_cast<int>(i));
        const auto synced = SccNumericWave::fromWaveform(waveform);
        require(synced.resolution == 256 && synced.values.size() == 32,
            "external waveform must sync as 256-resolution 32-value text");
        require(synced.waveform() == waveform && parse(synced.text()).waveform() == waveform,
            "external waveform signed-to-unsigned conversion lost a sample");
    }
    const auto compressed = parse("0 15", 16);
    const auto external = SccNumericWave::fromWaveform(compressed.waveform());
    require(compressed != external && compressed.waveform() == external.waveform(),
        "equal waveform must allow distinct editable resolution and value count");
}

void testRejections() {
    const auto reject = [](std::string_view text, int resolution,
        SccNumericError error, bool allow_shared = false) {
        const auto parsed = parseSccNumericWave(text, resolution, allow_shared);
        require(!parsed.value && parsed.error == error, "incorrect invalid-input result");
    };
    reject("", 256, SccNumericError::Empty);
    reject("   ", 16, SccNumericError::Empty);
    reject("0", 17, SccNumericError::InvalidResolution);
    for (const auto text : {"+1", "1.5", "0,1", "1\t2", "1\n2",
             "0x10", "one", "1a", "\xEF\xBC\x91"})
        reject(text, 256, SccNumericError::InvalidCharacter);
    reject("16", 16, SccNumericError::OutOfRange);
    reject("256", 256, SccNumericError::OutOfRange);
    reject("99999999999999999999999999999999999", 256, SccNumericError::OutOfRange);
    std::string too_many = "0";
    for (int i = 1; i < 33; ++i) too_many += " 0";
    reject(too_many, 256, SccNumericError::TooManyValues);
    reject("SCCWAV/2 L=16 0 15", 256, SccNumericError::UnsupportedFormat, true);
    reject("SCCWAV", 256, SccNumericError::UnsupportedFormat, true);
    reject("SCCWAVX L=16 0", 256, SccNumericError::UnsupportedFormat, true);
    reject("SCCWAV L=17 0", 256, SccNumericError::InvalidResolution, true);
    reject("SCCWAV l=16 0", 256, SccNumericError::InvalidResolution, true);
    reject("SCCWAV L=-16 0", 256, SccNumericError::InvalidResolution, true);
    reject("SCCWAV L=99999999999999999999 0", 256,
        SccNumericError::InvalidResolution, true);
    const auto header_only = parseSccNumericWave("  SCCWAV   L=16  ", 256, true);
    require(header_only.incomplete && header_only.error == SccNumericError::None
        && !header_only.value && header_only.header_resolution == 16,
        "complete header-only input must be a normal incomplete edit");
    require(std::string("  SCCWAV   L=16  ").substr(header_only.header_resolution_offset,
        header_only.header_resolution_length) == "16", "header digit span");
    reject("SCCWAV L=16 16", 256, SccNumericError::OutOfRange, true);
    reject("SCCWAV L=16 0", 256, SccNumericError::InvalidCharacter);
}
int sample(int value, int resolution = 16) {
    return static_cast<int>(std::round(255.0 * value / (resolution - 1))) - 128;
}

double oracleCurve(std::string_view op, double t) {
    const double pi = std::acos(-1.0);
    if (op == "-") return t;
    if (op == ">") return 1.0 - std::cos(pi * t / 2.0);
    if (op == "<") return std::sin(pi * t / 2.0);
    if (op == "><") return (1.0 - std::cos(pi * t)) / 2.0;
    return t <= 0.5 ? std::sin(pi * t) / 2.0
        : 1.0 - std::sin(pi * (1.0 - t)) / 2.0;
}

void expectHold(const mgstc::engine::SccWaveform& wave, int start, int width,
    int value, int resolution = 16) {
    for (int i = 0; i < width; ++i)
        require(wave[static_cast<std::size_t>(start + i)] == sample(value, resolution),
            "incorrect literal hold width or value");
}

void expectRamp(const mgstc::engine::SccWaveform& wave, int start, int width,
    int a, int b, std::string_view op, int resolution = 16, bool include_end = true) {
    for (int i = 0; i < width; ++i) {
        const double t = width > 1
            ? static_cast<double>(i) / (include_end ? width - 1 : width) : 0.0;
        const int quantized = std::clamp(static_cast<int>(std::round(
            a + (b - a) * oracleCurve(op, t))), 0, resolution - 1);
        require(wave[static_cast<std::size_t>(start + i)] == sample(quantized, resolution),
            "interpolation curve, endpoints, or two-stage quantization differ");
    }
}

void testCurves() {
    for (const auto resolution : kSccNumericResolutions) {
        const int maximum = resolution - 1;
        for (const std::string op : {"-", ">", "<", "><", "<>"}) {
            expectRamp(parse("0" + op + std::to_string(maximum), resolution).waveform(),
                0, 32, 0, maximum, op, resolution);
            expectRamp(parse(std::to_string(maximum) + op + "0", resolution).waveform(),
                0, 32, maximum, 0, op, resolution);
            expectRamp(parse("3" + op + std::to_string(maximum), resolution).waveform(),
                0, 32, 3, maximum, op, resolution);
            require(parse("0" + op + "0", resolution).waveform() == parse("0", resolution).waveform(),
                "flat interpolation must remain constant");
        }
    }
    for (const std::string op : {"-", ">", "<", "><", "<>"}) {
        const auto joined = parse("0" + op + "15", 16).waveform();
        for (const auto spacing : {"0 " + op + " 15", "0" + op + " 15", "0 " + op + "15"})
            require(parse(spacing,16).waveform() == joined, "operator-side whitespace changes timing");
    }
    for (const std::string op : {"-", "><"}) {
        const auto triangle = parse("0" + op + "15" + op + "0",16).waveform();
        expectRamp(triangle,0,16,0,15,op,16,false);
        expectRamp(triangle,16,16,15,0,op,16,false);
    }
    const auto sine = parse("8<15>8<0>8",16).waveform();
    expectRamp(sine,0,8,8,15,"<",16,false);
    expectRamp(sine,8,8,15,8,">",16,false);
    expectRamp(sine,16,8,8,0,"<",16,false);
    expectRamp(sine,24,8,0,8,">",16,false);
    const auto in = parse("0>255").waveform();
    const auto out = parse("0<255").waveform();
    const auto linear = parse("0-255").waveform();
    require(in[8] < linear[8] && linear[8] < out[8], "one-sided curve shape direction");
    require(parse("0><255").waveform()[8] < linear[8]
        && parse("0<>255").waveform()[8] > linear[8], "two-sided curve shapes");
    require(parse("0\xE2\x80\x90" "15",16).waveform() == parse("0-15",16).waveform(),
        "U+2010 hyphen alias");
}

void testPeriodicEndpointReproduction() {
    const auto actual = parse("SCCWAV L=256 127<255>127<0>127",256,true).waveform();
    const mgstc::engine::SccWaveform expected{
        -1,24,48,70,90,105,117,125,
        127,125,117,105,90,70,48,24,
        -1,-26,-50,-72,-91,-107,-118,-126,
        -128,-126,-118,-107,-91,-72,-50,-26};
    if (actual != expected) {
        std::cerr << "SCCWAV L=256 127<255>127<0>127 actual DEC:";
        for (const auto value : actual) std::cerr << ' ' << static_cast<int>(value);
        std::cerr << '\n';
    }
    require(actual == expected, "periodic endpoint reproduction differs from all 32 required samples");
    require(std::count(actual.begin(),actual.end(),127) == 1 && actual[8] == 127,
        "periodic peak must occur exactly once at sample 8");
    require(std::count(actual.begin(),actual.end(),-128) == 1 && actual[24] == -128,
        "periodic trough must occur exactly once at sample 24");
    require(std::adjacent_find(actual.begin(),actual.end()) == actual.end(),
        "high-resolution reproduction must not duplicate interior connection samples");
    require(actual.front() - actual.back() == actual[1] - actual.front(),
        "last-to-first periodic step must match the first outgoing step");
}

void testChainEndpointPolicies() {
    for (const auto resolution : kSccNumericResolutions) {
        const auto maximum = resolution - 1;
        for (const std::string op : {"-", ">", "<", "><", "<>"}) {
            const auto closed = parse("0" + op + std::to_string(maximum) + op + "0",resolution).waveform();
            expectRamp(closed,0,16,0,maximum,op,resolution,false);
            expectRamp(closed,16,16,maximum,0,op,resolution,false);
            const auto open = parse("0" + op + std::to_string(maximum) + op + "3",resolution).waveform();
            expectRamp(open,0,16,0,maximum,op,resolution,false);
            expectRamp(open,16,16,maximum,3,op,resolution,true);
            require(open.back() == sample(3,resolution), "open multi-segment chain must reach final value");
        }
    }
    const auto holds = parse("0-15 0",16).waveform();
    expectRamp(holds,0,16,0,15,"-"); expectHold(holds,16,16,0);
    const auto independent = parse("(0-15) (15-0)",16).waveform();
    expectRamp(independent,0,16,0,15,"-"); expectRamp(independent,16,16,15,0,"-");
    const auto across = parse("(0-15)>(0-15)",16).waveform();
    expectRamp(across,0,16,0,15,"-",16,false);
    expectRamp(across,16,8,15,0,">",16,false);
    expectRamp(across,24,8,0,15,"-",16,true);
    const auto closed_groups = parse("(0-15)>(0)",16).waveform();
    expectRamp(closed_groups,0,16,0,15,"-",16,false);
    expectRamp(closed_groups,16,16,15,0,">",16,false);
    require(closed_groups == parse("(0-15)(>0)",16).waveform(),
        "explicit cross-group closed connection must preserve equivalent syntax");
    std::string narrow;
    for (int i=0; i<29; ++i) narrow += "0 ";
    for (const auto tail : {"(8-15-0)", "(8-15-8)"}) {
        const auto tiny = parse(narrow + tail,16).waveform();
        expectHold(tiny,0,31,0); expectHold(tiny,31,1,8);
    }
}

void testGroupsAndCrossConnections() {
    const auto half = parse("0 15",16).waveform();
    require(parse("0 (15)",16).waveform() == half
        && parse("0 (15 15)",16).waveform() == half, "group occupies one parent time width");
    const auto flat_three = parse("0 15 15",16).waveform();
    expectHold(flat_three,0,11,0);
    expectHold(flat_three,11,21,15);
    require(flat_three != half, "grouping must change time allocation");
    const auto group = parse("0 (15 0)",16).waveform();
    expectHold(group,0,16,0); expectHold(group,16,8,15); expectHold(group,24,8,0);
    const auto pair = parse("(0 15) (15 0)",16).waveform();
    expectHold(pair,0,8,0); expectHold(pair,8,16,15); expectHold(pair,24,8,0);
    const auto nested = parse("0 (15 (0 8 15))",16).waveform();
    expectHold(nested,0,16,0); expectHold(nested,16,8,15);
    expectHold(nested,24,3,0); expectHold(nested,27,3,8); expectHold(nested,30,2,15);
    const auto nested_ramp = parse("0 (15 (0-15))",16).waveform();
    expectHold(nested_ramp,0,16,0); expectHold(nested_ramp,16,8,15);
    expectRamp(nested_ramp,24,8,0,15,"-");
    const auto nested_pair = parse("0 (15 (0 15))",16).waveform();
    expectHold(nested_pair,0,16,0); expectHold(nested_pair,16,8,15);
    expectHold(nested_pair,24,4,0); expectHold(nested_pair,28,4,15);
    const auto mixed = parse("0>15<0",16).waveform();
    expectRamp(mixed,0,16,0,15,">",16,false); expectRamp(mixed,16,16,15,0,"<",16,false);
    const auto shared_curve = parse("SCCWAV L=256 128><255><0",256,true).waveform();
    expectRamp(shared_curve,0,16,128,255,"><",256,false);
    expectRamp(shared_curve,16,16,255,0,"><",256);
    const auto separate_chains = parse("0-15 15-0",16).waveform();
    expectRamp(separate_chains,0,16,0,15,"-"); expectRamp(separate_chains,16,16,15,0,"-");
    const auto group_chains = parse("(0><15) (15><0)",16).waveform();
    expectRamp(group_chains,0,16,0,15,"><"); expectRamp(group_chains,16,16,15,0,"><");
    for (const std::string op : {"-", ">", "<", "><", "<>"}) {
        const auto outside = parse("0" + op + "(15 0)",16);
        const auto inside = parse("0(" + op + "15 0)",16);
        require(outside != inside && outside.waveform() == inside.waveform(),
            "cross-bracket equivalent expressions must retain distinct text and identical samples");
        expectHold(outside.waveform(),0,16,0);
        expectRamp(outside.waveform(),16,8,0,15,op);
        expectHold(outside.waveform(),24,8,0);
    }
    const auto left = parse("(0 15)>0",16).waveform();
    expectHold(left,0,8,0); expectHold(left,8,8,15); expectRamp(left,16,16,15,0,">");
    const auto both = parse("(0 8)>(15 0)",16).waveform();
    require(both == parse("(0 8)(>15 0)",16).waveform(), "two groups cross connection equivalence");
    expectHold(both,0,8,0); expectHold(both,8,8,8);
    expectRamp(both,16,8,8,15,">"); expectHold(both,24,8,0);
    const auto deep = parse("0>((15 0) 8)",16).waveform();
    require(deep == parse("0((>15 0) 8)",16).waveform(), "nested group cross connection equivalence");
    expectHold(deep,0,16,0); expectRamp(deep,16,4,0,15,">");
    expectHold(deep,20,4,0); expectHold(deep,24,8,8);
    for (const std::string inner : {"-", "<"}) {
        const auto chain = parse("0>(15" + inner + "0)",16).waveform();
        require(chain == parse("0(>15" + inner + "0)",16).waveform(),
            "incoming segment and nested chain equivalence");
        expectHold(chain,0,16,0);
        expectRamp(chain,16,8,0,15,">",16,false); expectRamp(chain,24,8,15,0,inner,16,false);
    }
    require(parse("0>15 0",16).waveform() != parse("0>(15 0)",16).waveform(),
        "unbracketed chain cannot borrow the destination group timing");
    require(parse("0 (0-15)",16).waveform() != parse("0-15",16).waveform(),
        "independent parent time width must not be merged into a chain");
}

void testExpressionStateAndLimits() {
    const std::string plain = " 0 > (08  15) ";
    const auto state = parse(plain,16);
    require(state.text() == plain, "parser must retain complete raw expression");
    require(state.requantized(256).text() == " 0 > (136  255) ",
        "requantization must preserve bracket/operator/space structure");
    const auto with_header = parse(" SCCWAV   L=16  0>(8 15) ",256,true);
    require(with_header.text() == " SCCWAV   L=16  0>(8 15) ", "direct shared input text retained");
    require(with_header.requantized(256).text() == " SCCWAV   L=256  0>(136 255) ",
        "header and literal values must change together");
    require(with_header.sharedText() == "SCCWAV L=16  0>(8 15) ", "shared header must occur once");
    for (const std::string expression : {"SCCWAV L=16 0>(15 0)",
             "SCCWAV L=16 0(>15 0)", "SCCWAV L=256 128<255>128<0>128",
             "SCCWAV L=256 128><255><0", "SCCWAV L=16 8<15>8<0>8"}) {
        const auto original = parse(expression,256,true);
        const auto copied = parse(original.sharedText(),16,true);
        require(copied.text() == expression && copied.waveform() == original.waveform(),
            "sharing must preserve expression structure and samples");
    }
    std::string very_narrow;
    for (int i = 0; i < 30; ++i) very_narrow += "0 ";
    for (const auto end : {"(0 15)", "(0-15)"}) {
        const auto tiny = parse(very_narrow + end,16);
        expectHold(tiny.waveform(),0,32,0);
        require(tiny.waveform() == parse(very_narrow + end,16).waveform(),
            "zero-width/one-width generation must be deterministic");
    }
    std::string deepest(kSccNumericMaxDepth,'(');
    deepest += '0'; deepest.append(kSccNumericMaxDepth,')');
    expectHold(parse(deepest,16).waveform(),0,32,0);
    require(parseSccNumericWave("(" + deepest + ")",16).error == SccNumericError::TooDeep,
        "nesting safety limit");
    require(parseSccNumericWave(std::string(kSccNumericMaxInputBytes+1,'0'),16).error
        == SccNumericError::InputTooLong, "input byte safety limit");
    const std::string body = "0>(15 0)";
    const auto padded = std::string(kSccNumericMaxInputBytes - body.size() - 5, ' ') + body;
    const auto edge = parse(padded,16);
    require(parse(edge.sharedText(),256,true).waveform() == edge.waveform(),
        "near-limit valid expression must survive shared header addition");
    const auto grown = edge.requantized(256);
    require(grown.resolution == 256 && grown.values == std::vector<int>({0,255,0})
        && grown.text().find("0>(255 0)") != std::string::npos,
        "near-limit requantization must not silently keep old resolution");
    require(!parseSccNumericWave(" " + padded,16).value,
        "future numeric digit reservation bounds accepted padded expressions");
    const std::string header = "SCCWAV L=16 ";
    const auto full = parse(std::string(kSccNumericMaxInputBytes - header.size() - 1,' ')
        + header + padded,256,true);
    require(full.requantized(256).resolution == 256
        && parse(full.sharedText(),16,true).waveform() == full.waveform(),
        "independent header and body budgets support sharing and header digit growth");
    for (const auto bad : {"0>(>15 0)", "0 (15 0", "0 15)", "0 ()", "0>",
             "0 > <15", "0--15", ">15", "-1", "(0 15))", "((0 15)", "()0"}) {
        const auto parsed = parseSccNumericWave(bad,16);
        require(!parsed.value && parsed.error == SccNumericError::InvalidSyntax,
            "malformed syntax must never yield a partial wave");
    }
    require(parseSccNumericWave("SCCNUM/1 L=16 0 15",256,true).error
        == SccNumericError::UnsupportedFormat, "retired sharing format must be rejected");
}
} // namespace

int main() {
    try {
        testExpansion();
        testPeriodicEndpointReproduction();
        testChainEndpointPolicies();
        testResolutionAndRounding();
        testSharingAndExternalSync();
        testRejections();
        testCurves();
        testGroupsAndCrossConnections();
        testExpressionStateAndLimits();
        std::cout << "SCC numeric waveform tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
