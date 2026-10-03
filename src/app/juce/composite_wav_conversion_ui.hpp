// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

// Included inside mgstc::app, after shared conversion/progress helpers.
// Owns completed PCM only; decoding, analysis and rendering run on workers.
class CompositeWavPcmPlayer final : private juce::AudioIODeviceCallback {
public:
    explicit CompositeWavPcmPlayer(PcmPreviewBoundary* host = nullptr) : host_(host) {}
    ~CompositeWavPcmPlayer() { stop(); if (manager_) manager_->closeAudioDevice(); }

    juce::String play(std::shared_ptr<const mgstc::engine::SourcePcm> pcm,
                      std::size_t begin, std::size_t end, float gain = 1.0f) {
        stop();
        if (!pcm || pcm->channels == 0 || pcm->sample_rate == 0
            || begin >= end || end > pcm->mono_samples.size()) return "Invalid PCM range";
        if (host_) return host_->play(std::move(pcm), begin, end, gain)
            ? juce::String{} : juce::String::fromUTF8("DAW出力から試聴できません。オーディオ処理を開始してください。");
        if (!manager_) manager_ = std::make_unique<juce::AudioDeviceManager>();
        if (!manager_->getCurrentAudioDevice()) {
            // Shared Windows endpoint only. Never selects ASIO or changes the editor route.
            bool found = false;
            for (auto* type : manager_->getAvailableDeviceTypes()) {
                if (type->getTypeName().equalsIgnoreCase("Windows Audio")) {
                    manager_->setCurrentAudioDeviceType(type->getTypeName(), true);
                    found = true;
                    break;
                }
            }
            if (!found) return juce::String::fromUTF8("Windows Audioの出力を利用できません");
            const auto error = manager_->initialise(0, 2, nullptr, true);
            if (error.isNotEmpty()) return error;
        }
        pcm_ = std::move(pcm);
        position_ = static_cast<double>(begin);
        end_ = end;
        gain_ = gain;
        step_ = static_cast<double>(pcm_->sample_rate)
            / manager_->getCurrentAudioDevice()->getCurrentSampleRate();
        playing_.store(true, std::memory_order_release);
        manager_->addAudioCallback(this);
        attached_ = true;
        return {};
    }

    void stop() {
        if (host_) { host_->stop(); return; }
        if (attached_) manager_->removeAudioCallback(this); // Quiesce before replacing PCM.
        attached_ = false;
        playing_.store(false, std::memory_order_release);
        pcm_.reset();
    }
private:
    void audioDeviceAboutToStart(juce::AudioIODevice*) override {}
    void audioDeviceStopped() override { playing_.store(false, std::memory_order_release); }
    void audioDeviceIOCallbackWithContext(const float* const*, int,
        float* const* outputs, int channels, int count,
        const juce::AudioIODeviceCallbackContext&) override {
        for (int channel = 0; channel < channels; ++channel)
            if (outputs[channel]) juce::FloatVectorOperations::clear(outputs[channel], count);
        if (!playing_.load(std::memory_order_acquire) || !pcm_) return;
        for (int sample = 0; sample < count && position_ < static_cast<double>(end_); ++sample) {
            const auto left = static_cast<std::size_t>(position_);
            const auto right = std::min(left + 1, end_ - 1);
            const auto fraction = static_cast<float>(position_ - static_cast<double>(left));
            for (int channel = 0; channel < channels; ++channel) {
                if (!outputs[channel]) continue;
                const auto source_channel = std::min<std::size_t>(
                    static_cast<std::size_t>(channel), pcm_->channels - 1);
                const auto a = pcm_->interleaved_samples[left * pcm_->channels + source_channel];
                const auto b = pcm_->interleaved_samples[right * pcm_->channels + source_channel];
                outputs[channel][sample] = juce::jlimit(-1.0f, 1.0f, (a + fraction * (b - a)) * gain_);
            }
            position_ += step_;
        }
        if (position_ >= static_cast<double>(end_)) playing_.store(false, std::memory_order_release);
    }
    PcmPreviewBoundary* host_{};
    std::unique_ptr<juce::AudioDeviceManager> manager_;
    std::shared_ptr<const mgstc::engine::SourcePcm> pcm_;
    std::atomic<bool> playing_{false};
    double position_{}, step_{1.0};
    float gain_{1.0f};
    std::size_t end_{};
    bool attached_{};
};

