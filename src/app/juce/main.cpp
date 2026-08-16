// SPDX-License-Identifier: AGPL-3.0-only

#include <array>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <objidl.h>
#include <gdiplus.h>

#pragma comment(lib, "Comctl32.lib")

#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include "BinaryData.h"

#include "asio_audio_sink.hpp"
#include "ui_hang_watchdog.hpp"
#include "mgstc/audio/wasapi_audio_sink.hpp"
#include "mgstc/engine/engine_command.hpp"
#include "mgstc/engine/chip_rack.hpp"
#include "mgstc/engine/composite_timbre.hpp"
#include "mgstc/engine/composite_timbre_library.hpp"
#include "mgstc/engine/mgs_envelope_io.hpp"
#include "mgstc/engine/mgs_timbre_io.hpp"
#include "mgstc/engine/opll_envelope_trace.hpp"
#include "mgstc/engine/opll_patch.hpp"
#include "mgstc/engine/opll_register_auto.hpp"
#include "mgstc/engine/note_pitch.hpp"
#include "mgstc/engine/pc_keyboard.hpp"
#include "mgstc/engine/realtime_engine_host.hpp"
#include "mgstc/engine/scc_waveform.hpp"
#include "mgstc/engine/timbre_library.hpp"
#include "mgstc/engine/timbre_tags.hpp"
#include "mgstc/engine/voice_allocator.hpp"
#include "mgstc/engine/wave_import.hpp"

#include "audio_import_bridge.hpp"

// Supplied by CMake from the SPECIFICATION.md document version.
#ifndef MGSTC_DOC_VERSION
#define MGSTC_DOC_VERSION "0.0"
#endif

namespace {

static_assert(
    sizeof(mgstc::engine::RealtimeEngineHost) < 128 * 1024,
    "RealtimeEngineHost must remain safe to create in an editor");

constexpr std::uint8_t kPsgTrack = 0;
constexpr std::uint8_t kSccTrack = 3;
constexpr std::uint8_t kOpllTrack = 8;
constexpr std::uint8_t kPreviewNote = 60;
// Editor Open/Paste/Settings icon over; shared UI hover accent.
constexpr juce::uint32 kUiHoverAccent = 0xFF53E3A6;
// PC演奏オクターブ移動は Page Down（下降）／Page Up（上昇）。
// 拡張キーのためスキャンコードではなく仮想キーで判定する。
constexpr int kPcOctaveDownVirtualKey = VK_NEXT;
constexpr int kPcOctaveUpVirtualKey = VK_PRIOR;
constexpr std::array<std::uint16_t, 33>
    kPcPerformanceScanCodes{
        0x2C, 0x1F, 0x2D, 0x20, 0x2E, 0x2F, 0x22, 0x30,
        0x23, 0x31, 0x24, 0x32,
        0x10, 0x03, 0x11, 0x04, 0x12, 0x13, 0x06, 0x14,
        0x07, 0x15, 0x08, 0x16, 0x17, 0x0A, 0x18, 0x0B,
        0x19, 0x1A, 0x0D, 0x1B, 0x7D};

using EditorOpenCallback =
    std::function<void(
        const juce::String&,
        std::optional<std::uint64_t>)>;
using EditorCloseCallback =
    std::function<void(const juce::String&)>;
using EditorActivateCallback =
    std::function<void(const juce::String&)>;
using TagManagementCallback =
    std::function<void(juce::Component*)>;

// Close-button / client clicks leave ModifierKeys with a button down until
// WM_*BUTTONUP. Caption (HTCAPTION) clicks are different: JUCE never forwards
// NCLBUTTONDOWN to DefWindowProc until the cursor *moves*, so button flags can
// stay "down" until move — do NOT use this helper for title-bar activation.
//
// Caption vs border (Windows peer): onNcLButtonDown returns 0 for HTCAPTION and
// only DefWindowProc's after mouse-move; HTBORDER/size hits fall through to
// DefWindowProc immediately (activate + raise). TopLevelWindowManager also keys
// "active" off keyboard focus, so caption-only clicks never reach
// activeWindowStatusChanged / toFront — raise Z-order in an HWND subclass.
// Deadline so a button flag that never clears (capture lost to another app)
// cannot swallow the callback forever: close/quit confirmation sets a pending
// flag first, and a never-shown dialog looks exactly like a frozen app.
constexpr int kMouseReleaseWaitPollMs = 16;
constexpr int kMouseReleaseWaitLimitMs = 2000;

void runWhenMouseReleased(
    std::function<void()> callback,
    int waited_ms = 0) {
    if (waited_ms < kMouseReleaseWaitLimitMs
        && juce::ModifierKeys::getCurrentModifiersRealtime()
               .isAnyMouseButtonDown()) {
        juce::Timer::callAfterDelay(
            kMouseReleaseWaitPollMs,
            [callback = std::move(callback), waited_ms]() mutable {
                runWhenMouseReleased(
                    std::move(callback),
                    waited_ms + kMouseReleaseWaitPollMs);
            });
        return;
    }
    callback();
}

void showAlertWhenMouseReleased(
    juce::MessageBoxOptions options,
    std::function<void(int)> callback) {
    runWhenMouseReleased(
        [options = std::move(options),
         callback = std::move(callback)]() mutable {
            juce::AlertWindow::showAsync(
                std::move(options), std::move(callback));
        });
}

void showDiscardConfirmation(
    juce::Component* parent,
    const juce::String& action,
    std::function<void()> continue_action) {
    juce::AlertWindow::showAsync(
        juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::WarningIcon)
            .withTitle(
                juce::String::fromUTF8("未保存の変更があります"))
            .withMessage(
                juce::String::fromUTF8(
                    "現在の編集内容は保存されていません。\n")
                + action
                + juce::String::fromUTF8("を続けますか？"))
            .withButton(
                juce::String::fromUTF8("保存せず続行"))
            .withButton(juce::String::fromUTF8("キャンセル"))
            .withAssociatedComponent(parent),
        [continue_action = std::move(continue_action)](int result) {
            if (result == 1 && continue_action) {
                continue_action();
            }
        });
}

[[nodiscard]] juce::File applicationDataDirectory() {
    return juce::File::getSpecialLocation(
               juce::File::userApplicationDataDirectory)
        .getChildFile("MgsToneCraft");
}

// Coalesces LastAuditionNote INI writes off the note-on hot path.
class LastAuditionNoteStore final {
public:
    [[nodiscard]] static LastAuditionNoteStore& instance() {
        static LastAuditionNoteStore store;
        return store;
    }

    [[nodiscard]] std::uint8_t get() const noexcept {
        ensureLoaded();
        return static_cast<std::uint8_t>(juce::jlimit(
            24,
            119,
            cached_note_.load(std::memory_order_relaxed)));
    }

    void mark(std::uint8_t note) noexcept {
        ensureLoaded();
        const int clamped = juce::jlimit(24, 119, static_cast<int>(note));
        cached_note_.store(clamped, std::memory_order_relaxed);
        dirty_.store(true, std::memory_order_release);
    }

    void flushIfDirty() {
        if (!dirty_.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        const auto file = applicationDataDirectory().getChildFile(
            "settings-v1.ini");
        if (file.getParentDirectory().createDirectory().failed()) {
            dirty_.store(true, std::memory_order_release);
            return;
        }
        const auto path = file.getFullPathName();
        const auto value = juce::String(
            cached_note_.load(std::memory_order_relaxed));
        static_cast<void>(WritePrivateProfileStringW(
            L"Application",
            L"SchemaVersion",
            L"1",
            path.toWideCharPointer()));
        static_cast<void>(WritePrivateProfileStringW(
            L"Application",
            L"LastAuditionNote",
            value.toWideCharPointer(),
            path.toWideCharPointer()));
        // Flush once on the deferred path, never on note-on.
        static_cast<void>(WritePrivateProfileStringW(
            nullptr, nullptr, nullptr, path.toWideCharPointer()));
    }

private:
    LastAuditionNoteStore() = default;
    ~LastAuditionNoteStore() { flushIfDirty(); }

    void ensureLoaded() const {
        if (loaded_.load(std::memory_order_acquire)) {
            return;
        }
        const auto file = applicationDataDirectory().getChildFile(
            "settings-v1.ini");
        int value = kPreviewNote;
        if (file.existsAsFile()) {
            value = static_cast<int>(GetPrivateProfileIntW(
                L"Application",
                L"LastAuditionNote",
                kPreviewNote,
                file.getFullPathName().toWideCharPointer()));
        }
        cached_note_.store(
            juce::jlimit(24, 119, value),
            std::memory_order_relaxed);
        loaded_.store(true, std::memory_order_release);
    }

    mutable std::atomic<bool> loaded_{false};
    mutable std::atomic<int> cached_note_{kPreviewNote};
    std::atomic<bool> dirty_{false};
};

[[nodiscard]] juce::File compositeTimbreLibraryFile() {
    return applicationDataDirectory().getChildFile(
        "composite-timbre-library-v1.mgstc");
}

[[nodiscard]] juce::File timbreLibraryFile() {
    return applicationDataDirectory().getChildFile(
        "timbre-library-v1.mgstc");
}

// Disk fingerprint for skipping unchanged library reloads on focus.
struct LibraryFileFingerprint {
    bool present{};
    std::int64_t mtime_ms{};
    std::int64_t size{};

    friend bool operator==(
        const LibraryFileFingerprint&,
        const LibraryFileFingerprint&) = default;
};

[[nodiscard]] LibraryFileFingerprint libraryFileFingerprint(
    const juce::File& file) {
    if (!file.existsAsFile()) {
        return {};
    }
    return {
        .present = true,
        .mtime_ms = file.getLastModificationTime().toMilliseconds(),
        .size = file.getSize(),
    };
}

[[nodiscard]] std::string utf8String(const juce::String& text) {
    const auto utf8 = text.toUTF8();
    return {
        utf8.getAddress(),
        static_cast<std::size_t>(utf8.sizeInBytes() - 1)};
}

[[nodiscard]] std::int64_t currentUnixTime() {
    return static_cast<std::int64_t>(
        juce::Time::getCurrentTime().toMilliseconds() / 1000);
}

// U+25B6 BLACK RIGHT-POINTING TRIANGLE as explicit UTF-8 bytes.
// Keep A/B labels independent of source-file encoding while still
// rendering the play glyph through juce::String::fromUTF8.
[[nodiscard]] juce::String abNextSideButtonText(bool next_is_a) {
    return juce::String::fromUTF8(
        next_is_a ? "\xE2\x96\xB6" "A" : "\xE2\x96\xB6" "B");
}

[[nodiscard]] bool sameEditableTimbreLibraryEntry(
    const mgstc::engine::TimbreLibraryEntry& left,
    const mgstc::engine::TimbreLibraryEntry& right) {
    return left.category == right.category
        && left.name == right.name
        && left.tags == right.tags
        && left.memo == right.memo
        && left.favorite == right.favorite
        && left.opll_registers == right.opll_registers
        && left.scc_waveform == right.scc_waveform;
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

[[nodiscard]] std::optional<mgstc::engine::TimbreLibrary>
loadTimbreLibrary(std::string* error = nullptr) {
    const auto file = timbreLibraryFile();
    if (!file.existsAsFile()) {
        return mgstc::engine::TimbreLibrary{};
    }
    return mgstc::engine::TimbreLibrary::deserialize(
        utf8String(file.loadFileAsString()), error);
}

[[nodiscard]] bool persistTimbreLibrary(
    const mgstc::engine::TimbreLibrary& library) {
    const auto target = timbreLibraryFile();
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
        if (const auto* editor =
                dynamic_cast<const juce::TextEditor*>(component);
            editor != nullptr && !editor->isReadOnly()) {
            // 読み取り専用のMGSCプレビューは入力欄とみなさない。
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

[[nodiscard]] bool isCommandLetter(
    const juce::KeyPress& key, int letter_lower) {
    if (!key.getModifiers().isCommandDown()
        || key.getModifiers().isAltDown()) {
        return false;
    }
    const auto code = key.getKeyCode();
    return code == letter_lower
        || code == (letter_lower - 32); // 'c'/'C'
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

[[nodiscard]] bool virtualKeyIsDown(int virtual_key) {
    return virtual_key != 0
        && (GetAsyncKeyState(virtual_key) & 0x8000) != 0;
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
        EditorIcon::Settings, juce::Colour(kUiHoverAccent));
    const auto down = makeEditorIcon(
        EditorIcon::Settings, juce::Colour(0xFF2AD6C9));
    button.setImages(normal.get(), over.get(), down.get());
    button.setTooltip(
        juce::String::fromUTF8("アプリ設定を開きます"));
    button.onClick = std::move(action);
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

[[nodiscard]] juce::Image loadImageMemory(
    const void* data,
    std::size_t size) {
    if (data == nullptr || size == 0) {
        return {};
    }
    if (auto image = juce::ImageFileFormat::loadFrom(data, size);
        image.isValid()) {
        return image;
    }
    ensureGdiplusInitialized();
    const auto memory = ::GlobalAlloc(
        GMEM_MOVEABLE, static_cast<SIZE_T>(size));
    if (memory == nullptr) {
        return {};
    }
    if (auto* locked = ::GlobalLock(memory)) {
        std::memcpy(locked, data, size);
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
    return loadImageMemory(bytes.data(), bytes.size());
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

    // GIFのフレーム遅延はPropertyTagFrameDelayに1/100秒単位で格納される。
    // タグがない、値が0、または読込に失敗したフレームだけ50msへフォールバックする。
    constexpr PROPID frame_delay_property = 0x5100;
    constexpr int fallback_delay_ms = 50;
    std::vector<std::uint64_t> delay_storage;
    const auto delay_property_size =
        bitmap.GetPropertyItemSize(frame_delay_property);
    if (delay_property_size > 0) {
        delay_storage.resize(
            (static_cast<std::size_t>(delay_property_size)
             + sizeof(std::uint64_t) - 1)
            / sizeof(std::uint64_t));
    }
    const Gdiplus::PropertyItem* delay_property = nullptr;
    if (!delay_storage.empty()) {
        auto* item = reinterpret_cast<Gdiplus::PropertyItem*>(
            delay_storage.data());
        if (bitmap.GetPropertyItem(
                frame_delay_property,
                delay_property_size,
                item)
            == Gdiplus::Ok) {
            delay_property = item;
        }
    }
    const auto delayForFrame = [delay_property](UINT frame) {
        if (delay_property == nullptr
            || delay_property->value == nullptr
            || delay_property->type
                != 4) {  // PropertyTagTypeLong
            return fallback_delay_ms;
        }
        const auto delay_count =
            delay_property->length / sizeof(UINT);
        if (frame >= delay_count) {
            return fallback_delay_ms;
        }
        const auto hundredths =
            static_cast<const UINT*>(delay_property->value)[frame];
        if (hundredths == 0) {
            return fallback_delay_ms;
        }
        constexpr auto maximum_hundredths =
            static_cast<UINT>(
                std::numeric_limits<int>::max() / 10);
        return static_cast<int>(
            juce::jmin(hundredths, maximum_hundredths) * 10);
    };

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
        delays_ms.push_back(delayForFrame(frame));
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

enum class OpllConversionQuality {
    standard,
    thorough,
};

// Discrete UI scale (View setting + optional per-editor session override).
// Global percent is INI-persisted; layout/fonts read the active factor via UiMetrics.
namespace UiScale {
constexpr int kDefaultPercent = 100;
constexpr std::array<int, 3> kSteps{75, 100, 125};

inline int global_percent = kDefaultPercent;
inline int active_percent = kDefaultPercent;
inline float active_factor = 1.0F;

inline std::vector<std::function<void()>> global_listeners;

[[nodiscard]] inline juce::File settingsIniFile() {
    return applicationDataDirectory().getChildFile("settings-v1.ini");
}

[[nodiscard]] inline int nearestStep(int percent) noexcept {
    int best = kDefaultPercent;
    int best_dist = std::abs(percent - best);
    for (const int step : kSteps) {
        const int dist = std::abs(percent - step);
        if (dist < best_dist) {
            best = step;
            best_dist = dist;
        }
    }
    return best;
}

[[nodiscard]] inline bool isStep(int percent) noexcept {
    for (const int step : kSteps) {
        if (step == percent) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline float factorFromPercent(int percent) noexcept {
    return static_cast<float>(nearestStep(percent)) / 100.0F;
}

[[nodiscard]] inline int nextLarger(int percent) noexcept {
    const int clamped = nearestStep(percent);
    for (const int step : kSteps) {
        if (step > clamped) {
            return step;
        }
    }
    return clamped;
}

[[nodiscard]] inline int nextSmaller(int percent) noexcept {
    const int clamped = nearestStep(percent);
    for (int i = static_cast<int>(kSteps.size()) - 1; i >= 0; --i) {
        if (kSteps[static_cast<std::size_t>(i)] < clamped) {
            return kSteps[static_cast<std::size_t>(i)];
        }
    }
    return clamped;
}

[[nodiscard]] inline int sx(int value) noexcept {
    return juce::jmax(
        1, juce::roundToInt(static_cast<float>(value) * active_factor));
}

void applyLayoutMetrics(float factor); // defined after UiLayout

inline void setActivePercent(int percent) {
    active_percent = nearestStep(percent);
    active_factor = factorFromPercent(active_percent);
    applyLayoutMetrics(active_factor);
}

inline void loadGlobalFromIni() {
    const auto file = settingsIniFile();
    int value = kDefaultPercent;
    if (file.existsAsFile()) {
        value = static_cast<int>(GetPrivateProfileIntW(
            L"Application",
            L"UiScalePercent",
            kDefaultPercent,
            file.getFullPathName().toWideCharPointer()));
    }
    // Only exact steps {75,100,125} are accepted. Invalid values (including
    // legacy 50) stay at default 100% for this session; do not rewrite INI.
    global_percent = isStep(value) ? value : kDefaultPercent;
    setActivePercent(global_percent);
}

inline void saveGlobalToIni() {
    const auto file = settingsIniFile();
    if (file.getParentDirectory().createDirectory().failed()) {
        return;
    }
    const auto path = file.getFullPathName();
    const auto value = juce::String(global_percent);
    static_cast<void>(WritePrivateProfileStringW(
        L"Application",
        L"SchemaVersion",
        L"1",
        path.toWideCharPointer()));
    static_cast<void>(WritePrivateProfileStringW(
        L"Application",
        L"UiScalePercent",
        value.toWideCharPointer(),
        path.toWideCharPointer()));
    static_cast<void>(WritePrivateProfileStringW(
        nullptr, nullptr, nullptr, path.toWideCharPointer()));
}

inline void addGlobalListener(std::function<void()> listener) {
    global_listeners.push_back(std::move(listener));
}

inline void notifyGlobalListeners() {
    for (auto& listener : global_listeners) {
        if (listener) {
            listener();
        }
    }
}

// Persist + apply to metrics, then refresh every registered root (editors + dialogs).
inline void setGlobalPercent(int percent, bool persist) {
    global_percent = nearestStep(percent);
    if (persist) {
        saveGlobalToIni();
    }
    setActivePercent(global_percent);
    notifyGlobalListeners();
}

// Settings / library / tags / other dialogs must never inherit an editor's
// Ctrl± session override via process-global metrics.
inline void forceGlobalForNonEditorUi() {
    setActivePercent(global_percent);
}

[[nodiscard]] inline bool modalUiBlocksEditorScale() {
    return juce::ModalComponentManager::getInstance()
               ->getNumModalComponents()
        > 0;
}

// Only the active editor window may push session scale into shared metrics.
// Background editors and any open modal must not leak Ctrl± into dialogs or
// newly created windows.
[[nodiscard]] inline bool editorMayDriveProcessScale(
    const juce::Component& editor) {
    if (modalUiBlocksEditorScale()) {
        return false;
    }
    if (auto* top = editor.getTopLevelComponent()) {
        if (auto* window =
                dynamic_cast<const juce::TopLevelWindow*>(top)) {
            return window->isActiveWindow();
        }
    }
    return false;
}

inline void syncEditorDrivenActivePercent(
    const juce::Component& editor,
    int effective_percent) {
    if (modalUiBlocksEditorScale()) {
        // Any modal (settings, library, tags, AlertWindow, …) owns the UI
        // context — keep process metrics on View/INI global, never Ctrl±.
        forceGlobalForNonEditorUi();
        return;
    }
    if (editorMayDriveProcessScale(editor)) {
        setActivePercent(effective_percent);
    }
}

[[nodiscard]] inline bool tryHandleEditorScaleKey(
    const juce::KeyPress& key,
    std::optional<int>& session_override_percent,
    const std::function<void(int effective_percent)>& apply) {
    if (!key.getModifiers().isCommandDown()
        || key.getModifiers().isAltDown()) {
        return false;
    }
    const auto code = key.getKeyCode();
    // Keep Ctrl+C/V/Z/Y for timbre edit; only handle zoom keys here.
    if (code == 'c' || code == 'C' || code == 'v' || code == 'V'
        || code == 'z' || code == 'Z' || code == 'y' || code == 'Y') {
        return false;
    }
    const int current = session_override_percent.value_or(global_percent);
    if (code == '=' || code == '+'
        || code == juce::KeyPress::numberPadAdd) {
        session_override_percent = nextLarger(current);
        apply(*session_override_percent);
        return true;
    }
    if (code == '-' || code == juce::KeyPress::numberPadSubtract) {
        session_override_percent = nextSmaller(current);
        apply(*session_override_percent);
        return true;
    }
    if (code == '0' || code == juce::KeyPress::numberPad0) {
        session_override_percent.reset();
        apply(global_percent);
        return true;
    }
    return false;
}
} // namespace UiScale

// UI type scale — exactly four roles (px heights; face = Windows message font).
//   title   : window / page title
//   heading : panel / section header
//   body    : buttons, labels, combos, dialogs
//   dense   : timbre params, MGSC preview, keyboard C*, DEC/HEX, graph chrome
namespace UiFonts {
[[nodiscard]] inline juce::String windowsMessageFaceName() {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoW(
            SPI_GETNONCLIENTMETRICS,
            sizeof(metrics),
            &metrics,
            0)
        != FALSE) {
        return juce::String(metrics.lfMessageFont.lfFaceName);
    }
    return "Segoe UI";
}

[[nodiscard]] inline float unscaledBodyHeight() {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    float pixels = 18.0F;
    if (SystemParametersInfoW(
            SPI_GETNONCLIENTMETRICS,
            sizeof(metrics),
            &metrics,
            0)
        != FALSE) {
        pixels = static_cast<float>(
            std::abs(metrics.lfMessageFont.lfHeight));
    }
    // One step above classic JUCE body (~15–16px) / prior MGSTC body (16).
    return juce::jmax(18.0F, pixels);
}

[[nodiscard]] inline float bodyHeight() {
    // Floor so 75% remains usable where possible.
    return juce::jmax(12.0F, unscaledBodyHeight() * UiScale::active_factor);
}

[[nodiscard]] inline float headingHeight() {
    return juce::jmax(13.0F, bodyHeight() * 1.15F);
}

[[nodiscard]] inline float titleHeight() {
    return juce::jmax(
        14.0F,
        juce::jmax(26.0F, unscaledBodyHeight() * 1.45F)
            * UiScale::active_factor);
}

[[nodiscard]] inline float denseHeight() {
    // ~14.6px at body=18; timeline/OPLL EG frames are ≥16px at 100%.
    return juce::jmax(10.0F, bodyHeight() * 0.8125F);
}

// Match LookAndFeel_V4 TextButton: min(heading, height×0.6), never below body.
[[nodiscard]] inline float controlTextHeight(float control_height_px) {
    const auto sized = juce::jmin(
        headingHeight(),
        control_height_px * 0.6F);
    return juce::jmax(bodyHeight(), sized);
}

[[nodiscard]] inline juce::Font make(
    float height_px,
    bool bold = false) {
    return juce::Font(
        juce::FontOptions(
            windowsMessageFaceName(),
            height_px,
            bold ? juce::Font::bold : juce::Font::plain));
}

[[nodiscard]] inline juce::Font title() {
    return make(titleHeight(), true);
}

[[nodiscard]] inline juce::Font heading() {
    return make(headingHeight(), true);
}

[[nodiscard]] inline juce::Font body(bool bold = false) {
    return make(bodyHeight(), bold);
}

[[nodiscard]] inline juce::Font dense(bool bold = false) {
    return make(denseHeight(), bold);
}

[[nodiscard]] inline juce::Font mono() {
    return juce::Font(
        juce::FontOptions(
            juce::Font::getDefaultMonospacedFontName(),
            denseHeight(),
            juce::Font::plain));
}

inline void setMgscPreviewText(
    juce::TextEditor& editor,
    const juce::String& text) {
    const auto font = mono();
    editor.setFont(font);
    editor.setText(text, false);
    editor.applyFontToAllText(font);
}

inline void refreshMgscPreviewFont(juce::TextEditor& editor) {
    const auto font = mono();
    editor.setFont(font);
    editor.applyFontToAllText(font);
}

// Compatibility aliases used across the UI.
[[nodiscard]] inline float height(float relative = 1.0F) {
    return bodyHeight() * relative;
}

[[nodiscard]] inline float parameterHeight() {
    return bodyHeight();
}

[[nodiscard]] inline float windowsMessageHeightPx() {
    return bodyHeight();
}

[[nodiscard]] inline juce::Font ui(
    float relative = 1.0F,
    bool bold = false) {
    if (std::abs(relative - 1.0F) < 0.001F) {
        return body(bold);
    }
    return make(bodyHeight() * relative, bold);
}

[[nodiscard]] inline juce::Font uiBold(float relative = 1.0F) {
    return ui(relative, true);
}

[[nodiscard]] inline juce::Font section() {
    return heading();
}

[[nodiscard]] inline juce::Font parameter(bool bold = false) {
    return body(bold);
}

[[nodiscard]] inline juce::Font compact() {
    return body();
}

inline void styleBodyField(juce::TextEditor& editor) {
    editor.setFont(body());
    editor.applyFontToAllText(body());
}

inline void styleDenseField(juce::TextEditor& editor) {
    editor.setFont(dense());
    editor.applyFontToAllText(dense());
}
} // namespace UiFonts


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
        // Design tokens match UiLayout::Base::switch* (scaled via UiScale).
        // Keep magic bases identical to UiLayout::Base::{switchTrackW,H,Thumb,LabelPad}.
        const auto track_w =
            static_cast<float>(UiScale::sx(34));
        const auto track_h =
            static_cast<float>(UiScale::sx(18));
        const auto thumb =
            static_cast<float>(UiScale::sx(14));
        const auto label_pad =
            static_cast<float>(UiScale::sx(8));
        const auto track_pad =
            juce::jmax(1.0F, (track_h - thumb) * 0.5F);
        const auto switch_bounds = juce::Rectangle<float>(
            bounds.getX(),
            bounds.getCentreY() - track_h * 0.5F,
            track_w,
            track_h);
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
        graphics.fillRoundedRectangle(
            switch_bounds, track_h * 0.5F);

        const auto thumb_x = active
            ? switch_bounds.getRight() - thumb - track_pad
            : switch_bounds.getX() + track_pad;
        graphics.setColour(
            juce::Colours::white.withMultipliedAlpha(
                enabled ? 0.95F : 0.45F));
        graphics.fillEllipse(
            thumb_x,
            switch_bounds.getY() + track_pad,
            thumb,
            thumb);

        const auto label_colour =
            (should_draw_highlight && enabled)
                ? juce::Colour(kUiHoverAccent)
                : juce::Colour(0xFFE6EDF3);
        graphics.setColour(
            label_colour.withMultipliedAlpha(enabled ? 1.0F : 0.45F));
        const auto font = UiFonts::make(
            UiFonts::controlTextHeight(
                static_cast<float>(button.getHeight())));
        graphics.setFont(font);
        // Track | labelPad | label — same formula as UiLayout::switchControlWidth.
        const auto label_bounds = juce::Rectangle<float>(
            switch_bounds.getRight() + label_pad,
            switch_bounds.getY(),
            juce::jmax(
                0.0F,
                bounds.getRight()
                    - (switch_bounds.getRight() + label_pad)),
            switch_bounds.getHeight());
        graphics.drawFittedText(
            button.getButtonText(),
            label_bounds.toNearestInt(),
            juce::Justification::centredLeft,
            1);

        if (button.hasKeyboardFocus(true)) {
            graphics.setColour(juce::Colour(kUiHoverAccent));
            graphics.drawRoundedRectangle(
                bounds.reduced(0.5F),
                static_cast<float>(UiScale::sx(4)),
                1.0F);
        }
    }
};

class OpllConversionQualityContent final : public juce::Component {
public:
    using ConvertCallback =
        std::function<void(OpllConversionQuality)>;

    explicit OpllConversionQualityContent(ConvertCallback callback)
        : callback_(std::move(callback)) {
        standard_label_.setText(
            juce::String::fromUTF8("標準"),
            juce::dontSendNotification);
        standard_label_.setJustificationType(
            juce::Justification::centredRight);
        addAndMakeVisible(standard_label_);

        quality_toggle_.setButtonText(
            juce::String::fromUTF8("じっくり（最大約2分）"));
        quality_toggle_.setTitle(juce::String::fromUTF8(
            "OPLL変換品質：標準（オフ）／じっくり（オン）"));
        quality_toggle_.setTooltip(juce::String::fromUTF8(
            "OPLL変換品質を切り替えます。"
            "オフは標準、オンはじっくり（最大約2分）です"));
        quality_toggle_.setToggleState(
            false, juce::dontSendNotification);
        quality_toggle_.setLookAndFeel(&switch_look_and_feel_);
        addAndMakeVisible(quality_toggle_);

        convert_.setButtonText(juce::String::fromUTF8("変換"));
        convert_.onClick = [this] {
            const auto quality = quality_toggle_.getToggleState()
                ? OpllConversionQuality::thorough
                : OpllConversionQuality::standard;
            auto callback = std::move(callback_);
            closeDialog(1);
            if (callback) {
                callback(quality);
            }
        };
        addAndMakeVisible(convert_);

        cancel_.setButtonText(juce::String::fromUTF8("キャンセル"));
        cancel_.onClick = [this] { closeDialog(0); };
        addAndMakeVisible(cancel_);
        setSize(UiScale::sx(400), UiScale::sx(150));
    }

    ~OpllConversionQualityContent() override {
        quality_toggle_.setLookAndFeel(nullptr);
    }

    void resized() override {
        auto area = getLocalBounds().reduced(UiScale::sx(18));
        auto quality = area.removeFromTop(UiScale::sx(34));
        standard_label_.setBounds(
            quality.removeFromLeft(UiScale::sx(86)));
        quality.removeFromLeft(UiScale::sx(8));
        quality_toggle_.setBounds(quality);
        auto buttons = area.removeFromBottom(UiScale::sx(36));
        cancel_.setBounds(buttons.removeFromRight(UiScale::sx(112)));
        buttons.removeFromRight(UiScale::sx(6));
        convert_.setBounds(buttons.removeFromRight(UiScale::sx(112)));
    }

private:
    void closeDialog(int result) {
        if (auto* dialog =
                findParentComponentOfClass<juce::DialogWindow>()) {
            dialog->exitModalState(result);
        }
    }

    ConvertCallback callback_;
    SwitchLookAndFeel switch_look_and_feel_;
    juce::Label standard_label_;
    juce::ToggleButton quality_toggle_;
    juce::TextButton convert_;
    juce::TextButton cancel_;
};

void showOpllConversionQualityDialog(
    juce::Component* anchor,
    OpllConversionQualityContent::ConvertCallback callback);

struct ConversionProgressState {
    std::shared_ptr<mgstc::engine::OpllApproximationControl> control{
        std::make_shared<
            mgstc::engine::OpllApproximationControl>()};

    void requestCancellation() const noexcept {
        control->requestCancel();
    }

    [[nodiscard]] bool cancellationRequested() const noexcept {
        return control->cancelRequested();
    }

    [[nodiscard]] double progressFraction() const noexcept {
        const auto current = control->progress();
        if (current.phase
            == mgstc::engine::OpllApproximationPhase::Completed) {
            return 1.0;
        }
        if (current.total == 0) {
            return 0.0;
        }
        return static_cast<double>(current.completed)
            / static_cast<double>(current.total);
    }
};

[[nodiscard]] mgstc::engine::OpllApproximationOptions
makeOpllApproximationOptions(
    OpllConversionQuality quality,
    const ConversionProgressState& progress) {
    mgstc::engine::OpllApproximationOptions options;
    options.effort = quality == OpllConversionQuality::thorough
        ? mgstc::engine::OpllApproximationEffort::Thorough
        : mgstc::engine::OpllApproximationEffort::Standard;
    options.control = progress.control;
    return options;
}

class ConversionBusyContent final
    : public juce::Component,
      private juce::Timer {
public:
    explicit ConversionBusyContent(
        std::shared_ptr<ConversionProgressState> state)
        : state_(std::move(state)),
          progress_bar_(displayed_progress_) {
        label_.setText(
            juce::String::fromUTF8("変換中…"),
            juce::dontSendNotification);
        label_.setJustificationType(juce::Justification::centred);
        label_.setColour(
            juce::Label::textColourId, juce::Colour(0xFFE6EDF3));
        addAndMakeVisible(label_);

        addAndMakeVisible(progress_bar_);
        progress_bar_.setColour(
            juce::ProgressBar::backgroundColourId,
            juce::Colour(0xFF26323C));
        progress_bar_.setColour(
            juce::ProgressBar::foregroundColourId,
            juce::Colour(0xFF53E3A6));

        gif_.loadFromMemory(
            BinaryData::mgstc_spin_gif,
            BinaryData::mgstc_spin_gifSize);
        // Source GIF is 512x512; show at about 50%.
        gif_.setDisplaySize(256, 256);
        addAndMakeVisible(gif_);

        cancel_.setButtonText(juce::String::fromUTF8("キャンセル"));
        cancel_.onClick = [this] { requestCancellation(); };
        addAndMakeVisible(cancel_);

        startTimerHz(30);
        setSize(UiScale::sx(320), UiScale::sx(402));
    }

    ~ConversionBusyContent() override {
        stopTimer();
    }

    void requestCancellation() {
        state_->requestCancellation();
        label_.setText(
            juce::String::fromUTF8("キャンセル中…"),
            juce::dontSendNotification);
        cancel_.setEnabled(false);
    }

    void resized() override {
        auto area = getLocalBounds().reduced(16);
        label_.setBounds(area.removeFromTop(28));
        area.removeFromTop(8);
        progress_bar_.setBounds(area.removeFromTop(22));
        area.removeFromTop(8);
        auto buttons = area.removeFromBottom(34);
        cancel_.setBounds(buttons.withSizeKeepingCentre(120, 34));
        area.removeFromBottom(8);
        gif_.setBounds(
            area.withSizeKeepingCentre(256, 256));
    }

private:
    void timerCallback() override {
        displayed_progress_ = juce::jlimit(
            0.0,
            1.0,
            state_->progressFraction());
    }

    std::shared_ptr<ConversionProgressState> state_;
    double displayed_progress_{0.0};
    juce::Label label_;
    juce::ProgressBar progress_bar_;
    AnimatedGifComponent gif_;
    juce::TextButton cancel_;
};

class ConversionBusyDialog final : public juce::DialogWindow {
public:
    explicit ConversionBusyDialog(
        std::shared_ptr<ConversionProgressState> state)
        : juce::DialogWindow(
              juce::String::fromUTF8("変換中"),
              juce::Colour(0xFF1B222C),
              false,
              true),
          state_(std::move(state)) {
        setUsingNativeTitleBar(true);
        setResizable(false, false);
        auto* content = new ConversionBusyContent(state_);
        content_ = content;
        setContentOwned(content, true);
        centreWithSize(content->getWidth(), content->getHeight() + 32);
    }

    ~ConversionBusyDialog() override {
        requestCancellation();
    }

    void keepTaskAlive(std::shared_ptr<void> task) {
        task_ = std::move(task);
    }

    void closeButtonPressed() override {
        requestCancellation();
    }

private:
    void requestCancellation() {
        state_->requestCancellation();
        if (content_ != nullptr) {
            content_->requestCancellation();
        }
    }

    std::shared_ptr<ConversionProgressState> state_;
    ConversionBusyContent* content_{};
    std::shared_ptr<void> task_;
};

// juce::DialogWindow の既定の閉じるボタンは非表示にするだけでモーダルを
// 解除しないため、タイトルバーの✘でも閉じられるようにする。
class ModalDialogWindow final : public juce::DialogWindow {
public:
    ModalDialogWindow(
        const juce::String& name,
        juce::Colour background)
        : juce::DialogWindow(name, background, true, true) {}

    void closeButtonPressed() override {
        exitModalState(0);
    }

    void activeWindowStatusChanged() override {
        juce::DialogWindow::activeWindowStatusChanged();
        if (!isActiveWindow()) {
            return;
        }
        // Keep LookAndFeel / UiFonts / UiLayout on View (global) scale while
        // any non-editor dialog is the active surface.
        UiScale::forceGlobalForNonEditorUi();
        if (auto* content = getContentComponent()) {
            content->sendLookAndFeelChange();
            content->resized();
            content->repaint();
        }
    }

    void resized() override {
        UiScale::forceGlobalForNonEditorUi();
        juce::DialogWindow::resized();
    }

    void paint(juce::Graphics& graphics) override {
        UiScale::forceGlobalForNonEditorUi();
        juce::DialogWindow::paint(graphics);
    }
};

void showOpllConversionQualityDialog(
    juce::Component* anchor,
    OpllConversionQualityContent::ConvertCallback callback) {
    UiScale::forceGlobalForNonEditorUi();
    auto* dialog = new ModalDialogWindow(
        juce::String::fromUTF8("OPLL変換品質"),
        juce::Colour(0xFF1B222C));
    auto* content =
        new OpllConversionQualityContent(std::move(callback));
    dialog->setUsingNativeTitleBar(true);
    dialog->setResizable(false, false);
    dialog->setContentOwned(content, true);
    if (anchor != nullptr) {
        dialog->centreAroundComponent(
            anchor, content->getWidth(), content->getHeight() + 32);
    } else {
        dialog->centreWithSize(
            content->getWidth(), content->getHeight() + 32);
    }
    dialog->enterModalState(true, nullptr, true);
}

struct TagChoice {
    std::string name;
    std::size_t count{};
};

[[nodiscard]] juce::String tagSelectionSummary(
    std::span<const std::string> tags,
    const juce::String& empty_text) {
    if (tags.empty()) {
        return empty_text;
    }
    auto text = juce::String::fromUTF8(tags.front().c_str());
    if (tags.size() > 1) {
        text += "  +"
            + juce::String(static_cast<int>(tags.size() - 1));
    }
    return text;
}

[[nodiscard]] int tagChipPreferredWidth(
    const juce::String& label,
    bool with_checkbox) {
    const auto font = UiFonts::body();
    const auto text_w = juce::GlyphArrangement::getStringWidthInt(
        font, label);
    const int leading = with_checkbox ? 36 : 12;
    return juce::jmax(56, leading + text_w + 12);
}

void layoutTagChipsFlow(
    juce::Component& host,
    juce::Array<juce::Component*>& chips,
    int available_width,
    int chip_height = 28,
    int gap = 6) {
    int x = 0;
    int y = 0;
    int row_h = 0;
    const int width = juce::jmax(1, available_width);
    for (auto* chip : chips) {
        if (chip == nullptr || !chip->isVisible()) {
            continue;
        }
        const int chip_w = chip->getProperties().contains("prefW")
            ? static_cast<int>(chip->getProperties()["prefW"])
            : 80;
        if (x > 0 && x + chip_w > width) {
            x = 0;
            y += row_h + gap;
            row_h = 0;
        }
        chip->setBounds(x, y, chip_w, chip_height);
        x += chip_w + gap;
        row_h = juce::jmax(row_h, chip_height);
    }
    host.setSize(width, y + (row_h > 0 ? row_h : chip_height));
}

class TagChipButton final : public juce::Component {
public:
    using Clicked = std::function<void()>;

    TagChipButton(
        juce::String label,
        bool checked,
        bool checkbox_style,
        Clicked on_click)
        : label_(std::move(label)),
          checked_(checked),
          checkbox_style_(checkbox_style),
          on_click_(std::move(on_click)) {
        const int pref = tagChipPreferredWidth(
            label_, checkbox_style_);
        getProperties().set("prefW", pref);
        setSize(pref, 28);
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
    }

    void setChecked(bool checked) {
        if (checked_ == checked) {
            return;
        }
        checked_ = checked;
        repaint();
    }

    [[nodiscard]] bool isChecked() const noexcept {
        return checked_;
    }

    void paint(juce::Graphics& graphics) override {
        auto bounds = getLocalBounds().toFloat().reduced(0.5F);
        graphics.setColour(
            checked_ ? juce::Colour(0xFF344454)
                     : juce::Colour(0xFF243040));
        graphics.fillRoundedRectangle(bounds, 4.0F);
        graphics.setColour(
            checked_ ? juce::Colour(0xFF2AD6C9)
                     : juce::Colour(0xFF607080));
        graphics.drawRoundedRectangle(bounds, 4.0F, 1.0F);

        int text_x = 10;
        if (checkbox_style_) {
            auto check = juce::Rectangle<float>(
                8.0F,
                (static_cast<float>(getHeight()) - 16.0F) * 0.5F,
                16.0F,
                16.0F);
            graphics.setColour(
                checked_ ? juce::Colour(0xFF2AD6C9)
                         : juce::Colour(0xFF68737E));
            graphics.drawRoundedRectangle(check, 2.0F, 1.5F);
            if (checked_) {
                graphics.drawLine(
                    check.getX() + 3.0F,
                    check.getCentreY(),
                    check.getX() + 6.5F,
                    check.getBottom() - 3.5F,
                    2.0F);
                graphics.drawLine(
                    check.getX() + 6.5F,
                    check.getBottom() - 3.5F,
                    check.getRight() - 3.0F,
                    check.getY() + 3.0F,
                    2.0F);
            }
            text_x = 30;
        }
        graphics.setColour(juce::Colour(0xFFE6EDF3));
        graphics.setFont(UiFonts::body());
        graphics.drawText(
            label_,
            text_x,
            0,
            getWidth() - text_x - 8,
            getHeight(),
            juce::Justification::centredLeft,
            true);
    }

    void mouseUp(const juce::MouseEvent& event) override {
        if (event.mouseWasClicked() && on_click_) {
            on_click_();
        }
    }

private:
    juce::String label_;
    bool checked_{};
    bool checkbox_style_{true};
    Clicked on_click_;
};

// Library-manager detail: assigned tag chip with a trailing × to remove.
class RemovableDetailTagChip final : public juce::Component {
public:
    using RemoveClicked = std::function<void()>;

    RemovableDetailTagChip(juce::String label, RemoveClicked on_remove)
        : label_(std::move(label)),
          on_remove_(std::move(on_remove)) {
        const int pref = tagChipPreferredWidth(label_, false) + 18;
        getProperties().set("prefW", pref);
        setSize(pref, 28);
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
    }

    void paint(juce::Graphics& graphics) override {
        auto bounds = getLocalBounds().toFloat().reduced(0.5F);
        graphics.setColour(juce::Colour(0xFF243040));
        graphics.fillRoundedRectangle(bounds, 4.0F);
        graphics.setColour(juce::Colour(0xFF607080));
        graphics.drawRoundedRectangle(bounds, 4.0F, 1.0F);

        const auto remove_area = removeHitArea();
        const int text_w = juce::jmax(
            0, getWidth() - remove_area.getWidth() - 12);
        graphics.setColour(juce::Colour(0xFFE6EDF3));
        graphics.setFont(UiFonts::body());
        graphics.drawText(
            label_,
            10,
            0,
            text_w,
            getHeight(),
            juce::Justification::centredLeft,
            true);

        const bool over_remove =
            remove_area.contains(getMouseXYRelative());
        graphics.setColour(
            over_remove ? juce::Colour(kUiHoverAccent)
                        : juce::Colour(0xFFB9C6D2));
        graphics.setFont(UiFonts::body());
        graphics.drawText(
            juce::String::fromUTF8("\xc3\x97"),
            remove_area,
            juce::Justification::centred,
            false);
    }

    void mouseMove(const juce::MouseEvent&) override { repaint(); }
    void mouseExit(const juce::MouseEvent&) override { repaint(); }

    void mouseUp(const juce::MouseEvent& event) override {
        if (event.mouseWasClicked()
            && removeHitArea().contains(event.getPosition())
            && on_remove_) {
            on_remove_();
        }
    }

private:
    [[nodiscard]] juce::Rectangle<int> removeHitArea() const {
        return getLocalBounds().removeFromRight(22);
    }

    juce::String label_;
    RemoveClicked on_remove_;
};

class TagSelectionContent final : public juce::Component {
public:
    using SelectionCallback =
        std::function<void(std::vector<std::string>)>;

    TagSelectionContent(
        std::vector<TagChoice> available,
        std::vector<std::string> selected,
        bool allow_custom,
        SelectionCallback callback)
        : available_(std::move(available)),
          selected_(std::move(selected)),
          allow_custom_(allow_custom),
          callback_(std::move(callback)) {
        filter_.setTextToShowWhenEmpty(
            juce::String::fromUTF8("タグ候補を検索"),
            juce::Colour(0xFF7F8993));
        filter_.onTextChange = [this] { rebuildFilteredChips(); };
        UiFonts::styleBodyField(filter_);
        addAndMakeVisible(filter_);

        viewport_.setViewedComponent(&chip_host_, false);
        viewport_.setScrollBarsShown(true, false);
        addAndMakeVisible(viewport_);

        custom_.setTextToShowWhenEmpty(
            juce::String::fromUTF8(
                "独自タグ（カンマ区切りで複数追加可）"),
            juce::Colour(0xFF7F8993));
        custom_.onReturnKey = [this] { addCustomTags(); };
        UiFonts::styleBodyField(custom_);
        addAndMakeVisible(custom_);
        custom_.setVisible(allow_custom_);

        add_.setButtonText(juce::String::fromUTF8("追加"));
        add_.onClick = [this] { addCustomTags(); };
        addAndMakeVisible(add_);
        add_.setVisible(allow_custom_);

        clear_.setButtonText(juce::String::fromUTF8("すべて解除"));
        clear_.onClick = [this] {
            selected_.clear();
            refreshChipCheckedState();
        };
        addAndMakeVisible(clear_);

        ok_.setButtonText(juce::String::fromUTF8("決定"));
        ok_.onClick = [this] {
            // 「追加」前に決定しても入力中の独自タグを取り込む。
            addCustomTags();
            // 親（ライブラリ管理など）が前面に戻ってから反映する。
            auto callback = callback_;
            auto selected = selected_;
            closeDialog(1);
            if (callback) {
                juce::MessageManager::callAsync(
                    [callback = std::move(callback),
                     selected = std::move(selected)]() mutable {
                        callback(std::move(selected));
                    });
            }
        };
        addAndMakeVisible(ok_);

        cancel_.setButtonText(juce::String::fromUTF8("キャンセル"));
        cancel_.onClick = [this] { closeDialog(0); };
        addAndMakeVisible(cancel_);

        rebuildFilteredChips();
        setSize(UiScale::sx(560), UiScale::sx(560));
    }

    void paint(juce::Graphics& graphics) override {
        UiScale::forceGlobalForNonEditorUi();
        juce::Component::paint(graphics);
    }

    void resized() override {
        UiScale::forceGlobalForNonEditorUi();
        auto area = getLocalBounds().reduced(16);
        filter_.setBounds(area.removeFromTop(32));
        area.removeFromTop(8);
        auto buttons = area.removeFromBottom(34);
        clear_.setBounds(buttons.removeFromLeft(100));
        buttons.removeFromLeft(8);
        cancel_.setBounds(buttons.removeFromRight(100));
        buttons.removeFromRight(8);
        ok_.setBounds(buttons.removeFromRight(100));
        area.removeFromBottom(8);
        if (allow_custom_) {
            auto custom_row = area.removeFromBottom(32);
            add_.setBounds(custom_row.removeFromRight(72));
            custom_row.removeFromRight(8);
            custom_.setBounds(custom_row);
            area.removeFromBottom(8);
        }
        viewport_.setBounds(area);
        layoutChips();
    }

private:
    [[nodiscard]] bool isSelected(std::string_view name) const {
        const std::array<std::string, 1> required{
            std::string(name)};
        return mgstc::engine::containsAllTimbreTags(
            selected_, required);
    }

    void toggleTag(const std::string& name) {
        if (isSelected(name)) {
            std::erase_if(
                selected_,
                [&name](const std::string& selected) {
                    return juce::String::fromUTF8(selected.c_str())
                        .equalsIgnoreCase(
                            juce::String::fromUTF8(name.c_str()));
                });
        } else {
            selected_.push_back(name);
        }
        selected_ = mgstc::engine::parseTimbreTags(
            mgstc::engine::serializeTimbreTags(selected_));
        refreshChipCheckedState();
    }

    void addCustomTags() {
        if (!allow_custom_) {
            return;
        }
        auto additions = mgstc::engine::parseTimbreTags(
            utf8String(custom_.getText()));
        if (additions.empty()) {
            return;
        }
        selected_.insert(
            selected_.end(), additions.begin(), additions.end());
        selected_ = mgstc::engine::parseTimbreTags(
            mgstc::engine::serializeTimbreTags(selected_));
        for (const auto& tag : additions) {
            const auto exists = std::any_of(
                available_.begin(), available_.end(),
                [&tag](const TagChoice& choice) {
                    return juce::String::fromUTF8(choice.name.c_str())
                        .equalsIgnoreCase(
                            juce::String::fromUTF8(tag.c_str()));
                });
            if (!exists) {
                available_.push_back({tag, 0});
            }
        }
        custom_.clear();
        rebuildFilteredChips();
    }

    void rebuildFilteredChips() {
        chip_host_.removeAllChildren();
        chips_.clear();
        const auto filter = filter_.getText().trim();
        for (std::size_t index = 0; index < available_.size(); ++index) {
            const auto& choice = available_[index];
            auto label = juce::String::fromUTF8(choice.name.c_str());
            if (!(filter.isEmpty()
                  || label.containsIgnoreCase(filter))) {
                continue;
            }
            if (choice.count != 0) {
                label += "  ("
                    + juce::String(static_cast<int>(choice.count))
                    + ")";
            }
            const auto name = choice.name;
            auto chip = std::make_unique<TagChipButton>(
                label,
                isSelected(name),
                true,
                [this, name] { toggleTag(name); });
            chip_host_.addAndMakeVisible(*chip);
            chips_.push_back(std::move(chip));
        }
        layoutChips();
    }

    void refreshChipCheckedState() {
        rebuildFilteredChips();
    }

    void layoutChips() {
        juce::Array<juce::Component*> views;
        for (auto& chip : chips_) {
            views.add(chip.get());
        }
        const int view_w = juce::jmax(
            1,
            viewport_.getWidth()
                - (viewport_.isVerticalScrollBarShown()
                       ? viewport_.getScrollBarThickness()
                       : 0));
        layoutTagChipsFlow(chip_host_, views, view_w);
        viewport_.setViewPosition(viewport_.getViewPosition());
    }

    void closeDialog(int result) {
        if (auto* dialog =
                findParentComponentOfClass<juce::DialogWindow>()) {
            dialog->exitModalState(result);
        }
    }

    std::vector<TagChoice> available_;
    std::vector<std::string> selected_;
    bool allow_custom_{};
    SelectionCallback callback_;
    juce::TextEditor filter_;
    juce::Viewport viewport_;
    juce::Component chip_host_;
    std::vector<std::unique_ptr<TagChipButton>> chips_;
    juce::TextEditor custom_;
    juce::TextButton add_;
    juce::TextButton clear_;
    juce::TextButton ok_;
    juce::TextButton cancel_;
};

void showTagSelectionDialog(
    juce::Component* anchor,
    const juce::String& title,
    std::vector<TagChoice> available,
    std::vector<std::string> selected,
    bool allow_custom,
    TagSelectionContent::SelectionCallback callback) {
    UiScale::forceGlobalForNonEditorUi();
    auto* dialog = new ModalDialogWindow(
        title, juce::Colour(0xFF1B222C));
    auto* content = new TagSelectionContent(
        std::move(available),
        std::move(selected),
        allow_custom,
        std::move(callback));
    dialog->setContentOwned(content, true);
    dialog->setResizable(false, false);
    if (anchor != nullptr) {
        dialog->centreAroundComponent(
            anchor, content->getWidth(), content->getHeight() + 32);
    } else {
        dialog->centreWithSize(
            content->getWidth(), content->getHeight() + 32);
    }
    dialog->enterModalState(true, nullptr, true);
}

class TagManagementContent final : public juce::Component {
public:
    using ApplyCallback =
        std::function<void(std::string, std::string)>;
    using ChangedCallback = std::function<void()>;

    TagManagementContent(
        std::vector<TagChoice> custom_tags,
        ApplyCallback callback,
        bool embedded = false,
        ChangedCallback on_changed = {})
        : custom_tags_(std::move(custom_tags)),
          callback_(std::move(callback)),
          embedded_(embedded),
          on_changed_(std::move(on_changed)) {
        source_label_.setText(
            juce::String::fromUTF8("管理する独自タグ"),
            juce::dontSendNotification);
        source_label_.setFont(UiFonts::body());
        addAndMakeVisible(source_label_);

        viewport_.setViewedComponent(&chip_host_, false);
        viewport_.setScrollBarsShown(true, false);
        addAndMakeVisible(viewport_);
        rebuildSourceChips();

        replacement_label_.setText(
            juce::String::fromUTF8("変更先タグ"),
            juce::dontSendNotification);
        replacement_label_.setFont(UiFonts::body());
        addAndMakeVisible(replacement_label_);
        replacement_.setTextToShowWhenEmpty(
            juce::String::fromUTF8(
                "新しい名前、既存タグ名、または標準タグ名"),
            juce::Colour(0xFF7F8993));
        replacement_.onTextChange = [this] { updateState(); };
        UiFonts::styleBodyField(replacement_);
        addAndMakeVisible(replacement_);

        impact_.setColour(
            juce::Label::textColourId, juce::Colour(0xFFB9C6D2));
        impact_.setJustificationType(juce::Justification::centredLeft);
        impact_.setFont(UiFonts::body());
        addAndMakeVisible(impact_);

        apply_.setButtonText(
            juce::String::fromUTF8("名前変更／統合"));
        apply_.onClick = [this] {
            if (const auto selected = selectedTag();
                selected && replacement_.getText().trim().isNotEmpty()) {
                if (callback_) {
                    callback_(
                        *selected,
                        utf8String(replacement_.getText().trim()));
                }
                finishAction();
            }
        };
        addAndMakeVisible(apply_);

        remove_.setButtonText(juce::String::fromUTF8("削除"));
        remove_.onClick = [this] { confirmDelete(); };
        addAndMakeVisible(remove_);

        cancel_.setButtonText(
            embedded_
                ? juce::String::fromUTF8("選択解除")
                : juce::String::fromUTF8("キャンセル"));
        cancel_.onClick = [this] {
            if (embedded_) {
                selected_index_ = -1;
                replacement_.clear();
                rebuildSourceChips();
                updateState();
            } else {
                closeDialog(0);
            }
        };
        addAndMakeVisible(cancel_);

        updateState();
        setSize(UiScale::sx(560), UiScale::sx(embedded_ ? 460 : 420));
    }

    void setCustomTags(std::vector<TagChoice> custom_tags) {
        custom_tags_ = std::move(custom_tags);
        selected_index_ = -1;
        replacement_.clear();
        rebuildSourceChips();
        updateState();
    }

    void paint(juce::Graphics& graphics) override {
        UiScale::forceGlobalForNonEditorUi();
        juce::Component::paint(graphics);
    }

    void resized() override {
        UiScale::forceGlobalForNonEditorUi();
        constexpr int pad = 10;
        constexpr int title_h = 30;
        constexpr int gap_xs = 4;
        constexpr int gap_sm = 8;
        constexpr int gap_lg = 16;
        constexpr int gap_xl = 24;
        constexpr int btn_h = 36;
        constexpr int btn_min_w = 80;
        constexpr int field_h = 34;
        auto area = getLocalBounds().reduced(pad);
        source_label_.setBounds(area.removeFromTop(title_h - gap_xs));
        area.removeFromTop(gap_xs);
        auto buttons = area.removeFromBottom(btn_h);
        cancel_.setBounds(buttons.removeFromRight(btn_min_w + gap_lg));
        buttons.removeFromRight(6);
        remove_.setBounds(buttons.removeFromRight(btn_min_w + gap_xs));
        buttons.removeFromRight(6);
        apply_.setBounds(buttons.removeFromRight(btn_min_w + gap_xl));
        area.removeFromBottom(gap_sm);
        impact_.setBounds(area.removeFromBottom(title_h - gap_xs));
        area.removeFromBottom(gap_sm);
        replacement_.setBounds(area.removeFromBottom(field_h));
        area.removeFromBottom(gap_xs);
        replacement_label_.setBounds(area.removeFromBottom(title_h - gap_xs));
        area.removeFromBottom(gap_sm);
        viewport_.setBounds(area);
        layoutSourceChips();
    }

private:
    [[nodiscard]] std::optional<std::string> selectedTag() const {
        if (selected_index_ < 0
            || static_cast<std::size_t>(selected_index_)
                >= custom_tags_.size()) {
            return std::nullopt;
        }
        return custom_tags_[
            static_cast<std::size_t>(selected_index_)].name;
    }

    void selectIndex(int index) {
        selected_index_ = index;
        rebuildSourceChips();
        updateState();
    }

    void rebuildSourceChips() {
        chip_host_.removeAllChildren();
        chips_.clear();
        for (std::size_t index = 0; index < custom_tags_.size();
             ++index) {
            const auto& tag = custom_tags_[index];
            auto label = juce::String::fromUTF8(tag.name.c_str())
                + "  ("
                + juce::String(static_cast<int>(tag.count))
                + ")";
            const int captured = static_cast<int>(index);
            auto chip = std::make_unique<TagChipButton>(
                label,
                selected_index_ == captured,
                false,
                [this, captured] { selectIndex(captured); });
            chip_host_.addAndMakeVisible(*chip);
            chips_.push_back(std::move(chip));
        }
        layoutSourceChips();
    }

    void layoutSourceChips() {
        juce::Array<juce::Component*> views;
        for (auto& chip : chips_) {
            views.add(chip.get());
        }
        const int view_w = juce::jmax(
            1,
            viewport_.getWidth()
                - (viewport_.isVerticalScrollBarShown()
                       ? viewport_.getScrollBarThickness()
                       : 0));
        layoutTagChipsFlow(chip_host_, views, view_w);
    }

    void updateState() {
        const bool valid = selectedTag().has_value();
        const auto count = valid
            ? custom_tags_[static_cast<std::size_t>(selected_index_)]
                  .count
            : 0;
        impact_.setText(
            valid
                ? juce::String::fromUTF8("使用中の音色: ")
                    + juce::String(static_cast<int>(count))
                : juce::String::fromUTF8(
                      "独自タグを選択してください"),
            juce::dontSendNotification);
        apply_.setEnabled(
            valid && replacement_.getText().trim().isNotEmpty());
        remove_.setEnabled(valid);
    }

    void finishAction() {
        if (embedded_) {
            selected_index_ = -1;
            replacement_.clear();
            if (on_changed_) {
                on_changed_();
            }
            updateState();
            return;
        }
        closeDialog(1);
    }

    void confirmDelete() {
        const auto selected = selectedTag();
        if (!selected) {
            return;
        }
        juce::Component::SafePointer<TagManagementContent> safe(this);
        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions()
                .withIconType(
                    juce::MessageBoxIconType::WarningIcon)
                .withTitle(
                    juce::String::fromUTF8("独自タグの削除"))
                .withMessage(
                    juce::String::fromUTF8("「")
                    + juce::String::fromUTF8(selected->c_str())
                    + juce::String::fromUTF8(
                        "」をすべての音色から削除しますか？"))
                .withButton(juce::String::fromUTF8("削除"))
                .withButton(
                    juce::String::fromUTF8("キャンセル"))
                .withAssociatedComponent(this),
            [safe, selected](int result) {
                if (safe == nullptr || result != 1) {
                    return;
                }
                if (safe->callback_) {
                    safe->callback_(*selected, {});
                }
                safe->finishAction();
            });
    }

    void closeDialog(int result) {
        if (auto* dialog =
                findParentComponentOfClass<juce::DialogWindow>()) {
            dialog->exitModalState(result);
        }
    }

    std::vector<TagChoice> custom_tags_;
    ApplyCallback callback_;
    ChangedCallback on_changed_;
    bool embedded_{};
    int selected_index_{-1};
    juce::Label source_label_;
    juce::Viewport viewport_;
    juce::Component chip_host_;
    std::vector<std::unique_ptr<TagChipButton>> chips_;
    juce::Label replacement_label_;
    juce::TextEditor replacement_;
    juce::Label impact_;
    juce::TextButton apply_;
    juce::TextButton remove_;
    juce::TextButton cancel_;
};

void showTagManagementDialog(
    juce::Component* anchor,
    std::vector<TagChoice> custom_tags,
    TagManagementContent::ApplyCallback callback) {
    UiScale::forceGlobalForNonEditorUi();
    auto* dialog = new ModalDialogWindow(
        juce::String::fromUTF8("独自タグ管理"),
        juce::Colour(0xFF1B222C));
    auto* content = new TagManagementContent(
        std::move(custom_tags), std::move(callback));
    dialog->setContentOwned(content, true);
    dialog->setResizable(false, false);
    if (anchor != nullptr) {
        dialog->centreAroundComponent(
            anchor, content->getWidth(), content->getHeight() + 32);
    } else {
        dialog->centreWithSize(
            content->getWidth(), content->getHeight() + 32);
    }
    dialog->enterModalState(true, nullptr, true);
}

[[nodiscard]] std::vector<TagChoice> presetTagChoices(
    std::span<const std::string> selected) {
    std::vector<TagChoice> choices;
    for (const auto tag : mgstc::engine::presetTimbreTags()) {
        choices.push_back({std::string(tag), 0});
    }
    for (const auto& tag : selected) {
        const auto exists = std::any_of(
            choices.begin(), choices.end(),
            [&tag](const TagChoice& choice) {
                return juce::String::fromUTF8(choice.name.c_str())
                    .equalsIgnoreCase(
                        juce::String::fromUTF8(tag.c_str()));
            });
        if (!exists) {
            choices.push_back({tag, 0});
        }
    }
    return choices;
}

[[nodiscard]] std::vector<TagChoice> usageTagChoices(
    std::span<const std::vector<std::string>> tag_sets) {
    std::vector<TagChoice> choices;
    for (auto usage :
         mgstc::engine::collectTimbreTagUsage(tag_sets)) {
        choices.push_back({std::move(usage.name), usage.count});
    }
    return choices;
}

[[nodiscard]] std::vector<TagChoice> mergeTagChoices(
    std::vector<TagChoice> base,
    std::span<const TagChoice> extra) {
    for (const auto& tag : extra) {
        const auto found = std::find_if(
            base.begin(),
            base.end(),
            [&tag](const TagChoice& choice) {
                return juce::String::fromUTF8(choice.name.c_str())
                    .equalsIgnoreCase(
                        juce::String::fromUTF8(tag.name.c_str()));
            });
        if (found == base.end()) {
            base.push_back(tag);
        } else if (tag.count > found->count) {
            found->count = tag.count;
        }
    }
    return base;
}

// 編集ダイアログ用: 標準78 + ライブラリ既存独自タグ + 現在選択中タグ。
[[nodiscard]] std::vector<TagChoice> editorTagChoices(
    std::span<const std::vector<std::string>> library_tag_sets,
    std::span<const std::string> selected) {
    return mergeTagChoices(
        presetTagChoices(selected),
        usageTagChoices(library_tag_sets));
}

// 検索ダイアログ用: 使用中タグ + 現在の絞り込み選択（未使用になっても表示を維持）。
[[nodiscard]] std::vector<TagChoice> filterTagChoices(
    std::span<const std::vector<std::string>> library_tag_sets,
    std::span<const std::string> selected) {
    std::vector<TagChoice> selected_choices;
    selected_choices.reserve(selected.size());
    for (const auto& tag : selected) {
        selected_choices.push_back({tag, 0});
    }
    return mergeTagChoices(
        usageTagChoices(library_tag_sets), selected_choices);
}

template <typename Work, typename OnDone>
void runWithConversionBusyDialog(
    juce::Component* anchor,
    Work work,
    OnDone on_done) {
    using Result =
        std::invoke_result_t<Work, ConversionProgressState&>;
    struct Task {
        std::jthread worker;
    };

    auto progress = std::make_shared<ConversionProgressState>();
    auto* dialog = new ConversionBusyDialog(progress);
    if (anchor != nullptr) {
        dialog->centreAroundComponent(
            anchor, dialog->getWidth(), dialog->getHeight());
    }
    dialog->enterModalState(true, nullptr, true);

    juce::Component::SafePointer<ConversionBusyDialog> safe_dialog(
        dialog);
    auto task = std::make_shared<Task>();
    dialog->keepTaskAlive(task);
    std::weak_ptr<Task> weak_task(task);
    task->worker = std::jthread(
        [safe_dialog,
         progress,
         weak_task,
         work = std::move(work),
         on_done = std::move(on_done)]() mutable {
            Result result = work(*progress);
            const bool cancelled = progress->cancellationRequested();
            if (auto task = weak_task.lock()) {
            juce::MessageManager::callAsync(
                [safe_dialog,
                     task = std::move(task),
                 result = std::move(result),
                     on_done = std::move(on_done),
                     cancelled]() mutable {
                        if (safe_dialog == nullptr) {
                            return;
                        }
                        safe_dialog->exitModalState(1);
                        on_done(std::move(result), cancelled);
                    });
                    }
                });
}

class AboutPanel final : public juce::Component {
public:
    AboutPanel() {
        logo_ = loadImageMemory(
            BinaryData::MGSTC_logo_png,
            static_cast<std::size_t>(BinaryData::MGSTC_logo_pngSize));
        title_.setText(
            "MGS Tone Craft", juce::dontSendNotification);
        title_.setFont(UiFonts::title());
        title_.setJustificationType(juce::Justification::centred);
        title_.setColour(
            juce::Label::textColourId, juce::Colour(0xFFE6EDF3));
        addAndMakeVisible(title_);

        subtitle_.setText(
            "MGSTC", juce::dontSendNotification);
        subtitle_.setFont(UiFonts::body());
        subtitle_.setJustificationType(juce::Justification::centred);
        subtitle_.setColour(
            juce::Label::textColourId, juce::Colour(0xFF9AA8B5));
        addAndMakeVisible(subtitle_);

        detail_.setText(
            juce::String::fromUTF8(
                "MGSDRV向け複合音色エディタ\n"
                "Version ")
                + juce::String(MGSTC_DOC_VERSION),
            juce::dontSendNotification);
        detail_.setJustificationType(juce::Justification::centred);
        detail_.setColour(
            juce::Label::textColourId, juce::Colour(0xFF9AA8B5));
        addAndMakeVisible(detail_);

        copyright_.setText(
            "Copyright(C) 2026 Takawo. All right reserves.",
            juce::dontSendNotification);
        copyright_.setJustificationType(juce::Justification::centred);
        copyright_.setColour(
            juce::Label::textColourId, juce::Colour(0xFF9AA8B5));
        addAndMakeVisible(copyright_);

        github_.setJustificationType(juce::Justification::centred);
        x_.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(github_);
        addAndMakeVisible(x_);
    }

    void paint(juce::Graphics& g) override {
        if (logo_.isValid()) {
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
        auto area = getLocalBounds().reduced(UiScale::sx(12));
        const int footer_row = UiScale::sx(22);
        const int title_h = UiScale::sx(28);
        const int subtitle_h = UiScale::sx(24);
        const int gap_logo_title = UiScale::sx(8);
        const int gap_detail = UiScale::sx(8);
        const int detail_h = UiScale::sx(48);

        // Natural (100%) asset size; shrink only when the panel is too small.
        const int logo_w = logo_.isValid()
            ? juce::jmax(1, logo_.getWidth())
            : UiScale::sx(256);
        const int logo_h = logo_.isValid()
            ? juce::jmax(1, logo_.getHeight())
            : UiScale::sx(256);

        x_.setBounds(area.removeFromBottom(footer_row));
        github_.setBounds(area.removeFromBottom(footer_row));
        copyright_.setBounds(area.removeFromBottom(footer_row));

        int display_logo_w = logo_w;
        int display_logo_h = logo_h;
        const int chrome_h = title_h + subtitle_h + gap_logo_title
            + gap_detail + detail_h;
        if (logo_w > area.getWidth()
            || logo_h > juce::jmax(1, area.getHeight() - chrome_h)) {
            const float fit = juce::jmin(
                static_cast<float>(area.getWidth())
                    / static_cast<float>(logo_w),
                static_cast<float>(
                    juce::jmax(1, area.getHeight() - chrome_h))
                    / static_cast<float>(logo_h));
            display_logo_w = juce::jmax(
                1, juce::roundToInt(static_cast<float>(logo_w) * fit));
            display_logo_h = juce::jmax(
                1, juce::roundToInt(static_cast<float>(logo_h) * fit));
        }

        const int display_stack_h =
            display_logo_h + gap_logo_title + title_h + subtitle_h;
        const int display_group_h =
            display_stack_h + gap_detail + detail_h;
        const int logo_y = (area.getHeight() < display_group_h)
            ? area.getY()
            : area.getY() + (area.getHeight() - display_group_h) / 2;

        logo_bounds_ = juce::Rectangle<int>(
                           area.getX(),
                           logo_y,
                           area.getWidth(),
                           display_logo_h)
                           .withSizeKeepingCentre(
                               display_logo_w, display_logo_h);
        title_.setBounds(
            area.getX(),
            logo_y + display_logo_h + gap_logo_title,
            area.getWidth(),
            title_h);
        subtitle_.setBounds(
            area.getX(),
            logo_y + display_logo_h + gap_logo_title + title_h,
            area.getWidth(),
            subtitle_h);
        detail_.setBounds(
            area.getX(),
            logo_y + display_stack_h + gap_detail,
            area.getWidth(),
            detail_h);
    }

private:
    juce::Image logo_;
    juce::Rectangle<int> logo_bounds_;
    juce::Label title_;
    juce::Label subtitle_;
    juce::Label detail_;
    juce::Label copyright_;
    juce::HyperlinkButton github_{
        "GitHub",
        juce::URL("https://github.com/takawon/mgs-tone-craft")};
    juce::HyperlinkButton x_{
        "X",
        juce::URL("https://x.com/takawo_n")};
};

void configureImmediateAuditionButton(
    juce::DrawableButton& button,
    bool enabled,
    std::function<void()> action) {
    const auto normal = makeEditorIcon(
        EditorIcon::Audition, juce::Colour(0xFF9AA8B5));
    const auto over = makeEditorIcon(
        EditorIcon::Audition, juce::Colour(0xFFE6EDF3));
    const auto normal_on = makeEditorIcon(
        EditorIcon::Audition, juce::Colour(0xFFFFFFFF));
    const auto over_on = makeEditorIcon(
        EditorIcon::Audition, juce::Colour(0xFFF2F4F5));
    // down == over, down_on == over_on
    button.setImages(
        normal.get(), over.get(), over.get(), nullptr,
        normal_on.get(), over_on.get(), over_on.get(), nullptr);
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
        setTooltip(
            juce::String::fromUTF8(
                "波形枠内をドラッグして32個の波形値を編集します"));
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

    void setApplyRange(mgstc::engine::SccApplyRange range) {
        if (apply_range_ == range) {
            return;
        }
        apply_range_ = range;
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

            graphics.setFont(UiFonts::dense());
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

            // 左半分／右半分適用時、変更しない側へ半透明マスクを重ねる。
            if (apply_range_
                != mgstc::engine::SccApplyRange::All) {
                const auto mid_x = graph.getCentreX();
                const auto masked =
                    apply_range_
                        == mgstc::engine::SccApplyRange::LeftHalf
                    ? juce::Rectangle<int>(
                          mid_x,
                          graph.getY(),
                          graph.getRight() - mid_x,
                          graph.getHeight())
                    : juce::Rectangle<int>(
                          graph.getX(),
                          graph.getY(),
                          mid_x - graph.getX(),
                          graph.getHeight());
                graphics.setColour(
                    juce::Colour(0x99091018));
                graphics.fillRect(masked);
                graphics.setColour(juce::Colour(0x66A0AEBC));
                graphics.drawVerticalLine(
                    mid_x,
                    static_cast<float>(graph.getY()),
                    static_cast<float>(graph.getBottom()));
            }
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
        graphics.setFont(UiFonts::dense(true));
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
            const auto text = formatSampleValueLabel(
                selected_index_,
                display_wave[static_cast<std::size_t>(selected_index_)]);
            graphics.setColour(juce::Colours::white);
            graphics.drawText(
                text,
                getLocalBounds().removeFromTop(24),
                juce::Justification::centredRight);
        }

        drawHoverValuePopup(graphics);
    }

    void mouseDown(const juce::MouseEvent& event) override {
        // 初回クリックは波形枠内だけを受け付ける。枠外からの開始は無視する。
        if (!graphBounds().contains(event.getPosition())) {
            mouse_edit_active_ = false;
            updateEditCursor(event.getPosition());
            updateHoverFromMouse(event.getPosition());
            return;
        }
        // DEC/HEX入力欄にフォーカスが残っているとPCキー演奏が抑制される。
        // 波形ドラッグ開始時にフォーカスを奪い、演奏できる状態へ戻す。
        setWantsKeyboardFocus(true);
        grabKeyboardFocus();
        mouse_edit_active_ = true;
        setMouseCursor(juce::MouseCursor::CrosshairCursor);
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
        updateHoverFromMouse(event.getPosition());
    }

    void mouseDrag(const juce::MouseEvent& event) override {
        // 枠内で開始したドラッグは、枠外へ出ても従来どおり追従する。
        if (!mouse_edit_active_) {
            return;
        }
        setMouseCursor(juce::MouseCursor::CrosshairCursor);
        updateFromMouse(event.position);
        updateHoverFromMouse(event.getPosition());
    }

    void mouseUp(const juce::MouseEvent& event) override {
        if (!mouse_edit_active_) {
            updateEditCursor(event.getPosition());
            updateHoverFromMouse(event.getPosition());
            return;
        }
        mouse_edit_active_ = false;
        if (drag_start_ != waveform_ && on_commit_) {
            on_commit_(drag_start_, waveform_);
        }
        updateEditCursor(event.getPosition());
        updateHoverFromMouse(event.getPosition());
    }

    void mouseMove(const juce::MouseEvent& event) override {
        updateEditCursor(event.getPosition());
        updateHoverFromMouse(event.getPosition());
    }

    void mouseExit(const juce::MouseEvent&) override {
        if (mouse_edit_active_) {
            return;
        }
        setMouseCursor(juce::MouseCursor::NormalCursor);
        if (hover_mouse_pos_ || hover_index_ >= 0 || hover_value_) {
            hover_mouse_pos_.reset();
            hover_index_ = -1;
            hover_value_.reset();
            repaint();
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

    void updateEditCursor(juce::Point<int> position) {
        if (mouse_edit_active_
            || graphBounds().contains(position)) {
            setMouseCursor(juce::MouseCursor::CrosshairCursor);
        } else {
            setMouseCursor(juce::MouseCursor::NormalCursor);
        }
    }

    [[nodiscard]] int sampleIndexAtX(float x) const {
        const auto graph = graphBounds();
        if (graph.isEmpty()) {
            return 0;
        }
        const auto relative_x = juce::jlimit(
            0.0F,
            static_cast<float>(graph.getWidth() - 1),
            x - static_cast<float>(graph.getX()));
        return juce::jlimit(
            0,
            static_cast<int>(waveform_.size()) - 1,
            static_cast<int>(
                relative_x * waveform_.size()
                / static_cast<float>(graph.getWidth())));
    }

    // Invert sampleY: graph Y → signed sample (−128…127), same as draw edit.
    [[nodiscard]] static std::int8_t sampleValueAtY(
        float y,
        juce::Rectangle<int> graph) {
        if (graph.isEmpty()) {
            return 0;
        }
        const auto normalized_y = juce::jlimit(
            0.0F,
            1.0F,
            (y - static_cast<float>(graph.getY()))
                / static_cast<float>(graph.getHeight()));
        return static_cast<std::int8_t>(juce::jlimit(
            -128,
            127,
            juce::roundToInt(127.0F - normalized_y * 255.0F)));
    }

    void updateHoverFromMouse(juce::Point<int> position) {
        const auto graph = graphBounds();
        const bool over_graph = graph.contains(position);
        if (!over_graph && !mouse_edit_active_) {
            if (hover_mouse_pos_ || hover_index_ >= 0) {
                hover_mouse_pos_.reset();
                hover_index_ = -1;
                hover_value_.reset();
                repaint();
            }
            return;
        }
        const auto index = sampleIndexAtX(
            static_cast<float>(position.x));
        const auto value = sampleValueAtY(
            static_cast<float>(position.y), graph);
        if (hover_mouse_pos_
            && *hover_mouse_pos_ == position
            && hover_index_ == index
            && hover_value_
            && *hover_value_ == value) {
            return;
        }
        hover_mouse_pos_ = position;
        hover_index_ = index;
        hover_value_ = value;
        repaint();
    }

    [[nodiscard]] static juce::String formatSampleValueLabel(
        int index,
        std::int8_t sample) {
        return juce::String::formatted(
            "%02d : %d (0x%02X)",
            index,
            static_cast<int>(sample),
            static_cast<unsigned int>(
                static_cast<std::uint8_t>(sample)));
    }

    void drawHoverValuePopup(juce::Graphics& graphics) const {
        if (!hover_mouse_pos_ || hover_index_ < 0 || !hover_value_) {
            return;
        }
        const auto graph = graphBounds();
        if (graph.isEmpty()) {
            return;
        }
        const auto text = formatSampleValueLabel(
            hover_index_, *hover_value_);
        const auto font = UiFonts::body(true);
        graphics.setFont(font);
        const int pad_x = UiScale::sx(6);
        const int pad_y = UiScale::sx(2);
        const int text_w = juce::jmax(
            UiScale::sx(24),
            juce::GlyphArrangement::getStringWidthInt(font, text)
                + pad_x * 2);
        const int text_h = juce::jmax(
            UiScale::sx(18),
            juce::roundToInt(font.getHeight()) + pad_y * 2);
        auto box = juce::Rectangle<int>(
            hover_mouse_pos_->x + UiScale::sx(12),
            hover_mouse_pos_->y - text_h - UiScale::sx(8),
            text_w,
            text_h);
        box.setX(juce::jlimit(
            graph.getX(),
            graph.getRight() - text_w,
            box.getX()));
        box.setY(juce::jlimit(
            graph.getY(),
            graph.getBottom() - text_h,
            box.getY()));
        graphics.setColour(juce::Colour(0xEE151A20));
        graphics.fillRoundedRectangle(box.toFloat(), 3.0F);
        graphics.setColour(juce::Colour(kUiHoverAccent));
        graphics.drawRoundedRectangle(box.toFloat(), 3.0F, 1.0F);
        graphics.setColour(juce::Colour(0xFFE6EDF3));
        graphics.drawText(
            text, box, juce::Justification::centred, false);
    }

    static void configureValueEditor(
        juce::TextEditor& editor,
        bool hexadecimal) {
        UiFonts::styleDenseField(editor);
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
        const auto next = sampleValueAtY(position.y, graph);
        selected_index_ = index;
        auto& sample =
            waveform_[static_cast<std::size_t>(index)];
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
    int hover_index_{-1};
    std::optional<std::int8_t> hover_value_;
    std::optional<juce::Point<int>> hover_mouse_pos_;
    bool mouse_edit_active_{};
    mgstc::engine::SccApplyRange apply_range_{
        mgstc::engine::SccApplyRange::All};
};

enum class PcAudioBackend : std::uint8_t {
    Wasapi,
    Asio,
};

class SharedAudioService final : private juce::Timer {
public:
    SharedAudioService() {
        master_volume_percent_ = loadMasterVolumePercent();
        wasapi_audio_.setMasterVolumePercent(
            static_cast<std::uint32_t>(master_volume_percent_));
        asio_audio_.setMasterVolumePercent(
            static_cast<std::uint32_t>(master_volume_percent_));
        loadAndApplySoundOutputSettings();
        loadAndStartPcAudioSettings();
        publishHangHints();
        opll_keyoff_force_silence_seconds_ =
            loadOpllKeyOffForceSilenceSeconds();
        startTimerHz(10);
    }

    ~SharedAudioService() {
        stopTimer();
        // Let an in-flight recovery skip its device open and finish fast; the
        // worker touches members, so it must be joined before teardown.
        device_recovery_cancel_.store(true, std::memory_order_release);
        joinDeviceWorkerThread();
        device_recovery_busy_ = false;
        static_cast<void>(
            engine_.submit(mgstc::engine::EngineCommand::stop()));
        asio_audio_.close();
        wasapi_audio_.stop();
    }

    SharedAudioService(const SharedAudioService&) = delete;
    SharedAudioService& operator=(const SharedAudioService&) = delete;

    [[nodiscard]] mgstc::engine::RealtimeEngineHost& engine() noexcept {
        return engine_;
    }

    [[nodiscard]] bool running() const noexcept {
        return active_audio_ != nullptr && active_audio_->running();
    }

    [[nodiscard]] int masterVolumePercent() const noexcept {
        return master_volume_percent_;
    }

    [[nodiscard]] std::uint64_t masterVolumeRevision() const noexcept {
        return master_volume_revision_;
    }

    [[nodiscard]] int opllKeyOffForceSilenceSeconds() const noexcept {
        return opll_keyoff_force_silence_seconds_;
    }

    void setOpllKeyOffForceSilenceSeconds(int seconds) {
        const auto next = juce::jlimit(0, 9999, seconds);
        if (next == opll_keyoff_force_silence_seconds_) {
            return;
        }
        opll_keyoff_force_silence_seconds_ = next;
        saveOpllKeyOffForceSilenceSeconds();
        if (next == 0) {
            opll_silence_dues_.clear();
        }
    }

    void armOpllKeyOffForceSilence(std::uint8_t track) {
        if (track < 8 || track >= 17) {
            return;
        }
        cancelOpllKeyOffForceSilence(track);
        const auto seconds = opll_keyoff_force_silence_seconds_;
        if (seconds <= 0) {
            return;
        }
        opll_silence_dues_.push_back(
            {
                .track = track,
                .due_ms =
                    juce::Time::getMillisecondCounterHiRes()
                    + (static_cast<double>(seconds) * 1000.0),
            });
    }

    void cancelOpllKeyOffForceSilence(std::uint8_t track) {
        std::erase_if(
            opll_silence_dues_,
            [track](const OpllSilenceDue& due) {
                return due.track == track;
            });
    }

    void forceSilenceOpllTrackNow(std::uint8_t track) {
        cancelOpllKeyOffForceSilence(track);
        if (track < 8 || track >= 17) {
            return;
        }
        static_cast<void>(engine_.submit(
            mgstc::engine::EngineCommand::silenceTrack(track)));
    }

    void forceSilenceAllOpllTracks() {
        opll_silence_dues_.clear();
        for (std::uint8_t track = 8; track < 17; ++track) {
            static_cast<void>(engine_.submit(
                mgstc::engine::EngineCommand::silenceTrack(track)));
        }
    }

    void clearOpllKeyOffForceSilence() {
        opll_silence_dues_.clear();
    }

    void setMasterVolumePercent(int percent) {
        const auto next = juce::jlimit(0, 100, percent);
        if (next == master_volume_percent_) {
            return;
        }
        master_volume_percent_ = next;
        if (!device_recovery_busy_) {
            // The device worker owns wasapi_audio_ while recovering; the volume
            // is re-applied when the recovery is collected.
            wasapi_audio_.setMasterVolumePercent(
                static_cast<std::uint32_t>(next));
        }
        asio_audio_.setMasterVolumePercent(
            static_cast<std::uint32_t>(next));
        saveMasterVolumePercent();
        ++master_volume_revision_;
    }

    [[nodiscard]] PcAudioBackend pcAudioBackend() const noexcept {
        return pc_audio_backend_;
    }

    [[nodiscard]] std::string asioDriverName() const {
        return asio_driver_name_;
    }

    [[nodiscard]] std::vector<std::string> asioDriverNames() {
        return asio_audio_.driverNames();
    }

    [[nodiscard]] bool applyPcAudioSettings(
        PcAudioBackend backend,
        std::string driver_name) {
        MGSTC_UI_ACTIVITY("settings: apply PC audio (device open)");
        waitForPcAudioRecovery();
        stopActiveAudio();
        pc_audio_note_.clear();

        if (backend == PcAudioBackend::Asio) {
            if (asio_audio_.open(driver_name)
                && asio_audio_.start(engine_)) {
                active_audio_ = &asio_audio_;
                pc_audio_backend_ = PcAudioBackend::Asio;
                asio_driver_name_ = std::move(driver_name);
                drainAudioStatuses(asio_audio_);
                savePcAudioSettings();
                publishHangHints();
                return true;
            }
            pc_audio_note_ = "ASIOを開始できません: "
                + asio_audio_.lastError()
                + "。既定出力へ戻しました。";
            asio_audio_.close();
        }

        const auto started = wasapi_audio_.start(engine_);
        active_audio_ = started ? &wasapi_audio_ : nullptr;
        pc_audio_backend_ = PcAudioBackend::Wasapi;
        if (!started && pc_audio_note_.empty()) {
            pc_audio_note_ = "既定のWASAPI出力を開始できません。";
        }
        if (backend == PcAudioBackend::Asio) {
            asio_driver_name_ = std::move(driver_name);
        }
        savePcAudioSettings();
        publishHangHints();
        return backend == PcAudioBackend::Wasapi && started;
    }

    [[nodiscard]] bool showAsioControlPanel() {
        MGSTC_UI_ACTIVITY("settings: ASIO control panel");
        waitForPcAudioRecovery();
        if (pc_audio_backend_ != PcAudioBackend::Asio
            || asio_driver_name_.empty()) {
            pc_audio_note_ =
                "ASIOを適用してからASIO設定を開いてください。";
            return false;
        }

        // 設定画面の開閉やドライバー再初期化で来るStoppedを、
        // 故障扱いの自動WASAPI復帰と混同しない。
        suppress_pc_audio_fallback_ = true;
        const auto driver = asio_driver_name_;
        asio_audio_.stop();
        // 多くのASIOパネルは非同期で戻る。戻り値だけで成否を決めず、
        // 必ず同じドライバーを開き直してから再開する。
        static_cast<void>(asio_audio_.showControlPanel());
        asio_audio_.close();
        drainAudioStatuses(asio_audio_);

        if (asio_audio_.open(driver) && asio_audio_.start(engine_)) {
            active_audio_ = &asio_audio_;
            pc_audio_backend_ = PcAudioBackend::Asio;
            drainAudioStatuses(asio_audio_);
            pc_audio_note_.clear();
            suppress_pc_audio_fallback_ = false;
            publishHangHints();
            return true;
        }

        const auto detail = asio_audio_.lastError();
        suppress_pc_audio_fallback_ = false;
        // ASIO is already closed above; the WASAPI reopen runs on the device
        // worker so the settings dialog stays responsive.
        beginPcAudioRecovery(
            "ASIO設定後に再開できません"
                + (detail.empty() ? std::string(".")
                                  : ": " + detail),
            /*restart_wasapi=*/true,
            /*close_asio=*/false);
        return false;
    }

    [[nodiscard]] bool asioControlPanelAvailable() const noexcept {
        return pc_audio_backend_ == PcAudioBackend::Asio
            && asio_audio_.hasControlPanel();
    }

    [[nodiscard]] std::string pcAudioStatus() const {
        if (pc_audio_backend_ == PcAudioBackend::Asio
            && active_audio_ == &asio_audio_
            && asio_audio_.running()) {
            const auto rate = asio_audio_.currentSampleRate();
            const auto rate_text = juce::String(rate / 1000.0, 1)
                                       .trimCharactersAtEnd(".0")
                                       .toStdString();
            std::string result = "ASIO: " + asio_driver_name_ + " (";
            if (std::abs(rate - 48'000.0) < 0.5) {
                result += "48 kHz、リアルタイム変換なし)";
            } else {
                result += "48 kHz → " + rate_text
                    + " kHz リアルタイム変換中)";
            }
            return result;
        }
        auto result = active_audio_ == &wasapi_audio_
                && wasapi_audio_.running()
            ? std::string("既定出力 (WASAPI共有モード)")
            : std::string("PC音声出力は停止しています");
        if (!pc_audio_note_.empty()) {
            result += "\n" + pc_audio_note_;
        }
        return result;
    }

    void clearScopeFrames() {
        mgstc::engine::OpllScopeFrame frame{};
        while (engine_.pollOpllScope(frame)) {
        }
    }

    void setSharedSccWaveform(const SccWaveform& waveform) {
        if (waveform == shared_scc_wave_) {
            return;
        }
        shared_scc_wave_ = waveform;
        ++shared_scc_wave_revision_;
    }

    [[nodiscard]] const SccWaveform& sharedSccWaveform() const noexcept {
        return shared_scc_wave_;
    }

    [[nodiscard]] std::uint64_t sharedSccWaveRevision() const noexcept {
        return shared_scc_wave_revision_;
    }

    void setSharedOpllPatch(
        const mgstc::engine::OpllPatchParameters& patch) {
        if (patch == shared_opll_patch_) {
            return;
        }
        shared_opll_patch_ = patch;
        ++shared_opll_patch_revision_;
    }

    [[nodiscard]] const mgstc::engine::OpllPatchParameters&
    sharedOpllPatch() const noexcept {
        return shared_opll_patch_;
    }

    [[nodiscard]] std::uint64_t sharedOpllPatchRevision() const noexcept {
        return shared_opll_patch_revision_;
    }

    [[nodiscard]] bool sharedEditorProgramActive() const noexcept {
        return shared_editor_program_active_;
    }

    void invalidateSharedEditorProgram() noexcept {
        shared_editor_program_active_ = false;
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
        if (submitted) {
            shared_editor_program_active_ = true;
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
        publishHangHints();
    }

    void publishHangHints() const noexcept {
        mgstc::app::publishUiHangPcAudioHint(
            pc_audio_backend_ == PcAudioBackend::Asio
                ? mgstc::app::kUiHangPcAsio
                : mgstc::app::kUiHangPcWasapi);
        mgstc::app::publishUiHangSoundOutputHint(
            engine_.soundOutputKind()
                    == mgstc::engine::SoundOutputKind::MAmidiMemo
                ? mgstc::app::kUiHangOutMAmidi
                : mgstc::app::kUiHangOutEmulator);
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
            // Blocking connect (poll + RPC timeout): breadcrumbed because a
            // failed attempt still reports sound_output=emulator.
            MGSTC_UI_ACTIVITY(
                "startup: MAmidiMEmo connect (blocking)");
            static_cast<void>(engine_.setSoundOutputKind(
                mgstc::engine::SoundOutputKind::MAmidiMemo));
        }
        publishHangHints();
    }

    void pollOpllKeyOffForceSilence() {
        if (opll_silence_dues_.empty()) {
            return;
        }
        const auto now = juce::Time::getMillisecondCounterHiRes();
        std::vector<std::uint8_t> due_tracks;
        for (const auto& due : opll_silence_dues_) {
            if (now >= due.due_ms) {
                due_tracks.push_back(due.track);
            }
        }
        for (const auto track : due_tracks) {
            forceSilenceOpllTrackNow(track);
        }
    }

    [[nodiscard]] static int loadOpllKeyOffForceSilenceSeconds() {
        const auto file = settingsFile();
        if (!file.existsAsFile()) {
            return 20;
        }
        return juce::jlimit(
            0,
            9999,
            static_cast<int>(GetPrivateProfileIntW(
                L"SoundOutput",
                L"OpllKeyOffForceSilenceSeconds",
                20,
                file.getFullPathName().toWideCharPointer())));
    }

    void saveOpllKeyOffForceSilenceSeconds() const {
        const auto file = settingsFile();
        if (file.getParentDirectory().createDirectory().failed()) {
            return;
        }
        const auto path = file.getFullPathName();
        const auto value =
            juce::String(opll_keyoff_force_silence_seconds_);
        static_cast<void>(WritePrivateProfileStringW(
            L"SoundOutput",
            L"OpllKeyOffForceSilenceSeconds",
            value.toWideCharPointer(),
            path.toWideCharPointer()));
        static_cast<void>(WritePrivateProfileStringW(
            nullptr, nullptr, nullptr, path.toWideCharPointer()));
    }

    void stopActiveAudio() {
        waitForPcAudioRecovery();
        if (active_audio_ != nullptr) {
            active_audio_->stop();
            active_audio_ = nullptr;
        }
        asio_audio_.close();
        wasapi_audio_.stop();
    }

    static void drainAudioStatuses(
        mgstc::audio::AudioSink& audio) noexcept {
        mgstc::audio::AudioSinkStatus status{};
        while (audio.pollStatus(status)) {
        }
    }

    // Device teardown / open takes hundreds of ms normally and seconds when the
    // endpoint is disappearing — which is exactly when a device error arrives.
    // Never run it on the message thread from the poll timer: that froze the UI
    // with no user action ("偶発フリーズ"). The timer only starts and collects.
    void beginPcAudioRecovery(
        std::string note,
        bool restart_wasapi,
        bool close_asio) {
        joinDeviceWorkerThread();
        active_audio_ = nullptr;
        pc_audio_backend_ = PcAudioBackend::Wasapi;
        pc_audio_note_ = std::move(note);
        if (close_asio) {
            MGSTC_UI_ACTIVITY("audio: ASIO close after device error");
            asio_audio_.close();
        }
        device_recovery_restart_ = restart_wasapi;
        device_recovery_started_.store(false, std::memory_order_relaxed);
        device_recovery_cancel_.store(false, std::memory_order_relaxed);
        device_recovery_done_.store(false, std::memory_order_release);
        try {
            device_worker_ =
                std::thread([this] { runPcAudioRecovery(); });
            device_recovery_busy_ = true;
        } catch (...) {
            // No worker available: do it inline rather than leave the sink
            // half-torn-down.
            MGSTC_UI_ACTIVITY("audio: inline device recovery");
            runPcAudioRecovery();
            finishPcAudioRecovery();
        }
    }

    // Device worker thread.
    void runPcAudioRecovery() {
        wasapi_audio_.stop();
        bool started = false;
        if (device_recovery_restart_
            && !device_recovery_cancel_.load(std::memory_order_acquire)) {
            started = wasapi_audio_.start(engine_);
        }
        device_recovery_started_.store(started, std::memory_order_relaxed);
        device_recovery_done_.store(true, std::memory_order_release);
    }

    void finishPcAudioRecovery() {
        joinDeviceWorkerThread();
        device_recovery_busy_ = false;
        const bool started =
            device_recovery_started_.load(std::memory_order_relaxed);
        wasapi_audio_.setMasterVolumePercent(
            static_cast<std::uint32_t>(master_volume_percent_));
        active_audio_ = started ? &wasapi_audio_ : nullptr;
        if (device_recovery_restart_ && !started) {
            pc_audio_note_ +=
                "\n既定のWASAPI出力も開始できません。";
        }
        drainAudioStatuses(wasapi_audio_);
        savePcAudioSettings();
        publishHangHints();
    }

    void joinDeviceWorkerThread() {
        if (!device_worker_.joinable()) {
            return;
        }
        MGSTC_UI_ACTIVITY("audio: waiting for device worker");
        device_worker_.join();
    }

    // User-initiated device work must not race the recovery worker.
    void waitForPcAudioRecovery() {
        if (!device_recovery_busy_) {
            return;
        }
        device_recovery_cancel_.store(true, std::memory_order_release);
        finishPcAudioRecovery();
    }

    void timerCallback() override {
        pollOpllKeyOffForceSilence();
        if (device_recovery_busy_) {
            if (device_recovery_done_.load(std::memory_order_acquire)) {
                finishPcAudioRecovery();
            }
            return;
        }
        if (suppress_pc_audio_fallback_ || active_audio_ == nullptr) {
            return;
        }

        mgstc::audio::AudioSinkStatus status{};
        bool device_failed = false;
        while (active_audio_->pollStatus(status)) {
            device_failed = device_failed
                || status.type
                    == mgstc::audio::AudioSinkStatusType::DeviceError
                || (status.type
                        == mgstc::audio::AudioSinkStatusType::Stopped
                    && !active_audio_->running());
        }
        if (!device_failed && active_audio_->running()) {
            return;
        }

        if (active_audio_ == &asio_audio_) {
            const auto detail = asio_audio_.lastError();
            beginPcAudioRecovery(
                "ASIOデバイスが停止したため既定出力へ戻しました。"
                    + (detail.empty() ? std::string{}
                                      : "\n" + detail),
                /*restart_wasapi=*/true,
                /*close_asio=*/true);
            return;
        }

        beginPcAudioRecovery(
            "既定のWASAPI出力でデバイスエラーが発生しました。",
            /*restart_wasapi=*/false,
            /*close_asio=*/false);
    }

    void loadAndStartPcAudioSettings() {
        MGSTC_UI_ACTIVITY("startup: PC audio device open");
        const auto file = settingsFile();
        wchar_t backend_buffer[32]{};
        wchar_t driver_buffer[256]{};
        if (file.existsAsFile()) {
            static_cast<void>(GetPrivateProfileStringW(
                L"PcAudio",
                L"Backend",
                L"WASAPI",
                backend_buffer,
                static_cast<DWORD>(std::size(backend_buffer)),
                file.getFullPathName().toWideCharPointer()));
            static_cast<void>(GetPrivateProfileStringW(
                L"PcAudio",
                L"AsioDriver",
                L"",
                driver_buffer,
                static_cast<DWORD>(std::size(driver_buffer)),
                file.getFullPathName().toWideCharPointer()));
        }
        asio_driver_name_ =
            juce::String(driver_buffer).toStdString();

        if (juce::String(backend_buffer) == "ASIO"
            && !asio_driver_name_.empty()
            && asio_audio_.open(asio_driver_name_)
            && asio_audio_.start(engine_)) {
            pc_audio_backend_ = PcAudioBackend::Asio;
            active_audio_ = &asio_audio_;
            drainAudioStatuses(asio_audio_);
            return;
        }

        if (juce::String(backend_buffer) == "ASIO") {
            pc_audio_note_ = "保存済みASIOを開始できないため、"
                "既定出力を使用しています。";
            asio_audio_.close();
        }
        pc_audio_backend_ = PcAudioBackend::Wasapi;
        if (wasapi_audio_.start(engine_)) {
            active_audio_ = &wasapi_audio_;
            drainAudioStatuses(wasapi_audio_);
        } else {
            pc_audio_note_ += pc_audio_note_.empty() ? "" : "\n";
            pc_audio_note_ +=
                "既定のWASAPI出力を開始できません。";
        }
    }

    void savePcAudioSettings() const {
        const auto file = settingsFile();
        if (file.getParentDirectory().createDirectory().failed()) {
            return;
        }
        const auto path = file.getFullPathName();
        const auto* backend = pc_audio_backend_ == PcAudioBackend::Asio
            ? L"ASIO"
            : L"WASAPI";
        static_cast<void>(WritePrivateProfileStringW(
            L"PcAudio",
            L"Backend",
            backend,
            path.toWideCharPointer()));
        static_cast<void>(WritePrivateProfileStringW(
            L"PcAudio",
            L"AsioDriver",
            juce::String(asio_driver_name_).toWideCharPointer(),
            path.toWideCharPointer()));
        static_cast<void>(WritePrivateProfileStringW(
            nullptr,
            nullptr,
            nullptr,
            path.toWideCharPointer()));
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
    mgstc::audio::WasapiAudioSink wasapi_audio_;
    mgstc::audio::AsioAudioSink asio_audio_;
    mgstc::audio::AudioSink* active_audio_{};
    SccWaveform shared_scc_wave_{
        mgstc::engine::generateSccPreset(
            mgstc::engine::SccWavePreset::Sine,
            mgstc::engine::SccHarmonic::One)};
    mgstc::engine::OpllPatchParameters shared_opll_patch_{
        mgstc::engine::defaultOpllPatch()};
    std::uint64_t shared_scc_wave_revision_{1};
    std::uint64_t shared_opll_patch_revision_{1};
    bool shared_editor_program_active_{};
    PcAudioBackend pc_audio_backend_{PcAudioBackend::Wasapi};
    std::string asio_driver_name_;
    std::string pc_audio_note_;
    bool suppress_pc_audio_fallback_{};
    // Message thread owns these; only the two atomics cross to the worker.
    std::thread device_worker_;
    bool device_recovery_busy_{};
    bool device_recovery_restart_{};
    std::atomic<bool> device_recovery_started_{false};
    std::atomic<bool> device_recovery_done_{false};
    std::atomic<bool> device_recovery_cancel_{false};
    int master_volume_percent_{100};
    std::uint64_t master_volume_revision_{1};
    int opll_keyoff_force_silence_seconds_{20};
    struct OpllSilenceDue {
        std::uint8_t track{};
        double due_ms{};
    };
    std::vector<OpllSilenceDue> opll_silence_dues_;
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
        title_.setFont(UiFonts::title());
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

// UI spacing scale (4px base). New controls should use these values.
namespace UiLayout {
// Unscaled design tokens (100%). Runtime values below are multiplied by UiScale.
namespace Base {
constexpr int xs = 4;
constexpr int sm = 8;
constexpr int md = 12;
constexpr int lg = 16;
constexpr int xl = 24;

constexpr int pageMargin = 24;
constexpr int pageMarginPaint = 20;
constexpr int panelGap = 16;
constexpr int panelRadius = 9;
constexpr int panelPad = 10;
constexpr int rowGap = 8;
constexpr int controlGap = 6;
constexpr int iconButton = 40;
constexpr int masterVolumeSize = 48;
constexpr int switchTrackW = 34;
constexpr int switchTrackH = 18;
constexpr int switchThumb = 14;
constexpr int switchLabelPad = 8; // track right → label
constexpr int textButtonH = 36;
constexpr int fieldH = 34;
constexpr int titleH = 36;
constexpr int descriptionH = 30;
constexpr int keyboardH = 94;
constexpr int keyboardGap = 10;
constexpr int statusH = 40;
constexpr int toolbarH = 44;

constexpr int editorWindowW = 1600;
constexpr int editorWindowH = 1050;
constexpr int libraryWidth = 360;
constexpr int libraryTitleH = 30;
constexpr int libraryMemoH = 80;
constexpr int libraryButtonH = 36;
constexpr int libraryButtonMinW = 80;
constexpr int libraryManageButtonW = 108;
constexpr int compositeToolRowH = 24;
constexpr int compositeToolGap = 5;
constexpr int commandStackH = 108;
constexpr int commandSummaryH = 36;
constexpr int envelopePreviewH = 48; // 2-line selectable MGSC preview
constexpr int editSubLaneLabelW = 44;
constexpr int setupValueLabelW = 64; // relative pitch etc.
constexpr int editMarkerLaneH = 16; // @ / y: no vertical value extent
constexpr int registerAutoLaneH = 44;
constexpr int compositeMixLaneH = 104;
constexpr int compositeEditLaneH = 520;
constexpr int registerAutoLanePairExtra = 44 * 2;
constexpr int compositeSetupColumnW = 420;
constexpr int countColumnMinW = 28;
constexpr int countColumnMaxW = 72;
constexpr int compositeTimbrePickW = 360;
constexpr int compositeTimbrePickH = 220;
constexpr int settingsDialogW = 500;
constexpr int settingsDialogH = 720;
constexpr int libraryManagerW = 1100;
constexpr int libraryManagerH = 700;
constexpr int channelLaneInnerPad = 4;
constexpr int channelLaneFrame = 6;
} // namespace Base

inline int xs = Base::xs;
inline int sm = Base::sm;
inline int md = Base::md;
inline int lg = Base::lg;
inline int xl = Base::xl;

inline int pageMargin = Base::pageMargin;
inline int pageMarginPaint = Base::pageMarginPaint;
inline int panelGap = Base::panelGap;
inline int panelRadius = Base::panelRadius;
inline int panelPad = Base::panelPad;
inline int rowGap = Base::rowGap;
inline int controlGap = Base::controlGap;
inline int iconButton = Base::iconButton;
inline int masterVolumeSize = Base::masterVolumeSize;
inline int switchTrackW = Base::switchTrackW;
inline int switchTrackH = Base::switchTrackH;
inline int switchThumb = Base::switchThumb;
inline int switchLabelPad = Base::switchLabelPad;
inline int textButtonH = Base::textButtonH;
inline int fieldH = Base::fieldH;
inline int titleH = Base::titleH;
inline int descriptionH = Base::descriptionH;
inline int keyboardH = Base::keyboardH;
inline int keyboardGap = Base::keyboardGap;
inline int statusH = Base::statusH;
inline int toolbarH = Base::toolbarH;

inline int editorWindowW = Base::editorWindowW;
inline int editorWindowH = Base::editorWindowH;
inline int libraryWidth = Base::libraryWidth;
inline int libraryTitleH = Base::libraryTitleH;
inline int libraryMemoH = Base::libraryMemoH;
inline int libraryButtonH = Base::libraryButtonH;
inline int libraryButtonMinW = Base::libraryButtonMinW;
inline int libraryManageButtonW = Base::libraryManageButtonW;
inline int compositeToolRowH = Base::compositeToolRowH;
inline int compositeToolGap = Base::compositeToolGap;
inline int commandStackH = Base::commandStackH;
inline int commandSummaryH = Base::commandSummaryH;
inline int envelopePreviewH = Base::envelopePreviewH;
inline int editSubLaneLabelW = Base::editSubLaneLabelW;
inline int setupValueLabelW = Base::setupValueLabelW;
inline int editMarkerLaneH = Base::editMarkerLaneH;
inline int registerAutoLaneH = Base::registerAutoLaneH;
inline int compositeSetupContentH = 0;
inline int compositeChannelLaneH = 0;
inline int compositeMixLaneH = Base::compositeMixLaneH;
inline int compositeEditLaneH = Base::compositeEditLaneH;
inline int compositeEditLaneOpllExtraH = Base::registerAutoLanePairExtra;
inline int compositeSetupColumnW = Base::compositeSetupColumnW;
inline int compositeLaneLabelW = Base::compositeSetupColumnW;
inline int countColumnMinW = Base::countColumnMinW;
inline int countColumnMaxW = Base::countColumnMaxW;
inline int compositeTimbrePickW = Base::compositeTimbrePickW;
inline int compositeTimbrePickH = Base::compositeTimbrePickH;
inline int compositeLayerLibraryH = 0;
inline int settingsDialogW = Base::settingsDialogW;
inline int settingsDialogH = Base::settingsDialogH;
inline int libraryManagerW = Base::libraryManagerW;
inline int libraryManagerH = Base::libraryManagerH;

constexpr juce::uint32 panelFill = 0xFF29323C;
constexpr juce::uint32 panelStroke = 0xFF435160;
constexpr juce::uint32 pageFill = 0xFF20262E;
constexpr juce::uint32 pageFillBottom = 0xFF161B22;

inline void recomputeDerived() {
    compositeSetupContentH =
        compositeToolRowH + xs
        + fieldH + xs
        + fieldH + xs
        + fieldH + xs
        + fieldH + xs
        + fieldH + xs
        + fieldH;
    compositeChannelLaneH =
        compositeSetupContentH + xs * 2
        + UiScale::sx(Base::channelLaneInnerPad)
        + UiScale::sx(Base::channelLaneFrame);
    compositeLaneLabelW = compositeSetupColumnW;
    compositeEditLaneOpllExtraH = registerAutoLaneH * 2;
    compositeLayerLibraryH =
        panelPad * 2 + libraryTitleH + sm + fieldH * 2 + sm + fieldH;
}

inline void applyScale(float factor) {
    auto s = [factor](int value) {
        return juce::jmax(
            1, juce::roundToInt(static_cast<float>(value) * factor));
    };
    xs = s(Base::xs);
    sm = s(Base::sm);
    md = s(Base::md);
    lg = s(Base::lg);
    xl = s(Base::xl);
    pageMargin = s(Base::pageMargin);
    pageMarginPaint = s(Base::pageMarginPaint);
    panelGap = s(Base::panelGap);
    panelRadius = s(Base::panelRadius);
    panelPad = s(Base::panelPad);
    rowGap = s(Base::rowGap);
    controlGap = s(Base::controlGap);
    iconButton = s(Base::iconButton);
    masterVolumeSize = s(Base::masterVolumeSize);
    switchTrackW = s(Base::switchTrackW);
    switchTrackH = s(Base::switchTrackH);
    switchThumb = s(Base::switchThumb);
    switchLabelPad = s(Base::switchLabelPad);
    textButtonH = s(Base::textButtonH);
    fieldH = s(Base::fieldH);
    titleH = s(Base::titleH);
    descriptionH = s(Base::descriptionH);
    keyboardH = s(Base::keyboardH);
    keyboardGap = s(Base::keyboardGap);
    statusH = s(Base::statusH);
    toolbarH = s(Base::toolbarH);
    editorWindowW = s(Base::editorWindowW);
    editorWindowH = s(Base::editorWindowH);
    libraryWidth = s(Base::libraryWidth);
    libraryTitleH = s(Base::libraryTitleH);
    libraryMemoH = s(Base::libraryMemoH);
    libraryButtonH = s(Base::libraryButtonH);
    libraryButtonMinW = s(Base::libraryButtonMinW);
    libraryManageButtonW = s(Base::libraryManageButtonW);
    compositeToolRowH = s(Base::compositeToolRowH);
    compositeToolGap = s(Base::compositeToolGap);
    commandStackH = s(Base::commandStackH);
    commandSummaryH = s(Base::commandSummaryH);
    envelopePreviewH = s(Base::envelopePreviewH);
    editSubLaneLabelW = s(Base::editSubLaneLabelW);
    setupValueLabelW = s(Base::setupValueLabelW);
    editMarkerLaneH = s(Base::editMarkerLaneH);
    registerAutoLaneH = s(Base::registerAutoLaneH);
    compositeMixLaneH = s(Base::compositeMixLaneH);
    compositeEditLaneH = s(Base::compositeEditLaneH);
    compositeSetupColumnW = s(Base::compositeSetupColumnW);
    countColumnMinW = s(Base::countColumnMinW);
    countColumnMaxW = s(Base::countColumnMaxW);
    compositeTimbrePickW = s(Base::compositeTimbrePickW);
    compositeTimbrePickH = s(Base::compositeTimbrePickH);
    settingsDialogW = s(Base::settingsDialogW);
    settingsDialogH = s(Base::settingsDialogH);
    libraryManagerW = s(Base::libraryManagerW);
    libraryManagerH = s(Base::libraryManagerH);
    recomputeDerived();
}

// Preferred width for a SwitchLookAndFeel toggle: track + pad + label + trailing xs.
[[nodiscard]] inline int switchControlWidth(
    const juce::String& label,
    int control_height = -1) {
    if (control_height < 0) {
        control_height = textButtonH;
    }
    const auto font = UiFonts::make(
        UiFonts::controlTextHeight(
            static_cast<float>(control_height)));
    const int text_w =
        juce::GlyphArrangement::getStringWidthInt(font, label);
    return switchTrackW + switchLabelPad + text_w + xs;
}

struct UiLayoutInit final {
    UiLayoutInit() { applyScale(1.0F); }
};
inline UiLayoutInit ui_layout_init{};
} // namespace UiLayout

inline void UiScale::applyLayoutMetrics(float factor) {
    UiLayout::applyScale(factor);
}

// Defined near MainWindow; editors/dialogs call it after preferred-size changes.
void clampWindowToDisplayWorkArea(juce::ResizableWindow& window);

// Apply active metrics' preferred content size and refresh LookAndFeel fonts.
inline void applyScaledContentSize(
    juce::Component& content,
    int preferred_w,
    int preferred_h,
    bool clamp_host_window) {
    content.setSize(preferred_w, preferred_h);
    content.sendLookAndFeelChange();
    content.resized();
    content.repaint();
    if (!clamp_host_window) {
        return;
    }
    if (auto* top = content.getTopLevelComponent()) {
        if (auto* window = dynamic_cast<juce::ResizableWindow*>(top)) {
            // Preferred size may exceed the work area; clamp keeps chrome visible.
            window->setSize(
                juce::jmax(preferred_w, window->getWidth()),
                juce::jmax(preferred_h, window->getHeight()));
            // Always set to preferred first so shrink works too.
            window->setSize(preferred_w, preferred_h);
            clampWindowToDisplayWorkArea(*window);
        }
    }
}

// Top-right chrome: settings / master volume / optional immediate audition.
// Icon buttons use the same UiLayout::iconButton size as Open/Save/Copy/Paste.
inline void layoutEditorTopRightChrome(
    int host_width,
    juce::DrawableButton& settings,
    juce::Slider& master_volume,
    juce::DrawableButton* immediate_audition = nullptr) {
    using namespace UiLayout;
    const int top = pageMargin;
    int right = host_width - pageMargin;
    settings.setBounds(right - iconButton, top, iconButton, iconButton);
    right -= iconButton + controlGap;
    const int vol_top = juce::jmax(
        0, top - (masterVolumeSize - iconButton) / 2);
    master_volume.setBounds(
        right - masterVolumeSize,
        vol_top,
        masterVolumeSize,
        masterVolumeSize);
    right -= masterVolumeSize + controlGap;
    if (immediate_audition != nullptr) {
        immediate_audition->setBounds(
            right - iconButton, top, iconButton, iconButton);
    }
}


void paintPageBackground(
    juce::Graphics& graphics,
    juce::Rectangle<int> bounds) {
    juce::ColourGradient grad(
        juce::Colour(UiLayout::pageFill),
        static_cast<float>(bounds.getCentreX()),
        static_cast<float>(bounds.getY()),
        juce::Colour(UiLayout::pageFillBottom),
        static_cast<float>(bounds.getCentreX()),
        static_cast<float>(bounds.getBottom()),
        false);
    graphics.setGradientFill(grad);
    graphics.fillRect(bounds);
}

void fillRoundedPanelFrame(
    juce::Graphics& graphics,
    juce::Rectangle<int> bounds) {
    if (bounds.isEmpty()) {
        return;
    }
    graphics.setColour(juce::Colour(UiLayout::panelFill));
    graphics.fillRoundedRectangle(
        bounds.toFloat(),
        static_cast<float>(UiLayout::panelRadius));
}

void strokeRoundedPanelFrame(
    juce::Graphics& graphics,
    juce::Rectangle<int> bounds) {
    if (bounds.isEmpty()) {
        return;
    }
    graphics.setColour(juce::Colour(UiLayout::panelStroke));
    graphics.drawRoundedRectangle(
        bounds.toFloat(),
        static_cast<float>(UiLayout::panelRadius),
        1.0F);
}

void paintRoundedPanelFrame(
    juce::Graphics& graphics,
    juce::Rectangle<int> bounds) {
    fillRoundedPanelFrame(graphics, bounds);
    strokeRoundedPanelFrame(graphics, bounds);
}

struct TimbreLibraryWidgets {
    juce::Label& title;
    juce::TextEditor& filter;
    juce::TextButton& tag_filter;
    juce::TextButton& tag_manage;
    juce::ToggleButton& favorite_only;
    juce::TextButton& ab_audition;
    juce::ComboBox& sort;
    juce::ComboBox& list;
    juce::TextButton& library_load;
    juce::TextButton& library_delete;
    juce::TextEditor& name;
    juce::TextButton& library_rename;
    juce::TextButton& tags;
    juce::TextEditor& memo;
    juce::ToggleButton& favorite;
    juce::TextButton& library_save;
    juce::TextButton& library_save_as;
    juce::TextButton& library_new;
    juce::TextButton& library_import;
    juce::TextButton& library_export;
    juce::Label& mgsc_title;
    juce::Label& output_number_label;
    juce::TextEditor& output_number;
    juce::TextEditor& mgsc_preview;
};

void layoutTimbreLibraryPanel(
    juce::Rectangle<int> panel,
    TimbreLibraryWidgets widgets) {
    using namespace UiLayout;
    auto area = panel.reduced(panelPad);
    widgets.title.setBounds(area.removeFromTop(libraryTitleH));
    area.removeFromTop(sm);
    widgets.filter.setBounds(area.removeFromTop(fieldH));
    area.removeFromTop(sm);
    auto tag_row = area.removeFromTop(fieldH);
    widgets.tag_manage.setBounds(
        tag_row.removeFromRight(libraryManageButtonW));
    tag_row.removeFromRight(controlGap);
    widgets.tag_filter.setBounds(tag_row);
    area.removeFromTop(sm);
    auto filter_options = area.removeFromTop(fieldH);
    widgets.favorite_only.setBounds(
        filter_options.removeFromLeft(UiScale::sx(100)));
    filter_options.removeFromLeft(controlGap);
    widgets.ab_audition.setBounds(
        filter_options.removeFromLeft(UiScale::sx(48)));
    filter_options.removeFromLeft(controlGap);
    widgets.sort.setBounds(filter_options);
    area.removeFromTop(sm);

    auto list_row = area.removeFromTop(fieldH + xs);
    widgets.library_delete.setBounds(
        list_row.removeFromRight(UiScale::sx(54)));
    list_row.removeFromRight(controlGap);
    widgets.library_load.setBounds(
        list_row.removeFromRight(UiScale::sx(54)));
    list_row.removeFromRight(controlGap);
    widgets.list.setBounds(list_row);
    area.removeFromTop(sm);

    auto name_row = area.removeFromTop(fieldH);
    widgets.library_rename.setBounds(
        name_row.removeFromRight(UiScale::sx(88)));
    name_row.removeFromRight(controlGap);
    widgets.name.setBounds(name_row);
    area.removeFromTop(sm);
    widgets.tags.setBounds(area.removeFromTop(fieldH));
    area.removeFromTop(sm);
    widgets.memo.setBounds(area.removeFromTop(libraryMemoH));
    area.removeFromTop(xs + xs / 2);
    widgets.favorite.setBounds(area.removeFromTop(fieldH));
    area.removeFromTop(sm);

    auto edit_row = area.removeFromTop(libraryButtonH);
    const int edit_w =
        (edit_row.getWidth() - controlGap * 2) / 3;
    widgets.library_save.setBounds(
        edit_row.removeFromLeft(juce::jmax(libraryButtonMinW, edit_w)));
    edit_row.removeFromLeft(controlGap);
    widgets.library_save_as.setBounds(
        edit_row.removeFromLeft(juce::jmax(libraryButtonMinW, edit_w)));
    edit_row.removeFromLeft(controlGap);
    widgets.library_new.setBounds(edit_row);

    area.removeFromTop(xs + xs / 2);
    auto file_row = area.removeFromTop(libraryButtonH);
    const int file_w =
        (file_row.getWidth() - controlGap) / 2;
    widgets.library_import.setBounds(
        file_row.removeFromLeft(juce::jmax(libraryButtonMinW, file_w)));
    file_row.removeFromLeft(controlGap);
    widgets.library_export.setBounds(file_row);

    area.removeFromTop(sm);
    widgets.mgsc_title.setBounds(area.removeFromTop(libraryTitleH));
    area.removeFromTop(xs);
    auto number_row = area.removeFromTop(fieldH);
    widgets.output_number_label.setBounds(
        number_row.removeFromLeft(UiScale::sx(118)));
    number_row.removeFromLeft(sm);
    widgets.output_number.setBounds(
        number_row.removeFromLeft(UiScale::sx(54)));
    area.removeFromTop(sm);
    widgets.mgsc_preview.setBounds(area);
}

struct CompositeLibraryWidgets {
    juce::Label& title;
    juce::TextEditor& filter;
    juce::TextButton& tag_filter;
    juce::TextButton& tag_manage;
    juce::ToggleButton& favorite_only;
    juce::TextButton& ab_audition;
    juce::ComboBox& sort;
    juce::ComboBox& list;
    juce::TextButton& library_load;
    juce::TextButton& library_delete;
    juce::TextEditor& name;
    juce::TextButton& library_rename;
    juce::TextButton& tags;
    juce::ToggleButton& favorite;
    juce::TextButton& library_save;
    juce::TextButton& library_save_as;
    juce::TextButton& library_new;
    juce::TextButton& library_duplicate;
};

void layoutCompositeLibraryPanel(
    juce::Rectangle<int> panel,
    CompositeLibraryWidgets widgets) {
    using namespace UiLayout;
    auto area = panel.reduced(panelPad);
    widgets.title.setBounds(area.removeFromTop(libraryTitleH));
    area.removeFromTop(sm);
    widgets.filter.setBounds(area.removeFromTop(fieldH));
    area.removeFromTop(sm);
    auto tag_row = area.removeFromTop(fieldH);
    widgets.tag_manage.setBounds(
        tag_row.removeFromRight(libraryManageButtonW));
    tag_row.removeFromRight(controlGap);
    widgets.tag_filter.setBounds(tag_row);
    area.removeFromTop(sm);
    auto filter_options = area.removeFromTop(fieldH);
    widgets.favorite_only.setBounds(
        filter_options.removeFromLeft(UiScale::sx(100)));
    filter_options.removeFromLeft(controlGap);
    widgets.ab_audition.setBounds(
        filter_options.removeFromLeft(UiScale::sx(48)));
    filter_options.removeFromLeft(controlGap);
    widgets.sort.setBounds(filter_options);
    area.removeFromTop(sm);

    auto list_row = area.removeFromTop(fieldH + xs);
    widgets.library_delete.setBounds(
        list_row.removeFromRight(UiScale::sx(54)));
    list_row.removeFromRight(controlGap);
    widgets.library_load.setBounds(
        list_row.removeFromRight(UiScale::sx(54)));
    list_row.removeFromRight(controlGap);
    widgets.list.setBounds(list_row);
    area.removeFromTop(sm);

    auto name_row = area.removeFromTop(fieldH);
    widgets.library_rename.setBounds(
        name_row.removeFromRight(UiScale::sx(88)));
    name_row.removeFromRight(controlGap);
    widgets.name.setBounds(name_row);
    area.removeFromTop(sm);
    widgets.tags.setBounds(area.removeFromTop(fieldH));
    area.removeFromTop(sm);
    widgets.favorite.setBounds(area.removeFromTop(fieldH));
    area.removeFromTop(sm);

    auto edit_row = area.removeFromTop(libraryButtonH);
    const int edit_w =
        (edit_row.getWidth() - controlGap * 2) / 3;
    widgets.library_save.setBounds(
        edit_row.removeFromLeft(juce::jmax(libraryButtonMinW, edit_w)));
    edit_row.removeFromLeft(controlGap);
    widgets.library_save_as.setBounds(
        edit_row.removeFromLeft(juce::jmax(libraryButtonMinW, edit_w)));
    edit_row.removeFromLeft(controlGap);
    widgets.library_new.setBounds(edit_row);

    area.removeFromTop(xs + xs / 2);
    auto extra_row = area.removeFromTop(libraryButtonH);
    widgets.library_duplicate.setBounds(
        extra_row.removeFromLeft(
            juce::jmax(libraryButtonMinW, extra_row.getWidth() / 2)));
}

void layoutCompositeLayerLibraryPanel(
    juce::Rectangle<int> panel,
    juce::Label& title,
    juce::Label& hint,
    juce::ToggleButton& favorite_only) {
    using namespace UiLayout;
    auto area = panel.reduced(panelPad);
    title.setBounds(area.removeFromTop(libraryTitleH));
    area.removeFromTop(sm);
    hint.setBounds(area.removeFromTop(fieldH * 2));
    area.removeFromTop(sm);
    favorite_only.setBounds(area.removeFromTop(fieldH));
}

enum class LibraryManagerKind : std::uint8_t {
    Scc,
    Opll,
    Composite,
};

struct LibraryManagerRow {
    LibraryManagerKind kind{};
    std::uint64_t id{};
    std::string name;
    std::vector<std::string> tags;
    std::string memo;
    bool favorite{};
    std::uint32_t revision{1};
    std::int64_t updated_unix_seconds{};
    std::int64_t last_used_unix_seconds{};
};

[[nodiscard]] juce::String formatLibraryManagerTime(
    std::int64_t unix_seconds) {
    if (unix_seconds <= 0) {
        return juce::String::fromUTF8("—");
    }
    return juce::Time(unix_seconds * 1000)
        .formatted("%Y-%m-%d %H:%M");
}

class LibraryManagerTableModel final
    : public juce::TableListBoxModel {
public:
    enum Column {
        kPreview = 1,
        kName = 2,
        kTags = 3,
        kUpdated = 4,
        kFavorite = 5,
    };

    using PreviewHandler =
        std::function<void(const LibraryManagerRow&)>;
    using FavoriteHandler =
        std::function<void(int, const LibraryManagerRow&)>;
    using SelectionHandler = std::function<void()>;
    using SortHandler = std::function<void()>;

    void setTable(juce::TableListBox* table) {
        table_ = table;
    }

    void setRows(std::vector<LibraryManagerRow> rows) {
        rows_ = std::move(rows);
    }

    [[nodiscard]] LibraryManagerRow* rowAtMutable(int index) {
        if (index < 0
            || static_cast<std::size_t>(index) >= rows_.size()) {
            return nullptr;
        }
        return &rows_[static_cast<std::size_t>(index)];
    }

    void setPreviewHandler(PreviewHandler handler) {
        preview_handler_ = std::move(handler);
    }

    void setFavoriteHandler(FavoriteHandler handler) {
        favorite_handler_ = std::move(handler);
    }

    void setSelectionHandler(SelectionHandler handler) {
        selection_handler_ = std::move(handler);
    }

    void setSortHandler(SortHandler handler) {
        sort_handler_ = std::move(handler);
    }

    void setRecentUsedMode(bool enabled) {
        recent_used_mode_ = enabled;
    }

    [[nodiscard]] bool recentUsedMode() const {
        return recent_used_mode_;
    }

    [[nodiscard]] int sortColumnId() const {
        return sort_column_id_;
    }

    [[nodiscard]] bool sortForwards() const {
        return sort_forwards_;
    }

    [[nodiscard]] const LibraryManagerRow* rowAt(int index) const {
        if (index < 0
            || static_cast<std::size_t>(index) >= rows_.size()) {
            return nullptr;
        }
        return &rows_[static_cast<std::size_t>(index)];
    }

    int getNumRows() override {
        return static_cast<int>(rows_.size());
    }

    void paintRowBackground(
        juce::Graphics& graphics,
        int row_number,
        int width,
        int height,
        bool row_is_selected) override {
        juce::ignoreUnused(width);
        if (row_is_selected) {
            graphics.fillAll(juce::Colour(0xFF34404A));
        } else if (row_number % 2 == 0) {
            graphics.fillAll(juce::Colour(0xFF232B34));
        } else {
            graphics.fillAll(juce::Colour(0xFF1E252D));
        }
        graphics.setColour(juce::Colour(UiLayout::panelStroke));
        graphics.drawHorizontalLine(
            height - 1, 0.0F, static_cast<float>(width));
    }

    void paintCell(
        juce::Graphics& graphics,
        int row_number,
        int column_id,
        int width,
        int height,
        bool row_is_selected) override {
        juce::ignoreUnused(row_is_selected);
        const auto* row = rowAt(row_number);
        if (row == nullptr) {
            return;
        }
        graphics.setFont(UiFonts::body());
        graphics.setColour(juce::Colour(0xFFE6EDF3));
        if (column_id == kPreview) {
            if (auto icon = makeEditorIcon(
                    EditorIcon::Audition,
                    juce::Colour(0xFF9AA8B5))) {
                const int icon_size =
                    juce::jmin(width, height) - UiLayout::xs * 2;
                icon->drawWithin(
                    graphics,
                    juce::Rectangle<float>(
                        static_cast<float>((width - icon_size) / 2),
                        static_cast<float>((height - icon_size) / 2),
                        static_cast<float>(icon_size),
                        static_cast<float>(icon_size)),
                    juce::RectanglePlacement::centred,
                    1.0F);
            }
            return;
        }
        if (column_id == kFavorite) {
            graphics.setColour(
                row->favorite ? juce::Colour(0xFFFFD866)
                              : juce::Colour(0xFF68737E));
            graphics.drawText(
                juce::String::fromUTF8("★"),
                0,
                0,
                width,
                height,
                juce::Justification::centred,
                false);
            return;
        }
        juce::String text;
        if (column_id == kName) {
            text = juce::String::fromUTF8(row->name.c_str());
            if (row->kind != LibraryManagerKind::Composite) {
                text += " (r"
                    + juce::String(static_cast<int>(row->revision))
                    + ")";
            }
        } else if (column_id == kTags) {
            if (row->tags.empty()) {
                text = juce::String::fromUTF8("（タグなし）");
            } else {
                juce::StringArray parts;
                for (const auto& tag : row->tags) {
                    parts.add(juce::String::fromUTF8(tag.c_str()));
                }
                text = parts.joinIntoString(" ");
            }
        } else if (column_id == kUpdated) {
            text = formatLibraryManagerTime(row->updated_unix_seconds);
        }
        graphics.drawText(
            text,
            UiLayout::xs,
            0,
            width - UiLayout::xs * 2,
            height,
            juce::Justification::centredLeft,
            true);
    }

    void sortOrderChanged(
        int new_sort_column_id,
        bool is_forwards) override {
        if (recent_used_mode_) {
            if (table_ != nullptr) {
                table_->getHeader().setSortColumnId(0, true);
            }
            return;
        }
        if (new_sort_column_id == 0
            || new_sort_column_id == kPreview) {
            return;
        }
        sort_column_id_ = new_sort_column_id;
        sort_forwards_ = is_forwards;
        if (sort_handler_) {
            sort_handler_();
        }
    }

    void cellClicked(
        int row_number,
        int column_id,
        const juce::MouseEvent& event) override {
        const auto* row = rowAt(row_number);
        if (row == nullptr) {
            return;
        }
        if (column_id == kPreview && preview_handler_) {
            preview_handler_(*row);
            return;
        }
        if (column_id == kFavorite && favorite_handler_) {
            if (!favoriteGlyphContains(row_number, event)) {
                return;
            }
            favorite_handler_(row_number, *row);
        }
    }

    void selectedRowsChanged(int) override {
        if (selection_handler_) {
            selection_handler_();
        }
    }

private:
    [[nodiscard]] bool favoriteGlyphContains(
        int row_number,
        const juce::MouseEvent& event) const {
        if (table_ == nullptr) {
            return false;
        }
        const int cell_w =
            table_->getHeader().getColumnWidth(kFavorite);
        const int cell_h = table_->getRowHeight();
        if (cell_w <= 0 || cell_h <= 0) {
            return false;
        }
        const auto font = UiFonts::body();
        const auto star = juce::String::fromUTF8("★");
        const float glyph_w = static_cast<float>(
            juce::GlyphArrangement::getStringWidthInt(font, star));
        const float glyph_h = font.getHeight();
        const auto glyph = juce::Rectangle<float>(
                               (static_cast<float>(cell_w) - glyph_w)
                                   * 0.5F,
                               (static_cast<float>(cell_h) - glyph_h)
                                   * 0.5F,
                               glyph_w,
                               glyph_h)
                               .expanded(1.0F);
        auto local = event.position;
        const auto cell = table_->getCellPosition(
            kFavorite, row_number, true);
        if (!cell.isEmpty()) {
            const auto table_pos =
                event.getEventRelativeTo(table_).getPosition();
            if (cell.contains(table_pos)) {
                local = (table_pos - cell.getPosition()).toFloat();
            }
        }
        return glyph.contains(local);
    }

    juce::TableListBox* table_{};
    std::vector<LibraryManagerRow> rows_;
    PreviewHandler preview_handler_;
    FavoriteHandler favorite_handler_;
    SelectionHandler selection_handler_;
    SortHandler sort_handler_;
    bool recent_used_mode_{};
    int sort_column_id_{kName};
    bool sort_forwards_{true};
};

class LibraryManagerListTab final : public juce::Component {
public:
    using PreviewCallback =
        std::function<void(LibraryManagerKind, std::uint64_t)>;
    using OpenEditorCallback =
        std::function<void(LibraryManagerKind, std::uint64_t)>;
    using PersistCallback = std::function<bool()>;
    using ReloadCallback = std::function<bool()>;
    using PerformanceTargetChangedCallback = std::function<void()>;
    using LibrariesChangedCallback = std::function<void()>;

    LibraryManagerListTab(
        LibraryManagerKind initial_kind,
        PreviewCallback preview,
        OpenEditorCallback open_editor,
        ReloadCallback reload,
        PersistCallback persist_timbres,
        PersistCallback persist_composites,
        std::function<mgstc::engine::TimbreLibrary*()> timbres,
        std::function<mgstc::engine::CompositeTimbreLibrary*()>
            composites,
        PerformanceTargetChangedCallback
            performance_target_changed = {},
        LibrariesChangedCallback libraries_changed = {})
        : preview_(std::move(preview)),
          open_editor_(std::move(open_editor)),
          reload_(std::move(reload)),
          persist_timbres_(std::move(persist_timbres)),
          persist_composites_(std::move(persist_composites)),
          timbres_(std::move(timbres)),
          composites_(std::move(composites)),
          performance_target_changed_(
              std::move(performance_target_changed)),
          libraries_changed_(std::move(libraries_changed)) {
        category_.addItem(juce::String::fromUTF8("SCC"), 1);
        category_.addItem(juce::String::fromUTF8("OPLL"), 2);
        category_.addItem(juce::String::fromUTF8("複合"), 3);
        const int initial_id =
            initial_kind == LibraryManagerKind::Opll
                ? 2
            : initial_kind == LibraryManagerKind::Composite ? 3
                                                           : 1;
        category_.setSelectedId(initial_id, juce::dontSendNotification);
        category_.onChange = [this] { refreshTable(true); };
        addAndMakeVisible(category_);

        filter_.setTextToShowWhenEmpty(
            juce::String::fromUTF8("名前・タグ・メモを検索"),
            juce::Colour(0xFF7F8993));
        filter_.onTextChange = [this] { refreshTable(true); };
        UiFonts::styleBodyField(filter_);
        addAndMakeVisible(filter_);

        tag_filter_.setButtonText(
            juce::String::fromUTF8("タグで絞り込み"));
        tag_filter_.setTooltip(
            juce::String::fromUTF8(
                "保存済み音色で使われているタグを複数選択します"
                "（すべて含むAND）"));
        tag_filter_.onClick = [this] { showTagFilter(); };
        addAndMakeVisible(tag_filter_);

        favorite_only_.setButtonText(
            juce::String::fromUTF8("★のみ"));
        favorite_only_.setLookAndFeel(&switch_look_and_feel_);
        favorite_only_.onClick = [this] { refreshTable(true); };
        addAndMakeVisible(favorite_only_);

        recent_used_.setButtonText(
            juce::String::fromUTF8("最近使った順"));
        recent_used_.setLookAndFeel(&switch_look_and_feel_);
        recent_used_.setTooltip(
            juce::String::fromUTF8(
                "ONのあいだは最終使用日時で並べ替え、"
                "列ヘッダでの昇順／降順は無効になります"));
        recent_used_.onClick = [this] { applyRecentUsedMode(); };
        addAndMakeVisible(recent_used_);

        table_model_.setTable(&table_);
        table_model_.setPreviewHandler(
            [this](const LibraryManagerRow& row) {
                if (preview_) {
                    preview_(row.kind, row.id);
                }
            });
        table_model_.setFavoriteHandler(
            [this](int row_number, const LibraryManagerRow& row) {
                toggleFavoriteAt(row_number, row);
            });
        table_model_.setSortHandler(
            [this] { refreshTable(false); });
        table_.setModel(&table_model_);
        table_.setMultipleSelectionEnabled(true);
        auto& header = table_.getHeader();
        header.addColumn(
            juce::String{},
            LibraryManagerTableModel::kPreview,
            34,
            28,
            -1,
            juce::TableHeaderComponent::notSortable);
        header.addColumn(
            juce::String::fromUTF8("名前"),
            LibraryManagerTableModel::kName,
            200);
        header.addColumn(
            juce::String::fromUTF8("タグ"),
            LibraryManagerTableModel::kTags,
            160);
        header.addColumn(
            juce::String::fromUTF8("更新日時"),
            LibraryManagerTableModel::kUpdated,
            130);
        header.addColumn(
            juce::String::fromUTF8("★"),
            LibraryManagerTableModel::kFavorite,
            36);
        header.setStretchToFitActive(true);
        header.setSortColumnId(
            LibraryManagerTableModel::kName, true);
        table_model_.setSelectionHandler([this] { updateDetail(); });
        table_.setColour(
            juce::ListBox::backgroundColourId,
            juce::Colour(UiLayout::panelFill));
        table_.setColour(
            juce::ListBox::outlineColourId,
            juce::Colours::transparentBlack);
        addAndMakeVisible(table_);

        detail_viewport_.setViewedComponent(&detail_host_, false);
        detail_viewport_.setScrollBarsShown(true, false);
        detail_viewport_.setScrollOnDragMode(
            juce::Viewport::ScrollOnDragMode::never);
        addAndMakeVisible(detail_viewport_);

        configureDetailEditable(detail_name_, false);
        detail_name_.setTooltip(
            juce::String::fromUTF8("音色名（空にはできません）"));
        detail_name_.onReturnKey = [this] { commitDetailName(); };
        detail_name_.onFocusLost = [this] { commitDetailName(); };
        detail_host_.addAndMakeVisible(detail_name_);
        detail_category_.setFont(UiFonts::body());
        detail_category_.setColour(
            juce::Label::textColourId, juce::Colour(0xFFB9C6D2));
        detail_host_.addAndMakeVisible(detail_category_);
        detail_favorite_.setFont(UiFonts::body());
        detail_favorite_.setInterceptsMouseClicks(true, false);
        detail_favorite_.setMouseCursor(
            juce::MouseCursor::PointingHandCursor);
        detail_favorite_.addMouseListener(this, false);
        detail_host_.addAndMakeVisible(detail_favorite_);
        detail_tags_empty_.setFont(UiFonts::body());
        detail_tags_empty_.setColour(
            juce::Label::textColourId, juce::Colour(0xFFB9C6D2));
        detail_tags_empty_.setText(
            juce::String::fromUTF8("（タグなし）"),
            juce::dontSendNotification);
        detail_host_.addAndMakeVisible(detail_tags_empty_);
        detail_host_.addAndMakeVisible(detail_tags_host_);
        configureDetailEditable(detail_memo_, true);
        detail_memo_.setTextToShowWhenEmpty(
            juce::String::fromUTF8("メモ"),
            juce::Colour(0xFF7F8993));
        detail_memo_.onFocusLost = [this] { commitDetailMemo(); };
        detail_memo_.onTextChange = [this] {
            if (!syncing_detail_) {
                layoutDetailHost();
            }
        };
        detail_host_.addAndMakeVisible(detail_memo_);

        duplicate_.setButtonText(juce::String::fromUTF8("複製"));
        duplicate_.onClick = [this] { duplicateSelected(); };
        addAndMakeVisible(duplicate_);
        remove_.setButtonText(
            juce::String::fromUTF8("選択を削除"));
        remove_.onClick = [this] { deleteSelected(); };
        addAndMakeVisible(remove_);
        assign_tags_.setButtonText(
            juce::String::fromUTF8("タグを付与"));
        assign_tags_.onClick = [this] { assignTagsToSelected(); };
        addAndMakeVisible(assign_tags_);
        load_edit_.setButtonText(
            juce::String::fromUTF8("読込して編集"));
        load_edit_.onClick = [this] { loadSelectedForEdit(); };
        addAndMakeVisible(load_edit_);
        close_.setButtonText(juce::String::fromUTF8("閉じる"));
        close_.onClick = [this] { closeParentDialog(); };
        addAndMakeVisible(close_);

        refreshTable(true);
    }

    ~LibraryManagerListTab() override {
        favorite_only_.setLookAndFeel(nullptr);
        recent_used_.setLookAndFeel(nullptr);
    }

    void reloadFromParent() {
        refreshTable(true);
    }

    [[nodiscard]] std::optional<LibraryManagerRow>
    performanceTarget() const {
        if (detail_row_) {
            return detail_row_;
        }
        const auto selected = selectedRows();
        if (!selected.empty()) {
            return selected.front();
        }
        return std::nullopt;
    }

    void paint(juce::Graphics& graphics) override {
        // 塗りだけ。枠線はリストの矩形塗りに角を隠されないよう
        // paintOverChildren で重ねる。
        if (!table_bounds_.isEmpty()) {
            fillRoundedPanelFrame(graphics, table_bounds_);
        }
        if (!detail_bounds_.isEmpty()) {
            fillRoundedPanelFrame(graphics, detail_bounds_);
        }
    }

    void paintOverChildren(juce::Graphics& graphics) override {
        if (!table_bounds_.isEmpty()) {
            strokeRoundedPanelFrame(graphics, table_bounds_);
        }
        if (!detail_bounds_.isEmpty()) {
            strokeRoundedPanelFrame(graphics, detail_bounds_);
        }
    }

    void resized() override {
        using namespace UiLayout;
        auto area = getLocalBounds().reduced(panelPad);
        auto controls = area.removeFromTop(fieldH);
        category_.setBounds(controls.removeFromLeft(96));
        controls.removeFromLeft(controlGap);
        recent_used_.setBounds(controls.removeFromRight(150));
        controls.removeFromRight(controlGap);
        favorite_only_.setBounds(controls.removeFromRight(100));
        controls.removeFromRight(controlGap);
        const int tag_w = juce::jlimit(
            libraryManageButtonW,
            220,
            juce::jmax(libraryManageButtonW, controls.getWidth() / 2));
        tag_filter_.setBounds(controls.removeFromRight(tag_w));
        controls.removeFromRight(controlGap);
        filter_.setBounds(controls);
        area.removeFromTop(sm);

        auto buttons = area.removeFromBottom(textButtonH);
        close_.setBounds(buttons.removeFromRight(libraryButtonMinW));
        buttons.removeFromRight(controlGap);
        load_edit_.setBounds(
            buttons.removeFromRight(libraryManageButtonW + xs));
        buttons.removeFromRight(controlGap);
        assign_tags_.setBounds(
            buttons.removeFromRight(libraryManageButtonW));
        buttons.removeFromRight(controlGap);
        remove_.setBounds(
            buttons.removeFromRight(libraryManageButtonW));
        buttons.removeFromRight(controlGap);
        duplicate_.setBounds(
            buttons.removeFromRight(libraryButtonMinW));
        area.removeFromBottom(sm);

        detail_bounds_ = area.removeFromRight(280);
        area.removeFromRight(panelGap);
        table_bounds_ = area;
        table_.setBounds(table_bounds_.reduced(1));

        detail_viewport_.setBounds(detail_bounds_.reduced(1));
        layoutDetailHost();
    }

    void mouseUp(const juce::MouseEvent& event) override {
        if (event.eventComponent == &detail_favorite_
            && detail_row_.has_value()) {
            const int index = indexOfDisplayedRow(*detail_row_);
            if (index >= 0) {
                toggleFavoriteAt(index, *detail_row_);
            }
        }
    }

private:
    static void configureDetailEditable(
        juce::TextEditor& editor,
        bool multiline) {
        editor.setMultiLine(multiline, true);
        editor.setReturnKeyStartsNewLine(multiline);
        editor.setReadOnly(false);
        editor.setCaretVisible(true);
        editor.setScrollbarsShown(false);
        editor.setPopupMenuEnabled(true);
        editor.setWantsKeyboardFocus(true);
        UiFonts::styleBodyField(editor);
    }

    void setDetailFieldsEditable(bool editable) {
        detail_name_.setReadOnly(!editable);
        detail_name_.setCaretVisible(editable);
        detail_name_.setEnabled(editable);
        detail_name_.setWantsKeyboardFocus(editable);
        detail_memo_.setReadOnly(!editable);
        detail_memo_.setCaretVisible(editable);
        detail_memo_.setEnabled(editable);
        detail_memo_.setWantsKeyboardFocus(editable);
    }

    void layoutDetailHost() {
        using namespace UiLayout;
        if (detail_bounds_.isEmpty()) {
            return;
        }
        const int host_w = juce::jmax(1, detail_viewport_.getWidth());
        const int inner_w = juce::jmax(1, host_w - panelPad * 2);
        int y = panelPad;

        detail_name_.setBounds(panelPad, y, inner_w, fieldH);
        y += fieldH + xs;
        detail_category_.setBounds(
            panelPad, y, inner_w, libraryTitleH - xs);
        y += libraryTitleH - xs + xs;
        detail_favorite_.setBounds(
            panelPad, y, inner_w, libraryTitleH - xs);
        y += libraryTitleH - xs + sm;

        const auto font = UiFonts::body();
        const int line_h = juce::jmax(
            18, juce::roundToInt(font.getHeight()) + 4);
        if (detail_tags_empty_.isVisible()) {
            detail_tags_empty_.setBounds(panelPad, y, inner_w, line_h);
            y += line_h + xs;
        } else {
            juce::Array<juce::Component*> views;
            for (auto& chip : detail_tag_chips_) {
                views.add(chip.get());
            }
            layoutTagChipsFlow(
                detail_tags_host_, views, inner_w, 28, controlGap);
            detail_tags_host_.setBounds(
                panelPad,
                y,
                inner_w,
                detail_tags_host_.getHeight());
            y += detail_tags_host_.getHeight() + xs;
        }

        juce::AttributedString memo_text;
        memo_text.append(
            detail_memo_.getText().isNotEmpty()
                ? detail_memo_.getText()
                : juce::String::fromUTF8("メモ"),
            font,
            juce::Colour(0xFFB9C6D2));
        juce::TextLayout memo_layout;
        memo_layout.createLayout(
            memo_text, static_cast<float>(inner_w));
        const int memo_h = juce::jmax(
            fieldH * 2,
            juce::roundToInt(std::ceil(memo_layout.getHeight())) + 8);
        detail_memo_.setBounds(panelPad, y, inner_w, memo_h);
        y += memo_h + panelPad;

        detail_host_.setSize(host_w, y);
    }

    void applyRecentUsedMode() {
        const bool recent = recent_used_.getToggleState();
        table_model_.setRecentUsedMode(recent);
        if (recent) {
            table_.getHeader().setSortColumnId(0, true);
        } else {
            table_.getHeader().setSortColumnId(
                table_model_.sortColumnId() == 0
                    ? LibraryManagerTableModel::kName
                    : table_model_.sortColumnId(),
                table_model_.sortForwards());
        }
        refreshTable(false);
    }

    [[nodiscard]] int indexOfDisplayedRow(
        const LibraryManagerRow& row) const {
        for (std::size_t i = 0; i < displayed_rows_.size(); ++i) {
            if (displayed_rows_[i].kind == row.kind
                && displayed_rows_[i].id == row.id) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    [[nodiscard]] LibraryManagerKind currentKind() const {
        switch (category_.getSelectedId()) {
        case 2:
            return LibraryManagerKind::Opll;
        case 3:
            return LibraryManagerKind::Composite;
        default:
            return LibraryManagerKind::Scc;
        }
    }

    [[nodiscard]] static int compareRows(
        const LibraryManagerRow& left,
        const LibraryManagerRow& right,
        bool recent_used,
        int sort_column_id,
        bool sort_forwards) {
        auto finish = [sort_forwards](int cmp) {
            return sort_forwards ? cmp : -cmp;
        };
        if (recent_used) {
            if (left.last_used_unix_seconds
                != right.last_used_unix_seconds) {
                return left.last_used_unix_seconds
                        > right.last_used_unix_seconds
                    ? -1
                    : 1;
            }
            return left.name < right.name
                ? -1
                : (left.name > right.name ? 1 : 0);
        }
        switch (sort_column_id) {
        case LibraryManagerTableModel::kTags: {
            const auto left_tags =
                mgstc::engine::serializeTimbreTags(left.tags);
            const auto right_tags =
                mgstc::engine::serializeTimbreTags(right.tags);
            if (left_tags != right_tags) {
                return finish(left_tags < right_tags ? -1 : 1);
            }
            break;
        }
        case LibraryManagerTableModel::kUpdated:
            if (left.updated_unix_seconds
                != right.updated_unix_seconds) {
                return finish(
                    left.updated_unix_seconds
                            < right.updated_unix_seconds
                        ? -1
                        : 1);
            }
            break;
        case LibraryManagerTableModel::kFavorite:
            if (left.favorite != right.favorite) {
                return finish(left.favorite ? -1 : 1);
            }
            break;
        case LibraryManagerTableModel::kName:
        default:
            if (left.name != right.name) {
                return finish(left.name < right.name ? -1 : 1);
            }
            break;
        }
        return 0;
    }

    [[nodiscard]] std::vector<std::vector<std::string>>
    currentCategoryTagSets() const {
        std::vector<std::vector<std::string>> tag_sets;
        const auto kind = currentKind();
        if (kind == LibraryManagerKind::Composite) {
            const auto* library = composites_();
            if (library == nullptr) {
                return tag_sets;
            }
            for (const auto& entry : library->entries()) {
                tag_sets.push_back(entry.timbre.tags);
            }
            return tag_sets;
        }
        const auto* library = timbres_();
        if (library == nullptr) {
            return tag_sets;
        }
        const auto category = kind == LibraryManagerKind::Scc
            ? mgstc::engine::TimbreCategory::Scc
            : mgstc::engine::TimbreCategory::Opll;
        for (const auto& entry : library->entries()) {
            if (entry.category == category) {
                tag_sets.push_back(entry.tags);
            }
        }
        return tag_sets;
    }

    void updateTagFilterButton() {
        tag_filter_.setButtonText(tagSelectionSummary(
            selected_filter_tags_,
            juce::String::fromUTF8("タグで絞り込み")));
    }

    void showTagFilter() {
        juce::Component::SafePointer<LibraryManagerListTab> safe(this);
        showTagSelectionDialog(
            this,
            juce::String::fromUTF8("ライブラリのタグ検索"),
            filterTagChoices(
                currentCategoryTagSets(), selected_filter_tags_),
            selected_filter_tags_,
            false,
            [safe](std::vector<std::string> selected) {
                if (safe == nullptr) {
                    return;
                }
                safe->selected_filter_tags_ = std::move(selected);
                safe->updateTagFilterButton();
                safe->refreshTable(true);
            });
    }

    [[nodiscard]] std::vector<LibraryManagerRow> buildRows() const {
        std::vector<LibraryManagerRow> rows;
        const auto kind = currentKind();
        const auto filter = filter_.getText().trim();
        const bool favorites_only = favorite_only_.getToggleState();
        const bool recent_used = recent_used_.getToggleState();
        const int sort_column_id = table_model_.sortColumnId();
        const bool sort_forwards = table_model_.sortForwards();
        if (kind == LibraryManagerKind::Composite) {
            const auto* library = composites_();
            if (library == nullptr) {
                return rows;
            }
            for (const auto& entry : library->entries()) {
                if (favorites_only && !entry.timbre.favorite) {
                    continue;
                }
                if (!mgstc::engine::containsAllTimbreTags(
                        entry.timbre.tags, selected_filter_tags_)) {
                    continue;
                }
                if (filter.isNotEmpty()) {
                    const auto name =
                        juce::String::fromUTF8(entry.timbre.name.c_str());
                    const auto tags =
                        juce::String::fromUTF8(
                            mgstc::engine::serializeTimbreTags(
                                entry.timbre.tags)
                                .c_str());
                    const auto memo =
                        juce::String::fromUTF8(entry.timbre.memo.c_str());
                    if (!name.containsIgnoreCase(filter)
                        && !tags.containsIgnoreCase(filter)
                        && !memo.containsIgnoreCase(filter)) {
                        continue;
                    }
                }
                rows.push_back(
                    {LibraryManagerKind::Composite,
                     entry.id,
                     entry.timbre.name,
                     entry.timbre.tags,
                     entry.timbre.memo,
                     entry.timbre.favorite,
                     entry.revision,
                     entry.updated_unix_seconds,
                     entry.last_used_unix_seconds});
            }
        } else {
            const auto* library = timbres_();
            if (library == nullptr) {
                return rows;
            }
            const auto category = kind == LibraryManagerKind::Scc
                ? mgstc::engine::TimbreCategory::Scc
                : mgstc::engine::TimbreCategory::Opll;
            for (const auto& entry : library->entries()) {
                if (entry.category != category
                    || (favorites_only && !entry.favorite)) {
                    continue;
                }
                if (!mgstc::engine::containsAllTimbreTags(
                        entry.tags, selected_filter_tags_)) {
                    continue;
                }
                if (filter.isNotEmpty()) {
                    const auto name =
                        juce::String::fromUTF8(entry.name.c_str());
                    const auto tags =
                        juce::String::fromUTF8(
                            mgstc::engine::serializeTimbreTags(entry.tags)
                                .c_str());
                    const auto memo =
                        juce::String::fromUTF8(entry.memo.c_str());
                    if (!name.containsIgnoreCase(filter)
                        && !tags.containsIgnoreCase(filter)
                        && !memo.containsIgnoreCase(filter)) {
                        continue;
                    }
                }
                rows.push_back(
                    {kind,
                     entry.id,
                     entry.name,
                     entry.tags,
                     entry.memo,
                     entry.favorite,
                     entry.revision,
                     entry.updated_unix_seconds,
                     entry.last_used_unix_seconds});
            }
        }
        std::stable_sort(
            rows.begin(),
            rows.end(),
            [recent_used, sort_column_id, sort_forwards](
                const LibraryManagerRow& left,
                const LibraryManagerRow& right) {
                return compareRows(
                           left,
                           right,
                           recent_used,
                           sort_column_id,
                           sort_forwards)
                    < 0;
            });
        return rows;
    }

    void refreshTable(bool reload_from_disk) {
        if (reload_from_disk) {
            if (!reload_ || !reload_()) {
                return;
            }
        }
        std::vector<std::pair<LibraryManagerKind, std::uint64_t>>
            previously_selected;
        for (const auto& row : selectedRows()) {
            previously_selected.push_back({row.kind, row.id});
        }
        displayed_rows_ = buildRows();
        table_model_.setRows(displayed_rows_);
        table_.updateContent();
        if (!previously_selected.empty()) {
            juce::SparseSet<int> selection;
            for (int index = 0;
                 index < static_cast<int>(displayed_rows_.size());
                 ++index) {
                const auto& row = displayed_rows_[static_cast<std::size_t>(
                    index)];
                for (const auto& key : previously_selected) {
                    if (row.kind == key.first && row.id == key.second) {
                        selection.addRange({index, index + 1});
                        break;
                    }
                }
            }
            table_.setSelectedRows(selection, juce::dontSendNotification);
        }
        updateDetail();
        table_.repaint();
        repaint();
    }

    [[nodiscard]] std::vector<LibraryManagerRow> selectedRows() const {
        std::vector<LibraryManagerRow> selected;
        const auto& indices = table_.getSelectedRows();
        for (int i = 0; i < indices.size(); ++i) {
            const auto index = indices[i];
            if (const auto* row = table_model_.rowAt(index)) {
                selected.push_back(*row);
            }
        }
        return selected;
    }

    void updateDetail() {
        const auto previous = detail_row_;
        const auto selected = selectedRows();
        syncing_detail_ = true;
        if (selected.size() != 1) {
            detail_row_.reset();
            setDetailFieldsEditable(false);
            detail_name_.setText(
                selected.empty()
                    ? juce::String::fromUTF8("音色を選択してください")
                    : juce::String::fromUTF8("複数選択中")
                      + " ("
                      + juce::String(static_cast<int>(selected.size()))
                      + ")",
                juce::dontSendNotification);
            detail_category_.setText({}, juce::dontSendNotification);
            detail_favorite_.setText({}, juce::dontSendNotification);
            rebuildDetailTagChips();
            detail_memo_.setText({}, juce::dontSendNotification);
            syncing_detail_ = false;
            layoutDetailHost();
            detail_viewport_.setViewPosition(0, 0);
            notifyPerformanceTargetChanged(previous);
            return;
        }
        detail_row_ = selected.front();
        const auto& row = *detail_row_;
        setDetailFieldsEditable(true);
        detail_name_.setText(
            juce::String::fromUTF8(row.name.c_str()),
            juce::dontSendNotification);
        juce::String category;
        switch (row.kind) {
        case LibraryManagerKind::Scc:
            category = juce::String::fromUTF8("SCC");
            break;
        case LibraryManagerKind::Opll:
            category = juce::String::fromUTF8("OPLL");
            break;
        case LibraryManagerKind::Composite:
            category = juce::String::fromUTF8("複合");
            break;
        }
        if (row.kind != LibraryManagerKind::Composite) {
            category += juce::String::fromUTF8("  r")
                + juce::String(static_cast<int>(row.revision));
        }
        detail_category_.setText(category, juce::dontSendNotification);
        detail_favorite_.setText(
            row.favorite
                ? juce::String::fromUTF8("★ お気に入り")
                : juce::String::fromUTF8("☆ お気に入りに追加"),
            juce::dontSendNotification);
        detail_favorite_.setColour(
            juce::Label::textColourId,
            row.favorite ? juce::Colour(0xFFFFD866)
                         : juce::Colour(0xFFB9C6D2));
        rebuildDetailTagChips();
        detail_memo_.setText(
            juce::String::fromUTF8(row.memo.c_str()),
            juce::dontSendNotification);
        syncing_detail_ = false;
        layoutDetailHost();
        detail_viewport_.setViewPosition(0, 0);
        notifyPerformanceTargetChanged(previous);
    }

    void rebuildDetailTagChips() {
        detail_tags_host_.removeAllChildren();
        detail_tag_chips_.clear();
        if (!detail_row_) {
            detail_tags_empty_.setVisible(false);
            detail_tags_host_.setVisible(false);
            return;
        }
        if (detail_row_->tags.empty()) {
            detail_tags_empty_.setText(
                juce::String::fromUTF8("（タグなし）"),
                juce::dontSendNotification);
            detail_tags_empty_.setVisible(true);
            detail_tags_host_.setVisible(false);
            return;
        }
        detail_tags_empty_.setVisible(false);
        detail_tags_host_.setVisible(true);
        for (const auto& tag : detail_row_->tags) {
            const auto tag_copy = tag;
            auto chip = std::make_unique<RemovableDetailTagChip>(
                juce::String::fromUTF8(tag.c_str()),
                [this, tag_copy] { confirmRemoveDetailTag(tag_copy); });
            detail_tags_host_.addAndMakeVisible(*chip);
            detail_tag_chips_.push_back(std::move(chip));
        }
    }

    void confirmRemoveDetailTag(std::string tag) {
        if (!detail_row_) {
            return;
        }
        juce::Component::SafePointer<LibraryManagerListTab> safe(this);
        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions()
                .withIconType(juce::MessageBoxIconType::WarningIcon)
                .withTitle(juce::String::fromUTF8("ライブラリ管理"))
                .withMessage(
                    juce::String::fromUTF8("「")
                    + juce::String::fromUTF8(tag.c_str())
                    + juce::String::fromUTF8(
                        "」をこの音色から外しますか？"))
                .withButton(juce::String::fromUTF8("外す"))
                .withButton(juce::String::fromUTF8("キャンセル"))
                .withAssociatedComponent(this),
            [safe, tag = std::move(tag)](int result) {
                if (safe == nullptr || result != 1) {
                    return;
                }
                safe->removeDetailTag(tag);
            });
    }

    void removeDetailTag(const std::string& tag) {
        if (!detail_row_) {
            return;
        }
        const auto kind = detail_row_->kind;
        const auto id = detail_row_->id;
        if (!reload_ || !reload_()) {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon,
                juce::String::fromUTF8("ライブラリ管理"),
                juce::String::fromUTF8(
                    "共有ライブラリを読み直せませんでした"));
            return;
        }
        const auto now = currentUnixTime();
        bool ok = false;
        if (kind == LibraryManagerKind::Composite) {
            auto* library = composites_();
            auto* entry = library ? library->find(id) : nullptr;
            if (entry != nullptr) {
                auto& tags = entry->timbre.tags;
                tags.erase(
                    std::remove(tags.begin(), tags.end(), tag),
                    tags.end());
                ok = library->update(id, entry->timbre, now)
                    && persist_composites_();
            }
        } else {
            auto* library = timbres_();
            auto* entry = library ? library->find(id) : nullptr;
            if (entry != nullptr) {
                auto updated = *entry;
                updated.tags.erase(
                    std::remove(
                        updated.tags.begin(),
                        updated.tags.end(),
                        tag),
                    updated.tags.end());
                ok = library->update(id, updated, now)
                    && persist_timbres_();
            }
        }
        if (!ok) {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon,
                juce::String::fromUTF8("ライブラリ管理"),
                juce::String::fromUTF8(
                    "タグを外した結果を保存できませんでした"));
            return;
        }
        refreshTable(false);
        if (libraries_changed_) {
            libraries_changed_();
        }
    }

    void restoreDetailNameField() {
        if (!detail_row_) {
            return;
        }
        syncing_detail_ = true;
        detail_name_.setText(
            juce::String::fromUTF8(detail_row_->name.c_str()),
            juce::dontSendNotification);
        syncing_detail_ = false;
    }

    void restoreDetailMemoField() {
        if (!detail_row_) {
            return;
        }
        syncing_detail_ = true;
        detail_memo_.setText(
            juce::String::fromUTF8(detail_row_->memo.c_str()),
            juce::dontSendNotification);
        syncing_detail_ = false;
        layoutDetailHost();
    }

    void commitDetailName() {
        if (syncing_detail_ || !detail_row_) {
            return;
        }
        const auto requested = utf8String(detail_name_.getText().trim());
        if (requested.empty()) {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon,
                juce::String::fromUTF8("ライブラリ管理"),
                juce::String::fromUTF8("新しい名前を入力してください"));
            restoreDetailNameField();
            return;
        }
        if (requested == detail_row_->name) {
            return;
        }
        const auto kind = detail_row_->kind;
        const auto id = detail_row_->id;
        if (!reload_ || !reload_()) {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon,
                juce::String::fromUTF8("ライブラリ管理"),
                juce::String::fromUTF8(
                    "共有ライブラリを読み直せませんでした"));
            restoreDetailNameField();
            return;
        }
        const auto now = currentUnixTime();
        bool ok = false;
        if (kind == LibraryManagerKind::Composite) {
            auto* library = composites_();
            auto* entry = library ? library->find(id) : nullptr;
            if (entry != nullptr) {
                entry->timbre.name = requested;
                ok = library->update(id, entry->timbre, now)
                    && persist_composites_();
            }
        } else {
            auto* library = timbres_();
            auto* entry = library ? library->find(id) : nullptr;
            if (entry != nullptr) {
                auto updated = *entry;
                updated.name = requested;
                ok = library->update(id, updated, now)
                    && persist_timbres_();
            }
        }
        if (!ok) {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon,
                juce::String::fromUTF8("ライブラリ管理"),
                juce::String::fromUTF8(
                    "名前の変更を保存できませんでした"));
            restoreDetailNameField();
            return;
        }
        refreshTable(false);
    }

    void commitDetailMemo() {
        if (syncing_detail_ || !detail_row_) {
            return;
        }
        const auto requested = utf8String(detail_memo_.getText());
        if (requested == detail_row_->memo) {
            return;
        }
        const auto kind = detail_row_->kind;
        const auto id = detail_row_->id;
        if (!reload_ || !reload_()) {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon,
                juce::String::fromUTF8("ライブラリ管理"),
                juce::String::fromUTF8(
                    "共有ライブラリを読み直せませんでした"));
            restoreDetailMemoField();
            return;
        }
        const auto now = currentUnixTime();
        bool ok = false;
        if (kind == LibraryManagerKind::Composite) {
            auto* library = composites_();
            auto* entry = library ? library->find(id) : nullptr;
            if (entry != nullptr) {
                entry->timbre.memo = requested;
                ok = library->update(id, entry->timbre, now)
                    && persist_composites_();
            }
        } else {
            auto* library = timbres_();
            auto* entry = library ? library->find(id) : nullptr;
            if (entry != nullptr) {
                auto updated = *entry;
                updated.memo = requested;
                ok = library->update(id, updated, now)
                    && persist_timbres_();
            }
        }
        if (!ok) {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon,
                juce::String::fromUTF8("ライブラリ管理"),
                juce::String::fromUTF8(
                    "メモの変更を保存できませんでした"));
            restoreDetailMemoField();
            return;
        }
        refreshTable(false);
    }

    void notifyPerformanceTargetChanged(
        const std::optional<LibraryManagerRow>& previous) {
        const auto current = performanceTarget();
        const bool changed =
            static_cast<bool>(previous) != static_cast<bool>(current)
            || (previous && current
                && (previous->kind != current->kind
                    || previous->id != current->id));
        if (changed && performance_target_changed_) {
            performance_target_changed_();
        }
    }

    void toggleFavoriteAt(
        int row_number,
        const LibraryManagerRow& row) {
        auto* mutable_row = table_model_.rowAtMutable(row_number);
        if (mutable_row == nullptr
            || mutable_row->id != row.id
            || mutable_row->kind != row.kind) {
            return;
        }
        const auto now = currentUnixTime();
        const bool next_favorite = !mutable_row->favorite;
        if (row.kind == LibraryManagerKind::Composite) {
            auto* library = composites_();
            if (library == nullptr) {
                return;
            }
            auto* entry = library->find(row.id);
            if (entry == nullptr) {
                refreshTable(true);
                return;
            }
            entry->timbre.favorite = next_favorite;
            if (!library->update(row.id, entry->timbre, now)
                || !persist_composites_()) {
                refreshTable(true);
                return;
            }
        } else {
            auto* library = timbres_();
            if (library == nullptr) {
                return;
            }
            auto* entry = library->find(row.id);
            if (entry == nullptr) {
                refreshTable(true);
                return;
            }
            auto updated = *entry;
            updated.favorite = next_favorite;
            if (!library->update(row.id, updated, now)
                || !persist_timbres_()) {
                refreshTable(true);
                return;
            }
        }
        mutable_row->favorite = next_favorite;
        if (static_cast<std::size_t>(row_number)
            < displayed_rows_.size()) {
            displayed_rows_[static_cast<std::size_t>(row_number)]
                .favorite = next_favorite;
        }
        if (detail_row_
            && detail_row_->id == row.id
            && detail_row_->kind == row.kind) {
            detail_row_->favorite = next_favorite;
            detail_favorite_.setText(
                next_favorite
                    ? juce::String::fromUTF8("★ お気に入り")
                    : juce::String::fromUTF8("☆ お気に入り解除"),
                juce::dontSendNotification);
            detail_favorite_.setColour(
                juce::Label::textColourId,
                next_favorite ? juce::Colour(0xFFFFD866)
                              : juce::Colour(0xFF9AA8B5));
        }
        table_.repaintRow(row_number);
    }

    void duplicateSelected() {
        const auto selected = selectedRows();
        if (selected.empty()) {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::InfoIcon,
                juce::String::fromUTF8("ライブラリ管理"),
                juce::String::fromUTF8(
                    "複製する音色を選択してください"));
            return;
        }
        if (!reload_ || !reload_()) {
            return;
        }
        const auto now = currentUnixTime();
        for (const auto& row : selected) {
            if (row.kind == LibraryManagerKind::Composite) {
                auto* library = composites_();
                const auto* source = library ? library->find(row.id) : nullptr;
                if (source == nullptr) {
                    continue;
                }
                auto copy = source->timbre;
                copy.name = library->uniqueName(copy.name);
                library->add(std::move(copy), now);
            } else {
                auto* library = timbres_();
                const auto* source = library ? library->find(row.id) : nullptr;
                if (source == nullptr) {
                    continue;
                }
                auto copy = *source;
                copy.name = library->uniqueName(
                    row.kind == LibraryManagerKind::Scc
                        ? mgstc::engine::TimbreCategory::Scc
                        : mgstc::engine::TimbreCategory::Opll,
                    copy.name);
                library->add(std::move(copy), now);
            }
        }
        if (!persist_timbres_() || !persist_composites_()) {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon,
                juce::String::fromUTF8("ライブラリ管理"),
                juce::String::fromUTF8("複製結果を保存できませんでした"));
        }
        refreshTable(true);
        if (libraries_changed_) {
            libraries_changed_();
        }
    }

    void deleteSelected() {
        const auto selected = selectedRows();
        if (selected.empty()) {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::InfoIcon,
                juce::String::fromUTF8("ライブラリ管理"),
                juce::String::fromUTF8(
                    "削除する音色を選択してください"));
            return;
        }
        juce::String message =
            juce::String::fromUTF8("選択した ")
            + juce::String(static_cast<int>(selected.size()))
            + juce::String::fromUTF8(" 件をライブラリから削除しますか？");
        for (const auto& row : selected) {
            if (row.kind != LibraryManagerKind::Composite) {
                const auto impact = inspectCompositeTimbreImpact(row.id);
                if (!impact.readable || !impact.uses.empty()) {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::MessageBoxIconType::WarningIcon,
                        juce::String::fromUTF8("ライブラリ管理"),
                        juce::String::fromUTF8("「")
                            + juce::String::fromUTF8(row.name.c_str())
                            + juce::String::fromUTF8(
                                  "」は総合音色から参照中のため削除できません。\n\n")
                            + describeCompositeTimbreImpact(impact));
                    return;
                }
            }
        }
        juce::Component::SafePointer<LibraryManagerListTab> safe(this);
        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions()
                .withIconType(
                    juce::MessageBoxIconType::WarningIcon)
                .withTitle(juce::String::fromUTF8("ライブラリ管理"))
                .withMessage(message)
                .withButton(juce::String::fromUTF8("削除"))
                .withButton(juce::String::fromUTF8("キャンセル"))
                .withAssociatedComponent(this),
            [safe, selected](int result) {
                if (safe == nullptr || result != 1) {
                    return;
                }
                if (!safe->reload_ || !safe->reload_()) {
                    return;
                }
                for (const auto& row : selected) {
                    if (row.kind == LibraryManagerKind::Composite) {
                        if (auto* library = safe->composites_()) {
                            library->erase(row.id);
                        }
                    } else if (auto* library = safe->timbres_()) {
                        library->erase(row.id);
                    }
                }
                if (!safe->persist_timbres_()
                    || !safe->persist_composites_()) {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::MessageBoxIconType::WarningIcon,
                        juce::String::fromUTF8("ライブラリ管理"),
                        juce::String::fromUTF8(
                            "削除結果を保存できませんでした"));
                }
                safe->refreshTable(true);
                if (safe->libraries_changed_) {
                    safe->libraries_changed_();
                }
            });
    }

    void assignTagsToSelected() {
        const auto selected = selectedRows();
        if (selected.empty()) {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::InfoIcon,
                juce::String::fromUTF8("ライブラリ管理"),
                juce::String::fromUTF8(
                    "タグを付与する音色を選択してください"));
            return;
        }
        std::vector<std::vector<std::string>> tag_sets;
        if (auto* timbres = timbres_()) {
            for (const auto& entry : timbres->entries()) {
                tag_sets.push_back(entry.tags);
            }
        }
        if (auto* composites = composites_()) {
            for (const auto& entry : composites->entries()) {
                tag_sets.push_back(entry.timbre.tags);
            }
        }
        juce::Component::SafePointer<LibraryManagerListTab> safe(this);
        showTagSelectionDialog(
            this,
            juce::String::fromUTF8("タグを付与"),
            editorTagChoices(tag_sets, {}),
            {},
            true,
            [safe, selected](std::vector<std::string> additions) {
                if (safe == nullptr || additions.empty()) {
                    return;
                }
                if (!safe->reload_ || !safe->reload_()) {
                    return;
                }
                const auto now = currentUnixTime();
                for (const auto& row : selected) {
                    if (row.kind == LibraryManagerKind::Composite) {
                        auto* library = safe->composites_();
                        auto* entry =
                            library ? library->find(row.id) : nullptr;
                        if (entry == nullptr) {
                            continue;
                        }
                        auto tags = entry->timbre.tags;
                        tags.insert(
                            tags.end(),
                            additions.begin(),
                            additions.end());
                        entry->timbre.tags =
                            mgstc::engine::parseTimbreTags(
                                mgstc::engine::serializeTimbreTags(
                                    tags));
                        library->update(row.id, entry->timbre, now);
                    } else {
                        auto* library = safe->timbres_();
                        auto* entry =
                            library ? library->find(row.id) : nullptr;
                        if (entry == nullptr) {
                            continue;
                        }
                        auto updated = *entry;
                        updated.tags.insert(
                            updated.tags.end(),
                            additions.begin(),
                            additions.end());
                        updated.tags =
                            mgstc::engine::parseTimbreTags(
                                mgstc::engine::serializeTimbreTags(
                                    updated.tags));
                        library->update(row.id, updated, now);
                    }
                }
                if (!safe->persist_timbres_()
                    || !safe->persist_composites_()) {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::MessageBoxIconType::WarningIcon,
                        juce::String::fromUTF8("ライブラリ管理"),
                        juce::String::fromUTF8(
                            "タグ付与結果を保存できませんでした"));
                }
                safe->refreshTable(true);
                if (safe->libraries_changed_) {
                    safe->libraries_changed_();
                }
            });
    }

    void loadSelectedForEdit() {
        const auto selected = selectedRows();
        if (selected.size() != 1) {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::InfoIcon,
                juce::String::fromUTF8("ライブラリ管理"),
                juce::String::fromUTF8(
                    "編集する音色を1件だけ選択してください"));
            return;
        }
        if (open_editor_) {
            open_editor_(selected.front().kind, selected.front().id);
        }
        closeParentDialog();
    }

    void closeParentDialog() {
        if (auto* dialog =
                findParentComponentOfClass<juce::DialogWindow>()) {
            dialog->exitModalState(0);
        }
    }

    PreviewCallback preview_;
    OpenEditorCallback open_editor_;
    ReloadCallback reload_;
    PersistCallback persist_timbres_;
    PersistCallback persist_composites_;
    std::function<mgstc::engine::TimbreLibrary*()> timbres_;
    std::function<mgstc::engine::CompositeTimbreLibrary*()>
        composites_;
    PerformanceTargetChangedCallback performance_target_changed_;
    LibrariesChangedCallback libraries_changed_;
    SwitchLookAndFeel switch_look_and_feel_;
    juce::ComboBox category_;
    juce::TextEditor filter_;
    juce::TextButton tag_filter_;
    juce::ToggleButton favorite_only_;
    juce::ToggleButton recent_used_;
    LibraryManagerTableModel table_model_;
    juce::TableListBox table_;
    juce::Rectangle<int> table_bounds_;
    juce::Rectangle<int> detail_bounds_;
    std::vector<LibraryManagerRow> displayed_rows_;
    std::vector<std::string> selected_filter_tags_;
    std::optional<LibraryManagerRow> detail_row_;
    bool syncing_detail_{false};
    juce::Viewport detail_viewport_;
    juce::Component detail_host_;
    juce::TextEditor detail_name_;
    juce::Label detail_category_;
    juce::Label detail_favorite_;
    juce::Label detail_tags_empty_;
    juce::Component detail_tags_host_;
    std::vector<std::unique_ptr<RemovableDetailTagChip>>
        detail_tag_chips_;
    juce::TextEditor detail_memo_;
    juce::TextButton duplicate_;
    juce::TextButton remove_;
    juce::TextButton assign_tags_;
    juce::TextButton load_edit_;
    juce::TextButton close_;
};

// Tab traverses siblings by explicit order, then Y/X. Without this, the right
// library column interleaves with the left editor by vertical position.
void assignExplicitFocusOrders(
    std::initializer_list<juce::Component*> components,
    int start_order) {
    int order = start_order;
    for (auto* component : components) {
        if (component != nullptr) {
            component->setExplicitFocusOrder(order++);
        }
    }
}

class MgstcLookAndFeel final : public juce::LookAndFeel_V4 {
public:
    MgstcLookAndFeel() {
        // Window chrome peeking at edges: solid page top colour (content paints gradient).
        const auto background = juce::Colour(UiLayout::pageFill);
        const auto panel = juce::Colour(0xFF2A3540);
        const auto border = juce::Colour(0xFF607080);
        const auto text = juce::Colour(0xFFF2F4F5);
        const auto blue = juce::Colour(0xFF0072B2);
        const auto cyan = juce::Colour(0xFF56B4E9);
        // Focus rings / hover accent / progress / SCC confirmed waveform.
        const auto accent_green = juce::Colour(kUiHoverAccent);
        // Toggle-on button fill: darker so #F2F4F5 text stays readable.
        const auto selected_green = juce::Colour(0xFF1F8A5C);
        setDefaultSansSerifTypefaceName(UiFonts::windowsMessageFaceName());
        setColour(juce::ResizableWindow::backgroundColourId, background);
        setColour(juce::TextEditor::backgroundColourId, panel);
        setColour(juce::TextEditor::outlineColourId, border);
        setColour(juce::TextEditor::focusedOutlineColourId, accent_green);
        setColour(juce::TextEditor::textColourId, text);
        setColour(juce::Label::textColourId, text);
        setColour(juce::ComboBox::backgroundColourId, panel);
        setColour(juce::ComboBox::outlineColourId, border);
        setColour(juce::ComboBox::focusedOutlineColourId, accent_green);
        setColour(juce::ComboBox::textColourId, text);
        setColour(juce::Slider::backgroundColourId,
                  juce::Colour(0xFF465562));
        setColour(juce::Slider::trackColourId, cyan);
        setColour(juce::Slider::thumbColourId, text);
        setColour(juce::Slider::rotarySliderFillColourId, blue);
        setColour(juce::Slider::rotarySliderOutlineColourId, border);
        setColour(juce::TextButton::buttonColourId, panel);
        setColour(juce::TextButton::buttonOnColourId, selected_green);
        setColour(juce::TextButton::textColourOffId, text);
        setColour(juce::TextButton::textColourOnId, text);
        // ImageOnButtonBackground uses TextButton::buttonOnColourId when
        // toggled; set backgroundOn too for other DrawableButton styles.
        setColour(juce::DrawableButton::backgroundColourId, panel);
        setColour(
            juce::DrawableButton::backgroundOnColourId, selected_green);
        setColour(juce::TabbedButtonBar::tabTextColourId, text);
        setColour(juce::TabbedButtonBar::frontTextColourId, text);
    }

    void drawButtonBackground(
        juce::Graphics& graphics,
        juce::Button& button,
        const juce::Colour& backgroundColour,
        bool should_draw_highlighted,
        bool should_draw_down) override {
        constexpr float corner = 6.0F;
        const auto bounds =
            button.getLocalBounds().toFloat().reduced(0.5F, 0.5F);
        const bool on = button.getToggleState();
        // V4 desaturates fills by 0.9x; keep toggle-on green as authored.
        auto base = on
            ? findColour(juce::TextButton::buttonOnColourId)
            : backgroundColour.withMultipliedSaturation(
                  button.hasKeyboardFocus(true) ? 1.3F : 0.9F);
        base = base.withMultipliedAlpha(button.isEnabled() ? 1.0F : 0.5F);
        if (should_draw_down || should_draw_highlighted) {
            base = base.contrasting(should_draw_down ? 0.2F : 0.05F);
        }
        graphics.setColour(base);
        graphics.fillRoundedRectangle(bounds, corner);

        const bool focus = button.hasKeyboardFocus(true);
        graphics.setColour(
            focus ? juce::Colour(kUiHoverAccent)
                  : button.findColour(juce::ComboBox::outlineColourId));
        graphics.drawRoundedRectangle(
            bounds, corner, focus ? 1.5F : 1.0F);
    }

    void drawButtonText(
        juce::Graphics& graphics,
        juce::TextButton& button,
        bool should_draw_highlighted,
        bool /*should_draw_down*/) override {
        const auto font = getTextButtonFont(button, button.getHeight());
        graphics.setFont(font);

        // Toggle-on uses dark green fill — keep light text for contrast.
        const bool toggle_on = button.getToggleState();
        const auto base_colour = toggle_on
            ? button.findColour(juce::TextButton::textColourOnId)
            : ((should_draw_highlighted && button.isEnabled())
                   ? juce::Colour(kUiHoverAccent)
                   : button.findColour(juce::TextButton::textColourOffId));
        graphics.setColour(
            base_colour.withMultipliedAlpha(
                button.isEnabled() ? 1.0F : 0.5F));

        const int y_indent = juce::jmin(
            4, button.proportionOfHeight(0.3F));
        const int corner_size =
            juce::jmin(button.getHeight(), button.getWidth()) / 2;
        const int font_height =
            juce::roundToInt(font.getHeight() * 0.6F);
        const int left_indent = juce::jmin(
            font_height,
            2 + corner_size / (button.isConnectedOnLeft() ? 4 : 2));
        const int right_indent = juce::jmin(
            font_height,
            2 + corner_size / (button.isConnectedOnRight() ? 4 : 2));
        const int text_width =
            button.getWidth() - left_indent - right_indent;
        if (text_width > 0) {
            graphics.drawFittedText(
                button.getButtonText(),
                left_indent,
                y_indent,
                text_width,
                button.getHeight() - y_indent * 2,
                juce::Justification::centred,
                2);
        }
    }

    juce::Font getTextButtonFont(
        juce::TextButton&,
        int buttonHeight) override {
        return UiFonts::body().withHeight(
            UiFonts::controlTextHeight(
                static_cast<float>(buttonHeight)));
    }

    juce::Font getLabelFont(juce::Label& label) override {
        // JUCE Label default height is 15px; treat that as unset → body.
        if (std::abs(label.getFont().getHeight() - 15.0F) < 0.05F) {
            return UiFonts::body();
        }
        return label.getFont();
    }

    juce::Font getComboBoxFont(juce::ComboBox& box) override {
        return UiFonts::body().withHeight(
            UiFonts::controlTextHeight(
                static_cast<float>(box.getHeight())));
    }

    // LookAndFeel_V4::drawComboBox ignores focusedOutlineColourId; draw the
    // same bright-green focus ring as TextEditor / focused buttons.
    void drawComboBox(
        juce::Graphics& graphics,
        int width,
        int height,
        bool,
        int,
        int,
        int,
        int,
        juce::ComboBox& box) override {
        constexpr float corner = 3.0F;
        const auto bounds =
            juce::Rectangle<int>(0, 0, width, height)
                .toFloat()
                .reduced(0.5F, 0.5F);

        graphics.setColour(
            box.findColour(juce::ComboBox::backgroundColourId));
        graphics.fillRoundedRectangle(bounds, corner);

        const bool focus = box.hasKeyboardFocus(true);
        graphics.setColour(
            focus ? box.findColour(
                        juce::ComboBox::focusedOutlineColourId)
                  : box.findColour(juce::ComboBox::outlineColourId));
        graphics.drawRoundedRectangle(
            bounds, corner, focus ? 1.5F : 1.0F);

        juce::Rectangle<int> arrow_zone(width - 30, 0, 20, height);
        juce::Path path;
        path.startNewSubPath(
            static_cast<float>(arrow_zone.getX()) + 3.0F,
            static_cast<float>(arrow_zone.getCentreY()) - 2.0F);
        path.lineTo(
            static_cast<float>(arrow_zone.getCentreX()),
            static_cast<float>(arrow_zone.getCentreY()) + 3.0F);
        path.lineTo(
            static_cast<float>(arrow_zone.getRight()) - 3.0F,
            static_cast<float>(arrow_zone.getCentreY()) - 2.0F);
        const bool arrow_hover =
            box.isEnabled() && box.isMouseOverOrDragging(true);
        graphics.setColour(
            arrow_hover
                ? juce::Colour(kUiHoverAccent).withAlpha(0.9F)
                : box.findColour(juce::ComboBox::arrowColourId)
                      .withAlpha(box.isEnabled() ? 0.9F : 0.2F));
        graphics.strokePath(path, juce::PathStrokeType(2.0F));
    }

    juce::Font getPopupMenuFont() override {
        return UiFonts::body();
    }

    // LookAndFeel_V4/V3 draws tabs via drawTabButton (not drawTabButtonText),
    // and hardcodes depth*0.5 font. Keep getTabButtonFont + drawTabButtonText
    // for the V2 path; override drawTabButton so settings tabs use body + hover.
    juce::Font getTabButtonFont(
        juce::TabBarButton&,
        float /*height*/) override {
        return UiFonts::body();
    }

    void drawTabButtonText(
        juce::TabBarButton& button,
        juce::Graphics& graphics,
        bool is_mouse_over,
        bool is_mouse_down) override {
        auto area = button.getTextArea().toFloat();
        auto length = area.getWidth();
        auto depth = area.getHeight();
        if (button.getTabbedButtonBar().isVertical()) {
            std::swap(length, depth);
        }

        auto font = getTabButtonFont(button, depth);
        font.setUnderline(button.hasKeyboardFocus(false));

        juce::AffineTransform transform;
        switch (button.getTabbedButtonBar().getOrientation()) {
            case juce::TabbedButtonBar::TabsAtLeft:
                transform = transform
                    .rotated(juce::MathConstants<float>::pi * -0.5F)
                    .translated(area.getX(), area.getBottom());
                break;
            case juce::TabbedButtonBar::TabsAtRight:
                transform = transform
                    .rotated(juce::MathConstants<float>::pi * 0.5F)
                    .translated(area.getRight(), area.getY());
                break;
            case juce::TabbedButtonBar::TabsAtTop:
            case juce::TabbedButtonBar::TabsAtBottom:
                transform = transform.translated(area.getX(), area.getY());
                break;
            default:
                jassertfalse;
                break;
        }

        const float alpha = button.isEnabled()
            ? ((is_mouse_over || is_mouse_down) ? 1.0F : 0.8F)
            : 0.3F;
        juce::Colour colour;
        if (button.isEnabled() && (is_mouse_over || is_mouse_down)) {
            colour = juce::Colour(kUiHoverAccent);
        } else if (
            button.isFrontTab()
            && (button.isColourSpecified(
                    juce::TabbedButtonBar::frontTextColourId)
                || isColourSpecified(
                    juce::TabbedButtonBar::frontTextColourId))) {
            colour = findColour(juce::TabbedButtonBar::frontTextColourId);
        } else if (
            button.isColourSpecified(juce::TabbedButtonBar::tabTextColourId)
            || isColourSpecified(juce::TabbedButtonBar::tabTextColourId)) {
            colour = findColour(juce::TabbedButtonBar::tabTextColourId);
        } else {
            colour = button.getTabBackgroundColour().contrasting();
        }

        graphics.setColour(colour.withMultipliedAlpha(alpha));
        graphics.setFont(font);
        graphics.addTransform(transform);
        graphics.drawFittedText(
            button.getButtonText().trim(),
            0,
            0,
            static_cast<int>(length),
            static_cast<int>(depth),
            juce::Justification::centred,
            juce::jmax(1, static_cast<int>(depth) / 12));
    }

    void drawTabButton(
        juce::TabBarButton& button,
        juce::Graphics& graphics,
        bool is_mouse_over,
        bool is_mouse_down) override {
        const auto active_area = button.getActiveArea();
        const auto orientation =
            button.getTabbedButtonBar().getOrientation();
        const auto background = button.getTabBackgroundColour();

        if (button.getToggleState()) {
            graphics.setColour(background);
        } else {
            juce::Point<int> p1;
            juce::Point<int> p2;
            switch (orientation) {
                case juce::TabbedButtonBar::TabsAtBottom:
                    p1 = active_area.getBottomLeft();
                    p2 = active_area.getTopLeft();
                    break;
                case juce::TabbedButtonBar::TabsAtTop:
                    p1 = active_area.getTopLeft();
                    p2 = active_area.getBottomLeft();
                    break;
                case juce::TabbedButtonBar::TabsAtRight:
                    p1 = active_area.getTopRight();
                    p2 = active_area.getTopLeft();
                    break;
                case juce::TabbedButtonBar::TabsAtLeft:
                    p1 = active_area.getTopLeft();
                    p2 = active_area.getTopRight();
                    break;
                default:
                    jassertfalse;
                    break;
            }
            graphics.setGradientFill(juce::ColourGradient(
                background.brighter(0.2F),
                p1.toFloat(),
                background.darker(0.1F),
                p2.toFloat(),
                false));
        }
        graphics.fillRect(active_area);

        graphics.setColour(
            button.findColour(juce::TabbedButtonBar::tabOutlineColourId));
        auto outline = active_area;
        if (orientation != juce::TabbedButtonBar::TabsAtBottom) {
            graphics.fillRect(outline.removeFromTop(1));
        }
        if (orientation != juce::TabbedButtonBar::TabsAtTop) {
            graphics.fillRect(outline.removeFromBottom(1));
        }
        if (orientation != juce::TabbedButtonBar::TabsAtRight) {
            graphics.fillRect(outline.removeFromLeft(1));
        }
        if (orientation != juce::TabbedButtonBar::TabsAtLeft) {
            graphics.fillRect(outline.removeFromRight(1));
        }

        drawTabButtonText(
            button, graphics, is_mouse_over, is_mouse_down);
    }

    juce::Rectangle<int> getTooltipBounds(
        const juce::String& tipText,
        juce::Point<int> screenPos,
        juce::Rectangle<int> parentArea) override {
        juce::AttributedString attributed;
        attributed.setWordWrap(
            juce::AttributedString::WordWrap::byWord);
        attributed.setJustification(juce::Justification::centred);
        attributed.append(
            tipText, UiFonts::body(), juce::Colours::black);
        juce::TextLayout layout;
        layout.createLayoutWithBalancedLineLengths(
            attributed, 400.0F);
        const auto width =
            static_cast<int>(layout.getWidth() + 14.0F);
        const auto height =
            static_cast<int>(layout.getHeight() + 8.0F);
        return juce::Rectangle<int>(
                   screenPos.x > parentArea.getCentreX()
                       ? screenPos.x - (width + 12)
                       : screenPos.x + 24,
                   screenPos.y > parentArea.getCentreY()
                       ? screenPos.y - (height + 6)
                       : screenPos.y + 6,
                   width,
                   height)
            .constrainedWithin(parentArea);
    }

    void drawTooltip(
        juce::Graphics& graphics,
        const juce::String& text,
        int width,
        int height) override {
        auto bounds = juce::Rectangle<int>(width, height).toFloat();
        constexpr float corner = 5.0F;
        graphics.setColour(
            findColour(juce::TooltipWindow::backgroundColourId));
        graphics.fillRoundedRectangle(bounds, corner);
        graphics.setColour(
            findColour(juce::TooltipWindow::outlineColourId));
        graphics.drawRoundedRectangle(
            bounds.reduced(0.5F), corner, 1.0F);

        juce::AttributedString attributed;
        attributed.setWordWrap(
            juce::AttributedString::WordWrap::byWord);
        attributed.setJustification(juce::Justification::centred);
        attributed.append(
            text,
            UiFonts::body(),
            findColour(juce::TooltipWindow::textColourId));
        juce::TextLayout layout;
        layout.createLayoutWithBalancedLineLengths(
            attributed, static_cast<float>(width) - 8.0F);
        layout.draw(
            graphics,
            juce::Rectangle<float>(
                static_cast<float>(width),
                static_cast<float>(height)));
    }

    juce::Font getAlertWindowTitleFont() override {
        return UiFonts::heading();
    }

    juce::Font getAlertWindowMessageFont() override {
        return UiFonts::body();
    }

    juce::Font getAlertWindowFont() override {
        return UiFonts::body();
    }

    juce::Label* createSliderTextBox(juce::Slider& slider) override {
        auto* label = juce::LookAndFeel_V4::createSliderTextBox(slider);
        // Param numerics = body (dense reserved for SCC cells / graph chrome).
        label->setFont(UiFonts::body());
        return label;
    }

    // Default checkbox toggles (tick + label). SwitchLookAndFeel owns switch-styled ones.
    void drawToggleButton(
        juce::Graphics& graphics,
        juce::ToggleButton& button,
        bool should_draw_highlight,
        bool should_draw_down) override {
        const auto font = UiFonts::body();
        const auto tick_width = font.getHeight() * 1.1F;

        drawTickBox(
            graphics,
            button,
            4.0F,
            (static_cast<float>(button.getHeight()) - tick_width) * 0.5F,
            tick_width,
            tick_width,
            button.getToggleState(),
            button.isEnabled(),
            should_draw_highlight,
            should_draw_down);

        const auto label_colour =
            (should_draw_highlight && button.isEnabled())
                ? juce::Colour(kUiHoverAccent)
                : button.findColour(juce::ToggleButton::textColourId);
        graphics.setColour(label_colour);
        graphics.setFont(font);

        if (!button.isEnabled()) {
            graphics.setOpacity(0.5F);
        }

        graphics.drawFittedText(
            button.getButtonText(),
            button.getLocalBounds()
                .withTrimmedLeft(juce::roundToInt(tick_width) + 10)
                .withTrimmedRight(2),
            juce::Justification::centredLeft,
            10);
    }

    void drawLinearSlider(
        juce::Graphics& graphics,
        int x,
        int y,
        int width,
        int height,
        float slider_position,
        float minimum_position,
        float maximum_position,
        juce::Slider::SliderStyle style,
        juce::Slider& slider) override {
        const auto draw = [&] {
            juce::LookAndFeel_V4::drawLinearSlider(
                graphics,
                x,
                y,
                width,
                height,
                slider_position,
                minimum_position,
                maximum_position,
                style,
                slider);
        };
        if (slider.isEnabled()) {
            draw();
            return;
        }
        graphics.beginTransparencyLayer(kDisabledOpacity);
        draw();
        graphics.endTransparencyLayer();
    }

    void drawRotarySlider(
        juce::Graphics& graphics,
        int x,
        int y,
        int width,
        int height,
        float slider_position,
        float start_angle,
        float end_angle,
        juce::Slider& slider) override {
        const auto draw = [&] {
            juce::LookAndFeel_V4::drawRotarySlider(
                graphics,
                x,
                y,
                width,
                height,
                slider_position,
                start_angle,
                end_angle,
                slider);
        };
        if (slider.isEnabled()) {
            draw();
            return;
        }
        graphics.beginTransparencyLayer(kDisabledOpacity);
        draw();
        graphics.endTransparencyLayer();
    }

private:
    // 無効なバーはスイッチ（位相自動・音量維持）と同じ薄さへ揃える。
    static constexpr float kDisabledOpacity = 0.45F;
};

class SharedMidiInputService final
    : private juce::MidiInputCallback,
      private juce::Timer,
      private juce::AsyncUpdater {
public:
    struct DrainListener {
        virtual ~DrainListener() = default;
        virtual void midiMessagesPending() = 0;
    };

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
        cancelPendingUpdate();
        closeDevice();
    }

    SharedMidiInputService(const SharedMidiInputService&) = delete;
    SharedMidiInputService& operator=(
        const SharedMidiInputService&) = delete;

    void addDrainListener(DrainListener* listener) {
        drain_listeners_.add(listener);
    }

    void removeDrainListener(DrainListener* listener) {
        drain_listeners_.remove(listener);
    }

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
        {
            const juce::ScopedLock lock(queue_lock_);
            if (pending_messages_.size() < 2048) {
                pending_messages_.push_back(message);
            }
        }
        // Coalesce to the message thread so active keyboards can drain
        // ASAP instead of waiting only for the safety timer.
        triggerAsyncUpdate();
    }

    void handleAsyncUpdate() override {
        drain_listeners_.call(&DrainListener::midiMessagesPending);
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
    juce::ListenerList<DrainListener> drain_listeners_;
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

    void drawWhiteNote(
        int midi_note_number,
        juce::Graphics& graphics,
        juce::Rectangle<float> area,
        bool is_down,
        bool is_over,
        juce::Colour line_colour,
        juce::Colour text_colour) override {
        auto overlay = juce::Colours::transparentWhite;
        if (is_down) {
            overlay = findColour(keyDownOverlayColourId);
        }
        if (is_over) {
            overlay = overlay.overlaidWith(
                findColour(mouseOverKeyOverlayColourId));
        }
        graphics.setColour(overlay);
        graphics.fillRect(area);

        const auto text = getWhiteNoteText(midi_note_number);
        if (text.isNotEmpty()) {
            // Body role: C* labels match parameter / value chrome.
            const auto font_height = juce::jmin(
                UiFonts::bodyHeight(),
                getKeyWidth() * 0.9F);
            graphics.setColour(text_colour);
            graphics.setFont(
                UiFonts::body()
                    .withHeight(font_height)
                    .withHorizontalScale(0.8F));
            switch (getOrientation()) {
            case horizontalKeyboard:
                graphics.drawText(
                    text,
                    area.withTrimmedLeft(1.0F).withTrimmedBottom(2.0F),
                    juce::Justification::centredBottom,
                    false);
                break;
            case verticalKeyboardFacingLeft:
                graphics.drawText(
                    text,
                    area.reduced(2.0F),
                    juce::Justification::centredLeft,
                    false);
                break;
            case verticalKeyboardFacingRight:
                graphics.drawText(
                    text,
                    area.reduced(2.0F),
                    juce::Justification::centredRight,
                    false);
                break;
            default:
                break;
            }
        }

        if (!line_colour.isTransparent()) {
            graphics.setColour(line_colour);
            switch (getOrientation()) {
            case horizontalKeyboard:
                graphics.fillRect(area.withWidth(1.0F));
                break;
            case verticalKeyboardFacingLeft:
                graphics.fillRect(area.withHeight(1.0F));
                break;
            case verticalKeyboardFacingRight:
                graphics.fillRect(area.removeFromBottom(1.0F));
                break;
            default:
                break;
            }
            if (midi_note_number == getRangeEnd()) {
                switch (getOrientation()) {
                case horizontalKeyboard:
                    graphics.fillRect(
                        area.expanded(1.0F, 0).removeFromRight(1.0F));
                    break;
                case verticalKeyboardFacingLeft:
                    graphics.fillRect(
                        area.expanded(0, 1.0F).removeFromBottom(1.0F));
                    break;
                case verticalKeyboardFacingRight:
                    graphics.fillRect(
                        area.expanded(0, 1.0F).removeFromTop(1.0F));
                    break;
                default:
                    break;
                }
            }
        }
    }
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

[[nodiscard]] SnapshotResult writeComponentPngSnapshot(
    juce::Component& component,
    const juce::File& output_file) {
    if (component.getWidth() <= 0 || component.getHeight() <= 0) {
        return SnapshotResult::MissingContent;
    }
    const auto image = component.createComponentSnapshot(
        component.getLocalBounds(),
        true,
        1.0F);
    if (!image.isValid()) {
        return SnapshotResult::InvalidImage;
    }
    if (output_file.getParentDirectory().createDirectory().failed()) {
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

class PerformanceKeyboard final
    : public juce::Component,
      private juce::MidiKeyboardState::Listener,
      private juce::Timer,
      private SharedMidiInputService::DrainListener {
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

        octave_down_.setButtonText("PgDn");
        octave_down_.setTooltip(
            juce::String::fromUTF8(
                "PCキーボードのオクターブを下げます（Page Down）"));
        octave_down_.onClick = [this] { changePcOctave(-1); };
        addAndMakeVisible(octave_down_);

        octave_up_.setButtonText("PgUp");
        octave_up_.setTooltip(
            juce::String::fromUTF8(
                "PCキーボードのオクターブを上げます（Page Up）"));
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
        midi_service_.addDrainListener(this);
        // Event-driven PC keys / MIDI drain are primary; keep a light
        // safety timer for stuck keys and inactive-window cleanup.
        startTimerHz(15);
    }

    ~PerformanceKeyboard() override {
        stopTimer();
        midi_service_.removeDrainListener(this);
        LastAuditionNoteStore::instance().flushIfDirty();
        keyboard_state_.removeListener(this);
    }

    // Host editors call this from keyPressed / keyStateChanged.
    void pollPerformanceInput() {
        serviceActiveWindowInput(true);
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
        static_cast<void>(showAppSettings(0, nullptr));
    }

    [[nodiscard]] SnapshotResult captureSettingsTab(
        int initial_tab,
        const juce::File& output_file) {
        return showAppSettings(initial_tab, &output_file);
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
        if (character >= 0x20 && character != 0x7F) {
            return true;
        }
        const auto code = key.getKeyCode();
        return code == juce::KeyPress::pageUpKey
            || code == juce::KeyPress::pageDownKey;
    }

    [[nodiscard]] bool hasActiveNote() const noexcept {
        return !held_notes_.empty();
    }

    // プログラム差し替え後に、押し中の鍵盤音を同じノートで張り直す。
    void retriggerHeldNotes() {
        if (!note_on_ || held_notes_.empty()) {
            return;
        }
        std::vector<std::uint8_t> notes;
        notes.reserve(held_notes_.size());
        for (const auto& held : held_notes_) {
            const auto note = static_cast<std::uint8_t>(held.note);
            if (std::find(notes.begin(), notes.end(), note)
                == notes.end()) {
                notes.push_back(note);
            }
        }
        for (const auto note : notes) {
            note_on_(note);
        }
    }

    [[nodiscard]] int pcOctave() const noexcept {
        return pc_octave_;
    }

    // ライブラリ管理など鍵盤UI不要な場面向け。MIDI／PCキーのポーリングは継続する。
    void setHeadlessMode(bool headless) {
        headless_ = headless;
        keyboard_.setVisible(!headless);
        midi_input_.setVisible(false);
        octave_down_.setVisible(!headless);
        octave_up_.setVisible(!headless);
        octave_label_.setVisible(!headless);
        midi_status_.setVisible(!headless);
        mode_.setVisible(!headless);
        setInterceptsMouseClicks(!headless, !headless);
        if (headless) {
            setSize(0, 0);
        }
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
        if (headless_) {
            return;
        }
        auto area = getLocalBounds();
        auto controls = area.removeFromTop(30);
        mode_.setBounds(controls.removeFromLeft(68));
        controls.removeFromLeft(10);
        octave_down_.setBounds(controls.removeFromLeft(48));
        controls.removeFromLeft(4);
        octave_label_.setBounds(controls.removeFromLeft(132));
        controls.removeFromLeft(4);
        octave_up_.setBounds(controls.removeFromLeft(48));
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

    [[nodiscard]] SnapshotResult showAppSettings(
        int initial_tab,
        const juce::File* capture_file) {
        UiScale::forceGlobalForNonEditorUi();
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

                auto* view_page = new juce::Component();
                ui_scale_label_.setText(
                    juce::String::fromUTF8("画面の大きさ"),
                    juce::dontSendNotification);
                ui_scale_label_.setColour(
                    juce::Label::textColourId,
                    juce::Colour(0xFFE6EDF3));
                ui_scale_label_.setTooltip(
                    juce::String::fromUTF8(
                        "編集画面・ダイアログなどアプリ全体の表示倍率。"
                        "ウィンドウの位置とサイズは保存しません。"));
                view_page->addAndMakeVisible(ui_scale_label_);
                ui_scale_.addItem("125%", 125);
                ui_scale_.addItem("100%", 100);
                ui_scale_.addItem("75%", 75);
                ui_scale_.setSelectedId(
                    UiScale::global_percent, juce::dontSendNotification);
                ui_scale_.setTooltip(ui_scale_label_.getTooltip());
                view_page->addAndMakeVisible(ui_scale_);

                auto* midi_page = new juce::Component();
                midi_help_.setText(
                    juce::String::fromUTF8(
                        "使用するMIDI入力機器を選択してください。\n"
                        "接続状態は鍵盤上部のオクターブ操作（PgUp）の右側へ常時表示されます。"),
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
                pc_audio_help_.setText(
                    juce::String::fromUTF8(
                        "PC音声出力を選びます。既定はWASAPI共有モードです。\n"
                        "ASIOはドライバー選択後に適用し、必要ならASIO設定を開きます。"),
                    juce::dontSendNotification);
                pc_audio_help_.setJustificationType(
                    juce::Justification::topLeft);
                pc_audio_help_.setColour(
                    juce::Label::textColourId,
                    juce::Colour(0xFFE6EDF3));
                output_page->addAndMakeVisible(pc_audio_help_);

                pc_audio_kind_.addItem(
                    juce::String::fromUTF8(
                        "既定出力 (WASAPI共有モード)"),
                    1);
                pc_audio_kind_.addItem("ASIO", 2);
                pc_audio_kind_.setSelectedId(
                    audio_service_.pcAudioBackend()
                            == PcAudioBackend::Asio
                        ? 2
                        : 1,
                    juce::dontSendNotification);
                output_page->addAndMakeVisible(pc_audio_kind_);

                const auto saved_driver =
                    audio_service_.asioDriverName();
                int saved_driver_id{};
                int driver_id = 1;
                for (const auto& driver :
                     audio_service_.asioDriverNames()) {
                    asio_driver_.addItem(
                        juce::String::fromUTF8(driver.c_str()),
                        driver_id);
                    if (driver == saved_driver) {
                        saved_driver_id = driver_id;
                    }
                    ++driver_id;
                }
                if (saved_driver_id != 0) {
                    asio_driver_.setSelectedId(
                        saved_driver_id, juce::dontSendNotification);
                } else if (asio_driver_.getNumItems() > 0) {
                    asio_driver_.setSelectedItemIndex(
                        0, juce::dontSendNotification);
                }
                output_page->addAndMakeVisible(asio_driver_);

                asio_settings_.setButtonText(
                    juce::String::fromUTF8("ASIO設定"));
                output_page->addAndMakeVisible(asio_settings_);
                pc_audio_status_.setColour(
                    juce::Label::textColourId,
                    juce::Colour(0xFF9AA8B5));
                pc_audio_status_.setJustificationType(
                    juce::Justification::topLeft);
                output_page->addAndMakeVisible(pc_audio_status_);
                pc_audio_kind_.onChange = [this] {
                    const auto asio = pc_audio_kind_.getSelectedId() == 2;
                    asio_driver_.setEnabled(asio);
                };
                asio_driver_.setEnabled(
                    pc_audio_kind_.getSelectedId() == 2);
                refreshPcAudioStatus();

                mamidi_help_.setText(
                    juce::String::fromUTF8(
                        "音源の試聴経路を選びます。MAmidiMEmo は "
                        "MAmidiMEmo.exe -chip_server 起動後に接続します。\n"
                        "切断時はエミュレータへ自動切替しません。"),
                    juce::dontSendNotification);
                mamidi_help_.setFont(UiFonts::body());
                mamidi_help_.setJustificationType(
                    juce::Justification::topLeft);
                mamidi_help_.setColour(
                    juce::Label::textColourId,
                    juce::Colour(0xFFE6EDF3));
                output_page->addAndMakeVisible(mamidi_help_);

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
                UiFonts::styleBodyField(host_);
                port_.setInputRestrictions(5, "0123456789");
                port_.setText(juce::String(mamidi.port), false);
                UiFonts::styleBodyField(port_);
                unit_.setInputRestrictions(3, "0123456789");
                unit_.setText(juce::String(mamidi.unit_no), false);
                UiFonts::styleBodyField(unit_);
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

                opll_silence_label_.setText(
                    juce::String::fromUTF8(
                        "OPLLキーオフ後の強制消音（秒）"),
                    juce::dontSendNotification);
                opll_silence_label_.setColour(
                    juce::Label::textColourId,
                    juce::Colour(0xFFE6EDF3));
                output_page->addAndMakeVisible(opll_silence_label_);
                opll_silence_seconds_.setInputRestrictions(
                    4, "0123456789");
                opll_silence_seconds_.setText(
                    juce::String(
                        audio_service_.opllKeyOffForceSilenceSeconds()),
                    false);
                UiFonts::styleBodyField(opll_silence_seconds_);
                opll_silence_seconds_.setTooltip(
                    juce::String::fromUTF8(
                        "OPLLでキーオフ後も鳴り続ける音色（RR=0など）を、"
                        "指定秒数後に強制消音します。0で無効。"
                        "鍵盤演奏と1秒プレビューの両方に適用します。"));
                output_page->addAndMakeVisible(opll_silence_seconds_);

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
                    "View",
                    juce::Colour(0xFF243040),
                    view_page,
                    true);
                tabs_.addTab(
                    "MIDI",
                    juce::Colour(0xFF243040),
                    midi_page,
                    true);
                tabs_.addTab(
                    "Output",
                    juce::Colour(0xFF243040),
                    output_page,
                    true);
                tabs_.addTab(
                    "About",
                    juce::Colour(0xFF243040),
                    new AboutPanel(),
                    true);
                tabs_.setCurrentTabIndex(
                    juce::jlimit(0, 3, initial_tab),
                    juce::dontSendNotification);

                close_.setButtonText(
                    juce::String::fromUTF8("閉じる"));
                addAndMakeVisible(close_);
                setSize(
                    UiLayout::settingsDialogW,
                    UiLayout::settingsDialogH);
            }

            void paint(juce::Graphics& graphics) override {
                UiScale::forceGlobalForNonEditorUi();
                juce::Component::paint(graphics);
            }

            void resized() override {
                UiScale::forceGlobalForNonEditorUi();
                auto area = getLocalBounds().reduced(UiLayout::panelPad);
                auto bottom = area.removeFromBottom(UiLayout::fieldH);
                close_.setBounds(bottom.removeFromRight(UiScale::sx(96)));
                area.removeFromBottom(UiLayout::sm);
                tabs_.setBounds(area);
                tabs_.setTabBarDepth(UiLayout::fieldH);

                if (auto* view_page = tabs_.getTabContentComponent(0)) {
                    auto page = view_page->getLocalBounds().reduced(UiLayout::md);
                    ui_scale_label_.setBounds(
                        page.removeFromTop(UiLayout::fieldH));
                    page.removeFromTop(UiLayout::sm);
                    ui_scale_.setBounds(
                        page.removeFromTop(UiLayout::fieldH).removeFromLeft(
                            UiScale::sx(160)));
                }
                if (auto* midi_page = tabs_.getTabContentComponent(1)) {
                    auto page = midi_page->getLocalBounds().reduced(UiLayout::md);
                    midi_help_.setBounds(page.removeFromTop(UiScale::sx(56)));
                    page.removeFromTop(UiLayout::sm);
                    midi_input_.setBounds(
                        page.removeFromTop(UiScale::sx(28)));
                    page.removeFromTop(UiLayout::md);
                    auto row = page.removeFromTop(UiScale::sx(30));
                    apply_midi_.setBounds(row.removeFromLeft(UiScale::sx(96)));
                    row.removeFromLeft(UiLayout::sm);
                    refresh_.setBounds(row.removeFromLeft(UiScale::sx(96)));
                }
                if (auto* output_page =
                        tabs_.getTabContentComponent(2)) {
                    auto page =
                        output_page->getLocalBounds().reduced(12);
                    pc_audio_help_.setBounds(page.removeFromTop(48));
                    page.removeFromTop(8);
                    pc_audio_kind_.setBounds(page.removeFromTop(28));
                    page.removeFromTop(8);
                    asio_driver_.setBounds(page.removeFromTop(28));
                    page.removeFromTop(8);
                    asio_settings_.setBounds(
                        page.removeFromTop(30).removeFromLeft(112));
                    page.removeFromTop(8);
                    pc_audio_status_.setBounds(page.removeFromTop(48));
                    page.removeFromTop(12);
                    mamidi_help_.setBounds(page.removeFromTop(72));
                    page.removeFromTop(10);
                    output_kind_.setBounds(page.removeFromTop(28));
                    page.removeFromTop(10);
                    auto row = page.removeFromTop(UiLayout::fieldH);
                    host_.setBounds(row.removeFromLeft(180));
                    row.removeFromLeft(8);
                    port_.setBounds(row.removeFromLeft(72));
                    row.removeFromLeft(8);
                    unit_.setBounds(row.removeFromLeft(54));
                    row.removeFromLeft(8);
                    scc_plus_.setBounds(row.removeFromLeft(72));
                    page.removeFromTop(10);
                    waveform_monitor_.setBounds(
                        page.removeFromTop(UiLayout::fieldH));
                    page.removeFromTop(10);
                    opll_silence_label_.setBounds(
                        page.removeFromTop(22));
                    page.removeFromTop(4);
                    opll_silence_seconds_.setBounds(
                        page.removeFromTop(28).removeFromLeft(96));
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

            void refreshPcAudioStatus() {
                pc_audio_status_.setText(
                    juce::String::fromUTF8(
                        audio_service_.pcAudioStatus().c_str()),
                    juce::dontSendNotification);
                asio_settings_.setEnabled(
                    audio_service_.asioControlPanelAvailable());
            }

            juce::TabbedComponent tabs_{
                juce::TabbedButtonBar::TabsAtTop};
            juce::Label ui_scale_label_;
            juce::ComboBox ui_scale_;
            juce::Label midi_help_;
            juce::ComboBox midi_input_;
            juce::TextButton apply_midi_;
            juce::TextButton refresh_;
            juce::Label pc_audio_help_;
            juce::ComboBox pc_audio_kind_;
            juce::ComboBox asio_driver_;
            juce::TextButton asio_settings_;
            juce::Label pc_audio_status_;
            juce::Label mamidi_help_;
            juce::ComboBox output_kind_;
            juce::TextEditor host_;
            juce::TextEditor port_;
            juce::TextEditor unit_;
            juce::ToggleButton scc_plus_;
            juce::ToggleButton waveform_monitor_;
            juce::Label opll_silence_label_;
            juce::TextEditor opll_silence_seconds_;
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
        auto* dialog = new ModalDialogWindow(
            juce::String::fromUTF8("アプリ設定"),
            juce::Colour(0xFF1B222C));
        dialog->setUsingNativeTitleBar(true);
        dialog->setResizable(false, false);
        dialog->setContentOwned(content, true);
        dialog->centreWithSize(
            content->getWidth(), content->getHeight() + 32);

        if (capture_file != nullptr) {
            const auto result =
                writeComponentPngSnapshot(*content, *capture_file);
            delete dialog;
            return result;
        }

        juce::Component::SafePointer<PerformanceKeyboard> safe(this);
        juce::Component::SafePointer<juce::DialogWindow> safe_dialog(
            dialog);
        juce::Component::SafePointer<SettingsContent> safe_content(
            content);

        content->ui_scale_.onChange = [safe_content, safe_dialog] {
            if (safe_content == nullptr) {
                return;
            }
            const int percent = safe_content->ui_scale_.getSelectedId();
            if (!UiScale::isStep(percent)
                || percent == UiScale::global_percent) {
                return;
            }
            // Clears editor session overrides via listeners; scales all roots.
            UiScale::setGlobalPercent(percent, true);
            safe_content->setSize(
                UiLayout::settingsDialogW,
                UiLayout::settingsDialogH);
            safe_content->sendLookAndFeelChange();
            safe_content->resized();
            if (safe_dialog != nullptr) {
                safe_dialog->setSize(
                    safe_content->getWidth(),
                    safe_content->getHeight() + 32);
                safe_dialog->centreWithSize(
                    safe_dialog->getWidth(),
                    safe_dialog->getHeight());
                clampWindowToDisplayWorkArea(*safe_dialog);
            }
        };
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
                        static_cast<void>(
                            safe->showAppSettings(1, nullptr));
                    }
                });
            };
        content->apply_output_.onClick = [safe, safe_content] {
            if (safe == nullptr || safe_content == nullptr) {
                return;
            }
            const auto pc_backend =
                safe_content->pc_audio_kind_.getSelectedId() == 2
                ? PcAudioBackend::Asio
                : PcAudioBackend::Wasapi;
            const auto driver_name = utf8String(
                safe_content->asio_driver_.getText());
            if (!safe->audio_service_.applyPcAudioSettings(
                    pc_backend,
                    driver_name)) {
                safe_content->pc_audio_kind_.setSelectedId(
                    1, juce::sendNotificationSync);
            }
            safe_content->refreshPcAudioStatus();

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

            safe->audio_service_.setOpllKeyOffForceSilenceSeconds(
                juce::jlimit(
                    0,
                    9999,
                    safe_content->opll_silence_seconds_.getText()
                        .getIntValue()));
            safe_content->opll_silence_seconds_.setText(
                juce::String(
                    safe->audio_service_
                        .opllKeyOffForceSilenceSeconds()),
                false);

            const auto kind =
                safe_content->output_kind_.getSelectedId() == 2
                    ? mgstc::engine::SoundOutputKind::MAmidiMemo
                    : mgstc::engine::SoundOutputKind::Emulator;
            MGSTC_UI_ACTIVITY(
                "settings: sound output connect (blocking)");
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
        content->asio_settings_.onClick = [safe, safe_content] {
            if (safe == nullptr || safe_content == nullptr) {
                return;
            }
            static_cast<void>(
                safe->audio_service_.showAsioControlPanel());
            if (safe->audio_service_.pcAudioBackend()
                != PcAudioBackend::Asio) {
                safe_content->pc_audio_kind_.setSelectedId(
                    1, juce::sendNotificationSync);
            }
            safe_content->refreshPcAudioStatus();
        };
        content->reconnect_.onClick = [safe, safe_content] {
            if (safe == nullptr || safe_content == nullptr) {
                return;
            }
            MGSTC_UI_ACTIVITY(
                "settings: MAmidiMEmo reconnect (blocking)");
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
        return SnapshotResult::Success;
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
        LastAuditionNoteStore::instance().mark(
            static_cast<std::uint8_t>(note));
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
            virtualKeyIsDown(kPcOctaveDownVirtualKey);
        octave_up_key_ =
            virtualKeyIsDown(kPcOctaveUpVirtualKey);
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
            virtualKeyIsDown(kPcOctaveDownVirtualKey);
        const bool up =
            virtualKeyIsDown(kPcOctaveUpVirtualKey);
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

    void drainPendingMidi() {
        for (const auto& message :
             midi_service_.takePendingMessages()) {
            keyboard_state_.processNextMidiEvent(message);
        }
    }

    void serviceActiveWindowInput(bool poll_pc_keys) {
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
        drainPendingMidi();
        if (poll_pc_keys) {
            pollPcKeyboard();
        }
    }

    void midiMessagesPending() override {
        serviceActiveWindowInput(false);
    }

    void timerCallback() override {
        LastAuditionNoteStore::instance().flushIfDirty();
        serviceActiveWindowInput(true);
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
    bool headless_{};
    bool octave_down_key_{};
    bool octave_up_key_{};
};

class LibraryManagerContent final : public juce::Component {
public:
    using ApplyTagCallback = TagManagementContent::ApplyCallback;
    using PreviewCallback =
        std::function<void(LibraryManagerKind, std::uint64_t)>;
    using OpenEditorCallback =
        std::function<void(LibraryManagerKind, std::uint64_t)>;
    using RefreshEditorsCallback = std::function<void()>;
    using NoteOnCallback = std::function<void(
        LibraryManagerKind, std::uint64_t, std::uint8_t)>;
    using NoteOffCallback = std::function<void(
        LibraryManagerKind, std::uint8_t)>;
    using AllNotesOffCallback = std::function<void()>;

    LibraryManagerContent(
        LibraryManagerKind initial_kind,
        mgstc::engine::TimbreLibrary timbres,
        mgstc::engine::CompositeTimbreLibrary composites,
        SharedMidiInputService& midi_service,
        SharedAudioService& audio_service,
        ApplyTagCallback apply_tag,
        PreviewCallback preview,
        OpenEditorCallback open_editor,
        RefreshEditorsCallback refresh_editors,
        NoteOnCallback note_on,
        NoteOffCallback note_off,
        AllNotesOffCallback all_notes_off)
        : timbres_(std::move(timbres)),
          composites_(std::move(composites)),
          apply_tag_(std::move(apply_tag)),
          preview_(std::move(preview)),
          open_editor_(std::move(open_editor)),
          refresh_editors_(std::move(refresh_editors)),
          note_on_(std::move(note_on)),
          note_off_(std::move(note_off)),
          all_notes_off_(std::move(all_notes_off)),
          performance_keyboard_(midi_service, audio_service) {
        setWantsKeyboardFocus(true);
        tabs_.setTabBarDepth(UiLayout::fieldH);
        addAndMakeVisible(tabs_);

        list_tab_ = std::make_unique<LibraryManagerListTab>(
            initial_kind,
            preview_,
            open_editor_,
            [this] { return reloadLibraries(); },
            [this] { return persistTimbreLibrary(timbres_); },
            [this] {
                return persistCompositeTimbreLibrary(composites_);
            },
            [this] { return &timbres_; },
            [this] { return &composites_; },
            [this] { performance_keyboard_.allNotesOff(); },
            [this] {
                refreshTagManagementFromLibraries();
                if (refresh_editors_) {
                    refresh_editors_();
                }
            });
        tabs_.addTab(
            juce::String::fromUTF8("音色一覧"),
            juce::Colour(UiLayout::panelFill),
            list_tab_.get(),
            false);

        tag_tab_ = std::make_unique<TagManagementContent>(
            collectCustomTags(),
            apply_tag_,
            true,
            [this] { handleTagManagementChanged(); });
        tabs_.addTab(
            juce::String::fromUTF8("タグ管理"),
            juce::Colour(UiLayout::panelFill),
            tag_tab_.get(),
            false);

        performance_keyboard_.setHeadlessMode(true);
        performance_keyboard_.setCallbacks(
            [this](std::uint8_t note) { handlePerformanceNoteOn(note); },
            [this](std::uint8_t note) {
                handlePerformanceNoteOff(note);
            });
        performance_keyboard_.setSuppressPcInputCallback(
            [this] { return textEntryHasFocusWithin(*this); });
        addChildComponent(performance_keyboard_);

        setSize(
            UiLayout::libraryManagerW,
            UiLayout::libraryManagerH);
        {
            juce::Component::SafePointer<LibraryManagerContent> safe(this);
            UiScale::addGlobalListener([safe] {
                if (safe == nullptr) {
                    return;
                }
                UiScale::forceGlobalForNonEditorUi();
                safe->setSize(
                    UiLayout::libraryManagerW,
                    UiLayout::libraryManagerH);
                safe->sendLookAndFeelChange();
                safe->resized();
                if (auto* top = safe->getTopLevelComponent()) {
                    if (auto* window =
                            dynamic_cast<juce::ResizableWindow*>(top)) {
                        window->setSize(
                            safe->getWidth(),
                            safe->getHeight() + 32);
                        clampWindowToDisplayWorkArea(*window);
                    }
                }
            });
        }
    }

    ~LibraryManagerContent() override {
        performance_keyboard_.allNotesOff();
        if (all_notes_off_) {
            all_notes_off_();
        }
    }

    void paint(juce::Graphics& graphics) override {
        UiScale::forceGlobalForNonEditorUi();
        juce::Component::paint(graphics);
    }

    void resized() override {
        UiScale::forceGlobalForNonEditorUi();
        tabs_.setBounds(getLocalBounds());
        tabs_.setTabBarDepth(UiLayout::fieldH);
    }

    bool keyPressed(const juce::KeyPress& key) override {
        // Scale shortcuts are editor-only (Composite / SCC / OPLL).
        if (performance_keyboard_.shouldConsumeKeyPress(key)) {
            performance_keyboard_.pollPerformanceInput();
            return true;
        }
        return false;
    }

    bool keyStateChanged(bool) override {
        performance_keyboard_.pollPerformanceInput();
        return false;
    }

private:
    void handlePerformanceNoteOn(std::uint8_t note) {
        if (list_tab_ == nullptr || !note_on_) {
            return;
        }
        const auto target = list_tab_->performanceTarget();
        if (!target) {
            return;
        }
        active_performance_kind_ = target->kind;
        note_on_(target->kind, target->id, note);
    }

    void handlePerformanceNoteOff(std::uint8_t note) {
        if (!note_off_ || !active_performance_kind_) {
            return;
        }
        note_off_(*active_performance_kind_, note);
    }

    [[nodiscard]] std::vector<TagChoice> collectCustomTags() const {
        std::vector<std::vector<std::string>> tag_sets;
        tag_sets.reserve(
            timbres_.entries().size()
            + composites_.entries().size());
        for (const auto& entry : timbres_.entries()) {
            tag_sets.push_back(entry.tags);
        }
        for (const auto& entry : composites_.entries()) {
            tag_sets.push_back(entry.timbre.tags);
        }
        std::vector<TagChoice> custom_tags;
        for (auto usage :
             mgstc::engine::collectTimbreTagUsage(tag_sets)) {
            if (!mgstc::engine::isPresetTimbreTag(usage.name)) {
                custom_tags.push_back(
                    {std::move(usage.name), usage.count});
            }
        }
        return custom_tags;
    }

    bool reloadLibraries() {
        std::string error;
        auto timbres = loadTimbreLibrary(&error);
        auto composites = loadCompositeTimbreLibrary(&error);
        if (!timbres || !composites) {
            return false;
        }
        timbres_ = std::move(*timbres);
        composites_ = std::move(*composites);
        return true;
    }

    void refreshTagManagementFromLibraries() {
        if (tag_tab_ != nullptr) {
            tag_tab_->setCustomTags(collectCustomTags());
        }
    }

    void handleTagManagementChanged() {
        if (!reloadLibraries()) {
            return;
        }
        if (refresh_editors_) {
            refresh_editors_();
        }
        if (list_tab_ != nullptr) {
            list_tab_->reloadFromParent();
        }
        refreshTagManagementFromLibraries();
    }

    mgstc::engine::TimbreLibrary timbres_;
    mgstc::engine::CompositeTimbreLibrary composites_;
    ApplyTagCallback apply_tag_;
    PreviewCallback preview_;
    OpenEditorCallback open_editor_;
    RefreshEditorsCallback refresh_editors_;
    NoteOnCallback note_on_;
    NoteOffCallback note_off_;
    AllNotesOffCallback all_notes_off_;
    PerformanceKeyboard performance_keyboard_;
    std::optional<LibraryManagerKind> active_performance_kind_;
    juce::TabbedComponent tabs_{
        juce::TabbedButtonBar::TabsAtTop};
    std::unique_ptr<LibraryManagerListTab> list_tab_;
    std::unique_ptr<TagManagementContent> tag_tab_;
};

void showLibraryManagerDialog(
    juce::Component* anchor,
    LibraryManagerKind initial_kind,
    mgstc::engine::TimbreLibrary timbres,
    mgstc::engine::CompositeTimbreLibrary composites,
    SharedMidiInputService& midi_service,
    SharedAudioService& audio_service,
    LibraryManagerContent::ApplyTagCallback apply_tag,
    LibraryManagerContent::PreviewCallback preview,
    LibraryManagerContent::OpenEditorCallback open_editor,
    LibraryManagerContent::RefreshEditorsCallback refresh_editors,
    LibraryManagerContent::NoteOnCallback note_on,
    LibraryManagerContent::NoteOffCallback note_off,
    LibraryManagerContent::AllNotesOffCallback all_notes_off) {
    UiScale::forceGlobalForNonEditorUi();
    auto* dialog = new ModalDialogWindow(
        juce::String::fromUTF8("ライブラリ管理"),
        juce::Colour(0xFF1B222C));
    auto* content = new LibraryManagerContent(
        initial_kind,
        std::move(timbres),
        std::move(composites),
        midi_service,
        audio_service,
        std::move(apply_tag),
        std::move(preview),
        std::move(open_editor),
        std::move(refresh_editors),
        std::move(note_on),
        std::move(note_off),
        std::move(all_notes_off));
    dialog->setUsingNativeTitleBar(true);
    dialog->setContentOwned(content, true);
    dialog->setResizable(true, true);
    dialog->setResizeLimits(
        UiScale::sx(900),
        UiScale::sx(560),
        UiScale::sx(1600),
        UiScale::sx(960));
    if (anchor != nullptr) {
        dialog->centreAroundComponent(
            anchor, content->getWidth(), content->getHeight() + 32);
    } else {
        dialog->centreWithSize(
            content->getWidth(), content->getHeight() + 32);
    }
    dialog->enterModalState(true, nullptr, true);
    content->grabKeyboardFocus();
}


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
    CompositeEnvelopeLane lane,
    const mgstc::engine::TimbreNumberResolution& numbers,
    const mgstc::engine::TimbreLibrary* library = nullptr,
    bool include_original_tone_y = true,
    bool expand_tl_auto = true,
    bool expand_fb_auto = true) {
    struct TimedEvent {
        std::uint32_t count{};
        mgstc::engine::EnvelopeEventKind kind{};
        std::int32_t value{};
        std::int32_t secondary{};
        std::uint64_t target_library_id{};
        mgstc::engine::TimbrePick timbre_pick{
            mgstc::engine::TimbrePick::Library};
    };
    std::vector<TimedEvent> events;
    const auto collect = [&events](
        const std::vector<mgstc::engine::EnvelopeEvent>& source,
        mgstc::engine::EnvelopeEventKind expected) {
        for (const auto& event : source) {
            if (event.kind == expected) {
                events.push_back({
                    event.count,
                    event.kind,
                    event.value,
                    event.secondary,
                    event.target_library_id,
                    event.timbre_pick});
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
        collect(
            layer.timbre_automation,
            mgstc::engine::EnvelopeEventKind::Timbre);
        for (const auto& event : layer.timbre_automation) {
            if (event.kind
                != mgstc::engine::EnvelopeEventKind::RegisterWrite) {
                continue;
            }
            // Poly secondary composite voices omit shared original-tone
            // y (regs 0–7). Channel-local y stays.
            if (!include_original_tone_y
                && event.value >= 0
                && event.value <= 7) {
                continue;
            }
            events.push_back({
                event.count,
                event.kind,
                event.value,
                event.secondary,
                event.target_library_id,
                event.timbre_pick});
        }
        if (include_original_tone_y
            && layer.source == mgstc::engine::TimbreSource::Opll
            && (expand_tl_auto || expand_fb_auto)) {
            for (const auto& event :
                 mgstc::engine::expandOpllLayerRegisterAutos(
                     layer,
                     library,
                     expand_tl_auto,
                     expand_fb_auto)) {
                if (event.kind
                    == mgstc::engine::EnvelopeEventKind::RegisterWrite) {
                    events.push_back({
                        event.count,
                        event.kind,
                        event.value,
                        event.secondary,
                        event.target_library_id,
                        event.timbre_pick});
                }
            }
        }
        if (std::none_of(
                events.begin(), events.end(),
                [](const TimedEvent& event) {
                    return event.kind
                        == mgstc::engine::EnvelopeEventKind::Timbre
                        && event.count == 0;
                })
            && mgstc::engine::layerEnvelopeHasPatchSlide(layer, &numbers)) {
            mgstc::engine::EnvelopeEvent leading{
                .kind = mgstc::engine::EnvelopeEventKind::Timbre,
            };
            bool have_base = false;
            if (mgstc::engine::layerUsesOpllRomBase(layer)) {
                leading.timbre_pick =
                    mgstc::engine::TimbrePick::OpllRom;
                leading.value = static_cast<std::int32_t>(
                    *layer.base_opll_rom);
                have_base = true;
            } else if (layer.base_timbre) {
                leading.timbre_pick =
                    mgstc::engine::TimbrePick::Library;
                leading.target_library_id =
                    layer.base_timbre->library_id;
                have_base = true;
            }
            if (have_base) {
                events.push_back({
                    0,
                    leading.kind,
                    leading.value,
                    0,
                    leading.target_library_id,
                    leading.timbre_pick});
            }
        }
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
                    juce::jlimit(-127, 127, event.value))});
            break;
        case mgstc::engine::EnvelopeEventKind::Timbre: {
            mgstc::engine::EnvelopeEvent resolved{
                .kind = mgstc::engine::EnvelopeEventKind::Timbre,
                .value = event.value,
                .target_library_id = event.target_library_id,
                .timbre_pick = event.timbre_pick,
            };
            const auto number =
                mgstc::engine::envelopeEventTimbreNumber(
                    resolved, &numbers);
            bytecode.insert(
                bytecode.end(),
                {0x10, number.value_or(0)});
            break;
        }
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
    const mgstc::engine::CompositeLayer& layer,
    const mgstc::engine::TimbreNumberResolution& numbers,
    const mgstc::engine::TimbreLibrary* library = nullptr,
    bool include_original_tone_y = true,
    bool expand_tl_auto = true,
    bool expand_fb_auto = true) {
    return {
        .volume = compileCompositeEnvelopeLane(
            layer,
            CompositeEnvelopeLane::Volume,
            numbers,
            library,
            include_original_tone_y,
            expand_tl_auto,
            expand_fb_auto),
        .pitch = compileCompositeEnvelopeLane(
            layer,
            CompositeEnvelopeLane::Pitch,
            numbers,
            library,
            include_original_tone_y,
            expand_tl_auto,
            expand_fb_auto),
        .timbre = compileCompositeEnvelopeLane(
            layer,
            CompositeEnvelopeLane::Timbre,
            numbers,
            library,
            include_original_tone_y,
            expand_tl_auto,
            expand_fb_auto),
    };
}

[[nodiscard]] juce::String pitchCommandLabel(std::int32_t value) {
    if (value >= 0) {
        return juce::String::fromUTF8("\\+") + juce::String(value);
    }
    return juce::String::fromUTF8("\\") + juce::String(value);
}

[[nodiscard]] juce::String opllRomPatchLabel(int rom_number) {
    static constexpr std::array<const char*, 15> names{
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
    if (rom_number < 0 || rom_number >= static_cast<int>(names.size())) {
        return "@" + juce::String(rom_number);
    }
    return juce::String::fromUTF8(names[static_cast<std::size_t>(rom_number)]);
}

struct EnvelopeTimbreCatalogItem {
    std::uint64_t library_id{};
    juce::String name;
    std::uint32_t revision{1};
    bool favorite{};
    std::optional<std::uint8_t> assigned_number;
};

struct EnvelopeTimbreChoice {
    mgstc::engine::TimbrePick pick{mgstc::engine::TimbrePick::Library};
    std::uint64_t library_id{};
    std::uint8_t rom_number{};
};

class EnvelopeTimbrePickContent final : public juce::Component {
public:
    EnvelopeTimbrePickContent(
        mgstc::engine::TimbreSource source,
        const std::vector<EnvelopeTimbreCatalogItem>& catalog,
        const EnvelopeTimbreChoice& current,
        std::function<void(EnvelopeTimbreChoice)> on_choose,
        std::function<void(std::uint64_t)> on_edit,
        std::function<void()> on_remove)
        : source_(source),
          catalog_(catalog),
          on_choose_(std::move(on_choose)),
          on_edit_(std::move(on_edit)),
          on_remove_(std::move(on_remove)) {
        hint_.setText(
            juce::String::fromUTF8(
                source_ == mgstc::engine::TimbreSource::Opll
                    ? "ライブラリ音色またはOPLL ROM（@0～@14）"
                    : "この音源の音色ライブラリから切替先を選びます"),
            juce::dontSendNotification);
        hint_.setFont(UiFonts::body());
        addAndMakeVisible(hint_);

        int item_id = 1;
        if (source_ == mgstc::engine::TimbreSource::Opll) {
            for (int rom = 0; rom < 15; ++rom) {
                combo_.addItem(opllRomPatchLabel(rom), item_id++);
            }
        }
        library_id_base_ = item_id;
        for (const auto& item : catalog_) {
            auto label = item.name;
            if (item.favorite) {
                label = juce::String::fromUTF8("★ ") + label;
            }
            if (item.revision != 0) {
                label += " (r" + juce::String(static_cast<int>(item.revision)) + ")";
            }
            if (item.assigned_number) {
                label += "  #"
                    + juce::String(static_cast<int>(*item.assigned_number));
            }
            combo_.addItem(label, item_id++);
        }
        combo_.setTextWhenNothingSelected(
            juce::String::fromUTF8("切替先の音色"));
        int selected = 0;
        if (current.pick == mgstc::engine::TimbrePick::OpllRom
            && source_ == mgstc::engine::TimbreSource::Opll) {
            selected = current.rom_number + 1;
        } else if (current.library_id != 0) {
            for (std::size_t index = 0; index < catalog_.size(); ++index) {
                if (catalog_[index].library_id == current.library_id) {
                    selected = library_id_base_ + static_cast<int>(index);
                    break;
                }
            }
        }
        combo_.setSelectedId(selected, juce::dontSendNotification);
        combo_.onChange = [this] { updateEditEnabled(); };
        addAndMakeVisible(combo_);

        edit_.setButtonText(juce::String::fromUTF8("単音色編集"));
        edit_.setTooltip(juce::String::fromUTF8(
            "選択したライブラリ音色をSCC／OPLLエディタで開きます。ROMは開けません"));
        edit_.onClick = [this] {
            if (const auto id = selectedLibraryId()) {
                on_edit_(*id);
            }
        };
        addAndMakeVisible(edit_);
        ok_.setButtonText(juce::String::fromUTF8("決定"));
        ok_.onClick = [this] {
            if (auto choice = selectedChoice()) {
                on_choose_(*choice);
                closeHost();
            }
        };
        addAndMakeVisible(ok_);
        remove_.setButtonText(juce::String::fromUTF8("削除"));
        remove_.setTooltip(juce::String::fromUTF8(
            "このカウントの@イベントを削除します"));
        remove_.onClick = [this] {
            on_remove_();
            closeHost();
        };
        addAndMakeVisible(remove_);
        cancel_.setButtonText(juce::String::fromUTF8("キャンセル"));
        cancel_.onClick = [this] {
            if (auto* window = findParentComponentOfClass<juce::DialogWindow>()) {
                window->exitModalState(0);
            }
        };
        addAndMakeVisible(cancel_);
        updateEditEnabled();
        setSize(UiLayout::compositeTimbrePickW, UiLayout::compositeTimbrePickH);
    }

    void resized() override {
        using namespace UiLayout;
        auto area = getLocalBounds().reduced(panelPad);
        hint_.setBounds(area.removeFromTop(fieldH));
        area.removeFromTop(sm);
        combo_.setBounds(area.removeFromTop(fieldH));
        area.removeFromTop(sm);
        auto buttons = area.removeFromTop(textButtonH);
        const int button_w = juce::jmax(
            libraryButtonMinW,
            (buttons.getWidth() - controlGap * 3) / 4);
        edit_.setBounds(buttons.removeFromLeft(button_w));
        buttons.removeFromLeft(controlGap);
        ok_.setBounds(buttons.removeFromLeft(button_w));
        buttons.removeFromLeft(controlGap);
        remove_.setBounds(buttons.removeFromLeft(button_w));
        buttons.removeFromLeft(controlGap);
        cancel_.setBounds(buttons);
    }

private:
    void closeHost() {
        if (auto* window = findParentComponentOfClass<juce::DialogWindow>()) {
            window->exitModalState(1);
        }
    }

    void updateEditEnabled() {
        edit_.setEnabled(selectedLibraryId().has_value());
        ok_.setEnabled(selectedChoice().has_value());
    }

    [[nodiscard]] std::optional<std::uint64_t> selectedLibraryId() const {
        const auto id = combo_.getSelectedId();
        if (id < library_id_base_) {
            return std::nullopt;
        }
        const auto index = static_cast<std::size_t>(id - library_id_base_);
        if (index >= catalog_.size()) {
            return std::nullopt;
        }
        return catalog_[index].library_id;
    }

    [[nodiscard]] std::optional<EnvelopeTimbreChoice> selectedChoice() const {
        const auto id = combo_.getSelectedId();
        if (id <= 0) {
            return std::nullopt;
        }
        EnvelopeTimbreChoice choice;
        if (source_ == mgstc::engine::TimbreSource::Opll && id < library_id_base_) {
            choice.pick = mgstc::engine::TimbrePick::OpllRom;
            choice.rom_number = static_cast<std::uint8_t>(id - 1);
            return choice;
        }
        if (const auto library_id = selectedLibraryId()) {
            choice.pick = mgstc::engine::TimbrePick::Library;
            choice.library_id = *library_id;
            return choice;
        }
        return std::nullopt;
    }

    mgstc::engine::TimbreSource source_;
    std::vector<EnvelopeTimbreCatalogItem> catalog_;
    int library_id_base_{1};
    std::function<void(EnvelopeTimbreChoice)> on_choose_;
    std::function<void(std::uint64_t)> on_edit_;
    std::function<void()> on_remove_;
    juce::Label hint_;
    juce::ComboBox combo_;
    juce::TextButton edit_;
    juce::TextButton ok_;
    juce::TextButton remove_;
    juce::TextButton cancel_;
};

struct LayerBaseTimbreAssign {
    std::optional<std::uint64_t> library_id;
    std::optional<std::uint8_t> opll_rom;
};

class CompositeLayerSetupView final : public juce::Component {
public:
    using ChangedCallback = std::function<void()>;
    using AssignCallback = std::function<void(LayerBaseTimbreAssign)>;
    using EditCallback = std::function<void()>;

    CompositeLayerSetupView() {
        source_.setFont(UiFonts::heading());
        addAndMakeVisible(source_);
        enabled_.setButtonText(juce::String::fromUTF8("有効"));
        mute_.setButtonText(juce::String::fromUTF8("ミュート"));
        solo_.setButtonText(juce::String::fromUTF8("ソロ"));
        for (auto* toggle : {&enabled_, &mute_, &solo_}) {
            toggle->onClick = [this] {
                if (changed_) {
                    changed_();
                }
            };
            addAndMakeVisible(*toggle);
        }
        UiFonts::styleBodyField(name_);
        name_.onTextChange = [this] {
            if (changed_) {
                changed_();
            }
        };
        addAndMakeVisible(name_);
        timbre_.setTextWhenNothingSelected(
            juce::String::fromUTF8("音色を選択"));
        timbre_.onChange = [this] {
            if (assigning_ || !assign_) {
                return;
            }
            const auto selected = timbre_.getSelectedId();
            if (selected <= 0) {
                assign_({});
                return;
            }
            if (rom_item_base_ > 0
                && selected >= rom_item_base_
                && selected < rom_item_base_ + 15) {
                assign_({
                    .opll_rom = static_cast<std::uint8_t>(
                        selected - rom_item_base_),
                });
                return;
            }
            const int library_index = selected - library_item_base_;
            if (library_index < 0
                || static_cast<std::size_t>(library_index)
                    >= library_ids_.size()) {
                assign_({});
                return;
            }
            assign_({
                .library_id =
                    library_ids_[static_cast<std::size_t>(library_index)],
            });
        };
        addAndMakeVisible(timbre_);
        channel_.onChange = [this] {
            if (changed_) {
                changed_();
            }
        };
        addAndMakeVisible(channel_);
        number_mode_.addItem(juce::String::fromUTF8("自動"), 1);
        number_mode_.addItem(juce::String::fromUTF8("手動"), 2);
        number_mode_.setSelectedId(1, juce::dontSendNotification);
        number_mode_.setTooltip(juce::String::fromUTF8(
            "MGSC音色番号の割り付け。自動＝空き番号、手動＝右の音色番号を使用"));
        number_mode_.onChange = [this] {
            syncTimbreNumberEnabled();
            if (!assigning_ && changed_) {
                changed_();
            }
        };
        addAndMakeVisible(number_mode_);
        timbre_number_label_.setText(
            juce::String::fromUTF8("音色番号"),
            juce::dontSendNotification);
        timbre_number_label_.setFont(UiFonts::dense());
        timbre_number_label_.setJustificationType(
            juce::Justification::centredLeft);
        timbre_number_label_.setTooltip(juce::String::fromUTF8(
            "手動時のMGSC @番号（15～31）。自動時は無効"));
        addAndMakeVisible(timbre_number_label_);
        timbre_number_.setRange(15.0, 31.0, 1.0);
        timbre_number_.setSliderStyle(juce::Slider::LinearHorizontal);
        timbre_number_.setTextBoxStyle(
            juce::Slider::TextBoxLeft, false, 42, UiLayout::fieldH - 10);
        timbre_number_.setTooltip(timbre_number_label_.getTooltip());
        timbre_number_.onValueChange = [this] {
            if (!assigning_ && changed_) {
                changed_();
            }
        };
        addAndMakeVisible(timbre_number_);
        edit_.setButtonText(juce::String::fromUTF8("単音色編集"));
        edit_.onClick = [this] {
            if (edit_callback_) {
                edit_callback_();
            }
        };
        addAndMakeVisible(edit_);
        configureValueSlider(pitch_, -24.0, 24.0, " st");
        configureValueSlider(detune_, -127.0, 127.0, "");
        configureValueSlider(micro_detune_, -32768.0, 32767.0, "");
        configureValueSlider(delay_, 0.0, 256.0, "");
        configureValueSlider(volume_, 0.0, 15.0, "");
        delay_form_.addItem("r", 1);
        delay_form_.addItem("r%", 2);
        delay_form_.setSelectedId(2, juce::dontSendNotification);
        delay_form_.onChange = [this] {
            updateDelayValueRange();
            if (!assigning_ && changed_) {
                changed_();
            }
        };
        delay_form_.setTooltip(juce::String::fromUTF8(
            "発音前休符の記法。r=音符長（テンポ依存）、"
            "r%=1/60秒ティック（テンポ非依存）。@e内の待ちではない"));
        addAndMakeVisible(delay_form_);
        pitch_label_.setText(
            juce::String::fromUTF8("相対音程"),
            juce::dontSendNotification);
        detune_label_.setText(juce::String::fromUTF8("\\"), juce::dontSendNotification);
        micro_detune_label_.setText(
            juce::String::fromUTF8("@\\"), juce::dontSendNotification);
        delay_label_.setText(juce::String::fromUTF8("休符"), juce::dontSendNotification);
        volume_label_.setText(juce::String::fromUTF8("v"), juce::dontSendNotification);
        pitch_.setTooltip(juce::String::fromUTF8(
            "相対音程（半音）。鍵盤／PCキー／MIDI試聴の音程オフセット（§6.5.3）"));
        detune_.setTooltip(juce::String::fromUTF8(
            "トラック・デチューン（MML \\）。@e内の\\とは別。"
            "キーオン時に先に適用し、エンベロープ\\はこれに加算"));
        micro_detune_.setTooltip(juce::String::fromUTF8(
            "トラック微デチューン（MML @\\）。\\と同時利用で効果が重なる。"
            "PSG/SCC: -32768～32767（128≒半音）、FM: 0～255（255≒半音上）"));
        delay_.setTooltip(juce::String::fromUTF8(
            "キーオン前の休符ディレイ（トラックMML。エンベロープ内カウントではない）。"
            "r<n>は音符長（例 r4＝4分、実時間は複合音色のテンポ依存）。"
            "r%<n>は1/60秒ティック（例 r%30＝0.5秒、テンポ非依存）。"
            "グリッド左側の薄表示は発音前待ちの目安"));
        volume_.setTooltip(juce::String::fromUTF8(
            "トラック音量 v（発音前MML）。@eのfはこの値を最大として引き算"));
        delay_label_.setTooltip(delay_.getTooltip());
        for (auto* label : {
                 &pitch_label_,
                 &detune_label_,
                 &micro_detune_label_,
                 &delay_label_,
                 &volume_label_}) {
            label->setFont(UiFonts::dense());
            label->setJustificationType(juce::Justification::centredLeft);
            addAndMakeVisible(*label);
        }
        updateDelayValueRange();
        syncTimbreNumberEnabled();
    }

    void paint(juce::Graphics& graphics) override {
        graphics.fillAll(juce::Colour(UiLayout::panelFill));
    }

    void setCallbacks(
        ChangedCallback changed,
        AssignCallback assign,
        EditCallback edit) {
        changed_ = std::move(changed);
        assign_ = std::move(assign);
        edit_callback_ = std::move(edit);
    }

    void syncFromLayer(
        const mgstc::engine::CompositeLayer& layer,
        const std::vector<EnvelopeTimbreCatalogItem>& catalog) {
        assigning_ = true;
        const char* source_name =
            layer.source == mgstc::engine::TimbreSource::Psg
                ? "PSG"
                : layer.source == mgstc::engine::TimbreSource::Scc
                    ? "SCC" : "OPLL";
        source_.setText(source_name, juce::dontSendNotification);
        source_.setColour(
            juce::Label::textColourId,
            layer.source == mgstc::engine::TimbreSource::Psg
                ? juce::Colour(0xFFB990FF)
                : layer.source == mgstc::engine::TimbreSource::Scc
                    ? juce::Colour(0xFF53E3A6)
                    : juce::Colour(0xFFFFA75E));
        enabled_.setToggleState(layer.enabled, juce::dontSendNotification);
        mute_.setToggleState(layer.muted, juce::dontSendNotification);
        solo_.setToggleState(layer.solo, juce::dontSendNotification);
        name_.setText(juce::String::fromUTF8(layer.name.c_str()), false);
        const int channel_count =
            layer.source == mgstc::engine::TimbreSource::Psg
                ? 3
                : layer.source == mgstc::engine::TimbreSource::Scc ? 5 : 9;
        channel_.clear(juce::dontSendNotification);
        for (int channel = 0; channel < channel_count; ++channel) {
            channel_.addItem(
                juce::String::fromUTF8("Ch.") + juce::String(channel + 1),
                channel + 1);
        }
        channel_.setSelectedId(
            static_cast<int>(layer.channel) + 1,
            juce::dontSendNotification);
        const bool has_timbre_choice =
            layer.source != mgstc::engine::TimbreSource::Psg;
        const bool rom_base = mgstc::engine::layerUsesOpllRomBase(layer);
        const bool library_base = layer.base_timbre.has_value();
        timbre_.setEnabled(has_timbre_choice);
        edit_.setEnabled(library_base);
        number_mode_.setEnabled(library_base && !rom_base);
        library_ids_.clear();
        timbre_.clear(juce::dontSendNotification);
        rom_item_base_ = 0;
        library_item_base_ = 1;
        int selected = 0;
        if (has_timbre_choice) {
            int item_id = 1;
            if (layer.source == mgstc::engine::TimbreSource::Opll) {
                rom_item_base_ = item_id;
                for (int rom = 0; rom < 15; ++rom) {
                    timbre_.addItem(opllRomPatchLabel(rom), item_id++);
                }
                if (rom_base) {
                    selected = rom_item_base_
                        + static_cast<int>(*layer.base_opll_rom);
                }
                library_item_base_ = item_id;
            }
            for (const auto& item : catalog) {
                library_ids_.push_back(item.library_id);
                auto label = item.name;
                if (item.favorite) {
                    label = juce::String::fromUTF8("★ ") + label;
                }
                const int this_id = item_id++;
                timbre_.addItem(label, this_id);
                if (library_base
                    && layer.base_timbre->library_id == item.library_id) {
                    selected = this_id;
                }
            }
            timbre_.setSelectedId(selected, juce::dontSendNotification);
            timbre_.setTextWhenNothingSelected(
                juce::String::fromUTF8(
                    layer.source == mgstc::engine::TimbreSource::Opll
                        ? "ROMまたは保存音色"
                        : "保存音色を選択"));
        } else {
            timbre_.setTextWhenNothingSelected(
                juce::String::fromUTF8("PSGは@なし"));
        }
        if (rom_base) {
            timbre_number_.setRange(0.0, 14.0, 1.0);
            number_mode_.setSelectedId(1, juce::dontSendNotification);
            timbre_number_.setValue(
                static_cast<double>(*layer.base_opll_rom),
                juce::dontSendNotification);
            edit_.setButtonText(juce::String::fromUTF8("単音色編集"));
            edit_.setTooltip(juce::String::fromUTF8(
                "ROM音色は個別エディタで開けません"));
            number_mode_.setTooltip(juce::String::fromUTF8(
                "ROM音色は音色番号 0〜14 固定のため変更できません"));
            timbre_number_label_.setTooltip(number_mode_.getTooltip());
            timbre_number_.setTooltip(number_mode_.getTooltip());
        } else if (library_base) {
            timbre_number_.setRange(15.0, 31.0, 1.0);
            const bool manual =
                layer.base_timbre->number_mode
                == mgstc::engine::TimbreNumberMode::Manual;
            number_mode_.setSelectedId(
                manual ? 2 : 1, juce::dontSendNotification);
            timbre_number_.setValue(
                layer.base_timbre->manual_number.value_or(15),
                juce::dontSendNotification);
            edit_.setButtonText(
                juce::String::fromUTF8("編集: ")
                    + juce::String::fromUTF8(layer.base_timbre->name.c_str()));
            edit_.setTooltip(juce::String::fromUTF8(
                "割り当て音色を個別画面で開く"));
            number_mode_.setTooltip(juce::String::fromUTF8(
                "MGSC音色番号の割り付け。自動＝空き番号、手動＝右の音色番号を使用"));
            timbre_number_label_.setTooltip(juce::String::fromUTF8(
                "手動時のMGSC @番号（15～31）。自動時は無効"));
            timbre_number_.setTooltip(timbre_number_label_.getTooltip());
        } else {
            timbre_number_.setRange(15.0, 31.0, 1.0);
            number_mode_.setSelectedId(1, juce::dontSendNotification);
            edit_.setButtonText(juce::String::fromUTF8("単音色編集"));
            edit_.setTooltip(juce::String::fromUTF8(
                "先に音色を割り当ててください"));
            number_mode_.setTooltip(juce::String::fromUTF8(
                "ライブラリ音色を割り当てると音色番号を設定できます"));
            timbre_number_label_.setTooltip(number_mode_.getTooltip());
            timbre_number_.setTooltip(number_mode_.getTooltip());
        }
        syncTimbreNumberEnabled();
        pitch_.setValue(layer.relative_semitones, juce::dontSendNotification);
        detune_.setValue(layer.detune, juce::dontSendNotification);
        updateMicroDetuneRange(layer.source);
        micro_detune_.setValue(
            layer.micro_detune, juce::dontSendNotification);
        delay_form_.setSelectedId(
            layer.start_delay_form
                    == mgstc::engine::StartDelayForm::NoteLength
                ? 1
                : 2,
            juce::dontSendNotification);
        updateDelayValueRange();
        delay_.setValue(
            static_cast<double>(layer.start_delay_value),
            juce::dontSendNotification);
        volume_.setValue(layer.volume, juce::dontSendNotification);
        assigning_ = false;
    }

    void applyToLayer(mgstc::engine::CompositeLayer& layer) const {
        layer.enabled = enabled_.getToggleState();
        layer.muted = mute_.getToggleState();
        layer.solo = solo_.getToggleState();
        layer.name = name_.getText().toStdString();
        layer.channel = static_cast<std::uint8_t>(
            juce::jmax(1, channel_.getSelectedId()) - 1);
        layer.relative_semitones = static_cast<std::int8_t>(pitch_.getValue());
        layer.detune = static_cast<std::int16_t>(detune_.getValue());
        {
            const int micro = static_cast<int>(micro_detune_.getValue());
            layer.micro_detune = layer.source == mgstc::engine::TimbreSource::Opll
                ? juce::jlimit(0, 255, micro)
                : juce::jlimit(-32768, 32767, micro);
        }
        layer.start_delay_form =
            delay_form_.getSelectedId() == 1
                ? mgstc::engine::StartDelayForm::NoteLength
                : mgstc::engine::StartDelayForm::AbsoluteTicks;
        layer.start_delay_value =
            static_cast<std::uint32_t>(delay_.getValue());
        layer.volume = static_cast<std::uint8_t>(volume_.getValue());
        if (!layer.base_timbre) {
            return;
        }
        const bool manual = number_mode_.getSelectedId() == 2;
        layer.base_timbre->number_mode = manual
            ? mgstc::engine::TimbreNumberMode::Manual
            : mgstc::engine::TimbreNumberMode::Automatic;
        layer.base_timbre->manual_number = manual
            ? std::optional<std::uint8_t>{
                  static_cast<std::uint8_t>(timbre_number_.getValue())}
            : std::nullopt;
    }

    void resized() override {
        using namespace UiLayout;
        auto area = getLocalBounds().reduced(xs, xs);
        auto title = area.removeFromTop(compositeToolRowH);
        source_.setBounds(title.removeFromLeft(48));
        solo_.setBounds(title.removeFromRight(UiScale::sx(56)));
        mute_.setBounds(title.removeFromRight(UiScale::sx(70)));
        enabled_.setBounds(title.removeFromRight(52));
        area.removeFromTop(xs);
        auto identity = area.removeFromTop(fieldH);
        channel_.setBounds(identity.removeFromRight(66));
        identity.removeFromRight(controlGap);
        name_.setBounds(identity);
        area.removeFromTop(xs);
        auto timbre_row = area.removeFromTop(fieldH);
        edit_.setBounds(timbre_row.removeFromRight(libraryButtonMinW + 8));
        timbre_row.removeFromRight(controlGap);
        timbre_.setBounds(timbre_row);
        area.removeFromTop(xs);
        auto number_row = area.removeFromTop(fieldH);
        number_mode_.setBounds(number_row.removeFromLeft(UiScale::sx(70)));
        number_row.removeFromLeft(controlGap);
        timbre_number_label_.setBounds(
            number_row.removeFromLeft(setupValueLabelW));
        number_row.removeFromLeft(xs);
        timbre_number_.setBounds(number_row);
        area.removeFromTop(xs);
        auto pitch_row = area.removeFromTop(fieldH);
        layoutValue(
            pitch_row, pitch_label_, pitch_, setupValueLabelW);
        area.removeFromTop(xs);
        auto detune_row = area.removeFromTop(fieldH);
        const int half = juce::jmax(
            1, (detune_row.getWidth() - controlGap) / 2);
        layoutValue(detune_row.removeFromLeft(half), detune_label_, detune_);
        detune_row.removeFromLeft(controlGap);
        layoutValue(detune_row, micro_detune_label_, micro_detune_);
        area.removeFromTop(xs);
        auto delay_row = area.removeFromTop(fieldH);
        auto delay_half = delay_row.removeFromLeft(half);
        delay_label_.setBounds(
            delay_half.removeFromLeft(UiLayout::editSubLaneLabelW));
        delay_form_.setBounds(delay_half.removeFromLeft(72));
        delay_half.removeFromLeft(controlGap);
        delay_.setBounds(delay_half);
        delay_row.removeFromLeft(controlGap);
        layoutValue(delay_row, volume_label_, volume_);
    }

    void mouseDown(const juce::MouseEvent&) override {
        if (changed_) {
            changed_();
        }
    }

private:
    void updateDelayValueRange() {
        const bool note_length = delay_form_.getSelectedId() == 1;
        delay_.setRange(
            0.0,
            note_length ? 192.0 : 256.0,
            1.0);
        delay_.setTextValueSuffix(note_length ? "" : "");
    }

    void updateMicroDetuneRange(mgstc::engine::TimbreSource source) {
        if (source == mgstc::engine::TimbreSource::Opll) {
            micro_detune_.setRange(0.0, 255.0, 1.0);
        } else {
            micro_detune_.setRange(-32768.0, 32767.0, 1.0);
        }
    }

    void syncTimbreNumberEnabled() {
        const bool can_assign = number_mode_.isEnabled();
        const bool manual = number_mode_.getSelectedId() == 2;
        timbre_number_.setEnabled(can_assign && manual);
        timbre_number_label_.setEnabled(can_assign && manual);
    }

    void configureValueSlider(
        juce::Slider& slider,
        double minimum,
        double maximum,
        const juce::String& suffix) {
        slider.setRange(minimum, maximum, 1.0);
        slider.setSliderStyle(juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle(
            juce::Slider::TextBoxLeft, false, 40, UiLayout::compositeToolRowH);
        slider.setTextValueSuffix(suffix);
        slider.onValueChange = [this] {
            if (!assigning_ && changed_) {
                changed_();
            }
        };
        addAndMakeVisible(slider);
    }

    static void layoutValue(
        juce::Rectangle<int> area,
        juce::Label& label,
        juce::Slider& slider,
        int label_w = UiLayout::editSubLaneLabelW) {
        label.setBounds(area.removeFromLeft(label_w));
        slider.setBounds(area);
    }

    ChangedCallback changed_;
    AssignCallback assign_;
    EditCallback edit_callback_;
    std::vector<std::uint64_t> library_ids_;
    int rom_item_base_{};
    int library_item_base_{1};
    bool assigning_{};
    juce::Label source_;
    juce::ToggleButton enabled_;
    juce::ToggleButton mute_;
    juce::ToggleButton solo_;
    juce::TextEditor name_;
    juce::ComboBox timbre_;
    juce::ComboBox channel_;
    juce::ComboBox number_mode_;
    juce::Label timbre_number_label_;
    juce::Slider timbre_number_;
    juce::TextButton edit_;
    juce::Label pitch_label_;
    juce::Label detune_label_;
    juce::Label micro_detune_label_;
    juce::Label delay_label_;
    juce::Label volume_label_;
    juce::ComboBox delay_form_;
    juce::Slider pitch_;
    juce::Slider detune_;
    juce::Slider micro_detune_;
    juce::Slider delay_;
    juce::Slider volume_;
};

class CountCommandStackView final
    : public juce::Component,
      public juce::SettableTooltipClient {
public:
    using DeleteCallback = std::function<void(int chip_index)>;

    void setDeleteCallback(DeleteCallback callback) {
        delete_callback_ = std::move(callback);
    }

    void clearStack() {
        count_ = -1;
        has_loop_start_ = false;
        has_loop_end_ = false;
        before_.clear();
        after_end_.clear();
        repaint();
    }

    void setStack(
        int count,
        bool has_loop_start,
        bool has_loop_end,
        std::vector<juce::String> before,
        std::vector<juce::String> after_end) {
        count_ = count;
        has_loop_start_ = has_loop_start;
        has_loop_end_ = has_loop_end;
        before_ = std::move(before);
        after_end_ = std::move(after_end);
        setTooltip(juce::String::fromUTF8(
            "選択中カウントのゼロ時間命令。右クリックでその命令を削除。"
            "[ より前は初回のみ、後はループのたびに実行。"
            "この版では前後配置は未対応で、既存命令はループ前ゾーンに表示します。"));
        repaint();
    }

    void mouseDown(const juce::MouseEvent& event) override {
        if (!event.mods.isPopupMenu() || delete_callback_ == nullptr) {
            return;
        }
        const int index = hitChipIndex(event.getPosition());
        if (index >= 0) {
            delete_callback_(index);
        }
    }

    void paint(juce::Graphics& graphics) override {
        auto bounds = getLocalBounds().toFloat();
        graphics.setColour(juce::Colour(0xFF26313B));
        graphics.fillRoundedRectangle(bounds, 5.0F);
        graphics.setColour(juce::Colour(0xFF435160));
        graphics.drawRoundedRectangle(bounds, 5.0F, 1.0F);

        auto area = getLocalBounds().reduced(UiLayout::sm, UiLayout::xs);
        graphics.setColour(juce::Colour(0xFFE6EDF3));
        graphics.setFont(UiFonts::dense(true));
        auto header = area.removeFromTop(16);
        if (count_ < 0) {
            graphics.drawText(
                juce::String::fromUTF8("チャンネルを選択すると命令スタックを表示"),
                header,
                juce::Justification::centredLeft,
                false);
            return;
        }
        graphics.drawText(
            juce::String::fromUTF8("カウント ")
                + juce::String(count_)
                + juce::String::fromUTF8(" の命令"),
            header,
            juce::Justification::centredLeft,
            false);

        drawZone(
            graphics,
            area.removeFromTop(20),
            juce::String::fromUTF8("ループ前"),
            before_,
            juce::Colour(kUiHoverAccent),
            false);
        drawDivider(
            graphics,
            area.removeFromTop(16),
            has_loop_start_
                ? juce::String::fromUTF8("[  ループ開始")
                : juce::String::fromUTF8("[  なし（この列）"),
            has_loop_start_);
        drawZone(
            graphics,
            area.removeFromTop(20),
            juce::String::fromUTF8("ループ後"),
            {},
            juce::Colour(kUiHoverAccent),
            !has_loop_start_);
        drawDivider(
            graphics,
            area.removeFromTop(16),
            has_loop_end_
                ? juce::String::fromUTF8("]  ループ終了")
                : juce::String::fromUTF8("]  なし（この列）"),
            has_loop_end_);
        drawZone(
            graphics,
            area,
            juce::String::fromUTF8("]以降"),
            after_end_,
            juce::Colour(0xFF8896A3),
            true);
    }

private:
    void drawDivider(
        juce::Graphics& graphics,
        juce::Rectangle<int> row,
        const juce::String& text,
        bool active) const {
        graphics.setColour(
            active
                ? juce::Colour(kUiHoverAccent).withAlpha(0.35F)
                : juce::Colour(0xFF40505E));
        graphics.fillRect(row.withHeight(1).withY(row.getCentreY()));
        graphics.setFont(UiFonts::dense(true));
        graphics.setColour(
            active ? juce::Colour(kUiHoverAccent) : juce::Colour(0xFF8896A3));
        graphics.drawText(
            text, row.reduced(UiLayout::xs, 0),
            juce::Justification::centredLeft, false);
    }

    void drawZone(
        juce::Graphics& graphics,
        juce::Rectangle<int> row,
        const juce::String& title,
        const std::vector<juce::String>& chips,
        juce::Colour chip_colour,
        bool dimmed) const {
        auto label = row.removeFromLeft(64);
        graphics.setFont(UiFonts::dense());
        graphics.setColour(
            dimmed ? juce::Colour(0xFF6B7785) : juce::Colour(0xFFC9D1D9));
        graphics.drawText(
            title, label, juce::Justification::centredLeft, false);
        if (chips.empty()) {
            graphics.setColour(juce::Colour(0xFF6B7785));
            graphics.drawText(
                juce::String::fromUTF8("（なし）"),
                row,
                juce::Justification::centredLeft,
                false);
            return;
        }
        int x = row.getX();
        graphics.setFont(UiFonts::dense(true));
        for (const auto& chip : chips) {
            const int width = juce::jmax(28, chip.length() * 8 + UiLayout::sm);
            if (x + width > row.getRight()) {
                break;
            }
            auto chip_bounds = juce::Rectangle<int>(
                x, row.getY() + 1, width - 2, row.getHeight() - 2);
            graphics.setColour(chip_colour.withAlpha(dimmed ? 0.18F : 0.28F));
            graphics.fillRoundedRectangle(chip_bounds.toFloat(), 3.0F);
            graphics.setColour(
                dimmed ? juce::Colour(0xFF8896A3) : juce::Colour(0xFFE6EDF3));
            graphics.drawText(
                chip, chip_bounds, juce::Justification::centred, false);
            x += width;
        }
    }

    [[nodiscard]] int hitChipIndex(juce::Point<int> point) const {
        auto area = getLocalBounds().reduced(UiLayout::sm, UiLayout::xs);
        area.removeFromTop(16);  // header
        auto before_row = area.removeFromTop(20);
        before_row.removeFromLeft(64);
        int x = before_row.getX();
        for (int index = 0; index < static_cast<int>(before_.size()); ++index) {
            const auto& chip = before_[static_cast<std::size_t>(index)];
            const int width = juce::jmax(28, chip.length() * 8 + UiLayout::sm);
            auto chip_bounds = juce::Rectangle<int>(
                x, before_row.getY() + 1, width - 2, before_row.getHeight() - 2);
            if (chip_bounds.contains(point)) {
                return index;
            }
            x += width;
            if (x > before_row.getRight()) {
                break;
            }
        }
        return -1;
    }

    DeleteCallback delete_callback_;
    int count_{-1};
    bool has_loop_start_{};
    bool has_loop_end_{};
    std::vector<juce::String> before_;
    std::vector<juce::String> after_end_;
};

class CompositeTimeline final
    : public juce::Component,
      private juce::ScrollBar::Listener {
public:
    using EditCallback = std::function<void(
        const mgstc::engine::CompositeTimbre&, bool)>;
    using StatusCallback = std::function<void(const juce::String&)>;
    using CatalogCallback = std::function<
        std::vector<EnvelopeTimbreCatalogItem>(mgstc::engine::TimbreSource)>;
    using OpenTimbreCallback = std::function<void(
        const juce::String&, std::optional<std::uint64_t>)>;
    using AssignTimbreCallback = std::function<void(
        std::size_t, LayerBaseTimbreAssign)>;
    using TimbreNameCallback = std::function<juce::String(std::uint64_t)>;
    using TimbreLibraryCallback =
        std::function<const mgstc::engine::TimbreLibrary*()>;

    CompositeTimeline()
        : horizontal_scroll_(false), vertical_scroll_(true) {
        parameter_.addItem(juce::String::fromUTF8("音量"), 1);
        parameter_.addItem(juce::String::fromUTF8("音程"), 2);
        parameter_.addItem(juce::String::fromUTF8("@音色"), 3);
        parameter_.addItem(juce::String::fromUTF8("TL自動"), 4);
        parameter_.addItem(juce::String::fromUTF8("FB自動"), 5);
        parameter_.setSelectedId(1, juce::dontSendNotification);
        parameter_.setTooltip(
            juce::String::fromUTF8(
                "時間軸へ描画するパラメーター。TL/FB自動はOPLL専用（手動yとは別）"));
        parameter_.onChange = [this] {
            syncPointEditors();
            syncRegisterAutoEditors();
            syncInspector();
            repaint();
        };
        addAndMakeVisible(parameter_);

        auto_mode_.addItem(juce::String::fromUTF8("オフ"), 1);
        auto_mode_.addItem(juce::String::fromUTF8("上昇"), 2);
        auto_mode_.addItem(juce::String::fromUTF8("下降"), 3);
        auto_mode_.addItem("LFO", 4);
        auto_mode_.addItem(juce::String::fromUTF8("自由曲線"), 5);
        auto_mode_.setSelectedId(1, juce::dontSendNotification);
        auto_mode_.setTooltip(juce::String::fromUTF8(
            "TL/FB自動の時間変化モード。出力は展開したyレジスタ書き込み"));
        auto_mode_.onChange = [this] {
            applyRegisterAutoFromEditors(false);
        };
        addAndMakeVisible(auto_mode_);

        configureAutoParamEditor(
            auto_depth_, juce::String::fromUTF8("深さ（開始値またはLFO振幅）"));
        configureAutoParamEditor(
            auto_speed_, juce::String::fromUTF8("変化量／変化スピード"));
        configureAutoParamEditor(
            auto_coarseness_,
            juce::String::fromUTF8("粗さ（何カウントごとに書き込むか）"));
        configureAutoParamEditor(
            auto_stop_, juce::String::fromUTF8("停止位置（到達値／LFO中心）"));
        auto_depth_label_.setText(
            juce::String::fromUTF8("深さ"), juce::dontSendNotification);
        auto_speed_label_.setText(
            juce::String::fromUTF8("変化"), juce::dontSendNotification);
        auto_coarseness_label_.setText(
            juce::String::fromUTF8("粗さ"), juce::dontSendNotification);
        auto_stop_label_.setText(
            juce::String::fromUTF8("停止"), juce::dontSendNotification);
        for (auto* label : {
                 &auto_depth_label_, &auto_speed_label_,
                 &auto_coarseness_label_, &auto_stop_label_}) {
            label->setJustificationType(juce::Justification::centredRight);
            addAndMakeVisible(*label);
        }

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
            // Defer so the button release paints before the modal builds.
            juce::MessageManager::callAsync(
                [safe = juce::Component::SafePointer<CompositeTimeline>(this)] {
                    if (safe != nullptr) {
                        safe->requestRemoveSelectedLayer();
                    }
                });
        };
        addAndMakeVisible(remove_layer_);

        tempo_label_.setText(
            juce::String::fromUTF8("テンポ"),
            juce::dontSendNotification);
        tempo_label_.setFont(UiFonts::body());
        tempo_label_.setJustificationType(juce::Justification::centredRight);
        tempo_label_.setTooltip(juce::String::fromUTF8(
            "この複合音色の演奏テンポ（MGSC #tempo 57～2047）。ライブラリ保存対象"));
        addAndMakeVisible(tempo_label_);
        UiFonts::styleBodyField(tempo_);
        tempo_.setInputRestrictions(4, "0123456789");
        tempo_.setJustification(juce::Justification::centred);
        tempo_.setText(
            juce::String(playback_tempo_),
            juce::dontSendNotification);
        tempo_.setTooltip(tempo_label_.getTooltip());
        tempo_.onReturnKey = [this] { commitPlaybackTempoFromEditor(); };
        tempo_.onFocusLost = [this] { commitPlaybackTempoFromEditor(); };
        addAndMakeVisible(tempo_);

        edit_mode_.addItem(juce::String::fromUTF8("描画"), 1);
        edit_mode_.addItem(juce::String::fromUTF8("終端"), 2);
        edit_mode_.addItem(juce::String::fromUTF8("L開始"), 3);
        edit_mode_.addItem(juce::String::fromUTF8("L終了"), 4);
        edit_mode_.setSelectedId(1, juce::dontSendNotification);
        edit_mode_.setTooltip(juce::String::fromUTF8(
            "編集モード: 描画／終端／ループ開始(L開始)／ループ終了(L終了)"));
        addAndMakeVisible(edit_mode_);

        value_.setInputRestrictions(5, "-0123456789");
        value_.setText("15", false);
        value_.setTooltip(
            juce::String::fromUTF8("選択パラメーターの値"));
        UiFonts::styleBodyField(value_);
        addAndMakeVisible(value_);
        position_.setText("ct 0", juce::dontSendNotification);
        position_.setJustificationType(juce::Justification::centredRight);
        position_.setTooltip(juce::String::fromUTF8(
            "ホバーまたはクリック中のカウント（数値入力では変更しません）"));
        value_label_.setText(
            juce::String::fromUTF8("値"), juce::dontSendNotification);
        value_label_.setJustificationType(juce::Justification::centredRight);
        addAndMakeVisible(position_);
        addAndMakeVisible(value_label_);
        apply_.setButtonText(juce::String::fromUTF8("設定"));
        apply_.setTooltip(
            juce::String::fromUTF8(
                "表示中のカウントへ入力値を設定します。"
                "TL/FB自動では開始ステップとパラメーターを確定します"));
        apply_.onClick = [this] {
            if (selectedEditMode() != EditMode::Draw) {
                return;
            }
            if (isRegisterAutoParameter(selectedParameter())) {
                applyRegisterAutoFromEditors(true);
                return;
            }
            if (selectedParameter() == Parameter::Timbre) {
                pickTimbreAt(selected_count_, false);
                return;
            }
            if (selectedParameter() == Parameter::Pitch) {
                addPitchCommand(
                    selected_count_, value_.getText().getIntValue(), true);
                return;
            }
            editPoint(selected_count_, value_.getText().getIntValue(), true);
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

        command_stack_.setInterceptsMouseClicks(true, false);
        command_stack_.setDeleteCallback([this](int chip_index) {
            deleteStackChipAt(chip_index);
        });
        addAndMakeVisible(command_stack_);

        envelope_mml_preview_.setMultiLine(true);
        envelope_mml_preview_.setReadOnly(true);
        envelope_mml_preview_.setWantsKeyboardFocus(false);
        envelope_mml_preview_.setScrollbarsShown(true);
        envelope_mml_preview_.setFont(UiFonts::mono());
        envelope_mml_preview_.setColour(
            juce::TextEditor::backgroundColourId,
            juce::Colour(0xFF151A20));
        envelope_mml_preview_.setColour(
            juce::TextEditor::textColourId,
            juce::Colour(0xFF9BE9C3));
        envelope_mml_preview_.setColour(
            juce::TextEditor::outlineColourId,
            juce::Colour(0xFF40505E));
        envelope_mml_preview_.setColour(
            juce::TextEditor::focusedOutlineColourId,
            juce::Colour(kUiHoverAccent));
        envelope_mml_preview_.setTooltip(juce::String::fromUTF8(
            "上段=@e定義、下段=発音前トラックMML。"
            "マウスで範囲選択して Ctrl+C でコピーできます"));
        addAndMakeVisible(envelope_mml_preview_);

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

    void setStatusCallback(StatusCallback callback) {
        status_callback_ = std::move(callback);
    }

    void setTimbreCatalogCallback(CatalogCallback callback) {
        catalog_callback_ = std::move(callback);
    }

    void setOpenTimbreCallback(OpenTimbreCallback callback) {
        open_timbre_callback_ = std::move(callback);
    }

    void setAssignTimbreCallback(AssignTimbreCallback callback) {
        assign_timbre_callback_ = std::move(callback);
    }

    void setTimbreNameCallback(TimbreNameCallback callback) {
        timbre_name_callback_ = std::move(callback);
    }

    void setTimbreLibraryCallback(TimbreLibraryCallback callback) {
        timbre_library_callback_ = std::move(callback);
    }

    void setPlaybackTempo(int tempo_bpm) {
        playback_tempo_ = juce::jlimit(
            mgstc::engine::kMgscTempoMin,
            mgstc::engine::kMgscTempoMax,
            tempo_bpm);
        timbre_.playback_tempo = playback_tempo_;
        tempo_.setText(
            juce::String(playback_tempo_), juce::dontSendNotification);
        repaint();
    }

    void setTimbre(const mgstc::engine::CompositeTimbre& timbre) {
        timbre_ = timbre;
        static_cast<void>(
            mgstc::engine::enforceOpllRegisterAutoExclusivity(timbre_));
        playback_tempo_ = juce::jlimit(
            mgstc::engine::kMgscTempoMin,
            mgstc::engine::kMgscTempoMax,
            timbre_.playback_tempo);
        timbre_.playback_tempo = playback_tempo_;
        tempo_.setText(
            juce::String(playback_tempo_), juce::dontSendNotification);
        clampAllLayersToBodyLimit();
        selected_layer_ = timbre_.layers.empty()
            ? -1
            : juce::jlimit(
                  0,
                  static_cast<int>(timbre_.layers.size()) - 1,
                  selected_layer_);
        refreshCountCeilingCache();
        updateScrollRanges();
        updateAddButtons();
        rebuildLayerSetups();
        syncPointEditors();
        syncRegisterAutoEditors();
        syncInspector();
        resized();
        repaint();
    }

    void refreshUiScaleFonts() {
        tempo_label_.setFont(UiFonts::body());
        UiFonts::styleBodyField(tempo_);
        UiFonts::styleBodyField(value_);
        UiFonts::refreshMgscPreviewFont(envelope_mml_preview_);
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
    }

    void resized() override {
        using namespace UiLayout;
        auto area = getLocalBounds().reduced(panelPad);
        // Single chrome row: help+tempo (paint) and channel add/remove.
        auto header = area.removeFromTop(compositeToolRowH);
        tempo_.setBounds(header.removeFromRight(UiScale::sx(56)));
        tempo_label_.setBounds(header.removeFromRight(UiScale::sx(44)));
        header.removeFromRight(sm);
        opll_add_.setBounds(header.removeFromRight(UiScale::sx(76)));
        header.removeFromRight(controlGap);
        scc_add_.setBounds(header.removeFromRight(UiScale::sx(70)));
        header.removeFromRight(controlGap);
        psg_add_.setBounds(header.removeFromRight(UiScale::sx(70)));
        header.removeFromRight(sm);
        remove_layer_.setBounds(header.removeFromRight(UiScale::sx(92)));

        auto graph = graphArea();
        vertical_scroll_.setBounds(
            graph.getRight(), graph.getY(), kScrollBarSize, graph.getHeight());
        horizontal_scroll_.setBounds(
            graph.getX(), graph.getBottom(), graph.getWidth(), kScrollBarSize);
        updateScrollRanges();
        applyCountColumnWidth();
        layoutLayerSetups();
        layoutEditToolsDock();
    }

    void mouseDown(const juce::MouseEvent& event) override {
        if (event.mods.isPopupMenu()) {
            deleteCommandAt(event.getPosition());
            return;
        }
        const int layer = layerAt(event.getPosition());
        if (layer < 0) {
            return;
        }
        const bool selection_changed = selected_layer_ != layer;
        selected_layer_ = layer;
        const auto graph = graph_bounds_[static_cast<std::size_t>(layer)];
        const bool in_graph = graph.contains(event.getPosition());
        if (in_graph) {
            auto count_bounds = timelinePlotBounds(graph);
            selected_count_ = countAtX(count_bounds, event.getPosition().x);
            position_.setText(
                "ct " + juce::String(selected_count_),
                juce::dontSendNotification);
        }
        bool edit_tier = false;
        if (!selection_changed && in_graph) {
            edit_tier = hitEditLane(
                timbre_.layers[static_cast<std::size_t>(layer)],
                graph,
                event.getPosition());
        } else {
            edit_draw_bounds_.reset();
        }
        syncPointEditors();
        syncRegisterAutoEditors();
        syncInspector();
        if (selection_changed) {
            refreshCountCeilingCache();
            updateScrollRanges();
            ensureSelectedLaneVisible();
            drawing_ = false;
            resized();
            repaint();
            return;
        }
        if (selectedEditMode() == EditMode::Draw) {
            if (!in_graph) {
                drawing_ = false;
            } else if (edit_tier) {
                drawing_ = edit_draw_bounds_.has_value();
                if (drawing_) {
                    editFromMouse(event, false);
                }
            } else {
                // Selected edit-tier uses sublane bounds only; never map
                // Y against the full lane (wrong volume/pitch values).
                drawing_ = false;
            }
        } else if (in_graph) {
            setTimelineMarker(selected_count_, true);
        }
        repaint();
    }

    void mouseDrag(const juce::MouseEvent& event) override {
        if (drawing_) {
            editFromMouse(event, false);
        }
    }

    void mouseUp(const juce::MouseEvent& event) override {
        if (!drawing_) {
            return;
        }
        editFromMouse(event, true);
        drawing_ = false;
    }

    void mouseDoubleClick(const juce::MouseEvent& event) override {
        const int layer = layerAt(event.getPosition());
        if (layer < 0
            || layer != selected_layer_
            || layer >= static_cast<int>(graph_bounds_.size())) {
            return;
        }
        const auto& selected =
            timbre_.layers[static_cast<std::size_t>(layer)];
        if (selected.source == mgstc::engine::TimbreSource::Psg) {
            return;
        }
        const auto graph = graph_bounds_[static_cast<std::size_t>(layer)];
        const auto slots = makeEditLaneSlots(graph, selected.source);
        if (slots.timbre.isEmpty() || !slots.timbre.contains(event.getPosition())) {
            return;
        }
        auto count_bounds = graphStrip(slots.timbre);
        selected_count_ = countAtX(count_bounds, event.getPosition().x);
        const auto* existing = timbreEventAt(
            selected, static_cast<std::uint32_t>(selected_count_));
        if (existing != nullptr
            && existing->timbre_pick == mgstc::engine::TimbrePick::Library
            && existing->target_library_id != 0
            && open_timbre_callback_) {
            const auto kind =
                selected.source == mgstc::engine::TimbreSource::Scc
                    ? "scc" : "opll";
            open_timbre_callback_(
                kind, existing->target_library_id);
            return;
        }
        pickTimbreAt(selected_count_, existing != nullptr);
    }

    void mouseMove(const juce::MouseEvent& event) override {
        hover_mouse_pos_ = event.getPosition();
        inspectFromMouse(event.getPosition());
        repaint();
    }

    void mouseExit(const juce::MouseEvent&) override {
        if (drawing_) {
            return;
        }
        hover_preview_value_.reset();
        hover_mouse_pos_.reset();
        setMouseCursor(juce::MouseCursor::CrosshairCursor);
        syncPointEditors();
        repaint();
    }

    void mouseWheelMove(
        const juce::MouseEvent& event,
        const juce::MouseWheelDetails& wheel) override {
        if (event.mods.isCtrlDown()) {
            const double factor = wheel.deltaY > 0.0 ? 0.85 : 1.18;
            zoomCountColumns(event.getPosition().x, factor);
            return;
        }
        const double step = wheel.deltaY != 0.0
            ? static_cast<double>(wheel.deltaY) * 4.0
            : static_cast<double>(wheel.deltaX) * 4.0;
        horizontal_scroll_.setCurrentRangeStart(
            horizontal_scroll_.getCurrentRangeStart() - step,
            juce::dontSendNotification);
        repaint();
    }

    void paint(juce::Graphics& graphics) override {
        graphics.fillAll(juce::Colour(0xFF182028));
        auto area = getLocalBounds().reduced(UiLayout::panelPad);
        auto header = area.removeFromTop(UiLayout::compositeToolRowH);
        // Leave room for tempo + add/remove buttons on the right (resized).
        header.removeFromRight(460);
        graphics.setColour(juce::Colour(0xFFE6EDF3));
        graphics.setFont(UiFonts::body(true));
        graphics.drawText(
            juce::String::fromUTF8(
                "共通時間軸  ホバーで値確認、枠クリックでch切替、ドラッグで編集（Ctrl＋ホイールでズーム）。音程は累積オフセット、\\は複数可（Shift＋ドラッグで同一ctへ追加）"),
            header,
            juce::Justification::centredLeft);
        area = graphArea();
        graphics.reduceClipRegion(area);

        // Keep draw bounds across mouseUp after mouseDown's repaint();
        // clearing here made Y map fall back to the full lane height.
        if (!drawing_) {
            edit_draw_bounds_.reset();
        }
        const std::size_t lane_count = timbre_.layers.size() + 1;
        const int first_lane = juce::jlimit(
            0, juce::jmax(0, static_cast<int>(lane_count) - 1),
            juce::roundToInt(vertical_scroll_.getCurrentRangeStart()));
        const auto numbers = mgstc::engine::resolveTimbreNumbers(timbre_);
            graph_bounds_.assign(timbre_.layers.size(), {});
            lane_frames_.assign(timbre_.layers.size(), {});
            int lane_y = area.getY();
            for (int lane_index = 0;
                 lane_index < static_cast<int>(lane_count);
                 ++lane_index) {
                const int lane_height = laneHeightForIndex(lane_index);
                if (lane_index < first_lane) {
                    continue;
                }
                if (lane_y >= area.getBottom()) {
                    break;
                }
                auto frame = juce::Rectangle<int>(
                    area.getX(),
                    lane_y,
                    area.getWidth(),
                    lane_height).reduced(0, 3);
                frame.setBottom(juce::jmin(frame.getBottom(), area.getBottom()));
                lane_y += lane_height;
                const bool mixed_lane =
                    lane_index == static_cast<int>(timbre_.layers.size());
                const std::size_t index = static_cast<std::size_t>(lane_index);
                const auto colour = mixed_lane
                    ? juce::Colour(0xFF35C4ED)
                    : sourceColour(timbre_.layers[index].source);
                graphics.setColour(juce::Colour(0xFF26313B));
                graphics.fillRoundedRectangle(frame.toFloat(), 5.0F);
                // Veil behind left setup/dock so scrolled envelope cannot show
                // through the controls (same family as frame fill).
                {
                    auto veil = frame;
                    veil.setWidth(UiLayout::compositeLaneLabelW);
                    graphics.setColour(juce::Colour(0xFF26313B).withAlpha(0.92F));
                    graphics.fillRect(veil);
                }
                const bool edit_tier = !mixed_lane
                    && static_cast<int>(index) == selected_layer_;
                if (edit_tier) {
                    const int setup_h =
                        UiLayout::compositeSetupContentH + UiLayout::xs * 2;
                    auto dock = frame;
                    dock.setWidth(UiLayout::compositeLaneLabelW);
                    dock.removeFromTop(setup_h);
                    graphics.setColour(juce::Colour(0xFF1B222A));
                    graphics.fillRect(dock);
                    graphics.setColour(juce::Colour(0xFF8B9BAB));
                    graphics.setFont(UiFonts::dense(true));
                    graphics.drawText(
                        juce::String::fromUTF8("エンベロープ編集"),
                        dock.removeFromTop(16).reduced(6, 0),
                        juce::Justification::centredLeft,
                        false);
                }
                graphics.setColour(
                    !mixed_lane && static_cast<int>(index) == selected_layer_
                        ? colour
                        : colour.withAlpha(0.72F));
                graphics.drawRoundedRectangle(
                    frame.toFloat(), 5.0F,
                    !mixed_lane && static_cast<int>(index) == selected_layer_
                        ? 2.0F : 1.0F);

                auto lane = frame;
                auto label = lane.removeFromLeft(UiLayout::compositeLaneLabelW).reduced(7, 0);
            if (mixed_lane) {
            graphics.setColour(colour);
            graphics.setFont(UiFonts::body(true));
            graphics.drawFittedText(
                juce::String::fromUTF8("MIX / 合成出力"),
                label, juce::Justification::centredLeft, 2);
            }

            lane.reduce(8, 7);
            if (!mixed_lane) {
                lane_frames_[index] = frame;
                graph_bounds_[index] = lane;
            }
            // Same count X for every tier/ch (label gutter excluded).
            const auto plot = timelinePlotBounds(lane);
            drawCountGrid(graphics, plot);
            drawSelectedCountBand(graphics, plot);
            if (!mixed_lane) {
                const auto& layer = timbre_.layers[index];
                // Rest is pre-key-on (not an @e count). Tint the left wait
                // band only; envelope counts still start at ct0.
                const int delay_counts = static_cast<int>(
                    mgstc::engine::startDelayGridCounts(
                        layer.start_delay_form,
                        layer.start_delay_value,
                        playback_tempo_));
                if (delay_counts > 0) {
                    const int start_x = xForCount(plot, delay_counts);
                    const int wait_w = juce::jmax(0, start_x - plot.getX());
                    if (wait_w > 0) {
                        graphics.setColour(juce::Colours::black.withAlpha(
                            mgstc::engine::layerIsAudible(timbre_, index)
                                ? 0.28F : 0.14F));
                        graphics.fillRect(
                            plot.getX(),
                            plot.getY() + 5,
                            wait_w,
                            plot.getHeight() - 10);
                    }
                }
            }
            if (edit_tier) {
                paintEditTier(graphics, lane, index, colour);
                drawHoverValuePopup(graphics);
            } else {
                drawScope(graphics, plot, index, colour.withAlpha(0.48F));
                if (!mixed_lane) {
                    drawTimelineMarkers(graphics, plot, index, colour);
                    // Collapsed lanes always show volume + pitch; do not
                    // follow the edit-tier parameter combo.
                    drawAutomation(
                        graphics, plot, index, colour, Parameter::Volume);
                    drawAutomation(
                        graphics,
                        plot,
                        index,
                        colour.withAlpha(0.75F),
                        Parameter::Pitch);
                }
            }
        }
        juce::ignoreUnused(numbers);
        layoutLayerSetups();
        layoutEditToolsDock();
    }

private:
    enum class Parameter : int {
        Volume = 1,
        Pitch = 2,
        Timbre = 3,
        OpllTlAuto = 4,
        OpllFbAuto = 5,
    };

    enum class EditMode : int {
        Draw = 1,
        End = 2,
        LoopStart = 3,
        LoopEnd = 4,
    };

    static constexpr int kDefaultVisibleCounts = 32;
    static constexpr int kScrollBarSize = 15;
    static constexpr int kPitchCommandMin = -127;
    static constexpr int kPitchCommandMax = 127;

    [[nodiscard]] static bool isRegisterAutoParameter(
        Parameter parameter) noexcept {
        return parameter == Parameter::OpllTlAuto
            || parameter == Parameter::OpllFbAuto;
    }

    [[nodiscard]] bool selectedLayerShowsRegisterAuto() const noexcept {
        return selected_layer_ >= 0
            && selected_layer_ < static_cast<int>(timbre_.layers.size())
            && timbre_.layers[static_cast<std::size_t>(selected_layer_)].source
                == mgstc::engine::TimbreSource::Opll;
    }

    [[nodiscard]] mgstc::engine::OpllRegisterAutoLane* selectedRegisterAutoLane() {
        if (!selectedLayerShowsRegisterAuto()
            || !isRegisterAutoParameter(selectedParameter())) {
            return nullptr;
        }
        auto& layer =
            timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        return selectedParameter() == Parameter::OpllTlAuto
            ? &layer.opll_tl_auto
            : &layer.opll_fb_auto;
    }

    [[nodiscard]] const mgstc::engine::OpllRegisterAutoLane*
    selectedRegisterAutoLane() const {
        return const_cast<CompositeTimeline*>(this)
            ->selectedRegisterAutoLane();
    }

    [[nodiscard]] mgstc::engine::OpllRegisterAutoTarget
    selectedRegisterAutoTarget() const noexcept {
        return selectedParameter() == Parameter::OpllFbAuto
            ? mgstc::engine::OpllRegisterAutoTarget::Feedback
            : mgstc::engine::OpllRegisterAutoTarget::TotalLevel;
    }

    void configureAutoParamEditor(
        juce::TextEditor& editor,
        const juce::String& tooltip) {
        UiFonts::styleBodyField(editor);
        editor.setInputRestrictions(3, "0123456789");
        editor.setJustification(juce::Justification::centred);
        editor.setTooltip(tooltip);
        editor.onReturnKey = [this] {
            applyRegisterAutoFromEditors(true);
        };
        addAndMakeVisible(editor);
    }

    [[nodiscard]] const mgstc::engine::TimbreLibrary* timbreLibrary() const {
        return timbre_library_callback_ != nullptr
            ? timbre_library_callback_()
            : nullptr;
    }

    [[nodiscard]] bool registerAutoAvailableAtSelectedCount() const {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return false;
        }
        const auto& layer =
            timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        if (layer.source != mgstc::engine::TimbreSource::Opll) {
            return false;
        }
        return mgstc::engine::opllRegisterAutoAvailableAt(
            layer, static_cast<std::uint32_t>(juce::jmax(0, selected_count_)));
    }

    void syncRegisterAutoEditors() {
        const auto* lane = selectedRegisterAutoLane();
        const bool selected =
            selected_layer_ >= 0
            && selected_layer_ < static_cast<int>(timbre_.layers.size());
        const bool enabled = lane != nullptr;
        const auto target = selectedRegisterAutoTarget();
        const bool owned = enabled
            && selected
            && mgstc::engine::opllRegisterAutoOwnedByLayer(
                   timbre_,
                   static_cast<std::size_t>(selected_layer_),
                   target);
        const bool count_ok =
            enabled && owned && registerAutoAvailableAtSelectedCount();
        // Other OPLL layers may view Off, but cannot take ownership.
        auto_mode_.setEnabled(enabled && (owned || !lane->active()));
        auto_depth_.setEnabled(enabled && owned);
        auto_speed_.setEnabled(enabled && owned);
        auto_coarseness_.setEnabled(enabled && owned);
        auto_stop_.setEnabled(enabled && owned);
        if (!enabled) {
            return;
        }
        auto_mode_.setSelectedId(
            static_cast<int>(lane->mode) + 1,
            juce::dontSendNotification);
        auto_depth_.setText(juce::String(static_cast<int>(lane->depth)), false);
        auto_speed_.setText(
            juce::String(static_cast<int>(lane->change_speed)), false);
        auto_coarseness_.setText(
            juce::String(static_cast<int>(lane->coarseness)), false);
        auto_stop_.setText(
            juce::String(static_cast<int>(lane->stop_position)), false);
        if (!owned) {
            const auto owner = mgstc::engine::opllRegisterAutoOwnerLayer(
                timbre_, target);
            juce::String owner_label = juce::String::fromUTF8("別のOPLL ch");
            if (owner.has_value()
                && *owner < timbre_.layers.size()) {
                owner_label = juce::String::fromUTF8("OPLL Ch.")
                    + juce::String(
                          static_cast<int>(
                              timbre_.layers[*owner].channel) + 1);
            }
            const auto kind = target
                    == mgstc::engine::OpllRegisterAutoTarget::TotalLevel
                ? juce::String::fromUTF8("TL自動")
                : juce::String::fromUTF8("FB自動");
            auto_mode_.setTooltip(
                kind + juce::String::fromUTF8("はチップ全体で1本のみ。")
                + owner_label
                + juce::String::fromUTF8("が使用中のため、このchでは設定できません"
                                         "（TLとFBを別chへ分けるのは可）"));
        } else if (!count_ok && isRegisterAutoParameter(selectedParameter())) {
            auto_mode_.setTooltip(juce::String::fromUTF8(
                "選択カウントはOPLL ROM音色区間のため、TL/FB自動のyは出力されません"));
        } else {
            auto_mode_.setTooltip(juce::String::fromUTF8(
                "TL/FB自動の時間変化モード。出力は展開したyレジスタ書き込み。"
                "TL・FBはそれぞれOPLL全体で1本（別ch分担可）"));
        }
    }

    void applyRegisterAutoFromEditors(bool set_start_count) {
        auto* lane = selectedRegisterAutoLane();
        if (lane == nullptr
            || selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        const auto target = selectedRegisterAutoTarget();
        const auto layer_index =
            static_cast<std::size_t>(selected_layer_);
        const auto next_mode =
            static_cast<mgstc::engine::OpllRegisterAutoMode>(
                juce::jlimit(0, 4, auto_mode_.getSelectedId() - 1));
        if (next_mode != mgstc::engine::OpllRegisterAutoMode::Off
            && !mgstc::engine::opllRegisterAutoOwnedByLayer(
                   timbre_, layer_index, target)) {
            reportStatus(juce::String::fromUTF8(
                "TL/FB自動はそれぞれOPLL全体で1チャンネルのみ設定できます"
                "（例: 1ch=FB・2ch=TL の分担は可）"));
            syncRegisterAutoEditors();
            return;
        }
        const int maximum = static_cast<int>(
            mgstc::engine::opllRegisterAutoValueMax(target));
        lane->mode = next_mode;
        if (set_start_count) {
            if (!registerAutoAvailableAtSelectedCount()) {
                reportStatus(juce::String::fromUTF8(
                    "ROM音色区間ではTL/FB自動の開始ステップを置けません"));
                syncRegisterAutoEditors();
                return;
            }
            lane->start_count = static_cast<std::uint32_t>(
                juce::jmax(0, selected_count_));
        }
        lane->depth = static_cast<std::uint8_t>(juce::jlimit(
            0, maximum, auto_depth_.getText().getIntValue()));
        lane->change_speed = static_cast<std::uint8_t>(juce::jlimit(
            1, 255, auto_speed_.getText().getIntValue()));
        lane->coarseness = static_cast<std::uint8_t>(juce::jlimit(
            1, 255, auto_coarseness_.getText().getIntValue()));
        lane->stop_position = static_cast<std::uint8_t>(juce::jlimit(
            0, maximum, auto_stop_.getText().getIntValue()));
        if (lane->mode == mgstc::engine::OpllRegisterAutoMode::FreeCurve
            && lane->free_curve.empty()
            && set_start_count) {
            lane->free_curve.push_back(lane->depth);
        }
        static_cast<void>(
            mgstc::engine::enforceOpllRegisterAutoExclusivity(timbre_));
        syncRegisterAutoEditors();
        repaint();
        if (edit_callback_) {
            edit_callback_(timbre_, true);
        }
    }

    void editRegisterAutoFreeCurve(
        int timeline_count,
        int parameter_value,
        bool commit) {
        auto* lane = selectedRegisterAutoLane();
        if (lane == nullptr
            || lane->mode
                != mgstc::engine::OpllRegisterAutoMode::FreeCurve) {
            return;
        }
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        const auto layer_index =
            static_cast<std::size_t>(selected_layer_);
        const auto target = selectedRegisterAutoTarget();
        if (!mgstc::engine::opllRegisterAutoOwnedByLayer(
                timbre_, layer_index, target)) {
            if (commit) {
                reportStatus(juce::String::fromUTF8(
                    "このTL/FB自動は他のOPLLチャンネルが使用中です"));
            }
            return;
        }
        const auto& layer = timbre_.layers[layer_index];
        if (!mgstc::engine::opllRegisterAutoAvailableAt(
                layer,
                static_cast<std::uint32_t>(juce::jmax(0, timeline_count)))) {
            if (commit) {
                reportStatus(juce::String::fromUTF8(
                    "ROM音色区間ではTL/FB自動を描けません"));
            }
            return;
        }
        const int maximum = static_cast<int>(
            mgstc::engine::opllRegisterAutoValueMax(target));
        parameter_value = juce::jlimit(0, maximum, parameter_value);
        timeline_count = juce::jmax(0, timeline_count);
        const auto step = juce::jmax<std::uint8_t>(1, lane->coarseness);
        if (lane->free_curve.empty()) {
            lane->start_count = static_cast<std::uint32_t>(timeline_count);
        }
        if (static_cast<std::uint32_t>(timeline_count) < lane->start_count) {
            lane->start_count = static_cast<std::uint32_t>(timeline_count);
            lane->free_curve.clear();
        }
        const auto offset =
            (static_cast<std::uint32_t>(timeline_count) - lane->start_count)
            / step;
        if (lane->free_curve.size() <= offset) {
            const auto fill = lane->free_curve.empty()
                ? static_cast<std::uint8_t>(parameter_value)
                : lane->free_curve.back();
            lane->free_curve.resize(offset + 1, fill);
        }
        lane->free_curve[offset] =
            static_cast<std::uint8_t>(parameter_value);
        syncPointEditors();
        if (commit && edit_callback_) {
            edit_callback_(timbre_, true);
        }
        repaint();
    }

    [[nodiscard]] int laneHeightForIndex(int lane_index) const noexcept {
        if (lane_index == static_cast<int>(timbre_.layers.size())) {
            return UiLayout::compositeMixLaneH;
        }
        if (lane_index == selected_layer_) {
            if (lane_index >= 0
                && lane_index < static_cast<int>(timbre_.layers.size())
                && timbre_.layers[static_cast<std::size_t>(lane_index)].source
                    == mgstc::engine::TimbreSource::Opll) {
                return UiLayout::compositeEditLaneH
                    + UiLayout::compositeEditLaneOpllExtraH;
            }
            return UiLayout::compositeEditLaneH;
        }
        return UiLayout::compositeChannelLaneH;
    }

    [[nodiscard]] int visibleLaneCount(int graph_height) const noexcept {
        const int lane_count = juce::jmax(
            1, static_cast<int>(timbre_.layers.size()) + 1);
        int used = 0;
        int count = 0;
        for (int index = 0; index < lane_count; ++index) {
            const int next = used + laneHeightForIndex(index);
            if (count > 0 && next > graph_height) {
                break;
            }
            used = next;
            ++count;
            if (used >= graph_height) {
                break;
            }
        }
        return juce::jmax(1, count);
    }

    [[nodiscard]] int maximumCount() const noexcept {
        return cached_count_ceiling_;
    }

    [[nodiscard]] int maximumCountForSelectedLayer() const noexcept {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return cached_count_ceiling_;
        }
        return cached_layer_fit_;
    }

    void refreshCountCeilingCache() {
        std::uint32_t max_fit = 1;
        std::uint32_t selected_fit = 1;
        const auto numbers = mgstc::engine::resolveTimbreNumbers(timbre_);
        const auto* library = timbreLibrary();
        for (std::size_t index = 0; index < timbre_.layers.size(); ++index) {
            const auto& layer = timbre_.layers[index];
            const auto fit = mgstc::engine::maxEnvelopeLengthFittingBodyLimit(
                layer, 255, &numbers, library);
            max_fit = std::max(max_fit, fit);
            max_fit = std::max(max_fit, layer.envelope_timeline.length_counts);
            if (static_cast<int>(index) == selected_layer_) {
                selected_fit = fit;
            }
        }
        cached_count_ceiling_ = static_cast<int>(std::min(
            max_fit, mgstc::engine::kMgscEnvelopeUiLengthCap));
        cached_layer_fit_ = selected_layer_ >= 0
            ? static_cast<int>(std::min(
                  selected_fit, mgstc::engine::kMgscEnvelopeUiLengthCap))
            : cached_count_ceiling_;
    }

    void clampLayerToBodyLimit(mgstc::engine::CompositeLayer& layer) {
        const auto numbers = mgstc::engine::resolveTimbreNumbers(timbre_);
        const auto fit = mgstc::engine::maxEnvelopeLengthFittingBodyLimit(
            layer, 255, &numbers, timbreLibrary());
        auto& timeline = layer.envelope_timeline;
        if (timeline.length_counts > fit) {
            timeline.length_counts = juce::jmax<std::uint32_t>(1, fit);
        }
        if (timeline.loop_start_count
            && *timeline.loop_start_count > timeline.length_counts) {
            timeline.loop_start_count = timeline.length_counts;
        }
        if (timeline.loop_end_count
            && *timeline.loop_end_count > timeline.length_counts) {
            timeline.loop_end_count = timeline.length_counts;
        }
    }

    void clampAllLayersToBodyLimit() {
        for (auto& layer : timbre_.layers) {
            clampLayerToBodyLimit(layer);
        }
        refreshCountCeilingCache();
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
        case Parameter::OpllTlAuto:
            return juce::String::fromUTF8("TL自動");
        case Parameter::OpllFbAuto:
            return juce::String::fromUTF8("FB自動");
        }
        return {};
    }

    void configureInspectorEditor(
        juce::TextEditor& editor,
        const juce::String& tooltip) {
        UiFonts::styleBodyField(editor);
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
            command_stack_.clearStack();
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
        refreshCommandStack(layer);
    }

    void applyInspector() {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        auto& timeline = selectedTimeline(
            timbre_.layers[static_cast<std::size_t>(selected_layer_)]);
        const auto length = static_cast<std::uint32_t>(juce::jlimit(
            1,
            maximumCountForSelectedLayer(),
            length_editor_.getText().getIntValue()));
        auto loop_start = inspectorCount(
            loop_start_editor_, length);
        auto loop_end = inspectorCount(
            loop_end_editor_, length);
        mgstc::engine::setEnvelopeTimelineRange(
            timeline, length, loop_start, loop_end);
        refreshCountCeilingCache();
        syncInspector();
        updateScrollRanges();
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
        return valueRangeFor(selectedParameter());
    }

    [[nodiscard]] static std::pair<int, int> valueRangeFor(
        Parameter parameter) noexcept {
        switch (parameter) {
        case Parameter::Volume:
            return {0, 15};
        case Parameter::Pitch:
            return {kPitchCommandMin, kPitchCommandMax};
        case Parameter::Timbre:
            return {0, 31};
        case Parameter::OpllTlAuto:
            return {0, 63};
        case Parameter::OpllFbAuto:
            return {0, 7};
        }
        return {0, 15};
    }

    [[nodiscard]] const std::vector<mgstc::engine::EnvelopeEvent>&
    selectedEvents(const mgstc::engine::CompositeLayer& layer) const {
        return eventsFor(layer, selectedParameter());
    }

    [[nodiscard]] const std::vector<mgstc::engine::EnvelopeEvent>&
    eventsFor(
        const mgstc::engine::CompositeLayer& layer,
        Parameter parameter) const {
        if (parameter == Parameter::Volume) {
            return layer.volume_envelope.events;
        }
        if (parameter == Parameter::Pitch) {
            return layer.pitch_envelope.events;
        }
        return layer.timbre_automation;
    }

    [[nodiscard]] mgstc::engine::EnvelopeEventKind selectedEventKind() const {
        return eventKindFor(selectedParameter());
    }

    [[nodiscard]] static mgstc::engine::EnvelopeEventKind eventKindFor(
        Parameter parameter) noexcept {
        if (parameter == Parameter::Volume) {
            return mgstc::engine::EnvelopeEventKind::Volume;
        }
        if (parameter == Parameter::Pitch) {
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
        refreshCountCeilingCache();
        updateScrollRanges();
        vertical_scroll_.setCurrentRangeStart(
            static_cast<double>(selected_layer_),
            juce::dontSendNotification);
        updateAddButtons();
        rebuildLayerSetups();
        syncPointEditors();
        syncRegisterAutoEditors();
        syncInspector();
        resized();
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
                .withIconType(juce::MessageBoxIconType::QuestionIcon)
                .withTitle(juce::String::fromUTF8("チャンネル削除"))
                .withMessage(
                    layer_name
                    + juce::String::fromUTF8(
                        " とそのエンベロープを削除しますか？"))
                .withButton(juce::String::fromUTF8("削除"))
                .withButton(juce::String::fromUTF8("キャンセル")),
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
        refreshCountCeilingCache();
        updateScrollRanges();
        vertical_scroll_.setCurrentRangeStart(
            static_cast<double>(juce::jmax(0, selected_layer_)),
            juce::dontSendNotification);
        updateAddButtons();
        rebuildLayerSetups();
        syncPointEditors();
        syncRegisterAutoEditors();
        syncInspector();
        resized();
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

    void editFromMouse(const juce::MouseEvent& event, bool commit) {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(graph_bounds_.size())) {
            return;
        }
        const auto point = event.getPosition();
        const auto bounds = edit_draw_bounds_.value_or(
            timelinePlotBounds(
                graph_bounds_[static_cast<std::size_t>(selected_layer_)]));
        if (bounds.isEmpty()) {
            return;
        }
        const auto parameter = selectedParameter();
        if (parameter == Parameter::Timbre) {
            return;
        }
        const auto& layer =
            timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        const int count = countAtX(bounds, point.x);
        if (isRegisterAutoParameter(parameter)) {
            const auto [minimum, maximum] = valueRangeFor(parameter);
            editRegisterAutoFreeCurve(
                count,
                valueFromY(bounds, point.y, minimum, maximum),
                commit);
            return;
        }
        if (parameter == Parameter::Pitch) {
            const auto range = pitchDisplayRange(layer);
            editPitchCumulative(
                count,
                valueFromY(bounds, point.y, range.first, range.second),
                commit,
                event.mods.isShiftDown());
            return;
        }
        editPoint(
            count,
            valueFromY(bounds, point.y, 0, 15),
            commit);
    }

    [[nodiscard]] static int valueFromY(
        juce::Rectangle<int> bounds,
        int y,
        int minimum,
        int maximum) noexcept {
        if (bounds.isEmpty()) {
            return minimum;
        }
        return juce::jlimit(
            minimum, maximum,
            juce::roundToInt(
                static_cast<double>(bounds.getBottom() - y)
                * (maximum - minimum) / juce::jmax(1, bounds.getHeight())
                + minimum));
    }

    [[nodiscard]] static int valueFromY(
        juce::Rectangle<int> bounds,
        int y,
        Parameter parameter) noexcept {
        const auto [minimum, maximum] = valueRangeFor(parameter);
        return valueFromY(bounds, y, minimum, maximum);
    }

    [[nodiscard]] int storedValueAt(
        const mgstc::engine::CompositeLayer& layer,
        Parameter parameter,
        int timeline_count) const {
        if (isRegisterAutoParameter(parameter)) {
            const auto& auto_lane = parameter == Parameter::OpllTlAuto
                ? layer.opll_tl_auto
                : layer.opll_fb_auto;
            const auto target = parameter == Parameter::OpllTlAuto
                ? mgstc::engine::OpllRegisterAutoTarget::TotalLevel
                : mgstc::engine::OpllRegisterAutoTarget::Feedback;
            if (const auto value = mgstc::engine::opllRegisterAutoValueAt(
                    auto_lane,
                    target,
                    static_cast<std::uint32_t>(juce::jmax(0, timeline_count)),
                    layer.envelope_timeline.length_counts)) {
                return *value;
            }
            return 0;
        }
        if (parameter == Parameter::Pitch) {
            return cumulativePitchAt(layer, timeline_count);
        }
        const auto& events = eventsFor(layer, parameter);
        const auto kind = eventKindFor(parameter);
        int current = parameter == Parameter::Volume ? layer.volume : 0;
        for (const auto& event : events) {
            if (event.kind == kind
                && event.count <= static_cast<std::uint32_t>(timeline_count)) {
                current = event.value;
            }
        }
        const auto [minimum, maximum] = valueRangeFor(parameter);
        return juce::jlimit(minimum, maximum, current);
    }

    [[nodiscard]] int lastPitchCommandAt(
        const mgstc::engine::CompositeLayer& layer,
        int timeline_count) const {
        int value = 0;
        bool found = false;
        for (const auto& event : layer.pitch_envelope.events) {
            if (event.kind == mgstc::engine::EnvelopeEventKind::Pitch
                && event.count == static_cast<std::uint32_t>(timeline_count)) {
                value = event.value;
                found = true;
            }
        }
        juce::ignoreUnused(found);
        return value;
    }

    [[nodiscard]] int layerAt(juce::Point<int> point) const noexcept {
        for (std::size_t index = 0; index < lane_frames_.size(); ++index) {
            if (lane_frames_[index].contains(point)) {
                return static_cast<int>(index);
            }
        }
        return -1;
    }

    void inspectFromMouse(juce::Point<int> point) {
        if (drawing_) {
            return;
        }
        const int layer = layerAt(point);
        if (layer < 0) {
            hover_preview_value_.reset();
            setMouseCursor(juce::MouseCursor::CrosshairCursor);
            return;
        }
        if (layer != selected_layer_) {
            hover_preview_value_.reset();
            setMouseCursor(juce::MouseCursor::PointingHandCursor);
            return;
        }
        setMouseCursor(juce::MouseCursor::CrosshairCursor);
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())
            || selected_layer_ >= static_cast<int>(graph_bounds_.size())) {
            return;
        }
        const auto graph =
            graph_bounds_[static_cast<std::size_t>(selected_layer_)];
        const auto& selected =
            timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        const bool in_graph = graph.contains(point);
        const int previous_count = selected_count_;
        bool parameter_changed = false;
        std::optional<int> mapped_value;
        auto count_bounds = timelinePlotBounds(graph);
        if (in_graph) {
            const auto slots = makeEditLaneSlots(graph, selected.source);
            const struct LaneHit {
                Parameter parameter;
                juce::Rectangle<int> slot;
                bool drawable;
            } hits[] = {
                {Parameter::Volume, slots.volume, true},
                {Parameter::Pitch, slots.pitch, true},
                {Parameter::Timbre, slots.timbre, false},
                {Parameter::Timbre, slots.register_write, false},
                {Parameter::OpllTlAuto, slots.tl_auto, true},
                {Parameter::OpllFbAuto, slots.fb_auto, true},
            };
            for (const auto& hit : hits) {
                if (hit.slot.isEmpty() || !hit.slot.contains(point)) {
                    continue;
                }
                if (hit.drawable) {
                    if (selectedParameter() != hit.parameter) {
                        parameter_.setSelectedId(
                            static_cast<int>(hit.parameter),
                            juce::dontSendNotification);
                        syncRegisterAutoEditors();
                        parameter_changed = true;
                    }
                    if (hit.parameter == Parameter::Pitch) {
                        const auto range = pitchDisplayRange(selected);
                        mapped_value = valueFromY(
                            graphStrip(hit.slot),
                            point.y,
                            range.first,
                            range.second);
                    } else {
                        mapped_value = valueFromY(
                            graphStrip(hit.slot), point.y, hit.parameter);
                    }
                }
                break;
            }
            selected_count_ = countAtX(count_bounds, point.x);
        }
        const int shown = mapped_value.value_or(
            storedValueAt(selected, selectedParameter(), selected_count_));
        const bool count_changed = selected_count_ != previous_count;
        hover_preview_value_ = mapped_value;
        position_.setText(
            "ct " + juce::String(selected_count_),
            juce::dontSendNotification);
        if (selectedParameter() == Parameter::Timbre && !mapped_value) {
            value_.setText(
                timbreInspectLabel(selected, selected_count_), false);
        } else {
            value_.setText(juce::String(shown), false);
        }
        if (selectedParameter() == Parameter::Pitch) {
            value_.setTooltip(
                mapped_value
                    ? juce::String::fromUTF8(
                          "ホバー位置の累積オフセット（未確定）。"
                          "「設定」は1個の\\命令を追加します")
                    : juce::String::fromUTF8(
                          "元音域からの累積オフセット。"
                          "「設定」は入力値を1個の\\として追加。"
                          "Shift＋ドラッグでも同一カウントへ追加"));
        } else if (isRegisterAutoParameter(selectedParameter())) {
            const bool available = mgstc::engine::opllRegisterAutoAvailableAt(
                selected,
                static_cast<std::uint32_t>(juce::jmax(0, selected_count_)));
            value_.setTooltip(
                available
                    ? juce::String::fromUTF8(
                          "TL/FB自動。展開時はアクティブなオリジナル音色の"
                          "レジスタ2/3をベースにフィールドだけ書き換えます")
                    : juce::String::fromUTF8(
                          "ROM音色区間ではTL/FB自動は使えません（yを出力しません）"));
            if (!available && (count_changed || parameter_changed)) {
                reportStatus(juce::String::fromUTF8(
                    "ROM音色区間 — TL/FB自動は無効"));
            }
        } else {
            value_.setTooltip(
                mapped_value
                    ? juce::String::fromUTF8(
                          "ホバー位置の値（未確定）。クリックまたは「設定」で反映")
                    : juce::String::fromUTF8("選択パラメーターの値"));
        }
        if (count_changed || parameter_changed) {
            refreshCommandStack(selected);
            if (parameter_changed) {
                syncInspector();
            }
            repaint();
        }
    }

    void editPoint(int timeline_count, int parameter_value, bool commit) {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        if (selectedParameter() == Parameter::Pitch) {
            addPitchCommand(timeline_count, parameter_value, commit);
            return;
        }
        if (selectedParameter() == Parameter::Timbre) {
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
        notifyLayerEdit(timeline_count, parameter_value, commit);
    }

    void syncPointEditors() {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        const auto& layer = timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        position_.setText(
            "ct " + juce::String(selected_count_),
            juce::dontSendNotification);
        if (selectedParameter() == Parameter::Pitch) {
            value_.setText(
                juce::String(lastPitchCommandAt(layer, selected_count_)),
                false);
            value_.setTooltip(juce::String::fromUTF8(
                "次に追加する\\のオペランド（-127～127）。"
                "グラフのYは累積オフセットです"));
        } else if (selectedParameter() == Parameter::Timbre) {
            value_.setText(
                timbreInspectLabel(layer, selected_count_),
                false);
            value_.setTooltip(juce::String::fromUTF8(
                "このカウントの@切替先。ライブラリから選びます"));
        } else {
            value_.setText(
                juce::String(
                    storedValueAt(layer, selectedParameter(), selected_count_)),
                false);
            value_.setTooltip(juce::String::fromUTF8("選択パラメーターの値"));
        }
        hover_preview_value_.reset();
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
        case EditMode::End: {
            timeline.length_counts = juce::jmax<std::uint32_t>(1, count);
            const auto fit = static_cast<std::uint32_t>(
                maximumCountForSelectedLayer());
            if (timeline.length_counts > fit) {
                timeline.length_counts = fit;
                reportStatus(
                    juce::String::fromUTF8(
                        "@e は約255文字までです。END を ")
                    + juce::String(static_cast<int>(fit))
                    + juce::String::fromUTF8(" に制限しました"));
            }
            if (timeline.loop_start_count
                && *timeline.loop_start_count > timeline.length_counts) {
                timeline.loop_start_count = timeline.length_counts;
            }
            if (timeline.loop_end_count
                && *timeline.loop_end_count > timeline.length_counts) {
                timeline.loop_end_count = timeline.length_counts;
            }
            break;
        }
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
        refreshCountCeilingCache();
        syncInspector();
        updateScrollRanges();
        repaint();
        if (edit_callback_) {
            edit_callback_(timbre_, commit);
        }
    }

    [[nodiscard]] int timelineChromeHeight() const noexcept {
        using namespace UiLayout;
        // Add/help/tempo row only; envelope edit tools live in the edit-tier dock.
        return compositeToolRowH;
    }

    [[nodiscard]] juce::Rectangle<int> graphArea() const {
        using namespace UiLayout;
        auto area = getLocalBounds().reduced(panelPad);
        area.removeFromTop(timelineChromeHeight());
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
            0, maximumCount(), static_cast<int>(std::floor(
                visible.getStart()
                + static_cast<double>(x - bounds.getX())
                    * visible.getLength()
                    / juce::jmax(1, bounds.getWidth()))));
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
            1, lane_count, visibleLaneCount(graph.getHeight()));
        vertical_scroll_.setRangeLimits(
            0.0, static_cast<double>(lane_count), juce::dontSendNotification);
        vertical_scroll_.setCurrentRange(
            vertical_scroll_.getCurrentRangeStart(),
            static_cast<double>(visible_lanes), juce::dontSendNotification);
        horizontal_scroll_.setRangeLimits(
            0.0,
            static_cast<double>(maximumCount()) + 1.0,
            juce::dontSendNotification);
        applyCountColumnWidth();
    }

    [[nodiscard]] int timelineGraphWidth() const {
        return juce::jmax(
            1,
            graphArea().getWidth()
                - UiLayout::compositeLaneLabelW
                - 16
                - UiLayout::editSubLaneLabelW);
    }

    [[nodiscard]] double countColumnWidthPx() const {
        return static_cast<double>(timelineGraphWidth())
            / juce::jmax(1.0, visibleCountRange().getLength());
    }

    void applyCountColumnWidth() {
        const double min_visible = juce::jmax(
            8.0,
            static_cast<double>(timelineGraphWidth())
                / static_cast<double>(UiLayout::countColumnMaxW));
        const double max_visible = juce::jmax(
            min_visible,
            static_cast<double>(timelineGraphWidth())
                / static_cast<double>(UiLayout::countColumnMinW));
        const double current = horizontal_scroll_.getCurrentRangeSize();
        const double visible = juce::jlimit(min_visible, max_visible, current);
        horizontal_scroll_.setCurrentRange(
            horizontal_scroll_.getCurrentRangeStart(),
            visible,
            juce::dontSendNotification);
    }

    void zoomCountColumns(int anchor_x, double length_factor) {
        const auto graph = graphArea();
        auto bounds = graph;
        bounds.removeFromLeft(UiLayout::compositeLaneLabelW);
        bounds.reduce(8, 0);
        bounds.removeFromLeft(UiLayout::editSubLaneLabelW);
        const int count = countAtX(bounds, anchor_x);
        const double min_visible = juce::jmax(
            8.0,
            static_cast<double>(timelineGraphWidth())
                / static_cast<double>(UiLayout::countColumnMaxW));
        const double max_visible = juce::jmax(
            min_visible,
            static_cast<double>(timelineGraphWidth())
                / static_cast<double>(UiLayout::countColumnMinW));
        const double visible = juce::jlimit(
            min_visible,
            max_visible,
            visibleCountRange().getLength() * length_factor);
        const double start = juce::jmax(
            0.0,
            static_cast<double>(count)
                - visible * juce::jlimit(
                    0.0, 1.0,
                    static_cast<double>(anchor_x - bounds.getX())
                        / juce::jmax(1.0, static_cast<double>(bounds.getWidth()))));
        horizontal_scroll_.setCurrentRange(
            start, visible, juce::dontSendNotification);
        repaint();
    }

    void drawCountGrid(
        juce::Graphics& graphics,
        juce::Rectangle<int> bounds) const {
        const auto visible = visibleCountRange();
        const int begin = juce::jmax(0, static_cast<int>(std::floor(visible.getStart())));
        const int end = juce::jmin(
            maximumCount(), static_cast<int>(std::ceil(visible.getEnd())));
        const double column_w = countColumnWidthPx();
        for (int timeline_count = begin; timeline_count <= end; ++timeline_count) {
            const int x = xForCount(bounds, timeline_count);
            const bool major = timeline_count % 10 == 0;
            graphics.setColour(
                major
                    ? juce::Colour(0xFF52616E)
                    : juce::Colour(0xFF35404A).withAlpha(
                        column_w >= static_cast<double>(UiLayout::countColumnMinW)
                            ? 0.95F : 0.45F));
            graphics.drawVerticalLine(
                x, static_cast<float>(bounds.getY()),
                static_cast<float>(bounds.getBottom()));
        }
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
            graphics.setFont(UiFonts::dense(true));
            graphics.drawText(
                label, x + 2, bounds.getY(), 34, 16,
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
        juce::Colour colour,
        std::optional<Parameter> forced_parameter = std::nullopt) const {
        const auto parameter = forced_parameter.value_or(selectedParameter());
        const auto& layer = timbre_.layers[layer_index];
        if (parameter == Parameter::Pitch) {
            drawPitchLane(graphics, bounds, layer, colour);
            return;
        }
        if (parameter == Parameter::Timbre) {
            drawTimbreMarkers(graphics, bounds, layer, colour);
            return;
        }
        if (parameter == Parameter::Volume) {
            drawVolumeBars(graphics, bounds, layer, colour);
            return;
        }
        const auto& events = eventsFor(layer, parameter);
        const auto kind = eventKindFor(parameter);
        const auto [minimum, maximum] = valueRangeFor(parameter);
        std::vector<std::pair<int, int>> points;
        for (const auto& event : events) {
            if (event.kind == kind) {
                points.emplace_back(
                    static_cast<int>(event.count),
                    juce::jlimit(minimum, maximum, event.value));
            }
        }
        if (points.empty()) {
            points.emplace_back(0, 0);
        }
        std::stable_sort(points.begin(), points.end());
        drawSteppedValuePath(graphics, bounds, points, minimum, maximum, colour);
    }

    void drawSteppedValuePath(
        juce::Graphics& graphics,
        juce::Rectangle<int> bounds,
        const std::vector<std::pair<int, int>>& points,
        int minimum,
        int maximum,
        juce::Colour colour) const {
        if (points.empty() || bounds.isEmpty()) {
            return;
        }
        const auto map_y = [&](int value) {
            return juce::jmap(
                static_cast<float>(value),
                static_cast<float>(minimum),
                static_cast<float>(maximum),
                static_cast<float>(bounds.getBottom()),
                static_cast<float>(bounds.getY()));
        };
        juce::Path path;
        float x = static_cast<float>(xForCount(bounds, points.front().first));
        float y = map_y(points.front().second);
        path.startNewSubPath(x, y);
        for (std::size_t index = 1; index < points.size(); ++index) {
            const float next_x = static_cast<float>(
                xForCount(bounds, points[index].first));
            const float next_y = map_y(points[index].second);
            // Hold horizontally until the next step, then jump vertically.
            path.lineTo(next_x, y);
            path.lineTo(next_x, next_y);
            x = next_x;
            y = next_y;
        }
        path.lineTo(static_cast<float>(bounds.getRight()), y);
        graphics.setColour(colour);
        graphics.strokePath(path, juce::PathStrokeType(2.0F));
        for (const auto& [timeline_count, parameter_value] : points) {
            const float px = static_cast<float>(
                xForCount(bounds, timeline_count));
            const float py = map_y(parameter_value);
            graphics.fillEllipse(px - 3.0F, py - 3.0F, 6.0F, 6.0F);
        }
    }

    void drawVolumeBars(
        juce::Graphics& graphics,
        juce::Rectangle<int> bounds,
        const mgstc::engine::CompositeLayer& layer,
        juce::Colour colour) const {
        struct Segment {
            int start{};
            int end{};
            int volume{};
        };
        std::vector<std::pair<int, int>> points;
        for (const auto& event : layer.volume_envelope.events) {
            if (event.kind == mgstc::engine::EnvelopeEventKind::Volume) {
                points.emplace_back(
                    static_cast<int>(event.count),
                    juce::jlimit(0, 15, event.value));
            }
        }
        if (points.empty()) {
            points.emplace_back(
                0, juce::jlimit(0, 15, static_cast<int>(layer.volume)));
        }
        std::stable_sort(points.begin(), points.end());
        const int end_count = static_cast<int>(
            layer.envelope_timeline.length_counts);
        std::vector<Segment> segments;
        for (std::size_t index = 0; index < points.size(); ++index) {
            const int start = points[index].first;
            const int stop = index + 1 < points.size()
                ? points[index + 1].first
                : juce::jmax(start + 1, end_count);
            segments.push_back(
                {start, juce::jmax(start + 1, stop), points[index].second});
        }
        for (const auto& segment : segments) {
            const int x0 = xForCount(bounds, segment.start);
            const int x1 = xForCount(bounds, segment.end);
            const float top = juce::jmap(
                static_cast<float>(segment.volume),
                0.0F,
                15.0F,
                static_cast<float>(bounds.getBottom()),
                static_cast<float>(bounds.getY() + 1));
            graphics.setColour(colour.withAlpha(0.55F));
            graphics.fillRect(
                x0,
                juce::roundToInt(top),
                juce::jmax(2, x1 - x0),
                juce::jmax(1, bounds.getBottom() - juce::roundToInt(top)));
            graphics.setColour(colour);
            graphics.drawRect(
                x0,
                juce::roundToInt(top),
                juce::jmax(2, x1 - x0),
                juce::jmax(1, bounds.getBottom() - juce::roundToInt(top)));
        }
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
        graphics.strokePath(path, juce::PathStrokeType(1.4F));
    }

    struct EditLaneSlots {
        juce::Rectangle<int> summary;
        juce::Rectangle<int> volume;
        juce::Rectangle<int> pitch;
        juce::Rectangle<int> timbre;
        juce::Rectangle<int> register_write;
        juce::Rectangle<int> tl_auto;
        juce::Rectangle<int> fb_auto;
        juce::Rectangle<int> preview;
    };

    [[nodiscard]] bool showsRegisterLane(
        mgstc::engine::TimbreSource source) const noexcept {
        return source != mgstc::engine::TimbreSource::Scc;
    }

    [[nodiscard]] bool showsRegisterAutoLanes(
        mgstc::engine::TimbreSource source) const noexcept {
        return source == mgstc::engine::TimbreSource::Opll;
    }

    [[nodiscard]] EditLaneSlots makeEditLaneSlots(
        juce::Rectangle<int> content,
        mgstc::engine::TimbreSource source) const {
        EditLaneSlots slots;
        slots.preview = content.removeFromBottom(UiLayout::envelopePreviewH);
        slots.summary = content.removeFromTop(UiLayout::commandSummaryH);
        if (showsRegisterAutoLanes(source)) {
            slots.fb_auto = content.removeFromBottom(UiLayout::registerAutoLaneH);
            slots.tl_auto = content.removeFromBottom(UiLayout::registerAutoLaneH);
        }
        const bool has_register = showsRegisterLane(source);
        const int marker_h = UiLayout::editMarkerLaneH;
        auto value_area = content;
        if (has_register) {
            auto markers = value_area.removeFromBottom(marker_h * 2);
            slots.timbre = markers.removeFromTop(marker_h);
            slots.register_write = markers;
        } else {
            slots.timbre = value_area.removeFromBottom(marker_h);
            slots.register_write = {};
        }
        // Volume / pitch get the remaining height (≈1.5× former equal split
        // after @/y shrink to one text line).
        const int value_h = juce::jmax(36, value_area.getHeight());
        const int pitch_h = value_h / 2;
        slots.volume = value_area.removeFromTop(value_h - pitch_h);
        slots.pitch = value_area;
        return slots;
    }

    [[nodiscard]] juce::Rectangle<int> timelinePlotBounds(
        juce::Rectangle<int> lane_content) const {
        if (lane_content.isEmpty()) {
            return {};
        }
        lane_content.removeFromLeft(UiLayout::editSubLaneLabelW);
        return lane_content;
    }

    [[nodiscard]] juce::Rectangle<int> graphStrip(juce::Rectangle<int> slot) const {
        if (slot.isEmpty()) {
            return {};
        }
        slot.removeFromLeft(UiLayout::editSubLaneLabelW);
        return slot;
    }

    bool hitEditLane(
        const mgstc::engine::CompositeLayer& layer,
        juce::Rectangle<int> content,
        juce::Point<int> point) {
        edit_draw_bounds_.reset();
        const auto slots = makeEditLaneSlots(content, layer.source);
        if (slots.summary.contains(point) || slots.preview.contains(point)) {
            return true;
        }
        const struct LaneHit {
            Parameter parameter;
            juce::Rectangle<int> slot;
            bool drawable;
        } hits[] = {
            {Parameter::Volume, slots.volume, true},
            {Parameter::Pitch, slots.pitch, true},
            {Parameter::Timbre, slots.timbre, false},
            {Parameter::Timbre, slots.register_write, false},
            {Parameter::OpllTlAuto, slots.tl_auto, true},
            {Parameter::OpllFbAuto, slots.fb_auto, true},
        };
        for (const auto& hit : hits) {
            if (hit.slot.isEmpty() || !hit.slot.contains(point)) {
                continue;
            }
            if (hit.drawable) {
                parameter_.setSelectedId(
                    static_cast<int>(hit.parameter),
                    juce::dontSendNotification);
                syncRegisterAutoEditors();
                const bool free_curve = isRegisterAutoParameter(hit.parameter)
                    && selectedRegisterAutoLane() != nullptr
                    && selectedRegisterAutoLane()->mode
                        == mgstc::engine::OpllRegisterAutoMode::FreeCurve;
                const bool rise_fall_params =
                    isRegisterAutoParameter(hit.parameter);
                // Free-curve is mouse-traced; rise/fall/LFO are param-driven
                // lines (still selectable). Only free-curve starts a draw.
                if (!isRegisterAutoParameter(hit.parameter) || free_curve) {
                    edit_draw_bounds_ = graphStrip(hit.slot);
                } else {
                    edit_draw_bounds_.reset();
                    juce::ignoreUnused(rise_fall_params);
                }
            }
            return true;
        }
        return true;
    }

    void ensureSelectedLaneVisible() {
        if (selected_layer_ < 0) {
            return;
        }
        const int first = juce::roundToInt(
            vertical_scroll_.getCurrentRangeStart());
        const int visible = juce::jmax(
            1, juce::roundToInt(vertical_scroll_.getCurrentRangeSize()));
        if (selected_layer_ < first) {
            vertical_scroll_.setCurrentRangeStart(
                static_cast<double>(selected_layer_),
                juce::dontSendNotification);
        } else if (selected_layer_ >= first + visible) {
            vertical_scroll_.setCurrentRangeStart(
                static_cast<double>(selected_layer_ - visible + 1),
                juce::dontSendNotification);
        }
    }

    void refreshCommandStack(const mgstc::engine::CompositeLayer& layer) {
        const auto count = static_cast<std::uint32_t>(
            juce::jmax(0, selected_count_));
        std::vector<juce::String> before;
        std::vector<juce::String> after_end;
        for (const auto& event : layer.pitch_envelope.events) {
            if (event.kind == mgstc::engine::EnvelopeEventKind::Pitch
                && event.count == count) {
                before.push_back(pitchCommandLabel(event.value));
            }
        }
        for (const auto& event : layer.timbre_automation) {
            if (event.count != count) {
                continue;
            }
            if (event.kind == mgstc::engine::EnvelopeEventKind::Timbre) {
                before.push_back(timbreCommandLabel(event));
            } else if (event.kind
                       == mgstc::engine::EnvelopeEventKind::RegisterWrite) {
                before.push_back(
                    "y" + juce::String(event.value) + ","
                    + juce::String(event.secondary));
            }
        }
        const bool loop_start = layer.envelope_timeline.loop_start_count
            && *layer.envelope_timeline.loop_start_count == count;
        const bool loop_end = layer.envelope_timeline.loop_end_count
            && *layer.envelope_timeline.loop_end_count == count;
        command_stack_.setStack(
            selected_count_, loop_start, loop_end,
            std::move(before), std::move(after_end));
    }

    void drawSelectedCountBand(
        juce::Graphics& graphics,
        juce::Rectangle<int> bounds) const {
        const auto visible = visibleCountRange();
        if (selected_count_ < visible.getStart()
            || selected_count_ > visible.getEnd()) {
            return;
        }
        const int x0 = xForCount(bounds, selected_count_);
        const int x1 = xForCount(bounds, selected_count_ + 1);
        const int width = juce::jmax(3, x1 - x0);
        graphics.setColour(juce::Colour(kUiHoverAccent).withAlpha(0.12F));
        graphics.fillRect(x0, bounds.getY(), width, bounds.getHeight());
    }

    void paintEditTier(
        juce::Graphics& graphics,
        juce::Rectangle<int> content,
        std::size_t layer_index,
        juce::Colour colour) {
        const auto& layer = timbre_.layers[layer_index];
        const auto slots = makeEditLaneSlots(content, layer.source);
        drawScope(
            graphics,
            timelinePlotBounds(content),
            layer_index,
            colour.withAlpha(0.32F));
        {
            auto summary = slots.summary;
            auto label = summary.removeFromLeft(UiLayout::editSubLaneLabelW);
            graphics.setColour(juce::Colour(0xFFC9D1D9));
            graphics.setFont(UiFonts::dense(true));
            graphics.drawFittedText(
                juce::String::fromUTF8("命令"),
                label,
                juce::Justification::centredLeft,
                1);
            drawCommandSummary(graphics, summary, layer, colour);
        }
        paintEditSubLane(
            graphics, slots.volume, layer_index, colour,
            Parameter::Volume, juce::String::fromUTF8("音量"));
        paintEditSubLane(
            graphics, slots.pitch, layer_index, colour,
            Parameter::Pitch, juce::String::fromUTF8("音程"));
        paintEditSubLane(
            graphics, slots.timbre, layer_index, colour,
            Parameter::Timbre, juce::String::fromUTF8("@"));
        if (!slots.register_write.isEmpty()) {
            paintRegisterLane(graphics, slots.register_write, layer, colour);
        }
        if (!slots.tl_auto.isEmpty()) {
            paintRegisterAutoLane(
                graphics, slots.tl_auto, layer, colour,
                Parameter::OpllTlAuto, juce::String::fromUTF8("TL自動"));
        }
        if (!slots.fb_auto.isEmpty()) {
            paintRegisterAutoLane(
                graphics, slots.fb_auto, layer, colour,
                Parameter::OpllFbAuto, juce::String::fromUTF8("FB自動"));
        }
        drawTimelineMarkers(graphics, graphStrip(slots.volume), layer_index, colour);
        {
            auto preview = slots.preview;
            auto label = preview.removeFromLeft(UiLayout::editSubLaneLabelW);
            graphics.setColour(juce::Colour(0xFFC9D1D9));
            graphics.setFont(UiFonts::dense(true));
            graphics.drawFittedText(
                "MML", label, juce::Justification::centredLeft, 1);
        }
    }

    void updateEnvelopeMmlPreview() {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            envelope_mml_preview_.clear();
            envelope_mml_preview_.setVisible(false);
            return;
        }
        const auto& layer =
            timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        const auto numbers = mgstc::engine::resolveTimbreNumbers(timbre_);
        const auto formatted = mgstc::engine::formatMgsCompositeEnvelope(
            layer, 0, 255, &numbers, timbreLibrary());
        auto definition = juce::String::fromUTF8(formatted.definition.c_str())
                              .trimEnd();
        if (definition.isEmpty()) {
            definition = juce::String::fromUTF8("@e0 = { }");
        }
        const auto track = juce::String::fromUTF8(
            mgstc::engine::formatMgsCompositeTrackSetup(layer, &numbers)
                .c_str());
        UiFonts::setMgscPreviewText(
            envelope_mml_preview_, definition + "\n" + track);
    }

    void paintRegisterAutoLane(
        juce::Graphics& graphics,
        juce::Rectangle<int> slot,
        const mgstc::engine::CompositeLayer& layer,
        juce::Colour colour,
        Parameter parameter,
        const juce::String& title) {
        auto label = slot.removeFromLeft(UiLayout::editSubLaneLabelW);
        graphics.setColour(
            parameter == selectedParameter()
                ? juce::Colour(kUiHoverAccent)
                : juce::Colour(0xFFC9D1D9));
        graphics.setFont(UiFonts::dense(true));
        graphics.drawFittedText(
            title, label, juce::Justification::centredLeft, 1);
        graphics.setColour(juce::Colour(0xFF40505E));
        graphics.drawHorizontalLine(
            slot.getBottom() - 1,
            static_cast<float>(slot.getX()),
            static_cast<float>(slot.getRight()));
        const auto& auto_lane = parameter == Parameter::OpllTlAuto
            ? layer.opll_tl_auto
            : layer.opll_fb_auto;
        const auto target = parameter == Parameter::OpllTlAuto
            ? mgstc::engine::OpllRegisterAutoTarget::TotalLevel
            : mgstc::engine::OpllRegisterAutoTarget::Feedback;
        const auto visible = visibleCountRange();
        const int begin = juce::jmax(
            0, static_cast<int>(std::floor(visible.getStart())));
        const int end = juce::jmin(
            maximumCount(), static_cast<int>(std::ceil(visible.getEnd())));
        // Dim ROM intervals — TL/FB auto cannot change ROM patches.
        {
            bool in_rom = false;
            int rom_start = begin;
            for (int timeline_count = begin; timeline_count <= end + 1;
                 ++timeline_count) {
                const bool rom_now = timeline_count <= end
                    && !mgstc::engine::opllRegisterAutoAvailableAt(
                        layer,
                        static_cast<std::uint32_t>(timeline_count));
                if (rom_now && !in_rom) {
                    in_rom = true;
                    rom_start = timeline_count;
                } else if (!rom_now && in_rom) {
                    in_rom = false;
                    const int x0 = xForCount(slot, rom_start);
                    const int x1 = xForCount(slot, timeline_count);
                    graphics.setColour(juce::Colour(0x33202830));
                    graphics.fillRect(
                        x0, slot.getY(), juce::jmax(1, x1 - x0),
                        slot.getHeight());
                }
            }
        }
        if (auto_lane.mode == mgstc::engine::OpllRegisterAutoMode::Off) {
            graphics.setColour(juce::Colour(0xFF6B7785));
            graphics.setFont(UiFonts::dense());
            graphics.drawText(
                juce::String::fromUTF8("オフ"),
                slot.reduced(UiLayout::xs, 0),
                juce::Justification::centredLeft,
                false);
            return;
        }
        const int maximum = static_cast<int>(
            mgstc::engine::opllRegisterAutoValueMax(target));
        const auto length = layer.envelope_timeline.length_counts;
        const auto reg_number =
            mgstc::engine::opllRegisterAutoRegisterNumber(target);
        // Authoring polyline uses field values; emit packing uses per-count
        // original image via expandOpllLayerRegisterAutos.
        const auto field_events = mgstc::engine::expandOpllRegisterAuto(
            auto_lane, target, 0, length);
        const auto packed_events =
            mgstc::engine::expandOpllLayerRegisterAutos(
                layer, timbreLibrary());
        if (field_events.empty()) {
            return;
        }
        const auto map_y = [&](int value) {
            return juce::jmap(
                static_cast<float>(value),
                0.0F,
                static_cast<float>(maximum),
                static_cast<float>(slot.getBottom() - 1),
                static_cast<float>(slot.getY() + 1));
        };
        juce::Path path;
        bool started = false;
        std::optional<std::uint8_t> held;
        std::optional<float> last_x;
        std::optional<float> last_y;
        std::size_t event_index = 0;
        for (int timeline_count = begin; timeline_count <= end; ++timeline_count) {
            while (event_index < field_events.size()
                   && field_events[event_index].count
                       <= static_cast<std::uint32_t>(timeline_count)) {
                if (target
                    == mgstc::engine::OpllRegisterAutoTarget::TotalLevel) {
                    held = static_cast<std::uint8_t>(
                        field_events[event_index].secondary & 0x3F);
                } else {
                    held = static_cast<std::uint8_t>(
                        field_events[event_index].secondary & 0x07);
                }
                ++event_index;
            }
            if (!held
                || !mgstc::engine::opllRegisterAutoAvailableAt(
                    layer, static_cast<std::uint32_t>(timeline_count))) {
                started = false;
                last_x.reset();
                last_y.reset();
                continue;
            }
            const float x = static_cast<float>(xForCount(slot, timeline_count));
            const float y = map_y(*held);
            if (!started) {
                path.startNewSubPath(x, y);
                started = true;
            } else if (last_x && last_y) {
                path.lineTo(x, *last_y);
                path.lineTo(x, y);
            }
            last_x = x;
            last_y = y;
        }
        graphics.setColour(colour);
        graphics.strokePath(path, juce::PathStrokeType(1.6F));
        graphics.setFont(UiFonts::dense());
        for (const auto& event : packed_events) {
            if (event.value != reg_number
                || event.count < visible.getStart()
                || event.count > visible.getEnd()) {
                continue;
            }
            const int value = target
                    == mgstc::engine::OpllRegisterAutoTarget::TotalLevel
                ? (event.secondary & 0x3F)
                : (event.secondary & 0x07);
            const int x = xForCount(slot, static_cast<int>(event.count));
            graphics.fillEllipse(
                static_cast<float>(x - 2),
                map_y(value) - 2.0F,
                4.0F,
                4.0F);
        }
    }

    void paintEditSubLane(
        juce::Graphics& graphics,
        juce::Rectangle<int> slot,
        std::size_t layer_index,
        juce::Colour colour,
        Parameter parameter,
        const juce::String& title) {
        auto label = slot.removeFromLeft(UiLayout::editSubLaneLabelW);
        graphics.setColour(
            parameter == selectedParameter()
                ? juce::Colour(kUiHoverAccent)
                : juce::Colour(0xFFC9D1D9));
        graphics.setFont(UiFonts::dense(true));
        graphics.drawFittedText(
            title, label, juce::Justification::centredLeft, 1);
        graphics.setColour(juce::Colour(0xFF40505E));
        graphics.drawHorizontalLine(
            slot.getBottom() - 1,
            static_cast<float>(slot.getX()),
            static_cast<float>(slot.getRight()));
        drawAutomation(graphics, slot, layer_index, colour, parameter);
    }

    void paintRegisterLane(
        juce::Graphics& graphics,
        juce::Rectangle<int> slot,
        const mgstc::engine::CompositeLayer& layer,
        juce::Colour colour) const {
        auto label = slot.removeFromLeft(UiLayout::editSubLaneLabelW);
        graphics.setColour(juce::Colour(0xFFC9D1D9));
        graphics.setFont(UiFonts::dense(true));
        graphics.drawFittedText(
            "y", label, juce::Justification::centredLeft, 1);
        graphics.setColour(juce::Colour(0xFF40505E));
        graphics.drawHorizontalLine(
            slot.getBottom() - 1,
            static_cast<float>(slot.getX()),
            static_cast<float>(slot.getRight()));
        graphics.setFont(UiFonts::dense());
        for (const auto& event : layer.timbre_automation) {
            if (event.kind != mgstc::engine::EnvelopeEventKind::RegisterWrite) {
                continue;
            }
            const int x = xForCount(slot, static_cast<int>(event.count));
            graphics.setColour(colour);
            graphics.fillEllipse(
                static_cast<float>(x - 3),
                static_cast<float>(slot.getCentreY() - 3),
                6.0F,
                6.0F);
            graphics.drawText(
                "y" + juce::String(event.value) + ","
                    + juce::String(event.secondary),
                x + 4,
                slot.getY(),
                64,
                slot.getHeight(),
                juce::Justification::centredLeft,
                false);
        }
    }

    void drawCommandSummary(
        juce::Graphics& graphics,
        juce::Rectangle<int> bounds,
        const mgstc::engine::CompositeLayer& layer,
        juce::Colour colour) const {
        graphics.setColour(juce::Colour(0xFF1B222A));
        graphics.fillRoundedRectangle(bounds.toFloat(), 3.0F);
        const auto visible = visibleCountRange();
        const int begin = juce::jmax(0, static_cast<int>(std::floor(visible.getStart())));
        const int end = juce::jmin(
            maximumCount(), static_cast<int>(std::ceil(visible.getEnd())));
        graphics.setFont(UiFonts::dense());
        for (int timeline_count = begin; timeline_count <= end; ++timeline_count) {
            const int x0 = xForCount(bounds, timeline_count);
            const int x1 = xForCount(bounds, timeline_count + 1);
            const int width = juce::jmax(1, x1 - x0);
            graphics.setColour(
                timeline_count == selected_count_
                    ? juce::Colour(kUiHoverAccent)
                    : juce::Colour(0xFF6B7785));
            graphics.drawText(
                juce::String(timeline_count),
                x0,
                bounds.getBottom() - 13,
                width,
                12,
                juce::Justification::centred,
                false);
        }
        graphics.setFont(UiFonts::dense(true));
        struct SummaryItem {
            std::uint32_t count{};
            juce::String text;
            bool dimmed{};
        };
        std::vector<SummaryItem> items;
        for (const auto& event : layer.pitch_envelope.events) {
            if (event.kind == mgstc::engine::EnvelopeEventKind::Pitch) {
                items.push_back({event.count, pitchCommandLabel(event.value), false});
            }
        }
        for (const auto& event : layer.timbre_automation) {
            if (event.kind == mgstc::engine::EnvelopeEventKind::Timbre) {
                items.push_back(
                    {event.count, timbreCommandLabel(event), false});
            } else if (event.kind
                       == mgstc::engine::EnvelopeEventKind::RegisterWrite) {
                items.push_back(
                    {event.count,
                     "y" + juce::String(event.value) + ","
                         + juce::String(event.secondary),
                     false});
            }
        }
        if (layer.envelope_timeline.loop_start_count) {
            items.push_back(
                {*layer.envelope_timeline.loop_start_count, "[", false});
        }
        if (layer.envelope_timeline.loop_end_count) {
            items.push_back(
                {*layer.envelope_timeline.loop_end_count, "]", false});
        }
        std::vector<int> stack_at_count(
            static_cast<std::size_t>(juce::jmax(1, end - begin + 1)), 0);
        for (const auto& item : items) {
            if (item.count < visible.getStart()
                || item.count > visible.getEnd()) {
                continue;
            }
            const int x0 = xForCount(bounds, static_cast<int>(item.count));
            const int x1 = xForCount(bounds, static_cast<int>(item.count) + 1);
            const int width = juce::jmax(8, x1 - x0 - 2);
            const auto slot = static_cast<std::size_t>(
                juce::jlimit(0, end - begin, static_cast<int>(item.count) - begin));
            const int stack = stack_at_count[slot]++;
            graphics.setColour(
                item.count == static_cast<std::uint32_t>(selected_count_)
                    ? juce::Colour(kUiHoverAccent)
                    : colour);
            graphics.drawText(
                item.text,
                x0 + 1,
                bounds.getY() + stack * 11,
                width,
                12,
                juce::Justification::centred,
                false);
        }
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

    [[nodiscard]] static int clampPitchCommand(int value) noexcept {
        return juce::jlimit(kPitchCommandMin, kPitchCommandMax, value);
    }

    [[nodiscard]] static int cumulativePitchAt(
        const mgstc::engine::CompositeLayer& layer,
        int timeline_count) noexcept {
        int acc = 0;
        for (const auto& event : layer.pitch_envelope.events) {
            if (event.kind == mgstc::engine::EnvelopeEventKind::Pitch
                && event.count <= static_cast<std::uint32_t>(
                       juce::jmax(0, timeline_count))) {
                acc += event.value;
            }
        }
        return acc;
    }

    [[nodiscard]] static int pitchCommandCountAt(
        const mgstc::engine::CompositeLayer& layer,
        int timeline_count) noexcept {
        int count = 0;
        for (const auto& event : layer.pitch_envelope.events) {
            if (event.kind == mgstc::engine::EnvelopeEventKind::Pitch
                && event.count == static_cast<std::uint32_t>(timeline_count)) {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] static std::vector<int> splitPitchDelta(int delta) {
        std::vector<int> parts;
        while (delta > kPitchCommandMax) {
            parts.push_back(kPitchCommandMax);
            delta -= kPitchCommandMax;
        }
        while (delta < kPitchCommandMin) {
            parts.push_back(kPitchCommandMin);
            delta -= kPitchCommandMin;
        }
        if (delta != 0) {
            parts.push_back(delta);
        }
        return parts;
    }

    [[nodiscard]] std::pair<int, int> pitchDisplayRange(
        const mgstc::engine::CompositeLayer& layer) const {
        int extent = kPitchCommandMax;
        int acc = 0;
        for (const auto& event : layer.pitch_envelope.events) {
            if (event.kind != mgstc::engine::EnvelopeEventKind::Pitch) {
                continue;
            }
            acc += event.value;
            extent = juce::jmax(extent, std::abs(acc));
        }
        if (hover_preview_value_) {
            extent = juce::jmax(extent, std::abs(*hover_preview_value_));
        }
        return {-extent, extent};
    }

    void notifyLayerEdit(int timeline_count, int shown_value, bool commit) {
        if (commit
            && selected_layer_ >= 0
            && selected_layer_ < static_cast<int>(timbre_.layers.size())) {
            clampLayerToBodyLimit(
                timbre_.layers[static_cast<std::size_t>(selected_layer_)]);
            refreshCountCeilingCache();
            updateScrollRanges();
        }
        selected_count_ = timeline_count;
        position_.setText(
            "ct " + juce::String(timeline_count),
            juce::dontSendNotification);
        value_.setText(juce::String(shown_value), false);
        if (selected_layer_ >= 0
            && selected_layer_ < static_cast<int>(timbre_.layers.size())) {
            refreshCommandStack(
                timbre_.layers[static_cast<std::size_t>(selected_layer_)]);
        }
        repaint();
        if (edit_callback_) {
            edit_callback_(timbre_, commit);
        }
    }

    void addPitchCommand(int timeline_count, int operand, bool commit) {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        timeline_count = juce::jlimit(0, maximumCount(), timeline_count);
        operand = clampPitchCommand(operand);
        auto& events =
            timbre_.layers[static_cast<std::size_t>(selected_layer_)]
                .pitch_envelope.events;
        timbre_.layers[static_cast<std::size_t>(selected_layer_)]
            .pitch_envelope.kind = mgstc::engine::EnvelopeKind::Sequence;
        events.push_back({
            .kind = mgstc::engine::EnvelopeEventKind::Pitch,
            .value = operand,
            .count = static_cast<std::uint32_t>(timeline_count),
        });
        std::stable_sort(
            events.begin(), events.end(),
            [](const auto& left, const auto& right) {
                return left.count < right.count;
            });
        notifyLayerEdit(timeline_count, operand, commit);
    }

    void editPitchCumulative(
        int timeline_count,
        int desired_cumulative,
        bool commit,
        bool append = false) {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        timeline_count = juce::jlimit(0, maximumCount(), timeline_count);
        auto& layer = timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        auto& events = layer.pitch_envelope.events;
        layer.pitch_envelope.kind = mgstc::engine::EnvelopeKind::Sequence;
        if (append) {
            const int previous = cumulativePitchAt(layer, timeline_count);
            const int desired_delta = desired_cumulative - previous;
            for (const int part : splitPitchDelta(desired_delta)) {
                events.push_back({
                    .kind = mgstc::engine::EnvelopeEventKind::Pitch,
                    .value = part,
                    .count = static_cast<std::uint32_t>(timeline_count),
                });
            }
            std::stable_sort(
                events.begin(), events.end(),
                [](const auto& left, const auto& right) {
                    return left.count < right.count;
                });
            notifyLayerEdit(timeline_count, desired_cumulative, commit);
            return;
        }
        const int previous = timeline_count <= 0
            ? 0
            : cumulativePitchAt(layer, timeline_count - 1);
        const int desired_delta = desired_cumulative - previous;
        std::erase_if(events, [timeline_count](const auto& event) {
            return event.kind == mgstc::engine::EnvelopeEventKind::Pitch
                && event.count == static_cast<std::uint32_t>(timeline_count);
        });
        for (const int part : splitPitchDelta(desired_delta)) {
            events.push_back({
                .kind = mgstc::engine::EnvelopeEventKind::Pitch,
                .value = part,
                .count = static_cast<std::uint32_t>(timeline_count),
            });
        }
        std::stable_sort(
            events.begin(), events.end(),
            [](const auto& left, const auto& right) {
                return left.count < right.count;
            });
        notifyLayerEdit(timeline_count, desired_cumulative, commit);
    }

    void drawPitchLane(
        juce::Graphics& graphics,
        juce::Rectangle<int> bounds,
        const mgstc::engine::CompositeLayer& layer,
        juce::Colour colour) const {
        const auto [minimum, maximum] = pitchDisplayRange(layer);
        const auto map_y = [&](int value) {
            return juce::jmap(
                static_cast<float>(value),
                static_cast<float>(minimum),
                static_cast<float>(maximum),
                static_cast<float>(bounds.getBottom()),
                static_cast<float>(bounds.getY()));
        };
        graphics.setColour(juce::Colour(0xFFE6EDF3).withAlpha(0.55F));
        graphics.drawHorizontalLine(
            juce::roundToInt(map_y(0)),
            static_cast<float>(bounds.getX()),
            static_cast<float>(bounds.getRight()));
        graphics.setFont(UiFonts::dense());
        graphics.drawText(
            "0",
            bounds.getX() + 2,
            juce::roundToInt(map_y(0)) - 12,
            24,
            12,
            juce::Justification::centredLeft,
            false);

        struct Point {
            int count{};
            int cumulative{};
            int sibling{};
        };
        std::vector<Point> points;
        points.push_back({0, 0, 0});
        int acc = 0;
        std::map<int, int> siblings;
        for (const auto& event : layer.pitch_envelope.events) {
            if (event.kind != mgstc::engine::EnvelopeEventKind::Pitch) {
                continue;
            }
            acc += event.value;
            const int count = static_cast<int>(event.count);
            points.push_back({count, acc, siblings[count]++});
        }
        juce::Path path;
        float x = static_cast<float>(xForCount(bounds, points.front().count));
        float y = map_y(points.front().cumulative);
        path.startNewSubPath(x, y);
        for (std::size_t index = 1; index < points.size(); ++index) {
            const float next_x = static_cast<float>(
                xForCount(bounds, points[index].count));
            const float next_y = map_y(points[index].cumulative);
            path.lineTo(next_x, y);
            path.lineTo(next_x, next_y);
            x = next_x;
            y = next_y;
        }
        path.lineTo(static_cast<float>(bounds.getRight()), y);
        graphics.setColour(colour);
        graphics.strokePath(path, juce::PathStrokeType(2.0F));
        for (const auto& point : points) {
            if (point.count == 0 && point.cumulative == 0 && point.sibling == 0
                && layer.pitch_envelope.events.empty()) {
                continue;
            }
            const float column = static_cast<float>(
                xForCount(bounds, point.count));
            const float next_column = static_cast<float>(
                xForCount(bounds, point.count + 1));
            const float span = juce::jmax(6.0F, next_column - column);
            const float px = column
                + span * (0.2F + 0.15F * static_cast<float>(point.sibling));
            const float py = map_y(point.cumulative);
            graphics.setColour(colour);
            graphics.fillEllipse(px - 3.0F, py - 3.0F, 6.0F, 6.0F);
        }
    }

    void drawTimbreMarkers(
        juce::Graphics& graphics,
        juce::Rectangle<int> bounds,
        const mgstc::engine::CompositeLayer& layer,
        juce::Colour colour) const {
        graphics.setFont(UiFonts::dense(true));
        for (const auto& event : layer.timbre_automation) {
            if (event.kind != mgstc::engine::EnvelopeEventKind::Timbre) {
                continue;
            }
            const int x = xForCount(bounds, static_cast<int>(event.count));
            graphics.setColour(colour);
            graphics.fillEllipse(
                static_cast<float>(x - 4),
                static_cast<float>(bounds.getCentreY() - 4),
                8.0F,
                8.0F);
            graphics.drawText(
                timbreCommandLabel(event),
                x + 6,
                bounds.getY(),
                juce::jmax(48, bounds.getRight() - x - 8),
                bounds.getHeight(),
                juce::Justification::centredLeft,
                false);
        }
    }

    [[nodiscard]] juce::String timbreCommandLabel(
        const mgstc::engine::EnvelopeEvent& event) const {
        if (event.timbre_pick == mgstc::engine::TimbrePick::OpllRom) {
            return opllRomPatchLabel(event.value);
        }
        if (event.target_library_id != 0 && timbre_name_callback_) {
            auto name = timbre_name_callback_(event.target_library_id);
            if (name.isNotEmpty()) {
                return name;
            }
        }
        return "@" + juce::String(event.value);
    }

    [[nodiscard]] juce::String timbreInspectLabel(
        const mgstc::engine::CompositeLayer& layer,
        int timeline_count) const {
        if (const auto* event = timbreEventAt(
                layer, static_cast<std::uint32_t>(timeline_count))) {
            return timbreCommandLabel(*event);
        }
        return juce::String::fromUTF8("（なし）");
    }

    [[nodiscard]] static const mgstc::engine::EnvelopeEvent* timbreEventAt(
        const mgstc::engine::CompositeLayer& layer,
        std::uint32_t count) noexcept {
        for (const auto& event : layer.timbre_automation) {
            if (event.kind == mgstc::engine::EnvelopeEventKind::Timbre
                && event.count == count) {
                return &event;
            }
        }
        return nullptr;
    }

    void pickTimbreAt(int timeline_count, bool) {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        auto& layer = timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        if (layer.source == mgstc::engine::TimbreSource::Psg) {
            return;
        }
        timeline_count = juce::jlimit(0, maximumCount(), timeline_count);
        selected_count_ = timeline_count;
        std::vector<EnvelopeTimbreCatalogItem> catalog;
        if (catalog_callback_) {
            catalog = catalog_callback_(layer.source);
        }
        EnvelopeTimbreChoice current;
        if (const auto* existing = timbreEventAt(
                layer, static_cast<std::uint32_t>(timeline_count))) {
            current.pick = existing->timbre_pick;
            current.library_id = existing->target_library_id;
            current.rom_number = static_cast<std::uint8_t>(
                juce::jlimit(0, 14, existing->value));
        }
        const auto count = timeline_count;
        auto* dialog = new ModalDialogWindow(
            juce::String::fromUTF8("音色（@）"),
            juce::Colour(0xFF1B222C));
        auto* content = new EnvelopeTimbrePickContent(
            layer.source,
            catalog,
            current,
            [this, count](EnvelopeTimbreChoice choice) {
                applyTimbreChoice(count, choice);
            },
            [this, source = layer.source](std::uint64_t library_id) {
                if (open_timbre_callback_) {
                    open_timbre_callback_(
                        source == mgstc::engine::TimbreSource::Scc
                            ? "scc" : "opll",
                        library_id);
                }
            },
            [this, count] {
                removeTimbreAt(count);
            });
        dialog->setUsingNativeTitleBar(true);
        dialog->setResizable(false, false);
        dialog->setContentOwned(content, true);
        dialog->centreAroundComponent(
            this, content->getWidth(), content->getHeight() + 32);
        dialog->enterModalState(true, nullptr, true);
    }

    void applyTimbreChoice(int timeline_count, const EnvelopeTimbreChoice& choice) {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        auto& events =
            timbre_.layers[static_cast<std::size_t>(selected_layer_)]
                .timbre_automation;
        std::erase_if(events, [timeline_count](const auto& event) {
            return event.kind == mgstc::engine::EnvelopeEventKind::Timbre
                && event.count == static_cast<std::uint32_t>(timeline_count);
        });
        mgstc::engine::EnvelopeEvent event;
        event.kind = mgstc::engine::EnvelopeEventKind::Timbre;
        event.count = static_cast<std::uint32_t>(timeline_count);
        event.timbre_pick = choice.pick;
        event.target_library_id = choice.library_id;
        event.value = choice.pick == mgstc::engine::TimbrePick::OpllRom
            ? static_cast<std::int32_t>(choice.rom_number)
            : 0;
        events.push_back(event);
        std::stable_sort(
            events.begin(), events.end(),
            [](const auto& left, const auto& right) {
                return left.count < right.count;
            });
        notifyLayerEdit(timeline_count, event.value, true);
    }

    void removeTimbreAt(int timeline_count) {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        auto& events =
            timbre_.layers[static_cast<std::size_t>(selected_layer_)]
                .timbre_automation;
        std::erase_if(events, [timeline_count](const auto& event) {
            return event.kind == mgstc::engine::EnvelopeEventKind::Timbre
                && event.count == static_cast<std::uint32_t>(timeline_count);
        });
        notifyLayerEdit(timeline_count, 0, true);
    }

    void reportStatus(const juce::String& text) {
        if (status_callback_) {
            status_callback_(text);
        }
    }

    void notifyDelete(const juce::String& what) {
        syncPointEditors();
        syncRegisterAutoEditors();
        if (selected_layer_ >= 0
            && selected_layer_ < static_cast<int>(timbre_.layers.size())) {
            refreshCommandStack(
                timbre_.layers[static_cast<std::size_t>(selected_layer_)]);
        }
        repaint();
        if (edit_callback_) {
            edit_callback_(timbre_, true);
        }
        reportStatus(what);
    }

    void deleteCommandAt(juce::Point<int> point) {
        const int layer_index = layerAt(point);
        if (layer_index < 0
            || layer_index >= static_cast<int>(timbre_.layers.size())
            || layer_index >= static_cast<int>(graph_bounds_.size())) {
            return;
        }
        selected_layer_ = layer_index;
        auto& layer = timbre_.layers[static_cast<std::size_t>(layer_index)];
        const auto graph = graph_bounds_[static_cast<std::size_t>(layer_index)];
        if (!graph.contains(point)) {
            return;
        }
        const auto slots = makeEditLaneSlots(graph, layer.source);
        auto count_bounds = graph;
        count_bounds.removeFromLeft(UiLayout::editSubLaneLabelW);
        selected_count_ = countAtX(count_bounds, point.x);
        const auto count = static_cast<std::uint32_t>(
            juce::jmax(0, selected_count_));

        if (slots.volume.contains(point)) {
            auto& events = layer.volume_envelope.events;
            const auto before = events.size();
            std::erase_if(events, [count](const auto& event) {
                return event.kind == mgstc::engine::EnvelopeEventKind::Volume
                    && event.count == count;
            });
            if (events.size() != before) {
                notifyDelete(juce::String::fromUTF8("音量イベントを削除しました"));
            }
            return;
        }
        if (slots.pitch.contains(point)) {
            auto& events = layer.pitch_envelope.events;
            int last = -1;
            for (int i = 0; i < static_cast<int>(events.size()); ++i) {
                if (events[static_cast<std::size_t>(i)].kind
                        == mgstc::engine::EnvelopeEventKind::Pitch
                    && events[static_cast<std::size_t>(i)].count == count) {
                    last = i;
                }
            }
            if (last >= 0) {
                events.erase(events.begin() + last);
                notifyDelete(juce::String::fromUTF8("\\命令を削除しました"));
            }
            return;
        }
        if (slots.timbre.contains(point)) {
            const auto before = layer.timbre_automation.size();
            std::erase_if(layer.timbre_automation, [count](const auto& event) {
                return event.kind == mgstc::engine::EnvelopeEventKind::Timbre
                    && event.count == count;
            });
            if (layer.timbre_automation.size() != before) {
                notifyDelete(juce::String::fromUTF8("@切替を削除しました"));
            }
            return;
        }
        if (slots.register_write.contains(point)) {
            auto& events = layer.timbre_automation;
            int last = -1;
            for (int i = 0; i < static_cast<int>(events.size()); ++i) {
                if (events[static_cast<std::size_t>(i)].kind
                        == mgstc::engine::EnvelopeEventKind::RegisterWrite
                    && events[static_cast<std::size_t>(i)].count == count) {
                    last = i;
                }
            }
            if (last >= 0) {
                events.erase(events.begin() + last);
                notifyDelete(juce::String::fromUTF8("y命令を削除しました"));
            }
            return;
        }
        if (slots.tl_auto.contains(point) || slots.fb_auto.contains(point)) {
            auto& auto_lane = slots.tl_auto.contains(point)
                ? layer.opll_tl_auto
                : layer.opll_fb_auto;
            // Right-click clear applies only to free-curve drawings.
            if (auto_lane.mode
                != mgstc::engine::OpllRegisterAutoMode::FreeCurve) {
                reportStatus(juce::String::fromUTF8(
                    "TL/FB自動の右クリック削除は自由曲線のみです"
                    "（上昇／下降／LFOはモードやパラメーターで変更）"));
                parameter_.setSelectedId(
                    slots.tl_auto.contains(point)
                        ? static_cast<int>(Parameter::OpllTlAuto)
                        : static_cast<int>(Parameter::OpllFbAuto),
                    juce::dontSendNotification);
                syncRegisterAutoEditors();
                return;
            }
            if (auto_lane.free_curve.empty()) {
                return;
            }
            const auto step = juce::jmax<std::uint8_t>(1, auto_lane.coarseness);
            if (count < auto_lane.start_count) {
                return;
            }
            const auto offset =
                (count - auto_lane.start_count) / step;
            if (offset >= auto_lane.free_curve.size()) {
                // Clear entire free curve when clicking past end.
                auto_lane.free_curve.clear();
                notifyDelete(juce::String::fromUTF8(
                    "自由曲線をクリアしました"));
                return;
            }
            auto_lane.free_curve.erase(
                auto_lane.free_curve.begin()
                + static_cast<std::ptrdiff_t>(offset));
            if (auto_lane.free_curve.empty()) {
                auto_lane.start_count = 0;
            }
            notifyDelete(juce::String::fromUTF8(
                "自由曲線の点を削除しました"));
            return;
        }
    }

    void deleteStackChipAt(int chip_index) {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())
            || chip_index < 0) {
            return;
        }
        auto& layer =
            timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        const auto count = static_cast<std::uint32_t>(
            juce::jmax(0, selected_count_));
        int cursor = 0;
        for (auto it = layer.pitch_envelope.events.begin();
             it != layer.pitch_envelope.events.end();
             ++it) {
            if (it->kind == mgstc::engine::EnvelopeEventKind::Pitch
                && it->count == count) {
                if (cursor == chip_index) {
                    layer.pitch_envelope.events.erase(it);
                    notifyDelete(
                        juce::String::fromUTF8("命令スタックから\\を削除"));
                    return;
                }
                ++cursor;
            }
        }
        for (auto it = layer.timbre_automation.begin();
             it != layer.timbre_automation.end();
             ++it) {
            if (it->count != count) {
                continue;
            }
            if (it->kind == mgstc::engine::EnvelopeEventKind::Timbre
                || it->kind
                    == mgstc::engine::EnvelopeEventKind::RegisterWrite) {
                if (cursor == chip_index) {
                    const bool was_y = it->kind
                        == mgstc::engine::EnvelopeEventKind::RegisterWrite;
                    layer.timbre_automation.erase(it);
                    notifyDelete(
                        was_y
                            ? juce::String::fromUTF8(
                                  "命令スタックからyを削除")
                            : juce::String::fromUTF8(
                                  "命令スタックから@を削除"));
                    return;
                }
                ++cursor;
            }
        }
    }

    void rebuildLayerSetups() {
        while (layer_setups_.size()
               > static_cast<int>(timbre_.layers.size())) {
            layer_setups_.removeLast();
        }
        while (layer_setups_.size()
               < static_cast<int>(timbre_.layers.size())) {
            auto* setup = layer_setups_.add(new CompositeLayerSetupView());
            addAndMakeVisible(setup);
        }
        for (int index = 0; index < layer_setups_.size(); ++index) {
            auto* setup = layer_setups_[index];
            const auto layer_index = static_cast<std::size_t>(index);
            setup->setCallbacks(
                [this, layer_index] {
                    selected_layer_ = static_cast<int>(layer_index);
                    if (layer_index < timbre_.layers.size()
                        && static_cast<int>(layer_index) < layer_setups_.size()) {
                        layer_setups_[static_cast<int>(layer_index)]
                            ->applyToLayer(timbre_.layers[layer_index]);
                    }
                    syncPointEditors();
                    syncRegisterAutoEditors();
                    syncInspector();
                    updateScrollRanges();
                    ensureSelectedLaneVisible();
                    resized();
                    repaint();
                    if (edit_callback_) {
                        edit_callback_(timbre_, true);
                    }
                },
                [this, layer_index](LayerBaseTimbreAssign assign) {
                    selected_layer_ = static_cast<int>(layer_index);
                    if (assign_timbre_callback_) {
                        assign_timbre_callback_(layer_index, assign);
                    }
                },
                [this, layer_index] {
                    if (layer_index >= timbre_.layers.size()
                        || !open_timbre_callback_) {
                        return;
                    }
                    const auto& layer = timbre_.layers[layer_index];
                    if (!layer.base_timbre) {
                        return;
                    }
                    open_timbre_callback_(
                        layer.source == mgstc::engine::TimbreSource::Scc
                            ? "scc" : "opll",
                        layer.base_timbre->library_id);
                });
            std::vector<EnvelopeTimbreCatalogItem> catalog;
            if (catalog_callback_) {
                catalog = catalog_callback_(timbre_.layers[layer_index].source);
            }
            setup->syncFromLayer(timbre_.layers[layer_index], catalog);
        }
        layoutLayerSetups();
    }

    void layoutLayerSetups() {
        using namespace UiLayout;
        for (int index = 0; index < layer_setups_.size(); ++index) {
            if (index >= static_cast<int>(lane_frames_.size())
                || lane_frames_[static_cast<std::size_t>(index)].isEmpty()) {
                layer_setups_[index]->setVisible(false);
                continue;
            }
            auto bounds = lane_frames_[static_cast<std::size_t>(index)];
            bounds.setWidth(compositeLaneLabelW);
            const bool edit_tier = index == selected_layer_;
            if (edit_tier) {
                bounds.setHeight(
                    juce::jmin(
                        bounds.getHeight(),
                        compositeSetupContentH + xs * 2));
            }
            layer_setups_[index]->setBounds(bounds.reduced(4, 2));
            layer_setups_[index]->setVisible(true);
            layer_setups_[index]->toFront(false);
        }
    }

    void layoutEditToolsDock() {
        using namespace UiLayout;
        const bool show = selected_layer_ >= 0
            && selected_layer_ < static_cast<int>(lane_frames_.size())
            && !lane_frames_[static_cast<std::size_t>(selected_layer_)]
                    .isEmpty();
        const bool show_auto = show && selectedLayerShowsRegisterAuto();

        auto set_tools_visible = [this, show, show_auto](bool visible) {
            for (auto* c : std::initializer_list<juce::Component*>{
                     &edit_mode_, &parameter_, &position_, &value_label_,
                     &value_, &apply_, &inspector_selection_,
                     &length_label_, &length_editor_, &loop_start_label_,
                     &loop_start_editor_, &loop_end_label_, &loop_end_editor_,
                     &inspector_apply_, &clear_loop_, &command_stack_}) {
                c->setVisible(visible);
            }
            envelope_mml_preview_.setVisible(visible);
            auto_mode_.setVisible(visible && show_auto);
            auto_depth_.setVisible(visible && show_auto);
            auto_speed_.setVisible(visible && show_auto);
            auto_coarseness_.setVisible(visible && show_auto);
            auto_stop_.setVisible(visible && show_auto);
            auto_depth_label_.setVisible(visible && show_auto);
            auto_speed_label_.setVisible(visible && show_auto);
            auto_coarseness_label_.setVisible(visible && show_auto);
            auto_stop_label_.setVisible(visible && show_auto);
        };

        if (!show) {
            set_tools_visible(false);
            return;
        }
        set_tools_visible(true);
        updateEnvelopeMmlPreview();

        auto frame = lane_frames_[static_cast<std::size_t>(selected_layer_)];
        const int setup_h = compositeSetupContentH + xs * 2;
        auto dock = frame;
        dock.setWidth(compositeLaneLabelW);
        dock.removeFromTop(setup_h);
        dock = dock.reduced(4, 2);
        dock.removeFromTop(16); // title painted in paint()

        auto row1 = dock.removeFromTop(compositeToolRowH);
        apply_.setBounds(row1.removeFromRight(44));
        row1.removeFromRight(xs);
        value_.setBounds(row1.removeFromRight(44));
        value_label_.setBounds(row1.removeFromRight(22));
        row1.removeFromRight(xs);
        position_.setBounds(row1.removeFromRight(56));
        row1.removeFromRight(xs);
        parameter_.setBounds(row1.removeFromRight(108));
        row1.removeFromRight(xs);
        edit_mode_.setBounds(row1);

        if (show_auto) {
            dock.removeFromTop(compositeToolGap);
            auto auto_row = dock.removeFromTop(compositeToolRowH);
            auto_stop_.setBounds(auto_row.removeFromRight(40));
            auto_stop_label_.setBounds(auto_row.removeFromRight(30));
            auto_row.removeFromRight(xs);
            auto_coarseness_.setBounds(auto_row.removeFromRight(40));
            auto_coarseness_label_.setBounds(auto_row.removeFromRight(30));
            auto_row.removeFromRight(xs);
            auto_speed_.setBounds(auto_row.removeFromRight(40));
            auto_speed_label_.setBounds(auto_row.removeFromRight(30));
            auto_row.removeFromRight(xs);
            auto_depth_.setBounds(auto_row.removeFromRight(40));
            auto_depth_label_.setBounds(auto_row.removeFromRight(30));
            auto_row.removeFromRight(xs);
            auto_mode_.setBounds(auto_row);
        }

        dock.removeFromTop(compositeToolGap);
        auto loop_row = dock.removeFromTop(compositeToolRowH);
        clear_loop_.setBounds(loop_row.removeFromRight(84));
        loop_row.removeFromRight(xs);
        inspector_apply_.setBounds(loop_row.removeFromRight(44));
        loop_row.removeFromRight(xs);
        loop_end_editor_.setBounds(loop_row.removeFromRight(44));
        loop_end_label_.setBounds(loop_row.removeFromRight(22));
        loop_row.removeFromRight(xs);
        loop_start_editor_.setBounds(loop_row.removeFromRight(44));
        loop_start_label_.setBounds(loop_row.removeFromRight(22));
        loop_row.removeFromRight(xs);
        length_editor_.setBounds(loop_row.removeFromRight(44));
        length_label_.setBounds(loop_row.removeFromRight(32));
        inspector_selection_.setBounds(loop_row);

        dock.removeFromTop(compositeToolGap);
        command_stack_.setBounds(dock.removeFromTop(
            juce::jmin(commandStackH, juce::jmax(48, dock.getHeight()))));

        // Position MGSC preview over the edit-tier preview strip.
        if (selected_layer_ >= 0
            && selected_layer_ < static_cast<int>(graph_bounds_.size())
            && selected_layer_ < static_cast<int>(timbre_.layers.size())) {
            const auto graph =
                graph_bounds_[static_cast<std::size_t>(selected_layer_)];
            const auto slots = makeEditLaneSlots(
                graph,
                timbre_.layers[static_cast<std::size_t>(selected_layer_)]
                    .source);
            auto preview = slots.preview;
            preview.removeFromLeft(editSubLaneLabelW);
            envelope_mml_preview_.setBounds(preview);
            envelope_mml_preview_.toFront(false);
        } else {
            envelope_mml_preview_.setVisible(false);
        }

        for (auto* c : std::initializer_list<juce::Component*>{
                 &edit_mode_, &parameter_, &position_, &value_label_, &value_,
                 &apply_, &auto_mode_, &auto_depth_, &auto_speed_,
                 &auto_coarseness_, &auto_stop_, &auto_depth_label_,
                 &auto_speed_label_, &auto_coarseness_label_, &auto_stop_label_,
                 &inspector_selection_, &length_label_, &length_editor_,
                 &loop_start_label_, &loop_start_editor_, &loop_end_label_,
                 &loop_end_editor_, &inspector_apply_, &clear_loop_,
                 &command_stack_}) {
            if (c->isVisible()) {
                c->toFront(false);
            }
        }
        if (envelope_mml_preview_.isVisible()) {
            envelope_mml_preview_.toFront(false);
        }
    }

    void commitPlaybackTempoFromEditor() {
        const int next = juce::jlimit(
            mgstc::engine::kMgscTempoMin,
            mgstc::engine::kMgscTempoMax,
            tempo_.getText().getIntValue() == 0
                ? mgstc::engine::kMgscDefaultTempo
                : tempo_.getText().getIntValue());
        tempo_.setText(juce::String(next), juce::dontSendNotification);
        if (next == playback_tempo_ && next == timbre_.playback_tempo) {
            return;
        }
        playback_tempo_ = next;
        timbre_.playback_tempo = next;
        repaint();
        if (edit_callback_) {
            edit_callback_(timbre_, true);
        }
    }

    mgstc::engine::CompositeTimbre timbre_;
    juce::ComboBox parameter_;
    juce::ComboBox edit_mode_;
    juce::ComboBox auto_mode_;
    juce::TextButton psg_add_;
    juce::TextButton scc_add_;
    juce::TextButton opll_add_;
    juce::TextButton remove_layer_;
    juce::TextEditor value_;
    juce::Label position_;
    juce::Label value_label_;
    juce::TextButton apply_;
    juce::Label auto_depth_label_;
    juce::Label auto_speed_label_;
    juce::Label auto_coarseness_label_;
    juce::Label auto_stop_label_;
    juce::TextEditor auto_depth_;
    juce::TextEditor auto_speed_;
    juce::TextEditor auto_coarseness_;
    juce::TextEditor auto_stop_;
    juce::Label inspector_selection_;
    juce::Label length_label_;
    juce::Label loop_start_label_;
    juce::Label loop_end_label_;
    juce::TextEditor length_editor_;
    juce::TextEditor loop_start_editor_;
    juce::TextEditor loop_end_editor_;
    juce::TextButton inspector_apply_;
    juce::TextButton clear_loop_;
    CountCommandStackView command_stack_;
    juce::TextEditor envelope_mml_preview_;
    juce::ScrollBar horizontal_scroll_;
    juce::ScrollBar vertical_scroll_;
    EditCallback edit_callback_;
    StatusCallback status_callback_;
    CatalogCallback catalog_callback_;
    OpenTimbreCallback open_timbre_callback_;
    AssignTimbreCallback assign_timbre_callback_;
    TimbreNameCallback timbre_name_callback_;
    TimbreLibraryCallback timbre_library_callback_;
    juce::OwnedArray<CompositeLayerSetupView> layer_setups_;
    juce::Label tempo_label_;
    juce::TextEditor tempo_;
    std::vector<juce::Rectangle<int>> graph_bounds_;
    std::vector<juce::Rectangle<int>> lane_frames_;
    std::optional<juce::Rectangle<int>> edit_draw_bounds_;
    std::optional<int> hover_preview_value_;
    std::optional<juce::Point<int>> hover_mouse_pos_;
    int selected_layer_{};
    int selected_count_{};
    int playback_tempo_{mgstc::engine::kMgscDefaultTempo};
    int cached_count_ceiling_{
        static_cast<int>(mgstc::engine::EnvelopeTimeline::kDefaultLengthCounts)};
    int cached_layer_fit_{
        static_cast<int>(mgstc::engine::EnvelopeTimeline::kDefaultLengthCounts)};
    bool drawing_{};
    static constexpr int kScopeHistorySize = 4096;
    std::array<std::array<float, kScopeHistorySize>, 4>
        scope_history_{};
    int scope_write_position_{};
    int scope_size_{};
    std::uint8_t audition_note_{kPreviewNote};

    void drawHoverValuePopup(juce::Graphics& graphics) const {
        if (!hover_preview_value_ || !hover_mouse_pos_) {
            return;
        }
        const auto parameter = selectedParameter();
        if (parameter != Parameter::Volume
            && parameter != Parameter::Pitch
            && parameter != Parameter::OpllTlAuto
            && parameter != Parameter::OpllFbAuto) {
            return;
        }
        const auto text = juce::String(*hover_preview_value_);
        graphics.setFont(UiFonts::dense(true));
        const int text_w = juce::jmax(
            24, text.length() * 8 + 12);
        const int text_h = 18;
        auto box = juce::Rectangle<int>(
            hover_mouse_pos_->x + 12,
            hover_mouse_pos_->y - text_h - 8,
            text_w,
            text_h);
        if (auto area = graphArea(); !area.isEmpty()) {
            box.setX(juce::jlimit(
                area.getX(), area.getRight() - text_w, box.getX()));
            box.setY(juce::jlimit(
                area.getY(), area.getBottom() - text_h, box.getY()));
        }
        graphics.setColour(juce::Colour(0xEE151A20));
        graphics.fillRoundedRectangle(box.toFloat(), 3.0F);
        graphics.setColour(juce::Colour(kUiHoverAccent));
        graphics.drawRoundedRectangle(box.toFloat(), 3.0F, 1.0F);
        graphics.setColour(juce::Colour(0xFFE6EDF3));
        graphics.drawText(
            text, box, juce::Justification::centred, false);
    }
};

class CompositeEditorComponent final
    : public juce::Component,
      private juce::Timer {
public:
    explicit CompositeEditorComponent(
        SharedAudioService& audio_service,
        SharedMidiInputService& midi_service,
        EditorOpenCallback open_editor,
        TagManagementCallback manage_tags)
        : open_editor_(std::move(open_editor)),
          manage_tags_(std::move(manage_tags)),
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
        title_.setFont(UiFonts::title());
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
        title_.setInterceptsMouseClicks(false, false);
        description_.setInterceptsMouseClicks(false, false);
        open_scc_.setButtonText(juce::String::fromUTF8("SCC音色"));
        open_scc_.setTooltip(juce::String::fromUTF8(
            "チャンネルを追加していなくてもSCC音色エディタを開けます"));
        open_scc_.onClick = [this] {
            open_editor_("scc", std::nullopt);
        };
        addAndMakeVisible(open_scc_);
        open_opll_.setButtonText(juce::String::fromUTF8("OPLL音色"));
        open_opll_.setTooltip(juce::String::fromUTF8(
            "チャンネルを追加していなくてもOPLL音色エディタを開けます"));
        open_opll_.onClick = [this] {
            open_editor_("opll", std::nullopt);
        };
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
        name_label_.setVisible(false);
        addChildComponent(name_label_);
        composite_library_title_.setText(
            juce::String::fromUTF8("総合音色ライブラリ"),
            juce::dontSendNotification);
        composite_library_title_.setFont(UiFonts::heading());
        addAndMakeVisible(composite_library_title_);
        layer_library_title_.setText(
            juce::String::fromUTF8("レイヤー用単音色"),
            juce::dontSendNotification);
        layer_library_title_.setFont(UiFonts::heading());
        addAndMakeVisible(layer_library_title_);
        layer_library_hint_.setText(
            juce::String::fromUTF8(
                "チャンネル行の初期設定でライブラリ音色を選びます。"
                "ここは候補の★絞り込みです。"),
            juce::dontSendNotification);
        layer_library_hint_.setFont(UiFonts::body());
        layer_library_hint_.setJustificationType(
            juce::Justification::topLeft);
        addAndMakeVisible(layer_library_hint_);
        name_.setText(
            juce::String::fromUTF8(timbre_.name.c_str()), false);
        name_.onTextChange = [this] {
            timbre_.name = name_.getText().toStdString();
        };
        UiFonts::styleBodyField(name_);
        addAndMakeVisible(name_);
        tags_.setButtonText(juce::String::fromUTF8("タグを選択"));
        tags_.setTooltip(
                juce::String::fromUTF8(
                "総合音色へ標準タグまたは独自タグを複数設定します"));
        tags_.onClick = [this] { showTagEditor(); };
        addAndMakeVisible(tags_);
        favorite_.setButtonText(juce::String::fromUTF8("★お気に入り"));
        favorite_.onClick = [this] {
            timbre_.favorite = favorite_.getToggleState();
        };
        addAndMakeVisible(favorite_);
        composite_filter_.setTextToShowWhenEmpty(
            juce::String::fromUTF8("名前・タグ・メモを検索"),
            juce::Colour(0xFF7F8993));
        composite_filter_.onTextChange = [this] {
            refreshCompositeSelector();
        };
        UiFonts::styleBodyField(composite_filter_);
        addAndMakeVisible(composite_filter_);
        composite_tag_filter_.setButtonText(
            juce::String::fromUTF8("タグで絞り込み"));
        composite_tag_filter_.onClick = [this] {
            showLibraryTagFilter();
        };
        addAndMakeVisible(composite_tag_filter_);
        tag_manage_.setButtonText(
            juce::String::fromUTF8("ライブラリ管理"));
        tag_manage_.setTooltip(
            juce::String::fromUTF8(
                "音色ライブラリの一覧・複製・削除・タグ管理を行います"));
        tag_manage_.onClick = [this] {
            if (manage_tags_) {
                manage_tags_(this);
            }
        };
        addAndMakeVisible(tag_manage_);
        composite_favorite_only_.setButtonText(
            juce::String::fromUTF8("★のみ"));
        composite_favorite_only_.setLookAndFeel(
            &switch_look_and_feel_);
        composite_favorite_only_.onClick = [this] {
            refreshCompositeSelector();
        };
        addAndMakeVisible(composite_favorite_only_);
        composite_sort_.addItem(
            juce::String::fromUTF8("★優先"), 1);
        composite_sort_.addItem(
            juce::String::fromUTF8("最近使った順"), 2);
        composite_sort_.addItem(
            juce::String::fromUTF8("更新日時順"), 3);
        composite_sort_.addItem(
            juce::String::fromUTF8("名前順"), 4);
        composite_sort_.setSelectedId(1, juce::dontSendNotification);
        composite_sort_.onChange = [this] {
            refreshCompositeSelector();
        };
        addAndMakeVisible(composite_sort_);
        layer_favorite_only_.setButtonText(
            juce::String::fromUTF8("レイヤー候補 ★のみ"));
        layer_favorite_only_.setLookAndFeel(
            &switch_look_and_feel_);
        layer_favorite_only_.onClick = [this] {
            refreshTimbreSelectors();
            syncControlsFromModel();
        };
        addAndMakeVisible(layer_favorite_only_);

        new_.setButtonText(juce::String::fromUTF8("新規"));
        new_.onClick = [this] { newCompositeTimbre(); };
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
        duplicate_.setButtonText(juce::String::fromUTF8("複製"));
        duplicate_.setTooltip(
            juce::String::fromUTF8(
                "選択した保存済み総合音色を複製します"));
        duplicate_.onClick = [this] {
            duplicateSelectedCompositeTimbre();
        };
        addAndMakeVisible(duplicate_);
        rename_.setButtonText(juce::String::fromUTF8("名前変更"));
        rename_.setTooltip(
            juce::String::fromUTF8(
                "名前欄の内容へ保存済み総合音色の名前だけを変更します"));
        rename_.onClick = [this] { renameSelectedCompositeTimbre(); };
        addAndMakeVisible(rename_);
        delete_.setButtonText(juce::String::fromUTF8("削除"));
        delete_.setTooltip(
            juce::String::fromUTF8(
                "選択した保存済み総合音色を削除します"));
        delete_.onClick = [this] { deleteSelectedCompositeTimbre(); };
        addAndMakeVisible(delete_);
        composite_select_.setTextWhenNothingSelected(
            juce::String::fromUTF8("保存済み総合音色"));
        composite_select_.setTooltip(
            juce::String::fromUTF8(
                "保存済み総合音色を選択して即時試聴します"));
        composite_select_.onChange = [this] {
            selectCompositePreview();
        };
        addAndMakeVisible(composite_select_);
        load_.setButtonText(juce::String::fromUTF8("読込"));
        load_.setTooltip(
            juce::String::fromUTF8(
                "選択した総合音色を編集値へ読み込みます"));
        load_.onClick = [this] { loadSelectedCompositeTimbre(); };
        addAndMakeVisible(load_);
        composite_ab_.setButtonText("A/B");
        composite_ab_.setEnabled(false);
        composite_ab_.setTooltip(
            juce::String::fromUTF8(
                "編集中(A)と選択した保存総合音色(B)を交互に試聴します"));
        composite_ab_.onClick = [this] { auditionCompositeAb(); };
        addAndMakeVisible(composite_ab_);

        constexpr std::array<const char*, 3> source_names{
            "PSG", "SCC", "OPLL"};
        for (std::size_t index = 0;
             index < juce::jmin(timbre_.layers.size(), source_.size());
             ++index) {
            source_[index].setText(
                source_names[index], juce::dontSendNotification);
            source_[index].setFont(
                UiFonts::heading());
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
            UiFonts::styleBodyField(layer_name_[index]);
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
                juce::String::fromUTF8("休符ディレイ"));
            delay_[index].setTooltip(juce::String::fromUTF8(
                "発音前の休符ディレイ（トラックMMLの r / r%）。"
                "@e内の待ちではない。実時間はグローバルテンポに依存"));
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
                    open_editor_(
                        "scc",
                        timbre_.layers[index].base_timbre
                            ? std::optional<std::uint64_t>{
                                  timbre_.layers[index]
                                      .base_timbre->library_id}
                            : std::nullopt);
                } else if (timbre_.layers[index].source
                           == mgstc::engine::TimbreSource::Opll) {
                    open_editor_(
                        "opll",
                        timbre_.layers[index].base_timbre
                            ? std::optional<std::uint64_t>{
                                  timbre_.layers[index]
                                      .base_timbre->library_id}
                            : std::nullopt);
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

        resource_.setFont(UiFonts::body(true));
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
                composite_program_stale_ = true;
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
                timeline_preview_pending_ = true;
                timeline_preview_due_ms_ =
                    juce::Time::getMillisecondCounterHiRes() + 50.0;
                updateStatus(
                    juce::String::fromUTF8(
                        "共通時間軸のイベントを設定しました"));
            });
        timeline_.setStatusCallback(
            [this](const juce::String& text) {
                updateStatus(text);
            });
        timeline_.setTimbreCatalogCallback(
            [this](mgstc::engine::TimbreSource source) {
                return envelopeTimbreCatalog(source);
            });
        timeline_.setOpenTimbreCallback(
            [this](
                const juce::String& kind,
                std::optional<std::uint64_t> library_id) {
                open_editor_(kind, library_id);
            });
        timeline_.setAssignTimbreCallback(
            [this](std::size_t index, LayerBaseTimbreAssign assign) {
                if (assign.opll_rom) {
                    assignLayerOpllRom(index, *assign.opll_rom);
                    return;
                }
                assignLayerLibraryId(index, assign.library_id);
            });
        timeline_.setTimbreNameCallback(
            [this](std::uint64_t library_id) {
                if (const auto* entry = timbre_library_.find(library_id)) {
                    return juce::String::fromUTF8(entry->name.c_str());
                }
                return juce::String{};
            });
        timeline_.setTimbreLibraryCallback(
            [this]() -> const mgstc::engine::TimbreLibrary* {
                return &timbre_library_;
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
        setEditorBaseline();
        setSize(UiLayout::editorWindowW, UiLayout::editorWindowH);
        {
            juce::Component::SafePointer<CompositeEditorComponent> safe(this);
            UiScale::addGlobalListener([safe] {
                if (safe != nullptr) {
                    safe->onGlobalUiScaleChanged();
                }
            });
        }
        engine_ready_ = audio_service.running() && configureEngine();
        startTimerHz(60);
        updateStatus(
            engine_ready_
                ? juce::String::fromUTF8(
                      "準備完了 / PC鍵盤 Z=C・上段+1oct・PgDn/PgUp=oct移動")
                : juce::String::fromUTF8(
                      "音声出力を開始できませんでした"));
    }

    ~CompositeEditorComponent() override {
        stopTimer();
        stopAudition();
        composite_favorite_only_.setLookAndFeel(nullptr);
        layer_favorite_only_.setLookAndFeel(nullptr);
        for (std::size_t index = 0; index < enabled_.size(); ++index) {
            enabled_[index].setLookAndFeel(nullptr);
            mute_[index].setLookAndFeel(nullptr);
            solo_[index].setLookAndFeel(nullptr);
        }
    }

    void prepareVisualInspection() {
        startAudition();
    }

    [[nodiscard]] SnapshotResult captureSettingsTab(
        int initial_tab,
        const juce::File& output_file) {
        return performance_keyboard_.captureSettingsTab(
            initial_tab, output_file);
    }

    void refreshExternalState() {
        synchronizeMasterVolumeSlider(
            master_volume_, master_volume_revision_, audio_service_);
        const bool composite_reloaded = loadCompositeLibrary();
        const bool timbre_reloaded = loadTimbreLibrary();
        if (!composite_reloaded && !timbre_reloaded) {
            return;
        }
        std::size_t updated_references{};
        for (const auto& entry : timbre_library_.entries()) {
            updated_references +=
                mgstc::engine::updateTimbreReferences(
                    std::span<mgstc::engine::CompositeTimbre>{
                        &timbre_, 1},
                    entry);
        }
        refreshCompositeSelector();
        refreshTimbreSelectors();
        if (updated_references != 0) {
            syncControlsFromModel();
            updateStatus(
                juce::String::fromUTF8(
                    "保存された単音色の更新を総合音色へ反映しました"
                    "（総合音色は未保存です）"));
        }
    }

    void requestLibraryEntry(std::uint64_t id) {
        if (hasUnsavedChanges()) {
            juce::Component::SafePointer<CompositeEditorComponent> safe(
                this);
            showDiscardConfirmation(
                this,
                juce::String::fromUTF8("ライブラリ音色の読込"),
                [safe, id] {
                    if (safe != nullptr) {
                        safe->performLoadCompositeTimbre(id);
                    }
                });
            return;
        }
        performLoadCompositeTimbre(id);
    }

    void auditionLibraryPreview(std::uint64_t id) {
        juce::InterProcessLock::ScopedLockType lock(library_lock_);
        if (!lock.isLocked() || !reloadCompositeLibraryFromDisk()) {
            return;
        }
        const auto* entry = composite_library_.find(id);
        if (entry == nullptr) {
            return;
        }
        last_audition_note_ = loadLastAuditionNoteSetting();
        auditionCompositeModel(entry->timbre);
        static_cast<void>(
            composite_library_.touch(id, unixTimeNow()));
        static_cast<void>(
            persistCompositeTimbreLibrary(composite_library_));
    }

    void libraryManagerNoteOn(std::uint64_t id, std::uint8_t note) {
        juce::InterProcessLock::ScopedLockType lock(library_lock_);
        if (!lock.isLocked() || !reloadCompositeLibraryFromDisk()) {
            return;
        }
        const auto* entry = composite_library_.find(id);
        if (entry == nullptr) {
            return;
        }
        if (library_manager_performance_id_ != id) {
            stopAudition();
            if (!library_manager_saved_timbre_) {
                library_manager_saved_timbre_ = timbre_;
            }
            timbre_ = entry->timbre;
            composite_program_stale_ = true;
            library_manager_performance_id_ = id;
            if (!configureEngine()) {
                restoreLibraryManagerTimbre();
                return;
            }
            startCompositeNote(note, false, true);
            return;
        }
        startCompositeNote(note, false);
    }

    void libraryManagerNoteOff(std::uint8_t note) {
        stopCompositeNote(note);
        if (voice_allocator_.activeVoiceCount() == 0) {
            restoreLibraryManagerTimbre();
        }
    }

    void libraryManagerAllNotesOff() {
        stopAudition();
        restoreLibraryManagerTimbre();
    }

    void applyTagRewrite(
        std::string_view source,
        std::string_view replacement) {
        static_cast<void>(mgstc::engine::rewriteTimbreTag(
            editor_baseline_.tags, source, replacement));
        const bool editing_changed =
            mgstc::engine::rewriteTimbreTag(
                timbre_.tags, source, replacement);
        static_cast<void>(mgstc::engine::rewriteTimbreTag(
            composite_filter_tags_, source, replacement));
        if (composite_preview_) {
            static_cast<void>(mgstc::engine::rewriteTimbreTag(
                composite_preview_->tags, source, replacement));
        }
        refreshExternalState();
        updateTagButtons();
        syncControlsFromModel();
        if (editing_changed && hasUnsavedChanges()) {
            updateStatus(
                juce::String::fromUTF8(
                    "独自タグの変更を編集中の総合音色へ反映しました"
                    "（未保存）"));
        }
    }

    void deactivate() {
        performance_keyboard_.allNotesOff();
        stopAudition();
        restoreLibraryManagerTimbre();
        // Other editors replace the shared engine program while we are idle.
        composite_program_stale_ = true;
    }

    [[nodiscard]] bool hasUnsavedChanges() const {
        return timbre_ != editor_baseline_
            || selected_composite_id_ != editor_baseline_id_;
    }


    [[nodiscard]] int effectiveUiScalePercent() const noexcept {
        return ui_scale_session_override_.value_or(UiScale::global_percent);
    }

    // Push this editor's effective scale into process metrics (activation /
    // Ctrl±). Unlike paint/resized, this does not require isActiveWindow —
    // the caller is making this editor the scale owner.
    void syncProcessUiScale() {
        UiScale::setActivePercent(effectiveUiScalePercent());
    }

    // Size content + host window from effective metrics (global unless this
    // editor has a Ctrl± session override). Used when creating/showing.
    void applyPreferredSizeForEffectiveScale() {
        applyEditorUiScale(effectiveUiScalePercent());
    }

    void applyEditorUiScale(int percent) {
        UiScale::setActivePercent(percent);
        title_.setFont(UiFonts::title());
        description_.setFont(UiFonts::body());
        composite_library_title_.setFont(UiFonts::heading());
        layer_library_title_.setFont(UiFonts::heading());
        layer_library_hint_.setFont(UiFonts::body());
        UiFonts::styleBodyField(name_);
        UiFonts::styleBodyField(composite_filter_);
        for (auto& editor : layer_name_) {
            UiFonts::styleBodyField(editor);
        }
        timeline_.refreshUiScaleFonts();
        applyScaledContentSize(
            *this,
            UiLayout::editorWindowW,
            UiLayout::editorWindowH,
            true);
    }

    void onGlobalUiScaleChanged() {
        ui_scale_session_override_.reset();
        applyEditorUiScale(UiScale::global_percent);
    }

    void paint(juce::Graphics& graphics) override {
        UiScale::syncEditorDrivenActivePercent(
            *this, effectiveUiScalePercent());
        paintPageBackground(graphics, getLocalBounds());
        paintRoundedPanelFrame(graphics, editor_panel_bounds_);
        paintRoundedPanelFrame(graphics, composite_library_bounds_);
        paintRoundedPanelFrame(graphics, layer_library_bounds_);
    }

    void resized() override {
        UiScale::syncEditorDrivenActivePercent(
            *this, effectiveUiScalePercent());
        using namespace UiLayout;
        auto area = getLocalBounds().reduced(pageMargin);
        title_.setBounds(
            area.getX(),
            area.getY(),
            juce::jmax(120, area.getWidth() - UiScale::sx(430)),
            titleH);
        description_.setBounds(
            area.getX(),
            area.getY() + titleH,
            juce::jmax(120, area.getWidth() - UiScale::sx(430)),
            descriptionH);
        layoutEditorTopRightChrome(
            getWidth(), settings_, master_volume_);
        open_opll_.setBounds(
            getWidth() - UiScale::sx(220),
            pageMargin + UiScale::sx(3),
            UiScale::sx(92),
            textButtonH);
        open_scc_.setBounds(
            getWidth() - UiScale::sx(314),
            pageMargin + UiScale::sx(3),
            UiScale::sx(88),
            textButtonH);
        area.removeFromTop(titleH + descriptionH);

        auto keyboard_area = area.removeFromBottom(keyboardH);
        area.removeFromBottom(keyboardGap);
        performance_keyboard_.setBounds(keyboard_area);

        auto library_column = area.removeFromRight(libraryWidth);
        area.removeFromRight(panelGap);
        layer_library_bounds_ =
            library_column.removeFromBottom(compositeLayerLibraryH);
        library_column.removeFromBottom(sm);
        composite_library_bounds_ = library_column;
        layoutCompositeLibraryPanel(
            composite_library_bounds_,
            CompositeLibraryWidgets{
                composite_library_title_,
                composite_filter_,
                composite_tag_filter_,
                tag_manage_,
                composite_favorite_only_,
                composite_ab_,
                composite_sort_,
                composite_select_,
                load_,
                delete_,
                name_,
                rename_,
                tags_,
                favorite_,
                save_,
                save_as_,
                new_,
                duplicate_});
        layoutCompositeLayerLibraryPanel(
            layer_library_bounds_,
            layer_library_title_,
            layer_library_hint_,
            layer_favorite_only_);

        editor_panel_bounds_ = area;
        auto editor = area.reduced(panelPad);
        auto footer = editor.removeFromBottom(statusH);
        stop_.setBounds(footer.removeFromLeft(90));
        footer.removeFromLeft(md);
        status_.setBounds(footer);
        resource_.setBounds(editor.removeFromTop(fieldH));
        warning_.setBounds(editor.removeFromTop(fieldH + xs));
        editor.removeFromTop(sm);
        timeline_.setBounds(editor);
        applyCompositeFocusOrder();
    }

    bool keyPressed(const juce::KeyPress& key) override {
        if (UiScale::tryHandleEditorScaleKey(
                key,
                ui_scale_session_override_,
                [this](int percent) { applyEditorUiScale(percent); })) {
            return true;
        }
        if (textEntryHasFocusWithin(*this)) {
            return false;
        }
        const auto modifiers = key.getModifiers();
        if (modifiers.isCtrlDown()
            || modifiers.isAltDown()
            || modifiers.isCommandDown()) {
            return false;
        }
        if (performance_keyboard_.shouldConsumeKeyPress(key)) {
            performance_keyboard_.pollPerformanceInput();
            return true;
        }
        return false;
    }

    bool keyStateChanged(bool) override {
        if (!textEntryHasFocusWithin(*this)) {
            performance_keyboard_.pollPerformanceInput();
        }
        return false;
    }

private:
    void applyCompositeFocusOrder() {
        int library_order = 100;
        for (juce::Component* component :
             std::initializer_list<juce::Component*>{
                 &composite_filter_,
                 &composite_tag_filter_,
                 &tag_manage_,
                 &composite_favorite_only_,
                 &composite_ab_,
                 &composite_sort_,
                 &composite_select_,
                 &load_,
                 &delete_,
                 &name_,
                 &rename_,
                 &tags_,
                 &favorite_,
                 &save_,
                 &save_as_,
                 &new_,
                 &duplicate_,
                 &layer_favorite_only_}) {
            component->setExplicitFocusOrder(library_order++);
        }
        int editor_order = 200;
        for (juce::Component* component :
             std::initializer_list<juce::Component*>{
                 &timeline_,
                 &stop_,
                 &status_}) {
            component->setExplicitFocusOrder(editor_order++);
        }
        performance_keyboard_.setExplicitFocusOrder(400);
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
        composite_library_fingerprint_ =
            libraryFileFingerprint(compositeTimbreLibraryFile());
        composite_library_fingerprint_valid_ = true;
        return true;
    }

    // Returns true when the in-memory library was replaced from disk.
    [[nodiscard]] bool loadCompositeLibrary() {
        const auto fingerprint =
            libraryFileFingerprint(compositeTimbreLibraryFile());
        if (composite_library_fingerprint_valid_
            && fingerprint == composite_library_fingerprint_) {
            return false;
        }
        if (!reloadCompositeLibraryFromDisk()) {
            composite_library_ = {};
            composite_library_fingerprint_valid_ = false;
            showCompositeError(
                juce::String::fromUTF8(
                    "既存の総合音色ライブラリを読み込めませんでした"));
            return true;
        }
        return true;
    }

    void updateTagButtons() {
        tags_.setButtonText(tagSelectionSummary(
            timbre_.tags, juce::String::fromUTF8("タグを選択")));
        composite_tag_filter_.setButtonText(tagSelectionSummary(
            composite_filter_tags_,
            juce::String::fromUTF8("タグで絞り込み")));
    }

    void showTagEditor() {
        std::vector<std::vector<std::string>> tag_sets;
        for (const auto& entry : composite_library_.entries()) {
            tag_sets.push_back(entry.timbre.tags);
        }
        juce::Component::SafePointer<CompositeEditorComponent> safe(this);
        showTagSelectionDialog(
            this,
            juce::String::fromUTF8("総合音色のタグ"),
            editorTagChoices(tag_sets, timbre_.tags),
            timbre_.tags,
            true,
            [safe](std::vector<std::string> selected) {
                if (safe != nullptr) {
                    safe->timbre_.tags = std::move(selected);
                    safe->updateTagButtons();
                }
            });
    }

    void showLibraryTagFilter() {
        std::vector<std::vector<std::string>> tag_sets;
        for (const auto& entry : composite_library_.entries()) {
            tag_sets.push_back(entry.timbre.tags);
        }
        juce::Component::SafePointer<CompositeEditorComponent> safe(this);
        showTagSelectionDialog(
            this,
            juce::String::fromUTF8("総合音色ライブラリのタグ検索"),
            filterTagChoices(tag_sets, composite_filter_tags_),
            composite_filter_tags_,
            false,
            [safe](std::vector<std::string> selected) {
                if (safe != nullptr) {
                    safe->composite_filter_tags_ = std::move(selected);
                    safe->updateTagButtons();
                    safe->refreshCompositeSelector();
                }
            });
    }

    void refreshCompositeSelector() {
        syncing_ = true;
        composite_select_.clear(juce::dontSendNotification);
        composite_library_ids_.clear();
        int item_id = 1;
        int selected_item = 0;
        const auto filter = composite_filter_.getText().trim();
        std::vector<const mgstc::engine::CompositeTimbreLibraryEntry*>
            ordered;
        for (const auto& entry : composite_library_.entries()) {
            if (!composite_favorite_only_.getToggleState()
                || entry.timbre.favorite) {
                ordered.push_back(&entry);
            }
        }
        const int sort_mode = composite_sort_.getSelectedId();
        std::stable_sort(
            ordered.begin(), ordered.end(),
            [sort_mode](const auto* left, const auto* right) {
                if (left->timbre.favorite != right->timbre.favorite) {
                    return left->timbre.favorite;
                }
                if (sort_mode == 2
                    && left->last_used_unix_seconds
                        != right->last_used_unix_seconds) {
                    return left->last_used_unix_seconds
                        > right->last_used_unix_seconds;
                }
                if (sort_mode == 3
                    && left->updated_unix_seconds
                        != right->updated_unix_seconds) {
                    return left->updated_unix_seconds
                        > right->updated_unix_seconds;
                }
                if (sort_mode == 4) {
                    return left->timbre.name < right->timbre.name;
                }
                return false;
            });
        for (const auto* entry_ptr : ordered) {
            if (!entry_ptr) {
                continue;
            }
            const auto& entry = *entry_ptr;
            if (filter.isNotEmpty()) {
                const auto name = juce::String::fromUTF8(
                    entry.timbre.name.c_str());
                const auto tags = juce::String::fromUTF8(
                    mgstc::engine::serializeTimbreTags(
                        entry.timbre.tags).c_str());
                const auto memo = juce::String::fromUTF8(
                    entry.timbre.memo.c_str());
                if (!name.containsIgnoreCase(filter)
                    && !tags.containsIgnoreCase(filter)
                    && !memo.containsIgnoreCase(filter)) {
                    continue;
                }
            }
            if (!mgstc::engine::containsAllTimbreTags(
                    entry.timbre.tags, composite_filter_tags_)) {
                continue;
            }
            composite_library_ids_.push_back(entry.id);
            auto label = juce::String::fromUTF8(
                    entry.timbre.name.c_str())
                    + " (r"
                    + juce::String(
                        static_cast<int>(entry.revision))
                    + ")";
            if (entry.timbre.favorite) {
                label = juce::String::fromUTF8("★ ") + label;
            }
            composite_select_.addItem(label, item_id);
            if (selected_composite_id_
                && *selected_composite_id_ == entry.id) {
                selected_item = item_id;
            }
            ++item_id;
        }
        composite_select_.setSelectedId(
            selected_item, juce::dontSendNotification);
        syncing_ = false;
    }

    void newCompositeTimbre() {
        if (hasUnsavedChanges()) {
            juce::Component::SafePointer<CompositeEditorComponent> safe(
                this);
            showDiscardConfirmation(
                this,
                juce::String::fromUTF8("新規作成"),
                [safe] {
                    if (safe != nullptr) {
                        safe->performNewCompositeTimbre();
                    }
                });
            return;
        }
        performNewCompositeTimbre();
    }

    void performNewCompositeTimbre() {
        stopAudition();
        timbre_ = mgstc::engine::defaultCompositeTimbre();
        composite_program_stale_ = true;
        timbre_.layers.clear();
        selected_composite_id_.reset();
        composite_select_.setSelectedId(
            0, juce::dontSendNotification);
        syncControlsFromModel();
        setEditorBaseline();
        updateStatus(
            juce::String::fromUTF8(
                "新しい総合音色を作成しました"));
    }

    void selectCompositePreview() {
        if (syncing_) {
            return;
        }
        const auto selected = composite_select_.getSelectedId();
        if (selected <= 0
            || static_cast<std::size_t>(selected)
                > composite_library_ids_.size()) {
            composite_preview_.reset();
            composite_ab_.setEnabled(false);
            return;
        }
        const auto id = composite_library_ids_[
            static_cast<std::size_t>(selected - 1)];
        const auto* entry = composite_library_.find(id);
        if (!entry) {
            return;
        }
        composite_preview_ = entry->timbre;
        composite_ab_next_b_ = false;
        composite_ab_.setEnabled(true);
        auditionCompositeModel(*composite_preview_);
        static_cast<void>(
            composite_library_.touch(id, unixTimeNow()));
        static_cast<void>(
            persistCompositeTimbreLibrary(composite_library_));
        updateStatus(
            juce::String::fromUTF8(
                "B 保存済み総合音色を試聴中。「読込」で編集へ反映します"));
    }

    void auditionCompositeModel(
        const mgstc::engine::CompositeTimbre& model) {
        stopAudition();
        auto editing = timbre_;
        timbre_ = model;
        composite_program_stale_ = true;
        startAudition();
        timbre_ = std::move(editing);
        composite_program_stale_ = true;
    }

    void auditionCompositeAb() {
        if (!composite_preview_) {
            return;
        }
        const bool play_b = composite_ab_next_b_;
        composite_ab_next_b_ = !composite_ab_next_b_;
        if (play_b) {
            auditionCompositeModel(*composite_preview_);
        } else {
            startAudition();
        }
        composite_ab_.setButtonText(abNextSideButtonText(play_b));
        updateStatus(
            play_b
                ? juce::String::fromUTF8("B 保存済み総合音色を試聴中")
                : juce::String::fromUTF8("A 編集中総合音色を試聴中"));
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
        if (hasUnsavedChanges()) {
            juce::Component::SafePointer<CompositeEditorComponent> safe(
                this);
            showDiscardConfirmation(
                this,
                juce::String::fromUTF8("選択音色の読込"),
                [safe, id] {
                    if (safe != nullptr) {
                        safe->performLoadCompositeTimbre(id);
                    }
                });
            return;
        }
        performLoadCompositeTimbre(id);
    }

    void performLoadCompositeTimbre(std::uint64_t id) {
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
        composite_program_stale_ = true;
        static_cast<void>(
            composite_library_.touch(id, unixTimeNow()));
        static_cast<void>(
            persistCompositeTimbreLibrary(composite_library_));
        loadTimbreLibrary();
        refreshTimbreSelectors();
        refreshCompositeSelector();
        syncControlsFromModel();
        setEditorBaseline();
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
        setEditorBaseline();
        updateStatus(
            save_as
                ? juce::String::fromUTF8(
                    "新しい総合音色として保存しました")
                : juce::String::fromUTF8(
                    "総合音色を保存しました"));
    }

    void duplicateSelectedCompositeTimbre() {
        if (!selected_composite_id_) {
            showCompositeError(
                juce::String::fromUTF8(
                    "複製する総合音色を選択してください"));
            return;
        }
        juce::InterProcessLock::ScopedLockType lock(library_lock_);
        if (!lock.isLocked() || !reloadCompositeLibraryFromDisk()) {
            showCompositeError(
                juce::String::fromUTF8(
                    "共有ライブラリを読み直せませんでした"));
            return;
        }
        const auto* source =
            composite_library_.find(*selected_composite_id_);
        if (!source) {
            showCompositeError(
                juce::String::fromUTF8(
                    "選択した総合音色は削除されています"));
            return;
        }
        auto copy = source->timbre;
        copy.name = composite_library_.uniqueName(copy.name);
        selected_composite_id_ =
            composite_library_.add(copy, unixTimeNow());
        if (!persistCompositeTimbreLibrary(composite_library_)) {
            showCompositeError(
                juce::String::fromUTF8("複製を保存できませんでした"));
            return;
        }
        timbre_ = std::move(copy);
        refreshCompositeSelector();
        syncControlsFromModel();
        setEditorBaseline();
        updateStatus(
            juce::String::fromUTF8("総合音色を複製しました"));
    }

    void renameSelectedCompositeTimbre() {
        if (!selected_composite_id_) {
            showCompositeError(
                juce::String::fromUTF8(
                    "名前を変更する総合音色を選択してください"));
            return;
        }
        const auto requested = utf8String(name_.getText().trim());
        if (requested.empty()) {
            showCompositeError(
                juce::String::fromUTF8("新しい名前を入力してください"));
            return;
        }
        juce::InterProcessLock::ScopedLockType lock(library_lock_);
        if (!lock.isLocked() || !reloadCompositeLibraryFromDisk()) {
            showCompositeError(
                juce::String::fromUTF8(
                    "共有ライブラリを読み直せませんでした"));
            return;
        }
        const auto* source =
            composite_library_.find(*selected_composite_id_);
        if (!source) {
            showCompositeError(
                juce::String::fromUTF8(
                    "選択した総合音色は削除されています"));
            return;
        }
        auto renamed = source->timbre;
        renamed.name = requested;
        if (!composite_library_.update(
                *selected_composite_id_, renamed, unixTimeNow())
            || !persistCompositeTimbreLibrary(composite_library_)) {
            showCompositeError(
                juce::String::fromUTF8(
                    "名前の変更を保存できませんでした"));
            return;
        }
        timbre_.name = requested;
        refreshCompositeSelector();
        syncControlsFromModel();
        setEditorBaseline();
        updateStatus(
            juce::String::fromUTF8("総合音色名を変更しました"));
    }

    void deleteSelectedCompositeTimbre() {
        if (!selected_composite_id_) {
            showCompositeError(
                juce::String::fromUTF8(
                    "削除する総合音色を選択してください"));
            return;
        }
        const auto id = *selected_composite_id_;
        juce::Component::SafePointer<CompositeEditorComponent> safe(this);
        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions()
                .withIconType(
                    juce::MessageBoxIconType::WarningIcon)
                .withTitle(
                    juce::String::fromUTF8("総合音色ライブラリ"))
                .withMessage(
                    juce::String::fromUTF8(
                        "選択した総合音色を削除しますか？"))
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
                    || !safe->reloadCompositeLibraryFromDisk()
                    || !safe->composite_library_.erase(id)
                    || !persistCompositeTimbreLibrary(
                        safe->composite_library_)) {
                    safe->showCompositeError(
                        juce::String::fromUTF8(
                            "削除結果を保存できませんでした"));
                    return;
                }
                safe->performNewCompositeTimbre();
                safe->refreshCompositeSelector();
            });
    }

    void setEditorBaseline() {
        editor_baseline_ = timbre_;
        editor_baseline_id_ = selected_composite_id_;
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

    // Returns true when the in-memory library was replaced from disk.
    [[nodiscard]] bool loadTimbreLibrary() {
        const auto file = timbreLibraryFile();
        const auto fingerprint = libraryFileFingerprint(file);
        if (timbre_library_fingerprint_valid_
            && fingerprint == timbre_library_fingerprint_) {
            return false;
        }
        if (!file.existsAsFile()) {
            timbre_library_ = {};
            timbre_library_fingerprint_ = fingerprint;
            timbre_library_fingerprint_valid_ = true;
            return true;
        }
        std::string error;
        const auto loaded =
            mgstc::engine::TimbreLibrary::deserialize(
                utf8Text(file.loadFileAsString()), &error);
        if (loaded) {
            timbre_library_ = *loaded;
            timbre_library_fingerprint_ = fingerprint;
            timbre_library_fingerprint_valid_ = true;
        } else {
            timbre_library_fingerprint_valid_ = false;
            updateStatus(
                juce::String::fromUTF8(
                    "保存音色ライブラリを読み込めませんでした"));
        }
        return true;
    }

    [[nodiscard]] std::vector<EnvelopeTimbreCatalogItem>
    envelopeTimbreCatalog(mgstc::engine::TimbreSource source) const {
        std::vector<EnvelopeTimbreCatalogItem> items;
        if (source == mgstc::engine::TimbreSource::Psg) {
            return items;
        }
        const auto category = source == mgstc::engine::TimbreSource::Scc
            ? mgstc::engine::TimbreCategory::Scc
            : mgstc::engine::TimbreCategory::Opll;
        const auto numbers = mgstc::engine::resolveTimbreNumbers(timbre_);
        for (const auto& entry : timbre_library_.entries()) {
            if (entry.category != category) {
                continue;
            }
            EnvelopeTimbreCatalogItem item;
            item.library_id = entry.id;
            item.name = juce::String::fromUTF8(entry.name.c_str());
            item.revision = entry.revision;
            item.favorite = entry.favorite;
            item.assigned_number =
                mgstc::engine::assignedNumberForLibraryId(numbers, entry.id);
            items.push_back(std::move(item));
        }
        return items;
    }

    void assignLayerLibraryId(
        std::size_t index,
        std::optional<std::uint64_t> library_id) {
        if (index >= timbre_.layers.size()) {
            return;
        }
        composite_program_stale_ = true;
        if (!library_id) {
            timbre_.layers[index].base_timbre.reset();
            timbre_.layers[index].base_opll_rom.reset();
            refreshTimbreSelectors();
            syncControlsFromModel();
            timeline_.setTimbre(timbre_);
            return;
        }
        const auto* entry = timbre_library_.find(*library_id);
        if (entry == nullptr) {
            return;
        }
        auto reference = mgstc::engine::makeSavedTimbreReference(*entry);
        if (const auto& current = timbre_.layers[index].base_timbre) {
            reference.number_mode = current->number_mode;
            reference.manual_number = current->manual_number;
        }
        timbre_.layers[index].base_opll_rom.reset();
        timbre_.layers[index].base_timbre = std::move(reference);
        refreshTimbreSelectors();
        syncControlsFromModel();
        timeline_.setTimbre(timbre_);
        startAudition();
    }

    void assignLayerOpllRom(std::size_t index, std::uint8_t rom) {
        if (index >= timbre_.layers.size()
            || timbre_.layers[index].source
                != mgstc::engine::TimbreSource::Opll
            || rom > 14) {
            return;
        }
        composite_program_stale_ = true;
        timbre_.layers[index].base_timbre.reset();
        timbre_.layers[index].base_opll_rom = rom;
        refreshTimbreSelectors();
        syncControlsFromModel();
        timeline_.setTimbre(timbre_);
        startAudition();
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
            const auto assigned_id =
                timbre_.layers[index].base_timbre
                ? std::optional<std::uint64_t>{
                      timbre_.layers[index].base_timbre->library_id}
                : std::nullopt;
            std::vector<const mgstc::engine::TimbreLibraryEntry*> ordered;
            for (const auto& entry : timbre_library_.entries()) {
                if (entry.category == category
                    && (!layer_favorite_only_.getToggleState()
                        || entry.favorite
                        || (assigned_id && *assigned_id == entry.id))) {
                    ordered.push_back(&entry);
                }
            }
            std::stable_sort(
                ordered.begin(), ordered.end(),
                [](const auto* left, const auto* right) {
                    return left->favorite && !right->favorite;
                });
            int item_id = 1;
            for (const auto* entry_ptr : ordered) {
                if (!entry_ptr) {
                    continue;
                }
                const auto& entry = *entry_ptr;
                timbre_library_ids_[index].push_back(entry.id);
                auto label = juce::String::fromUTF8(entry.name.c_str())
                        + " (r"
                        + juce::String(
                            static_cast<int>(entry.revision))
                        + ")";
                if (entry.favorite) {
                    label = juce::String::fromUTF8("★ ") + label;
                }
                timbre_select_[index].addItem(label, item_id++);
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
            timbre_.layers[index].base_opll_rom.reset();
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
        timbre_.layers[index].base_opll_rom.reset();
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
        if (mgstc::engine::layerUsesOpllRomBase(timbre_.layers[index])) {
            number_mode_[index].setSelectedId(
                1, juce::dontSendNotification);
            number_mode_[index].setEnabled(false);
            timbre_number_[index].setEnabled(false);
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
        label.setFont(UiFonts::body());
        label.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(label);
    }

    static void layoutLayerSlider(
        juce::Rectangle<int> area,
        juce::Label& label,
        juce::Slider& slider) {
        label.setBounds(area.removeFromTop(18));
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
        updateTagButtons();
        favorite_.setToggleState(
            timbre_.favorite, juce::dontSendNotification);
        for (std::size_t index = 0; index < source_.size(); ++index) {
            setLayerControlsVisible(index, false);
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
                    : layer.base_timbre
                        ? juce::String::fromUTF8("編集: ")
                            + juce::String::fromUTF8(
                                layer.base_timbre->name.c_str())
                    : juce::String::fromUTF8("単音色編集"));
            edit_[index].setEnabled(
                has_saved_timbre && layer.base_timbre.has_value());
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
                layer.start_delay_value,
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
        layer.start_delay_value = static_cast<std::uint32_t>(
            delay_[index].getValue());
        // Legacy chrome sliders (hidden) keep r% step form.
        layer.start_delay_form =
            mgstc::engine::StartDelayForm::AbsoluteTicks;
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
        audio_service_.clearOpllKeyOffForceSilence();
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
        const auto numbers = mgstc::engine::resolveTimbreNumbers(timbre_);
        bool configured =
            edit.engine->session().mapper().defineSccPatch(0, raw_scc)
                == mgstc::engine::MapError::None
            && edit.engine->session().mapper()
                   .defineOpllOriginalPatch(16, opll)
                == mgstc::engine::MapError::None;
        for (const auto& assignment : numbers.assignments) {
            const auto* live = timbre_library_.find(assignment.library_id);
            const mgstc::engine::SavedTimbreReference* snap{};
            mgstc::engine::SavedTimbreReference live_ref;
            if (live != nullptr) {
                live_ref = mgstc::engine::makeSavedTimbreReference(*live);
                snap = &live_ref;
            } else {
                for (const auto& layer : timbre_.layers) {
                    if (layer.base_timbre
                        && layer.base_timbre->library_id
                            == assignment.library_id) {
                        snap = &*layer.base_timbre;
                        break;
                    }
                }
            }
            if (snap == nullptr) {
                continue;
            }
            if (snap->source == mgstc::engine::TimbreSource::Scc) {
                configured = configured
                    && edit.engine->session().mapper().defineSccPatch(
                           assignment.number, snap->scc_waveform)
                        == mgstc::engine::MapError::None;
            } else if (snap->source
                       == mgstc::engine::TimbreSource::Opll) {
                configured = configured
                    && edit.engine->session().mapper()
                           .defineOpllOriginalPatch(
                               assignment.number, snap->opll_registers)
                        == mgstc::engine::MapError::None;
            }
        }
        const auto counts = audibleLayerCounts();
        const auto voice_capacity = compositeVoiceCapacity(counts);
        voice_allocator_.setChannelCount(voice_capacity);
        voice_allocator_.setPolyphonic(performance_keyboard_.polyphonic());
        static_cast<void>(
            mgstc::engine::enforceOpllRegisterAutoExclusivity(timbre_));
        const bool polyphonic = performance_keyboard_.polyphonic();
        for (std::uint8_t voice = 0; voice < voice_capacity; ++voice) {
          // First composite voice keeps per-OPLL-layer y / TL/FB auto.
          // Later poly voices omit shared original-tone y (regs 0–7).
          const bool include_original_tone_y =
              !polyphonic || voice == 0;
          for (std::size_t index = 0; index < timbre_.layers.size(); ++index) {
            const auto& layer = timbre_.layers[index];
            if (!mgstc::engine::layerIsAudible(timbre_, index)) {
                continue;
            }
            const auto track = trackForVoice(index, voice, counts);
            const bool expand_tl = include_original_tone_y
                && mgstc::engine::opllRegisterAutoOwnedByLayer(
                       timbre_,
                       index,
                       mgstc::engine::OpllRegisterAutoTarget::TotalLevel);
            const bool expand_fb = include_original_tone_y
                && mgstc::engine::opllRegisterAutoOwnedByLayer(
                       timbre_,
                       index,
                       mgstc::engine::OpllRegisterAutoTarget::Feedback);
            auto envelopes = compileCompositeEnvelopes(
                layer,
                numbers,
                &timbre_library_,
                include_original_tone_y,
                expand_tl,
                expand_fb);
            configured = configured
                && edit.engine->session().setCompositeSequenceEnvelopes(
                    track,
                    std::move(envelopes.volume),
                    std::move(envelopes.pitch),
                    std::move(envelopes.timbre))
                && edit.engine->session().setTrackVolume(
                    track, layer.volume)
                && edit.engine->session().setTrackDetune(
                    track, layer.detune, layer.micro_detune)
                && edit.engine->session().setTrackPatch(
                    track,
                    mgstc::engine::layerBasePatchNumber(layer, &numbers));
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
        if (!engine_.submitProgram(edit)) {
            return false;
        }
        composite_program_stale_ = false;
        audio_service_.invalidateSharedEditorProgram();
        return true;
    }

    void startAudition() {
        startCompositeNote(last_audition_note_, true);
    }

    void startCompositeNote(
        std::uint8_t base_note,
        bool stop_after_one_second,
        bool already_configured = false) {
        if (stop_after_one_second || !performance_keyboard_.polyphonic()) {
            stopAudition();
        }
        if (!engine_ready_
            || (!already_configured
                && voice_allocator_.activeVoiceCount() == 0
                && composite_program_stale_
                && !configureEngine())) {
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
            const double delay_ms = mgstc::engine::startDelayMilliseconds(
                layer.start_delay_form,
                layer.start_delay_value,
                timbre_.playback_tempo);
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
    }

    void restoreLibraryManagerTimbre() {
        if (!library_manager_saved_timbre_) {
            library_manager_performance_id_.reset();
            return;
        }
        timbre_ = std::move(*library_manager_saved_timbre_);
        library_manager_saved_timbre_.reset();
        library_manager_performance_id_.reset();
        static_cast<void>(configureEngine());
    }

    void stopAudition() {
        timeline_preview_pending_ = false;
        audition_stop_time_ms_.reset();
        for (const auto track : sounding_tracks_) {
            static_cast<void>(
                engine_.submit(
                    mgstc::engine::EngineCommand::noteOff(track)));
            if (track >= kOpllTrack) {
                audio_service_.armOpllKeyOffForceSilence(track);
            }
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
            if (track >= kOpllTrack) {
                audio_service_.armOpllKeyOffForceSilence(track);
            }
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
    }

    void startLayerNote(std::uint8_t track, std::uint8_t note) {
        if (track >= kOpllTrack) {
            audio_service_.cancelOpllKeyOffForceSilence(track);
        }
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
        return LastAuditionNoteStore::instance().get();
    }

    void saveLastAuditionNoteSetting(std::uint8_t note) {
        LastAuditionNoteStore::instance().mark(note);
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
        bool got_scope = false;
        while (engine_.pollOpllScope(scope_frame)) {
            timeline_.appendScopeFrame(
                scope_frame, last_audition_note_);
            got_scope = true;
        }
        if (got_scope) {
            timeline_.repaint();
        }
        if (timeline_preview_pending_
            && now >= timeline_preview_due_ms_) {
            if (configureEngine()) {
                timeline_preview_pending_ = false;
                startCompositeNote(last_audition_note_, true, true);
            }
        }
        if (audition_stop_time_ms_
            && now >= *audition_stop_time_ms_) {
            stopAudition();
        }
    }

    void updateStatus(const juce::String& text) {
        status_.setText(text, juce::dontSendNotification);
    }

    EditorOpenCallback open_editor_;
    TagManagementCallback manage_tags_;
    SharedAudioService& audio_service_;
    mgstc::engine::RealtimeEngineHost& engine_;
    juce::TooltipWindow tooltip_window_;
    SwitchLookAndFeel switch_look_and_feel_;
    mgstc::engine::CompositeTimbre timbre_;
    juce::Label title_;
    juce::Label description_;
    juce::Label composite_library_title_;
    juce::Label layer_library_title_;
    juce::Label layer_library_hint_;
    juce::Label name_label_;
    juce::TextEditor name_;
    juce::TextButton tags_;
    juce::TextButton tag_manage_;
    juce::ToggleButton favorite_;
    juce::TextEditor composite_filter_;
    juce::TextButton composite_tag_filter_;
    juce::ToggleButton composite_favorite_only_;
    juce::ComboBox composite_sort_;
    juce::ToggleButton layer_favorite_only_;
    juce::ComboBox composite_select_;
    juce::TextButton new_;
    juce::TextButton load_;
    juce::TextButton save_;
    juce::TextButton save_as_;
    juce::TextButton composite_ab_;
    juce::TextButton duplicate_;
    juce::TextButton rename_;
    juce::TextButton delete_;
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
    juce::Rectangle<int> editor_panel_bounds_;
    juce::Rectangle<int> composite_library_bounds_;
    juce::Rectangle<int> layer_library_bounds_;
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
    std::optional<mgstc::engine::CompositeTimbre>
        composite_preview_;
    std::optional<std::uint64_t> library_manager_performance_id_;
    std::optional<mgstc::engine::CompositeTimbre>
        library_manager_saved_timbre_;
    bool composite_ab_next_b_{true};
    std::vector<std::string> composite_filter_tags_;
    mgstc::engine::CompositeTimbre editor_baseline_;
    std::optional<int> ui_scale_session_override_;
    std::optional<std::uint64_t> editor_baseline_id_;
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
    bool composite_program_stale_{true};
    LibraryFileFingerprint composite_library_fingerprint_{};
    LibraryFileFingerprint timbre_library_fingerprint_{};
    bool composite_library_fingerprint_valid_{};
    bool timbre_library_fingerprint_valid_{};
    std::optional<double> audition_stop_time_ms_;
    bool timeline_preview_pending_{};
    double timeline_preview_due_ms_{};
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
        has_waveform_ = true;
        repaint();
    }

    void clear() {
        waveform_ = {};
        has_waveform_ = false;
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

        if (!has_waveform_) {
            graphics.setColour(juce::Colour(0xFF8A97A4));
            graphics.setFont(UiFonts::body());
            graphics.drawFittedText(
                juce::String::fromUTF8("未選択"),
                getLocalBounds(),
                juce::Justification::centred,
                1);
            return;
        }

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
    bool has_waveform_{};
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
        graphics.setFont(UiFonts::body(true));
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
        EditorOpenCallback open_editor,
        TagManagementCallback manage_tags)
        : open_editor_(std::move(open_editor)),
          manage_tags_(std::move(manage_tags)),
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
        title_.setFont(UiFonts::title());
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
            [this] { open_editor_("opll", std::nullopt); });
        configureIconButton(
            convert_to_opll_, EditorIcon::Convert,
            juce::String::fromUTF8(
                "現在のSCC波形をOPLL音色として近似変換"),
            [this] { convertToOpll(); });

        rebuildPresetMenu();
        preset_.setTextWhenNothingSelected(
            juce::String::fromUTF8("プリセット未選択"));
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
                if (!immediate_audition_.getToggleState()
                    || !engine_ready_) {
                    return;
                }
                // 鍵盤押し中に hardReset 付き1秒試聴へ入ると、押し中ノートが
                // 消えてPCキーの立ち上がり判定とも食い違う。波形だけ差し替え、
                // 押し中ノートを同じキーのまま張り直す。
                if (performance_keyboard_.hasActiveNote()) {
                    if (!configureEngine(false, &waveform)) {
                        return;
                    }
                    const juce::ScopedValueSetter<bool> keep(
                        keep_temporary_program_on_note_on_, true);
                    performance_keyboard_.retriggerHeldNotes();
                    return;
                }
                static_cast<void>(auditionOneSecond(&waveform));
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
            juce::String::fromUTF8("背景読込"));
        background_load_.setTooltip(
            juce::String::fromUTF8(
                "波形トレース用の背景画像を読み込みます。"
                "音色値は変更しません"));
        background_load_.onClick = [this] { chooseBackgroundImage(); };
        addAndMakeVisible(background_load_);
        background_clear_.setButtonText(
            juce::String::fromUTF8("背景消去"));
        background_clear_.setTooltip(
            juce::String::fromUTF8(
                "背景画像の参照をクリアします"));
        background_clear_.onClick = [this] {
            background_path_.clear();
            background_image_ = {};
            applyBackgroundToGraph();
            updateBackgroundControlState();
            saveBackgroundSettings();
            updateStatus(
                juce::String::fromUTF8("背景画像をクリアしました"));
        };
        addAndMakeVisible(background_clear_);
        background_visible_.setButtonText(
            juce::String::fromUTF8("背景表示"));
        background_visible_.setLookAndFeel(&switch_look_and_feel_);
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
                juce::Slider::TextBoxRight,
                false,
                UiScale::sx(56),
                UiScale::sx(22));
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
            label->setFont(UiFonts::body());
            label->setJustificationType(
                juce::Justification::centredRight);
            addAndMakeVisible(*label);
        }

        library_title_.setText(
            juce::String::fromUTF8("音色ライブラリ"),
            juce::dontSendNotification);
        library_title_.setFont(UiFonts::heading());
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
        UiFonts::styleBodyField(library_filter_);
        addAndMakeVisible(library_filter_);
        library_tag_filter_.setButtonText(
            juce::String::fromUTF8("タグで絞り込み"));
        library_tag_filter_.setTooltip(
            juce::String::fromUTF8(
                "保存済み音色で使われているタグを複数選択します"));
        library_tag_filter_.onClick = [this] {
            showLibraryTagFilter();
        };
        addAndMakeVisible(library_tag_filter_);
        tag_manage_.setButtonText(
            juce::String::fromUTF8("ライブラリ管理"));
        tag_manage_.setTooltip(
            juce::String::fromUTF8(
                "音色ライブラリの一覧・複製・削除・タグ管理を行います"));
        tag_manage_.onClick = [this] {
            if (manage_tags_) {
                manage_tags_(this);
            }
        };
        addAndMakeVisible(tag_manage_);
        favorite_only_.setButtonText(
            juce::String::fromUTF8("★のみ"));
        favorite_only_.setLookAndFeel(&switch_look_and_feel_);
        favorite_only_.onClick = [this] { refreshLibraryList(); };
        addAndMakeVisible(favorite_only_);
        library_ab_.setButtonText("A/B");
        library_ab_.setTooltip(
            juce::String::fromUTF8(
                "編集中(A)と選択した保存音色(B)を交互に試聴します"));
        library_ab_.setEnabled(false);
        library_ab_.onClick = [this] { auditionLibraryAb(); };
        addAndMakeVisible(library_ab_);
        library_sort_.addItem(
            juce::String::fromUTF8("★優先"), 1);
        library_sort_.addItem(
            juce::String::fromUTF8("最近使った順"), 2);
        library_sort_.addItem(
            juce::String::fromUTF8("更新日時順"), 3);
        library_sort_.addItem(
            juce::String::fromUTF8("名前順"), 4);
        library_sort_.setSelectedId(1, juce::dontSendNotification);
        library_sort_.onChange = [this] { refreshLibraryList(); };
        addAndMakeVisible(library_sort_);

        name_.setTextToShowWhenEmpty(
            juce::String::fromUTF8("音色名"),
            juce::Colour(0xFF7F8993));
        name_.onTextChange = [this] { updateDefinitionPreview(); };
        tags_.setButtonText(juce::String::fromUTF8("タグを選択"));
        tags_.setTooltip(
            juce::String::fromUTF8(
                "標準タグを複数選択、または独自タグを追加します"));
        tags_.onClick = [this] { showTagEditor(); };
        memo_.setTextToShowWhenEmpty(
            juce::String::fromUTF8("メモ"),
            juce::Colour(0xFF7F8993));
        memo_.setMultiLine(true);
        memo_.setReturnKeyStartsNewLine(true);
        UiFonts::styleBodyField(name_);
        addAndMakeVisible(name_);
        addAndMakeVisible(tags_);
        UiFonts::styleBodyField(memo_);
        addAndMakeVisible(memo_);

        favorite_.setButtonText(
            juce::String::fromUTF8("★お気に入り"));
        favorite_.setLookAndFeel(&switch_look_and_feel_);
        addAndMakeVisible(favorite_);
        configureButton(
            library_new_,
            juce::String::fromUTF8("新規"),
            juce::String::fromUTF8("新しいSCC音色を作成します"),
            [this] { newLibraryEntry(); });
        configureButton(
            library_load_,
            juce::String::fromUTF8("読込"),
            juce::String::fromUTF8("選択音色を編集値へ読み込みます"),
            [this] { loadSelectedLibraryEntry(); });
        configureButton(
            library_save_,
            juce::String::fromUTF8("保存"),
            juce::String::fromUTF8("選択音色を更新します"),
            [this] { saveLibraryEntry(false); });
        configureButton(
            library_save_as_,
            juce::String::fromUTF8("別名保存"),
            juce::String::fromUTF8("新しい音色として保存します"),
            [this] { saveLibraryEntry(true); });
        configureButton(
            library_rename_,
            juce::String::fromUTF8("名前変更"),
            juce::String::fromUTF8(
                "名前欄の内容へ選択音色の名前だけを変更します"),
            [this] { renameSelectedLibraryEntry(); });
        configureButton(
            library_delete_,
            juce::String::fromUTF8("削除"),
            juce::String::fromUTF8("選択音色を削除します"),
            [this] { deleteSelectedLibraryEntry(); });
        configureButton(
            library_import_,
            juce::String::fromUTF8("取込"),
            juce::String::fromUTF8("外部.mgstcライブラリを取り込みます"),
            [this] { importLibraryFile(); });
        configureButton(
            library_export_,
            juce::String::fromUTF8("書出"),
            juce::String::fromUTF8("選択音色を.mgstcへ書き出します"),
            [this] { exportSelectedLibraryEntry(); });

        mgsc_title_.setText(
            juce::String::fromUTF8("MGSC @s 定義プレビュー"),
            juce::dontSendNotification);
        mgsc_title_.setFont(UiFonts::heading());
        addAndMakeVisible(mgsc_title_);
        output_number_label_.setText(
            juce::String::fromUTF8("一時出力番号"),
            juce::dontSendNotification);
        addAndMakeVisible(output_number_label_);
        UiFonts::styleBodyField(output_number_);
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
        mgsc_preview_.setWantsKeyboardFocus(false);
        mgsc_preview_.setScrollbarsShown(true);
        mgsc_preview_.setFont(UiFonts::mono());
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
        updateBackgroundControlState();
        clearPresetSelectionState();
        // Tab: Library (100+) → editor/toolbar (200+) → wave (300+) → keyboard (400+).
        // Contiguous ranges so Y/X layout cannot interleave library with left column.
        assignExplicitFocusOrders(
            {
                &library_filter_,
                &library_tag_filter_,
                &tag_manage_,
                &favorite_only_,
                &library_ab_,
                &library_sort_,
                &library_list_,
                &library_load_,
                &library_delete_,
                &name_,
                &library_rename_,
                &tags_,
                &memo_,
                &favorite_,
                &library_save_,
                &library_save_as_,
                &library_new_,
                &library_import_,
                &library_export_,
                &output_number_,
            },
            100);
        assignExplicitFocusOrders(
            {
                &immediate_audition_,
                &master_volume_,
                &settings_,
                &load_,
                &save_,
                &paste_,
                &copy_,
                &undo_,
                &redo_,
                &import_wave_,
                &import_audacity_,
                &open_opll_,
                &convert_to_opll_,
                &preset_,
                &harmonic_,
                &apply_range_,
                &apply_preset_,
                &cancel_preview_,
                &ab_audition_,
                &merge_enabled_,
                &merge_amount_,
                &auto_phase_,
                &polarity_,
                &preserve_volume_,
                &preset_flip_h_,
                &preset_flip_v_,
            },
            200);
        assignExplicitFocusOrders(
            {
                &graph_,
                &average_,
                &normalize_,
                &invert_,
                &shift_up_,
                &rotate_left_,
                &rotate_right_,
                &shift_down_,
                &vertical_scale_,
                &scale_reset_,
                &background_load_,
                &background_clear_,
                &background_visible_,
                &background_opacity_,
                &background_x_,
                &background_y_,
                &background_width_,
                &background_height_,
            },
            300);
        assignExplicitFocusOrders(
            {
                &performance_keyboard_,
            },
            400);
        setSize(UiLayout::editorWindowW, UiLayout::editorWindowH);
        {
            juce::Component::SafePointer<SccEditorComponent> safe(this);
            UiScale::addGlobalListener([safe] {
                if (safe != nullptr) {
                    safe->onGlobalUiScaleChanged();
                }
            });
        }
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

    [[nodiscard]] SnapshotResult captureSettingsTab(
        int initial_tab,
        const juce::File& output_file) {
        return performance_keyboard_.captureSettingsTab(
            initial_tab, output_file);
    }

    void refreshExternalState() {
        synchronizeMasterVolumeSlider(
            master_volume_, master_volume_revision_, audio_service_);
        // pending取込が試聴を始めた直後に configureEngine(false) すると
        // hardResetで音が消えるため、取込成功時は再構成しない。
        if (consumePendingSccConversion() || !engine_ready_) {
            return;
        }
        if (!needsSharedEngineReconfigure()) {
            return;
        }
        static_cast<void>(configureEngine(false));
    }

    [[nodiscard]] bool needsSharedEngineReconfigure() const {
        if (engine_holds_temporary_program_) {
            return true;
        }
        if (!audio_service_.sharedEditorProgramActive()) {
            return true;
        }
        if (scc_wave_ != audio_service_.sharedSccWaveform()) {
            return true;
        }
        return applied_shared_scc_revision_
                != audio_service_.sharedSccWaveRevision()
            || applied_shared_opll_revision_
                != audio_service_.sharedOpllPatchRevision();
    }

    void applyTagRewrite(
        std::string_view source,
        std::string_view replacement) {
        if (editor_baseline_valid_) {
            static_cast<void>(mgstc::engine::rewriteTimbreTag(
                editor_baseline_.tags, source, replacement));
        }
        const bool editing_changed =
            mgstc::engine::rewriteTimbreTag(
                selected_tags_, source, replacement);
        static_cast<void>(mgstc::engine::rewriteTimbreTag(
            selected_filter_tags_, source, replacement));
        if (library_preview_) {
            static_cast<void>(mgstc::engine::rewriteTimbreTag(
                library_preview_->tags, source, replacement));
        }
        loadLibrary();
        updateTagButtons();
        refreshLibraryList();
        if (editing_changed && hasUnsavedChanges()) {
            updateStatus(
                juce::String::fromUTF8(
                    "独自タグの変更を編集中のSCC音色へ反映しました"
                    "（未保存）"));
        }
    }

    void requestLibraryEntry(std::uint64_t id) {
        {
            juce::InterProcessLock::ScopedLockType lock(library_lock_);
            if (!lock.isLocked() || !reloadLibraryFromDisk()
                || !library_.find(id)) {
                showError(
                    juce::String::fromUTF8("音色ライブラリ"),
                    juce::String::fromUTF8(
                        "割当音色を読み込めませんでした"));
                return;
            }
        }
        selected_library_id_ = id;
        refreshLibraryList();
        loadSelectedLibraryEntry();
    }

    void auditionLibraryPreview(std::uint64_t id) {
        juce::InterProcessLock::ScopedLockType lock(library_lock_);
        if (!lock.isLocked() || !reloadLibraryFromDisk()) {
            return;
        }
        const auto* entry = library_.find(id);
        if (entry == nullptr
            || entry->category != mgstc::engine::TimbreCategory::Scc) {
            return;
        }
        last_audition_note_ = loadLastAuditionNoteSetting();
        SccWaveform preview{};
        std::transform(
            entry->scc_waveform.begin(),
            entry->scc_waveform.end(),
            preview.begin(),
            [](std::uint8_t value) {
                return static_cast<std::int8_t>(value);
            });
        static_cast<void>(auditionOneSecond(&preview));
        static_cast<void>(library_.touch(id, unixTimeNow()));
        static_cast<void>(persistLibrary());
    }

    void libraryManagerNoteOn(std::uint64_t id, std::uint8_t note) {
        juce::InterProcessLock::ScopedLockType lock(library_lock_);
        if (!lock.isLocked() || !reloadLibraryFromDisk()) {
            return;
        }
        const auto* entry = library_.find(id);
        if (entry == nullptr
            || entry->category != mgstc::engine::TimbreCategory::Scc) {
            return;
        }
        SccWaveform preview{};
        std::transform(
            entry->scc_waveform.begin(),
            entry->scc_waveform.end(),
            preview.begin(),
            [](std::uint8_t value) {
                return static_cast<std::int8_t>(value);
            });
        audition_stop_time_ms_.reset();
        performance_keyboard_.clearPreviewNote();
        last_audition_note_ = note;
        saveLastAuditionNoteSetting(last_audition_note_);
        if (!engine_ready_) {
            return;
        }
        if (library_manager_performance_id_ != id
            || voice_allocator_.activeVoiceCount() == 0) {
            if (!configureEngine(false, &preview)) {
                return;
            }
            library_manager_performance_id_ = id;
            library_manager_preview_wave_ = preview;
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
            return;
        }
    }

    void libraryManagerNoteOff(std::uint8_t note) {
        stopPerformanceNote(note);
        if (voice_allocator_.activeVoiceCount() == 0) {
            library_manager_performance_id_.reset();
            library_manager_preview_wave_.reset();
        }
    }

    void libraryManagerAllNotesOff() {
        silenceAllVoices();
        library_manager_performance_id_.reset();
        library_manager_preview_wave_.reset();
    }

    [[nodiscard]] bool hasUnsavedChanges() const {
        return editor_baseline_valid_
            && (selected_library_id_ != editor_baseline_id_
                || !sameEditableTimbreLibraryEntry(
                    captureLibraryEntry(), editor_baseline_));
    }

    void deactivate() {
        performance_keyboard_.allNotesOff();
        silenceAllVoices();
        library_manager_performance_id_.reset();
        library_manager_preview_wave_.reset();
    }

    void prepareToHide() {
        saveBackgroundSettings();
        deactivate();
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
        favorite_only_.setLookAndFeel(nullptr);
        background_visible_.setLookAndFeel(nullptr);
    }


    [[nodiscard]] int effectiveUiScalePercent() const noexcept {
        return ui_scale_session_override_.value_or(UiScale::global_percent);
    }

    void syncProcessUiScale() {
        UiScale::setActivePercent(effectiveUiScalePercent());
    }

    void applyPreferredSizeForEffectiveScale() {
        applyEditorUiScale(effectiveUiScalePercent());
    }

    void applyEditorUiScale(int percent) {
        UiScale::setActivePercent(percent);
        title_.setFont(UiFonts::title());
        description_.setFont(UiFonts::body());
        library_title_.setFont(UiFonts::heading());
        mgsc_title_.setFont(UiFonts::heading());
        UiFonts::styleBodyField(library_filter_);
        UiFonts::styleBodyField(name_);
        UiFonts::styleBodyField(memo_);
        UiFonts::styleBodyField(output_number_);
        for (auto* label :
             {&background_opacity_label_,
              &background_x_label_,
              &background_y_label_,
              &background_w_label_,
              &background_h_label_}) {
            label->setFont(UiFonts::body());
        }
        for (auto* slider :
             {&background_opacity_,
              &background_x_,
              &background_y_,
              &background_width_,
              &background_height_}) {
            slider->setTextBoxStyle(
                juce::Slider::TextBoxRight,
                false,
                UiScale::sx(56),
                UiScale::sx(22));
        }
        UiFonts::refreshMgscPreviewFont(mgsc_preview_);
        applyScaledContentSize(
            *this,
            UiLayout::editorWindowW,
            UiLayout::editorWindowH,
            true);
    }

    void onGlobalUiScaleChanged() {
        ui_scale_session_override_.reset();
        applyEditorUiScale(UiScale::global_percent);
    }

    void paint(juce::Graphics& graphics) override {
        UiScale::syncEditorDrivenActivePercent(
            *this, effectiveUiScalePercent());
        paintPageBackground(graphics, getLocalBounds());
        paintRoundedPanelFrame(graphics, editor_panel_bounds_);
        paintRoundedPanelFrame(graphics, library_panel_bounds_);
    }

    void resized() override {
        UiScale::syncEditorDrivenActivePercent(
            *this, effectiveUiScalePercent());
        using namespace UiLayout;
        auto area = getLocalBounds().reduced(pageMargin);
        title_.setBounds(area.removeFromTop(titleH));
        description_.setBounds(area.removeFromTop(descriptionH));
        layoutEditorTopRightChrome(
            getWidth(),
            settings_,
            master_volume_,
            &immediate_audition_);
        area.removeFromTop(md);

        auto keyboard_area = area.removeFromBottom(keyboardH);
        area.removeFromBottom(keyboardGap);
        performance_keyboard_.setBounds(keyboard_area);

        auto library_area = area.removeFromRight(libraryWidth);
        library_panel_bounds_ = library_area;
        area.removeFromRight(panelGap);

        status_.setBounds(area.removeFromBottom(statusH));
        area.removeFromBottom(sm);

        const int background_block_h =
            fieldH + xs + fieldH + xs + fieldH;
        auto background_block = area.removeFromBottom(background_block_h);
        area.removeFromBottom(sm);

        auto file_row = area.removeFromTop(toolbarH);
        load_.setBounds(file_row.removeFromLeft(iconButton));
        file_row.removeFromLeft(controlGap);
        save_.setBounds(file_row.removeFromLeft(iconButton));
        file_row.removeFromLeft(controlGap);
        paste_.setBounds(file_row.removeFromLeft(iconButton));
        file_row.removeFromLeft(controlGap);
        copy_.setBounds(file_row.removeFromLeft(iconButton));
        file_row.removeFromLeft(controlGap);
        undo_.setBounds(file_row.removeFromLeft(iconButton));
        file_row.removeFromLeft(controlGap);
        redo_.setBounds(file_row.removeFromLeft(iconButton));
        file_row.removeFromLeft(lg - xs);
        import_wave_.setBounds(file_row.removeFromLeft(UiScale::sx(66)));
        file_row.removeFromLeft(sm);
        import_audacity_.setBounds(file_row.removeFromLeft(UiScale::sx(184)));
        file_row.removeFromLeft(sm);
        open_opll_.setBounds(file_row.removeFromLeft(UiScale::sx(120)));
        file_row.removeFromLeft(sm);
        convert_to_opll_.setBounds(file_row.removeFromLeft(iconButton));
        area.removeFromTop(sm);

        auto preset_row = area.removeFromTop(textButtonH);
        auto preset_bounds = preset_row.removeFromLeft(UiScale::sx(240));
        preset_.setBounds(preset_bounds);
        auto preview_bounds = preset_bounds;
        preview_bounds.removeFromRight(UiScale::sx(24));
        preset_preview_.setBounds(
            preview_bounds.removeFromRight(UiScale::sx(76)).reduced(xs, xs));
        preset_preview_.toFront(false);
        preset_row.removeFromLeft(sm);
        harmonic_.setBounds(preset_row.removeFromLeft(UiScale::sx(230)));
        preset_row.removeFromLeft(sm);
        apply_range_.setBounds(preset_row.removeFromLeft(UiScale::sx(110)));
        preset_row.removeFromLeft(sm);
        apply_preset_.setBounds(preset_row.removeFromLeft(UiScale::sx(70)));
        preset_row.removeFromLeft(controlGap);
        cancel_preview_.setBounds(preset_row.removeFromLeft(UiScale::sx(56)));
        preset_row.removeFromLeft(sm);
        ab_audition_.setBounds(preset_row.removeFromLeft(UiScale::sx(52)));

        area.removeFromTop(sm);
        auto merge_row = area.removeFromTop(textButtonH);
        // Switch widths from measured label + shared track/pad (75/100/125).
        auto place_merge_switch = [&](juce::ToggleButton& toggle) {
            toggle.setBounds(merge_row.removeFromLeft(
                switchControlWidth(
                    toggle.getButtonText(), textButtonH)));
        };
        place_merge_switch(merge_enabled_);
        merge_row.removeFromLeft(sm);
        merge_amount_.setBounds(merge_row.removeFromLeft(UiScale::sx(210)));
        merge_row.removeFromLeft(sm);
        place_merge_switch(auto_phase_);
        merge_row.removeFromLeft(controlGap);
        place_merge_switch(polarity_);
        merge_row.removeFromLeft(controlGap);
        place_merge_switch(preserve_volume_);
        merge_row.removeFromLeft(md);
        place_merge_switch(preset_flip_h_);
        merge_row.removeFromLeft(controlGap);
        place_merge_switch(preset_flip_v_);

        area.removeFromTop(md);
        editor_panel_bounds_ = area;
        const int button_size = iconButton;
        const int cursor_pad_w = button_size * 2 + controlGap;
        const int scale_bar_w = UiScale::sx(62);
        const int tool_column_w =
            cursor_pad_w + controlGap + scale_bar_w;
        auto editor_row = area.reduced(panelPad);
        graph_.setBounds(
            editor_row.withTrimmedRight(tool_column_w + panelGap));
        auto tool_area = editor_row.removeFromRight(tool_column_w);
        auto wave_tools = tool_area.removeFromBottom(button_size)
            .withSizeKeepingCentre(
                button_size * 3 + controlGap * 2, button_size);
        tool_area.removeFromBottom(md);
        average_.setBounds(wave_tools.removeFromLeft(button_size));
        wave_tools.removeFromLeft(controlGap);
        normalize_.setBounds(wave_tools.removeFromLeft(button_size));
        wave_tools.removeFromLeft(controlGap);
        invert_.setBounds(wave_tools.removeFromLeft(button_size));

        // Cross-pad sits immediately above Average / Normalize / Invert.
        const int pad_h = button_size * 3 + controlGap * 2;
        auto cursor_strip = tool_area.removeFromBottom(pad_h);
        auto cursor_area = cursor_strip.removeFromLeft(cursor_pad_w);
        const int pad_step = button_size + controlGap;
        shift_up_.setBounds(
            cursor_area.getCentreX() - button_size / 2,
            cursor_area.getY(),
            button_size,
            button_size);
        rotate_left_.setBounds(
            cursor_area.getX(),
            cursor_area.getY() + pad_step,
            button_size,
            button_size);
        rotate_right_.setBounds(
            cursor_area.getRight() - button_size,
            cursor_area.getY() + pad_step,
            button_size,
            button_size);
        shift_down_.setBounds(
            cursor_area.getCentreX() - button_size / 2,
            cursor_area.getY() + pad_step * 2,
            button_size,
            button_size);

        auto scale_area =
            tool_area.removeFromRight(scale_bar_w).reduced(
                UiScale::sx(5), UiScale::sx(8));
        // Extend the scale bar beside the cross-pad strip.
        scale_area.setBottom(cursor_strip.getBottom());
        scale_reset_.setBounds(scale_area.removeFromBottom(textButtonH));
        scale_area.removeFromBottom(sm);
        vertical_scale_.setBounds(scale_area);

        auto background_tools = background_block.removeFromTop(fieldH);
        background_load_.setBounds(
            background_tools.removeFromLeft(UiScale::sx(96)));
        background_tools.removeFromLeft(controlGap);
        background_clear_.setBounds(
            background_tools.removeFromLeft(UiScale::sx(96)));
        background_tools.removeFromLeft(sm);
        background_visible_.setBounds(
            background_tools.removeFromLeft(
                switchControlWidth(
                    background_visible_.getButtonText(), fieldH)));
        background_tools.removeFromLeft(sm);
        background_opacity_label_.setBounds(
            background_tools.removeFromLeft(UiScale::sx(56)));
        background_opacity_.setBounds(background_tools);
        background_block.removeFromTop(xs);
        auto background_pos = background_block.removeFromTop(fieldH);
        const auto pos_half =
            (background_pos.getWidth() - md) / 2;
        auto x_area = background_pos.removeFromLeft(pos_half);
        background_x_label_.setBounds(x_area.removeFromLeft(UiScale::sx(22)));
        background_x_.setBounds(x_area);
        background_pos.removeFromLeft(md);
        background_y_label_.setBounds(
            background_pos.removeFromLeft(UiScale::sx(22)));
        background_y_.setBounds(background_pos);
        background_block.removeFromTop(xs);
        auto background_size = background_block;
        const auto size_half =
            (background_size.getWidth() - md) / 2;
        auto w_area = background_size.removeFromLeft(size_half);
        background_w_label_.setBounds(w_area.removeFromLeft(UiScale::sx(22)));
        background_width_.setBounds(w_area);
        background_size.removeFromLeft(md);
        background_h_label_.setBounds(
            background_size.removeFromLeft(UiScale::sx(22)));
        background_height_.setBounds(background_size);

        layoutTimbreLibraryPanel(
            library_area,
            TimbreLibraryWidgets{
                library_title_,
                library_filter_,
                library_tag_filter_,
                tag_manage_,
                favorite_only_,
                library_ab_,
                library_sort_,
                library_list_,
                library_load_,
                library_delete_,
                name_,
                library_rename_,
                tags_,
                memo_,
                favorite_,
                library_save_,
                library_save_as_,
                library_new_,
                library_import_,
                library_export_,
                mgsc_title_,
                output_number_label_,
                output_number_,
                mgsc_preview_});
    }

    bool keyPressed(const juce::KeyPress& key) override {
        if (UiScale::tryHandleEditorScaleKey(
                key,
                ui_scale_session_override_,
                [this](int percent) { applyEditorUiScale(percent); })) {
            return true;
        }
        if (isCommandLetter(key, 'v')) {
            pasteClipboard();
            return true;
        }
        if (isCommandLetter(key, 'c')) {
            copyDefinition();
            return true;
        }
        if (isCommandLetter(key, 'y')
            || (isCommandLetter(key, 'z')
                && key.getModifiers().isShiftDown())) {
            redo();
            return true;
        }
        if (isCommandLetter(key, 'z')) {
            undo();
            return true;
        }
        if (performance_keyboard_.shouldConsumeKeyPress(key)) {
            performance_keyboard_.pollPerformanceInput();
            return true;
        }
        return false;
    }

    bool keyStateChanged(bool) override {
        if (!textEntryHasFocusWithin(*this)) {
            performance_keyboard_.pollPerformanceInput();
        }
        return false;
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
        // 0 は未選択。倍音変更で作り直しても選択なしのまま保つ。
        const int selected = hasPresetSelection()
            ? juce::jlimit(1, 6, preset_.getSelectedId())
            : 0;
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

    [[nodiscard]] bool hasPresetSelection() const noexcept {
        const auto value = preset_.getSelectedId();
        return value >= 1 && value <= 6;
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
                scc_wave_,
                *number,
                utf8Text(name_.getText().trim()));
        return juce::String::fromUTF8(
            text.data(),
            static_cast<int>(text.size()));
    }

    void updateDefinitionPreview() {
        if (!outputNumber()) {
            UiFonts::setMgscPreviewText(
                mgsc_preview_,
                juce::String::fromUTF8(
                    "一時出力番号は0～31で入力してください。"));
            return;
        }
        UiFonts::setMgscPreviewText(mgsc_preview_, definitionText());
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
                .getChildFile("scc-tone.txt"),
            "*.txt");
        if (!chooser.browseForFileToSave(true)) {
            return;
        }
        const auto output_file =
            chooser.getResult().withFileExtension(".txt");
        if (!output_file.replaceWithText(
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
                updateBackgroundControlState();
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
            updateBackgroundControlState();
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
        updateBackgroundControlState();
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

    void updateBackgroundControlState() {
        const bool has_background = background_image_.isValid()
            || background_path_.isNotEmpty();
        background_load_.setEnabled(true);
        background_clear_.setEnabled(has_background);
        background_visible_.setEnabled(has_background);
        background_opacity_label_.setEnabled(has_background);
        background_opacity_.setEnabled(has_background);
        background_x_label_.setEnabled(has_background);
        background_x_.setEnabled(has_background);
        background_y_label_.setEnabled(has_background);
        background_y_.setEnabled(has_background);
        background_w_label_.setEnabled(has_background);
        background_width_.setEnabled(has_background);
        background_h_label_.setEnabled(has_background);
        background_height_.setEnabled(has_background);
    }

    void loadBackgroundSettings() {
        const auto file = settingsFile();
        background_path_.clear();
        background_image_ = {};
        if (!file.existsAsFile()) {
            applyBackgroundToGraph();
            updateBackgroundControlState();
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
        updateBackgroundControlState();
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
        juce::Component::SafePointer<SccEditorComponent> safe(this);
        const auto waveform = scc_wave_;
        showOpllConversionQualityDialog(
            this,
            [safe, waveform](OpllConversionQuality quality) {
                if (safe == nullptr) {
                    return;
                }
                safe->updateStatus(juce::String::fromUTF8(
                    "SCC音色をOPLLへ近似変換しています…"));
                safe->audio_service_.setSharedSccWaveform(waveform);
                static_cast<void>(safe->configureEngine(false));
                runWithConversionBusyDialog(
                    safe.getComponent(),
                    [waveform, quality](
                        ConversionProgressState& progress) {
                return mgstc::engine::
                            approximateSccWaveformWithOpllResult(
                                waveform,
                                makeOpllApproximationOptions(
                                    quality, progress));
            },
                    [safe](
                        mgstc::engine::OpllApproximationResult result,
                        bool cancellation_requested) {
                if (safe == nullptr) {
                    return;
                }
                        if (cancellation_requested
                            || result.completion
                                == mgstc::engine::
                                    OpllApproximationCompletion::
                                        Cancelled) {
                            safe->updateStatus(
                                juce::String::fromUTF8(
                                    "OPLL近似変換をキャンセルしました"));
                            return;
                        }
                        if (result.candidates.empty()) {
                            safe->updateStatus(
                                juce::String::fromUTF8(
                        "OPLL近似候補を生成できませんでした"));
                    return;
                }
                const auto definition =
                    mgstc::engine::formatMgsOpllDefinition(
                                result.candidates.front(), 15, {});
                const auto file = pendingConversionFile("opll");
                if (!file.getParentDirectory().createDirectory()
                    || !file.replaceWithText(
                                juce::String::fromUTF8(
                                    definition.c_str()))) {
                            safe->updateStatus(
                                juce::String::fromUTF8(
                        "OPLL変換データを保存できませんでした"));
                    return;
                }
                        safe->open_editor_("opll", std::nullopt);
                        safe->updateStatus(
                            juce::String::fromUTF8(
                                "OPLL近似音色を生成し、"
                                "OPLLエディタへ送りました"));
                    });
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
        return LastAuditionNoteStore::instance().get();
    }

    void saveLastAuditionNoteSetting(std::uint8_t midi_note) {
        LastAuditionNoteStore::instance().mark(midi_note);
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

    void updateTagButtons() {
        tags_.setButtonText(tagSelectionSummary(
            selected_tags_, juce::String::fromUTF8("タグを選択")));
        library_tag_filter_.setButtonText(tagSelectionSummary(
            selected_filter_tags_,
            juce::String::fromUTF8("タグで絞り込み")));
    }

    void showTagEditor() {
        std::vector<std::vector<std::string>> tag_sets;
        for (const auto& entry : library_.entries()) {
            if (entry.category == mgstc::engine::TimbreCategory::Scc) {
                tag_sets.push_back(entry.tags);
            }
        }
        juce::Component::SafePointer<SccEditorComponent> safe(this);
        showTagSelectionDialog(
            this,
            juce::String::fromUTF8("SCC音色のタグ"),
            editorTagChoices(tag_sets, selected_tags_),
            selected_tags_,
            true,
            [safe](std::vector<std::string> selected) {
                if (safe != nullptr) {
                    safe->selected_tags_ = std::move(selected);
                    safe->updateTagButtons();
                }
            });
    }

    void showLibraryTagFilter() {
        std::vector<std::vector<std::string>> tag_sets;
        for (const auto& entry : library_.entries()) {
            if (entry.category == mgstc::engine::TimbreCategory::Scc) {
                tag_sets.push_back(entry.tags);
            }
        }
        juce::Component::SafePointer<SccEditorComponent> safe(this);
        showTagSelectionDialog(
            this,
            juce::String::fromUTF8("SCCライブラリのタグ検索"),
            filterTagChoices(tag_sets, selected_filter_tags_),
            selected_filter_tags_,
            false,
            [safe](std::vector<std::string> selected) {
                if (safe != nullptr) {
                    safe->selected_filter_tags_ = std::move(selected);
                    safe->updateTagButtons();
                    safe->refreshLibraryList();
                }
            });
    }

    void refreshLibraryList() {
        library_list_.clear(juce::dontSendNotification);
        library_ids_.clear();
        const auto filter = library_filter_.getText().trim();
        int item_id = 1;
        int selected_item = 0;
        std::vector<const mgstc::engine::TimbreLibraryEntry*> ordered;
        for (const auto& entry : library_.entries()) {
            if (entry.category == mgstc::engine::TimbreCategory::Scc
                && (!favorite_only_.getToggleState()
                    || entry.favorite)) {
                ordered.push_back(&entry);
            }
        }
        const int sort_mode = library_sort_.getSelectedId();
        std::stable_sort(
            ordered.begin(), ordered.end(),
            [sort_mode](const auto* left, const auto* right) {
                if (left->favorite != right->favorite) {
                    return left->favorite;
                }
                if (sort_mode == 2
                    && left->last_used_unix_seconds
                        != right->last_used_unix_seconds) {
                    return left->last_used_unix_seconds
                        > right->last_used_unix_seconds;
                }
                if (sort_mode == 3
                    && left->updated_unix_seconds
                        != right->updated_unix_seconds) {
                    return left->updated_unix_seconds
                        > right->updated_unix_seconds;
                }
                if (sort_mode == 4) {
                    return left->name < right->name;
                }
                return false;
            });
        for (const auto* entry_ptr : ordered) {
            if (!entry_ptr) {
                continue;
            }
            const auto& entry = *entry_ptr;
            if (filter.isNotEmpty()) {
                const auto name =
                    juce::String::fromUTF8(entry.name.c_str());
                const auto tags =
                    juce::String::fromUTF8(
                        mgstc::engine::serializeTimbreTags(
                            entry.tags).c_str());
                const auto memo =
                    juce::String::fromUTF8(entry.memo.c_str());
                if (!name.containsIgnoreCase(filter)
                    && !tags.containsIgnoreCase(filter)
                    && !memo.containsIgnoreCase(filter)) {
                    continue;
                }
            }
            if (!mgstc::engine::containsAllTimbreTags(
                    entry.tags, selected_filter_tags_)) {
                continue;
            }
            library_ids_.push_back(entry.id);
            auto label = juce::String::fromUTF8(entry.name.c_str())
                + " (r"
                + juce::String(static_cast<int>(entry.revision))
                + ")";
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
        updateDefinitionPreview();
        selected_tags_ = entry->tags;
        updateTagButtons();
        memo_.setText(
            juce::String::fromUTF8(entry->memo.c_str()), false);
        favorite_.setToggleState(
            entry->favorite, juce::dontSendNotification);
        library_preview_ = *entry;
        library_ab_.setEnabled(true);
        library_ab_next_b_ = false;
        SccWaveform preview{};
        std::transform(
            entry->scc_waveform.begin(),
            entry->scc_waveform.end(),
            preview.begin(),
            [](std::uint8_t value) {
                return static_cast<std::int8_t>(value);
            });
        static_cast<void>(auditionOneSecond(&preview));
        static_cast<void>(
            library_.touch(entry->id, unixTimeNow()));
        static_cast<void>(persistLibrary());
        updateStatus(
            juce::String::fromUTF8("音色を選択しました。"
                                   "「読込」で波形へ反映します"));
    }

    void auditionLibraryAb() {
        if (!library_preview_) {
            return;
        }
        const bool play_b = library_ab_next_b_;
        library_ab_next_b_ = !library_ab_next_b_;
        if (play_b) {
            SccWaveform preview{};
            std::transform(
                library_preview_->scc_waveform.begin(),
                library_preview_->scc_waveform.end(),
                preview.begin(),
                [](std::uint8_t value) {
                    return static_cast<std::int8_t>(value);
                });
            static_cast<void>(auditionOneSecond(&preview));
        } else {
            static_cast<void>(auditionOneSecond());
        }
        library_ab_.setButtonText(abNextSideButtonText(play_b));
        updateStatus(
            play_b
                ? juce::String::fromUTF8("B 保存音色を試聴中")
                : juce::String::fromUTF8("A 編集中音色を試聴中"));
    }

    void newLibraryEntry() {
        if (hasUnsavedChanges()) {
            juce::Component::SafePointer<SccEditorComponent> safe(this);
            showDiscardConfirmation(
                this,
                juce::String::fromUTF8("新規作成"),
                [safe] {
                    if (safe != nullptr) {
                        safe->performNewLibraryEntry();
                    }
                });
            return;
        }
        performNewLibraryEntry();
    }

    void performNewLibraryEntry() {
        selected_library_id_.reset();
        library_list_.setSelectedId(0, juce::dontSendNotification);
        name_.clear();
        selected_tags_.clear();
        updateTagButtons();
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
        if (hasUnsavedChanges()) {
            juce::Component::SafePointer<SccEditorComponent> safe(this);
            showDiscardConfirmation(
                this,
                juce::String::fromUTF8("選択音色の読込"),
                [safe] {
                    if (safe != nullptr) {
                        safe->performLoadSelectedLibraryEntry();
                    }
                });
            return;
        }
        performLoadSelectedLibraryEntry();
    }

    void performLoadSelectedLibraryEntry() {
        if (!selected_library_id_) {
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
        static_cast<void>(
            library_.touch(id, unixTimeNow()));
        static_cast<void>(persistLibrary());
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
        entry.tags = selected_tags_;
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
        if (!saved) {
            showError(
                juce::String::fromUTF8("音色ライブラリ"),
                juce::String::fromUTF8(
                    "音色はセッションへ保存されましたが、"
                    "ライブラリファイルを更新できませんでした"));
            return;
        }
        setEditorBaseline();
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

    void renameSelectedLibraryEntry() {
        if (!selected_library_id_) {
            showError(
                juce::String::fromUTF8("音色ライブラリ"),
                juce::String::fromUTF8(
                    "名前を変更する音色を選択してください"));
            return;
        }
        const auto requested = utf8Text(name_.getText().trim());
        if (requested.empty()) {
            showError(
                juce::String::fromUTF8("音色ライブラリ"),
                juce::String::fromUTF8("新しい名前を入力してください"));
            return;
        }
        juce::InterProcessLock::ScopedLockType lock(library_lock_);
        if (!lock.isLocked() || !reloadLibraryFromDisk()) {
            showError(
                juce::String::fromUTF8("音色ライブラリ"),
                juce::String::fromUTF8(
                    "共有ライブラリを読み直せませんでした"));
            return;
        }
        const auto* source = library_.find(*selected_library_id_);
        if (!source) {
            showError(
                juce::String::fromUTF8("音色ライブラリ"),
                juce::String::fromUTF8(
                    "選択した音色は削除されています"));
            return;
        }
        auto renamed = *source;
        renamed.name = requested;
        if (!library_.update(
                *selected_library_id_, renamed, unixTimeNow())
            || !persistLibrary()) {
            showError(
                juce::String::fromUTF8("音色ライブラリ"),
                juce::String::fromUTF8(
                    "名前の変更を保存できませんでした"));
            return;
        }
        refreshLibraryList();
        selectLibraryEntryFromList();
        setEditorBaseline();
        updateStatus(juce::String::fromUTF8("音色名を変更しました"));
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
        const auto impact = inspectCompositeTimbreImpact(id);
        if (!impact.readable || !impact.uses.empty()) {
            showError(
                juce::String::fromUTF8("音色ライブラリ"),
                !impact.readable
                    ? juce::String::fromUTF8(
                          "総合音色ライブラリを確認できないため、"
                          "安全に削除できません")
                    : juce::String::fromUTF8(
                          "総合音色から参照中のため削除できません。\n\n")
                        + describeCompositeTimbreImpact(impact));
            return;
        }
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
                safe->selected_tags_.clear();
                safe->updateTagButtons();
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
        if (!hasPresetSelection()) {
            updateStatus(
                juce::String::fromUTF8(
                    "プリセットを選んでから適用してください"));
            return;
        }
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

    // プリセットが未選択の間は候補波形を作らず、適用もできない状態にする。
    void clearPresetSelectionState() {
        preview_wave_ = scc_wave_;
        dismissPreviewState(true);
        preset_preview_.clear();
        apply_preset_.setEnabled(false);
    }

    void refreshPresetPreview(bool audition = true) {
        if (!hasPresetSelection()) {
            clearPresetSelectionState();
            return;
        }
        apply_preset_.setEnabled(true);
        preview_wave_ = makePresetCandidate();
        preview_active_ = true;
        // プリセット選択直後は候補(B)を鍵盤でも確認できる状態にする。
        // A/Bボタンの次の操作は原音(A)への切り替えとなる。
        ab_keyboard_plays_b_ = true;
        ab_next_plays_b_ = false;
        cancel_preview_.setEnabled(true);
        ab_audition_.setEnabled(true);
        updateAbAuditionButton();
        preset_preview_.setWaveform(makePresetPreviewWave());
        graph_.setApplyRange(selectedApplyRange());
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
        ab_keyboard_plays_b_ = play_b;
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
        ab_keyboard_plays_b_ = false;
        ab_next_plays_b_ = true;
        cancel_preview_.setEnabled(false);
        ab_audition_.setEnabled(false);
        updateAbAuditionButton();
        graph_.setApplyRange(mgstc::engine::SccApplyRange::All);
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
                : abNextSideButtonText(!ab_next_plays_b_));
        ab_audition_.setTooltip(tip);
        ab_audition_.setTitle(tip);
        ab_audition_.setDescription(tip);
    }

    [[nodiscard]] const SccWaveform* keyboardPreviewWave() const noexcept {
        return preview_active_ && ab_keyboard_plays_b_
            ? &preview_wave_
            : nullptr;
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
        const auto configured = audio_service_.submitSharedEditorProgram(
            waveform,
            audio_service_.sharedOpllPatch(),
            retrigger,
            kSccTrack,
            last_audition_note_);
        if (configured && preview == nullptr) {
            applied_shared_scc_revision_ =
                audio_service_.sharedSccWaveRevision();
            applied_shared_opll_revision_ =
                audio_service_.sharedOpllPatchRevision();
        }
        return configured;
    }

    void startPerformanceNote(std::uint8_t note) {
        audition_stop_time_ms_.reset();
        performance_keyboard_.clearPreviewNote();
        last_audition_note_ = note;
        saveLastAuditionNoteSetting(last_audition_note_);
        if (!engine_ready_) {
            return;
        }
        if (preview_active_) {
            // A/Bで現在選ばれている側を鍵盤でも確認する。
            // Polyの追加ノートでは hardReset しない。
            if (voice_allocator_.activeVoiceCount() == 0
                && !configureEngine(false, keyboardPreviewWave())) {
                return;
            }
        } else if (
            engine_holds_temporary_program_
            && !keep_temporary_program_on_note_on_
            && !configureEngine(false)) {
            // 一時試聴（1秒／A/B）が残っていると鍵盤が未確定音色を
            // 鳴らすため、プレビュー中以外は確定へ戻す。
            // 波形ドラッグ中の張り直しでは一時波形を維持する。
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
            return;
        }
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
    }

    void stopNote() {
        silenceAllVoices();
        if (preview_active_) {
            // 現在のA/B側を維持し、続けて鍵盤で確認できるようにする。
            static_cast<void>(configureEngine(
                false, keyboardPreviewWave()));
        } else if (engine_holds_temporary_program_) {
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
        // 即時発声OFFでも候補／確定をエンジンへ載せ、鍵盤で確認できる
        // ようにする。共有SCC波形は確定時だけ更新する。
        if (preview != nullptr) {
            return configureEngine(false, preview);
        }
        return configureEngine(false);
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
            if (preview_active_) {
                static_cast<void>(configureEngine(
                    false, keyboardPreviewWave()));
            } else if (engine_holds_temporary_program_) {
                static_cast<void>(configureEngine(false));
            }
        }
    }

    void updateStatus(const juce::String& text) {
        status_.setText(text, juce::dontSendNotification);
    }

    EditorOpenCallback open_editor_;
    TagManagementCallback manage_tags_;
    SharedAudioService& audio_service_;
    mgstc::engine::RealtimeEngineHost& engine_;
    juce::TooltipWindow tooltip_window_;
    SwitchLookAndFeel switch_look_and_feel_;
    SccWaveform scc_wave_{};
    std::optional<int> ui_scale_session_override_;
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
    juce::Rectangle<int> editor_panel_bounds_;
    juce::Rectangle<int> library_panel_bounds_;
    juce::Label library_title_;
    juce::ComboBox library_list_;
    juce::TextEditor library_filter_;
    juce::TextButton library_tag_filter_;
    juce::TextButton tag_manage_;
    juce::ToggleButton favorite_only_;
    juce::TextButton library_ab_;
    juce::ComboBox library_sort_;
    juce::TextEditor name_;
    juce::TextButton tags_;
    juce::TextEditor memo_;
    juce::ToggleButton favorite_;
    juce::TextButton library_new_;
    juce::TextButton library_load_;
    juce::TextButton library_save_;
    juce::TextButton library_save_as_;
    juce::TextButton library_rename_;
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
    std::optional<mgstc::engine::TimbreLibraryEntry>
        library_preview_;
    bool library_ab_next_b_{true};
    std::vector<std::string> selected_tags_;
    std::vector<std::string> selected_filter_tags_;
    mgstc::engine::TimbreLibraryEntry editor_baseline_;
    std::optional<std::uint64_t> editor_baseline_id_;
    juce::InterProcessLock library_lock_{
        "MgsToneCraftTimbreLibraryV1"};
    bool editor_baseline_valid_{};
    bool preview_active_{};
    bool ab_keyboard_plays_b_{};
    bool ab_next_plays_b_{true};
    bool scale_previewing_{};
    bool engine_ready_{};
    bool engine_holds_temporary_program_{};
    bool keep_temporary_program_on_note_on_{};
    std::uint64_t applied_shared_scc_revision_{};
    std::uint64_t applied_shared_opll_revision_{};
    std::optional<std::uint64_t> library_manager_performance_id_;
    std::optional<SccWaveform> library_manager_preview_wave_;
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
        graphics.setFont(UiFonts::dense());
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

        graphics.setFont(UiFonts::dense());
        graphics.setColour(juce::Colour(0xFF9BA8B2));
        graphics.drawText(
            "0s",
            getLocalBounds().reduced(UiScale::sx(5)).removeFromBottom(
                UiScale::sx(15)),
            juce::Justification::bottomLeft);
        graphics.drawText(
            "4s",
            getLocalBounds().reduced(UiScale::sx(5)).removeFromBottom(
                UiScale::sx(15)),
            juce::Justification::bottomRight);
        graphics.setColour(juce::Colour(0xFFD98A8A));
        graphics.drawText(
            "KO 1s",
            juce::Rectangle<int>(
                juce::roundToInt(key_off_x) + UiScale::sx(4),
                getHeight() - UiScale::sx(21),
                UiScale::sx(42),
                UiScale::sx(16)),
            juce::Justification::centredLeft);

        constexpr std::array<const char*, 4> section_names{
            "A / AR", "D / DR", "S / SL", "R / RR"};
        const float section_width = graph.getWidth() / 4.0F;
        graphics.setFont(UiFonts::dense(true));
        for (std::size_t index = 0;
             index < section_names.size();
             ++index) {
            const auto section = juce::Rectangle<float>(
                graph.getX()
                    + section_width * static_cast<float>(index),
                graph.getY(),
                section_width,
                static_cast<float>(UiScale::sx(18)));
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
        title_.setFont(UiFonts::heading());
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
        envelope_title_.setFont(UiFonts::body(true));
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
        const auto radius =
            static_cast<float>(UiScale::sx(9));
        graphics.setColour(juce::Colour(0xFF29323C));
        graphics.fillRoundedRectangle(bounds, radius);
        graphics.setColour(juce::Colour(0xFF435160));
        graphics.drawRoundedRectangle(bounds, radius, 1.0F);
    }

    void resized() override {
        title_.setFont(UiFonts::heading());
        envelope_title_.setFont(UiFonts::body(true));
        for (auto* label :
             {&multiplier_label_,
              &key_scale_level_label_,
              &attack_rate_label_,
              &decay_rate_label_,
              &sustain_level_label_,
              &release_rate_label_}) {
            label->setFont(UiFonts::body());
        }
        auto area = getLocalBounds().reduced(UiScale::sx(14));
        title_.setBounds(area.removeFromTop(UiScale::sx(28)));
        area.removeFromTop(UiScale::sx(6));
        auto flag_area = area.removeFromTop(UiScale::sx(30));
        const auto flag_width =
            juce::jmax(UiScale::sx(54), flag_area.getWidth() / 5);
        for (auto& flag : flags_) {
            flag.setBounds(
                flag_area.removeFromLeft(flag_width));
        }
        area.removeFromTop(UiScale::sx(10));
        auto basic_row = area.removeFromTop(UiScale::sx(36));
        const auto basic_gap = UiScale::sx(8);
        const auto basic_width =
            (basic_row.getWidth() - basic_gap) / 2;
        auto multiplier_area = basic_row.removeFromLeft(basic_width);
        multiplier_label_.setBounds(
            multiplier_area.removeFromLeft(UiScale::sx(48)));
        multiplier_.setBounds(multiplier_area);
        basic_row.removeFromLeft(basic_gap);
        key_scale_level_label_.setBounds(
            basic_row.removeFromLeft(UiScale::sx(42)));
        key_scale_level_.setBounds(basic_row);
        area.removeFromTop(UiScale::sx(10));

        auto graph_area = area.removeFromBottom(UiScale::sx(112));
        area.removeFromBottom(UiScale::sx(8));
        auto envelope = area;
        const int gap = UiScale::sx(8);
        const auto column_width =
            (envelope.getWidth() - gap * 3) / 4;
        const int label_h = UiScale::sx(24);
        const int text_box_w = UiScale::sx(46);
        const int text_box_h = UiScale::sx(24);
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
            labels[index]->setBounds(column.removeFromTop(label_h));
            sliders[index]->setTextBoxStyle(
                juce::Slider::TextBoxBelow,
                false,
                text_box_w,
                text_box_h);
            sliders[index]->setBounds(column);
            if (index + 1 < sliders.size()) {
                envelope.removeFromLeft(gap);
            }
        }
        envelope_title_.setBounds(
            graph_area.removeFromTop(UiScale::sx(20)));
        graph_area.removeFromTop(UiScale::sx(3));
        envelope_graph_.setBounds(graph_area);

        // Horizontal MULT / KSL text boxes also follow scale.
        multiplier_.setTextBoxStyle(
            juce::Slider::TextBoxRight,
            false,
            text_box_w,
            text_box_h);
        key_scale_level_.setTextBoxStyle(
            juce::Slider::TextBoxRight,
            false,
            text_box_w,
            text_box_h);
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
        label->setFont(UiFonts::body());
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
            UiScale::sx(46),
            UiScale::sx(24));
        // Text box Label is created lazily; apply after first layout via LAF.
        slider.setColour(
            juce::Slider::textBoxTextColourId,
            juce::Colour(0xFFF2F4F5));
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
        EditorOpenCallback open_editor,
        TagManagementCallback manage_tags)
        : open_editor_(std::move(open_editor)),
          manage_tags_(std::move(manage_tags)),
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
        title_.setFont(UiFonts::title());
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
                "YM2413オリジナル音色のパラメーターを編集します。"
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
        common_parameters_title_.setFont(UiFonts::heading());
        addAndMakeVisible(common_parameters_title_);
        const auto configure_common_slider = [this](
            juce::Label& label,
            juce::Slider& slider,
            const juce::String& text,
            double maximum) {
            label.setText(text, juce::dontSendNotification);
            label.setFont(UiFonts::body());
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
        open_scc_.onClick = [this] {
            open_editor_("scc", std::nullopt);
        };
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
        scope_title_.setFont(UiFonts::heading());
        addAndMakeVisible(scope_title_);
        scope_.setSynchronized(true);
        addAndMakeVisible(scope_);

        output_number_label_.setText(
            juce::String::fromUTF8("一時出力番号"),
            juce::dontSendNotification);
        addAndMakeVisible(output_number_label_);
        UiFonts::styleBodyField(output_number_);
        output_number_.setInputRestrictions(2, "0123456789");
        output_number_.setText("15", false);
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
        mgsc_title_.setFont(UiFonts::heading());
        addAndMakeVisible(mgsc_title_);
        mgsc_preview_.setMultiLine(true);
        mgsc_preview_.setReadOnly(true);
        mgsc_preview_.setWantsKeyboardFocus(false);
        mgsc_preview_.setFont(UiFonts::mono());
        addAndMakeVisible(mgsc_preview_);

        library_title_.setText(
            juce::String::fromUTF8("音色ライブラリ"),
            juce::dontSendNotification);
        library_title_.setFont(UiFonts::heading());
        addAndMakeVisible(library_title_);
        library_filter_.setTextToShowWhenEmpty(
            juce::String::fromUTF8("名前・タグ・メモを検索"),
            juce::Colour(0xFF7F8993));
        library_filter_.setTooltip(
            juce::String::fromUTF8("OPLL音色ライブラリを絞り込みます"));
        library_filter_.onTextChange =
            [this] { refreshLibraryList(); };
        UiFonts::styleBodyField(library_filter_);
        addAndMakeVisible(library_filter_);
        library_tag_filter_.setButtonText(
            juce::String::fromUTF8("タグで絞り込み"));
        library_tag_filter_.setTooltip(
            juce::String::fromUTF8(
                "保存済み音色で使われているタグを複数選択します"));
        library_tag_filter_.onClick = [this] {
            showLibraryTagFilter();
        };
        addAndMakeVisible(library_tag_filter_);
        tag_manage_.setButtonText(
            juce::String::fromUTF8("ライブラリ管理"));
        tag_manage_.setTooltip(
            juce::String::fromUTF8(
                "音色ライブラリの一覧・複製・削除・タグ管理を行います"));
        tag_manage_.onClick = [this] {
            if (manage_tags_) {
                manage_tags_(this);
            }
        };
        addAndMakeVisible(tag_manage_);
        favorite_only_.setButtonText(
            juce::String::fromUTF8("★のみ"));
        favorite_only_.setLookAndFeel(&switch_look_and_feel_);
        favorite_only_.onClick = [this] { refreshLibraryList(); };
        addAndMakeVisible(favorite_only_);
        library_ab_.setButtonText("A/B");
        library_ab_.setTooltip(
            juce::String::fromUTF8(
                "編集中(A)と選択した保存音色(B)を交互に試聴します"));
        library_ab_.setEnabled(false);
        library_ab_.onClick = [this] { auditionLibraryAb(); };
        addAndMakeVisible(library_ab_);
        library_sort_.addItem(
            juce::String::fromUTF8("★優先"), 1);
        library_sort_.addItem(
            juce::String::fromUTF8("最近使った順"), 2);
        library_sort_.addItem(
            juce::String::fromUTF8("更新日時順"), 3);
        library_sort_.addItem(
            juce::String::fromUTF8("名前順"), 4);
        library_sort_.setSelectedId(1, juce::dontSendNotification);
        library_sort_.onChange = [this] { refreshLibraryList(); };
        addAndMakeVisible(library_sort_);
        library_list_.setTextWhenNothingSelected(
            juce::String::fromUTF8("保存済み音色を選択"));
        library_list_.setTooltip(
            juce::String::fromUTF8("OPLL音色ライブラリ"));
        library_list_.onChange =
            [this] { selectLibraryEntryFromList(); };
        addAndMakeVisible(library_list_);
        name_.setTextToShowWhenEmpty(
            juce::String::fromUTF8("音色名"),
            juce::Colour(0xFF7F8993));
        name_.onTextChange = [this] { updateDefinitionPreview(); };
        tags_.setButtonText(juce::String::fromUTF8("タグを選択"));
        tags_.setTooltip(
            juce::String::fromUTF8(
                "標準タグを複数選択、または独自タグを追加します"));
        tags_.onClick = [this] { showTagEditor(); };
        memo_.setTextToShowWhenEmpty(
            juce::String::fromUTF8("メモ"),
            juce::Colour(0xFF7F8993));
        memo_.setMultiLine(true);
        memo_.setReturnKeyStartsNewLine(true);
        UiFonts::styleBodyField(name_);
        addAndMakeVisible(name_);
        addAndMakeVisible(tags_);
        UiFonts::styleBodyField(memo_);
        addAndMakeVisible(memo_);
        favorite_.setButtonText(
            juce::String::fromUTF8("★お気に入り"));
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
            juce::String::fromUTF8("別名保存"),
            juce::String::fromUTF8("新しい音色として保存します"),
            [this] { saveLibraryEntry(true); });
        configureButton(
            library_rename_, juce::String::fromUTF8("名前変更"),
            juce::String::fromUTF8(
                "名前欄の内容へ選択音色の名前だけを変更します"),
            [this] { renameSelectedLibraryEntry(); });
        configureButton(
            library_delete_, juce::String::fromUTF8("削除"),
            juce::String::fromUTF8("選択音色を削除します"),
            [this] { deleteSelectedLibraryEntry(); });
        configureButton(
            library_import_, juce::String::fromUTF8("取込"),
            juce::String::fromUTF8("外部.mgstcライブラリを取り込みます"),
            [this] { importLibraryFile(); });
        configureButton(
            library_export_, juce::String::fromUTF8("書出"),
            juce::String::fromUTF8("選択音色を.mgstcへ書き出します"),
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
        // Tab: Library (100+) → editor/toolbar (200+) → scope (300+) → keyboard (400+).
        assignExplicitFocusOrders(
            {
                &library_filter_,
                &library_tag_filter_,
                &tag_manage_,
                &favorite_only_,
                &library_ab_,
                &library_sort_,
                &library_list_,
                &library_load_,
                &library_delete_,
                &name_,
                &library_rename_,
                &tags_,
                &memo_,
                &favorite_,
                &library_save_,
                &library_save_as_,
                &library_new_,
                &library_import_,
                &library_export_,
                &output_number_,
            },
            100);
        assignExplicitFocusOrders(
            {
                &immediate_audition_,
                &master_volume_,
                &settings_,
                &load_,
                &save_,
                &paste_,
                &copy_,
                &undo_,
                &redo_,
                &import_wave_,
                &import_audacity_,
                &wave_previous_,
                &wave_next_,
                &open_scc_,
                &convert_to_scc_,
                &rom_preset_,
                &rom_load_,
                &mod_total_level_,
                &feedback_,
                &modulator_,
                &carrier_,
            },
            200);
        assignExplicitFocusOrders(
            {
                &scope_,
            },
            300);
        assignExplicitFocusOrders(
            {
                &performance_keyboard_,
            },
            400);
        setSize(UiLayout::editorWindowW, UiLayout::editorWindowH);
        {
            juce::Component::SafePointer<OpllEditorComponent> safe(this);
            UiScale::addGlobalListener([safe] {
                if (safe != nullptr) {
                    safe->onGlobalUiScaleChanged();
                }
            });
        }
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
        favorite_only_.setLookAndFeel(nullptr);
        silenceAllVoices();
    }

    void prepareVisualInspection() {
        static_cast<void>(auditionOneSecond());
    }

    [[nodiscard]] SnapshotResult captureSettingsTab(
        int initial_tab,
        const juce::File& output_file) {
        return performance_keyboard_.captureSettingsTab(
            initial_tab, output_file);
    }

    void refreshExternalState() {
        synchronizeMasterVolumeSlider(
            master_volume_, master_volume_revision_, audio_service_);
        // pending取込が試聴を始めた直後に configureEngine(false) すると
        // hardResetで音が消えるため、取込成功時は再構成しない。
        if (consumePendingOpllConversion() || !engine_ready_) {
            return;
        }
        if (!needsSharedEngineReconfigure()) {
            return;
        }
        static_cast<void>(configureEngine(false));
    }

    [[nodiscard]] bool needsSharedEngineReconfigure() const {
        if (engine_holds_temporary_program_) {
            return true;
        }
        if (!audio_service_.sharedEditorProgramActive()) {
            return true;
        }
        if (patch_ != audio_service_.sharedOpllPatch()) {
            return true;
        }
        return applied_shared_scc_revision_
                != audio_service_.sharedSccWaveRevision()
            || applied_shared_opll_revision_
                != audio_service_.sharedOpllPatchRevision();
    }

    void applyTagRewrite(
        std::string_view source,
        std::string_view replacement) {
        if (editor_baseline_valid_) {
            static_cast<void>(mgstc::engine::rewriteTimbreTag(
                editor_baseline_.tags, source, replacement));
        }
        const bool editing_changed =
            mgstc::engine::rewriteTimbreTag(
                selected_tags_, source, replacement);
        static_cast<void>(mgstc::engine::rewriteTimbreTag(
            selected_filter_tags_, source, replacement));
        if (library_preview_) {
            static_cast<void>(mgstc::engine::rewriteTimbreTag(
                library_preview_->tags, source, replacement));
        }
        loadLibrary();
        updateTagButtons();
        refreshLibraryList();
        if (editing_changed && hasUnsavedChanges()) {
            updateStatus(
                juce::String::fromUTF8(
                    "独自タグの変更を編集中のOPLL音色へ反映しました"
                    "（未保存）"));
        }
    }

    void requestLibraryEntry(std::uint64_t id) {
        {
            juce::InterProcessLock::ScopedLockType lock(library_lock_);
            if (!lock.isLocked() || !reloadLibraryFromDisk()
                || !library_.find(id)) {
                showError(
                    juce::String::fromUTF8("OPLL音色ライブラリ"),
                    juce::String::fromUTF8(
                        "割当音色を読み込めませんでした"));
                return;
            }
        }
        selected_library_id_ = id;
        refreshLibraryList();
        loadSelectedLibraryEntry();
    }

    void auditionLibraryPreview(std::uint64_t id) {
        juce::InterProcessLock::ScopedLockType lock(library_lock_);
        if (!lock.isLocked() || !reloadLibraryFromDisk()) {
            return;
        }
        const auto* entry = library_.find(id);
        if (entry == nullptr
            || entry->category != mgstc::engine::TimbreCategory::Opll) {
            return;
        }
        last_audition_note_ = loadLastAuditionNoteSetting();
        const auto preview =
            mgstc::engine::decodeOpllPatch(entry->opll_registers);
        static_cast<void>(auditionOneSecond(&preview));
        static_cast<void>(library_.touch(id, unixTimeNow()));
        static_cast<void>(persistLibrary());
    }

    void libraryManagerNoteOn(std::uint64_t id, std::uint8_t note) {
        juce::InterProcessLock::ScopedLockType lock(library_lock_);
        if (!lock.isLocked() || !reloadLibraryFromDisk()) {
            return;
        }
        const auto* entry = library_.find(id);
        if (entry == nullptr
            || entry->category != mgstc::engine::TimbreCategory::Opll) {
            return;
        }
        const auto preview =
            mgstc::engine::decodeOpllPatch(entry->opll_registers);
        audition_stop_time_ms_.reset();
        performance_keyboard_.clearPreviewNote();
        last_audition_note_ = note;
        saveLastAuditionNoteSetting(last_audition_note_);
        updateAuditionNoteLabels();
        pending_envelope_trace_refresh_ = true;
        if (!engine_ready_) {
            return;
        }
        if (library_manager_performance_id_ != id
            || voice_allocator_.activeVoiceCount() == 0) {
            if (!configureEngine(false, &preview)) {
                return;
            }
            library_manager_performance_id_ = id;
            library_manager_preview_patch_ = preview;
        }
        const auto assignment = voice_allocator_.noteOn(note);
        const auto track = static_cast<std::uint8_t>(
            kOpllTrack + assignment.channel);
        audio_service_.cancelOpllKeyOffForceSilence(track);
        if (assignment.stolen_note) {
            static_cast<void>(engine_.submit(
                mgstc::engine::EngineCommand::noteOff(track)));
        }
        if (!engine_.submit(
                mgstc::engine::EngineCommand::noteOn(
                    track, note))) {
            static_cast<void>(voice_allocator_.noteOff(note));
            return;
        }
    }

    void libraryManagerNoteOff(std::uint8_t note) {
        stopPerformanceNote(note);
        if (voice_allocator_.activeVoiceCount() == 0) {
            library_manager_performance_id_.reset();
            library_manager_preview_patch_.reset();
        }
    }

    void libraryManagerAllNotesOff() {
        silenceAllVoices();
        library_manager_performance_id_.reset();
        library_manager_preview_patch_.reset();
    }

    [[nodiscard]] bool hasUnsavedChanges() const {
        return editor_baseline_valid_
            && (selected_library_id_ != editor_baseline_id_
                || !sameEditableTimbreLibraryEntry(
                    captureLibraryEntry(), editor_baseline_));
    }

    void deactivate() {
        performance_keyboard_.allNotesOff();
        silenceAllVoices();
        library_manager_performance_id_.reset();
        library_manager_preview_patch_.reset();
    }


    [[nodiscard]] int effectiveUiScalePercent() const noexcept {
        return ui_scale_session_override_.value_or(UiScale::global_percent);
    }

    void syncProcessUiScale() {
        UiScale::setActivePercent(effectiveUiScalePercent());
    }

    void applyPreferredSizeForEffectiveScale() {
        applyEditorUiScale(effectiveUiScalePercent());
    }

    void applyEditorUiScale(int percent) {
        UiScale::setActivePercent(percent);
        title_.setFont(UiFonts::title());
        description_.setFont(UiFonts::body());
        library_title_.setFont(UiFonts::heading());
        mgsc_title_.setFont(UiFonts::heading());
        scope_title_.setFont(UiFonts::heading());
        common_parameters_title_.setFont(UiFonts::heading());
        UiFonts::styleBodyField(library_filter_);
        UiFonts::styleBodyField(name_);
        UiFonts::styleBodyField(memo_);
        UiFonts::styleBodyField(output_number_);
        UiFonts::refreshMgscPreviewFont(mgsc_preview_);
        applyScaledContentSize(
            *this,
            UiLayout::editorWindowW,
            UiLayout::editorWindowH,
            true);
    }

    void onGlobalUiScaleChanged() {
        ui_scale_session_override_.reset();
        applyEditorUiScale(UiScale::global_percent);
    }

    void paint(juce::Graphics& graphics) override {
        UiScale::syncEditorDrivenActivePercent(
            *this, effectiveUiScalePercent());
        paintPageBackground(graphics, getLocalBounds());
        paintRoundedPanelFrame(graphics, library_panel_bounds_);
        paintRoundedPanelFrame(graphics, common_parameter_bounds_);
    }

    void resized() override {
        UiScale::syncEditorDrivenActivePercent(
            *this, effectiveUiScalePercent());
        using namespace UiLayout;
        auto area = getLocalBounds().reduced(pageMargin);
        title_.setBounds(area.removeFromTop(titleH));
        description_.setBounds(area.removeFromTop(descriptionH));
        layoutEditorTopRightChrome(
            getWidth(),
            settings_,
            master_volume_,
            &immediate_audition_);
        area.removeFromTop(sm);

        auto keyboard_area = area.removeFromBottom(keyboardH);
        area.removeFromBottom(keyboardGap);
        performance_keyboard_.setBounds(keyboard_area);
        auto library_area = area.removeFromRight(libraryWidth);
        area.removeFromRight(panelGap);
        library_panel_bounds_ = library_area;
        auto toolbar = area.removeFromTop(toolbarH);
        for (auto* button :
             std::array<juce::DrawableButton*, 6>{
                 &load_, &save_, &paste_, &copy_, &undo_, &redo_}) {
            button->setBounds(
                toolbar.removeFromLeft(iconButton));
            toolbar.removeFromLeft(controlGap);
        }
        toolbar.removeFromLeft(controlGap);
        import_wave_.setBounds(
            toolbar.removeFromLeft(UiScale::sx(70)));
        toolbar.removeFromLeft(controlGap);
        import_audacity_.setBounds(
            toolbar.removeFromLeft(UiScale::sx(154)));
        toolbar.removeFromLeft(md);
        wave_previous_.setBounds(
            toolbar.removeFromLeft(iconButton));
        wave_candidate_label_.setBounds(
            toolbar.removeFromLeft(UiScale::sx(112)));
        wave_next_.setBounds(
            toolbar.removeFromLeft(iconButton));
        toolbar.removeFromLeft(sm);
        open_scc_.setBounds(
            toolbar.removeFromLeft(UiScale::sx(120)));
        toolbar.removeFromLeft(sm);
        convert_to_scc_.setBounds(
            toolbar.removeFromLeft(iconButton));
        area.removeFromTop(sm);

        auto preset_row = area.removeFromTop(textButtonH);
        rom_preset_.setBounds(
            preset_row.removeFromLeft(UiScale::sx(230)));
        preset_row.removeFromLeft(sm);
        rom_load_.setBounds(
            preset_row.removeFromLeft(UiScale::sx(150)));
        area.removeFromTop(md);

        common_parameter_bounds_ = area.removeFromTop(UiScale::sx(62));
        auto common_parameters = common_parameter_bounds_.reduced(panelPad, xs + 1);
        common_parameters_title_.setBounds(
            common_parameters.removeFromTop(UiScale::sx(20)));
        auto common_row = common_parameters;
        const auto common_width = (common_row.getWidth() - panelGap) / 2;
        auto tl_area = common_row.removeFromLeft(common_width);
        mod_total_level_label_.setBounds(tl_area.removeFromLeft(UiScale::sx(34)));
        mod_total_level_.setBounds(tl_area);
        common_row.removeFromLeft(panelGap);
        feedback_label_.setBounds(common_row.removeFromLeft(UiScale::sx(72)));
        feedback_.setBounds(common_row);
        area.removeFromTop(sm);
        auto operators = area.removeFromTop(UiScale::sx(408));
        const auto panel_width =
            (operators.getWidth() - panelGap + xs) / 2;
        modulator_.setBounds(
            operators.removeFromLeft(panel_width));
        operators.removeFromLeft(panelGap - xs);
        carrier_.setBounds(operators);

        area.removeFromTop(md);
        scope_title_.setBounds(area.removeFromTop(UiScale::sx(28)));
        scope_.setBounds(area.removeFromTop(UiScale::sx(110)));
        area.removeFromTop(sm);
        status_.setBounds(area.removeFromTop(statusH));

        layoutTimbreLibraryPanel(
            library_area,
            TimbreLibraryWidgets{
                library_title_,
                library_filter_,
                library_tag_filter_,
                tag_manage_,
                favorite_only_,
                library_ab_,
                library_sort_,
                library_list_,
                library_load_,
                library_delete_,
                name_,
                library_rename_,
                tags_,
                memo_,
                favorite_,
                library_save_,
                library_save_as_,
                library_new_,
                library_import_,
                library_export_,
                mgsc_title_,
                output_number_label_,
                output_number_,
                mgsc_preview_});
    }

    bool keyPressed(const juce::KeyPress& key) override {
        if (UiScale::tryHandleEditorScaleKey(
                key,
                ui_scale_session_override_,
                [this](int percent) { applyEditorUiScale(percent); })) {
            return true;
        }
        if (isCommandLetter(key, 'v')) {
            pasteClipboard();
            return true;
        }
        if (isCommandLetter(key, 'c')) {
            copyDefinition();
            return true;
        }
        if (isCommandLetter(key, 'y')
            || (isCommandLetter(key, 'z')
                && key.getModifiers().isShiftDown())) {
            redo();
            return true;
        }
        if (isCommandLetter(key, 'z')) {
            undo();
            return true;
        }
        if (performance_keyboard_.shouldConsumeKeyPress(key)) {
            performance_keyboard_.pollPerformanceInput();
            return true;
        }
        return false;
    }

    bool keyStateChanged(bool) override {
        if (!textEntryHasFocusWithin(*this)) {
            performance_keyboard_.pollPerformanceInput();
        }
        return false;
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
        return LastAuditionNoteStore::instance().get();
    }

    void saveLastAuditionNoteSetting(std::uint8_t midi_note) {
        LastAuditionNoteStore::instance().mark(midi_note);
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
                patch_,
                *number,
                utf8Text(name_.getText().trim()));
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
                .getChildFile("opll-tone.txt"),
            "*.txt");
        if (!chooser.browseForFileToSave(true)) {
            return;
        }
        const auto output_file =
            chooser.getResult().withFileExtension(".txt");
        if (!output_file.replaceWithText(
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
        if (auto* frame_editor = dialog->getTextEditor("frame")) {
            UiFonts::styleBodyField(*frame_editor);
        }
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
            mgstc::engine::formatMgsSccDefinition(waveform, 0, {});
        const auto file = pendingConversionFile("scc");
        if (!file.getParentDirectory().createDirectory()
            || !file.replaceWithText(
                juce::String::fromUTF8(definition.c_str()))) {
            updateStatus(juce::String::fromUTF8(
                "SCC変換データを保存できませんでした"));
            return;
        }
        open_editor_("scc", std::nullopt);
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
        juce::Component::SafePointer<OpllEditorComponent> safe(this);
        showOpllConversionQualityDialog(
            this,
            [safe, bytes, source_name](
                OpllConversionQuality quality) {
                if (safe == nullptr) {
                    return;
                }
        mgstc::engine::WavePcm pcm;
        std::string error;
        if (!mgstc::engine::parseWavePcm(
                std::span<const std::uint8_t>(bytes),
                pcm,
                &error)) {
                    safe->showError(
                source_name,
                juce::String::fromUTF8(
                    "WAVを読み込めませんでした。"
                    "PCM 8/16/24/32bitまたは32bit floatの"
                    "WAVEを使用してください"));
                    return;
        }
        const auto analysis =
            mgstc::engine::analyzeWaveCycle(pcm);
        if (analysis.cycle.size() < 2) {
                    safe->showError(
                source_name,
                juce::String::fromUTF8(
                    "波形の1周期を抽出できませんでした"));
                    return;
        }
                const auto frequency_hz =
                    analysis.estimated_frequency_hz;
        runWithConversionBusyDialog(
                    safe.getComponent(),
                    [pcm = std::move(pcm), quality](
                        ConversionProgressState& progress) {
                return mgstc::engine::
                            approximateWavePcmWithOpllResult(
                                pcm,
                                makeOpllApproximationOptions(
                                    quality, progress));
            },
            [safe, source_name, frequency_hz](
                        mgstc::engine::OpllApproximationResult result,
                        bool cancellation_requested) {
                if (safe == nullptr) {
                    return;
                }
                        if (cancellation_requested
                            || result.completion
                                == mgstc::engine::
                                    OpllApproximationCompletion::
                                        Cancelled) {
                            safe->updateStatus(
                                source_name
                                + juce::String::fromUTF8(
                                    "をキャンセルしました"));
                            return;
                        }
                        if (result.candidates.empty()) {
                    safe->showError(
                        source_name,
                        juce::String::fromUTF8(
                                    "OPLL近似音色を"
                                    "生成できませんでした"));
                    return;
                }
                        safe->wave_candidates_ =
                            std::move(result.candidates);
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

    void updateTagButtons() {
        tags_.setButtonText(tagSelectionSummary(
            selected_tags_, juce::String::fromUTF8("タグを選択")));
        library_tag_filter_.setButtonText(tagSelectionSummary(
            selected_filter_tags_,
            juce::String::fromUTF8("タグで絞り込み")));
    }

    void showTagEditor() {
        std::vector<std::vector<std::string>> tag_sets;
        for (const auto& entry : library_.entries()) {
            if (entry.category == mgstc::engine::TimbreCategory::Opll) {
                tag_sets.push_back(entry.tags);
            }
        }
        juce::Component::SafePointer<OpllEditorComponent> safe(this);
        showTagSelectionDialog(
            this,
            juce::String::fromUTF8("OPLL音色のタグ"),
            editorTagChoices(tag_sets, selected_tags_),
            selected_tags_,
            true,
            [safe](std::vector<std::string> selected) {
                if (safe != nullptr) {
                    safe->selected_tags_ = std::move(selected);
                    safe->updateTagButtons();
                }
            });
    }

    void showLibraryTagFilter() {
        std::vector<std::vector<std::string>> tag_sets;
        for (const auto& entry : library_.entries()) {
            if (entry.category == mgstc::engine::TimbreCategory::Opll) {
                tag_sets.push_back(entry.tags);
            }
        }
        juce::Component::SafePointer<OpllEditorComponent> safe(this);
        showTagSelectionDialog(
            this,
            juce::String::fromUTF8("OPLLライブラリのタグ検索"),
            filterTagChoices(tag_sets, selected_filter_tags_),
            selected_filter_tags_,
            false,
            [safe](std::vector<std::string> selected) {
                if (safe != nullptr) {
                    safe->selected_filter_tags_ = std::move(selected);
                    safe->updateTagButtons();
                    safe->refreshLibraryList();
                }
            });
    }

    void refreshLibraryList() {
        library_list_.clear(juce::dontSendNotification);
        library_ids_.clear();
        const auto filter =
            library_filter_.getText().trim();
        int item_id = 1;
        int selected_item = 0;
        std::vector<const mgstc::engine::TimbreLibraryEntry*> ordered;
        for (const auto& entry : library_.entries()) {
            if (entry.category == mgstc::engine::TimbreCategory::Opll
                && (!favorite_only_.getToggleState()
                    || entry.favorite)) {
                ordered.push_back(&entry);
            }
        }
        const int sort_mode = library_sort_.getSelectedId();
        std::stable_sort(
            ordered.begin(), ordered.end(),
            [sort_mode](const auto* left, const auto* right) {
                if (left->favorite != right->favorite) {
                    return left->favorite;
                }
                if (sort_mode == 2
                    && left->last_used_unix_seconds
                        != right->last_used_unix_seconds) {
                    return left->last_used_unix_seconds
                        > right->last_used_unix_seconds;
                }
                if (sort_mode == 3
                    && left->updated_unix_seconds
                        != right->updated_unix_seconds) {
                    return left->updated_unix_seconds
                        > right->updated_unix_seconds;
                }
                if (sort_mode == 4) {
                    return left->name < right->name;
                }
                return false;
            });
        for (const auto* entry_ptr : ordered) {
            if (!entry_ptr) {
                continue;
            }
            const auto& entry = *entry_ptr;
            if (filter.isNotEmpty()) {
                const auto name =
                    juce::String::fromUTF8(
                        entry.name.c_str());
                const auto tags =
                    juce::String::fromUTF8(
                        mgstc::engine::serializeTimbreTags(
                            entry.tags).c_str());
                const auto memo =
                    juce::String::fromUTF8(
                        entry.memo.c_str());
                if (!name.containsIgnoreCase(filter)
                    && !tags.containsIgnoreCase(filter)
                    && !memo.containsIgnoreCase(filter)) {
                    continue;
                }
            }
            if (!mgstc::engine::containsAllTimbreTags(
                    entry.tags, selected_filter_tags_)) {
                continue;
            }
            library_ids_.push_back(entry.id);
            auto label =
                juce::String::fromUTF8(entry.name.c_str())
                + " (r"
                + juce::String(static_cast<int>(entry.revision))
                + ")";
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
        updateDefinitionPreview();
        selected_tags_ = entry->tags;
        updateTagButtons();
        memo_.setText(
            juce::String::fromUTF8(entry->memo.c_str()),
            false);
        favorite_.setToggleState(
            entry->favorite, juce::dontSendNotification);
        library_preview_ = *entry;
        library_ab_.setEnabled(true);
        library_ab_next_b_ = false;
        const auto preview =
            mgstc::engine::decodeOpllPatch(entry->opll_registers);
        static_cast<void>(auditionOneSecond(&preview));
        static_cast<void>(
            library_.touch(entry->id, unixTimeNow()));
        static_cast<void>(persistLibrary());
        updateStatus(
            juce::String::fromUTF8(
                "音色を選択しました。「読込」で編集値へ反映します"));
    }

    void auditionLibraryAb() {
        if (!library_preview_) {
            return;
        }
        const bool play_b = library_ab_next_b_;
        library_ab_next_b_ = !library_ab_next_b_;
        if (play_b) {
            const auto preview = mgstc::engine::decodeOpllPatch(
                library_preview_->opll_registers);
            static_cast<void>(auditionOneSecond(&preview));
        } else {
            static_cast<void>(auditionOneSecond());
        }
        library_ab_.setButtonText(abNextSideButtonText(play_b));
        updateStatus(
            play_b
                ? juce::String::fromUTF8("B 保存音色を試聴中")
                : juce::String::fromUTF8("A 編集中音色を試聴中"));
    }

    [[nodiscard]] mgstc::engine::TimbreLibraryEntry
    captureLibraryEntry() const {
        mgstc::engine::TimbreLibraryEntry entry;
        entry.category =
            mgstc::engine::TimbreCategory::Opll;
        entry.name = utf8Text(name_.getText().trim());
        entry.tags = selected_tags_;
        entry.memo = utf8Text(memo_.getText());
        entry.favorite = favorite_.getToggleState();
        entry.opll_registers =
            mgstc::engine::encodeOpllPatch(patch_);
        return entry;
    }

    void newLibraryEntry() {
        if (hasUnsavedChanges()) {
            juce::Component::SafePointer<OpllEditorComponent> safe(this);
            showDiscardConfirmation(
                this,
                juce::String::fromUTF8("新規作成"),
                [safe] {
                    if (safe != nullptr) {
                        safe->performNewLibraryEntry();
                    }
                });
            return;
        }
        performNewLibraryEntry();
    }

    void performNewLibraryEntry() {
        selected_library_id_.reset();
        library_list_.setSelectedId(
            0, juce::dontSendNotification);
        name_.clear();
        selected_tags_.clear();
        updateTagButtons();
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
        if (hasUnsavedChanges()) {
            juce::Component::SafePointer<OpllEditorComponent> safe(this);
            showDiscardConfirmation(
                this,
                juce::String::fromUTF8("選択音色の読込"),
                [safe] {
                    if (safe != nullptr) {
                        safe->performLoadSelectedLibraryEntry();
                    }
                });
            return;
        }
        performLoadSelectedLibraryEntry();
    }

    void performLoadSelectedLibraryEntry() {
        if (!selected_library_id_) {
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
        static_cast<void>(
            library_.touch(id, unixTimeNow()));
        static_cast<void>(persistLibrary());
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

    void renameSelectedLibraryEntry() {
        if (!selected_library_id_) {
            showError(
                juce::String::fromUTF8("OPLL音色ライブラリ"),
                juce::String::fromUTF8(
                    "名前を変更する音色を選択してください"));
            return;
        }
        const auto requested = utf8Text(name_.getText().trim());
        if (requested.empty()) {
            showError(
                juce::String::fromUTF8("OPLL音色ライブラリ"),
                juce::String::fromUTF8("新しい名前を入力してください"));
            return;
        }
        juce::InterProcessLock::ScopedLockType lock(library_lock_);
        if (!lock.isLocked() || !reloadLibraryFromDisk()) {
            showError(
                juce::String::fromUTF8("OPLL音色ライブラリ"),
                juce::String::fromUTF8(
                    "共有ライブラリを読み直せませんでした"));
            return;
        }
        const auto* source = library_.find(*selected_library_id_);
        if (!source) {
            showError(
                juce::String::fromUTF8("OPLL音色ライブラリ"),
                juce::String::fromUTF8(
                    "選択した音色は削除されています"));
            return;
        }
        auto renamed = *source;
        renamed.name = requested;
        if (!library_.update(
                *selected_library_id_, renamed, unixTimeNow())
            || !persistLibrary()) {
            showError(
                juce::String::fromUTF8("OPLL音色ライブラリ"),
                juce::String::fromUTF8(
                    "名前の変更を保存できませんでした"));
            return;
        }
        refreshLibraryList();
        selectLibraryEntryFromList();
        setEditorBaseline();
        updateStatus(juce::String::fromUTF8("音色名を変更しました"));
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
        const auto impact = inspectCompositeTimbreImpact(id);
        if (!impact.readable || !impact.uses.empty()) {
            showError(
                juce::String::fromUTF8("OPLL音色ライブラリ"),
                !impact.readable
                    ? juce::String::fromUTF8(
                          "総合音色ライブラリを確認できないため、"
                          "安全に削除できません")
                    : juce::String::fromUTF8(
                          "総合音色から参照中のため削除できません。\n\n")
                        + describeCompositeTimbreImpact(impact));
            return;
        }
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
                safe->selected_tags_.clear();
                safe->updateTagButtons();
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
            UiFonts::setMgscPreviewText(
                mgsc_preview_,
                juce::String::fromUTF8(
                    "一時出力番号は15～31で入力してください。"));
            return;
        }
        const auto definition =
            mgstc::engine::formatMgsOpllDefinition(
                patch_,
                *number,
                utf8Text(name_.getText().trim()));
        UiFonts::setMgscPreviewText(
            mgsc_preview_,
            juce::String::fromUTF8(
                definition.data(),
                static_cast<int>(definition.size())));
    }

    void clearEngineVoices(bool arm_hang_silence = true) {
        for (std::uint8_t channel = 0; channel < 9; ++channel) {
            const auto track = static_cast<std::uint8_t>(
                kOpllTrack + channel);
            static_cast<void>(engine_.submit(
                mgstc::engine::EngineCommand::noteOff(track)));
            if (arm_hang_silence) {
                audio_service_.armOpllKeyOffForceSilence(track);
            } else {
                audio_service_.cancelOpllKeyOffForceSilence(track);
            }
        }
        static_cast<void>(voice_allocator_.allNotesOff());
    }

    void silenceAllVoices() {
        audition_stop_time_ms_.reset();
        performance_keyboard_.clearPreviewNote();
        clearEngineVoices(true);
    }

    bool configureEngine(
        bool retrigger,
        const mgstc::engine::OpllPatchParameters*
            preview = nullptr) {
        // hardReset前にアロケータと実発音を揃え、Poly残留を防ぐ。
        clearEngineVoices(false);
        audio_service_.clearOpllKeyOffForceSilence();
        engine_holds_temporary_program_ = (preview != nullptr);
        const auto& patch = preview ? *preview : patch_;
        if (preview == nullptr) {
            audio_service_.setSharedOpllPatch(patch_);
        }
        const auto configured = audio_service_.submitSharedEditorProgram(
            audio_service_.sharedSccWaveform(),
            patch,
            retrigger,
            kOpllTrack,
            last_audition_note_);
        if (configured && preview == nullptr) {
            applied_shared_scc_revision_ =
                audio_service_.sharedSccWaveRevision();
            applied_shared_opll_revision_ =
                audio_service_.sharedOpllPatchRevision();
        }
        return configured;
    }

    void startPerformanceNote(std::uint8_t note) {
        audition_stop_time_ms_.reset();
        performance_keyboard_.clearPreviewNote();
        last_audition_note_ = note;
        saveLastAuditionNoteSetting(last_audition_note_);
        updateAuditionNoteLabels();
        pending_envelope_trace_refresh_ = true;
        if (!engine_ready_) {
            return;
        }
        // 一時試聴が残っていると鍵盤が未確定音色を鳴らすため、確定へ戻す。
        if (engine_holds_temporary_program_
            && !configureEngine(false)) {
            return;
        }
        const auto assignment = voice_allocator_.noteOn(note);
        const auto track = static_cast<std::uint8_t>(
            kOpllTrack + assignment.channel);
        audio_service_.cancelOpllKeyOffForceSilence(track);
        if (assignment.stolen_note) {
            static_cast<void>(engine_.submit(
                mgstc::engine::EngineCommand::noteOff(track)));
        }
        if (!engine_.submit(
                mgstc::engine::EngineCommand::noteOn(
                    track, note))) {
            static_cast<void>(voice_allocator_.noteOff(note));
            return;
        }
    }

    void stopPerformanceNote(std::uint8_t note) {
        audition_stop_time_ms_.reset();
        const auto channel = voice_allocator_.noteOff(note);
        if (!channel) {
            return;
        }
        const auto track = static_cast<std::uint8_t>(
            kOpllTrack + *channel);
        static_cast<void>(engine_.submit(
            mgstc::engine::EngineCommand::noteOff(track)));
        audio_service_.armOpllKeyOffForceSilence(track);
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
        if (pending_envelope_trace_refresh_) {
            pending_envelope_trace_refresh_ = false;
            refreshEnvelopeTrace();
        }
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
                pending_envelope_trace_refresh_ = true;
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
        }
    }

    void updateStatus(const juce::String& text) {
        status_.setText(text, juce::dontSendNotification);
    }

    EditorOpenCallback open_editor_;
    TagManagementCallback manage_tags_;
    SharedAudioService& audio_service_;
    mgstc::engine::RealtimeEngineHost& engine_;
    juce::TooltipWindow tooltip_window_;
    SwitchLookAndFeel switch_look_and_feel_;
    mgstc::engine::OpllPatchParameters patch_;
    std::optional<int> ui_scale_session_override_;
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
    juce::TextButton library_tag_filter_;
    juce::TextButton tag_manage_;
    juce::ToggleButton favorite_only_;
    juce::TextButton library_ab_;
    juce::ComboBox library_sort_;
    juce::ComboBox library_list_;
    juce::TextEditor name_;
    juce::TextButton tags_;
    juce::TextEditor memo_;
    juce::ToggleButton favorite_;
    juce::TextButton library_new_;
    juce::TextButton library_load_;
    juce::TextButton library_save_;
    juce::TextButton library_save_as_;
    juce::TextButton library_rename_;
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
    std::optional<mgstc::engine::TimbreLibraryEntry>
        library_preview_;
    bool library_ab_next_b_{true};
    std::vector<std::string> selected_tags_;
    std::vector<std::string> selected_filter_tags_;
    mgstc::engine::TimbreLibraryEntry editor_baseline_;
    std::optional<std::uint64_t> editor_baseline_id_;
    juce::InterProcessLock library_lock_{
        "MgsToneCraftTimbreLibraryV1"};
    bool editor_baseline_valid_{};
    bool syncing_{};
    bool engine_ready_{};
    bool engine_holds_temporary_program_{};
    bool pending_envelope_trace_refresh_{};
    std::uint64_t applied_shared_scc_revision_{};
    std::uint64_t applied_shared_opll_revision_{};
    std::optional<std::uint64_t> library_manager_performance_id_;
    std::optional<mgstc::engine::OpllPatchParameters>
        library_manager_preview_patch_;
    std::uint8_t last_audition_note_{kPreviewNote};
    int settings_poll_ticks_{};
    std::uint64_t master_volume_revision_{};
    std::optional<double> audition_stop_time_ms_;
};

// Fit a top-level editor window entirely inside the target monitor work
// area (taskbar excluded). Size/position are never written to settings INI.
//
// On Windows with a native title bar, DocumentWindow / HWNDComponentPeer
// getBounds() is the *client* rectangle (GetClientRect). The OS frame
// (caption + borders) is reported separately by peer->getFrameSize().
// Clamping client bounds to userBounds leaves the framed HWND extending
// into the taskbar — same model as ComponentBoundsConstrainer and
// ResizableWindow::restoreWindowStateFromString (inflate → clamp → deflate).
void clampWindowToDisplayWorkArea(juce::ResizableWindow& window) {
    auto bounds = window.getBounds();
    const auto& displays = juce::Desktop::getInstance().getDisplays();

    juce::BorderSize<int> frame;
    if (auto* peer = window.getPeer()) {
        if (const auto frame_size = peer->getFrameSizeIfPresent()) {
            frame = *frame_size;
        }
    }

    const auto outer_for_display = frame.addedTo(bounds);
    const juce::Displays::Display* display = nullptr;
    if (!outer_for_display.isEmpty()) {
        display = displays.getDisplayForRect(outer_for_display);
    }
    if (display == nullptr) {
        display = displays.getDisplayForPoint(
            juce::Desktop::getMousePosition().toFloat());
    }
    if (display == nullptr) {
        display = displays.getPrimaryDisplay();
    }
    if (display == nullptr) {
        return;
    }

    const auto work = display->userBounds.toNearestIntEdges();
    if (work.getWidth() <= 0 || work.getHeight() <= 0) {
        return;
    }

    // Max *content* size so the framed outer window still fits in work.
    const int max_content_w = juce::jmax(
        1, work.getWidth() - frame.getLeftAndRight());
    const int max_content_h = juce::jmax(
        1, work.getHeight() - frame.getTopAndBottom());

    // If a prior min size exceeds the work area (small laptop / 150% DPI),
    // lower the floor so the window can still open fully on-screen.
    if (auto* constrainer = window.getConstrainer()) {
        constexpr int kNoMax = 0x3fffffff;
        const int max_w = constrainer->getMaximumWidth() > 0
            ? constrainer->getMaximumWidth()
            : kNoMax;
        const int max_h = constrainer->getMaximumHeight() > 0
            ? constrainer->getMaximumHeight()
            : kNoMax;
        const int min_w = juce::jmin(
            juce::jmax(1, constrainer->getMinimumWidth()),
            max_content_w);
        const int min_h = juce::jmin(
            juce::jmax(1, constrainer->getMinimumHeight()),
            max_content_h);
        constrainer->setSizeLimits(min_w, min_h, max_w, max_h);
    }

    auto outer = frame.addedTo(bounds);
    outer.setWidth(juce::jmin(outer.getWidth(), work.getWidth()));
    outer.setHeight(juce::jmin(outer.getHeight(), work.getHeight()));

    if (!outer.intersects(work) || bounds.isEmpty()) {
        outer.setCentre(work.getCentre());
    } else {
        if (outer.getX() < work.getX()) {
            outer.setX(work.getX());
        }
        if (outer.getY() < work.getY()) {
            outer.setY(work.getY());
        }
        if (outer.getRight() > work.getRight()) {
            outer.setX(work.getRight() - outer.getWidth());
        }
        if (outer.getBottom() > work.getBottom()) {
            outer.setY(work.getBottom() - outer.getHeight());
        }
    }

    auto clamped = frame.subtractedFrom(outer);
    clamped.setWidth(juce::jmax(1, clamped.getWidth()));
    clamped.setHeight(juce::jmax(1, clamped.getHeight()));

    if (clamped != window.getBounds()) {
        window.setBoundsConstrained(clamped);
    }
}

class MainWindow final : public juce::DocumentWindow {
public:
    MainWindow(
        const juce::String& name,
        const juce::String& editor,
        SharedAudioService& audio_service,
        SharedMidiInputService& midi_service,
        EditorOpenCallback open_editor,
        TagManagementCallback manage_tags,
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
                    audio_service, midi_service, open_editor,
                    manage_tags), true);
        } else if (editor.equalsIgnoreCase("scc")) {
            setContentOwned(
                new SccEditorComponent(
                    audio_service, midi_service, open_editor,
                    manage_tags), true);
        } else {
            setContentOwned(
                new CompositeEditorComponent(
                    audio_service, midi_service, open_editor,
                    manage_tags), true);
        }
        setResizable(true, false);
        centreWithSize(getWidth(), getHeight());
        clampWindowToDisplayWorkArea(*this);
        setWantsKeyboardFocus(true);
        setVisible(true);
        installCaptionZOrderHook();
    }

    ~MainWindow() override {
        removeCaptionZOrderHook();
    }

    void parentHierarchyChanged() override {
        DocumentWindow::parentHierarchyChanged();
        installCaptionZOrderHook();
    }

    bool keyPressed(const juce::KeyPress& key) override {
        if (auto* content = getContentComponent()) {
            if (content->keyPressed(key)) {
                return true;
            }
        }
        return juce::DocumentWindow::keyPressed(key);
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

    [[nodiscard]] SnapshotResult captureSettingsTab(
        int initial_tab,
        const juce::File& output_file) {
        if (auto* opll = dynamic_cast<OpllEditorComponent*>(
                getContentComponent())) {
            return opll->captureSettingsTab(
                initial_tab, output_file);
        }
        if (auto* scc = dynamic_cast<SccEditorComponent*>(
                getContentComponent())) {
            return scc->captureSettingsTab(
                initial_tab, output_file);
        }
        if (auto* composite =
                dynamic_cast<CompositeEditorComponent*>(
                    getContentComponent())) {
            return composite->captureSettingsTab(
                initial_tab, output_file);
        }
        return SnapshotResult::MissingContent;
    }

    void refreshExternalState() {
        MGSTC_UI_ACTIVITY("window: refresh external state (library reload)");
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

    void requestLibraryEntry(std::uint64_t id) {
        if (auto* opll =
                dynamic_cast<OpllEditorComponent*>(
                    getContentComponent())) {
            opll->requestLibraryEntry(id);
        } else if (auto* scc =
                       dynamic_cast<SccEditorComponent*>(
                           getContentComponent())) {
            scc->requestLibraryEntry(id);
        } else if (auto* composite =
                       dynamic_cast<CompositeEditorComponent*>(
                           getContentComponent())) {
            composite->requestLibraryEntry(id);
        }
    }

    void auditionLibraryPreview(
        LibraryManagerKind kind,
        std::uint64_t id) {
        if (kind == LibraryManagerKind::Composite) {
            if (auto* composite =
                    dynamic_cast<CompositeEditorComponent*>(
                        getContentComponent())) {
                composite->auditionLibraryPreview(id);
            }
            return;
        }
        if (kind == LibraryManagerKind::Scc) {
            if (auto* scc = dynamic_cast<SccEditorComponent*>(
                    getContentComponent())) {
                scc->auditionLibraryPreview(id);
            }
        } else if (auto* opll =
                       dynamic_cast<OpllEditorComponent*>(
                           getContentComponent())) {
            opll->auditionLibraryPreview(id);
        }
    }

    void libraryManagerNoteOn(
        LibraryManagerKind kind,
        std::uint64_t id,
        std::uint8_t note) {
        if (kind == LibraryManagerKind::Composite) {
            if (auto* composite =
                    dynamic_cast<CompositeEditorComponent*>(
                        getContentComponent())) {
                composite->libraryManagerNoteOn(id, note);
            }
            return;
        }
        if (kind == LibraryManagerKind::Scc) {
            if (auto* scc = dynamic_cast<SccEditorComponent*>(
                    getContentComponent())) {
                scc->libraryManagerNoteOn(id, note);
            }
            return;
        }
        if (auto* opll = dynamic_cast<OpllEditorComponent*>(
                getContentComponent())) {
            opll->libraryManagerNoteOn(id, note);
        }
    }

    void libraryManagerNoteOff(
        LibraryManagerKind kind,
        std::uint8_t note) {
        if (kind == LibraryManagerKind::Composite) {
            if (auto* composite =
                    dynamic_cast<CompositeEditorComponent*>(
                        getContentComponent())) {
                composite->libraryManagerNoteOff(note);
            }
            return;
        }
        if (kind == LibraryManagerKind::Scc) {
            if (auto* scc = dynamic_cast<SccEditorComponent*>(
                    getContentComponent())) {
                scc->libraryManagerNoteOff(note);
            }
            return;
        }
        if (auto* opll = dynamic_cast<OpllEditorComponent*>(
                getContentComponent())) {
            opll->libraryManagerNoteOff(note);
        }
    }

    void libraryManagerAllNotesOff() {
        if (auto* composite =
                dynamic_cast<CompositeEditorComponent*>(
                    getContentComponent())) {
            composite->libraryManagerAllNotesOff();
        } else if (auto* scc = dynamic_cast<SccEditorComponent*>(
                       getContentComponent())) {
            scc->libraryManagerAllNotesOff();
        } else if (auto* opll =
                       dynamic_cast<OpllEditorComponent*>(
                           getContentComponent())) {
            opll->libraryManagerAllNotesOff();
        }
    }

    void applyTagRewrite(
        std::string_view source,
        std::string_view replacement) {
        if (auto* opll =
                dynamic_cast<OpllEditorComponent*>(
                    getContentComponent())) {
            opll->applyTagRewrite(source, replacement);
        } else if (auto* scc =
                       dynamic_cast<SccEditorComponent*>(
                           getContentComponent())) {
            scc->applyTagRewrite(source, replacement);
        } else if (auto* composite =
                       dynamic_cast<CompositeEditorComponent*>(
                           getContentComponent())) {
            composite->applyTagRewrite(source, replacement);
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

    // Make process-global UiScale match this editor's effective percent
    // (session override if any, else View/INI global).
    void syncProcessUiScale() {
        if (auto* opll =
                dynamic_cast<OpllEditorComponent*>(
                    getContentComponent())) {
            opll->syncProcessUiScale();
        } else if (auto* scc =
                       dynamic_cast<SccEditorComponent*>(
                           getContentComponent())) {
            scc->syncProcessUiScale();
        } else if (auto* composite =
                       dynamic_cast<CompositeEditorComponent*>(
                           getContentComponent())) {
            composite->syncProcessUiScale();
        }
    }

    // Resize content + window from this editor's effective metrics.
    void applyPreferredSizeForEffectiveScale() {
        if (auto* opll =
                dynamic_cast<OpllEditorComponent*>(
                    getContentComponent())) {
            opll->applyPreferredSizeForEffectiveScale();
        } else if (auto* scc =
                       dynamic_cast<SccEditorComponent*>(
                           getContentComponent())) {
            scc->applyPreferredSizeForEffectiveScale();
        } else if (auto* composite =
                       dynamic_cast<CompositeEditorComponent*>(
                           getContentComponent())) {
            composite->applyPreferredSizeForEffectiveScale();
        }
    }

    void prepareToHide() {
        if (auto* scc = dynamic_cast<SccEditorComponent*>(
                getContentComponent())) {
            scc->prepareToHide();
            return;
        }
        deactivate();
    }

    [[nodiscard]] bool hasUnsavedChanges() const {
        const auto* content = getContentComponent();
        if (const auto* opll =
                dynamic_cast<const OpllEditorComponent*>(content)) {
            return opll->hasUnsavedChanges();
        }
        if (const auto* scc =
                dynamic_cast<const SccEditorComponent*>(content)) {
            return scc->hasUnsavedChanges();
        }
        if (const auto* composite =
                dynamic_cast<const CompositeEditorComponent*>(content)) {
            return composite->hasUnsavedChanges();
        }
        return false;
    }

    [[nodiscard]] juce::String editorDisplayName() const {
        if (editor_ == "opll") {
            return juce::String::fromUTF8("OPLL音色エディタ");
        }
        if (editor_ == "scc") {
            return juce::String::fromUTF8("SCC音色エディタ");
        }
        return juce::String::fromUTF8("総合画面");
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

        return writeComponentPngSnapshot(*content, output_file);
    }

    void closeButtonPressed() override {
        // Unsaved confirmation must appear before full deactivate
        // (SCC INI flush / silence). close_editor_ owns hide + silence.
        close_editor_(editor_);
    }

    void activeWindowStatusChanged() override {
        DocumentWindow::activeWindowStatusChanged();
        if (!isActiveWindow()) {
            return;
        }
        // Belt-and-suspenders for non-caption activation paths (client /
        // border). Caption raise is handled in the HWND subclass below —
        // activeWindowStatusChanged often never fires for HTCAPTION alone
        // because TopLevelWindowManager tracks keyboard focus, which stays
        // on the previous window until client focus is restored.
        toFront(false);
        onWindowRaisedForEditing();
    }

    void restoreClientKeyboardFocus() {
        auto* content = getContentComponent();
        if (content == nullptr) {
            return;
        }
        auto* const focused =
            juce::Component::getCurrentlyFocusedComponent();
        const bool focus_ok =
            focused != nullptr
            && (focused == content || content->isParentOf(focused));
        if (!focus_ok) {
            content->grabKeyboardFocus();
        }
    }

private:
    static constexpr UINT_PTR kCaptionZOrderSubclassId = 0x4d475354u; // MGST

    void onWindowRaisedForEditing() {
        MGSTC_UI_ACTIVITY("window: raise + activate (caption/border)");
        if (activate_editor_) {
            activate_editor_(editor_);
        }
        restoreClientKeyboardFocus();
        juce::Component::SafePointer<MainWindow> safe_this(this);
        juce::Timer::callAfterDelay(1, [safe_this]() {
            if (safe_this == nullptr || !safe_this->isShowing()) {
                return;
            }
            safe_this->restoreClientKeyboardFocus();
        });
    }

    void installCaptionZOrderHook() {
        auto* peer = getPeer();
        if (peer == nullptr) {
            caption_hook_hwnd_ = nullptr;
            return;
        }
        const auto hwnd = static_cast<HWND>(peer->getNativeHandle());
        if (hwnd == nullptr || hwnd == caption_hook_hwnd_) {
            return;
        }
        // Prior HWND is destroyed with the old peer; do not Remove on stale.
        caption_hook_hwnd_ = nullptr;
        if (SetWindowSubclass(
                hwnd,
                &captionZOrderSubclassProc,
                kCaptionZOrderSubclassId,
                reinterpret_cast<DWORD_PTR>(this))) {
            caption_hook_hwnd_ = hwnd;
        }
    }

    void removeCaptionZOrderHook() {
        if (caption_hook_hwnd_ == nullptr) {
            return;
        }
        RemoveWindowSubclass(
            caption_hook_hwnd_,
            &captionZOrderSubclassProc,
            kCaptionZOrderSubclassId);
        caption_hook_hwnd_ = nullptr;
    }

    static LRESULT CALLBACK captionZOrderSubclassProc(
        HWND hwnd,
        UINT msg,
        WPARAM wParam,
        LPARAM lParam,
        UINT_PTR /*subclass_id*/,
        DWORD_PTR ref_data) {
        if (msg == WM_NCLBUTTONDOWN && wParam == HTCAPTION) {
            // Mirror the Z-order side-effect of DefWindowProc(HTCAPTION)
            // without entering its modal drag loop (JUCE defers that until
            // mouse-move so painting keeps running).
            SetWindowPos(
                hwnd,
                HWND_TOP,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            if (auto* self = reinterpret_cast<MainWindow*>(ref_data);
                self != nullptr) {
                juce::Component::SafePointer<MainWindow> safe(self);
                juce::MessageManager::callAsync([safe]() {
                    if (safe == nullptr) {
                        return;
                    }
                    safe->toFront(false);
                    safe->onWindowRaisedForEditing();
                });
            }
        } else if (msg == WM_NCDESTROY) {
            if (auto* self = reinterpret_cast<MainWindow*>(ref_data);
                self != nullptr && self->caption_hook_hwnd_ == hwnd) {
                self->caption_hook_hwnd_ = nullptr;
            }
            RemoveWindowSubclass(
                hwnd,
                &captionZOrderSubclassProc,
                kCaptionZOrderSubclassId);
        }
        return DefSubclassProc(hwnd, msg, wParam, lParam);
    }

    juce::String editor_;
    EditorCloseCallback close_editor_;
    EditorActivateCallback activate_editor_;
    HWND caption_hook_hwnd_{nullptr};
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
        UiScale::loadGlobalFromIni();
        juce::LookAndFeel::setDefaultLookAndFeel(&look_and_feel_);
        hang_watchdog_.start();
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
            auto capture_target = arguments
                .getValueForOption("--capture-target")
                .trim()
                .toLowerCase();
            if (capture_target.isEmpty()) {
                capture_target = "editor";
            }
            snapshot_target_ = capture_target;
            if (snapshot_target_ == "editor") {
                snapshot_window_->prepareVisualInspection();
            }
            startTimer(500);
        }
    }

    void shutdown() override {
        stopTimer();
        hang_watchdog_.stop();
        snapshot_window_ = nullptr;
        opll_window_.reset();
        scc_window_.reset();
        main_window_.reset();
        midi_service_.reset();
        audio_service_.reset();
        juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    }

    void systemRequestedQuit() override {
        requestApplicationQuit();
    }

private:
    static constexpr int kMaxActivationPasses = 8;

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

    [[nodiscard]] MainWindow* ensureEditor(
        const juce::String& requested_editor,
        bool bring_to_front) {
        const auto editor = canonicalEditor(requested_editor);
        auto& window = windowSlot(editor);
        const bool created = window == nullptr;
        if (created) {
            // New editors always construct under View/INI global metrics —
            // never under another editor's Ctrl± session override.
            UiScale::forceGlobalForNonEditorUi();
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
                [this](
                    const juce::String& target,
                    std::optional<std::uint64_t> library_id) {
                    auto* opened = showEditor(target);
                    if (opened != nullptr && library_id) {
                        opened->requestLibraryEntry(*library_id);
                    }
                },
                [this](juce::Component* anchor) {
                    showLibraryManager(anchor);
                },
                [this](const juce::String& closed) {
                    closeEditor(closed);
                },
                [this](const juce::String& active) {
                    activateEditor(active);
                });
        }
        // Create or re-show: size from this window's effective scale (global
        // unless it already has its own Ctrl± session override).
        if (created || bring_to_front) {
            window->applyPreferredSizeForEffectiveScale();
        }
        if (created || bring_to_front || active_editor_ != editor) {
            activateEditor(editor);
        } else {
            // Already active + not bringing to front (library preview /
            // manager note-on): keep a light/heavy-smart refresh only.
            window->refreshExternalState();
        }
        // Re-show (e.g. SCC/OPLL from Composite) also clamps — monitor
        // layout or DPI may have changed while the window was hidden.
        clampWindowToDisplayWorkArea(*window);
        window->setVisible(true);
        if (bring_to_front) {
            window->toFront(true);
            window->grabKeyboardFocus();
        }
        return window.get();
    }

    [[nodiscard]] MainWindow* showEditor(
        const juce::String& requested_editor) {
        return ensureEditor(requested_editor, true);
    }

    void showLibraryManager(juce::Component* anchor) {
        // InterProcessLock::ScopedLockType waits without a timeout, so a second
        // MGSTC instance holding the shared library can stall this thread.
        MGSTC_UI_ACTIVITY(
            "library: manager open (shared lock + load)");
        juce::InterProcessLock::ScopedLockType lock(
            tag_management_lock_);
        std::string error;
        auto timbres = loadTimbreLibrary(&error);
        auto composites = loadCompositeTimbreLibrary(&error);
        if (!lock.isLocked() || !timbres || !composites) {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon,
                juce::String::fromUTF8("ライブラリ管理"),
                juce::String::fromUTF8(
                    "共有ライブラリを読み込めませんでした"));
            return;
        }
        auto initial_kind = LibraryManagerKind::Composite;
        if (dynamic_cast<SccEditorComponent*>(anchor) != nullptr) {
            initial_kind = LibraryManagerKind::Scc;
        } else if (
            dynamic_cast<OpllEditorComponent*>(anchor) != nullptr) {
            initial_kind = LibraryManagerKind::Opll;
        }
        showLibraryManagerDialog(
            anchor,
            initial_kind,
            std::move(*timbres),
            std::move(*composites),
            *midi_service_,
            *audio_service_,
            [this](std::string source, std::string replacement) {
                applyGlobalTagRewrite(
                    std::move(source), std::move(replacement));
            },
            [this](LibraryManagerKind kind, std::uint64_t id) {
                previewLibraryEntry(kind, id);
            },
            [this](LibraryManagerKind kind, std::uint64_t id) {
                openLibraryEntryForEdit(kind, id);
            },
            [this] { refreshAllEditorLibraries(); },
            [this](
                LibraryManagerKind kind,
                std::uint64_t id,
                std::uint8_t note) {
                libraryManagerNoteOn(kind, id, note);
            },
            [this](LibraryManagerKind kind, std::uint8_t note) {
                libraryManagerNoteOff(kind, note);
            },
            [this] { libraryManagerAllNotesOff(); });
    }

    void previewLibraryEntry(
        LibraryManagerKind kind,
        std::uint64_t id) {
        const juce::String editor =
            kind == LibraryManagerKind::Composite
                ? "main"
            : kind == LibraryManagerKind::Scc ? "scc" : "opll";
        if (auto* window = ensureEditor(editor, false)) {
            window->auditionLibraryPreview(kind, id);
        }
    }

    void libraryManagerNoteOn(
        LibraryManagerKind kind,
        std::uint64_t id,
        std::uint8_t note) {
        const juce::String editor =
            kind == LibraryManagerKind::Composite
                ? "main"
            : kind == LibraryManagerKind::Scc ? "scc" : "opll";
        if (auto* window = ensureEditor(editor, false)) {
            window->libraryManagerNoteOn(kind, id, note);
        }
    }

    void libraryManagerNoteOff(
        LibraryManagerKind kind,
        std::uint8_t note) {
        const juce::String editor =
            kind == LibraryManagerKind::Composite
                ? "main"
            : kind == LibraryManagerKind::Scc ? "scc" : "opll";
        auto& window = windowSlot(editor);
        if (window != nullptr) {
            window->libraryManagerNoteOff(kind, note);
        }
    }

    void libraryManagerAllNotesOff() {
        for (auto* window :
             std::array<MainWindow*, 3>{
                 main_window_.get(),
                 scc_window_.get(),
                 opll_window_.get()}) {
            if (window != nullptr) {
                window->libraryManagerAllNotesOff();
            }
        }
    }

    void openLibraryEntryForEdit(
        LibraryManagerKind kind,
        std::uint64_t id) {
        const juce::String editor =
            kind == LibraryManagerKind::Composite
                ? "main"
            : kind == LibraryManagerKind::Scc ? "scc" : "opll";
        if (auto* window = showEditor(editor)) {
            window->requestLibraryEntry(id);
        }
    }

    void refreshAllEditorLibraries() {
        for (auto* window :
             std::array<MainWindow*, 3>{
                 main_window_.get(),
                 scc_window_.get(),
                 opll_window_.get()}) {
            if (window != nullptr) {
                window->refreshExternalState();
            }
        }
    }

    void applyGlobalTagRewrite(
        std::string source,
        std::string replacement) {
        MGSTC_UI_ACTIVITY(
            "library: tag rewrite (shared lock + save)");
        juce::InterProcessLock::ScopedLockType lock(
            tag_management_lock_);
        std::string error;
        auto timbres = loadTimbreLibrary(&error);
        auto composites = loadCompositeTimbreLibrary(&error);
        if (!lock.isLocked() || !timbres || !composites) {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon,
                juce::String::fromUTF8("ライブラリ管理"),
                juce::String::fromUTF8(
                    "共有ライブラリを更新できませんでした"));
            return;
        }
        const auto original_timbres = *timbres;
        const auto original_composites = *composites;
        const auto now = currentUnixTime();
        const auto timbre_changes =
            timbres->rewriteTag(source, replacement, now);
        const auto composite_changes =
            composites->rewriteTag(source, replacement, now);
        if (timbre_changes == 0 && composite_changes == 0) {
            return;
        }

        const bool timbres_saved =
            timbre_changes == 0 || persistTimbreLibrary(*timbres);
        const bool composites_saved = timbres_saved
            && (composite_changes == 0
                || persistCompositeTimbreLibrary(*composites));
        if (!timbres_saved || !composites_saved) {
            const bool rolled_back_timbres =
                timbre_changes == 0
                || persistTimbreLibrary(original_timbres);
            const bool rolled_back_composites =
                composite_changes == 0
                || persistCompositeTimbreLibrary(
                    original_composites);
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon,
                juce::String::fromUTF8("ライブラリ管理"),
                rolled_back_timbres && rolled_back_composites
                    ? juce::String::fromUTF8(
                          "更新に失敗したため変更を取り消しました")
                    : juce::String::fromUTF8(
                          "更新とロールバックに失敗しました。"
                          "ライブラリファイルを確認してください"));
            return;
        }

        for (auto* window :
             std::array<MainWindow*, 3>{
                 main_window_.get(),
                 scc_window_.get(),
                 opll_window_.get()}) {
            if (window != nullptr) {
                window->applyTagRewrite(source, replacement);
            }
        }
        auto completion_message =
            juce::String::fromUTF8("更新した音色: ");
        completion_message
            << static_cast<int>(
                   timbre_changes + composite_changes);
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::InfoIcon,
            juce::String::fromUTF8("独自タグ管理"),
            completion_message);
    }

    void closeEditor(const juce::String& requested_editor) {
        const auto editor = canonicalEditor(requested_editor);
        if (editor == primary_editor_) {
            requestApplicationQuit();
            return;
        }
        auto& window = windowSlot(editor);
        if (window == nullptr || !window->hasUnsavedChanges()) {
            if (window != nullptr) {
                window->prepareToHide();
                window->setVisible(false);
            }
            return;
        }
        if (close_confirmation_pending_) {
            return;
        }
        close_confirmation_pending_ = true;
        const auto display_name = window->editorDisplayName();
        // Confirm before deactivate (0.205). Wait for mouse-up so the
        // alert is not created mid title-bar click (would drag with cursor).
        showAlertWhenMouseReleased(
            juce::MessageBoxOptions()
                .withIconType(
                    juce::MessageBoxIconType::WarningIcon)
                .withTitle(
                    juce::String::fromUTF8("未保存の変更"))
                .withMessage(
                    display_name
                    + juce::String::fromUTF8(
                        "に保存していない変更があります。\n"
                        "保存せずに閉じますか？"))
                .withButton(
                    juce::String::fromUTF8("保存せず閉じる"))
                .withButton(
                    juce::String::fromUTF8("キャンセル"))
                .withAssociatedComponent(window.get()),
            [this, editor](int result) {
                close_confirmation_pending_ = false;
                if (result == 1) {
                    if (auto& target = windowSlot(editor);
                        target != nullptr) {
                        target->prepareToHide();
                        target->setVisible(false);
                    }
                }
            });
    }

    void requestApplicationQuit() {
        if (close_confirmation_pending_) {
            return;
        }
        juce::String unsaved_editors;
        const auto append_if_dirty =
            [&unsaved_editors](
                const std::unique_ptr<MainWindow>& window) {
                if (window == nullptr
                    || !window->hasUnsavedChanges()) {
                    return;
                }
                if (unsaved_editors.isNotEmpty()) {
                    unsaved_editors += "\n";
                }
                unsaved_editors += juce::String::fromUTF8("・")
                    + window->editorDisplayName();
            };
        append_if_dirty(main_window_);
        append_if_dirty(scc_window_);
        append_if_dirty(opll_window_);
        if (unsaved_editors.isEmpty()) {
            quit();
            return;
        }

        close_confirmation_pending_ = true;
        showAlertWhenMouseReleased(
            juce::MessageBoxOptions()
                .withIconType(
                    juce::MessageBoxIconType::WarningIcon)
                .withTitle(
                    juce::String::fromUTF8("未保存の変更"))
                .withMessage(
                    juce::String::fromUTF8(
                        "次の画面に保存していない変更があります。\n")
                    + unsaved_editors
                    + juce::String::fromUTF8(
                        "\n\n保存せずにアプリを終了しますか？"))
                .withButton(
                    juce::String::fromUTF8("保存せず終了"))
                .withButton(
                    juce::String::fromUTF8("キャンセル"))
                .withAssociatedComponent(
                    main_window_ != nullptr
                        ? main_window_.get()
                        : snapshot_window_),
            [this](int result) {
                close_confirmation_pending_ = false;
                if (result == 1) {
                    quit();
                }
            });
    }

    void activateEditor(const juce::String& requested_editor) {
        pending_activate_editor_ = canonicalEditor(requested_editor);
        if (activating_editor_) {
            // Keep the latest request; do not drop nested title-bar/focus
            // activations (dropping left the UI half-switched until mouse move).
            return;
        }
        MGSTC_UI_ACTIVITY("window: activate editor");
        activating_editor_ = true;
        struct ActivatingGuard {
            bool& flag;
            ~ActivatingGuard() { flag = false; }
        } guard{activating_editor_};

        // Bounded drain: deactivate/refresh can queue another activation, and
        // two windows re-requesting each other would spin here without pumping
        // (message thread hang). Switching more than a couple of times per
        // click is already a bug, so bail and let the next event settle it.
        for (int pass = 0;
             pass < kMaxActivationPasses
             && pending_activate_editor_.has_value();
             ++pass) {
            const auto editor = *pending_activate_editor_;
            pending_activate_editor_.reset();
            if (active_editor_ == editor) {
                if (auto& window = windowSlot(editor);
                    window != nullptr) {
                    // Modal may have forced global; restore this editor's
                    // effective scale when it remains the active surface.
                    window->syncProcessUiScale();
                    window->refreshExternalState();
                }
                continue;
            }
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
                window->syncProcessUiScale();
                window->refreshExternalState();
            }
        }
        pending_activate_editor_.reset();
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
        SnapshotResult result = SnapshotResult::MissingContent;
        if (snapshot_target_ == "settings-view") {
            result = snapshot_window_ != nullptr
                ? snapshot_window_->captureSettingsTab(
                    0, snapshot_file_)
                : SnapshotResult::MissingContent;
        } else if (snapshot_target_ == "settings-midi") {
            result = snapshot_window_ != nullptr
                ? snapshot_window_->captureSettingsTab(
                    1, snapshot_file_)
                : SnapshotResult::MissingContent;
        } else if (snapshot_target_ == "settings-output") {
            result = snapshot_window_ != nullptr
                ? snapshot_window_->captureSettingsTab(
                    2, snapshot_file_)
                : SnapshotResult::MissingContent;
        } else if (snapshot_target_ == "library") {
            result = captureLibraryManagerSnapshot(snapshot_file_);
        } else if (snapshot_window_ != nullptr) {
            result = snapshot_window_->writeContentSnapshot(
                snapshot_file_);
        }
        setApplicationReturnValue(static_cast<int>(result));
        quit();
    }

    [[nodiscard]] SnapshotResult captureLibraryManagerSnapshot(
        const juce::File& output_file) {
        juce::InterProcessLock::ScopedLockType lock(
            tag_management_lock_);
        std::string error;
        auto timbres = loadTimbreLibrary(&error);
        auto composites = loadCompositeTimbreLibrary(&error);
        if (!lock.isLocked() || !timbres || !composites
            || midi_service_ == nullptr
            || audio_service_ == nullptr) {
            return SnapshotResult::MissingContent;
        }
        LibraryManagerContent content(
            LibraryManagerKind::Scc,
            std::move(*timbres),
            std::move(*composites),
            *midi_service_,
            *audio_service_,
            [](std::string, std::string) {},
            [](LibraryManagerKind, std::uint64_t) {},
            [](LibraryManagerKind, std::uint64_t) {},
            [] {},
            [](LibraryManagerKind, std::uint64_t, std::uint8_t) {},
            [](LibraryManagerKind, std::uint8_t) {},
            [] {});
        return writeComponentPngSnapshot(content, output_file);
    }

    std::unique_ptr<MainWindow> main_window_;
    std::unique_ptr<MainWindow> scc_window_;
    std::unique_ptr<MainWindow> opll_window_;
    MainWindow* snapshot_window_{};
    std::unique_ptr<SharedAudioService> audio_service_;
    std::unique_ptr<SharedMidiInputService> midi_service_;
    mgstc::app::UiHangWatchdog hang_watchdog_;
    juce::String primary_editor_{"main"};
    juce::String active_editor_{"main"};
    juce::String snapshot_target_{"editor"};
    bool routing_test_mode_{};
    bool routing_test_passed_{};
    bool close_confirmation_pending_{};
    bool activating_editor_{};
    std::optional<juce::String> pending_activate_editor_;
    juce::InterProcessLock tag_management_lock_{
        "MgsToneCraftTimbreLibraryV1"};
    juce::File snapshot_file_;
    MgstcLookAndFeel look_and_feel_;
};

} // namespace

START_JUCE_APPLICATION(Application)
