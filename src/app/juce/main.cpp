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
#include "tone_library_session.hpp"
#include "tone_import_tab.hpp"
#include "import_preview_engine.hpp"

#include "switch_look_and_feel.hpp"
#include "mgstc_look_and_feel.hpp"
#include "about_panel.hpp"
#include "spectrogram_window.hpp"
#include "mgstc/audio/wasapi_audio_sink.hpp"
#include "mgstc/engine/engine_command.hpp"
#include "mgstc/engine/chip_rack.hpp"
#include "mgstc/engine/chip_volume_curve.hpp"
#include "mgstc/engine/composite_timbre.hpp"
#include "mgstc/engine/composite_program_compiler.hpp"
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
#include "editor_session.hpp"
#include "mgstc_editor_view.hpp"

// Supplied by CMake from the SPECIFICATION.md document version.
#ifndef MGSTC_DOC_VERSION
#define MGSTC_DOC_VERSION "0.0"
#endif

namespace {

using mgstc::app::CompositeTimeline;
using mgstc::app::EnvelopeTimbreCatalogItem;
using mgstc::app::LayerBaseTimbreAssign;
using mgstc::app::LibraryFileFingerprint;
using mgstc::app::ScopedLibraryIpcLock;
using mgstc::app::ToneLibrarySnapshot;
using mgstc::app::currentUnixTime;
using mgstc::app::libraryFileFingerprint;
using mgstc::app::loadSharedCompositeTimbreLibrary;
using mgstc::app::loadSharedTimbreLibrary;
using mgstc::app::loadToneLibraries;
using mgstc::app::persistCompositeTimbreLibrary;
using mgstc::app::persistTimbreLibrary;
using mgstc::app::persistToneLibraries;
using mgstc::app::toneLibraryFile;
using mgstc::app::CompositeEditorComponent;
using mgstc::app::EditorActivateCallback;
using mgstc::app::EditorCloseCallback;
using mgstc::app::EditorLink;
using mgstc::app::EditorOpenCallback;
using mgstc::app::EditorSession;
using mgstc::app::LibrariesChangedCallback;
using mgstc::app::LibraryManagerContent;
using mgstc::app::LibraryManagerWindow;
using mgstc::app::OpllCandidateCallback;
using mgstc::app::OpllEditorComponent;
using mgstc::app::SccEditorComponent;
using mgstc::app::SnapshotResult;
using mgstc::app::SpectrogramOpenCallback;
using mgstc::app::SpectrogramSourceMaskCallback;
using mgstc::app::TagManagementCallback;
using mgstc::app::writeComponentPngSnapshot;

static_assert(
    sizeof(mgstc::engine::RealtimeEngineHost) < 128 * 1024,
    "RealtimeEngineHost must remain safe to create in an editor");

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
// The same subclass rewrites WM_WINDOWPOSCHANGING so a pinned spectrogram
// stays above this editor before paint (the old 15 Hz toFront flashed).
// Deadline so a button flag that never clears (capture lost to another app)
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

[[nodiscard]] juce::File applicationDataDirectory() {
    return mgstcApplicationDataDirectory();
}

using mgstc::app::PcAudioBackend;
using SccWaveform = mgstc::engine::SccWaveform;

class SharedAudioService final : public SharedAudioHost, private juce::Timer {
public:
    explicit SharedAudioService(bool offline_snapshot_capture = false)
        : offline_snapshot_capture_(offline_snapshot_capture) {
        master_volume_percent_ = loadMasterVolumePercent();
        engine_.setSpectrumOutputGain(static_cast<float>(master_volume_percent_) / 100.0F);
        wasapi_audio_.setMasterVolumePercent(
            static_cast<std::uint32_t>(master_volume_percent_));
        asio_audio_.setMasterVolumePercent(
            static_cast<std::uint32_t>(master_volume_percent_));
        loadAndApplySoundOutputSettings();
        if (!offline_snapshot_capture_) {
            loadAndStartPcAudioSettings();
        }
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
        return offline_snapshot_capture_
            || (active_audio_ != nullptr && active_audio_->running());
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
        engine_.setSpectrumOutputGain(static_cast<float>(next) / 100.0F);
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

    void setCompositeOwnedEditTarget(
        std::optional<mgstc::engine::SavedTimbreReference> target) {
        composite_owned_edit_target_ = std::move(target);
    }

    [[nodiscard]] const std::optional<mgstc::engine::SavedTimbreReference>&
    compositeOwnedEditTarget() const noexcept {
        return composite_owned_edit_target_;
    }

    void publishCompositeOwnedTimbre(
        mgstc::engine::SavedTimbreReference snapshot) {
        published_composite_owned_ = std::move(snapshot);
        ++composite_owned_timbre_revision_;
    }

    struct CompositeOwnedAuditionCallbacks {
        std::function<void()> sync_program;
        std::function<void()> one_second;
        std::function<void(std::uint8_t)> note_on;
        std::function<void(std::uint8_t)> note_off;
        std::function<void()> stop_preview;
    };

    void setCompositeOwnedAuditionCallbacks(
        CompositeOwnedAuditionCallbacks callbacks) {
        composite_owned_audition_ = std::move(callbacks);
    }

    [[nodiscard]] const CompositeOwnedAuditionCallbacks&
    compositeOwnedAuditionCallbacks() const noexcept {
        return composite_owned_audition_;
    }

    [[nodiscard]] bool compositeOwnedAuditionReady() const noexcept {
        return composite_owned_audition_.sync_program
            && composite_owned_audition_.one_second
            && composite_owned_audition_.note_on
            && composite_owned_audition_.note_off
            && composite_owned_audition_.stop_preview;
    }

    [[nodiscard]] std::uint64_t compositeOwnedTimbreRevision() const noexcept {
        return composite_owned_timbre_revision_;
    }

    [[nodiscard]] const std::optional<mgstc::engine::SavedTimbreReference>&
    publishedCompositeOwnedTimbre() const noexcept {
        return published_composite_owned_;
    }

    [[nodiscard]] bool sharedEditorProgramActive() const noexcept {
        return shared_editor_program_active_;
    }

    void invalidateSharedEditorProgram() noexcept override {
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
            if (running()) {
                static_cast<void>(
                    engine_.waitForPendingProgramActivation());
            }
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
    std::optional<mgstc::engine::SavedTimbreReference>
        composite_owned_edit_target_;
    std::optional<mgstc::engine::SavedTimbreReference>
        published_composite_owned_;
    CompositeOwnedAuditionCallbacks composite_owned_audition_;
    std::uint64_t composite_owned_timbre_revision_{1};
    bool shared_editor_program_active_{};
    PcAudioBackend pc_audio_backend_{PcAudioBackend::Wasapi};
    std::string asio_driver_name_;
    std::string pc_audio_note_;
    bool suppress_pc_audio_fallback_{};
    bool offline_snapshot_capture_{};
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

        if (!engine_.submitProgram(edit)) {
            static_cast<void>(engine_.discardProgramEdit(edit));
            return false;
        }
        return true;
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

#include "standalone_editor_context.hpp"

class MainWindow final : public juce::DocumentWindow {
public:
    MainWindow(
        const juce::String& name,
        const juce::String& editor,
        EditorSession& session,
        EditorLink& link,
        EditorOpenCallback open_editor,
        SpectrogramOpenCallback open_spectrogram,
        SpectrogramSourceMaskCallback spectrogram_source_mask_changed,
        OpllCandidateCallback open_opll_candidates,
        TagManagementCallback manage_tags,
        LibrariesChangedCallback libraries_changed,
        EditorCloseCallback close_editor,
        EditorActivateCallback activate_editor)
        : DocumentWindow(
            name,
            juce::Desktop::getInstance()
                .getDefaultLookAndFeel()
                .findColour(
                    juce::ResizableWindow::backgroundColourId),
            DocumentWindow::allButtons,
            false),
          editor_(editor),
          close_editor_(std::move(close_editor)),
          activate_editor_(std::move(activate_editor)) {
        setUsingNativeTitleBar(true);
        if (editor.equalsIgnoreCase("opll")
            || editor.equalsIgnoreCase("opll-envelope")) {
            setContentOwned(
                new OpllEditorComponent(
                    session, link, open_editor,
                    open_spectrogram,
                    manage_tags,
                    editor.equalsIgnoreCase("opll-envelope"),
                    libraries_changed), true);
        } else if (editor.equalsIgnoreCase("scc")
                   || editor.equalsIgnoreCase("scc-envelope")) {
            setContentOwned(
                new SccEditorComponent(
                    session, link, open_editor,
                    open_spectrogram,
                    open_opll_candidates,
                    manage_tags,
                    editor.equalsIgnoreCase("scc-envelope"),
                    libraries_changed), true);
        } else {
            setContentOwned(
                new CompositeEditorComponent(
                    session, link, open_editor,
                    open_spectrogram,
                    std::move(spectrogram_source_mask_changed),
                    manage_tags,
                    std::move(libraries_changed)), true);
        }
        setResizable(true, false);
        centreWithSize(getWidth(), getHeight());
        clampWindowToDisplayWorkArea(*this);
        setWantsKeyboardFocus(true);
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

    [[nodiscard]] bool polyphonic() const {
        if (auto* composite =
                dynamic_cast<CompositeEditorComponent*>(
                    getContentComponent())) {
            return composite->polyphonic();
        }
        if (auto* scc = dynamic_cast<SccEditorComponent*>(
                getContentComponent())) {
            return scc->polyphonic();
        }
        if (auto* opll = dynamic_cast<OpllEditorComponent*>(
                getContentComponent())) {
            return opll->polyphonic();
        }
        return true;
    }

    void setSpectrogramInputActive(bool active) noexcept {
        if (auto* composite =
                dynamic_cast<CompositeEditorComponent*>(
                    getContentComponent())) {
            composite->setSpectrogramInputActive(active);
        } else if (auto* scc = dynamic_cast<SccEditorComponent*>(
                       getContentComponent())) {
            scc->setSpectrogramInputActive(active);
        } else if (auto* opll = dynamic_cast<OpllEditorComponent*>(
                       getContentComponent())) {
            opll->setSpectrogramInputActive(active);
        }
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

    void auditionImportedCandidate(
        const mgstc::engine::ImportedToneCandidate& candidate) {
        if (candidate.default_register_as
            == mgstc::engine::ImportRegisterAs::Composite) {
            if (auto* composite =
                    dynamic_cast<CompositeEditorComponent*>(
                        getContentComponent())) {
                composite->auditionImportedCandidate(candidate);
            }
            return;
        }
        if (candidate.default_register_as
            == mgstc::engine::ImportRegisterAs::Scc) {
            if (auto* scc = dynamic_cast<SccEditorComponent*>(
                    getContentComponent())) {
                scc->auditionImportedCandidate(candidate);
            }
            return;
        }
        if (auto* opll = dynamic_cast<OpllEditorComponent*>(
                getContentComponent())) {
            opll->auditionImportedCandidate(candidate);
        }
    }

    void silenceImportedPreview() {
        if (auto* composite =
                dynamic_cast<CompositeEditorComponent*>(
                    getContentComponent())) {
            composite->stopImportedPreview();
            return;
        }
        if (auto* scc = dynamic_cast<SccEditorComponent*>(
                getContentComponent())) {
            scc->stopImportedPreview();
            return;
        }
        if (auto* opll = dynamic_cast<OpllEditorComponent*>(
                getContentComponent())) {
            opll->stopImportedPreview();
        }
    }

    void loadImportedCandidate(
        const mgstc::engine::ImportedToneCandidate& candidate) {
        if (candidate.default_register_as
            == mgstc::engine::ImportRegisterAs::Composite) {
            if (auto* composite =
                    dynamic_cast<CompositeEditorComponent*>(
                        getContentComponent())) {
                composite->loadImportedCandidate(candidate);
            }
            return;
        }
        if (candidate.default_register_as
            == mgstc::engine::ImportRegisterAs::Scc) {
            if (auto* scc = dynamic_cast<SccEditorComponent*>(
                    getContentComponent())) {
                scc->loadImportedCandidate(candidate);
            }
            return;
        }
        if (auto* opll = dynamic_cast<OpllEditorComponent*>(
                getContentComponent())) {
            opll->loadImportedCandidate(candidate);
        }
    }

    void libraryManagerImportedNoteOn(
        const mgstc::engine::ImportedToneCandidate& candidate,
        std::uint8_t note) {
        if (candidate.default_register_as
            == mgstc::engine::ImportRegisterAs::Composite) {
            if (auto* composite =
                    dynamic_cast<CompositeEditorComponent*>(
                        getContentComponent())) {
                composite->libraryManagerImportedNoteOn(candidate, note);
            }
            return;
        }
        if (candidate.default_register_as
            == mgstc::engine::ImportRegisterAs::Scc) {
            if (auto* scc = dynamic_cast<SccEditorComponent*>(
                    getContentComponent())) {
                scc->libraryManagerImportedNoteOn(candidate, note);
            }
            return;
        }
        if (auto* opll = dynamic_cast<OpllEditorComponent*>(
                getContentComponent())) {
            opll->libraryManagerImportedNoteOn(candidate, note);
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
        if (!isActiveWindow() || !isShowing()) {
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

    void restoreClientKeyboardFocus(bool only_if_still_active = false) {
        // grabKeyboardFocus on a showing-but-inactive DocumentWindow makes
        // that window the OS foreground. A 1ms delayed restore queued while
        // 総合 was active (modal 音色設定 closing) must not run after
        // 総合音色編集 has already taken the foreground, or the two windows
        // bounce forever via activateEditor / toFront.
        if (only_if_still_active && !isActiveWindow()) {
            return;
        }
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
        // Background import preview builds editors with addToDesktop deferred.
        // A hidden peer must not call activateEditor and deactivate 総合.
        if (!isShowing()) {
            return;
        }
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
            safe_this->restoreClientKeyboardFocus(true);
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
        if (msg == WM_WINDOWPOSCHANGING) {
            mgstc::app::SpectrogramWindow::constrainSiblingZOrder(
                reinterpret_cast<void*>(lParam),
                hwnd);
        } else if (msg == WM_WINDOWPOSCHANGED) {
            if (const auto* position =
                    reinterpret_cast<const WINDOWPOS*>(lParam);
                position != nullptr
                && (position->flags & SWP_NOZORDER) == 0) {
                mgstc::app::SpectrogramWindow::raisePinnedAfterSibling(hwnd);
            }
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
        const juce::ArgumentList arguments(
            getApplicationName(),
            command_line);
        const auto snapshot_target = arguments
            .getValueForOption("--capture-target")
            .trim()
            .toLowerCase();
        offline_spectrogram_capture_ =
            arguments.getValueForOption("--capture-ui").isNotEmpty()
            && (snapshot_target == "spectrogram" || snapshot_target == "spectrum");
        audio_service_ = std::make_unique<SharedAudioService>(
            offline_spectrogram_capture_);
        midi_service_ = std::make_unique<SharedMidiInputService>();
        editor_session_ = std::make_unique<StandaloneEditorContext>(
            *audio_service_, *midi_service_);
        wireCompositeOwnedAuditionCallbacks();
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
            showSpectrogram();
            auto* const first_spectrogram = spectrogram_window_.get();
            showSpectrogram();
            first_spectrogram->closeButtonPressed();
            const bool spectrogram_input_released =
                !spectrogram_performance_input_active_
                && spectrogram_input_editor_.isEmpty()
                && !first_spectrogram->isVisible();
            showSpectrogram();
            routing_test_passed_ = main != nullptr
                && first_scc != nullptr
                && opll != nullptr
                && first_scc == reopened_scc
                && main->isVisible()
                && reopened_scc->isVisible()
                && opll->isVisible()
                && first_spectrogram != nullptr
                && first_spectrogram == spectrogram_window_.get()
                && first_spectrogram->isVisible()
                && spectrogram_performance_input_active_
                && spectrogram_input_editor_ == active_editor_
                && spectrogram_input_released;
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
            if (snapshot_target_ == "spectrogram" || snapshot_target_ == "spectrum") {
                showSpectrogram();
                spectrogram_window_->setSpectrumMode(snapshot_target_ == "spectrum");
                // Enable the analyzer before starting the one-second audition,
                // otherwise a fast initial note can complete before the
                // capture queue begins receiving PCM frames.
                snapshot_window_->prepareVisualInspection();
            } else if (snapshot_target_ == "editor") {
                snapshot_window_->prepareVisualInspection();
            }
            if (offline_spectrogram_capture_) {
                snapshot_due_ms_ =
                    juce::Time::getMillisecondCounterHiRes() + 900.0;
                startTimer(10);
            } else {
                startTimer((snapshot_target_ == "spectrogram" || snapshot_target_ == "spectrum") ? 900 : 500);
            }
        }
    }

    void shutdown() override {
        stopTimer();
        hang_watchdog_.stop();
        snapshot_window_ = nullptr;
        spectrogram_window_.reset();
        library_manager_window_.reset();
        opll_envelope_window_.reset();
        scc_envelope_window_.reset();
        opll_window_.reset();
        scc_window_.reset();
        main_window_.reset();
        editor_session_.reset();
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

    [[nodiscard]] mgstc::app::SpectrogramAnalysisMode
    spectrogramAnalysisModeForEditor(const juce::String& editor) const noexcept {
        if (editor == "scc" || editor == "scc-envelope") {
            return mgstc::app::SpectrogramAnalysisMode::SccOnly;
        }
        if (editor == "opll" || editor == "opll-envelope") {
            return mgstc::app::SpectrogramAnalysisMode::OpllOnly;
        }
        return static_cast<mgstc::app::SpectrogramAnalysisMode>(
            composite_spectrogram_source_mask_);
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
        bool bring_to_front,
        bool show = true) {
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
                *editor_session_,
                editor_link_,
                [this](
                    const juce::String& target,
                    std::optional<std::uint64_t> library_id) {
                    auto* opened = showEditor(target);
                    if (opened != nullptr && library_id) {
                        opened->requestLibraryEntry(*library_id);
                    }
                },
                [this] { showSpectrogram(); },
                [this](std::uint8_t mask) {
                    composite_spectrogram_source_mask_ =
                        static_cast<std::uint8_t>(mask & 0x07U);
                    if (active_editor_ == "main"
                        && spectrogram_window_ != nullptr) {
                        spectrogram_window_->setAnalysisMode(
                            spectrogramAnalysisModeForEditor("main"));
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
                [this] { notifyLibraryContentsChanged(); },
                [this](const juce::String& closed) {
                    closeEditor(closed);
                },
                [this](const juce::String& active) {
                    activateEditor(active);
                });
        }
        if (!show) {
            // addToDesktop on a visible peer can synchronously activate the
            // window and deactivate the current editor before audition runs.
            if (created) {
                window->setVisible(false);
            }
            if (!window->isOnDesktop()) {
                window->addToDesktop();
            }
            return window.get();
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
        if (!window->isOnDesktop()) {
            window->addToDesktop();
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

    void showSpectrogram() {
        if (spectrogram_window_ == nullptr) {
            UiScale::forceGlobalForNonEditorUi();
            spectrogram_window_ =
                std::make_unique<mgstc::app::SpectrogramWindow>(
                    audio_service_->engine(),
                    [this](bool active) {
                        setSpectrogramPerformanceInputActive(active);
                    });
        }
        spectrogram_window_->setAnalysisMode(
            spectrogramAnalysisModeForEditor(active_editor_));
        spectrogram_window_->showWindow();
    }

    void setSpectrogramPerformanceInputActive(bool active) {
        spectrogram_performance_input_active_ = active;
        updateSpectrogramPerformanceInputTarget();
    }

    void updateSpectrogramPerformanceInputTarget() {
        const auto clear = [](const std::unique_ptr<MainWindow>& window) {
            if (window != nullptr) {
                window->setSpectrogramInputActive(false);
            }
        };
        clear(main_window_);
        clear(scc_window_);
        clear(opll_window_);
        clear(scc_envelope_window_);
        clear(opll_envelope_window_);
        spectrogram_input_editor_.clear();
        if (!spectrogram_performance_input_active_) {
            return;
        }
        auto& target = windowSlot(active_editor_);
        if (target != nullptr && target->isVisible()) {
            target->setSpectrogramInputActive(true);
            spectrogram_input_editor_ = active_editor_;
        }
    }

    [[nodiscard]] MainWindow* showEditor(
        const juce::String& requested_editor) {
        return ensureEditor(requested_editor, true);
    }

    void showLibraryManager(juce::Component* anchor) {
        if (library_manager_window_ != nullptr) {
            UiScale::forceGlobalForNonEditorUi();
            library_manager_window_->reloadFromExternalChange();
            library_manager_window_->setVisible(true);
            library_manager_window_->toFront(true);
            return;
        }
        // Bounded IPC wait (see ScopedLibraryIpcLock); a second MGSTC
        // instance holding the shared library must not stall forever.
        MGSTC_UI_ACTIVITY(
            "library: manager open (shared lock + load)");
        ScopedLibraryIpcLock lock(
            tag_management_lock_);
        std::string error;
        auto loaded = loadToneLibraries(&error);
        if (!lock.isLocked() || !loaded) {
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
        UiScale::forceGlobalForNonEditorUi();
        auto content = std::make_unique<LibraryManagerContent>(
            initial_kind,
            std::move(loaded->timbres),
            std::move(loaded->composites),
            *editor_session_,
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
            [this] { notifyLibraryContentsChanged(); },
            [this](
                LibraryManagerKind kind,
                std::uint64_t id,
                std::uint8_t note) {
                libraryManagerNoteOn(kind, id, note);
            },
            [this](LibraryManagerKind kind, std::uint8_t note) {
                libraryManagerNoteOff(kind, note);
            },
            [this] { libraryManagerAllNotesOff(); },
            [this](const mgstc::engine::ImportedToneCandidate& candidate) {
                previewImportedCandidate(candidate);
            },
            [this](const mgstc::engine::ImportedToneCandidate& candidate) {
                editImportedCandidate(candidate);
            },
            [this](
                const mgstc::engine::ImportedToneCandidate& candidate,
                std::uint8_t note) {
                libraryManagerImportedNoteOn(candidate, note);
            },
            [this](LibraryManagerKind kind) {
                const juce::String editor =
                    kind == LibraryManagerKind::Composite
                        ? "main"
                    : kind == LibraryManagerKind::Scc ? "scc" : "opll";
                auto& window = windowSlot(editor);
                if (window != nullptr) {
                    return window->polyphonic();
                }
                return true;
            });
        library_manager_window_ =
            std::make_unique<LibraryManagerWindow>(std::move(content));
        if (anchor != nullptr) {
            library_manager_window_->centreAroundComponent(
                anchor,
                library_manager_window_->getWidth(),
                library_manager_window_->getHeight());
        } else {
            library_manager_window_->centreWithSize(
                library_manager_window_->getWidth(),
                library_manager_window_->getHeight());
        }
        clampWindowToDisplayWorkArea(*library_manager_window_);
        library_manager_window_->setVisible(true);
        library_manager_window_->toFront(true);
        if (auto* hosted = library_manager_window_->getContentComponent()) {
            hosted->grabKeyboardFocus();
        }
    }

    void stopAllImportPreviews() {
        const auto stop = [](const std::unique_ptr<MainWindow>& window) {
            if (window != nullptr) {
                window->silenceImportedPreview();
            }
        };
        stop(main_window_);
        stop(scc_window_);
        stop(opll_window_);
    }

    void previewImportedCandidate(
        const mgstc::engine::ImportedToneCandidate& candidate) {
        const juce::String editor =
            candidate.default_register_as
                    == mgstc::engine::ImportRegisterAs::Composite
                ? "main"
            : candidate.default_register_as
                    == mgstc::engine::ImportRegisterAs::Scc
                ? "scc"
                : "opll";
        stopAllImportPreviews();
        if (auto* window = ensureEditor(editor, false, false)) {
            window->auditionImportedCandidate(candidate);
        }
    }

    [[nodiscard]] CompositeEditorComponent* compositeEditor() const {
        if (main_window_ == nullptr) {
            return nullptr;
        }
        return dynamic_cast<CompositeEditorComponent*>(
            main_window_->getContentComponent());
    }

    void wireCompositeOwnedAuditionCallbacks() {
        editor_link_.owned_audition = {
            [this] {
                if (auto* editor = compositeEditor()) {
                    editor->syncOwnedEditProgram();
                }
            },
            [this] {
                if (auto* editor = compositeEditor()) {
                    editor->auditionOwnedEditOneSecond();
                }
            },
            [this](std::uint8_t note) {
                if (auto* editor = compositeEditor()) {
                    editor->ownedEditNoteOn(note);
                }
            },
            [this](std::uint8_t note) {
                if (auto* editor = compositeEditor()) {
                    editor->ownedEditNoteOff(note);
                }
            },
            [this] {
                if (auto* editor = compositeEditor()) {
                    editor->stopImportedPreview();
                }
            },
        };
    }

    void editImportedCandidate(
        const mgstc::engine::ImportedToneCandidate& candidate) {
        const juce::String editor =
            candidate.default_register_as
                    == mgstc::engine::ImportRegisterAs::Composite
                ? "main"
            : candidate.default_register_as
                    == mgstc::engine::ImportRegisterAs::Scc
                ? "scc"
                : "opll";
        if (auto* window = ensureEditor(editor, false, false)) {
            window->silenceImportedPreview();
            window->loadImportedCandidate(candidate);
        }
        static_cast<void>(showEditor(editor));
    }

    void libraryManagerImportedNoteOn(
        const mgstc::engine::ImportedToneCandidate& candidate,
        std::uint8_t note) {
        const juce::String editor =
            candidate.default_register_as
                    == mgstc::engine::ImportRegisterAs::Composite
                ? "main"
            : candidate.default_register_as
                    == mgstc::engine::ImportRegisterAs::Scc
                ? "scc"
                : "opll";
        stopAllImportPreviews();
        if (auto* window = ensureEditor(editor, false, false)) {
            window->libraryManagerImportedNoteOn(candidate, note);
        }
    }

    void previewLibraryEntry(
        LibraryManagerKind kind,
        std::uint64_t id) {
        const juce::String editor =
            kind == LibraryManagerKind::Composite
                ? "main"
            : kind == LibraryManagerKind::Scc ? "scc" : "opll";
        stopAllImportPreviews();
        if (auto* window = ensureEditor(editor, false, false)) {
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
        stopAllImportPreviews();
        if (auto* window = ensureEditor(editor, false, false)) {
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

    void notifyLibraryContentsChanged() {
        if (notifying_library_change_) {
            return;
        }
        notifying_library_change_ = true;
        refreshAllEditorLibraries();
        if (library_manager_window_ != nullptr
            && library_manager_window_->isVisible()) {
            library_manager_window_->reloadFromExternalChange();
        }
        notifying_library_change_ = false;
    }

    void applyGlobalTagRewrite(
        std::string source,
        std::string replacement) {
        MGSTC_UI_ACTIVITY(
            "library: tag rewrite (shared lock + save)");
        ScopedLibraryIpcLock lock(
            tag_management_lock_);
        std::string error;
        auto loaded = loadToneLibraries(&error);
        if (!lock.isLocked() || !loaded) {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon,
                juce::String::fromUTF8("ライブラリ管理"),
                juce::String::fromUTF8(
                    "共有ライブラリを更新できませんでした"));
            return;
        }
        const auto original_timbres = loaded->timbres;
        const auto original_composites = loaded->composites;
        const auto now = currentUnixTime();
        const auto timbre_changes =
            loaded->timbres.rewriteTag(source, replacement, now);
        const auto composite_changes =
            loaded->composites.rewriteTag(source, replacement, now);
        if (timbre_changes == 0 && composite_changes == 0) {
            return;
        }

        const bool saved = persistToneLibraries(
            loaded->timbres, loaded->composites);
        if (!saved) {
            const bool rolled_back = persistToneLibraries(
                original_timbres, original_composites);
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon,
                juce::String::fromUTF8("ライブラリ管理"),
                rolled_back
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
            if (spectrogram_window_ != nullptr) {
                spectrogram_window_->setAnalysisMode(
                    spectrogramAnalysisModeForEditor(active_editor_));
            }
            if (auto& window = windowSlot(editor); window != nullptr) {
                window->syncProcessUiScale();
                window->refreshExternalState();
            }
        }
        pending_activate_editor_.reset();
        updateSpectrogramPerformanceInputTarget();
    }

    void timerCallback() override {
        if (offline_spectrogram_capture_) {
            // `--capture-ui --capture-target spectrogram` is a headless
            // documentation path: no device callback runs, so it advances
            // the same engine rendering path in bounded chunks until the
            // analyzer has received the one-second audition.
            std::array<float, 1'600> offline_pcm{};
            static_cast<void>(audio_service_->engine().render(offline_pcm));
            if (juce::Time::getMillisecondCounterHiRes() < snapshot_due_ms_) {
                return;
            }
            offline_spectrogram_capture_ = false;
        }
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
            result = captureLibraryManagerSnapshot(snapshot_file_, 0);
        } else if (snapshot_target_ == "library-tags") {
            result = captureLibraryManagerSnapshot(snapshot_file_, 1);
        } else if (snapshot_target_ == "library-import") {
            result = captureLibraryManagerSnapshot(snapshot_file_, 2);
        } else if (snapshot_target_ == "spectrogram" || snapshot_target_ == "spectrum") {
            auto* const content = spectrogram_window_ != nullptr
                ? spectrogram_window_->snapshotContent()
                : nullptr;
            result = content != nullptr
                ? writeComponentPngSnapshot(*content, snapshot_file_)
                : SnapshotResult::MissingContent;
        } else if (snapshot_window_ != nullptr) {
            result = snapshot_window_->writeContentSnapshot(
                snapshot_file_);
        }
        setApplicationReturnValue(static_cast<int>(result));
        quit();
    }

    [[nodiscard]] SnapshotResult captureLibraryManagerSnapshot(
        const juce::File& output_file,
        int tab_index) {
        ScopedLibraryIpcLock lock(
            tag_management_lock_);
        std::string error;
        auto loaded = loadToneLibraries(&error);
        if (!lock.isLocked() || !loaded
            || midi_service_ == nullptr
            || audio_service_ == nullptr
            || editor_session_ == nullptr) {
            return SnapshotResult::MissingContent;
        }
        LibraryManagerContent content(
            LibraryManagerKind::Scc,
            std::move(loaded->timbres),
            std::move(loaded->composites),
            *editor_session_,
            [](std::string, std::string) {},
            [](LibraryManagerKind, std::uint64_t) {},
            [](LibraryManagerKind, std::uint64_t) {},
            [] {},
            [](LibraryManagerKind, std::uint64_t, std::uint8_t) {},
            [](LibraryManagerKind, std::uint8_t) {},
            [] {});
        content.setCaptureTab(tab_index);
        return writeComponentPngSnapshot(content, output_file);
    }

    std::unique_ptr<MainWindow> main_window_;
    std::unique_ptr<MainWindow> scc_window_;
    std::unique_ptr<MainWindow> opll_window_;
    std::unique_ptr<MainWindow> scc_envelope_window_;
    std::unique_ptr<MainWindow> opll_envelope_window_;
    std::unique_ptr<mgstc::app::SpectrogramWindow> spectrogram_window_;
    std::unique_ptr<LibraryManagerWindow> library_manager_window_;
    bool notifying_library_change_{};
    MainWindow* snapshot_window_{};
    std::unique_ptr<SharedAudioService> audio_service_;
    std::unique_ptr<SharedMidiInputService> midi_service_;
    std::unique_ptr<StandaloneEditorContext> editor_session_;
    mgstc::app::EditorLink editor_link_;
    mgstc::app::UiHangWatchdog hang_watchdog_;
    juce::String primary_editor_{"main"};
    juce::String active_editor_{"main"};
    juce::String spectrogram_input_editor_;
    std::uint8_t composite_spectrogram_source_mask_{};
    juce::String snapshot_target_{"editor"};
    bool offline_spectrogram_capture_{};
    double snapshot_due_ms_{};
    bool routing_test_mode_{};
    bool routing_test_passed_{};
    bool spectrogram_performance_input_active_{};
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
