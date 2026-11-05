// SPDX-License-Identifier: AGPL-3.0-only
#include "editor_header_layout.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>

int main() {
    juce::ScopedJuceInitialiser_GUI gui;
    try {
        int cases = 0;
        for (int scale : UiScale::kSteps) {
            UiScale::setActivePercent(scale);
            for (int width : {mgstc::app::minimumEditorHeaderWidth(), 640, 700, 960, 1600}) {
                for (bool navigation : {false, true}) {
                    const auto layout = mgstc::app::calculateEditorHeaderLayout(width,
                        UiScale::sx(600), navigation ? UiScale::sx(126) : 0,
                        navigation ? UiScale::sx(114) : 0, mgstc::app::editorGraphButtonWidth());
                    const juce::Rectangle<int> host{0, 0, width, layout.bottom};
                    std::vector<juce::Rectangle<int>> bounds{layout.title, layout.description,
                        layout.graph, layout.output, layout.immediate, layout.volume,
                        layout.volume_label, layout.settings};
                    if (navigation) {
                        bounds.push_back(layout.navigation[0]);
                        bounds.push_back(layout.navigation[1]);
                    }
                    for (std::size_t i = 0; i < bounds.size(); ++i) {
                        if (bounds[i].isEmpty() || !host.contains(bounds[i]))
                            throw std::runtime_error("header control outside host: scale=" + std::to_string(scale) + " width=" + std::to_string(width));
                        for (std::size_t j = i + 1; j < bounds.size(); ++j)
                            if (bounds[i].intersects(bounds[j]))
                                throw std::runtime_error("header controls overlap: scale=" + std::to_string(scale) + " width=" + std::to_string(width));
                    }
                    if (layout.graph.getRight() + UiLayout::controlGap != layout.output.getX()
                        || layout.graph.getY() != layout.output.getY())
                        throw std::runtime_error("graph is not immediately left of output switch");
                    if (navigation && layout.navigation[1].getY() == layout.graph.getY()
                        && layout.graph.getX() - layout.navigation[1].getRight() < UiLayout::lg)
                        throw std::runtime_error("graph/navigation margin too narrow");
                    ++cases;
                }
            }
        }
        UiScale::setActivePercent(100);
        std::cout << "PASS: " << cases << " header layouts; bounds, overlaps, graph ordering and margin\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
