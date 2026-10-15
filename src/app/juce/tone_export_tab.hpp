// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <juce_gui_extra/juce_gui_extra.h>

#include "mgstc/engine/composite_timbre_library.hpp"
#include "mgstc/engine/furnace_exchange.hpp"
#include "mgstc/engine/mgstc_tone_set.hpp"
#include "mgstc/engine/timbre_library.hpp"
#include "mgstc/engine/tone_library_store.hpp"
#include "tone_library_session.hpp"
#include "ui_fonts.hpp"
#include "ui_layout.hpp"

// Queue entry. kind matches LibraryManagerKind: 0 SCC, 1 OPLL, 2 composite.
struct ToneExportRequest {
    bool is_set{};
    std::uint8_t kind{};
    std::uint64_t id{};
};

class ToneExportTab final : public juce::Component {
public:
    using ReloadCallback = std::function<bool()>;

    ToneExportTab(
        juce::InterProcessLock& lock,
        ReloadCallback reload,
        std::function<const mgstc::engine::TimbreLibrary*()> timbres,
        std::function<const mgstc::engine::CompositeTimbreLibrary*()>
            composites,
        std::function<const std::vector<mgstc::engine::ToneSetRecord>*()>
            sets,
        std::function<const std::vector<mgstc::engine::ToneSetMemberRecord>*()>
            members)
        : lock_(lock),
          reload_(std::move(reload)),
          timbres_(std::move(timbres)),
          composites_(std::move(composites)),
          sets_(std::move(sets)),
          members_(std::move(members)),
          worker_(*this) {
        choose_folder_.setButtonText(juce::String::fromUTF8("保存先"));
        choose_folder_.onClick = [this] { chooseFolder(); };
        addAndMakeVisible(choose_folder_);

        folder_.setColour(
            juce::Label::textColourId, juce::Colour(0xFF9AA8B5));
        folder_.setText(
            juce::String::fromUTF8("保存先フォルダは未選択"),
            juce::dontSendNotification);
        addAndMakeVisible(folder_);

        format_.addItem(juce::String::fromUTF8("セット (.mgstc)"), 1);
        format_.addItem(juce::String::fromUTF8("個別 (.mgstc)"), 2);
        format_.addItem(juce::String::fromUTF8("Furnace (.fui)"), 3);
        format_.addItem(juce::String::fromUTF8("Furnace 波形 (.fuw)"), 4);
        format_.setSelectedId(1, juce::dontSendNotification);
        format_.setTooltip(juce::String::fromUTF8(
            "セット、個別の .mgstc、Furnace の .fui / .fuw"));
        addAndMakeVisible(format_);

        collision_.addItem(juce::String::fromUTF8("上書きを確認"), 1);
        collision_.addItem(juce::String::fromUTF8("連番で避ける"), 2);
        collision_.addItem(juce::String::fromUTF8("既存はスキップ"), 3);
        collision_.setSelectedId(1, juce::dontSendNotification);
        addAndMakeVisible(collision_);

        export_.setButtonText(juce::String::fromUTF8("書き出し"));
        export_.onClick = [this] { startExport(); };
        addAndMakeVisible(export_);
        cancel_.setButtonText(juce::String::fromUTF8("中止"));
        cancel_.onClick = [this] { worker_.signalThreadShouldExit(); };
        cancel_.setEnabled(false);
        addAndMakeVisible(cancel_);
        remove_.setButtonText(juce::String::fromUTF8("選択を外す"));
        remove_.onClick = [this] { removeSelected(); };
        addAndMakeVisible(remove_);
        clear_.setButtonText(juce::String::fromUTF8("クリア"));
        clear_.onClick = [this] {
            queue_.clear();
            list_.updateContent();
        };
        addAndMakeVisible(clear_);

        list_.setModel(&model_);
        list_.setMultipleSelectionEnabled(true);
        list_.setRowHeight(UiLayout::fieldH);
        addAndMakeVisible(list_);

        log_.setMultiLine(true);
        log_.setReadOnly(true);
        log_.setFont(UiFonts::body());
        addAndMakeVisible(log_);
    }

    ~ToneExportTab() override {
        worker_.signalThreadShouldExit();
        worker_.stopThread(4000);
    }

