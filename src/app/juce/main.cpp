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
#include <fstream>
#include <functional>
#include <sstream>
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
#include "ui_chrome_constants.hpp"
#include "ui_paths.hpp"
#include "ui_scale.hpp"
#include "ui_fonts.hpp"
#include "ui_layout.hpp"
#include "ui_paint.hpp"
#include "rate_envelope_trace.hpp"
#include "ui_focus.hpp"
#include "ui_modal_dialog.hpp"
#include "juce_chip_tracks.hpp"
#include "last_audition_note.hpp"
#include "pitch_command_labels.hpp"
#include "midi_note_name.hpp"
#include "tag_ui.hpp"
#include "opll_patch_ui.hpp"
#include "shared_audio_host.hpp"
#include "composite_timeline.hpp"
#include "juce_utf8.hpp"
#include "library_browser_chrome.hpp"
#include "composite_envelope_compile.hpp"
#include "switch_look_and_feel.hpp"
#include "mgstc_look_and_feel.hpp"
#include "about_panel.hpp"
#include "mgstc/audio/wasapi_audio_sink.hpp"
#include "mgstc/engine/engine_command.hpp"
#include "mgstc/engine/chip_rack.hpp"
#include "mgstc/engine/chip_volume_curve.hpp"
#include "mgstc/engine/composite_timbre.hpp"
#include "mgstc/engine/composite_timbre_library.hpp"
#include "mgstc/engine/volume.hpp"
#include "mgstc/engine/envelope_sequence.hpp"
#include "mgstc/engine/envelope_rate.hpp"
#include "mgstc/engine/mgs_composite_io.hpp"
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

using mgstc::app::CompositeTimeline;
using mgstc::app::EnvelopeTimbreCatalogItem;
using mgstc::app::LayerBaseTimbreAssign;

// #region agent log
void dbg7ae407(
    const char* hypothesisId,
    const char* location,
    const char* message,
    const std::string& dataObject) {
    try {
        std::ofstream out(
            "debug-7ae407.log",
            std::ios::app | std::ios::binary);
        if (!out) {
            return;
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        out << "{\"sessionId\":\"7ae407\",\"runId\":\"pre-fix\",\"hypothesisId\":\""
            << hypothesisId << "\",\"location\":\"" << location
            << "\",\"message\":\"" << message << "\",\"data\":" << dataObject
            << ",\"timestamp\":" << ms << "}\n";
    } catch (...) {
    }
}
// #endregion

static_assert(
    sizeof(mgstc::engine::RealtimeEngineHost) < 128 * 1024,
    "RealtimeEngineHost must remain safe to create in an editor");

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
using OpllCandidateCallback =
    std::function<void(
        std::vector<mgstc::engine::OpllPatchParameters>)>;
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
// Shared library IPC: never wait forever on the message thread (second
// MGSTC instance or a crashed holder previously caused multi-second freezes
// with ui_activity still "idle").
constexpr int kLibraryIpcLockTimeoutMs = 2500;

class ScopedLibraryIpcLock final {
public:
    explicit ScopedLibraryIpcLock(juce::InterProcessLock& lock)
        : lock_(lock),
          locked_(lock.enter(kLibraryIpcLockTimeoutMs)) {}
    ~ScopedLibraryIpcLock() {
        if (locked_) {
            lock_.exit();
        }
    }
    ScopedLibraryIpcLock(const ScopedLibraryIpcLock&) = delete;
    ScopedLibraryIpcLock& operator=(const ScopedLibraryIpcLock&) = delete;
    [[nodiscard]] bool isLocked() const noexcept { return locked_; }

private:
    juce::InterProcessLock& lock_;
    bool locked_;
};

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
    return mgstcApplicationDataDirectory();
}


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


// UI type scale — exactly four roles (px heights; face = Windows message font).
//   title   : window / page title
//   heading : panel / section header
//   body    : buttons, labels, combos, dialogs
//   dense   : timbre params, MGSC preview, keyboard C*, DEC/HEX, graph chrome



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

class SharedAudioService final : public SharedAudioHost, private juce::Timer {
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

    [[nodiscard]] mgstc::engine::RealtimeEngineHost& engine() noexcept override {
        return engine_;
    }

