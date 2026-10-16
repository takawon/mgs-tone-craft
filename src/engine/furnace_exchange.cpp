#include "mgstc/engine/furnace_exchange.hpp"

#include "mgstc/engine/mgs_envelope_io.hpp"
#include "mgstc/engine/opll_patch.hpp"
#include "mgstc/engine/scc_morph.hpp"

#include <algorithm>
#include <cstring>
#include <optional>
#include <set>
#include <sstream>
#include <utility>

namespace mgstc::engine {
namespace {

constexpr std::string_view kInstrumentMagic = "-Furnace instr.-";
constexpr std::string_view kWavetableMagic = "-Furnace waveta-";
constexpr int kMacroTickLimit = 4096;

class Cursor {
public:
    explicit Cursor(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    [[nodiscard]] bool ok() const noexcept { return !failed_; }
    [[nodiscard]] std::size_t tell() const noexcept { return pos_; }
    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }

    void seek(std::size_t position) {
        if (position > bytes_.size()) {
            failed_ = true;
            pos_ = bytes_.size();
            return;
        }
        pos_ = position;
    }

    std::uint8_t u8() {
        if (pos_ >= bytes_.size()) {
            failed_ = true;
            return 0;
        }
        return bytes_[pos_++];
    }

    std::uint16_t u16() {
        const auto low = u8();
        const auto high = u8();
        return static_cast<std::uint16_t>(low | (high << 8));
    }

    std::uint32_t u32() {
        const auto low = u16();
        const auto high = u16();
        return static_cast<std::uint32_t>(low)
            | (static_cast<std::uint32_t>(high) << 16);
    }

    std::int32_t i32() {
        return static_cast<std::int32_t>(u32());
    }

    std::string zstr() {
        std::string text;
        while (ok()) {
            const auto byte = u8();
            if (!ok() || byte == 0) {
                break;
            }
            text.push_back(static_cast<char>(byte));
            if (text.size() > 256) {
                failed_ = true;
                break;
            }
        }
        return text;
    }

