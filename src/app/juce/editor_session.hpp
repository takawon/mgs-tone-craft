// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>

#include "mgstc/engine/composite_timbre.hpp"
#include "mgstc/engine/opll_patch.hpp"
#include "mgstc/engine/scc_waveform.hpp"
#include "mgstc/engine/timbre_library.hpp"

namespace mgstc::engine {
struct OpllScopeFrame;
}

namespace mgstc::app {

// What this host may run. The shared editor reads these flags and must not
// start a backend whose flag is false. Standalone keeps the current set on.
// Plugin preset turns off device, hardware, library, and analysis backends.
struct EditorCapabilities {
    bool spectrum_analyzer{true};
    bool spectrogram{true};
    bool waveform_scope{true};
    bool audio_device_settings{true};
    bool midi_device_settings{true};
    bool hardware_output{true};
    bool mamidi{true};
    bool vsif{true};
    bool tone_library{true};
    bool on_screen_keyboard{true};
};

[[nodiscard]] inline EditorCapabilities standaloneEditorCapabilities() noexcept {
    return {};
}

[[nodiscard]] inline EditorCapabilities pluginEditorCapabilities() noexcept {
    EditorCapabilities caps;
    caps.spectrum_analyzer = false;
    caps.spectrogram = false;
    caps.waveform_scope = false;
    caps.audio_device_settings = false;
    caps.midi_device_settings = false;
    caps.hardware_output = false;
    caps.mamidi = false;
    caps.vsif = false;
    caps.tone_library = false;
    return caps;
}

// Standalone settings.ini and device lists. Plugin capabilities leave this
// false, so the shared view must not read or write that file.
[[nodiscard]] inline bool standaloneHostPreferences(
    const EditorCapabilities& caps) noexcept {
    return caps.audio_device_settings || caps.midi_device_settings;
}

enum class PcAudioBackend : std::uint8_t {
    Wasapi,
    Asio,
};

// Plain copy of the MAmidi route fields. Defaults match MAmidiMemoSoundOutput.
// The session header does not include RealtimeEngineHost.
struct HardwareOutputSettings {
    std::string host{"localhost"};
    std::uint16_t port{30000};
    std::uint8_t unit_no{0};
    bool scc_plus{false};
    bool waveform_monitor{false};
};

struct EditorSnapshot {
    struct MidiDevice {
        juce::String name;
        juce::String identifier;
    };

    EditorCapabilities capabilities{};
    bool audio_running{false};
    int master_volume_percent{100};
    std::uint64_t master_volume_revision{0};
    int opll_keyoff_silence_seconds{0};
    PcAudioBackend pc_audio_backend{PcAudioBackend::Wasapi};
    std::string asio_driver_name;
    bool asio_control_panel_available{false};
    std::string pc_audio_status;
    bool hardware_output{false};
    HardwareOutputSettings hardware_settings{};
    std::vector<MidiDevice> midi_devices;
    int midi_selected_combo_id{1};
    juce::String midi_status;
    std::uint64_t midi_revision{0};
    bool shared_program_active{false};
    mgstc::engine::SccWaveform shared_scc_waveform{};
    std::uint64_t shared_scc_revision{0};
    mgstc::engine::OpllPatchParameters shared_opll_patch{};
    std::uint64_t shared_opll_revision{0};
};

struct CompositeAuditionRequest {
    mgstc::engine::CompositeTimbre* timbre{};
    bool polyphonic{true};
    const mgstc::engine::TimbreLibrary* library{};
};

struct AuditionResult {
    bool ok{false};
    bool update_allocator{false};
    std::uint8_t voice_capacity{1};
};

struct SharedAuditionRequest {
    const mgstc::engine::SccWaveform* scc{};
    const mgstc::engine::OpllPatchParameters* opll{};
    bool retrigger{false};
    std::uint8_t track{};
    std::uint8_t note{60};
    bool commit_scc{false};
    bool commit_opll{false};
};

class OutputBoundary {
public:
    virtual ~OutputBoundary() = default;

