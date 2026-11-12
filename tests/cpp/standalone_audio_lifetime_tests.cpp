// SPDX-License-Identifier: AGPL-3.0-only
#include "mgstc/audio/detail/wasapi_audio_sink_test_access.hpp"

#if !defined(MGSTC_WASAPI_TESTING) || !defined(MGSTC_STANDALONE_AUDIO_TESTING)
#error Standalone audio lifetime integration tests require test-only hooks
#endif

// Exercise the real SharedAudioService owner/transition code in an isolated
// console test binary. Its CMake target supplies the same UI include/definitions
// and substitutes only the WASAPI device API.
#include "../../src/app/juce/main.cpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using namespace std::chrono_literals;
using mgstc::audio::detail::WasapiAudioSinkTestAccess;
using mgstc::audio::detail::WasapiTestHooks;
using mgstc::audio::detail::WasapiTestSnapshot;
using mgstc::audio::detail::WasapiTestStage;

struct SharedAudioServiceTestAccess {
    static void installHooks(SharedAudioService& service,
        WasapiTestHooks* hooks) noexcept {
        WasapiAudioSinkTestAccess::setHooks(service.wasapi_audio_, hooks);
    }
    static void advance(SharedAudioService& service) {
        service.advancePcAudioTransition();
    }
    static WasapiTestSnapshot snapshot(
        const SharedAudioService& service) noexcept {
        return WasapiAudioSinkTestAccess::snapshot(service.wasapi_audio_);
    }
    static std::uintptr_t hostAddress(
        const SharedAudioService& service) noexcept {
        return reinterpret_cast<std::uintptr_t>(&service.engine_);
    }
    static bool wasapiActive(const SharedAudioService& service) noexcept {
        return service.active_audio_ == &service.wasapi_audio_;
    }
    static bool asioActive(const SharedAudioService& service) noexcept {
        return service.active_audio_ == &service.asio_audio_;
    }
    static bool busy(const SharedAudioService& service) noexcept {
        return service.device_recovery_busy_;
    }
    static bool transitionPending(const SharedAudioService& service) noexcept {
        return service.pc_audio_stop_requested_;
    }
    static bool wasapiStopPending(const SharedAudioService& service) noexcept {
        return service.wasapi_audio_.stopPending();
    }
    static const std::string& note(const SharedAudioService& service) noexcept {
        return service.pc_audio_note_;
    }
};

struct ApplicationTestAccess {
    static void enableStandaloneConstruction() {
        previous_factory() = Application::createInstance;
        Application::createInstance = &createForJuce;
    }
    static void restoreFactory() noexcept {
        Application::createInstance = previous_factory();
    }
    static std::unique_ptr<Application> create() {
        auto app = std::unique_ptr<Application>(new Application());
        juce::LookAndFeel::setDefaultLookAndFeel(&app->look_and_feel_);
        return app;
    }
    static void createService(Application& app, WasapiTestHooks* hooks) {
        app.audio_service_ = std::make_unique<SharedAudioService>(true);
        SharedAudioServiceTestAccess::installHooks(*app.audio_service_, hooks);
    }
    static SharedAudioService& service(Application& app) {
        return *app.audio_service_;
    }
    static bool hasService(const Application& app) noexcept {
        return app.audio_service_ != nullptr;
    }
    static bool quitPending(const Application& app) noexcept {
        return app.audio_shutdown_pending_;
    }
    static juce::AlertWindow* shutdownNotice(Application& app) noexcept {
        return app.audio_shutdown_notice_.get();
    }
    static void showLauncher(Application& app) {
        app.midi_service_ = std::make_unique<SharedMidiInputService>(false);
        app.editor_session_ = std::make_unique<StandaloneEditorContext>(
            *app.audio_service_, *app.midi_service_);
        app.showLauncher();
    }
    static bool launcherVisible(const Application& app) noexcept {
        return app.launcher_window_ != nullptr && app.launcher_window_->isVisible();
    }
    static void closeLauncher(Application& app) {
        app.launcher_window_->closeButtonPressed();
    }
    static void requestQuit(Application& app) { app.requestApplicationQuit(); }
    static void timer(Application& app) { app.timerCallback(); }
    static void shutdown(Application& app) { app.shutdown(); }
private:
    static juce::JUCEApplicationBase* createForJuce() {
        return new Application();
    }
    static juce::JUCEApplicationBase::CreateInstanceFunction& previous_factory() {
        static auto value = Application::createInstance;
        return value;
    }
};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class StageBarrier final {
public:
    void arm(WasapiTestStage stage) {
        std::lock_guard lock(mutex_);
        stage_ = stage;
        armed_ = true;
        entered_ = false;
    }

    void hit(WasapiTestStage stage) noexcept {
        std::unique_lock lock(mutex_);
        if (!armed_ || stage_ != stage) return;
        armed_ = false;
        entered_ = true;
        const auto wait_epoch = release_epoch_;
        changed_.notify_all();
        changed_.wait_for(lock, 8s, [this, wait_epoch] {
            return release_epoch_ != wait_epoch;
        });
    }

    bool waitEntered(std::chrono::milliseconds timeout = 3s) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [this] { return entered_; });
    }

    void release() noexcept {
        std::lock_guard lock(mutex_);
        ++release_epoch_;
        changed_.notify_all();
    }

    void releaseAndArm(WasapiTestStage next) {
        std::lock_guard lock(mutex_);
        ++release_epoch_;
        stage_ = next;
        armed_ = true;
        entered_ = false;
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    WasapiTestStage stage_{};
    bool armed_{};
    bool entered_{};
    std::uint64_t release_epoch_{};
};

