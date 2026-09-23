// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

// Message-thread editor diagnostics. Debug only: Release compiles this
// down to empty macros / always-false skip flags so product builds have
// no QPC, no log files, and no INI I/O.

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#if !defined(NDEBUG)
#define MGSTC_EDITOR_DIAG 1
#else
#define MGSTC_EDITOR_DIAG 0
#endif

#if MGSTC_EDITOR_DIAG
#include <algorithm>
#include <cstdio>
#include <cwchar>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace mgstc::app {

enum class StallProbeId : std::uint8_t {
    CompositeTimer = 0,
    KeyboardTimer,
    TimelinePaint,
    EditorPaint,
    TimelineResized,
    EditorResized,
    CacheRebuild,
    Snapshot,
    ComponentUpdate,
    TimelineMouseMove,
    Count
};

enum class JankRepaintKind : std::uint8_t {
    FullEditor = 0,
    Timeline,
    Waveform,
    Keyboard,
    HoverPopup,
    Other,
    Count
};

inline const char* stallProbeName(StallProbeId id) noexcept {
    switch (id) {
    case StallProbeId::CompositeTimer:
        return "CompositeEditor timerCallback";
    case StallProbeId::KeyboardTimer:
        return "PerformanceKeyboard timer";
    case StallProbeId::TimelinePaint:
        return "CompositeTimeline paint";
    case StallProbeId::EditorPaint:
        return "CompositeEditor paint";
    case StallProbeId::TimelineResized:
        return "CompositeTimeline resized";
    case StallProbeId::EditorResized:
        return "CompositeEditor resized";
    case StallProbeId::CacheRebuild:
        return "cache rebuild ENV/rate/volume/MML";
    case StallProbeId::Snapshot:
        return "EditorSession snapshot";
    case StallProbeId::ComponentUpdate:
        return "Component enable/disable/update";
    case StallProbeId::TimelineMouseMove:
        return "CompositeTimeline mouseMove";
    default:
        return "unknown";
    }
}

inline const char* jankRepaintName(JankRepaintKind kind) noexcept {
    switch (kind) {
    case JankRepaintKind::FullEditor:
        return "fullEditor";
    case JankRepaintKind::Timeline:
        return "timeline";
    case JankRepaintKind::Waveform:
        return "waveform";
    case JankRepaintKind::Keyboard:
        return "keyboard";
    case JankRepaintKind::HoverPopup:
        return "hoverPopup";
    case JankRepaintKind::Other:
        return "other";
    default:
        return "unknown";
    }
}

class EditorStallProbe final {
public:
    static constexpr std::size_t kRing = 512;
    static constexpr std::size_t kEventSlots = 24;
    static constexpr std::size_t kSecondSlots = 60;
    static constexpr std::size_t kTimelineEvents = 96;
    static constexpr std::size_t kKindCount =
        static_cast<std::size_t>(JankRepaintKind::Count);

    struct Stats {
        std::uint64_t count{};
        double sum_ms{};
        double max_ms{};
        std::uint32_t over_10{};
        std::uint32_t over_16{};
        std::uint32_t over_33{};
        double p95_ms{};
        double p99_ms{};
    };

    struct EventCount {
        const char* name{};
        std::uint32_t count{};
    };

    struct SkipFlags {
        bool periodic{};
        bool waveform{};
        bool keyboard{};
        bool hover{};
    };

    static EditorStallProbe& instance() noexcept {
        static EditorStallProbe probe;
        return probe;
    }

#if MGSTC_EDITOR_DIAG
    void beginSession(double warmup_ms = 1500.0) noexcept {
        reset();
        warmup_until_ms_ = nowMs() + warmup_ms;
        session_started_ms_ = nowMs();
        session_started_ = true;
        last_second_ms_ = session_started_ms_;
        last_skip_reload_ms_ = 0.0;
        ensureJankIni();
        reloadSkipFlags();
        noteEvent("editorOpen");
    }

    void reset() noexcept {
        buckets_ = {};
        events_ = {};
        event_used_ = 0;
        warmup_until_ms_ = 0.0;
        session_started_ = false;
        session_started_ms_ = 0.0;
        last_second_ms_ = 0.0;
        current_ = {};
        seconds_ = {};
        second_used_ = 0;
        second_i_ = 0;
        timeline_ = {};
        timeline_used_ = 0;
        timeline_i_ = 0;
        editor_pixels_ = 0;
        skip_ = {};
        last_skip_reload_ms_ = 0.0;
        last_paint_busy_ = false;
        busy_since_s_ = 0.0;
    }