    virtual void setMasterVolumePercent(int percent) = 0;
    [[nodiscard]] virtual std::vector<std::string> availableAsioDrivers() = 0;
    [[nodiscard]] virtual bool applyPcAudio(
        PcAudioBackend backend,
        std::string driver_name) = 0;
    [[nodiscard]] virtual bool showAsioControlPanel() = 0;
    [[nodiscard]] virtual std::string hardwareStatus() = 0;
    [[nodiscard]] virtual bool applySoundRoute(
        bool hardware,
        HardwareOutputSettings settings,
        int opll_keyoff_silence_seconds) = 0;
    [[nodiscard]] virtual bool reconnectHardware() = 0;
};

class MidiDrainListener {
public:
    virtual ~MidiDrainListener() = default;
    virtual void midiMessagesPending() = 0;
};

class MidiInputBoundary {
public:
    virtual ~MidiInputBoundary() = default;

    virtual void addDrainListener(MidiDrainListener* listener) = 0;
    virtual void removeDrainListener(MidiDrainListener* listener) = 0;
    virtual void refreshDevices(bool select_first) = 0;
    virtual void selectDevice(int combo_id, bool persist) = 0;
    [[nodiscard]] virtual std::vector<juce::MidiMessage>
    takePendingMessages() = 0;
    virtual void clearPendingMessages() = 0;
};

class AuditionBoundary {
public:
    virtual ~AuditionBoundary() = default;

    [[nodiscard]] virtual AuditionResult submitComposite(
        const CompositeAuditionRequest& request) = 0;
    [[nodiscard]] virtual bool submitShared(
        const SharedAuditionRequest& request) = 0;
    virtual void publishSharedScc(const mgstc::engine::SccWaveform& waveform) = 0;
    virtual void publishSharedOpll(
        const mgstc::engine::OpllPatchParameters& patch) = 0;
    [[nodiscard]] virtual bool noteOn(
        std::uint8_t track,
        std::uint8_t note) = 0;
    [[nodiscard]] virtual bool noteOff(std::uint8_t track) = 0;
    virtual void armOpllKeyOffSilence(std::uint8_t track) = 0;
    virtual void cancelOpllKeyOffSilence(std::uint8_t track) = 0;
    virtual void clearOpllKeyOffSilence() = 0;
    virtual void flushPending() = 0;
};

// Thin composition root. No engine() accessor. Scope polls consume frames and
// are not audition. Call them only when capabilities.waveform_scope is set.
class EditorSession {
public:
    virtual ~EditorSession() = default;

    // Read-only copy. Must not consume queues or change host state.
    // ASIO driver enumeration and hardware status text are queries on Output.
    [[nodiscard]] virtual EditorSnapshot snapshot() = 0;
    [[nodiscard]] virtual OutputBoundary& output() noexcept = 0;
    [[nodiscard]] virtual MidiInputBoundary& midi() noexcept = 0;
    [[nodiscard]] virtual AuditionBoundary& audition() noexcept = 0;
    [[nodiscard]] virtual bool pollOpllScope(
        mgstc::engine::OpllScopeFrame& frame) = 0;
    [[nodiscard]] virtual bool pollLatestOpllScope(
        mgstc::engine::OpllScopeFrame& frame) = 0;
};

// Per-instance editor collaboration. Not engine or plugin state authority.
// Reflecting a program into the host goes through EditorSession only.
struct OwnedAuditionHooks {
    std::function<void()> sync_program;
    std::function<void()> one_second;
    std::function<void(std::uint8_t)> note_on;
    std::function<void(std::uint8_t)> note_off;
    std::function<void()> stop_preview;

    [[nodiscard]] bool ready() const noexcept {
        return static_cast<bool>(sync_program)
            && static_cast<bool>(one_second)
            && static_cast<bool>(note_on)
            && static_cast<bool>(note_off)
            && static_cast<bool>(stop_preview);
    }
};

struct EditorLink {
    std::optional<mgstc::engine::SavedTimbreReference> owned_target;
    std::optional<mgstc::engine::SavedTimbreReference> owned_published;
    std::uint64_t owned_revision{1};
    OwnedAuditionHooks owned_audition{};

    void publishOwned(mgstc::engine::SavedTimbreReference snapshot) {
        owned_published = std::move(snapshot);
        ++owned_revision;
    }
};

}  // namespace mgstc::app
