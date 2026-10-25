// SPDX-License-Identifier: AGPL-3.0-only
#include "../../src/app/juce/patchslide_browser.hpp"
#include "../../src/app/juce/mgstc_look_and_feel.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace mgstc::app {
class PatchSlideBrowserTestAccess {
    static void require(bool condition, const char* description) {
        if (!condition) throw std::runtime_error(description);
    }
    static void settle(PatchSlideBrowserComponent& browser) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(40);
        while (browser.busy_ && std::chrono::steady_clock::now() < end) {
            browser.timerCallback();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        require(!browser.busy_, "Browser worker did not complete");
    }
    static void press(juce::Button& button) {
        button.triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    }
    struct TaskRegistry final : BackgroundTaskBoundary {
        std::vector<std::shared_ptr<EditorBackgroundTask>> tasks;
        bool registerTask(std::shared_ptr<EditorBackgroundTask> task) override {
            tasks.push_back(std::move(task));
            return true;
        }
        ~TaskRegistry() override {
            for (const auto& task : tasks) task->cancel();
            for (const auto& task : tasks) task->join();
        }
    };
public:
    static void run(const std::filesystem::path& directory, const std::filesystem::path& screenshots) {
        // Missing deployment fails in the browser without invoking a host action.
        const auto missing = std::filesystem::current_path()
            / ("missing-patchslide-" + std::to_string(juce::Random::getSystemRandom().nextInt()));
        {
            PatchSlideBrowserComponent browser({}, nullptr, missing);
            settle(browser);
            require(!browser.db_, "Missing deployment unexpectedly loaded");
            require(!browser.selectedTimbre(), "Missing deployment exposes an audition");
            require(browser.details_.getText().contains("patchslide.sqlite"), "Missing file explanation absent");
            require(!browser.preview_.isEnabled() && !browser.apply_.isEnabled(), "Missing deployment actions enabled");
        }
        // A registered task survives browser destruction until the host joins it.
        {
            TaskRegistry registry;
            {
                PatchSlideBrowserComponent browser({}, &registry, directory);
                browser.requestBrowse();
                browser.requestBrowse();
            }
            require(registry.tasks.size() == 1, "Browser worker was not registered");
            registry.tasks.front()->join();
            require(registry.tasks.front()->completed(), "Closed browser worker survived host join");
        }
        int previews = 0, applies = 0, registrations = 0, stops = 0;
        std::optional<engine::CompositeTimbre> last_preview, last_apply, last_registration;
        PatchSlideBrowserComponent browser({
            [&](const auto& sound) { ++previews; last_preview = sound; },
            [&](const auto& sound) { ++applies; last_apply = sound; },
            [&](const auto& sound) { ++registrations; last_registration = sound; },
            [&] { ++stops; }}, nullptr, directory);
        const int stops_after_initial_request = stops;
        settle(browser);
        require(stops == stops_after_initial_request,
            "Async initial selection stopped notes begun outside the browser");
        if (!browser.db_) {
            std::cout << "SKIP: deployed PatchSlide DB unavailable; missing/closure tests passed\n";
            return;
        }
        require(!browser.rejected_.getToggleState(), "Reject visibility must default off");
        require(!browser.used_tags_.empty(), "Used tags not populated");
        require(std::all_of(browser.used_tags_.begin(), browser.used_tags_.end(),
            [](const auto& tag) { return tag.candidate_count > 0; }), "Unused tag shown");
        const auto initial_selection = browser.selectedTimbre();
        require(initial_selection.has_value(), "First candidate was not selected");
        require(previews == 0 && applies == 0 && registrations == 0, "Selection mutated or auditioned the host");

        // Queue several different requests before polling. The last mode/filter
        // must determine the displayed candidates, even if an old job finishes.
        browser.mode_.setSelectedId(2, juce::sendNotificationSync);
        browser.cluster_size_.setSelectedId(64, juce::sendNotificationSync);
        browser.group_.setSelectedId(3, juce::sendNotificationSync);
        browser.mode_.setSelectedId(3, juce::sendNotificationSync);
        const int stops_after_requests = stops;
        settle(browser);
        require(stops == stops_after_requests,
            "Async search completion stopped notes begun after its request");
        engine::PatchSlideFilter all;
        require(browser.rows_ == browser.db_->filter(all), "Old group result replaced latest all-candidate request");
        require(browser.random_.isVisible(), "Discovery action is missing from all-candidate mode");
        for (const auto index : browser.rows_)
            require(!engine::PatchSlideDatabase::isRejected(browser.db_->candidates()[index]), "Reject exposed by default");
        browser.random_.onClick();
        require(previews == 1, "Discovery did not audition its selection");
        require(browser.selected_index_.has_value(), "Discovery did not select a candidate");
        const auto before = browser.list_.getSelectedRow();
        if (before + 1 < browser.getNumRows()) {
            browser.list_.keyPressed(juce::KeyPress(juce::KeyPress::downKey));
            require(browser.list_.getSelectedRow() == before + 1, "Candidate list keyboard selection failed");
        }
        browser.returnKeyPressed(browser.list_.getSelectedRow());
        require(previews == 2, "Return did not audition the selected candidate");
        browser.apply_.onClick();
        browser.register_.onClick();
        require(applies == 1 && registrations == 1, "Native timbre action callbacks failed");
        require(stops > 0, "Selection changes did not stop previous audition");

        browser.mode_.setSelectedId(1, juce::sendNotificationSync);
        settle(browser);
        require(browser.tag_list_.isVisible() && !browser.random_.isVisible(), "Tag mode controls incorrect");
        browser.tag_list_.selectRow(0);
        if (browser.used_tags_.size() > 1) browser.tag_list_.selectRow(1, true, false);
        browser.match_.setSelectedId(1, juce::sendNotificationSync);
        settle(browser);
        engine::PatchSlideFilter tags;
        tags.tags.push_back(browser.used_tags_[0].tag);
        if (browser.used_tags_.size() > 1) tags.tags.push_back(browser.used_tags_[1].tag);
        require(browser.rows_ == browser.db_->filter(tags), "AND tag filter mismatch");
        browser.match_.setSelectedId(2, juce::sendNotificationSync);
        settle(browser);
        tags.tags_match_all = false;
        require(browser.rows_ == browser.db_->filter(tags), "OR tag filter mismatch");

        browser.mode_.setSelectedId(2, juce::sendNotificationSync);
        for (const int count : {16, 64}) {
            browser.cluster_size_.setSelectedId(count, juce::sendNotificationSync);
            browser.group_.setSelectedId(2, juce::sendNotificationSync);
            settle(browser);
            engine::PatchSlideFilter group;
            group.cluster_count = count;
            group.cluster_id = 0;
            require(browser.rows_ == browser.db_->filter(group), "Independent classification filter mismatch");
            require(browser.selectedGroup() && browser.selectedGroup()->cluster_count == count, "Representative belongs to wrong classification");
        }

        // A representative is absent from its own Standard B results. Pressing
        // the representative action there must not silently replace the
        // highlighted result's preview/apply/register target.
        require(browser.representative_.isEnabled(), "Browse representative action is unavailable");
        const auto representative = browser.selectedGroup()->representative_index;
        press(browser.representative_);
        require(browser.selected_index_ == representative && last_preview == browser.selection_,
            "Representative action did not audition the selected representative");
        require(browser.list_.getSelectedRow() >= 0
            && browser.rows_[static_cast<std::size_t>(browser.list_.getSelectedRow())] == representative,
            "Representative action did not synchronize list selection");
        press(browser.similar_);
        settle(browser);
        require(browser.query_kind_ == PatchSlideBrowserComponent::QueryKind::StandardB
            && std::find(browser.rows_.begin(), browser.rows_.end(), representative) == browser.rows_.end(),
            "Representative Standard B setup did not exclude its source");
        require(!browser.representative_.isEnabled(), "Standard B exposes an out-of-result representative action");
        const auto result_index = browser.selected_index_;
        const auto result_row = browser.list_.getSelectedRow();
        const auto result_timbre = browser.selection_;
        const auto result_previews = previews;
        require(result_timbre.has_value(), "Standard B did not select an actionable result");
        press(browser.representative_);
        require(browser.selected_index_ == result_index && browser.list_.getSelectedRow() == result_row
            && browser.selection_ == result_timbre && previews == result_previews,
            "Disabled representative action changed the highlighted result's target");
        press(browser.apply_);
        press(browser.register_);
        require(last_apply == result_timbre && last_registration == result_timbre,
            "Search result actions did not use the highlighted timbre");
        press(browser.variation_);
        settle(browser);
        require(browser.query_kind_ == PatchSlideBrowserComponent::QueryKind::Variation
            && !browser.representative_.isEnabled(), "Variation exposes the classification representative action");
        press(browser.back_);
        settle(browser);
        require(browser.query_kind_ == PatchSlideBrowserComponent::QueryKind::Browse
            && browser.representative_.isEnabled(), "Returning to classification did not restore representative browsing");
        press(browser.representative_);
        require(browser.selected_index_ == representative
            && browser.rows_[static_cast<std::size_t>(browser.list_.getSelectedRow())] == representative,
            "Representative selection was not restored after returning to browse");

        browser.mode_.setSelectedId(3, juce::sendNotificationSync);
        settle(browser);
        const auto source = *browser.selected_index_;
        browser.similar_.onClick();
        settle(browser);
        require(browser.query_kind_ == PatchSlideBrowserComponent::QueryKind::StandardB, "Standard B mode was not retained");
        require(std::find(browser.rows_.begin(), browser.rows_.end(), source) == browser.rows_.end(), "Standard B included its source");
        require(browser.ranks_.size() == browser.db_->candidates().size() - 1, "Standard B did not search all candidates");
        browser.variation_.onClick();
        settle(browser);
        require(browser.ranks_.size() == 3 && browser.rows_.size() <= 3, "Variation filled filtered positions or changed count");
        std::vector<std::size_t> expected_visible;
        for (const auto& row : browser.ranks_)
            if (engine::PatchSlideDatabase::isVisible(browser.db_->candidates()[row.candidate_index])) expected_visible.push_back(row.candidate_index);
        require(browser.rows_ == expected_visible, "Variation display reordered or replaced selected candidates");

        // Inject only an obsolete delivery, representing a finished earlier job.
        const auto retained_rows = browser.rows_;
        {
            std::lock_guard lock(browser.worker_->mutex);
            PatchSlideBrowserComponent::Result old;
            old.generation = browser.generation_ - 1;
            browser.worker_->ready = std::move(old);
        }
        browser.timerCallback();
        require(browser.rows_ == retained_rows && browser.db_, "Obsolete generation changed current UI");
        browser.back_.onClick();
        settle(browser);
        browser.rejected_.setToggleState(true, juce::dontSendNotification);
        browser.rejected_.onClick();
        settle(browser);
        all.show_rejected = true;
        require(browser.rows_ == browser.db_->filter(all), "Explicit Reject setting not applied");

        if (!screenshots.empty()) {
            std::filesystem::create_directories(screenshots);
            for (const int scale : {75, 100, 125}) {
                UiScale::setActivePercent(scale);
                browser.setSize(UiLayout::libraryManagerW, UiLayout::libraryManagerH);
                browser.resized();
                require(browser.list_.getHeight() > UiLayout::fieldH, "Scaled candidate list has no usable height");
                require(browser.register_.getRight() <= browser.getWidth(), "Scaled actions overflow browser");
                const auto image = browser.createComponentSnapshot(browser.getLocalBounds());
                juce::File output(juce::String((screenshots / ("patchslide-" + std::to_string(scale) + ".png")).wstring().c_str()));
                auto stream = output.createOutputStream();
                require(stream && juce::PNGImageFormat().writeImageToStream(image, *stream), "Browser snapshot failed");
            }
            UiScale::setActivePercent(100);
        }
        std::cout << "PatchSlide browser interaction/async/filter tests passed\n";
    }
};
} // namespace mgstc::app

int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI gui;
    struct ScopedStyle {
        MgstcLookAndFeel style;
        ScopedStyle() { juce::LookAndFeel::setDefaultLookAndFeel(&style); }
        ~ScopedStyle() { juce::LookAndFeel::setDefaultLookAndFeel(nullptr); }
    } style;
    try {
        mgstc::app::PatchSlideBrowserTestAccess::run(
            argc > 1 ? std::filesystem::path(argv[1]) : mgstc::engine::PatchSlideDatabase::defaultDirectory(),
            argc > 2 ? std::filesystem::path(argv[2])
                : std::getenv("MGSTC_PATCHSLIDE_SCREENSHOTS")
                    ? std::filesystem::path(std::getenv("MGSTC_PATCHSLIDE_SCREENSHOTS"))
                    : std::filesystem::path{});
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
