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
    static void checkBrowseRejectToggle(PatchSlideBrowserComponent& browser, engine::PatchSlideFilter filter) {
        require(!browser.rejected_.getToggleState(), "Reject toggle setup must be OFF");
        press(browser.rejected_);
        settle(browser);
        filter.show_rejected = true;
        require(browser.rejected_.getToggleState() && browser.rows_ == browser.db_->filter(filter),
            "Reject ON did not update the current browse filter");
        press(browser.rejected_);
        settle(browser);
        filter.show_rejected = false;
        require(!browser.rejected_.getToggleState() && browser.rows_ == browser.db_->filter(filter),
            "Reject OFF did not restore the current browse filter");
    }
    static void checkSearchRejectToggle(PatchSlideBrowserComponent& browser) {
        const auto ranks = browser.ranks_;
        const auto visible = browser.rows_;
        press(browser.rejected_);
        settle(browser);
        require(browser.rejected_.getToggleState() && browser.ranks_.size() == ranks.size(),
            "Reject ON changed global search size");
        for (std::size_t i = 0; i < ranks.size(); ++i)
            require(browser.ranks_[i].candidate_index == ranks[i].candidate_index
                && browser.ranks_[i].rank == ranks[i].rank
                && browser.ranks_[i].squared_distance == ranks[i].squared_distance,
                "Reject visibility changed the global search ranking");
        std::vector<std::size_t> expected;
        for (const auto& row : ranks)
            if (engine::PatchSlideDatabase::isVisible(browser.db_->candidates()[row.candidate_index], true))
                expected.push_back(row.candidate_index);
        require(browser.rows_ == expected, "Reject ON did not expose the original search candidates");
        press(browser.rejected_);
        settle(browser);
        require(!browser.rejected_.getToggleState() && browser.rows_ == visible,
            "Reject OFF did not restore the filtered global search results");
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
        using Browser = PatchSlideBrowserComponent;
        const auto missing = std::filesystem::current_path() / "missing-patchslide-ui-test";
        { Browser absent({}, nullptr, missing); settle(absent);
          require(!absent.db_ && !absent.selectedTimbre(), "Missing database exposes an action");
          require(absent.details_.getText().contains("patchslide.sqlite"), "Missing-file diagnostic lost"); }
        { TaskRegistry registry;
          { Browser temporary({}, &registry, directory); temporary.requestBrowse(); }
          require(registry.tasks.size() == 1, "Worker was not registered before browser lifetime");
          registry.tasks.front()->join(); require(registry.tasks.front()->completed(), "Closed browser worker survived host join"); }
        {
            Browser::Worker pending;
            Browser::Query reload;reload.generation=1;reload.reload=true;pending.submit(reload);
            Browser::Query filter;filter.generation=2;pending.submit(filter);
            require(pending.pending && pending.pending->generation==2 && pending.pending->reload,
                "Coalescing a filter request discarded an unconsumed reload");
        }
        auto stock = std::make_shared<PatchSlideStock>();
        int previews=0, applies=0, registrations=0, stops=0;
        std::optional<engine::CompositeTimbre> last;
        Browser browser({[&](const auto& sound){++previews;last=sound;},
                         [&](const auto& sound){++applies;last=sound;},
                         [&](const auto& sound){++registrations;last=sound;}, [&]{++stops;}}, nullptr, directory, stock);
        settle(browser);
        if (!browser.db_) throw std::runtime_error("The deployed database is required for UI integration: " + browser.details_.getText().toStdString());
        require(!browser.two_changes_.getToggleState() && !browser.rejected_.getToggleState(), "Filter defaults are not OFF");
        require(!browser.selectedTimbre() && !browser.apply_.isEnabled(), "Initial result implicitly became an action target");
        browser.mode_.setSelectedId(3, juce::sendNotificationSync); settle(browser);
        engine::PatchSlideFilter all; all.max_change_count=1;
        require(browser.rows_==browser.db_->filter(all), "Initial one-change filter was not applied");
        for(auto index:browser.rows_) require(browser.db_->candidates()[index].change_count==1,"Two-change result leaked by default");
        const auto one=browser.rows_.front();
        browser.list_.selectRow(0);
        require(browser.selected_index_==one,"Explicit candidate selection failed");
        auto soundOne=browser.selection_;
        std::unique_ptr<Browser::RowControls> check(static_cast<Browser::RowControls*>(browser.refreshComponentForRow(0,false,nullptr)));
        press(check->check);
        const auto oneId=browser.db_->candidates()[one].candidate_id;
        require(stock->contains(oneId) && browser.selection_==soundOne,"Checkbox changed active sound or failed to stock");
        require(stock->ids().size()==1,"Stock duplicate appeared");
        browser.list_.keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));
        require(stock->ids().empty(),"Keyboard Space did not unstock");
        browser.list_.keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));
        press(browser.two_changes_); settle(browser); all.max_change_count=2;
        require(browser.rows_==browser.db_->filter(all),"Two-change ON failed");
        if(browser.db_->candidates().size()==12180) require(browser.rows_.size()==11974,"Reject OFF coverage changed");
        const auto twoRow=std::find_if(browser.rows_.begin(),browser.rows_.end(),[&](auto i){return browser.db_->candidates()[i].change_count==2;});
        require(twoRow!=browser.rows_.end(),"No two-change candidate available");
        const auto two=*twoRow; const auto twoId=browser.db_->candidates()[two].candidate_id;
        const auto beforeCheck = browser.selection_;
        std::unique_ptr<Browser::RowControls> twoCheck(static_cast<Browser::RowControls*>(
            browser.refreshComponentForRow(static_cast<int>(std::distance(browser.rows_.begin(),twoRow)),false,nullptr)));
        press(twoCheck->check); press(twoCheck->check); press(twoCheck->check);
        require(browser.selection_==beforeCheck,"Checking another candidate implicitly changed the action target");
        require(stock->ids()==std::vector<std::string>{oneId,twoId},"Stable-ID stock insertion/dedup order failed");
        browser.stock_list_.selectRow(1);
        const auto twoSound=browser.selection_;
        require(twoSound && browser.selected_index_==two && browser.list_.getSelectedRow()<0,"Stock did not become the exclusive target");
        require(browser.details_.getText().contains(juce::String(twoId)),"Stock details did not follow selection");
        press(browser.preview_); require(last==twoSound && previews==1,"Stock audition used another sound");
        press(browser.apply_); require(last==twoSound && applies==1,"Stock apply used another sound");
        press(browser.register_); require(last==twoSound && registrations==1 && stock->ids().size()==2,"Stock registration removed or replaced the item");
        press(browser.two_changes_); settle(browser); all.max_change_count=1;
        require(stock->contains(twoId) && browser.selected_index_==two && browser.selection_==twoSound,"Filter removed the stock target");
        require(browser.rows_==browser.db_->filter(all),"One-change OFF restoration failed");
        press(browser.similar_); settle(browser);
        require(browser.ranks_.size()==browser.db_->candidates().size()-1,"Standard B truncated before eligibility filtering");
        for(auto i:browser.rows_)require(browser.db_->candidates()[i].change_count==1,"Standard B ignored change count");
        require(browser.selection_==twoSound,"Search completion stole the stock action target");
        press(browser.variation_); settle(browser);
        require(browser.ranks_.size()==3,"Filtered Variation lost the three eligible candidates");
        for(const auto& r:browser.ranks_)require(browser.db_->candidates()[r.candidate_index].change_count==1,"Variation selected an ineligible candidate");
        checkSearchRejectToggle(browser);
        require(browser.selection_==twoSound,"Reject display stole the stock selection");
        press(browser.back_); settle(browser);
        std::unique_ptr<Browser::RowControls> trash(static_cast<Browser::RowControls*>(
            browser.stock_model_.refreshComponentForRow(browser.stock_list_.getSelectedRow(),true,nullptr)));
        press(trash->trash);
        require(!stock->contains(twoId) && !browser.selectedTimbre() && !browser.apply_.isEnabled(),"Removing active stock implicitly selected another item");
        browser.list_.selectRow(0); require(browser.stock_list_.getSelectedRow()<0,"Candidate selection left a competing stock highlight");
        browser.returnKeyPressed(0); require(previews==2,"Candidate Return audition failed");
        browser.random_.onClick();require(previews==3,"Discovery did not audition an explicitly chosen result");
        press(browser.two_changes_);settle(browser);
        const auto twoAgain=std::find(browser.rows_.begin(),browser.rows_.end(),two);
        browser.list_.selectRow(static_cast<int>(std::distance(browser.rows_.begin(),twoAgain)));
        press(browser.two_changes_);settle(browser);
        require(!browser.selectedTimbre() && browser.list_.getSelectedRow()<0,"Disappearing candidate silently selected another sound");
        browser.mode_.setSelectedId(1,juce::sendNotificationSync);settle(browser);
        browser.tag_list_.selectRow(0);if(browser.used_tags_.size()>1)browser.tag_list_.selectRow(1,true,false);
        browser.match_.setSelectedId(1,juce::sendNotificationSync);settle(browser);
        engine::PatchSlideFilter tags;tags.max_change_count=1;tags.tags={browser.used_tags_[0].tag};
        if(browser.used_tags_.size()>1)tags.tags.push_back(browser.used_tags_[1].tag);
        require(browser.rows_==browser.db_->filter(tags),"One-change AND tags mismatch");
        checkBrowseRejectToggle(browser,tags);
        browser.match_.setSelectedId(2,juce::sendNotificationSync);settle(browser);tags.tags_match_all=false;
        require(browser.rows_==browser.db_->filter(tags),"One-change OR tags mismatch");
        checkBrowseRejectToggle(browser,tags);
        browser.mode_.setSelectedId(2,juce::sendNotificationSync);
        for(int count:{16,64}) {
            browser.cluster_size_.setSelectedId(count,juce::sendNotificationSync);
            browser.group_.setSelectedId(2,juce::sendNotificationSync);settle(browser);
            engine::PatchSlideFilter group;group.max_change_count=1;group.cluster_count=count;group.cluster_id=0;
            require(browser.rows_==browser.db_->filter(group),"One-change cluster mismatch");
            const auto rep=browser.selectedGroup()->representative_index;
            const bool visible=std::find(browser.rows_.begin(),browser.rows_.end(),rep)!=browser.rows_.end();
            require(browser.representative_.isEnabled()==visible,"Representative ignored eligibility");
            checkBrowseRejectToggle(browser,group);
        }
        press(browser.two_changes_);settle(browser);
        require(browser.two_changes_.getToggleState(),"Changing category reset the toggle");
        const auto representative=browser.selectedGroup()->representative_index;
        press(browser.representative_);require(browser.selected_index_==representative,"Representative failed to select/audition");
        press(browser.similar_);settle(browser);
        require(!browser.representative_.isEnabled() && !browser.selectedTimbre(),"Search auto-selected a result or enabled an out-of-result representative");
        press(browser.representative_);require(!browser.selectedTimbre(),"Disabled representative changed the target");
        if(!browser.rows_.empty()) {browser.list_.selectRow(0);auto chosen=browser.selection_;press(browser.apply_);require(last==chosen,"Result apply disagreed with highlight");}
        const auto retained=browser.rows_;
        {std::lock_guard lock(browser.worker_->mutex);Browser::Result old;old.generation=browser.generation_-1;browser.worker_->ready=std::move(old);}
        browser.timerCallback();require(browser.rows_==retained && browser.db_,"Obsolete result overwrote current data");
        press(browser.back_);settle(browser);
        browser.mode_.setSelectedId(3,juce::sendNotificationSync);settle(browser);
        checkBrowseRejectToggle(browser,engine::PatchSlideFilter{});
        press(browser.rejected_);settle(browser);
        if(browser.db_->candidates().size()==12180)require(browser.rows_.size()==12180,"Reject ON coverage changed");
        const auto rejected=std::find_if(browser.rows_.begin(),browser.rows_.end(),[&](auto i){return engine::PatchSlideDatabase::isRejected(browser.db_->candidates()[i]);});
        require(rejected!=browser.rows_.end(),"No Reject row exposed");
        const auto rejectId=browser.db_->candidates()[*rejected].candidate_id;
        browser.toggleStock(rejectId);browser.stock_list_.selectRow(static_cast<int>(stock->ids().size()-1));
        require(browser.details_.getText().contains("Reject"),"Stock lost the Reject warning");
        press(browser.rejected_);settle(browser);require(browser.selectedTimbre() && stock->contains(rejectId),"Reject OFF deleted existing stock");
        {Browser reopened({},nullptr,directory,stock);settle(reopened);
         require(reopened.stock_ids_==stock->ids() && !reopened.two_changes_.getToggleState(),"Browser recreation lost stock or changed default filter");
         require(!reopened.selectedTimbre(),"Reopened browser implicitly selected a sound");
         reopened.removeStock(oneId);browser.timerCallback();require(!browser.stock_->contains(oneId),"Shared stock removal did not propagate");}
        stock->add("missing_stable_id");browser.timerCallback();
        browser.stock_list_.selectRow(static_cast<int>(stock->ids().size()-1));
        require(!browser.selectedTimbre() && !browser.apply_.isEnabled(),"Unavailable stock ID bound to a different row");
        browser.stock_list_.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey));
        require(!stock->contains("missing_stable_id"),"Delete could not remove an unavailable stock ID");
        {
            const auto db=browser.db_;const auto names=browser.names_;const auto rows=browser.rows_;
            const auto tagCount=browser.used_tags_.size();
            Browser::Result failed;failed.generation=++browser.generation_;failed.error="Test reload failure: patchslide.sqlite";
            {std::lock_guard lock(browser.worker_->mutex);browser.worker_->ready=std::move(failed);}
            browser.timerCallback();require(!browser.db_ && browser.used_tags_.empty(),"Failed reload kept a stale tag catalog");
            Browser::Result recovered;recovered.generation=++browser.generation_;recovered.db=db;recovered.names=names;recovered.rows=rows;recovered.total=rows.size();
            {std::lock_guard lock(browser.worker_->mutex);browser.worker_->ready=std::move(recovered);}
            browser.timerCallback();require(browser.db_==db && browser.used_tags_.size()==tagCount,"Reload recovery duplicated the tag catalog");
            require(browser.stock_ids_==stock->ids(),"Reload failure/recovery deleted stock IDs");
        }
        Browser independent({},nullptr,directory);settle(independent);require(independent.stock_ids_.empty(),"Unrelated browser owner inherited stock");
        if (browser.two_changes_.getToggleState()) { press(browser.two_changes_); settle(browser); }
        if (!browser.stock_ids_.empty()) browser.stock_list_.selectRow(0);
        if(!screenshots.empty())std::filesystem::create_directories(screenshots);
        for(int scale:{75,100,125}) {
            UiScale::setActivePercent(scale);
            for(int baseWidth:{1100,1250,1500}) for(int baseHeight:{560,700}) {
                browser.setSize(UiScale::sx(baseWidth),UiScale::sx(baseHeight));browser.resized();
                const auto resultRowHeight = UiLayout::fieldH * 3 / 2;
                require(browser.list_.getRowHeight()==resultRowHeight
                    && browser.stock_list_.getRowHeight()==resultRowHeight,
                    "Candidate and stock rows did not use the compact shared height");
                Browser::RowControls rowControls;
                rowControls.setBounds(0,0,100,resultRowHeight);
                require(rowControls.check.getWidth()==UiLayout::fieldH
                    && rowControls.check.getHeight()==resultRowHeight,
                    "Candidate stock checkbox lost its full-height hit area");
                require(rowControls.trash.getWidth()==UiLayout::fieldH
                    && rowControls.trash.getHeight()==UiLayout::fieldH,
                    "Stock removal button no longer fits its compact row");
                require(browser.details_.getWidth()==UiLayout::libraryWidth && browser.details_.getHeight()==UiLayout::libraryMemoH+UiLayout::fieldH,"Fixed detail dimensions changed");
                require(browser.details_.getX()==browser.tag_list_.getX(),"Detail is not beneath the left column");
                require(browser.list_.getBottom()==browser.stock_list_.getBottom() && browser.list_.getBottom()<browser.preview_.getY(),"Lists did not stretch to the bottom controls");
                require(!browser.list_.getBounds().intersects(browser.stock_list_.getBounds()),"Columns overlap");
                require(browser.two_changes_.getRight()<browser.stock_list_.getX(),"Toolbar overlaps stock");
                if(baseWidth==1100 && baseHeight==700 && !screenshots.empty()) {
                    browser.list_.updateContent();browser.stock_list_.updateContent();
                    auto image=browser.createComponentSnapshot(browser.getLocalBounds());
                    juce::File file(juce::String((screenshots/("patchslide-"+std::to_string(scale)+".png")).wstring().c_str()));
                    auto out=file.createOutputStream();require(out && juce::PNGImageFormat().writeImageToStream(image,*out),"Snapshot failed");
                }
            }
        }
        UiScale::setActivePercent(100);
        require(stops>0,"No selection/preview stop boundary ran");
        std::cout<<"PatchSlide stock/filter/explicit selection/layout and async regressions passed\n";
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
        const auto* test_db_directory = std::getenv("MGSTC_PATCHSLIDE_TEST_DB_DIR");
        mgstc::app::PatchSlideBrowserTestAccess::run(
            argc > 1 ? std::filesystem::path(argv[1])
                : test_db_directory ? std::filesystem::path(test_db_directory)
                    : mgstc::engine::PatchSlideDatabase::defaultDirectory(),
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