void stageCallback(void* context, WasapiTestStage stage) noexcept {
    static_cast<StageBarrier*>(context)->hit(stage);
}

struct HookGuard final {
    StageBarrier& barrier;
    ~HookGuard() { barrier.release(); }
};

bool pumpUntil(SharedAudioService& service,
    const auto& predicate, std::chrono::milliseconds timeout = 4s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        SharedAudioServiceTestAccess::advance(service);
        if (auto* manager = juce::MessageManager::getInstanceWithoutCreating())
            static_cast<void>(manager->runDispatchLoopUntil(1));
        std::this_thread::sleep_for(1ms);
    }
    SharedAudioServiceTestAccess::advance(service);
    return predicate();
}

void testPendingSwitchRetargetAndSafeShutdown() {
    require(mgstcUseIsolatedApplicationDataDirectory(),
        "Could not create isolated application data for integration test");
    juce::ScopedJuceInitialiser_GUI gui;
    StageBarrier barrier;
    WasapiTestHooks hooks;
    hooks.context = &barrier;
    hooks.at = stageCallback;
    auto service_owner = std::make_unique<SharedAudioService>(true);
    auto& service = *service_owner;
    SharedAudioServiceTestAccess::installHooks(service, &hooks);
    HookGuard hook_cleanup{barrier};

    const auto host_address = SharedAudioServiceTestAccess::hostAddress(service);
    require(!service.applyPcAudioSettings(PcAudioBackend::Wasapi, {}),
        "Asynchronous WASAPI start was reported as synchronous completion");
    require(pumpUntil(service, [&] {
        return SharedAudioServiceTestAccess::wasapiActive(service);
    }), "Initial fake WASAPI output did not become active");
    const auto first = SharedAudioServiceTestAccess::snapshot(service);
    require(first.generation != 0 && first.starts == 1,
        "Initial service output did not create one WASAPI generation");

    barrier.arm(WasapiTestStage::AfterExitSignal);
    require(!service.applyPcAudioSettings(PcAudioBackend::Asio, "mock-driver"),
        "An ASIO switch completed while the WASAPI generation was still alive");
    require(barrier.waitEntered(),
        "WASAPI worker did not reach the post-exit-signal barrier");

    auto pending = SharedAudioServiceTestAccess::snapshot(service);
    require(pending.generation == first.generation && !pending.joined,
        "Service replaced a still-running WASAPI generation");
    require(SharedAudioServiceTestAccess::transitionPending(service),
        "Service did not retain the transition while reaping");
    require(!SharedAudioServiceTestAccess::asioActive(service),
        "Service activated ASIO before the old WASAPI thread joined");

    // Latest user intent replaces a pending ASIO request. The old worker is
    // still parked after its advisory event: no new stream may start yet.
    require(!service.applyPcAudioSettings(PcAudioBackend::Wasapi, {}),
        "A replacement request was mistaken for a completed switch");
    pending = SharedAudioServiceTestAccess::snapshot(service);
    require(pending.generation == first.generation && pending.starts == 1
            && !pending.joined,
        "A new WASAPI generation started before the retired worker was joined");
    require(SharedAudioServiceTestAccess::hostAddress(service) == host_address,
        "SharedAudioService changed Host ownership during a pending switch");

    barrier.release();
    require(pumpUntil(service, [&] {
        const auto state = SharedAudioServiceTestAccess::snapshot(service);
        return SharedAudioServiceTestAccess::wasapiActive(service)
            && !SharedAudioServiceTestAccess::busy(service)
            && state.generation != first.generation
            && state.starts == 1;
    }), "Retargeted WASAPI request did not start after true old-thread exit");
    const auto second = SharedAudioServiceTestAccess::snapshot(service);
    require(second.generation != first.generation && second.host == host_address,
        "Fresh generation did not retain the same immutable service Host");

    service.requestAudioShutdown();
    require(!service.audioShutdownReady(),
        "Shutdown was declared ready while a WASAPI worker remained active");
    require(pumpUntil(service, [&] { return service.audioShutdownReady(); }),
        "Service could not finish shutdown after normal worker exit");
    const auto destroy_begin = std::chrono::steady_clock::now();
    service_owner.reset();
    std::cout << "MOCK service_destruction_after_join_ms="
        << std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - destroy_begin).count() << '\n';
}