    void record(StallProbeId id, double ms) noexcept {
        if (!session_started_) {
            return;
        }
        if (nowMs() < warmup_until_ms_) {
            return;
        }
        const auto index = static_cast<std::size_t>(id);
        if (index >= buckets_.size()) {
            return;
        }
        auto& bucket = buckets_[index];
        ++bucket.count;
        bucket.sum_ms += ms;
        if (ms > bucket.max_ms) {
            bucket.max_ms = ms;
        }
        if (ms >= 10.0) {
            ++bucket.over_10;
        }
        if (ms >= 16.0) {
            ++bucket.over_16;
        }
        if (ms >= 33.0) {
            ++bucket.over_33;
        }
        bucket.ring[bucket.ring_i] = static_cast<float>(ms);
        bucket.ring_i = (bucket.ring_i + 1) % kRing;
        if (bucket.ring_n < kRing) {
            ++bucket.ring_n;
        }
    }

    void countEvent(const char* name) noexcept {
        if (!session_started_ || name == nullptr) {
            return;
        }
        if (nowMs() < warmup_until_ms_) {
            return;
        }
        for (std::size_t i = 0; i < event_used_; ++i) {
            if (events_[i].name == name
                || std::strcmp(events_[i].name, name) == 0) {
                ++events_[i].count;
                return;
            }
        }
        if (event_used_ >= events_.size()) {
            return;
        }
        events_[event_used_++] = EventCount{name, 1};
    }

    void setEditorPixels(int pixels) noexcept {
        editor_pixels_ = pixels > 0 ? pixels : 0;
    }

    [[nodiscard]] SkipFlags skipFlags() const noexcept {
        return skip_;
    }

    void reloadSkipFlags() noexcept {
        const double now = nowMs();
        if (now - last_skip_reload_ms_ < 1000.0) {
            return;
        }
        last_skip_reload_ms_ = now;
        wchar_t path[MAX_PATH]{};
        fillJankIniPath(path);
        const auto previous = skip_;
        skip_.periodic =
            GetPrivateProfileIntW(L"Skip", L"periodic", 0, path) != 0;
        skip_.waveform =
            GetPrivateProfileIntW(L"Skip", L"waveform", 0, path) != 0;
        skip_.keyboard =
            GetPrivateProfileIntW(L"Skip", L"keyboard", 0, path) != 0;
        skip_.hover =
            GetPrivateProfileIntW(L"Skip", L"hover", 0, path) != 0;
        if (previous.periodic != skip_.periodic) {
            noteEvent(skip_.periodic ? "skip periodic ON" : "skip periodic OFF");
        }
        if (previous.waveform != skip_.waveform) {
            noteEvent(skip_.waveform ? "skip waveform ON" : "skip waveform OFF");
        }
        if (previous.keyboard != skip_.keyboard) {
            noteEvent(skip_.keyboard ? "skip keyboard ON" : "skip keyboard OFF");
        }
        if (previous.hover != skip_.hover) {
            noteEvent(skip_.hover ? "skip hover ON" : "skip hover OFF");
        }
    }

    void noteRepaintRequest(JankRepaintKind kind, int pixels) noexcept {
        maybeFlushSecond();
        const auto index = static_cast<std::size_t>(kind);
        if (index >= kKindCount) {
            return;
        }
        ++current_.requests[index];
        if (pixels > 0) {
            current_.request_pixels += static_cast<std::uint64_t>(pixels);
        }
    }

    void notePaint(JankRepaintKind kind, int clip_pixels, int component_pixels)
        noexcept {
        maybeFlushSecond();
        const auto index = static_cast<std::size_t>(kind);
        if (index >= kKindCount) {
            return;
        }
        ++current_.paints[index];
        if (clip_pixels > 0) {
            current_.dirty_pixels += static_cast<std::uint64_t>(clip_pixels);
        }
        const int area = clip_pixels > 0 ? clip_pixels : 0;
        const int whole = component_pixels > 0 ? component_pixels : 0;
        if (whole > 0 && area * 20 >= whole * 19) {
            ++current_.full_component_paints;
        }
        if (editor_pixels_ > 0 && area * 20 >= editor_pixels_ * 19) {
            ++current_.full_editor_paints;
            if (kind != JankRepaintKind::FullEditor) {
                ++current_.paints[static_cast<std::size_t>(
                    JankRepaintKind::FullEditor)];
            }
        }
    }

