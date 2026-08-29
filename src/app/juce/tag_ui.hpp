// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <algorithm>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include <juce_gui_extra/juce_gui_extra.h>

#include "mgstc/engine/timbre_tags.hpp"
#include "switch_look_and_feel.hpp"
#include "ui_chrome_constants.hpp"
#include "ui_fonts.hpp"
#include "ui_layout.hpp"
#include "ui_modal_dialog.hpp"
#include "ui_scale.hpp"
#include "juce_utf8.hpp"

struct TagChoice {
    std::string name;
    std::size_t count{};
};

[[nodiscard]] inline juce::String tagSelectionSummary(
    std::span<const std::string> tags,
    const juce::String& empty_text) {
    if (tags.empty()) {
        return empty_text;
    }
    auto text = juce::String::fromUTF8(tags.front().c_str());
    if (tags.size() > 1) {
        text += "  +"
            + juce::String(static_cast<int>(tags.size() - 1));
    }
    return text;
}

[[nodiscard]] inline int tagChipPreferredWidth(
    const juce::String& label,
    bool with_checkbox) {
    const auto font = UiFonts::body();
    const auto text_w = juce::GlyphArrangement::getStringWidthInt(
        font, label);
    const int leading = with_checkbox ? 36 : 12;
    return juce::jmax(56, leading + text_w + 12);
}

inline void layoutTagChipsFlow(
    juce::Component& host,
    juce::Array<juce::Component*>& chips,
    int available_width,
    int chip_height = 28,
    int gap = 6) {
    int x = 0;
    int y = 0;
    int row_h = 0;
    const int width = juce::jmax(1, available_width);
    for (auto* chip : chips) {
        if (chip == nullptr || !chip->isVisible()) {
            continue;
        }
        const int chip_w = chip->getProperties().contains("prefW")
            ? static_cast<int>(chip->getProperties()["prefW"])
            : 80;
        if (x > 0 && x + chip_w > width) {
            x = 0;
            y += row_h + gap;
            row_h = 0;
        }
        chip->setBounds(x, y, chip_w, chip_height);
        x += chip_w + gap;
        row_h = juce::jmax(row_h, chip_height);
    }
    host.setSize(width, y + (row_h > 0 ? row_h : chip_height));
}

class TagChipButton final : public juce::Component {
public:
    using Clicked = std::function<void()>;

    TagChipButton(
        juce::String label,
        bool checked,
        bool checkbox_style,
        Clicked on_click)
        : label_(std::move(label)),
          checked_(checked),
          checkbox_style_(checkbox_style),
          on_click_(std::move(on_click)) {
        const int pref = tagChipPreferredWidth(
            label_, checkbox_style_);
        getProperties().set("prefW", pref);
        setSize(pref, 28);
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
    }

    void setChecked(bool checked) {
        if (checked_ == checked) {
            return;
        }
        checked_ = checked;
        repaint();
    }

    [[nodiscard]] bool isChecked() const noexcept {
        return checked_;
    }

    void paint(juce::Graphics& graphics) override {
        auto bounds = getLocalBounds().toFloat().reduced(0.5F);
        graphics.setColour(
            checked_ ? juce::Colour(0xFF344454)
                     : juce::Colour(0xFF243040));
        graphics.fillRoundedRectangle(bounds, 4.0F);
        graphics.setColour(
            checked_ ? juce::Colour(0xFF2AD6C9)
                     : juce::Colour(0xFF607080));
        graphics.drawRoundedRectangle(bounds, 4.0F, 1.0F);

        int text_x = 10;
        if (checkbox_style_) {
            auto check = juce::Rectangle<float>(
                8.0F,
                (static_cast<float>(getHeight()) - 16.0F) * 0.5F,
                16.0F,
                16.0F);
            graphics.setColour(
                checked_ ? juce::Colour(0xFF2AD6C9)
                         : juce::Colour(0xFF68737E));
            graphics.drawRoundedRectangle(check, 2.0F, 1.5F);
            if (checked_) {
                graphics.drawLine(
                    check.getX() + 3.0F,
                    check.getCentreY(),
                    check.getX() + 6.5F,
                    check.getBottom() - 3.5F,
                    2.0F);
                graphics.drawLine(
                    check.getX() + 6.5F,
                    check.getBottom() - 3.5F,
                    check.getRight() - 3.0F,
                    check.getY() + 3.0F,
                    2.0F);
            }
            text_x = 30;
        }
        graphics.setColour(juce::Colour(0xFFE6EDF3));
        graphics.setFont(UiFonts::body());
        graphics.drawText(
            label_,
            text_x,
            0,
            getWidth() - text_x - 8,
            getHeight(),
            juce::Justification::centredLeft,
            true);
    }

