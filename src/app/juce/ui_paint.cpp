// SPDX-License-Identifier: AGPL-3.0-only

#include "ui_paint.hpp"

// Fit a top-level editor window entirely inside the target monitor work
// area (taskbar excluded). Size/position are never written to settings INI.
//
// On Windows with a native title bar, DocumentWindow / HWNDComponentPeer
// getBounds() is the *client* rectangle (GetClientRect). The OS frame
// (caption + borders) is reported separately by peer->getFrameSize().
// Clamping client bounds to userBounds leaves the framed HWND extending
// into the taskbar — same model as ComponentBoundsConstrainer and
// ResizableWindow::restoreWindowStateFromString (inflate → clamp → deflate).
void clampWindowToDisplayWorkArea(juce::ResizableWindow& window) {
    auto bounds = window.getBounds();
    const auto& displays = juce::Desktop::getInstance().getDisplays();

    juce::BorderSize<int> frame;
    if (auto* peer = window.getPeer()) {
        if (const auto frame_size = peer->getFrameSizeIfPresent()) {
            frame = *frame_size;
        }
    }

    const auto outer_for_display = frame.addedTo(bounds);
    const juce::Displays::Display* display = nullptr;
    if (!outer_for_display.isEmpty()) {
        display = displays.getDisplayForRect(outer_for_display);
    }
    if (display == nullptr) {
        display = displays.getDisplayForPoint(
            juce::Desktop::getMousePosition().toFloat());
    }
    if (display == nullptr) {
        display = displays.getPrimaryDisplay();
    }
    if (display == nullptr) {
        return;
    }

    const auto work = display->userBounds.toNearestIntEdges();
    if (work.getWidth() <= 0 || work.getHeight() <= 0) {
        return;
    }

    // Max *content* size so the framed outer window still fits in work.
    const int max_content_w = juce::jmax(
        1, work.getWidth() - frame.getLeftAndRight());
    const int max_content_h = juce::jmax(
        1, work.getHeight() - frame.getTopAndBottom());

    // If a prior min size exceeds the work area (small laptop / 150% DPI),
    // lower the floor so the window can still open fully on-screen.
    if (auto* constrainer = window.getConstrainer()) {
        constexpr int kNoMax = 0x3fffffff;
        const int max_w = constrainer->getMaximumWidth() > 0
            ? constrainer->getMaximumWidth()
            : kNoMax;
        const int max_h = constrainer->getMaximumHeight() > 0
            ? constrainer->getMaximumHeight()
            : kNoMax;
        const int min_w = juce::jmin(
            juce::jmax(1, constrainer->getMinimumWidth()),
            max_content_w);
        const int min_h = juce::jmin(
            juce::jmax(1, constrainer->getMinimumHeight()),
            max_content_h);
        constrainer->setSizeLimits(min_w, min_h, max_w, max_h);
    }

    auto outer = frame.addedTo(bounds);
    outer.setWidth(juce::jmin(outer.getWidth(), work.getWidth()));
    outer.setHeight(juce::jmin(outer.getHeight(), work.getHeight()));

    if (!outer.intersects(work) || bounds.isEmpty()) {
        outer.setCentre(work.getCentre());
    } else {
        if (outer.getX() < work.getX()) {
            outer.setX(work.getX());
        }
        if (outer.getY() < work.getY()) {
            outer.setY(work.getY());
        }
        if (outer.getRight() > work.getRight()) {
            outer.setX(work.getRight() - outer.getWidth());
        }
        if (outer.getBottom() > work.getBottom()) {
            outer.setY(work.getBottom() - outer.getHeight());
        }
    }

    auto clamped = frame.subtractedFrom(outer);
    clamped.setWidth(juce::jmax(1, clamped.getWidth()));
    clamped.setHeight(juce::jmax(1, clamped.getHeight()));

    if (clamped != window.getBounds()) {
        window.setBoundsConstrained(clamped);
    }
}

void applyScaledContentSize(
    juce::Component& content,
    int preferred_w,
    int preferred_h,
    bool clamp_host_window) {
    content.setSize(preferred_w, preferred_h);
    content.sendLookAndFeelChange();
    content.resized();
    content.repaint();
    if (!clamp_host_window) {
        return;
    }
    if (auto* top = content.getTopLevelComponent()) {
        if (auto* window = dynamic_cast<juce::ResizableWindow*>(top)) {
            // Preferred size may exceed the work area; clamp keeps chrome visible.
            window->setSize(
                juce::jmax(preferred_w, window->getWidth()),
                juce::jmax(preferred_h, window->getHeight()));
            // Always set to preferred first so shrink works too.
            window->setSize(preferred_w, preferred_h);
            clampWindowToDisplayWorkArea(*window);
        }
    }
}

void layoutEditorTopRightChrome(
    int host_width,
    juce::DrawableButton& settings,
    juce::Slider& master_volume,
    juce::DrawableButton* immediate_audition) {
    using namespace UiLayout;
    const int top = pageMargin;
    int right = host_width - pageMargin;
    settings.setBounds(right - iconButton, top, iconButton, iconButton);
    right -= iconButton + controlGap;
    const int vol_top = juce::jmax(
        0, top - (masterVolumeSize - iconButton) / 2);
    master_volume.setBounds(
        right - masterVolumeSize,
        vol_top,
        masterVolumeSize,
        masterVolumeSize);
    right -= masterVolumeSize + controlGap;
    if (immediate_audition != nullptr) {
        immediate_audition->setBounds(
            right - iconButton, top, iconButton, iconButton);
    }
}

void paintPageBackground(
    juce::Graphics& graphics,
    juce::Rectangle<int> bounds) {
    juce::ColourGradient grad(
        juce::Colour(UiLayout::pageFill),
        static_cast<float>(bounds.getCentreX()),
        static_cast<float>(bounds.getY()),
        juce::Colour(UiLayout::pageFillBottom),
        static_cast<float>(bounds.getCentreX()),
        static_cast<float>(bounds.getBottom()),
        false);
    graphics.setGradientFill(grad);
    graphics.fillRect(bounds);
}

void fillRoundedPanelFrame(
    juce::Graphics& graphics,
    juce::Rectangle<int> bounds) {
    if (bounds.isEmpty()) {
        return;
    }
    graphics.setColour(juce::Colour(UiLayout::panelFill));
    graphics.fillRoundedRectangle(
        bounds.toFloat(),
        static_cast<float>(UiLayout::panelRadius));
}

void strokeRoundedPanelFrame(
    juce::Graphics& graphics,
    juce::Rectangle<int> bounds) {
    if (bounds.isEmpty()) {
        return;
    }
    graphics.setColour(juce::Colour(UiLayout::panelStroke));
    graphics.drawRoundedRectangle(
        bounds.toFloat(),
        static_cast<float>(UiLayout::panelRadius),
        1.0F);
}

void paintRoundedPanelFrame(
    juce::Graphics& graphics,
    juce::Rectangle<int> bounds) {
    fillRoundedPanelFrame(graphics, bounds);
    strokeRoundedPanelFrame(graphics, bounds);
}