void testCancelShutdownDoesNotResumeClosedGeneration() {
    require(mgstcUseIsolatedApplicationDataDirectory(),
        "Could not create isolated application data for shutdown test");
    juce::ScopedJuceInitialiser_GUI gui;
    StageBarrier barrier;
    WasapiTestHooks hooks;
    hooks.context = &barrier;
    hooks.at = stageCallback;
    SharedAudioService service(true);
    SharedAudioServiceTestAccess::installHooks(service, &hooks);
    HookGuard hook_cleanup{barrier};

    const auto host_address = SharedAudioServiceTestAccess::hostAddress(service);
    static_cast<void>(service.applyPcAudioSettings(PcAudioBackend::Wasapi, {}));
    require(pumpUntil(service, [&] {
        return SharedAudioServiceTestAccess::wasapiActive(service);
    }), "Initial fake WASAPI output did not become active");
    barrier.arm(WasapiTestStage::AfterExitSignal);
    service.requestAudioShutdown();
    require(barrier.waitEntered(),
        "Shutdown did not wait for the retired worker's true exit");
    require(!service.audioShutdownReady(),
        "Shutdown completed before true worker join");
    require(SharedAudioServiceTestAccess::hostAddress(service) == host_address,
        "Host ownership was released while shutdown remained pending");

    service.cancelAudioShutdown();
    require(!service.audioShutdownReady(),
        "Canceling the close request falsely completed worker recovery");
    barrier.release();
    require(pumpUntil(service, [&] {
        return !SharedAudioServiceTestAccess::busy(service)
            && !SharedAudioServiceTestAccess::transitionPending(service);
    }), "Canceled shutdown did not safely reap the closed WASAPI generation");
    const auto state = SharedAudioServiceTestAccess::snapshot(service);
    require(state.joined && state.starts == 1,
        "Canceling shutdown restarted a closed generation");
    require(!SharedAudioServiceTestAccess::wasapiActive(service)
            && !SharedAudioServiceTestAccess::asioActive(service),
        "Canceled shutdown reactivated an output without a new user request");
    require(SharedAudioServiceTestAccess::hostAddress(service) == host_address,
        "Canceled shutdown changed the service Host address");
}

