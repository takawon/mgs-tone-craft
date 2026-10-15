// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "mgstc/engine/tone_library_store.hpp"

namespace mgstc::app {

struct LibrarySetDragItem {
    std::uint64_t source_set_id{};
    bool composite{};
    std::uint64_t body_id{};
};

enum class LibrarySetDropPlace : std::uint8_t {
    SetHeader,
    Member,
    Unassigned,
};

struct LibrarySetDropTarget {
    LibrarySetDropPlace place{LibrarySetDropPlace::SetHeader};
    std::uint64_t set_id{};
    bool composite{};
    std::uint64_t body_id{};
};

[[nodiscard]] inline bool librarySetBodyEquals(
    const mgstc::engine::ToneSetMemberRecord& member,
    bool composite,
    std::uint64_t body_id) {
    if (composite) {
        return member.composite_id == body_id && member.timbre_id == 0;
    }
    return member.timbre_id == body_id && member.composite_id == 0;
}

inline void librarySetCompact(
    std::vector<mgstc::engine::ToneSetMemberRecord>& members,
    std::uint64_t set_id) {
    std::vector<mgstc::engine::ToneSetMemberRecord*> rows;
    for (auto& member : members) {
        if (member.set_id == set_id) {
            rows.push_back(&member);
        }
    }
    std::stable_sort(
        rows.begin(),
        rows.end(),
        [](const auto* left, const auto* right) {
            return left->position < right->position;
        });
    int position = 0;
    for (auto* row : rows) {
        row->position = position++;
    }
}

inline bool librarySetRemoveMembership(
    std::vector<mgstc::engine::ToneSetMemberRecord>& members,
    std::uint64_t set_id,
    bool composite,
    std::uint64_t body_id) {
    const auto before = members.size();
    members.erase(
        std::remove_if(
            members.begin(),
            members.end(),
            [&](const mgstc::engine::ToneSetMemberRecord& member) {
                return member.set_id == set_id
                    && librarySetBodyEquals(member, composite, body_id);
            }),
        members.end());
    if (members.size() == before) {
        return false;
    }
    librarySetCompact(members, set_id);
    return true;
}

[[nodiscard]] inline bool librarySetContains(
    const std::vector<mgstc::engine::ToneSetMemberRecord>& members,
    std::uint64_t set_id,
    bool composite,
    std::uint64_t body_id) {
    return std::any_of(
        members.begin(),
        members.end(),
        [&](const mgstc::engine::ToneSetMemberRecord& member) {
            return member.set_id == set_id
                && librarySetBodyEquals(member, composite, body_id);
        });
}

[[nodiscard]] inline std::uint64_t librarySetNextId(
    const std::vector<mgstc::engine::ToneSetRecord>& sets) {
    std::uint64_t next = 1;
    for (const auto& set : sets) {
        next = std::max(next, set.id + 1);
    }
    return next;
}

[[nodiscard]] inline bool librarySetNameTaken(
    const std::vector<mgstc::engine::ToneSetRecord>& sets,
    const std::string& name,
    std::uint64_t except_id = 0) {
    return std::any_of(
        sets.begin(),
        sets.end(),
        [&](const mgstc::engine::ToneSetRecord& set) {
            return set.id != except_id && set.name == name;
        });
}

[[nodiscard]] inline std::string librarySetUniqueName(
    const std::vector<mgstc::engine::ToneSetRecord>& sets,
    std::string base) {
    if (base.empty()) {
        base = "新しいセット";
    }
    if (!librarySetNameTaken(sets, base)) {
        return base;
    }
    for (int index = 2; index < 1000; ++index) {
        const auto candidate = base + " (" + std::to_string(index) + ")";
        if (!librarySetNameTaken(sets, candidate)) {
            return candidate;
        }
    }
    return base + " (1000)";
}

inline void librarySetDelete(
    std::vector<mgstc::engine::ToneSetRecord>& sets,
    std::vector<mgstc::engine::ToneSetMemberRecord>& members,
    std::uint64_t set_id) {
    sets.erase(
        std::remove_if(
            sets.begin(),
            sets.end(),
            [&](const mgstc::engine::ToneSetRecord& set) {
                return set.id == set_id;
            }),
        sets.end());
    members.erase(
        std::remove_if(
            members.begin(),
            members.end(),
            [&](const mgstc::engine::ToneSetMemberRecord& member) {
                return member.set_id == set_id;
            }),
        members.end());
}

// Ctrl adds a membership and leaves the source set alone.
// A plain drop moves membership from the source set into the target.
// A plain drop on 未所属 removes only the source membership.
// Dropping an unassigned timbre never clears memberships in other sets.
inline bool applyLibrarySetDrop(
    std::vector<mgstc::engine::ToneSetMemberRecord>& members,
    const std::vector<LibrarySetDragItem>& items,
    const LibrarySetDropTarget& target,
    bool ctrl_add) {
    if (items.empty()) {
        return false;
    }
    if (target.place == LibrarySetDropPlace::Unassigned) {
        if (ctrl_add) {
            return false;
        }
        bool changed = false;
        for (const auto& item : items) {
            if (item.source_set_id == 0 || item.body_id == 0) {
                continue;
            }
            changed = librarySetRemoveMembership(
                          members,
                          item.source_set_id,
                          item.composite,
                          item.body_id)
                || changed;
        }
        return changed;
    }
    if (target.set_id == 0) {
        return false;
    }
    bool changed = false;

    const auto dragged_from_target =
        [&](const mgstc::engine::ToneSetMemberRecord& member) {
            for (const auto& item : items) {
                if (item.source_set_id == target.set_id
                    && librarySetBodyEquals(
                        member, item.composite, item.body_id)) {
                    return true;
                }
            }
            return false;
        };

    std::vector<mgstc::engine::ToneSetMemberRecord> target_rows;
    target_rows.reserve(members.size());
    for (const auto& member : members) {
        if (member.set_id == target.set_id) {
            target_rows.push_back(member);
        }
    }
    std::stable_sort(
        target_rows.begin(),
        target_rows.end(),
        [](const auto& left, const auto& right) {
            return left.position < right.position;
        });

    int insert_at = static_cast<int>(target_rows.size());
    if (target.place == LibrarySetDropPlace::Member) {
        insert_at = 0;
        for (const auto& member : target_rows) {
            if (librarySetBodyEquals(
                    member, target.composite, target.body_id)) {
                break;
            }
            if (!dragged_from_target(member)) {
                ++insert_at;
            }
        }
    }

    std::vector<mgstc::engine::ToneSetMemberRecord> incoming;
    for (const auto& item : items) {
        if (item.body_id == 0) {
            continue;
        }
        if (item.source_set_id == target.set_id && ctrl_add) {
            continue;
        }
        const bool already = std::any_of(
            target_rows.begin(),
            target_rows.end(),
            [&](const mgstc::engine::ToneSetMemberRecord& member) {
                return librarySetBodyEquals(
                    member, item.composite, item.body_id);
            });
        if (already && item.source_set_id != target.set_id) {
            if (!ctrl_add && item.source_set_id != 0) {
                changed = librarySetRemoveMembership(
                              members,
                              item.source_set_id,
                              item.composite,
                              item.body_id)
                    || changed;
            }
            continue;
        }
        if (std::any_of(
                incoming.begin(),
                incoming.end(),
                [&](const mgstc::engine::ToneSetMemberRecord& member) {
                    return librarySetBodyEquals(
                        member, item.composite, item.body_id);
                })) {
            continue;
        }
        mgstc::engine::ToneSetMemberRecord created;
        created.set_id = target.set_id;
        if (item.composite) {
            created.composite_id = item.body_id;
        } else {
            created.timbre_id = item.body_id;
        }
        incoming.push_back(created);
        if (!ctrl_add && item.source_set_id != 0
            && item.source_set_id != target.set_id) {
            changed = librarySetRemoveMembership(
                          members,
                          item.source_set_id,
                          item.composite,
                          item.body_id)
                || changed;
        }
    }

    if (incoming.empty()) {
        return changed;
    }

    std::vector<mgstc::engine::ToneSetMemberRecord> kept;
    for (const auto& member : target_rows) {
        const bool reinsert = std::any_of(
            incoming.begin(),
            incoming.end(),
            [&](const mgstc::engine::ToneSetMemberRecord& item) {
                return librarySetBodyEquals(
                    member,
                    item.composite_id != 0,
                    item.composite_id != 0 ? item.composite_id
                                           : item.timbre_id);
            });
        if (!reinsert) {
            kept.push_back(member);
        }
    }
    if (insert_at < 0) {
        insert_at = 0;
    }
    if (insert_at > static_cast<int>(kept.size())) {
        insert_at = static_cast<int>(kept.size());
    }
    kept.insert(
        kept.begin() + insert_at, incoming.begin(), incoming.end());

    const bool same_order = kept.size() == target_rows.size()
        && std::equal(
            kept.begin(),
            kept.end(),
            target_rows.begin(),
            [](const mgstc::engine::ToneSetMemberRecord& left,
               const mgstc::engine::ToneSetMemberRecord& right) {
                return left.timbre_id == right.timbre_id
                    && left.composite_id == right.composite_id;
            });
    if (same_order) {
        return changed;
    }

    members.erase(
        std::remove_if(
            members.begin(),
            members.end(),
            [&](const mgstc::engine::ToneSetMemberRecord& member) {
                return member.set_id == target.set_id;
            }),
        members.end());
    int position = 0;
    for (auto& member : kept) {
        member.set_id = target.set_id;
        member.position = position++;
        members.push_back(member);
    }
    return true;
}

}  // namespace mgstc::app