    void addRequest(ToneExportRequest request) {
        const auto same = [&](const ToneExportRequest& item) {
            return item.is_set == request.is_set
                && item.id == request.id
                && (request.is_set || item.kind == request.kind);
        };
        if (std::any_of(queue_.begin(), queue_.end(), same)) {
            return;
        }
        queue_.push_back(request);
        list_.updateContent();
        list_.selectRow(static_cast<int>(queue_.size()) - 1);
    }

    void resized() override {
        list_.setRowHeight(juce::jmax(1, UiLayout::fieldH));
        auto area = getLocalBounds().reduced(UiLayout::sm);
        auto row = area.removeFromTop(UiLayout::textButtonH);
        choose_folder_.setBounds(row.removeFromLeft(96));
        row.removeFromLeft(UiLayout::sm);
        format_.setBounds(row.removeFromRight(210));
        row.removeFromRight(UiLayout::sm);
        collision_.setBounds(row.removeFromRight(160));
        folder_.setBounds(row);
        area.removeFromTop(UiLayout::sm);
        auto buttons = area.removeFromTop(UiLayout::textButtonH);
        export_.setBounds(buttons.removeFromLeft(96));
        buttons.removeFromLeft(UiLayout::sm);
        cancel_.setBounds(buttons.removeFromLeft(72));
        buttons.removeFromLeft(UiLayout::sm);
        remove_.setBounds(buttons.removeFromLeft(112));
        buttons.removeFromLeft(UiLayout::sm);
        clear_.setBounds(buttons.removeFromLeft(72));
        area.removeFromTop(UiLayout::sm);
        log_.setBounds(area.removeFromBottom(120));
        area.removeFromBottom(UiLayout::sm);
        list_.setBounds(area);
    }

private:
    enum class Collision { Replace, Number, Skip };
    enum class ExportKind {
        SetFile,
        SeparateMgstc,
        FurnaceInstrument,
        FurnaceWave,
    };

    struct Snapshot {
        mgstc::engine::TimbreLibrary timbres;
        mgstc::engine::CompositeTimbreLibrary composites;
        std::vector<mgstc::engine::ToneSetRecord> sets;
        std::vector<mgstc::engine::ToneSetMemberRecord> members;
        std::vector<ToneExportRequest> queue;
        std::filesystem::path folder;
        ExportKind kind{ExportKind::SetFile};
        Collision collision{Collision::Replace};
    };

    class QueueModel final : public juce::ListBoxModel {
    public:
        explicit QueueModel(ToneExportTab& owner) : owner_(owner) {}

        int getNumRows() override {
            return static_cast<int>(owner_.queue_.size());
        }

        void paintListBoxItem(
            int row,
            juce::Graphics& graphics,
            int width,
            int height,
            bool selected) override {
            if (selected) {
                graphics.fillAll(juce::Colour(0xFF1E3A32));
            }
            graphics.setColour(juce::Colour(0xFFE6E8EA));
            graphics.setFont(UiFonts::body());
            graphics.drawText(
                owner_.rowLabel(row),
                8,
                0,
                width - 16,
                height,
                juce::Justification::centredLeft,
                true);
        }

    private:
        ToneExportTab& owner_;
    };

    class Worker final : public juce::Thread {
    public:
        explicit Worker(ToneExportTab& owner)
            : juce::Thread("mgstc-tone-export"), owner_(owner) {}

        std::shared_ptr<Snapshot> snapshot;
        int generation{};

        void run() override {
            auto lines = writeSnapshot(*snapshot, [this] {
                return threadShouldExit();
            });
            const int job_generation = generation;
            juce::MessageManager::callAsync(
                [safe = juce::Component::SafePointer<ToneExportTab>(&owner_),
                 job_generation,
                 lines = std::move(lines)]() mutable {
                    if (safe != nullptr) {
                        safe->finish(job_generation, std::move(lines));
                    }
                });
        }

    private:
        ToneExportTab& owner_;
    };

    void chooseFolder() {
        juce::FileChooser chooser(
            juce::String::fromUTF8("書き出し先フォルダ"),
            destination_.exists() ? destination_ : juce::File());
        if (!chooser.browseForDirectory()) {
            return;
        }
        destination_ = chooser.getResult();
        folder_.setText(
            destination_.getFullPathName(), juce::dontSendNotification);
    }