void testSettingsFollowFallbackAndPreserveUnappliedSelection() {
    require(mgstcUseIsolatedApplicationDataDirectory(),
        "Could not isolate settings integration test data");
    juce::ScopedJuceInitialiser_GUI gui;
    StageBarrier barrier;
    WasapiTestHooks hooks;
    hooks.context = &barrier;
    hooks.at = stageCallback;
    SharedAudioService service(true);
    SharedAudioServiceTestAccess::installHooks(service, &hooks);
    HookGuard hook_cleanup{barrier};
    SharedMidiInputService midi(false);
    StandaloneEditorContext session(service, midi);
    juce::Component owner;
    static_cast<void>(service.applyPcAudioSettings(PcAudioBackend::Wasapi, {}));
    require(pumpUntil(service, [&] {
        return SharedAudioServiceTestAccess::wasapiActive(service);
    }), "Settings test could not start fake WASAPI output");
    require(mgstc::app::showAppSettings(session, owner, 2)
            == mgstc::app::SnapshotResult::Success,
        "Could not open the real Output settings dialog");
    std::unique_ptr<juce::Component> dialog(
        juce::Component::getCurrentlyModalComponent());
    require(dialog != nullptr, "Settings dialog did not enter modal state");
    juce::ComboBox* backend = nullptr;
    const auto find_backend = [&](auto&& self, juce::Component& component) -> void {
        if (auto* combo = dynamic_cast<juce::ComboBox*>(&component);
            combo != nullptr && combo->getNumItems() == 2
            && combo->getItemText(1) == "ASIO") backend = combo;
        for (auto* child : component.getChildren()) self(self, *child);
    };
    find_backend(find_backend, *dialog);
    require(backend != nullptr, "Output backend selector was not found");
    juce::ComboBox* driver = nullptr;
    juce::TextButton* apply = nullptr;
    for (auto* child : backend->getParentComponent()->getChildren()) {
        if (auto* combo = dynamic_cast<juce::ComboBox*>(child);
            combo != nullptr && combo != backend) driver = combo;
        if (auto* button = dynamic_cast<juce::TextButton*>(child);
            button != nullptr && button->getButtonText() == juce::String::fromUTF8("適用"))
            apply = button;
    }
    require(driver != nullptr && apply != nullptr,
        "Output driver selector or Apply button was not found");
    // Select a deliberately absent driver: no real ASIO stream is opened.
    driver->clear(juce::dontSendNotification);
    driver->addItem("MGSTC absent integration-test ASIO driver", 1);
    driver->setSelectedId(1, juce::sendNotificationSync);
    const auto apply_asio_while_exit_pending = [&] {
        barrier.arm(WasapiTestStage::AfterExitSignal);
        backend->setSelectedId(2, juce::sendNotificationSync);
        apply->onClick();
        require(barrier.waitEntered(),
            "Applied ASIO request did not wait for the old WASAPI generation");
        require(backend->getSelectedId() == 2
                && SharedAudioServiceTestAccess::transitionPending(service),
            "Applied backend selection did not remain visible while pending");
    };
    apply_asio_while_exit_pending();
    barrier.release();
    require(pumpUntil(service, [&] {
        return SharedAudioServiceTestAccess::wasapiActive(service)
            && backend->getSelectedId() == 1;
    }), "Completed ASIO failure did not update the applied selection to WASAPI");

    // User ComboBox values change before their asynchronous onChange arrives.
    // Force the due settings timer to run in that gap, without pumping messages.
    std::this_thread::sleep_for(350ms);
    backend->setSelectedId(2, juce::sendNotificationAsync);
    juce::Timer::callPendingTimersSynchronously();
    require(backend->getSelectedId() == 2,
        "Status timer overwrote a user selection before its asynchronous notification");

    apply_asio_while_exit_pending();
    // An edit after Apply is new user intent, not a stale applied selection.
    backend->setSelectedId(1, juce::sendNotificationSync);
    backend->setSelectedId(2, juce::sendNotificationSync);
    barrier.release();
    require(pumpUntil(service, [&] {
        return SharedAudioServiceTestAccess::wasapiActive(service)
            && service.pcAudioBackend() == PcAudioBackend::Wasapi;
    }), "Second failed ASIO request did not fall back to WASAPI");
    static_cast<void>(juce::MessageManager::getInstance()->runDispatchLoopUntil(300));
    require(backend->getSelectedId() == 2,
        "Asynchronous status refresh overwrote an unapplied backend edit");
    dialog.reset();
    service.requestAudioShutdown();
    require(pumpUntil(service, [&] { return service.audioShutdownReady(); }),
        "Settings test did not join its final WASAPI generation");
}

