// SPDX-License-Identifier: AGPL-3.0-only
#include "../../src/app/juce/patchslide_stock.hpp"

#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
}

int main() {
    try {
        using mgstc::app::PatchSlideStock;
        auto owner = std::make_shared<PatchSlideStock>();
        const std::weak_ptr<PatchSlideStock> lifetime = owner;
        require(owner->ids().empty() && owner->revision() == 0, "Stock must start empty");
        require(!owner->add("") && !owner->remove("missing"), "Invalid mutations changed stock");
        require(owner->add("slide_b") && owner->add("slide_a"), "Could not add stock IDs");
        require(!owner->add("slide_b") && owner->revision() == 2, "Duplicate stock changed order/revision");
        require(owner->ids() == std::vector<std::string>{"slide_b", "slide_a"}, "Stock is not insertion ordered");
        {
            auto view = owner;
            require(view->contains("slide_a"), "View did not share owner state");
            require(view->remove("slide_b") && view->add("slide_b"), "View mutations failed");
        }
        require(owner->ids() == std::vector<std::string>{"slide_a", "slide_b"}, "View close lost stock or re-add order");
        PatchSlideStock independent;
        require(independent.ids().empty(), "Independent owner shares stock");
        for (int i = 0; i < 20000; ++i)
            require(owner->add("candidate_" + std::to_string(i)), "Stock has an arbitrary capacity limit");
        require(owner->ids().size() == 20002 && owner->contains("candidate_19999"), "Large stock lost entries");
        require(owner->remove("candidate_10000") && !owner->contains("candidate_10000"), "Stable-ID removal failed");
        require(owner->ids()[10002] == "candidate_10001", "Removal reordered the remaining IDs");
        owner.reset();
        require(lifetime.expired(), "Stock outlived its owner without a view");
        std::cout << "Transient PatchSlide stock identity/order/lifetime checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