    void removeSelected() {
        auto selected = list_.getSelectedRows();
        for (int index = selected.size() - 1; index >= 0; --index) {
            const int row = selected[index];
            if (row >= 0 && row < static_cast<int>(queue_.size())) {
                queue_.erase(queue_.begin() + row);
            }
        }
        list_.updateContent();
    }

    juce::String rowLabel(int row) const {
        if (row < 0 || row >= static_cast<int>(queue_.size())) {
            return {};
        }
        const auto& item = queue_[static_cast<std::size_t>(row)];
        if (item.is_set) {
            return juce::String::fromUTF8("セット  ") + setName(item.id);
        }
        return kindName(item.kind) + "  " + timbreName(item.kind, item.id);
    }

    juce::String setName(std::uint64_t id) const {
        if (const auto* sets = sets_ ? sets_() : nullptr) {
            for (const auto& set : *sets) {
                if (set.id == id) {
                    return juce::String::fromUTF8(set.name.c_str());
                }
            }
        }
        return juce::String::fromUTF8("（見つからない）");
    }

    juce::String timbreName(std::uint8_t kind, std::uint64_t id) const {
        if (kind == 2) {
            if (const auto* library = composites_ ? composites_() : nullptr) {
                if (const auto* entry = library->find(id)) {
                    return juce::String::fromUTF8(entry->timbre.name.c_str());
                }
            }
        } else if (const auto* library = timbres_ ? timbres_() : nullptr) {
            if (const auto* entry = library->find(id)) {
                return juce::String::fromUTF8(entry->name.c_str());
            }
        }
        return juce::String::fromUTF8("（見つからない）");
    }

    static juce::String kindName(std::uint8_t kind) {
        if (kind == 0) {
            return "SCC";
        }
        if (kind == 1) {
            return "OPLL";
        }
        if (kind == 2) {
            return juce::String::fromUTF8("複合");
        }
        return juce::String::fromUTF8("音色");
    }

    void startExport() {
        if (worker_.isThreadRunning()) {
            return;
        }
        if (!destination_.isDirectory()) {
            appendLog(juce::String::fromUTF8("保存先フォルダを選んでください"));
            return;
        }
        if (queue_.empty()) {
            appendLog(juce::String::fromUTF8("書き出し対象がありません"));
            return;
        }
        auto snapshot = std::make_shared<Snapshot>();
        {
            mgstc::app::ScopedLibraryIpcLock lock(lock_);
            if (!lock.isLocked() || !reload_ || !reload_()) {
                appendLog(juce::String::fromUTF8(
                    "ライブラリを読み込めませんでした"));
                return;
            }
            if (const auto* timbres = timbres_()) {
                snapshot->timbres = *timbres;
            }
            if (const auto* composites = composites_()) {
                snapshot->composites = *composites;
            }
            if (const auto* sets = sets_()) {
                snapshot->sets = *sets;
            }
            if (const auto* members = members_()) {
                snapshot->members = *members;
            }
        }
        snapshot->queue = queue_;
        snapshot->folder = std::filesystem::path(
            destination_.getFullPathName().toWideCharPointer());
        const int format_id = format_.getSelectedId();
        snapshot->kind = format_id == 2
            ? ExportKind::SeparateMgstc
            : format_id == 3
                ? ExportKind::FurnaceInstrument
                : format_id == 4
                    ? ExportKind::FurnaceWave
                    : ExportKind::SetFile;
        snapshot->collision = collision_.getSelectedId() == 2
            ? Collision::Number
            : collision_.getSelectedId() == 3
                ? Collision::Skip
                : Collision::Replace;
        if (snapshot->collision == Collision::Replace) {
            const auto existing = existingTargets(*snapshot);
            if (!existing.isEmpty()) {
                juce::String message;
                const int shown = juce::jmin(8, existing.size());
                for (int index = 0; index < shown; ++index) {
                    message += existing[index] + "\n";
                }
                if (existing.size() > shown) {
                    message += juce::String::fromUTF8("ほか ")
                        + juce::String(existing.size() - shown)
                        + juce::String::fromUTF8(" 件\n");
                }
                message += juce::String::fromUTF8("既存のファイルを上書きしますか？");
                if (!juce::AlertWindow::showOkCancelBox(
                        juce::MessageBoxIconType::QuestionIcon,
                        juce::String::fromUTF8("書き出し"),
                        message)) {
                    appendLog(juce::String::fromUTF8("書き出しをキャンセルしました"));
                    return;
                }
            }
        }
        worker_.snapshot = std::move(snapshot);
        worker_.generation = ++generation_;
        cancel_.setEnabled(true);
        export_.setEnabled(false);
        appendLog(juce::String::fromUTF8("書き出しを開始しました"));
        worker_.startThread();
    }

