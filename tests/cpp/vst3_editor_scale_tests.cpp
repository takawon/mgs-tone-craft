// SPDX-License-Identifier: AGPL-3.0-only
#include "plugin_editor.hpp"
#include "plugin_processor.hpp"
#include "mgstc_editor_view.hpp"
#include "ui_paths.hpp"
#include "ui_scale.hpp"

#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

juce::Point<int> sizeFor(int percent) {
    const auto factor = static_cast<float>(percent) / 100.0F;
    return {juce::roundToInt(UiLayout::Base::editorWindowW * factor),
            juce::roundToInt(UiLayout::Base::editorWindowH * factor)};
}

mgstc::app::CompositeEditorComponent& compositeOf(juce::Component& editor) {
    for (auto* child : editor.getChildren())
        if (auto* composite = dynamic_cast<mgstc::app::CompositeEditorComponent*>(child))
            return *composite;
    throw std::runtime_error("plugin contains the shared composite editor");
}

// JUCE's VST3 ContentWrapperComponent observes its direct AudioProcessorEditor
// child before requesting IPlugFrame::resizeView. Exercise that same Component
// notification boundary, including the host's onSize acknowledgement direction.
// This probe does not claim to be an actual DAW or an IPlugFrame implementation.
class HostResizeProbe final : public juce::Component {
public:
    explicit HostResizeProbe(juce::AudioProcessorEditor& editor) : editor_(editor) {
        addAndMakeVisible(editor_);
        setSize(editor_.getWidth(), editor_.getHeight());
        setVisible(true);
    }
    ~HostResizeProbe() override { removeChildComponent(&editor_); }
    void childBoundsChanged(juce::Component* child) override {
        if (child != &editor_ || applying_host_size_) return;
        ++requests;
        requested = {editor_.getWidth(), editor_.getHeight()};
        if (next_accepted_size) {
            const auto accepted = *next_accepted_size;
            next_accepted_size.reset();
            resizeFromHost(accepted);
        }
    }
    void resizeFromHost(juce::Point<int> size) {
        const juce::ScopedValueSetter<bool> accepting(applying_host_size_, true);
        editor_.setBoundsConstrained({0, 0, size.x, size.y});
    }
    int requests{};
    juce::Point<int> requested;
    std::optional<juce::Point<int>> next_accepted_size;
private:
    juce::AudioProcessorEditor& editor_;
    bool applying_host_size_{};
};

void requireSize(juce::AudioProcessorEditor& editor, juce::Point<int> expected) {
    require(editor.getWidth() == expected.x && editor.getHeight() == expected.y,
            "plugin outer size follows the requested scale immediately");
    const auto& content = compositeOf(editor);
    require(content.getBounds() == editor.getLocalBounds(),
            "shared content fills the current host-accepted editor bounds");
}

void pumpUntil(const std::function<bool()>& condition) {
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 5000.0;
    while (!condition() && juce::Time::getMillisecondCounterHiRes() < deadline)
        if (!juce::MessageManager::getInstance()->runDispatchLoopUntil(20)) break;
    require(condition(), "queued settings command completes within the test deadline");
}

juce::ComboBox& openScaleControl(juce::Component& editor) {
    juce::Button* settings = nullptr;
    std::function<void(juce::Component&)> find_settings = [&](juce::Component& parent) {
        for (auto* child : parent.getChildren()) {
            if (auto* button = dynamic_cast<juce::DrawableButton*>(child);
                button && button->getName() == "settings") settings = button;
            find_settings(*child);
        }
    };
    find_settings(editor);
    require(settings != nullptr, "shared editor exposes its real settings action");
    settings->triggerClick();
    pumpUntil([] { return juce::Component::getCurrentlyModalComponent() != nullptr; });
    juce::ComboBox* scale = nullptr;
    std::function<void(juce::Component&)> find_scale = [&](juce::Component& parent) {
        for (auto* child : parent.getChildren()) {
            if (auto* combo = dynamic_cast<juce::ComboBox*>(child);
                combo && combo->getNumItems() == 3
                && combo->getItemId(0) == 125 && combo->getItemId(2) == 75) scale = combo;
            find_scale(*child);
        }
    };
    find_scale(*juce::Component::getCurrentlyModalComponent());
    require(scale != nullptr, "real View settings contain the scale selector");
    return *scale;
}

