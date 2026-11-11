// SPDX-License-Identifier: AGPL-3.0-only

#include "mgstc/engine/composite_program_compiler.hpp"
#include "mgstc/engine/composite_timbre.hpp"
#include "mgstc/engine/engine_command.hpp"
#include "mgstc/engine/realtime_engine_host.hpp"
#include "realtime_engine_host_test_access.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <memory>
#include <thread>

namespace {

using mgstc::engine::test::HeldHostAccess;

void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

void loadDefaultProgram(
    mgstc::engine::RealtimeEngineHost& host,
    mgstc::engine::CompositePlaybackPlan& plan) {
    using namespace mgstc::engine;
    auto edit = host.beginProgramEdit();
    require(edit.valid(), "beginProgramEdit failed");
    require(
        compileCompositeProgram(
            *edit.engine,
            defaultCompositeTimbre(),
            {.polyphonic = true},
            &plan),
        "compileCompositeProgram failed");
    require(host.submitProgram(edit), "submitProgram failed");
    std::array<float, 2> drain{};
    host.drainPendingCommands(drain);
}

double peakOf(const std::array<float, 1600>& pcm) {
    double peak = 0.0;
    for (const auto sample : pcm) {
        peak = std::max(peak, static_cast<double>(std::fabs(sample)));
    }
    return peak;
}

void testRealtimeNoteOnRendersWithoutSpscNoteCommands() {
    using namespace mgstc::engine;
    RealtimeEngineHost host;
    CompositePlaybackPlan plan;
    loadDefaultProgram(host, plan);
    require(!plan.audible_layers.empty(), "default plan has no layers");
    require(
        host.realtimeNoteOn(plan.physicalTrack(plan.audible_layers.front(), 0), 60),
        "realtimeNoteOn failed");
    std::array<float, 1600> pcm{};
    double peak = 0.0;
    for (int frame = 0; frame < 60; ++frame) {
        const auto result = host.renderAudio(pcm);
        require(result.ok(), "renderAudio failed");
        peak = std::max(peak, peakOf(pcm));
    }
    require(peak > 0.001, "realtime note rendered silence");
}

void testRealtimeNoteOnDoesNotAffectPriorSamples() {
    using namespace mgstc::engine;
    RealtimeEngineHost host;
    CompositePlaybackPlan plan;
    loadDefaultProgram(host, plan);
    std::array<float, 256> prefix{};
    const auto before = host.renderAudio(prefix);
    require(before.ok(), "prefix render failed");
    double prefix_peak = 0.0;
    for (const auto sample : prefix) {
        prefix_peak = std::max(
            prefix_peak, static_cast<double>(std::fabs(sample)));
    }
    require(prefix_peak < 0.001, "gated program should be silent before note");
    require(
        host.realtimeNoteOn(plan.physicalTrack(plan.audible_layers.front(), 0), 60),
        "realtimeNoteOn failed");
    std::array<float, 1600> pcm{};
    double later_peak = 0.0;
    for (int frame = 0; frame < 60; ++frame) {
        const auto result = host.renderAudio(pcm);
        require(result.ok(), "post-note render failed");
        later_peak = std::max(later_peak, peakOf(pcm));
    }
    require(later_peak > 0.001, "note after prefix rendered silence");
}

void testTwoHostsDoNotShareNoteState() {
    using namespace mgstc::engine;
    RealtimeEngineHost a;
    RealtimeEngineHost b;
    CompositePlaybackPlan plan_a;
    CompositePlaybackPlan plan_b;
    loadDefaultProgram(a, plan_a);
    loadDefaultProgram(b, plan_b);
    require(
        a.realtimeNoteOn(plan_a.physicalTrack(plan_a.audible_layers.front(), 0), 60),
        "host A noteOn failed");
    std::array<float, 1600> pcm{};
    double peak_a = 0.0;
    double peak_b = 0.0;
    for (int frame = 0; frame < 60; ++frame) {
        require(a.renderAudio(pcm).ok(), "host A render");
        peak_a = std::max(peak_a, peakOf(pcm));
        require(b.renderAudio(pcm).ok(), "host B render");
        peak_b = std::max(peak_b, peakOf(pcm));
    }
    require(peak_a > 0.001, "host A should sound");
    require(peak_b < 0.001, "host B should stay silent");
}

void testCallerDrainDefersDuringOutputTransition() {
    using namespace mgstc::engine;
    RealtimeEngineHost host;
    CompositePlaybackPlan plan;
    loadDefaultProgram(host, plan);
    const auto track = plan.physicalTrack(plan.audible_layers.front(), 0);
    require(host.submit(EngineCommand::noteOn(track, 60)), "note command was not queued");
    std::mutex mutex;
    std::condition_variable wake;
    bool entered = false, release = false;
    std::thread control([&] {
        RealtimeEngineHostTestAccess::duringOutputTransition(host, [&] {
            std::unique_lock lock(mutex);
            entered = true;
            wake.notify_one();
            wake.wait_for(lock, std::chrono::seconds(5), [&] { return release; });
        });
    });
    bool started;
    {
        std::unique_lock lock(mutex);
        started = wake.wait_for(lock, std::chrono::seconds(5), [&] { return entered; });
    }
    const auto begin = std::chrono::steady_clock::now();
    std::array<float, 2> drain{};
    if (started) host.drainPendingCommands(drain, 250, true);
    const auto duration = std::chrono::steady_clock::now() - begin;
    const auto retained = host.pendingCommandCount();
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    wake.notify_one();
    control.join();
    require(started, "Output transition did not start");
    require(duration < std::chrono::seconds(1), "Caller drain waited for output connection");
    require(retained == 1, "Closed gate consumed or discarded the queued note");
    host.drainPendingCommands(drain, 250, true);
    require(host.pendingCommandCount() == 0, "Reopened gate did not drain the note");
    std::array<float, 1600> pcm{};
    double peak = 0.0;
    for (int frame = 0; frame < 60; ++frame) {
        require(host.renderAudio(pcm).ok(), "render after deferred drain failed");
        peak = std::max(peak, peakOf(pcm));
    }
    require(peak > 0.001, "Deferred note did not sound after connection completed");
}

void testControlTimeoutPreservesAdmittedAudioAndState() {
    using namespace mgstc::engine;
    auto host = std::make_unique<RealtimeEngineHost>();
    CompositePlaybackPlan plan;
    loadDefaultProgram(*host, plan);
    require(host->submit(EngineCommand::noteOn(0, 60)), "prepare retained note");
    const auto revision = host->outputRoutingRevision();
    const auto original_settings = host->mamidiSettings();
    const auto original_scope = host->opllScopeEnabled();
    auto changed = original_settings;
    changed.host = "127.0.0.2";
    changed.waveform_monitor = !changed.waveform_monitor;
    {
        HeldHostAccess audio(*host, HeldHostAccess::Kind::Audio);
        const auto start = std::chrono::steady_clock::now();
        require(!host->setSoundOutputKind(SoundOutputKind::MAmidiMemo), "unresponsive audio must reject switching");
        const auto elapsed = std::chrono::steady_clock::now() - start;
        require(elapsed >= std::chrono::milliseconds(400) && elapsed < std::chrono::seconds(2),
            "audio exclusion did not use its bounded wait");
        require(!host->setMAmidiSettings(changed), "unresponsive audio must reject settings");
        require(RealtimeEngineHostTestAccess::accessState(*host) == 1,
            "timeout erased an admitted audio reader or left the gate closed");
        require(!host->outputSwitchPending(), "timeout left its pending flag set");
        require(host->soundOutputKind() == SoundOutputKind::Emulator
            && host->desiredSoundOutputKind() == SoundOutputKind::Emulator
            && !host->ownsHardwareOutput(), "timeout changed output selection or ownership");
        require(host->mamidiSettings().host == original_settings.host
            && host->mamidiSettings().waveform_monitor == original_settings.waveform_monitor
            && host->opllScopeEnabled() == original_scope, "timeout changed protected settings");
        require(host->outputRoutingRevision() == revision && host->pendingCommandCount() == 1,
            "timeout performed routing or consumed queued work");
        require(host->soundOutputLastError().find("timed out") != std::string::npos,
            "timeout was not reported");
    }
    require(RealtimeEngineHostTestAccess::accessState(*host) == 0, "audio reader count did not recover");
    require(host->setMAmidiSettings(changed), "settings retry failed after audio returned");
    host->setOpllScopeEnabled(!original_scope);
    require(host->opllScopeEnabled() != original_scope, "scope request was not retained");
    require(host->setSoundOutputKind(SoundOutputKind::Emulator), "output retry failed after audio returned");
    require(host->soundOutputLastError().empty(), "successful retry did not clear timeout status");
}

void testControlMutexTimeoutDoesNotReopenAnotherTransition() {
    using namespace mgstc::engine;
    auto host = std::make_unique<RealtimeEngineHost>();
    const auto original = host->mamidiSettings();
    auto changed = original;
    changed.port = 39130;
    {
        HeldHostAccess control(*host, HeldHostAccess::Kind::Control);
        const auto start = std::chrono::steady_clock::now();
        require(!host->setMAmidiSettings(changed), "busy control mutex accepted settings");
        require(std::chrono::steady_clock::now() - start < std::chrono::seconds(2),
            "control mutex wait was not bounded");
        require(host->outputSwitchPending()
            && RealtimeEngineHostTestAccess::accessState(*host) == 0x80000000U,
            "failed contender reopened another control transition");
        require(host->mamidiSettings().port == original.port, "busy control changed settings");
    }
    require(!host->outputSwitchPending() && RealtimeEngineHostTestAccess::accessState(*host) == 0,
        "original control transition did not release its gate");
    require(host->setMAmidiSettings(changed), "control retry failed after mutex release");
}

void testDestructionStillDrainsAdmittedAudio() {
    using namespace mgstc::engine;
    auto host = std::make_unique<RealtimeEngineHost>();
    HeldHostAccess audio(*host, HeldHostAccess::Kind::Audio);
    std::atomic<bool> started{false}, finished{false};
    std::thread destroy([owned = std::move(host), &started, &finished]() mutable {
        started.store(true, std::memory_order_release);
        owned.reset();
        finished.store(true, std::memory_order_release);
    });
    while (!started.load(std::memory_order_acquire)) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(650));
    const bool waited = !finished.load(std::memory_order_acquire);
    audio.release();
    destroy.join();
    require(waited, "destruction incorrectly applied the interactive timeout");
    require(finished.load(std::memory_order_acquire), "destruction did not finish after audio returned");
}