void testApplicationQuitRemainsPendingThenShutsDownSafely() {
    require(mgstcUseIsolatedApplicationDataDirectory(),
        "Could not create isolated application data for app shutdown test");
    juce::ScopedJuceInitialiser_GUI gui;
    struct FactoryReset final {
        ~FactoryReset() { ApplicationTestAccess::restoreFactory(); }
    } reset_factory;
    StageBarrier barrier;
    WasapiTestHooks hooks;
    hooks.context = &barrier;
    hooks.at = stageCallback;
    ApplicationTestAccess::enableStandaloneConstruction();
    auto app = ApplicationTestAccess::create();
    ApplicationTestAccess::createService(*app, &hooks);
    HookGuard hook_cleanup{barrier};
    auto& service = ApplicationTestAccess::service(*app);

    const auto host_address = SharedAudioServiceTestAccess::hostAddress(service);
    // Hold the first in-flight engine borrow while start is running on the
    // service's device worker. Closing the app must publish cancellation into
    // that generation, keeping the Host alive until the borrow releases.
    barrier.arm(WasapiTestStage::AfterInitialBorrow);
    static_cast<void>(service.applyPcAudioSettings(PcAudioBackend::Wasapi, {}));
    require(pumpUntil(service, [&] {
        return barrier.waitEntered(1ms)
            && SharedAudioServiceTestAccess::busy(service);
    }),
        "Initial engine borrow did not reach the shutdown barrier");
    ApplicationTestAccess::showLauncher(*app);
    const auto initial_quit_request = std::chrono::steady_clock::now();
    ApplicationTestAccess::closeLauncher(*app);
    require(pumpUntil(service, [&] { return ApplicationTestAccess::quitPending(*app); }),
        "Closing the launcher bypassed the pending application quit route");
    const auto pending_reply = std::chrono::steady_clock::now();
    require(ApplicationTestAccess::quitPending(*app)
            && ApplicationTestAccess::hasService(*app)
            && !service.audioShutdownReady(),
        "Application treated an unjoined worker as a completed quit");
    require(SharedAudioServiceTestAccess::hostAddress(service) == host_address,
        "Application released its Host while the quit was pending");
    require(ApplicationTestAccess::launcherVisible(*app),
        "Closing the launcher removed the usable window before shutdown was safe");
    require(ApplicationTestAccess::shutdownNotice(*app) == nullptr,
        "Pending-quit notice appeared before its one-second delay");
    ApplicationTestAccess::timer(*app);
    require(ApplicationTestAccess::shutdownNotice(*app) == nullptr,
        "Early timer dispatch displayed the pending-quit notice");
    require(pumpUntil(service, [&] {
        return ApplicationTestAccess::shutdownNotice(*app) != nullptr;
    }, 2s), "Application did not display its delayed pending-quit notice");
    require(std::chrono::steady_clock::now() - initial_quit_request >= 1s,
        "Pending-quit notice was shown less than one second after closing");
    {
        auto* notice = ApplicationTestAccess::shutdownNotice(*app);
        const auto image = notice->createComponentSnapshot(notice->getLocalBounds());
        auto output = juce::File::getCurrentWorkingDirectory()
            .getChildFile("wasapi-shutdown-pending.png").createOutputStream();
        juce::PNGImageFormat png;
        require(image.isValid() && output != nullptr && png.writeImageToStream(image, *output),
            "Could not capture the actual pending-quit component");
    }

    // Cancel through the real notice callback. The in-flight start must still
    // observe the closed generation when the borrow returns; it cannot Start.
    ApplicationTestAccess::shutdownNotice(*app)->triggerButtonClick(
        juce::String::fromUTF8("終了を取り消す"));
    auto* manager = juce::MessageManager::getInstanceWithoutCreating();
    require(manager != nullptr, "JUCE message manager disappeared during quit");
    const auto cancel_deadline = std::chrono::steady_clock::now() + 1s;
    while (ApplicationTestAccess::quitPending(*app)
        && std::chrono::steady_clock::now() < cancel_deadline)
        static_cast<void>(manager->runDispatchLoopUntil(1));
    require(!ApplicationTestAccess::quitPending(*app)
            && ApplicationTestAccess::launcherVisible(*app),
        "Cancel did not return control to the retained launcher");
    const auto* cancelled_notice = ApplicationTestAccess::shutdownNotice(*app);
    require(cancelled_notice == nullptr || !cancelled_notice->isVisible(),
        "Cancel left the stale shutdown-pending notice visible");
    require(SharedAudioServiceTestAccess::hostAddress(service) == host_address,
        "Cancel changed Host ownership");

    barrier.release();
    require(pumpUntil(service, [&] {
        return !SharedAudioServiceTestAccess::busy(service)
            && !SharedAudioServiceTestAccess::transitionPending(service);
    }), "Canceled application quit did not reap its closed generation");
    auto state = SharedAudioServiceTestAccess::snapshot(service);
    require(state.joined && state.starts == 0
            && !SharedAudioServiceTestAccess::wasapiActive(service),
        "Canceled in-flight start reached Start or resumed an old generation");
    require(SharedAudioServiceTestAccess::hostAddress(service) == host_address,
        "Host was destroyed before the in-flight borrow released");

    // A later retry starts a new generation. Closing it waits for true thread
    // exit, and the Application timer commits quit exactly once.
    barrier.arm(WasapiTestStage::BeforeLoopWait);
    static_cast<void>(service.applyPcAudioSettings(PcAudioBackend::Wasapi, {}));
    require(pumpUntil(service, [&] {
        return SharedAudioServiceTestAccess::wasapiActive(service);
    }), "Application retry could not start a fresh output generation");
    require(pumpUntil(service, [&] { return barrier.waitEntered(1ms); }),
        "Retried worker did not enter its audio loop");
    barrier.releaseAndArm(WasapiTestStage::AfterExitSignal);
    const auto final_quit_request = std::chrono::steady_clock::now();
    ApplicationTestAccess::requestQuit(*app);
    require(pumpUntil(service, [&] { return barrier.waitEntered(1ms); }),
        "Retried worker did not remain pending at true exit");
    require(ApplicationTestAccess::quitPending(*app)
            && ApplicationTestAccess::hasService(*app),
        "Retry shutdown released application resources before worker exit");
    require(ApplicationTestAccess::shutdownNotice(*app) == nullptr,
        "Retry reused the canceled notice instead of waiting a new second");
    barrier.release();
    require(pumpUntil(service, [&] { return service.audioShutdownReady(); }),
        "Retried generation did not become safe to close");
    const auto safe_to_quit = std::chrono::steady_clock::now();
    // pumpUntil may already have dispatched the actual Application timer.
    if (ApplicationTestAccess::quitPending(*app))
        ApplicationTestAccess::timer(*app);
    require(!ApplicationTestAccess::quitPending(*app)
            && ApplicationTestAccess::shutdownNotice(*app) == nullptr
            && ApplicationTestAccess::hasService(*app),
        "Application timer did not defer teardown until safe quit readiness");
    const auto shutdown_begin = std::chrono::steady_clock::now();
    ApplicationTestAccess::shutdown(*app);
    const auto shutdown_end = std::chrono::steady_clock::now();
    require(!ApplicationTestAccess::hasService(*app),
        "Application shutdown did not release the completed service");
    const auto millis = [](auto duration) {
        return std::chrono::duration<double, std::milli>(duration).count();
    };
    std::cout << "MOCK lifecycle pending_reply_ms=" << millis(pending_reply - initial_quit_request)
        << " final_request_to_safe_join_ms=" << millis(safe_to_quit - final_quit_request)
        << " application_shutdown_with_service_destruction_ms=" << millis(shutdown_end - shutdown_begin)
        << " (injected pauses and UI dispatch; not physical device timing)\n";
}

