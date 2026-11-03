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
        {
            Browser loading({}, nullptr, missing);
            settle(loading);
            loading.setSize(900, 600);
            loading.addToDesktop(juce::ComponentPeer::windowIsTemporary);
            loading.setVisible(true);
            loading.worker_->load_attempt.fetch_add(1);
            loading.worker_->loading_database = true;
            loading.worker_->load_stage = engine::PatchSlideLoadStage::Vectors;
            loading.worker_->load_fraction = .6;
            loading.updateDatabaseLoadDialog();
            require(loading.database_busy_dialog_
                && loading.database_busy_dialog_->getName().contains(juce::String::fromUTF8("DB読込")),
                "A visible first DB load must show the existing rotating-parrot progress dialog");
            if (!screenshots.empty()) {
                const juce::File folder(screenshots.string());
                require(folder.createDirectory().wasOk(), "DB progress snapshot directory failed");
                juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
                auto* content = loading.database_busy_dialog_->getContentComponent();
                auto output = folder.getChildFile("patchslide-db-progress.png").createOutputStream();
                require(content && output && output->setPosition(0) && output->truncate().wasOk()
                    && juce::PNGImageFormat().writeImageToStream(content->createComponentSnapshot(content->getLocalBounds()), *output),
                    "DB progress snapshot failed");
            }
            loading.database_busy_dialog_->closeButtonPressed();
            require(loading.worker_->load_cancelled, "The progress cancel action must reach the existing DB worker");
            loading.worker_->loading_database = false;
            juce::MessageManager::getInstance()->runDispatchLoopUntil(80);
            require(!loading.database_busy_dialog_, "Completed or cancelled DB progress must close without a dangling modal window");
        }
        {
            TaskRegistry registry;
            auto loading = std::unique_ptr<Browser>(new Browser(Browser::Callbacks{}, &registry, missing));
            settle(*loading);
            loading->setSize(900, 600);
            loading->addToDesktop(juce::ComponentPeer::windowIsTemporary);
            loading->setVisible(true);
            loading->worker_->load_attempt.fetch_add(1);
            loading->worker_->loading_database = true;
            loading->updateDatabaseLoadDialog();
            auto dialog = loading->database_busy_dialog_;
            require(dialog != nullptr, "Plugin DB progress setup failed");
            loading.reset();
            require(dialog == nullptr, "Destroying a plugin browser must immediately destroy its progress window");
            registry.tasks.front()->join();
            require(registry.tasks.front()->completed(), "Plugin closure must leave its DB worker joinable by the host boundary");
        }
        {
            juce::Component owner;
            Browser loading({}, nullptr, missing);
            settle(loading);
            owner.addAndMakeVisible(loading);
            owner.setSize(900, 600);
            loading.setBounds(owner.getLocalBounds());
            owner.addToDesktop(juce::ComponentPeer::windowIsTemporary);
            owner.setVisible(true);
            loading.worker_->load_attempt.fetch_add(1);
            loading.worker_->loading_database = true;
            loading.updateDatabaseLoadDialog();
            auto dialog = loading.database_busy_dialog_;
            require(dialog != nullptr, "Hide-only DB progress setup failed");
            const auto generation = loading.generation_;
            // Library manager keeps its content alive when closing the window.
            loading.prepareToHide();
            owner.setVisible(false);
            require(dialog == nullptr && loading.worker_->load_cancelled
                && !loading.worker_->cancelled && loading.retry_load_when_visible_,
                "Hiding a retained library browser must close its progress modal and cancel only that load attempt");
            loading.worker_->loading_database = false;
            owner.setVisible(true);
            loading.timerCallback();
            require(loading.generation_ > generation && !loading.retry_load_when_visible_
                && !loading.worker_->cancelled,
                "Reshowing a retained library browser must retry its interrupted load on the same worker");
            settle(loading);
        }
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
        int selection_notifications=0;
        Browser* observed_browser=nullptr;
        std::optional<engine::CompositeTimbre> keyboardSelection;
        std::optional<engine::CompositeTimbre> last;
        Browser browser({[&](const auto& sound){++previews;last=sound;},
                         [&](const auto& sound){++applies;last=sound;},
                         [&](const auto& sound){++registrations;last=sound;}, [&]{++stops;}, [&](const auto& sound){
                             ++selection_notifications;
                             keyboardSelection = sound;
                             if (sound.layers.empty() && observed_browser)
                                 require(!observed_browser->selectedCandidate(), "Empty notification still exposes the old candidate identity");
                         }}, nullptr, directory, stock);
        observed_browser=&browser;
        {
            juce::LookAndFeel_V4 host_style;
            browser.setLookAndFeel(&host_style);
            bool matches = true;
            for (auto* choices : {&browser.mode_, &browser.match_, &browser.cluster_size_, &browser.audition_mode_})
                for (int index = 0; index < choices->getNumItems(); ++index)
                    matches = matches && isSegmentButton(choices->buttonAt(static_cast<std::size_t>(index)))
                        && dynamic_cast<MgstcLookAndFeel*>(&choices->buttonAt(static_cast<std::size_t>(index)).getLookAndFeel());
            browser.setLookAndFeel(nullptr);
            require(matches, "All requested toggle strips keep the existing segment design under a host's default theme");
        }
        settle(browser);
        if (!browser.db_) throw std::runtime_error("The deployed database is required for UI integration: " + browser.details_.getText().toStdString());
        {
            Browser reopened({}, nullptr, directory);
            reopened.setVisible(true);
            settle(reopened);
            require(reopened.db_ == browser.db_ && !reopened.database_busy_dialog_,
                "A hot shared DB must reopen without another loading dialog");
        }
        require(!browser.two_changes_.getToggleState() && !browser.rejected_.getToggleState(), "Filter defaults are not OFF");
        require(!browser.selectedTimbre() && !browser.apply_.isEnabled(), "Initial result implicitly became an action target");
        require(!browser.audition_mode_.isVisible() && !browser.compare_.isVisible(),
            "Library browser exposed envelope-only audition controls");
        require(browser.rejected_.getButtonText() == juce::String::fromUTF8("不安定な音色も表示 (注意)"),
            "Unstable-sound display label differs from the requested UI");
        for (auto* choices : {&browser.mode_, &browser.match_, &browser.cluster_size_})
            for (int index = 0; index < choices->getNumItems(); ++index)
                require(isSegmentButton(choices->buttonAt(static_cast<std::size_t>(index)))
                    && choices->buttonAt(static_cast<std::size_t>(index)).getClickingTogglesState(),
                    "PatchSlide choices do not use the shared segmented toggle style");
        press(browser.mode_.buttonAt(2)); settle(browser);
        engine::PatchSlideFilter all; all.max_change_count=1;
        require(browser.rows_==browser.db_->filter(all), "Initial one-change filter was not applied");
        for(auto index:browser.rows_) require(browser.db_->candidates()[index].change_count==1,"Two-change result leaked by default");
        const auto one=browser.rows_.front();
        browser.list_.selectRow(0);
        require(browser.selected_index_==one,"Explicit candidate selection failed");
        auto soundOne=browser.selection_;
        require(keyboardSelection == soundOne && previews == 0,
            "Keyboard selection must follow explicit selection without starting a timed preview");
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
        require(keyboardSelection == twoSound, "Stock keyboard selection differs from audition/apply");
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
        press(browser.match_.buttonAt(1));settle(browser);tags.tags_match_all=false;
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
                require(browser.tag_hint_.getY() == browser.match_.getY()
                    && browser.tag_hint_.getRight() < browser.match_.getX()
                    && browser.tag_list_.getY() == browser.tag_hint_.getBottom() + UiLayout::xs,
                    "Tag AND/OR must share the hint row and return the freed row to the list");
                require(browser.cluster_size_.getY() == browser.group_.getY()
                    && browser.cluster_size_.getRight() < browser.group_.getX()
                    && browser.group_.getRight() == browser.details_.getRight(),
                    "Classification toggles/group selector must share the unchanged left-column width");
                require(!browser.mode_.getBounds().intersects(browser.rejected_.getBounds())
                    && browser.mode_.getWidth() > UiLayout::libraryWidth,
                    "Search modes overlap the unstable-sound toggle or retain the old narrow menu width");
                if(baseWidth==1100 && baseHeight==700 && !screenshots.empty()) {
                    browser.list_.updateContent();browser.stock_list_.updateContent();
                    auto image=browser.createComponentSnapshot(browser.getLocalBounds());
                    juce::File file(juce::String((screenshots/("patchslide-"+std::to_string(scale)+".png")).wstring().c_str()));
                    auto out=file.createOutputStream();require(out && juce::PNGImageFormat().writeImageToStream(image,*out),"Snapshot failed");
                    for (const int mode : {1, 2}) {
                        browser.mode_.setSelectedId(mode, juce::dontSendNotification); browser.updateMode();
                        auto modeOutput = juce::File(juce::String((screenshots / ("patchslide-mode-" + std::to_string(mode)
                            + "-" + std::to_string(scale) + ".png")).wstring().c_str())).createOutputStream();
                        require(modeOutput && juce::PNGImageFormat().writeImageToStream(browser.createComponentSnapshot(browser.getLocalBounds()), *modeOutput),
                            "Tag/classification browser snapshot failed");
                    }
                    browser.mode_.setSelectedId(3, juce::dontSendNotification); browser.updateMode();
                }
            }
        }
        UiScale::setActivePercent(100);
        browser.setExternalQuerySource(*soundOne);
        require(browser.busy_ && !browser.selectedTimbre(), "External query retained an old implicit source");
        press(browser.stop_);
        require(!browser.busy_ && !browser.selectedTimbre(), "External query stop did not cancel without selecting a candidate");
        browser.requestBrowse();
        settle(browser);
        require(browser.query_kind_ == Browser::QueryKind::External && !browser.rows_.empty(),
            "Edited OPLL sound did not produce external similarity results");
        require(!browser.selectedTimbre() && !browser.variation_.isEnabled(),
            "External result invented a candidate ID for the query");
        require(browser.status_.getText().contains(juce::String::fromUTF8("編集中の音色から近似")),
            "External query was mislabeled as a DB candidate search");
        browser.list_.selectRow(0);
        require(browser.selectedCandidate() && browser.variation_.isEnabled(),
            "External result selection did not enable candidate-origin Variation");
        int mode_changes{}, compare_changes{};
        bool requested_whole = true, requested_original = false;
        browser.setEnvelopeAuditionControls([&](bool whole) { ++mode_changes; requested_whole = whole; },
            [&](bool original) { ++compare_changes; requested_original = original; });
        require(browser.auditionWholeComposite() && !browser.auditionOriginal()
            && browser.audition_mode_.isVisible() && browser.compare_.isVisible(),
            "Envelope audition must expose its controls with whole-composite/candidate defaults");
        press(browser.compare_);
        require(requested_original && browser.auditionOriginal() && compare_changes == 1,
            "A/B did not route to the original editing sound");
        press(browser.audition_mode_.buttonAt(0));
        require(!requested_whole && !browser.auditionWholeComposite() && mode_changes == 1,
            "Single PatchSlide audition switch did not reach its callback");
        browser.clearSelection();
        require(browser.auditionOriginal(), "Result clearing changed the retained audition side");
        browser.selectCandidate(one);
        require(!browser.auditionOriginal() && compare_changes == 1,
            "Explicit new candidate must silently reset A/B before its sole selection notification");
        browser.setEnvelopeAuditionWholeComposite(true, juce::sendNotificationSync);
        for (const int scale : {75, 100, 125}) {
            UiScale::setActivePercent(scale);
            browser.setSize(UiScale::sx(1100), UiScale::sx(700)); browser.resized();
            require(browser.audition_mode_.getY() == browser.stop_.getY()
                && browser.audition_mode_.getX() > browser.stop_.getRight()
                && browser.audition_mode_.getRight() < browser.apply_.getX(),
                "Envelope audition toggle does not fit to the right of Stop");
            require(browser.compare_.getY() == browser.two_changes_.getY()
                && browser.compare_.getX() > browser.two_changes_.getRight()
                && browser.compare_.getRight() < browser.stock_list_.getX(),
                "Envelope A/B does not fit beside the two-change filter");
            if (!screenshots.empty()) {
                auto output = juce::File(juce::String((screenshots / ("patchslide-envelope-" + std::to_string(scale) + ".png")).wstring().c_str())).createOutputStream();
                require(output && juce::PNGImageFormat().writeImageToStream(browser.createComponentSnapshot(browser.getLocalBounds()), *output),
                    "Envelope browser snapshot failed");
            }
        }
        UiScale::setActivePercent(100);
        press(browser.variation_); settle(browser);
        require(browser.query_kind_ == Browser::QueryKind::Variation,
            "Variation did not transition to the explicitly selected DB candidate");
        const auto notifications_before_adoption=selection_notifications;
        {
            const juce::ScopedValueSetter<bool> adopting(browser.adopting_result_,true);
            browser.selectCandidate(one);
            browser.clearSelection();
        }
        require(selection_notifications==notifications_before_adoption && !browser.selectedCandidate(),
            "Late result reconciliation changed the performance program or retained a cleared identity");
        browser.selectCandidate(one);
        browser.clearSelection();
        require(selection_notifications==notifications_before_adoption+2,
            "Explicit selection/clear must each deliver exactly one performance notification");
        auto invalid_query=*soundOne;
        invalid_query.layers.front().relative_semitones=-100;
        browser.setExternalQuerySource(std::move(invalid_query)); settle(browser);
        require(browser.db_ && !browser.selectedTimbre() && !browser.apply_.isEnabled()
            && browser.details_.getText().contains("supported pitch")
            && !browser.details_.getText().contains(juce::String::fromUTF8("正しい3ファイル")),
            "A failed acoustic query must preserve authoring and explain its input error without blaming a valid DB");
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