    void noteEvent(const char* name) noexcept {
        if (name == nullptr || !session_started_) {
            return;
        }
        maybeFlushSecond();
        pushTimeline(name);
        countEvent(name);
    }

    void maybeFlushSecond() noexcept {
        if (!session_started_) {
            return;
        }
        const double now = nowMs();
        if (now - last_second_ms_ < 1000.0) {
            return;
        }
        const int elapsed = static_cast<int>((now - last_second_ms_) / 1000.0);
        for (int step = 0; step < elapsed && step < 8; ++step) {
            commitCurrentSecond();
            last_second_ms_ += 1000.0;
        }
        if (elapsed >= 8) {
            last_second_ms_ = now;
        }
        reloadSkipFlags();
    }

    [[nodiscard]] Stats stats(StallProbeId id) const {
        Stats out;
        const auto index = static_cast<std::size_t>(id);
        if (index >= buckets_.size()) {
            return out;
        }
        const auto& bucket = buckets_[index];
        out.count = bucket.count;
        out.sum_ms = bucket.sum_ms;
        out.max_ms = bucket.max_ms;
        out.over_10 = bucket.over_10;
        out.over_16 = bucket.over_16;
        out.over_33 = bucket.over_33;
        if (bucket.ring_n == 0) {
            return out;
        }
        std::vector<float> sorted(
            bucket.ring.begin(),
            bucket.ring.begin() + static_cast<std::ptrdiff_t>(bucket.ring_n));
        std::sort(sorted.begin(), sorted.end());
        const auto percentile = [&](double p) {
            const auto pos = static_cast<std::size_t>(
                std::clamp(
                    p * static_cast<double>(sorted.size() - 1),
                    0.0,
                    static_cast<double>(sorted.size() - 1)));
            return static_cast<double>(sorted[pos]);
        };
        out.p95_ms = percentile(0.95);
        out.p99_ms = percentile(0.99);
        return out;
    }

    [[nodiscard]] std::vector<EventCount> events() const {
        return {events_.begin(), events_.begin()
            + static_cast<std::ptrdiff_t>(event_used_)};
    }

    void formatReport(char* buffer, std::size_t bytes) const {
        if (buffer == nullptr || bytes == 0) {
            return;
        }
        std::size_t used = 0;
        const auto append = [&](const char* text) {
            const auto len = std::strlen(text);
            if (used + len + 1 >= bytes) {
                return;
            }
            std::memcpy(buffer + used, text, len);
            used += len;
            buffer[used] = '\0';
        };
        char line[384];
        append("MGSTC editor jank probe (Debug; Release compiles this out)\n");
        std::snprintf(
            line,
            sizeof(line),
            "  skip periodic=%d waveform=%d keyboard=%d hover=%d  editor_px=%d\n",
            skip_.periodic ? 1 : 0,
            skip_.waveform ? 1 : 0,
            skip_.keyboard ? 1 : 0,
            skip_.hover ? 1 : 0,
            editor_pixels_);
        append(line);
        append("  INI %LOCALAPPDATA%\\MgsToneCraft\\editor-jank.ini  "
               "try periodic=1 first\n");
        append("  stall histogram (idle after 1.5s warmup):\n");
        for (std::uint8_t i = 0;
             i < static_cast<std::uint8_t>(StallProbeId::Count);
             ++i) {
            const auto id = static_cast<StallProbeId>(i);
            const auto s = stats(id);
            if (s.count == 0) {
                continue;
            }
            std::snprintf(
                line,
                sizeof(line),
                "    %-36s n=%llu max=%.2f p95=%.2f p99=%.2f "
                ">10=%u >16=%u >33=%u ms\n",
                stallProbeName(id),
                static_cast<unsigned long long>(s.count),
                s.max_ms,
                s.p95_ms,
                s.p99_ms,
                s.over_10,
                s.over_16,
                s.over_33);
            append(line);
        }
        append("  1s invalidation (oldest -> newest):\n");
        if (second_used_ == 0) {
            append("    (waiting for first full second)\n");
        }
        const std::size_t start =
            second_used_ < kSecondSlots
                ? 0
                : second_i_;
        for (std::size_t n = 0; n < second_used_; ++n) {
            const auto& sec =
                seconds_[(start + n) % kSecondSlots];
            std::uint32_t req_total = 0;
            std::uint32_t paint_total = 0;
            for (std::size_t k = 0; k < kKindCount; ++k) {
                req_total += sec.requests[k];
                paint_total += sec.paints[k];
            }
            const double full_eq =
                editor_pixels_ > 0
                    ? static_cast<double>(sec.dirty_pixels)
                        / static_cast<double>(editor_pixels_)
                    : 0.0;
            std::snprintf(
                line,
                sizeof(line),
                "    t+%3ds req=%u paint=%u fullEd=%u fullComp=%u "
                "dirtyEq=%.2fx  req[ed=%u tl=%u wf=%u kb=%u hv=%u] "
                "paint[ed=%u tl=%u wf=%u kb=%u hv=%u]%s\n",
                sec.t_s,
                req_total,
                paint_total,
                sec.full_editor_paints,
                sec.full_component_paints,
                full_eq,
                sec.requests[0],
                sec.requests[1],
                sec.requests[2],
                sec.requests[3],
                sec.requests[4],
                sec.paints[0],
                sec.paints[1],
                sec.paints[2],
                sec.paints[3],
                sec.paints[4],
                sec.busy_note[0] != '\0' ? sec.busy_note : "");
            append(line);
        }
        append("  events:\n");
        if (timeline_used_ == 0) {
            append("    (none)\n");
        }
        const std::size_t ev_start =
            timeline_used_ < kTimelineEvents ? 0 : timeline_i_;
        for (std::size_t n = 0; n < timeline_used_; ++n) {
            const auto& ev = timeline_[(ev_start + n) % kTimelineEvents];
            std::snprintf(
                line,
                sizeof(line),
                "    +%6.2fs  %s\n",
                static_cast<double>(ev.t_s),
                ev.text);
            append(line);
        }
        append("  full-repaint / notable counts:\n");
        if (event_used_ == 0) {
            append("    (none)\n");
        }
        for (std::size_t i = 0; i < event_used_; ++i) {
            std::snprintf(
                line,
                sizeof(line),
                "    %s x%u\n",
                events_[i].name,
                events_[i].count);
            append(line);
        }
    }