    void skip(std::size_t count) {
        if (pos_ + count > bytes_.size()) {
            failed_ = true;
            pos_ = bytes_.size();
            return;
        }
        pos_ += count;
    }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t pos_{};
    bool failed_{};
};

void appendU16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

void appendU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    appendU16(out, static_cast<std::uint16_t>(value & 0xFFFF));
    appendU16(out, static_cast<std::uint16_t>((value >> 16) & 0xFFFF));
}

void appendFeature(
    std::vector<std::uint8_t>& out,
    char first,
    char second,
    const std::vector<std::uint8_t>& payload) {
    out.push_back(static_cast<std::uint8_t>(first));
    out.push_back(static_cast<std::uint8_t>(second));
    appendU16(out, static_cast<std::uint16_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
}

std::string safeStem(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (const unsigned char byte : name) {
        const bool forbidden = byte < 32
            || byte == '\\' || byte == '/' || byte == ':' || byte == '*'
            || byte == '?' || byte == '"' || byte == '<' || byte == '>'
            || byte == '|';
        out.push_back(forbidden ? '_' : static_cast<char>(byte));
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
        out.pop_back();
    }
    if (out.size() > 60) {
        out.resize(60);
    }
    if (out.empty()) {
        out = "timbre";
    }
    return out;
}

std::string displayName(std::string_view preferred, std::string_view fallback) {
    if (!preferred.empty()) {
        return std::string(preferred);
    }
    auto name = std::string(fallback);
    const auto slash = name.find_last_of("\\/");
    if (slash != std::string::npos) {
        name.erase(0, slash + 1);
    }
    const auto dot = name.find_last_of('.');
    if (dot != std::string::npos) {
        name.erase(dot);
    }
    if (name.empty()) {
        name = "Furnace";
    }
    return name;
}

struct Wave {
    std::string name;
    int width{};
    int maximum{255};
    std::vector<int> samples;
};

struct SequenceMacro {
    int code{};
    int length{};
    int loop{255};
    int release{255};
    int type{};
    int delay{};
    int speed{1};
    std::vector<int> values;
};

struct Operator {
    int am{};
    int vib{};
    int ksr{};
    int mult{};
    int tl{};
    int ksl{};
    int ar{};
    int dr{};
    int sl{};
    int rr{};
    int ssg{};
    int sus{};
};

struct Instrument {
    int version{};
    int type{-1};
    std::string name;
    bool fm{};
    int alg{};
    int fb{};
    int fms{};
    int ams{};
    int preset{};
    Operator ops[2]{};
    std::vector<SequenceMacro> macros;
    std::vector<SequenceMacro> modulator_macros;
    bool wavetable_synth{};
    std::vector<std::string> notes;
    std::vector<Wave> waves;
    std::vector<int> wave_indexes;
};

struct TickSequence {
    std::vector<int> values;
    std::optional<std::uint32_t> loop_start;
    bool converted{};
    std::string warning;
};

bool readWave(Cursor& cursor, Wave& wave) {
    char magic[4] = {};
    for (auto& byte : magic) {
        byte = static_cast<char>(cursor.u8());
    }
    if (!cursor.ok() || std::string_view(magic, 4) != "WAVE") {
        return false;
    }
    static_cast<void>(cursor.u32());
    wave.name = cursor.zstr();
    wave.width = static_cast<int>(cursor.u32());
    static_cast<void>(cursor.u32());
    wave.maximum = static_cast<int>(cursor.u32());
    if (!cursor.ok() || wave.width < 1 || wave.width > 256) {
        return false;
    }
    wave.samples.resize(static_cast<std::size_t>(wave.width));
    for (int index = 0; index < wave.width; ++index) {
        wave.samples[static_cast<std::size_t>(index)] = cursor.i32();
    }
    return cursor.ok();
}

SccWaveform sccFromWave(const Wave& wave, bool& converted, std::string& note) {
    converted = wave.width != 32;
    SccWaveform waveform{};
    if (wave.width < 1 || wave.samples.empty()) {
        return waveform;
    }
    const bool silent = wave.maximum < 1;
    int maximum = wave.maximum;
    if (maximum > 255) {
        maximum = 255;
        note = "波形の高さが 255 を超えたため 255 に制限しました";
    } else if (!silent && maximum != 255) {
        note = "波形の高さを 255 段階へ整数倍率で写しました";
    }
    const int count = std::min(
        wave.width, static_cast<int>(wave.samples.size()));
    for (int index = 0; index < 32; ++index) {
        int source = (index * count) / 32;
        if (source >= count) {
            source = count - 1;
        }
        int sample = silent
            ? 0
            : wave.samples[static_cast<std::size_t>(source)];
        if (sample < 0) {
            sample = 0;
        }
        if (!silent && maximum > 0) {
            sample = sample * 255 / maximum;
        }
        if (sample > 255) {
            sample = 255;
        }
        waveform[static_cast<std::size_t>(index)] =
            static_cast<std::int8_t>(sample - 128);
    }
    return waveform;
}

TickSequence expandSequence(const SequenceMacro& macro, int low, int high) {
    TickSequence sequence;
    if (macro.length <= 0 || macro.values.empty()) {
        return sequence;
    }
    if (macro.type != 0) {
        sequence.warning = "ADSR/LFO マクロは変換しません";
        return sequence;
    }
    if (macro.speed < 1) {
        sequence.warning = "Speed が 1 未満のマクロは変換しません";
        return sequence;
    }
    int count = std::min(macro.length, static_cast<int>(macro.values.size()));
    bool dropped_release = false;
    if (macro.release >= 0 && macro.release < count) {
        count = macro.release;
        dropped_release = true;
    }
    if (count <= 0) {
        sequence.warning = "Release より前のマクロが空のため変換しません";
        return sequence;
    }
    bool clamped = false;
    std::uint32_t tick = 0;
    for (int index = 0; index < count; ++index) {
        if (macro.loop >= 0 && macro.loop < count && index == macro.loop) {
            sequence.loop_start = tick;
        }
        const int duration = index == 0
            ? 1 + std::max(macro.delay, 0)
            : macro.speed;
        int value = macro.values[static_cast<std::size_t>(index)];
        if (value < low || value > high) {
            clamped = true;
            value = std::clamp(value, low, high);
        }
        for (int step = 0; step < duration; ++step) {
            if (sequence.values.size() >= kMacroTickLimit || tick > 65535) {
                sequence.values.clear();
                sequence.loop_start.reset();
                sequence.warning = "マクロが長すぎるため変換しません";
                return sequence;
            }
            sequence.values.push_back(value);
            ++tick;
        }
    }
    sequence.converted = true;
    if (dropped_release) {
        sequence.warning = "キーオフ後の Release は取り込みません";
    } else if (clamped) {
        sequence.warning = "マクロの値が音源の範囲外だったため制限しました";
    }
    return sequence;
}

bool readNewMacros(Cursor& cursor, std::size_t end, std::vector<SequenceMacro>& macros) {
    const auto header = cursor.u16();
    if (!cursor.ok() || header < 8) {
        return false;
    }
    while (cursor.ok() && cursor.tell() < end) {
        const int code = cursor.u8();
        if (!cursor.ok()) {
            return false;
        }
        if (code == 255) {
            break;
        }
        const auto header_end = cursor.tell() - 1 + header;
        if (header_end > end) {
            return false;
        }
        SequenceMacro macro;
        macro.code = code;
        macro.length = cursor.u8();
        macro.loop = cursor.u8();
        macro.release = cursor.u8();
        static_cast<void>(cursor.u8());
        const int flags = cursor.u8();
        macro.delay = cursor.u8();
        macro.speed = cursor.u8();
        macro.type = (flags >> 1) & 0x03;
        const int word = flags >> 6;
        if (!cursor.ok()) {
            return false;
        }
        cursor.seek(header_end);
        macro.values.reserve(static_cast<std::size_t>(macro.length));
        for (int index = 0; index < macro.length; ++index) {
            int value = 0;
            if (word == 0) {
                value = cursor.u8();
            } else if (word == 1) {
                value = static_cast<std::int8_t>(cursor.u8());
            } else if (word == 2) {
                value = static_cast<std::int16_t>(cursor.u16());
            } else {
                value = cursor.i32();
            }
            macro.values.push_back(value);
        }
        if (!cursor.ok() || cursor.tell() > end) {
            return false;
        }
        macros.push_back(std::move(macro));
    }
    return cursor.ok();
}

bool readNewOperators(Cursor& cursor, int count, Instrument& instrument) {
    const int stored = std::clamp(count, 0, 4);
    for (int index = 0; index < stored; ++index) {
        Operator op;
        const auto first = cursor.u8();
        op.ksr = (first & 0x80) != 0;
        op.mult = first & 0x0F;
        const auto second = cursor.u8();
        op.sus = (second & 0x80) != 0;
        op.tl = second & 0x7F;
        const auto third = cursor.u8();
        op.vib = (third & 0x20) != 0;
        op.ar = third & 0x1F;
        const auto fourth = cursor.u8();
        op.am = (fourth & 0x80) != 0;
        op.ksl = (fourth >> 5) & 0x03;
        op.dr = fourth & 0x1F;
        static_cast<void>(cursor.u8());
        const auto sixth = cursor.u8();
        op.sl = (sixth >> 4) & 0x0F;
        op.rr = sixth & 0x0F;
        op.ssg = cursor.u8() & 0x0F;
        static_cast<void>(cursor.u8());
        if (index < 2) {
            instrument.ops[index] = op;
        }
    }
    return cursor.ok();
}

bool readNewInstrument(Cursor& cursor, Instrument& instrument) {
    instrument.version = cursor.u16();
    instrument.type = cursor.u16();
    if (!cursor.ok()) {
        return false;
    }
    if (instrument.version > kFurnaceEngineVersion0683) {
        instrument.notes.push_back(
            "Furnace 0.6.8.3 より新しい版のファイルです");
    }
    while (cursor.ok() && cursor.tell() + 4 <= cursor.size()) {
        const char first = static_cast<char>(cursor.u8());
        const char second = static_cast<char>(cursor.u8());
        if (first == 'E' && second == 'N') {
            break;
        }
        const auto length = cursor.u16();
        const auto end = cursor.tell() + length;
        if (!cursor.ok() || end > cursor.size()) {
            return false;
        }
        if (first == 'N' && second == 'A') {
            instrument.name = cursor.zstr();
        } else if (first == 'F' && second == 'M') {
            const int flag = cursor.u8();
            const int op_count = flag & 0x0F;
            const auto base0 = cursor.u8();
            const auto base1 = cursor.u8();
            const auto base2 = cursor.u8();
            instrument.alg = (base0 >> 4) & 0x07;
            instrument.fb = base0 & 0x07;
            instrument.ams = (base1 >> 3) & 0x03;
            instrument.fms = base1 & 0x07;
            instrument.preset = base2 & 0x1F;
            if (instrument.version >= 224) {
                static_cast<void>(cursor.u8());
            }
            if (!readNewOperators(cursor, op_count, instrument)) {
                return false;
            }
            instrument.fm = true;
        } else if (first == 'M' && second == 'A') {
            if (!readNewMacros(cursor, end, instrument.macros)) {
                return false;
            }
        } else if (first == 'O' && second == '1') {
            if (!readNewMacros(cursor, end, instrument.modulator_macros)) {
                return false;
            }
        } else if (first == 'W' && second == 'S') {
            instrument.wavetable_synth = true;
            instrument.notes.push_back("Wavetable Synth は変換しません");
        } else if (first == 'W' && second == 'L') {
            const int count = cursor.u8();
            if (count > 256) {
                return false;
            }
            std::vector<int> indexes(static_cast<std::size_t>(count));
            std::vector<std::uint32_t> pointers(static_cast<std::size_t>(count));
            for (int index = 0; index < count; ++index) {
                indexes[static_cast<std::size_t>(index)] = cursor.u8();
            }
            for (int index = 0; index < count; ++index) {
                pointers[static_cast<std::size_t>(index)] = cursor.u32();
            }
            const auto resume = end;
            for (int index = 0; index < count; ++index) {
                cursor.seek(pointers[static_cast<std::size_t>(index)]);
                Wave wave;
                if (!readWave(cursor, wave)) {
                    return false;
                }
                instrument.wave_indexes.push_back(
                    indexes[static_cast<std::size_t>(index)]);
                instrument.waves.push_back(std::move(wave));
            }
            cursor.seek(resume);
        } else if (!(first == 'S' && (second == 'L'))) {
            const std::string code{first, second};
            instrument.notes.push_back("未対応の機能 " + code + " は読み飛ばしました");
        }
        cursor.seek(end);
    }
    return cursor.ok();
}

bool readOldInstrument(Cursor& cursor, Instrument& instrument) {
    instrument.version = cursor.u16();
    static_cast<void>(cursor.u16());
    const auto data_ptr = cursor.u32();
    const int wave_count = cursor.u16();
    const int sample_count = cursor.u16();
    static_cast<void>(cursor.u32());
    if (!cursor.ok() || wave_count > 256 || sample_count > 256) {
        return false;
    }
    std::vector<std::uint32_t> wave_ptrs(static_cast<std::size_t>(wave_count));
    for (int index = 0; index < wave_count; ++index) {
        wave_ptrs[static_cast<std::size_t>(index)] = cursor.u32();
    }
    cursor.skip(static_cast<std::size_t>(sample_count) * 4);
    for (int index = 0; index < wave_count; ++index) {
        cursor.seek(wave_ptrs[static_cast<std::size_t>(index)]);
        Wave wave;
        if (!readWave(cursor, wave)) {
            return false;
        }
        instrument.wave_indexes.push_back(index);
        instrument.waves.push_back(std::move(wave));
    }
    cursor.seek(data_ptr);
    char magic[4] = {};
    for (auto& byte : magic) {
        byte = static_cast<char>(cursor.u8());
    }
    if (std::string_view(magic, 4) != "INST") {
        return false;
    }
    static_cast<void>(cursor.u32());
    static_cast<void>(cursor.u16());
    instrument.type = cursor.u8();
    static_cast<void>(cursor.u8());
    instrument.name = cursor.zstr();
    instrument.alg = cursor.u8();
    instrument.fb = cursor.u8();
    instrument.fms = cursor.u8();
    instrument.ams = cursor.u8();
    static_cast<void>(cursor.u8());
    const int preset = cursor.u8();
    instrument.preset = instrument.version >= 60 ? preset : 0;
    cursor.skip(2);
    for (int index = 0; index < 4; ++index) {
        Operator op;
        op.am = cursor.u8();
        op.ar = cursor.u8();
        op.dr = cursor.u8();
        op.mult = cursor.u8();
        op.rr = cursor.u8();
        op.sl = cursor.u8();
        op.tl = cursor.u8();
        cursor.skip(4);
        op.ssg = cursor.u8();
        cursor.skip(3);
        op.ksl = cursor.u8();
        op.sus = cursor.u8();
        op.vib = cursor.u8();
        cursor.skip(1);
        op.ksr = cursor.u8();
        cursor.skip(12);
        if (index < 2) {
            instrument.ops[index] = op;
        }
    }
    instrument.fm = cursor.ok();
    cursor.skip(4);
    cursor.skip(24);
    cursor.skip(16);
    auto readLengths = [&](int count, std::vector<int>& lengths) {
        for (int index = 0; index < count; ++index) {
            const auto length = cursor.u32();
            if (length > 255) {
                return false;
            }
            lengths.push_back(static_cast<int>(length));
        }
        return cursor.ok();
    };
    std::vector<int> lengths;
    std::vector<int> loops;
    const int macro_count = instrument.version >= 17 ? 8 : 4;
    if (!readLengths(macro_count, lengths)) {
        return false;
    }
    if (!readLengths(macro_count, loops)) {
        return false;
    }
    cursor.skip(4);
    for (int index = 0; index < 4; ++index) {
        SequenceMacro macro;
        macro.code = index;
        macro.length = lengths[static_cast<std::size_t>(index)];
        macro.loop = loops[static_cast<std::size_t>(index)];
        macro.speed = 1;
        for (int step = 0; step < macro.length; ++step) {
            macro.values.push_back(cursor.i32());
        }
        instrument.macros.push_back(std::move(macro));
    }
    return cursor.ok();
}

const SequenceMacro* findMacro(const std::vector<SequenceMacro>& macros, int code) {
    for (const auto& macro : macros) {
        if (macro.code == code && macro.length > 0) {
            return &macro;
        }
    }
    return nullptr;
}

const Wave* waveAt(const Instrument& instrument, int furnace_index) {
    for (std::size_t index = 0; index < instrument.wave_indexes.size(); ++index) {
        if (instrument.wave_indexes[index] == furnace_index
            && index < instrument.waves.size()) {
            return &instrument.waves[index];
        }
    }
    if (furnace_index >= 0
        && static_cast<std::size_t>(furnace_index) < instrument.waves.size()
        && instrument.wave_indexes.empty()) {
        return &instrument.waves[static_cast<std::size_t>(furnace_index)];
    }
    return nullptr;
}

OpllPatchParameters patchFromInstrument(const Instrument& instrument) {
    const auto convert = [](const Operator& op) {
        OpllOperatorParameters parameters;
        parameters.amplitude_modulation = op.am != 0;
        parameters.pitch_modulation = op.vib != 0;
        parameters.sustained_tone = (op.ssg & 0x08) != 0;
        parameters.key_rate_scaling = op.ksr != 0;
        parameters.multiplier = static_cast<std::uint8_t>(op.mult & 0x0F);
        parameters.key_scale_level = static_cast<std::uint8_t>(op.ksl & 0x03);
        parameters.total_level = static_cast<std::uint8_t>(op.tl & 0x3F);
        parameters.attack_rate = static_cast<std::uint8_t>(op.ar & 0x0F);
        parameters.decay_rate = static_cast<std::uint8_t>(op.dr & 0x0F);
        parameters.sustain_level = static_cast<std::uint8_t>(op.sl & 0x0F);
        parameters.release_rate = static_cast<std::uint8_t>(op.rr & 0x0F);
        return parameters;
    };
    OpllPatchParameters patch;
    patch.modulator = convert(instrument.ops[0]);
    patch.carrier = convert(instrument.ops[1]);
    patch.carrier.total_level = 0;
    patch.modulator.waveform = (instrument.ams & 0x01) != 0;
    patch.carrier.waveform = (instrument.fms & 0x01) != 0;
    patch.feedback = static_cast<std::uint8_t>(instrument.fb & 0x07);
    return patch;
}

SavedTimbreReference makeOpllSnapshot(
    CompositeTimbre& timbre,
    std::string name,
    const OpllPatchParameters& patch) {
    SavedTimbreReference reference;
    reference.library_id = allocateCompositeOwnedTimbreId(timbre);
    reference.revision = 1;
    reference.name = std::move(name);
    reference.source = TimbreSource::Opll;
    reference.opll_registers = encodeOpllPatch(patch);
    return reference;
}

SavedTimbreReference makeSccSnapshot(
    CompositeTimbre& timbre,
    std::string name,
    const SccWaveform& waveform) {
    SavedTimbreReference reference;
    reference.library_id = allocateCompositeOwnedTimbreId(timbre);
    reference.revision = 1;
    reference.name = std::move(name);
    reference.source = TimbreSource::Scc;
    reference.scc_waveform = sccWaveformToBytes(waveform);
    return reference;
}

void addVolume(
    CompositeLayer& layer,
    const TickSequence& sequence,
    std::vector<ImportWarning>& warnings) {
    if (!sequence.warning.empty()) {
        warnings.push_back({sequence.warning});
    }
    if (!sequence.converted || sequence.values.empty()) {
        return;
    }
    layer.volume = static_cast<std::uint8_t>(
        std::clamp(sequence.values.front(), 0, 15));
    if (std::all_of(
            sequence.values.begin(),
            sequence.values.end(),
            [&](int value) { return value == sequence.values.front(); })
        && !sequence.loop_start) {
        return;
    }
    int previous = -1;
    for (std::uint32_t tick = 0; tick < sequence.values.size(); ++tick) {
        const int value = sequence.values[tick];
        if (value == previous) {
            continue;
        }
        previous = value;
        EnvelopeEvent event;
        event.kind = EnvelopeEventKind::Volume;
        event.value = value;
        event.count = tick;
        event.after_loop_start = sequence.loop_start
            && tick >= *sequence.loop_start;
        layer.volume_envelope.events.push_back(event);
    }
    setEnvelopeTimelineRange(
        layer.envelope_timeline,
        static_cast<std::uint32_t>(sequence.values.size()),
        sequence.loop_start,
        sequence.loop_start
            ? std::optional<std::uint32_t>(
                static_cast<std::uint32_t>(sequence.values.size()))
            : std::nullopt);
}

void addFreeCurve(
    OpllRegisterAutoLane& lane,
    const TickSequence& sequence,
    std::vector<ImportWarning>& warnings) {
    if (!sequence.warning.empty()) {
        warnings.push_back({sequence.warning});
    }
    if (!sequence.converted || sequence.values.empty()) {
        return;
    }
    if (sequence.loop_start) {
        warnings.push_back({"ループする TL/FB マクロは変換しません"});
        return;
    }
    lane.mode = OpllRegisterAutoMode::FreeCurve;
    lane.coarseness = 1;
    lane.free_curve.reserve(sequence.values.size());
    for (const int value : sequence.values) {
        lane.free_curve.push_back(static_cast<std::uint8_t>(value));
    }
}

ImportedToneCandidate sccCandidate(
    std::string name,
    const SccWaveform& waveform,
    bool converted,
    std::string source) {
    ImportedToneCandidate candidate;
    candidate.type = ImportedToneType::Scc;
    candidate.name = std::move(name);
    candidate.source_format = std::move(source);
    candidate.data = waveform;
    candidate.default_register_as = ImportRegisterAs::Scc;
    candidate.register_choices = {ImportRegisterAs::Scc};
    candidate.furnace_wave_converted = converted;
    candidate.tags = {"Furnace"};
    candidate.memo = "Furnace";
    return candidate;
}

ImportedToneCandidate opllCandidate(
    std::string name,
    const OpllPatchParameters& patch) {
    ImportedToneCandidate candidate;
    candidate.type = ImportedToneType::Opll;
    candidate.name = std::move(name);
    candidate.source_format = "FUI";
    candidate.data = patch;
    candidate.default_register_as = ImportRegisterAs::Opll;
    candidate.register_choices = {ImportRegisterAs::Opll};
    candidate.tags = {"Furnace"};
    candidate.memo = "Furnace";
    return candidate;
}

ImportedToneCandidate compositeCandidate(CompositeTimbre timbre) {
    ImportedToneCandidate candidate;
    candidate.type = ImportedToneType::Composite;
    candidate.name = timbre.name;
    candidate.source_format = "FUI";
    candidate.default_register_as = ImportRegisterAs::Composite;
    candidate.register_choices = {ImportRegisterAs::Composite};
    candidate.data = std::move(timbre);
    return candidate;
}

void pushNotes(ImportedToneCandidate& candidate, const Instrument& instrument) {
    for (const auto& note : instrument.notes) {
        candidate.warnings.push_back({note});
    }
}

void failWithNotes(
    ToneImportResult& result,
    const Instrument& instrument,
    std::string error) {
    result.errors.push_back(std::move(error));
    for (const auto& note : instrument.notes) {
        result.errors.push_back(note);
    }
}

bool macroChanges(const SequenceMacro& macro) {
    return std::any_of(
        macro.values.begin(),
        macro.values.end(),
        [&](int value) { return value != macro.values.front(); });
}

ToneImportResult convertInstrument(
    Instrument instrument,
    std::string_view source_name,
    bool old_format) {
    ToneImportResult result;
    result.format = ToneImportFormat::FurnaceInstrument;
    result.source_name = std::string(source_name);
    const auto name = displayName(instrument.name, source_name);
    if (instrument.type != 6 && instrument.type != 13 && instrument.type != 18) {
        result.errors.push_back("この Furnace 音源種別は取り込みません");
        return result;
    }
    if (instrument.type == 13 && instrument.preset == 16) {
        result.errors.push_back("OPLL のドラムは取り込みません");
        return result;
    }
    if (instrument.type == 13 && !instrument.fm) {
        result.errors.push_back("OPLL の FM データがありません");
        return result;
    }
    if (instrument.type == 13 && instrument.preset > 15) {
        result.errors.push_back("未対応の OPLL プリセットです");
        return result;
    }

    const bool old_changing_macro = old_format && std::any_of(
        instrument.macros.begin(),
        instrument.macros.end(),
        [](const SequenceMacro& macro) { return macroChanges(macro); });
    if (old_changing_macro) {
        instrument.notes.push_back("旧形式マクロの時間変化は変換しません");
    }

    auto timbre = defaultCompositeTimbre();
    timbre.name = name;
    timbre.tags = {"Furnace"};
    timbre.memo = "Furnace";
    timbre.layers.clear();
    CompositeLayer layer;
    layer.name = name;
    layer.volume = 15;
    layer.envelope_number = 0;
    std::vector<ImportWarning> warnings;
    bool sampled_converted = false;

    if (instrument.type == 13) {
        layer.source = TimbreSource::Opll;
        layer.opll_sustain = instrument.alg != 0;
        if (instrument.preset >= 1) {
            layer.base_opll_rom =
                static_cast<std::uint8_t>(instrument.preset - 1);
        } else if (instrument.fm) {
            auto snapshot = makeOpllSnapshot(
                timbre, name, patchFromInstrument(instrument));
            layer.base_timbre = snapshot;
            timbre.embedded_timbres.push_back(std::move(snapshot));
        }
        if (instrument.ops[0].sus || instrument.ops[1].sus) {
            warnings.push_back(
                {"オペレータの SUS は YM2413 のサスティンではないため取り込みません"});
        }
        if (instrument.ops[1].tl != 0) {
            warnings.push_back(
                {"キャリア TL は音量オフセットのためユーザー音色には残りません"});
        }
        if (!old_format) {
            if (const auto* volume = findMacro(instrument.macros, 0)) {
                addVolume(layer, expandSequence(*volume, 0, 15), warnings);
            }
            if (const auto* feedback = findMacro(instrument.macros, 9)) {
                addFreeCurve(
                    layer.opll_fb_auto,
                    expandSequence(*feedback, 0, 7),
                    warnings);
            }
            for (const auto& macro : instrument.macros) {
                if (macro.code != 0 && macro.code != 9 && macro.length > 0) {
                    warnings.push_back({"未確認のマクロは変換しません"});
                    break;
                }
            }
            for (const auto& macro : instrument.modulator_macros) {
                if (macro.code == 6) {
                    addFreeCurve(
                        layer.opll_tl_auto,
                        expandSequence(macro, 0, 63),
                        warnings);
                } else if (macro.length > 0) {
                    warnings.push_back(
                        {"モジュレータの TL 以外のマクロは変換しません"});
                    break;
                }
            }
        }
        const bool shaped = !layer.volume_envelope.events.empty()
            || layer.opll_tl_auto.active()
            || layer.opll_fb_auto.active()
            || layer.opll_sustain
            || layer.base_opll_rom.has_value();
        if (!shaped && instrument.preset == 0 && instrument.fm) {
            auto candidate = opllCandidate(name, patchFromInstrument(instrument));
            candidate.warnings = std::move(warnings);
            pushNotes(candidate, instrument);
            result.candidates.push_back(std::move(candidate));
            return result;
        }
        if (layer.opll_sustain && instrument.preset == 0) {
            warnings.push_back(
                {"SUS は単体音色に残らないため複合音色の so にしました"});
        }
    } else if (instrument.type == 18) {
        layer.source = TimbreSource::Scc;
        const SequenceMacro* wave_macro = findMacro(instrument.macros, 3);
        std::vector<int> indexes;
        TickSequence wave_ticks;
        if (wave_macro != nullptr) {
            if (old_format) {
                indexes.push_back(wave_macro->values.front());
            } else {
                wave_ticks = expandSequence(*wave_macro, 0, 255);
                if (!wave_ticks.warning.empty()) {
                    warnings.push_back({wave_ticks.warning});
                }
                if (wave_ticks.converted) {
                    indexes = wave_ticks.values;
                }
            }
        } else if (!instrument.waves.empty()) {
            indexes.push_back(instrument.wave_indexes.empty()
                ? 0
                : instrument.wave_indexes.front());
        }
        if (indexes.empty()) {
            failWithNotes(result, instrument, "SCC の波形がありません");
            return result;
        }
        bool any_converted = false;
        std::vector<SavedTimbreReference> snapshots;
        std::vector<int> snapshot_for_index(256, -1);
        for (const int furnace_index : indexes) {
            if (furnace_index < 0 || furnace_index > 255) {
                warnings.push_back({"波形番号が範囲外のため読み飛ばしました"});
                continue;
            }
            if (snapshot_for_index[static_cast<std::size_t>(furnace_index)] >= 0) {
                continue;
            }
            const auto* wave = waveAt(instrument, furnace_index);
            if (wave == nullptr) {
                warnings.push_back({"参照された波形がファイルにありません"});
                continue;
            }
            bool converted = false;
            std::string note;
            const auto scc = sccFromWave(*wave, converted, note);
            any_converted = any_converted || converted;
            if (!note.empty()) {
                warnings.push_back({note});
            }
            auto snapshot = makeSccSnapshot(
                timbre,
                wave->name.empty() ? name : wave->name,
                scc);
            snapshot_for_index[static_cast<std::size_t>(furnace_index)] =
                static_cast<int>(snapshots.size());
            snapshots.push_back(std::move(snapshot));
        }
        if (snapshots.empty()) {
            failWithNotes(result, instrument, "SCC の波形がありません");
            return result;
        }
        layer.base_timbre = snapshots.front();
        for (const auto& snapshot : snapshots) {
            timbre.embedded_timbres.push_back(snapshot);
        }
        int previous = -1;
        for (std::uint32_t tick = 0; tick < indexes.size(); ++tick) {
            const int furnace_index = indexes[tick];
            if (furnace_index < 0 || furnace_index > 255) {
                continue;
            }
            const int slot =
                snapshot_for_index[static_cast<std::size_t>(furnace_index)];
            if (slot < 0 || slot == previous) {
                continue;
            }
            previous = slot;
            if (tick == 0 && snapshots[static_cast<std::size_t>(slot)].library_id
                == layer.base_timbre->library_id) {
                continue;
            }
            EnvelopeEvent event;
            event.kind = EnvelopeEventKind::Timbre;
            event.count = tick;
            event.timbre_pick = TimbrePick::Library;
            event.target_library_id =
                snapshots[static_cast<std::size_t>(slot)].library_id;
            layer.timbre_automation.push_back(event);
        }
        if (wave_ticks.loop_start) {
            setEnvelopeTimelineRange(
                layer.envelope_timeline,
                static_cast<std::uint32_t>(wave_ticks.values.size()),
                wave_ticks.loop_start,
                static_cast<std::uint32_t>(wave_ticks.values.size()));
        }
        if (!old_format) {
            if (const auto* volume = findMacro(instrument.macros, 0)) {
                addVolume(layer, expandSequence(*volume, 0, 15), warnings);
            }
            for (const auto& macro : instrument.macros) {
                if (macro.code != 0 && macro.code != 3 && macro.length > 0) {
                    warnings.push_back({"未確認のマクロは変換しません"});
                    break;
                }
            }
        }
        sampled_converted = any_converted;
        const bool shaped = !layer.timbre_automation.empty()
            || !layer.volume_envelope.events.empty()
            || wave_ticks.loop_start.has_value();
        if (!shaped) {
            auto candidate = sccCandidate(
                name,
                sccWaveformFromBytes(snapshots.front().scc_waveform),
                any_converted,
                "FUI");
            candidate.warnings.insert(
                candidate.warnings.end(), warnings.begin(), warnings.end());
            pushNotes(candidate, instrument);
            result.candidates.push_back(std::move(candidate));
            return result;
        }
        if (any_converted) {
            warnings.push_back({"32 サンプル以外の波形を既存サンプルだけで 32 個にしました"});
        }
    } else {
        layer.source = TimbreSource::Psg;
        if (const auto* duty = findMacro(instrument.macros, 2)) {
            if (!old_format && duty->type != 0) {
                warnings.push_back({"ADSR/LFO マクロは変換しません"});
            } else {
                int noise = 31 - duty->values.front();
                if (noise < 0 || noise > 31 || duty->values.front() < 0
                    || duty->values.front() > 31) {
                    warnings.push_back({"ノイズ周波数が範囲外だったため制限しました"});
                }
                layer.volume_envelope.rate.noise =
                    static_cast<std::uint8_t>(std::clamp(noise, 0, 31));
                if (!old_format && macroChanges(*duty)) {
                    warnings.push_back({"変化するノイズ周波数は最初の値だけを使います"});
                }
            }
        }
        if (const auto* wave = findMacro(instrument.macros, 3)) {
            if (!old_format && wave->type != 0) {
                warnings.push_back({"ADSR/LFO マクロは変換しません"});
            } else {
                const int mode = wave->values.front();
                layer.volume_envelope.rate.tone_mode =
                    static_cast<std::uint8_t>(mode & 0x03);
                if ((mode & 0x04) != 0) {
                    warnings.push_back({"AY のハードウェアエンベロープは変換しません"});
                }
                if (!old_format && macroChanges(*wave)) {
                    warnings.push_back(
                        {"変化するトーン／ノイズ切替は最初の値だけを使います"});
                }
            }
        }
        if (!old_format) {
            if (const auto* volume = findMacro(instrument.macros, 0)) {
                addVolume(layer, expandSequence(*volume, 0, 15), warnings);
            }
        }
        for (const auto& macro : instrument.macros) {
            if (macro.code != 0 && macro.code != 2 && macro.code != 3
                && macro.length > 0) {
                warnings.push_back({"arp/pitch など未確認のマクロは変換しません"});
                break;
            }
        }
    }

    timbre.layers.push_back(std::move(layer));
    auto candidate = compositeCandidate(std::move(timbre));
    candidate.warnings = std::move(warnings);
    pushNotes(candidate, instrument);
    if (instrument.type == 18) {
        candidate.furnace_wave_converted = sampled_converted;
    }
    result.candidates.push_back(std::move(candidate));
    return result;
}

ToneImportResult importInstrumentBytes(
    std::span<const std::uint8_t> bytes,
    std::string_view source_name) {
    ToneImportResult result;
    result.format = ToneImportFormat::FurnaceInstrument;
    result.source_name = std::string(source_name);
    Cursor cursor(bytes);
    Instrument instrument;
    bool parsed = false;
    if (bytes.size() >= 4
        && (std::memcmp(bytes.data(), "FINS", 4) == 0
            || std::memcmp(bytes.data(), "FINB", 4) == 0)) {
        cursor.skip(4);
        parsed = readNewInstrument(cursor, instrument);
    } else if (bytes.size() >= kInstrumentMagic.size()
        && std::memcmp(bytes.data(), kInstrumentMagic.data(), kInstrumentMagic.size()) == 0) {
        cursor.skip(kInstrumentMagic.size());
        parsed = readOldInstrument(cursor, instrument);
        if (parsed) {
            return convertInstrument(std::move(instrument), source_name, true);
        }
    }
    if (!parsed || !cursor.ok()) {
        result.errors.push_back("Furnace インストゥルメントを読めません");
        result.candidates.clear();
        return result;
    }
    return convertInstrument(std::move(instrument), source_name, false);
}

ToneImportResult importWavetableBytes(
    std::span<const std::uint8_t> bytes,
    std::string_view source_name) {
    ToneImportResult result;
    result.format = ToneImportFormat::FurnaceWavetable;
    result.source_name = std::string(source_name);
    if (bytes.size() < kWavetableMagic.size() + 4
        || std::memcmp(bytes.data(), kWavetableMagic.data(), kWavetableMagic.size()) != 0) {
        result.errors.push_back("Furnace 波形を読めません");
        return result;
    }
    Cursor cursor(bytes);
    cursor.skip(kWavetableMagic.size());
    const int version = cursor.u16();
    static_cast<void>(cursor.u16());
    Wave wave;
    if (!readWave(cursor, wave)) {
        result.errors.push_back("Furnace 波形を読めません");
        return result;
    }
    bool converted = false;
    std::string note;
    const auto waveform = sccFromWave(wave, converted, note);
    auto candidate = sccCandidate(
        displayName(wave.name, source_name),
        waveform,
        converted,
        "FUW");
    if (version > kFurnaceEngineVersion0683) {
        candidate.warnings.push_back(
            {"Furnace 0.6.8.3 より新しい版のファイルです"});
    }
    if (!note.empty()) {
        candidate.warnings.push_back({note});
    }
    result.candidates.push_back(std::move(candidate));
    return result;
}

std::vector<std::uint8_t> waveBlock(std::string_view name, const SccWaveform& waveform) {
    std::vector<std::uint8_t> block;
    block.insert(block.end(), {'W', 'A', 'V', 'E'});
    const auto size_at = block.size();
    appendU32(block, 0);
    const auto payload_at = block.size();
    if (name.empty()) {
        block.push_back(0);
    } else {
        block.insert(block.end(), name.begin(), name.end());
        block.push_back(0);
    }
    appendU32(block, 32);
    appendU32(block, 0);
    appendU32(block, 255);
    for (const auto sample : waveform) {
        appendU32(block, static_cast<std::uint32_t>(
            static_cast<int>(sample) + 128));
    }
    const auto size = static_cast<std::uint32_t>(block.size() - payload_at);
    block[size_at] = static_cast<std::uint8_t>(size & 0xFF);
    block[size_at + 1] = static_cast<std::uint8_t>((size >> 8) & 0xFF);
    block[size_at + 2] = static_cast<std::uint8_t>((size >> 16) & 0xFF);
    block[size_at + 3] = static_cast<std::uint8_t>((size >> 24) & 0xFF);
    return block;
}

void writeOperator(std::vector<std::uint8_t>& payload, const OpllOperatorParameters& op) {
    payload.push_back(static_cast<std::uint8_t>(
        (op.key_rate_scaling ? 0x80 : 0) | (op.multiplier & 0x0F)));
    payload.push_back(static_cast<std::uint8_t>(op.total_level & 0x3F));
    payload.push_back(static_cast<std::uint8_t>(
        (op.pitch_modulation ? 0x20 : 0) | (op.attack_rate & 0x0F)));
    payload.push_back(static_cast<std::uint8_t>(
        (op.amplitude_modulation ? 0x80 : 0)
        | ((op.key_scale_level & 0x03) << 5)
        | (op.decay_rate & 0x0F)));
    payload.push_back(0);
    payload.push_back(static_cast<std::uint8_t>(
        ((op.sustain_level & 0x0F) << 4) | (op.release_rate & 0x0F)));
    payload.push_back(static_cast<std::uint8_t>(op.sustained_tone ? 8 : 0));
    payload.push_back(0);
}

std::vector<std::uint8_t> writeOpll(
    std::string_view name,
    const OpllPatchParameters& patch,
    int preset,
    bool sustain) {
    std::vector<std::uint8_t> out;
    out.insert(out.end(), {'F', 'I', 'N', 'S'});
    appendU16(out, kFurnaceEngineVersion0683);
    appendU16(out, 13);
    std::vector<std::uint8_t> label;
    label.insert(label.end(), name.begin(), name.end());
    label.push_back(0);
    appendFeature(out, 'N', 'A', label);
    std::vector<std::uint8_t> fm;
    fm.push_back(static_cast<std::uint8_t>(0x30 | 2));
    fm.push_back(static_cast<std::uint8_t>(
        ((sustain ? 1 : 0) << 4) | (patch.feedback & 0x07)));
    fm.push_back(static_cast<std::uint8_t>(
        ((patch.modulator.waveform ? 1 : 0) << 3)
        | (patch.carrier.waveform ? 1 : 0)));
    fm.push_back(static_cast<std::uint8_t>(preset & 0x1F));
    fm.push_back(0);
    auto modulator = patch.modulator;
    auto carrier = patch.carrier;
    carrier.total_level = 0;
    writeOperator(fm, modulator);
    writeOperator(fm, carrier);
    appendFeature(out, 'F', 'M', fm);
    return out;
}

std::vector<std::uint8_t> writeSccInstrument(
    std::string_view name,
    const SccWaveform& waveform) {
    std::vector<std::uint8_t> out;
    out.insert(out.end(), {'F', 'I', 'N', 'S'});
    appendU16(out, kFurnaceEngineVersion0683);
    appendU16(out, 18);
    std::vector<std::uint8_t> label;
    label.insert(label.end(), name.begin(), name.end());
    label.push_back(0);
    appendFeature(out, 'N', 'A', label);
    std::vector<std::uint8_t> macros;
    appendU16(macros, 8);
    macros.push_back(3);
    macros.push_back(1);
    macros.push_back(255);
    macros.push_back(255);
    macros.push_back(0);
    macros.push_back(0);
    macros.push_back(0);
    macros.push_back(1);
    macros.push_back(0);
    macros.push_back(255);
    appendFeature(out, 'M', 'A', macros);
    std::vector<std::uint8_t> list;
    list.push_back(1);
    list.push_back(0);
    const auto pointer_at = out.size() + 4 + list.size();
    appendU32(list, 0);
    appendFeature(out, 'W', 'L', list);
    out.insert(out.end(), {'E', 'N'});
    const auto wave_at = out.size();
    const auto wave = waveBlock(name, waveform);
    out.insert(out.end(), wave.begin(), wave.end());
    out[pointer_at] = static_cast<std::uint8_t>(wave_at & 0xFF);
    out[pointer_at + 1] = static_cast<std::uint8_t>((wave_at >> 8) & 0xFF);
    out[pointer_at + 2] = static_cast<std::uint8_t>((wave_at >> 16) & 0xFF);
    out[pointer_at + 3] = static_cast<std::uint8_t>((wave_at >> 24) & 0xFF);
    return out;
}

bool authoredId(const CompositeTimbre& timbre, std::uint64_t id) {
    if (findEmbeddedTimbreSnapshot(timbre, id) != nullptr) {
        return true;
    }
    for (const auto& layer : timbre.layers) {
        if (layer.base_timbre && layer.base_timbre->library_id == id) {
            return true;
        }
    }
    return false;
}

std::vector<std::uint8_t> toneNumbersInEnvelope(std::span<const std::uint8_t> bytecode) {
    std::vector<std::uint8_t> numbers;
    std::size_t position = 0;
    while (position < bytecode.size()) {
        const auto opcode = bytecode[position];
        if (opcode == 0x60) {
            break;
        }
        std::size_t step = 1;
        if (opcode == 0x11) {
            step = 3;
        } else if (opcode == 0x10 || opcode == 0x12
            || (opcode >= 0x20 && opcode <= 0x2F) || opcode >= 0xE0) {
            step = 2;
        }
        if (position + step > bytecode.size()) {
            break;
        }
        if (opcode == 0x10) {
            numbers.push_back(bytecode[position + 1]);
        }
        position += step;
    }
    return numbers;
}

SccWaveform waveOrSilence(const SavedTimbreReference* snapshot) {
    if (snapshot == nullptr || snapshot->source != TimbreSource::Scc) {
        return {};
    }
    return sccWaveformFromBytes(snapshot->scc_waveform);
}

void addWaveFile(
    FurnaceExportBatch& batch,
    int order,
    int tone,
    bool morph,
    std::string_view name,
    const SccWaveform& waveform) {
    FurnaceExportFile file;
    std::ostringstream filename;
    filename << (order < 10 ? "0" : "") << order << "_@" << tone << '_'
             << (morph ? "morph" : "key") << '_' << safeStem(name) << ".fuw";
    file.filename = filename.str();
    file.bytes = writeFurnaceWavetable(name, waveform);
    batch.files.push_back(std::move(file));
}

}  // namespace

ToneImportFormat detectFurnaceToneFormat(
    std::span<const std::uint8_t> bytes) noexcept {
    if (bytes.size() >= 4
        && (std::memcmp(bytes.data(), "FINS", 4) == 0
            || std::memcmp(bytes.data(), "FINB", 4) == 0)) {
        return ToneImportFormat::FurnaceInstrument;
    }
    if (bytes.size() >= kInstrumentMagic.size()
        && std::memcmp(
            bytes.data(), kInstrumentMagic.data(), kInstrumentMagic.size()) == 0) {
        return ToneImportFormat::FurnaceInstrument;
    }
    if (bytes.size() >= kWavetableMagic.size()
        && std::memcmp(
            bytes.data(), kWavetableMagic.data(), kWavetableMagic.size()) == 0) {
        return ToneImportFormat::FurnaceWavetable;
    }
    return ToneImportFormat::Unknown;
}

ToneImportResult importFurnaceTones(
    std::span<const std::uint8_t> bytes,
    std::string_view source_name) {
    const auto format = detectFurnaceToneFormat(bytes);
    if (format == ToneImportFormat::FurnaceWavetable) {
        return importWavetableBytes(bytes, source_name);
    }
    if (format == ToneImportFormat::FurnaceInstrument) {
        return importInstrumentBytes(bytes, source_name);
    }
    ToneImportResult result;
    result.errors.push_back("Furnace のファイルではありません");
    return result;
}

std::vector<std::uint8_t> writeFurnaceWavetable(
    std::string_view name,
    const SccWaveform& waveform) {
    std::vector<std::uint8_t> out;
    out.insert(out.end(), kWavetableMagic.begin(), kWavetableMagic.end());
    appendU16(out, kFurnaceEngineVersion0683);
    appendU16(out, 0);
    const auto block = waveBlock(name, waveform);
    out.insert(out.end(), block.begin(), block.end());
    return out;
}

FurnaceExportBatch exportFurnaceInstruments(const TimbreLibraryEntry& entry) {
    FurnaceExportBatch batch;
    FurnaceExportFile file;
    file.filename = safeStem(entry.name);
    if (entry.category == TimbreCategory::Opll) {
        file.filename += ".fui";
        file.bytes = writeOpll(
            entry.name, decodeOpllPatch(entry.opll_registers), 0, false);
        batch.files.push_back(std::move(file));
        return batch;
    }
    file.filename += ".fui";
    file.bytes = writeSccInstrument(
        entry.name, sccWaveformFromBytes(entry.scc_waveform));
    batch.files.push_back(std::move(file));
    return batch;
}

FurnaceExportBatch exportFurnaceInstruments(const CompositeTimbre& timbre) {
    FurnaceExportBatch batch;
    batch.partial = true;
    batch.warnings.push_back("複合音色はレイヤーごとの静的な値だけを書き出します");
    int order = 1;
    for (const auto& layer : timbre.layers) {
        FurnaceExportFile file;
        std::ostringstream filename;
        filename << (order < 10 ? "0" : "") << order << '_'
                 << safeStem(layer.name.empty() ? timbre.name : layer.name);
        if (layer.source == TimbreSource::Opll && layer.base_opll_rom) {
            filename << ".fui";
            OpllPatchParameters patch;
            file.bytes = writeOpll(
                layer.name,
                patch,
                static_cast<int>(*layer.base_opll_rom) + 1,
                layer.opll_sustain);
        } else if (layer.source == TimbreSource::Opll && layer.base_timbre
            && layer.base_timbre->source == TimbreSource::Opll) {
            filename << ".fui";
            file.bytes = writeOpll(
                layer.name,
                decodeOpllPatch(layer.base_timbre->opll_registers),
                0,
                layer.opll_sustain);
        } else if (layer.source == TimbreSource::Scc && layer.base_timbre) {
            filename << ".fui";
            file.bytes = writeSccInstrument(
                layer.name, sccWaveformFromBytes(layer.base_timbre->scc_waveform));
        } else if (layer.source == TimbreSource::Psg) {
            batch.warnings.push_back("PSG レイヤーのエンベロープは .fui に書き出しません");
            continue;
        } else {
            batch.warnings.push_back("書き出せないレイヤーを飛ばしました");
            continue;
        }
        file.filename = filename.str();
        batch.files.push_back(std::move(file));
        ++order;
    }
    if (batch.files.empty()) {
        batch.partial = false;
    }
    return batch;
}

FurnaceExportBatch exportFurnaceWavetables(const TimbreLibraryEntry& entry) {
    FurnaceExportBatch batch;
    if (entry.category != TimbreCategory::Scc) {
        batch.warnings.push_back(".fuw は SCC の波形だけを書き出します");
        return batch;
    }
    FurnaceExportFile file;
    file.filename = safeStem(entry.name) + ".fuw";
    file.bytes = writeFurnaceWavetable(
        entry.name, sccWaveformFromBytes(entry.scc_waveform));
    batch.files.push_back(std::move(file));
    return batch;
}

FurnaceExportBatch exportFurnaceWavetables(const CompositeTimbre& timbre) {
    FurnaceExportBatch batch;
    const bool scc = std::any_of(
        timbre.layers.begin(),
        timbre.layers.end(),
        [](const CompositeLayer& layer) {
            return layer.source == TimbreSource::Scc;
        });
    if (!scc) {
        batch.warnings.push_back("SCC の波形がありません");
        return batch;
    }
    const auto compiled = compileSccMorphCached(timbre);
    const CompositeTimbre* source = &timbre;
    TimbreNumberResolution numbers;
    if (compiled && compiled->valid) {
        numbers = resolveTimbreNumbers(compiled->timbre);
        std::set<int> exported_numbers;
        for (std::size_t layer_index = 0; layer_index < compiled->timbre.layers.size();
             ++layer_index) {
            const auto& layer = compiled->timbre.layers[layer_index];
            if (layer.source != TimbreSource::Scc) {
                continue;
            }
            const auto formatted = formatMgsCompositeEnvelope(
                layer,
                layer.envelope_number,
                kMgscEnvelopeCompiledByteLimit,
                &numbers);
            if (formatted.output_cutoff_count) {
                batch.warnings.push_back(
                    "253 バイトを超えた @e の続きは書き出しません");
            }
            const auto tones = toneNumbersInEnvelope(formatted.bytecode);
            if (tones.empty()) {
                if (layer.base_timbre) {
                    const auto number = assignedNumberForLibraryId(
                        numbers, layer.base_timbre->library_id);
                    const auto tone = number.value_or(0);
                    if (exported_numbers.insert(tone).second) {
                        addWaveFile(
                            batch,
                            static_cast<int>(batch.files.size()) + 1,
                            tone,
                            false,
                            layer.base_timbre->name.empty()
                                ? timbre.name
                                : layer.base_timbre->name,
                            sccWaveformFromBytes(layer.base_timbre->scc_waveform));
                    }
                }
                continue;
            }
            for (const auto tone : tones) {
                const auto assigned = std::find_if(
                    numbers.assignments.begin(),
                    numbers.assignments.end(),
                    [&](const ResolvedTimbreNumber& item) {
                        return item.layer_index == layer_index
                            && item.number == tone;
                    });
                if (assigned == numbers.assignments.end()) {
                    batch.warnings.push_back("音色番号の波形を解決できません");
                    continue;
                }
                const auto* snapshot = findEmbeddedTimbreSnapshot(
                    compiled->timbre, assigned->library_id);
                if (snapshot == nullptr) {
                    batch.warnings.push_back("音色番号の波形を解決できません");
                    continue;
                }
                if (!exported_numbers.insert(tone).second) {
                    continue;
                }
                addWaveFile(
                    batch,
                    static_cast<int>(batch.files.size()) + 1,
                    tone,
                    !authoredId(timbre, assigned->library_id),
                    snapshot->name.empty() ? timbre.name : snapshot->name,
                    waveOrSilence(snapshot));
            }
        }
        if (!batch.files.empty()) {
            batch.partial = true;
        }
        return batch;
    }
    batch.warnings.push_back("モーフのコンパイルに失敗したため基音だけを書き出します");
    int order = 1;
    std::set<std::uint64_t> seen;
    for (const auto& layer : source->layers) {
        if (layer.source != TimbreSource::Scc || !layer.base_timbre) {
            continue;
        }
        if (!seen.insert(layer.base_timbre->library_id).second) {
            continue;
        }
        const auto number = assignedNumberForLibraryId(
            resolveTimbreNumbers(*source), layer.base_timbre->library_id);
        addWaveFile(
            batch,
            order,
            number.value_or(0),
            false,
            layer.base_timbre->name.empty() ? timbre.name : layer.base_timbre->name,
            sccWaveformFromBytes(layer.base_timbre->scc_waveform));
        ++order;
    }
    if (batch.files.size() > 1 || !batch.warnings.empty()) {
        batch.partial = !batch.files.empty();
    }
    return batch;
}

}  // namespace mgstc::engine