    void mouseUp(const juce::MouseEvent& event) override {
        if (event.mouseWasClicked() && on_click_) {
            on_click_();
        }
    }

private:
    juce::String label_;
    bool checked_{};
    bool checkbox_style_{true};
    Clicked on_click_;
};

// Library-manager detail: assigned tag chip with a trailing × to remove.
class RemovableDetailTagChip final : public juce::Component {
public:
    using RemoveClicked = std::function<void()>;

    RemovableDetailTagChip(juce::String label, RemoveClicked on_remove)
        : label_(std::move(label)),
          on_remove_(std::move(on_remove)) {
        const int pref = tagChipPreferredWidth(label_, false) + 18;
        getProperties().set("prefW", pref);
        setSize(pref, 28);
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
    }

    void paint(juce::Graphics& graphics) override {
        auto bounds = getLocalBounds().toFloat().reduced(0.5F);
        graphics.setColour(juce::Colour(0xFF243040));
        graphics.fillRoundedRectangle(bounds, 4.0F);
        graphics.setColour(juce::Colour(0xFF607080));
        graphics.drawRoundedRectangle(bounds, 4.0F, 1.0F);

        const auto remove_area = removeHitArea();
        const int text_w = juce::jmax(
            0, getWidth() - remove_area.getWidth() - 12);
        graphics.setColour(juce::Colour(0xFFE6EDF3));
        graphics.setFont(UiFonts::body());
        graphics.drawText(
            label_,
            10,
            0,
            text_w,
            getHeight(),
            juce::Justification::centredLeft,
            true);

        const bool over_remove =
            remove_area.contains(getMouseXYRelative());
        graphics.setColour(
            over_remove ? juce::Colour(kUiHoverAccent)
                        : juce::Colour(0xFFB9C6D2));
        graphics.setFont(UiFonts::body());
        graphics.drawText(
            juce::String::fromUTF8("\xc3\x97"),
            remove_area,
            juce::Justification::centred,
            false);
    }

    void mouseMove(const juce::MouseEvent&) override { repaint(); }
    void mouseExit(const juce::MouseEvent&) override { repaint(); }

    void mouseUp(const juce::MouseEvent& event) override {
        if (event.mouseWasClicked()
            && removeHitArea().contains(event.getPosition())
            && on_remove_) {
            on_remove_();
        }
    }

private:
    [[nodiscard]] juce::Rectangle<int> removeHitArea() const {
        return getLocalBounds().removeFromRight(22);
    }

    juce::String label_;
    RemoveClicked on_remove_;
};

class TagSelectionContent final : public juce::Component {
public:
    using SelectionCallback =
        std::function<void(std::vector<std::string>)>;