void testScopeRequestsRemainNonblockingAndRejectPreviousCaptureLifetime() {
    using namespace mgstc::engine;
    auto host = std::make_unique<RealtimeEngineHost>();
    auto reference = std::make_unique<RealtimeEngineHost>();
    CompositePlaybackPlan plan, reference_plan;
    loadDefaultProgram(*host, plan);
    loadDefaultProgram(*reference, reference_plan);
    reference->setOpllScopeEnabled(false);
    require(host->realtimeNoteOn(plan.physicalTrack(plan.audible_layers.front(), 0), 60), "scope note failed");
    require(reference->realtimeNoteOn(reference_plan.physicalTrack(reference_plan.audible_layers.front(), 0), 60),
        "reference note failed");
    host->setSpectrogramCaptureEnabled(true);
    host->setOpllScopeEnabled(false);
    host->setOpllScopeEnabled(true);
    std::array<float, 800> pcm{}, reference_pcm{};
    bool heard_note = false;
    const auto render_pair = [&] {
        require(host->renderAudio(pcm).ok() && reference->renderAudio(reference_pcm).ok(), "scope test render failed");
        require(pcm == reference_pcm, "scope request changed audible PCM");
        heard_note = heard_note || std::any_of(reference_pcm.begin(), reference_pcm.end(),
            [](float sample) { return std::abs(sample) > 0.0001F; });
    };
    render_pair(); // 400 samples from the new capture lifetime.
    auto frame = std::make_unique<OpllScopeFrame>();
    require(!host->pollOpllScope(*frame), "partial scope window was published");
    {
        HeldHostAccess audio(*host, HeldHostAccess::Kind::Audio);
        const auto start = std::chrono::steady_clock::now();
        host->setOpllScopeEnabled(false);
        host->setOpllScopeEnabled(true);
        require(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(100),
            "scope request waited for the admitted audio callback");
        require(host->opllScopeEnabled(), "scope request was lost while audio was admitted");
        require(RealtimeEngineHostTestAccess::accessState(*host) == 1, "scope request modified the audio gate");
    }
    render_pair();
    require(!host->pollOpllScope(*frame) && !host->pollSpectrogramScope(*frame),
        "close/reopen combined samples from two capture lifetimes");
    render_pair();
    require(host->pollOpllScope(*frame), "new scope lifetime never published a frame");
    auto previous = std::make_unique<OpllScopeFrame>(*frame);
    require(host->pollSpectrogramScope(*frame), "spectrogram capture lost its scope source");
    require(frame->capture_generation == previous->capture_generation, "scope consumers received inconsistent lifetimes");
    host->setOpllScopeEnabled(false);
    host->setOpllScopeEnabled(true);
    // Simulate an older callback completing its bounded publication after the
    // UI's close/reopen. Every consumer must discard that older generation.
    require(RealtimeEngineHostTestAccess::publishScopeFrame(*host, *previous), "inject late scope frame");
    require(!host->pollOpllScope(*frame), "scope poll accepted a late old frame");
    require(RealtimeEngineHostTestAccess::publishScopeFrame(*host, *previous), "inject late latest frame");
    require(!host->pollLatestOpllScope(*frame), "latest scope poll accepted a late old frame");
    require(RealtimeEngineHostTestAccess::publishScopeFrame(*host, *previous, true), "inject late spectrogram frame");
    require(!host->pollSpectrogramScope(*frame), "spectrogram poll accepted a late old frame");
    render_pair();
    render_pair();
    require(host->pollOpllScope(*frame) && frame->capture_generation != previous->capture_generation,
        "scope capture did not resume with the new lifetime");
    require(frame->sample_end == previous->sample_end + OpllScopeFrame::kSampleCount,
        "scope lifecycle changed the engine sample clock");
    require(host->pollSpectrogramScope(*frame), "new generation permanently suppressed spectrogram capture");
    require(heard_note, "scope PCM equivalence compared only silent buffers");
}

}  // namespace

int main() {
    try {
        testRealtimeNoteOnRendersWithoutSpscNoteCommands();
        testRealtimeNoteOnDoesNotAffectPriorSamples();
        testTwoHostsDoNotShareNoteState();
        testCallerDrainDefersDuringOutputTransition();
        testControlTimeoutPreservesAdmittedAudioAndState();
        testControlMutexTimeoutDoesNotReopenAnotherTransition();
        testDestructionStillDrainsAdmittedAudio();
        testScopeRequestsRemainNonblockingAndRejectPreviousCaptureLifetime();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}