    void finish(int generation, std::vector<juce::String> lines) {
        if (generation != generation_) {
            return;
        }
        for (const auto& line : lines) {
            appendLog(line);
        }
        if (lines.empty()) {
            appendLog(juce::String::fromUTF8("書き出しは中止されました"));
        }
        cancel_.setEnabled(false);
        export_.setEnabled(true);
    }

    void appendLog(const juce::String& line) {
        log_.moveCaretToEnd();
        log_.insertTextAtCaret(line + "\n");
    }

    static std::filesystem::path pathFromUtf8(std::string_view utf8) {
        const std::u8string text(
            reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
        return std::filesystem::path(text);
    }

    static std::string utf8Of(const juce::String& text) {
        return text.toStdString();
    }

    static juce::String sanitize(juce::String name, const char* fallback) {
        juce::String out;
        const juce::String forbidden("\\/:*?\"<>|");
        for (int index = 0; index < name.length(); ++index) {
            const auto character = name[index];
            if (character < 32 || forbidden.containsChar(character)) {
                out += juce::juce_wchar('_');
            } else {
                out += character;
            }
        }
        while (out.endsWithChar(' ') || out.endsWithChar('.')) {
            out = out.dropLastCharacters(1);
        }
        out = out.trim();
        if (out.length() > 60) {
            out = out.substring(0, 60);
        }
        if (out.isEmpty()) {
            out = juce::String::fromUTF8(fallback);
        }
        return out;
    }

    static std::filesystem::path collided(
        std::filesystem::path path,
        Collision collision) {
        std::error_code error;
        if (!std::filesystem::exists(path, error)) {
            return path;
        }
        if (collision == Collision::Skip) {
            return {};
        }
        if (collision == Collision::Replace) {
            return path;
        }
        const auto parent = path.parent_path();
        const auto stem = path.stem().u8string();
        const auto extension = path.extension().u8string();
        for (int index = 2; index < 1000; ++index) {
            std::string name;
            name.append(reinterpret_cast<const char*>(stem.data()), stem.size());
            name += "_";
            name += std::to_string(index);
            name.append(
                reinterpret_cast<const char*>(extension.data()),
                extension.size());
            auto candidate = parent / pathFromUtf8(name);
            if (!std::filesystem::exists(candidate, error)) {
                return candidate;
            }
        }
        return {};
    }

    static juce::StringArray existingTargets(const Snapshot& snapshot) {
        juce::StringArray names;
        auto note = [&](const std::filesystem::path& path) {
            std::error_code error;
            if (!path.empty() && std::filesystem::exists(path, error)) {
                names.add(juce::String(path.wstring().c_str()));
            }
        };
        int order = 1;
        for (const auto& item : snapshot.queue) {
            if (snapshot.kind == ExportKind::FurnaceInstrument
                || snapshot.kind == ExportKind::FurnaceWave) {
                for (const auto& path : furnacePaths(snapshot, item)) {
                    note(path);
                }
            } else if (item.is_set && snapshot.kind == ExportKind::SetFile) {
                note(setFile(snapshot, item.id));
            } else if (item.is_set) {
                for (const auto& path : individualFiles(snapshot, item, order)) {
                    note(path);
                }
            } else {
                note(individualTimbreFile(snapshot, item, order));
                ++order;
            }
        }
        return names;
    }

    static std::vector<juce::String> writeSnapshot(
        const Snapshot& snapshot,
        const std::function<bool()>& cancelled) {
        std::vector<juce::String> lines;
        int order = 1;
        for (const auto& item : snapshot.queue) {
            if (cancelled()) {
                break;
            }
            if (snapshot.kind == ExportKind::FurnaceInstrument
                || snapshot.kind == ExportKind::FurnaceWave) {
                writeFurnace(snapshot, item, lines, cancelled);
            } else if (item.is_set && snapshot.kind == ExportKind::SetFile) {
                writeSet(snapshot, item.id, lines);
            } else if (item.is_set) {
                writeSetMembers(snapshot, item, order, lines, cancelled);
            } else {
                writeOneTimbre(snapshot, item, order, lines);
                ++order;
            }
        }
        return lines;
    }

    static void writeSet(
        const Snapshot& snapshot,
        std::uint64_t set_id,
        std::vector<juce::String>& lines) {
        const auto* set = findSet(snapshot, set_id);
        if (set == nullptr) {
            lines.push_back(juce::String::fromUTF8(
                "変換不能: セットが見つかりません"));
            return;
        }
        int missing = 0;
        auto document = documentFor(*set, snapshot, missing);
        auto path = collided(setFile(snapshot, set_id), snapshot.collision);
        if (path.empty()) {
            lines.push_back(
                juce::String::fromUTF8("スキップ: ")
                + juce::String::fromUTF8(set->name.c_str()));
            return;
        }
        std::string error;
        if (!mgstc::engine::writeMgstcToneSetFile(path, document, &error)) {
            lines.push_back(
                juce::String::fromUTF8("失敗: ")
                + juce::String(path.wstring().c_str()));
            return;
        }
        auto line = missing == 0
            ? juce::String::fromUTF8("完了: ")
            : juce::String::fromUTF8("部分変換: ");
        line += juce::String(path.wstring().c_str());
        if (missing != 0) {
            line += juce::String::fromUTF8("（不足 ")
                + juce::String(missing)
                + juce::String::fromUTF8(" 件）");
        }
        lines.push_back(line);
    }

    static void writeOneTimbre(
        const Snapshot& snapshot,
        const ToneExportRequest& item,
        int order,
        std::vector<juce::String>& lines) {
        std::string bytes;
        juce::String label;
        if (!timbreBytes(snapshot, item.kind, item.id, bytes, label)) {
            lines.push_back(
                juce::String::fromUTF8("変換不能: ") + label);
            return;
        }
        auto path = collided(
            individualTimbreFile(snapshot, item, order), snapshot.collision);
        writeBytes(path, bytes, label, lines);
    }

    static void writeSetMembers(
        const Snapshot& snapshot,
        const ToneExportRequest& item,
        int& order,
        std::vector<juce::String>& lines,
        const std::function<bool()>& cancelled) {
        const auto* set = findSet(snapshot, item.id);
        if (set == nullptr) {
            lines.push_back(juce::String::fromUTF8(
                "変換不能: セットが見つかりません"));
            return;
        }
        auto members = membersOf(snapshot, set->id);
        if (members.empty()) {
            lines.push_back(
                juce::String::fromUTF8("完了: ")
                + juce::String::fromUTF8(set->name.c_str())
                + juce::String::fromUTF8("（空のセット）"));
            return;
        }
        int missing = 0;
        for (const auto& member : members) {
            if (cancelled()) {
                return;
            }
            const bool composite = member.composite_id != 0;
            ToneExportRequest one;
            one.kind = composite ? 2 : kindOfTimbre(snapshot, member.timbre_id);
            one.id = composite ? member.composite_id : member.timbre_id;
            std::string bytes;
            juce::String label;
            if (!timbreBytes(snapshot, one.kind, one.id, bytes, label)) {
                ++missing;
                lines.push_back(
                    juce::String::fromUTF8("変換不能: ") + label);
                continue;
            }
            auto path = collided(
                memberFile(snapshot, *set, one, order), snapshot.collision);
            writeBytes(path, bytes, label, lines);
            ++order;
        }
        if (missing != 0) {
            lines.push_back(
                juce::String::fromUTF8("部分変換: ")
                + juce::String::fromUTF8(set->name.c_str()));
        }
    }

    static void writeBytes(
        const std::filesystem::path& path,
        const std::string& bytes,
        const juce::String& label,
        std::vector<juce::String>& lines) {
        if (path.empty()) {
            lines.push_back(juce::String::fromUTF8("スキップ: ") + label);
            return;
        }
        std::error_code mkdir_error;
        std::filesystem::create_directories(path.parent_path(), mkdir_error);
        if (mkdir_error) {
            lines.push_back(juce::String::fromUTF8("失敗: ") + label);
            return;
        }
        std::string error;
        if (!mgstc::engine::replaceFileBytesAtomically(path, bytes, &error)) {
            lines.push_back(
                juce::String::fromUTF8("失敗: ") + label);
            return;
        }
        lines.push_back(
            juce::String::fromUTF8("完了: ")
            + juce::String(path.wstring().c_str()));
    }

    static std::filesystem::path furnaceDirectory(
        const Snapshot& snapshot,
        const ToneExportRequest& item) {
        if (!item.is_set) {
            return snapshot.folder;
        }
        const auto* set = findSet(snapshot, item.id);
        const auto name = set == nullptr
            ? juce::String("set")
            : sanitize(juce::String::fromUTF8(set->name.c_str()), "set");
        return snapshot.folder / pathFromUtf8(utf8Of(name));
    }

    static mgstc::engine::FurnaceExportBatch furnaceBatch(
        const Snapshot& snapshot,
        std::uint8_t kind,
        std::uint64_t id) {
        const bool waves = snapshot.kind == ExportKind::FurnaceWave;
        if (kind == 2) {
            if (const auto* entry = snapshot.composites.find(id)) {
                return waves
                    ? mgstc::engine::exportFurnaceWavetables(entry->timbre)
                    : mgstc::engine::exportFurnaceInstruments(entry->timbre);
            }
        } else if (const auto* entry = snapshot.timbres.find(id)) {
            return waves
                ? mgstc::engine::exportFurnaceWavetables(*entry)
                : mgstc::engine::exportFurnaceInstruments(*entry);
        }
        mgstc::engine::FurnaceExportBatch missing;
        missing.warnings.push_back("音色が見つかりません");
        return missing;
    }

    static std::vector<std::filesystem::path> furnacePaths(
        const Snapshot& snapshot,
        const ToneExportRequest& item) {
        std::vector<std::filesystem::path> paths;
        const auto add = [&](std::uint8_t kind, std::uint64_t id, bool in_set) {
            ToneExportRequest one;
            one.is_set = in_set;
            one.id = in_set ? item.id : id;
            const auto directory = furnaceDirectory(snapshot, one);
            const auto batch = furnaceBatch(snapshot, kind, id);
            for (const auto& file : batch.files) {
                paths.push_back(directory / pathFromUtf8(file.filename));
            }
        };
        if (item.is_set) {
            for (const auto& member : membersOf(snapshot, item.id)) {
                if (member.composite_id != 0) {
                    add(2, member.composite_id, true);
                } else {
                    add(kindOfTimbre(snapshot, member.timbre_id), member.timbre_id, true);
                }
            }
            return paths;
        }
        add(item.kind, item.id, false);
        return paths;
    }

    static void writeFurnaceFiles(
        const Snapshot& snapshot,
        const std::filesystem::path& directory,
        const mgstc::engine::FurnaceExportBatch& batch,
        const juce::String& label,
        std::vector<juce::String>& lines) {
        if (batch.files.empty()) {
            auto line = juce::String::fromUTF8("変換不能: ") + label;
            for (const auto& warning : batch.warnings) {
                line += "\n  " + juce::String::fromUTF8(warning.c_str());
            }
            lines.push_back(line);
            return;
        }
        int written = 0;
        for (const auto& file : batch.files) {
            const auto path = collided(
                directory / pathFromUtf8(file.filename), snapshot.collision);
            std::string bytes(file.bytes.begin(), file.bytes.end());
            const auto before = lines.size();
            writeBytes(
                path,
                bytes,
                juce::String::fromUTF8(file.filename.c_str()),
                lines);
            if (lines.size() > before
                && lines.back().startsWith(juce::String::fromUTF8("完了"))) {
                ++written;
            }
        }
        if (batch.files.size() > 1 || batch.partial) {
            auto line = juce::String::fromUTF8(batch.partial ? "部分変換: " : "完了: ");
            line += label + juce::String::fromUTF8("  ")
                + juce::String(written)
                + juce::String::fromUTF8(" 件  ")
                + juce::String(directory.wstring().c_str());
            for (const auto& warning : batch.warnings) {
                line += "\n  " + juce::String::fromUTF8(warning.c_str());
            }
            lines.push_back(line);
        }
    }

    static void writeFurnace(
        const Snapshot& snapshot,
        const ToneExportRequest& item,
        std::vector<juce::String>& lines,
        const std::function<bool()>& cancelled) {
        if (item.is_set) {
            const auto* set = findSet(snapshot, item.id);
            if (set == nullptr) {
                lines.push_back(juce::String::fromUTF8(
                    "変換不能: セットが見つかりません"));
                return;
            }
            const auto members = membersOf(snapshot, set->id);
            if (members.empty()) {
                lines.push_back(
                    juce::String::fromUTF8("完了: ")
                    + juce::String::fromUTF8(set->name.c_str())
                    + juce::String::fromUTF8("（空のセット）"));
                return;
            }
            const auto directory = furnaceDirectory(snapshot, item);
            int files = 0;
            bool partial = false;
            for (const auto& member : members) {
                if (cancelled()) {
                    return;
                }
                const bool composite = member.composite_id != 0;
                const auto batch = furnaceBatch(
                    snapshot,
                    composite ? 2 : kindOfTimbre(snapshot, member.timbre_id),
                    composite ? member.composite_id : member.timbre_id);
                files += static_cast<int>(batch.files.size());
                partial = partial || batch.partial || batch.files.size() > 1;
                writeFurnaceFiles(
                    snapshot,
                    directory,
                    batch,
                    juce::String::fromUTF8(set->name.c_str()),
                    lines);
            }
            if (files > 1) {
                lines.push_back(
                    juce::String::fromUTF8(partial ? "部分変換: " : "完了: ")
                    + juce::String::fromUTF8(set->name.c_str())
                    + juce::String::fromUTF8("  ")
                    + juce::String(files)
                    + juce::String::fromUTF8(" 件"));
            }
            return;
        }
        const auto batch = furnaceBatch(snapshot, item.kind, item.id);
        writeFurnaceFiles(
            snapshot,
            snapshot.folder,
            batch,
            timbreLabel(snapshot, item.kind, item.id),
            lines);
    }

    static juce::String timbreLabel(
        const Snapshot& snapshot,
        std::uint8_t kind,
        std::uint64_t id) {
        if (kind == 2) {
            if (const auto* entry = snapshot.composites.find(id)) {
                return juce::String::fromUTF8(entry->timbre.name.c_str());
            }
        } else if (const auto* entry = snapshot.timbres.find(id)) {
            return juce::String::fromUTF8(entry->name.c_str());
        }
        return juce::String::fromUTF8("音色");
    }

    static const mgstc::engine::ToneSetRecord* findSet(
        const Snapshot& snapshot,
        std::uint64_t id) {
        for (const auto& set : snapshot.sets) {
            if (set.id == id) {
                return &set;
            }
        }
        return nullptr;
    }

    static std::vector<mgstc::engine::ToneSetMemberRecord> membersOf(
        const Snapshot& snapshot,
        std::uint64_t set_id) {
        std::vector<mgstc::engine::ToneSetMemberRecord> rows;
        for (const auto& member : snapshot.members) {
            if (member.set_id == set_id) {
                rows.push_back(member);
            }
        }
        std::stable_sort(
            rows.begin(),
            rows.end(),
            [](const auto& left, const auto& right) {
                return left.position < right.position;
            });
        return rows;
    }

    static mgstc::engine::MgstcToneSetDocument documentFor(
        const mgstc::engine::ToneSetRecord& set,
        const Snapshot& snapshot,
        int& missing) {
        mgstc::engine::MgstcToneSetDocument document;
        document.name = set.name;
        document.memo = set.memo;
        for (const auto& member : membersOf(snapshot, set.id)) {
            if (member.composite_id != 0) {
                if (const auto* entry =
                        snapshot.composites.find(member.composite_id)) {
                    document.members.emplace_back(*entry);
                } else {
                    ++missing;
                }
            } else if (const auto* entry =
                           snapshot.timbres.find(member.timbre_id)) {
                document.members.emplace_back(*entry);
            } else {
                ++missing;
            }
        }
        return document;
    }

    static bool timbreBytes(
        const Snapshot& snapshot,
        std::uint8_t kind,
        std::uint64_t id,
        std::string& bytes,
        juce::String& label) {
        if (kind == 2) {
            const auto* entry = snapshot.composites.find(id);
            label = entry == nullptr
                ? juce::String::fromUTF8("複合音色")
                : juce::String::fromUTF8(entry->timbre.name.c_str());
            if (entry == nullptr) {
                return false;
            }
            mgstc::engine::CompositeTimbreLibrary one;
            one.installEntries({*entry});
            bytes = one.serialize();
            return true;
        }
        const auto* entry = snapshot.timbres.find(id);
        label = entry == nullptr
            ? juce::String::fromUTF8("音色")
            : juce::String::fromUTF8(entry->name.c_str());
        if (entry == nullptr) {
            return false;
        }
        const auto text = snapshot.timbres.serializeEntry(id);
        if (!text) {
            return false;
        }
        bytes = *text;
        return true;
    }

    static std::uint8_t kindOfTimbre(
        const Snapshot& snapshot,
        std::uint64_t id) {
        const auto* entry = snapshot.timbres.find(id);
        if (entry != nullptr
            && entry->category == mgstc::engine::TimbreCategory::Opll) {
            return 1;
        }
        return 0;
    }

    static std::filesystem::path setFile(
        const Snapshot& snapshot,
        std::uint64_t set_id) {
        const auto* set = findSet(snapshot, set_id);
        const auto name = set == nullptr
            ? juce::String("set")
            : sanitize(
                juce::String::fromUTF8(set->name.c_str()), "set");
        return snapshot.folder
            / pathFromUtf8(utf8Of(name + ".mgstc"));
    }

    static std::filesystem::path individualTimbreFile(
        const Snapshot& snapshot,
        const ToneExportRequest& item,
        int order) {
        juce::String label = "timbre";
        if (item.kind == 2) {
            if (const auto* entry = snapshot.composites.find(item.id)) {
                label = juce::String::fromUTF8(entry->timbre.name.c_str());
            }
        } else if (const auto* entry = snapshot.timbres.find(item.id)) {
            label = juce::String::fromUTF8(entry->name.c_str());
        }
        return snapshot.folder
            / pathFromUtf8(utf8Of(numbered(order, label)));
    }

    static std::filesystem::path memberFile(
        const Snapshot& snapshot,
        const mgstc::engine::ToneSetRecord& set,
        const ToneExportRequest& item,
        int order) {
        const auto folder_name = sanitize(
            juce::String::fromUTF8(set.name.c_str()), "set");
        const auto folder =
            snapshot.folder / pathFromUtf8(utf8Of(folder_name));
        juce::String label = "timbre";
        if (item.kind == 2) {
            if (const auto* entry = snapshot.composites.find(item.id)) {
                label = juce::String::fromUTF8(entry->timbre.name.c_str());
            }
        } else if (const auto* entry = snapshot.timbres.find(item.id)) {
            label = juce::String::fromUTF8(entry->name.c_str());
        }
        return folder / pathFromUtf8(utf8Of(numbered(order, label)));
    }

    static std::vector<std::filesystem::path> individualFiles(
        const Snapshot& snapshot,
        const ToneExportRequest& item,
        int& order) {
        std::vector<std::filesystem::path> paths;
        const auto* set = findSet(snapshot, item.id);
        if (set == nullptr) {
            return paths;
        }
        for (const auto& member : membersOf(snapshot, set->id)) {
            ToneExportRequest one;
            one.kind = member.composite_id != 0
                ? 2
                : kindOfTimbre(snapshot, member.timbre_id);
            one.id = member.composite_id != 0
                ? member.composite_id
                : member.timbre_id;
            paths.push_back(memberFile(snapshot, *set, one, order));
            ++order;
        }
        return paths;
    }

    static juce::String numbered(int order, juce::String name) {
        const auto prefix = juce::String(order).paddedLeft('0', 2);
        return prefix + "_" + sanitize(name, "timbre") + ".mgstc";
    }

    juce::InterProcessLock& lock_;
    ReloadCallback reload_;
    std::function<const mgstc::engine::TimbreLibrary*()> timbres_;
    std::function<const mgstc::engine::CompositeTimbreLibrary*()> composites_;
    std::function<const std::vector<mgstc::engine::ToneSetRecord>*()> sets_;
    std::function<const std::vector<mgstc::engine::ToneSetMemberRecord>*()>
        members_;
    Worker worker_;
    QueueModel model_{*this};
    std::vector<ToneExportRequest> queue_;
    juce::File destination_;
    int generation_{};
    juce::TextButton choose_folder_;
    juce::Label folder_;
    juce::ComboBox format_;
    juce::ComboBox collision_;
    juce::TextButton export_;
    juce::TextButton cancel_;
    juce::TextButton remove_;
    juce::TextButton clear_;
    juce::ListBox list_;
    juce::TextEditor log_;
};