    TagSelectionContent(
        std::vector<TagChoice> available,
        std::vector<std::string> selected,
        bool allow_custom,
        SelectionCallback callback)
        : available_(std::move(available)),
          selected_(std::move(selected)),
          allow_custom_(allow_custom),
          callback_(std::move(callback)) {
        filter_.setTextToShowWhenEmpty(
            juce::String::fromUTF8("タグ候補を検索"),
            juce::Colour(0xFF7F8993));
        filter_.onTextChange = [this] { rebuildFilteredChips(); };
        UiFonts::styleBodyField(filter_);
        addAndMakeVisible(filter_);

        viewport_.setViewedComponent(&chip_host_, false);
        viewport_.setScrollBarsShown(true, false);
        addAndMakeVisible(viewport_);

        custom_.setTextToShowWhenEmpty(
            juce::String::fromUTF8(
                "独自タグ（カンマ区切りで複数追加可）"),
            juce::Colour(0xFF7F8993));
        custom_.onReturnKey = [this] { addCustomTags(); };
        UiFonts::styleBodyField(custom_);
        addAndMakeVisible(custom_);
        custom_.setVisible(allow_custom_);

        add_.setButtonText(juce::String::fromUTF8("追加"));
        add_.onClick = [this] { addCustomTags(); };
        addAndMakeVisible(add_);
        add_.setVisible(allow_custom_);

        clear_.setButtonText(juce::String::fromUTF8("すべて解除"));
        clear_.onClick = [this] {
            selected_.clear();
            refreshChipCheckedState();
        };
        addAndMakeVisible(clear_);

        ok_.setButtonText(juce::String::fromUTF8("決定"));
        ok_.onClick = [this] {
            // 「追加」前に決定しても入力中の独自タグを取り込む。
            addCustomTags();
            // 親（ライブラリ管理など）が前面に戻ってから反映する。
            auto callback = callback_;
            auto selected = selected_;
            closeDialog(1);
            if (callback) {
                juce::MessageManager::callAsync(
                    [callback = std::move(callback),
                     selected = std::move(selected)]() mutable {
                        callback(std::move(selected));
                    });
            }
        };
        addAndMakeVisible(ok_);

        cancel_.setButtonText(juce::String::fromUTF8("キャンセル"));
        cancel_.onClick = [this] { closeDialog(0); };
        addAndMakeVisible(cancel_);

        rebuildFilteredChips();
        setSize(UiScale::sx(560), UiScale::sx(560));
    }

    void paint(juce::Graphics& graphics) override {
        UiScale::forceGlobalForNonEditorUi();
        juce::Component::paint(graphics);
    }

    void resized() override {
        UiScale::forceGlobalForNonEditorUi();
        auto area = getLocalBounds().reduced(16);
        filter_.setBounds(area.removeFromTop(32));
        area.removeFromTop(8);
        auto buttons = area.removeFromBottom(34);
        clear_.setBounds(buttons.removeFromLeft(100));
        buttons.removeFromLeft(8);
        cancel_.setBounds(buttons.removeFromRight(100));
        buttons.removeFromRight(8);
        ok_.setBounds(buttons.removeFromRight(100));
        area.removeFromBottom(8);
        if (allow_custom_) {
            auto custom_row = area.removeFromBottom(32);
            add_.setBounds(custom_row.removeFromRight(72));
            custom_row.removeFromRight(8);
            custom_.setBounds(custom_row);
            area.removeFromBottom(8);
        }
        viewport_.setBounds(area);
        layoutChips();
    }

private:
    [[nodiscard]] bool isSelected(std::string_view name) const {
        const std::array<std::string, 1> required{
            std::string(name)};
        return mgstc::engine::containsAllTimbreTags(
            selected_, required);
    }

    void toggleTag(const std::string& name) {
        if (isSelected(name)) {
            std::erase_if(
                selected_,
                [&name](const std::string& selected) {
                    return juce::String::fromUTF8(selected.c_str())
                        .equalsIgnoreCase(
                            juce::String::fromUTF8(name.c_str()));
                });
        } else {
            selected_.push_back(name);
        }
        selected_ = mgstc::engine::parseTimbreTags(
            mgstc::engine::serializeTimbreTags(selected_));
        refreshChipCheckedState();
    }

