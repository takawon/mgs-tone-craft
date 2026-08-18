// SPDX-License-Identifier: AGPL-3.0-only

#include "ui_layout.hpp"

namespace {
struct UiLayoutInit final {
    UiLayoutInit() { UiLayout::applyScale(1.0F); }
};
const UiLayoutInit ui_layout_init{};
} // namespace

void UiScale::applyLayoutMetrics(float factor) {
    UiLayout::applyScale(factor);
}