using Test = void (*)();
struct Case { const wchar_t* name; Test run; };
constexpr Case cases[] = {
    {L"pending-switch", testPendingSwitchRetargetAndSafeShutdown},
    {L"cancel-close", testCancelShutdownDoesNotResumeClosedGeneration},
    {L"settings-fallback", testSettingsFollowFallbackAndPreserveUnappliedSelection},
    {L"application-quit", testApplicationQuitRemainsPendingThenShutsDownSafely},
};

int runChild(const wchar_t* name) {
    for (const auto& test : cases) {
        if (_wcsicmp(test.name, name) == 0) {
            test.run();
            return 0;
        }
    }
    return 2;
}

std::wstring quoted(const wchar_t* value) {
    std::wstring result = L"\"";
    for (const auto ch : std::wstring(value)) {
        if (ch == L'\"') result += L'\\';
        result += ch;
    }
    result += L'\"';
    return result;
}

std::string narrowAscii(const std::wstring& value) {
    std::string result;
    result.reserve(value.size());
    for (const auto ch : value) result.push_back(static_cast<char>(ch));
    return result;
}

struct ChildResult {
    DWORD wait_result{WAIT_FAILED};
    DWORD exit_code{STILL_ACTIVE};
    std::wstring log_path;
    std::string log;
    [[nodiscard]] bool passed() const noexcept {
        return wait_result == WAIT_OBJECT_0 && exit_code == 0;
    }
};