    void addCustomTags() {
        if (!allow_custom_) {
            return;
        }
        auto additions = mgstc::engine::parseTimbreTags(
            utf8String(custom_.getText()));
        if (additions.empty()) {
            return;
        }
        selected_.insert(
            selected_.end(), additions.begin(), additions.end());
        selected_ = mgstc::engine::parseTimbreTags(
            mgstc::engine::serializeTimbreTags(selected_));
        for (const auto& tag : additions) {
            const auto exists = std::any_of(
                available_.begin(), available_.end(),
                [&tag](const TagChoice& choice) {
                    return juce::String::fromUTF8(choice.name.c_str())
                        .equalsIgnoreCase(
                            juce::String::fromUTF8(tag.c_str()));
                });
            if (!exists) {
                available_.push_back({tag, 0});
            }
        }
        custom_.clear();
        rebuildFilteredChips();
    }

    void rebuildFilteredChips() {
        chip_host_.removeAllChildren();
        chips_.clear();
        const auto filter = filter_.getText().trim();
        for (std::size_t index = 0; index < available_.size(); ++index) {
            const auto& choice = available_[index];
            auto label = juce::String::fromUTF8(choice.name.c_str());
            if (!(filter.isEmpty()
                  || label.containsIgnoreCase(filter))) {
                continue;
            }
            if (choice.count != 0) {
                label += "  ("
                    + juce::String(static_cast<int>(choice.count))
                    + ")";
            }
            const auto name = choice.name;
            auto chip = std::make_unique<TagChipButton>(
                label,
                isSelected(name),
                true,
                [this, name] { toggleTag(name); });
            chip_host_.addAndMakeVisible(*chip);
            chips_.push_back(std::move(chip));
        }
        layoutChips();
    }

    void refreshChipCheckedState() {
        rebuildFilteredChips();
    }

    void layoutChips() {
        juce::Array<juce::Component*> views;
        for (auto& chip : chips_) {
            views.add(chip.get());
        }
        const int view_w = juce::jmax(
            1,
            viewport_.getWidth()
                - (viewport_.isVerticalScrollBarShown()
                       ? viewport_.getScrollBarThickness()
                       : 0));
        layoutTagChipsFlow(chip_host_, views, view_w);
        viewport_.setViewPosition(viewport_.getViewPosition());
    }

    void closeDialog(int result) {
        if (auto* dialog =
                findParentComponentOfClass<juce::DialogWindow>()) {
            dialog->exitModalState(result);
        }
    }

    std::vector<TagChoice> available_;
    std::vector<std::string> selected_;
    bool allow_custom_{};
    SelectionCallback callback_;
    juce::TextEditor filter_;
    juce::Viewport viewport_;
    juce::Component chip_host_;
    std::vector<std::unique_ptr<TagChipButton>> chips_;
    juce::TextEditor custom_;
    juce::TextButton add_;
    juce::TextButton clear_;
    juce::TextButton ok_;
    juce::TextButton cancel_;
};

inline void showTagSelectionDialog(
    juce::Component* anchor,
    const juce::String& title,
    std::vector<TagChoice> available,
    std::vector<std::string> selected,
    bool allow_custom,
    TagSelectionContent::SelectionCallback callback) {
    UiScale::forceGlobalForNonEditorUi();
    auto* dialog = new ModalDialogWindow(
        title, juce::Colour(0xFF1B222C));
    auto* content = new TagSelectionContent(
        std::move(available),
        std::move(selected),
        allow_custom,
        std::move(callback));
    dialog->setContentOwned(content, true);
    dialog->setResizable(false, false);
    if (anchor != nullptr) {
        dialog->centreAroundComponent(
            anchor, content->getWidth(), content->getHeight() + 32);
    } else {
        dialog->centreWithSize(
            content->getWidth(), content->getHeight() + 32);
    }
    dialog->enterModalState(true, nullptr, true);
}

class TagManagementContent final : public juce::Component {
public:
    using ApplyCallback =
        std::function<void(std::string, std::string)>;
    using ChangedCallback = std::function<void()>;