class CompositeWavRange final : public juce::Component, public juce::SettableTooltipClient {
    friend struct CompositeWavConversionTestAccess;
public:
    std::function<void(bool)> changed; // true = start handle, called on release only.
    CompositeWavRange() { setWantsKeyboardFocus(true); }
    void setSource(std::shared_ptr<const mgstc::engine::SourcePcm> source) {
        source_ = std::move(source);
        begin_ = 0;
        end_ = source_ ? source_->mono_samples.size() : 0;
        zoom_ = 1;
        repaint();
    }
    void setSelection(std::size_t begin, std::size_t end) {
        if (!source_ || source_->mono_samples.empty()) return;
        begin_ = std::min(begin, source_->mono_samples.size() - 1);
        end_ = std::clamp(end, begin_ + 1, source_->mono_samples.size());
        repaint();
    }
    mgstc::engine::SampleSelection selection() const { return {begin_, end_}; }
    void zoom(int direction) { zoom_ = std::clamp(zoom_ * (direction > 0 ? 2 : 0.5), 1.0, 64.0); repaint(); }
    void paint(juce::Graphics& g) override {
        fillRoundedPanelFrame(g, getLocalBounds());
        if (!source_ || end_ == 0) return;
        const auto [first, last] = visibleRange();
        const auto width = std::max(1, getWidth());
        const float middle = getHeight() * 0.5f;
        g.setColour(juce::Colour(0xFF9AA8B5));
        for (int x = 0; x < width; ++x) {
            const auto a = first + static_cast<std::size_t>((last - first) * static_cast<double>(x) / width);
            const auto b = std::min(last, first + static_cast<std::size_t>((last - first) * static_cast<double>(x + 1) / width) + 1);
            float low = 0, high = 0;
            // Bound display work independent of WAV duration.
            const auto stride = std::max<std::size_t>(1, (b - a) / 32);
            for (auto i = a; i < b; i += stride) {
                for (std::size_t channel = 0; channel < source_->channels; ++channel) {
                    const auto value = source_->interleaved_samples[i * source_->channels + channel];
                    low = std::min(low, value); high = std::max(high, value);
                }
            }
            g.drawVerticalLine(x, middle - std::min(1.0f, high) * middle,
                              middle - std::max(-1.0f, low) * middle);
        }
        const auto start_x = sampleX(begin_), end_x = sampleX(end_);
        g.setColour(juce::Colour(kUiHoverAccent).withAlpha(0.15f));
        g.fillRect(juce::Rectangle<float>(start_x, 0, end_x - start_x, static_cast<float>(getHeight())));
        g.setColour(juce::Colour(kUiHoverAccent));
        for (const auto x : {start_x, end_x}) {
            g.drawLine(x, 0, x, static_cast<float>(getHeight()), static_cast<float>(UiLayout::xs));
        }
        g.setFont(UiFonts::dense());
        g.drawText("Start", static_cast<int>(start_x), 0, UiScale::sx(70), UiLayout::fieldH, juce::Justification::left);
        g.drawText("End", static_cast<int>(end_x) - UiScale::sx(70), 0, UiScale::sx(70), UiLayout::fieldH, juce::Justification::right);
    }
    void mouseDown(const juce::MouseEvent& e) override {
        if (!source_) return;
        grabKeyboardFocus();
        active_start_ = std::abs(e.position.x - sampleX(begin_)) <= std::abs(e.position.x - sampleX(end_));
        dragging_ = true;
        mouseDrag(e);
    }
    void mouseDrag(const juce::MouseEvent& e) override {
        if (!dragging_ || !source_) return;
        const auto [first, last] = visibleRange();
        const auto at = first + static_cast<std::size_t>(juce::jlimit(0.0, 1.0,
            static_cast<double>(e.position.x) / std::max(1, getWidth())) * static_cast<double>(last - first));
        if (active_start_) begin_ = std::min(at, end_ - 1);
        else end_ = std::clamp(at, begin_ + 1, source_->mono_samples.size());
        repaint();
    }
    void mouseUp(const juce::MouseEvent&) override {
        if (dragging_ && changed) changed(active_start_);
        dragging_ = false;
    }
    bool keyPressed(const juce::KeyPress& key) override {
        if (!source_) return false;
        if (key == juce::KeyPress::tabKey) { active_start_ = !active_start_; repaint(); return true; }
        const auto code = key.getKeyCode();
        if (code != juce::KeyPress::leftKey && code != juce::KeyPress::rightKey) return false;
        const auto step = key.getModifiers().isShiftDown() ? std::max<std::size_t>(1, source_->sample_rate / 100) : 1;
        auto& position = active_start_ ? begin_ : end_;
        if (code == juce::KeyPress::leftKey) position = position > step ? position - step : 0;
        else position = std::min(position + step, source_->mono_samples.size());
        setSelection(begin_, end_);
        if (changed) changed(active_start_);
        return true;
    }
private:
    std::pair<std::size_t, std::size_t> visibleRange() const {
        const auto count = source_->mono_samples.size();
        const auto span = std::max<std::size_t>(1, static_cast<std::size_t>(count / zoom_));
        const auto centre = begin_ + (end_ - begin_) / 2;
        const auto first = std::min(centre > span / 2 ? centre - span / 2 : 0, count - span);
        return {first, first + span};
    }
    float sampleX(std::size_t sample) const {
        const auto [first, last] = visibleRange();
        return static_cast<float>((static_cast<double>(sample) - first) / (last - first) * getWidth());
    }
    std::shared_ptr<const mgstc::engine::SourcePcm> source_;
    std::size_t begin_{}, end_{};
    double zoom_{1};
    bool active_start_{true}, dragging_{};
};