    [[nodiscard]] bool running() const noexcept override {
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

    void armOpllKeyOffForceSilence(std::uint8_t track) override {
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

    void clearOpllKeyOffForceSilence() override {
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

    [[nodiscard]] const SccWaveform& sharedSccWaveform() const noexcept override {
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
    sharedOpllPatch() const noexcept override {
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
        std::uint8_t midi_note) override {
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

void configureMasterVolumeLabel(juce::Label& label) {
    label.setText(
        juce::String::fromUTF8("音量"),
        juce::dontSendNotification);
    label.setFont(UiFonts::body());
    label.setJustificationType(juce::Justification::centred);
    label.setInterceptsMouseClicks(false, false);
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



// Apply active metrics' preferred content size and refresh LookAndFeel fonts.

// Top-right chrome: settings / master volume / optional immediate audition.
// Icon buttons use the same UiLayout::iconButton size as Open/Save/Copy/Paste.



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
    layoutLibraryBrowserChrome(
        area,
        LibraryBrowserChromeWidgets{
            widgets.filter,
            widgets.tag_filter,
            widgets.tag_manage,
            widgets.favorite_only,
            widgets.ab_audition,
            widgets.sort,
            widgets.list,
            widgets.library_load,
            widgets.library_delete,
            widgets.name,
            widgets.library_rename,
            widgets.tags,
            &widgets.memo});
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
    layoutLibraryBrowserChrome(
        area,
        LibraryBrowserChromeWidgets{
            widgets.filter,
            widgets.tag_filter,
            widgets.tag_manage,
            widgets.favorite_only,
            widgets.ab_audition,
            widgets.sort,
            widgets.list,
            widgets.library_load,
            widgets.library_delete,
            widgets.name,
            widgets.library_rename,
            widgets.tags,
            nullptr});
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

    // Keep MIDI / PC keys alive while a modal (e.g. software LFO) holds focus.
    void setInputActiveOverride(bool enabled) noexcept {
        input_active_override_ = enabled;
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
        if (input_active_override_) {
            return juce::Process::isForegroundProcess();
        }
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
    bool input_active_override_{};
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
        configureMasterVolumeLabel(master_volume_label_);
        addAndMakeVisible(master_volume_label_);
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
        name_.onReturnKey = [this] { recordHistory(); };
        name_.onFocusLost = [this] { recordHistory(); };
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
            recordHistory();
        };
        addAndMakeVisible(favorite_);

        configureIconButton(
            file_open_,
            EditorIcon::Open,
            juce::String::fromUTF8(
                "総合音色ファイルを開く（.mgstc）"),
            [this] { openCompositeFile(); });
        configureIconButton(
            file_save_,
            EditorIcon::Save,
            juce::String::fromUTF8(
                "総合音色をファイルへ保存（.mgstc）"),
            [this] { saveCompositeFile(); });
        configureIconButton(
            file_paste_,
            EditorIcon::Paste,
            juce::String::fromUTF8(
                "クリップボードから総合音色を貼り付け"),
            [this] { pasteCompositeProgram(); });
        configureIconButton(
            file_copy_,
            EditorIcon::Copy,
            juce::String::fromUTF8(
                "総合音色をクリップボードへコピー"),
            [this] { copyCompositeProgram(); });
        configureButton(
            mgsc_open_,
            juce::String::fromUTF8("MGSC開く"),
            juce::String::fromUTF8(
                "MGSCソース（.mgs／.mus／.txt）から総合音色を開きます"),
            [this] { openMgsCompositeFile(); });
        configureButton(
            mgsc_save_,
            juce::String::fromUTF8("MGSC書出"),
            juce::String::fromUTF8(
                "総合音色をMGSCソース（.mgs）として書き出します"),
            [this] { saveMgsCompositeFile(); });
        configureIconButton(
            undo_,
            EditorIcon::Undo,
            juce::String::fromUTF8("Undo (Ctrl+Z)"),
            [this] { undo(); });
        configureIconButton(
            redo_,
            EditorIcon::Redo,
            juce::String::fromUTF8("Redo (Ctrl+Y)"),
            [this] { redo(); });
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
        history_.clear();
        history_.push_back(timbre_);
        history_cursor_ = 0;
        updateHistoryButtons();

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
                bool commit,
                bool request_preview) {
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
                recordHistory();
                if (request_preview && !satellite_session_) {
                    timeline_preview_pending_ = true;
                    timeline_preview_due_ms_ =
                        juce::Time::getMillisecondCounterHiRes() + 50.0;
                }
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
        timeline_.setManageTagsCallback([this] {
            if (manage_tags_) {
                manage_tags_(this);
            }
        });
        timeline_.setMutateTimbreNameCallback(
            [this](std::uint64_t id, juce::String name) {
                return mutateSharedTimbreName(id, std::move(name));
            });
        timeline_.setMutateTimbreTagsCallback(
            [this](std::uint64_t id, std::vector<std::string> tags) {
                return mutateSharedTimbreTags(id, std::move(tags));
            });
        timeline_.setMutateTimbreMemoCallback(
            [this](std::uint64_t id, juce::String memo) {
                return mutateSharedTimbreMemo(id, std::move(memo));
            });
        timeline_.setLfoSessionCallback(
            [this](
                std::optional<std::size_t> layer,
                bool start_audition,
                bool isolate) {
                satellite_session_ = layer.has_value();
                audition_layer_filter_ =
                    isolate && layer ? layer : std::nullopt;
                performance_keyboard_.setInputActiveOverride(
                    layer.has_value());
                composite_program_stale_ = true;
                if (!layer) {
                    stopAudition();
                    static_cast<void>(configureEngine());
                    return;
                }
                if (start_audition) {
                    startCompositeNote(last_audition_note_, true, false);
                    return;
                }
                if (!configureEngine()) {
                    return;
                }
                timeline_preview_pending_ = false;
                stopAudition();
            });
        timeline_.setLfoPollKeysCallback(
            [this] {
                if (textEntryHasFocusWithin(*this)) {
                    return;
                }
                auto* focused =
                    juce::Component::getCurrentlyFocusedComponent();
                if (focused != nullptr) {
                    for (auto* component = focused;
                         component != nullptr;
                         component = component->getParentComponent()) {
                        if (const auto* editor =
                                dynamic_cast<const juce::TextEditor*>(
                                    component);
                            editor != nullptr && !editor->isReadOnly()) {
                            return;
                        }
                    }
                }
                performance_keyboard_.pollPerformanceInput();
            });
        timeline_.setAudioService(&audio_service_);
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
            [this] {
                if (textEntryHasFocusWithin(*this)) {
                    return true;
                }
                if (!audition_layer_filter_) {
                    return false;
                }
                auto* focused =
                    juce::Component::getCurrentlyFocusedComponent();
                if (focused == nullptr) {
                    return false;
                }
                for (auto* component = focused;
                     component != nullptr;
                     component = component->getParentComponent()) {
                    if (const auto* editor =
                            dynamic_cast<const juce::TextEditor*>(component);
                        editor != nullptr && !editor->isReadOnly()) {
                        return true;
                    }
                    if (const auto* label =
                            dynamic_cast<juce::Label*>(component);
                        label != nullptr && label->isBeingEdited()) {
                        return true;
                    }
                }
                return false;
            });
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
        MGSTC_UI_ACTIVITY("library: composite audition preview");
        ScopedLibraryIpcLock lock(library_lock_);
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
        ScopedLibraryIpcLock lock(library_lock_);
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
        master_volume_label_.setFont(UiFonts::body());
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
        layoutEditorTopRightChrome(
            getWidth(),
            settings_,
            master_volume_,
            &master_volume_label_);
        int chrome_right =
            getWidth() - pageMargin - iconButton - controlGap - masterVolumeSize
            - controlGap;
        open_opll_.setBounds(
            chrome_right - compositeOpenOpllW,
            pageMargin,
            compositeOpenOpllW,
            textButtonH);
        chrome_right -= compositeOpenOpllW + controlGap;
        open_scc_.setBounds(
            chrome_right - compositeOpenSccW,
            pageMargin,
            compositeOpenSccW,
            textButtonH);
        const int title_w = juce::jmax(
            120,
            chrome_right - compositeOpenSccW - area.getX() - sm);
        title_.setBounds(area.getX(), area.getY(), title_w, titleH);
        description_.setBounds(
            area.getX(),
            area.getY() + titleH,
            title_w,
            descriptionH);
        area.removeFromTop(titleH + descriptionH);
        area.removeFromTop(sm);
        auto file_row = area.removeFromTop(toolbarH);
        for (auto* button :
             std::array<juce::DrawableButton*, 6>{
                 &file_open_,
                 &file_save_,
                 &file_paste_,
                 &file_copy_,
                 &undo_,
                 &redo_}) {
            button->setBounds(file_row.removeFromLeft(iconButton));
            file_row.removeFromLeft(controlGap);
        }
        mgsc_open_.setBounds(
            file_row.removeFromLeft(
                UiScale::sx(UiLayout::compositeMgscOpenW)));
        file_row.removeFromLeft(controlGap);
        mgsc_save_.setBounds(
            file_row.removeFromLeft(
                UiScale::sx(UiLayout::compositeMgscSaveW)));
        area.removeFromTop(sm);

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
        stop_.setBounds(footer.removeFromLeft(compositeStopW));
        footer.removeFromLeft(controlGap);
        status_.setBounds(footer);
        editor.removeFromBottom(sm);
        resource_.setBounds(editor.removeFromTop(fieldH));
        editor.removeFromTop(sm);
        warning_.setBounds(editor.removeFromTop(fieldH));
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
        if (isCommandLetter(key, 'v')) {
            pasteCompositeProgram();
            return true;
        }
        if (isCommandLetter(key, 'c')) {
            copyCompositeProgram();
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
                 &file_open_,
                 &file_save_,
                 &file_paste_,
                 &file_copy_,
                 &undo_,
                 &redo_,
                 &timeline_,
                 &stop_,
                 &status_}) {
            component->setExplicitFocusOrder(editor_order++);
        }
        performance_keyboard_.setExplicitFocusOrder(400);
    }

    void resetHistoryToCurrent() {
        history_.clear();
        history_.push_back(timbre_);
        history_cursor_ = 0;
        updateHistoryButtons();
    }

    void recordHistory() {
        if (history_.empty()) {
            history_.push_back(timbre_);
            history_cursor_ = 0;
            updateHistoryButtons();
            return;
        }
        if (history_[history_cursor_] == timbre_) {
            return;
        }
        history_.erase(
            history_.begin()
                + static_cast<std::ptrdiff_t>(history_cursor_ + 1),
            history_.end());
        history_.push_back(timbre_);
        if (history_.size() > 257) {
            history_.erase(history_.begin());
        } else {
            ++history_cursor_;
        }
        updateHistoryButtons();
    }

    void updateHistoryButtons() {
        undo_.setEnabled(history_cursor_ > 0);
        redo_.setEnabled(
            history_cursor_ + 1 < history_.size());
    }

    void restoreHistory() {
        if (history_cursor_ >= history_.size()) {
            return;
        }
        timbre_ = history_[history_cursor_];
        composite_program_stale_ = true;
        name_.setText(
            juce::String::fromUTF8(timbre_.name.c_str()), false);
        favorite_.setToggleState(
            timbre_.favorite, juce::dontSendNotification);
        refreshTimbreSelectors();
        syncControlsFromModel();
        timeline_.setTimbre(timbre_);
        updateHistoryButtons();
        resized();
        composite_program_stale_ = true;
        static_cast<void>(configureEngine());
        updateStatus(
            juce::String::fromUTF8(
                "履歴から総合音色を復元しました"));
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

    [[nodiscard]] juce::String compositeProgramFileText() const {
        const auto text =
            mgstc::engine::CompositeTimbreLibrary::serializeTimbreFile(
                timbre_, unixTimeNow());
        return juce::String::fromUTF8(
            text.data(), static_cast<int>(text.size()));
    }

    [[nodiscard]] std::optional<juce::String>
    compositeMgsSourceText() {
        const auto formatted =
            mgstc::engine::formatMgsComposite(
                timbre_, &timbre_library_);
        if (!formatted.valid()) {
            showCompositeError(
                juce::String::fromUTF8(
                    formatted.issues.empty()
                        ? "MGSCへ書き出せません"
                        : formatted.issues.front().c_str()));
            return std::nullopt;
        }
        return juce::String::fromUTF8(
            formatted.source.data(),
            static_cast<int>(formatted.source.size()));
    }

    void applyLoadedCompositeProgram(
        mgstc::engine::CompositeTimbre loaded,
        const juce::String& status) {
        timbre_ = std::move(loaded);
        selected_composite_id_.reset();
        composite_preview_.reset();
        composite_ab_.setEnabled(false);
        composite_program_stale_ = true;
        refreshTimbreSelectors();
        syncControlsFromModel();
        timeline_.setTimbre(timbre_, true);
        resetHistoryToCurrent();
        setEditorBaseline();
        resized();
        static_cast<void>(configureEngine());
        updateStatus(status);
    }

    void openCompositeFile() {
        juce::FileChooser chooser(
            juce::String::fromUTF8("総合音色を開く"),
            {},
            "*.mgstc");
        if (!chooser.browseForFileToOpen()) {
            return;
        }
        const auto text = chooser.getResult().loadFileAsString();
        std::string error;
        auto loaded =
            mgstc::engine::CompositeTimbreLibrary::deserializeTimbreFile(
                utf8Text(text), &error);
        if (!loaded) {
            showCompositeError(
                juce::String::fromUTF8(
                    "総合音色ファイルを読み取れませんでした"));
            return;
        }
        applyLoadedCompositeProgram(
            std::move(*loaded),
            juce::String::fromUTF8("総合音色ファイルを開きました"));
    }

    void saveCompositeFile() {
        const auto validation =
            mgstc::engine::validateCompositeTimbre(timbre_);
        if (!validation.valid()) {
            showCompositeError(
                juce::String::fromUTF8(
                    validation.warnings.empty()
                        ? "総合音色を保存できません"
                        : validation.warnings.front().c_str()));
            return;
        }
        juce::FileChooser chooser(
            juce::String::fromUTF8("総合音色を保存する"),
            juce::File::getCurrentWorkingDirectory()
                .getChildFile("composite-timbre.mgstc"),
            "*.mgstc");
        if (!chooser.browseForFileToSave(true)) {
            return;
        }
        const auto output =
            chooser.getResult().withFileExtension(".mgstc");
        const auto contents = compositeProgramFileText();
        if (!output.replaceWithText(contents)) {
            showCompositeError(
                juce::String::fromUTF8(
                    "総合音色ファイルを書き込めませんでした"));
            return;
        }
        updateStatus(
            juce::String::fromUTF8("総合音色をファイルへ保存しました"));
    }

    void openMgsCompositeFile() {
        juce::FileChooser chooser(
            juce::String::fromUTF8("MGSC総合音色を開く"),
            {},
            "*.mgs;*.mus;*.txt");
        if (!chooser.browseForFileToOpen()) {
            return;
        }
        const auto result = mgstc::engine::parseMgsComposite(
            utf8Text(chooser.getResult().loadFileAsString()));
        if (!result.valid()) {
            showCompositeError(
                juce::String::fromUTF8(
                    result.issues.empty()
                        ? "MGSCソースを読み取れませんでした"
                        : result.issues.front().c_str()));
            return;
        }
        applyLoadedCompositeProgram(
            result.timbre,
            juce::String::fromUTF8(
                "MGSC総合音色を開きました（編集用メタデータは既定値）"));
    }

    void saveMgsCompositeFile() {
        const auto source = compositeMgsSourceText();
        if (!source) {
            return;
        }
        juce::FileChooser chooser(
            juce::String::fromUTF8("MGSC総合音色を書き出す"),
            juce::File::getCurrentWorkingDirectory()
                .getChildFile("composite-timbre.mgs"),
            "*.mgs;*.mus;*.txt");
        if (!chooser.browseForFileToSave(true)) {
            return;
        }
        if (!chooser.getResult().replaceWithText(*source)) {
            showCompositeError(
                juce::String::fromUTF8(
                    "MGSCソースを書き込めませんでした"));
            return;
        }
        updateStatus(
            juce::String::fromUTF8(
                "総合音色をMGSCソースへ書き出しました"));
    }

    void copyCompositeProgram() {
        const auto text = compositeMgsSourceText();
        if (!text) {
            return;
        }
        if (!mgstc::platform::copyTextToClipboardUnicodeAndAnsi(
                std::wstring(text->toWideCharPointer()))) {
            juce::SystemClipboard::copyTextToClipboard(*text);
        }
        updateStatus(
            juce::String::fromUTF8(
                "総合音色をクリップボードへコピーしました"));
    }

    void pasteCompositeProgram() {
        const auto text =
            juce::SystemClipboard::getTextFromClipboard();
        if (text.isEmpty()) {
            showCompositeError(
                juce::String::fromUTF8(
                    "クリップボードにMGSC総合音色がありません"));
            return;
        }
        const auto loaded =
            mgstc::engine::parseMgsComposite(utf8Text(text));
        if (!loaded.valid()) {
            showCompositeError(
                juce::String::fromUTF8(
                    loaded.issues.empty()
                        ? "クリップボードのMGSC総合音色を読み取れませんでした"
                        : loaded.issues.front().c_str()));
            return;
        }
        applyLoadedCompositeProgram(
            loaded.timbre,
            juce::String::fromUTF8(
                "MGSC総合音色をクリップボードから貼り付けました"));
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
                    safe->recordHistory();
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
        timeline_.setTimbre(timbre_, true);
        setEditorBaseline();
        resetHistoryToCurrent();
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
        ScopedLibraryIpcLock lock(
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
        timeline_.setTimbre(timbre_, true);
        setEditorBaseline();
        resetHistoryToCurrent();
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

        ScopedLibraryIpcLock lock(
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
        ScopedLibraryIpcLock lock(library_lock_);
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
        ScopedLibraryIpcLock lock(library_lock_);
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
                ScopedLibraryIpcLock lock(
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
            item.tags = entry.tags;
            item.memo = juce::String::fromUTF8(entry.memo.c_str());
            item.last_used_unix_seconds = entry.last_used_unix_seconds;
            item.updated_unix_seconds = entry.updated_unix_seconds;
            items.push_back(std::move(item));
        }
        return items;
    }

    bool mutateSharedTimbreEntry(
        std::uint64_t id,
        const std::function<void(mgstc::engine::TimbreLibraryEntry&)>& mutator) {
        ScopedLibraryIpcLock lock(library_lock_);
        if (!lock.isLocked()) {
            return false;
        }
        const auto file = timbreLibraryFile();
        std::string error;
        std::optional<mgstc::engine::TimbreLibrary> loaded;
        if (file.existsAsFile()) {
            loaded = mgstc::engine::TimbreLibrary::deserialize(
                utf8Text(file.loadFileAsString()), &error);
            if (!loaded) {
                return false;
            }
        } else {
            loaded = mgstc::engine::TimbreLibrary{};
        }
        auto* entry = loaded->find(id);
        if (entry == nullptr) {
            return false;
        }
        auto copy = *entry;
        mutator(copy);
        if (!loaded->update(id, copy, unixTimeNow())
            || !persistTimbreLibrary(*loaded)) {
            return false;
        }
        timbre_library_ = std::move(*loaded);
        timbre_library_fingerprint_valid_ = false;
        return true;
    }

    bool mutateSharedTimbreName(std::uint64_t id, juce::String name) {
        const auto requested = utf8Text(name.trim());
        if (requested.empty()) {
            return false;
        }
        return mutateSharedTimbreEntry(id, [&](auto& entry) {
            entry.name = requested;
        });
    }

    bool mutateSharedTimbreTags(
        std::uint64_t id, std::vector<std::string> tags) {
        return mutateSharedTimbreEntry(id, [&](auto& entry) {
            entry.tags = std::move(tags);
        });
    }

    bool mutateSharedTimbreMemo(std::uint64_t id, juce::String memo) {
        const auto text = utf8Text(memo);
        return mutateSharedTimbreEntry(id, [&](auto& entry) {
            entry.memo = text;
        });
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
            recordHistory();
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
        recordHistory();
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
        recordHistory();
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
        MGSTC_UI_ACTIVITY("composite: configureEngine (compile + submit)");
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
            const bool rate_kind =
                layer.volume_envelope.kind
                == mgstc::engine::EnvelopeKind::Rate;
            if (rate_kind) {
                const auto rate = mgstc::engine::clampRateEnvelope(
                    layer.volume_envelope.rate);
                configured = configured
                    && edit.engine->session().setRateEnvelope(
                        track,
                        mgstc::app::rateDefinitionFrom(rate),
                        layer.volume);
            } else {
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
                        std::move(envelopes.timbre));
            }
            configured = configured
                && edit.engine->session().setTrackVolume(
                    track, layer.volume)
                && edit.engine->session().setTrackDetune(
                    track, layer.detune, layer.micro_detune)
                && edit.engine->session().setTrackPatch(
                    track,
                    mgstc::engine::layerBasePatchNumber(layer, &numbers))
                && edit.engine->session().setTrackSoftwareLfo(
                    track, layer.software_lfo)
                && edit.engine->session().setTrackPitchSweep(
                    track, layer.pitch_sweep)
                && edit.engine->session().setTrackKeyOffHang(
                    track, layer.key_off_hang)
                && edit.engine->session().setTrackOpllSustain(
                    track,
                    layer.source == mgstc::engine::TimbreSource::Opll
                        && layer.opll_sustain);
            if (layer.source == mgstc::engine::TimbreSource::Psg) {
                const auto rate = mgstc::engine::clampRateEnvelope(
                    layer.volume_envelope.rate);
                const auto mode = rate_kind ? rate.tone_mode : std::uint8_t{1};
                const auto noise = rate_kind ? rate.noise : std::uint8_t{0};
                configured = configured
                    && edit.engine->session().setPsgToneNoise(
                        track, mode, noise)
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
        // #region agent log
        const auto note_t0 = juce::Time::getMillisecondCounterHiRes();
        dbg7ae407(
            "H5",
            "main.cpp:startCompositeNote",
            "enter",
            std::string("{\"note\":") + std::to_string(base_note)
                + ",\"one_sec\":"
                + (stop_after_one_second ? "true" : "false")
                + ",\"stale\":"
                + (composite_program_stale_ ? "true" : "false")
                + ",\"voices\":"
                + std::to_string(voice_allocator_.activeVoiceCount())
                + "}");
        // #endregion
        if (stop_after_one_second || !performance_keyboard_.polyphonic()) {
            stopAudition();
        }
            if (!engine_ready_
            || (!already_configured
                && voice_allocator_.activeVoiceCount() == 0
                && composite_program_stale_
                && !configureEngine())) {
            // #region agent log
            dbg7ae407(
                "H5",
                "main.cpp:startCompositeNote",
                "abort",
                std::string("{\"note\":") + std::to_string(base_note)
                    + ",\"ms\":"
                    + std::to_string(
                        juce::Time::getMillisecondCounterHiRes() - note_t0)
                    + "}");
            // #endregion
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
            if (audition_layer_filter_
                && index != *audition_layer_filter_) {
                continue;
            }
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
            if (!audition_layer_filter_) {
                performance_keyboard_.showPreviewNote(base_note);
            }
        }
        last_audition_note_ = base_note;
        saveLastAuditionNoteSetting(last_audition_note_);
        // #region agent log
        dbg7ae407(
            "H5",
            "main.cpp:startCompositeNote",
            "exit",
            std::string("{\"note\":") + std::to_string(base_note)
                + ",\"ms\":"
                + std::to_string(
                    juce::Time::getMillisecondCounterHiRes() - note_t0)
                + "}");
        // #endregion
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
            // #region agent log
            dbg7ae407(
                "H4",
                "main.cpp:timerCallback",
                "scope repaint",
                std::string("{\"playing\":true}"));
            // #endregion
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
    juce::Label master_volume_label_;
    juce::DrawableButton file_open_{
        "file open", juce::DrawableButton::ImageOnButtonBackground};
    juce::DrawableButton file_save_{
        "file save", juce::DrawableButton::ImageOnButtonBackground};
    juce::DrawableButton file_paste_{
        "file paste", juce::DrawableButton::ImageOnButtonBackground};
    juce::DrawableButton file_copy_{
        "file copy", juce::DrawableButton::ImageOnButtonBackground};
    juce::TextButton mgsc_open_;
    juce::TextButton mgsc_save_;
    juce::DrawableButton undo_{
        "undo", juce::DrawableButton::ImageOnButtonBackground};
    juce::DrawableButton redo_{
        "redo", juce::DrawableButton::ImageOnButtonBackground};
    std::vector<mgstc::engine::CompositeTimbre> history_;
    std::size_t history_cursor_{};
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
    bool satellite_session_{};
    std::optional<std::size_t> audition_layer_filter_{};
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
        OpllCandidateCallback open_opll_candidates,
        TagManagementCallback manage_tags,
        bool envelope_context = false)
        : open_editor_(std::move(open_editor)),
          open_opll_candidates_(std::move(open_opll_candidates)),
          manage_tags_(std::move(manage_tags)),
          audio_service_(audio_service),
          engine_(audio_service.engine()),
          tooltip_window_(this, 450),
          performance_keyboard_(midi_service, audio_service_),
          envelope_context_(envelope_context) {
        setWantsKeyboardFocus(true);
        scc_wave_ = mgstc::engine::generateSccPreset(
            mgstc::engine::SccWavePreset::Sine,
            mgstc::engine::SccHarmonic::One);
        history_.push_back(scc_wave_);
        last_audition_note_ = loadLastAuditionNoteSetting();

        title_.setText(
            juce::String::fromUTF8(
                envelope_context_
                    ? "総合音色編集"
                    : "SCC音色エディタ"),
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
        configureMasterVolumeLabel(master_volume_label_);
        addAndMakeVisible(master_volume_label_);
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
        preset_.onChange = [this] {
            if (isRandomPresetSelected()) {
                rollRandomPresetRecipe();
            } else {
                random_preset_recipe_.reset();
            }
            updatePresetModeControlState();
            refreshPresetPreview();
        };
        addAndMakeVisible(preset_);
        preset_preview_.setInterceptsMouseClicks(false, false);
        addAndMakeVisible(preset_preview_);
        configureButton(
            random_reroll_,
            juce::String::fromUTF8("再抽選"),
            juce::String::fromUTF8(
                "Randomのプリセット、倍音、反転、マージ条件、適用範囲を再抽選します"),
            [this] {
                rollRandomPresetRecipe();
                refreshPresetPreview();
            });
        random_reroll_.setVisible(false);

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
                &random_reroll_,
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
            ScopedLibraryIpcLock lock(library_lock_);
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
        MGSTC_UI_ACTIVITY("library: scc audition preview");
        ScopedLibraryIpcLock lock(library_lock_);
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
        ScopedLibraryIpcLock lock(library_lock_);
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
        master_volume_label_.setFont(UiFonts::body());
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
            &master_volume_label_,
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
        if (random_reroll_.isVisible()) {
            random_reroll_.setBounds(
                preset_row.removeFromLeft(libraryButtonMinW));
            preset_row.removeFromLeft(controlGap);
        } else {
            random_reroll_.setBounds({});
        }
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
            ? juce::jlimit(1, 7, preset_.getSelectedId())
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
        menu->addItem(
            7,
            juce::String::fromUTF8("Random"));
        preset_.setSelectedId(selected, juce::dontSendNotification);
    }

    [[nodiscard]] bool hasPresetSelection() const noexcept {
        const auto value = preset_.getSelectedId();
        return value >= 1 && value <= 7;
    }

    [[nodiscard]] bool isRandomPresetSelected() const noexcept {
        return preset_.getSelectedId() == 7;
    }

    void rollRandomPresetRecipe() {
        const auto seed =
            static_cast<std::uint32_t>(
                juce::Random::getSystemRandom().nextInt())
            ^ static_cast<std::uint32_t>(
                ++random_roll_serial_ * 0x9E3779B9U)
            ^ juce::Time::getMillisecondCounter();
        random_preset_recipe_ =
            mgstc::engine::makeSccRandomPresetRecipe(seed);
        const auto range =
            random_preset_recipe_->apply_range
            == mgstc::engine::SccApplyRange::LeftHalf
                ? 2
            : random_preset_recipe_->apply_range
                    == mgstc::engine::SccApplyRange::RightHalf
                ? 3
                : 1;
        apply_range_.setSelectedId(
            range, juce::dontSendNotification);
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
                if (safe->open_opll_candidates_) {
                    safe->open_opll_candidates_(
                        std::move(result.candidates));
                } else {
                    safe->open_editor_("opll", std::nullopt);
                }
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
        ScopedLibraryIpcLock lock(
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
        ScopedLibraryIpcLock lock(
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
        ScopedLibraryIpcLock lock(library_lock_);
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
                ScopedLibraryIpcLock lock(
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
        ScopedLibraryIpcLock lock(
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
            ScopedLibraryIpcLock lock(
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
        auto mergeAndApplyRange =
            [this](SccWaveform generated) {
                if (merge_enabled_.getToggleState()) {
                    generated = mgstc::engine::mergeSccWaveforms(
                        scc_wave_,
                        generated,
                        {
                            .amount = merge_amount_.getValue() / 100.0,
                            .auto_phase = auto_phase_.getToggleState(),
                            .allow_polarity_inversion =
                                polarity_.getToggleState(),
                            .preserve_volume =
                                preserve_volume_.getToggleState(),
                        })
                        .waveform;
                }
                return mgstc::engine::applySccWaveformRange(
                    scc_wave_, generated, selectedApplyRange());
            };
        if (isRandomPresetSelected() && random_preset_recipe_) {
            return mergeAndApplyRange(
                mgstc::engine::generateSccRandomPreset(
                    *random_preset_recipe_));
        }
        auto generated = mgstc::engine::generateSccPreset(
            selectedPreset(), selectedHarmonic());
        if (preset_flip_h_.getToggleState()) {
            generated = mgstc::engine::mirrorSccWaveform(generated);
        }
        if (preset_flip_v_.getToggleState()) {
            generated = mgstc::engine::invertSccWaveform(generated);
        }
        return mergeAndApplyRange(std::move(generated));
    }

    [[nodiscard]] SccWaveform makePresetPreviewWave() const {
        if (isRandomPresetSelected() && random_preset_recipe_) {
            return mgstc::engine::generateSccRandomPreset(
                *random_preset_recipe_);
        }
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

    [[nodiscard]] mgstc::engine::SccApplyRange
    previewApplyRange() const noexcept {
        return selectedApplyRange();
    }

    [[nodiscard]] juce::String randomPresetDescription() const {
        if (!random_preset_recipe_) {
            return juce::String::fromUTF8("Random");
        }
        constexpr std::array<const char*, 6> preset_names{
            "Sine", "Square", "Triangle", "Saw",
            "Pulse 25%", "Pulse 12.5%"};
        constexpr std::array<const char*, 5> harmonic_names{
            "1x", "1.5x", "2x", "3x", "4x"};
        juce::String result = juce::String::fromUTF8("Random: ");
        for (std::size_t index = 0;
             index < random_preset_recipe_->stage_count;
             ++index) {
            if (index != 0) {
                result += " + ";
            }
            const auto& stage = random_preset_recipe_->stages[index];
            result += preset_names[static_cast<std::size_t>(stage.preset)];
            result += " ";
            result += harmonic_names[static_cast<std::size_t>(stage.harmonic)];
            if (stage.flip_horizontal) {
                result += "/左右";
            }
            if (stage.flip_vertical) {
                result += "/上下";
            }
            if (index != 0) {
                result += "/M";
                result += juce::String(
                    juce::roundToInt(stage.merge.amount * 100.0));
                result += "%";
                if (stage.merge.auto_phase) {
                    result += "/位相";
                }
                if (stage.merge.allow_polarity_inversion) {
                    result += "/極性";
                }
                if (stage.merge.preserve_volume) {
                    result += "/音量";
                }
            }
        }
        const char* range = selectedApplyRange()
                == mgstc::engine::SccApplyRange::LeftHalf
            ? "／左半分"
            : selectedApplyRange()
                    == mgstc::engine::SccApplyRange::RightHalf
                ? "／右半分"
                : "／全体";
        return result + juce::String::fromUTF8(range);
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
        if (isRandomPresetSelected() && !random_preset_recipe_) {
            rollRandomPresetRecipe();
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
        graph_.setApplyRange(previewApplyRange());
        graph_.setPreview(scc_wave_, preview_wave_);
        if (audition && engine_ready_) {
            static_cast<void>(auditionAfterEdit(&preview_wave_));
        }
        if (isRandomPresetSelected()) {
            updateStatus(
                randomPresetDescription()
                + juce::String::fromUTF8(
                    " をプレビュー中（再抽選／適用／取消）"));
        } else {
            updateStatus(
                merge_enabled_.getToggleState()
                    ? juce::String::fromUTF8(
                        "マージ候補をプレビュー中（適用／取消）")
                    : juce::String::fromUTF8(
                        "プリセット候補をプレビュー中（適用／取消）"));
        }
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

    void updatePresetModeControlState() {
        const bool random = isRandomPresetSelected();
        random_reroll_.setVisible(random);
        random_reroll_.setEnabled(random);
        harmonic_.setEnabled(!random);
        apply_range_.setEnabled(true);
        merge_enabled_.setEnabled(true);
        preset_flip_h_.setEnabled(!random);
        preset_flip_v_.setEnabled(!random);
        updateMergeControlState();
        resized();
    }

    void updateMergeControlState() {
        const auto enabled =
            merge_enabled_.getToggleState();
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
        MGSTC_UI_ACTIVITY("scc: configureEngine (shared program)");
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
    OpllCandidateCallback open_opll_candidates_;
    TagManagementCallback manage_tags_;
    SharedAudioService& audio_service_;
    mgstc::engine::RealtimeEngineHost& engine_;
    juce::TooltipWindow tooltip_window_;
    SwitchLookAndFeel switch_look_and_feel_;
    bool envelope_context_{};
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
    juce::TextButton random_reroll_;
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
    juce::Label master_volume_label_;
    PerformanceKeyboard performance_keyboard_;
    mgstc::engine::SequentialVoiceAllocator voice_allocator_{5};
    SccWaveform scale_source_{};
    SccWaveform preview_wave_{};
    std::optional<mgstc::engine::SccRandomPresetRecipe>
        random_preset_recipe_;
    std::uint32_t random_roll_serial_{};
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

class OpllEditorComponent final
    : public juce::Component,
      private juce::Timer {
public:
    explicit OpllEditorComponent(
        SharedAudioService& audio_service,
        SharedMidiInputService& midi_service,
        EditorOpenCallback open_editor,
        TagManagementCallback manage_tags,
        bool envelope_context = false)
        : open_editor_(std::move(open_editor)),
          manage_tags_(std::move(manage_tags)),
          audio_service_(audio_service),
          engine_(audio_service.engine()),
          tooltip_window_(this, 450),
          patch_panel_(switch_look_and_feel_),
          performance_keyboard_(midi_service, audio_service_),
          envelope_context_(envelope_context) {
        setWantsKeyboardFocus(true);
        patch_ = mgstc::engine::defaultOpllPatch();
        history_.push_back(patch_);
        last_audition_note_ = loadLastAuditionNoteSetting();

        title_.setText(
            juce::String::fromUTF8(
                envelope_context_
                    ? "総合音色編集"
                    : "OPLL音色エディタ"),
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
        configureMasterVolumeLabel(master_volume_label_);
        addAndMakeVisible(master_volume_label_);
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
            juce::String::fromUTF8("前のOPLL近似候補"),
            [this] { selectAdjacentWaveCandidate(-1); });
        configureButton(
            wave_next_,
            juce::String::fromUTF8("▶"),
            juce::String::fromUTF8("次のOPLL近似候補"),
            [this] { selectAdjacentWaveCandidate(1); });
        wave_candidate_label_.setText(
            juce::String::fromUTF8("OPLL候補 --/--"),
            juce::dontSendNotification);
        wave_candidate_label_.setJustificationType(
            juce::Justification::centred);
        addAndMakeVisible(wave_candidate_label_);
        updateWaveCandidateControls();

        addAndMakeVisible(patch_panel_);
        patch_panel_.setAuditionNote(last_audition_note_);
        patch_panel_.onChange = [this](bool commit) {
            controlsChanged(commit);
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
                &patch_panel_,
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

    void receiveConversionCandidates(
        std::vector<mgstc::engine::OpllPatchParameters> candidates) {
        if (candidates.empty()) {
            return;
        }
        wave_candidates_ = std::move(candidates);
        wave_candidate_index_ = 0;
        commitPatch(wave_candidates_.front(), false);
        updateWaveCandidateControls();
        updateStatus(
            juce::String::fromUTF8("OPLL近似候補 ")
            + juce::String(
                static_cast<int>(wave_candidates_.size()))
            + juce::String::fromUTF8("件を読み込みました"));
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
            ScopedLibraryIpcLock lock(library_lock_);
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
        MGSTC_UI_ACTIVITY("library: opll audition preview");
        ScopedLibraryIpcLock lock(library_lock_);
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
        ScopedLibraryIpcLock lock(library_lock_);
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
        master_volume_label_.setFont(UiFonts::body());
        library_title_.setFont(UiFonts::heading());
        mgsc_title_.setFont(UiFonts::heading());
        scope_title_.setFont(UiFonts::heading());
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
            &master_volume_label_,
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

        auto operators = area.removeFromTop(UiScale::sx(470));
        patch_panel_.setBounds(operators);

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
                ? juce::String::fromUTF8("OPLL候補 --/--")
                : juce::String::fromUTF8("OPLL候補 ")
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
            juce::String::fromUTF8("OPLL近似候補 ")
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
        ScopedLibraryIpcLock lock(
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
        ScopedLibraryIpcLock lock(
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
        ScopedLibraryIpcLock lock(library_lock_);
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
                ScopedLibraryIpcLock lock(
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
        ScopedLibraryIpcLock lock(
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
            ScopedLibraryIpcLock lock(
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
        patch_panel_.setParameters(patch_);
        syncing_ = false;
        refreshEnvelopeTrace();
    }

    void refreshEnvelopeTrace() {
        patch_panel_.setAuditionNote(last_audition_note_);
        patch_panel_.refreshEnvelopeTrace();
    }

    void updateAuditionNoteLabels() {
        patch_panel_.setAuditionNote(last_audition_note_);
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
        patch_ = patch_panel_.parameters();
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
        controlsChanged(commit);
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
        MGSTC_UI_ACTIVITY("opll: configureEngine (shared program)");
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
    bool envelope_context_{};
    mgstc::engine::OpllPatchParameters patch_;
    std::optional<int> ui_scale_session_override_;
    std::vector<mgstc::engine::OpllPatchParameters> history_;
    std::size_t history_cursor_{};
    std::vector<mgstc::engine::OpllPatchParameters>
        wave_candidates_;
    std::size_t wave_candidate_index_{};
    OpllPatchParameterPanel patch_panel_;
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
    juce::Label master_volume_label_;
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

class MainWindow final : public juce::DocumentWindow {
public:
    MainWindow(
        const juce::String& name,
        const juce::String& editor,
        SharedAudioService& audio_service,
        SharedMidiInputService& midi_service,
        EditorOpenCallback open_editor,
        OpllCandidateCallback open_opll_candidates,
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
        if (editor.equalsIgnoreCase("opll")
            || editor.equalsIgnoreCase("opll-envelope")) {
            setContentOwned(
                new OpllEditorComponent(
                    audio_service, midi_service, open_editor,
                    manage_tags,
                    editor.equalsIgnoreCase("opll-envelope")), true);
        } else if (editor.equalsIgnoreCase("scc")
                   || editor.equalsIgnoreCase("scc-envelope")) {
            setContentOwned(
                new SccEditorComponent(
                    audio_service, midi_service, open_editor,
                    open_opll_candidates,
                    manage_tags,
                    editor.equalsIgnoreCase("scc-envelope")), true);
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

    void receiveOpllConversionCandidates(
        std::vector<mgstc::engine::OpllPatchParameters> candidates) {
        if (auto* opll =
                dynamic_cast<OpllEditorComponent*>(
                    getContentComponent())) {
            opll->receiveConversionCandidates(std::move(candidates));
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
        if (editor_ == "opll-envelope") {
            return juce::String::fromUTF8("総合音色編集（OPLL）");
        }
        if (editor_ == "scc-envelope") {
            return juce::String::fromUTF8("総合音色編集（SCC）");
        }
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
        // DefWindowProc title-bar move / border resize owns the thread in a
        // nested modal loop; JUCE timers (hang heartbeat) do not run. Mark
        // that expected stall so the watchdog does not write false hangs.
        if (msg == WM_ENTERSIZEMOVE) {
            mgstc::app::enterUiHangNativeModal();
        } else if (msg == WM_EXITSIZEMOVE) {
            mgstc::app::exitUiHangNativeModal();
        }
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
        opll_envelope_window_.reset();
        scc_envelope_window_.reset();
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
        if (editor.equalsIgnoreCase("opll-envelope")) {
            return "opll-envelope";
        }
        if (editor.equalsIgnoreCase("scc-envelope")) {
            return "scc-envelope";
        }
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
        if (editor == "opll-envelope") {
            return opll_envelope_window_;
        }
        if (editor == "scc-envelope") {
            return scc_envelope_window_;
        }
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
            if (editor == "scc-envelope") {
                title += juce::String::fromUTF8(" - 総合音色編集（SCC）");
            } else if (editor == "opll-envelope") {
                title += juce::String::fromUTF8(" - 総合音色編集（OPLL）");
            } else if (editor == "scc") {
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
                [this](
                    std::vector<
                        mgstc::engine::OpllPatchParameters> candidates) {
                    if (auto* opened = showEditor("opll")) {
                        opened->receiveOpllConversionCandidates(
                            std::move(candidates));
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
        // Bounded IPC wait (see ScopedLibraryIpcLock); a second MGSTC
        // instance holding the shared library must not stall forever.
        MGSTC_UI_ACTIVITY(
            "library: manager open (shared lock + load)");
        ScopedLibraryIpcLock lock(
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
             std::array<MainWindow*, 5>{
                 main_window_.get(),
                 scc_window_.get(),
                 opll_window_.get(),
                 scc_envelope_window_.get(),
                 opll_envelope_window_.get()}) {
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
             std::array<MainWindow*, 5>{
                 main_window_.get(),
                 scc_window_.get(),
                 opll_window_.get(),
                 scc_envelope_window_.get(),
                 opll_envelope_window_.get()}) {
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
        ScopedLibraryIpcLock lock(
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
             std::array<MainWindow*, 5>{
                 main_window_.get(),
                 scc_window_.get(),
                 opll_window_.get(),
                 scc_envelope_window_.get(),
                 opll_envelope_window_.get()}) {
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
        append_if_dirty(scc_envelope_window_);
        append_if_dirty(opll_envelope_window_);
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
            deactivate_if_other("scc-envelope", scc_envelope_window_);
            deactivate_if_other("opll-envelope", opll_envelope_window_);
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
        ScopedLibraryIpcLock lock(
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
    std::unique_ptr<MainWindow> scc_envelope_window_;
    std::unique_ptr<MainWindow> opll_envelope_window_;
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
