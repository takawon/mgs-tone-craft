// SPDX-License-Identifier: AGPL-3.0-only

#include "tone_library_session.hpp"

#include "juce_utf8.hpp"
#include "mgstc/engine/tone_library_store.hpp"
#include "ui_paths.hpp"

namespace mgstc::app {
namespace {

// Only startup and the message-thread library operations access this snapshot.
std::optional<ToneLibrarySnapshot> tone_library_session_fallback;

[[nodiscard]] bool openToneLibraryDatabase(
    engine::ToneLibraryDatabase& database,
    bool create_if_missing,
    std::string* error) {
    if (tone_library_session_fallback) {
        return false;
    }
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

bool initialiseToneLibrarySession(
    juce::InterProcessLock& library_lock,
    std::string* error) {
    if (error) { error->clear(); }
    ScopedLibraryIpcLock lock(library_lock);
    if (!lock.isLocked()) {
        if (error) { *error = "failed to acquire shared tone library lock"; }
        disableToneLibraryForSession();
        return false;
    }
    if (!toneLibraryFile().existsAsFile()) {
        return true;
    }
    engine::ToneLibraryDatabase database;
    ToneLibrarySnapshot available;
    if (!database.open(toneLibraryUtf8Path(), error)
        || !database.loadAvailable(available.timbres, available.composites, error)) {
        disableToneLibraryForSession(std::move(available));
        return false;
    }
    return true;
}

void disableToneLibraryForSession(ToneLibrarySnapshot available) {
    tone_library_session_fallback = std::move(available);
}

std::optional<ToneLibrarySnapshot> loadToneLibraries(std::string* error) {
    ToneLibrarySnapshot snapshot;
    if (tone_library_session_fallback) {
        return *tone_library_session_fallback;
    }
    if (!toneLibraryFile().existsAsFile()) {
        return snapshot;
    }
    engine::ToneLibraryDatabase database;
    if (!database.open(toneLibraryUtf8Path(), error)
        || !database.load(snapshot.timbres, snapshot.composites, error)
        || !database.loadToneSets(snapshot.sets, snapshot.members, error)) {
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

bool persistToneSets(
    const std::vector<engine::ToneSetRecord>& sets,
    const std::vector<engine::ToneSetMemberRecord>& members) {
    engine::ToneLibraryDatabase database;
    std::string error;
    if (!openToneLibraryDatabase(database, true, &error)
        || !database.isOpen()) {
        return false;
    }
    return database.replaceToneSets(sets, members);
}

bool persistToneLibrariesAndSets(
    const engine::TimbreLibrary& timbres,
    const engine::CompositeTimbreLibrary& composites,
    const std::vector<engine::ToneSetRecord>& sets,
    const std::vector<engine::ToneSetMemberRecord>& members) {
    engine::ToneLibraryDatabase database;
    std::string error;
    if (!openToneLibraryDatabase(database, true, &error)
        || !database.isOpen()) {
        return false;
    }
    return database.replaceAllAndToneSets(
        timbres, composites, sets, members);
}

}  // namespace mgstc::app