    void dumpToFile(const char* path) const {
        if (path == nullptr) {
            return;
        }
        char report[32768]{};
        formatReport(report, sizeof(report));
        if (std::FILE* file = std::fopen(path, "a")) {
            std::fputs(report, file);
            std::fputc('\n', file);
            std::fclose(file);
        }
        OutputDebugStringA(report);
    }

    [[nodiscard]] static double nowMs() noexcept {
        static const double freq = [] {
            LARGE_INTEGER value{};
            QueryPerformanceFrequency(&value);
            return static_cast<double>(value.QuadPart);
        }();
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        return 1000.0 * static_cast<double>(now.QuadPart) / freq;
    }

#else
    void beginSession(double = 1500.0) noexcept {}
    void reset() noexcept {}
    void record(StallProbeId, double) noexcept {}
    void countEvent(const char*) noexcept {}
    void setEditorPixels(int) noexcept {}
    [[nodiscard]] SkipFlags skipFlags() const noexcept { return {}; }
    void reloadSkipFlags() noexcept {}
    void noteRepaintRequest(JankRepaintKind, int) noexcept {}
    void notePaint(JankRepaintKind, int, int) noexcept {}
    void noteEvent(const char*) noexcept {}
    void maybeFlushSecond() noexcept {}
    [[nodiscard]] Stats stats(StallProbeId) const { return {}; }
    [[nodiscard]] std::vector<EventCount> events() const { return {}; }
    void formatReport(char* buffer, std::size_t bytes) const {
        if (buffer != nullptr && bytes > 0) {
            buffer[0] = '\0';
        }
    }
    void dumpToFile(const char*) const {}
#endif

private:
#if MGSTC_EDITOR_DIAG
    struct Bucket {
        std::uint64_t count{};
        double sum_ms{};
        double max_ms{};
        std::uint32_t over_10{};
        std::uint32_t over_16{};
        std::uint32_t over_33{};
        std::array<float, kRing> ring{};
        std::size_t ring_i{};
        std::size_t ring_n{};
    };

    struct SecondBucket {
        int t_s{};
        std::array<std::uint32_t, kKindCount> requests{};
        std::array<std::uint32_t, kKindCount> paints{};
        std::uint32_t full_editor_paints{};
        std::uint32_t full_component_paints{};
        std::uint64_t dirty_pixels{};
        std::uint64_t request_pixels{};
        char busy_note[48]{};
    };