    TagManagementContent(
        std::vector<TagChoice> custom_tags,
        ApplyCallback callback,
        bool embedded = false,
        ChangedCallback on_changed = {})
        : custom_tags_(std::move(custom_tags)),
          callback_(std::move(callback)),
          embedded_(embedded),
          on_changed_(std::move(on_changed)) {
        source_label_.setText(
            juce::String::fromUTF8("管理する独自タグ"),
            juce::dontSendNotification);
        source_label_.setFont(UiFonts::body());
        addAndMakeVisible(source_label_);

        viewport_.setViewedComponent(&chip_host_, false);
        viewport_.setScrollBarsShown(true, false);
        addAndMakeVisible(viewport_);
        rebuildSourceChips();

        replacement_label_.setText(
            juce::String::fromUTF8("変更先タグ"),
            juce::dontSendNotification);
        replacement_label_.setFont(UiFonts::body());
        addAndMakeVisible(replacement_label_);
        replacement_.setTextToShowWhenEmpty(
            juce::String::fromUTF8(
                "新しい名前、既存タグ名、または標準タグ名"),
            juce::Colour(0xFF7F8993));
        replacement_.onTextChange = [this] { updateState(); };
        UiFonts::styleBodyField(replacement_);
        addAndMakeVisible(replacement_);

        impact_.setColour(
            juce::Label::textColourId, juce::Colour(0xFFB9C6D2));
        impact_.setJustificationType(juce::Justification::centredLeft);
        impact_.setFont(UiFonts::body());
        addAndMakeVisible(impact_);

        apply_.setButtonText(
            juce::String::fromUTF8("名前変更／統合"));
        apply_.onClick = [this] {
            if (const auto selected = selectedTag();
                selected && replacement_.getText().trim().isNotEmpty()) {
                if (callback_) {
                    callback_(
                        *selected,
                        utf8String(replacement_.getText().trim()));
                }
                finishAction();
            }
        };
        addAndMakeVisible(apply_);

        remove_.setButtonText(juce::String::fromUTF8("削除"));
        remove_.onClick = [this] { confirmDelete(); };
        addAndMakeVisible(remove_);

        cancel_.setButtonText(
            embedded_
                ? juce::String::fromUTF8("選択解除")
                : juce::String::fromUTF8("キャンセル"));
        cancel_.onClick = [this] {
            if (embedded_) {
                selected_index_ = -1;
                replacement_.clear();
                rebuildSourceChips();
                updateState();
            } else {
                closeDialog(0);
            }
        };
        addAndMakeVisible(cancel_);

        updateState();
        setSize(UiScale::sx(560), UiScale::sx(embedded_ ? 460 : 420));
    }

    void setCustomTags(std::vector<TagChoice> custom_tags) {
        custom_tags_ = std::move(custom_tags);
        selected_index_ = -1;
        replacement_.clear();
        rebuildSourceChips();
        updateState();
    }

    void paint(juce::Graphics& graphics) override {
        UiScale::forceGlobalForNonEditorUi();
        juce::Component::paint(graphics);
    }

    void resized() override {
        UiScale::forceGlobalForNonEditorUi();
        constexpr int pad = 10;
        constexpr int title_h = 30;
        constexpr int gap_xs = 4;
        constexpr int gap_sm = 8;
        constexpr int gap_lg = 16;
        constexpr int gap_xl = 24;
        constexpr int btn_h = 36;
        constexpr int btn_min_w = 80;
        constexpr int field_h = 34;
        auto area = getLocalBounds().reduced(pad);
        source_label_.setBounds(area.removeFromTop(title_h - gap_xs));
        area.removeFromTop(gap_xs);
        auto buttons = area.removeFromBottom(btn_h);
        cancel_.setBounds(buttons.removeFromRight(btn_min_w + gap_lg));
        buttons.removeFromRight(6);
        remove_.setBounds(buttons.removeFromRight(btn_min_w + gap_xs));
        buttons.removeFromRight(6);
        apply_.setBounds(buttons.removeFromRight(btn_min_w + gap_xl));
        area.removeFromBottom(gap_sm);
        impact_.setBounds(area.removeFromBottom(title_h - gap_xs));
        area.removeFromBottom(gap_sm);
        replacement_.setBounds(area.removeFromBottom(field_h));
        area.removeFromBottom(gap_xs);
        replacement_label_.setBounds(area.removeFromBottom(title_h - gap_xs));
        area.removeFromBottom(gap_sm);
        viewport_.setBounds(area);
        layoutSourceChips();
    }

private:
    [[nodiscard]] std::optional<std::string> selectedTag() const {
        if (selected_index_ < 0
            || static_cast<std::size_t>(selected_index_)
                >= custom_tags_.size()) {
            return std::nullopt;
        }
        return custom_tags_[
            static_cast<std::size_t>(selected_index_)].name;
    }