class CompositeWavConversionContent final : public juce::Component {
    friend struct CompositeWavConversionTestAccess;
public:
    using ApplyCallback = std::function<void(const mgstc::engine::CompositeTimbre&, std::function<void()>)>;
    explicit CompositeWavConversionContent(ApplyCallback apply, PcmPreviewBoundary* pcm_preview = nullptr,
        BackgroundTaskBoundary* background_tasks = nullptr)
        : apply_callback_(std::move(apply)),
          player_(std::make_unique<CompositeWavPcmPlayer>(pcm_preview)),
          background_tasks_(background_tasks) {
        auto button = [this](juce::TextButton& control, const char* text, std::function<void()> action) {
            control.setButtonText(juce::String::fromUTF8(text)); control.onClick = std::move(action); addAndMakeVisible(control);
        };
        button(load_, "WAVを開く…", [this] { loadWave(); });
        button(original_, "原音 ▶", [this] { previewSource(); });
        button(converted_, "変換音 ▶", [this] { previewResult(); });
        button(stop_, "停止", [this] { stopPcm(); });
        button(ab_, "A/B ▶", [this] { if (ab_result_) previewResult(); else previewSource(); ab_result_ = !ab_result_; });
        button(zoom_in_, "＋", [this] { range_.zoom(1); });
        button(zoom_out_, "−", [this] { range_.zoom(-1); });
        button(convert_, "変換 / 再変換", [this] { convert(); });
        button(apply_, "適用", [this] {
            if (!result_ || !result_->composite_tone) return;
            stopPcm();
            const juce::Component::SafePointer<CompositeWavConversionContent> safe(this);
            apply_callback_(*result_->composite_tone, [safe] { if (safe) safe->close(); });
        });
        button(cancel_, "キャンセル", [this] { close(); });
        level_match_.setButtonText(juce::String::fromUTF8("比較音量を合わせる"));
        level_match_.setToggleState(true, juce::dontSendNotification);
        level_match_.setTooltip(juce::String::fromUTF8("試聴音量だけを調整します。生成音色・ENVは変更しません。"));
        addAndMakeVisible(level_match_);
        for (auto* field : {&begin_, &end_, &pitch_, &key_off_, &budget_}) {
            UiFonts::styleBodyField(*field); addAndMakeVisible(field);
        }
        pitch_.setText("auto", false); key_off_.setText("auto", false); budget_.setText("16", false);
        begin_.onReturnKey = begin_.onFocusLost = [this] { numericRange(true); };
        end_.onReturnKey = end_.onFocusLost = [this] { numericRange(false); };
        pitch_.onTextChange = [this] { analysis_.reset(); };
        configuration_.addItem(juce::String::fromUTF8("SCC単音"), 1);
        configuration_.addItem(juce::String::fromUTF8("SCC × SCC"), 2);
        configuration_.addItem(juce::String::fromUTF8("SCC × OPLL固定"), 3);
        configuration_.addItem(juce::String::fromUTF8("SCC × OPLLオリジナル"), 4);
        const char* strategies[] = {"自動（標準）", "基音＋残りの倍音", "低次倍音＋高次倍音", "アタック＋持続", "独立最適化"};
        for (int i = 0; i < 5; ++i) strategy_.addItem(juce::String::fromUTF8(strategies[i]), i + 1);
        fixed_tone_.addItem(juce::String::fromUTF8("音色自動選択"), 1);
        for (int i = 0; i < 15; ++i) fixed_tone_.addItem("OPLL @" + juce::String(i), i + 2);
        preference_.addItem(juce::String::fromUTF8("標準"), 1);
        preference_.addItem(juce::String::fromUTF8("品質優先"), 2);
        preference_.addItem(juce::String::fromUTF8("音色数優先"), 3);
        sustain_.addItem(juce::String::fromUTF8("サステイン自動"), 1);
        sustain_.addItem(juce::String::fromUTF8("ループなし"), 2);
        for (auto* combo : {&configuration_, &strategy_, &fixed_tone_, &preference_, &sustain_}) {
            combo->setSelectedId(1, juce::dontSendNotification); addAndMakeVisible(combo);
        }
        configuration_.onChange = [this] { refreshEnabled(); };
        metadata_.setFont(UiFonts::body()); metadata_.setJustificationType(juce::Justification::topLeft);
        addAndMakeVisible(metadata_);
        status_.setMultiLine(true); status_.setReadOnly(true); UiFonts::styleBodyField(status_); addAndMakeVisible(status_);
        const char* labels[] = {"開始サンプル / 秒", "終了サンプル / 秒（終端を含まない）", "基準音高 Hz（autoで推定）", "キーオフ秒（autoで推定）", "最大SCC音色数（1〜32）", "音源構成", "2ch分担", "OPLL固定音色", "最適化方針", "サステイン"};
        for (std::size_t i = 0; i < labels_.size(); ++i) {
            labels_[i].setText(juce::String::fromUTF8(labels[i]), juce::dontSendNotification);
            labels_[i].setFont(UiFonts::body()); addAndMakeVisible(labels_[i]);
        }
        range_.setTooltip(juce::String::fromUTF8("開始・終了ハンドルをドラッグ。左右キーで1サンプル、Shiftで10ms調整。"));
        range_.changed = [this](bool start) { analysis_.reset(); syncRange(); previewHandle(start); };
        addAndMakeVisible(range_);
        int order = 1;
        for (auto* c : std::initializer_list<juce::Component*>{&load_, &range_, &zoom_in_, &zoom_out_, &begin_, &end_,
                &pitch_, &key_off_, &configuration_, &strategy_, &fixed_tone_, &budget_, &preference_, &sustain_,
                &original_, &converted_, &stop_, &ab_, &level_match_, &convert_, &apply_, &cancel_}) c->setExplicitFocusOrder(order++);
        refreshEnabled();
        setSize(UiScale::sx(1080), UiScale::sx(900));
    }
    ~CompositeWavConversionContent() override {
        stopPcm();
        if (control_) control_->cancel_requested.store(true);
        // A registered plugin worker remains owned by its processor; deleting
        // the UI here cancels it without joining on editor close.
        if (background_tasks_) delete busy_dialog_.getComponent();
    }
    void paint(juce::Graphics& g) override { paintPageBackground(g, getLocalBounds()); }
    void resized() override {
        auto area = getLocalBounds().reduced(UiLayout::panelPad);
        auto top = area.removeFromTop(UiLayout::textButtonH);
        load_.setBounds(top.removeFromLeft(UiScale::sx(160))); top.removeFromLeft(UiLayout::controlGap); metadata_.setBounds(top);
        area.removeFromTop(UiLayout::sm); range_.setBounds(area.removeFromTop(UiScale::sx(150)));
        area.removeFromTop(UiLayout::sm);
        auto row = area.removeFromTop(UiLayout::textButtonH);
        for (auto* b : {&original_, &converted_, &stop_, &ab_}) {
            b->setBounds(row.removeFromLeft(UiScale::sx(130))); row.removeFromLeft(UiLayout::controlGap);
        }
        zoom_out_.setBounds(row.removeFromRight(UiLayout::iconButton)); row.removeFromRight(UiLayout::controlGap);
        zoom_in_.setBounds(row.removeFromRight(UiLayout::iconButton));
        area.removeFromTop(UiLayout::xs);
        level_match_.setBounds(area.removeFromTop(UiLayout::fieldH));
        area.removeFromTop(UiLayout::sm);
        std::array<juce::Component*, 10> controls{&begin_, &end_, &pitch_, &key_off_, &budget_,
            &configuration_, &strategy_, &fixed_tone_, &preference_, &sustain_};
        for (std::size_t i = 0; i < 5; ++i) {
            auto settings = area.removeFromTop(UiLayout::fieldH * 2);
            auto left = settings.removeFromLeft((settings.getWidth() - UiLayout::panelGap) / 2);
            settings.removeFromLeft(UiLayout::panelGap);
            labels_[i].setBounds(left.removeFromTop(UiLayout::fieldH)); controls[i]->setBounds(left);
            labels_[i + 5].setBounds(settings.removeFromTop(UiLayout::fieldH)); controls[i + 5]->setBounds(settings);
            area.removeFromTop(UiLayout::xs);
        }
        auto buttons = area.removeFromBottom(UiLayout::textButtonH);
        cancel_.setBounds(buttons.removeFromRight(UiScale::sx(130))); buttons.removeFromRight(UiLayout::controlGap);
        apply_.setBounds(buttons.removeFromRight(UiScale::sx(130))); buttons.removeFromRight(UiLayout::controlGap);
        convert_.setBounds(buttons.removeFromRight(UiScale::sx(190)));
        area.removeFromBottom(UiLayout::sm); status_.setBounds(area);
        for (auto& label : labels_) label.setFont(UiFonts::body()); metadata_.setFont(UiFonts::body());
    }
private:
    void stopPcm() { if (player_) player_->stop(); }
    void close() {
        stopPcm();
        if (background_tasks_) delete busy_dialog_.getComponent();
        if (auto* d = findParentComponentOfClass<juce::DialogWindow>()) d->exitModalState(0);
    }
    void refreshEnabled() {
        const bool loaded = source_ != nullptr;
        convert_.setEnabled(loaded); original_.setEnabled(loaded && player_ != nullptr);
        apply_.setEnabled(result_ && result_->composite_tone.has_value());
        converted_.setEnabled(player_ && result_pcm_ != nullptr); ab_.setEnabled(player_ && loaded && result_pcm_ != nullptr);
        stop_.setEnabled(player_ != nullptr); level_match_.setEnabled(player_ != nullptr);
        strategy_.setEnabled(configuration_.getSelectedId() != 1);
        fixed_tone_.setEnabled(configuration_.getSelectedId() == 3);
        if (configuration_.getSelectedId() == 1) strategy_.setSelectedId(1, juce::dontSendNotification);
    }
    void report(const juce::String& message) { status_.setText(message, false); }
    void syncRange() {
        const auto selection = range_.selection();
        begin_.setText(juce::String(static_cast<juce::int64>(selection.begin)), false);
        end_.setText(juce::String(static_cast<juce::int64>(selection.end)), false);
        if (source_) {
            labels_[0].setText(juce::String::fromUTF8("開始サンプル / ") + juce::String(static_cast<double>(selection.begin) / source_->sample_rate, 6) + " s", juce::dontSendNotification);
            labels_[1].setText(juce::String::fromUTF8("終了サンプル / ") + juce::String(static_cast<double>(selection.end) / source_->sample_rate, 6) + " s (exclusive)", juce::dontSendNotification);
        }
    }
    void numericRange(bool start) {
        if (!source_) return;
        const auto selection = range_.selection();
        const auto a = begin_.getText().getLargeIntValue(), b = end_.getText().getLargeIntValue();
        if (a < 0 || b <= a || static_cast<std::uint64_t>(b) > source_->mono_samples.size()) { syncRange(); return; }
        if (selection.begin == static_cast<std::size_t>(a) && selection.end == static_cast<std::size_t>(b)) return;
        range_.setSelection(static_cast<std::size_t>(a), static_cast<std::size_t>(b)); analysis_.reset(); syncRange(); previewHandle(start);
    }
    void play(std::shared_ptr<const mgstc::engine::SourcePcm> pcm, std::size_t begin, std::size_t end, float gain = 1.0f) {
        if (!player_) return;
        const auto error = player_->play(std::move(pcm), begin, end, gain); if (error.isNotEmpty()) report(error);
    }
    void previewSource() {
        if (!source_) return;
        const auto s = range_.selection();
        const bool match = level_match_.getToggleState() && result_ && result_->analysis_reference
            && s.begin == result_->analysis_reference->selection.begin && s.end == result_->analysis_reference->selection.end;
        play(source_, s.begin, s.end, match ? source_gain_ : 1.0f);
    }
    void previewResult() { if (result_pcm_) play(result_pcm_, 0, result_pcm_->mono_samples.size(), level_match_.getToggleState() ? result_gain_ : 1.0f); }
    void previewHandle(bool start) {
        if (!source_) return;
        const auto s = range_.selection(); const auto length = source_->sample_rate / 4;
        if (start) play(source_, s.begin, std::min(s.end, s.begin + length));
        else play(source_, s.end > length ? std::max(s.begin, s.end - length) : s.begin, s.end);
    }
    void loadWave() {
        juce::FileChooser chooser(juce::String::fromUTF8("WAVから総合音色へ変換"), {}, "*.wav;*.wave");
        if (!chooser.browseForFileToOpen()) return;
        stopPcm();
        const auto path = std::filesystem::path(chooser.getResult().getFullPathName().toWideCharPointer());
        struct Loaded { std::shared_ptr<const mgstc::engine::SourcePcm> pcm; std::string error; };
        const auto cancellation = std::make_shared<std::atomic<bool>>(false);
        auto progress = std::make_shared<ConversionProgressState>();
        progress->custom_cancel = [cancellation] { cancellation->store(true); };
        progress->custom_cancelled = [cancellation] { return cancellation->load(); };
        progress->custom_stage = [] { return juce::String::fromUTF8("WAV読み込み中"); };
        const juce::Component::SafePointer<CompositeWavConversionContent> safe(this);
        busy_dialog_ = runWithConversionBusyDialog(this, [path, cancellation](ConversionProgressState& progress) {
            Loaded loaded;
            if (progress.cancellationRequested()) return loaded;
            const auto bytes = mgstc::platform::readWaveFileBytes(path);
            if (!bytes) { loaded.error = "WAV file could not be read"; return loaded; }
            if (progress.cancellationRequested()) return loaded;
            auto pcm = std::make_shared<mgstc::engine::SourcePcm>();
            if (mgstc::engine::parseCompositeWavePcm(*bytes, *pcm, &loaded.error, cancellation)) loaded.pcm = std::move(pcm);
            return loaded;
        }, [safe, path](Loaded loaded, bool cancelled) {
            if (!safe || cancelled) return;
            if (!loaded.pcm) { safe->report(juce::String::fromUTF8(loaded.error.c_str())); return; }
            safe->source_ = std::move(loaded.pcm); safe->analysis_.reset(); safe->result_.reset(); safe->result_pcm_.reset();
            safe->range_.setSource(safe->source_); safe->syncRange(); safe->refreshEnabled();
            safe->metadata_.setText(juce::String(path.filename().wstring().c_str()) + " / " + juce::String(safe->source_->sample_rate) + " Hz / "
                + juce::String(safe->source_->channels) + " ch / " + juce::String(safe->source_->bit_depth) + " bit", juce::dontSendNotification);
            safe->report(juce::String::fromUTF8("範囲を選択し、変換してください。autoは基準音高とキーオフを推定します。"));
        }, progress, background_tasks_);
    }
    void convert();
    bool acceptCompletedConversion(mgstc::engine::CompositeWavConversionResult conversion,
        std::shared_ptr<const mgstc::engine::SourcePcm> pcm, bool cancelled,
        float source_gain, float result_gain) {
        if (cancelled || !pcm || !conversion.composite_tone
            || conversion.completion != mgstc::engine::CompositeWavConversionCompletion::Completed) return false;
        analysis_ = conversion.analysis_reference;
        result_pcm_ = std::move(pcm);
        source_gain_ = source_gain; result_gain_ = result_gain;
        result_ = std::move(conversion);
        refreshEnabled();
        return true;
    }
    ApplyCallback apply_callback_;
    std::unique_ptr<CompositeWavPcmPlayer> player_;
    BackgroundTaskBoundary* background_tasks_{};
    juce::Component::SafePointer<ConversionBusyDialog> busy_dialog_;
    CompositeWavRange range_;
    juce::TextButton load_, original_, converted_, stop_, ab_, zoom_in_, zoom_out_, convert_, apply_, cancel_;
    juce::TextEditor begin_, end_, pitch_, key_off_, budget_, status_;
    juce::ComboBox configuration_, strategy_, fixed_tone_, preference_, sustain_;
    juce::ToggleButton level_match_;
    juce::Label metadata_;
    std::array<juce::Label, 10> labels_;
    std::shared_ptr<const mgstc::engine::SourcePcm> source_, result_pcm_;
    std::shared_ptr<const mgstc::engine::SourceAnalysis> analysis_;
    std::optional<mgstc::engine::CompositeWavConversionResult> result_;
    std::shared_ptr<mgstc::engine::CompositeWavConversionControl> control_;
    bool ab_result_{};
    float source_gain_{1.0f}, result_gain_{1.0f};
};