void closeSettings() {
    if (auto* modal = juce::Component::getCurrentlyModalComponent()) modal->exitModalState(0);
    pumpUntil([] { return juce::Component::getCurrentlyModalComponent() == nullptr; });
}

void testImmediatePluginScale() {
    UiScale::setGlobalPercent(100, false);
    auto processor_a = std::make_unique<mgstc::plugin::MgstcAudioProcessor>();
    auto processor_b = std::make_unique<mgstc::plugin::MgstcAudioProcessor>();
    std::unique_ptr<juce::AudioProcessorEditor> editor_a(processor_a->createEditor());
    std::unique_ptr<juce::AudioProcessorEditor> editor_b(processor_b->createEditor());
    HostResizeProbe host_a(*editor_a), host_b(*editor_b);
    juce::MemoryBlock state_before, state_after;
    processor_a->getStateInformation(state_before);
    requireSize(*editor_a, sizeFor(100));
    requireSize(*editor_b, sizeFor(100));

    auto& scale = openScaleControl(*editor_a);
    for (const int percent : {125, 75, 100}) {
        const int requests_a = host_a.requests, requests_b = host_b.requests;
        scale.setSelectedId(percent, juce::sendNotificationSync);
        requireSize(*editor_a, sizeFor(percent));
        requireSize(*editor_b, sizeFor(percent));
        require(host_a.requests > requests_a && host_b.requests > requests_b,
                "View scale change reaches both editors' direct host notification boundary");
        require(host_a.requested == sizeFor(percent) && host_b.requested == sizeFor(percent),
                "host receives the new requested dimensions before editor recreation");
    }
    closeSettings();

    auto& composite_a = compositeOf(*editor_a);
    require(composite_a.keyPressed(juce::KeyPress('-', juce::ModifierKeys::ctrlModifier, '-')),
            "Ctrl-minus dispatches the existing per-editor scale handler");
    requireSize(*editor_a, sizeFor(75));
    requireSize(*editor_b, sizeFor(100));
    require(UiScale::global_percent == 100,
            "per-editor zoom does not change the global View scale");
    require(composite_a.keyPressed(juce::KeyPress('0', juce::ModifierKeys::ctrlModifier, '0')),
            "Ctrl-zero restores the editor to the global View scale");
    requireSize(*editor_a, sizeFor(100));

    const auto requests = host_a.requests;
    host_a.resizeFromHost({1111, 777});
    requireSize(*editor_a, {1111, 777});
    require(host_a.requests == requests,
            "host-origin bounds acknowledgement does not request another frame size");

    auto& acknowledged_scale = openScaleControl(*editor_a);
    host_a.next_accepted_size = juce::Point<int>{1550, 990};
    acknowledged_scale.setSelectedId(125, juce::sendNotificationSync);
    requireSize(*editor_a, {1550, 990});
    require(host_a.requested == sizeFor(125), "host sees the preferred size before accepting constrained bounds");
    requireSize(*editor_b, sizeFor(125));
    closeSettings();
    processor_a->getStateInformation(state_after);
    require(state_before == state_after, "UI-only resize leaves serialized Plugin State unchanged");
    UiScale::setGlobalPercent(100, false);
}
} // namespace

int main() {
    juce::ScopedJuceInitialiser_GUI juce_init;
    try {
        require(mgstcUseIsolatedApplicationDataDirectory(), "isolated application data is available");
        testImmediatePluginScale();
        std::puts("PASS: real View scale action, two plugin editors, host notification, host acknowledgement, per-editor zoom, state unchanged");
        return 0;
    } catch (const std::exception& error) {
        if (auto* modal = juce::Component::getCurrentlyModalComponent()) modal->exitModalState(0);
        std::fprintf(stderr, "VST3 editor scale regression failure: %s\n", error.what());
        return 1;
    }
}