    void selectIndex(int index) {
        selected_index_ = index;
        rebuildSourceChips();
        updateState();
    }

    void rebuildSourceChips() {
        chip_host_.removeAllChildren();
        chips_.clear();
        for (std::size_t index = 0; index < custom_tags_.size();
             ++index) {
            const auto& tag = custom_tags_[index];
            auto label = juce::String::fromUTF8(tag.name.c_str())
                + "  ("
                + juce::String(static_cast<int>(tag.count))
                + ")";
            const int captured = static_cast<int>(index);
            auto chip = std::make_unique<TagChipButton>(
                label,
                selected_index_ == captured,
                false,
                [this, captured] { selectIndex(captured); });
            chip_host_.addAndMakeVisible(*chip);
            chips_.push_back(std::move(chip));
        }
        layoutSourceChips();
    }

    void layoutSourceChips() {
        juce::Array<juce::Component*> views;
        for (auto& chip : chips_) {
            views.add(chip.get());
        }
        const int view_w = juce::jmax(
            1,
            viewport_.getWidth()
                - (viewport_.isVerticalScrollBarShown()
                       ? viewport_.getScrollBarThickness()
                       : 0));
        layoutTagChipsFlow(chip_host_, views, view_w);
    }

    void updateState() {
        const bool valid = selectedTag().has_value();
        const auto count = valid
            ? custom_tags_[static_cast<std::size_t>(selected_index_)]
                  .count
            : 0;
        impact_.setText(
            valid
                ? juce::String::fromUTF8("使用中の音色: ")
                    + juce::String(static_cast<int>(count))
                : juce::String::fromUTF8(
                      "独自タグを選択してください"),
            juce::dontSendNotification);
        apply_.setEnabled(
            valid && replacement_.getText().trim().isNotEmpty());
        remove_.setEnabled(valid);
    }

    void finishAction() {
        if (embedded_) {
            selected_index_ = -1;
            replacement_.clear();
            if (on_changed_) {
                on_changed_();
            }
            updateState();
            return;
        }
        closeDialog(1);
    }

    void confirmDelete() {
        const auto selected = selectedTag();
        if (!selected) {
            return;
        }
        juce::Component::SafePointer<TagManagementContent> safe(this);
        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions()
                .withIconType(
                    juce::MessageBoxIconType::WarningIcon)
                .withTitle(
                    juce::String::fromUTF8("独自タグの削除"))
                .withMessage(
                    juce::String::fromUTF8("「")
                    + juce::String::fromUTF8(selected->c_str())
                    + juce::String::fromUTF8(
                        "」をすべての音色から削除しますか？"))
                .withButton(juce::String::fromUTF8("削除"))
                .withButton(
                    juce::String::fromUTF8("キャンセル"))
                .withAssociatedComponent(this),
            [safe, selected](int result) {
                if (safe == nullptr || result != 1) {
                    return;
                }
                if (safe->callback_) {
                    safe->callback_(*selected, {});
                }
                safe->finishAction();
            });
    }

    void closeDialog(int result) {
        if (auto* dialog =
                findParentComponentOfClass<juce::DialogWindow>()) {
            dialog->exitModalState(result);
        }
    }

    std::vector<TagChoice> custom_tags_;
    ApplyCallback callback_;
    ChangedCallback on_changed_;
    bool embedded_{};
    int selected_index_{-1};
    juce::Label source_label_;
    juce::Viewport viewport_;
    juce::Component chip_host_;
    std::vector<std::unique_ptr<TagChipButton>> chips_;
    juce::Label replacement_label_;
    juce::TextEditor replacement_;
    juce::Label impact_;
    juce::TextButton apply_;
    juce::TextButton remove_;
    juce::TextButton cancel_;
};