    struct TimelineEvent {
        float t_s{};
        char text[72]{};
    };

    static void fillJankIniPath(wchar_t* path) noexcept {
        path[0] = L'\0';
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH) == 0) {
            return;
        }
        const wchar_t suffix[] = L"\\MgsToneCraft\\editor-jank.ini";
        wcsncat_s(path, MAX_PATH, suffix, _TRUNCATE);
    }

    static void ensureJankIni() noexcept {
        wchar_t path[MAX_PATH]{};
        fillJankIniPath(path);
        wchar_t dir[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH) != 0) {
            wcsncat_s(dir, MAX_PATH, L"\\MgsToneCraft", _TRUNCATE);
            CreateDirectoryW(dir, nullptr);
        }
        if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
            return;
        }
        if (std::FILE* file = _wfopen(path, L"w")) {
            std::fputs(
                "; Debug-only Cubase jank isolation. Release compiles this out.\n"
                "; Re-read skip flags about once per second. Test [Skip] periodic=1 first.\n"
                "; [Minimal] replaces the shared editor. Change stage, then close and reopen.\n"
                "[Skip]\n"
                "periodic=0\n"
                "waveform=0\n"
                "keyboard=0\n"
                "hover=0\n"
                "[Minimal]\n"
                "enable=0\n"
                "stage=1\n"
                "opaque_root=0\n",
                file);
            std::fclose(file);
        }
    }

    void pushTimeline(const char* name) noexcept {
        if (name == nullptr) {
            return;
        }
        auto& slot = timeline_[timeline_i_];
        slot.t_s = static_cast<float>(
            (nowMs() - session_started_ms_) * 0.001);
        std::snprintf(slot.text, sizeof(slot.text), "%s", name);
        timeline_i_ = (timeline_i_ + 1) % kTimelineEvents;
        if (timeline_used_ < kTimelineEvents) {
            ++timeline_used_;
        }
    }

    void commitCurrentSecond() noexcept {
        SecondBucket committed = current_;
        committed.t_s = static_cast<int>(
            (last_second_ms_ - session_started_ms_) * 0.001);
        std::uint32_t paint_total = 0;
        for (std::size_t k = 0; k < kKindCount; ++k) {
            paint_total += committed.paints[k];
        }
        const bool busy = paint_total > 3
            || committed.requests[static_cast<std::size_t>(
                   JankRepaintKind::Waveform)]
                > 0
            || committed.requests[static_cast<std::size_t>(
                   JankRepaintKind::HoverPopup)]
                > 0;
        if (busy && !last_paint_busy_) {
            busy_since_s_ = static_cast<double>(committed.t_s);
            std::snprintf(
                committed.busy_note,
                sizeof(committed.busy_note),
                "  BUSY start");
            pushTimeline("paintBusy");
        } else if (!busy && last_paint_busy_) {
            const double lasted =
                static_cast<double>(committed.t_s) - busy_since_s_;
            std::snprintf(
                committed.busy_note,
                sizeof(committed.busy_note),
                "  idle after %.0fs",
                lasted);
            pushTimeline("paintIdle");
        }
        last_paint_busy_ = busy;
        seconds_[second_i_] = committed;
        second_i_ = (second_i_ + 1) % kSecondSlots;
        if (second_used_ < kSecondSlots) {
            ++second_used_;
        }
        current_ = {};
        char brief[240];
        std::snprintf(
            brief,
            sizeof(brief),
            "MGSTC jank t+%ds req tl=%u wf=%u kb=%u hv=%u paint tl=%u "
            "kb=%u hv=%u fullEd=%u%s\n",
            committed.t_s,
            committed.requests[1],
            committed.requests[2],
            committed.requests[3],
            committed.requests[4],
            committed.paints[1],
            committed.paints[3],
            committed.paints[4],
            committed.full_editor_paints,
            committed.busy_note);
        OutputDebugStringA(brief);
    }

    std::array<Bucket, static_cast<std::size_t>(StallProbeId::Count)>
        buckets_{};
    std::array<EventCount, kEventSlots> events_{};
    std::size_t event_used_{};
    double warmup_until_ms_{};
    bool session_started_{};
    double session_started_ms_{};
    double last_second_ms_{};
    double last_skip_reload_ms_{};
    SecondBucket current_{};
    std::array<SecondBucket, kSecondSlots> seconds_{};
    std::size_t second_used_{};
    std::size_t second_i_{};
    std::array<TimelineEvent, kTimelineEvents> timeline_{};
    std::size_t timeline_used_{};
    std::size_t timeline_i_{};
    int editor_pixels_{};
    SkipFlags skip_{};
    bool last_paint_busy_{};
    double busy_since_s_{};
