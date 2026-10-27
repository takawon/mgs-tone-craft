// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace mgstc::app {

// Message-thread-only transient UI state. IDs retain insertion order and are
// resolved against the current database by the view, never treated as row IDs.
// This model is not part of a timbre, preferences, or plugin serialization.
class PatchSlideStock final {
public:
    [[nodiscard]] const std::vector<std::string>& ids() const noexcept { return ids_; }
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    [[nodiscard]] bool contains(std::string_view id) const { return lookup_.find(id) != lookup_.end(); }

    bool add(std::string_view id) {
        if (id.empty() || contains(id)) return false;
        auto [position, inserted] = lookup_.emplace(id);
        if (!inserted) return false;
        try { ids_.push_back(*position); }
        catch (...) { lookup_.erase(position); throw; }
        ++revision_;
        return true;
    }
    bool remove(std::string_view id) {
        const auto position = lookup_.find(id);
        if (position == lookup_.end()) return false;
        const auto row = std::find(ids_.begin(), ids_.end(), *position);
        if (row != ids_.end()) ids_.erase(row);
        lookup_.erase(position);
        ++revision_;
        return true;
    }

private:
    std::vector<std::string> ids_;
    std::set<std::string, std::less<>> lookup_;
    std::uint64_t revision_{};
};

} // namespace mgstc::app
