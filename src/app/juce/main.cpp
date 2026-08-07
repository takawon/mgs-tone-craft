// SPDX-License-Identifier: AGPL-3.0-only

#include <array>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include "BinaryData.h"

#include "mgstc/audio/wasapi_audio_sink.hpp"
#include "mgstc/engine/engine_command.hpp"
#include "mgstc/engine/chip_rack.hpp"
#include "mgstc/engine/composite_timbre.hpp"
#include "mgstc/engine/composite_timbre_library.hpp"
#include "mgstc/engine/mgs_timbre_io.hpp"
#include "mgstc/engine/opll_envelope_trace.hpp"
#include "mgstc/engine/opll_patch.hpp"
#include "mgstc/engine/note_pitch.hpp"
#include "mgstc/engine/pc_keyboard.hpp"
#include "mgstc/engine/realtime_engine_host.hpp"
#include "mgstc/engine/scc_waveform.hpp"
#include "mgstc/engine/timbre_library.hpp"
#include "mgstc/engine/voice_allocator.hpp"
#include "mgstc/engine/wave_import.hpp"

#include "audio_import_bridge.hpp"

namespace {

static_assert(
    sizeof(mgstc::engine::RealtimeEngineHost) < 128 * 1024,
    "RealtimeEngineHost must remain safe to create in an editor");

constexpr std::uint8_t kPsgTrack = 0;
constexpr std::uint8_t kSccTrack = 3;
constexpr std::uint8_t kOpllTrack = 8;
constexpr std::uint8_t kPreviewNote = 60;
constexpr std::uint16_t kPcOctaveDownScanCode = 0x33;
constexpr std::uint16_t kPcOctaveUpScanCode = 0x34;
constexpr std::array<std::uint16_t, 33>
    kPcPerformanceScanCodes{
        0x2C, 0x1F, 0x2D, 0x20, 0x2E, 0x2F, 0x22, 0x30,
        0x23, 0x31, 0x24, 0x32,
        0x10, 0x03, 0x11, 0x04, 0x12, 0x13, 0x06, 0x14,
        0x07, 0x15, 0x08, 0x16, 0x17, 0x0A, 0x18, 0x0B,
        0x19, 0x1A, 0x0D, 0x1B, 0x7D};

using EditorOpenCallback =
    std::function<void(const juce::String&)>;
using EditorCloseCallback =
    std::function<void(const juce::String&)>;
using EditorActivateCallback =
    std::function<void(const juce::String&)>;

[[nodiscard]] juce::File applicationDataDirectory() {
    return juce::File::getSpecialLocation(
               juce::File::userApplicationDataDirectory)
        .getChildFile("MgsToneCraft");
}

[[nodiscard]] juce::File compositeTimbreLibraryFile() {
    return applicationDataDirectory().getChildFile(
        "composite-timbre-library-v1.mgstc");
}

[[nodiscard]] std::string utf8String(const juce::String& text) {
    const auto utf8 = text.toUTF8();
    return {
        utf8.getAddress(),
        static_cast<std::size_t>(utf8.sizeInBytes() - 1)};
}

[[nodiscard]] std::optional<
    mgstc::engine::CompositeTimbreLibrary>
loadCompositeTimbreLibrary(std::string* error = nullptr) {
    const auto file = compositeTimbreLibraryFile();
    if (!file.existsAsFile()) {
        return mgstc::engine::CompositeTimbreLibrary{};
    }
    return mgstc::engine::CompositeTimbreLibrary::deserialize(
        utf8String(file.loadFileAsString()), error);
}

[[nodiscard]] bool persistCompositeTimbreLibrary(
    const mgstc::engine::CompositeTimbreLibrary& library) {
    const auto target = compositeTimbreLibraryFile();
    if (!target.getParentDirectory().createDirectory()) {
        return false;
    }
    juce::TemporaryFile temporary(target);
    const auto contents = library.serialize();
    return temporary.getFile().replaceWithData(
               contents.data(), contents.size())
        && temporary.overwriteTargetFileWithTemporary();
}

struct CompositeTimbreImpact {
    bool readable{true};
    std::vector<mgstc::engine::TimbreUse> uses;
};

[[nodiscard]] CompositeTimbreImpact inspectCompositeTimbreImpact(
    std::uint64_t timbre_library_id) {
    CompositeTimbreImpact impact;
    std::string error;
    const auto library = loadCompositeTimbreLibrary(&error);
    if (!library) {
        impact.readable = false;
        return impact;
    }
    impact.uses = library->findTimbreUses(timbre_library_id);
    return impact;
}

[[nodiscard]] std::size_t impactedCompositeCount(
    std::span<const mgstc::engine::TimbreUse> uses) {
    std::vector<std::size_t> indices;
    for (const auto& use : uses) {
        if (std::find(
                indices.begin(),
                indices.end(),
                use.composite_index)
            == indices.end()) {
            indices.push_back(use.composite_index);
        }
    }
    return indices.size();
}

[[nodiscard]] juce::String describeCompositeTimbreImpact(
    const CompositeTimbreImpact& impact) {
    if (!impact.readable) {
        return juce::String::fromUTF8(
            "総合音色ライブラリを読み込めないため、"
            "影響対象を確認できません。");
    }
    if (impact.uses.empty()) {
        return juce::String::fromUTF8(
            "保存済み総合音色への影響はありません。");
    }
    juce::String result =
        juce::String::fromUTF8("影響対象: ")
        + juce::String(static_cast<int>(
            impactedCompositeCount(impact.uses)))
        + juce::String::fromUTF8("件の総合音色 / ")
        + juce::String(static_cast<int>(impact.uses.size()))
        + juce::String::fromUTF8("レイヤー\n");
    std::vector<std::size_t> listed;
    for (const auto& use : impact.uses) {
        if (std::find(
                listed.begin(), listed.end(), use.composite_index)
            != listed.end()) {
            continue;
        }
        listed.push_back(use.composite_index);
        result += juce::String::fromUTF8("・")
            + juce::String::fromUTF8(use.composite_name.c_str())
            + "\n";
        if (listed.size() == 6) {
            if (listed.size()
                < impactedCompositeCount(impact.uses)) {
                result += juce::String::fromUTF8("・ほか\n");
            }
            break;
        }
    }
    return result.trimEnd();
}

[[nodiscard]] bool textEntryHasFocusWithin(
    const juce::Component& root) {
    auto* focused = juce::Component::getCurrentlyFocusedComponent();
    if (!focused
        || (focused != &root && !root.isParentOf(focused))) {
        return false;
    }
    for (auto* component = focused;
         component != nullptr;
         component = component->getParentComponent()) {
        if (dynamic_cast<juce::TextEditor*>(component) != nullptr) {
            return true;
        }
        if (const auto* label =
                dynamic_cast<juce::Label*>(component);
            label && label->isBeingEdited()) {
            return true;
        }
        if (component == &root) {
            break;
        }
    }
    return false;
}

[[nodiscard]] bool componentWindowIsActive(
    const juce::Component& component) {
    const auto* top_level = component.getTopLevelComponent();
    const auto* peer = top_level ? top_level->getPeer() : nullptr;
    return peer != nullptr && peer->isFocused();
}

[[nodiscard]] bool physicalKeyIsDown(std::uint16_t scan_code) {
    const auto virtual_key = MapVirtualKeyW(
        scan_code, MAPVK_VSC_TO_VK_EX);
    return virtual_key != 0
        && (GetAsyncKeyState(static_cast<int>(virtual_key))
            & 0x8000)
            != 0;
}

[[nodiscard]] juce::String midiNoteName(std::uint8_t midi_note) {
    constexpr std::array<const char*, 12> names{
        "C", "C#", "D", "D#", "E", "F",
        "F#", "G", "G#", "A", "A#", "B"};
    const auto name =
        names[static_cast<std::size_t>(midi_note % 12)];
    const int octave = static_cast<int>(midi_note / 12) - 1;
    return juce::String(name) + juce::String(octave);
}

using SccWaveform = mgstc::engine::SccWaveform;

[[nodiscard]] mgstc::engine::WavePcm makeSccReferencePcm(
    const SccWaveform& waveform) {
    constexpr std::uint32_t sample_rate = 48'000;
    constexpr double frequency = 261.625565;
    mgstc::engine::WavePcm pcm;
    pcm.sample_rate = sample_rate;
    pcm.mono_samples.resize(sample_rate);
    double phase = 0.0;
    const double step = frequency * 32.0 / sample_rate;
    for (auto& sample : pcm.mono_samples) {
        const auto first = static_cast<std::size_t>(phase) % 32;
        const auto second = (first + 1) % 32;
        const float fraction = static_cast<float>(
            phase - std::floor(phase));
        sample = (static_cast<float>(waveform[first])
                    + (static_cast<float>(waveform[second])
                       - static_cast<float>(waveform[first]))
                        * fraction)
            / 128.0F;
        phase += step;
        while (phase >= 32.0) {
            phase -= 32.0;
        }
    }
    return pcm;
}

[[nodiscard]] mgstc::engine::WavePcm makeOpllReferencePcm(
    const mgstc::engine::OpllPatchParameters& patch,
    std::size_t start_sample) {
    constexpr std::uint32_t sample_rate = 48'000;
    constexpr std::size_t capture_samples = 8'192;
    constexpr std::uint8_t kCaptureMidiNote = 60;
    mgstc::engine::Ym2413Adapter opll;
    const auto registers = mgstc::engine::encodeOpllPatch(patch);
    for (std::size_t index = 0; index < registers.size(); ++index) {
        static_cast<void>(opll.write(
            static_cast<std::uint8_t>(index), registers[index]));
    }
    mgstc::engine::NotePitch pitch{};
    if (!mgstc::engine::notePitch(kCaptureMidiNote, pitch)) {
        return {};
    }
    static_cast<void>(opll.write(0x30, 0x00));
    static_cast<void>(opll.write(
        0x10, static_cast<std::uint8_t>(pitch.opll.f_number & 0xFF)));
    static_cast<void>(opll.write(
        0x20,
        static_cast<std::uint8_t>(
            0x10
            | ((pitch.opll.block & 7) << 1)
            | ((pitch.opll.f_number >> 8) & 1))));
    for (std::size_t index = 0; index < start_sample; ++index) {
        static_cast<void>(opll.renderSample());
    }
    mgstc::engine::WavePcm pcm;
    pcm.sample_rate = sample_rate;
    pcm.mono_samples.resize(capture_samples);
    for (auto& sample : pcm.mono_samples) {
        sample = opll.renderSample();
    }
    return pcm;
}

[[nodiscard]] juce::File pendingConversionFile(
    const juce::String& target) {
    return applicationDataDirectory().getChildFile(
        "pending-" + target + "-conversion.txt");
}

enum class EditorIcon {
    Open,
    Save,
    Paste,
    Copy,
    Undo,
    Redo,
    Average,
    Normalize,
    Invert,
    Up,
    Left,
    Right,
    Down,
    Convert,
    Audition,
    Settings,
    AbAudition,
    CancelPreview,
};

std::unique_ptr<juce::Drawable> makeEditorIcon(
    EditorIcon icon,
    juce::Colour colour) {
    juce::Path path;
    switch (icon) {
    case EditorIcon::Open:
        path.startNewSubPath(2.0F, 7.0F);
        path.lineTo(9.0F, 7.0F);
        path.lineTo(11.0F, 10.0F);
        path.lineTo(22.0F, 10.0F);
        path.lineTo(19.0F, 20.0F);
        path.lineTo(3.0F, 20.0F);
        path.closeSubPath();
        path.startNewSubPath(3.0F, 7.0F);
        path.lineTo(3.0F, 4.0F);
        path.lineTo(10.0F, 4.0F);
        path.lineTo(12.0F, 7.0F);
        path.lineTo(20.0F, 7.0F);
        path.lineTo(20.0F, 10.0F);
        break;
    case EditorIcon::Save:
        path.addRectangle(4.0F, 3.0F, 16.0F, 18.0F);
        path.addRectangle(7.0F, 4.0F, 9.0F, 6.0F);
        path.addRectangle(7.0F, 14.0F, 10.0F, 7.0F);
        path.startNewSubPath(15.0F, 5.0F);
        path.lineTo(15.0F, 9.0F);
        break;
    case EditorIcon::Paste:
        path.addRoundedRectangle(
            5.0F, 6.0F, 14.0F, 16.0F, 1.5F);
        path.addRoundedRectangle(
            8.0F, 2.0F, 8.0F, 5.0F, 1.5F);
        path.startNewSubPath(9.0F, 11.0F);
        path.lineTo(15.0F, 11.0F);
        path.startNewSubPath(9.0F, 15.0F);
        path.lineTo(15.0F, 15.0F);
        break;
    case EditorIcon::Copy:
        path.addRoundedRectangle(
            7.0F, 7.0F, 13.0F, 14.0F, 1.5F);
        path.addRoundedRectangle(
            3.0F, 3.0F, 13.0F, 14.0F, 1.5F);
        break;
    case EditorIcon::Undo:
    case EditorIcon::Redo: {
        const auto direction =
            icon == EditorIcon::Undo ? 1.0F : -1.0F;
        const auto centre = 12.0F;
        path.startNewSubPath(
            centre - direction * 7.0F, 8.0F);
        path.lineTo(
            centre - direction * 2.0F, 4.0F);
        path.startNewSubPath(
            centre - direction * 7.0F, 8.0F);
        path.lineTo(
            centre - direction * 2.0F, 12.0F);
        path.startNewSubPath(
            centre - direction * 6.0F, 8.0F);
        path.cubicTo(
            centre + direction * 8.0F,
            5.0F,
            centre + direction * 8.0F,
            18.0F,
            centre,
            19.0F);
        break;
    }
    case EditorIcon::Average:
        path.startNewSubPath(2.0F, 15.0F);
        path.cubicTo(
            5.0F, 5.0F, 9.0F, 5.0F, 12.0F, 15.0F);
        path.cubicTo(
            15.0F, 22.0F, 19.0F, 22.0F, 22.0F, 11.0F);
        break;
    case EditorIcon::Normalize:
        path.startNewSubPath(12.0F, 3.0F);
        path.lineTo(8.0F, 8.0F);
        path.startNewSubPath(12.0F, 3.0F);
        path.lineTo(16.0F, 8.0F);
        path.startNewSubPath(12.0F, 3.0F);
        path.lineTo(12.0F, 21.0F);
        path.startNewSubPath(12.0F, 21.0F);
        path.lineTo(8.0F, 16.0F);
        path.startNewSubPath(12.0F, 21.0F);
        path.lineTo(16.0F, 16.0F);
        break;
    case EditorIcon::Invert:
        path.startNewSubPath(4.0F, 7.0F);
        path.lineTo(10.0F, 7.0F);
        path.startNewSubPath(7.0F, 4.0F);
        path.lineTo(7.0F, 10.0F);
        path.startNewSubPath(14.0F, 17.0F);
        path.lineTo(20.0F, 17.0F);
        path.startNewSubPath(12.0F, 21.0F);
        path.lineTo(22.0F, 3.0F);
        break;
    case EditorIcon::Convert:
        path.startNewSubPath(3.0F, 8.0F);
        path.lineTo(20.0F, 8.0F);
        path.lineTo(16.0F, 4.0F);
        path.startNewSubPath(21.0F, 16.0F);
        path.lineTo(4.0F, 16.0F);
        path.lineTo(8.0F, 20.0F);
        break;
    case EditorIcon::Audition:
        path.addTriangle(4.0F, 9.0F, 9.0F, 9.0F, 9.0F, 15.0F);
        path.startNewSubPath(9.0F, 9.0F);
        path.lineTo(14.0F, 5.0F);
        path.lineTo(14.0F, 19.0F);
        path.lineTo(9.0F, 15.0F);
        path.startNewSubPath(16.0F, 8.0F);
        path.cubicTo(20.0F, 10.0F, 20.0F, 14.0F, 16.0F, 16.0F);
        path.startNewSubPath(18.0F, 5.0F);
        path.cubicTo(24.0F, 9.0F, 24.0F, 15.0F, 18.0F, 19.0F);
        break;
    case EditorIcon::Settings:
        path.addEllipse(8.0F, 8.0F, 8.0F, 8.0F);
        path.addEllipse(4.0F, 4.0F, 16.0F, 16.0F);
        for (int index = 0; index < 8; ++index) {
            const auto angle = juce::MathConstants<float>::twoPi
                * static_cast<float>(index) / 8.0F;
            path.startNewSubPath(
                12.0F + std::cos(angle) * 8.0F,
                12.0F + std::sin(angle) * 8.0F);
            path.lineTo(
                12.0F + std::cos(angle) * 11.0F,
                12.0F + std::sin(angle) * 11.0F);
        }
        break;
    case EditorIcon::AbAudition:
        // A / B を左右パネルで対比（試聴切替）
        path.addRoundedRectangle(2.0F, 4.0F, 9.0F, 16.0F, 1.5F);
        path.addRoundedRectangle(13.0F, 4.0F, 9.0F, 16.0F, 1.5F);
        path.startNewSubPath(6.5F, 8.0F);
        path.lineTo(4.5F, 16.0F);
        path.startNewSubPath(6.5F, 8.0F);
        path.lineTo(8.5F, 16.0F);
        path.startNewSubPath(5.2F, 13.0F);
        path.lineTo(7.8F, 13.0F);
        path.startNewSubPath(15.5F, 8.0F);
        path.lineTo(15.5F, 16.0F);
        path.startNewSubPath(15.5F, 8.0F);
        path.lineTo(18.5F, 8.0F);
        path.cubicTo(20.2F, 8.0F, 20.2F, 11.0F, 18.5F, 11.0F);
        path.lineTo(15.5F, 11.0F);
        path.startNewSubPath(15.5F, 11.0F);
        path.lineTo(18.8F, 11.0F);
        path.cubicTo(20.5F, 11.0F, 20.5F, 16.0F, 18.5F, 16.0F);
        path.lineTo(15.5F, 16.0F);
        break;
    case EditorIcon::CancelPreview:
        path.startNewSubPath(5.0F, 5.0F);
        path.lineTo(19.0F, 19.0F);
        path.startNewSubPath(19.0F, 5.0F);
        path.lineTo(5.0F, 19.0F);
        path.addEllipse(3.0F, 3.0F, 18.0F, 18.0F);
        break;
    case EditorIcon::Up:
    case EditorIcon::Down:
    case EditorIcon::Left:
    case EditorIcon::Right: {
        juce::Point<float> start;
        juce::Point<float> end;
        juce::Point<float> wing_a;
        juce::Point<float> wing_b;
        if (icon == EditorIcon::Up) {
            start = {12.0F, 21.0F};
            end = {12.0F, 3.0F};
            wing_a = {6.0F, 9.0F};
            wing_b = {18.0F, 9.0F};
        } else if (icon == EditorIcon::Down) {
            start = {12.0F, 3.0F};
            end = {12.0F, 21.0F};
            wing_a = {6.0F, 15.0F};
            wing_b = {18.0F, 15.0F};
        } else if (icon == EditorIcon::Left) {
            start = {21.0F, 12.0F};
            end = {3.0F, 12.0F};
            wing_a = {9.0F, 6.0F};
            wing_b = {9.0F, 18.0F};
        } else {
            start = {3.0F, 12.0F};
            end = {21.0F, 12.0F};
            wing_a = {15.0F, 6.0F};
            wing_b = {15.0F, 18.0F};
        }
        path.startNewSubPath(start);
        path.lineTo(end);
        path.startNewSubPath(end);
        path.lineTo(wing_a);
        path.startNewSubPath(end);
        path.lineTo(wing_b);
        break;
    }
    }

    auto drawable = std::make_unique<juce::DrawablePath>();
    drawable->setPath(std::move(path));
    drawable->setFill(juce::Colours::transparentBlack);
    drawable->setStrokeFill(colour);
    drawable->setStrokeType(
        juce::PathStrokeType(
            1.7F,
            juce::PathStrokeType::curved,
            juce::PathStrokeType::rounded));
    return drawable;
}

void configureSettingsButton(
    juce::DrawableButton& button,
    std::function<void()> action) {
    const auto normal = makeEditorIcon(
        EditorIcon::Settings, juce::Colour(0xFFE6EDF3));
    const auto over = makeEditorIcon(
        EditorIcon::Settings, juce::Colour(0xFF56B4E9));
    const auto down = makeEditorIcon(
        EditorIcon::Settings, juce::Colour(0xFF0072B2));
    button.setImages(normal.get(), over.get(), down.get());
    button.setTooltip(
        juce::String::fromUTF8("アプリ設定を開きます"));
    button.onClick = std::move(action);
}

[[nodiscard]] juce::Image loadEmbeddedPng(
    const char* data,
    int size) {
    return juce::ImageFileFormat::loadFrom(data, static_cast<size_t>(size));
}

struct AnimatedGifSupport final {
    AnimatedGifSupport() {
        Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartup(&token_, &input, nullptr);
    }

    ~AnimatedGifSupport() {
        if (token_ != 0) {
            Gdiplus::GdiplusShutdown(token_);
        }
    }

    AnimatedGifSupport(const AnimatedGifSupport&) = delete;
    AnimatedGifSupport& operator=(const AnimatedGifSupport&) = delete;

    ULONG_PTR token_ = 0;
};

[[nodiscard]] AnimatedGifSupport& ensureGdiplusInitialized() {
    static AnimatedGifSupport runtime;
    return runtime;
}

[[nodiscard]] juce::Image gdiplusBitmapToImage(
    Gdiplus::Bitmap& bitmap) {
    const auto width = static_cast<int>(bitmap.GetWidth());
    const auto height = static_cast<int>(bitmap.GetHeight());
    if (width <= 0 || height <= 0) {
        return {};
    }

    Gdiplus::BitmapData data{};
    const Gdiplus::Rect rect(0, 0, width, height);
    if (bitmap.LockBits(
            &rect,
            Gdiplus::ImageLockModeRead,
            PixelFormat32bppARGB,
            &data)
        != Gdiplus::Ok) {
        return {};
    }

    juce::Image image(
        juce::Image::ARGB, width, height, true);
    {
        juce::Image::BitmapData dest(
            image,
            juce::Image::BitmapData::writeOnly);
        for (int y = 0; y < height; ++y) {
            const auto* src = static_cast<const std::uint8_t*>(
                data.Scan0)
                + static_cast<std::ptrdiff_t>(y) * data.Stride;
            auto* out = dest.getLinePointer(y);
            for (int x = 0; x < width; ++x) {
                const auto b = src[x * 4 + 0];
                const auto g = src[x * 4 + 1];
                const auto r = src[x * 4 + 2];
                const auto a = src[x * 4 + 3];
                juce::PixelARGB pixel(a, r, g, b);
                pixel.premultiply();
                reinterpret_cast<juce::PixelARGB*>(out)[x] = pixel;
            }
        }
    }
    bitmap.UnlockBits(&data);
    return image;
}

[[nodiscard]] juce::Image loadImageFileForBackground(
    const juce::File& file) {
    if (!file.existsAsFile()) {
        return {};
    }
    if (auto image = juce::ImageFileFormat::loadFrom(file);
        image.isValid()) {
        return image;
    }
    ensureGdiplusInitialized();
    Gdiplus::Bitmap bitmap(file.getFullPathName().toWideCharPointer());
    if (bitmap.GetLastStatus() != Gdiplus::Ok) {
        return {};
    }
    return gdiplusBitmapToImage(bitmap);
}

[[nodiscard]] juce::Image loadImageBytesForBackground(
    const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) {
        return {};
    }
    if (auto image = juce::ImageFileFormat::loadFrom(
            bytes.data(), bytes.size());
        image.isValid()) {
        return image;
    }
    ensureGdiplusInitialized();
    const auto memory = ::GlobalAlloc(
        GMEM_MOVEABLE, static_cast<SIZE_T>(bytes.size()));
    if (memory == nullptr) {
        return {};
    }
    if (auto* locked = ::GlobalLock(memory)) {
        std::memcpy(locked, bytes.data(), bytes.size());
        ::GlobalUnlock(memory);
    } else {
        ::GlobalFree(memory);
        return {};
    }
    IStream* stream = nullptr;
    if (::CreateStreamOnHGlobal(memory, TRUE, &stream) != S_OK
        || stream == nullptr) {
        ::GlobalFree(memory);
        return {};
    }
    juce::Image image;
    {
        Gdiplus::Bitmap bitmap(stream);
        if (bitmap.GetLastStatus() == Gdiplus::Ok) {
            image = gdiplusBitmapToImage(bitmap);
        }
    }
    stream->Release();
    return image;
}

bool loadAnimatedGifFrames(
    const void* data,
    int size,
    std::vector<juce::Image>& frames,
    std::vector<int>& delays_ms) {
    ensureGdiplusInitialized();
    frames.clear();
    delays_ms.clear();
    if (data == nullptr || size <= 0) {
        return false;
    }

    const auto bytes = static_cast<SIZE_T>(size);
    const auto memory = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory == nullptr) {
        return false;
    }
    if (auto* locked = ::GlobalLock(memory)) {
        std::memcpy(locked, data, bytes);
        ::GlobalUnlock(memory);
    } else {
        ::GlobalFree(memory);
        return false;
    }

    IStream* stream = nullptr;
    if (FAILED(::CreateStreamOnHGlobal(memory, TRUE, &stream))
        || stream == nullptr) {
        ::GlobalFree(memory);
        return false;
    }

    Gdiplus::Bitmap bitmap(stream);
    stream->Release();
    if (bitmap.GetLastStatus() != Gdiplus::Ok) {
        return false;
    }

    static const GUID dimension = { 0x6aedbd6d, 0x3fb5, 0x418a, { 0x83, 0xa6, 0x7f, 0x45, 0x22, 0x9d, 0xc8, 0x72 } };
    const UINT frame_count = bitmap.GetFrameCount(&dimension);
    if (frame_count == 0) {
        return false;
    }

    frames.reserve(frame_count);
    delays_ms.reserve(frame_count);
    for (UINT frame = 0; frame < frame_count; ++frame) {
        if (bitmap.SelectActiveFrame(&dimension, frame)
            != Gdiplus::Ok) {
            continue;
        }
        auto converted = gdiplusBitmapToImage(bitmap);
        if (!converted.isValid()) {
            continue;
        }
        frames.push_back(std::move(converted));
        delays_ms.push_back(50);
    }
    return !frames.empty();
}

class AnimatedGifComponent final
    : public juce::Component,
      private juce::Timer {
public:
    void loadFromMemory(const void* data, int size) {
        stopTimer();
        frames_.clear();
        delays_ms_.clear();
        frame_index_ = 0;
        static_cast<void>(
            loadAnimatedGifFrames(data, size, frames_, delays_ms_));
        if (!frames_.empty()) {
            startTimer(delays_ms_.front());
        }
        repaint();
    }

    void setDisplaySize(int width, int height) {
        display_width_ = juce::jmax(1, width);
        display_height_ = juce::jmax(1, height);
        setSize(display_width_, display_height_);
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colours::transparentBlack);
        if (frames_.empty()) {
            return;
        }
        g.drawImageWithin(
            frames_[static_cast<size_t>(frame_index_)],
            0,
            0,
            getWidth(),
            getHeight(),
            juce::RectanglePlacement::centred
                | juce::RectanglePlacement::onlyReduceInSize);
    }

private:
    void timerCallback() override {
        if (frames_.size() <= 1) {
            return;
        }
        frame_index_ =
            (frame_index_ + 1) % static_cast<int>(frames_.size());
        repaint();
        startTimer(
            delays_ms_[static_cast<size_t>(frame_index_)]);
    }

    std::vector<juce::Image> frames_;
    std::vector<int> delays_ms_;
    int frame_index_{0};
    int display_width_{256};
    int display_height_{256};
};

class ConversionBusyContent final : public juce::Component {
public:
    ConversionBusyContent() {
        label_.setText(
            juce::String::fromUTF8("変換中…"),
            juce::dontSendNotification);
        label_.setJustificationType(juce::Justification::centred);
        label_.setColour(
            juce::Label::textColourId, juce::Colour(0xFFE6EDF3));
        addAndMakeVisible(label_);

        gif_.loadFromMemory(
            BinaryData::mgstc_spin_gif,
            BinaryData::mgstc_spin_gifSize);
        // Source GIF is 512x512; show at about 50%.
        gif_.setDisplaySize(256, 256);
        addAndMakeVisible(gif_);
        setSize(320, 330);
    }

    void resized() override {
        auto area = getLocalBounds().reduced(16);
        label_.setBounds(area.removeFromTop(28));
        area.removeFromTop(8);
        gif_.setBounds(
            area.withSizeKeepingCentre(256, 256));
    }

private:
    juce::Label label_;
    AnimatedGifComponent gif_;
};

class ConversionBusyDialog final : public juce::DialogWindow {
public:
    ConversionBusyDialog()
        : juce::DialogWindow(
              juce::String::fromUTF8("変換中"),
              juce::Colour(0xFF1B222C),
              false,
              true) {
        setUsingNativeTitleBar(true);
        setResizable(false, false);
        auto* content = new ConversionBusyContent();
        setContentOwned(content, true);
        centreWithSize(content->getWidth(), content->getHeight() + 32);
    }

    void closeButtonPressed() override {
        // Conversion cannot be cancelled; ignore close.
    }
};

template <typename Work, typename OnDone>
void runWithConversionBusyDialog(
    juce::Component* anchor,
    Work work,
    OnDone on_done) {
    auto* dialog = new ConversionBusyDialog();
    if (anchor != nullptr) {
        dialog->centreAroundComponent(
            anchor, dialog->getWidth(), dialog->getHeight());
    }
    dialog->enterModalState(true, nullptr, true);

    juce::Component::SafePointer<ConversionBusyDialog> safe_dialog(
        dialog);
    std::thread(
        [safe_dialog, work = std::move(work),
         on_done = std::move(on_done)]() mutable {
            auto result = work();
            juce::MessageManager::callAsync(
                [safe_dialog,
                 result = std::move(result),
                 on_done = std::move(on_done)]() mutable {
                    if (safe_dialog != nullptr) {
                        safe_dialog->exitModalState(1);
                    }
                    on_done(std::move(result));
                });
        })
        .detach();
}

class AboutPanel final : public juce::Component {
public:
    AboutPanel() {
        logo_ = loadEmbeddedPng(
            BinaryData::MGSTC_logo_png,
            BinaryData::MGSTC_logo_pngSize);
        title_.setText(
            "MGS Tone Craft", juce::dontSendNotification);
        title_.setFont(
            juce::FontOptions(22.0F, juce::Font::bold));
        title_.setJustificationType(juce::Justification::centred);
        title_.setColour(
            juce::Label::textColourId, juce::Colour(0xFFE6EDF3));
        addAndMakeVisible(title_);

        subtitle_.setText(
            "MGSTC", juce::dontSendNotification);
        subtitle_.setFont(juce::FontOptions(16.0F));
        subtitle_.setJustificationType(juce::Justification::centred);
        subtitle_.setColour(
            juce::Label::textColourId, juce::Colour(0xFF9AA8B5));
        addAndMakeVisible(subtitle_);

        detail_.setText(
            juce::String::fromUTF8(
                "MGSDRV向け複合音色エディタ\n"
                "Version 0.1.0"),
            juce::dontSendNotification);
        detail_.setJustificationType(juce::Justification::centred);
        detail_.setColour(
            juce::Label::textColourId, juce::Colour(0xFF9AA8B5));
        addAndMakeVisible(detail_);
    }

    void paint(juce::Graphics& g) override {
        if (logo_.isValid()) {
            // Source logo is 1024x1024; show at 25% (256x256).
            g.drawImageWithin(
                logo_,
                logo_bounds_.getX(),
                logo_bounds_.getY(),
                logo_bounds_.getWidth(),
                logo_bounds_.getHeight(),
                juce::RectanglePlacement::centred
                    | juce::RectanglePlacement::onlyReduceInSize);
        }
    }

    void resized() override {
        auto area = getLocalBounds().reduced(12);
        logo_bounds_ = area.removeFromTop(256)
                           .withSizeKeepingCentre(256, 256);
        area.removeFromTop(12);
        title_.setBounds(area.removeFromTop(28));
        subtitle_.setBounds(area.removeFromTop(24));
        area.removeFromTop(8);
        detail_.setBounds(area.removeFromTop(48));
    }

private:
    juce::Image logo_;
    juce::Rectangle<int> logo_bounds_;
    juce::Label title_;
    juce::Label subtitle_;
    juce::Label detail_;
};

void configureImmediateAuditionButton(
    juce::DrawableButton& button,
    bool enabled,
    std::function<void()> action) {
    const auto normal = makeEditorIcon(
        EditorIcon::Audition, juce::Colour(0xFF9AA8B5));
    const auto over = makeEditorIcon(
        EditorIcon::Audition, juce::Colour(0xFFE6EDF3));
    const auto down = makeEditorIcon(
        EditorIcon::Audition, juce::Colour(0xFF0072B2));
    const auto normal_on = makeEditorIcon(
        EditorIcon::Audition, juce::Colour(0xFF35C4ED));
    const auto over_on = makeEditorIcon(
        EditorIcon::Audition, juce::Colour(0xFF73D8F5));
    button.setImages(
        normal.get(), over.get(), down.get(), nullptr,
        normal_on.get(), over_on.get(), down.get(), nullptr);
    button.setClickingTogglesState(true);
    button.setToggleState(enabled, juce::dontSendNotification);
    button.setTooltip(
        juce::String::fromUTF8(
            "即時発音。ONでは設定変更時に最後の手動音程で1秒発音します"));
    button.onClick = std::move(action);
}

class SccWaveGraph final
    : public juce::Component,
      public juce::SettableTooltipClient {
public:
    using CommitCallback =
        std::function<void(const SccWaveform&, const SccWaveform&)>;
    using LiveEditCallback =
        std::function<void(const SccWaveform&)>;
    using ValueCommitCallback =
        std::function<void(const SccWaveform&)>;

    SccWaveGraph() {
        setMouseCursor(juce::MouseCursor::CrosshairCursor);
        setTooltip(
            juce::String::fromUTF8(
                "ドラッグして32個の波形値を編集します"));
        for (std::size_t index = 0; index < waveform_.size(); ++index) {
            auto decimal = std::make_unique<juce::TextEditor>();
            auto hexadecimal = std::make_unique<juce::TextEditor>();
            configureValueEditor(*decimal, false);
            configureValueEditor(*hexadecimal, true);
            decimal->setExplicitFocusOrder(
                static_cast<int>(index + 1));
            hexadecimal->setExplicitFocusOrder(
                static_cast<int>(index + 33));
            decimal->onReturnKey = [this, index] {
                commitValueEditor(index, false);
            };
            decimal->onFocusLost = [this, index] {
                commitValueEditor(index, false);
            };
            hexadecimal->onReturnKey = [this, index] {
                commitValueEditor(index, true);
            };
            hexadecimal->onFocusLost = [this, index] {
                commitValueEditor(index, true);
            };
            addAndMakeVisible(*decimal);
            addAndMakeVisible(*hexadecimal);
            decimal_editors_[index] = std::move(decimal);
            hex_editors_[index] = std::move(hexadecimal);
        }
        syncValueEditors(waveform_);
    }

    void setWaveform(const SccWaveform& waveform) {
        reference_waveform_.reset();
        waveform_ = waveform;
        syncValueEditors(waveform_);
        repaint();
    }

    void setPreview(
        const SccWaveform& current,
        const SccWaveform& candidate) {
        reference_waveform_ = current;
        waveform_ = candidate;
        syncValueEditors(current);
        repaint();
    }

    void clearPreviewOverlay() {
        if (!reference_waveform_) {
            return;
        }
        waveform_ = *reference_waveform_;
        reference_waveform_.reset();
        syncValueEditors(waveform_);
        repaint();
    }

    [[nodiscard]] bool hasPreviewOverlay() const noexcept {
        return reference_waveform_.has_value();
    }

    [[nodiscard]] const SccWaveform& waveform() const noexcept {
        return waveform_;
    }

    void setCommitCallback(CommitCallback callback) {
        on_commit_ = std::move(callback);
    }

    void setLiveEditCallback(LiveEditCallback callback) {
        on_live_edit_ = std::move(callback);
    }

    void setValueCommitCallback(ValueCommitCallback callback) {
        on_value_commit_ = std::move(callback);
    }

    void setPreviewClearedCallback(std::function<void()> callback) {
        on_preview_cleared_ = std::move(callback);
    }

    void setBackgroundImage(
        const juce::Image& image,
        bool visible,
        float opacity,
        int offset_x,
        int offset_y,
        int display_width,
        int display_height) {
        background_image_ = image;
        background_visible_ = visible;
        background_opacity_ =
            juce::jlimit(0.0F, 1.0F, opacity);
        background_x_ = offset_x;
        background_y_ = offset_y;
        background_width_ = juce::jmax(1, display_width);
        background_height_ = juce::jmax(1, display_height);
        repaint();
    }

    void clearBackgroundImage() {
        background_image_ = {};
        repaint();
    }

    void paint(juce::Graphics& graphics) override {
        const auto graph = graphBounds();
        graphics.setColour(juce::Colour(0xFF111820));
        graphics.fillRoundedRectangle(graph.toFloat(), 6.0F);

        if (background_visible_
            && background_image_.isValid()) {
            juce::Graphics::ScopedSaveState clip(graphics);
            graphics.reduceClipRegion(graph);
            graphics.setOpacity(background_opacity_);
            graphics.drawImageWithin(
                background_image_,
                graph.getX() + background_x_,
                graph.getY() + background_y_,
                background_width_,
                background_height_,
                juce::RectanglePlacement::stretchToFit,
                false);
            graphics.setOpacity(1.0F);
        }

        graphics.setColour(juce::Colour(0xFF34404C));
        graphics.drawRoundedRectangle(graph.toFloat(), 6.0F, 1.0F);
        graphics.drawHorizontalLine(
            graph.getCentreY(),
            static_cast<float>(graph.getX()),
            static_cast<float>(graph.getRight()));

        const auto pitch =
            static_cast<float>(graph.getWidth()) / waveform_.size();
        graphics.setColour(juce::Colour(0x302AD6C9));
        for (std::size_t index = 0; index < waveform_.size(); ++index) {
            const auto x = static_cast<float>(graph.getX())
                + (static_cast<float>(index) + 0.5F) * pitch;
            graphics.drawVerticalLine(
                juce::roundToInt(x),
                static_cast<float>(graph.getY()),
                static_cast<float>(graph.getBottom()));
        }

        const auto make_path =
            [&](const SccWaveform& waveform) {
                juce::Path path;
                const auto first_y =
                    sampleY(waveform.front(), graph);
                const auto last_y =
                    sampleY(waveform.back(), graph);
                path.startNewSubPath(
                    static_cast<float>(graph.getX()),
                    (first_y + last_y) * 0.5F);
                for (std::size_t index = 0;
                     index < waveform.size();
                     ++index) {
                    path.lineTo(
                        static_cast<float>(graph.getX())
                            + (static_cast<float>(index) + 0.5F)
                                * pitch,
                        sampleY(waveform[index], graph));
                }
                path.lineTo(
                    static_cast<float>(graph.getRight()),
                    (first_y + last_y) * 0.5F);
                return path;
            };

        const auto confirmed_colour = juce::Colour(0xFF53E3A6);
        const auto candidate_colour = juce::Colour(0xFFFF9F43);

        if (reference_waveform_) {
            graphics.setColour(confirmed_colour);
            graphics.strokePath(
                make_path(*reference_waveform_),
                juce::PathStrokeType(
                    2.0F,
                    juce::PathStrokeType::curved,
                    juce::PathStrokeType::rounded));

            juce::Path dashed;
            const float dash_lengths[] = {5.0F, 4.0F};
            juce::PathStrokeType(1.8F).createDashedStroke(
                dashed,
                make_path(waveform_),
                dash_lengths,
                2);
            graphics.setColour(candidate_colour);
            graphics.strokePath(
                dashed,
                juce::PathStrokeType(
                    1.8F,
                    juce::PathStrokeType::curved,
                    juce::PathStrokeType::rounded));

            graphics.setFont(juce::FontOptions(12.0F));
            graphics.setColour(confirmed_colour);
            graphics.drawText(
                juce::String::fromUTF8("確定"),
                8,
                2,
                42,
                20,
                juce::Justification::centredLeft);
            graphics.setColour(candidate_colour);
            graphics.drawText(
                juce::String::fromUTF8("候補"),
                52,
                2,
                58,
                20,
                juce::Justification::centredLeft);
        } else {
            graphics.setColour(confirmed_colour);
            graphics.strokePath(
                make_path(waveform_),
                juce::PathStrokeType(
                    2.0F,
                    juce::PathStrokeType::curved,
                    juce::PathStrokeType::rounded));
        }

        graphics.setColour(juce::Colours::white.withAlpha(0.82F));
        graphics.setFont(juce::FontOptions(9.0F, juce::Font::bold));
        graphics.drawFittedText(
            "DEC",
            1,
            graph.getBottom() + 3,
            26,
            21,
            juce::Justification::centred,
            1);
        graphics.drawFittedText(
            "HEX",
            1,
            graph.getBottom() + 26,
            26,
            21,
            juce::Justification::centred,
            1);

        if (selected_index_ >= 0) {
            const auto& display_wave = reference_waveform_
                ? *reference_waveform_
                : waveform_;
            const auto text = juce::String::formatted(
                "%02d : %d (0x%02X)",
                selected_index_,
                static_cast<int>(
                    display_wave[static_cast<std::size_t>(
                        selected_index_)]),
                static_cast<unsigned int>(
                    static_cast<std::uint8_t>(
                        display_wave[static_cast<std::size_t>(
                            selected_index_)])));
            graphics.setColour(juce::Colours::white);
            graphics.drawText(
                text,
                getLocalBounds().removeFromTop(24),
                juce::Justification::centredRight);
        }
    }

    void mouseDown(const juce::MouseEvent& event) override {
        if (reference_waveform_) {
            waveform_ = *reference_waveform_;
            reference_waveform_.reset();
            syncValueEditors(waveform_);
            if (on_preview_cleared_) {
                on_preview_cleared_();
            }
        }
        drag_start_ = waveform_;
        updateFromMouse(event.position);
    }

    void mouseDrag(const juce::MouseEvent& event) override {
        updateFromMouse(event.position);
    }

    void mouseUp(const juce::MouseEvent&) override {
        if (drag_start_ != waveform_ && on_commit_) {
            on_commit_(drag_start_, waveform_);
        }
    }

    void resized() override {
        const auto graph = graphBounds();
        const auto pitch =
            static_cast<float>(graph.getWidth()) / waveform_.size();
        for (std::size_t index = 0; index < waveform_.size(); ++index) {
            const auto left = juce::roundToInt(
                static_cast<float>(graph.getX())
                + static_cast<float>(index) * pitch);
            const auto right = juce::roundToInt(
                static_cast<float>(graph.getX())
                + static_cast<float>(index + 1) * pitch);
            decimal_editors_[index]->setBounds(
                left, graph.getBottom() + 3, right - left, 21);
            hex_editors_[index]->setBounds(
                left, graph.getBottom() + 26, right - left, 21);
        }
    }

private:
    [[nodiscard]] juce::Rectangle<int> graphBounds() const {
        return getLocalBounds()
            .withTrimmedTop(26)
            .withTrimmedBottom(52)
            .withTrimmedLeft(28)
            .reduced(2, 0);
    }

    static void configureValueEditor(
        juce::TextEditor& editor,
        bool hexadecimal) {
        editor.setFont(juce::FontOptions(9.0F));
        editor.setJustification(juce::Justification::centred);
        editor.setBorder(juce::BorderSize<int>(1));
        editor.setIndents(1, 0);
        editor.setSelectAllWhenFocused(true);
        editor.setPopupMenuEnabled(false);
        editor.setInputRestrictions(
            hexadecimal ? 2 : 4,
            hexadecimal
                ? "0123456789abcdefABCDEF"
                : "-0123456789");
        editor.setTooltip(
            hexadecimal
                ? juce::String::fromUTF8(
                    "8bit 16進数 00～FF。Enterで確定")
                : juce::String::fromUTF8(
                    "符号付き10進数 -128～127。Enterで確定。"
                    "左右キーで桁を移動できます"));
    }

    static std::optional<int> parseEditorValue(
        const juce::String& text,
        bool hexadecimal) {
        const auto trimmed = text.trim();
        const auto utf8 = trimmed.toUTF8();
        const std::string_view source(
            utf8.getAddress(),
            static_cast<std::size_t>(utf8.sizeInBytes() - 1));
        if (source.empty()
            || (hexadecimal && source.size() != 2)) {
            return std::nullopt;
        }
        int value{};
        const auto parsed = std::from_chars(
            source.data(),
            source.data() + source.size(),
            value,
            hexadecimal ? 16 : 10);
        if (parsed.ec != std::errc{}
            || parsed.ptr != source.data() + source.size()) {
            return std::nullopt;
        }
        if (hexadecimal) {
            if (value < 0 || value > 255) {
                return std::nullopt;
            }
            return static_cast<int>(
                static_cast<std::int8_t>(
                    static_cast<std::uint8_t>(value)));
        }
        return value >= -128 && value <= 127
            ? std::optional<int>(value)
            : std::nullopt;
    }

    void commitValueEditor(
        std::size_t index,
        bool hexadecimal) {
        if (reference_waveform_) {
            waveform_ = *reference_waveform_;
            reference_waveform_.reset();
            if (on_preview_cleared_) {
                on_preview_cleared_();
            }
        }
        auto& editor = hexadecimal
            ? *hex_editors_[index]
            : *decimal_editors_[index];
        const auto parsed =
            parseEditorValue(editor.getText(), hexadecimal);
        if (!parsed) {
            syncValueEditors(waveform_);
            return;
        }
        reference_waveform_.reset();
        const auto value = static_cast<std::int8_t>(*parsed);
        if (waveform_[index] == value) {
            syncValueEditors(waveform_);
            return;
        }
        waveform_[index] = value;
        syncValueEditors(waveform_);
        repaint();
        if (on_value_commit_) {
            on_value_commit_(waveform_);
        }
    }

    void syncValueEditors(const SccWaveform& waveform) {
        for (std::size_t index = 0; index < waveform.size(); ++index) {
            decimal_editors_[index]->setText(
                juce::String(static_cast<int>(waveform[index])), false);
            hex_editors_[index]->setText(
                juce::String::formatted(
                    "%02X",
                    static_cast<unsigned int>(
                        static_cast<std::uint8_t>(waveform[index]))),
                false);
        }
    }

    [[nodiscard]] static float sampleY(
        std::int8_t sample,
        juce::Rectangle<int> graph) {
        const auto normalized =
            (127.0F - static_cast<float>(sample)) / 255.0F;
        return static_cast<float>(graph.getY())
            + normalized * static_cast<float>(graph.getHeight());
    }

    void updateFromMouse(juce::Point<float> position) {
        const auto graph = graphBounds();
        if (graph.isEmpty()) {
            return;
        }
        const auto relative_x = juce::jlimit(
            0.0F,
            static_cast<float>(graph.getWidth() - 1),
            position.x - static_cast<float>(graph.getX()));
        const auto index = juce::jlimit(
            0,
            static_cast<int>(waveform_.size()) - 1,
            static_cast<int>(
                relative_x * waveform_.size()
                / static_cast<float>(graph.getWidth())));
        const auto normalized_y = juce::jlimit(
            0.0F,
            1.0F,
            (position.y - static_cast<float>(graph.getY()))
                / static_cast<float>(graph.getHeight()));
        const auto value = juce::jlimit(
            -128,
            127,
            juce::roundToInt(127.0F - normalized_y * 255.0F));
        selected_index_ = index;
        auto& sample =
            waveform_[static_cast<std::size_t>(index)];
        const auto next = static_cast<std::int8_t>(value);
        if (sample == next) {
            repaint();
            return;
        }
        sample = next;
        repaint();
        if (on_live_edit_) {
            on_live_edit_(waveform_);
        }
    }

    SccWaveform waveform_{};
    std::optional<SccWaveform> reference_waveform_;
    SccWaveform drag_start_{};
    CommitCallback on_commit_;
    LiveEditCallback on_live_edit_;
    ValueCommitCallback on_value_commit_;
    std::function<void()> on_preview_cleared_;
    juce::Image background_image_;
    bool background_visible_{true};
    float background_opacity_{0.35F};
    int background_x_{0};
    int background_y_{0};
    int background_width_{256};
    int background_height_{128};
    std::array<std::unique_ptr<juce::TextEditor>, 32>
        decimal_editors_;
    std::array<std::unique_ptr<juce::TextEditor>, 32>
        hex_editors_;
    int selected_index_{-1};
};

class SharedAudioService final {
public:
    SharedAudioService() {
        master_volume_percent_ = loadMasterVolumePercent();
        audio_.setMasterVolumePercent(
            static_cast<std::uint32_t>(master_volume_percent_));
        loadAndApplySoundOutputSettings();
        running_ = audio_.start(engine_);
    }

    ~SharedAudioService() {
        static_cast<void>(
            engine_.submit(mgstc::engine::EngineCommand::stop()));
        audio_.stop();
    }

    SharedAudioService(const SharedAudioService&) = delete;
    SharedAudioService& operator=(const SharedAudioService&) = delete;

    [[nodiscard]] mgstc::engine::RealtimeEngineHost& engine() noexcept {
        return engine_;
    }

    [[nodiscard]] bool running() const noexcept {
        return running_;
    }

    [[nodiscard]] int masterVolumePercent() const noexcept {
        return master_volume_percent_;
    }

    [[nodiscard]] std::uint64_t masterVolumeRevision() const noexcept {
        return master_volume_revision_;
    }

    void setMasterVolumePercent(int percent) {
        const auto next = juce::jlimit(0, 100, percent);
        if (next == master_volume_percent_) {
            return;
        }
        master_volume_percent_ = next;
        audio_.setMasterVolumePercent(
            static_cast<std::uint32_t>(next));
        saveMasterVolumePercent();
        ++master_volume_revision_;
    }

    void clearScopeFrames() {
        mgstc::engine::OpllScopeFrame frame{};
        while (engine_.pollOpllScope(frame)) {
        }
    }

    void setSharedSccWaveform(const SccWaveform& waveform) {
        shared_scc_wave_ = waveform;
    }

    [[nodiscard]] const SccWaveform& sharedSccWaveform() const noexcept {
        return shared_scc_wave_;
    }

    void setSharedOpllPatch(
        const mgstc::engine::OpllPatchParameters& patch) {
        shared_opll_patch_ = patch;
    }

    [[nodiscard]] const mgstc::engine::OpllPatchParameters&
    sharedOpllPatch() const noexcept {
        return shared_opll_patch_;
    }

    // SCC／OPLL単音色エディタ共通の試聴プログラムを構築する。
    // 片方のエディタが再構成しても、もう一方の確定音色を消さない。
    [[nodiscard]] bool submitSharedEditorProgram(
        const SccWaveform& scc_wave,
        const mgstc::engine::OpllPatchParameters& opll_patch,
        bool retrigger,
        std::uint8_t retrigger_track,
        std::uint8_t midi_note) {
        auto edit = engine_.beginProgramEdit();
        if (!edit.valid()) {
            return false;
        }

        std::array<std::uint8_t, 32> raw_wave{};
        std::transform(
            scc_wave.begin(),
            scc_wave.end(),
            raw_wave.begin(),
            [](std::int8_t sample) {
                return static_cast<std::uint8_t>(sample);
            });
        const auto opll_registers =
            mgstc::engine::encodeOpllPatch(opll_patch);

        bool configured =
            edit.engine->session().setSequenceEnvelope(
                kPsgTrack, {0x40, 0xEF, 0x01, 0x60})
            && edit.engine->session().setPsgToneNoise(
                kPsgTrack, 1, 0)
            && edit.engine->session().setPsgFixedVolume(
                kPsgTrack, 15)
            && edit.engine->session().mapper().defineSccPatch(
                0, raw_wave)
                == mgstc::engine::MapError::None
            && edit.engine->session().mapper().defineOpllOriginalPatch(
                16, opll_registers)
                == mgstc::engine::MapError::None;
        for (std::uint8_t track = kSccTrack;
             track < kSccTrack + 5;
             ++track) {
            configured = configured
                && edit.engine->session().setSequenceEnvelope(
                    track, {0x10, 0x00, 0x40, 0xEF, 0x01, 0x60})
                && edit.engine->session().setTrackVolume(track, 15);
        }
        for (std::uint8_t track = kOpllTrack;
             track < kOpllTrack + 9;
             ++track) {
            configured = configured
                && edit.engine->session().setSequenceEnvelope(
                    track, {0x10, 0x10, 0x40, 0xEF, 0x01, 0x60});
        }
        if (!configured) {
            static_cast<void>(engine_.discardProgramEdit(edit));
            return false;
        }

        const auto submitted = engine_.submitProgram(
            edit,
            {
                .retrigger = retrigger,
                .track = retrigger_track,
                .midi_note = midi_note,
            });
        if (!submitted && edit.valid()) {
            static_cast<void>(engine_.discardProgramEdit(edit));
        }
        return submitted;
    }

    void saveSoundOutputSettings() const {
        const auto file = settingsFile();
        if (file.getParentDirectory().createDirectory().failed()) {
            return;
        }
        const auto path = file.getFullPathName();
        const auto& settings = engine_.mamidiSettings();
        const auto kind =
            engine_.soundOutputKind()
                    == mgstc::engine::SoundOutputKind::MAmidiMemo
                ? L"MAmidiMemo"
                : L"Emulator";
        static_cast<void>(WritePrivateProfileStringW(
            L"SoundOutput",
            L"Kind",
            kind,
            path.toWideCharPointer()));
        static_cast<void>(WritePrivateProfileStringW(
            L"SoundOutput",
            L"Host",
            juce::String(settings.host).toWideCharPointer(),
            path.toWideCharPointer()));
        static_cast<void>(WritePrivateProfileStringW(
            L"SoundOutput",
            L"Port",
            juce::String(static_cast<int>(settings.port))
                .toWideCharPointer(),
            path.toWideCharPointer()));
        static_cast<void>(WritePrivateProfileStringW(
            L"SoundOutput",
            L"UnitNo",
            juce::String(static_cast<int>(settings.unit_no))
                .toWideCharPointer(),
            path.toWideCharPointer()));
        static_cast<void>(WritePrivateProfileStringW(
            L"SoundOutput",
            L"SccPlus",
            settings.scc_plus ? L"1" : L"0",
            path.toWideCharPointer()));
        static_cast<void>(WritePrivateProfileStringW(
            L"SoundOutput",
            L"WaveformMonitor",
            settings.waveform_monitor ? L"1" : L"0",
            path.toWideCharPointer()));
        static_cast<void>(WritePrivateProfileStringW(
            nullptr,
            nullptr,
            nullptr,
            path.toWideCharPointer()));
    }

private:
    [[nodiscard]] static juce::File settingsFile() {
        return applicationDataDirectory().getChildFile(
            "settings-v1.ini");
    }

    void loadAndApplySoundOutputSettings() {
        const auto file = settingsFile();
        mgstc::engine::MAmidiOutputSettings settings{};
        wchar_t host_buffer[256]{};
        if (file.existsAsFile()) {
            static_cast<void>(GetPrivateProfileStringW(
                L"SoundOutput",
                L"Host",
                L"localhost",
                host_buffer,
                static_cast<DWORD>(std::size(host_buffer)),
                file.getFullPathName().toWideCharPointer()));
            settings.host =
                juce::String(host_buffer).toStdString();
            settings.port = static_cast<std::uint16_t>(juce::jlimit(
                1,
                65535,
                static_cast<int>(GetPrivateProfileIntW(
                    L"SoundOutput",
                    L"Port",
                    30000,
                    file.getFullPathName().toWideCharPointer()))));
            settings.unit_no = static_cast<std::uint8_t>(juce::jlimit(
                0,
                255,
                static_cast<int>(GetPrivateProfileIntW(
                    L"SoundOutput",
                    L"UnitNo",
                    0,
                    file.getFullPathName().toWideCharPointer()))));
            settings.scc_plus =
                GetPrivateProfileIntW(
                    L"SoundOutput",
                    L"SccPlus",
                    0,
                    file.getFullPathName().toWideCharPointer())
                != 0;
            settings.waveform_monitor =
                GetPrivateProfileIntW(
                    L"SoundOutput",
                    L"WaveformMonitor",
                    0,
                    file.getFullPathName().toWideCharPointer())
                != 0;
        }
        engine_.setMAmidiSettings(std::move(settings));

        wchar_t kind_buffer[64]{};
        if (file.existsAsFile()) {
            static_cast<void>(GetPrivateProfileStringW(
                L"SoundOutput",
                L"Kind",
                L"Emulator",
                kind_buffer,
                static_cast<DWORD>(std::size(kind_buffer)),
                file.getFullPathName().toWideCharPointer()));
        }
        if (juce::String(kind_buffer) == "MAmidiMemo") {
            // Connect if possible; keep Emulator if chip_server is down.
            static_cast<void>(engine_.setSoundOutputKind(
                mgstc::engine::SoundOutputKind::MAmidiMemo));
        }
    }

    [[nodiscard]] static int loadMasterVolumePercent() {
        const auto file = settingsFile();
        if (!file.existsAsFile()) {
            return 100;
        }
        return juce::jlimit(
            0,
            100,
            static_cast<int>(GetPrivateProfileIntW(
                L"Application",
                L"MasterVolumePercent",
                100,
                file.getFullPathName().toWideCharPointer())));
    }

    void saveMasterVolumePercent() const {
        const auto file = settingsFile();
        if (file.getParentDirectory().createDirectory().failed()) {
            return;
        }
        const auto path_text = file.getFullPathName();
        const auto value = juce::String(master_volume_percent_);
        static_cast<void>(WritePrivateProfileStringW(
            L"Application",
            L"SchemaVersion",
            L"1",
            path_text.toWideCharPointer()));
        static_cast<void>(WritePrivateProfileStringW(
            L"Application",
            L"MasterVolumePercent",
            value.toWideCharPointer(),
            path_text.toWideCharPointer()));
        static_cast<void>(WritePrivateProfileStringW(
            nullptr,
            nullptr,
            nullptr,
            path_text.toWideCharPointer()));
    }

    mgstc::engine::RealtimeEngineHost engine_;
    mgstc::audio::WasapiAudioSink audio_;
    SccWaveform shared_scc_wave_{
        mgstc::engine::generateSccPreset(
            mgstc::engine::SccWavePreset::Sine,
            mgstc::engine::SccHarmonic::One)};
    mgstc::engine::OpllPatchParameters shared_opll_patch_{
        mgstc::engine::defaultOpllPatch()};
    bool running_{};
    int master_volume_percent_{100};
    std::uint64_t master_volume_revision_{1};
};

void configureMasterVolumeSlider(
    juce::Slider& slider,
    SharedAudioService& audio_service,
    juce::Component* popup_parent) {
    slider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    slider.setRange(0.0, 100.0, 1.0);
    slider.setValue(
        audio_service.masterVolumePercent(),
        juce::dontSendNotification);
    slider.setVelocityBasedMode(false);
    slider.setMouseDragSensitivity(120);
    slider.setScrollWheelEnabled(true);
    slider.setDoubleClickReturnValue(true, 100.0);
    slider.setTextBoxStyle(
        juce::Slider::NoTextBox, false, 0, 0);
    slider.setTextValueSuffix("%");
    slider.setPopupDisplayEnabled(true, true, popup_parent);
    slider.setTooltip(
        juce::String::fromUTF8(
            "マスターボリューム 0～100%。PSG・SCC・OPLLの"
            "合成後、Windowsへ出力する全画面共通音量です。"
            "上下ドラッグまたはホイールで変更、"
            "ダブルクリックで100%へ戻します"));
    slider.onValueChange = [&slider, &audio_service] {
        audio_service.setMasterVolumePercent(
            juce::roundToInt(slider.getValue()));
    };
}

void synchronizeMasterVolumeSlider(
    juce::Slider& slider,
    std::uint64_t& observed_revision,
    const SharedAudioService& audio_service) {
    const auto revision = audio_service.masterVolumeRevision();
    if (revision == observed_revision) {
        return;
    }
    observed_revision = revision;
    slider.setValue(
        audio_service.masterVolumePercent(),
        juce::dontSendNotification);
}

class PreviewComponent final : public juce::Component {
public:
    explicit PreviewComponent(SharedAudioService& audio_service)
        : engine_(audio_service.engine()),
          tooltip_window_(this, 500) {
        title_.setText(
            juce::String::fromUTF8("MGS Tone Craft"),
            juce::dontSendNotification);
        title_.setFont(juce::FontOptions(22.0F, juce::Font::bold));
        title_.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(title_);

        description_.setText(
            juce::String::fromUTF8(
                "音源エンジンとWASAPI出力を使い、"
                "総合・SCC・OPLLの音色を編集します。"),
            juce::dontSendNotification);
        description_.setJustificationType(juce::Justification::topLeft);
        addAndMakeVisible(description_);

        source_.addItem("PSG Ch.A", 1);
        source_.addItem("SCC Ch.D", 2);
        source_.addItem("OPLL Ch.I", 3);
        source_.setSelectedId(1, juce::dontSendNotification);
        source_.setTooltip(
            juce::String::fromUTF8("試聴する音源を選択します"));
        source_.onChange = [this] {
            stopNote();
            updateStatus();
        };
        addAndMakeVisible(source_);

        audition_.setButtonText(
            juce::String::fromUTF8("中央Cを発音"));
        audition_.setClickingTogglesState(true);
        audition_.setTooltip(
            juce::String::fromUTF8(
                "選択中の音源で中央Cを発音します。"
                "もう一度押すと停止します"));
        audition_.onClick = [this] {
            if (audition_.getToggleState()) {
                startNote();
            } else {
                stopNote();
            }
        };
        addAndMakeVisible(audition_);

        tooltip_test_.setButtonText("?");
        tooltip_test_.setTooltip(
            juce::String::fromUTF8(
                "JUCE標準ツールチップです。"
                "無効なボタンでも表示できます"));
        tooltip_test_.setEnabled(false);
        addAndMakeVisible(tooltip_test_);

        status_.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(status_);

        setSize(680, 300);
        engine_ready_ = audio_service.running() && prepareEngine();
        updateStatus();
    }

    ~PreviewComponent() override {
        stopNote();
    }

    void paint(juce::Graphics& graphics) override {
        graphics.fillAll(
            getLookAndFeel().findColour(
                juce::ResizableWindow::backgroundColourId));

        const auto panel = getLocalBounds()
            .reduced(20)
            .withTrimmedTop(88)
            .withTrimmedBottom(44);
        graphics.setColour(
            getLookAndFeel().findColour(
                juce::TextEditor::backgroundColourId));
        graphics.fillRoundedRectangle(panel.toFloat(), 8.0F);
        graphics.setColour(
            getLookAndFeel().findColour(
                juce::TextEditor::outlineColourId));
        graphics.drawRoundedRectangle(panel.toFloat(), 8.0F, 1.0F);
    }

    void resized() override {
        auto area = getLocalBounds().reduced(24);
        title_.setBounds(area.removeFromTop(34));
        description_.setBounds(area.removeFromTop(50));
        area.removeFromTop(34);

        auto controls = area.removeFromTop(42);
        source_.setBounds(controls.removeFromLeft(190));
        controls.removeFromLeft(12);
        audition_.setBounds(controls.removeFromLeft(180));
        controls.removeFromLeft(12);
        tooltip_test_.setBounds(controls.removeFromLeft(42));

        area.removeFromTop(28);
        status_.setBounds(area.removeFromTop(32));
    }

private:
    [[nodiscard]] std::uint8_t selectedTrack() const noexcept {
        switch (source_.getSelectedId()) {
        case 2:
            return kSccTrack;
        case 3:
            return kOpllTrack;
        default:
            return kPsgTrack;
        }
    }

    bool prepareEngine() {
        std::array<std::uint8_t, 32> scc_wave{};
        for (std::size_t index = 0; index < scc_wave.size(); ++index) {
            scc_wave[index] = index < 16 ? 0x7F : 0x81;
        }

        auto edit = engine_.beginProgramEdit();
        if (!edit.valid()) {
            return false;
        }

        const auto opll_registers = mgstc::engine::encodeOpllPatch(
            mgstc::engine::defaultOpllPatch());
        const bool configured =
            edit.engine->session().setSequenceEnvelope(
                kPsgTrack,
                {0x40, 0xEF, 0x01, 0x60})
            && edit.engine->session().setSequenceEnvelope(
                kSccTrack,
                {0x10, 0x00, 0x40, 0xEF, 0x01, 0x60})
            && edit.engine->session().setSequenceEnvelope(
                kOpllTrack,
                {0x10, 0x10, 0x40, 0xEF, 0x01, 0x60})
            && edit.engine->session().setPsgToneNoise(
                kPsgTrack, 1, 0)
            && edit.engine->session().setPsgFixedVolume(
                kPsgTrack, 15)
            && edit.engine->session().mapper().defineSccPatch(
                0, scc_wave) == mgstc::engine::MapError::None
            && edit.engine->session().mapper().defineOpllOriginalPatch(
                16, opll_registers)
                == mgstc::engine::MapError::None;
        if (!configured) {
            static_cast<void>(engine_.discardProgramEdit(edit));
            return false;
        }

        return engine_.submitProgram(edit);
    }

    void startNote() {
        if (!engine_ready_) {
            audition_.setToggleState(
                false, juce::dontSendNotification);
            updateStatus();
            return;
        }

        stopNote();
        const auto track = selectedTrack();
        if (engine_.submit(
                mgstc::engine::EngineCommand::noteOn(
                    track, kPreviewNote))) {
            sounding_track_ = track;
        } else {
            audition_.setToggleState(
                false, juce::dontSendNotification);
        }
        updateStatus();
    }

    void stopNote() {
        if (sounding_track_ <= kOpllTrack) {
            static_cast<void>(
                engine_.submit(
                    mgstc::engine::EngineCommand::noteOff(
                        sounding_track_)));
        }
        sounding_track_ = 0xFF;
        audition_.setToggleState(
            false, juce::dontSendNotification);
        updateStatus();
    }

    void updateStatus() {
        juce::String text;
        if (!engine_ready_) {
            text = juce::String::fromUTF8(
                "音声出力を開始できませんでした");
        } else if (sounding_track_ <= kOpllTrack) {
            text = juce::String::fromUTF8("発音中: 中央C / ");
            text += source_.getText();
        } else {
            text = juce::String::fromUTF8(
                "準備完了 - 各コントロールにカーソルを置くと"
                "説明を表示します");
        }
        status_.setText(text, juce::dontSendNotification);
    }

    mgstc::engine::RealtimeEngineHost& engine_;
    juce::TooltipWindow tooltip_window_;
    juce::Label title_;
    juce::Label description_;
    juce::ComboBox source_;
    juce::TextButton audition_;
    juce::TextButton tooltip_test_;
    juce::Label status_;
    std::uint8_t sounding_track_{0xFF};
    bool engine_ready_{};
};

class MgstcLookAndFeel final : public juce::LookAndFeel_V4 {
public:
    MgstcLookAndFeel() {
        const auto background = juce::Colour(0xFF1F2730);
        const auto panel = juce::Colour(0xFF2A3540);
        const auto border = juce::Colour(0xFF607080);
        const auto text = juce::Colour(0xFFF2F4F5);
        const auto blue = juce::Colour(0xFF0072B2);
        const auto cyan = juce::Colour(0xFF56B4E9);
        setColour(juce::ResizableWindow::backgroundColourId, background);
        setColour(juce::TextEditor::backgroundColourId, panel);
        setColour(juce::TextEditor::outlineColourId, border);
        setColour(juce::TextEditor::textColourId, text);
        setColour(juce::Label::textColourId, text);
        setColour(juce::ComboBox::backgroundColourId, panel);
        setColour(juce::ComboBox::outlineColourId, border);
        setColour(juce::ComboBox::textColourId, text);
        setColour(juce::Slider::backgroundColourId,
                  juce::Colour(0xFF465562));
        setColour(juce::Slider::trackColourId, cyan);
        setColour(juce::Slider::thumbColourId, text);
        setColour(juce::Slider::rotarySliderFillColourId, blue);
        setColour(juce::Slider::rotarySliderOutlineColourId, border);
        setColour(juce::TextButton::buttonColourId, panel);
        setColour(juce::TextButton::buttonOnColourId, blue);
        setColour(juce::TextButton::textColourOffId, text);
        setColour(juce::TextButton::textColourOnId, text);
    }
};

class SwitchLookAndFeel final : public juce::LookAndFeel_V4 {
public:
    void drawToggleButton(
        juce::Graphics& graphics,
        juce::ToggleButton& button,
        bool should_draw_highlight,
        bool should_draw_down) override {
        auto bounds = button.getLocalBounds().toFloat();
        const auto enabled = button.isEnabled();
        const auto active = button.getToggleState();
        const auto switch_bounds = juce::Rectangle<float>(
            bounds.getX(),
            bounds.getCentreY() - 9.0F,
            34.0F,
            18.0F);
        auto track = active
            ? juce::Colour(0xFF2FA9D6)
            : juce::Colour(0xFF4C5863);
        if (should_draw_highlight || should_draw_down) {
            track = track.brighter(0.12F);
        }
        if (!enabled) {
            track = track.withMultipliedAlpha(0.45F);
        }
        graphics.setColour(track);
        graphics.fillRoundedRectangle(switch_bounds, 9.0F);

        const auto thumb_x = active
            ? switch_bounds.getRight() - 16.0F
            : switch_bounds.getX() + 2.0F;
        graphics.setColour(
            juce::Colours::white.withMultipliedAlpha(
                enabled ? 0.95F : 0.45F));
        graphics.fillEllipse(
            thumb_x, switch_bounds.getY() + 2.0F, 14.0F, 14.0F);

        graphics.setColour(
            juce::Colour(0xFFE6EDF3).withMultipliedAlpha(
                enabled ? 1.0F : 0.45F));
        graphics.setFont(juce::FontOptions(13.0F));
        graphics.drawFittedText(
            button.getButtonText(),
            button.getLocalBounds().withTrimmedLeft(42),
            juce::Justification::centredLeft,
            1);

        if (button.hasKeyboardFocus(true)) {
            graphics.setColour(juce::Colour(0xFF53E3A6));
            graphics.drawRoundedRectangle(
                bounds.reduced(0.5F), 4.0F, 1.0F);
        }
    }
};

class SharedMidiInputService final
    : private juce::MidiInputCallback,
      private juce::Timer {
public:
    SharedMidiInputService() {
        pending_messages_.reserve(2048);
        const auto saved = loadSelectedIdentifier();
        if (saved) {
            selected_identifier_ = *saved;
        }
        refreshDevices(!saved.has_value());
        if (!saved) {
            persistSelectedIdentifier(selected_identifier_);
        }
        startTimerHz(2);
    }

    ~SharedMidiInputService() override {
        stopTimer();
        closeDevice();
    }

    SharedMidiInputService(const SharedMidiInputService&) = delete;
    SharedMidiInputService& operator=(
        const SharedMidiInputService&) = delete;

    [[nodiscard]] const juce::Array<juce::MidiDeviceInfo>&
    devices() const noexcept {
        return devices_;
    }

    [[nodiscard]] int selectedComboId() const noexcept {
        for (int index = 0; index < devices_.size(); ++index) {
            if (devices_.getReference(index).identifier
                == selected_identifier_) {
                return index + 2;
            }
        }
        return 1;
    }

    [[nodiscard]] const juce::String& statusText() const noexcept {
        return status_text_;
    }

    [[nodiscard]] std::uint64_t revision() const noexcept {
        return revision_;
    }

    void refreshDevices(bool select_first = false) {
        const auto current = juce::MidiInput::getAvailableDevices();
        devices_ = current;
        if (selected_identifier_.isEmpty()
            && select_first && !devices_.isEmpty()) {
            selected_identifier_ = devices_.getFirst().identifier;
        }
        reopenSelectedDevice();
        ++revision_;
    }

    void selectComboId(int selected_id, bool persist) {
        closeDevice();
        const int index = selected_id - 2;
        selected_identifier_.clear();
        if (index >= 0 && index < devices_.size()) {
            selected_identifier_ =
                devices_.getReference(index).identifier;
        }
        if (persist) {
            persistSelectedIdentifier(selected_identifier_);
        }
        reopenSelectedDevice();
        clearPendingMessages();
        ++revision_;
    }

    [[nodiscard]] std::vector<juce::MidiMessage>
    takePendingMessages() {
        std::vector<juce::MidiMessage> result;
        const juce::ScopedLock lock(queue_lock_);
        result.assign(
            pending_messages_.begin(), pending_messages_.end());
        pending_messages_.clear();
        return result;
    }

    void clearPendingMessages() {
        const juce::ScopedLock lock(queue_lock_);
        pending_messages_.clear();
    }

private:
    [[nodiscard]] static juce::File settingsFile() {
        return applicationDataDirectory().getChildFile(
            "settings-v1.ini");
    }

    [[nodiscard]] static std::optional<juce::String>
    loadSelectedIdentifier() {
        const auto file = settingsFile();
        if (GetPrivateProfileIntW(
                L"Application",
                L"MidiInputConfigured",
                0,
                file.getFullPathName().toWideCharPointer()) == 0) {
            return std::nullopt;
        }
        std::array<wchar_t, 1024> value{};
        static_cast<void>(GetPrivateProfileStringW(
            L"Application",
            L"MidiInputIdentifier",
            L"",
            value.data(),
            static_cast<DWORD>(value.size()),
            file.getFullPathName().toWideCharPointer()));
        return juce::String(value.data());
    }

    static void persistSelectedIdentifier(
        const juce::String& identifier) {
        const auto file = settingsFile();
        if (file.getParentDirectory().createDirectory().failed()) {
            return;
        }
        const auto path = file.getFullPathName();
        const auto revision =
            juce::String(juce::Time::currentTimeMillis());
        static_cast<void>(WritePrivateProfileStringW(
            L"Application", L"SchemaVersion", L"1",
            path.toWideCharPointer()));
        static_cast<void>(WritePrivateProfileStringW(
            L"Application", L"MidiInputConfigured", L"1",
            path.toWideCharPointer()));
        static_cast<void>(WritePrivateProfileStringW(
            L"Application", L"MidiInputIdentifier",
            identifier.toWideCharPointer(),
            path.toWideCharPointer()));
        static_cast<void>(WritePrivateProfileStringW(
            L"Application", L"MidiInputRevision",
            revision.toWideCharPointer(),
            path.toWideCharPointer()));
        static_cast<void>(WritePrivateProfileStringW(
            nullptr, nullptr, nullptr, path.toWideCharPointer()));
    }

    void reopenSelectedDevice() {
        closeDevice();
        if (selected_identifier_.isEmpty()) {
            status_text_ = juce::String::fromUTF8(
                "画面鍵盤 / PCキー Z=C・上段+1oct");
            return;
        }
        const auto found = std::find_if(
            devices_.begin(), devices_.end(),
            [this](const auto& device) {
                return device.identifier == selected_identifier_;
            });
        if (found == devices_.end()) {
            status_text_ = juce::String::fromUTF8(
                "選択中のMIDI入力は現在接続されていません");
            return;
        }
        device_ = juce::MidiInput::openDevice(
            found->identifier, this);
        if (!device_) {
            status_text_ = juce::String::fromUTF8(
                "MIDI入力を開けませんでした");
            return;
        }
        device_->start();
        status_text_ =
            juce::String::fromUTF8("接続: ") + found->name;
    }

    void closeDevice() {
        if (device_) {
            device_->stop();
            device_.reset();
        }
    }

    void handleIncomingMidiMessage(
        juce::MidiInput*,
        const juce::MidiMessage& message) override {
        const juce::ScopedLock lock(queue_lock_);
        if (pending_messages_.size() < 2048) {
            pending_messages_.push_back(message);
        }
    }

    void timerCallback() override {
        const auto current = juce::MidiInput::getAvailableDevices();
        if (current != devices_) {
            refreshDevices(false);
        } else if (!device_ && !selected_identifier_.isEmpty()) {
            reopenSelectedDevice();
            ++revision_;
        }
    }

    juce::Array<juce::MidiDeviceInfo> devices_;
    std::unique_ptr<juce::MidiInput> device_;
    juce::String selected_identifier_;
    juce::String status_text_;
    juce::CriticalSection queue_lock_;
    std::vector<juce::MidiMessage> pending_messages_;
    std::uint64_t revision_{1};
};

class MgscMidiKeyboardComponent final
    : public juce::MidiKeyboardComponent {
public:
    MgscMidiKeyboardComponent(
        juce::MidiKeyboardState& state,
        Orientation orientation)
        : juce::MidiKeyboardComponent(state, orientation) {}

    juce::String getWhiteNoteText(int midi_note_number) override {
        if (midi_note_number % 12 != 0) {
            return {};
        }
        return "C" + juce::String(midi_note_number / 12 - 1);
    }
};

class PerformanceKeyboard final
    : public juce::Component,
      private juce::MidiKeyboardState::Listener,
      private juce::Timer {
public:
    using NoteCallback = std::function<void(std::uint8_t)>;
    using ModeCallback = std::function<void(bool)>;
    using SuppressCallback = std::function<bool()>;

    explicit PerformanceKeyboard(
        SharedMidiInputService& midi_service,
        SharedAudioService& audio_service)
        : midi_service_(midi_service),
          audio_service_(audio_service),
          keyboard_(
              keyboard_state_,
              juce::MidiKeyboardComponent::horizontalKeyboard) {
        keyboard_state_.addListener(this);
        keyboard_.setAvailableRange(24, 119);
        keyboard_.setLowestVisibleKey(24);
        keyboard_.setScrollButtonsVisible(false);
        keyboard_.setMidiChannel(kScreenMidiChannel);
        keyboard_.setColour(
            juce::MidiKeyboardComponent::keyDownOverlayColourId,
            juce::Colour(0xFF35C4ED));
        // JUCE default is "awsedftgyhujkolp;" (home-row). Spec uses
        // Z/S/X… + Q-row via pollPcKeyboard scan codes instead.
        keyboard_.clearKeyMappings();
        keyboard_.setWantsKeyboardFocus(false);
        addAndMakeVisible(keyboard_);

        midi_input_.setTextWhenNothingSelected(
            juce::String::fromUTF8("MIDI入力なし"));
        midi_input_.setTooltip(
            juce::String::fromUTF8(
                "演奏に使用するMIDI入力機器を選択します"));
        midi_input_.onChange = [this] {
            if (!syncing_midi_controls_) {
                allNotesOff();
                midi_service_.selectComboId(
                    midi_input_.getSelectedId(), true);
                syncMidiControls();
            }
        };
        addChildComponent(midi_input_);

        octave_down_.setButtonText(",");
        octave_down_.setTooltip(
            juce::String::fromUTF8(
                "PCキーボードのオクターブを下げます"));
        octave_down_.onClick = [this] { changePcOctave(-1); };
        addAndMakeVisible(octave_down_);

        octave_up_.setButtonText(".");
        octave_up_.setTooltip(
            juce::String::fromUTF8(
                "PCキーボードのオクターブを上げます"));
        octave_up_.onClick = [this] { changePcOctave(1); };
        addAndMakeVisible(octave_up_);

        pc_octave_ = loadPcOctave();
        updateOctaveLabel();
        octave_label_.setJustificationType(
            juce::Justification::centred);
        addAndMakeVisible(octave_label_);

        midi_status_.setJustificationType(
            juce::Justification::centredLeft);
        midi_status_.setColour(
            juce::Label::textColourId,
            juce::Colour(0xFF9AA8B5));
        addAndMakeVisible(midi_status_);

        mode_.setButtonText("Poly");
        mode_.setTooltip(
            juce::String::fromUTF8(
                "Poly: 複数キーを同時発音 / Mono: 従来の単音発音"));
        mode_.onClick = [this] {
            allNotesOff();
            polyphonic_ = !polyphonic_;
            mode_.setButtonText(polyphonic_ ? "Poly" : "Mono");
            if (mode_changed_) {
                mode_changed_(polyphonic_);
            }
        };
        addAndMakeVisible(mode_);

        syncMidiControls();
        startTimerHz(60);
    }

    ~PerformanceKeyboard() override {
        stopTimer();
        keyboard_state_.removeListener(this);
    }

    void setCallbacks(
        NoteCallback note_on,
        NoteCallback note_off) {
        note_on_ = std::move(note_on);
        note_off_ = std::move(note_off);
    }

    void setSuppressPcInputCallback(SuppressCallback callback) {
        suppress_pc_input_ = std::move(callback);
    }

    void setModeCallback(ModeCallback callback) {
        mode_changed_ = std::move(callback);
    }

    [[nodiscard]] bool polyphonic() const noexcept {
        return polyphonic_;
    }

    void showSettingsDialog() {
        showAppSettings();
    }

    [[nodiscard]] bool shouldConsumeKeyPress(
        const juce::KeyPress& key) const {
        if (suppress_pc_input_ && suppress_pc_input_()) {
            return false;
        }
        const auto modifiers = key.getModifiers();
        if (modifiers.isCtrlDown()
            || modifiers.isAltDown()
            || modifiers.isCommandDown()) {
            return false;
        }
        const auto character = key.getTextCharacter();
        return character >= 0x20 && character != 0x7F;
    }

    [[nodiscard]] bool hasActiveNote() const noexcept {
        return !held_notes_.empty();
    }

    [[nodiscard]] int pcOctave() const noexcept {
        return pc_octave_;
    }

    void allNotesOff() {
        clearPreviewNote();
        releasePcNote();
        keyboard_state_.allNotesOff(0);
    }

    void showPreviewNote(std::uint8_t note) {
        const auto clamped = static_cast<std::uint8_t>(
            juce::jlimit(24, 119, static_cast<int>(note)));
        if (preview_note_ == clamped) {
            return;
        }
        clearPreviewNote();
        juce::ScopedValueSetter<bool> scoped(preview_update_, true);
        preview_note_ = clamped;
        keyboard_state_.noteOn(
            kPreviewMidiChannel, clamped, 1.0F);
    }

    void clearPreviewNote() {
        if (!preview_note_) {
            return;
        }
        juce::ScopedValueSetter<bool> scoped(preview_update_, true);
        keyboard_state_.noteOff(
            kPreviewMidiChannel, *preview_note_, 1.0F);
        preview_note_.reset();
    }

    void resized() override {
        auto area = getLocalBounds();
        auto controls = area.removeFromTop(30);
        mode_.setBounds(controls.removeFromLeft(68));
        controls.removeFromLeft(10);
        octave_down_.setBounds(controls.removeFromLeft(34));
        controls.removeFromLeft(4);
        octave_label_.setBounds(controls.removeFromLeft(132));
        controls.removeFromLeft(4);
        octave_up_.setBounds(controls.removeFromLeft(34));
        controls.removeFromLeft(12);
        midi_status_.setBounds(controls);
        area.removeFromTop(5);
        keyboard_.setKeyWidth(
            static_cast<float>(area.getWidth()) / 56.0F);
        keyboard_.setBounds(area);
        keyboard_.setLowestVisibleKey(24);
    }

private:
    static constexpr int kPreviewMidiChannel = 14;
    static constexpr int kScreenMidiChannel = 15;
    static constexpr int kPcMidiChannel = 16;

    void showAppSettings(int initial_tab = 0) {
        syncMidiControls();

        class SettingsContent final : public juce::Component {
        public:
            SettingsContent(
                SharedMidiInputService& midi_service,
                SharedAudioService& audio_service,
                int selected_midi_id,
                int initial_tab)
                : midi_service_(midi_service),
                  audio_service_(audio_service) {
                tabs_.setOutline(0);
                addAndMakeVisible(tabs_);

                auto* midi_page = new juce::Component();
                midi_help_.setText(
                    juce::String::fromUTF8(
                        "使用するMIDI入力機器を選択してください。\n"
                        "接続状態は鍵盤上部のオクターブ操作（.）の右側へ常時表示されます。"),
                    juce::dontSendNotification);
                midi_help_.setJustificationType(
                    juce::Justification::topLeft);
                midi_help_.setColour(
                    juce::Label::textColourId,
                    juce::Colour(0xFFE6EDF3));
                midi_page->addAndMakeVisible(midi_help_);

                juce::StringArray devices;
                devices.add(juce::String::fromUTF8("MIDI入力なし"));
                for (const auto& device : midi_service_.devices()) {
                    devices.add(device.name);
                }
                midi_input_.addItemList(devices, 1);
                midi_input_.setSelectedId(
                    selected_midi_id, juce::dontSendNotification);
                midi_page->addAndMakeVisible(midi_input_);

                apply_midi_.setButtonText(
                    juce::String::fromUTF8("適用"));
                refresh_.setButtonText(
                    juce::String::fromUTF8("一覧更新"));
                midi_page->addAndMakeVisible(apply_midi_);
                midi_page->addAndMakeVisible(refresh_);

                auto* output_page = new juce::Component();
                output_help_.setText(
                    juce::String::fromUTF8(
                        "音声の出力先を選びます。MAmidiMEmo は "
                        "MAmidiMEmo.exe -chip_server 起動後に接続します。\n"
                        "切断時はエミュレータへ自動切替しません。"),
                    juce::dontSendNotification);
                output_help_.setJustificationType(
                    juce::Justification::topLeft);
                output_help_.setColour(
                    juce::Label::textColourId,
                    juce::Colour(0xFFE6EDF3));
                output_page->addAndMakeVisible(output_help_);

                output_kind_.addItem(
                    juce::String::fromUTF8("内蔵エミュレータ"), 1);
                output_kind_.addItem("MAmidiMEmo RPC", 2);
                output_kind_.setSelectedId(
                    audio_service_.engine().soundOutputKind()
                            == mgstc::engine::SoundOutputKind::MAmidiMemo
                        ? 2
                        : 1,
                    juce::dontSendNotification);
                output_page->addAndMakeVisible(output_kind_);

                const auto& mamidi =
                    audio_service_.engine().mamidiSettings();
                host_.setText(mamidi.host, false);
                port_.setInputRestrictions(5, "0123456789");
                port_.setText(juce::String(mamidi.port), false);
                unit_.setInputRestrictions(3, "0123456789");
                unit_.setText(juce::String(mamidi.unit_no), false);
                scc_plus_.setButtonText("SCC+");
                scc_plus_.setToggleState(
                    mamidi.scc_plus, juce::dontSendNotification);
                waveform_monitor_.setButtonText(
                    juce::String::fromUTF8("波形モニタ"));
                waveform_monitor_.setTooltip(
                    juce::String::fromUTF8(
                        "MAmidiMEmo 経由で実機へ出力しているときも、"
                        "画面の波形表示用に内蔵の音源シミュレーションで"
                        "波形を生成します。"
                        "パソコンのスピーカーからは鳴りません。"));
                waveform_monitor_.setToggleState(
                    mamidi.waveform_monitor,
                    juce::dontSendNotification);
                output_page->addAndMakeVisible(host_);
                output_page->addAndMakeVisible(port_);
                output_page->addAndMakeVisible(unit_);
                output_page->addAndMakeVisible(scc_plus_);
                output_page->addAndMakeVisible(waveform_monitor_);

                apply_output_.setButtonText(
                    juce::String::fromUTF8("適用"));
                reconnect_.setButtonText(
                    juce::String::fromUTF8("再接続"));
                output_page->addAndMakeVisible(apply_output_);
                output_page->addAndMakeVisible(reconnect_);

                output_status_.setColour(
                    juce::Label::textColourId,
                    juce::Colour(0xFF9AA8B5));
                output_status_.setText(
                    juce::String::fromUTF8(
                        audio_service_.engine()
                            .soundOutputStatus()
                            .c_str()),
                    juce::dontSendNotification);
                output_page->addAndMakeVisible(output_status_);

                tabs_.addTab(
                    "MIDI",
                    juce::Colour(0xFF243040),
                    midi_page,
                    true);
                tabs_.addTab(
                    juce::String::fromUTF8("出力"),
                    juce::Colour(0xFF243040),
                    output_page,
                    true);
                tabs_.addTab(
                    "About",
                    juce::Colour(0xFF243040),
                    new AboutPanel(),
                    true);
                tabs_.setCurrentTabIndex(
                    juce::jlimit(0, 2, initial_tab),
                    juce::dontSendNotification);

                close_.setButtonText(
                    juce::String::fromUTF8("閉じる"));
                addAndMakeVisible(close_);
                setSize(460, 560);
            }

            void resized() override {
                auto area = getLocalBounds().reduced(10);
                auto bottom = area.removeFromBottom(34);
                close_.setBounds(bottom.removeFromRight(96));
                area.removeFromBottom(8);
                tabs_.setBounds(area);

                if (auto* midi_page = tabs_.getTabContentComponent(0)) {
                    auto page = midi_page->getLocalBounds().reduced(12);
                    midi_help_.setBounds(page.removeFromTop(56));
                    page.removeFromTop(8);
                    midi_input_.setBounds(page.removeFromTop(28));
                    page.removeFromTop(12);
                    auto row = page.removeFromTop(30);
                    apply_midi_.setBounds(row.removeFromLeft(96));
                    row.removeFromLeft(8);
                    refresh_.setBounds(row.removeFromLeft(96));
                }
                if (auto* output_page =
                        tabs_.getTabContentComponent(1)) {
                    auto page =
                        output_page->getLocalBounds().reduced(12);
                    output_help_.setBounds(page.removeFromTop(64));
                    page.removeFromTop(8);
                    output_kind_.setBounds(page.removeFromTop(28));
                    page.removeFromTop(10);
                    auto row = page.removeFromTop(28);
                    host_.setBounds(row.removeFromLeft(180));
                    row.removeFromLeft(8);
                    port_.setBounds(row.removeFromLeft(72));
                    row.removeFromLeft(8);
                    unit_.setBounds(row.removeFromLeft(54));
                    row.removeFromLeft(8);
                    scc_plus_.setBounds(row.removeFromLeft(72));
                    page.removeFromTop(10);
                    waveform_monitor_.setBounds(page.removeFromTop(28));
                    page.removeFromTop(12);
                    auto buttons = page.removeFromTop(30);
                    apply_output_.setBounds(buttons.removeFromLeft(96));
                    buttons.removeFromLeft(8);
                    reconnect_.setBounds(buttons.removeFromLeft(96));
                    page.removeFromTop(12);
                    output_status_.setBounds(page.removeFromTop(48));
                }
            }

            void refreshOutputStatus() {
                output_status_.setText(
                    juce::String::fromUTF8(
                        audio_service_.engine()
                            .soundOutputStatus()
                            .c_str()),
                    juce::dontSendNotification);
            }

            juce::TabbedComponent tabs_{
                juce::TabbedButtonBar::TabsAtTop};
            juce::Label midi_help_;
            juce::ComboBox midi_input_;
            juce::TextButton apply_midi_;
            juce::TextButton refresh_;
            juce::Label output_help_;
            juce::ComboBox output_kind_;
            juce::TextEditor host_;
            juce::TextEditor port_;
            juce::TextEditor unit_;
            juce::ToggleButton scc_plus_;
            juce::ToggleButton waveform_monitor_;
            juce::TextButton apply_output_;
            juce::TextButton reconnect_;
            juce::Label output_status_;
            juce::TextButton close_;
            SharedMidiInputService& midi_service_;
            SharedAudioService& audio_service_;
        };

        auto* content = new SettingsContent(
            midi_service_,
            audio_service_,
            midi_input_.getSelectedId(),
            initial_tab);
        auto* dialog = new juce::DialogWindow(
            juce::String::fromUTF8("アプリ設定"),
            juce::Colour(0xFF1B222C),
            true,
            true);
        dialog->setUsingNativeTitleBar(true);
        dialog->setResizable(false, false);
        dialog->setContentOwned(content, true);
        dialog->centreAroundComponent(
            this, content->getWidth(), content->getHeight() + 32);

        juce::Component::SafePointer<PerformanceKeyboard> safe(this);
        juce::Component::SafePointer<juce::DialogWindow> safe_dialog(
            dialog);
        juce::Component::SafePointer<SettingsContent> safe_content(
            content);

        content->apply_midi_.onClick = [safe, safe_content] {
            if (safe == nullptr || safe_content == nullptr) {
                return;
            }
            safe->midi_input_.setSelectedId(
                safe_content->midi_input_.getSelectedId(),
                juce::sendNotificationSync);
            safe->syncMidiControls();
        };
        content->refresh_.onClick =
            [safe, safe_dialog] {
                if (safe == nullptr) {
                    return;
                }
                safe->midi_service_.refreshDevices(false);
                safe->syncMidiControls();
                if (safe_dialog != nullptr) {
                    safe_dialog->exitModalState(0);
                }
                juce::MessageManager::callAsync([safe] {
                    if (safe != nullptr) {
                        safe->showAppSettings(0);
                    }
                });
            };
        content->apply_output_.onClick = [safe, safe_content] {
            if (safe == nullptr || safe_content == nullptr) {
                return;
            }
            auto& engine = safe->audio_service_.engine();
            mgstc::engine::MAmidiOutputSettings settings{};
            settings.host = utf8String(
                safe_content->host_.getText().trim());
            if (settings.host.empty()) {
                settings.host = "localhost";
            }
            settings.port = static_cast<std::uint16_t>(juce::jlimit(
                1,
                65535,
                safe_content->port_.getText().getIntValue()));
            settings.unit_no = static_cast<std::uint8_t>(juce::jlimit(
                0,
                255,
                safe_content->unit_.getText().getIntValue()));
            settings.scc_plus =
                safe_content->scc_plus_.getToggleState();
            settings.waveform_monitor =
                safe_content->waveform_monitor_.getToggleState();
            engine.setMAmidiSettings(std::move(settings));

            const auto kind =
                safe_content->output_kind_.getSelectedId() == 2
                    ? mgstc::engine::SoundOutputKind::MAmidiMemo
                    : mgstc::engine::SoundOutputKind::Emulator;
            if (!engine.setSoundOutputKind(kind)
                && kind
                    == mgstc::engine::SoundOutputKind::MAmidiMemo) {
                safe_content->output_kind_.setSelectedId(
                    1, juce::dontSendNotification);
            }
            safe->audio_service_.saveSoundOutputSettings();
            safe_content->refreshOutputStatus();
            safe->syncMidiControls();
        };
        content->reconnect_.onClick = [safe, safe_content] {
            if (safe == nullptr || safe_content == nullptr) {
                return;
            }
            static_cast<void>(
                safe->audio_service_.engine().reconnectMAmidi());
            safe_content->refreshOutputStatus();
            safe->syncMidiControls();
        };
        content->close_.onClick = [safe_dialog] {
            if (safe_dialog != nullptr) {
                safe_dialog->exitModalState(0);
            }
        };

        dialog->enterModalState(true, nullptr, true);
    }

    struct HeldNote {
        int channel{};
        int note{};
    };

    [[nodiscard]] static juce::File settingsFile() {
        return applicationDataDirectory().getChildFile(
            "settings-v1.ini");
    }

    [[nodiscard]] static int loadPcOctave() {
        const auto file = settingsFile();
        const int value = GetPrivateProfileIntW(
            L"Application",
            L"PcKeyboardOctave",
            4,
            file.getFullPathName().toWideCharPointer());
        return juce::jlimit(1, 6, value);
    }

    static void savePcOctave(int octave) {
        const auto file = settingsFile();
        if (file.getParentDirectory()
                .createDirectory()
                .failed()) {
            return;
        }
        const auto path = file.getFullPathName();
        const auto value = juce::String(octave);
        static_cast<void>(WritePrivateProfileStringW(
            L"Application",
            L"SchemaVersion",
            L"1",
            path.toWideCharPointer()));
        static_cast<void>(WritePrivateProfileStringW(
            L"Application",
            L"PcKeyboardOctave",
            value.toWideCharPointer(),
            path.toWideCharPointer()));
        static_cast<void>(WritePrivateProfileStringW(
            nullptr, nullptr, nullptr, path.toWideCharPointer()));
    }

    static void saveSharedAuditionNote(int note) {
        const auto file = settingsFile();
        if (file.getParentDirectory().createDirectory().failed()) {
            return;
        }
        const auto value = juce::String(juce::jlimit(24, 119, note));
        const auto path = file.getFullPathName();
        static_cast<void>(WritePrivateProfileStringW(
            L"Application", L"LastAuditionNote",
            value.toWideCharPointer(),
            path.toWideCharPointer()));
        static_cast<void>(WritePrivateProfileStringW(
            nullptr, nullptr, nullptr, path.toWideCharPointer()));
    }

    void syncMidiControls() {
        midi_service_revision_ = midi_service_.revision();
        midi_input_.clear(juce::dontSendNotification);
        midi_input_.addItem(
            juce::String::fromUTF8("MIDI入力なし"), 1);
        const auto& devices = midi_service_.devices();
        for (int index = 0; index < devices.size(); ++index) {
            const auto& device = devices.getReference(index);
            midi_input_.addItem(device.name, index + 2);
        }
        syncing_midi_controls_ = true;
        midi_input_.setSelectedId(
            midi_service_.selectedComboId(),
            juce::dontSendNotification);
        syncing_midi_controls_ = false;
        midi_status_.setText(
            midi_service_.statusText()
                + " | "
                + juce::String::fromUTF8(
                    audio_service_.engine()
                        .soundOutputStatus()
                        .c_str()),
            juce::dontSendNotification);
    }

    [[nodiscard]] bool windowIsActive() const {
        const auto* top_level = getTopLevelComponent();
        const auto* peer =
            top_level ? top_level->getPeer() : nullptr;
        return peer != nullptr && peer->isFocused();
    }

    void handleNoteOn(
        juce::MidiKeyboardState*,
        int channel,
        int note,
        float) override {
        if (preview_update_) {
            return;
        }
        const HeldNote incoming{channel, note};
        std::erase_if(
            held_notes_,
            [&](const auto& item) {
                return item.channel == incoming.channel
                    && item.note == incoming.note;
            });
        if (!polyphonic_ && !held_notes_.empty() && note_off_) {
            note_off_(
                static_cast<std::uint8_t>(
                    held_notes_.back().note));
        }
        held_notes_.push_back(incoming);
        saveSharedAuditionNote(note);
        if (note_on_) {
            note_on_(static_cast<std::uint8_t>(note));
        }
    }

    void handleNoteOff(
        juce::MidiKeyboardState*,
        int channel,
        int note,
        float) override {
        if (preview_update_) {
            return;
        }
        const bool was_active = polyphonic_
            || (
            !held_notes_.empty()
            && held_notes_.back().channel == channel
            && held_notes_.back().note == note);
        std::erase_if(
            held_notes_,
            [&](const auto& item) {
                return item.channel == channel
                    && item.note == note;
            });
        if (!was_active) {
            return;
        }
        if (note_off_) {
            note_off_(static_cast<std::uint8_t>(note));
        }
        if (!polyphonic_ && !held_notes_.empty() && note_on_) {
            note_on_(
                static_cast<std::uint8_t>(
                    held_notes_.back().note));
        }
    }

    void setPcNote(std::optional<std::uint8_t> note) {
        if (pc_note_ == note) {
            return;
        }
        if (pc_note_) {
            keyboard_state_.noteOff(
                kPcMidiChannel, *pc_note_, 1.0F);
        }
        pc_note_ = note;
        if (pc_note_) {
            keyboard_state_.noteOn(
                kPcMidiChannel, *pc_note_, 1.0F);
        }
    }

    void releasePcNote() {
        if (polyphonic_) {
            keyboard_state_.allNotesOff(kPcMidiChannel);
        }
        setPcNote(std::nullopt);
        active_pc_scan_.reset();
    }

    void resetPcKeyEdges() {
        for (std::size_t index = 0;
             index < kPcPerformanceScanCodes.size();
             ++index) {
            pc_key_down_[index] = physicalKeyIsDown(
                kPcPerformanceScanCodes[index]);
        }
        octave_down_key_ =
            physicalKeyIsDown(kPcOctaveDownScanCode);
        octave_up_key_ =
            physicalKeyIsDown(kPcOctaveUpScanCode);
    }

    void pollPcKeyboard() {
        const auto modifiers =
            juce::ModifierKeys::getCurrentModifiersRealtime();
        if (modifiers.isCtrlDown()
            || modifiers.isCommandDown()
            || (suppress_pc_input_ && suppress_pc_input_())) {
            releasePcNote();
            resetPcKeyEdges();
            return;
        }
        const bool down =
            physicalKeyIsDown(kPcOctaveDownScanCode);
        const bool up =
            physicalKeyIsDown(kPcOctaveUpScanCode);
        if (down && !octave_down_key_) {
            changePcOctave(-1);
        }
        if (up && !octave_up_key_) {
            changePcOctave(1);
        }
        octave_down_key_ = down;
        octave_up_key_ = up;

        if (polyphonic_) {
            for (std::size_t index = 0;
                 index < kPcPerformanceScanCodes.size();
                 ++index) {
                const auto scan_code = kPcPerformanceScanCodes[index];
                const bool key_down = physicalKeyIsDown(scan_code);
                if (key_down == pc_key_down_[index]) {
                    continue;
                }
                pc_key_down_[index] = key_down;
                const auto note =
                    mgstc::engine::pcKeyboardMidiNoteFromScanCode(
                        scan_code, pc_octave_);
                if (!note) {
                    continue;
                }
                if (key_down) {
                    keyboard_state_.noteOn(
                        kPcMidiChannel, *note, 1.0F);
                } else {
                    keyboard_state_.noteOff(
                        kPcMidiChannel, *note, 1.0F);
                }
            }
            return;
        }

        std::optional<std::uint16_t> newly_pressed;
        for (std::size_t index = 0;
             index < kPcPerformanceScanCodes.size();
             ++index) {
            const auto scan_code =
                kPcPerformanceScanCodes[index];
            const bool key_down = physicalKeyIsDown(scan_code);
            if (key_down && !pc_key_down_[index]) {
                newly_pressed = scan_code;
            }
            pc_key_down_[index] = key_down;
        }
        if (newly_pressed) {
            const auto note =
                mgstc::engine::pcKeyboardMidiNoteFromScanCode(
                    *newly_pressed, pc_octave_);
            if (note) {
                active_pc_scan_ = newly_pressed;
                setPcNote(note);
            }
            return;
        }
        if (!active_pc_scan_
            || physicalKeyIsDown(*active_pc_scan_)) {
            return;
        }
        active_pc_scan_.reset();
        for (std::size_t index =
                 kPcPerformanceScanCodes.size();
             index-- > 0;) {
            if (!pc_key_down_[index]) {
                continue;
            }
            const auto scan_code =
                kPcPerformanceScanCodes[index];
            const auto note =
                mgstc::engine::pcKeyboardMidiNoteFromScanCode(
                    scan_code, pc_octave_);
            if (note) {
                active_pc_scan_ = scan_code;
                setPcNote(note);
                return;
            }
        }
        setPcNote(std::nullopt);
    }

    void changePcOctave(int delta) {
        releasePcNote();
        pc_octave_ = juce::jlimit(1, 6, pc_octave_ + delta);
        savePcOctave(pc_octave_);
        updateOctaveLabel();
    }

    void updateOctaveLabel() {
        octave_label_.setText(
            juce::String::fromUTF8("PC Oct ")
                + juce::String(pc_octave_)
                + juce::String::fromUTF8("  Z=C"),
            juce::dontSendNotification);
    }

    void timerCallback() override {
        if (midi_service_revision_ != midi_service_.revision()) {
            syncMidiControls();
        }
        if (!windowIsActive()) {
            allNotesOff();
            resetPcKeyEdges();
            was_window_active_ = false;
            return;
        }
        if (!was_window_active_) {
            midi_service_.clearPendingMessages();
            was_window_active_ = true;
        }
        for (const auto& message :
             midi_service_.takePendingMessages()) {
            keyboard_state_.processNextMidiEvent(message);
        }
        pollPcKeyboard();
    }

    SharedMidiInputService& midi_service_;
    SharedAudioService& audio_service_;
    juce::MidiKeyboardState keyboard_state_;
    MgscMidiKeyboardComponent keyboard_;
    juce::ComboBox midi_input_;
    juce::TextButton octave_down_;
    juce::TextButton octave_up_;
    juce::Label octave_label_;
    juce::Label midi_status_;
    juce::TextButton mode_;
    std::vector<HeldNote> held_notes_;
    std::array<bool, kPcPerformanceScanCodes.size()>
        pc_key_down_{};
    std::optional<std::uint16_t> active_pc_scan_;
    std::optional<std::uint8_t> pc_note_;
    std::optional<std::uint8_t> preview_note_;
    NoteCallback note_on_;
    NoteCallback note_off_;
    ModeCallback mode_changed_;
    SuppressCallback suppress_pc_input_;
    int pc_octave_{4};
    std::uint64_t midi_service_revision_{};
    bool syncing_midi_controls_{};
    bool was_window_active_{};
    bool polyphonic_{true};
    bool preview_update_{};
    bool octave_down_key_{};
    bool octave_up_key_{};
};

struct CompositeEnvelopePrograms {
    std::vector<std::uint8_t> volume;
    std::vector<std::uint8_t> pitch;
    std::vector<std::uint8_t> timbre;
};

enum class CompositeEnvelopeLane : std::uint8_t {
    Volume,
    Pitch,
    Timbre,
};

[[nodiscard]] std::vector<std::uint8_t> compileCompositeEnvelopeLane(
    const mgstc::engine::CompositeLayer& layer,
    CompositeEnvelopeLane lane) {
    struct TimedEvent {
        std::uint32_t count{};
        mgstc::engine::EnvelopeEventKind kind{};
        std::int32_t value{};
        std::int32_t secondary{};
    };
    std::vector<TimedEvent> events;
    const auto collect = [&events](
        const std::vector<mgstc::engine::EnvelopeEvent>& source,
        mgstc::engine::EnvelopeEventKind expected) {
        for (const auto& event : source) {
            if (event.kind == expected) {
                events.push_back({
                    event.count, event.kind, event.value, event.secondary});
            }
        }
    };
    const mgstc::engine::EnvelopeTimeline* timeline{};
    switch (lane) {
    case CompositeEnvelopeLane::Volume:
        timeline = &layer.envelope_timeline;
        collect(
            layer.volume_envelope.events,
            mgstc::engine::EnvelopeEventKind::Volume);
        if (std::none_of(
                events.begin(), events.end(),
                [](const TimedEvent& event) {
                    return event.count == 0;
                })) {
            events.push_back({
                0,
                mgstc::engine::EnvelopeEventKind::Volume,
                layer.volume,
                0});
        }
        break;
    case CompositeEnvelopeLane::Pitch:
        timeline = &layer.envelope_timeline;
        collect(
            layer.pitch_envelope.events,
            mgstc::engine::EnvelopeEventKind::Pitch);
        break;
    case CompositeEnvelopeLane::Timbre:
        timeline = &layer.envelope_timeline;
        if (layer.source == mgstc::engine::TimbreSource::Scc) {
            events.push_back({
                0,
                mgstc::engine::EnvelopeEventKind::Timbre,
                0,
                0});
        } else if (layer.source
                   == mgstc::engine::TimbreSource::Opll) {
            events.push_back({
                0,
                mgstc::engine::EnvelopeEventKind::Timbre,
                16,
                0});
        }
        collect(
            layer.timbre_automation,
            mgstc::engine::EnvelopeEventKind::Timbre);
        collect(
            layer.timbre_automation,
            mgstc::engine::EnvelopeEventKind::RegisterWrite);
        break;
    }

    const bool valid_loop = timeline->loop_start_count
        && timeline->loop_end_count
        && *timeline->loop_start_count < *timeline->loop_end_count
        && *timeline->loop_end_count <= timeline->length_counts;
    if (valid_loop) {
        events.push_back({
            *timeline->loop_start_count,
            mgstc::engine::EnvelopeEventKind::LoopStart,
            0,
            0});
        events.push_back({
            *timeline->loop_end_count,
            mgstc::engine::EnvelopeEventKind::LoopEnd,
            0,
            0});
    }
    events.erase(
        std::remove_if(
            events.begin(), events.end(),
            [timeline](const TimedEvent& event) {
                return event.count > timeline->length_counts;
            }),
        events.end());

    const auto priority = [](mgstc::engine::EnvelopeEventKind kind) {
        if (kind == mgstc::engine::EnvelopeEventKind::LoopEnd) {
            return 0;
        }
        if (kind == mgstc::engine::EnvelopeEventKind::LoopStart) {
            return 1;
        }
        return 2;
    };
    std::stable_sort(
        events.begin(), events.end(),
        [&priority](const TimedEvent& left, const TimedEvent& right) {
            if (left.count != right.count) {
                return left.count < right.count;
            }
            return priority(left.kind) < priority(right.kind);
        });

    std::vector<std::uint8_t> bytecode;
    bytecode.reserve(events.size() * 3 + 8);
    std::uint32_t cursor{};
    auto volume = static_cast<std::uint8_t>(
        juce::jlimit(0, 15, static_cast<int>(layer.volume)));
    const auto append_wait = [&bytecode, &volume, lane](
                                 std::uint32_t count) {
        while (count != 0) {
            const auto chunk = static_cast<std::uint8_t>(
                juce::jmin<std::uint32_t>(255, count));
            const auto held_volume = lane == CompositeEnvelopeLane::Volume
                ? volume
                : std::uint8_t{0};
            bytecode.push_back(static_cast<std::uint8_t>(
                0xE0 | held_volume));
            bytecode.push_back(chunk);
            count -= chunk;
        }
    };
    for (const auto& event : events) {
        if (event.count > cursor) {
            append_wait(event.count - cursor);
            cursor = event.count;
        }
        switch (event.kind) {
        case mgstc::engine::EnvelopeEventKind::Volume:
            volume = static_cast<std::uint8_t>(
                juce::jlimit(0, 15, event.value));
            bytecode.push_back(volume);
            cursor = juce::jmax(cursor, event.count + 1);
            break;
        case mgstc::engine::EnvelopeEventKind::Pitch:
            bytecode.insert(
                bytecode.end(),
                {0x12, static_cast<std::uint8_t>(
                    juce::jlimit(-128, 127, event.value))});
            break;
        case mgstc::engine::EnvelopeEventKind::Timbre:
            bytecode.insert(
                bytecode.end(),
                {0x10, static_cast<std::uint8_t>(
                    juce::jlimit(0, 31, event.value))});
            break;
        case mgstc::engine::EnvelopeEventKind::RegisterWrite:
            bytecode.insert(
                bytecode.end(),
                {0x11,
                 static_cast<std::uint8_t>(
                     juce::jlimit(0, 255, event.value)),
                 static_cast<std::uint8_t>(
                     juce::jlimit(0, 255, event.secondary))});
            break;
        case mgstc::engine::EnvelopeEventKind::LoopStart:
            bytecode.push_back(0x40);
            break;
        case mgstc::engine::EnvelopeEventKind::LoopEnd:
            bytecode.push_back(0x60);
            break;
        default:
            break;
        }
    }
    if (cursor < timeline->length_counts) {
        append_wait(timeline->length_counts - cursor);
    }
    return bytecode;
}

[[nodiscard]] CompositeEnvelopePrograms compileCompositeEnvelopes(
    const mgstc::engine::CompositeLayer& layer) {
    return {
        .volume = compileCompositeEnvelopeLane(
            layer, CompositeEnvelopeLane::Volume),
        .pitch = compileCompositeEnvelopeLane(
            layer, CompositeEnvelopeLane::Pitch),
        .timbre = compileCompositeEnvelopeLane(
            layer, CompositeEnvelopeLane::Timbre),
    };
}

class CompositeTimeline final
    : public juce::Component,
      private juce::ScrollBar::Listener {
public:
    using EditCallback = std::function<void(
        const mgstc::engine::CompositeTimbre&, bool)>;

    CompositeTimeline()
        : horizontal_scroll_(false), vertical_scroll_(true) {
        parameter_.addItem(juce::String::fromUTF8("音量"), 1);
        parameter_.addItem(juce::String::fromUTF8("音程"), 2);
        parameter_.addItem(juce::String::fromUTF8("音色番号"), 3);
        parameter_.setSelectedId(1, juce::dontSendNotification);
        parameter_.setTooltip(
            juce::String::fromUTF8("時間軸へ描画するパラメーター"));
        parameter_.onChange = [this] {
            syncPointEditors();
            syncInspector();
            repaint();
        };
        addAndMakeVisible(parameter_);

        psg_add_.setButtonText(juce::String::fromUTF8("PSG追加"));
        scc_add_.setButtonText(juce::String::fromUTF8("SCC追加"));
        opll_add_.setButtonText(juce::String::fromUTF8("OPLL追加"));
        psg_add_.setTooltip(juce::String::fromUTF8(
            "未使用のPSGチャンネルを追加します（最大3ch）"));
        scc_add_.setTooltip(juce::String::fromUTF8(
            "未使用のSCCチャンネルを追加します（最大5ch）"));
        opll_add_.setTooltip(juce::String::fromUTF8(
            "未使用のOPLLチャンネルを追加します（最大9ch）"));
        psg_add_.onClick = [this] {
            addLayer(mgstc::engine::TimbreSource::Psg);
        };
        scc_add_.onClick = [this] {
            addLayer(mgstc::engine::TimbreSource::Scc);
        };
        opll_add_.onClick = [this] {
            addLayer(mgstc::engine::TimbreSource::Opll);
        };
        addAndMakeVisible(psg_add_);
        addAndMakeVisible(scc_add_);
        addAndMakeVisible(opll_add_);

        remove_layer_.setButtonText(
            juce::String::fromUTF8("選択ch削除"));
        remove_layer_.setTooltip(juce::String::fromUTF8(
            "選択中のチャンネルとその独立エンベロープを削除します"));
        remove_layer_.onClick = [this] {
            requestRemoveSelectedLayer();
        };
        addAndMakeVisible(remove_layer_);

        edit_mode_.addItem(juce::String::fromUTF8("波形描画"), 1);
        edit_mode_.addItem(juce::String::fromUTF8("終端指定"), 2);
        edit_mode_.addItem(juce::String::fromUTF8("ループ開始"), 3);
        edit_mode_.addItem(juce::String::fromUTF8("ループ終了"), 4);
        edit_mode_.setSelectedId(1, juce::dontSendNotification);
        edit_mode_.setTooltip(juce::String::fromUTF8(
            "選択中チャンネルの選択中エンベロープを編集します"));
        addAndMakeVisible(edit_mode_);

        value_.setInputRestrictions(4, "-0123456789");
        value_.setText("15", false);
        value_.setTooltip(
            juce::String::fromUTF8("選択パラメーターの値"));
        addAndMakeVisible(value_);
        position_.setText("ct 0", juce::dontSendNotification);
        position_.setJustificationType(juce::Justification::centredRight);
        position_.setTooltip(juce::String::fromUTF8(
            "最後にクリックしたカウント（数値入力では変更しません）"));
        value_label_.setText(
            juce::String::fromUTF8("値"), juce::dontSendNotification);
        value_label_.setJustificationType(juce::Justification::centredRight);
        addAndMakeVisible(position_);
        addAndMakeVisible(value_label_);
        apply_.setButtonText(juce::String::fromUTF8("設定"));
        apply_.setTooltip(
            juce::String::fromUTF8(
                "最後にクリックした位置へ入力値を設定します"));
        apply_.onClick = [this] {
            if (selectedEditMode() == EditMode::Draw) {
                editPoint(selected_count_, value_.getText().getIntValue(), true);
            }
        };
        addAndMakeVisible(apply_);

        inspector_selection_.setJustificationType(
            juce::Justification::centredLeft);
        inspector_selection_.setTooltip(juce::String::fromUTF8(
            "選択中のチャンネルとエンベロープ種類"));
        addAndMakeVisible(inspector_selection_);

        configureInspectorEditor(
            length_editor_, juce::String::fromUTF8("終端カウント"));
        configureInspectorEditor(
            loop_start_editor_, juce::String::fromUTF8("ループ開始カウント"));
        configureInspectorEditor(
            loop_end_editor_, juce::String::fromUTF8("ループ終了カウント"));
        configureInspectorLabel(length_label_, "END");
        configureInspectorLabel(loop_start_label_, "L>");
        configureInspectorLabel(loop_end_label_, "<L");

        inspector_apply_.setButtonText(juce::String::fromUTF8("反映"));
        inspector_apply_.setTooltip(juce::String::fromUTF8(
            "表示中の終端とループ位置を選択エンベロープへ反映します"));
        inspector_apply_.onClick = [this] { applyInspector(); };
        addAndMakeVisible(inspector_apply_);

        clear_loop_.setButtonText(juce::String::fromUTF8("ループ解除"));
        clear_loop_.setTooltip(juce::String::fromUTF8(
            "選択エンベロープのループ開始と終了を両方解除します"));
        clear_loop_.onClick = [this] { clearSelectedLoop(); };
        addAndMakeVisible(clear_loop_);

        horizontal_scroll_.setRangeLimits(
            0.0, static_cast<double>(maximumCount()) + 1.0,
            juce::dontSendNotification);
        horizontal_scroll_.setCurrentRange(
            0.0, kDefaultVisibleCounts, juce::dontSendNotification);
        horizontal_scroll_.setSingleStepSize(1.0);
        horizontal_scroll_.setAutoHide(false);
        horizontal_scroll_.addListener(this);
        vertical_scroll_.setSingleStepSize(1.0);
        vertical_scroll_.setAutoHide(false);
        vertical_scroll_.addListener(this);
        addAndMakeVisible(horizontal_scroll_);
        addAndMakeVisible(vertical_scroll_);
        setMouseCursor(juce::MouseCursor::CrosshairCursor);
    }

    ~CompositeTimeline() override {
        horizontal_scroll_.removeListener(this);
        vertical_scroll_.removeListener(this);
    }

    void setEditCallback(EditCallback callback) {
        edit_callback_ = std::move(callback);
    }

    void setTimbre(const mgstc::engine::CompositeTimbre& timbre) {
        timbre_ = timbre;
        selected_layer_ = timbre_.layers.empty()
            ? -1
            : juce::jlimit(
                  0,
                  static_cast<int>(timbre_.layers.size()) - 1,
                  selected_layer_);
        updateScrollRanges();
        updateAddButtons();
        syncPointEditors();
        syncInspector();
        repaint();
    }

    void appendScopeFrame(
        const mgstc::engine::OpllScopeFrame& frame,
        std::uint8_t midi_note) {
        audition_note_ = midi_note;
        for (std::size_t index = 0;
             index < mgstc::engine::OpllScopeFrame::kSampleCount;
             ++index) {
            scope_history_[0][scope_write_position_] = frame.psg_samples[index];
            scope_history_[1][scope_write_position_] = frame.scc_samples[index];
            scope_history_[2][scope_write_position_] = frame.samples[index];
            scope_history_[3][scope_write_position_] = frame.mixed_samples[index];
            scope_write_position_ =
                (scope_write_position_ + 1) % kScopeHistorySize;
            scope_size_ = juce::jmin(kScopeHistorySize, scope_size_ + 1);
        }
        repaint();
    }

    void resized() override {
        auto area = getLocalBounds().reduced(10);
        auto add_row = area.removeFromTop(24);
        opll_add_.setBounds(add_row.removeFromRight(76));
        add_row.removeFromRight(5);
        scc_add_.setBounds(add_row.removeFromRight(70));
        add_row.removeFromRight(5);
        psg_add_.setBounds(add_row.removeFromRight(70));
        add_row.removeFromRight(8);
        remove_layer_.setBounds(add_row.removeFromRight(92));
        area.removeFromTop(5);
        auto header = area.removeFromTop(24);
        apply_.setBounds(header.removeFromRight(48));
        header.removeFromRight(4);
        value_.setBounds(header.removeFromRight(48));
        value_label_.setBounds(header.removeFromRight(22));
        header.removeFromRight(4);
        position_.setBounds(header.removeFromRight(66));
        header.removeFromRight(6);
        parameter_.setBounds(header.removeFromRight(112));
        header.removeFromRight(6);
        edit_mode_.setBounds(header.removeFromRight(116));

        area.removeFromTop(5);
        auto inspector = area.removeFromTop(24);
        inspector_selection_.setBounds(inspector.removeFromLeft(170));
        inspector.removeFromLeft(6);
        length_label_.setBounds(inspector.removeFromLeft(34));
        length_editor_.setBounds(inspector.removeFromLeft(62));
        inspector.removeFromLeft(6);
        loop_start_label_.setBounds(inspector.removeFromLeft(24));
        loop_start_editor_.setBounds(inspector.removeFromLeft(62));
        inspector.removeFromLeft(6);
        loop_end_label_.setBounds(inspector.removeFromLeft(24));
        loop_end_editor_.setBounds(inspector.removeFromLeft(62));
        inspector.removeFromLeft(6);
        clear_loop_.setBounds(inspector.removeFromRight(88));
        inspector.removeFromRight(5);
        inspector_apply_.setBounds(inspector.removeFromRight(52));

        auto graph = graphArea();
        vertical_scroll_.setBounds(
            graph.getRight(), graph.getY(), kScrollBarSize, graph.getHeight());
        horizontal_scroll_.setBounds(
            graph.getX(), graph.getBottom(), graph.getWidth(), kScrollBarSize);
        updateScrollRanges();
    }

    void mouseDown(const juce::MouseEvent& event) override {
        for (std::size_t index = 0; index < graph_bounds_.size(); ++index) {
            if (graph_bounds_[index].contains(event.getPosition())) {
                selected_layer_ = static_cast<int>(index);
                selected_count_ = countAtX(
                    graph_bounds_[index], event.getPosition().x);
                position_.setText(
                    "ct " + juce::String(selected_count_),
                    juce::dontSendNotification);
                syncPointEditors();
                syncInspector();
                if (selectedEditMode() == EditMode::Draw) {
                    drawing_ = true;
                    editFromMouse(event.getPosition(), false);
                } else {
                    setTimelineMarker(selected_count_, true);
                }
                return;
            }
        }
    }

    void mouseDrag(const juce::MouseEvent& event) override {
        if (drawing_) {
            editFromMouse(event.getPosition(), false);
        }
    }

    void mouseUp(const juce::MouseEvent& event) override {
        if (!drawing_) {
            return;
        }
        editFromMouse(event.getPosition(), true);
        drawing_ = false;
    }

    void paint(juce::Graphics& graphics) override {
        graphics.fillAll(juce::Colour(0xFF182028));
        auto area = getLocalBounds().reduced(10);
        auto header = area.removeFromTop(24);
        header.removeFromRight(236);
        graphics.setColour(juce::Colour(0xFFE6EDF3));
        graphics.setFont(juce::FontOptions(13.0F, juce::Font::bold));
        graphics.drawText(
            juce::String::fromUTF8("共通時間軸  クリック／ドラッグで編集"),
            header,
            juce::Justification::centredLeft);
        area = graphArea();

        const std::size_t lane_count = timbre_.layers.size() + 1;
        const int first_lane = juce::jlimit(
            0, juce::jmax(0, static_cast<int>(lane_count) - 1),
            juce::roundToInt(vertical_scroll_.getCurrentRangeStart()));
        const int visible_lanes = juce::jmax(1, area.getHeight() / kLaneHeight);
        const int last_lane = juce::jmin(
            static_cast<int>(lane_count), first_lane + visible_lanes + 1);
        const auto numbers = mgstc::engine::resolveTimbreNumbers(timbre_);
        graph_bounds_.assign(timbre_.layers.size(), {});
        for (int lane_index = first_lane; lane_index < last_lane; ++lane_index) {
            auto lane = juce::Rectangle<int>(
                area.getX(),
                area.getY() + (lane_index - first_lane) * kLaneHeight,
                area.getWidth(), kLaneHeight).reduced(0, 3);
            if (lane.getY() >= area.getBottom()) {
                break;
            }
            lane.setBottom(juce::jmin(lane.getBottom(), area.getBottom()));
            const bool mixed_lane = lane_index == static_cast<int>(timbre_.layers.size());
            const std::size_t index = static_cast<std::size_t>(lane_index);
            const auto colour = mixed_lane
                ? juce::Colour(0xFF35C4ED)
                : sourceColour(timbre_.layers[index].source);
            graphics.setColour(juce::Colour(0xFF26313B));
            graphics.fillRoundedRectangle(lane.toFloat(), 5.0F);
            graphics.setColour(
                !mixed_lane && static_cast<int>(index) == selected_layer_
                    ? colour
                    : colour.withAlpha(0.72F));
            graphics.drawRoundedRectangle(
                lane.toFloat(), 5.0F,
                !mixed_lane && static_cast<int>(index) == selected_layer_
                    ? 2.0F : 1.0F);

            auto label = lane.removeFromLeft(150).reduced(7, 0);
            graphics.setColour(colour);
            graphics.setFont(juce::FontOptions(12.0F, juce::Font::bold));
            juce::String label_text = mixed_lane
                ? juce::String::fromUTF8("MIX / 合成出力")
                : juce::String::fromUTF8(timbre_.layers[index].name.c_str());
            if (!mixed_lane && timbre_.layers[index].base_timbre) {
                const auto& layer = timbre_.layers[index];
                label_text += "\n"
                    + juce::String::fromUTF8(layer.base_timbre->name.c_str())
                    + " r" + juce::String(static_cast<int>(layer.base_timbre->revision));
                const auto assigned = std::find_if(
                    numbers.assignments.begin(), numbers.assignments.end(),
                    [index](const auto& item) { return item.layer_index == index; });
                if (assigned != numbers.assignments.end()) {
                    label_text += "  #" + juce::String(static_cast<int>(assigned->number))
                        + (assigned->manually_assigned ? " M" : " A");
                }
            }
            graphics.drawFittedText(
                label_text, label, juce::Justification::centredLeft, 2);

            lane.reduce(8, 7);
            if (!mixed_lane) {
                graph_bounds_[index] = lane;
            }
            const auto visible = visibleCountRange();
            const int grid_begin =
                (static_cast<int>(visible.getStart()) / 30) * 30;
            for (int timeline_count = grid_begin;
                 timeline_count <= static_cast<int>(visible.getEnd());
                 timeline_count += 30) {
                const int x = xForCount(lane, timeline_count);
                graphics.setColour(juce::Colour(0xFF40505E));
                graphics.drawVerticalLine(
                    x, static_cast<float>(lane.getY()),
                    static_cast<float>(lane.getBottom()));
            }
            if (!mixed_lane) {
                const auto& layer = timbre_.layers[index];
                const int start_x = xForCount(
                    lane, static_cast<int>(layer.start_delay_counts));
                graphics.setColour(colour.withAlpha(
                    mgstc::engine::layerIsAudible(timbre_, index)
                        ? 0.10F : 0.04F));
                graphics.fillRoundedRectangle(
                    static_cast<float>(start_x),
                    static_cast<float>(lane.getY() + 5),
                    static_cast<float>(juce::jmax(4, lane.getRight() - start_x)),
                    static_cast<float>(lane.getHeight() - 10), 4.0F);
            }
            drawScope(graphics, lane, index, colour.withAlpha(0.48F));
            if (!mixed_lane) {
                drawTimelineMarkers(graphics, lane, index, colour);
                drawAutomation(graphics, lane, index, colour);
            }
        }
    }

private:
    enum class Parameter : int {
        Volume = 1,
        Pitch = 2,
        Timbre = 3,
    };

    enum class EditMode : int {
        Draw = 1,
        End = 2,
        LoopStart = 3,
        LoopEnd = 4,
    };

    static constexpr int kDefaultVisibleCounts = 120;
    static constexpr int kLaneHeight = 104;
    static constexpr int kScrollBarSize = 15;

    [[nodiscard]] static constexpr int maximumCount() noexcept {
        return static_cast<int>(
            mgstc::engine::EnvelopeTimeline::kMaximumLengthCounts);
    }

    [[nodiscard]] EditMode selectedEditMode() const noexcept {
        return static_cast<EditMode>(edit_mode_.getSelectedId());
    }

    [[nodiscard]] Parameter selectedParameter() const noexcept {
        return static_cast<Parameter>(parameter_.getSelectedId());
    }

    [[nodiscard]] juce::String selectedParameterName() const {
        switch (selectedParameter()) {
        case Parameter::Volume:
            return juce::String::fromUTF8("音量");
        case Parameter::Pitch:
            return juce::String::fromUTF8("音程");
        case Parameter::Timbre:
            return juce::String::fromUTF8("音色番号");
        }
        return {};
    }

    void configureInspectorEditor(
        juce::TextEditor& editor,
        const juce::String& tooltip) {
        editor.setInputRestrictions(5, "0123456789");
        editor.setJustification(juce::Justification::centred);
        editor.setTextToShowWhenEmpty(
            juce::String::fromUTF8("なし"), juce::Colour(0xFF8896A3));
        editor.setTooltip(tooltip);
        editor.onReturnKey = [this] { applyInspector(); };
        addAndMakeVisible(editor);
    }

    void configureInspectorLabel(
        juce::Label& label,
        const juce::String& text) {
        label.setText(text, juce::dontSendNotification);
        label.setJustificationType(juce::Justification::centredRight);
        addAndMakeVisible(label);
    }

    [[nodiscard]] std::optional<std::uint32_t> inspectorCount(
        const juce::TextEditor& editor,
        std::uint32_t length) const {
        const auto text = editor.getText().trim();
        if (text.isEmpty()) {
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(juce::jlimit(
            0, static_cast<int>(length), text.getIntValue()));
    }

    void syncInspector() {
        const bool has_selection = selected_layer_ >= 0
            && selected_layer_ < static_cast<int>(timbre_.layers.size());
        length_editor_.setEnabled(has_selection);
        loop_start_editor_.setEnabled(has_selection);
        loop_end_editor_.setEnabled(has_selection);
        inspector_apply_.setEnabled(has_selection);
        clear_loop_.setEnabled(has_selection);
        if (!has_selection) {
            inspector_selection_.setText(
                juce::String::fromUTF8("チャンネル未選択"),
                juce::dontSendNotification);
            length_editor_.setText({}, false);
            loop_start_editor_.setText({}, false);
            loop_end_editor_.setText({}, false);
            return;
        }

        const auto& layer =
            timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        const auto& timeline = layer.envelope_timeline;
        inspector_selection_.setText(
            juce::String::fromUTF8(layer.name.c_str())
                + juce::String::fromUTF8(" / ソフトウェアEG"),
            juce::dontSendNotification);
        length_editor_.setText(
            juce::String(static_cast<int>(timeline.length_counts)), false);
        loop_start_editor_.setText(
            timeline.loop_start_count
                ? juce::String(static_cast<int>(*timeline.loop_start_count))
                : juce::String{},
            false);
        loop_end_editor_.setText(
            timeline.loop_end_count
                ? juce::String(static_cast<int>(*timeline.loop_end_count))
                : juce::String{},
            false);
    }

    void applyInspector() {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        auto& timeline = selectedTimeline(
            timbre_.layers[static_cast<std::size_t>(selected_layer_)]);
        const auto length = static_cast<std::uint32_t>(juce::jlimit(
            1, maximumCount(), length_editor_.getText().getIntValue()));
        auto loop_start = inspectorCount(
            loop_start_editor_, length);
        auto loop_end = inspectorCount(
            loop_end_editor_, length);
        mgstc::engine::setEnvelopeTimelineRange(
            timeline, length, loop_start, loop_end);
        syncInspector();
        repaint();
        if (edit_callback_) {
            edit_callback_(timbre_, true);
        }
    }

    void clearSelectedLoop() {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        auto& timeline = selectedTimeline(
            timbre_.layers[static_cast<std::size_t>(selected_layer_)]);
        timeline.loop_start_count.reset();
        timeline.loop_end_count.reset();
        syncInspector();
        repaint();
        if (edit_callback_) {
            edit_callback_(timbre_, true);
        }
    }

    [[nodiscard]] std::pair<int, int> valueRange() const noexcept {
        switch (selectedParameter()) {
        case Parameter::Volume:
            return {0, 15};
        case Parameter::Pitch:
            return {-24, 24};
        case Parameter::Timbre:
            return {0, 31};
        }
        return {0, 15};
    }

    [[nodiscard]] const std::vector<mgstc::engine::EnvelopeEvent>&
    selectedEvents(const mgstc::engine::CompositeLayer& layer) const {
        if (selectedParameter() == Parameter::Volume) {
            return layer.volume_envelope.events;
        }
        if (selectedParameter() == Parameter::Pitch) {
            return layer.pitch_envelope.events;
        }
        return layer.timbre_automation;
    }

    [[nodiscard]] mgstc::engine::EnvelopeEventKind selectedEventKind() const {
        if (selectedParameter() == Parameter::Volume) {
            return mgstc::engine::EnvelopeEventKind::Volume;
        }
        if (selectedParameter() == Parameter::Pitch) {
            return mgstc::engine::EnvelopeEventKind::Pitch;
        }
        return mgstc::engine::EnvelopeEventKind::Timbre;
    }

    [[nodiscard]] const mgstc::engine::EnvelopeTimeline& selectedTimeline(
        const mgstc::engine::CompositeLayer& layer) const {
        return layer.envelope_timeline;
    }

    [[nodiscard]] mgstc::engine::EnvelopeTimeline& selectedTimeline(
        mgstc::engine::CompositeLayer& layer) {
        return const_cast<mgstc::engine::EnvelopeTimeline&>(
            std::as_const(*this).selectedTimeline(layer));
    }

    void updateAddButtons() {
        psg_add_.setEnabled(
            mgstc::engine::firstAvailableChannel(
                timbre_, mgstc::engine::TimbreSource::Psg).has_value());
        scc_add_.setEnabled(
            mgstc::engine::firstAvailableChannel(
                timbre_, mgstc::engine::TimbreSource::Scc).has_value());
        opll_add_.setEnabled(
            mgstc::engine::firstAvailableChannel(
                timbre_, mgstc::engine::TimbreSource::Opll).has_value());
        remove_layer_.setEnabled(
            selected_layer_ >= 0
            && selected_layer_ < static_cast<int>(timbre_.layers.size()));
    }

    void addLayer(mgstc::engine::TimbreSource source) {
        const auto free_channel = mgstc::engine::firstAvailableChannel(
            timbre_, source);
        if (!free_channel) {
            return;
        }
        const auto defaults = mgstc::engine::defaultCompositeTimbre();
        const auto template_layer = std::find_if(
            defaults.layers.begin(), defaults.layers.end(),
            [source](const auto& layer) {
                return layer.source == source;
            });
        if (template_layer == defaults.layers.end()) {
            return;
        }
        auto layer = *template_layer;
        layer.channel = *free_channel;
        const char* source_name = source == mgstc::engine::TimbreSource::Psg
            ? "PSG"
            : source == mgstc::engine::TimbreSource::Scc ? "SCC" : "OPLL";
        layer.name = std::string(source_name) + " Ch."
            + std::to_string(static_cast<int>(layer.channel) + 1);
        timbre_.layers.push_back(std::move(layer));
        selected_layer_ = static_cast<int>(timbre_.layers.size()) - 1;
        updateScrollRanges();
        vertical_scroll_.setCurrentRangeStart(
            static_cast<double>(selected_layer_),
            juce::dontSendNotification);
        updateAddButtons();
        syncPointEditors();
        syncInspector();
        repaint();
        if (edit_callback_) {
            edit_callback_(timbre_, true);
        }
    }

    void requestRemoveSelectedLayer() {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        const auto layer_name = juce::String::fromUTF8(
            timbre_.layers[static_cast<std::size_t>(selected_layer_)]
                .name.c_str());
        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions()
                .withIconType(juce::MessageBoxIconType::WarningIcon)
                .withTitle(juce::String::fromUTF8("チャンネル削除"))
                .withMessage(
                    layer_name
                    + juce::String::fromUTF8(
                        " とそのエンベロープを削除しますか？"))
                .withButton(juce::String::fromUTF8("削除"))
                .withButton(juce::String::fromUTF8("キャンセル"))
                .withAssociatedComponent(this),
            [safe = juce::Component::SafePointer<CompositeTimeline>(this)](
                int result) {
                if (safe != nullptr && result == 1) {
                    safe->removeSelectedLayer();
                }
            });
    }

    void removeSelectedLayer() {
        if (selected_layer_ < 0
            || !mgstc::engine::removeCompositeLayer(
                timbre_, static_cast<std::size_t>(selected_layer_))) {
            return;
        }
        selected_layer_ = timbre_.layers.empty()
            ? -1
            : juce::jmin(
                  selected_layer_,
                  static_cast<int>(timbre_.layers.size()) - 1);
        selected_count_ = 0;
        position_.setText("ct 0", juce::dontSendNotification);
        updateScrollRanges();
        vertical_scroll_.setCurrentRangeStart(
            static_cast<double>(juce::jmax(0, selected_layer_)),
            juce::dontSendNotification);
        updateAddButtons();
        syncPointEditors();
        syncInspector();
        repaint();
        if (edit_callback_) {
            edit_callback_(timbre_, true);
        }
    }

    std::vector<mgstc::engine::EnvelopeEvent>& selectedEvents(
        mgstc::engine::CompositeLayer& layer) {
        if (selectedParameter() == Parameter::Volume) {
            layer.volume_envelope.kind = mgstc::engine::EnvelopeKind::Sequence;
            return layer.volume_envelope.events;
        }
        if (selectedParameter() == Parameter::Pitch) {
            layer.pitch_envelope.kind = mgstc::engine::EnvelopeKind::Sequence;
            return layer.pitch_envelope.events;
        }
        return layer.timbre_automation;
    }

    void editFromMouse(juce::Point<int> point, bool commit) {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(graph_bounds_.size())) {
            return;
        }
        const auto bounds = graph_bounds_[static_cast<std::size_t>(selected_layer_)];
        if (bounds.isEmpty()) {
            return;
        }
        const int timeline_count = countAtX(bounds, point.x);
        const auto [minimum, maximum] = valueRange();
        const int parameter_value = juce::jlimit(
            minimum, maximum,
            juce::roundToInt(
                static_cast<double>(bounds.getBottom() - point.y)
                * (maximum - minimum) / juce::jmax(1, bounds.getHeight())
                + minimum));
        editPoint(timeline_count, parameter_value, commit);
    }

    void editPoint(int timeline_count, int parameter_value, bool commit) {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        const auto [minimum, maximum] = valueRange();
        timeline_count = juce::jlimit(0, maximumCount(), timeline_count);
        parameter_value = juce::jlimit(minimum, maximum, parameter_value);
        auto& events = selectedEvents(
            timbre_.layers[static_cast<std::size_t>(selected_layer_)]);
        const auto kind = selectedEventKind();
        std::erase_if(events, [kind, timeline_count](const auto& event) {
            return event.kind == kind
                && event.count == static_cast<std::uint32_t>(timeline_count);
        });
        events.push_back({
            .kind = kind,
            .value = parameter_value,
            .count = static_cast<std::uint32_t>(timeline_count),
        });
        std::stable_sort(events.begin(), events.end(), [](const auto& left, const auto& right) {
            return left.count < right.count;
        });
        selected_count_ = timeline_count;
        position_.setText(
            "ct " + juce::String(timeline_count),
            juce::dontSendNotification);
        value_.setText(juce::String(parameter_value), false);
        repaint();
        if (edit_callback_) {
            edit_callback_(timbre_, commit);
        }
    }

    void syncPointEditors() {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        const auto& layer = timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        const auto& events = selectedEvents(layer);
        const auto kind = selectedEventKind();
        int current = selectedParameter() == Parameter::Volume
            ? layer.volume : 0;
        for (const auto& event : events) {
            if (event.kind == kind
                && event.count <= static_cast<std::uint32_t>(selected_count_)) {
                current = event.value;
            }
        }
        const auto [minimum, maximum] = valueRange();
        position_.setText(
            "ct " + juce::String(selected_count_),
            juce::dontSendNotification);
        value_.setText(juce::String(juce::jlimit(minimum, maximum, current)), false);
    }

    void setTimelineMarker(int timeline_count, bool commit) {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        auto& timeline = selectedTimeline(
            timbre_.layers[static_cast<std::size_t>(selected_layer_)]);
        const auto count = static_cast<std::uint32_t>(
            juce::jlimit(0, maximumCount(), timeline_count));
        switch (selectedEditMode()) {
        case EditMode::Draw:
            return;
        case EditMode::End:
            timeline.length_counts = juce::jmax<std::uint32_t>(1, count);
            if (timeline.loop_start_count
                && *timeline.loop_start_count > timeline.length_counts) {
                timeline.loop_start_count = timeline.length_counts;
            }
            if (timeline.loop_end_count
                && *timeline.loop_end_count > timeline.length_counts) {
                timeline.loop_end_count = timeline.length_counts;
            }
            break;
        case EditMode::LoopStart:
            timeline.loop_start_count = juce::jmin(count, timeline.length_counts);
            if (timeline.loop_end_count
                && *timeline.loop_end_count < *timeline.loop_start_count) {
                timeline.loop_end_count = timeline.loop_start_count;
            }
            break;
        case EditMode::LoopEnd:
            timeline.loop_end_count = juce::jmin(count, timeline.length_counts);
            if (timeline.loop_start_count
                && *timeline.loop_start_count > *timeline.loop_end_count) {
                timeline.loop_start_count = timeline.loop_end_count;
            }
            break;
        }
        syncInspector();
        repaint();
        if (edit_callback_) {
            edit_callback_(timbre_, commit);
        }
    }

    [[nodiscard]] juce::Rectangle<int> graphArea() const {
        auto area = getLocalBounds().reduced(10);
        area.removeFromTop(87);
        area.removeFromRight(kScrollBarSize);
        area.removeFromBottom(kScrollBarSize);
        return area;
    }

    [[nodiscard]] juce::Range<double> visibleCountRange() const {
        return horizontal_scroll_.getCurrentRange();
    }

    [[nodiscard]] int countAtX(
        juce::Rectangle<int> bounds, int x) const {
        const auto visible = visibleCountRange();
        return juce::jlimit(
            0, maximumCount(), juce::roundToInt(
                visible.getStart()
                + static_cast<double>(x - bounds.getX())
                    * visible.getLength()
                    / juce::jmax(1, bounds.getWidth())));
    }

    [[nodiscard]] int xForCount(
        juce::Rectangle<int> bounds, int count) const {
        const auto visible = visibleCountRange();
        return bounds.getX() + juce::roundToInt(
            (static_cast<double>(count) - visible.getStart())
            * bounds.getWidth() / juce::jmax(1.0, visible.getLength()));
    }

    void updateScrollRanges() {
        const auto graph = graphArea();
        const int lane_count = juce::jmax(
            1, static_cast<int>(timbre_.layers.size()) + 1);
        const int visible_lanes = juce::jlimit(
            1, lane_count, juce::jmax(1, graph.getHeight() / kLaneHeight));
        vertical_scroll_.setRangeLimits(
            0.0, static_cast<double>(lane_count), juce::dontSendNotification);
        vertical_scroll_.setCurrentRange(
            vertical_scroll_.getCurrentRangeStart(),
            static_cast<double>(visible_lanes), juce::dontSendNotification);
    }

    void scrollBarMoved(juce::ScrollBar*, double) override {
        repaint();
    }

    void drawTimelineMarkers(
        juce::Graphics& graphics,
        juce::Rectangle<int> bounds,
        std::size_t layer_index,
        juce::Colour colour) const {
        const auto& timeline = selectedTimeline(timbre_.layers[layer_index]);
        const auto draw_marker = [&](std::uint32_t count,
                                     juce::Colour marker_colour,
                                     const juce::String& label) {
            const auto visible = visibleCountRange();
            if (count < visible.getStart() || count > visible.getEnd()) {
                return;
            }
            const int x = xForCount(bounds, static_cast<int>(count));
            graphics.setColour(marker_colour);
            graphics.drawVerticalLine(
                x, static_cast<float>(bounds.getY()),
                static_cast<float>(bounds.getBottom()));
            graphics.setFont(juce::FontOptions(10.0F, juce::Font::bold));
            graphics.drawText(
                label, x + 2, bounds.getY(), 34, 13,
                juce::Justification::centredLeft, false);
        };
        draw_marker(timeline.length_counts,
                    juce::Colour(0xFFFFD166), "END");
        if (timeline.loop_start_count) {
            draw_marker(*timeline.loop_start_count,
                        colour.brighter(0.35F), "L>");
        }
        if (timeline.loop_end_count) {
            draw_marker(*timeline.loop_end_count,
                        colour.brighter(0.35F), "<L");
        }
    }

    void drawAutomation(
        juce::Graphics& graphics,
        juce::Rectangle<int> bounds,
        std::size_t layer_index,
        juce::Colour colour) const {
        const auto& layer = timbre_.layers[layer_index];
        const auto& events = selectedEvents(layer);
        const auto kind = selectedEventKind();
        const auto [minimum, maximum] = valueRange();
        std::vector<std::pair<int, int>> points;
        for (const auto& event : events) {
            if (event.kind == kind) {
                points.emplace_back(
                    static_cast<int>(event.count),
                    juce::jlimit(minimum, maximum, event.value));
            }
        }
        if (points.empty()) {
            points.emplace_back(
                0, selectedParameter() == Parameter::Volume
                    ? layer.volume : 0);
        }
        std::stable_sort(points.begin(), points.end());
        juce::Path path;
        for (std::size_t index = 0; index < points.size(); ++index) {
            const auto [timeline_count, parameter_value] = points[index];
            const float x = static_cast<float>(
                xForCount(bounds, timeline_count));
            const float y = juce::jmap(
                static_cast<float>(parameter_value),
                static_cast<float>(minimum), static_cast<float>(maximum),
                static_cast<float>(bounds.getBottom()),
                static_cast<float>(bounds.getY()));
            if (index == 0) {
                path.startNewSubPath(x, y);
            } else {
                path.lineTo(x, y);
            }
            graphics.setColour(colour);
            graphics.fillEllipse(x - 3.0F, y - 3.0F, 6.0F, 6.0F);
        }
        const auto& last = points.back();
        const float last_y = juce::jmap(
            static_cast<float>(last.second),
            static_cast<float>(minimum), static_cast<float>(maximum),
            static_cast<float>(bounds.getBottom()),
            static_cast<float>(bounds.getY()));
        path.lineTo(static_cast<float>(bounds.getRight()), last_y);
        graphics.setColour(colour);
        graphics.strokePath(path, juce::PathStrokeType(2.0F));
    }

    void drawScope(
        juce::Graphics& graphics,
        juce::Rectangle<int> bounds,
        std::size_t channel,
        juce::Colour colour) const {
        const float centre_y = static_cast<float>(bounds.getCentreY());
        graphics.setColour(juce::Colour(0xFF52616E).withAlpha(0.65F));
        graphics.drawHorizontalLine(
            bounds.getCentreY(),
            static_cast<float>(bounds.getX()),
            static_cast<float>(bounds.getRight()));
        if (scope_size_ < 2 || bounds.getWidth() < 2) {
            return;
        }
        constexpr double sample_rate = 48000.0;
        const double frequency = 440.0 * std::pow(
            2.0,
            (static_cast<double>(audition_note_) - 69.0) / 12.0);
        const int sample_count = juce::jlimit(
            2,
            scope_size_,
            juce::roundToInt(2.0 * sample_rate / frequency));
        const int oldest =
            (scope_write_position_ + kScopeHistorySize - scope_size_)
            % kScopeHistorySize;
        const int logical_start = scope_size_ - sample_count;
        const std::size_t scope_channel = channel >= timbre_.layers.size()
            ? 3
            : static_cast<std::size_t>(timbre_.layers[channel].source);
        const auto sample_at = [&](int logical_index) {
            return scope_history_[scope_channel][static_cast<std::size_t>(
                (oldest + logical_start + logical_index)
                % kScopeHistorySize)];
        };
        float peak = 0.0001F;
        for (int index = 0; index < sample_count; ++index) {
            peak = juce::jmax(peak, std::abs(sample_at(index)));
        }
        const float scale =
            static_cast<float>(bounds.getHeight()) * 0.43F / peak;
        juce::Path path;
        for (int x = 0; x < bounds.getWidth(); ++x) {
            const int sample_index = juce::jlimit(
                0,
                sample_count - 1,
                juce::roundToInt(
                    static_cast<double>(x)
                    * static_cast<double>(sample_count - 1)
                    / static_cast<double>(bounds.getWidth() - 1)));
            const float point_x =
                static_cast<float>(bounds.getX() + x);
            const float point_y = centre_y - sample_at(sample_index) * scale;
            if (x == 0) {
                path.startNewSubPath(point_x, point_y);
            } else {
                path.lineTo(point_x, point_y);
            }
        }
        graphics.setColour(colour);
        graphics.strokePath(
            path, juce::PathStrokeType(1.5F));
    }

    [[nodiscard]] static juce::Colour sourceColour(
        mgstc::engine::TimbreSource source) {
        switch (source) {
        case mgstc::engine::TimbreSource::Psg:
            return juce::Colour(0xFFB990FF);
        case mgstc::engine::TimbreSource::Scc:
            return juce::Colour(0xFF53E3A6);
        case mgstc::engine::TimbreSource::Opll:
            return juce::Colour(0xFFFFA75E);
        }
        return juce::Colours::white;
    }

    mgstc::engine::CompositeTimbre timbre_;
    juce::ComboBox parameter_;
    juce::ComboBox edit_mode_;
    juce::TextButton psg_add_;
    juce::TextButton scc_add_;
    juce::TextButton opll_add_;
    juce::TextButton remove_layer_;
    juce::TextEditor value_;
    juce::Label position_;
    juce::Label value_label_;
    juce::TextButton apply_;
    juce::Label inspector_selection_;
    juce::Label length_label_;
    juce::Label loop_start_label_;
    juce::Label loop_end_label_;
    juce::TextEditor length_editor_;
    juce::TextEditor loop_start_editor_;
    juce::TextEditor loop_end_editor_;
    juce::TextButton inspector_apply_;
    juce::TextButton clear_loop_;
    juce::ScrollBar horizontal_scroll_;
    juce::ScrollBar vertical_scroll_;
    EditCallback edit_callback_;
    std::vector<juce::Rectangle<int>> graph_bounds_;
    int selected_layer_{};
    int selected_count_{};
    bool drawing_{};
    static constexpr int kScopeHistorySize = 4096;
    std::array<std::array<float, kScopeHistorySize>, 4>
        scope_history_{};
    int scope_write_position_{};
    int scope_size_{};
    std::uint8_t audition_note_{kPreviewNote};
};

class CompositeEditorComponent final
    : public juce::Component,
      private juce::Timer {
public:
    explicit CompositeEditorComponent(
        SharedAudioService& audio_service,
        SharedMidiInputService& midi_service,
        EditorOpenCallback open_editor)
        : open_editor_(std::move(open_editor)),
          audio_service_(audio_service),
          engine_(audio_service.engine()),
          tooltip_window_(this, 450),
          timbre_(mgstc::engine::defaultCompositeTimbre()),
          performance_keyboard_(midi_service, audio_service_) {
        setWantsKeyboardFocus(true);
        last_audition_note_ = loadLastAuditionNoteSetting();

        title_.setText(
            juce::String::fromUTF8(
                "MGS Tone Craft - 総合音色エディタ"),
            juce::dontSendNotification);
        title_.setFont(juce::FontOptions(23.0F, juce::Font::bold));
        addAndMakeVisible(title_);
        configureSettingsButton(
            settings_, [this] {
                performance_keyboard_.showSettingsDialog();
            });
        addAndMakeVisible(settings_);
        configureMasterVolumeSlider(
            master_volume_, audio_service_, this);
        master_volume_revision_ =
            audio_service_.masterVolumeRevision();
        addAndMakeVisible(master_volume_);
        open_scc_.setButtonText(juce::String::fromUTF8("SCC音色"));
        open_scc_.setTooltip(juce::String::fromUTF8(
            "チャンネルを追加していなくてもSCC音色エディタを開けます"));
        open_scc_.onClick = [this] { open_editor_("scc"); };
        addAndMakeVisible(open_scc_);
        open_opll_.setButtonText(juce::String::fromUTF8("OPLL音色"));
        open_opll_.setTooltip(juce::String::fromUTF8(
            "チャンネルを追加していなくてもOPLL音色エディタを開けます"));
        open_opll_.onClick = [this] { open_editor_("opll"); };
        addAndMakeVisible(open_opll_);
        description_.setText(
            juce::String::fromUTF8(
                "PSG・SCC・OPLLの音色とソフトウェアエンベロープを"
                "レイヤーとして組み合わせます。"),
            juce::dontSendNotification);
        addAndMakeVisible(description_);

        name_label_.setText(
            juce::String::fromUTF8("総合音色名"),
            juce::dontSendNotification);
        addAndMakeVisible(name_label_);
        name_.setText(
            juce::String::fromUTF8(timbre_.name.c_str()), false);
        name_.onTextChange = [this] {
            timbre_.name = name_.getText().toStdString();
        };
        addAndMakeVisible(name_);

        new_.setButtonText(juce::String::fromUTF8("新規"));
        new_.onClick = [this] {
            stopAudition();
            timbre_ = mgstc::engine::defaultCompositeTimbre();
            timbre_.layers.clear();
            selected_composite_id_.reset();
            composite_select_.setSelectedId(
                0, juce::dontSendNotification);
            syncControlsFromModel();
            updateStatus(
                juce::String::fromUTF8(
                    "新しい総合音色を作成しました"));
        };
        addAndMakeVisible(new_);
        save_.setButtonText(juce::String::fromUTF8("保存"));
        save_.setTooltip(
            juce::String::fromUTF8(
                "選択中の総合音色へ上書きします。未保存なら新規保存します"));
        save_.onClick = [this] { saveCompositeTimbre(false); };
        addAndMakeVisible(save_);
        save_as_.setButtonText(
            juce::String::fromUTF8("別名保存"));
        save_as_.setTooltip(
            juce::String::fromUTF8(
                "現在の内容を新しい総合音色として保存します"));
        save_as_.onClick = [this] {
            saveCompositeTimbre(true);
        };
        addAndMakeVisible(save_as_);
        composite_select_.setTextWhenNothingSelected(
            juce::String::fromUTF8("保存済み総合音色"));
        composite_select_.setTooltip(
            juce::String::fromUTF8(
                "保存済みの総合音色を選択して読み込みます"));
        composite_select_.onChange = [this] {
            loadSelectedCompositeTimbre();
        };
        addAndMakeVisible(composite_select_);

        constexpr std::array<const char*, 3> source_names{
            "PSG", "SCC", "OPLL"};
        for (std::size_t index = 0;
             index < juce::jmin(timbre_.layers.size(), source_.size());
             ++index) {
            source_[index].setText(
                source_names[index], juce::dontSendNotification);
            source_[index].setFont(
                juce::FontOptions(17.0F, juce::Font::bold));
            source_[index].setColour(
                juce::Label::textColourId,
                sourceColour(timbre_.layers[index].source));
            addAndMakeVisible(source_[index]);

            enabled_[index].setButtonText(
                juce::String::fromUTF8("有効"));
            mute_[index].setButtonText(
                juce::String::fromUTF8("ミュート"));
            solo_[index].setButtonText(
                juce::String::fromUTF8("ソロ"));
            for (auto* toggle :
                 {&enabled_[index], &mute_[index], &solo_[index]}) {
                toggle->setLookAndFeel(&switch_look_and_feel_);
                toggle->onClick = [this, index] {
                    syncLayerToModel(index);
                };
                addAndMakeVisible(*toggle);
            }

            layer_name_[index].onTextChange = [this, index] {
                syncLayerToModel(index);
            };
            addAndMakeVisible(layer_name_[index]);

            timbre_select_[index].setTextWhenNothingSelected(
                index == 0
                    ? juce::String::fromUTF8("PSG音色は準備中")
                    : juce::String::fromUTF8("保存音色を選択"));
            timbre_select_[index].setEnabled(index != 0);
            timbre_select_[index].setTooltip(
                juce::String::fromUTF8(
                    "音色エディタで名前を付けて保存した音色を選択"));
            timbre_select_[index].onChange = [this, index] {
                selectSavedTimbre(index);
            };
            addAndMakeVisible(timbre_select_[index]);

            const int channel_count = index == 0 ? 3 : (index == 1 ? 5 : 9);
            for (int channel = 0; channel < channel_count; ++channel) {
                channel_[index].addItem(
                    juce::String::fromUTF8("Ch.")
                        + juce::String(channel + 1),
                    channel + 1);
            }
            channel_[index].onChange = [this, index] {
                syncLayerToModel(index);
            };
            addAndMakeVisible(channel_[index]);

            number_mode_[index].addItem(
                juce::String::fromUTF8("自動"), 1);
            number_mode_[index].addItem(
                juce::String::fromUTF8("手動"), 2);
            number_mode_[index].setSelectedId(
                1, juce::dontSendNotification);
            number_mode_[index].setEnabled(index != 0);
            number_mode_[index].setTooltip(
                juce::String::fromUTF8(
                    "MGSC音色番号を自動または手動で割り付け"));
            number_mode_[index].onChange = [this, index] {
                syncNumberAssignment(index);
            };
            addAndMakeVisible(number_mode_[index]);

            timbre_number_[index].setRange(15.0, 31.0, 1.0);
            timbre_number_[index].setSliderStyle(
                juce::Slider::LinearHorizontal);
            timbre_number_[index].setTextBoxStyle(
                juce::Slider::TextBoxLeft, false, 42, 24);
            timbre_number_[index].setValue(
                15.0, juce::dontSendNotification);
            timbre_number_[index].setEnabled(false);
            timbre_number_[index].setTooltip(
                juce::String::fromUTF8(
                    "手動割り付け時のMGSC音色番号（15～31）"));
            timbre_number_[index].onValueChange = [this, index] {
                syncNumberAssignment(index);
            };
            addAndMakeVisible(timbre_number_[index]);

            configureLayerSlider(
                pitch_[index], -24.0, 24.0, " st", index);
            configureLayerSlider(
                detune_[index], -127.0, 127.0, "", index);
            configureLayerSlider(
                delay_[index], 0.0, 120.0, " ct", index);
            configureLayerSlider(
                volume_[index], 0.0, 15.0, "", index);
            configureLayerLabel(
                pitch_label_[index],
                juce::String::fromUTF8("相対音程"));
            configureLayerLabel(
                detune_label_[index],
                juce::String::fromUTF8("デチューン"));
            configureLayerLabel(
                delay_label_[index],
                juce::String::fromUTF8("開始ディレイ"));
            configureLayerLabel(
                volume_label_[index],
                juce::String::fromUTF8("音量"));

            edit_[index].setButtonText(
                index == 0
                    ? juce::String::fromUTF8("PSG設定")
                    : juce::String::fromUTF8("単音色編集"));
            edit_[index].onClick = [this, index] {
                if (index >= timbre_.layers.size()) {
                    return;
                }
                if (timbre_.layers[index].source
                    == mgstc::engine::TimbreSource::Scc) {
                    open_editor_("scc");
                } else if (timbre_.layers[index].source
                           == mgstc::engine::TimbreSource::Opll) {
                    open_editor_("opll");
                }
            };
            edit_[index].setEnabled(index != 0);
            edit_[index].setTooltip(
                index == 0
                    ? juce::String::fromUTF8(
                          "PSG詳細設定は次の実装段階です")
                    : juce::String::fromUTF8(
                          "音源別の単音色エディタを開きます"));
            addAndMakeVisible(edit_[index]);
        }

        // A new composite starts empty. The controls above are reusable slots;
        // channels are created only through the three add buttons.
        timbre_.layers.clear();

        stop_.setButtonText(juce::String::fromUTF8("全停止"));
        stop_.onClick = [this] { stopAudition(); };
        addAndMakeVisible(stop_);

        resource_.setFont(
            juce::FontOptions(13.0F, juce::Font::bold));
        addAndMakeVisible(resource_);
        warning_.setColour(
            juce::Label::textColourId,
            juce::Colour(0xFFFFC56B));
        warning_.setJustificationType(
            juce::Justification::centredLeft);
        addAndMakeVisible(warning_);
        status_.setJustificationType(
            juce::Justification::centredLeft);
        addAndMakeVisible(status_);
        addAndMakeVisible(timeline_);
        timeline_.setEditCallback(
            [this](
                const mgstc::engine::CompositeTimbre& edited,
                bool commit) {
                const bool structure_changed =
                    edited.layers.size() != timbre_.layers.size();
                timbre_ = edited;
                if (structure_changed) {
                    refreshTimbreSelectors();
                    syncControlsFromModel();
                    resized();
                }
                if (!commit) {
                    updateStatus(
                        juce::String::fromUTF8(
                            "共通時間軸をマウス編集中"));
                    return;
                }
                static_cast<void>(configureEngine());
                startCompositeNote(last_audition_note_, true);
                updateStatus(
                    juce::String::fromUTF8(
                        "共通時間軸のイベントを設定しました"));
            });
        performance_keyboard_.setCallbacks(
            [this](std::uint8_t note) {
                startCompositeNote(note, false);
            },
            [this](std::uint8_t note) {
                stopCompositeNote(note);
            });
        performance_keyboard_.setModeCallback(
            [this](bool polyphonic) {
                voice_allocator_.setPolyphonic(polyphonic);
            });
        performance_keyboard_.setSuppressPcInputCallback(
            [this] { return textEntryHasFocusWithin(*this); });
        addAndMakeVisible(performance_keyboard_);

        loadCompositeLibrary();
        loadTimbreLibrary();
        refreshTimbreSelectors();
        refreshCompositeSelector();
        syncControlsFromModel();
        setSize(1440, 940);
        engine_ready_ = audio_service.running() && configureEngine();
        startTimerHz(60);
        updateStatus(
            engine_ready_
                ? juce::String::fromUTF8(
                      "準備完了 / PC鍵盤 Z=C・上段+1oct・[,] [.]=oct移動")
                : juce::String::fromUTF8(
                      "音声出力を開始できませんでした"));
    }

    ~CompositeEditorComponent() override {
        stopTimer();
        stopAudition();
        for (std::size_t index = 0; index < enabled_.size(); ++index) {
            enabled_[index].setLookAndFeel(nullptr);
            mute_[index].setLookAndFeel(nullptr);
            solo_[index].setLookAndFeel(nullptr);
        }
    }

    void prepareVisualInspection() {
        startAudition();
    }

    void refreshExternalState() {
        synchronizeMasterVolumeSlider(
            master_volume_, master_volume_revision_, audio_service_);
        loadCompositeLibrary();
        loadTimbreLibrary();
        refreshCompositeSelector();
        refreshTimbreSelectors();
    }

    void deactivate() {
        performance_keyboard_.allNotesOff();
        stopAudition();
    }

    void paint(juce::Graphics& graphics) override {
        graphics.fillAll(juce::Colour(0xFF20262E));
        const auto outer = getLocalBounds().reduced(20);
        auto right = outer;
        right.removeFromTop(64);
        right.removeFromBottom(108);
        right = right.removeFromRight(commonPanelWidth());
        auto left = outer;
        left.removeFromTop(118);
        left.removeFromBottom(108);
        left.removeFromRight(commonPanelWidth() + 14);
        graphics.setColour(juce::Colour(0xFF29323C));
        graphics.fillRoundedRectangle(left.toFloat(), 9.0F);
        graphics.fillRoundedRectangle(right.toFloat(), 9.0F);
        graphics.setColour(juce::Colour(0xFF435160));
        graphics.drawRoundedRectangle(left.toFloat(), 9.0F, 1.0F);
        graphics.drawRoundedRectangle(right.toFloat(), 9.0F, 1.0F);
    }

    void resized() override {
        auto area = getLocalBounds().reduced(24);
        title_.setBounds(area.removeFromTop(34));
        description_.setBounds(area.removeFromTop(26));
        settings_.setBounds(getWidth() - 58, 24, 34, 34);
        master_volume_.setBounds(getWidth() - 104, 21, 40, 40);
        open_opll_.setBounds(getWidth() - 202, 27, 92, 28);
        open_scc_.setBounds(getWidth() - 296, 27, 88, 28);
        area.removeFromTop(4);

        auto keyboard_area = area.removeFromBottom(96);
        area.removeFromBottom(10);
        performance_keyboard_.setBounds(keyboard_area);
        auto right = area.removeFromRight(commonPanelWidth());
        area.removeFromRight(14);
        auto metadata = area.removeFromTop(38);
        name_label_.setBounds(metadata.removeFromLeft(78));
        name_.setBounds(metadata.removeFromLeft(180));
        metadata.removeFromLeft(6);
        composite_select_.setBounds(
            metadata.removeFromLeft(150));
        metadata.removeFromLeft(6);
        new_.setBounds(metadata.removeFromLeft(50));
        metadata.removeFromLeft(5);
        save_.setBounds(metadata.removeFromLeft(50));
        metadata.removeFromLeft(5);
        save_as_.setBounds(metadata);
        area.removeFromTop(12);

        auto left = area.reduced(14);
        for (std::size_t index = 0;
             index < juce::jmin(timbre_.layers.size(), source_.size());
             ++index) {
            auto row = left.removeFromTop(168);
            source_[index].setBounds(row.removeFromTop(26));
            auto switches = row.removeFromTop(28);
            enabled_[index].setBounds(switches.removeFromLeft(92));
            mute_[index].setBounds(switches.removeFromLeft(112));
            solo_[index].setBounds(switches.removeFromLeft(88));
            row.removeFromTop(5);
            auto identity = row.removeFromTop(30);
            layer_name_[index].setBounds(
                identity.removeFromLeft(110));
            identity.removeFromLeft(6);
            timbre_select_[index].setBounds(
                identity.removeFromLeft(150));
            identity.removeFromLeft(6);
            channel_[index].setBounds(
                identity.removeFromLeft(66));
            identity.removeFromLeft(6);
            number_mode_[index].setBounds(
                identity.removeFromLeft(70));
            identity.removeFromLeft(6);
            timbre_number_[index].setBounds(
                identity.removeFromLeft(54));
            identity.removeFromLeft(6);
            edit_[index].setBounds(identity);
            row.removeFromTop(5);
            auto sliders = row.removeFromTop(54);
            const int width = (sliders.getWidth() - 18) / 4;
            layoutLayerSlider(
                sliders.removeFromLeft(width),
                pitch_label_[index],
                pitch_[index]);
            sliders.removeFromLeft(6);
            layoutLayerSlider(
                sliders.removeFromLeft(width),
                detune_label_[index],
                detune_[index]);
            sliders.removeFromLeft(6);
            layoutLayerSlider(
                sliders.removeFromLeft(width),
                delay_label_[index],
                delay_[index]);
            sliders.removeFromLeft(6);
            layoutLayerSlider(
                sliders,
                volume_label_[index],
                volume_[index]);
            left.removeFromTop(8);
        }

        auto footer = left.removeFromTop(42);
        stop_.setBounds(footer.removeFromLeft(90));
        footer.removeFromLeft(12);
        status_.setBounds(footer);

        right.reduce(12, 12);
        resource_.setBounds(right.removeFromTop(28));
        warning_.setBounds(right.removeFromTop(42));
        right.removeFromTop(8);
        timeline_.setBounds(right);
    }

    bool keyPressed(const juce::KeyPress& key) override {
        if (textEntryHasFocusWithin(*this)) {
            return false;
        }
        const auto modifiers = key.getModifiers();
        if (modifiers.isCtrlDown()
            || modifiers.isAltDown()
            || modifiers.isCommandDown()) {
            return false;
        }
        return performance_keyboard_.shouldConsumeKeyPress(key);
    }

    bool keyStateChanged(bool) override {
        return false;
    }

private:
    [[nodiscard]] int commonPanelWidth() const {
        return juce::jlimit(
            620,
            760,
            juce::roundToInt(
                static_cast<float>(getWidth()) * 0.52F));
    }

    [[nodiscard]] static std::int64_t unixTimeNow() {
        return static_cast<std::int64_t>(
            juce::Time::getCurrentTime().toMilliseconds() / 1000);
    }

    void showCompositeError(const juce::String& message) {
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon,
            juce::String::fromUTF8("総合音色ライブラリ"),
            message,
            juce::String::fromUTF8("閉じる"),
            this);
    }

    [[nodiscard]] bool reloadCompositeLibraryFromDisk() {
        std::string error;
        auto loaded = loadCompositeTimbreLibrary(&error);
        if (!loaded) {
            return false;
        }
        composite_library_ = std::move(*loaded);
        return true;
    }

    void loadCompositeLibrary() {
        if (!reloadCompositeLibraryFromDisk()) {
            composite_library_ = {};
            showCompositeError(
                juce::String::fromUTF8(
                    "既存の総合音色ライブラリを読み込めませんでした"));
        }
    }

    void refreshCompositeSelector() {
        syncing_ = true;
        composite_select_.clear(juce::dontSendNotification);
        composite_library_ids_.clear();
        int item_id = 1;
        int selected_item = 0;
        for (const auto& entry : composite_library_.entries()) {
            composite_library_ids_.push_back(entry.id);
            composite_select_.addItem(
                juce::String::fromUTF8(
                    entry.timbre.name.c_str())
                    + "  r"
                    + juce::String(
                        static_cast<int>(entry.revision)),
                item_id);
            if (selected_composite_id_
                && *selected_composite_id_ == entry.id) {
                selected_item = item_id;
            }
            ++item_id;
        }
        if (selected_composite_id_ && selected_item == 0) {
            selected_composite_id_.reset();
        }
        composite_select_.setSelectedId(
            selected_item, juce::dontSendNotification);
        syncing_ = false;
    }

    void loadSelectedCompositeTimbre() {
        if (syncing_) {
            return;
        }
        const auto selected = composite_select_.getSelectedId();
        if (selected <= 0
            || static_cast<std::size_t>(selected)
                > composite_library_ids_.size()) {
            return;
        }
        const auto id = composite_library_ids_[
            static_cast<std::size_t>(selected - 1)];
        juce::InterProcessLock::ScopedLockType lock(
            library_lock_);
        if (!lock.isLocked()
            || !reloadCompositeLibraryFromDisk()) {
            showCompositeError(
                juce::String::fromUTF8(
                    "共有ライブラリを読み直せませんでした"));
            return;
        }
        const auto* entry = composite_library_.find(id);
        if (!entry) {
            selected_composite_id_.reset();
            refreshCompositeSelector();
            showCompositeError(
                juce::String::fromUTF8(
                    "選択した総合音色は削除されています"));
            return;
        }
        stopAudition();
        selected_composite_id_ = id;
        timbre_ = entry->timbre;
        loadTimbreLibrary();
        refreshTimbreSelectors();
        refreshCompositeSelector();
        syncControlsFromModel();
        updateStatus(
            juce::String::fromUTF8("総合音色を読み込みました"));
        startAudition();
    }

    void saveCompositeTimbre(bool save_as) {
        timbre_.name = utf8String(name_.getText().trim());
        if (timbre_.name.empty()) {
            showCompositeError(
                juce::String::fromUTF8(
                    "保存する総合音色名を入力してください"));
            name_.grabKeyboardFocus();
            return;
        }
        const auto validation =
            mgstc::engine::validateCompositeTimbre(timbre_);
        const auto numbers =
            mgstc::engine::resolveTimbreNumbers(timbre_);
        if (!validation.valid() || !numbers.valid()) {
            showCompositeError(
                juce::String::fromUTF8(
                    "チャンネルまたは音色番号の競合を解消してから"
                    "保存してください"));
            return;
        }

        juce::InterProcessLock::ScopedLockType lock(
            library_lock_);
        if (!lock.isLocked()
            || !reloadCompositeLibraryFromDisk()) {
            showCompositeError(
                juce::String::fromUTF8(
                    "共有ライブラリを更新できませんでした"));
            return;
        }
        if (save_as || !selected_composite_id_) {
            timbre_.name =
                composite_library_.uniqueName(timbre_.name);
            name_.setText(
                juce::String::fromUTF8(timbre_.name.c_str()),
                false);
            selected_composite_id_ = composite_library_.add(
                timbre_, unixTimeNow());
        } else if (!composite_library_.update(
                       *selected_composite_id_,
                       timbre_,
                       unixTimeNow())) {
            selected_composite_id_.reset();
            refreshCompositeSelector();
            showCompositeError(
                juce::String::fromUTF8(
                    "選択中の総合音色は削除されています"));
            return;
        }
        if (!persistCompositeTimbreLibrary(composite_library_)) {
            showCompositeError(
                juce::String::fromUTF8(
                    "総合音色ライブラリを書き込めませんでした"));
            return;
        }
        refreshCompositeSelector();
        updateStatus(
            save_as
                ? juce::String::fromUTF8(
                    "新しい総合音色として保存しました")
                : juce::String::fromUTF8(
                    "総合音色を保存しました"));
    }

    [[nodiscard]] juce::File timbreLibraryFile() const {
        return juce::File::getSpecialLocation(
                   juce::File::userApplicationDataDirectory)
            .getChildFile("MgsToneCraft")
            .getChildFile("timbre-library-v1.mgstc");
    }

    [[nodiscard]] static std::string utf8Text(
        const juce::String& text) {
        const auto utf8 = text.toUTF8();
        return {
            utf8.getAddress(),
            static_cast<std::size_t>(utf8.sizeInBytes() - 1)};
    }

    void loadTimbreLibrary() {
        const auto file = timbreLibraryFile();
        if (!file.existsAsFile()) {
            timbre_library_ = {};
            return;
        }
        std::string error;
        const auto loaded =
            mgstc::engine::TimbreLibrary::deserialize(
                utf8Text(file.loadFileAsString()), &error);
        if (loaded) {
            timbre_library_ = *loaded;
        } else {
            updateStatus(
                juce::String::fromUTF8(
                    "保存音色ライブラリを読み込めませんでした"));
        }
    }

    void refreshTimbreSelectors() {
        for (std::size_t index = 0;
             index < timbre_select_.size();
             ++index) {
            timbre_select_[index].clear(
                juce::dontSendNotification);
            timbre_library_ids_[index].clear();
            if (index >= timbre_.layers.size()
                || timbre_.layers[index].source
                    == mgstc::engine::TimbreSource::Psg) {
                continue;
            }
            const auto category = timbre_.layers[index].source
                    == mgstc::engine::TimbreSource::Scc
                ? mgstc::engine::TimbreCategory::Scc
                : mgstc::engine::TimbreCategory::Opll;
            int item_id = 1;
            for (const auto& entry : timbre_library_.entries()) {
                if (entry.category != category) {
                    continue;
                }
                timbre_library_ids_[index].push_back(entry.id);
                timbre_select_[index].addItem(
                    juce::String::fromUTF8(entry.name.c_str())
                        + "  r"
                        + juce::String(
                            static_cast<int>(entry.revision)),
                    item_id++);
            }
        }
    }

    void selectSavedTimbre(std::size_t index) {
        if (syncing_ || index >= timbre_.layers.size()) {
            return;
        }
        const auto selected =
            timbre_select_[index].getSelectedId();
        if (selected <= 0
            || static_cast<std::size_t>(selected)
                > timbre_library_ids_[index].size()) {
            timbre_.layers[index].base_timbre.reset();
            number_mode_[index].setEnabled(false);
            timbre_number_[index].setEnabled(false);
            refreshModelViews();
            return;
        }
        const auto id =
            timbre_library_ids_[index][
                static_cast<std::size_t>(selected - 1)];
        const auto* entry = timbre_library_.find(id);
        if (!entry) {
            return;
        }
        auto reference =
            mgstc::engine::makeSavedTimbreReference(*entry);
        if (const auto& current =
                timbre_.layers[index].base_timbre) {
            reference.number_mode = current->number_mode;
            reference.manual_number = current->manual_number;
        }
        timbre_.layers[index].base_timbre =
            std::move(reference);
        number_mode_[index].setEnabled(true);
        syncNumberControlsFromModel(index);
        refreshModelViews();
        startAudition();
    }

    void syncNumberControlsFromModel(std::size_t index) {
        if (index >= timbre_.layers.size()) {
            return;
        }
        const auto& reference =
            timbre_.layers[index].base_timbre;
        if (!reference) {
            number_mode_[index].setSelectedId(
                1, juce::dontSendNotification);
            number_mode_[index].setEnabled(false);
            timbre_number_[index].setEnabled(false);
            return;
        }
        const bool manual =
            reference->number_mode
            == mgstc::engine::TimbreNumberMode::Manual;
        number_mode_[index].setSelectedId(
            manual ? 2 : 1, juce::dontSendNotification);
        number_mode_[index].setEnabled(true);
        timbre_number_[index].setValue(
            reference->manual_number.value_or(15),
            juce::dontSendNotification);
        timbre_number_[index].setEnabled(manual);
    }

    void syncNumberAssignment(std::size_t index) {
        if (syncing_ || index >= timbre_.layers.size()
            || !timbre_.layers[index].base_timbre) {
            return;
        }
        auto& reference =
            *timbre_.layers[index].base_timbre;
        const bool manual =
            number_mode_[index].getSelectedId() == 2;
        reference.number_mode = manual
            ? mgstc::engine::TimbreNumberMode::Manual
            : mgstc::engine::TimbreNumberMode::Automatic;
        reference.manual_number = manual
            ? std::optional<std::uint8_t>{
                  static_cast<std::uint8_t>(
                      timbre_number_[index].getValue())}
            : std::nullopt;
        timbre_number_[index].setEnabled(manual);
        refreshModelViews();
    }

    [[nodiscard]] static juce::Colour sourceColour(
        mgstc::engine::TimbreSource source) {
        switch (source) {
        case mgstc::engine::TimbreSource::Psg:
            return juce::Colour(0xFFB990FF);
        case mgstc::engine::TimbreSource::Scc:
            return juce::Colour(0xFF53E3A6);
        case mgstc::engine::TimbreSource::Opll:
            return juce::Colour(0xFFFFA75E);
        }
        return juce::Colours::white;
    }

    void configureLayerSlider(
        juce::Slider& slider,
        double minimum,
        double maximum,
        const juce::String& suffix,
        std::size_t index) {
        slider.setRange(minimum, maximum, 1.0);
        slider.setSliderStyle(juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle(
            juce::Slider::TextBoxBelow, false, 58, 20);
        slider.setTextValueSuffix(suffix);
        slider.onValueChange = [this, index] {
            syncLayerToModel(index);
        };
        addAndMakeVisible(slider);
    }

    void configureLayerLabel(
        juce::Label& label,
        const juce::String& text) {
        label.setText(text, juce::dontSendNotification);
        label.setFont(juce::FontOptions(10.5F));
        label.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(label);
    }

    static void layoutLayerSlider(
        juce::Rectangle<int> area,
        juce::Label& label,
        juce::Slider& slider) {
        label.setBounds(area.removeFromTop(16));
        slider.setBounds(area);
    }

    void setLayerControlsVisible(std::size_t index, bool visible) {
        source_[index].setVisible(visible);
        enabled_[index].setVisible(visible);
        mute_[index].setVisible(visible);
        solo_[index].setVisible(visible);
        layer_name_[index].setVisible(visible);
        timbre_select_[index].setVisible(visible);
        channel_[index].setVisible(visible);
        number_mode_[index].setVisible(visible);
        timbre_number_[index].setVisible(visible);
        pitch_[index].setVisible(visible);
        detune_[index].setVisible(visible);
        delay_[index].setVisible(visible);
        volume_[index].setVisible(visible);
        pitch_label_[index].setVisible(visible);
        detune_label_[index].setVisible(visible);
        delay_label_[index].setVisible(visible);
        volume_label_[index].setVisible(visible);
        edit_[index].setVisible(visible);
    }

    void syncControlsFromModel() {
        syncing_ = true;
        name_.setText(
            juce::String::fromUTF8(timbre_.name.c_str()), false);
        for (std::size_t index = 0; index < source_.size(); ++index) {
            setLayerControlsVisible(index, index < timbre_.layers.size());
        }
        for (std::size_t index = 0;
             index < juce::jmin(timbre_.layers.size(), source_.size());
             ++index) {
            const auto& layer = timbre_.layers[index];
            const char* source_name =
                layer.source == mgstc::engine::TimbreSource::Psg
                ? "PSG"
                : layer.source == mgstc::engine::TimbreSource::Scc
                    ? "SCC" : "OPLL";
            source_[index].setText(
                source_name, juce::dontSendNotification);
            source_[index].setColour(
                juce::Label::textColourId, sourceColour(layer.source));
            const int channel_count =
                layer.source == mgstc::engine::TimbreSource::Psg
                ? 3
                : layer.source == mgstc::engine::TimbreSource::Scc ? 5 : 9;
            channel_[index].clear(juce::dontSendNotification);
            for (int channel = 0; channel < channel_count; ++channel) {
                channel_[index].addItem(
                    juce::String::fromUTF8("Ch.")
                        + juce::String(channel + 1),
                    channel + 1);
            }
            const bool has_saved_timbre =
                layer.source != mgstc::engine::TimbreSource::Psg;
            timbre_select_[index].setEnabled(has_saved_timbre);
            timbre_select_[index].setTextWhenNothingSelected(
                has_saved_timbre
                    ? juce::String::fromUTF8("保存音色を選択")
                    : juce::String::fromUTF8("PSG音色は準備中"));
            edit_[index].setButtonText(
                layer.source == mgstc::engine::TimbreSource::Psg
                    ? juce::String::fromUTF8("PSG設定")
                    : juce::String::fromUTF8("単音色編集"));
            edit_[index].setEnabled(has_saved_timbre);
            enabled_[index].setToggleState(
                layer.enabled, juce::dontSendNotification);
            mute_[index].setToggleState(
                layer.muted, juce::dontSendNotification);
            solo_[index].setToggleState(
                layer.solo, juce::dontSendNotification);
            layer_name_[index].setText(
                juce::String::fromUTF8(layer.name.c_str()), false);
            int selected_timbre = 0;
            if (layer.base_timbre) {
                const auto found = std::find(
                    timbre_library_ids_[index].begin(),
                    timbre_library_ids_[index].end(),
                    layer.base_timbre->library_id);
                if (found != timbre_library_ids_[index].end()) {
                    selected_timbre = static_cast<int>(
                        std::distance(
                            timbre_library_ids_[index].begin(),
                            found))
                        + 1;
                }
            }
            timbre_select_[index].setSelectedId(
                selected_timbre,
                juce::dontSendNotification);
            syncNumberControlsFromModel(index);
            channel_[index].setSelectedId(
                static_cast<int>(layer.channel) + 1,
                juce::dontSendNotification);
            pitch_[index].setValue(
                layer.relative_semitones,
                juce::dontSendNotification);
            detune_[index].setValue(
                layer.detune, juce::dontSendNotification);
            delay_[index].setValue(
                layer.start_delay_counts,
                juce::dontSendNotification);
            volume_[index].setValue(
                layer.volume, juce::dontSendNotification);
        }
        syncing_ = false;
        refreshModelViews();
    }

    void syncLayerToModel(std::size_t index) {
        if (syncing_ || index >= timbre_.layers.size()) {
            return;
        }
        auto& layer = timbre_.layers[index];
        layer.enabled = enabled_[index].getToggleState();
        layer.muted = mute_[index].getToggleState();
        layer.solo = solo_[index].getToggleState();
        layer.name = layer_name_[index].getText().toStdString();
        layer.channel = static_cast<std::uint8_t>(
            juce::jmax(1, channel_[index].getSelectedId()) - 1);
        layer.relative_semitones = static_cast<std::int8_t>(
            pitch_[index].getValue());
        layer.detune = static_cast<std::int16_t>(
            detune_[index].getValue());
        layer.start_delay_counts = static_cast<std::uint32_t>(
            delay_[index].getValue());
        layer.volume = static_cast<std::uint8_t>(
            volume_[index].getValue());
        refreshModelViews();
    }

    void refreshModelViews() {
        timeline_.setTimbre(timbre_);
        const auto validation =
            mgstc::engine::validateCompositeTimbre(timbre_);
        const auto numbers =
            mgstc::engine::resolveTimbreNumbers(timbre_);
        juce::String number_summary;
        for (const auto& assignment : numbers.assignments) {
            const auto& layer =
                timbre_.layers[assignment.layer_index];
            number_summary
                += "  "
                + juce::String::fromUTF8(layer.name.c_str())
                + ":"
                + juce::String(
                    static_cast<int>(assignment.number))
                + (assignment.manually_assigned ? "M" : "A");
        }
        resource_.setText(
            juce::String::fromUTF8("使用チャンネル  PSG ")
                + juce::String(
                    static_cast<int>(validation.psg_channels))
                + " / 3    SCC "
                + juce::String(
                    static_cast<int>(validation.scc_channels))
                + " / 5    OPLL "
                + juce::String(
                    static_cast<int>(validation.opll_channels))
                + " / 9"
                + number_summary,
            juce::dontSendNotification);
        juce::String warning_text =
            juce::String::fromUTF8(
                "共有資源・チャンネル競合なし");
        if (timbre_.layers.empty()) {
            warning_text = juce::String::fromUTF8(
                "上の追加ボタンから必要なチャンネルを追加してください");
        } else if (!validation.warnings.empty()) {
            warning_text = juce::String::fromUTF8(
                validation.warnings.front().c_str());
        } else if (!numbers.warnings.empty()) {
            warning_text = juce::String::fromUTF8(
                numbers.warnings.front().c_str());
        }
        warning_.setText(
            warning_text,
            juce::dontSendNotification);
    }

    [[nodiscard]] static std::uint8_t trackFor(
        const mgstc::engine::CompositeLayer& layer) {
        switch (layer.source) {
        case mgstc::engine::TimbreSource::Psg:
            return layer.channel;
        case mgstc::engine::TimbreSource::Scc:
            return static_cast<std::uint8_t>(3 + layer.channel);
        case mgstc::engine::TimbreSource::Opll:
            return static_cast<std::uint8_t>(8 + layer.channel);
        }
        return 0;
    }

    [[nodiscard]] std::array<std::uint8_t, 3>
    audibleLayerCounts() const {
        std::array<std::uint8_t, 3> counts{};
        for (std::size_t index = 0; index < timbre_.layers.size(); ++index) {
            if (!mgstc::engine::layerIsAudible(timbre_, index)) {
                continue;
            }
            ++counts[static_cast<std::size_t>(timbre_.layers[index].source)];
        }
        return counts;
    }

    [[nodiscard]] std::uint8_t compositeVoiceCapacity(
        const std::array<std::uint8_t, 3>& counts) const {
        constexpr std::array<std::uint8_t, 3> capacities{3, 5, 9};
        std::uint8_t result = 9;
        bool has_source = false;
        for (std::size_t source = 0; source < counts.size(); ++source) {
            if (counts[source] == 0) {
                continue;
            }
            has_source = true;
            result = std::min<std::uint8_t>(
                result,
                static_cast<std::uint8_t>(
                    capacities[source] / counts[source]));
        }
        return has_source ? std::max<std::uint8_t>(result, 1) : 1;
    }

    [[nodiscard]] std::uint8_t trackForVoice(
        std::size_t layer_index,
        std::uint8_t voice,
        const std::array<std::uint8_t, 3>& counts) const {
        const auto& layer = timbre_.layers[layer_index];
        const auto source = static_cast<std::size_t>(layer.source);
        std::uint8_t ordinal = 0;
        for (std::size_t index = 0; index < layer_index; ++index) {
            if (mgstc::engine::layerIsAudible(timbre_, index)
                && timbre_.layers[index].source == layer.source) {
                ++ordinal;
            }
        }
        constexpr std::array<std::uint8_t, 3> bases{0, 3, 8};
        return static_cast<std::uint8_t>(
            bases[source] + voice * counts[source] + ordinal);
    }

    bool configureEngine() {
        auto edit = engine_.beginProgramEdit();
        if (!edit.valid()) {
            return false;
        }
        const auto scc = mgstc::engine::generateSccPreset(
            mgstc::engine::SccWavePreset::Sine,
            mgstc::engine::SccHarmonic::One);
        std::array<std::uint8_t, 32> raw_scc{};
        std::transform(
            scc.begin(),
            scc.end(),
            raw_scc.begin(),
            [](std::int8_t sample) {
                return static_cast<std::uint8_t>(sample);
            });
        auto opll = mgstc::engine::encodeOpllPatch(
            mgstc::engine::defaultOpllPatch());
        for (const auto& layer : timbre_.layers) {
            if (!layer.base_timbre
                || layer.base_timbre->source != layer.source) {
                continue;
            }
            if (layer.source
                == mgstc::engine::TimbreSource::Scc) {
                raw_scc = layer.base_timbre->scc_waveform;
            } else if (layer.source
                       == mgstc::engine::TimbreSource::Opll) {
                opll = layer.base_timbre->opll_registers;
            }
        }
        bool configured =
            edit.engine->session().mapper().defineSccPatch(0, raw_scc)
                == mgstc::engine::MapError::None
            && edit.engine->session().mapper()
                   .defineOpllOriginalPatch(16, opll)
                == mgstc::engine::MapError::None;
        const auto counts = audibleLayerCounts();
        const auto voice_capacity = compositeVoiceCapacity(counts);
        voice_allocator_.setChannelCount(voice_capacity);
        voice_allocator_.setPolyphonic(performance_keyboard_.polyphonic());
        for (std::uint8_t voice = 0; voice < voice_capacity; ++voice) {
          for (std::size_t index = 0; index < timbre_.layers.size(); ++index) {
            const auto& layer = timbre_.layers[index];
            if (!mgstc::engine::layerIsAudible(timbre_, index)) {
                continue;
            }
            const auto track = trackForVoice(index, voice, counts);
            auto envelopes = compileCompositeEnvelopes(layer);
            configured = configured
                && edit.engine->session().setCompositeSequenceEnvelopes(
                    track,
                    std::move(envelopes.volume),
                    std::move(envelopes.pitch),
                    std::move(envelopes.timbre))
                && edit.engine->session().setTrackVolume(
                    track, layer.volume);
            if (layer.source == mgstc::engine::TimbreSource::Psg) {
                configured = configured
                    && edit.engine->session().setPsgToneNoise(
                        track, 1, 0)
                    && edit.engine->session().setPsgFixedVolume(
                        track, layer.volume);
            }
          }
        }
        if (!configured) {
            static_cast<void>(engine_.discardProgramEdit(edit));
            return false;
        }
        return engine_.submitProgram(edit);
    }

    void startAudition() {
        startCompositeNote(last_audition_note_, true);
    }

    void startCompositeNote(
        std::uint8_t base_note,
        bool stop_after_one_second) {
        if (stop_after_one_second || !performance_keyboard_.polyphonic()) {
            stopAudition();
        }
        if (!engine_ready_
            || (voice_allocator_.activeVoiceCount() == 0
                && !configureEngine())) {
            updateStatus(
                juce::String::fromUTF8(
                    "総合音色の発音準備に失敗しました"));
            return;
        }
        const double now =
            juce::Time::getMillisecondCounterHiRes();
        const auto assignment = voice_allocator_.noteOn(base_note);
        if (assignment.stolen_note) {
            stopCompositeVoice(assignment.channel);
        }
        const auto counts = audibleLayerCounts();
        double maximum_delay_ms = 0.0;
        for (std::size_t index = 0; index < timbre_.layers.size(); ++index) {
            const auto& layer = timbre_.layers[index];
            if (!mgstc::engine::layerIsAudible(timbre_, index)) {
                continue;
            }
            const auto note = mgstc::engine::layerMidiNote(
                layer, base_note);
            if (!note) {
                continue;
            }
            const auto track = trackForVoice(
                index, assignment.channel, counts);
            const double delay_ms =
                static_cast<double>(layer.start_delay_counts)
                * (1000.0 / 60.0);
            maximum_delay_ms =
                juce::jmax(maximum_delay_ms, delay_ms);
            if (delay_ms <= 0.0) {
                startLayerNote(track, *note);
            } else {
                pending_notes_.push_back(
                    {
                        .track = track,
                        .note = *note,
                        .start_time_ms = now + delay_ms,
                    });
            }
        }
        if (stop_after_one_second) {
            audition_stop_time_ms_ =
                now + maximum_delay_ms + 1000.0;
            performance_keyboard_.showPreviewNote(base_note);
        }
        last_audition_note_ = base_note;
        saveLastAuditionNoteSetting(last_audition_note_);
        updateStatus(
            (stop_after_one_second
                 ? juce::String::fromUTF8(
                       "総合音色を1秒試聴中 / ")
                 : juce::String::fromUTF8(
                       "鍵盤演奏中 / "))
            + midiNoteName(last_audition_note_)
            + juce::String::fromUTF8("  オクターブ ")
            + juce::String(performance_keyboard_.pcOctave()));
    }

    void stopAudition() {
        audition_stop_time_ms_.reset();
        for (const auto track : sounding_tracks_) {
            static_cast<void>(
                engine_.submit(
                    mgstc::engine::EngineCommand::noteOff(track)));
        }
        sounding_tracks_.clear();
        pending_notes_.clear();
        static_cast<void>(voice_allocator_.allNotesOff());
        performance_keyboard_.clearPreviewNote();
    }

    void stopCompositeVoice(std::uint8_t voice) {
        const auto counts = audibleLayerCounts();
        for (std::size_t index = 0; index < timbre_.layers.size(); ++index) {
            if (!mgstc::engine::layerIsAudible(timbre_, index)) {
                continue;
            }
            const auto track = trackForVoice(index, voice, counts);
            static_cast<void>(engine_.submit(
                mgstc::engine::EngineCommand::noteOff(track)));
            std::erase(sounding_tracks_, track);
            std::erase_if(
                pending_notes_,
                [track](const PendingNote& pending) {
                    return pending.track == track;
                });
        }
    }

    void stopCompositeNote(std::uint8_t note) {
        audition_stop_time_ms_.reset();
        const auto voice = voice_allocator_.noteOff(note);
        if (!voice) {
            return;
        }
        stopCompositeVoice(*voice);
        if (voice_allocator_.activeVoiceCount() == 0) {
            updateStatus(
                juce::String::fromUTF8("鍵盤演奏を停止しました"));
        }
    }

    void startLayerNote(std::uint8_t track, std::uint8_t note) {
        if (engine_.submit(
                mgstc::engine::EngineCommand::noteOn(
                    track, note))) {
            sounding_tracks_.push_back(track);
        }
    }

    [[nodiscard]] juce::File settingsFile() const {
        return juce::File::getSpecialLocation(
                   juce::File::userApplicationDataDirectory)
            .getChildFile("MgsToneCraft")
            .getChildFile("settings-v1.ini");
    }

    [[nodiscard]] std::uint8_t
    loadLastAuditionNoteSetting() const {
        const auto file = settingsFile();
        if (!file.existsAsFile()) {
            return kPreviewNote;
        }
        return static_cast<std::uint8_t>(juce::jlimit(
            24,
            119,
            static_cast<int>(GetPrivateProfileIntW(
                L"Application",
                L"LastAuditionNote",
                kPreviewNote,
                file.getFullPathName().toWideCharPointer()))));
    }

    void saveLastAuditionNoteSetting(std::uint8_t note) {
        const auto file = settingsFile();
        if (file.getParentDirectory().createDirectory().failed()) {
            return;
        }
        const auto path_text = file.getFullPathName();
        const auto value = juce::String(static_cast<int>(note));
        static_cast<void>(WritePrivateProfileStringW(
            L"Application",
            L"LastAuditionNote",
            value.toWideCharPointer(),
            path_text.toWideCharPointer()));
        static_cast<void>(WritePrivateProfileStringW(
            nullptr,
            nullptr,
            nullptr,
            path_text.toWideCharPointer()));
    }

    void timerCallback() override {
        synchronizeMasterVolumeSlider(
            master_volume_, master_volume_revision_, audio_service_);
        const double now =
            juce::Time::getMillisecondCounterHiRes();
        for (auto pending = pending_notes_.begin();
             pending != pending_notes_.end();) {
            if (now < pending->start_time_ms) {
                ++pending;
                continue;
            }
            startLayerNote(pending->track, pending->note);
            pending = pending_notes_.erase(pending);
        }
        if (++settings_poll_ticks_ >= 30) {
            settings_poll_ticks_ = 0;
            const auto note = loadLastAuditionNoteSetting();
            if (!performance_keyboard_.hasActiveNote()
                && note != last_audition_note_) {
                last_audition_note_ = note;
            }
        }
        mgstc::engine::OpllScopeFrame scope_frame{};
        if (componentWindowIsActive(*this)) {
            while (engine_.pollOpllScope(scope_frame)) {
                timeline_.appendScopeFrame(
                    scope_frame, last_audition_note_);
            }
        }
        if (audition_stop_time_ms_
            && now >= *audition_stop_time_ms_) {
            stopAudition();
            updateStatus(
                juce::String::fromUTF8(
                    "総合音色の1秒試聴が完了しました"));
        }
    }

    void updateStatus(const juce::String& text) {
        status_.setText(text, juce::dontSendNotification);
    }

    EditorOpenCallback open_editor_;
    SharedAudioService& audio_service_;
    mgstc::engine::RealtimeEngineHost& engine_;
    juce::TooltipWindow tooltip_window_;
    SwitchLookAndFeel switch_look_and_feel_;
    mgstc::engine::CompositeTimbre timbre_;
    juce::Label title_;
    juce::Label description_;
    juce::Label name_label_;
    juce::TextEditor name_;
    juce::ComboBox composite_select_;
    juce::TextButton new_;
    juce::TextButton save_;
    juce::TextButton save_as_;
    std::array<juce::Label, 3> source_;
    std::array<juce::ToggleButton, 3> enabled_;
    std::array<juce::ToggleButton, 3> mute_;
    std::array<juce::ToggleButton, 3> solo_;
    std::array<juce::TextEditor, 3> layer_name_;
    std::array<juce::ComboBox, 3> timbre_select_;
    std::array<juce::ComboBox, 3> channel_;
    std::array<juce::ComboBox, 3> number_mode_;
    std::array<juce::Slider, 3> timbre_number_;
    std::array<juce::Slider, 3> pitch_;
    std::array<juce::Slider, 3> detune_;
    std::array<juce::Slider, 3> delay_;
    std::array<juce::Slider, 3> volume_;
    std::array<juce::Label, 3> pitch_label_;
    std::array<juce::Label, 3> detune_label_;
    std::array<juce::Label, 3> delay_label_;
    std::array<juce::Label, 3> volume_label_;
    std::array<juce::TextButton, 3> edit_;
    juce::TextButton stop_;
    juce::Label resource_;
    juce::Label warning_;
    juce::Label status_;
    CompositeTimeline timeline_;
    juce::TextButton open_scc_;
    juce::TextButton open_opll_;
    juce::DrawableButton settings_{
        "settings", juce::DrawableButton::ImageOnButtonBackground};
    juce::Slider master_volume_;
    PerformanceKeyboard performance_keyboard_;
    mgstc::engine::SequentialVoiceAllocator voice_allocator_{9};
    mgstc::engine::TimbreLibrary timbre_library_;
    mgstc::engine::CompositeTimbreLibrary composite_library_;
    std::vector<std::uint64_t> composite_library_ids_;
    std::optional<std::uint64_t> selected_composite_id_;
    std::array<std::vector<std::uint64_t>, 3>
        timbre_library_ids_;
    juce::InterProcessLock library_lock_{
        "MgsToneCraftTimbreLibraryV1"};
    struct PendingNote {
        std::uint8_t track{};
        std::uint8_t note{};
        double start_time_ms{};
    };
    std::vector<std::uint8_t> sounding_tracks_;
    std::vector<PendingNote> pending_notes_;
    std::uint8_t last_audition_note_{kPreviewNote};
    int settings_poll_ticks_{};
    std::uint64_t master_volume_revision_{};
    bool syncing_{};
    bool engine_ready_{};
    std::optional<double> audition_stop_time_ms_;
};

class PresetWavePreview final
    : public juce::Component,
      public juce::SettableTooltipClient {
public:
    PresetWavePreview() {
        setTooltip(
            juce::String::fromUTF8(
                "選択中のプリセットと倍音を適用した後の波形"));
    }

    void setWaveform(const SccWaveform& waveform) {
        waveform_ = waveform;
        repaint();
    }

    void paint(juce::Graphics& graphics) override {
        const auto bounds = getLocalBounds().toFloat().reduced(1.0F);
        graphics.setColour(juce::Colour(0xFF182028));
        graphics.fillRoundedRectangle(bounds, 5.0F);
        graphics.setColour(juce::Colour(0xFF435160));
        graphics.drawRoundedRectangle(bounds, 5.0F, 1.0F);

        const auto graph = bounds.reduced(5.0F, 4.0F);
        graphics.setColour(juce::Colour(0xFF586675));
        graphics.drawHorizontalLine(
            juce::roundToInt(graph.getCentreY()),
            graph.getX(),
            graph.getRight());

        juce::Path path;
        for (std::size_t index = 0; index < waveform_.size(); ++index) {
            const auto x = juce::jmap(
                static_cast<float>(index),
                0.0F,
                static_cast<float>(waveform_.size() - 1),
                graph.getX(),
                graph.getRight());
            const auto y = juce::jmap(
                static_cast<float>(waveform_[index]),
                -128.0F,
                127.0F,
                graph.getBottom(),
                graph.getY());
            if (index == 0) {
                path.startNewSubPath(x, y);
            } else {
                path.lineTo(x, y);
            }
        }
        graphics.setColour(juce::Colour(0xFF53E3A6));
        graphics.strokePath(
            path,
            juce::PathStrokeType(
                1.4F,
                juce::PathStrokeType::curved,
                juce::PathStrokeType::rounded));
    }

private:
    SccWaveform waveform_{};
};

class SccPresetMenuItem final : public juce::PopupMenu::CustomComponent {
public:
    SccPresetMenuItem(juce::String name, SccWaveform waveform)
        : juce::PopupMenu::CustomComponent(true),
          name_(std::move(name)), waveform_(waveform) {}

    void getIdealSize(int& width, int& height) override {
        width = 292;
        height = 46;
    }

    void paint(juce::Graphics& graphics) override {
        if (isItemHighlighted()) {
            graphics.fillAll(juce::Colour(0xFF31414B));
        }
        auto area = getLocalBounds().reduced(9, 5);
        auto label = area.removeFromLeft(104);
        graphics.setColour(juce::Colour(0xFFE6EDF3));
        graphics.setFont(juce::FontOptions(13.0F, juce::Font::bold));
        graphics.drawText(
            name_, label, juce::Justification::centredLeft, false);

        const auto graph = area.toFloat().reduced(4.0F, 2.0F);
        graphics.setColour(juce::Colour(0xFF172028));
        graphics.fillRoundedRectangle(graph, 4.0F);
        graphics.setColour(juce::Colour(0xFF53616D));
        graphics.drawHorizontalLine(
            juce::roundToInt(graph.getCentreY()),
            graph.getX(), graph.getRight());
        juce::Path path;
        for (std::size_t index = 0; index < waveform_.size(); ++index) {
            const float x = juce::jmap(
                static_cast<float>(index), 0.0F,
                static_cast<float>(waveform_.size() - 1),
                graph.getX(), graph.getRight());
            const float y = juce::jmap(
                static_cast<float>(waveform_[index]),
                -128.0F, 127.0F,
                graph.getBottom(), graph.getY());
            if (index == 0) {
                path.startNewSubPath(x, y);
            } else {
                path.lineTo(x, y);
            }
        }
        graphics.setColour(juce::Colour(0xFF53E3A6));
        graphics.strokePath(path, juce::PathStrokeType(1.5F));
    }

private:
    juce::String name_;
    SccWaveform waveform_{};
};

class SccEditorComponent final
    : public juce::Component,
      private juce::Timer {
public:
    explicit SccEditorComponent(
        SharedAudioService& audio_service,
        SharedMidiInputService& midi_service,
        EditorOpenCallback open_editor)
        : open_editor_(std::move(open_editor)),
          audio_service_(audio_service),
          engine_(audio_service.engine()),
          tooltip_window_(this, 450),
          performance_keyboard_(midi_service, audio_service_) {
        setWantsKeyboardFocus(true);
        scc_wave_ = mgstc::engine::generateSccPreset(
            mgstc::engine::SccWavePreset::Sine,
            mgstc::engine::SccHarmonic::One);
        history_.push_back(scc_wave_);
        last_audition_note_ = loadLastAuditionNoteSetting();

        title_.setText(
            juce::String::fromUTF8("SCC音色エディタ"),
            juce::dontSendNotification);
        title_.setFont(juce::FontOptions(23.0F, juce::Font::bold));
        addAndMakeVisible(title_);
        configureSettingsButton(
            settings_, [this] {
                performance_keyboard_.showSettingsDialog();
            });
        addAndMakeVisible(settings_);
        configureMasterVolumeSlider(
            master_volume_, audio_service_, this);
        master_volume_revision_ =
            audio_service_.masterVolumeRevision();
        addAndMakeVisible(master_volume_);
        configureImmediateAuditionButton(
            immediate_audition_,
            loadSccImmediateAuditionSetting(),
            [this] {
                saveSccImmediateAuditionSetting(
                    immediate_audition_.getToggleState());
                updateStatus(
                    immediate_audition_.getToggleState()
                        ? juce::String::fromUTF8(
                              "SCCの即時発音をONにしました")
                        : juce::String::fromUTF8(
                              "SCCの即時発音をOFFにしました"));
            });
        addAndMakeVisible(immediate_audition_);

        description_.setText(
            juce::String::fromUTF8(
                "32サンプルの波形を直接編集できます。"
                "グラフ両端は周期的につながっています。"),
            juce::dontSendNotification);
        addAndMakeVisible(description_);

        configureIconButton(
            load_,
            EditorIcon::Open,
            juce::String::fromUTF8("ファイル読込"),
            [this] { loadDefinition(); });
        configureIconButton(
            save_,
            EditorIcon::Save,
            juce::String::fromUTF8("ファイル保存"),
            [this] { saveDefinition(); });
        configureIconButton(
            paste_,
            EditorIcon::Paste,
            juce::String::fromUTF8(
                "貼り付け（音声、画像背景、WAV、SCC定義）"),
            [this] { pasteClipboard(); });
        configureIconButton(
            copy_,
            EditorIcon::Copy,
            juce::String::fromUTF8("SCC定義をコピー"),
            [this] { copyDefinition(); });
        configureButton(
            import_wave_,
            "WAV",
            juce::String::fromUTF8("WAVファイルから変換"),
            [this] { importWaveFile(); });
        configureButton(
            import_audacity_,
            juce::String::fromUTF8("Audacityから変換"),
            juce::String::fromUTF8(
                "Audacityの選択範囲から変換します。"
                "事前にmod-script-pipeを有効にしてください。"
                "有効中は同じPC上の他のプログラムからも"
                "Audacityを操作できるため、信頼できる環境で"
                "使用してください"),
            [this] { importAudacitySelection(); });
        configureButton(
            open_opll_,
            juce::String::fromUTF8("OPLLを開く"),
            juce::String::fromUTF8(
                "OPLL音色エディタを別ウィンドウで開きます"),
            [this] { open_editor_("opll"); });
        configureIconButton(
            convert_to_opll_, EditorIcon::Convert,
            juce::String::fromUTF8(
                "現在のSCC波形をOPLL音色として近似変換"),
            [this] { convertToOpll(); });

        rebuildPresetMenu();
        preset_.setSelectedId(1, juce::dontSendNotification);
        preset_.setTooltip(
            juce::String::fromUTF8("適用するプリセット波形"));
        preset_.onChange = [this] { refreshPresetPreview(); };
        addAndMakeVisible(preset_);
        preset_preview_.setInterceptsMouseClicks(false, false);
        addAndMakeVisible(preset_preview_);

        harmonic_.setSliderStyle(juce::Slider::LinearHorizontal);
        harmonic_.setRange(0.0, 4.0, 1.0);
        harmonic_.setTextBoxStyle(
            juce::Slider::TextBoxRight, false, 54, 24);
        harmonic_.textFromValueFunction = [](double value) {
            constexpr std::array<const char*, 5> labels{
                "1x", "1.5x", "2x", "3x", "4x"};
            return juce::String(
                labels[static_cast<std::size_t>(
                    juce::jlimit(0, 4, juce::roundToInt(value)))]);
        };
        harmonic_.valueFromTextFunction =
            [](const juce::String& text) {
                if (text.startsWith("1.5")) {
                    return 1.0;
                }
                if (text.startsWith("2")) {
                    return 2.0;
                }
                if (text.startsWith("3")) {
                    return 3.0;
                }
                if (text.startsWith("4")) {
                    return 4.0;
                }
                return 0.0;
            };
        harmonic_.setValue(0.0, juce::dontSendNotification);
        harmonic_.setTooltip(
            juce::String::fromUTF8(
                "プリセットの倍音。1.5xは1xと2xの"
                "位相を揃えた50%ブレンドです"));
        harmonic_.onValueChange = [this] {
            rebuildPresetMenu();
            refreshPresetPreview();
        };
        addAndMakeVisible(harmonic_);

        apply_range_.addItem(
            juce::String::fromUTF8("全体"), 1);
        apply_range_.addItem(
            juce::String::fromUTF8("左半分"), 2);
        apply_range_.addItem(
            juce::String::fromUTF8("右半分"), 3);
        apply_range_.setSelectedId(1, juce::dontSendNotification);
        apply_range_.setTooltip(
            juce::String::fromUTF8("プリセットの適用範囲"));
        apply_range_.onChange = [this] { refreshPresetPreview(); };
        addAndMakeVisible(apply_range_);

        merge_enabled_.setButtonText(
            juce::String::fromUTF8("現在波形とマージ"));
        merge_enabled_.setTooltip(
            juce::String::fromUTF8(
                "プリセットで置換せず、現在波形へ混ぜます"));
        merge_enabled_.setLookAndFeel(&switch_look_and_feel_);
        merge_enabled_.onClick = [this] {
            updateMergeControlState();
            refreshPresetPreview();
        };
        addAndMakeVisible(merge_enabled_);

        merge_amount_.setSliderStyle(juce::Slider::LinearHorizontal);
        merge_amount_.setRange(0.0, 100.0, 1.0);
        merge_amount_.setValue(50.0, juce::dontSendNotification);
        merge_amount_.setTextBoxStyle(
            juce::Slider::TextBoxRight, false, 54, 24);
        merge_amount_.setTextValueSuffix("%");
        merge_amount_.setTooltip(
            juce::String::fromUTF8("プリセットを混ぜる量"));
        merge_amount_.onValueChange =
            [this] { refreshPresetPreview(); };
        addAndMakeVisible(merge_amount_);

        auto_phase_.setButtonText(
            juce::String::fromUTF8("位相自動"));
        polarity_.setButtonText(
            juce::String::fromUTF8("極性反転可"));
        preserve_volume_.setButtonText(
            juce::String::fromUTF8("音量維持"));
        auto_phase_.setToggleState(true, juce::dontSendNotification);
        polarity_.setToggleState(true, juce::dontSendNotification);
        preserve_volume_.setToggleState(
            true, juce::dontSendNotification);
        for (auto* option :
             {&auto_phase_, &polarity_, &preserve_volume_}) {
            option->setLookAndFeel(&switch_look_and_feel_);
            option->onClick = [this] { refreshPresetPreview(); };
            addAndMakeVisible(*option);
        }
        updateMergeControlState();

        preset_flip_h_.setButtonText(
            juce::String::fromUTF8("左右反転"));
        preset_flip_h_.setTooltip(
            juce::String::fromUTF8(
                "プリセット候補を左右反転（サンプル順の反転）します"));
        preset_flip_h_.setLookAndFeel(&switch_look_and_feel_);
        preset_flip_h_.onClick = [this] { refreshPresetPreview(); };
        addAndMakeVisible(preset_flip_h_);
        preset_flip_v_.setButtonText(
            juce::String::fromUTF8("上下反転"));
        preset_flip_v_.setTooltip(
            juce::String::fromUTF8(
                "プリセット候補を上下反転（極性反転）します。"
                "Saw の向き切替にも使えます"));
        preset_flip_v_.setLookAndFeel(&switch_look_and_feel_);
        preset_flip_v_.onClick = [this] { refreshPresetPreview(); };
        addAndMakeVisible(preset_flip_v_);

        configureButton(
            apply_preset_,
            juce::String::fromUTF8("適用"),
            juce::String::fromUTF8(
                "選択したプリセットを波形へ適用します"),
            [this] { applyPreset(); });
        configureButton(
            cancel_preview_,
            juce::String::fromUTF8("取消"),
            juce::String::fromUTF8(
                "プレビュー候補を破棄し、確定波形へ戻します"),
            [this] { cancelPreview(); });
        cancel_preview_.setEnabled(false);

        configureIconButton(
            undo_,
            EditorIcon::Undo,
            "Undo (Ctrl+Z)",
            [this] { undo(); });
        configureIconButton(
            redo_,
            EditorIcon::Redo,
            "Redo (Ctrl+Y)",
            [this] { redo(); });
        configureButton(
            ab_audition_,
            "A/B",
            juce::String::fromUTF8(
                "候補があるとき、確定波形(A)と候補(B)を交互に試聴します"),
            [this] { auditionAbCompare(); });
        ab_audition_.setEnabled(false);
        configureIconButton(
            average_,
            EditorIcon::Average,
            juce::String::fromUTF8("平均化"),
            [this] {
                commitWave(
                    mgstc::engine::averageSccWaveform(scc_wave_));
            });
        configureIconButton(
            normalize_,
            EditorIcon::Normalize,
            juce::String::fromUTF8("正規化"),
            [this] {
                commitWave(
                    mgstc::engine::normalizeSccWaveform(scc_wave_));
            });
        configureIconButton(
            invert_,
            EditorIcon::Invert,
            juce::String::fromUTF8("反転"),
            [this] {
                commitWave(
                    mgstc::engine::invertSccWaveform(scc_wave_));
            });
        configureIconButton(
            shift_up_,
            EditorIcon::Up,
            juce::String::fromUTF8("波形全体を上へ移動"),
            [this] {
                commitWave(
                    mgstc::engine::shiftSccWaveformVertically(
                        scc_wave_, 1));
            });
        configureIconButton(
            rotate_left_,
            EditorIcon::Left,
            juce::String::fromUTF8("位相-1"),
            [this] {
                commitWave(
                    mgstc::engine::rotateSccWaveform(scc_wave_, -1));
            });
        configureIconButton(
            rotate_right_,
            EditorIcon::Right,
            juce::String::fromUTF8("位相+1"),
            [this] {
                commitWave(
                    mgstc::engine::rotateSccWaveform(scc_wave_, 1));
            });
        configureIconButton(
            shift_down_,
            EditorIcon::Down,
            juce::String::fromUTF8("波形全体を下へ移動"),
            [this] {
                commitWave(
                    mgstc::engine::shiftSccWaveformVertically(
                        scc_wave_, -1));
            });

        vertical_scale_.setSliderStyle(
            juce::Slider::LinearVertical);
        vertical_scale_.setRange(0.0, 200.0, 1.0);
        vertical_scale_.setValue(
            100.0, juce::dontSendNotification);
        vertical_scale_.setTextBoxStyle(
            juce::Slider::NoTextBox, false, 0, 0);
        vertical_scale_.setTooltip(
            juce::String::fromUTF8(
                "波形の縦倍率。バーを離した時点で適用します"));
        vertical_scale_.onDragStart = [this] {
            if (preview_active_) {
                dismissPreviewState(false);
            }
            scale_source_ = scc_wave_;
            scale_previewing_ = true;
        };
        vertical_scale_.onValueChange = [this] {
            scale_reset_.setButtonText(
                juce::String(
                    juce::roundToInt(vertical_scale_.getValue()))
                + "%");
            if (!scale_previewing_
                && !vertical_scale_.isMouseButtonDown()) {
                return;
            }
            const auto scaled =
                mgstc::engine::scaleSccWaveformVertically(
                    scale_source_,
                    juce::roundToInt(vertical_scale_.getValue()));
            graph_.setPreview(scc_wave_, scaled);
            if (immediate_audition_.getToggleState()
                && engine_ready_) {
                static_cast<void>(auditionAfterEdit(&scaled));
            }
        };
        vertical_scale_.onDragEnd = [this] {
            scale_previewing_ = false;
            const auto percent =
                juce::roundToInt(vertical_scale_.getValue());
            commitWave(
                mgstc::engine::scaleSccWaveformVertically(
                    scale_source_, percent));
            vertical_scale_.setValue(
                100.0, juce::dontSendNotification);
            scale_reset_.setButtonText("100%");
        };
        addAndMakeVisible(vertical_scale_);

        configureButton(
            scale_reset_,
            "100%",
            juce::String::fromUTF8(
                "クリックすると倍率表示を100%へ戻します。"
                "この操作では波形を拡縮しません"),
            [this] {
                vertical_scale_.setValue(
                    100.0, juce::dontSendNotification);
                scale_reset_.setButtonText("100%");
                if (scale_previewing_) {
                    graph_.setPreview(scc_wave_, scale_source_);
                }
            });

        graph_.setWaveform(scc_wave_);
        graph_.setCommitCallback(
            [this](
                const SccWaveform& before,
                const SccWaveform& after) {
                if (before != after) {
                    commitWave(after);
                }
            });
        graph_.setLiveEditCallback(
            [this](const SccWaveform& waveform) {
                if (immediate_audition_.getToggleState()
                    && auditionOneSecond(&waveform)) {
                    updateStatus(
                        juce::String::fromUTF8(
                            "波形描画を1秒試聴中"));
                    }
            });
        graph_.setValueCommitCallback(
            [this](const SccWaveform& waveform) {
                commitWave(waveform);
            });
        graph_.setPreviewClearedCallback([this] {
            dismissPreviewState(false);
        });
        addAndMakeVisible(graph_);

        background_load_.setButtonText(
            juce::String::fromUTF8("背景…"));
        background_load_.setTooltip(
            juce::String::fromUTF8(
                "波形トレース用の背景画像を読み込みます。"
                "音色値は変更しません"));
        background_load_.onClick = [this] { chooseBackgroundImage(); };
        addAndMakeVisible(background_load_);
        background_clear_.setButtonText(
            juce::String::fromUTF8("背景消"));
        background_clear_.setTooltip(
            juce::String::fromUTF8(
                "背景画像の参照をクリアします"));
        background_clear_.onClick = [this] {
            background_path_.clear();
            background_image_ = {};
            applyBackgroundToGraph();
            saveBackgroundSettings();
            updateStatus(
                juce::String::fromUTF8("背景画像をクリアしました"));
        };
        addAndMakeVisible(background_clear_);
        background_visible_.setButtonText(
            juce::String::fromUTF8("背景表示"));
        background_visible_.setToggleState(
            true, juce::dontSendNotification);
        background_visible_.onClick = [this] {
            applyBackgroundToGraph();
            saveBackgroundSettings();
        };
        addAndMakeVisible(background_visible_);
        const auto configure_bg_slider = [this](
            juce::Slider& slider,
            double minimum,
            double maximum,
            double initial,
            const juce::String& tip) {
            slider.setRange(minimum, maximum, 1.0);
            slider.setValue(initial, juce::dontSendNotification);
            slider.setSliderStyle(juce::Slider::LinearHorizontal);
            slider.setTextBoxStyle(
                juce::Slider::TextBoxRight, false, 52, 22);
            slider.setTooltip(tip);
            slider.onValueChange = [this] {
                applyBackgroundToGraph();
            };
            slider.onDragEnd = [this] {
                saveBackgroundSettings();
            };
            addAndMakeVisible(slider);
        };
        configure_bg_slider(
            background_opacity_,
            0.0,
            100.0,
            35.0,
            juce::String::fromUTF8("背景の不透明度 %"));
        configure_bg_slider(
            background_x_,
            -512.0,
            512.0,
            0.0,
            juce::String::fromUTF8("背景のX位置（px）"));
        configure_bg_slider(
            background_y_,
            -512.0,
            512.0,
            0.0,
            juce::String::fromUTF8("背景のY位置（px）"));
        configure_bg_slider(
            background_width_,
            8.0,
            2048.0,
            320.0,
            juce::String::fromUTF8("背景の表示幅（px）"));
        configure_bg_slider(
            background_height_,
            8.0,
            2048.0,
            160.0,
            juce::String::fromUTF8("背景の表示高さ（px）"));
        background_opacity_label_.setText(
            "Op%", juce::dontSendNotification);
        background_x_label_.setText("X", juce::dontSendNotification);
        background_y_label_.setText("Y", juce::dontSendNotification);
        background_w_label_.setText("W", juce::dontSendNotification);
        background_h_label_.setText("H", juce::dontSendNotification);
        for (auto* label :
             {&background_opacity_label_,
              &background_x_label_,
              &background_y_label_,
              &background_w_label_,
              &background_h_label_}) {
            label->setJustificationType(
                juce::Justification::centredRight);
            addAndMakeVisible(*label);
        }

        library_title_.setText(
            juce::String::fromUTF8("音色ライブラリ"),
            juce::dontSendNotification);
        library_title_.setFont(
            juce::FontOptions(18.0F, juce::Font::bold));
        addAndMakeVisible(library_title_);

        library_list_.setTextWhenNothingSelected(
            juce::String::fromUTF8("保存済み音色を選択"));
        library_list_.setTooltip(
            juce::String::fromUTF8("SCC音色ライブラリ"));
        library_list_.onChange = [this] {
            selectLibraryEntryFromList();
        };
        addAndMakeVisible(library_list_);

        library_filter_.setTextToShowWhenEmpty(
            juce::String::fromUTF8("名前・タグ・メモを検索"),
            juce::Colour(0xFF7F8993));
        library_filter_.setTooltip(
            juce::String::fromUTF8("SCC音色ライブラリを絞り込みます"));
        library_filter_.onTextChange = [this] {
            refreshLibraryList();
        };
        addAndMakeVisible(library_filter_);

        name_.setTextToShowWhenEmpty(
            juce::String::fromUTF8("音色名"),
            juce::Colour(0xFF7F8993));
        tags_.setTextToShowWhenEmpty(
            juce::String::fromUTF8("タグ"),
            juce::Colour(0xFF7F8993));
        memo_.setTextToShowWhenEmpty(
            juce::String::fromUTF8("メモ"),
            juce::Colour(0xFF7F8993));
        memo_.setMultiLine(true);
        memo_.setReturnKeyStartsNewLine(true);
        addAndMakeVisible(name_);
        addAndMakeVisible(tags_);
        addAndMakeVisible(memo_);

        favorite_.setButtonText(
            juce::String::fromUTF8("お気に入り"));
        favorite_.setLookAndFeel(&switch_look_and_feel_);
        addAndMakeVisible(favorite_);
        configureButton(
            library_new_,
            juce::String::fromUTF8("新規"),
            juce::String::fromUTF8("新しいSCC音色を作成"),
            [this] { newLibraryEntry(); });
        configureButton(
            library_load_,
            juce::String::fromUTF8("読込"),
            juce::String::fromUTF8("選択した音色を読み込みます"),
            [this] { loadSelectedLibraryEntry(); });
        configureButton(
            library_save_,
            juce::String::fromUTF8("保存"),
            juce::String::fromUTF8("選択中の音色を更新します"),
            [this] { saveLibraryEntry(false); });
        configureButton(
            library_save_as_,
            juce::String::fromUTF8("別名保存"),
            juce::String::fromUTF8("新しい音色として保存します"),
            [this] { saveLibraryEntry(true); });
        configureButton(
            library_cancel_,
            juce::String::fromUTF8("取消"),
            juce::String::fromUTF8("編集開始時点の音色へ戻します"),
            [this] { restoreEditorBaseline(); });
        configureButton(
            library_delete_,
            juce::String::fromUTF8("削除"),
            juce::String::fromUTF8("選択中の音色をライブラリから削除します"),
            [this] { deleteSelectedLibraryEntry(); });
        configureButton(
            library_import_,
            juce::String::fromUTF8("取込"),
            juce::String::fromUTF8("外部.mgstcライブラリを取り込みます"),
            [this] { importLibraryFile(); });
        configureButton(
            library_export_,
            juce::String::fromUTF8("書出"),
            juce::String::fromUTF8("選択中の音色を.mgstcへ書き出します"),
            [this] { exportSelectedLibraryEntry(); });

        mgsc_title_.setText(
            juce::String::fromUTF8("MGSC @s 定義プレビュー"),
            juce::dontSendNotification);
        mgsc_title_.setFont(
            juce::FontOptions(15.0F, juce::Font::bold));
        addAndMakeVisible(mgsc_title_);
        output_number_label_.setText(
            juce::String::fromUTF8("一時出力番号"),
            juce::dontSendNotification);
        addAndMakeVisible(output_number_label_);
        output_number_.setInputRestrictions(2, "0123456789");
        output_number_.setText("0", false);
        output_number_.setTooltip(
            juce::String::fromUTF8(
                "SCCのMGSC出力番号 0～31。"
                "ライブラリには保存されません"));
        output_number_.onTextChange =
            [this] { updateDefinitionPreview(); };
        addAndMakeVisible(output_number_);
        mgsc_preview_.setMultiLine(true);
        mgsc_preview_.setReadOnly(true);
        mgsc_preview_.setScrollbarsShown(true);
        mgsc_preview_.setFont(
            juce::FontOptions(12.0F, juce::Font::plain));
        mgsc_preview_.setTooltip(
            juce::String::fromUTF8(
                "確定済み波形から生成したMGSC定義"));
        addAndMakeVisible(mgsc_preview_);

        status_.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(status_);
        performance_keyboard_.setCallbacks(
            [this](std::uint8_t note) {
                startPerformanceNote(note);
            },
            [this](std::uint8_t note) {
                stopPerformanceNote(note);
            });
        performance_keyboard_.setModeCallback(
            [this](bool polyphonic) {
                voice_allocator_.setPolyphonic(polyphonic);
            });
        performance_keyboard_.setSuppressPcInputCallback(
            [this] { return textEntryHasFocusWithin(*this); });
        addAndMakeVisible(performance_keyboard_);
        updateHistoryButtons();

        loadLibrary();
        setEditorBaseline();
        updateDefinitionPreview();
        loadBackgroundSettings();
        refreshPresetPreview(false);
        setSize(1360, 900);
        engine_ready_ = audio_service.running()
            && configureEngine(false);
        startTimerHz(60);
        updateStatus(
            engine_ready_
                ? juce::String::fromUTF8("準備完了")
                : juce::String::fromUTF8(
                    "音声出力を開始できませんでした"));
        static_cast<void>(consumePendingSccConversion());
    }

    void prepareVisualInspection() {
        preset_.setSelectedId(1, juce::dontSendNotification);
        refreshPresetPreview(false);
        updateStatus(
            juce::String::fromUTF8(
                "目視検査: 確定（緑）と候補（橙破線）を重ね表示"));
    }

    void refreshExternalState() {
        synchronizeMasterVolumeSlider(
            master_volume_, master_volume_revision_, audio_service_);
        // pending取込が試聴を始めた直後に configureEngine(false) すると
        // hardResetで音が消えるため、取込成功時は再構成しない。
        if (!consumePendingSccConversion() && engine_ready_) {
            static_cast<void>(configureEngine(false));
        }
    }

    void deactivate() {
        saveBackgroundSettings();
        performance_keyboard_.allNotesOff();
        silenceAllVoices();
    }

    ~SccEditorComponent() override {
        saveBackgroundSettings();
        stopTimer();
        performance_keyboard_.allNotesOff();
        merge_enabled_.setLookAndFeel(nullptr);
        auto_phase_.setLookAndFeel(nullptr);
        polarity_.setLookAndFeel(nullptr);
        preserve_volume_.setLookAndFeel(nullptr);
        preset_flip_h_.setLookAndFeel(nullptr);
        preset_flip_v_.setLookAndFeel(nullptr);
        favorite_.setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& graphics) override {
        graphics.fillAll(juce::Colour(0xFF20262E));
        auto content = getLocalBounds().reduced(20);
        content.removeFromBottom(106);
        auto library_panel = content.removeFromRight(268);
        content.removeFromRight(8);
        auto panel = content
            .withTrimmedTop(216)
            .withTrimmedBottom(64);
        graphics.setColour(juce::Colour(0xFF29323C));
        graphics.fillRoundedRectangle(panel.toFloat(), 9.0F);
        graphics.fillRoundedRectangle(
            library_panel.withTrimmedTop(74).toFloat(), 9.0F);
        graphics.setColour(juce::Colour(0xFF435160));
        graphics.drawRoundedRectangle(panel.toFloat(), 9.0F, 1.0F);
        graphics.drawRoundedRectangle(
            library_panel.withTrimmedTop(74).toFloat(), 9.0F, 1.0F);
    }

    void resized() override {
        auto area = getLocalBounds().reduced(24);
        title_.setBounds(area.removeFromTop(34));
        description_.setBounds(area.removeFromTop(28));
        settings_.setBounds(getWidth() - 58, 24, 34, 34);
        master_volume_.setBounds(getWidth() - 104, 21, 40, 40);
        immediate_audition_.setBounds(getWidth() - 146, 24, 34, 34);
        area.removeFromTop(12);

        auto keyboard_area = area.removeFromBottom(94);
        area.removeFromBottom(10);
        performance_keyboard_.setBounds(keyboard_area);
        auto library_area = area.removeFromRight(256);
        area.removeFromRight(16);

        auto file_row = area.removeFromTop(40);
        constexpr int icon_size = 40;
        load_.setBounds(file_row.removeFromLeft(icon_size));
        file_row.removeFromLeft(6);
        save_.setBounds(file_row.removeFromLeft(icon_size));
        file_row.removeFromLeft(6);
        paste_.setBounds(file_row.removeFromLeft(icon_size));
        file_row.removeFromLeft(6);
        copy_.setBounds(file_row.removeFromLeft(icon_size));
        file_row.removeFromLeft(6);
        undo_.setBounds(file_row.removeFromLeft(icon_size));
        file_row.removeFromLeft(6);
        redo_.setBounds(file_row.removeFromLeft(icon_size));
        file_row.removeFromLeft(14);
        import_wave_.setBounds(file_row.removeFromLeft(66));
        file_row.removeFromLeft(10);
        import_audacity_.setBounds(file_row.removeFromLeft(184));
        file_row.removeFromLeft(10);
        open_opll_.setBounds(file_row.removeFromLeft(120));
        file_row.removeFromLeft(8);
        convert_to_opll_.setBounds(file_row.removeFromLeft(40));
        area.removeFromTop(10);

        auto preset_row = area.removeFromTop(38);
        auto preset_bounds = preset_row.removeFromLeft(240);
        preset_.setBounds(preset_bounds);
        auto preview_bounds = preset_bounds;
        preview_bounds.removeFromRight(24);
        preset_preview_.setBounds(
            preview_bounds.removeFromRight(76).reduced(4, 4));
        preset_preview_.toFront(false);
        preset_row.removeFromLeft(8);
        harmonic_.setBounds(preset_row.removeFromLeft(230));
        preset_row.removeFromLeft(8);
        apply_range_.setBounds(preset_row.removeFromLeft(110));
        preset_row.removeFromLeft(8);
        apply_preset_.setBounds(preset_row.removeFromLeft(70));
        preset_row.removeFromLeft(6);
        cancel_preview_.setBounds(preset_row.removeFromLeft(56));
        preset_row.removeFromLeft(8);
        ab_audition_.setBounds(preset_row.removeFromLeft(52));
        preset_row.removeFromLeft(8);
        average_.setBounds(preset_row.removeFromLeft(42));
        preset_row.removeFromLeft(6);
        normalize_.setBounds(preset_row.removeFromLeft(42));
        preset_row.removeFromLeft(6);
        invert_.setBounds(preset_row.removeFromLeft(42));

        area.removeFromTop(8);
        auto merge_row = area.removeFromTop(38);
        merge_enabled_.setBounds(merge_row.removeFromLeft(176));
        merge_row.removeFromLeft(8);
        merge_amount_.setBounds(merge_row.removeFromLeft(210));
        merge_row.removeFromLeft(10);
        auto_phase_.setBounds(merge_row.removeFromLeft(104));
        polarity_.setBounds(merge_row.removeFromLeft(112));
        preserve_volume_.setBounds(merge_row.removeFromLeft(104));
        merge_row.removeFromLeft(12);
        preset_flip_h_.setBounds(merge_row.removeFromLeft(88));
        merge_row.removeFromLeft(6);
        preset_flip_v_.setBounds(merge_row.removeFromLeft(88));

        area.removeFromTop(14);
        auto editor_row = area.removeFromTop(
            juce::jmax(240, area.getHeight() - 196));
        graph_.setBounds(editor_row.withTrimmedRight(220));
        auto tool_area = editor_row.removeFromRight(210);
        auto cursor_area = tool_area.removeFromLeft(86);
        const auto button_size = 42;
        shift_up_.setBounds(
            cursor_area.getCentreX() - button_size / 2,
            cursor_area.getY() + 54,
            button_size,
            button_size);
        rotate_left_.setBounds(
            cursor_area.getX(),
            cursor_area.getY() + 102,
            button_size,
            button_size);
        rotate_right_.setBounds(
            cursor_area.getRight() - button_size,
            cursor_area.getY() + 102,
            button_size,
            button_size);
        shift_down_.setBounds(
            cursor_area.getCentreX() - button_size / 2,
            cursor_area.getY() + 150,
            button_size,
            button_size);
        auto scale_area = tool_area.removeFromLeft(62).reduced(5, 44);
        scale_reset_.setBounds(scale_area.removeFromBottom(34));
        scale_area.removeFromBottom(8);
        vertical_scale_.setBounds(scale_area);
        area.removeFromTop(8);
        auto background_tools = area.removeFromTop(30);
        background_load_.setBounds(
            background_tools.removeFromLeft(64));
        background_tools.removeFromLeft(4);
        background_clear_.setBounds(
            background_tools.removeFromLeft(64));
        background_tools.removeFromLeft(6);
        background_visible_.setBounds(
            background_tools.removeFromLeft(90));
        background_tools.removeFromLeft(6);
        background_opacity_label_.setBounds(
            background_tools.removeFromLeft(28));
        background_opacity_.setBounds(
            background_tools.removeFromLeft(140));
        area.removeFromTop(4);
        auto background_pos = area.removeFromTop(30);
        const auto pos_half =
            (background_pos.getWidth() - 12) / 2;
        auto x_area = background_pos.removeFromLeft(pos_half);
        background_x_label_.setBounds(x_area.removeFromLeft(18));
        background_x_.setBounds(x_area);
        background_pos.removeFromLeft(12);
        background_y_label_.setBounds(
            background_pos.removeFromLeft(18));
        background_y_.setBounds(background_pos);
        area.removeFromTop(4);
        auto background_size = area.removeFromTop(30);
        const auto size_half =
            (background_size.getWidth() - 12) / 2;
        auto w_area = background_size.removeFromLeft(size_half);
        background_w_label_.setBounds(w_area.removeFromLeft(18));
        background_width_.setBounds(w_area);
        background_size.removeFromLeft(12);
        background_h_label_.setBounds(
            background_size.removeFromLeft(18));
        background_height_.setBounds(background_size);
        area.removeFromTop(8);
        auto footer = area.removeFromTop(42);
        status_.setBounds(footer);

        library_title_.setBounds(
            library_area.removeFromTop(34));
        library_area.removeFromTop(8);
        library_filter_.setBounds(
            library_area.removeFromTop(32));
        library_area.removeFromTop(8);
        library_list_.setBounds(
            library_area.removeFromTop(34));
        library_area.removeFromTop(10);
        name_.setBounds(library_area.removeFromTop(34));
        library_area.removeFromTop(8);
        tags_.setBounds(library_area.removeFromTop(34));
        library_area.removeFromTop(8);
        memo_.setBounds(library_area.removeFromTop(82));
        library_area.removeFromTop(6);
        favorite_.setBounds(library_area.removeFromTop(30));
        library_area.removeFromTop(8);
        auto library_buttons = library_area.removeFromTop(34);
        library_new_.setBounds(
            library_buttons.removeFromLeft(54));
        library_buttons.removeFromLeft(6);
        library_load_.setBounds(
            library_buttons.removeFromLeft(54));
        library_buttons.removeFromLeft(6);
        library_save_.setBounds(
            library_buttons.removeFromLeft(54));
        library_buttons.removeFromLeft(6);
        library_save_as_.setBounds(library_buttons);
        library_area.removeFromTop(6);
        auto library_file_buttons = library_area.removeFromTop(34);
        library_cancel_.setBounds(
            library_file_buttons.removeFromLeft(54));
        library_file_buttons.removeFromLeft(6);
        library_delete_.setBounds(
            library_file_buttons.removeFromLeft(54));
        library_file_buttons.removeFromLeft(6);
        library_import_.setBounds(
            library_file_buttons.removeFromLeft(54));
        library_file_buttons.removeFromLeft(6);
        library_export_.setBounds(library_file_buttons);
        library_area.removeFromTop(10);
        mgsc_title_.setBounds(library_area.removeFromTop(26));
        auto number_row = library_area.removeFromTop(30);
        output_number_label_.setBounds(
            number_row.removeFromLeft(118));
        number_row.removeFromLeft(8);
        output_number_.setBounds(number_row.removeFromLeft(54));
        library_area.removeFromTop(8);
        mgsc_preview_.setBounds(library_area);
    }

    bool keyPressed(const juce::KeyPress& key) override {
        if (key == juce::KeyPress(
                'v', juce::ModifierKeys::commandModifier, 0)) {
            pasteClipboard();
            return true;
        }
        if (key == juce::KeyPress(
                'c', juce::ModifierKeys::commandModifier, 0)) {
            copyDefinition();
            return true;
        }
        if (key == juce::KeyPress(
                'z', juce::ModifierKeys::commandModifier, 0)) {
            undo();
            return true;
        }
        if (key == juce::KeyPress(
                'y', juce::ModifierKeys::commandModifier, 0)
            || key == juce::KeyPress(
                'z',
                juce::ModifierKeys::commandModifier
                    | juce::ModifierKeys::shiftModifier,
                0)) {
            redo();
            return true;
        }
        return performance_keyboard_.shouldConsumeKeyPress(key);
    }

private:
    void configureIconButton(
        juce::DrawableButton& button,
        EditorIcon icon,
        const juce::String& tooltip,
        std::function<void()> action) {
        const auto normal = makeEditorIcon(
            icon, juce::Colour(0xFFE6EDF3));
        const auto over = makeEditorIcon(
            icon, juce::Colour(0xFF53E3A6));
        const auto down = makeEditorIcon(
            icon, juce::Colour(0xFF2AD6C9));
        const auto disabled = makeEditorIcon(
            icon, juce::Colour(0xFF68737E));
        button.setImages(
            normal.get(),
            over.get(),
            down.get(),
            disabled.get());
        button.setTooltip(tooltip);
        button.setTitle(tooltip);
        button.setDescription(tooltip);
        button.onClick = std::move(action);
        addAndMakeVisible(button);
    }

    void configureButton(
        juce::TextButton& button,
        const juce::String& text,
        const juce::String& tooltip,
        std::function<void()> action) {
        button.setButtonText(text);
        button.setTooltip(tooltip);
        button.onClick = std::move(action);
        addAndMakeVisible(button);
    }

    void rebuildPresetMenu() {
        constexpr std::array<const char*, 6> names{
            "Sine", "Square", "Triangle", "Saw",
            "Pulse 25%", "Pulse 12.5%"};
        const int selected = juce::jlimit(1, 6, preset_.getSelectedId());
        auto* menu = preset_.getRootMenu();
        menu->clear();
        const auto harmonic = selectedHarmonic();
        for (std::size_t index = 0; index < names.size(); ++index) {
            const auto preset = static_cast<mgstc::engine::SccWavePreset>(index);
            auto item = std::make_unique<SccPresetMenuItem>(
                names[index],
                mgstc::engine::generateSccPreset(preset, harmonic));
            menu->addCustomItem(
                static_cast<int>(index) + 1,
                std::move(item), nullptr,
                juce::String(names[index]));
        }
        preset_.setSelectedId(selected, juce::dontSendNotification);
    }

    [[nodiscard]] mgstc::engine::SccWavePreset
    selectedPreset() const noexcept {
        const auto value = juce::jlimit(1, 6, preset_.getSelectedId());
        return static_cast<mgstc::engine::SccWavePreset>(value - 1);
    }

    [[nodiscard]] mgstc::engine::SccHarmonic
    selectedHarmonic() const noexcept {
        const auto value = juce::jlimit(
            0, 4, juce::roundToInt(harmonic_.getValue()));
        return static_cast<mgstc::engine::SccHarmonic>(value);
    }

    [[nodiscard]] mgstc::engine::SccApplyRange
    selectedApplyRange() const noexcept {
        switch (apply_range_.getSelectedId()) {
        case 2:
            return mgstc::engine::SccApplyRange::LeftHalf;
        case 3:
            return mgstc::engine::SccApplyRange::RightHalf;
        default:
            return mgstc::engine::SccApplyRange::All;
        }
    }

    [[nodiscard]] std::optional<std::uint8_t>
    outputNumber() const {
        const auto text = utf8Text(output_number_.getText().trim());
        if (text.empty()) {
            return std::nullopt;
        }
        int value{};
        const auto parsed = std::from_chars(
            text.data(), text.data() + text.size(), value, 10);
        if (parsed.ec != std::errc{}
            || parsed.ptr != text.data() + text.size()
            || value < 0
            || value > 31) {
            return std::nullopt;
        }
        return static_cast<std::uint8_t>(value);
    }

    [[nodiscard]] juce::String definitionText() const {
        const auto number = outputNumber();
        if (!number) {
            return {};
        }
        const auto text =
            mgstc::engine::formatMgsSccDefinition(
                scc_wave_, *number);
        return juce::String::fromUTF8(
            text.data(),
            static_cast<int>(text.size()));
    }

    void updateDefinitionPreview() {
        if (!outputNumber()) {
            mgsc_preview_.setText(
                juce::String::fromUTF8(
                    "一時出力番号は0～31で入力してください。"),
                false);
            return;
        }
        mgsc_preview_.setText(definitionText(), false);
    }

    void showError(
        const juce::String& title,
        const juce::String& message) {
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon,
            title,
            message);
    }


    void loadDefinition() {
        juce::FileChooser chooser(
            juce::String::fromUTF8("SCC音色を読み込む"),
            {},
            "*.mgs;*.txt;*.scc");
        if (!chooser.browseForFileToOpen()) {
            return;
        }
        const auto text = chooser.getResult().loadFileAsString();
        const auto utf8 = text.toRawUTF8();
        const auto parsed = mgstc::engine::parseMgsSccDefinition(
            std::string_view(
                utf8,
                static_cast<std::size_t>(text.getNumBytesAsUTF8())));
        if (!parsed) {
            showError(
                juce::String::fromUTF8("ファイル読込"),
                juce::String::fromUTF8(
                    "SCC音色定義を読み取れませんでした"));
            return;
        }
        output_number_.setText(
            juce::String(static_cast<int>(parsed->number)), false);
        commitWave(parsed->waveform);
        updateStatus(
            juce::String::fromUTF8("SCC音色を読み込みました"));
    }

    void saveDefinition() {
        const auto definition = definitionText();
        if (definition.isEmpty()) {
            showError(
                juce::String::fromUTF8("ファイル保存"),
                juce::String::fromUTF8(
                    "一時出力番号を0～31で入力してください"));
            output_number_.grabKeyboardFocus();
            return;
        }
        juce::FileChooser chooser(
            juce::String::fromUTF8("SCC音色を保存する"),
            juce::File::getCurrentWorkingDirectory()
                .getChildFile("scc-tone.mgs"),
            "*.mgs;*.txt");
        if (!chooser.browseForFileToSave(true)) {
            return;
        }
        if (!chooser.getResult().replaceWithText(
                definition, false, false, "\r\n")) {
            showError(
                juce::String::fromUTF8("ファイル保存"),
                juce::String::fromUTF8(
                    "SCC音色ファイルを保存できませんでした"));
            return;
        }
        updateStatus(
            juce::String::fromUTF8("SCC音色を保存しました"));
    }

    void copyDefinition() {
        const auto definition = definitionText();
        if (definition.isEmpty()) {
            showError(
                juce::String::fromUTF8("コピー"),
                juce::String::fromUTF8(
                    "一時出力番号を0～31で入力してください"));
            output_number_.grabKeyboardFocus();
            return;
        }
        if (!mgstc::platform::copyTextToClipboardUnicodeAndAnsi(
                std::wstring(definition.toWideCharPointer()))) {
            juce::SystemClipboard::copyTextToClipboard(definition);
        }
        updateStatus(
            juce::String::fromUTF8(
                "SCC音色定義をコピーしました"));
    }

    [[nodiscard]] bool consumePendingSccConversion() {
        const auto file = pendingConversionFile("scc");
        if (!file.existsAsFile()) {
            return false;
        }
        const auto text = utf8Text(file.loadFileAsString());
        const auto parsed =
            mgstc::engine::parseMgsSccDefinition(text);
        if (!parsed) {
            static_cast<void>(file.deleteFile());
            updateStatus(juce::String::fromUTF8(
                "受信したSCC近似波形を読み込めませんでした"));
            return false;
        }
        static_cast<void>(file.deleteFile());
        commitWave(parsed->waveform);
        updateStatus(juce::String::fromUTF8(
            "OPLLから近似変換したSCC波形を読み込みました"));
        return true;
    }

    bool importWaveBytes(
        const std::vector<std::uint8_t>& bytes,
        const juce::String& source_name) {
        mgstc::engine::WavePcm pcm;
        std::string error;
        if (!mgstc::engine::parseWavePcm(
                std::span<const std::uint8_t>(bytes),
                pcm,
                &error)) {
            showError(
                source_name,
                juce::String::fromUTF8(
                    "WAVを読み込めませんでした。"
                    "PCM 8/16/24/32bitまたは32bit floatの"
                    "WAVEを使用してください"));
            return false;
        }
        const auto analysis = mgstc::engine::analyzeWaveCycle(pcm);
        if (analysis.cycle.size() < 2) {
            showError(
                source_name,
                juce::String::fromUTF8(
                    "波形の1周期を抽出できませんでした"));
            return false;
        }
        commitWave(
            mgstc::engine::waveCycleToScc(analysis.cycle));
        updateStatus(
            source_name
            + juce::String::fromUTF8("完了: 約 ")
            + juce::String(
                analysis.estimated_frequency_hz, 1)
            + " Hz");
        return true;
    }

    void pasteClipboard() {
        if (const auto wave =
                mgstc::platform::clipboardWaveBytes()) {
            static_cast<void>(importWaveBytes(
                *wave,
                juce::String::fromUTF8("音声の貼り付け変換")));
            return;
        }

        if (const auto image_bytes =
                mgstc::platform::clipboardImageBytes()) {
            auto image = loadImageBytesForBackground(*image_bytes);
            if (image.isValid()) {
                background_path_.clear();
                background_image_ = std::move(image);
                background_visible_.setToggleState(
                    true, juce::dontSendNotification);
                if (background_width_.getValue() < 8.0
                    || background_height_.getValue() < 8.0) {
                    background_width_.setValue(
                        static_cast<double>(
                            background_image_.getWidth()),
                        juce::dontSendNotification);
                    background_height_.setValue(
                        static_cast<double>(
                            background_image_.getHeight()),
                        juce::dontSendNotification);
                }
                applyBackgroundToGraph();
                saveBackgroundSettings();
                updateStatus(
                    juce::String::fromUTF8(
                        "クリップボード画像を背景へ貼り付けました"));
                return;
            }
        }

        const auto text =
            juce::SystemClipboard::getTextFromClipboard();
        const auto utf8 = text.toRawUTF8();
        const auto parsed = mgstc::engine::parseMgsSccDefinition(
            std::string_view(
                utf8,
                static_cast<std::size_t>(text.getNumBytesAsUTF8())));
        if (!parsed) {
            showError(
                juce::String::fromUTF8("貼り付け"),
                juce::String::fromUTF8(
                    "クリップボードにWAV音声、画像、"
                    "またはSCC音色定義がありません"));
            return;
        }
        output_number_.setText(
            juce::String(static_cast<int>(parsed->number)), false);
        commitWave(parsed->waveform);
        updateStatus(
            juce::String::fromUTF8(
                "SCC音色定義を貼り付けました"));
    }

    void chooseBackgroundImage() {
        juce::FileChooser chooser(
            juce::String::fromUTF8("背景画像を開く"),
            juce::File(),
            "*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff");
        if (!chooser.browseForFileToOpen()) {
            return;
        }
        setBackgroundFromFile(chooser.getResult());
    }

    void setBackgroundFromFile(const juce::File& file) {
        auto image = loadImageFileForBackground(file);
        background_path_ = file.getFullPathName();
        if (!image.isValid()) {
            background_image_ = {};
            applyBackgroundToGraph();
            saveBackgroundSettings();
            updateStatus(
                juce::String::fromUTF8(
                    "背景画像を開けませんでした（パスは保持）"));
            return;
        }
        background_image_ = std::move(image);
        background_visible_.setToggleState(
            true, juce::dontSendNotification);
        background_width_.setValue(
            static_cast<double>(background_image_.getWidth()),
            juce::dontSendNotification);
        background_height_.setValue(
            static_cast<double>(background_image_.getHeight()),
            juce::dontSendNotification);
        applyBackgroundToGraph();
        saveBackgroundSettings();
        updateStatus(
            juce::String::fromUTF8("背景画像を読み込みました"));
    }

    void applyBackgroundToGraph() {
        graph_.setBackgroundImage(
            background_image_,
            background_visible_.getToggleState(),
            static_cast<float>(background_opacity_.getValue())
                / 100.0F,
            juce::roundToInt(background_x_.getValue()),
            juce::roundToInt(background_y_.getValue()),
            juce::roundToInt(background_width_.getValue()),
            juce::roundToInt(background_height_.getValue()));
    }

    void loadBackgroundSettings() {
        const auto file = settingsFile();
        background_path_.clear();
        background_image_ = {};
        if (!file.existsAsFile()) {
            applyBackgroundToGraph();
            return;
        }
        const auto path = file.getFullPathName();
        wchar_t path_buffer[1024]{};
        static_cast<void>(GetPrivateProfileStringW(
            L"SccBackground",
            L"Path",
            L"",
            path_buffer,
            static_cast<DWORD>(std::size(path_buffer)),
            path.toWideCharPointer()));
        background_path_ = juce::String(path_buffer);
        background_visible_.setToggleState(
            GetPrivateProfileIntW(
                L"SccBackground",
                L"Visible",
                1,
                path.toWideCharPointer())
                != 0,
            juce::dontSendNotification);
        background_opacity_.setValue(
            GetPrivateProfileIntW(
                L"SccBackground",
                L"Opacity",
                35,
                path.toWideCharPointer()),
            juce::dontSendNotification);
        background_x_.setValue(
            static_cast<int>(GetPrivateProfileIntW(
                L"SccBackground",
                L"X",
                0,
                path.toWideCharPointer())),
            juce::dontSendNotification);
        background_y_.setValue(
            static_cast<int>(GetPrivateProfileIntW(
                L"SccBackground",
                L"Y",
                0,
                path.toWideCharPointer())),
            juce::dontSendNotification);
        background_width_.setValue(
            GetPrivateProfileIntW(
                L"SccBackground",
                L"Width",
                320,
                path.toWideCharPointer()),
            juce::dontSendNotification);
        background_height_.setValue(
            GetPrivateProfileIntW(
                L"SccBackground",
                L"Height",
                160,
                path.toWideCharPointer()),
            juce::dontSendNotification);

        if (background_path_.isNotEmpty()) {
            background_image_ = loadImageFileForBackground(
                juce::File(background_path_));
            if (!background_image_.isValid()) {
                updateStatus(
                    juce::String::fromUTF8(
                        "背景画像ファイルが見つかりません"
                        "（設定は保持）"));
            }
        }
        applyBackgroundToGraph();
    }

    void saveBackgroundSettings() {
        const auto file = settingsFile();
        if (file.getParentDirectory()
                .createDirectory()
                .failed()) {
            return;
        }
        const auto path = file.getFullPathName();
        static_cast<void>(WritePrivateProfileStringW(
            L"SccBackground",
            L"SchemaVersion",
            L"1",
            path.toWideCharPointer()));
        static_cast<void>(WritePrivateProfileStringW(
            L"SccBackground",
            L"Path",
            background_path_.toWideCharPointer(),
            path.toWideCharPointer()));
        static_cast<void>(WritePrivateProfileStringW(
            L"SccBackground",
            L"Visible",
            background_visible_.getToggleState() ? L"1" : L"0",
            path.toWideCharPointer()));
        static_cast<void>(WritePrivateProfileStringW(
            L"SccBackground",
            L"Opacity",
            juce::String(
                juce::roundToInt(background_opacity_.getValue()))
                .toWideCharPointer(),
            path.toWideCharPointer()));
        static_cast<void>(WritePrivateProfileStringW(
            L"SccBackground",
            L"X",
            juce::String(
                juce::roundToInt(background_x_.getValue()))
                .toWideCharPointer(),
            path.toWideCharPointer()));
        static_cast<void>(WritePrivateProfileStringW(
            L"SccBackground",
            L"Y",
            juce::String(
                juce::roundToInt(background_y_.getValue()))
                .toWideCharPointer(),
            path.toWideCharPointer()));
        static_cast<void>(WritePrivateProfileStringW(
            L"SccBackground",
            L"Width",
            juce::String(
                juce::roundToInt(background_width_.getValue()))
                .toWideCharPointer(),
            path.toWideCharPointer()));
        static_cast<void>(WritePrivateProfileStringW(
            L"SccBackground",
            L"Height",
            juce::String(
                juce::roundToInt(background_height_.getValue()))
                .toWideCharPointer(),
            path.toWideCharPointer()));
        static_cast<void>(WritePrivateProfileStringW(
            nullptr, nullptr, nullptr, path.toWideCharPointer()));
    }

    void convertToOpll() {
        audio_service_.setSharedSccWaveform(scc_wave_);
        static_cast<void>(configureEngine(false));
        updateStatus(juce::String::fromUTF8(
            "SCC音色をOPLLへ近似変換しています…"));
        juce::Component::SafePointer<SccEditorComponent> safe(this);
        const auto waveform = scc_wave_;
        runWithConversionBusyDialog(
            this,
            [waveform] {
                return mgstc::engine::
                    approximateSccWaveformCandidatesWithOpll(waveform);
            },
            [safe](std::vector<mgstc::engine::OpllPatchParameters>
                       candidates) {
                if (safe == nullptr) {
                    return;
                }
                if (candidates.empty()) {
                    safe->updateStatus(juce::String::fromUTF8(
                        "OPLL近似候補を生成できませんでした"));
                    return;
                }
                const auto definition =
                    mgstc::engine::formatMgsOpllDefinition(
                        candidates.front(), 15);
                const auto file = pendingConversionFile("opll");
                if (!file.getParentDirectory().createDirectory()
                    || !file.replaceWithText(
                        juce::String::fromUTF8(definition.c_str()))) {
                    safe->updateStatus(juce::String::fromUTF8(
                        "OPLL変換データを保存できませんでした"));
                    return;
                }
                safe->open_editor_("opll");
                safe->updateStatus(juce::String::fromUTF8(
                    "OPLL近似音色を生成し、OPLLエディタへ送りました"));
            });
    }

    void importWaveFile() {
        juce::FileChooser chooser(
            juce::String::fromUTF8("WAVからSCC音色へ変換"),
            {},
            "*.wav;*.wave");
        if (!chooser.browseForFileToOpen()) {
            return;
        }
        const auto file = chooser.getResult();
        const auto wave = mgstc::platform::readWaveFileBytes(
            std::filesystem::path(
                file.getFullPathName().toWideCharPointer()));
        if (!wave) {
            showError(
                "WAV",
                juce::String::fromUTF8(
                    "WAVファイルを読み込めませんでした"));
            return;
        }
        static_cast<void>(importWaveBytes(*wave, "WAV"));
    }

    void importAudacitySelection() {
        juce::MouseCursor::showWaitCursor();
        auto imported =
            mgstc::platform::exportAudacitySelectionToWave();
        juce::MouseCursor::hideWaitCursor();
        if (!imported.wave) {
            showError(
                juce::String::fromUTF8("Audacityから変換"),
                juce::String(imported.error.c_str()));
            return;
        }
        static_cast<void>(
            importWaveBytes(
                *imported.wave,
                juce::String::fromUTF8("Audacity変換")));
    }

    [[nodiscard]] static std::string utf8Text(
        const juce::String& text) {
        const auto utf8 = text.toUTF8();
        return {
            utf8.getAddress(),
            static_cast<std::size_t>(utf8.sizeInBytes() - 1)};
    }

    [[nodiscard]] static std::int64_t unixTimeNow() {
        return static_cast<std::int64_t>(
            juce::Time::getCurrentTime().toMilliseconds() / 1000);
    }

    [[nodiscard]] juce::File libraryFile() const {
        return juce::File::getSpecialLocation(
                   juce::File::userApplicationDataDirectory)
            .getChildFile("MgsToneCraft")
            .getChildFile("timbre-library-v1.mgstc");
    }

    [[nodiscard]] juce::File settingsFile() const {
        return juce::File::getSpecialLocation(
                   juce::File::userApplicationDataDirectory)
            .getChildFile("MgsToneCraft")
            .getChildFile("settings-v1.ini");
    }

    [[nodiscard]] bool loadSccImmediateAuditionSetting() const {
        const auto file = settingsFile();
        if (!file.existsAsFile()) {
            return true;
        }
        return GetPrivateProfileIntW(
                   L"Application",
                   L"SccImmediateAudition",
                   1,
                   file.getFullPathName()
                       .toWideCharPointer())
            != 0;
    }

    void saveSccImmediateAuditionSetting(bool enabled) {
        saveApplicationSetting(
            L"SccImmediateAudition", enabled ? L"1" : L"0");
    }

    [[nodiscard]] std::uint8_t
    loadLastAuditionNoteSetting() const {
        const auto file = settingsFile();
        if (!file.existsAsFile()) {
            return kPreviewNote;
        }
        const int value = GetPrivateProfileIntW(
            L"Application",
            L"LastAuditionNote",
            kPreviewNote,
            file.getFullPathName().toWideCharPointer());
        return static_cast<std::uint8_t>(
            juce::jlimit(24, 119, value));
    }

    void saveLastAuditionNoteSetting(std::uint8_t midi_note) {
        const auto value = juce::String(
            static_cast<int>(midi_note));
        saveApplicationSetting(
            L"LastAuditionNote",
            value.toWideCharPointer());
    }

    void saveApplicationSetting(
        const wchar_t* key,
        const wchar_t* value) {
        const auto file = settingsFile();
        if (file.getParentDirectory()
                .createDirectory()
                .failed()) {
            return;
        }
        const auto path_text = file.getFullPathName();
        const auto path = path_text.toWideCharPointer();
        static_cast<void>(WritePrivateProfileStringW(
            L"Application", L"SchemaVersion", L"1", path));
        static_cast<void>(WritePrivateProfileStringW(
            L"Application", key, value, path));
        static_cast<void>(WritePrivateProfileStringW(
            nullptr, nullptr, nullptr, path));
    }

    void loadLibrary() {
        const auto file = libraryFile();
        if (file.existsAsFile()) {
            const auto text = file.loadFileAsString();
            const auto source = utf8Text(text);
            std::string error;
            if (auto loaded =
                    mgstc::engine::TimbreLibrary::deserialize(
                        source, &error)) {
                library_ = std::move(*loaded);
            } else {
                showError(
                    juce::String::fromUTF8("音色ライブラリ"),
                    juce::String::fromUTF8(
                        "既存の音色ライブラリを読み込めませんでした"));
            }
        }
        refreshLibraryList();
    }

    [[nodiscard]] bool persistLibrary() {
        const auto target = libraryFile();
        if (!target.getParentDirectory().createDirectory()) {
            return false;
        }
        juce::TemporaryFile temporary(target);
        const auto contents = library_.serialize();
        if (!temporary.getFile().replaceWithData(
                contents.data(), contents.size())) {
            return false;
        }
        return temporary.overwriteTargetFileWithTemporary();
    }

    [[nodiscard]] bool reloadLibraryFromDisk() {
        const auto file = libraryFile();
        if (!file.existsAsFile()) {
            library_ = {};
            return true;
        }
        const auto source =
            utf8Text(file.loadFileAsString());
        std::string error;
        auto loaded =
            mgstc::engine::TimbreLibrary::deserialize(
                source, &error);
        if (!loaded) {
            return false;
        }
        library_ = std::move(*loaded);
        return true;
    }

    void refreshLibraryList() {
        library_list_.clear(juce::dontSendNotification);
        library_ids_.clear();
        const auto filter = library_filter_.getText().trim();
        int item_id = 1;
        int selected_item = 0;
        for (const auto& entry : library_.entries()) {
            if (entry.category
                != mgstc::engine::TimbreCategory::Scc) {
                continue;
            }
            if (filter.isNotEmpty()) {
                const auto name =
                    juce::String::fromUTF8(entry.name.c_str());
                const auto tags =
                    juce::String::fromUTF8(entry.tags.c_str());
                const auto memo =
                    juce::String::fromUTF8(entry.memo.c_str());
                if (!name.containsIgnoreCase(filter)
                    && !tags.containsIgnoreCase(filter)
                    && !memo.containsIgnoreCase(filter)) {
                    continue;
                }
            }
            library_ids_.push_back(entry.id);
            auto label = juce::String::fromUTF8(entry.name.c_str())
                + "  r"
                + juce::String(static_cast<int>(entry.revision));
            if (entry.favorite) {
                label = juce::String::fromUTF8("★ ") + label;
            }
            library_list_.addItem(label, item_id);
            if (selected_library_id_
                && *selected_library_id_ == entry.id) {
                selected_item = item_id;
            }
            ++item_id;
        }
        library_list_.setSelectedId(
            selected_item, juce::dontSendNotification);
        if (selected_item == 0) {
            selected_library_id_.reset();
        }
    }

    [[nodiscard]] const mgstc::engine::TimbreLibraryEntry*
    selectedLibraryEntry() const {
        if (!selected_library_id_) {
            return nullptr;
        }
        return library_.find(*selected_library_id_);
    }

    void selectLibraryEntryFromList() {
        const auto selected = library_list_.getSelectedId();
        if (selected <= 0
            || static_cast<std::size_t>(selected)
                > library_ids_.size()) {
            selected_library_id_.reset();
            return;
        }
        selected_library_id_ =
            library_ids_[static_cast<std::size_t>(selected - 1)];
        const auto* entry = selectedLibraryEntry();
        if (!entry) {
            return;
        }
        name_.setText(
            juce::String::fromUTF8(entry->name.c_str()), false);
        tags_.setText(
            juce::String::fromUTF8(entry->tags.c_str()), false);
        memo_.setText(
            juce::String::fromUTF8(entry->memo.c_str()), false);
        favorite_.setToggleState(
            entry->favorite, juce::dontSendNotification);
        updateStatus(
            juce::String::fromUTF8("音色を選択しました。"
                                   "「読込」で波形へ反映します"));
    }

    void newLibraryEntry() {
        selected_library_id_.reset();
        library_list_.setSelectedId(0, juce::dontSendNotification);
        name_.clear();
        tags_.clear();
        memo_.clear();
        favorite_.setToggleState(false, juce::dontSendNotification);
        commitWave({});
        setEditorBaseline();
        updateStatus(
            juce::String::fromUTF8("新しいSCC音色を編集中"));
    }

    void loadSelectedLibraryEntry() {
        if (!selected_library_id_) {
            showError(
                juce::String::fromUTF8("音色ライブラリ"),
                juce::String::fromUTF8(
                    "読み込む音色を選択してください"));
            return;
        }
        const auto id = *selected_library_id_;
        juce::InterProcessLock::ScopedLockType lock(
            library_lock_);
        if (!lock.isLocked() || !reloadLibraryFromDisk()) {
            showError(
                juce::String::fromUTF8("音色ライブラリ"),
                juce::String::fromUTF8(
                    "共有ライブラリを読み直せません"));
            return;
        }
        const auto* entry = library_.find(id);
        if (!entry) {
            selected_library_id_.reset();
            refreshLibraryList();
            showError(
                juce::String::fromUTF8("音色ライブラリ"),
                juce::String::fromUTF8(
                    "選択音色は他の画面で削除されています"));
            return;
        }
        SccWaveform waveform{};
        std::transform(
            entry->scc_waveform.begin(),
            entry->scc_waveform.end(),
            waveform.begin(),
            [](std::uint8_t value) {
                return static_cast<std::int8_t>(value);
            });
        commitWave(waveform);
        refreshLibraryList();
        setEditorBaseline();
        updateStatus(
            juce::String::fromUTF8("ライブラリ音色を読み込みました"));
    }

    [[nodiscard]] mgstc::engine::TimbreLibraryEntry
    captureLibraryEntry() const {
        mgstc::engine::TimbreLibraryEntry entry;
        entry.category = mgstc::engine::TimbreCategory::Scc;
        entry.name = utf8Text(name_.getText().trim());
        entry.tags = utf8Text(tags_.getText().trim());
        entry.memo = utf8Text(memo_.getText());
        entry.favorite = favorite_.getToggleState();
        std::transform(
            scc_wave_.begin(),
            scc_wave_.end(),
            entry.scc_waveform.begin(),
            [](std::int8_t value) {
                return static_cast<std::uint8_t>(value);
            });
        return entry;
    }

    void saveLibraryEntry(bool save_as) {
        if (save_as || !selected_library_id_) {
            performSaveLibraryEntry(save_as);
            return;
        }
        const auto* current = selectedLibraryEntry();
        const auto name = current
            ? juce::String::fromUTF8(current->name.c_str())
            : juce::String::fromUTF8("選択中の音色");
        const auto revision = current ? current->revision : 1;
        const auto impact = inspectCompositeTimbreImpact(
            *selected_library_id_);
        juce::Component::SafePointer<SccEditorComponent> safe(this);
        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions()
                .withIconType(
                    juce::MessageBoxIconType::WarningIcon)
                .withTitle(
                    juce::String::fromUTF8("SCC音色の上書き"))
                .withMessage(
                    juce::String::fromUTF8("「")
                    + name
                    + juce::String::fromUTF8(
                        "」を上書きしてリビジョンを更新します。\n"
                        "保存済み総合音色の参照も同時に更新します。\n\n")
                    + describeCompositeTimbreImpact(impact)
                    + juce::String::fromUTF8("\n\n現在: r")
                    + juce::String(static_cast<int>(revision))
                    + juce::String::fromUTF8("  更新後: r")
                    + juce::String(static_cast<int>(revision + 1)))
                .withButton(
                    juce::String::fromUTF8("更新して上書き"))
                .withButton(
                    juce::String::fromUTF8("キャンセル"))
                .withAssociatedComponent(this),
            [safe](int result) {
                if (safe != nullptr && result == 1) {
                    safe->performSaveLibraryEntry(false);
                }
            });
    }

    void performSaveLibraryEntry(bool save_as) {
        auto entry = captureLibraryEntry();
        if (entry.name.empty()) {
            showError(
                juce::String::fromUTF8("音色ライブラリ"),
                juce::String::fromUTF8(
                    "保存する音色名を入力してください"));
            name_.grabKeyboardFocus();
            return;
        }
        juce::InterProcessLock::ScopedLockType lock(
            library_lock_);
        if (!lock.isLocked() || !reloadLibraryFromDisk()) {
            showError(
                juce::String::fromUTF8("音色ライブラリ"),
                juce::String::fromUTF8(
                    "共有ライブラリを更新できません"));
            return;
        }
        auto composite_library = loadCompositeTimbreLibrary();
        if (!save_as && selected_library_id_
            && !composite_library) {
            showError(
                juce::String::fromUTF8("音色ライブラリ"),
                juce::String::fromUTF8(
                    "総合音色ライブラリを読み込めないため、"
                    "安全に上書き更新できません"));
            return;
        }
        const auto original_library = library_;
        std::size_t updated_composite_references{};
        if (save_as || !selected_library_id_) {
            entry.name = library_.uniqueName(
                mgstc::engine::TimbreCategory::Scc, entry.name);
            selected_library_id_ =
                library_.add(std::move(entry), unixTimeNow());
        } else if (!library_.update(
                       *selected_library_id_,
                       entry,
                       unixTimeNow())) {
            showError(
                juce::String::fromUTF8("音色ライブラリ"),
                juce::String::fromUTF8(
                    "選択中の音色を更新できませんでした"));
            return;
        }
        if (!save_as && selected_library_id_
            && composite_library) {
            const auto* updated_entry =
                library_.find(*selected_library_id_);
            if (updated_entry) {
                updated_composite_references =
                    composite_library->updateTimbreReferences(
                        *updated_entry, unixTimeNow());
            }
        }
        const auto saved = persistLibrary();
        if (saved && updated_composite_references != 0
            && !persistCompositeTimbreLibrary(
                *composite_library)) {
            library_ = original_library;
            const bool rolled_back = persistLibrary();
            refreshLibraryList();
            showError(
                juce::String::fromUTF8("音色ライブラリ"),
                rolled_back
                    ? juce::String::fromUTF8(
                          "総合音色を更新できなかったため、"
                          "音色の上書きを取り消しました")
                    : juce::String::fromUTF8(
                          "総合音色の更新と音色の復元に失敗しました。"
                          "ライブラリファイルを確認してください"));
            return;
        }
        refreshLibraryList();
        selectLibraryEntryFromList();
        setEditorBaseline();
        if (!saved) {
            showError(
                juce::String::fromUTF8("音色ライブラリ"),
                juce::String::fromUTF8(
                    "音色はセッションへ保存されましたが、"
                    "ライブラリファイルを更新できませんでした"));
            return;
        }
        updateStatus(
            save_as
                ? juce::String::fromUTF8(
                    "新しい音色として保存しました")
                : juce::String::fromUTF8("音色を保存しました")
                    + (updated_composite_references == 0
                           ? juce::String{}
                           : juce::String::fromUTF8(
                                 "（総合音色の参照も更新）")));
    }

    void setEditorBaseline() {
        editor_baseline_ = captureLibraryEntry();
        editor_baseline_id_ = selected_library_id_;
        editor_baseline_valid_ = true;
    }

    void restoreEditorBaseline() {
        if (!editor_baseline_valid_) {
            return;
        }
        selected_library_id_ = editor_baseline_id_;
        name_.setText(
            juce::String::fromUTF8(editor_baseline_.name.c_str()), false);
        tags_.setText(
            juce::String::fromUTF8(editor_baseline_.tags.c_str()), false);
        memo_.setText(
            juce::String::fromUTF8(editor_baseline_.memo.c_str()), false);
        favorite_.setToggleState(
            editor_baseline_.favorite, juce::dontSendNotification);
        SccWaveform waveform{};
        std::transform(
            editor_baseline_.scc_waveform.begin(),
            editor_baseline_.scc_waveform.end(),
            waveform.begin(),
            [](std::uint8_t value) {
                return static_cast<std::int8_t>(value);
            });
        commitWave(waveform);
        refreshLibraryList();
        updateStatus(
            juce::String::fromUTF8("編集開始時点の音色へ戻しました"));
    }

    void deleteSelectedLibraryEntry() {
        const auto* entry = selectedLibraryEntry();
        if (!entry) {
            showError(
                juce::String::fromUTF8("音色ライブラリ"),
                juce::String::fromUTF8(
                    "削除する音色を一覧から選択してください"));
            return;
        }
        const auto id = entry->id;
        const auto name =
            juce::String::fromUTF8(entry->name.c_str());
        juce::Component::SafePointer<SccEditorComponent> safe(this);
        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions()
                .withIconType(
                    juce::MessageBoxIconType::WarningIcon)
                .withTitle(
                    juce::String::fromUTF8("音色ライブラリ"))
                .withMessage(
                    juce::String::fromUTF8("「")
                    + name
                    + juce::String::fromUTF8(
                        "」をライブラリから削除しますか？"))
                .withButton(juce::String::fromUTF8("削除"))
                .withButton(juce::String::fromUTF8("キャンセル"))
                .withAssociatedComponent(this),
            [safe, id](int result) {
                if (safe == nullptr || result != 1) {
                    return;
                }
                juce::InterProcessLock::ScopedLockType lock(
                    safe->library_lock_);
                if (!lock.isLocked()
                    || !safe->reloadLibraryFromDisk()
                    || !safe->library_.erase(id)
                    || !safe->persistLibrary()) {
                    safe->showError(
                        juce::String::fromUTF8(
                            "音色ライブラリ"),
                        juce::String::fromUTF8(
                            "削除結果を保存できませんでした"));
                    return;
                }
                safe->selected_library_id_.reset();
                safe->name_.clear();
                safe->tags_.clear();
                safe->memo_.clear();
                safe->favorite_.setToggleState(
                    false, juce::dontSendNotification);
                safe->refreshLibraryList();
                safe->setEditorBaseline();
                safe->updateStatus(
                    juce::String::fromUTF8(
                        "音色をライブラリから削除しました"));
            });
    }

    void importLibraryFile() {
        juce::FileChooser chooser(
            juce::String::fromUTF8("音色ライブラリを取り込む"),
            {},
            "*.mgstc;*.dat");
        if (!chooser.browseForFileToOpen()) {
            return;
        }
        const auto text = chooser.getResult().loadFileAsString();
        const auto source = utf8Text(text);
        juce::InterProcessLock::ScopedLockType lock(
            library_lock_);
        if (!lock.isLocked() || !reloadLibraryFromDisk()) {
            showError(
                juce::String::fromUTF8("ライブラリ取込"),
                juce::String::fromUTF8(
                    "共有ライブラリを更新できません"));
            return;
        }
        std::string error;
        const auto imported_ids = library_.importSerialized(
            source, unixTimeNow(), &error);
        if (!imported_ids) {
            showError(
                juce::String::fromUTF8("ライブラリ取込"),
                juce::String::fromUTF8(
                    "対応する音色ライブラリを読み込めませんでした"));
            return;
        }
        std::optional<std::uint64_t> first_scc;
        for (const auto id : *imported_ids) {
            const auto* imported = library_.find(id);
            if (imported
                && imported->category
                    == mgstc::engine::TimbreCategory::Scc) {
                first_scc = id;
                break;
            }
        }
        if (first_scc) {
            selected_library_id_ = first_scc;
        }
        refreshLibraryList();
        if (first_scc && selected_library_id_) {
            selectLibraryEntryFromList();
        }
        if (!persistLibrary()) {
            showError(
                juce::String::fromUTF8("ライブラリ取込"),
                juce::String::fromUTF8(
                    "音色は追加されましたが、ローカルファイルを更新できませんでした"));
            return;
        }
        updateStatus(
            juce::String(static_cast<int>(imported_ids->size()))
            + juce::String::fromUTF8(
                "件の音色を取り込みました（同名は自動改名）"));
    }

    void exportSelectedLibraryEntry() {
        if (!selected_library_id_) {
            showError(
                juce::String::fromUTF8("ライブラリ書出"),
                juce::String::fromUTF8(
                    "書き出す音色を一覧から選択してください"));
            return;
        }
        const auto id = *selected_library_id_;
        std::optional<std::string> contents;
        {
            juce::InterProcessLock::ScopedLockType lock(
                library_lock_);
            if (!lock.isLocked() || !reloadLibraryFromDisk()) {
                showError(
                    juce::String::fromUTF8("ライブラリ書出"),
                    juce::String::fromUTF8(
                        "共有ライブラリを読み直せません"));
                return;
            }
            contents = library_.serializeEntry(id);
            if (!contents) {
                selected_library_id_.reset();
                refreshLibraryList();
                showError(
                    juce::String::fromUTF8("ライブラリ書出"),
                    juce::String::fromUTF8(
                        "選択音色は他の画面で削除されています"));
                return;
            }
        }
        juce::FileChooser chooser(
            juce::String::fromUTF8("選択音色を書き出す"),
            juce::File::getCurrentWorkingDirectory()
                .getChildFile("scc-timbre.mgstc"),
            "*.mgstc");
        if (!chooser.browseForFileToSave(true)) {
            return;
        }
        if (!chooser.getResult().replaceWithData(
                contents->data(), contents->size())) {
            showError(
                juce::String::fromUTF8("ライブラリ書出"),
                juce::String::fromUTF8(
                    "音色ライブラリファイルを書き出せませんでした"));
            return;
        }
        updateStatus(
            juce::String::fromUTF8("選択音色を書き出しました"));
    }

    void applyPreset() {
        const auto candidate = makePresetCandidate();
        dismissPreviewState(false);
        commitWave(candidate);
    }

    [[nodiscard]] SccWaveform makePresetCandidate() const {
        auto generated = mgstc::engine::generateSccPreset(
            selectedPreset(), selectedHarmonic());
        if (preset_flip_h_.getToggleState()) {
            generated = mgstc::engine::mirrorSccWaveform(generated);
        }
        if (preset_flip_v_.getToggleState()) {
            generated = mgstc::engine::invertSccWaveform(generated);
        }
        if (!merge_enabled_.getToggleState()) {
            return mgstc::engine::applySccWaveformRange(
                scc_wave_, generated, selectedApplyRange());
        }
        const auto merged = mgstc::engine::mergeSccWaveforms(
            scc_wave_,
            generated,
            {
                .amount = merge_amount_.getValue() / 100.0,
                .auto_phase = auto_phase_.getToggleState(),
                .allow_polarity_inversion =
                    polarity_.getToggleState(),
                .preserve_volume =
                    preserve_volume_.getToggleState(),
            });
        return mgstc::engine::applySccWaveformRange(
            scc_wave_, merged.waveform, selectedApplyRange());
    }

    [[nodiscard]] SccWaveform makePresetPreviewWave() const {
        auto generated = mgstc::engine::generateSccPreset(
            selectedPreset(), selectedHarmonic());
        if (preset_flip_h_.getToggleState()) {
            generated = mgstc::engine::mirrorSccWaveform(generated);
        }
        if (preset_flip_v_.getToggleState()) {
            generated = mgstc::engine::invertSccWaveform(generated);
        }
        return generated;
    }

    void refreshPresetPreview(bool audition = true) {
        preview_wave_ = makePresetCandidate();
        preview_active_ = true;
        ab_next_plays_b_ = true;
        cancel_preview_.setEnabled(true);
        ab_audition_.setEnabled(true);
        updateAbAuditionButton();
        preset_preview_.setWaveform(makePresetPreviewWave());
        graph_.setPreview(scc_wave_, preview_wave_);
        if (audition && engine_ready_) {
            static_cast<void>(auditionAfterEdit(&preview_wave_));
        }
        updateStatus(
            merge_enabled_.getToggleState()
                ? juce::String::fromUTF8(
                    "マージ候補をプレビュー中（適用／取消）")
                : juce::String::fromUTF8(
                    "プリセット候補をプレビュー中（適用／取消）"));
    }

    void cancelPreview() {
        if (!preview_active_) {
            return;
        }
        dismissPreviewState(true);
        graph_.setWaveform(scc_wave_);
        updateDefinitionPreview();
        if (immediate_audition_.getToggleState()) {
            static_cast<void>(auditionAfterEdit());
        } else {
            stopNote();
            static_cast<void>(configureEngine(false));
        }
        updateStatus(
            juce::String::fromUTF8("プレビューを取り消しました"));
    }

    void auditionAbCompare() {
        if (!preview_active_ || !engine_ready_) {
            return;
        }
        const bool play_b = ab_next_plays_b_;
        const SccWaveform* wave =
            play_b ? &preview_wave_ : &scc_wave_;
        if (!auditionOneSecond(wave)) {
            updateStatus(
                juce::String::fromUTF8(
                    "A/B試聴を開始できませんでした"));
            return;
        }
        ab_next_plays_b_ = !play_b;
        updateAbAuditionButton();
        updateStatus(
            (play_b
                 ? juce::String::fromUTF8("B 候補を試聴中: ")
                 : juce::String::fromUTF8("A 原音を試聴中: "))
            + midiNoteName(last_audition_note_));
    }

    void dismissPreviewState(bool keep_graph_overlay_clear) {
        preview_active_ = false;
        ab_next_plays_b_ = true;
        cancel_preview_.setEnabled(false);
        ab_audition_.setEnabled(false);
        updateAbAuditionButton();
        if (keep_graph_overlay_clear) {
            graph_.clearPreviewOverlay();
        }
    }

    void updateAbAuditionButton() {
        const auto tip = !preview_active_
            ? juce::String::fromUTF8(
                  "候補があるとき、確定波形(A)と候補(B)を交互に試聴します")
            : (ab_next_plays_b_
                   ? juce::String::fromUTF8(
                         "次は B（候補）を試聴します")
                   : juce::String::fromUTF8(
                         "次は A（確定）を試聴します"));
        ab_audition_.setButtonText(
            !preview_active_
                ? "A/B"
                : (ab_next_plays_b_
                       ? juce::String::fromUTF8("▶B")
                       : juce::String::fromUTF8("▶A")));
        ab_audition_.setTooltip(tip);
        ab_audition_.setTitle(tip);
        ab_audition_.setDescription(tip);
    }

    void updateMergeControlState() {
        const auto enabled = merge_enabled_.getToggleState();
        merge_amount_.setEnabled(enabled);
        auto_phase_.setEnabled(enabled);
        polarity_.setEnabled(enabled);
        preserve_volume_.setEnabled(enabled);
    }

    void commitWave(const SccWaveform& waveform) {
        dismissPreviewState(false);
        scale_previewing_ = false;
        if (waveform == scc_wave_) {
            graph_.setWaveform(scc_wave_);
            updateDefinitionPreview();
            // 縦スケール100%でもドラッグ中の一時試聴を確定音色へ戻す。
            if (engine_ready_) {
                static_cast<void>(configureEngine(false));
            }
            return;
        }
        scc_wave_ = waveform;
        graph_.setWaveform(scc_wave_);
        updateDefinitionPreview();
        history_.erase(
            history_.begin()
                + static_cast<std::ptrdiff_t>(history_cursor_ + 1),
            history_.end());
        history_.push_back(scc_wave_);
        if (history_.size() > 257) {
            history_.erase(history_.begin());
        } else {
            ++history_cursor_;
        }
        updateHistoryButtons();
        const bool audition = immediate_audition_.getToggleState();
        const auto refreshed = configureEngine(audition);
        if (audition && refreshed) {
            armOneSecondPreview();
        }
        updateStatus(
            refreshed
                ? juce::String::fromUTF8("波形を更新しました")
                : juce::String::fromUTF8(
                    "音源への波形反映を待機できませんでした"));
    }

    void restoreHistory() {
        dismissPreviewState(false);
        scale_previewing_ = false;
        scc_wave_ = history_[history_cursor_];
        graph_.setWaveform(scc_wave_);
        updateDefinitionPreview();
        updateHistoryButtons();
        const bool audition = immediate_audition_.getToggleState();
        if (configureEngine(audition) && audition) {
            armOneSecondPreview();
        }
        updateStatus(
            juce::String::fromUTF8("履歴から波形を復元しました"));
    }

    void undo() {
        if (history_cursor_ == 0) {
            return;
        }
        --history_cursor_;
        restoreHistory();
    }

    void redo() {
        if (history_cursor_ + 1 >= history_.size()) {
            return;
        }
        ++history_cursor_;
        restoreHistory();
    }

    void updateHistoryButtons() {
        undo_.setEnabled(history_cursor_ > 0);
        redo_.setEnabled(history_cursor_ + 1 < history_.size());
    }

    void clearEngineVoices() {
        for (std::uint8_t channel = 0; channel < 5; ++channel) {
            static_cast<void>(engine_.submit(
                mgstc::engine::EngineCommand::noteOff(
                    static_cast<std::uint8_t>(
                        kSccTrack + channel))));
        }
        static_cast<void>(voice_allocator_.allNotesOff());
    }

    void silenceAllVoices() {
        audition_stop_time_ms_.reset();
        performance_keyboard_.clearPreviewNote();
        clearEngineVoices();
    }

    bool configureEngine(
        bool retrigger,
        const SccWaveform* preview = nullptr) {
        // hardReset前にアロケータと実発音を揃え、Poly残留を防ぐ。
        clearEngineVoices();
        engine_holds_temporary_program_ = (preview != nullptr);
        const auto& waveform = preview ? *preview : scc_wave_;
        if (preview == nullptr) {
            audio_service_.setSharedSccWaveform(scc_wave_);
        }
        return audio_service_.submitSharedEditorProgram(
            waveform,
            audio_service_.sharedOpllPatch(),
            retrigger,
            kSccTrack,
            last_audition_note_);
    }

    void startPerformanceNote(std::uint8_t note) {
        audition_stop_time_ms_.reset();
        performance_keyboard_.clearPreviewNote();
        last_audition_note_ = note;
        saveLastAuditionNoteSetting(last_audition_note_);
        if (!engine_ready_) {
            updateStatus(
                juce::String::fromUTF8(
                    "鍵盤演奏を開始できませんでした"));
            return;
        }
        // 一時試聴が残っていると鍵盤が未確定音色を鳴らすため、確定へ戻す。
        if (engine_holds_temporary_program_
            && !configureEngine(false)) {
            updateStatus(
                juce::String::fromUTF8(
                    "鍵盤演奏を開始できませんでした"));
            return;
        }
        const auto assignment = voice_allocator_.noteOn(note);
        const auto track = static_cast<std::uint8_t>(
            kSccTrack + assignment.channel);
        if (assignment.stolen_note) {
            static_cast<void>(engine_.submit(
                mgstc::engine::EngineCommand::noteOff(track)));
        }
        if (!engine_.submit(
                mgstc::engine::EngineCommand::noteOn(
                    track, note))) {
            static_cast<void>(voice_allocator_.noteOff(note));
            updateStatus(
                juce::String::fromUTF8(
                    "鍵盤演奏を開始できませんでした"));
            return;
        }
        updateStatus(
            juce::String::fromUTF8("鍵盤演奏中: SCC / ")
            + midiNoteName(note));
    }

    void stopPerformanceNote(std::uint8_t note) {
        audition_stop_time_ms_.reset();
        const auto channel = voice_allocator_.noteOff(note);
        if (!channel) {
            return;
        }
        static_cast<void>(engine_.submit(
            mgstc::engine::EngineCommand::noteOff(
                static_cast<std::uint8_t>(kSccTrack + *channel))));
        if (voice_allocator_.activeVoiceCount() == 0) {
            updateStatus(
                juce::String::fromUTF8("鍵盤演奏を停止しました"));
        }
    }

    void stopNote() {
        silenceAllVoices();
        if (engine_holds_temporary_program_) {
            static_cast<void>(configureEngine(false));
        }
        updateStatus(juce::String::fromUTF8("発音を停止しました"));
    }

    [[nodiscard]] bool auditionOneSecond(
        const SccWaveform* preview = nullptr) {
        if (!engine_ready_ || !configureEngine(true, preview)) {
            return false;
        }
        armOneSecondPreview();
        return true;
    }

    [[nodiscard]] bool auditionAfterEdit(
        const SccWaveform* preview = nullptr) {
        if (immediate_audition_.getToggleState()) {
            return auditionOneSecond(preview);
        }
        // 即時発声OFFでも共有プログラムへ確定波形を反映し、
        // 他エディタの音色を消さない。
        return preview != nullptr || configureEngine(false);
    }

    void armOneSecondPreview() {
        performance_keyboard_.showPreviewNote(last_audition_note_);
        audition_stop_time_ms_ =
            juce::Time::getMillisecondCounterHiRes() + 1000.0;
    }

    void timerCallback() override {
        synchronizeMasterVolumeSlider(
            master_volume_, master_volume_revision_, audio_service_);
        if (++settings_poll_ticks_ >= 30) {
            settings_poll_ticks_ = 0;
            immediate_audition_.setToggleState(
                loadSccImmediateAuditionSetting(),
                juce::dontSendNotification);
            const auto shared_note =
                loadLastAuditionNoteSetting();
            if (!performance_keyboard_.hasActiveNote()
                && shared_note != last_audition_note_) {
                last_audition_note_ = shared_note;
            }
        }
        if (audition_stop_time_ms_
            && juce::Time::getMillisecondCounterHiRes()
                >= *audition_stop_time_ms_) {
            silenceAllVoices();
            if (engine_holds_temporary_program_) {
                static_cast<void>(configureEngine(false));
            }
            updateStatus(
                juce::String::fromUTF8(
                    "1秒試聴が完了しました"));
        }
    }

    void updateStatus(const juce::String& text) {
        status_.setText(text, juce::dontSendNotification);
    }

    EditorOpenCallback open_editor_;
    SharedAudioService& audio_service_;
    mgstc::engine::RealtimeEngineHost& engine_;
    juce::TooltipWindow tooltip_window_;
    SwitchLookAndFeel switch_look_and_feel_;
    SccWaveform scc_wave_{};
    std::vector<SccWaveform> history_;
    std::size_t history_cursor_{};
    juce::Label title_;
    juce::Label description_;
    juce::DrawableButton load_{
        "load", juce::DrawableButton::ImageOnButtonBackground};
    juce::DrawableButton save_{
        "save", juce::DrawableButton::ImageOnButtonBackground};
    juce::DrawableButton paste_{
        "paste", juce::DrawableButton::ImageOnButtonBackground};
    juce::DrawableButton copy_{
        "copy", juce::DrawableButton::ImageOnButtonBackground};
    juce::TextButton import_wave_;
    juce::TextButton import_audacity_;
    juce::TextButton open_opll_;
    juce::DrawableButton convert_to_opll_{
        "convert-to-opll",
        juce::DrawableButton::ImageOnButtonBackground};
    juce::ComboBox preset_;
    PresetWavePreview preset_preview_;
    juce::Slider harmonic_;
    juce::ComboBox apply_range_;
    juce::ToggleButton merge_enabled_;
    juce::Slider merge_amount_;
    juce::ToggleButton auto_phase_;
    juce::ToggleButton polarity_;
    juce::ToggleButton preserve_volume_;
    juce::ToggleButton preset_flip_h_;
    juce::ToggleButton preset_flip_v_;
    juce::TextButton apply_preset_;
    juce::TextButton cancel_preview_;
    juce::DrawableButton undo_{
        "undo", juce::DrawableButton::ImageOnButtonBackground};
    juce::DrawableButton redo_{
        "redo", juce::DrawableButton::ImageOnButtonBackground};
    juce::TextButton ab_audition_;
    juce::DrawableButton average_{
        "average", juce::DrawableButton::ImageOnButtonBackground};
    juce::DrawableButton normalize_{
        "normalize", juce::DrawableButton::ImageOnButtonBackground};
    juce::DrawableButton invert_{
        "invert", juce::DrawableButton::ImageOnButtonBackground};
    juce::DrawableButton shift_up_{
        "shift up", juce::DrawableButton::ImageOnButtonBackground};
    juce::DrawableButton rotate_left_{
        "phase left", juce::DrawableButton::ImageOnButtonBackground};
    juce::DrawableButton rotate_right_{
        "phase right", juce::DrawableButton::ImageOnButtonBackground};
    juce::DrawableButton shift_down_{
        "shift down", juce::DrawableButton::ImageOnButtonBackground};
    juce::Slider vertical_scale_;
    juce::TextButton scale_reset_;
    juce::TextButton background_load_;
    juce::TextButton background_clear_;
    juce::ToggleButton background_visible_;
    juce::Label background_opacity_label_;
    juce::Slider background_opacity_;
    juce::Label background_x_label_;
    juce::Slider background_x_;
    juce::Label background_y_label_;
    juce::Slider background_y_;
    juce::Label background_w_label_;
    juce::Slider background_width_;
    juce::Label background_h_label_;
    juce::Slider background_height_;
    juce::DrawableButton immediate_audition_{
        "immediate audition",
        juce::DrawableButton::ImageOnButtonBackground};
    SccWaveGraph graph_;
    juce::Label library_title_;
    juce::ComboBox library_list_;
    juce::TextEditor library_filter_;
    juce::TextEditor name_;
    juce::TextEditor tags_;
    juce::TextEditor memo_;
    juce::ToggleButton favorite_;
    juce::TextButton library_new_;
    juce::TextButton library_load_;
    juce::TextButton library_save_;
    juce::TextButton library_save_as_;
    juce::TextButton library_cancel_;
    juce::TextButton library_delete_;
    juce::TextButton library_import_;
    juce::TextButton library_export_;
    juce::Label mgsc_title_;
    juce::Label output_number_label_;
    juce::TextEditor output_number_;
    juce::TextEditor mgsc_preview_;
    juce::Label status_;
    juce::DrawableButton settings_{
        "settings", juce::DrawableButton::ImageOnButtonBackground};
    juce::Slider master_volume_;
    PerformanceKeyboard performance_keyboard_;
    mgstc::engine::SequentialVoiceAllocator voice_allocator_{5};
    SccWaveform scale_source_{};
    SccWaveform preview_wave_{};
    juce::Image background_image_;
    juce::String background_path_;
    mgstc::engine::TimbreLibrary library_;
    std::vector<std::uint64_t> library_ids_;
    std::optional<std::uint64_t> selected_library_id_;
    mgstc::engine::TimbreLibraryEntry editor_baseline_;
    std::optional<std::uint64_t> editor_baseline_id_;
    juce::InterProcessLock library_lock_{
        "MgsToneCraftTimbreLibraryV1"};
    bool editor_baseline_valid_{};
    bool preview_active_{};
    bool ab_next_plays_b_{true};
    bool scale_previewing_{};
    bool engine_ready_{};
    bool engine_holds_temporary_program_{};
    std::uint8_t last_audition_note_{kPreviewNote};
    int settings_poll_ticks_{};
    std::uint64_t master_volume_revision_{};
    std::optional<double> audition_stop_time_ms_;
};

class OpllScopeComponent final : public juce::Component {
public:
    static constexpr std::size_t kHistorySampleCount = 4096;

    void setSynchronized(bool synchronized) {
        synchronized_ = synchronized;
        if (synchronized_) {
            rebuildSynchronizedDisplay();
        }
        repaint();
    }

    void appendFrame(
        const mgstc::engine::OpllScopeFrame& frame,
        std::uint8_t midi_note) {
        latest_ = frame.samples;
        latest_available_ = true;
        if (display_note_ != midi_note) {
            history_size_ = 0;
            history_write_ = 0;
            synchronized_available_ = false;
            display_note_ = midi_note;
        }
        for (const float sample : frame.samples) {
            history_[history_write_] = sample;
            history_write_ =
                (history_write_ + 1) % history_.size();
            history_size_ = std::min(
                history_size_ + 1, history_.size());
        }
        rebuildSynchronizedDisplay();
        repaint();
    }

    [[nodiscard]] bool synchronizedDisplayAvailable() const {
        return synchronized_available_;
    }

    void paint(juce::Graphics& graphics) override {
        const auto graph =
            getLocalBounds().toFloat().reduced(0.5F);
        graphics.setColour(juce::Colour(0xFF111920));
        graphics.fillRoundedRectangle(graph, 6.0F);
        graphics.setColour(juce::Colour(0xFF435160));
        graphics.drawRoundedRectangle(graph, 6.0F, 1.0F);

        graphics.setColour(juce::Colour(0xFF34434E));
        const float middle = graph.getCentreY();
        graphics.drawHorizontalLine(
            juce::roundToInt(middle),
            graph.getX() + 1.0F,
            graph.getRight() - 1.0F);
        for (int division = 1; division < 6; ++division) {
            const float x = graph.getX()
                + graph.getWidth()
                    * static_cast<float>(division) / 6.0F;
            graphics.drawVerticalLine(
                juce::roundToInt(x),
                graph.getY() + 1.0F,
                graph.getBottom() - 1.0F);
        }

        const bool available = synchronized_
            ? synchronized_available_
            : latest_available_;
        if (!available) {
            graphics.setColour(juce::Colour(0xFF9BA8B2));
            graphics.drawFittedText(
                synchronized_
                    ? juce::String::fromUTF8(
                          "OPLLを発声すると2周期へ同期した実波形を表示します")
                    : juce::String::fromUTF8(
                          "OPLLを発声すると800サンプルの実波形を表示します"),
                getLocalBounds().reduced(12),
                juce::Justification::centred,
                1);
            return;
        }

        const auto& samples =
            synchronized_ ? synchronized_display_ : latest_;
        float peak = 0.0F;
        for (const float sample : samples) {
            peak = std::max(peak, std::abs(sample));
        }
        const float gain =
            peak > 0.0001F ? 0.9F / peak : 1.0F;
        juce::Path waveform;
        for (std::size_t index = 0;
             index < samples.size();
             ++index) {
            const float x = juce::jmap(
                static_cast<float>(index),
                0.0F,
                static_cast<float>(samples.size() - 1),
                graph.getX() + 2.0F,
                graph.getRight() - 2.0F);
            const float value = juce::jlimit(
                -1.0F, 1.0F, samples[index] * gain);
            const float y = middle
                - value * (graph.getHeight() - 6.0F) * 0.5F;
            if (index == 0) {
                waveform.startNewSubPath(x, y);
            } else {
                waveform.lineTo(x, y);
            }
        }
        graphics.setColour(juce::Colour(0xFF4DD6B2));
        graphics.strokePath(
            waveform,
            juce::PathStrokeType(
                1.6F,
                juce::PathStrokeType::curved,
                juce::PathStrokeType::rounded));

        graphics.setColour(juce::Colour(0xFF9BA8B2));
        graphics.setFont(juce::FontOptions(11.0F));
        graphics.drawText(
            juce::String::fromUTF8("表示専用 自動スケール"),
            getLocalBounds().reduced(8).removeFromTop(18),
            juce::Justification::topRight);
    }

private:
    [[nodiscard]] float sampleHistory(double position) const {
        if (history_size_ == 0) {
            return 0.0F;
        }
        position = std::clamp(
            position,
            0.0,
            static_cast<double>(history_size_ - 1));
        const auto left = static_cast<std::size_t>(position);
        const auto right =
            std::min(left + 1, history_size_ - 1);
        const std::size_t oldest =
            (history_write_ + history_.size() - history_size_)
            % history_.size();
        const auto sample = [&](std::size_t index) {
            return history_[
                (oldest + index) % history_.size()];
        };
        const float fraction = static_cast<float>(
            position - static_cast<double>(left));
        return sample(left)
            + (sample(right) - sample(left)) * fraction;
    }

    void sampleWindow(
        double start,
        double sample_span,
        std::array<
            float,
            mgstc::engine::OpllScopeFrame::kSampleCount>& output)
        const {
        const double step = sample_span
            / static_cast<double>(output.size() - 1);
        for (std::size_t index = 0;
             index < output.size();
             ++index) {
            output[index] = sampleHistory(
                start + static_cast<double>(index) * step);
        }
    }

    [[nodiscard]] static double correlation(
        const std::array<
            float,
            mgstc::engine::OpllScopeFrame::kSampleCount>& left,
        const std::array<
            float,
            mgstc::engine::OpllScopeFrame::kSampleCount>& right) {
        double left_mean = 0.0;
        double right_mean = 0.0;
        for (std::size_t index = 0;
             index < left.size();
             ++index) {
            left_mean += left[index];
            right_mean += right[index];
        }
        left_mean /= static_cast<double>(left.size());
        right_mean /= static_cast<double>(right.size());
        double dot = 0.0;
        double left_energy = 0.0;
        double right_energy = 0.0;
        for (std::size_t index = 0;
             index < left.size();
             ++index) {
            const double a =
                static_cast<double>(left[index]) - left_mean;
            const double b =
                static_cast<double>(right[index]) - right_mean;
            dot += a * b;
            left_energy += a * a;
            right_energy += b * b;
        }
        const double scale =
            std::sqrt(left_energy * right_energy);
        return scale > 1.0e-12 ? dot / scale : -1.0;
    }

    void rebuildSynchronizedDisplay() {
        constexpr double sample_rate = 48000.0;
        const double frequency = 440.0 * std::pow(
            2.0,
            (static_cast<double>(display_note_) - 69.0) / 12.0);
        const double period = sample_rate / frequency;
        const double sample_span = period * 2.0;
        if (history_size_
            < static_cast<std::size_t>(
                  std::ceil(sample_span))
                + 2) {
            synchronized_available_ = false;
            return;
        }

        const double maximum_start =
            static_cast<double>(history_size_ - 1)
            - sample_span;
        const std::size_t search_first =
            static_cast<std::size_t>(
                std::max(1.0, maximum_start - period * 2.5));
        const std::size_t search_last =
            static_cast<std::size_t>(
                std::floor(maximum_start));
        std::vector<double> candidates;
        for (std::size_t index = search_first;
             index <= search_last;
             ++index) {
            const float previous = sampleHistory(
                static_cast<double>(index - 1));
            const float current = sampleHistory(
                static_cast<double>(index));
            if (previous <= 0.0F && current > 0.0F) {
                const double denominator =
                    static_cast<double>(current - previous);
                const double fraction =
                    denominator > 1.0e-12
                    ? -static_cast<double>(previous)
                        / denominator
                    : 0.0;
                candidates.push_back(
                    static_cast<double>(index - 1)
                    + fraction);
            }
        }
        if (candidates.empty()) {
            candidates.push_back(maximum_start);
        }

        const bool compare_previous =
            synchronized_available_;
        double best_start = candidates.back();
        double best_score =
            -std::numeric_limits<double>::infinity();
        std::array<
            float,
            mgstc::engine::OpllScopeFrame::kSampleCount>
            candidate{};
        for (const double start : candidates) {
            sampleWindow(start, sample_span, candidate);
            const double similarity = compare_previous
                ? correlation(
                      candidate, synchronized_display_)
                : 0.0;
            const double recency = maximum_start > 0.0
                ? start / maximum_start
                : 0.0;
            const double score =
                similarity + recency * 1.0e-4;
            if (score > best_score) {
                best_score = score;
                best_start = start;
            }
        }
        sampleWindow(
            best_start,
            sample_span,
            synchronized_display_);
        synchronized_available_ = true;
    }

    std::array<float, kHistorySampleCount> history_{};
    std::array<
        float,
        mgstc::engine::OpllScopeFrame::kSampleCount>
        latest_{};
    std::array<
        float,
        mgstc::engine::OpllScopeFrame::kSampleCount>
        synchronized_display_{};
    std::size_t history_size_{};
    std::size_t history_write_{};
    std::uint8_t display_note_{kPreviewNote};
    bool synchronized_{true};
    bool latest_available_{};
    bool synchronized_available_{};
};

enum class OpllEnvelopeParameter {
    AttackRate,
    DecayRate,
    SustainLevel,
    ReleaseRate,
};

class OpllEnvelopeGraph final
    : public juce::Component,
      public juce::SettableTooltipClient {
public:
    explicit OpllEnvelopeGraph(juce::Colour waveform_colour)
        : waveform_colour_(waveform_colour) {}

    void setTrace(
        const std::array<
            float,
            mgstc::engine::OpllEnvelopeTrace::kPointCount>& levels,
        bool valid) {
        levels_ = levels;
        valid_ = valid;
        repaint();
    }

    std::function<void(
        OpllEnvelopeParameter,
        std::uint8_t,
        bool)> onEdit;
    std::function<std::uint8_t(OpllEnvelopeParameter)> getValue;

    void paint(juce::Graphics& graphics) override {
        const auto graph =
            getLocalBounds().toFloat().reduced(0.5F);
        graphics.setColour(juce::Colour(0xFF111920));
        graphics.fillRoundedRectangle(graph, 5.0F);
        graphics.setColour(juce::Colour(0xFF435160));
        graphics.drawRoundedRectangle(graph, 5.0F, 1.0F);

        graphics.setColour(juce::Colour(0xFF34434E));
        for (int section = 1; section < 4; ++section) {
            const float x = graph.getX()
                + graph.getWidth()
                    * static_cast<float>(section) / 4.0F;
            graphics.drawVerticalLine(
                juce::roundToInt(x),
                graph.getY() + 1.0F,
                graph.getBottom() - 1.0F);
        }
        graphics.drawHorizontalLine(
            juce::roundToInt(graph.getCentreY()),
            graph.getX() + 1.0F,
            graph.getRight() - 1.0F);

        const float key_off_x = graph.getX()
            + graph.getWidth()
                * mgstc::engine::OpllEnvelopeTrace::
                      kKeyOffSeconds
                / mgstc::engine::OpllEnvelopeTrace::
                      kDurationSeconds;
        const float dash_pattern[]{4.0F, 3.0F};
        graphics.setColour(juce::Colour(0xFFD66B6B));
        graphics.drawDashedLine(
            {key_off_x,
             graph.getY() + 1.0F,
             key_off_x,
             graph.getBottom() - 1.0F},
            dash_pattern,
            2,
            1.0F);

        if (valid_) {
            juce::Path envelope;
            for (std::size_t index = 0;
                 index < levels_.size();
                 ++index) {
                const float x = juce::jmap(
                    static_cast<float>(index),
                    0.0F,
                    static_cast<float>(levels_.size() - 1),
                    graph.getX() + 2.0F,
                    graph.getRight() - 2.0F);
                const float level = juce::jlimit(
                    0.0F, 1.0F, levels_[index]);
                const float y = juce::jmap(
                    level,
                    0.0F,
                    1.0F,
                    graph.getBottom() - 3.0F,
                    graph.getY() + 3.0F);
                if (index == 0) {
                    envelope.startNewSubPath(x, y);
                } else {
                    envelope.lineTo(x, y);
                }
            }
            graphics.setColour(waveform_colour_);
            graphics.strokePath(
                envelope,
                juce::PathStrokeType(
                    1.7F,
                    juce::PathStrokeType::curved,
                    juce::PathStrokeType::rounded));
        }

        graphics.setFont(juce::FontOptions(10.5F));
        graphics.setColour(juce::Colour(0xFF9BA8B2));
        graphics.drawText(
            "0s",
            getLocalBounds().reduced(5).removeFromBottom(15),
            juce::Justification::bottomLeft);
        graphics.drawText(
            "4s",
            getLocalBounds().reduced(5).removeFromBottom(15),
            juce::Justification::bottomRight);
        graphics.setColour(juce::Colour(0xFFD98A8A));
        graphics.drawText(
            "KO 1s",
            juce::Rectangle<int>(
                juce::roundToInt(key_off_x) + 4,
                getHeight() - 19,
                42,
                14),
            juce::Justification::centredLeft);

        constexpr std::array<const char*, 4> section_names{
            "A / AR", "D / DR", "S / SL", "R / RR"};
        const float section_width = graph.getWidth() / 4.0F;
        graphics.setFont(juce::FontOptions(10.5F, juce::Font::bold));
        for (std::size_t index = 0;
             index < section_names.size();
             ++index) {
            const auto section = juce::Rectangle<float>(
                graph.getX()
                    + section_width * static_cast<float>(index),
                graph.getY(),
                section_width,
                18.0F);
            if (drag_parameter_
                && parameterIndex(*drag_parameter_) == index) {
                graphics.setColour(
                    waveform_colour_.withAlpha(0.18F));
                graphics.fillRect(
                    juce::Rectangle<float>(
                        section.getX(),
                        graph.getY() + 1.0F,
                        section.getWidth(),
                        graph.getHeight() - 2.0F));
            }
            graphics.setColour(juce::Colour(0xFFC6D0D8));
            graphics.drawText(
                section_names[index],
                section.toNearestInt(),
                juce::Justification::centred);
        }
    }

    void mouseDown(const juce::MouseEvent& event) override {
        drag_parameter_ = parameterAt(event.position);
        drag_changed_ = false;
        updateFromMouse(event.position);
        repaint();
    }

    void mouseDrag(const juce::MouseEvent& event) override {
        updateFromMouse(event.position);
    }

    void mouseUp(const juce::MouseEvent& event) override {
        updateFromMouse(event.position);
        if (drag_parameter_ && drag_changed_ && onEdit) {
            onEdit(*drag_parameter_, drag_value_, true);
        }
        drag_parameter_.reset();
        drag_changed_ = false;
        repaint();
    }

    void mouseMove(const juce::MouseEvent& event) override {
        setMouseCursor(
            parameterAt(event.position)
                    == OpllEnvelopeParameter::SustainLevel
                ? juce::MouseCursor::UpDownResizeCursor
                : juce::MouseCursor::LeftRightResizeCursor);
    }

    void mouseExit(const juce::MouseEvent&) override {
        if (!drag_parameter_) {
            setMouseCursor(juce::MouseCursor::NormalCursor);
        }
    }

    void mouseWheelMove(
        const juce::MouseEvent& event,
        const juce::MouseWheelDetails& wheel) override {
        if (drag_parameter_) {
            return;
        }
        const auto parameter = parameterAt(event.position);
        if (!getValue || !onEdit) {
            return;
        }
        const float primary =
            std::abs(wheel.deltaY) >= std::abs(wheel.deltaX)
                ? wheel.deltaY
                : wheel.deltaX;
        if (std::abs(primary) < 1.0e-4F) {
            return;
        }
        const int step = primary > 0.0F ? 1 : -1;
        const auto current = getValue(parameter);
        const auto next = static_cast<std::uint8_t>(juce::jlimit(
            0,
            15,
            static_cast<int>(current) + step));
        if (next == current) {
            return;
        }
        onEdit(parameter, next, true);
        repaint();
    }

private:
    [[nodiscard]] static std::size_t parameterIndex(
        OpllEnvelopeParameter parameter) {
        return static_cast<std::size_t>(parameter);
    }

    [[nodiscard]] OpllEnvelopeParameter parameterAt(
        juce::Point<float> position) const {
        const auto graph =
            getLocalBounds().toFloat().reduced(0.5F);
        const float normalized = juce::jlimit(
            0.0F,
            0.9999F,
            (position.x - graph.getX())
                / juce::jmax(1.0F, graph.getWidth()));
        return static_cast<OpllEnvelopeParameter>(
            juce::jlimit(
                0,
                3,
                static_cast<int>(normalized * 4.0F)));
    }

    [[nodiscard]] std::uint8_t valueAt(
        OpllEnvelopeParameter parameter,
        juce::Point<float> position) const {
        const auto graph =
            getLocalBounds().toFloat().reduced(0.5F);
        if (parameter == OpllEnvelopeParameter::SustainLevel) {
            const float normalized = juce::jlimit(
                0.0F,
                1.0F,
                (position.y - graph.getY())
                    / juce::jmax(1.0F, graph.getHeight()));
            return static_cast<std::uint8_t>(
                juce::roundToInt(normalized * 15.0F));
        }

        const auto index =
            static_cast<float>(parameterIndex(parameter));
        const float section_width = graph.getWidth() / 4.0F;
        const float section_x =
            graph.getX() + section_width * index;
        float normalized = juce::jlimit(
            0.0F,
            1.0F,
            (position.x - section_x)
                / juce::jmax(1.0F, section_width));
        if (parameter != OpllEnvelopeParameter::ReleaseRate) {
            normalized = 1.0F - normalized;
        }
        return static_cast<std::uint8_t>(
            juce::roundToInt(normalized * 15.0F));
    }

    void updateFromMouse(juce::Point<float> position) {
        if (!drag_parameter_) {
            return;
        }
        const auto value = valueAt(*drag_parameter_, position);
        if (drag_changed_ && value == drag_value_) {
            return;
        }
        drag_value_ = value;
        drag_changed_ = true;
        if (onEdit) {
            onEdit(*drag_parameter_, value, false);
        }
    }

    std::array<
        float,
        mgstc::engine::OpllEnvelopeTrace::kPointCount>
        levels_{};
    juce::Colour waveform_colour_;
    std::optional<OpllEnvelopeParameter> drag_parameter_;
    std::uint8_t drag_value_{};
    bool drag_changed_{};
    bool valid_{};
};

class OpllOperatorPanel final : public juce::Component {
public:
    OpllOperatorPanel(
        const juce::String& title,
        bool modulator,
        SwitchLookAndFeel& switch_look_and_feel)
        : modulator_(modulator),
          switch_look_and_feel_(switch_look_and_feel),
          envelope_graph_(
              modulator
                  ? juce::Colour(0xFF64A7FF)
                  : juce::Colour(0xFFFFA75E)) {
        title_.setText(title, juce::dontSendNotification);
        title_.setFont(
            juce::FontOptions(18.0F, juce::Font::bold));
        addAndMakeVisible(title_);

        constexpr std::array<const char*, 5> flag_names{
            "AM", "PM", "EG", "KR", "WS"};
        for (std::size_t index = 0; index < flags_.size(); ++index) {
            flags_[index].setButtonText(flag_names[index]);
            flags_[index].setLookAndFeel(&switch_look_and_feel_);
            flags_[index].onClick = [this] {
                notifyChanged(true);
            };
            addAndMakeVisible(flags_[index]);
        }

        configureSlider(multiplier_, "MULT", 0.0, 15.0);
        configureSlider(key_scale_level_, "KSL", 0.0, 3.0);
        configureSlider(attack_rate_, "AR (A)", 0.0, 15.0);
        configureSlider(decay_rate_, "DR (D)", 0.0, 15.0);
        configureSlider(sustain_level_, "SL (S)", 0.0, 15.0);
        configureSlider(release_rate_, "RR (R)", 0.0, 15.0);
        envelope_title_.setText(
            modulator_
                ? juce::String::fromUTF8(
                      "MOD 実効EG  C4／4秒")
                : juce::String::fromUTF8(
                      "CAR 実効EG  C4／4秒"),
            juce::dontSendNotification);
        envelope_title_.setFont(
            juce::FontOptions(13.0F, juce::Font::bold));
        addAndMakeVisible(envelope_title_);
        envelope_graph_.setTooltip(
            juce::String::fromUTF8(
                "AR / DR / SL / RR の区間をドラッグまたは"
                "マウスホイールで編集します。"
                "AR・DRは右ほど遅く、RRは右ほど速く、"
                "SLは下ほど減衰量が大きくなります。"
                "赤い線は1秒地点のキーオフです"));
        envelope_graph_.onEdit =
            [this](
                OpllEnvelopeParameter parameter,
                std::uint8_t value,
                bool commit) {
                sliderFor(parameter).setValue(
                    value, juce::dontSendNotification);
                if (onEnvelopeEdit) {
                    onEnvelopeEdit(parameter, value, commit);
                }
            };
        envelope_graph_.getValue =
            [this](OpllEnvelopeParameter parameter) {
                return static_cast<std::uint8_t>(juce::jlimit(
                    0.0,
                    15.0,
                    sliderFor(parameter).getValue()));
            };
        addAndMakeVisible(envelope_graph_);
    }

    ~OpllOperatorPanel() override {
        for (auto& flag : flags_) {
            flag.setLookAndFeel(nullptr);
        }
    }

    void setParameters(
        const mgstc::engine::OpllOperatorParameters& parameters) {
        syncing_ = true;
        flags_[0].setToggleState(
            parameters.amplitude_modulation,
            juce::dontSendNotification);
        flags_[1].setToggleState(
            parameters.pitch_modulation,
            juce::dontSendNotification);
        flags_[2].setToggleState(
            parameters.sustained_tone,
            juce::dontSendNotification);
        flags_[3].setToggleState(
            parameters.key_rate_scaling,
            juce::dontSendNotification);
        flags_[4].setToggleState(
            parameters.waveform,
            juce::dontSendNotification);
        multiplier_.setValue(
            parameters.multiplier, juce::dontSendNotification);
        key_scale_level_.setValue(
            parameters.key_scale_level, juce::dontSendNotification);
        attack_rate_.setValue(
            parameters.attack_rate, juce::dontSendNotification);
        decay_rate_.setValue(
            parameters.decay_rate, juce::dontSendNotification);
        sustain_level_.setValue(
            parameters.sustain_level, juce::dontSendNotification);
        release_rate_.setValue(
            parameters.release_rate, juce::dontSendNotification);
        syncing_ = false;
    }

    void setEnvelopeTrace(
        const std::array<
            float,
            mgstc::engine::OpllEnvelopeTrace::kPointCount>& levels,
        bool valid) {
        envelope_graph_.setTrace(levels, valid);
    }

    void setAuditionNote(std::uint8_t midi_note) {
        envelope_title_.setText(
            (modulator_ ? "MOD" : "CAR")
                + juce::String::fromUTF8(" 実効EG  ")
                + midiNoteName(midi_note)
                + juce::String::fromUTF8("／4秒"),
            juce::dontSendNotification);
    }

    [[nodiscard]] mgstc::engine::OpllOperatorParameters
    parameters() const {
        mgstc::engine::OpllOperatorParameters result;
        result.amplitude_modulation = flags_[0].getToggleState();
        result.pitch_modulation = flags_[1].getToggleState();
        result.sustained_tone = flags_[2].getToggleState();
        result.key_rate_scaling = flags_[3].getToggleState();
        result.waveform = flags_[4].getToggleState();
        result.multiplier =
            static_cast<std::uint8_t>(multiplier_.getValue());
        result.key_scale_level =
            static_cast<std::uint8_t>(key_scale_level_.getValue());
        result.total_level = 0;
        result.attack_rate =
            static_cast<std::uint8_t>(attack_rate_.getValue());
        result.decay_rate =
            static_cast<std::uint8_t>(decay_rate_.getValue());
        result.sustain_level =
            static_cast<std::uint8_t>(sustain_level_.getValue());
        result.release_rate =
            static_cast<std::uint8_t>(release_rate_.getValue());
        return result;
    }

    void paint(juce::Graphics& graphics) override {
        const auto bounds =
            getLocalBounds().toFloat().reduced(0.5F);
        graphics.setColour(juce::Colour(0xFF29323C));
        graphics.fillRoundedRectangle(bounds, 9.0F);
        graphics.setColour(juce::Colour(0xFF435160));
        graphics.drawRoundedRectangle(bounds, 9.0F, 1.0F);
    }

    void resized() override {
        auto area = getLocalBounds().reduced(14);
        title_.setBounds(area.removeFromTop(28));
        area.removeFromTop(6);
        auto flag_area = area.removeFromTop(30);
        const auto flag_width =
            juce::jmax(54, flag_area.getWidth() / 5);
        for (auto& flag : flags_) {
            flag.setBounds(
                flag_area.removeFromLeft(flag_width));
        }
        area.removeFromTop(10);
        auto basic_row = area.removeFromTop(36);
        const auto basic_width = (basic_row.getWidth() - 8) / 2;
        auto multiplier_area = basic_row.removeFromLeft(basic_width);
        multiplier_label_.setBounds(
            multiplier_area.removeFromLeft(48));
        multiplier_.setBounds(multiplier_area);
        basic_row.removeFromLeft(8);
        key_scale_level_label_.setBounds(
            basic_row.removeFromLeft(42));
        key_scale_level_.setBounds(basic_row);
        area.removeFromTop(10);

        auto graph_area = area.removeFromBottom(112);
        area.removeFromBottom(8);
        auto envelope = area;
        constexpr int gap = 8;
        const auto column_width =
            (envelope.getWidth() - gap * 3) / 4;
        std::array<juce::Label*, 4> labels{
            &attack_rate_label_,
            &decay_rate_label_,
            &sustain_level_label_,
            &release_rate_label_};
        std::array<juce::Slider*, 4> sliders{
            &attack_rate_,
            &decay_rate_,
            &sustain_level_,
            &release_rate_};
        for (std::size_t index = 0; index < sliders.size(); ++index) {
            auto column = envelope.removeFromLeft(column_width);
            labels[index]->setBounds(column.removeFromTop(24));
            sliders[index]->setBounds(column);
            if (index + 1 < sliders.size()) {
                envelope.removeFromLeft(gap);
            }
        }
        envelope_title_.setBounds(
            graph_area.removeFromTop(20));
        graph_area.removeFromTop(3);
        envelope_graph_.setBounds(graph_area);
    }

    std::function<void(bool commit)> onChange;
    std::function<void(
        OpllEnvelopeParameter,
        std::uint8_t,
        bool)> onEnvelopeEdit;

private:
    [[nodiscard]] juce::Slider& sliderFor(
        OpllEnvelopeParameter parameter) {
        switch (parameter) {
        case OpllEnvelopeParameter::AttackRate:
            return attack_rate_;
        case OpllEnvelopeParameter::DecayRate:
            return decay_rate_;
        case OpllEnvelopeParameter::SustainLevel:
            return sustain_level_;
        case OpllEnvelopeParameter::ReleaseRate:
            return release_rate_;
        }
        jassertfalse;
        return attack_rate_;
    }

    void configureSlider(
        juce::Slider& slider,
        const juce::String& label_text,
        double minimum,
        double maximum) {
        auto* label = labelFor(slider);
        label->setText(label_text, juce::dontSendNotification);
        label->setJustificationType(
            juce::Justification::centred);
        addAndMakeVisible(*label);
        slider.setRange(minimum, maximum, 1.0);
        slider.setSliderStyle(
            &slider == &attack_rate_
                || &slider == &decay_rate_
                || &slider == &sustain_level_
                || &slider == &release_rate_
                ? juce::Slider::LinearVertical
                : juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle(
            slider.getSliderStyle() == juce::Slider::LinearVertical
                ? juce::Slider::TextBoxBelow
                : juce::Slider::TextBoxRight,
            false,
            46,
            24);
        slider.setScrollWheelEnabled(true);
        slider.onValueChange = [this, &slider] {
            const bool dragging =
                slider.isMouseButtonDown()
                || slider.getThumbBeingDragged() >= 0;
            notifyChanged(!dragging);
        };
        slider.onDragEnd = [this] {
            notifyChanged(true);
        };
        addAndMakeVisible(slider);
    }

    [[nodiscard]] juce::Label* labelFor(juce::Slider& slider) {
        if (&slider == &multiplier_) {
            return &multiplier_label_;
        }
        if (&slider == &key_scale_level_) {
            return &key_scale_level_label_;
        }
        if (&slider == &attack_rate_) {
            return &attack_rate_label_;
        }
        if (&slider == &decay_rate_) {
            return &decay_rate_label_;
        }
        if (&slider == &sustain_level_) {
            return &sustain_level_label_;
        }
        return &release_rate_label_;
    }

    void notifyChanged(bool commit = true) {
        if (!syncing_ && onChange) {
            onChange(commit);
        }
    }

    bool modulator_{};
    bool syncing_{};
    SwitchLookAndFeel& switch_look_and_feel_;
    juce::Label title_;
    std::array<juce::ToggleButton, 5> flags_;
    juce::Slider multiplier_;
    juce::Slider key_scale_level_;
    juce::Slider attack_rate_;
    juce::Slider decay_rate_;
    juce::Slider sustain_level_;
    juce::Slider release_rate_;
    juce::Label multiplier_label_;
    juce::Label key_scale_level_label_;
    juce::Label attack_rate_label_;
    juce::Label decay_rate_label_;
    juce::Label sustain_level_label_;
    juce::Label release_rate_label_;
    juce::Label envelope_title_;
    OpllEnvelopeGraph envelope_graph_;
};

class OpllEditorComponent final
    : public juce::Component,
      private juce::Timer {
public:
    explicit OpllEditorComponent(
        SharedAudioService& audio_service,
        SharedMidiInputService& midi_service,
        EditorOpenCallback open_editor)
        : open_editor_(std::move(open_editor)),
          audio_service_(audio_service),
          engine_(audio_service.engine()),
          tooltip_window_(this, 450),
          modulator_(
              "MODULATOR", true, switch_look_and_feel_),
          carrier_(
              "CARRIER", false, switch_look_and_feel_),
          performance_keyboard_(midi_service, audio_service_) {
        setWantsKeyboardFocus(true);
        patch_ = mgstc::engine::defaultOpllPatch();
        history_.push_back(patch_);
        last_audition_note_ = loadLastAuditionNoteSetting();

        title_.setText(
            juce::String::fromUTF8(
                "OPLL音色エディタ"),
            juce::dontSendNotification);
        title_.setFont(
            juce::FontOptions(23.0F, juce::Font::bold));
        addAndMakeVisible(title_);
        configureSettingsButton(
            settings_, [this] {
                performance_keyboard_.showSettingsDialog();
            });
        addAndMakeVisible(settings_);
        configureMasterVolumeSlider(
            master_volume_, audio_service_, this);
        master_volume_revision_ =
            audio_service_.masterVolumeRevision();
        addAndMakeVisible(master_volume_);
        configureImmediateAuditionButton(
            immediate_audition_,
            loadOpllImmediateAuditionSetting(),
            [this] {
                saveOpllImmediateAuditionSetting(
                    immediate_audition_.getToggleState());
                updateStatus(
                    immediate_audition_.getToggleState()
                        ? juce::String::fromUTF8(
                              "OPLLの即時発音をONにしました")
                        : juce::String::fromUTF8(
                              "OPLLの即時発音をOFFにしました"));
            });
        addAndMakeVisible(immediate_audition_);
        description_.setText(
            juce::String::fromUTF8(
                "YM2413オリジナル音色の意味パラメーターを編集します。"
                "値の変更ごとに1秒試聴します。"),
            juce::dontSendNotification);
        addAndMakeVisible(description_);

        configureIconButton(
            load_,
            EditorIcon::Open,
            juce::String::fromUTF8("ファイル読込"),
            [this] { loadDefinition(); });
        configureIconButton(
            save_,
            EditorIcon::Save,
            juce::String::fromUTF8("ファイル保存"),
            [this] { saveDefinition(); });
        configureIconButton(
            paste_,
            EditorIcon::Paste,
            juce::String::fromUTF8(
                "貼り付け（音声、WAVファイル、OPLL定義）"),
            [this] { pasteClipboard(); });
        configureIconButton(
            copy_,
            EditorIcon::Copy,
            juce::String::fromUTF8("OPLL定義をコピー"),
            [this] { copyDefinition(); });
        configureIconButton(
            undo_,
            EditorIcon::Undo,
            juce::String::fromUTF8("Undo"),
            [this] { undo(); });
        configureIconButton(
            redo_,
            EditorIcon::Redo,
            juce::String::fromUTF8("Redo"),
            [this] { redo(); });
        configureButton(
            import_wave_,
            "WAV",
            juce::String::fromUTF8("WAVファイルからOPLL近似変換"),
            [this] { importWaveFile(); });
        configureButton(
            import_audacity_,
            juce::String::fromUTF8("Audacityから変換"),
            juce::String::fromUTF8(
                "Audacityの選択範囲からOPLL近似変換します。"
                "事前にmod-script-pipeを有効にしてください。"
                "有効中は同じPC上の他のプログラムからも"
                "Audacityを操作できるため、信頼できる環境で"
                "使用してください"),
            [this] { importAudacitySelection(); });
        configureButton(
            wave_previous_,
            juce::String::fromUTF8("◀"),
            juce::String::fromUTF8("前のWAV近似候補"),
            [this] { selectAdjacentWaveCandidate(-1); });
        configureButton(
            wave_next_,
            juce::String::fromUTF8("▶"),
            juce::String::fromUTF8("次のWAV近似候補"),
            [this] { selectAdjacentWaveCandidate(1); });
        wave_candidate_label_.setText(
            juce::String::fromUTF8("WAV候補 --/--"),
            juce::dontSendNotification);
        wave_candidate_label_.setJustificationType(
            juce::Justification::centred);
        addAndMakeVisible(wave_candidate_label_);
        updateWaveCandidateControls();

        addAndMakeVisible(modulator_);
        addAndMakeVisible(carrier_);
        common_parameters_title_.setText(
            juce::String::fromUTF8("モジュレーター制御"),
            juce::dontSendNotification);
        common_parameters_title_.setFont(
            juce::FontOptions(14.0F, juce::Font::bold));
        addAndMakeVisible(common_parameters_title_);
        const auto configure_common_slider = [this](
            juce::Label& label,
            juce::Slider& slider,
            const juce::String& text,
            double maximum) {
            label.setText(text, juce::dontSendNotification);
            label.setJustificationType(juce::Justification::centredRight);
            addAndMakeVisible(label);
            slider.setRange(0.0, maximum, 1.0);
            slider.setSliderStyle(juce::Slider::LinearHorizontal);
            slider.setTextBoxStyle(
                juce::Slider::TextBoxRight, false, 44, 24);
            slider.setScrollWheelEnabled(true);
            slider.onValueChange = [this, &slider] {
                const bool dragging =
                    slider.isMouseButtonDown()
                    || slider.getThumbBeingDragged() >= 0;
                controlsChanged(!dragging);
            };
            slider.onDragEnd = [this] {
                controlsChanged(true);
            };
            addAndMakeVisible(slider);
        };
        configure_common_slider(
            mod_total_level_label_, mod_total_level_, "TL", 63.0);
        mod_total_level_.setTooltip(
            juce::String::fromUTF8(
                "モジュレーターの出力レベル 0～63"));
        configure_common_slider(
            feedback_label_, feedback_, "Feedback", 7.0);
        feedback_.setTooltip(
            juce::String::fromUTF8(
                "モジュレーターの自己帰還量 0～7"));
        modulator_.onChange = [this](bool commit) {
            controlsChanged(commit);
        };
        carrier_.onChange = [this](bool commit) {
            controlsChanged(commit);
        };
        modulator_.onEnvelopeEdit =
            [this](
                OpllEnvelopeParameter,
                std::uint8_t,
                bool commit) {
                envelopeGraphChanged(commit);
            };
        carrier_.onEnvelopeEdit =
            [this](
                OpllEnvelopeParameter,
                std::uint8_t,
                bool commit) {
                envelopeGraphChanged(commit);
            };

        constexpr std::array<const char*, 15> names{
            "@0  Violin",
            "@1  Guitar",
            "@2  Piano",
            "@3  Flute",
            "@4  Clarinet",
            "@5  Oboe",
            "@6  Trumpet",
            "@7  Organ",
            "@8  Horn",
            "@9  Synthesizer",
            "@10 Harpsichord",
            "@11 Vibraphone",
            "@12 Synthesizer Bass",
            "@13 Acoustic Bass",
            "@14 Electric Guitar"};
        for (int index = 0; index < 15; ++index) {
            rom_preset_.addItem(names[static_cast<std::size_t>(index)],
                                index + 1);
        }
        rom_preset_.setSelectedId(1, juce::dontSendNotification);
        rom_preset_.setTooltip(
            juce::String::fromUTF8(
                "選択変更だけで固定音色を1秒試聴します"));
        rom_preset_.onChange = [this] {
            if (const auto preset = selectedRomPatch()) {
                static_cast<void>(
                    auditionOneSecond(&*preset));
                updateStatus(
                    juce::String::fromUTF8(
                        "固定音色を試聴中（編集音色は未変更）"));
            }
        };
        addAndMakeVisible(rom_preset_);

        rom_load_.setButtonText(
            juce::String::fromUTF8("編集音色へ読込"));
        rom_load_.setTooltip(
            juce::String::fromUTF8(
                "選択中の固定音色を編集可能な音色へコピーします"));
        rom_load_.onClick = [this] {
            if (const auto preset = selectedRomPatch()) {
                commitPatch(*preset);
                updateStatus(
                    juce::String::fromUTF8(
                        "固定音色を編集音色へ読み込みました"));
            }
        };
        addAndMakeVisible(rom_load_);

        updateAuditionNoteLabels();
        open_scc_.setButtonText(
            juce::String::fromUTF8("SCCを開く"));
        open_scc_.setTooltip(
            juce::String::fromUTF8(
                "SCC音色エディタを別ウィンドウで開きます"));
        open_scc_.onClick = [this] { open_editor_("scc"); };
        addAndMakeVisible(open_scc_);
        configureIconButton(
            convert_to_scc_, EditorIcon::Convert,
            juce::String::fromUTF8(
                "現在のOPLL音色をSCC波形として近似変換"),
            [this] { showOpllToSccDialog(); });

        scope_title_.setText(
            juce::String::fromUTF8(
                "emu2413 OPLL実波形 — 2周期／自動スケール"),
            juce::dontSendNotification);
        scope_title_.setFont(
            juce::FontOptions(15.0F, juce::Font::bold));
        addAndMakeVisible(scope_title_);
        scope_.setSynchronized(true);
        addAndMakeVisible(scope_);

        output_number_label_.setText(
            juce::String::fromUTF8("一時出力番号"),
            juce::dontSendNotification);
        addAndMakeVisible(output_number_label_);
        output_number_.setInputRestrictions(2, "0123456789");
        output_number_.setText("16", false);
        output_number_.setTooltip(
            juce::String::fromUTF8(
                "OPLLオリジナル音色のMGSC出力番号 15～31"));
        output_number_.onTextChange =
            [this] { updateDefinitionPreview(); };
        addAndMakeVisible(output_number_);

        mgsc_title_.setText(
            juce::String::fromUTF8(
                "MGSC @v 定義プレビュー"),
            juce::dontSendNotification);
        mgsc_title_.setFont(
            juce::FontOptions(17.0F, juce::Font::bold));
        addAndMakeVisible(mgsc_title_);
        mgsc_preview_.setMultiLine(true);
        mgsc_preview_.setReadOnly(true);
        mgsc_preview_.setFont(
            juce::FontOptions(13.0F, juce::Font::plain));
        addAndMakeVisible(mgsc_preview_);

        library_title_.setText(
            juce::String::fromUTF8("OPLL音色ライブラリ"),
            juce::dontSendNotification);
        library_title_.setFont(
            juce::FontOptions(17.0F, juce::Font::bold));
        addAndMakeVisible(library_title_);
        library_filter_.setTextToShowWhenEmpty(
            juce::String::fromUTF8("名前・タグ・メモを検索"),
            juce::Colour(0xFF7F8993));
        library_filter_.onTextChange =
            [this] { refreshLibraryList(); };
        addAndMakeVisible(library_filter_);
        library_list_.setTextWhenNothingSelected(
            juce::String::fromUTF8("保存済み音色を選択"));
        library_list_.onChange =
            [this] { selectLibraryEntryFromList(); };
        addAndMakeVisible(library_list_);
        name_.setTextToShowWhenEmpty(
            juce::String::fromUTF8("音色名"),
            juce::Colour(0xFF7F8993));
        tags_.setTextToShowWhenEmpty(
            juce::String::fromUTF8("タグ"),
            juce::Colour(0xFF7F8993));
        memo_.setTextToShowWhenEmpty(
            juce::String::fromUTF8("メモ"),
            juce::Colour(0xFF7F8993));
        memo_.setMultiLine(true);
        memo_.setReturnKeyStartsNewLine(true);
        addAndMakeVisible(name_);
        addAndMakeVisible(tags_);
        addAndMakeVisible(memo_);
        favorite_.setButtonText(
            juce::String::fromUTF8("お気に入り"));
        favorite_.setLookAndFeel(&switch_look_and_feel_);
        addAndMakeVisible(favorite_);
        configureButton(
            library_new_, juce::String::fromUTF8("新規"),
            juce::String::fromUTF8("新しいOPLL音色を作成します"),
            [this] { newLibraryEntry(); });
        configureButton(
            library_load_, juce::String::fromUTF8("読込"),
            juce::String::fromUTF8("選択音色を編集値へ読み込みます"),
            [this] { loadSelectedLibraryEntry(); });
        configureButton(
            library_save_, juce::String::fromUTF8("保存"),
            juce::String::fromUTF8("選択音色を更新します"),
            [this] { saveLibraryEntry(false); });
        configureButton(
            library_save_as_,
            juce::String::fromUTF8("別名"),
            juce::String::fromUTF8("新しい音色として保存します"),
            [this] { saveLibraryEntry(true); });
        configureButton(
            library_cancel_, juce::String::fromUTF8("取消"),
            juce::String::fromUTF8("編集開始時点へ戻します"),
            [this] { restoreEditorBaseline(); });
        configureButton(
            library_delete_, juce::String::fromUTF8("削除"),
            juce::String::fromUTF8("選択音色を削除します"),
            [this] { deleteSelectedLibraryEntry(); });
        configureButton(
            library_import_, juce::String::fromUTF8("取込"),
            juce::String::fromUTF8("外部.mgstcを取り込みます"),
            [this] { importLibraryFile(); });
        configureButton(
            library_export_, juce::String::fromUTF8("書出"),
            juce::String::fromUTF8("選択音色を書き出します"),
            [this] { exportSelectedLibraryEntry(); });

        status_.setJustificationType(
            juce::Justification::centredLeft);
        addAndMakeVisible(status_);
        performance_keyboard_.setCallbacks(
            [this](std::uint8_t note) {
                startPerformanceNote(note);
            },
            [this](std::uint8_t note) {
                stopPerformanceNote(note);
            });
        performance_keyboard_.setModeCallback(
            [this](bool polyphonic) {
                voice_allocator_.setPolyphonic(polyphonic);
            });
        performance_keyboard_.setSuppressPcInputCallback(
            [this] { return textEntryHasFocusWithin(*this); });
        addAndMakeVisible(performance_keyboard_);

        syncControls();
        updateDefinitionPreview();
        loadLibrary();
        setEditorBaseline();
        updateHistoryButtons();
        setSize(1320, 980);
        engine_ready_ = audio_service.running()
            && configureEngine(false);
        startTimerHz(60);
        updateStatus(
            engine_ready_
                ? juce::String::fromUTF8("準備完了")
                : juce::String::fromUTF8(
                    "音声出力を開始できませんでした"));
        static_cast<void>(consumePendingOpllConversion());
    }

    ~OpllEditorComponent() override {
        stopTimer();
        performance_keyboard_.allNotesOff();
        favorite_.setLookAndFeel(nullptr);
        silenceAllVoices();
    }

    void prepareVisualInspection() {
        static_cast<void>(auditionOneSecond());
    }

    void refreshExternalState() {
        synchronizeMasterVolumeSlider(
            master_volume_, master_volume_revision_, audio_service_);
        // pending取込が試聴を始めた直後に configureEngine(false) すると
        // hardResetで音が消えるため、取込成功時は再構成しない。
        if (!consumePendingOpllConversion() && engine_ready_) {
            static_cast<void>(configureEngine(false));
        }
    }

    void deactivate() {
        performance_keyboard_.allNotesOff();
        silenceAllVoices();
    }

    void paint(juce::Graphics& graphics) override {
        graphics.fillAll(
            getLookAndFeel().findColour(
                juce::ResizableWindow::backgroundColourId));
        if (!library_panel_bounds_.isEmpty()) {
            graphics.setColour(juce::Colour(0xFF29323C));
            graphics.fillRoundedRectangle(
                library_panel_bounds_.toFloat(), 9.0F);
            graphics.setColour(juce::Colour(0xFF435160));
            graphics.drawRoundedRectangle(
                library_panel_bounds_.toFloat(), 9.0F, 1.0F);
        }
        if (!common_parameter_bounds_.isEmpty()) {
            graphics.setColour(juce::Colour(0xFF29323C));
            graphics.fillRoundedRectangle(
                common_parameter_bounds_.toFloat(), 9.0F);
            graphics.setColour(juce::Colour(0xFF435160));
            graphics.drawRoundedRectangle(
                common_parameter_bounds_.toFloat(), 9.0F, 1.0F);
        }
    }

    void resized() override {
        auto area = getLocalBounds().reduced(24);
        title_.setBounds(area.removeFromTop(34));
        description_.setBounds(area.removeFromTop(28));
        settings_.setBounds(getWidth() - 58, 24, 34, 34);
        master_volume_.setBounds(getWidth() - 104, 21, 40, 40);
        immediate_audition_.setBounds(getWidth() - 146, 24, 34, 34);
        area.removeFromTop(10);

        auto keyboard_area = area.removeFromBottom(94);
        area.removeFromBottom(10);
        performance_keyboard_.setBounds(keyboard_area);
        auto toolbar = area.removeFromTop(40);
        constexpr int icon_size = 40;
        for (auto* button :
             std::array<juce::DrawableButton*, 6>{
                 &load_, &save_, &paste_, &copy_, &undo_, &redo_}) {
            button->setBounds(
                toolbar.removeFromLeft(icon_size));
            toolbar.removeFromLeft(6);
        }
        toolbar.removeFromLeft(6);
        import_wave_.setBounds(
            toolbar.removeFromLeft(70));
        toolbar.removeFromLeft(6);
        import_audacity_.setBounds(
            toolbar.removeFromLeft(154));
        toolbar.removeFromLeft(12);
        wave_previous_.setBounds(
            toolbar.removeFromLeft(40));
        wave_candidate_label_.setBounds(
            toolbar.removeFromLeft(112));
        wave_next_.setBounds(
            toolbar.removeFromLeft(40));
        area.removeFromTop(10);

        auto preset_row = area.removeFromTop(38);
        rom_preset_.setBounds(
            preset_row.removeFromLeft(230));
        preset_row.removeFromLeft(8);
        rom_load_.setBounds(
            preset_row.removeFromLeft(150));
        preset_row.removeFromLeft(20);
        open_scc_.setBounds(
            preset_row.removeFromLeft(110));
        preset_row.removeFromLeft(8);
        convert_to_scc_.setBounds(
            preset_row.removeFromLeft(40));
        area.removeFromTop(14);

        auto right = area.removeFromRight(356);
        area.removeFromRight(16);
        library_panel_bounds_ = right;
        common_parameter_bounds_ = area.removeFromTop(62);
        auto common_parameters = common_parameter_bounds_.reduced(12, 5);
        common_parameters_title_.setBounds(
            common_parameters.removeFromTop(20));
        auto common_row = common_parameters;
        const auto common_width = (common_row.getWidth() - 16) / 2;
        auto tl_area = common_row.removeFromLeft(common_width);
        mod_total_level_label_.setBounds(tl_area.removeFromLeft(34));
        mod_total_level_.setBounds(tl_area);
        common_row.removeFromLeft(16);
        feedback_label_.setBounds(common_row.removeFromLeft(72));
        feedback_.setBounds(common_row);
        area.removeFromTop(10);
        auto operators = area.removeFromTop(408);
        const auto panel_width =
            (operators.getWidth() - 14) / 2;
        modulator_.setBounds(
            operators.removeFromLeft(panel_width));
        operators.removeFromLeft(14);
        carrier_.setBounds(operators);

        area.removeFromTop(12);
        scope_title_.setBounds(area.removeFromTop(28));
        scope_.setBounds(area.removeFromTop(110));
        area.removeFromTop(8);
        status_.setBounds(area.removeFromTop(36));

        library_title_.setBounds(right.removeFromTop(28));
        right.removeFromTop(6);
        library_filter_.setBounds(right.removeFromTop(28));
        right.removeFromTop(6);
        library_list_.setBounds(right.removeFromTop(32));
        right.removeFromTop(6);
        name_.setBounds(right.removeFromTop(28));
        right.removeFromTop(5);
        tags_.setBounds(right.removeFromTop(28));
        right.removeFromTop(5);
        memo_.setBounds(right.removeFromTop(52));
        right.removeFromTop(5);
        favorite_.setBounds(right.removeFromTop(26));
        right.removeFromTop(6);
        auto edit_buttons = right.removeFromTop(30);
        const auto button_width =
            (edit_buttons.getWidth() - 15) / 4;
        library_new_.setBounds(
            edit_buttons.removeFromLeft(button_width));
        edit_buttons.removeFromLeft(5);
        library_load_.setBounds(
            edit_buttons.removeFromLeft(button_width));
        edit_buttons.removeFromLeft(5);
        library_save_.setBounds(
            edit_buttons.removeFromLeft(button_width));
        edit_buttons.removeFromLeft(5);
        library_save_as_.setBounds(edit_buttons);
        right.removeFromTop(5);
        auto file_buttons = right.removeFromTop(30);
        library_cancel_.setBounds(
            file_buttons.removeFromLeft(button_width));
        file_buttons.removeFromLeft(5);
        library_delete_.setBounds(
            file_buttons.removeFromLeft(button_width));
        file_buttons.removeFromLeft(5);
        library_import_.setBounds(
            file_buttons.removeFromLeft(button_width));
        file_buttons.removeFromLeft(5);
        library_export_.setBounds(file_buttons);
        right.removeFromTop(10);
        mgsc_title_.setBounds(right.removeFromTop(24));
        right.removeFromTop(5);
        auto number_row = right.removeFromTop(32);
        output_number_label_.setBounds(
            number_row.removeFromLeft(122));
        number_row.removeFromLeft(8);
        output_number_.setBounds(
            number_row.removeFromLeft(64));
        right.removeFromTop(6);
        mgsc_preview_.setBounds(right);
    }

    bool keyPressed(const juce::KeyPress& key) override {
        if (key == juce::KeyPress(
                'v', juce::ModifierKeys::commandModifier, 0)) {
            pasteClipboard();
            return true;
        }
        if (key == juce::KeyPress(
                'c', juce::ModifierKeys::commandModifier, 0)) {
            copyDefinition();
            return true;
        }
        if (key == juce::KeyPress(
                'z', juce::ModifierKeys::commandModifier, 0)) {
            undo();
            return true;
        }
        if (key == juce::KeyPress(
                'y', juce::ModifierKeys::commandModifier, 0)
            || key == juce::KeyPress(
                'z',
                juce::ModifierKeys::commandModifier
                    | juce::ModifierKeys::shiftModifier,
                0)) {
            redo();
            return true;
        }
        return performance_keyboard_.shouldConsumeKeyPress(key);
    }

private:
    void configureIconButton(
        juce::DrawableButton& button,
        EditorIcon icon,
        const juce::String& tooltip,
        std::function<void()> action) {
        const auto normal = makeEditorIcon(
            icon, juce::Colour(0xFFE6EDF3));
        const auto over = makeEditorIcon(
            icon, juce::Colour(0xFF53E3A6));
        const auto down = makeEditorIcon(
            icon, juce::Colour(0xFF2AD6C9));
        const auto disabled = makeEditorIcon(
            icon, juce::Colour(0xFF68737E));
        button.setImages(
            normal.get(),
            over.get(),
            down.get(),
            disabled.get());
        button.setTooltip(tooltip);
        button.setTitle(tooltip);
        button.setDescription(tooltip);
        button.onClick = std::move(action);
        addAndMakeVisible(button);
    }

    void configureButton(
        juce::TextButton& button,
        const juce::String& text,
        const juce::String& tooltip,
        std::function<void()> action) {
        button.setButtonText(text);
        button.setTooltip(tooltip);
        button.onClick = std::move(action);
        addAndMakeVisible(button);
    }

    void showError(
        const juce::String& title,
        const juce::String& message) {
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon,
            title,
            message);
    }

    [[nodiscard]] static std::string utf8Text(
        const juce::String& text) {
        const auto utf8 = text.toUTF8();
        return {
            utf8.getAddress(),
            static_cast<std::size_t>(
                utf8.sizeInBytes() - 1)};
    }

    [[nodiscard]] static std::int64_t unixTimeNow() {
        return static_cast<std::int64_t>(
            juce::Time::getCurrentTime()
                .toMilliseconds()
            / 1000);
    }

    [[nodiscard]] juce::File libraryFile() const {
        return juce::File::getSpecialLocation(
                   juce::File::userApplicationDataDirectory)
            .getChildFile("MgsToneCraft")
            .getChildFile("timbre-library-v1.mgstc");
    }

    [[nodiscard]] juce::File settingsFile() const {
        return juce::File::getSpecialLocation(
                   juce::File::userApplicationDataDirectory)
            .getChildFile("MgsToneCraft")
            .getChildFile("settings-v1.ini");
    }

    [[nodiscard]] bool loadOpllImmediateAuditionSetting() const {
        const auto file = settingsFile();
        if (!file.existsAsFile()) {
            return true;
        }
        return GetPrivateProfileIntW(
                   L"Application",
                   L"OpllImmediateAudition",
                   1,
                   file.getFullPathName()
                       .toWideCharPointer())
            != 0;
    }

    void saveOpllImmediateAuditionSetting(bool enabled) {
        saveApplicationSetting(
            L"OpllImmediateAudition", enabled ? L"1" : L"0");
    }

    [[nodiscard]] std::uint8_t
    loadLastAuditionNoteSetting() const {
        const auto file = settingsFile();
        if (!file.existsAsFile()) {
            return kPreviewNote;
        }
        const int value = GetPrivateProfileIntW(
            L"Application",
            L"LastAuditionNote",
            kPreviewNote,
            file.getFullPathName().toWideCharPointer());
        return static_cast<std::uint8_t>(
            juce::jlimit(24, 119, value));
    }

    void saveLastAuditionNoteSetting(std::uint8_t midi_note) {
        const auto value = juce::String(
            static_cast<int>(midi_note));
        saveApplicationSetting(
            L"LastAuditionNote",
            value.toWideCharPointer());
    }

    void saveApplicationSetting(
        const wchar_t* key,
        const wchar_t* value) {
        const auto file = settingsFile();
        if (file.getParentDirectory()
                .createDirectory()
                .failed()) {
            return;
        }
        const auto path_text = file.getFullPathName();
        const auto path = path_text.toWideCharPointer();
        static_cast<void>(WritePrivateProfileStringW(
            L"Application", L"SchemaVersion", L"1", path));
        static_cast<void>(WritePrivateProfileStringW(
            L"Application", key, value, path));
        static_cast<void>(WritePrivateProfileStringW(
            nullptr, nullptr, nullptr, path));
    }

    [[nodiscard]] juce::String definitionText() const {
        const auto number = outputNumber();
        if (!number) {
            return {};
        }
        const auto text =
            mgstc::engine::formatMgsOpllDefinition(
                patch_, *number);
        return juce::String::fromUTF8(
            text.data(),
            static_cast<int>(text.size()));
    }

    void loadDefinition() {
        juce::FileChooser chooser(
            juce::String::fromUTF8("OPLL音色を読み込む"),
            {},
            "*.mgs;*.txt;*.opll");
        if (!chooser.browseForFileToOpen()) {
            return;
        }
        const auto parsed =
            mgstc::engine::parseMgsOpllDefinition(
                utf8Text(
                    chooser.getResult().loadFileAsString()));
        if (!parsed) {
            showError(
                juce::String::fromUTF8("ファイル読込"),
                juce::String::fromUTF8(
                    "OPLL音色定義を読み取れませんでした"));
            return;
        }
        output_number_.setText(
            juce::String(static_cast<int>(parsed->number)),
            false);
        commitPatch(parsed->patch);
        updateStatus(
            juce::String::fromUTF8(
                "OPLL音色を読み込みました"));
    }

    void saveDefinition() {
        const auto definition = definitionText();
        if (definition.isEmpty()) {
            showError(
                juce::String::fromUTF8("ファイル保存"),
                juce::String::fromUTF8(
                    "一時出力番号を15～31で入力してください"));
            output_number_.grabKeyboardFocus();
            return;
        }
        juce::FileChooser chooser(
            juce::String::fromUTF8("OPLL音色を保存する"),
            juce::File::getCurrentWorkingDirectory()
                .getChildFile("opll-tone.mgs"),
            "*.mgs;*.txt");
        if (!chooser.browseForFileToSave(true)) {
            return;
        }
        if (!chooser.getResult().replaceWithText(
                definition, false, false, "\r\n")) {
            showError(
                juce::String::fromUTF8("ファイル保存"),
                juce::String::fromUTF8(
                    "OPLL音色ファイルを保存できませんでした"));
            return;
        }
        updateStatus(
            juce::String::fromUTF8(
                "OPLL音色を保存しました"));
    }

    void copyDefinition() {
        const auto definition = definitionText();
        if (definition.isEmpty()) {
            showError(
                juce::String::fromUTF8("コピー"),
                juce::String::fromUTF8(
                    "一時出力番号を15～31で入力してください"));
            output_number_.grabKeyboardFocus();
            return;
        }
        if (!mgstc::platform::copyTextToClipboardUnicodeAndAnsi(
                std::wstring(definition.toWideCharPointer()))) {
            juce::SystemClipboard::copyTextToClipboard(definition);
        }
        updateStatus(
            juce::String::fromUTF8(
                "OPLL音色定義をコピーしました"));
    }

    void showOpllToSccDialog() {
        auto* dialog = new juce::AlertWindow(
            juce::String::fromUTF8("OPLL → SCC 音色近似"),
            juce::String::fromUTF8(
                "SCC波形として抽出する発音位置を選択してください。"),
            juce::MessageBoxIconType::NoIcon);
        dialog->addComboBox(
            "position",
            {juce::String::fromUTF8("自動（最大音量付近）"),
             juce::String::fromUTF8("アタック直後"),
             juce::String::fromUTF8("サステイン部分"),
             juce::String::fromUTF8("フレーム指定")},
            juce::String::fromUTF8("変換位置"));
        dialog->getComboBoxComponent("position")
            ->setSelectedId(1, juce::dontSendNotification);
        dialog->addTextEditor(
            "frame", "6",
            juce::String::fromUTF8("フレーム（1/60秒）"));
        dialog->addButton(
            juce::String::fromUTF8("変換"), 1,
            juce::KeyPress(juce::KeyPress::returnKey));
        dialog->addButton(
            juce::String::fromUTF8("キャンセル"), 0,
            juce::KeyPress(juce::KeyPress::escapeKey));
        juce::Component::SafePointer<OpllEditorComponent> safe(this);
        dialog->enterModalState(
            true,
            juce::ModalCallbackFunction::create(
                [safe, dialog](int result) {
                    if (safe == nullptr || result != 1) {
                        return;
                    }
                    const int selection =
                        dialog->getComboBoxComponent("position")
                            ->getSelectedId();
                    int frame = 6;
                    if (selection == 2) {
                        frame = 1;
                    } else if (selection == 3) {
                        frame = 30;
                    } else if (selection == 4) {
                        frame = dialog->getTextEditor("frame")
                                    ->getText().getIntValue();
                    }
                    safe->convertToScc(juce::jlimit(0, 120, frame));
                }),
            true);
    }

    void convertToScc(int frame) {
        audio_service_.setSharedOpllPatch(patch_);
        static_cast<void>(configureEngine(false));
        updateStatus(juce::String::fromUTF8(
            "OPLL音色をSCC波形へ近似変換しています…"));
        const auto pcm = makeOpllReferencePcm(
            patch_, static_cast<std::size_t>(frame) * 800U);
        const auto analysis = mgstc::engine::analyzeWaveCycle(pcm);
        if (analysis.cycle.size() < 2) {
            updateStatus(juce::String::fromUTF8(
                "SCC用の周期波形を抽出できませんでした"));
            return;
        }
        const auto waveform =
            mgstc::engine::waveCycleToScc(analysis.cycle);
        const auto definition =
            mgstc::engine::formatMgsSccDefinition(waveform, 0);
        const auto file = pendingConversionFile("scc");
        if (!file.getParentDirectory().createDirectory()
            || !file.replaceWithText(
                juce::String::fromUTF8(definition.c_str()))) {
            updateStatus(juce::String::fromUTF8(
                "SCC変換データを保存できませんでした"));
            return;
        }
        open_editor_("scc");
        updateStatus(juce::String::fromUTF8(
            "SCC近似波形を生成し、SCCエディタへ送りました"));
    }

    [[nodiscard]] bool consumePendingOpllConversion() {
        const auto file = pendingConversionFile("opll");
        if (!file.existsAsFile()) {
            return false;
        }
        const auto text = utf8Text(file.loadFileAsString());
        const auto parsed =
            mgstc::engine::parseMgsOpllDefinition(text);
        if (!parsed) {
            static_cast<void>(file.deleteFile());
            updateStatus(juce::String::fromUTF8(
                "受信したOPLL近似音色を読み込めませんでした"));
            return false;
        }
        static_cast<void>(file.deleteFile());
        commitPatch(parsed->patch);
        updateStatus(juce::String::fromUTF8(
            "SCCから近似変換したOPLL音色を読み込みました"));
        return true;
    }

    bool importWaveBytes(
        const std::vector<std::uint8_t>& bytes,
        const juce::String& source_name) {
        mgstc::engine::WavePcm pcm;
        std::string error;
        if (!mgstc::engine::parseWavePcm(
                std::span<const std::uint8_t>(bytes),
                pcm,
                &error)) {
            showError(
                source_name,
                juce::String::fromUTF8(
                    "WAVを読み込めませんでした。"
                    "PCM 8/16/24/32bitまたは32bit floatの"
                    "WAVEを使用してください"));
            return false;
        }
        const auto analysis =
            mgstc::engine::analyzeWaveCycle(pcm);
        if (analysis.cycle.size() < 2) {
            showError(
                source_name,
                juce::String::fromUTF8(
                    "波形の1周期を抽出できませんでした"));
            return false;
        }
        const auto frequency_hz = analysis.estimated_frequency_hz;
        juce::Component::SafePointer<OpllEditorComponent> safe(this);
        runWithConversionBusyDialog(
            this,
            [pcm] {
                return mgstc::engine::
                    approximateWavePcmCandidatesWithOpll(pcm);
            },
            [safe, source_name, frequency_hz](
                std::vector<mgstc::engine::OpllPatchParameters>
                    candidates) {
                if (safe == nullptr) {
                    return;
                }
                if (candidates.empty()) {
                    safe->showError(
                        source_name,
                        juce::String::fromUTF8(
                            "OPLL近似音色を生成できませんでした"));
                    return;
                }
                safe->wave_candidates_ = std::move(candidates);
                safe->wave_candidate_index_ = 0;
                safe->commitPatch(
                    safe->wave_candidates_.front(), false);
                safe->updateWaveCandidateControls();
                safe->updateStatus(
                    source_name
                    + juce::String::fromUTF8(
                        "完了: OPLL近似候補 ")
                    + juce::String(
                        static_cast<int>(
                            safe->wave_candidates_.size()))
                    + juce::String::fromUTF8("件／約 ")
                    + juce::String(frequency_hz, 1)
                    + " Hz");
            });
        return true;
    }

    void pasteClipboard() {
        if (const auto wave =
                mgstc::platform::clipboardWaveBytes()) {
            static_cast<void>(importWaveBytes(
                *wave,
                juce::String::fromUTF8(
                    "音声の貼り付け変換")));
            return;
        }
        const auto parsed =
            mgstc::engine::parseMgsOpllDefinition(
                utf8Text(
                    juce::SystemClipboard::
                        getTextFromClipboard()));
        if (!parsed) {
            showError(
                juce::String::fromUTF8("貼り付け"),
                juce::String::fromUTF8(
                    "クリップボードにWAV音声、WAVファイル、"
                    "またはOPLL音色定義がありません"));
            return;
        }
        output_number_.setText(
            juce::String(static_cast<int>(parsed->number)),
            false);
        commitPatch(parsed->patch);
        updateStatus(
            juce::String::fromUTF8(
                "OPLL音色定義を貼り付けました"));
    }

    void importWaveFile() {
        juce::FileChooser chooser(
            juce::String::fromUTF8(
                "WAVからOPLL音色へ近似変換"),
            {},
            "*.wav;*.wave");
        if (!chooser.browseForFileToOpen()) {
            return;
        }
        const auto file = chooser.getResult();
        const auto wave =
            mgstc::platform::readWaveFileBytes(
                std::filesystem::path(
                    file.getFullPathName()
                        .toWideCharPointer()));
        if (!wave) {
            showError(
                "WAV",
                juce::String::fromUTF8(
                    "WAVファイルを読み込めませんでした"));
            return;
        }
        static_cast<void>(
            importWaveBytes(*wave, "WAV"));
    }

    void importAudacitySelection() {
        juce::MouseCursor::showWaitCursor();
        auto imported =
            mgstc::platform::
                exportAudacitySelectionToWave();
        juce::MouseCursor::hideWaitCursor();
        if (!imported.wave) {
            showError(
                juce::String::fromUTF8(
                    "Audacityから変換"),
                juce::String(imported.error.c_str()));
            return;
        }
        static_cast<void>(importWaveBytes(
            *imported.wave,
            juce::String::fromUTF8("Audacity変換")));
    }

    void updateWaveCandidateControls() {
        const bool multiple = wave_candidates_.size() > 1;
        wave_previous_.setEnabled(multiple);
        wave_next_.setEnabled(multiple);
        wave_candidate_label_.setText(
            wave_candidates_.empty()
                ? juce::String::fromUTF8("WAV候補 --/--")
                : juce::String::fromUTF8("WAV候補 ")
                    + juce::String(
                        static_cast<int>(
                            wave_candidate_index_ + 1))
                    + "/"
                    + juce::String(
                        static_cast<int>(
                            wave_candidates_.size())),
            juce::dontSendNotification);
    }

    void selectAdjacentWaveCandidate(int direction) {
        if (wave_candidates_.size() < 2) {
            return;
        }
        const auto count = wave_candidates_.size();
        wave_candidate_index_ = direction < 0
            ? (wave_candidate_index_ + count - 1) % count
            : (wave_candidate_index_ + 1) % count;
        patch_ = wave_candidates_[wave_candidate_index_];
        history_[history_cursor_] = patch_;
        syncControls();
        updateDefinitionPreview();
        auditionAfterEdit();
        updateWaveCandidateControls();
        updateStatus(
            juce::String::fromUTF8("WAV近似候補 ")
            + juce::String(
                static_cast<int>(wave_candidate_index_ + 1))
            + "/"
            + juce::String(
                static_cast<int>(wave_candidates_.size())));
    }

    void loadLibrary() {
        const auto file = libraryFile();
        if (file.existsAsFile()) {
            const auto source =
                utf8Text(file.loadFileAsString());
            std::string error;
            if (auto loaded =
                    mgstc::engine::TimbreLibrary::deserialize(
                        source, &error)) {
                library_ = std::move(*loaded);
            } else {
                showError(
                    juce::String::fromUTF8(
                        "OPLL音色ライブラリ"),
                    juce::String::fromUTF8(
                        "既存ライブラリを読み込めませんでした"));
            }
        }
        refreshLibraryList();
    }

    [[nodiscard]] bool reloadLibraryFromDisk() {
        const auto file = libraryFile();
        if (!file.existsAsFile()) {
            library_ = {};
            return true;
        }
        const auto source =
            utf8Text(file.loadFileAsString());
        std::string error;
        auto loaded =
            mgstc::engine::TimbreLibrary::deserialize(
                source, &error);
        if (!loaded) {
            return false;
        }
        library_ = std::move(*loaded);
        return true;
    }

    [[nodiscard]] bool persistLibrary() {
        const auto target = libraryFile();
        if (!target.getParentDirectory().createDirectory()) {
            return false;
        }
        juce::TemporaryFile temporary(target);
        const auto contents = library_.serialize();
        if (!temporary.getFile().replaceWithData(
                contents.data(), contents.size())) {
            return false;
        }
        return temporary.overwriteTargetFileWithTemporary();
    }

    void refreshLibraryList() {
        library_list_.clear(juce::dontSendNotification);
        library_ids_.clear();
        const auto filter =
            library_filter_.getText().trim();
        int item_id = 1;
        int selected_item = 0;
        for (const auto& entry : library_.entries()) {
            if (entry.category
                != mgstc::engine::TimbreCategory::Opll) {
                continue;
            }
            if (filter.isNotEmpty()) {
                const auto name =
                    juce::String::fromUTF8(
                        entry.name.c_str());
                const auto tags =
                    juce::String::fromUTF8(
                        entry.tags.c_str());
                const auto memo =
                    juce::String::fromUTF8(
                        entry.memo.c_str());
                if (!name.containsIgnoreCase(filter)
                    && !tags.containsIgnoreCase(filter)
                    && !memo.containsIgnoreCase(filter)) {
                    continue;
                }
            }
            library_ids_.push_back(entry.id);
            auto label =
                juce::String::fromUTF8(entry.name.c_str())
                + "  r"
                + juce::String(static_cast<int>(entry.revision));
            if (entry.favorite) {
                label =
                    juce::String::fromUTF8("★ ") + label;
            }
            library_list_.addItem(label, item_id);
            if (selected_library_id_
                && *selected_library_id_ == entry.id) {
                selected_item = item_id;
            }
            ++item_id;
        }
        library_list_.setSelectedId(
            selected_item, juce::dontSendNotification);
        if (selected_item == 0) {
            selected_library_id_.reset();
        }
    }

    [[nodiscard]] const
        mgstc::engine::TimbreLibraryEntry*
    selectedLibraryEntry() const {
        if (!selected_library_id_) {
            return nullptr;
        }
        return library_.find(*selected_library_id_);
    }

    void selectLibraryEntryFromList() {
        const auto selected =
            library_list_.getSelectedId();
        if (selected <= 0
            || static_cast<std::size_t>(selected)
                > library_ids_.size()) {
            selected_library_id_.reset();
            return;
        }
        selected_library_id_ =
            library_ids_[
                static_cast<std::size_t>(selected - 1)];
        const auto* entry = selectedLibraryEntry();
        if (!entry) {
            return;
        }
        name_.setText(
            juce::String::fromUTF8(entry->name.c_str()),
            false);
        tags_.setText(
            juce::String::fromUTF8(entry->tags.c_str()),
            false);
        memo_.setText(
            juce::String::fromUTF8(entry->memo.c_str()),
            false);
        favorite_.setToggleState(
            entry->favorite, juce::dontSendNotification);
        updateStatus(
            juce::String::fromUTF8(
                "音色を選択しました。「読込」で編集値へ反映します"));
    }

    [[nodiscard]] mgstc::engine::TimbreLibraryEntry
    captureLibraryEntry() const {
        mgstc::engine::TimbreLibraryEntry entry;
        entry.category =
            mgstc::engine::TimbreCategory::Opll;
        entry.name = utf8Text(name_.getText().trim());
        entry.tags = utf8Text(tags_.getText().trim());
        entry.memo = utf8Text(memo_.getText());
        entry.favorite = favorite_.getToggleState();
        entry.opll_registers =
            mgstc::engine::encodeOpllPatch(patch_);
        return entry;
    }

    void newLibraryEntry() {
        selected_library_id_.reset();
        library_list_.setSelectedId(
            0, juce::dontSendNotification);
        name_.clear();
        tags_.clear();
        memo_.clear();
        favorite_.setToggleState(
            false, juce::dontSendNotification);
        commitPatch(
            mgstc::engine::defaultOpllPatch());
        setEditorBaseline();
        updateStatus(
            juce::String::fromUTF8(
                "新しいOPLL音色を編集中"));
    }

    void loadSelectedLibraryEntry() {
        if (!selected_library_id_) {
            showError(
                juce::String::fromUTF8(
                    "OPLL音色ライブラリ"),
                juce::String::fromUTF8(
                    "読み込む音色を選択してください"));
            return;
        }
        const auto id = *selected_library_id_;
        juce::InterProcessLock::ScopedLockType lock(
            library_lock_);
        if (!lock.isLocked() || !reloadLibraryFromDisk()) {
            showError(
                juce::String::fromUTF8(
                    "OPLL音色ライブラリ"),
                juce::String::fromUTF8(
                    "共有ライブラリを読み直せません"));
            return;
        }
        const auto* entry = library_.find(id);
        if (!entry) {
            selected_library_id_.reset();
            refreshLibraryList();
            showError(
                juce::String::fromUTF8(
                    "OPLL音色ライブラリ"),
                juce::String::fromUTF8(
                    "選択音色は他の画面で削除されています"));
            return;
        }
        commitPatch(
            mgstc::engine::decodeOpllPatch(
                entry->opll_registers));
        refreshLibraryList();
        setEditorBaseline();
        updateStatus(
            juce::String::fromUTF8(
                "ライブラリ音色を読み込みました"));
    }

    void saveLibraryEntry(bool save_as) {
        if (save_as || !selected_library_id_) {
            performSaveLibraryEntry(save_as);
            return;
        }
        const auto* current = selectedLibraryEntry();
        const auto name = current
            ? juce::String::fromUTF8(current->name.c_str())
            : juce::String::fromUTF8("選択中の音色");
        const auto revision = current ? current->revision : 1;
        const auto impact = inspectCompositeTimbreImpact(
            *selected_library_id_);
        juce::Component::SafePointer<OpllEditorComponent> safe(this);
        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions()
                .withIconType(
                    juce::MessageBoxIconType::WarningIcon)
                .withTitle(
                    juce::String::fromUTF8("OPLL音色の上書き"))
                .withMessage(
                    juce::String::fromUTF8("「")
                    + name
                    + juce::String::fromUTF8(
                        "」を上書きしてリビジョンを更新します。\n"
                        "保存済み総合音色の参照も同時に更新します。\n\n")
                    + describeCompositeTimbreImpact(impact)
                    + juce::String::fromUTF8("\n\n現在: r")
                    + juce::String(static_cast<int>(revision))
                    + juce::String::fromUTF8("  更新後: r")
                    + juce::String(static_cast<int>(revision + 1)))
                .withButton(
                    juce::String::fromUTF8("更新して上書き"))
                .withButton(
                    juce::String::fromUTF8("キャンセル"))
                .withAssociatedComponent(this),
            [safe](int result) {
                if (safe != nullptr && result == 1) {
                    safe->performSaveLibraryEntry(false);
                }
            });
    }

    void performSaveLibraryEntry(bool save_as) {
        auto entry = captureLibraryEntry();
        if (entry.name.empty()) {
            showError(
                juce::String::fromUTF8(
                    "OPLL音色ライブラリ"),
                juce::String::fromUTF8(
                    "保存する音色名を入力してください"));
            name_.grabKeyboardFocus();
            return;
        }
        juce::InterProcessLock::ScopedLockType lock(
            library_lock_);
        if (!lock.isLocked() || !reloadLibraryFromDisk()) {
            showError(
                juce::String::fromUTF8(
                    "OPLL音色ライブラリ"),
                juce::String::fromUTF8(
                    "共有ライブラリを更新できません"));
            return;
        }
        auto composite_library = loadCompositeTimbreLibrary();
        if (!save_as && selected_library_id_
            && !composite_library) {
            showError(
                juce::String::fromUTF8(
                    "OPLL音色ライブラリ"),
                juce::String::fromUTF8(
                    "総合音色ライブラリを読み込めないため、"
                    "安全に上書き更新できません"));
            return;
        }
        const auto original_library = library_;
        std::size_t updated_composite_references{};
        if (save_as || !selected_library_id_) {
            entry.name = library_.uniqueName(
                mgstc::engine::TimbreCategory::Opll,
                entry.name);
            selected_library_id_ = library_.add(
                std::move(entry), unixTimeNow());
        } else if (!library_.update(
                       *selected_library_id_,
                       entry,
                       unixTimeNow())) {
            showError(
                juce::String::fromUTF8(
                    "OPLL音色ライブラリ"),
                juce::String::fromUTF8(
                    "選択音色は他の画面で削除されています"));
            selected_library_id_.reset();
            refreshLibraryList();
            return;
        }
        if (!save_as && selected_library_id_
            && composite_library) {
            const auto* updated_entry =
                library_.find(*selected_library_id_);
            if (updated_entry) {
                updated_composite_references =
                    composite_library->updateTimbreReferences(
                        *updated_entry, unixTimeNow());
            }
        }
        const bool saved = persistLibrary();
        if (saved && updated_composite_references != 0
            && !persistCompositeTimbreLibrary(
                *composite_library)) {
            library_ = original_library;
            const bool rolled_back = persistLibrary();
            refreshLibraryList();
            showError(
                juce::String::fromUTF8(
                    "OPLL音色ライブラリ"),
                rolled_back
                    ? juce::String::fromUTF8(
                          "総合音色を更新できなかったため、"
                          "音色の上書きを取り消しました")
                    : juce::String::fromUTF8(
                          "総合音色の更新と音色の復元に失敗しました。"
                          "ライブラリファイルを確認してください"));
            return;
        }
        if (!saved) {
            showError(
                juce::String::fromUTF8(
                    "OPLL音色ライブラリ"),
                juce::String::fromUTF8(
                    "ライブラリファイルを更新できませんでした"));
            return;
        }
        refreshLibraryList();
        selectLibraryEntryFromList();
        setEditorBaseline();
        updateStatus(
            save_as
                ? juce::String::fromUTF8(
                    "新しいOPLL音色として保存しました")
                : juce::String::fromUTF8(
                    "OPLL音色を保存しました")
                    + (updated_composite_references == 0
                           ? juce::String{}
                           : juce::String::fromUTF8(
                                 "（総合音色の参照も更新）")));
    }

    void setEditorBaseline() {
        editor_baseline_ = captureLibraryEntry();
        editor_baseline_id_ = selected_library_id_;
        editor_baseline_valid_ = true;
    }

    void restoreEditorBaseline() {
        if (!editor_baseline_valid_) {
            return;
        }
        selected_library_id_ = editor_baseline_id_;
        name_.setText(
            juce::String::fromUTF8(
                editor_baseline_.name.c_str()),
            false);
        tags_.setText(
            juce::String::fromUTF8(
                editor_baseline_.tags.c_str()),
            false);
        memo_.setText(
            juce::String::fromUTF8(
                editor_baseline_.memo.c_str()),
            false);
        favorite_.setToggleState(
            editor_baseline_.favorite,
            juce::dontSendNotification);
        commitPatch(
            mgstc::engine::decodeOpllPatch(
                editor_baseline_.opll_registers));
        refreshLibraryList();
        updateStatus(
            juce::String::fromUTF8(
                "編集開始時点の音色へ戻しました"));
    }

    void deleteSelectedLibraryEntry() {
        const auto* entry = selectedLibraryEntry();
        if (!entry) {
            showError(
                juce::String::fromUTF8(
                    "OPLL音色ライブラリ"),
                juce::String::fromUTF8(
                    "削除する音色を選択してください"));
            return;
        }
        const auto id = entry->id;
        const auto entry_name =
            juce::String::fromUTF8(entry->name.c_str());
        juce::Component::SafePointer<
            OpllEditorComponent> safe(this);
        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions()
                .withIconType(
                    juce::MessageBoxIconType::WarningIcon)
                .withTitle(
                    juce::String::fromUTF8(
                        "OPLL音色ライブラリ"))
                .withMessage(
                    juce::String::fromUTF8("「")
                    + entry_name
                    + juce::String::fromUTF8(
                        "」を削除しますか？"))
                .withButton(
                    juce::String::fromUTF8("削除"))
                .withButton(
                    juce::String::fromUTF8(
                        "キャンセル"))
                .withAssociatedComponent(this),
            [safe, id](int result) {
                if (safe == nullptr || result != 1) {
                    return;
                }
                juce::InterProcessLock::ScopedLockType lock(
                    safe->library_lock_);
                if (!lock.isLocked()
                    || !safe->reloadLibraryFromDisk()
                    || !safe->library_.erase(id)
                    || !safe->persistLibrary()) {
                    safe->showError(
                        juce::String::fromUTF8(
                            "OPLL音色ライブラリ"),
                        juce::String::fromUTF8(
                            "削除結果を保存できませんでした"));
                    return;
                }
                safe->selected_library_id_.reset();
                safe->name_.clear();
                safe->tags_.clear();
                safe->memo_.clear();
                safe->favorite_.setToggleState(
                    false, juce::dontSendNotification);
                safe->refreshLibraryList();
                safe->setEditorBaseline();
                safe->updateStatus(
                    juce::String::fromUTF8(
                        "OPLL音色を削除しました"));
            });
    }

    void importLibraryFile() {
        juce::FileChooser chooser(
            juce::String::fromUTF8(
                "音色ライブラリを取り込む"),
            {},
            "*.mgstc;*.dat");
        if (!chooser.browseForFileToOpen()) {
            return;
        }
        const auto source =
            utf8Text(chooser.getResult().loadFileAsString());
        juce::InterProcessLock::ScopedLockType lock(
            library_lock_);
        if (!lock.isLocked() || !reloadLibraryFromDisk()) {
            showError(
                juce::String::fromUTF8("ライブラリ取込"),
                juce::String::fromUTF8(
                    "共有ライブラリを更新できません"));
            return;
        }
        std::string error;
        const auto imported_ids = library_.importSerialized(
            source, unixTimeNow(), &error);
        if (!imported_ids) {
            showError(
                juce::String::fromUTF8("ライブラリ取込"),
                juce::String::fromUTF8(
                    "対応するライブラリではありません"));
            return;
        }
        std::optional<std::uint64_t> first_opll;
        for (const auto id : *imported_ids) {
            const auto* imported = library_.find(id);
            if (imported
                && imported->category
                    == mgstc::engine::TimbreCategory::Opll) {
                first_opll = id;
                break;
            }
        }
        if (!persistLibrary()) {
            showError(
                juce::String::fromUTF8("ライブラリ取込"),
                juce::String::fromUTF8(
                    "取込結果を保存できませんでした"));
            return;
        }
        selected_library_id_ = first_opll;
        refreshLibraryList();
        if (first_opll && selected_library_id_) {
            selectLibraryEntryFromList();
        }
        updateStatus(
            juce::String(
                static_cast<int>(imported_ids->size()))
            + juce::String::fromUTF8(
                "件の音色を取り込みました"));
    }

    void exportSelectedLibraryEntry() {
        if (!selected_library_id_) {
            showError(
                juce::String::fromUTF8("ライブラリ書出"),
                juce::String::fromUTF8(
                    "書き出す音色を選択してください"));
            return;
        }
        const auto id = *selected_library_id_;
        std::optional<std::string> contents;
        {
            juce::InterProcessLock::ScopedLockType lock(
                library_lock_);
            if (!lock.isLocked() || !reloadLibraryFromDisk()) {
                showError(
                    juce::String::fromUTF8("ライブラリ書出"),
                    juce::String::fromUTF8(
                        "共有ライブラリを読み直せません"));
                return;
            }
            contents = library_.serializeEntry(id);
            if (!contents) {
                selected_library_id_.reset();
                refreshLibraryList();
                showError(
                    juce::String::fromUTF8("ライブラリ書出"),
                    juce::String::fromUTF8(
                        "選択音色は他の画面で削除されています"));
                return;
            }
        }
        juce::FileChooser chooser(
            juce::String::fromUTF8(
                "選択音色を書き出す"),
            juce::File::getCurrentWorkingDirectory()
                .getChildFile("opll-timbre.mgstc"),
            "*.mgstc");
        if (!chooser.browseForFileToSave(true)) {
            return;
        }
        if (!chooser.getResult().replaceWithData(
                contents->data(), contents->size())) {
            showError(
                juce::String::fromUTF8("ライブラリ書出"),
                juce::String::fromUTF8(
                    "音色を書き出せませんでした"));
            return;
        }
        updateStatus(
            juce::String::fromUTF8(
                "選択OPLL音色を書き出しました"));
    }

    [[nodiscard]] static bool samePatch(
        const mgstc::engine::OpllPatchParameters& left,
        const mgstc::engine::OpllPatchParameters& right) {
        return mgstc::engine::encodeOpllPatch(left)
            == mgstc::engine::encodeOpllPatch(right);
    }

    void recordHistory() {
        if (samePatch(history_[history_cursor_], patch_)) {
            return;
        }
        history_.erase(
            history_.begin()
                + static_cast<std::ptrdiff_t>(
                    history_cursor_ + 1),
            history_.end());
        history_.push_back(patch_);
        if (history_.size() > 257) {
            history_.erase(history_.begin());
        } else {
            ++history_cursor_;
        }
        updateHistoryButtons();
    }

    void clearWaveCandidates() {
        wave_candidates_.clear();
        wave_candidate_index_ = 0;
        updateWaveCandidateControls();
    }

    void commitPatch(
        const mgstc::engine::OpllPatchParameters& patch,
        bool clear_candidates = true) {
        const bool changed = !samePatch(patch_, patch);
        patch_ = patch;
        if (clear_candidates) {
            clearWaveCandidates();
        }
        syncControls();
        updateDefinitionPreview();
        if (changed) {
            recordHistory();
        }
        auditionAfterEdit();
    }

    void restoreHistory() {
        patch_ = history_[history_cursor_];
        clearWaveCandidates();
        syncControls();
        updateDefinitionPreview();
        updateHistoryButtons();
        auditionAfterEdit();
        updateStatus(
            juce::String::fromUTF8(
                "履歴からOPLL音色を復元しました"));
    }

    void undo() {
        if (history_cursor_ == 0) {
            return;
        }
        --history_cursor_;
        restoreHistory();
    }

    void redo() {
        if (history_cursor_ + 1 >= history_.size()) {
            return;
        }
        ++history_cursor_;
        restoreHistory();
    }

    void updateHistoryButtons() {
        undo_.setEnabled(history_cursor_ > 0);
        redo_.setEnabled(
            history_cursor_ + 1 < history_.size());
    }

    [[nodiscard]] std::optional<
        mgstc::engine::OpllPatchParameters>
    selectedRomPatch() const {
        const auto selected = rom_preset_.getSelectedId();
        if (selected < 1 || selected > 15) {
            return std::nullopt;
        }
        return mgstc::engine::ym2413RomPatch(
            static_cast<std::uint8_t>(selected));
    }

    void syncControls() {
        syncing_ = true;
        modulator_.setParameters(patch_.modulator);
        carrier_.setParameters(patch_.carrier);
        mod_total_level_.setValue(
            patch_.modulator.total_level,
            juce::dontSendNotification);
        feedback_.setValue(
            patch_.feedback,
            juce::dontSendNotification);
        syncing_ = false;
        refreshEnvelopeTrace();
    }

    void refreshEnvelopeTrace() {
        const auto trace =
            mgstc::engine::traceOpllEnvelope(
                patch_, last_audition_note_);
        modulator_.setEnvelopeTrace(
            trace.modulator, trace.valid);
        carrier_.setEnvelopeTrace(
            trace.carrier, trace.valid);
    }

    void updateAuditionNoteLabels() {
        modulator_.setAuditionNote(last_audition_note_);
        carrier_.setAuditionNote(last_audition_note_);
    }

    void auditionAfterEdit(
        const mgstc::engine::OpllPatchParameters*
            preview = nullptr) {
        if (immediate_audition_.getToggleState()) {
            static_cast<void>(auditionOneSecond(preview));
            return;
        }
        // 即時発声OFFでも共有プログラムへ確定音色を反映する。
        if (preview == nullptr) {
            static_cast<void>(configureEngine(false));
        }
    }

    void controlsChanged(bool commit = true) {
        if (syncing_) {
            return;
        }
        patch_.modulator = modulator_.parameters();
        patch_.carrier = carrier_.parameters();
        patch_.modulator.total_level = static_cast<std::uint8_t>(
            mod_total_level_.getValue());
        patch_.feedback = static_cast<std::uint8_t>(
            feedback_.getValue());
        clearWaveCandidates();
        if (commit) {
            recordHistory();
        }
        refreshEnvelopeTrace();
        updateDefinitionPreview();
        auditionAfterEdit();
        updateStatus(
            commit
                ? juce::String::fromUTF8(
                      "OPLL音色を更新しました")
                : juce::String::fromUTF8(
                      "OPLL音色を編集中"));
    }

    void envelopeGraphChanged(bool commit) {
        if (syncing_) {
            return;
        }
        patch_.modulator = modulator_.parameters();
        patch_.carrier = carrier_.parameters();
        patch_.modulator.total_level = static_cast<std::uint8_t>(
            mod_total_level_.getValue());
        patch_.feedback = static_cast<std::uint8_t>(
            feedback_.getValue());
        clearWaveCandidates();
        refreshEnvelopeTrace();
        updateDefinitionPreview();
        auditionAfterEdit();
        if (commit) {
            recordHistory();
            updateStatus(
                juce::String::fromUTF8(
                    "EGグラフの編集をUndo履歴へ確定しました"));
        } else {
            updateStatus(
                juce::String::fromUTF8(
                    "EGグラフをドラッグ編集中"));
        }
    }

    [[nodiscard]] std::optional<std::uint8_t>
    outputNumber() const {
        const auto text = output_number_.getText().trim();
        if (text.isEmpty()) {
            return std::nullopt;
        }
        const auto value = text.getIntValue();
        if (value < 15 || value > 31) {
            return std::nullopt;
        }
        return static_cast<std::uint8_t>(value);
    }

    void updateDefinitionPreview() {
        const auto number = outputNumber();
        if (!number) {
            mgsc_preview_.setText(
                juce::String::fromUTF8(
                    "一時出力番号は15～31で入力してください。"),
                false);
            return;
        }
        const auto definition =
            mgstc::engine::formatMgsOpllDefinition(
                patch_, *number);
        mgsc_preview_.setText(
            juce::String::fromUTF8(
                definition.data(),
                static_cast<int>(definition.size())),
            false);
    }

    void clearEngineVoices() {
        for (std::uint8_t channel = 0; channel < 9; ++channel) {
            static_cast<void>(engine_.submit(
                mgstc::engine::EngineCommand::noteOff(
                    static_cast<std::uint8_t>(
                        kOpllTrack + channel))));
        }
        static_cast<void>(voice_allocator_.allNotesOff());
    }

    void silenceAllVoices() {
        audition_stop_time_ms_.reset();
        performance_keyboard_.clearPreviewNote();
        clearEngineVoices();
    }

    bool configureEngine(
        bool retrigger,
        const mgstc::engine::OpllPatchParameters*
            preview = nullptr) {
        // hardReset前にアロケータと実発音を揃え、Poly残留を防ぐ。
        clearEngineVoices();
        engine_holds_temporary_program_ = (preview != nullptr);
        const auto& patch = preview ? *preview : patch_;
        if (preview == nullptr) {
            audio_service_.setSharedOpllPatch(patch_);
        }
        return audio_service_.submitSharedEditorProgram(
            audio_service_.sharedSccWaveform(),
            patch,
            retrigger,
            kOpllTrack,
            last_audition_note_);
    }

    void startPerformanceNote(std::uint8_t note) {
        audition_stop_time_ms_.reset();
        performance_keyboard_.clearPreviewNote();
        last_audition_note_ = note;
        saveLastAuditionNoteSetting(last_audition_note_);
        updateAuditionNoteLabels();
        refreshEnvelopeTrace();
        if (!engine_ready_) {
            updateStatus(
                juce::String::fromUTF8(
                    "鍵盤演奏を開始できませんでした"));
            return;
        }
        // 一時試聴が残っていると鍵盤が未確定音色を鳴らすため、確定へ戻す。
        if (engine_holds_temporary_program_
            && !configureEngine(false)) {
            updateStatus(
                juce::String::fromUTF8(
                    "鍵盤演奏を開始できませんでした"));
            return;
        }
        const auto assignment = voice_allocator_.noteOn(note);
        const auto track = static_cast<std::uint8_t>(
            kOpllTrack + assignment.channel);
        if (assignment.stolen_note) {
            static_cast<void>(engine_.submit(
                mgstc::engine::EngineCommand::noteOff(track)));
        }
        if (!engine_.submit(
                mgstc::engine::EngineCommand::noteOn(
                    track, note))) {
            static_cast<void>(voice_allocator_.noteOff(note));
            updateStatus(
                juce::String::fromUTF8(
                    "鍵盤演奏を開始できませんでした"));
            return;
        }
        updateStatus(
            juce::String::fromUTF8("鍵盤演奏中: OPLL / ")
            + midiNoteName(note));
    }

    void stopPerformanceNote(std::uint8_t note) {
        audition_stop_time_ms_.reset();
        const auto channel = voice_allocator_.noteOff(note);
        if (!channel) {
            return;
        }
        static_cast<void>(engine_.submit(
            mgstc::engine::EngineCommand::noteOff(
                static_cast<std::uint8_t>(kOpllTrack + *channel))));
        if (voice_allocator_.activeVoiceCount() == 0) {
            updateStatus(
                juce::String::fromUTF8("鍵盤演奏を停止しました"));
        }
    }

    [[nodiscard]] bool auditionOneSecond(
        const mgstc::engine::OpllPatchParameters*
            preview = nullptr) {
        if (!engine_ready_
            || !configureEngine(true, preview)) {
            return false;
        }
        performance_keyboard_.showPreviewNote(last_audition_note_);
        audition_stop_time_ms_ =
            juce::Time::getMillisecondCounterHiRes() + 1000.0;
        return true;
    }

    void timerCallback() override {
        synchronizeMasterVolumeSlider(
            master_volume_, master_volume_revision_, audio_service_);
        if (++settings_poll_ticks_ >= 30) {
            settings_poll_ticks_ = 0;
            immediate_audition_.setToggleState(
                loadOpllImmediateAuditionSetting(),
                juce::dontSendNotification);
            const auto shared_note =
                loadLastAuditionNoteSetting();
            if (!performance_keyboard_.hasActiveNote()
                && shared_note != last_audition_note_) {
                last_audition_note_ = shared_note;
                updateAuditionNoteLabels();
                refreshEnvelopeTrace();
            }
        }
        mgstc::engine::OpllScopeFrame frame{};
        if (componentWindowIsActive(*this)) {
            while (engine_.pollOpllScope(frame)) {
                scope_.appendFrame(frame, last_audition_note_);
            }
        }
        if (audition_stop_time_ms_
            && juce::Time::getMillisecondCounterHiRes()
                >= *audition_stop_time_ms_) {
            silenceAllVoices();
            if (engine_holds_temporary_program_) {
                static_cast<void>(configureEngine(false));
            }
            updateStatus(
                juce::String::fromUTF8(
                    "1秒試聴が完了しました"));
        }
    }

    void updateStatus(const juce::String& text) {
        status_.setText(text, juce::dontSendNotification);
    }

    EditorOpenCallback open_editor_;
    SharedAudioService& audio_service_;
    mgstc::engine::RealtimeEngineHost& engine_;
    juce::TooltipWindow tooltip_window_;
    SwitchLookAndFeel switch_look_and_feel_;
    mgstc::engine::OpllPatchParameters patch_;
    std::vector<mgstc::engine::OpllPatchParameters> history_;
    std::size_t history_cursor_{};
    std::vector<mgstc::engine::OpllPatchParameters>
        wave_candidates_;
    std::size_t wave_candidate_index_{};
    OpllOperatorPanel modulator_;
    OpllOperatorPanel carrier_;
    juce::Label common_parameters_title_;
    juce::Label mod_total_level_label_;
    juce::Slider mod_total_level_;
    juce::Label feedback_label_;
    juce::Slider feedback_;
    juce::Rectangle<int> common_parameter_bounds_;
    juce::Rectangle<int> library_panel_bounds_;
    juce::Label title_;
    juce::Label description_;
    juce::DrawableButton load_{
        "load", juce::DrawableButton::ImageOnButtonBackground};
    juce::DrawableButton save_{
        "save", juce::DrawableButton::ImageOnButtonBackground};
    juce::DrawableButton paste_{
        "paste", juce::DrawableButton::ImageOnButtonBackground};
    juce::DrawableButton copy_{
        "copy", juce::DrawableButton::ImageOnButtonBackground};
    juce::DrawableButton undo_{
        "undo", juce::DrawableButton::ImageOnButtonBackground};
    juce::DrawableButton redo_{
        "redo", juce::DrawableButton::ImageOnButtonBackground};
    juce::TextButton import_wave_;
    juce::TextButton import_audacity_;
    juce::TextButton wave_previous_;
    juce::Label wave_candidate_label_;
    juce::TextButton wave_next_;
    juce::ComboBox rom_preset_;
    juce::TextButton rom_load_;
    juce::DrawableButton immediate_audition_{
        "immediate audition",
        juce::DrawableButton::ImageOnButtonBackground};
    juce::TextButton open_scc_;
    juce::DrawableButton convert_to_scc_{
        "convert-to-scc",
        juce::DrawableButton::ImageOnButtonBackground};
    juce::Label scope_title_;
    OpllScopeComponent scope_;
    juce::Label output_number_label_;
    juce::TextEditor output_number_;
    juce::Label mgsc_title_;
    juce::TextEditor mgsc_preview_;
    juce::Label library_title_;
    juce::TextEditor library_filter_;
    juce::ComboBox library_list_;
    juce::TextEditor name_;
    juce::TextEditor tags_;
    juce::TextEditor memo_;
    juce::ToggleButton favorite_;
    juce::TextButton library_new_;
    juce::TextButton library_load_;
    juce::TextButton library_save_;
    juce::TextButton library_save_as_;
    juce::TextButton library_cancel_;
    juce::TextButton library_delete_;
    juce::TextButton library_import_;
    juce::TextButton library_export_;
    juce::Label status_;
    juce::DrawableButton settings_{
        "settings", juce::DrawableButton::ImageOnButtonBackground};
    juce::Slider master_volume_;
    PerformanceKeyboard performance_keyboard_;
    mgstc::engine::SequentialVoiceAllocator voice_allocator_{9};
    mgstc::engine::TimbreLibrary library_;
    std::vector<std::uint64_t> library_ids_;
    std::optional<std::uint64_t> selected_library_id_;
    mgstc::engine::TimbreLibraryEntry editor_baseline_;
    std::optional<std::uint64_t> editor_baseline_id_;
    juce::InterProcessLock library_lock_{
        "MgsToneCraftTimbreLibraryV1"};
    bool editor_baseline_valid_{};
    bool syncing_{};
    bool engine_ready_{};
    bool engine_holds_temporary_program_{};
    std::uint8_t last_audition_note_{kPreviewNote};
    int settings_poll_ticks_{};
    std::uint64_t master_volume_revision_{};
    std::optional<double> audition_stop_time_ms_;
};

enum class SnapshotResult : int {
    Success = 0,
    MissingContent = 10,
    InvalidImage = 11,
    DirectoryFailure = 12,
    StreamFailure = 13,
    PngFailure = 14,
    InvalidPath = 15,
    WindowRoutingFailure = 16,
};

class MainWindow final : public juce::DocumentWindow {
public:
    MainWindow(
        const juce::String& name,
        const juce::String& editor,
        SharedAudioService& audio_service,
        SharedMidiInputService& midi_service,
        EditorOpenCallback open_editor,
        EditorCloseCallback close_editor,
        EditorActivateCallback activate_editor)
        : DocumentWindow(
            name,
            juce::Desktop::getInstance()
                .getDefaultLookAndFeel()
                .findColour(
                    juce::ResizableWindow::backgroundColourId),
            DocumentWindow::allButtons),
          editor_(editor),
          close_editor_(std::move(close_editor)),
          activate_editor_(std::move(activate_editor)) {
        setUsingNativeTitleBar(true);
        if (editor.equalsIgnoreCase("opll")) {
            setContentOwned(
                new OpllEditorComponent(
                    audio_service, midi_service, open_editor), true);
        } else if (editor.equalsIgnoreCase("scc")) {
            setContentOwned(
                new SccEditorComponent(
                    audio_service, midi_service, open_editor), true);
        } else {
            setContentOwned(
                new CompositeEditorComponent(
                    audio_service, midi_service, open_editor), true);
        }
        setResizable(true, false);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
    }

    void prepareVisualInspection() {
        if (auto* editor =
                dynamic_cast<OpllEditorComponent*>(
                    getContentComponent())) {
            editor->prepareVisualInspection();
        } else if (auto* composite =
                       dynamic_cast<CompositeEditorComponent*>(
                           getContentComponent())) {
            composite->prepareVisualInspection();
        }
    }

    void refreshExternalState() {
        if (auto* opll =
                dynamic_cast<OpllEditorComponent*>(
                    getContentComponent())) {
            opll->refreshExternalState();
        } else if (auto* scc =
                       dynamic_cast<SccEditorComponent*>(
                           getContentComponent())) {
            scc->refreshExternalState();
        } else if (auto* composite =
                       dynamic_cast<CompositeEditorComponent*>(
                           getContentComponent())) {
            composite->refreshExternalState();
        }
    }

    void deactivate() {
        if (auto* opll =
                dynamic_cast<OpllEditorComponent*>(
                    getContentComponent())) {
            opll->deactivate();
        } else if (auto* scc =
                       dynamic_cast<SccEditorComponent*>(
                           getContentComponent())) {
            scc->deactivate();
        } else if (auto* composite =
                       dynamic_cast<CompositeEditorComponent*>(
                           getContentComponent())) {
            composite->deactivate();
        }
    }

    [[nodiscard]] SnapshotResult writeContentSnapshot(
        const juce::File& output_file) {
        auto* content = getContentComponent();
        if (content == nullptr || content->getWidth() <= 0
            || content->getHeight() <= 0) {
            return SnapshotResult::MissingContent;
        }
        if (auto* editor =
                dynamic_cast<SccEditorComponent*>(content)) {
            editor->prepareVisualInspection();
        }

        const auto image = content->createComponentSnapshot(
            content->getLocalBounds(),
            true,
            1.0F);
        if (!image.isValid()) {
            return SnapshotResult::InvalidImage;
        }

        if (output_file.getParentDirectory()
                .createDirectory()
                .failed()) {
            return SnapshotResult::DirectoryFailure;
        }
        auto stream = output_file.createOutputStream();
        if (stream == nullptr || !stream->openedOk()) {
            return SnapshotResult::StreamFailure;
        }
        if (!stream->setPosition(0) || !stream->truncate()) {
            return SnapshotResult::StreamFailure;
        }

        juce::PNGImageFormat format;
        return format.writeImageToStream(image, *stream)
            ? SnapshotResult::Success
            : SnapshotResult::PngFailure;
    }

    void closeButtonPressed() override {
        deactivate();
        close_editor_(editor_);
    }

    void activeWindowStatusChanged() override {
        if (isActiveWindow() && activate_editor_) {
            activate_editor_(editor_);
        }
    }

private:
    juce::String editor_;
    EditorCloseCallback close_editor_;
    EditorActivateCallback activate_editor_;
};

class Application final
    : public juce::JUCEApplication,
      private juce::Timer {
public:
    [[nodiscard]] const juce::String
    getApplicationName() override {
        return "MGS Tone Craft";
    }

    [[nodiscard]] const juce::String
    getApplicationVersion() override {
        return "0.1.0";
    }

    void initialise(const juce::String& command_line) override {
        juce::LookAndFeel::setDefaultLookAndFeel(&look_and_feel_);
        audio_service_ = std::make_unique<SharedAudioService>();
        midi_service_ = std::make_unique<SharedMidiInputService>();
        const juce::ArgumentList arguments(
            getApplicationName(),
            command_line);
        auto requested_editor =
            arguments.getValueForOption("--editor");
        if (requested_editor.isEmpty()) {
            requested_editor = "main";
        }
        primary_editor_ = canonicalEditor(requested_editor);
        snapshot_window_ = showEditor(primary_editor_);
        if (arguments.containsOption("--verify-window-routing")) {
            routing_test_mode_ = true;
            auto* const main = showEditor("main");
            auto* const first_scc = showEditor("scc");
            auto* const opll = showEditor("opll");
            closeEditor("scc");
            auto* const reopened_scc = showEditor("scc");
            routing_test_passed_ = main != nullptr
                && first_scc != nullptr
                && opll != nullptr
                && first_scc == reopened_scc
                && main->isVisible()
                && reopened_scc->isVisible()
                && opll->isVisible();
            startTimer(250);
            return;
        }
        const auto snapshot_path = arguments
            .getValueForOption("--capture-ui")
            .trim()
            .trimCharactersAtStart("\"")
            .trimCharactersAtEnd("\"");
        if (snapshot_path.isNotEmpty()) {
            if (!juce::File::isAbsolutePath(snapshot_path)) {
                setApplicationReturnValue(
                    static_cast<int>(SnapshotResult::InvalidPath));
                quit();
                return;
            }
            snapshot_file_ = juce::File::getCurrentWorkingDirectory()
                .getChildFile(snapshot_path);
            snapshot_window_->prepareVisualInspection();
            startTimer(500);
        }
    }

    void shutdown() override {
        stopTimer();
        snapshot_window_ = nullptr;
        opll_window_.reset();
        scc_window_.reset();
        main_window_.reset();
        midi_service_.reset();
        audio_service_.reset();
        juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    }

private:
    [[nodiscard]] static juce::String canonicalEditor(
        const juce::String& editor) {
        if (editor.equalsIgnoreCase("opll")) {
            return "opll";
        }
        if (editor.equalsIgnoreCase("scc")) {
            return "scc";
        }
        return "main";
    }

    [[nodiscard]] std::unique_ptr<MainWindow>& windowSlot(
        const juce::String& editor) {
        if (editor == "opll") {
            return opll_window_;
        }
        if (editor == "scc") {
            return scc_window_;
        }
        return main_window_;
    }

    [[nodiscard]] MainWindow* showEditor(
        const juce::String& requested_editor) {
        const auto editor = canonicalEditor(requested_editor);
        activateEditor(editor);
        auto& window = windowSlot(editor);
        if (window == nullptr) {
            juce::String title = getApplicationName();
            if (editor == "scc") {
                title += juce::String::fromUTF8(" - SCC音色エディタ");
            } else if (editor == "opll") {
                title += juce::String::fromUTF8(" - OPLL音色エディタ");
            }
            window = std::make_unique<MainWindow>(
                title,
                editor,
                *audio_service_,
                *midi_service_,
                [this](const juce::String& target) {
                    static_cast<void>(showEditor(target));
                },
                [this](const juce::String& closed) {
                    closeEditor(closed);
                },
                [this](const juce::String& active) {
                    activateEditor(active);
                });
        }
        window->refreshExternalState();
        window->setVisible(true);
        window->toFront(true);
        window->grabKeyboardFocus();
        return window.get();
    }

    void closeEditor(const juce::String& requested_editor) {
        const auto editor = canonicalEditor(requested_editor);
        if (editor == primary_editor_) {
            quit();
            return;
        }
        if (auto& window = windowSlot(editor); window != nullptr) {
            window->setVisible(false);
        }
    }

    void activateEditor(const juce::String& requested_editor) {
        const auto editor = canonicalEditor(requested_editor);
        const auto deactivate_if_other = [&editor](
            const juce::String& name,
            const std::unique_ptr<MainWindow>& window) {
            if (name != editor && window != nullptr
                && window->isVisible()) {
                window->deactivate();
            }
        };
        deactivate_if_other("main", main_window_);
        deactivate_if_other("scc", scc_window_);
        deactivate_if_other("opll", opll_window_);
        if (midi_service_ != nullptr) {
            midi_service_->clearPendingMessages();
        }
        if (audio_service_ != nullptr) {
            audio_service_->clearScopeFrames();
        }
        active_editor_ = editor;
        if (auto& window = windowSlot(editor); window != nullptr) {
            window->refreshExternalState();
        }
    }

    void timerCallback() override {
        stopTimer();
        if (routing_test_mode_) {
            setApplicationReturnValue(static_cast<int>(
                routing_test_passed_
                    ? SnapshotResult::Success
                    : SnapshotResult::WindowRoutingFailure));
            quit();
            return;
        }
        const auto result = snapshot_window_ != nullptr
            ? snapshot_window_->writeContentSnapshot(snapshot_file_)
            : SnapshotResult::MissingContent;
        setApplicationReturnValue(static_cast<int>(result));
        quit();
    }

    std::unique_ptr<MainWindow> main_window_;
    std::unique_ptr<MainWindow> scc_window_;
    std::unique_ptr<MainWindow> opll_window_;
    MainWindow* snapshot_window_{};
    std::unique_ptr<SharedAudioService> audio_service_;
    std::unique_ptr<SharedMidiInputService> midi_service_;
    juce::String primary_editor_{"main"};
    juce::String active_editor_{"main"};
    bool routing_test_mode_{};
    bool routing_test_passed_{};
    juce::File snapshot_file_;
    MgstcLookAndFeel look_and_feel_;
};

} // namespace

START_JUCE_APPLICATION(Application)