std::string readLog(HANDLE file) {
    LARGE_INTEGER start{};
    if (!SetFilePointerEx(file, start, nullptr, FILE_BEGIN)) return {};
    std::string result;
    char buffer[1024];
    DWORD count{};
    while (ReadFile(file, buffer, sizeof(buffer), &count, nullptr) && count > 0)
        result.append(buffer, count);
    return result;
}

ChildResult runIsolated(const wchar_t* name) {
    wchar_t executable[MAX_PATH]{};
    const auto length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    require(length > 0 && length < MAX_PATH,
        "Could not obtain the integration test executable path");

    wchar_t temp_directory[MAX_PATH]{};
    const auto temp_length = GetTempPathW(MAX_PATH, temp_directory);
    require(temp_length > 0 && temp_length < MAX_PATH,
        "Could not obtain a temporary directory for child diagnostics");
    ChildResult result;
    result.log_path = temp_directory;
    result.log_path += L"MGSTC_standalone_audio_lifetime_";
    result.log_path += std::to_wstring(GetCurrentProcessId());
    result.log_path += L"_";
    result.log_path += name;
    result.log_path += L".log";
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE log_file = CreateFileW(result.log_path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &security, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    require(log_file != INVALID_HANDLE_VALUE,
        "Could not create a child diagnostic log");

    auto command = quoted(executable) + L" --child=" + name;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = log_file;
    startup.hStdOutput = log_file;
    startup.hStdError = log_file;
    PROCESS_INFORMATION process{};
    if (CreateProcessW(executable, command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) == FALSE) {
        CloseHandle(log_file);
        throw std::runtime_error("Could not create an isolated SharedAudioService child");
    }
    result.wait_result = WaitForSingleObject(process.hProcess, 15000);
    if (result.wait_result == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 124);
        static_cast<void>(WaitForSingleObject(process.hProcess, 2000));
    }
    static_cast<void>(GetExitCodeProcess(process.hProcess, &result.exit_code));
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    static_cast<void>(FlushFileBuffers(log_file));
    result.log = readLog(log_file);
    CloseHandle(log_file);
    return result;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 2 && wcsncmp(argv[1], L"--child=", 8) == 0)
            return runChild(argv[1] + 8);
        for (const auto& test : cases) {
            const auto result = runIsolated(test.name);
            std::cout << "CASE " << narrowAscii(test.name)
                << " wait=" << result.wait_result << " exit=" << result.exit_code
                << " log=" << narrowAscii(result.log_path)
                << '\n' << result.log << std::flush;
            require(result.passed(),
                "SharedAudioService lifetime child failed or exceeded watchdog");
            std::wcout << L"PASS " << test.name << L'\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n' << std::flush;
        return 1;
    }
}
