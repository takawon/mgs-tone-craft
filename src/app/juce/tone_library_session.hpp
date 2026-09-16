// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <juce_core/juce_core.h>

#include "mgstc/engine/composite_timbre_library.hpp"
#include "mgstc/engine/timbre_library.hpp"

namespace mgstc::app {

// Message-thread wait for the shared library lock. A second MGSTC instance
// or a crashed holder must not freeze the UI with activity still "idle".
inline constexpr int kLibraryIpcLockTimeoutMs = 2500;

class ScopedLibraryIpcLock final {
public:
    explicit ScopedLibraryIpcLock(juce::InterProcessLock& lock);
    ~ScopedLibraryIpcLock();
    ScopedLibraryIpcLock(const ScopedLibraryIpcLock&) = delete;
    ScopedLibraryIpcLock& operator=(const ScopedLibraryIpcLock&) = delete;
    [[nodiscard]] bool isLocked() const noexcept;

private:
    juce::InterProcessLock& lock_;
    bool locked_;
};

[[nodiscard]] juce::File toneLibraryFile();
[[nodiscard]] std::string toneLibraryUtf8Path();

[[nodiscard]] inline std::int64_t currentUnixTime() {
    return static_cast<std::int64_t>(
        juce::Time::getCurrentTime().toMilliseconds() / 1000);
}

// Disk fingerprint for skipping unchanged library reloads on focus.
struct LibraryFileFingerprint {
    bool present{};
    std::int64_t mtime_ms{};
    std::int64_t size{};

    friend bool operator==(
        const LibraryFileFingerprint&,
        const LibraryFileFingerprint&) = default;
};

[[nodiscard]] LibraryFileFingerprint libraryFileFingerprint(
    const juce::File& file);

struct ToneLibrarySnapshot {
    engine::TimbreLibrary timbres;
    engine::CompositeTimbreLibrary composites;
};

// One SQLite open. Single-side editors may take only one member.
[[nodiscard]] std::optional<ToneLibrarySnapshot> loadToneLibraries(
    std::string* error = nullptr);
[[nodiscard]] std::optional<engine::TimbreLibrary> loadSharedTimbreLibrary(
    std::string* error = nullptr);
[[nodiscard]] std::optional<engine::CompositeTimbreLibrary>
loadSharedCompositeTimbreLibrary(std::string* error = nullptr);

[[nodiscard]] bool persistTimbreLibrary(
    const engine::TimbreLibrary& library);
[[nodiscard]] bool persistCompositeTimbreLibrary(
    const engine::CompositeTimbreLibrary& library);
// Both tables, then unused-tag GC. Use when the caller holds both libraries.
[[nodiscard]] bool persistToneLibraries(
    const engine::TimbreLibrary& timbres,
    const engine::CompositeTimbreLibrary& composites);

}  // namespace mgstc::app
