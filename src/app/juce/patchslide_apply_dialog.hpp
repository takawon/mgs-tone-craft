// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>
#include "ui_fonts.hpp"
#include "ui_layout.hpp"

namespace mgstc::app {

struct PatchSlideApplyTarget {
    std::size_t layer_index{};
    juce::String label;
    bool replacing{};
};

// Content for the existing ModalDialogWindow. The result is a dense target
// number, not a layer index, so gaps between OPLL layers remain unambiguous.
class PatchSlideApplyContent final : public juce::Component,
                                    private juce::ComponentListener {
public:
    PatchSlideApplyContent(juce::String source, const std::vector<PatchSlideApplyTarget>& targets,
                          juce::Component* owner)
        : owner_(owner), targets_(targets) {
        name_.setText(source, juce::dontSendNotification);
        name_.setFont(UiFonts::body());
        name_.setJustificationType(juce::Justification::topLeft);
        for (std::size_t i = 0; i < targets.size(); ++i)
            target_.addItem(targets[i].label, static_cast<int>(i) + 1);
        target_.setTitle(juce::String::fromUTF8("適用先"));
        target_.setSelectedId(targets.empty() ? 0 : 1, juce::dontSendNotification);
        target_.shortcuts = [this](const juce::KeyPress& key) { return keyPressed(key); };
        target_.onChange = [this] { updateExplanation(); };
        explanation_.setFont(UiFonts::body());
        explanation_.setJustificationType(juce::Justification::topLeft);
        apply_.setButtonText(juce::String::fromUTF8("適用"));
        cancel_.setButtonText(juce::String::fromUTF8("キャンセル"));
        apply_.addShortcut(juce::KeyPress(juce::KeyPress::returnKey));
        cancel_.addShortcut(juce::KeyPress(juce::KeyPress::escapeKey));
        apply_.onClick = [this] { finish(target_.getSelectedId()); };
        cancel_.onClick = [this] { finish(0); };
        apply_.setEnabled(!targets.empty());
        target_.setExplicitFocusOrder(100);
        apply_.setExplicitFocusOrder(101);
        cancel_.setExplicitFocusOrder(102);
        for (auto* component : std::initializer_list<juce::Component*>{&name_, &target_, &explanation_, &apply_, &cancel_})
            addAndMakeVisible(component);
        if (owner_) owner_->addComponentListener(this);
        updateExplanation();
        setSize(UiScale::sx(640), UiScale::sx(300));
    }
    ~PatchSlideApplyContent() override { if (owner_) owner_->removeComponentListener(this); }
    void resized() override {
        auto area = getLocalBounds().reduced(UiLayout::panelPad);
        name_.setBounds(area.removeFromTop(UiLayout::fieldH * 2));
        area.removeFromTop(UiLayout::sm);
        target_.setBounds(area.removeFromTop(UiLayout::fieldH));
        area.removeFromTop(UiLayout::sm);
        auto buttons = area.removeFromBottom(UiLayout::textButtonH);
        cancel_.setBounds(buttons.removeFromRight(UiScale::sx(140)));
        buttons.removeFromRight(UiLayout::controlGap);
        apply_.setBounds(buttons.removeFromRight(UiScale::sx(100)));
        area.removeFromBottom(UiLayout::sm);
        explanation_.setBounds(area);
    }
    bool keyPressed(const juce::KeyPress& key) override {
        if (key == juce::KeyPress::escapeKey) { finish(0); return true; }
        if (key == juce::KeyPress::returnKey && apply_.isEnabled()) { finish(target_.getSelectedId()); return true; }
        return false;
    }
    [[nodiscard]] juce::ComboBox& targetControl() noexcept { return target_; }
    [[nodiscard]] juce::String sourceName() const { return name_.getText(); }
private:
    class TargetBox final : public juce::ComboBox {
    public:
        std::function<bool(const juce::KeyPress&)> shortcuts;
        bool keyPressed(const juce::KeyPress& key) override {
            if (shortcuts && shortcuts(key)) return true;
            return juce::ComboBox::keyPressed(key);
        }
    };
    void finish(int result) {
        if (auto* dialog = findParentComponentOfClass<juce::DialogWindow>()) dialog->exitModalState(result);
    }
    void componentBeingDeleted(juce::Component&) override { owner_ = nullptr; finish(0); }
    void componentVisibilityChanged(juce::Component& owner) override { if (!owner.isVisible()) finish(0); }
    void updateExplanation() {
        const int selected = target_.getSelectedId() - 1;
        explanation_.setText(selected >= 0 && selected < static_cast<int>(targets_.size())
            ? juce::String::fromUTF8(targets_[static_cast<std::size_t>(selected)].replacing
                ? "選択したOPLLレイヤーの音色・ENV・変調設定を置き換えます。他のレイヤーは保持します。"
                : "OPLLレイヤーを追加します。既存のレイヤーは保持します。")
            : juce::String{}, juce::dontSendNotification);
    }
    juce::Component::SafePointer<juce::Component> owner_;
    std::vector<PatchSlideApplyTarget> targets_;
    juce::Label name_, explanation_;
    TargetBox target_;
    juce::TextButton apply_, cancel_;
};

} // namespace mgstc::app