inline void showTagManagementDialog(
    juce::Component* anchor,
    std::vector<TagChoice> custom_tags,
    TagManagementContent::ApplyCallback callback) {
    UiScale::forceGlobalForNonEditorUi();
    auto* dialog = new ModalDialogWindow(
        juce::String::fromUTF8("独自タグ管理"),
        juce::Colour(0xFF1B222C));
    auto* content = new TagManagementContent(
        std::move(custom_tags), std::move(callback));
    dialog->setContentOwned(content, true);
    dialog->setResizable(false, false);
    if (anchor != nullptr) {
        dialog->centreAroundComponent(
            anchor, content->getWidth(), content->getHeight() + 32);
    } else {
        dialog->centreWithSize(
            content->getWidth(), content->getHeight() + 32);
    }
    dialog->enterModalState(true, nullptr, true);
}

[[nodiscard]] inline std::vector<TagChoice> presetTagChoices(
    std::span<const std::string> selected) {
    std::vector<TagChoice> choices;
    for (const auto tag : mgstc::engine::presetTimbreTags()) {
        choices.push_back({std::string(tag), 0});
    }
    for (const auto& tag : selected) {
        const auto exists = std::any_of(
            choices.begin(), choices.end(),
            [&tag](const TagChoice& choice) {
                return juce::String::fromUTF8(choice.name.c_str())
                    .equalsIgnoreCase(
                        juce::String::fromUTF8(tag.c_str()));
            });
        if (!exists) {
            choices.push_back({tag, 0});
        }
    }
    return choices;
}

[[nodiscard]] inline std::vector<TagChoice> usageTagChoices(
    std::span<const std::vector<std::string>> tag_sets) {
    std::vector<TagChoice> choices;
    for (auto usage :
         mgstc::engine::collectTimbreTagUsage(tag_sets)) {
        choices.push_back({std::move(usage.name), usage.count});
    }
    return choices;
}

[[nodiscard]] inline std::vector<TagChoice> mergeTagChoices(
    std::vector<TagChoice> base,
    std::span<const TagChoice> extra) {
    for (const auto& tag : extra) {
        const auto found = std::find_if(
            base.begin(),
            base.end(),
            [&tag](const TagChoice& choice) {
                return juce::String::fromUTF8(choice.name.c_str())
                    .equalsIgnoreCase(
                        juce::String::fromUTF8(tag.name.c_str()));
            });
        if (found == base.end()) {
            base.push_back(tag);
        } else if (tag.count > found->count) {
            found->count = tag.count;
        }
    }
    return base;
}

// 編集ダイアログ用: 標準78 + ライブラリ既存独自タグ + 現在選択中タグ。
[[nodiscard]] inline std::vector<TagChoice> editorTagChoices(
    std::span<const std::vector<std::string>> library_tag_sets,
    std::span<const std::string> selected) {
    return mergeTagChoices(
        presetTagChoices(selected),
        usageTagChoices(library_tag_sets));
}

// 検索ダイアログ用: 使用中タグ + 現在の絞り込み選択（未使用になっても表示を維持）。
[[nodiscard]] inline std::vector<TagChoice> filterTagChoices(
    std::span<const std::vector<std::string>> library_tag_sets,
    std::span<const std::string> selected) {
    std::vector<TagChoice> selected_choices;
    selected_choices.reserve(selected.size());
    for (const auto& tag : selected) {
        selected_choices.push_back({tag, 0});
    }
    return mergeTagChoices(
        usageTagChoices(library_tag_sets), selected_choices);
}