#endif
};

#if MGSTC_EDITOR_DIAG
class ScopedStallProbe final {
public:
    explicit ScopedStallProbe(StallProbeId id) noexcept
        : id_(id), started_ms_(EditorStallProbe::nowMs()) {}

    ~ScopedStallProbe() {
        EditorStallProbe::instance().record(
            id_, EditorStallProbe::nowMs() - started_ms_);
    }

    ScopedStallProbe(const ScopedStallProbe&) = delete;
    ScopedStallProbe& operator=(const ScopedStallProbe&) = delete;

private:
    StallProbeId id_;
    double started_ms_;
};

#define MGSTC_STALL_PROBE(id) \
    const ::mgstc::app::ScopedStallProbe mgstc_stall_probe_scope_ { id }

inline bool editorJankSkipPeriodic() noexcept {
    return EditorStallProbe::instance().skipFlags().periodic;
}
inline bool editorJankSkipWaveform() noexcept {
    return EditorStallProbe::instance().skipFlags().waveform;
}
inline bool editorJankSkipKeyboard() noexcept {
    return EditorStallProbe::instance().skipFlags().keyboard;
}
inline bool editorJankSkipHover() noexcept {
    return EditorStallProbe::instance().skipFlags().hover;
}
inline void editorJankNoteEvent(const char* name) noexcept {
    EditorStallProbe::instance().noteEvent(name);
}
inline void editorJankNoteRepaint(JankRepaintKind kind, int pixels) noexcept {
    EditorStallProbe::instance().noteRepaintRequest(kind, pixels);
}
inline void editorJankNotePaint(
    JankRepaintKind kind,
    int clip_pixels,
    int component_pixels) noexcept {
    EditorStallProbe::instance().notePaint(kind, clip_pixels, component_pixels);
}

#define MGSTC_JANK_PAINT(kind, graphics, component)                       \
    do {                                                                  \
        const auto mgstc_jank_clip =                                      \
            (graphics).getClipBounds().getSmallestIntegerContainer();     \
        ::mgstc::app::editorJankNotePaint(                                \
            (kind),                                                       \
            mgstc_jank_clip.getWidth() * mgstc_jank_clip.getHeight(),     \
            (component).getWidth() * (component).getHeight());            \
    } while (0)
#define MGSTC_JANK_REPAINT_FULL(component, kind)                          \
    do {                                                                  \
        ::mgstc::app::editorJankNoteRepaint(                              \
            (kind),                                                       \
            (component).getWidth() * (component).getHeight());            \
        (component).repaint();                                            \
    } while (0)
#define MGSTC_JANK_REPAINT_RECT(component, rect, kind)                    \
    do {                                                                  \
        const auto mgstc_jank_r = (rect);                                 \
        ::mgstc::app::editorJankNoteRepaint(                              \
            (kind),                                                       \
            mgstc_jank_r.getWidth() * mgstc_jank_r.getHeight());          \
        (component).repaint(mgstc_jank_r);                                \
    } while (0)
#define MGSTC_JANK_EVENT(name) ::mgstc::app::editorJankNoteEvent(name)

#else
inline bool editorJankSkipPeriodic() noexcept { return false; }
inline bool editorJankSkipWaveform() noexcept { return false; }
inline bool editorJankSkipKeyboard() noexcept { return false; }
inline bool editorJankSkipHover() noexcept { return false; }
inline void editorJankNoteEvent(const char*) noexcept {}
inline void editorJankNoteRepaint(JankRepaintKind, int) noexcept {}
inline void editorJankNotePaint(JankRepaintKind, int, int) noexcept {}

#define MGSTC_STALL_PROBE(id) ((void)0)
#define MGSTC_JANK_PAINT(kind, graphics, component) ((void)0)
#define MGSTC_JANK_REPAINT_FULL(component, kind) (component).repaint()
#define MGSTC_JANK_REPAINT_RECT(component, rect, kind) \
    (component).repaint(rect)
#define MGSTC_JANK_EVENT(name) ((void)0)
#endif

}  // namespace mgstc::app