inline void CompositeWavConversionContent::convert() {
    if (!source_) return;
    stopPcm();
    numericRange(true); numericRange(false); stopPcm();
    const auto selection = range_.selection();
    const auto max_waves = budget_.getText().getIntValue();
    const auto manual_pitch = pitch_.getText().trim();
    const auto manual_keyoff = key_off_.getText().trim();
    const bool pitch_auto = manual_pitch.equalsIgnoreCase("auto");
    const bool keyoff_auto = manual_keyoff.equalsIgnoreCase("auto");
    const auto frequency = manual_pitch.getDoubleValue(), keyoff = manual_keyoff.getDoubleValue();
    if (max_waves < 1 || max_waves > 32 || (!pitch_auto && (!std::isfinite(frequency) || frequency <= 0))
        || (!keyoff_auto && (!std::isfinite(keyoff) || keyoff < 0
            || keyoff * source_->sample_rate < static_cast<double>(selection.begin)
            || keyoff * source_->sample_rate > static_cast<double>(selection.end)))) {
        report(juce::String::fromUTF8("最大音色数、基準音高、キーオフの入力を確認してください。")); return;
    }
    control_ = std::make_shared<mgstc::engine::CompositeWavConversionControl>();
    auto options = mgstc::engine::CompositeWavConversionOptions{};
    options.configuration = static_cast<mgstc::engine::CompositeWavConfiguration>(configuration_.getSelectedId() - 1);
    options.strategy = static_cast<mgstc::engine::CompositeWavStrategy>(strategy_.getSelectedId() - 1);
    options.preference = static_cast<mgstc::engine::CompositeWavPreference>(preference_.getSelectedId() - 1);
    options.loop_mode = sustain_.getSelectedId() == 1 ? mgstc::engine::CompositeWavLoopMode::Automatic : mgstc::engine::CompositeWavLoopMode::None;
    options.max_scc_waveforms = static_cast<std::size_t>(max_waves);
    if (options.configuration == mgstc::engine::CompositeWavConfiguration::SccOpllRom && fixed_tone_.getSelectedId() > 1)
        options.fixed_opll_tone = static_cast<std::uint8_t>(fixed_tone_.getSelectedId() - 2);
    if (!keyoff_auto) options.key_off_position = static_cast<std::size_t>(std::llround(keyoff * source_->sample_rate));
    options.control = control_;
    mgstc::engine::SourceAnalysisOptions analysis_options;
    if (!pitch_auto) analysis_options.reference_pitch_hz = frequency;
    analysis_options.cancel_requested = std::shared_ptr<std::atomic<bool>>(control_, &control_->cancel_requested);
    const auto control = control_;
    const auto analysing = std::make_shared<std::atomic<bool>>(analysis_ == nullptr);
    auto progress = std::make_shared<ConversionProgressState>();
    progress->custom_cancel = [control] { control->cancel_requested.store(true, std::memory_order_release); };
    progress->custom_cancelled = [control] { return control->cancel_requested.load(std::memory_order_acquire); };
    progress->custom_progress = [control] {
        return control->stage.load() == mgstc::engine::CompositeWavStage::Completed ? 1.0
            : std::min(0.95, static_cast<double>(control->evaluations.load()) / 96.0);
    };
    progress->custom_stage = [control, analysing] {
        if (analysing->load()) return juce::String::fromUTF8("WAV・基本周波数・倍音解析中");
        switch (control->stage.load()) {
        case mgstc::engine::CompositeWavStage::Morph: return juce::String::fromUTF8("モーフィング生成中");
        case mgstc::engine::CompositeWavStage::Envelope: return juce::String::fromUTF8("ENV最適化中");
        case mgstc::engine::CompositeWavStage::Evaluation: return juce::String::fromUTF8("再現性評価中");
        default: return juce::String::fromUTF8("音源候補探索中");
        }
    };
    struct Completed {
        mgstc::engine::CompositeWavConversionResult conversion;
        std::shared_ptr<const mgstc::engine::SourcePcm> pcm;
        float source_gain{1.0f}, result_gain{1.0f};
    };
    const juce::Component::SafePointer<CompositeWavConversionContent> safe(this);
    busy_dialog_ = runWithConversionBusyDialog(this,
        [source = source_, analysis = analysis_, selection, analysis_options, options, analysing](ConversionProgressState&) mutable {
            Completed completed;
            if (!analysis) {
                const auto analysed = mgstc::engine::analyzeCompositeWaveSource(source, selection, analysis_options);
                if (!analysed.analysis) { completed.conversion.error = analysed.error; return completed; }
                analysis = analysed.analysis;
            }
            analysing->store(false);
            completed.conversion = mgstc::engine::convertCompositeWave(analysis, options);
            if (completed.conversion.completion == mgstc::engine::CompositeWavConversionCompletion::Completed
                && completed.conversion.composite_tone && completed.conversion.preview.ok()) {
                auto pcm = std::make_shared<mgstc::engine::SourcePcm>();
                pcm->sample_rate = 48000; pcm->channels = 2; pcm->bit_depth = 32;
                pcm->sample_format = mgstc::engine::SourceSampleFormat::FloatPcm;
                pcm->interleaved_samples = std::move(completed.conversion.preview.stereo_pcm);
                pcm->mono_samples = std::move(completed.conversion.preview.mono_pcm);
                double energy = 0.0, peak = 0.0;
                for (const auto sample : pcm->interleaved_samples) { energy += static_cast<double>(sample) * sample; peak = std::max(peak, std::abs(static_cast<double>(sample))); }
                const auto rms = std::sqrt(energy / std::max<std::size_t>(1, pcm->interleaved_samples.size()));
                const auto source_rms = analysis->metadata.rms;
                if (rms > 1e-12 && source_rms > 1e-12) {
                    const auto target = std::min({0.125, 0.9 * rms / std::max(1e-12, peak),
                        0.9 * source_rms / std::max(1e-12, analysis->metadata.peak)});
                    completed.source_gain = static_cast<float>(target / source_rms);
                    completed.result_gain = static_cast<float>(target / rms);
                }
                completed.pcm = std::move(pcm);
            }
            return completed;
        }, [safe](Completed completed, bool cancelled) {
            if (!safe) return;
            if (cancelled || !completed.pcm || !completed.conversion.composite_tone) {
                safe->report(cancelled ? juce::String::fromUTF8("変換をキャンセルしました。前回の結果を保持しています。")
                    : juce::String::fromUTF8(completed.conversion.error.c_str())
                        + juce::String::fromUTF8("\n変換できませんでした。前回の結果を保持しています。"));
                return;
            }
            if (!safe->acceptCompletedConversion(std::move(completed.conversion), std::move(completed.pcm),
                cancelled, completed.source_gain, completed.result_gain)) return;
            const auto& result = *safe->result_;
            const auto& q = result.quality;
            juce::String message = juce::String::fromUTF8("変換完了。原音と変換音を比較してから適用してください。\n")
                + "SCC: " + juce::String(static_cast<int>(result.resource_plan.scc_waveforms))
                + juce::String::fromUTF8(" 音色 / 評価 ") + juce::String(static_cast<int>(result.evaluations))
                + " / " + juce::String(result.elapsed_seconds, 2) + " s\n"
                + "STFT " + juce::String(q.multi_resolution_stft, 4) + " / Harmonic " + juce::String(q.harmonic, 4)
                + " / ERB " + juce::String(q.erb, 4) + " / Attack " + juce::String(q.attack, 4)
                + " / ENV " + juce::String(q.volume, 4);
            if (safe->analysis_) {
                const auto& analysis = *safe->analysis_;
                message += "\nF0 " + juce::String(analysis.reference_pitch_hz, 2) + " Hz / "
                    + juce::String::fromUTF8("信頼度 ") + juce::String(analysis.confidence, 2);
                if (analysis.estimated_key_off) message += juce::String::fromUTF8(" / キーオフ推定 ")
                    + juce::String(static_cast<double>(analysis.estimated_key_off->sample_position) / analysis.source->sample_rate, 4)
                    + " s / " + juce::String(analysis.estimated_key_off->confidence, 2);
                else message += juce::String::fromUTF8(" / キーオフ推定不可・手動指定可能");
                for (const auto& warning : analysis.warnings) message += "\n" + juce::String::fromUTF8(warning.c_str());
            }
            for (const auto& warning : result.warnings) message += "\n" + juce::String::fromUTF8(warning.c_str());
            safe->report(message); safe->refreshEnabled();
        }, progress, background_tasks_);
}
