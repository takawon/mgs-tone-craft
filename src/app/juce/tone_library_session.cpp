// SPDX-License-Identifier: AGPL-3.0-only

#include "tone_library_session.hpp"

#include "juce_utf8.hpp"
#include "mgstc/engine/tone_library_store.hpp"
#include "ui_paths.hpp"

namespace mgstc::app {
namespace {

[[nodiscard]] bool openToneLibraryDatabase(
    engine::ToneLibraryDatabase& database,
    bool create_if_missing,
    std::string* error) {
    const auto target = toneLibraryFile();
    if (!target.existsAsFile()) {
        if (!create_if_missing) {
            return true;
        }
        if (!target.getParentDirectory().createDirectory()) {
            if (error) {
                *error = "failed to create application data directory";
            }
            return false;
        }
    }
    return database.open(toneLibraryUtf8Path(), error);
}

}  // namespace

ScopedLibraryIpcLock::ScopedLibraryIpcLock(juce::InterProcessLock& lock)
    : lock_(lock),
      locked_(lock.enter(kLibraryIpcLockTimeoutMs)) {}

ScopedLibraryIpcLock::~ScopedLibraryIpcLock() {
    if (locked_) {
        lock_.exit();
    }
}

bool ScopedLibraryIpcLock::isLocked() const noexcept {
    return locked_;
}

juce::File toneLibraryFile() {
    return mgstcApplicationDataDirectory().getChildFile("tone_library.sqlite");
}

std::string toneLibraryUtf8Path() {
    return utf8String(toneLibraryFile().getFullPathName());
}

LibraryFileFingerprint libraryFileFingerprint(const juce::File& file) {
    if (!file.existsAsFile()) {
        return {};
    }
    return {
        .present = true,
        .mtime_ms = file.getLastModificationTime().toMilliseconds(),
        .size = file.getSize(),
    };
}

std::optional<ToneLibrarySnapshot> loadToneLibraries(std::string* error) {
    ToneLibrarySnapshot snapshot;
    if (!toneLibraryFile().existsAsFile()) {
        return snapshot;
    }
    engine::ToneLibraryDatabase database;
    if (!database.open(toneLibraryUtf8Path(), error)
        || !database.load(snapshot.timbres, snapshot.composites, error)) {
        return std::nullopt;
    }
    return snapshot;
}

std::optional<engine::TimbreLibrary> loadSharedTimbreLibrary(
    std::string* error) {
    auto snapshot = loadToneLibraries(error);
    if (!snapshot) {
        return std::nullopt;
    }
    return std::move(snapshot->timbres);
}

std::optional<engine::CompositeTimbreLibrary>
loadSharedCompositeTimbreLibrary(std::string* error) {
    auto snapshot = loadToneLibraries(error);
    if (!snapshot) {
        return std::nullopt;
    }
    return std::move(snapshot->composites);
}

bool persistTimbreLibrary(const engine::TimbreLibrary& library) {
    engine::ToneLibraryDatabase database;
    std::string error;
    if (!openToneLibraryDatabase(database, true, &error)
        || !database.isOpen()) {
        return false;
    }
    return database.replaceTimbres(library);
}

bool persistCompositeTimbreLibrary(
    const engine::CompositeTimbreLibrary& library) {
    engine::ToneLibraryDatabase database;
    std::string error;
    if (!openToneLibraryDatabase(database, true, &error)
        || !database.isOpen()) {
        return false;
    }
    return database.replaceComposites(library);
}

bool persistToneLibraries(
    const engine::TimbreLibrary& timbres,
    const engine::CompositeTimbreLibrary& composites) {
    engine::ToneLibraryDatabase database;
    std::string error;
    if (!openToneLibraryDatabase(database, true, &error)
        || !database.isOpen()) {
        return false;
    }
    return database.replaceAll(timbres, composites);
}

}  // namespace mgstc::app
