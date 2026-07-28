#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <Windows.h>
#include <CommCtrl.h>
#include <commdlg.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <wincodec.h>
#include <windowsx.h>

#include "mgstc/audio/wasapi_audio_sink.hpp"
#include "mgstc/engine/engine_command.hpp"
#include "mgstc/engine/mgs_timbre_io.hpp"
#include "mgstc/engine/opll_envelope_trace.hpp"
#include "mgstc/engine/opll_patch.hpp"
#include "mgstc/engine/realtime_engine_host.hpp"
#include "mgstc/engine/scc_waveform.hpp"
#include "mgstc/engine/timbre_library.hpp"
#include "mgstc/engine/wave_import.hpp"

namespace {

constexpr wchar_t kMainClass[] = L"MgsToneCraft.Main";
constexpr wchar_t kKeyboardClass[] = L"MgsToneCraft.Keyboard";
constexpr wchar_t kEditorClass[] = L"MgsToneCraft.ModelessEditor";
constexpr UINT kNoteOnMessage = WM_APP + 1;
constexpr UINT kNoteOffMessage = WM_APP + 2;
constexpr UINT kMidiInputMessage = WM_APP + 3;
constexpr UINT kOpllPatchChangedMessage = WM_APP + 4;
constexpr UINT kTimbreChangedMessage = WM_APP + 5;
constexpr UINT_PTR kAudioStatusTimer = 1;
constexpr UINT_PTR kImmediateAuditionTimer = 2;
constexpr UINT_PTR kOpllScopeTimer = 3;
constexpr int kOpllButton = 1001;
constexpr int kSccButton = 1002;
constexpr int kStopButton = 1003;
constexpr int kStatusLabel = 1004;
constexpr int kPsgSourceButton = 1005;
constexpr int kSccSourceButton = 1006;
constexpr int kOpllSourceButton = 1007;
constexpr int kMidiInputCombo = 1008;
constexpr int kMidiConnectButton = 1009;
constexpr int kImmediateAuditionCheck = 2000;
constexpr int kOpllRegisterBase = 2100;
constexpr int kOpllValueBase = 2120;
constexpr int kSccSampleBase = 2200;
constexpr int kSccValueBase = 2240;
constexpr int kSccHexValueBase = 2280;
constexpr int kOpllFlagBase = 2300;
constexpr int kOpllMultiBase = 2320;
constexpr int kOpllKslBase = 2330;
constexpr int kOpllTlSlider = 2340;
constexpr int kOpllWaveBase = 2350;
constexpr int kOpllFeedbackSlider = 2360;
constexpr int kOpllRomPresetCombo = 2370;
constexpr int kOpllRomPresetLoad = 2371;
constexpr int kOpllEnvelopeBase = 2400;
constexpr int kOpllEnvelopeValueBase = 2420;
constexpr int kSccBackgroundLoad = 2500;
constexpr int kSccBackgroundVisible = 2501;
constexpr int kSccBackgroundX = 2502;
constexpr int kSccBackgroundY = 2503;
constexpr int kSccBackgroundWidth = 2504;
constexpr int kSccBackgroundHeight = 2505;
constexpr int kSccBackgroundOpacity = 2506;
constexpr int kSccPresetCombo = 2510;
constexpr int kSccHarmonicCombo = 2511;
constexpr int kSccMergeCheck = 2512;
constexpr int kSccMergeAmount = 2513;
constexpr int kSccAutoPhaseCheck = 2514;
constexpr int kSccPolarityCheck = 2515;
constexpr int kSccPreserveVolumeCheck = 2516;
constexpr int kSccApplyPreset = 2517;
constexpr int kSccAverage = 2518;
constexpr int kSccNormalize = 2519;
constexpr int kSccInvert = 2520;
constexpr int kSccRotateLeft = 2521;
constexpr int kSccRotateRight = 2522;
constexpr int kSccUndo = 2523;
constexpr int kSccRedo = 2529;
constexpr int kSccMergeAmountLabel = 2524;
constexpr int kSccOperationStatus = 2525;
constexpr int kSccCancelPreset = 2526;
constexpr int kSccHarmonicLabel = 2527;
constexpr int kSccToggleAudition = 2528;
constexpr int kTimbreImport = 2600;
constexpr int kTimbreExport = 2601;
constexpr int kTimbreExportNumber = 2602;
constexpr int kTimbreClipboardPaste = 2603;
constexpr int kTimbreClipboardCopy = 2604;
constexpr int kLibraryList = 2610;
constexpr int kLibraryName = 2611;
constexpr int kLibraryTags = 2612;
constexpr int kLibraryMemo = 2613;
constexpr int kLibraryFavorite = 2614;
constexpr int kLibraryNew = 2615;
constexpr int kLibraryLoad = 2616;
constexpr int kLibrarySave = 2617;
constexpr int kLibrarySaveAs = 2618;
constexpr int kLibraryDelete = 2619;
constexpr int kLibraryFilter = 2620;
constexpr int kLibraryImport = 2621;
constexpr int kLibraryExportSelected = 2622;
constexpr int kLibraryCancelEdit = 2623;
constexpr int kOpllMultiValueBase = 2630;
constexpr int kOpllKslValueBase = 2632;
constexpr int kOpllTlValue = 2634;
constexpr int kOpllFeedbackValue = 2635;
constexpr int kTimbreDefinitionPreview = 2640;
constexpr int kEditorAuditionButton = 2641;
constexpr int kEditorUndo = 2642;
constexpr int kEditorRedo = 2643;
constexpr int kWaveConvert = 2644;
constexpr int kSccBackgroundSliderBase = 2650;

struct TimbreHistorySnapshot {
    std::array<std::uint8_t, 8> opll{};
    std::array<std::uint8_t, 32> scc{};

    bool operator==(const TimbreHistorySnapshot&) const = default;
};

struct AppState;
void CALLBACK midiInputCallback(
    HMIDIIN,
    UINT,
    DWORD_PTR,
    DWORD_PTR,
    DWORD_PTR);

struct EditorState {
    AppState* app{};
    bool opll{};
    int graph_operator{-1};
    int graph_phase{-1};
    bool scc_dragging{};
    int scc_last_index{-1};
    int scc_last_value{};
    HBITMAP scc_background{};
    std::wstring scc_background_path;
    int scc_background_source_width{};
    int scc_background_source_height{};
    int scc_background_x{};
    int scc_background_y{};
    int scc_background_width{768};
    int scc_background_height{262};
    int scc_background_opacity{112};
    bool scc_background_visible{true};
    std::vector<TimbreHistorySnapshot> history;
    std::size_t history_cursor{};
    bool history_suspended{};
    mgstc::engine::SccWaveform scc_preview_wave{};
    bool scc_preview_active{};
    bool scc_preview_hearing_candidate{true};
    mgstc::engine::OpllEnvelopeTrace opll_envelope_trace{};
    mgstc::engine::OpllScopeFrame opll_scope{};
    bool opll_scope_available{};
    std::optional<std::uint64_t> selected_library_id;
    mgstc::engine::TimbreLibraryEntry edit_baseline{};
    std::optional<std::uint64_t> baseline_library_id;
    bool edit_baseline_valid{};
};

struct KeyboardState {
    std::optional<std::uint8_t> held_note;
};

struct AppState {
    mgstc::engine::RealtimeEngineHost engine;
    mgstc::audio::WasapiAudioSink audio;
    HWND main_window{};
    HWND keyboard{};
    HWND status{};
    HWND midi_combo{};
    HWND midi_connect{};
    HWND opll_editor{};
    HWND scc_editor{};
    EditorState opll_editor_state{this, true};
    EditorState scc_editor_state{this, false};
    std::optional<std::uint8_t> sounding_note;
    std::uint8_t last_note{60};
    int pc_octave{4};
    std::uint8_t audition_track{};
    std::array<std::uint8_t, 32> scc_wave{};
    mgstc::engine::OpllPatchParameters opll_patch{
        mgstc::engine::defaultOpllPatch()};
    mgstc::engine::TimbreLibrary timbre_library;
    bool opll_immediate_audition{true};
    bool scc_immediate_audition{true};
    bool audio_started{};
    HMIDIIN midi_input{};
    bool immediate_audition_pending{};
    bool mouse_audition_active{};

    void setStatus(const std::wstring& text) const {
        if (status) {
            SetWindowTextW(status, text.c_str());
        }
    }

    void updateEditorAuditionButtons() const {
        const std::wstring label =
            L"発声 " + noteName(last_note);
        if (opll_editor) {
            SetDlgItemTextW(
                opll_editor,
                kEditorAuditionButton,
                label.c_str());
        }
        if (scc_editor) {
            SetDlgItemTextW(
                scc_editor,
                kEditorAuditionButton,
                label.c_str());
        }
    }

    bool submitCurrentProgram(bool retrigger = false) {
        auto edit = engine.beginProgramEdit();
        if (!edit.valid()) {
            return false;
        }
        const auto opll_registers =
            mgstc::engine::encodeOpllPatch(opll_patch);
        const bool configured =
            edit.engine->session().setSequenceEnvelope(
                0,
                {0x40, 0xEF, 0x01, 0x60})
            && edit.engine->session().setSequenceEnvelope(
                3,
                {0x10, 0x00, 0x40, 0xEF, 0x01, 0x60})
            && edit.engine->session().setSequenceEnvelope(
                8,
                {0x10, 0x10, 0x40, 0xEF, 0x01, 0x60})
            && edit.engine->session().setPsgToneNoise(0, 1, 0)
            && edit.engine->session().setPsgFixedVolume(0, 15)
            && edit.engine->session().mapper().defineSccPatch(
                0,
                scc_wave) == mgstc::engine::MapError::None
            && edit.engine->session().mapper().defineOpllOriginalPatch(
                16,
                opll_registers) == mgstc::engine::MapError::None;
        if (!configured) {
            static_cast<void>(engine.discardProgramEdit(edit));
            return false;
        }
        return engine.submitProgram(
            edit,
            {
                .retrigger = retrigger,
                .track = audition_track,
                .midi_note = last_note,
            });
    }

    bool prepareDefaultProgram() {
        for (std::size_t index = 0; index < scc_wave.size(); ++index) {
            scc_wave[index] = index < 16 ? 0x7F : 0x81;
        }
        return submitCurrentProgram(false);
    }

    void auditionEditedProgram(bool opll) {
        mouse_audition_active = false;
        if (opll && opll_editor) {
            SendMessageW(
                opll_editor,
                kOpllPatchChangedMessage,
                0,
                0);
        } else if (!opll && scc_editor) {
            SendMessageW(
                scc_editor,
                kTimbreChangedMessage,
                0,
                0);
        }
        const auto editor_track = static_cast<std::uint8_t>(opll ? 8 : 3);
        const bool retrigger =
            opll ? opll_immediate_audition : scc_immediate_audition;
        if (retrigger) {
            audition_track = editor_track;
            CheckRadioButton(
                main_window,
                kPsgSourceButton,
                kOpllSourceButton,
                opll ? kOpllSourceButton : kSccSourceButton);
        }
        if (!submitCurrentProgram(retrigger)) {
            setStatus(L"音色変更の反映待ちです");
            return;
        }
        if (retrigger) {
            sounding_note = last_note;
            immediate_audition_pending = true;
            SetTimer(
                main_window,
                kImmediateAuditionTimer,
                1000,
                nullptr);
        }
        setStatus(
            std::wstring(opll ? L"OPLL" : L"SCC")
            + (retrigger ? L" 音色を更新して再発声" : L" 音色を更新"));
    }

    [[nodiscard]] const wchar_t* sourceName() const noexcept {
        if (audition_track == 3) {
            return L"SCC Ch.D";
        }
        if (audition_track == 8) {
            return L"OPLL Ch.I";
        }
        return L"PSG Ch.A";
    }

    void selectSource(std::uint8_t track) {
        noteOff();
        audition_track = track;
        setStatus(std::wstring(L"試聴音源: ") + sourceName());
    }

    void auditionEditor(bool opll) {
        const auto track = static_cast<std::uint8_t>(opll ? 8 : 3);
        selectSource(track);
        if (!submitCurrentProgram(false)) {
            setStatus(L"試聴プログラムを反映できませんでした");
            return;
        }
        noteOn(last_note);
        immediate_audition_pending = true;
        SetTimer(
            main_window,
            kImmediateAuditionTimer,
            1000,
            nullptr);
    }

    void noteOn(std::uint8_t note) {
        if (immediate_audition_pending) {
            KillTimer(main_window, kImmediateAuditionTimer);
            immediate_audition_pending = false;
        }
        if (sounding_note == note) {
            return;
        }
        if (sounding_note) {
            static_cast<void>(engine.submit(
                mgstc::engine::EngineCommand::noteOff(audition_track)));
        }
        if (engine.submit(
                mgstc::engine::EngineCommand::noteOn(
                    audition_track,
                    note))) {
            sounding_note = note;
            last_note = note;
            updateEditorAuditionButtons();
            if (audition_track == 8 && opll_editor) {
                SendMessageW(
                    opll_editor,
                    kOpllPatchChangedMessage,
                    0,
                    0);
            }
            setStatus(
                std::wstring(L"発声中  ")
                + noteName(note)
                + L"  |  "
                + sourceName());
        } else {
            setStatus(L"コマンドキューが満杯です");
        }
        if (keyboard) {
            InvalidateRect(keyboard, nullptr, FALSE);
        }
    }

    void noteOff(std::optional<std::uint8_t> note = std::nullopt) {
        if (!sounding_note || (note && note != sounding_note)) {
            return;
        }
        static_cast<void>(engine.submit(
            mgstc::engine::EngineCommand::noteOff(audition_track)));
        sounding_note.reset();
        if (immediate_audition_pending) {
            KillTimer(main_window, kImmediateAuditionTimer);
            immediate_audition_pending = false;
        }
        setStatus(L"Key Off  |  Ready");
        if (keyboard) {
            InvalidateRect(keyboard, nullptr, FALSE);
        }
    }

    void populateMidiInputs() {
        if (!midi_combo) {
            return;
        }
        SendMessageW(midi_combo, CB_RESETCONTENT, 0, 0);
        SendMessageW(
            midi_combo,
            CB_ADDSTRING,
            0,
            reinterpret_cast<LPARAM>(L"MIDI入力なし"));
        const auto count = midiInGetNumDevs();
        for (UINT index = 0; index < count; ++index) {
            MIDIINCAPSW caps{};
            const auto result = midiInGetDevCapsW(
                index,
                &caps,
                sizeof(caps));
            if (result == MMSYSERR_NOERROR) {
                const auto item = SendMessageW(
                    midi_combo,
                    CB_ADDSTRING,
                    0,
                    reinterpret_cast<LPARAM>(caps.szPname));
                if (item != CB_ERR && item != CB_ERRSPACE) {
                    SendMessageW(
                        midi_combo,
                        CB_SETITEMDATA,
                        static_cast<WPARAM>(item),
                        static_cast<LPARAM>(index));
                }
            }
        }
        SendMessageW(midi_combo, CB_SETCURSEL, 0, 0);
    }

    void closeMidiInput() {
        noteOff();
        if (midi_input) {
            midiInStop(midi_input);
            midiInReset(midi_input);
            midiInClose(midi_input);
            midi_input = nullptr;
        }
        if (midi_connect) {
            SetWindowTextW(midi_connect, L"接続");
        }
    }

    bool openSelectedMidiInput() {
        closeMidiInput();
        const auto selected = static_cast<int>(
            SendMessageW(midi_combo, CB_GETCURSEL, 0, 0));
        if (selected <= 0) {
            setStatus(L"MIDI入力ポートを選択してください");
            return false;
        }
        const auto device_id = SendMessageW(
            midi_combo,
            CB_GETITEMDATA,
            static_cast<WPARAM>(selected),
            0);
        if (device_id == CB_ERR) {
            setStatus(L"MIDI入力ポートを取得できませんでした");
            return false;
        }
        const auto result = midiInOpen(
            &midi_input,
            static_cast<UINT>(device_id),
            reinterpret_cast<DWORD_PTR>(&midiInputCallback),
            reinterpret_cast<DWORD_PTR>(main_window),
            CALLBACK_FUNCTION);
        if (result != MMSYSERR_NOERROR) {
            midi_input = nullptr;
            setStatus(
                L"MIDI入力を開けません: "
                + std::to_wstring(result));
            return false;
        }
        if (midiInStart(midi_input) != MMSYSERR_NOERROR) {
            closeMidiInput();
            setStatus(L"MIDI入力を開始できませんでした");
            return false;
        }
        SetWindowTextW(midi_connect, L"切断");
        setStatus(L"MIDI入力を接続しました");
        return true;
    }

    [[nodiscard]] static std::wstring noteName(std::uint8_t note) {
        constexpr std::array<const wchar_t*, 12> names{
            L"C", L"C#", L"D", L"D#", L"E", L"F",
            L"F#", L"G", L"G#", L"A", L"A#", L"B"};
        return std::wstring(names[note % 12])
            + std::to_wstring(static_cast<int>(note / 12) - 1);
    }
};

void CALLBACK midiInputCallback(
    HMIDIIN,
    UINT message,
    DWORD_PTR instance,
    DWORD_PTR param1,
    DWORD_PTR) {
    if ((message != MIM_DATA && message != MIM_MOREDATA)
        || instance == 0) {
        return;
    }
    PostMessageW(
        reinterpret_cast<HWND>(instance),
        kMidiInputMessage,
        static_cast<WPARAM>(param1 & 0xFFFFFFFFU),
        0);
}

std::optional<std::uint8_t> pcKeyNote(WPARAM key, int octave) {
    const auto base = static_cast<std::uint8_t>((octave + 1) * 12);
    int offset = -1;
    switch (key) {
    case 'A': offset = 0; break;
    case 'W': offset = 1; break;
    case 'S': offset = 2; break;
    case 'E': offset = 3; break;
    case 'D': offset = 4; break;
    case 'F': offset = 5; break;
    case 'T': offset = 6; break;
    case 'G': offset = 7; break;
    case 'Y': offset = 8; break;
    case 'H': offset = 9; break;
    case 'U': offset = 10; break;
    case 'J': offset = 11; break;
    case 'K': offset = 12; break;
    default: break;
    }
    const int note = static_cast<int>(base) + offset;
    return offset >= 0 && note >= 24 && note <= 119
        ? std::optional<std::uint8_t>(static_cast<std::uint8_t>(note))
        : std::nullopt;
}

RECT whiteKeyRect(int index, const RECT& client) {
    constexpr int kWhiteKeyCount = 56;
    const int width = (client.right - client.left) / kWhiteKeyCount;
    return {
        client.left + index * width,
        client.top,
        index == kWhiteKeyCount - 1
            ? client.right
            : client.left + (index + 1) * width,
        client.bottom,
    };
}

std::optional<std::uint8_t> keyboardHitTest(
    POINT point,
    const RECT& client) {
    constexpr int kWhiteKeyCount = 56;
    const int white_width =
        (client.right - client.left) / kWhiteKeyCount;
    constexpr std::array<int, 5> boundaries{1, 2, 4, 5, 6};
    constexpr std::array<int, 5> black_offsets{1, 3, 6, 8, 10};
    const int black_width = std::max(8, white_width * 3 / 5);
    const int black_height = (client.bottom - client.top) * 2 / 3;
    for (int octave = 0; octave < 8; ++octave) {
        for (std::size_t index = 0; index < boundaries.size(); ++index) {
            const int boundary = octave * 7 + boundaries[index];
            const int center = client.left + boundary * white_width;
            const RECT key{
                center - black_width / 2,
                client.top,
                center + black_width / 2,
                client.top + black_height,
            };
            if (PtInRect(&key, point)) {
                return static_cast<std::uint8_t>(
                    24 + octave * 12 + black_offsets[index]);
            }
        }
    }
    constexpr std::array<int, 7> white_offsets{0, 2, 4, 5, 7, 9, 11};
    for (int index = 0; index < kWhiteKeyCount; ++index) {
        const auto key = whiteKeyRect(index, client);
        if (PtInRect(&key, point)) {
            return static_cast<std::uint8_t>(
                24 + (index / 7) * 12
                + white_offsets[static_cast<std::size_t>(index % 7)]);
        }
    }
    return std::nullopt;
}

void paintKeyboard(HWND window, KeyboardState* state) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(DKGRAY_BRUSH)));
    constexpr std::array<int, 7> white_offsets{0, 2, 4, 5, 7, 9, 11};
    constexpr std::array<wchar_t, 7> white_labels{
        L'A', L'S', L'D', L'F', L'G', L'H', L'J'};
    const auto* app = reinterpret_cast<AppState*>(
        GetWindowLongPtrW(GetParent(window), GWLP_USERDATA));
    for (int index = 0; index < 56; ++index) {
        const auto key = whiteKeyRect(index, client);
        const auto note = static_cast<std::uint8_t>(
            24 + (index / 7) * 12
            + white_offsets[static_cast<std::size_t>(index % 7)]);
        const bool pressed =
            (state && state->held_note == note)
            || (app && app->sounding_note == note);
        HBRUSH brush = CreateSolidBrush(
            pressed ? RGB(140, 196, 234) : RGB(248, 248, 246));
        FillRect(dc, &key, brush);
        DeleteObject(brush);
        FrameRect(dc, &key, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        RECT label = key;
        label.top = label.bottom - 28;
        const bool pc_octave_key =
            app && index / 7 == app->pc_octave - 1;
        const bool pc_top_c =
            app && app->pc_octave < 8
            && index / 7 == app->pc_octave
            && index % 7 == 0;
        if (pc_octave_key || pc_top_c) {
            wchar_t text[]{
                pc_top_c
                    ? L'K'
                    : white_labels[static_cast<std::size_t>(index % 7)],
                L'\0'};
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(30, 30, 30));
            DrawTextW(
                dc, text, 1, &label,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }

    const int white_width = (client.right - client.left) / 56;
    constexpr std::array<int, 5> boundaries{1, 2, 4, 5, 6};
    constexpr std::array<int, 5> black_offsets{1, 3, 6, 8, 10};
    constexpr std::array<wchar_t, 5> black_labels{
        L'W', L'E', L'T', L'Y', L'U'};
    const int black_width = std::max(8, white_width * 3 / 5);
    const int black_height = (client.bottom - client.top) * 2 / 3;
    for (int octave = 0; octave < 8; ++octave) {
        for (std::size_t index = 0; index < boundaries.size(); ++index) {
            const int boundary = octave * 7 + boundaries[index];
            const int center = client.left + boundary * white_width;
            RECT key{
                center - black_width / 2,
                client.top,
                center + black_width / 2,
                client.top + black_height,
            };
            const auto note = static_cast<std::uint8_t>(
                24 + octave * 12 + black_offsets[index]);
            const bool pressed =
                (state && state->held_note == note)
                || (app && app->sounding_note == note);
            HBRUSH brush = CreateSolidBrush(
                pressed ? RGB(65, 145, 198) : RGB(28, 30, 34));
            FillRect(dc, &key, brush);
            DeleteObject(brush);
            FrameRect(
                dc, &key,
                static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            if (app && octave == app->pc_octave - 1) {
                wchar_t text[]{black_labels[index], L'\0'};
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, RGB(240, 240, 240));
                DrawTextW(
                    dc, text, 1, &key,
                    DT_CENTER | DT_BOTTOM | DT_SINGLELINE);
            }
        }
    }
    EndPaint(window, &paint);
}

LRESULT CALLBACK keyboardProcedure(
    HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam) {
    auto* state = reinterpret_cast<KeyboardState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        auto* created = new KeyboardState{};
        SetWindowLongPtrW(
            window,
            GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(created));
        return TRUE;
    }
    case WM_NCDESTROY:
        if (state && state->held_note) {
            SendMessageW(
                GetParent(window),
                kNoteOffMessage,
                0,
                0);
        }
        delete state;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    case WM_PAINT:
        paintKeyboard(window, state);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_MOUSEMOVE: {
        if (message == WM_MOUSEMOVE && !(wparam & MK_LBUTTON)) {
            break;
        }
        RECT client{};
        GetClientRect(window, &client);
        const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        const auto note = keyboardHitTest(point, client);
        if (note && state && state->held_note != note) {
            SetFocus(window);
            state->held_note = note;
            SetCapture(window);
            SendMessageW(GetParent(window), kNoteOnMessage, *note, 0);
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
    case WM_CANCELMODE:
    case WM_KILLFOCUS:
        if (state && state->held_note) {
            SendMessageW(
                GetParent(window),
                kNoteOffMessage,
                0,
                0);
            state->held_note.reset();
            InvalidateRect(window, nullptr, FALSE);
        }
        if (GetCapture() == window) {
            ReleaseCapture();
        }
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

mgstc::engine::OpllOperatorParameters& opllOperator(
    AppState& app,
    int index) {
    return index == 0
        ? app.opll_patch.modulator
        : app.opll_patch.carrier;
}

std::uint8_t opllEnvelopeValue(
    const mgstc::engine::OpllOperatorParameters& parameters,
    int phase) {
    switch (phase) {
    case 0: return parameters.attack_rate;
    case 1: return parameters.decay_rate;
    case 2: return parameters.sustain_level;
    default: return parameters.release_rate;
    }
}

void setOpllEnvelopeValue(
    mgstc::engine::OpllOperatorParameters& parameters,
    int phase,
    std::uint8_t value) {
    switch (phase) {
    case 0: parameters.attack_rate = value; break;
    case 1: parameters.decay_rate = value; break;
    case 2: parameters.sustain_level = value; break;
    default: parameters.release_rate = value; break;
    }
}

void setUnsignedDialogValue(HWND window, int id, int value) {
    wchar_t text[16]{};
    swprintf_s(text, L"%d", value);
    SetDlgItemTextW(window, id, text);
}

void syncOpllEditorControls(
    const EditorState& state,
    HWND window) {
    for (int operator_index = 0; operator_index < 2;
         ++operator_index) {
        const auto& parameters = operator_index == 0
            ? state.app->opll_patch.modulator
            : state.app->opll_patch.carrier;
        const std::array<bool, 4> flags{
            parameters.amplitude_modulation,
            parameters.pitch_modulation,
            parameters.sustained_tone,
            parameters.key_rate_scaling};
        for (int flag = 0; flag < 4; ++flag) {
            CheckDlgButton(
                window,
                kOpllFlagBase + operator_index * 4 + flag,
                flags[static_cast<std::size_t>(flag)]
                    ? BST_CHECKED
                    : BST_UNCHECKED);
        }
        SendDlgItemMessageW(
            window,
            kOpllMultiBase + operator_index,
            TBM_SETPOS,
            TRUE,
            parameters.multiplier);
        setUnsignedDialogValue(
            window,
            kOpllMultiValueBase + operator_index,
            parameters.multiplier);
        SendDlgItemMessageW(
            window,
            kOpllKslBase + operator_index,
            TBM_SETPOS,
            TRUE,
            parameters.key_scale_level);
        setUnsignedDialogValue(
            window,
            kOpllKslValueBase + operator_index,
            parameters.key_scale_level);
        CheckDlgButton(
            window,
            kOpllWaveBase + operator_index,
            parameters.waveform ? BST_CHECKED : BST_UNCHECKED);
        for (int phase = 0; phase < 4; ++phase) {
            const int offset = operator_index * 4 + phase;
            const int value = opllEnvelopeValue(parameters, phase);
            SendDlgItemMessageW(
                window,
                kOpllEnvelopeBase + offset,
                TBM_SETPOS,
                TRUE,
                15 - value);
            wchar_t text[4]{};
            swprintf_s(text, L"%d", value);
            SetDlgItemTextW(
                window,
                kOpllEnvelopeValueBase + offset,
                text);
        }
    }
    SendDlgItemMessageW(
        window,
        kOpllTlSlider,
        TBM_SETPOS,
        TRUE,
        state.app->opll_patch.modulator.total_level);
    setUnsignedDialogValue(
        window,
        kOpllTlValue,
        state.app->opll_patch.modulator.total_level);
    SendDlgItemMessageW(
        window,
        kOpllFeedbackSlider,
        TBM_SETPOS,
        TRUE,
        state.app->opll_patch.feedback);
    setUnsignedDialogValue(
        window,
        kOpllFeedbackValue,
        state.app->opll_patch.feedback);
    InvalidateRect(window, nullptr, FALSE);
}

void refreshOpllEnvelopeTrace(EditorState& state) {
    state.opll_envelope_trace =
        mgstc::engine::traceOpllEnvelope(
            state.app->opll_patch,
            state.app->last_note);
}

void paintOpllEnvelope(
    HDC dc,
    const RECT& graph,
    const std::array<
        float,
        mgstc::engine::OpllEnvelopeTrace::kPointCount>& levels,
    bool valid,
    COLORREF color) {
    HBRUSH background = CreateSolidBrush(RGB(248, 249, 250));
    FillRect(dc, &graph, background);
    DeleteObject(background);
    FrameRect(dc, &graph, static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));

    HPEN grid = CreatePen(PS_SOLID, 1, RGB(222, 226, 230));
    const auto previous_grid = SelectObject(dc, grid);
    for (int second = 1; second < 4; ++second) {
        const int x = graph.left
            + second * (graph.right - graph.left) / 4;
        MoveToEx(dc, x, graph.top + 1, nullptr);
        LineTo(dc, x, graph.bottom - 1);
    }
    SelectObject(dc, previous_grid);
    DeleteObject(grid);

    const int key_off_x = graph.left
        + static_cast<int>(
            mgstc::engine::OpllEnvelopeTrace::kKeyOffSeconds
            * (graph.right - graph.left)
            / mgstc::engine::OpllEnvelopeTrace::kDurationSeconds);
    HPEN key_off_pen = CreatePen(PS_DOT, 1, RGB(190, 70, 70));
    const auto previous_key_off = SelectObject(dc, key_off_pen);
    MoveToEx(dc, key_off_x, graph.top + 1, nullptr);
    LineTo(dc, key_off_x, graph.bottom - 1);
    SelectObject(dc, previous_key_off);
    DeleteObject(key_off_pen);

    if (!valid) {
        return;
    }
    std::array<
        POINT,
        mgstc::engine::OpllEnvelopeTrace::kPointCount> points{};
    const int width = graph.right - graph.left - 2;
    const int height = graph.bottom - graph.top - 8;
    for (std::size_t index = 0; index < levels.size(); ++index) {
        const float level = std::clamp(levels[index], 0.0F, 1.0F);
        points[index] = {
            graph.left + 1
                + static_cast<int>(
                    index * static_cast<std::size_t>(width)
                    / (levels.size() - 1)),
            graph.bottom - 4
                - static_cast<int>(level * height),
        };
    }
    HPEN pen = CreatePen(PS_SOLID, 2, color);
    const auto old_pen = SelectObject(dc, pen);
    Polyline(dc, points.data(), static_cast<int>(points.size()));
    SelectObject(dc, old_pen);
    DeleteObject(pen);
}

RECT opllScopeGraphRect() {
    return RECT{24, 656, 750, 810};
}

void paintOpllScope(
    HDC dc,
    const mgstc::engine::OpllScopeFrame& frame,
    bool available) {
    const auto graph = opllScopeGraphRect();
    HBRUSH background = CreateSolidBrush(RGB(20, 25, 30));
    FillRect(dc, &graph, background);
    DeleteObject(background);
    FrameRect(dc, &graph, static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));

    HPEN grid = CreatePen(PS_SOLID, 1, RGB(55, 65, 72));
    const auto old_pen = SelectObject(dc, grid);
    const int middle = (graph.top + graph.bottom) / 2;
    MoveToEx(dc, graph.left + 1, middle, nullptr);
    LineTo(dc, graph.right - 1, middle);
    for (int division = 1; division < 6; ++division) {
        const int x = graph.left
            + division * (graph.right - graph.left) / 6;
        MoveToEx(dc, x, graph.top + 1, nullptr);
        LineTo(dc, x, graph.bottom - 1);
    }
    SelectObject(dc, old_pen);
    DeleteObject(grid);

    if (!available) {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(155, 165, 172));
        RECT text = graph;
        DrawTextW(
            dc,
            L"OPLLを発声するとemu2413の実波形を表示します",
            -1,
            &text,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    std::array<POINT, mgstc::engine::OpllScopeFrame::kSampleCount>
        points{};
    const int width = graph.right - graph.left - 2;
    const int half_height = (graph.bottom - graph.top - 4) / 2;
    float peak{};
    for (const float sample : frame.samples) {
        peak = std::max(peak, std::abs(sample));
    }
    const float display_gain = peak > 0.0001F
        ? 0.9F / peak
        : 1.0F;
    for (std::size_t index = 0; index < frame.samples.size(); ++index) {
        const float sample = std::clamp(
            frame.samples[index] * display_gain,
            -1.0F,
            1.0F);
        points[index] = {
            graph.left + 1
                + static_cast<int>(
                    index * static_cast<std::size_t>(width)
                    / (frame.samples.size() - 1)),
            middle - static_cast<int>(sample * half_height),
        };
    }
    HPEN waveform = CreatePen(PS_SOLID, 1, RGB(80, 230, 145));
    const auto previous = SelectObject(dc, waveform);
    Polyline(dc, points.data(), static_cast<int>(points.size()));
    SelectObject(dc, previous);
    DeleteObject(waveform);
}

RECT opllEnvelopeGraphRect(int operator_index) {
    return operator_index == 0
        ? RECT{24, 526, 352, 620}
        : RECT{404, 526, 732, 620};
}

bool updateOpllEnvelopeFromGraph(
    EditorState& state,
    HWND window,
    POINT point) {
    if (state.graph_operator < 0 || state.graph_phase < 0) {
        return false;
    }
    const auto graph = opllEnvelopeGraphRect(state.graph_operator);
    const int width = std::max<int>(
        1,
        static_cast<int>(graph.right - graph.left - 1));
    const int height = std::max<int>(
        1,
        static_cast<int>(graph.bottom - graph.top - 1));
    const int x = std::clamp<int>(
        point.x,
        graph.left,
        graph.right - 1);
    const int y = std::clamp<int>(
        point.y,
        graph.top,
        graph.bottom - 1);
    int value{};
    if (state.graph_phase == 2) {
        value = ((y - graph.top) * 15 + height / 2) / height;
    } else {
        const int horizontal =
            ((x - graph.left) * 15 + width / 2) / width;
        value = state.graph_phase == 3
            ? horizontal
            : 15 - horizontal;
    }
    value = std::clamp(value, 0, 15);

    auto& parameters =
        opllOperator(*state.app, state.graph_operator);
    if (opllEnvelopeValue(parameters, state.graph_phase) == value) {
        return false;
    }
    setOpllEnvelopeValue(
        parameters,
        state.graph_phase,
        static_cast<std::uint8_t>(value));
    const int offset =
        state.graph_operator * 4 + state.graph_phase;
    SendDlgItemMessageW(
        window,
        kOpllEnvelopeBase + offset,
        TBM_SETPOS,
        TRUE,
        15 - value);
    wchar_t text[4]{};
    swprintf_s(text, L"%d", value);
    SetDlgItemTextW(
        window,
        kOpllEnvelopeValueBase + offset,
        text);
    InvalidateRect(window, nullptr, FALSE);
    state.app->auditionEditedProgram(true);
    return true;
}

RECT sccWaveGraphRect() {
    return RECT{24, 88, 792, 350};
}

HBITMAP loadBackgroundBitmap(
    const wchar_t* path,
    int& width,
    int& height) {
    IWICImagingFactory* factory{};
    IWICBitmapDecoder* decoder{};
    IWICBitmapFrameDecode* frame{};
    IWICFormatConverter* converter{};
    HBITMAP bitmap{};
    void* pixels{};
    UINT source_width{};
    UINT source_height{};

    HRESULT result = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));
    if (SUCCEEDED(result)) {
        result = factory->CreateDecoderFromFilename(
            path,
            nullptr,
            GENERIC_READ,
            WICDecodeMetadataCacheOnLoad,
            &decoder);
    }
    if (SUCCEEDED(result)) {
        result = decoder->GetFrame(0, &frame);
    }
    if (SUCCEEDED(result)) {
        result = frame->GetSize(&source_width, &source_height);
    }
    if (SUCCEEDED(result)) {
        result = factory->CreateFormatConverter(&converter);
    }
    if (SUCCEEDED(result)) {
        result = converter->Initialize(
            frame,
            GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom);
    }
    if (SUCCEEDED(result)
        && source_width > 0
        && source_height > 0
        && source_width <= 16384
        && source_height <= 16384) {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = static_cast<LONG>(source_width);
        info.bmiHeader.biHeight = -static_cast<LONG>(source_height);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        bitmap = CreateDIBSection(
            nullptr,
            &info,
            DIB_RGB_COLORS,
            &pixels,
            nullptr,
            0);
        if (bitmap) {
            const UINT stride = source_width * 4;
            result = converter->CopyPixels(
                nullptr,
                stride,
                stride * source_height,
                static_cast<BYTE*>(pixels));
            if (FAILED(result)) {
                DeleteObject(bitmap);
                bitmap = nullptr;
            }
        }
    }

    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (factory) factory->Release();
    if (bitmap) {
        width = static_cast<int>(source_width);
        height = static_cast<int>(source_height);
    }
    return bitmap;
}

void setSignedDialogValue(HWND window, int id, int value) {
    wchar_t text[16]{};
    swprintf_s(text, L"%d", value);
    SetDlgItemTextW(window, id, text);
}

void setSccDialogValue(HWND window, int index, std::uint8_t value) {
    setSignedDialogValue(
        window,
        kSccValueBase + index,
        static_cast<int>(static_cast<std::int8_t>(value)));
    wchar_t hexadecimal[4]{};
    swprintf_s(
        hexadecimal,
        L"%02X",
        static_cast<unsigned int>(value));
    SetDlgItemTextW(
        window,
        kSccHexValueBase + index,
        hexadecimal);
}

mgstc::engine::SccWaveform currentSccWaveform(
    const AppState& app) {
    mgstc::engine::SccWaveform waveform{};
    for (std::size_t index = 0; index < waveform.size(); ++index) {
        waveform[index] = static_cast<std::int8_t>(app.scc_wave[index]);
    }
    return waveform;
}

void applySccWaveform(
    EditorState& state,
    HWND window,
    const mgstc::engine::SccWaveform& waveform,
    bool audition = true);

void captureSccUndo(EditorState& state) {
    // History is recorded after each completed model mutation.  Existing
    // callers retain this pre-mutation hook so a drag can initialize history.
    if (state.history.empty()) {
        TimbreHistorySnapshot snapshot{};
        snapshot.opll =
            mgstc::engine::encodeOpllPatch(state.app->opll_patch);
        snapshot.scc = state.app->scc_wave;
        state.history.push_back(snapshot);
        state.history_cursor = 0;
    }
}

TimbreHistorySnapshot currentHistorySnapshot(
    const EditorState& state) {
    return {
        .opll =
            mgstc::engine::encodeOpllPatch(state.app->opll_patch),
        .scc = state.app->scc_wave,
    };
}

void updateHistoryButtons(EditorState& state) {
    HWND window = state.opll
        ? state.app->opll_editor
        : state.app->scc_editor;
    if (!window) {
        return;
    }
    const bool can_undo =
        !state.history.empty() && state.history_cursor > 0;
    const bool can_redo =
        !state.history.empty()
        && state.history_cursor + 1 < state.history.size();
    if (!state.opll) {
        EnableWindow(GetDlgItem(window, kSccUndo), can_undo);
        EnableWindow(GetDlgItem(window, kSccRedo), can_redo);
    }
    EnableWindow(GetDlgItem(window, kEditorUndo), can_undo);
    EnableWindow(GetDlgItem(window, kEditorRedo), can_redo);
}

void initializeEditorHistory(EditorState& state) {
    state.history.clear();
    state.history.push_back(currentHistorySnapshot(state));
    state.history_cursor = 0;
    updateHistoryButtons(state);
}

void recordEditorHistory(EditorState& state) {
    if (state.history_suspended) {
        return;
    }
    const auto snapshot = currentHistorySnapshot(state);
    if (state.history.empty()) {
        state.history.push_back(snapshot);
        state.history_cursor = 0;
    } else if (state.history[state.history_cursor] != snapshot) {
        state.history.erase(
            state.history.begin()
                + static_cast<std::ptrdiff_t>(state.history_cursor + 1),
            state.history.end());
        state.history.push_back(snapshot);
        state.history_cursor = state.history.size() - 1;
        // 257 states represent the current state plus 256 undo transitions.
        if (state.history.size() > 257) {
            state.history.erase(state.history.begin());
            --state.history_cursor;
        }
    }
    updateHistoryButtons(state);
}

void restoreEditorHistory(
    EditorState& state,
    HWND window,
    std::size_t cursor) {
    if (cursor >= state.history.size()) {
        return;
    }
    state.history_suspended = true;
    state.history_cursor = cursor;
    const auto snapshot = state.history[cursor];
    if (state.opll) {
        state.app->opll_patch =
            mgstc::engine::decodeOpllPatch(snapshot.opll);
        syncOpllEditorControls(state, window);
        state.app->auditionEditedProgram(true);
    } else {
        state.app->scc_wave = snapshot.scc;
        applySccWaveform(
            state,
            window,
            currentSccWaveform(*state.app));
    }
    state.history_suspended = false;
    updateHistoryButtons(state);
}

bool undoEditorHistory(EditorState& state, HWND window) {
    if (state.history.empty() || state.history_cursor == 0) {
        return false;
    }
    restoreEditorHistory(state, window, state.history_cursor - 1);
    return true;
}

bool redoEditorHistory(EditorState& state, HWND window) {
    if (state.history.empty()
        || state.history_cursor + 1 >= state.history.size()) {
        return false;
    }
    restoreEditorHistory(state, window, state.history_cursor + 1);
    return true;
}

void updateSccPreviewAuditionButton(
    const EditorState& state,
    HWND window) {
    HWND button = GetDlgItem(window, kSccToggleAudition);
    if (!button) {
        return;
    }
    EnableWindow(button, state.scc_preview_active ? TRUE : FALSE);
    SetWindowTextW(
        button,
        !state.scc_preview_active
            ? L"A/B試聴"
            : state.scc_preview_hearing_candidate
                ? L"A 原音を試聴"
                : L"B 候補を試聴");
}

void applySccWaveform(
    EditorState& state,
    HWND window,
    const mgstc::engine::SccWaveform& waveform,
    bool audition) {
    state.scc_preview_active = false;
    state.scc_preview_hearing_candidate = true;
    updateSccPreviewAuditionButton(state, window);
    for (std::size_t index = 0; index < waveform.size(); ++index) {
        state.app->scc_wave[index] =
            static_cast<std::uint8_t>(waveform[index]);
        setSccDialogValue(
            window,
            static_cast<int>(index),
            static_cast<std::uint8_t>(waveform[index]));
    }
    InvalidateRect(window, nullptr, FALSE);
    if (audition) {
        state.app->auditionEditedProgram(false);
    }
}

void setSccOperationStatus(
    HWND window,
    const std::wstring& text) {
    SetDlgItemTextW(window, kSccOperationStatus, text.c_str());
}

mgstc::engine::SccWavePreset selectedSccPreset(
    HWND window) {
    const int selection = std::clamp<int>(
        static_cast<int>(SendDlgItemMessageW(
            window, kSccPresetCombo, CB_GETCURSEL, 0, 0)),
        0,
        5);
    return static_cast<mgstc::engine::SccWavePreset>(selection);
}

mgstc::engine::SccHarmonic selectedSccHarmonic(
    HWND window) {
    const int selection = std::clamp<int>(
        static_cast<int>(SendDlgItemMessageW(
            window, kSccHarmonicCombo, TBM_GETPOS, 0, 0)),
        0,
        2);
    return static_cast<mgstc::engine::SccHarmonic>(selection);
}

void auditionSccPreview(
    EditorState& state,
    const mgstc::engine::SccWaveform& waveform,
    bool force = false) {
    if (!force && !state.app->scc_immediate_audition) {
        return;
    }
    const auto saved = state.app->scc_wave;
    const bool previous_history_suspension = state.history_suspended;
    state.history_suspended = true;
    for (std::size_t index = 0; index < waveform.size(); ++index) {
        state.app->scc_wave[index] =
            static_cast<std::uint8_t>(waveform[index]);
    }
    const bool immediate_setting =
        state.app->scc_immediate_audition;
    if (force) {
        state.app->scc_immediate_audition = true;
    }
    state.app->auditionEditedProgram(false);
    state.app->scc_immediate_audition = immediate_setting;
    state.app->scc_wave = saved;
    if (state.app->scc_editor) {
        SendMessageW(
            state.app->scc_editor,
            kTimbreChangedMessage,
            0,
            0);
    }
    state.history_suspended = previous_history_suspension;
}

void refreshSccPresetPreview(
    EditorState& state,
    HWND window,
    bool audition) {
    const auto preset =
        mgstc::engine::generateSccPreset(
            selectedSccPreset(window),
            selectedSccHarmonic(window));
    if (IsDlgButtonChecked(window, kSccMergeCheck) == BST_CHECKED) {
        const int amount = static_cast<int>(SendDlgItemMessageW(
            window, kSccMergeAmount, TBM_GETPOS, 0, 0));
        const auto merged = mgstc::engine::mergeSccWaveforms(
            currentSccWaveform(*state.app),
            preset,
            {
                .amount = amount / 100.0,
                .auto_phase =
                    IsDlgButtonChecked(
                        window, kSccAutoPhaseCheck) == BST_CHECKED,
                .allow_polarity_inversion =
                    IsDlgButtonChecked(
                        window, kSccPolarityCheck) == BST_CHECKED,
                .preserve_volume =
                    IsDlgButtonChecked(
                        window, kSccPreserveVolumeCheck) == BST_CHECKED,
            });
        state.scc_preview_wave = merged.waveform;
        setSccOperationStatus(
            window,
            std::wstring(L"プレビュー: 位相 ")
                + std::to_wstring(merged.circular_shift)
                + (merged.polarity_inverted
                    ? L" / 極性反転"
                    : L" / 正極性"));
    } else {
        state.scc_preview_wave = preset;
        setSccOperationStatus(
            window, L"プレビュー: プリセットで置換");
    }
    state.scc_preview_active = true;
    state.scc_preview_hearing_candidate = true;
    updateSccPreviewAuditionButton(state, window);
    InvalidateRect(window, nullptr, FALSE);
    if (audition) {
        auditionSccPreview(state, state.scc_preview_wave);
    }
}

std::optional<std::vector<std::uint8_t>> readWaveFileBytes(
    const wchar_t* path) {
    HANDLE file = CreateFileW(
        path,
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)
        || size.QuadPart < 12
        || size.QuadPart > 64 * 1024 * 1024) {
        CloseHandle(file);
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes(
        static_cast<std::size_t>(size.QuadPart));
    DWORD read{};
    const bool succeeded = ReadFile(
        file,
        bytes.data(),
        static_cast<DWORD>(bytes.size()),
        &read,
        nullptr)
        && read == bytes.size();
    CloseHandle(file);
    return succeeded
        ? std::optional<std::vector<std::uint8_t>>(std::move(bytes))
        : std::nullopt;
}

std::optional<std::vector<std::uint8_t>> openWaveFile(HWND window) {
    wchar_t path[MAX_PATH]{};
    OPENFILENAMEW dialog{
        .lStructSize = sizeof(OPENFILENAMEW),
        .hwndOwner = window,
        .lpstrFilter =
            L"WAVE audio\0*.wav;*.wave\0All files\0*.*\0",
        .lpstrFile = path,
        .nMaxFile = MAX_PATH,
        .Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST,
        .lpstrDefExt = L"wav",
    };
    if (!GetOpenFileNameW(&dialog)) {
        return std::nullopt;
    }
    return readWaveFileBytes(path);
}

std::optional<std::vector<std::uint8_t>> clipboardWaveBytes(
    HWND window) {
    if (!IsClipboardFormatAvailable(CF_WAVE)
        && !IsClipboardFormatAvailable(CF_RIFF)
        && !IsClipboardFormatAvailable(CF_HDROP)) {
        return std::nullopt;
    }
    if (!OpenClipboard(window)) {
        return std::nullopt;
    }
    std::optional<std::vector<std::uint8_t>> result;
    const UINT memory_format =
        IsClipboardFormatAvailable(CF_WAVE) ? CF_WAVE : CF_RIFF;
    if (IsClipboardFormatAvailable(memory_format)) {
        if (HGLOBAL memory = GetClipboardData(memory_format)) {
            const std::size_t size = GlobalSize(memory);
            if (size >= 12 && size <= 64 * 1024 * 1024) {
                if (const auto* data = static_cast<const std::uint8_t*>(
                        GlobalLock(memory))) {
                    result = std::vector<std::uint8_t>(data, data + size);
                    GlobalUnlock(memory);
                }
            }
        }
    }
    std::wstring dropped_path;
    if (!result && IsClipboardFormatAvailable(CF_HDROP)) {
        if (HDROP drop = static_cast<HDROP>(
                GetClipboardData(CF_HDROP))) {
            wchar_t path[MAX_PATH]{};
            if (DragQueryFileW(
                    drop,
                    0,
                    path,
                    static_cast<UINT>(std::size(path))) > 0) {
                dropped_path = path;
            }
        }
    }
    CloseClipboard();
    if (!result && !dropped_path.empty()) {
        const auto extension =
            dropped_path.size() >= 4
                ? dropped_path.substr(dropped_path.size() - 4)
                : std::wstring{};
        if (_wcsicmp(extension.c_str(), L".wav") == 0) {
            result = readWaveFileBytes(dropped_path.c_str());
        }
    }
    return result;
}

bool applyWaveConversion(
    EditorState& state,
    HWND window,
    std::span<const std::uint8_t> bytes) {
    mgstc::engine::WavePcm pcm;
    std::string error;
    if (!mgstc::engine::parseWavePcm(bytes, pcm, &error)) {
        MessageBoxW(
            window,
            L"WAVを読み込めませんでした。PCM 8/16/24/32bitまたは"
            L"32bit floatのWAVEファイルを使用してください。",
            L"WAV変換",
            MB_OK | MB_ICONERROR);
        return false;
    }
    const auto analysis =
        mgstc::engine::analyzeWaveCycle(pcm);
    if (analysis.cycle.size() < 2) {
        MessageBoxW(
            window,
            L"波形の1周期を抽出できませんでした。",
            L"WAV変換",
            MB_OK | MB_ICONERROR);
        return false;
    }
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    captureSccUndo(state);
    if (state.opll) {
        state.app->opll_patch =
            mgstc::engine::approximateWaveCycleWithOpll(
                analysis.cycle);
        syncOpllEditorControls(state, window);
        state.app->auditionEditedProgram(true);
    } else {
        applySccWaveform(
            state,
            window,
            mgstc::engine::waveCycleToScc(analysis.cycle));
    }
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    wchar_t status[160]{};
    swprintf_s(
        status,
        state.opll
            ? L"WAV解析完了: 約 %.1f Hz / OPLL近似音色を生成"
            : L"WAV解析完了: 約 %.1f Hz / SCC 32サンプルへ変換",
        analysis.estimated_frequency_hz);
    state.app->setStatus(status);
    if (!state.opll) {
        setSccOperationStatus(window, status);
    }
    return true;
}

std::optional<std::string> openMgsTimbreText(HWND window) {
    wchar_t path[MAX_PATH]{};
    OPENFILENAMEW dialog{
        .lStructSize = sizeof(OPENFILENAMEW),
        .hwndOwner = window,
        .lpstrFilter =
            L"MGSCソース\0*.mml;*.mus;*.txt\0"
            L"すべてのファイル\0*.*\0",
        .lpstrFile = path,
        .nMaxFile = MAX_PATH,
        .Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST,
    };
    if (!GetOpenFileNameW(&dialog)) {
        return std::nullopt;
    }
    HANDLE file = CreateFileW(
        path,
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)
        || size.QuadPart < 0
        || size.QuadPart > 16 * 1024 * 1024) {
        CloseHandle(file);
        return std::nullopt;
    }
    std::string contents(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD read{};
    const bool succeeded = contents.empty()
        || (ReadFile(
                file,
                contents.data(),
                static_cast<DWORD>(contents.size()),
                &read,
                nullptr)
            && read == contents.size());
    CloseHandle(file);
    if (!succeeded) {
        return std::nullopt;
    }
    return contents;
}

bool saveMgsTimbreText(
    HWND window,
    const wchar_t* suggested_name,
    std::string_view contents) {
    wchar_t path[MAX_PATH]{};
    wcscpy_s(path, suggested_name);
    OPENFILENAMEW dialog{
        .lStructSize = sizeof(OPENFILENAMEW),
        .hwndOwner = window,
        .lpstrFilter =
            L"MGSCソース\0*.mml;*.mus\0"
            L"テキスト\0*.txt\0"
            L"すべてのファイル\0*.*\0",
        .lpstrFile = path,
        .nMaxFile = MAX_PATH,
        .Flags =
            OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST,
        .lpstrDefExt = L"mml",
    };
    if (!GetSaveFileNameW(&dialog)) {
        return false;
    }
    HANDLE file = CreateFileW(
        path,
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written{};
    const bool succeeded = contents.empty()
        || (WriteFile(
                file,
                contents.data(),
                static_cast<DWORD>(contents.size()),
                &written,
                nullptr)
            && written == contents.size());
    CloseHandle(file);
    return succeeded;
}

std::optional<std::string> openTimbreLibraryFile(HWND window) {
    wchar_t path[MAX_PATH]{};
    OPENFILENAMEW dialog{
        .lStructSize = sizeof(OPENFILENAMEW),
        .hwndOwner = window,
        .lpstrFilter =
            L"MGS Tone Craft Timbre\0*.mgstc;*.dat\0"
            L"すべてのファイル\0*.*\0",
        .lpstrFile = path,
        .nMaxFile = MAX_PATH,
        .Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST,
        .lpstrDefExt = L"mgstc",
    };
    if (!GetOpenFileNameW(&dialog)) {
        return std::nullopt;
    }
    HANDLE file = CreateFileW(
        path,
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)
        || size.QuadPart < 0
        || size.QuadPart > 16 * 1024 * 1024) {
        CloseHandle(file);
        return std::nullopt;
    }
    std::string contents(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD read{};
    const bool succeeded = contents.empty()
        || (ReadFile(
                file,
                contents.data(),
                static_cast<DWORD>(contents.size()),
                &read,
                nullptr)
            && read == contents.size());
    CloseHandle(file);
    return succeeded
        ? std::optional<std::string>(std::move(contents))
        : std::nullopt;
}

bool saveTimbreLibraryFile(
    HWND window,
    const wchar_t* suggested_name,
    std::string_view contents) {
    wchar_t path[MAX_PATH]{};
    wcscpy_s(path, suggested_name);
    OPENFILENAMEW dialog{
        .lStructSize = sizeof(OPENFILENAMEW),
        .hwndOwner = window,
        .lpstrFilter =
            L"MGS Tone Craft Timbre\0*.mgstc\0"
            L"すべてのファイル\0*.*\0",
        .lpstrFile = path,
        .nMaxFile = MAX_PATH,
        .Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST,
        .lpstrDefExt = L"mgstc",
    };
    if (!GetSaveFileNameW(&dialog)) {
        return false;
    }
    HANDLE file = CreateFileW(
        path,
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written{};
    const bool succeeded = contents.empty()
        || (WriteFile(
                file,
                contents.data(),
                static_cast<DWORD>(contents.size()),
                &written,
                nullptr)
            && written == contents.size());
    const bool flush_ok = succeeded && FlushFileBuffers(file);
    CloseHandle(file);
    return flush_ok;
}

bool copyMgsTimbreTextToClipboard(
    HWND window,
    std::string_view contents) {
    const int wide_length = MultiByteToWideChar(
        CP_UTF8,
        0,
        contents.data(),
        static_cast<int>(contents.size()),
        nullptr,
        0);
    if (wide_length < 0) {
        return false;
    }
    HGLOBAL unicode_memory = GlobalAlloc(
        GMEM_MOVEABLE,
        (static_cast<std::size_t>(wide_length) + 1)
            * sizeof(wchar_t));
    HGLOBAL ansi_memory = GlobalAlloc(
        GMEM_MOVEABLE,
        contents.size() + 1);
    if (!unicode_memory || !ansi_memory) {
        if (unicode_memory) GlobalFree(unicode_memory);
        if (ansi_memory) GlobalFree(ansi_memory);
        return false;
    }
    auto* unicode = static_cast<wchar_t*>(
        GlobalLock(unicode_memory));
    auto* ansi = static_cast<char*>(GlobalLock(ansi_memory));
    if (!unicode || !ansi) {
        if (unicode) GlobalUnlock(unicode_memory);
        if (ansi) GlobalUnlock(ansi_memory);
        GlobalFree(unicode_memory);
        GlobalFree(ansi_memory);
        return false;
    }
    MultiByteToWideChar(
        CP_UTF8,
        0,
        contents.data(),
        static_cast<int>(contents.size()),
        unicode,
        wide_length);
    unicode[wide_length] = L'\0';
    std::copy(contents.begin(), contents.end(), ansi);
    ansi[contents.size()] = '\0';
    GlobalUnlock(unicode_memory);
    GlobalUnlock(ansi_memory);

    if (!OpenClipboard(window)) {
        GlobalFree(unicode_memory);
        GlobalFree(ansi_memory);
        return false;
    }
    EmptyClipboard();
    const bool unicode_set =
        SetClipboardData(CF_UNICODETEXT, unicode_memory) != nullptr;
    if (!unicode_set) {
        GlobalFree(unicode_memory);
    }
    const bool ansi_set =
        SetClipboardData(CF_TEXT, ansi_memory) != nullptr;
    if (!ansi_set) {
        GlobalFree(ansi_memory);
    }
    CloseClipboard();
    return unicode_set || ansi_set;
}

std::optional<std::string> pasteMgsTimbreTextFromClipboard(
    HWND window) {
    if (!OpenClipboard(window)) {
        return std::nullopt;
    }
    std::optional<std::string> result;
    if (HANDLE handle = GetClipboardData(CF_UNICODETEXT)) {
        const auto* text = static_cast<const wchar_t*>(
            GlobalLock(handle));
        if (text) {
            const std::size_t maximum =
                GlobalSize(handle) / sizeof(wchar_t);
            const std::size_t length =
                wcsnlen_s(text, maximum);
            const int bytes = WideCharToMultiByte(
                CP_UTF8,
                0,
                text,
                static_cast<int>(length),
                nullptr,
                0,
                nullptr,
                nullptr);
            if (bytes >= 0
                && bytes <= 16 * 1024 * 1024) {
                std::string converted(
                    static_cast<std::size_t>(bytes),
                    '\0');
                WideCharToMultiByte(
                    CP_UTF8,
                    0,
                    text,
                    static_cast<int>(length),
                    converted.data(),
                    bytes,
                    nullptr,
                    nullptr);
                result = std::move(converted);
            }
            GlobalUnlock(handle);
        }
    } else if (HANDLE ansi_handle = GetClipboardData(CF_TEXT)) {
        const auto* text = static_cast<const char*>(
            GlobalLock(ansi_handle));
        if (text) {
            const std::size_t maximum = GlobalSize(ansi_handle);
            const std::size_t length =
                strnlen_s(text, maximum);
            if (length <= 16 * 1024 * 1024) {
                result = std::string(text, length);
            }
            GlobalUnlock(ansi_handle);
        }
    }
    CloseClipboard();
    return result;
}

std::optional<std::uint16_t> timbreExportNumber(
    HWND window,
    bool opll) {
    wchar_t text[16]{};
    GetDlgItemTextW(
        window,
        kTimbreExportNumber,
        text,
        static_cast<int>(std::size(text)));
    wchar_t* end{};
    const long parsed = wcstol(text, &end, 10);
    const long minimum = opll ? 15 : 0;
    const long maximum = 31;
    if (end == text || *end != L'\0'
        || parsed < minimum || parsed > maximum) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(parsed);
}

std::string wideToUtf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        result.data(),
        size,
        nullptr,
        nullptr);
    return result;
}

std::wstring utf8ToWide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0);
    if (size <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        result.data(),
        size);
    return result;
}

std::wstring dialogText(HWND window, int id) {
    const HWND control = GetDlgItem(window, id);
    const int length = control ? GetWindowTextLengthW(control) : 0;
    std::wstring value(static_cast<std::size_t>(length + 1), L'\0');
    if (length > 0) {
        GetWindowTextW(control, value.data(), length + 1);
    }
    value.resize(static_cast<std::size_t>(length));
    return value;
}

void refreshTimbreDefinitionPreview(
    const EditorState& state,
    HWND window) {
    HWND preview = GetDlgItem(window, kTimbreDefinitionPreview);
    if (!preview) {
        return;
    }
    const auto number = timbreExportNumber(window, state.opll);
    if (!number) {
        SetWindowTextW(
            preview,
            state.opll
                ? L"出力番号を15～31で指定してください。"
                : L"出力番号を0～31で指定してください。");
        return;
    }
    const std::string definition = state.opll
        ? mgstc::engine::formatMgsOpllDefinition(
            state.app->opll_patch,
            *number)
        : mgstc::engine::formatMgsSccDefinition(
            currentSccWaveform(*state.app),
            *number);
    const std::wstring converted = utf8ToWide(definition);
    SetWindowTextW(preview, converted.c_str());
}

std::wstring timbreLibraryPath(bool create_directory) {
    wchar_t local_app_data[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA",
        local_app_data,
        static_cast<DWORD>(std::size(local_app_data)));
    if (length == 0 || length >= std::size(local_app_data)) {
        return {};
    }
    std::wstring directory =
        std::wstring(local_app_data) + L"\\MgsToneCraft";
    if (create_directory
        && !CreateDirectoryW(directory.c_str(), nullptr)
        && GetLastError() != ERROR_ALREADY_EXISTS) {
        return {};
    }
    return directory + L"\\timbre-library-v1.mgstc";
}

bool loadTimbreLibrary(AppState& app) {
    const auto path = timbreLibraryPath(false);
    if (path.empty()) {
        return false;
    }
    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND
            || error == ERROR_PATH_NOT_FOUND;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)
        || size.QuadPart < 0
        || size.QuadPart > 16 * 1024 * 1024) {
        CloseHandle(file);
        return false;
    }
    std::string contents(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD read{};
    const bool read_ok = contents.empty()
        || (ReadFile(
                file,
                contents.data(),
                static_cast<DWORD>(contents.size()),
                &read,
                nullptr)
            && read == contents.size());
    CloseHandle(file);
    if (!read_ok) {
        return false;
    }
    std::string error;
    auto library =
        mgstc::engine::TimbreLibrary::deserialize(contents, &error);
    if (!library) {
        return false;
    }
    app.timbre_library = std::move(*library);
    return true;
}

bool saveTimbreLibrary(const AppState& app) {
    const auto path = timbreLibraryPath(true);
    if (path.empty()) {
        return false;
    }
    const std::wstring temporary = path + L".tmp";
    const std::string contents = app.timbre_library.serialize();
    HANDLE file = CreateFileW(
        temporary.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written{};
    const bool write_ok = contents.empty()
        || (WriteFile(
                file,
                contents.data(),
                static_cast<DWORD>(contents.size()),
                &written,
                nullptr)
            && written == contents.size());
    const bool flush_ok = write_ok && FlushFileBuffers(file);
    CloseHandle(file);
    if (!flush_ok
        || !MoveFileExW(
            temporary.c_str(),
            path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

std::wstring applicationSettingsPath(bool create_directory) {
    wchar_t local_app_data[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA",
        local_app_data,
        static_cast<DWORD>(std::size(local_app_data)));
    if (length == 0 || length >= std::size(local_app_data)) {
        return {};
    }
    std::wstring directory =
        std::wstring(local_app_data) + L"\\MgsToneCraft";
    if (create_directory
        && !CreateDirectoryW(directory.c_str(), nullptr)
        && GetLastError() != ERROR_ALREADY_EXISTS) {
        return {};
    }
    return directory + L"\\settings-v1.ini";
}

void loadApplicationSettings(AppState& app) {
    const auto path = applicationSettingsPath(false);
    if (path.empty()
        || GetFileAttributesW(path.c_str())
            == INVALID_FILE_ATTRIBUTES) {
        return;
    }
    constexpr wchar_t section[] = L"Application";
    if (GetPrivateProfileIntW(
            section, L"SchemaVersion", 0, path.c_str()) != 1) {
        return;
    }
    app.opll_immediate_audition =
        GetPrivateProfileIntW(
            section, L"OpllImmediateAudition", 1, path.c_str()) != 0;
    app.scc_immediate_audition =
        GetPrivateProfileIntW(
            section, L"SccImmediateAudition", 1, path.c_str()) != 0;
    app.pc_octave = std::clamp(
        static_cast<int>(GetPrivateProfileIntW(
            section, L"PcKeyboardOctave", 4, path.c_str())),
        1,
        8);
}

bool saveApplicationSettings(const AppState& app) {
    const auto path = applicationSettingsPath(true);
    if (path.empty()) {
        return false;
    }
    constexpr wchar_t section[] = L"Application";
    const auto write_value = [&](const wchar_t* key, int value) {
        wchar_t text[16]{};
        swprintf_s(text, L"%d", value);
        return WritePrivateProfileStringW(
            section, key, text, path.c_str()) != FALSE;
    };
    return write_value(L"SchemaVersion", 1)
        && write_value(
            L"OpllImmediateAudition",
            app.opll_immediate_audition ? 1 : 0)
        && write_value(
            L"SccImmediateAudition",
            app.scc_immediate_audition ? 1 : 0)
        && write_value(L"PcKeyboardOctave", app.pc_octave)
        && WritePrivateProfileStringW(
            nullptr, nullptr, nullptr, path.c_str()) != FALSE;
}

std::int64_t unixTimeNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void populateLibraryList(
    EditorState& state,
    HWND window,
    std::optional<std::uint64_t> select_id = std::nullopt) {
    HWND list = GetDlgItem(window, kLibraryList);
    if (!list) {
        return;
    }
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    std::wstring filter = dialogText(window, kLibraryFilter);
    std::transform(
        filter.begin(),
        filter.end(),
        filter.begin(),
        [](wchar_t value) {
            return static_cast<wchar_t>(std::towlower(value));
        });
    const auto category = state.opll
        ? mgstc::engine::TimbreCategory::Opll
        : mgstc::engine::TimbreCategory::Scc;
    int selection = -1;
    for (const auto& entry : state.app->timbre_library.entries()) {
        if (entry.category != category) {
            continue;
        }
        std::wstring searchable =
            utf8ToWide(entry.name) + L" " + utf8ToWide(entry.tags);
        std::transform(
            searchable.begin(),
            searchable.end(),
            searchable.begin(),
            [](wchar_t value) {
                return static_cast<wchar_t>(std::towlower(value));
            });
        if (!filter.empty()
            && searchable.find(filter) == std::wstring::npos) {
            continue;
        }
        std::wstring label = entry.favorite ? L"★ " : L"";
        label += utf8ToWide(entry.name);
        if (label.empty()) {
            label = L"（名称未設定）";
        }
        const auto index = static_cast<int>(SendMessageW(
            list,
            LB_ADDSTRING,
            0,
            reinterpret_cast<LPARAM>(label.c_str())));
        SendMessageW(
            list,
            LB_SETITEMDATA,
            index,
            static_cast<LPARAM>(entry.id));
        if (select_id && entry.id == *select_id) {
            selection = index;
        }
    }
    if (selection >= 0) {
        SendMessageW(list, LB_SETCURSEL, selection, 0);
    }
}

void refreshLibraryLists(AppState& app) {
    if (app.opll_editor) {
        populateLibraryList(
            app.opll_editor_state,
            app.opll_editor,
            app.opll_editor_state.selected_library_id);
    }
    if (app.scc_editor) {
        populateLibraryList(
            app.scc_editor_state,
            app.scc_editor,
            app.scc_editor_state.selected_library_id);
    }
}

std::optional<std::uint64_t> selectedLibraryListId(HWND window) {
    HWND list = GetDlgItem(window, kLibraryList);
    const auto selection = SendMessageW(list, LB_GETCURSEL, 0, 0);
    if (selection == LB_ERR) {
        return std::nullopt;
    }
    const auto data = SendMessageW(
        list, LB_GETITEMDATA, selection, 0);
    if (data == LB_ERR || data <= 0) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(data);
}

mgstc::engine::TimbreLibraryEntry captureLibraryEntry(
    const EditorState& state,
    HWND window) {
    mgstc::engine::TimbreLibraryEntry entry;
    entry.category = state.opll
        ? mgstc::engine::TimbreCategory::Opll
        : mgstc::engine::TimbreCategory::Scc;
    entry.name = wideToUtf8(dialogText(window, kLibraryName));
    entry.tags = wideToUtf8(dialogText(window, kLibraryTags));
    entry.memo = wideToUtf8(dialogText(window, kLibraryMemo));
    entry.favorite =
        IsDlgButtonChecked(window, kLibraryFavorite) == BST_CHECKED;
    if (state.opll) {
        entry.opll_registers =
            mgstc::engine::encodeOpllPatch(state.app->opll_patch);
    } else {
        entry.scc_waveform = state.app->scc_wave;
    }
    return entry;
}

void setEditorBaseline(EditorState& state, HWND window) {
    state.edit_baseline = captureLibraryEntry(state, window);
    state.baseline_library_id = state.selected_library_id;
    state.edit_baseline_valid = true;
}

void restoreEditorBaseline(EditorState& state, HWND window) {
    if (!state.edit_baseline_valid) {
        return;
    }
    const auto& entry = state.edit_baseline;
    SetDlgItemTextW(
        window, kLibraryName, utf8ToWide(entry.name).c_str());
    SetDlgItemTextW(
        window, kLibraryTags, utf8ToWide(entry.tags).c_str());
    SetDlgItemTextW(
        window, kLibraryMemo, utf8ToWide(entry.memo).c_str());
    CheckDlgButton(
        window,
        kLibraryFavorite,
        entry.favorite ? BST_CHECKED : BST_UNCHECKED);
    state.selected_library_id = state.baseline_library_id;
    if (state.opll) {
        state.app->opll_patch =
            mgstc::engine::decodeOpllPatch(entry.opll_registers);
        syncOpllEditorControls(state, window);
        state.app->auditionEditedProgram(true);
    } else {
        captureSccUndo(state);
        mgstc::engine::SccWaveform waveform{};
        for (std::size_t index = 0; index < waveform.size(); ++index) {
            waveform[index] =
                static_cast<std::int8_t>(entry.scc_waveform[index]);
        }
        applySccWaveform(state, window, waveform);
    }
    populateLibraryList(
        state, window, state.selected_library_id);
}

void loadLibraryEntry(
    EditorState& state,
    HWND window,
    const mgstc::engine::TimbreLibraryEntry& entry) {
    SetDlgItemTextW(
        window, kLibraryName, utf8ToWide(entry.name).c_str());
    SetDlgItemTextW(
        window, kLibraryTags, utf8ToWide(entry.tags).c_str());
    SetDlgItemTextW(
        window, kLibraryMemo, utf8ToWide(entry.memo).c_str());
    CheckDlgButton(
        window,
        kLibraryFavorite,
        entry.favorite ? BST_CHECKED : BST_UNCHECKED);
    state.selected_library_id = entry.id;
    if (state.opll) {
        state.app->opll_patch =
            mgstc::engine::decodeOpllPatch(entry.opll_registers);
        syncOpllEditorControls(state, window);
        state.app->auditionEditedProgram(true);
    } else {
        captureSccUndo(state);
        mgstc::engine::SccWaveform waveform{};
        for (std::size_t index = 0; index < waveform.size(); ++index) {
            waveform[index] =
                static_cast<std::int8_t>(entry.scc_waveform[index]);
        }
        applySccWaveform(state, window, waveform);
    }
    populateLibraryList(state, window, entry.id);
    setEditorBaseline(state, window);
}

bool saveSccBackgroundSettings(const EditorState& state) {
    const auto path = applicationSettingsPath(true);
    if (path.empty()) {
        return false;
    }
    constexpr wchar_t section[] = L"SccBackground";
    const auto write_integer = [&](const wchar_t* key, int value) {
        wchar_t text[24]{};
        swprintf_s(text, L"%d", value);
        return WritePrivateProfileStringW(
            section, key, text, path.c_str()) != FALSE;
    };
    return write_integer(L"SchemaVersion", 1)
        && WritePrivateProfileStringW(
            section,
            L"ImagePath",
            state.scc_background_path.c_str(),
            path.c_str()) != FALSE
        && write_integer(L"X", state.scc_background_x)
        && write_integer(L"Y", state.scc_background_y)
        && write_integer(L"Width", state.scc_background_width)
        && write_integer(L"Height", state.scc_background_height)
        && write_integer(L"Opacity", state.scc_background_opacity)
        && write_integer(
            L"Visible", state.scc_background_visible ? 1 : 0)
        && WritePrivateProfileStringW(
            nullptr, nullptr, nullptr, path.c_str()) != FALSE;
}

void syncSccBackgroundTransformControls(
    const EditorState& state,
    HWND window) {
    const std::array<int, 4> values{
        state.scc_background_x,
        state.scc_background_y,
        state.scc_background_width,
        state.scc_background_height};
    for (int index = 0; index < 4; ++index) {
        setSignedDialogValue(
            window,
            kSccBackgroundX + index,
            values[static_cast<std::size_t>(index)]);
        SendDlgItemMessageW(
            window,
            kSccBackgroundSliderBase + index,
            TBM_SETPOS,
            TRUE,
            std::clamp(
                values[static_cast<std::size_t>(index)],
                index < 2 ? -768 : 1,
                index < 2 ? 768 : 2048));
    }
}

void loadSccBackgroundSettings(EditorState& state, HWND window) {
    const auto path = applicationSettingsPath(false);
    if (path.empty()
        || GetPrivateProfileIntW(
            L"SccBackground",
            L"SchemaVersion",
            0,
            path.c_str()) != 1) {
        return;
    }
    constexpr wchar_t section[] = L"SccBackground";
    wchar_t image_path[MAX_PATH]{};
    GetPrivateProfileStringW(
        section,
        L"ImagePath",
        L"",
        image_path,
        static_cast<DWORD>(std::size(image_path)),
        path.c_str());
    state.scc_background_path = image_path;
    state.scc_background_x = std::clamp(
        static_cast<int>(GetPrivateProfileIntW(
            section, L"X", 0, path.c_str())),
        -32768,
        32768);
    state.scc_background_y = std::clamp(
        static_cast<int>(GetPrivateProfileIntW(
            section, L"Y", 0, path.c_str())),
        -32768,
        32768);
    state.scc_background_width = std::clamp(
        static_cast<int>(GetPrivateProfileIntW(
            section, L"Width", 768, path.c_str())),
        1,
        32768);
    state.scc_background_height = std::clamp(
        static_cast<int>(GetPrivateProfileIntW(
            section, L"Height", 262, path.c_str())),
        1,
        32768);
    state.scc_background_opacity = std::clamp(
        static_cast<int>(GetPrivateProfileIntW(
            section, L"Opacity", 112, path.c_str())),
        0,
        255);
    state.scc_background_visible =
        GetPrivateProfileIntW(
            section, L"Visible", 1, path.c_str()) != 0;

    syncSccBackgroundTransformControls(state, window);
    SendDlgItemMessageW(
        window,
        kSccBackgroundOpacity,
        TBM_SETPOS,
        TRUE,
        state.scc_background_opacity);
    CheckDlgButton(
        window,
        kSccBackgroundVisible,
        state.scc_background_visible
            ? BST_CHECKED
            : BST_UNCHECKED);

    if (state.scc_background_path.empty()) {
        return;
    }
    int source_width{};
    int source_height{};
    HBITMAP bitmap = loadBackgroundBitmap(
        state.scc_background_path.c_str(),
        source_width,
        source_height);
    if (!bitmap) {
        SetDlgItemTextW(
            window,
            kSccOperationStatus,
            L"背景画像が見つかりません（音色データは保持）");
        return;
    }
    if (state.scc_background) {
        DeleteObject(state.scc_background);
    }
    state.scc_background = bitmap;
    state.scc_background_source_width = source_width;
    state.scc_background_source_height = source_height;
    SetDlgItemTextW(
        window,
        kSccOperationStatus,
        L"保存済み背景画像を復元しました");
    InvalidateRect(window, nullptr, FALSE);
}

bool chooseSccBackgroundImage(EditorState& state, HWND window) {
    wchar_t path[MAX_PATH]{};
    OPENFILENAMEW dialog{
        .lStructSize = sizeof(OPENFILENAMEW),
        .hwndOwner = window,
        .lpstrFilter =
            L"画像ファイル\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff\0"
            L"すべてのファイル\0*.*\0",
        .lpstrFile = path,
        .nMaxFile = MAX_PATH,
        .Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST,
    };
    if (!GetOpenFileNameW(&dialog)) {
        return false;
    }
    int source_width{};
    int source_height{};
    HBITMAP bitmap = loadBackgroundBitmap(
        path,
        source_width,
        source_height);
    if (!bitmap) {
        MessageBoxW(
            window,
            L"画像を読み込めませんでした。",
            L"SCC背景画像",
            MB_OK | MB_ICONERROR);
        return false;
    }
    if (state.scc_background) {
        DeleteObject(state.scc_background);
    }
    state.scc_background = bitmap;
    state.scc_background_path = path;
    state.scc_background_source_width = source_width;
    state.scc_background_source_height = source_height;
    state.scc_background_x = 0;
    state.scc_background_y = 0;
    const auto graph = sccWaveGraphRect();
    state.scc_background_width = graph.right - graph.left;
    state.scc_background_height = graph.bottom - graph.top;
    state.scc_background_visible = true;
    CheckDlgButton(
        window,
        kSccBackgroundVisible,
        BST_CHECKED);
    syncSccBackgroundTransformControls(state, window);
    static_cast<void>(saveSccBackgroundSettings(state));
    InvalidateRect(window, nullptr, FALSE);
    return true;
}

bool pasteClipboardImageAsSccBackground(
    EditorState& state,
    HWND window) {
    if (!IsClipboardFormatAvailable(CF_BITMAP)
        && !IsClipboardFormatAvailable(CF_DIB)
        && !IsClipboardFormatAvailable(CF_DIBV5)) {
        return false;
    }
    if (!OpenClipboard(window)) {
        return false;
    }
    HBITMAP bitmap{};
    if (HANDLE source = GetClipboardData(CF_BITMAP)) {
        bitmap = static_cast<HBITMAP>(CopyImage(
            source, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION));
    }
    if (!bitmap) {
        const UINT format =
            IsClipboardFormatAvailable(CF_DIBV5) ? CF_DIBV5 : CF_DIB;
        if (HGLOBAL dib = GetClipboardData(format)) {
            const auto* header = static_cast<const BITMAPINFOHEADER*>(
                GlobalLock(dib));
            if (header && header->biSize >= sizeof(BITMAPINFOHEADER)) {
                std::size_t color_bytes{};
                if (header->biBitCount <= 8) {
                    const std::size_t colors = header->biClrUsed
                        ? header->biClrUsed
                        : (std::size_t{1} << header->biBitCount);
                    color_bytes = colors * sizeof(RGBQUAD);
                } else if (header->biCompression == BI_BITFIELDS
                           && header->biSize
                               == sizeof(BITMAPINFOHEADER)) {
                    color_bytes = 3 * sizeof(DWORD);
                }
                const auto* bits =
                    reinterpret_cast<const BYTE*>(header)
                    + header->biSize + color_bytes;
                HDC dc = GetDC(window);
                bitmap = CreateDIBitmap(
                    dc,
                    header,
                    CBM_INIT,
                    bits,
                    reinterpret_cast<const BITMAPINFO*>(header),
                    DIB_RGB_COLORS);
                ReleaseDC(window, dc);
            }
            if (header) {
                GlobalUnlock(dib);
            }
        }
    }
    CloseClipboard();
    if (!bitmap) {
        return false;
    }
    BITMAP details{};
    GetObjectW(bitmap, sizeof(details), &details);
    if (state.scc_background) {
        DeleteObject(state.scc_background);
    }
    state.scc_background = bitmap;
    state.scc_background_path.clear();
    state.scc_background_source_width = details.bmWidth;
    state.scc_background_source_height = std::abs(details.bmHeight);
    state.scc_background_x = 0;
    state.scc_background_y = 0;
    const auto graph = sccWaveGraphRect();
    state.scc_background_width = graph.right - graph.left;
    state.scc_background_height = graph.bottom - graph.top;
    state.scc_background_visible = true;
    CheckDlgButton(window, kSccBackgroundVisible, BST_CHECKED);
    syncSccBackgroundTransformControls(state, window);
    static_cast<void>(saveSccBackgroundSettings(state));
    InvalidateRect(window, nullptr, FALSE);
    setSccOperationStatus(
        window,
        L"Clipboard image loaded as SCC waveform background");
    return true;
}

std::pair<int, int> sccSampleFromPoint(POINT point) {
    const auto graph = sccWaveGraphRect();
    const int width = std::max<int>(
        1,
        static_cast<int>(graph.right - graph.left - 1));
    const int height = std::max<int>(
        1,
        static_cast<int>(graph.bottom - graph.top - 1));
    const int x = std::clamp<int>(
        point.x,
        graph.left,
        graph.right - 1);
    const int y = std::clamp<int>(
        point.y,
        graph.top,
        graph.bottom - 1);
    const int index = std::clamp<int>(
        ((x - graph.left) * 31 + width / 2) / width,
        0,
        31);
    const int value = std::clamp<int>(
        127 - ((y - graph.top) * 255 + height / 2) / height,
        -128,
        127);
    return {index, value};
}

bool updateSccWaveFromGraph(
    EditorState& state,
    HWND window,
    POINT point) {
    const auto [index, value] = sccSampleFromPoint(point);
    const int first_index = state.scc_last_index < 0
        ? index
        : state.scc_last_index;
    const int first_value = state.scc_last_index < 0
        ? value
        : state.scc_last_value;
    const int direction = index >= first_index ? 1 : -1;
    const int distance = std::abs(index - first_index);
    bool changed{};
    for (int step = 0; step <= distance; ++step) {
        const int target_index = first_index + step * direction;
        const int value_delta = value - first_value;
        const int target_value = distance == 0
            ? value
            : first_value
                + (value_delta * step
                    + (value_delta >= 0
                        ? distance / 2
                        : -distance / 2))
                    / distance;
        const auto encoded = static_cast<std::uint8_t>(
            static_cast<std::int8_t>(target_value));
        if (state.app->scc_wave[
                static_cast<std::size_t>(target_index)] == encoded) {
            continue;
        }
        state.app->scc_wave[
            static_cast<std::size_t>(target_index)] = encoded;
        setSccDialogValue(window, target_index, encoded);
        changed = true;
    }
    state.scc_last_index = index;
    state.scc_last_value = value;
    if (changed) {
        state.scc_preview_active = false;
        state.scc_preview_hearing_candidate = true;
        updateSccPreviewAuditionButton(state, window);
        InvalidateRect(window, nullptr, FALSE);
        state.app->auditionEditedProgram(false);
    }
    return changed;
}

void paintSccWave(
    HDC dc,
    const EditorState& state) {
    const auto graph = sccWaveGraphRect();
    HBRUSH background = CreateSolidBrush(RGB(248, 249, 250));
    FillRect(dc, &graph, background);
    DeleteObject(background);

    if (state.scc_background
        && state.scc_background_visible
        && state.scc_background_width > 0
        && state.scc_background_height > 0
        && state.scc_background_opacity > 0) {
        const int saved = SaveDC(dc);
        IntersectClipRect(
            dc,
            graph.left + 1,
            graph.top + 1,
            graph.right - 1,
            graph.bottom - 1);
        HDC memory = CreateCompatibleDC(dc);
        const auto previous =
            SelectObject(memory, state.scc_background);
        const BLENDFUNCTION blend{
            AC_SRC_OVER,
            0,
            static_cast<BYTE>(state.scc_background_opacity),
            AC_SRC_ALPHA,
        };
        AlphaBlend(
            dc,
            graph.left + state.scc_background_x,
            graph.top + state.scc_background_y,
            state.scc_background_width,
            state.scc_background_height,
            memory,
            0,
            0,
            state.scc_background_source_width,
            state.scc_background_source_height,
            blend);
        SelectObject(memory, previous);
        DeleteDC(memory);
        RestoreDC(dc, saved);
    }
    FrameRect(dc, &graph, static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));

    HPEN grid_pen = CreatePen(PS_SOLID, 1, RGB(220, 224, 228));
    const auto old_pen = SelectObject(dc, grid_pen);
    const int zero_y =
        graph.top + ((graph.bottom - graph.top) * 127) / 255;
    MoveToEx(dc, graph.left + 1, zero_y, nullptr);
    LineTo(dc, graph.right - 1, zero_y);
    for (int index = 0; index < 32; ++index) {
        const int x = graph.left
            + (index * (graph.right - graph.left - 1)) / 31;
        MoveToEx(dc, x, graph.top + 1, nullptr);
        LineTo(dc, x, graph.bottom - 1);
    }
    SelectObject(dc, old_pen);
    DeleteObject(grid_pen);

    std::array<POINT, 32> points{};
    for (int index = 0; index < 32; ++index) {
        const int sample = static_cast<std::int8_t>(
            state.app->scc_wave[static_cast<std::size_t>(index)]);
        points[static_cast<std::size_t>(index)] = {
            graph.left
                + (index * (graph.right - graph.left - 1)) / 31,
            graph.top
                + ((127 - sample) * (graph.bottom - graph.top - 1))
                    / 255,
        };
    }
    HPEN wave_pen = CreatePen(PS_SOLID, 2, RGB(40, 150, 128));
    const auto previous = SelectObject(dc, wave_pen);
    Polyline(dc, points.data(), static_cast<int>(points.size()));
    SelectObject(dc, previous);
    DeleteObject(wave_pen);

    if (state.scc_preview_active) {
        for (int index = 0; index < 32; ++index) {
            const int sample = static_cast<int>(
                state.scc_preview_wave[
                    static_cast<std::size_t>(index)]);
            points[static_cast<std::size_t>(index)].y =
                graph.top
                + ((127 - sample)
                    * (graph.bottom - graph.top - 1))
                    / 255;
        }
        HPEN preview_pen =
            CreatePen(PS_DASH, 1, RGB(224, 104, 55));
        const auto old_preview = SelectObject(dc, preview_pen);
        Polyline(dc, points.data(), static_cast<int>(points.size()));
        SelectObject(dc, old_preview);
        DeleteObject(preview_pen);
    }
}

LRESULT CALLBACK editorProcedure(
    HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam) {
    auto* state = reinterpret_cast<EditorState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        const auto* create =
            reinterpret_cast<const CREATESTRUCTW*>(lparam);
        state = static_cast<EditorState*>(create->lpCreateParams);
        SetWindowLongPtrW(
            window,
            GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(state));
        return TRUE;
    }
    case WM_NCDESTROY:
        KillTimer(window, kOpllScopeTimer);
        if (state && !state->opll && state->scc_background) {
            DeleteObject(state->scc_background);
            state->scc_background = nullptr;
        }
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    case WM_CREATE: {
        const auto instance = GetModuleHandleW(nullptr);
        CreateWindowExW(
            0, L"STATIC",
            state->opll
                ? L"OPLLオリジナル音色 — MOD / CAR"
                : L"SCC波形（符号付き8 bit × 32サンプル）",
            WS_CHILD | WS_VISIBLE,
            24, 18, 300, 24,
            window, nullptr, nullptr, nullptr);
        CreateWindowExW(
            0, L"STATIC", L"出力番号",
            WS_CHILD | WS_VISIBLE,
            326, 22, 58, 20,
            window, nullptr, instance, nullptr);
        CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            state->opll ? L"16" : L"0",
            WS_CHILD | WS_VISIBLE | ES_CENTER | ES_NUMBER,
            386, 17, 42, 25,
            window,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(kTimbreExportNumber)),
            instance,
            nullptr);
        CreateWindowExW(
            0, L"BUTTON", L"ファイル読込",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            438, 14, 70, 30,
            window,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(kTimbreImport)),
            instance,
            nullptr);
        CreateWindowExW(
            0, L"BUTTON", L"ファイル保存",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            512, 14, 70, 30,
            window,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(kTimbreExport)),
            instance,
            nullptr);
        CreateWindowExW(
            0, L"BUTTON", L"貼り付け",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            586, 14, 58, 30,
            window,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(kTimbreClipboardPaste)),
            instance,
            nullptr);
        CreateWindowExW(
            0, L"BUTTON", L"コピー",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            648, 14, 58, 30,
            window,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(kTimbreClipboardCopy)),
            instance,
            nullptr);
        CreateWindowExW(
            0,
            L"BUTTON",
            L"WAV変換",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            710,
            14,
            78,
            30,
            window,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(kWaveConvert)),
            instance,
            nullptr);
        CreateWindowExW(
            0, L"BUTTON", L"変更時に最後の音程で即時発声",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            24, 48, 270, 24,
            window,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(kImmediateAuditionCheck)),
            instance,
            nullptr);
        CheckDlgButton(
            window,
            kImmediateAuditionCheck,
            (state->opll
                ? state->app->opll_immediate_audition
                : state->app->scc_immediate_audition)
                ? BST_CHECKED
                : BST_UNCHECKED);
        for (int index = 0; index < 2; ++index) {
            HWND history_button = CreateWindowExW(
                0,
                L"BUTTON",
                index == 0 ? L"Undo" : L"Redo",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                298 + index * 58,
                46,
                54,
                26,
                window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(
                    index == 0 ? kEditorUndo : kEditorRedo)),
                instance,
                nullptr);
            EnableWindow(history_button, FALSE);
        }

        CreateWindowExW(
            0, L"STATIC",
            state->opll
                ? L"OPLL 音色ライブラリ"
                : L"SCC 音色ライブラリ",
            WS_CHILD | WS_VISIBLE,
            810, 18, 150, 24,
            window, nullptr, instance, nullptr);
        const std::wstring audition_label =
            L"発声 " + AppState::noteName(state->app->last_note);
        CreateWindowExW(
            0, L"BUTTON", audition_label.c_str(),
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            968, 12, 120, 30,
            window,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(kEditorAuditionButton)),
            instance,
            nullptr);
        CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            810, 44, 278, 26,
            window,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(kLibraryFilter)),
            instance,
            nullptr);
        SendDlgItemMessageW(
            window,
            kLibraryFilter,
            EM_SETCUEBANNER,
            TRUE,
            reinterpret_cast<LPARAM>(L"名前・タグを検索"));
        CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"LISTBOX",
            L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL
                | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            810, 76, 278, 190,
            window,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(kLibraryList)),
            instance,
            nullptr);
        CreateWindowExW(
            0, L"STATIC", L"音色名",
            WS_CHILD | WS_VISIBLE,
            810, 278, 60, 20,
            window, nullptr, instance, nullptr);
        CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            810, 300, 278, 26,
            window,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(kLibraryName)),
            instance,
            nullptr);
        CreateWindowExW(
            0, L"STATIC", L"タグ（カンマ区切り）",
            WS_CHILD | WS_VISIBLE,
            810, 336, 180, 20,
            window, nullptr, instance, nullptr);
        CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            810, 358, 278, 26,
            window,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(kLibraryTags)),
            instance,
            nullptr);
        CreateWindowExW(
            0, L"STATIC", L"メモ",
            WS_CHILD | WS_VISIBLE,
            810, 394, 60, 20,
            window, nullptr, instance, nullptr);
        CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL
                | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
            810, 416, 278, 100,
            window,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(kLibraryMemo)),
            instance,
            nullptr);
        CreateWindowExW(
            0, L"BUTTON", L"お気に入り",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            810, 524, 130, 24,
            window,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(kLibraryFavorite)),
            instance,
            nullptr);
        constexpr std::array<const wchar_t*, 5> library_buttons{
            L"新規", L"読込", L"適用", L"別名保存", L"削除"};
        constexpr std::array<int, 5> library_button_ids{
            kLibraryNew,
            kLibraryLoad,
            kLibrarySave,
            kLibrarySaveAs,
            kLibraryDelete};
        for (int index = 0; index < 5; ++index) {
            const int column = index % 2;
            const int row = index / 2;
            CreateWindowExW(
                0, L"BUTTON", library_buttons[index],
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                810 + column * 142,
                556 + row * 36,
                column == 0 ? 134 : 136,
                30,
                window,
                reinterpret_cast<HMENU>(
                    static_cast<INT_PTR>(library_button_ids[index])),
                instance,
                nullptr);
        }
        CreateWindowExW(
            0, L"BUTTON", L"変更を取消",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            952, 628, 136, 30,
            window,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(kLibraryCancelEdit)),
            instance,
            nullptr);
        CreateWindowExW(
            0, L"BUTTON", L"外部から取込",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            810, 664, 134, 30,
            window,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(kLibraryImport)),
            instance,
            nullptr);
        CreateWindowExW(
            0, L"BUTTON", L"選択を書出",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            952, 664, 136, 30,
            window,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(kLibraryExportSelected)),
            instance,
            nullptr);
        CreateWindowExW(
            0, L"STATIC", L"MGSC定義プレビュー（確定音色）",
            WS_CHILD | WS_VISIBLE,
            810, 704, 278, 20,
            window, nullptr, instance, nullptr);
        CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL
                | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
            810, 726, 278, 98,
            window,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(kTimbreDefinitionPreview)),
            instance,
            nullptr);
        if (state->opll) {
            SetTimer(window, kOpllScopeTimer, 16, nullptr);
        }

        if (state->opll) {
            CreateWindowExW(
                0, L"STATIC", L"固定音色",
                WS_CHILD | WS_VISIBLE,
                314, 52, 62, 20,
                window, nullptr, instance, nullptr);
            HWND rom_preset = CreateWindowExW(
                0, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                378, 46, 220, 320,
                window,
                reinterpret_cast<HMENU>(
                    static_cast<INT_PTR>(kOpllRomPresetCombo)),
                instance,
                nullptr);
            constexpr std::array<const wchar_t*, 15> rom_names{
                L"@0  Violin",
                L"@1  Guitar",
                L"@2  Piano",
                L"@3  Flute",
                L"@4  Clarinet",
                L"@5  Oboe",
                L"@6  Trumpet",
                L"@7  Organ",
                L"@8  Horn",
                L"@9  Synthesizer",
                L"@10 Harpsichord",
                L"@11 Vibraphone",
                L"@12 Synthesizer Bass",
                L"@13 Acoustic Bass",
                L"@14 Electric Guitar"};
            for (const auto* name : rom_names) {
                SendMessageW(
                    rom_preset,
                    CB_ADDSTRING,
                    0,
                    reinterpret_cast<LPARAM>(name));
            }
            SendMessageW(rom_preset, CB_SETCURSEL, 0, 0);
            CreateWindowExW(
                0, L"BUTTON", L"編集音色へ読込",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                616, 45, 134, 29,
                window,
                reinterpret_cast<HMENU>(
                    static_cast<INT_PTR>(kOpllRomPresetLoad)),
                instance,
                nullptr);

            constexpr std::array<const wchar_t*, 4> flag_names{
                L"AM", L"PM", L"EG", L"KR"};
            constexpr std::array<const wchar_t*, 4> envelope_names{
                L"AR (A)", L"DR (D)", L"SL (S)", L"RR (R)"};
            for (int operator_index = 0; operator_index < 2;
                 ++operator_index) {
                auto& parameters =
                    opllOperator(*state->app, operator_index);
                const int left = 24 + operator_index * 380;
                CreateWindowExW(
                    0, L"STATIC",
                    operator_index == 0
                        ? L"MODULATOR"
                        : L"CARRIER",
                    WS_CHILD | WS_VISIBLE,
                    left, 82, 180, 24,
                    window, nullptr, instance, nullptr);
                const std::array<bool, 4> flags{
                    parameters.amplitude_modulation,
                    parameters.pitch_modulation,
                    parameters.sustained_tone,
                    parameters.key_rate_scaling};
                for (int flag = 0; flag < 4; ++flag) {
                    const int id =
                        kOpllFlagBase + operator_index * 4 + flag;
                    CreateWindowExW(
                        0, L"BUTTON", flag_names[flag],
                        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                        left + flag * 72, 108, 66, 24,
                        window,
                        reinterpret_cast<HMENU>(
                            static_cast<INT_PTR>(id)),
                        instance,
                        nullptr);
                    CheckDlgButton(
                        window,
                        id,
                        flags[static_cast<std::size_t>(flag)]
                            ? BST_CHECKED
                            : BST_UNCHECKED);
                }

                const auto createHorizontal = [&](
                    const wchar_t* label,
                    int id,
                    int value_id,
                    int y,
                    int maximum,
                    int value) {
                    CreateWindowExW(
                        0, L"STATIC", label,
                        WS_CHILD | WS_VISIBLE,
                        left, y + 7, 54, 20,
                        window, nullptr, instance, nullptr);
                    HWND slider = CreateWindowExW(
                        0, TRACKBAR_CLASSW, L"",
                        WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                        left + 56, y, 220, 32,
                        window,
                        reinterpret_cast<HMENU>(
                            static_cast<INT_PTR>(id)),
                        instance,
                        nullptr);
                    SendMessageW(
                        slider,
                        TBM_SETRANGE,
                        TRUE,
                        MAKELONG(0, maximum));
                    SendMessageW(slider, TBM_SETPOS, TRUE, value);
                    wchar_t text[16]{};
                    swprintf_s(text, L"%d", value);
                    CreateWindowExW(
                        WS_EX_CLIENTEDGE,
                        L"EDIT",
                        text,
                        WS_CHILD | WS_VISIBLE
                            | WS_TABSTOP | ES_CENTER | ES_NUMBER,
                        left + 282, y + 3, 42, 25,
                        window,
                        reinterpret_cast<HMENU>(
                            static_cast<INT_PTR>(value_id)),
                        instance,
                        nullptr);
                };
                createHorizontal(
                    L"MULT",
                    kOpllMultiBase + operator_index,
                    kOpllMultiValueBase + operator_index,
                    140,
                    15,
                    parameters.multiplier);
                createHorizontal(
                    L"KSL",
                    kOpllKslBase + operator_index,
                    kOpllKslValueBase + operator_index,
                    174,
                    3,
                    parameters.key_scale_level);
                if (operator_index == 0) {
                    createHorizontal(
                        L"TL",
                        kOpllTlSlider,
                        kOpllTlValue,
                        208,
                        63,
                        parameters.total_level);
                    createHorizontal(
                        L"FB",
                        kOpllFeedbackSlider,
                        kOpllFeedbackValue,
                        242,
                        7,
                        state->app->opll_patch.feedback);
                }
                CreateWindowExW(
                    0, L"BUTTON", L"WS",
                    WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                    left + 330, 177, 46, 24,
                    window,
                    reinterpret_cast<HMENU>(
                        static_cast<INT_PTR>(
                            kOpllWaveBase + operator_index)),
                    instance,
                    nullptr);
                CheckDlgButton(
                    window,
                    kOpllWaveBase + operator_index,
                    parameters.waveform ? BST_CHECKED : BST_UNCHECKED);

                for (int phase = 0; phase < 4; ++phase) {
                    const int x = left + 14 + phase * 82;
                    CreateWindowExW(
                        0, L"STATIC", envelope_names[phase],
                        WS_CHILD | WS_VISIBLE | SS_CENTER,
                        x - 8, 280, 72, 20,
                        window, nullptr, instance, nullptr);
                    HWND slider = CreateWindowExW(
                        0, TRACKBAR_CLASSW, L"",
                        WS_CHILD | WS_VISIBLE | TBS_VERT | TBS_NOTICKS,
                        x + 12, 304, 30, 158,
                        window,
                        reinterpret_cast<HMENU>(
                            static_cast<INT_PTR>(
                                kOpllEnvelopeBase
                                + operator_index * 4
                                + phase)),
                        instance,
                        nullptr);
                    SendMessageW(
                        slider, TBM_SETRANGE, TRUE, MAKELONG(0, 15));
                    SendMessageW(
                        slider,
                        TBM_SETPOS,
                        TRUE,
                        15 - opllEnvelopeValue(parameters, phase));
                    wchar_t value[4]{};
                    swprintf_s(
                        value,
                        L"%u",
                        opllEnvelopeValue(parameters, phase));
                    CreateWindowExW(
                        WS_EX_CLIENTEDGE, L"EDIT", value,
                        WS_CHILD | WS_VISIBLE | ES_CENTER | ES_NUMBER,
                        x, 466, 54, 24,
                        window,
                        reinterpret_cast<HMENU>(
                            static_cast<INT_PTR>(
                                kOpllEnvelopeValueBase
                                + operator_index * 4
                                + phase)),
                        instance,
                        nullptr);
                }
            }
        } else {
            CreateWindowExW(
                0, L"STATIC",
                L"波形をなぞって描画 / 各列 上段DEC・下段HEX",
                WS_CHILD | WS_VISIBLE,
                414, 50, 378, 22,
                window, nullptr, instance, nullptr);
            for (int index = 0; index < 32; ++index) {
                const int x = 24 + index * 24;
                const auto sample = static_cast<std::int8_t>(
                    state->app->scc_wave[static_cast<std::size_t>(index)]);
                wchar_t value[8]{};
                swprintf_s(value, L"%d", static_cast<int>(sample));
                CreateWindowExW(
                    WS_EX_CLIENTEDGE, L"EDIT", value,
                    WS_CHILD | WS_VISIBLE | ES_CENTER,
                    x - 1, 364, 25, 24,
                    window,
                    reinterpret_cast<HMENU>(
                        static_cast<INT_PTR>(kSccValueBase + index)),
                    instance,
                    nullptr);
                wchar_t hexadecimal[4]{};
                swprintf_s(
                    hexadecimal,
                    L"%02X",
                    static_cast<unsigned int>(
                        state->app->scc_wave[
                            static_cast<std::size_t>(index)]));
                CreateWindowExW(
                    WS_EX_CLIENTEDGE, L"EDIT", hexadecimal,
                    WS_CHILD | WS_VISIBLE | ES_CENTER | ES_UPPERCASE,
                    x - 1, 394, 25, 24,
                    window,
                    reinterpret_cast<HMENU>(
                        static_cast<INT_PTR>(
                            kSccHexValueBase + index)),
                    instance,
                    nullptr);
                SendDlgItemMessageW(
                    window,
                    kSccHexValueBase + index,
                    EM_SETLIMITTEXT,
                    2,
                    0);
            }
            CreateWindowExW(
                0, L"BUTTON", L"背景画像を読込",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                24, 430, 124, 28,
                window,
                reinterpret_cast<HMENU>(
                    static_cast<INT_PTR>(kSccBackgroundLoad)),
                instance,
                nullptr);
            CreateWindowExW(
                0, L"BUTTON", L"背景を表示",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                164, 432, 112, 24,
                window,
                reinterpret_cast<HMENU>(
                    static_cast<INT_PTR>(kSccBackgroundVisible)),
                instance,
                nullptr);
            CheckDlgButton(
                window,
                kSccBackgroundVisible,
                BST_CHECKED);
            CreateWindowExW(
                0, L"STATIC", L"不透明度",
                WS_CHILD | WS_VISIBLE,
                300, 437, 68, 20,
                window, nullptr, instance, nullptr);
            HWND opacity = CreateWindowExW(
                0, TRACKBAR_CLASSW, L"",
                WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                370, 430, 250, 30,
                window,
                reinterpret_cast<HMENU>(
                    static_cast<INT_PTR>(kSccBackgroundOpacity)),
                instance,
                nullptr);
            SendMessageW(
                opacity, TBM_SETRANGE, TRUE, MAKELONG(0, 255));
            SendMessageW(
                opacity,
                TBM_SETPOS,
                TRUE,
                state->scc_background_opacity);

            constexpr std::array<const wchar_t*, 4> transform_labels{
                L"X", L"Y", L"幅", L"高さ"};
            constexpr std::array<int, 4> transform_ids{
                kSccBackgroundX,
                kSccBackgroundY,
                kSccBackgroundWidth,
                kSccBackgroundHeight};
            const std::array<int, 4> transform_values{
                state->scc_background_x,
                state->scc_background_y,
                state->scc_background_width,
                state->scc_background_height};
            for (int index = 0; index < 4; ++index) {
                const int x = 24 + index * 150;
                CreateWindowExW(
                    0, L"STATIC", transform_labels[index],
                    WS_CHILD | WS_VISIBLE,
                    x, 480, 36, 22,
                    window, nullptr, instance, nullptr);
                wchar_t text[16]{};
                swprintf_s(text, L"%d", transform_values[index]);
                CreateWindowExW(
                    WS_EX_CLIENTEDGE, L"EDIT", text,
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_CENTER,
                    x + 38, 476, 88, 25,
                    window,
                    reinterpret_cast<HMENU>(
                        static_cast<INT_PTR>(transform_ids[index])),
                    instance,
                    nullptr);
                HWND transform_slider = CreateWindowExW(
                    0, TRACKBAR_CLASSW, L"",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP
                        | TBS_HORZ | TBS_NOTICKS,
                    x, 502, 126, 28,
                    window,
                    reinterpret_cast<HMENU>(
                        static_cast<INT_PTR>(
                            kSccBackgroundSliderBase + index)),
                    instance,
                    nullptr);
                const int minimum = index < 2 ? -768 : 1;
                const int maximum = index < 2 ? 768 : 2048;
                SendMessageW(
                    transform_slider,
                    TBM_SETRANGE,
                    TRUE,
                    MAKELONG(minimum, maximum));
                SendMessageW(
                    transform_slider,
                    TBM_SETPOS,
                    TRUE,
                    std::clamp(
                        transform_values[index],
                        minimum,
                        maximum));
            }

            CreateWindowExW(
                0, L"STATIC", L"プリセット",
                WS_CHILD | WS_VISIBLE,
                24, 568, 64, 22,
                window, nullptr, instance, nullptr);
            HWND preset = CreateWindowExW(
                0, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP
                    | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED
                    | CBS_HASSTRINGS,
                90, 558, 150, 240,
                window,
                reinterpret_cast<HMENU>(
                    static_cast<INT_PTR>(kSccPresetCombo)),
                instance,
                nullptr);
            constexpr std::array<const wchar_t*, 6> preset_names{
                L"Sine", L"Square", L"Triangle",
                L"Saw Up", L"Saw Down", L"Pulse"};
            for (const auto* name : preset_names) {
                SendMessageW(
                    preset,
                    CB_ADDSTRING,
                    0,
                    reinterpret_cast<LPARAM>(name));
            }
            SendMessageW(preset, CB_SETCURSEL, 0, 0);
            SendMessageW(preset, CB_SETITEMHEIGHT, 0, 36);
            SendMessageW(
                preset,
                CB_SETITEMHEIGHT,
                static_cast<WPARAM>(-1),
                36);

            CreateWindowExW(
                0, L"STATIC", L"倍音",
                WS_CHILD | WS_VISIBLE,
                252, 568, 42, 22,
                window, nullptr, instance, nullptr);
            HWND harmonic = CreateWindowExW(
                0, TRACKBAR_CLASSW, L"",
                WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS,
                290, 560, 72, 34,
                window,
                reinterpret_cast<HMENU>(
                    static_cast<INT_PTR>(kSccHarmonicCombo)),
                instance,
                nullptr);
            SendMessageW(
                harmonic, TBM_SETRANGE, TRUE, MAKELONG(0, 2));
            SendMessageW(harmonic, TBM_SETTICFREQ, 1, 0);
            SendMessageW(harmonic, TBM_SETPOS, TRUE, 0);
            CreateWindowExW(
                0, L"STATIC", L"1.0x",
                WS_CHILD | WS_VISIBLE | SS_CENTER,
                360, 568, 38, 22,
                window,
                reinterpret_cast<HMENU>(
                    static_cast<INT_PTR>(kSccHarmonicLabel)),
                instance,
                nullptr);

            CreateWindowExW(
                0, L"BUTTON", L"現在波形とマージ",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                402, 566, 152, 24,
                window,
                reinterpret_cast<HMENU>(
                    static_cast<INT_PTR>(kSccMergeCheck)),
                instance,
                nullptr);
            CreateWindowExW(
                0, L"BUTTON", L"プレビュー取消",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                558, 562, 114, 30,
                window,
                reinterpret_cast<HMENU>(
                    static_cast<INT_PTR>(kSccCancelPreset)),
                instance,
                nullptr);
            CreateWindowExW(
                0, L"BUTTON", L"適用",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                680, 562, 108, 30,
                window,
                reinterpret_cast<HMENU>(
                    static_cast<INT_PTR>(kSccApplyPreset)),
                instance,
                nullptr);

            CreateWindowExW(
                0, L"STATIC", L"マージ量",
                WS_CHILD | WS_VISIBLE,
                24, 606, 62, 22,
                window, nullptr, instance, nullptr);
            HWND merge_amount = CreateWindowExW(
                0, TRACKBAR_CLASSW, L"",
                WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                88, 598, 190, 32,
                window,
                reinterpret_cast<HMENU>(
                    static_cast<INT_PTR>(kSccMergeAmount)),
                instance,
                nullptr);
            SendMessageW(
                merge_amount, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
            SendMessageW(merge_amount, TBM_SETPOS, TRUE, 50);
            CreateWindowExW(
                0, L"STATIC", L"50%",
                WS_CHILD | WS_VISIBLE | SS_CENTER,
                280, 606, 48, 22,
                window,
                reinterpret_cast<HMENU>(
                    static_cast<INT_PTR>(kSccMergeAmountLabel)),
                instance,
                nullptr);
            constexpr std::array<const wchar_t*, 3> option_names{
                L"位相を自動調整", L"極性反転を許可", L"音量を維持"};
            constexpr std::array<int, 3> option_ids{
                kSccAutoPhaseCheck,
                kSccPolarityCheck,
                kSccPreserveVolumeCheck};
            for (int index = 0; index < 3; ++index) {
                CreateWindowExW(
                    0, L"BUTTON", option_names[index],
                    WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                    350 + index * 146, 603, 140, 24,
                    window,
                    reinterpret_cast<HMENU>(
                        static_cast<INT_PTR>(option_ids[index])),
                    instance,
                    nullptr);
                CheckDlgButton(
                    window,
                    option_ids[index],
                    BST_CHECKED);
            }

            constexpr std::array<const wchar_t*, 7> tool_names{
                L"平均化", L"正規化", L"反転",
                L"位相 -1", L"位相 +1", L"Undo", L""};
            constexpr std::array<int, 7> tool_ids{
                kSccAverage,
                kSccNormalize,
                kSccInvert,
                kSccRotateLeft,
                kSccRotateRight,
                kSccUndo,
                kSccRedo};
            for (int index = 0; index < 7; ++index) {
                HWND button = CreateWindowExW(
                    0, L"BUTTON", tool_names[index],
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                    24 + index * 96, 644, 88, 30,
                    window,
                    reinterpret_cast<HMENU>(
                        static_cast<INT_PTR>(tool_ids[index])),
                    instance,
                    nullptr);
                if (tool_ids[index] == kSccUndo
                    || tool_ids[index] == kSccRedo) {
                    EnableWindow(button, FALSE);
                }
                if (tool_ids[index] == kSccRedo) {
                    SetWindowTextW(button, L"Redo");
                }
            }
            HWND ab_audition = CreateWindowExW(
                0, L"BUTTON", L"A/B試聴",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                696, 644, 92, 30,
                window,
                reinterpret_cast<HMENU>(
                    static_cast<INT_PTR>(kSccToggleAudition)),
                instance,
                nullptr);
            EnableWindow(ab_audition, FALSE);
            CreateWindowExW(
                0, L"STATIC",
                L"1.5xは1.0xと2.0xを同位相で50%ずつブレンド",
                WS_CHILD | WS_VISIBLE,
                24, 688, 390, 22,
                window, nullptr, instance, nullptr);
            CreateWindowExW(
                0, L"STATIC", L"",
                WS_CHILD | WS_VISIBLE,
                430, 688, 358, 22,
                window,
                reinterpret_cast<HMENU>(
                    static_cast<INT_PTR>(kSccOperationStatus)),
                instance,
                nullptr);
        }
        if (state->opll) {
            refreshOpllEnvelopeTrace(*state);
        } else {
            loadSccBackgroundSettings(*state, window);
        }
        populateLibraryList(*state, window);
        setEditorBaseline(*state, window);
        refreshTimbreDefinitionPreview(*state, window);
        EnumChildWindows(
            window,
            [](HWND control, LPARAM) -> BOOL {
                wchar_t class_name[32]{};
                GetClassNameW(
                    control,
                    class_name,
                    static_cast<int>(std::size(class_name)));
                const LONG_PTR style =
                    GetWindowLongPtrW(control, GWL_STYLE);
                const bool read_only_edit =
                    _wcsicmp(class_name, L"EDIT") == 0
                    && (style & ES_READONLY) != 0;
                const bool interactive =
                    _wcsicmp(class_name, L"BUTTON") == 0
                    || _wcsicmp(class_name, L"COMBOBOX") == 0
                    || _wcsicmp(class_name, L"LISTBOX") == 0
                    || _wcsicmp(class_name, TRACKBAR_CLASSW) == 0
                    || (_wcsicmp(class_name, L"EDIT") == 0
                        && !read_only_edit);
                if (interactive) {
                    SetWindowLongPtrW(
                        control,
                        GWL_STYLE,
                        style | WS_TABSTOP);
                }
                return TRUE;
            },
            0);
        initializeEditorHistory(*state);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        if (!state) {
            break;
        }
        const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (!state->opll) {
            const auto graph = sccWaveGraphRect();
            if (!PtInRect(&graph, point)) {
                break;
            }
            state->scc_dragging = true;
            state->scc_last_index = -1;
            captureSccUndo(*state);
            SetFocus(window);
            SetCapture(window);
            static_cast<void>(
                updateSccWaveFromGraph(*state, window, point));
            return 0;
        }
        for (int operator_index = 0; operator_index < 2;
             ++operator_index) {
            const auto graph = opllEnvelopeGraphRect(operator_index);
            if (!PtInRect(&graph, point)) {
                continue;
            }
            state->graph_operator = operator_index;
            const int width = std::max<int>(
                1,
                static_cast<int>(graph.right - graph.left));
            state->graph_phase = std::clamp<int>(
                ((point.x - graph.left) * 4) / width,
                0,
                3);
            SetFocus(window);
            SetCapture(window);
            static_cast<void>(
                updateOpllEnvelopeFromGraph(*state, window, point));
            return 0;
        }
        break;
    }
    case WM_MOUSEMOVE:
        if (state
            && !state->opll
            && state->scc_dragging
            && (wparam & MK_LBUTTON)) {
            const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            static_cast<void>(
                updateSccWaveFromGraph(*state, window, point));
            return 0;
        }
        if (state
            && state->opll
            && state->graph_operator >= 0
            && (wparam & MK_LBUTTON)) {
            const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            static_cast<void>(
                updateOpllEnvelopeFromGraph(*state, window, point));
            return 0;
        }
        break;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
    case WM_CANCELMODE:
    case WM_KILLFOCUS:
        if (state && !state->opll && state->scc_dragging) {
            state->scc_dragging = false;
            state->scc_last_index = -1;
            if (GetCapture() == window) {
                ReleaseCapture();
            }
            return 0;
        }
        if (state && state->opll && state->graph_operator >= 0) {
            state->graph_operator = -1;
            state->graph_phase = -1;
            if (GetCapture() == window) {
                ReleaseCapture();
            }
            return 0;
        }
        break;
    case WM_HSCROLL:
    case WM_VSCROLL: {
        if (!state || !lparam) {
            break;
        }
        const auto control = reinterpret_cast<HWND>(lparam);
        const int id = GetDlgCtrlID(control);
        const int position = static_cast<int>(
            SendMessageW(control, TBM_GETPOS, 0, 0));
        wchar_t value[16]{};
        if (!state->opll && id == kSccHarmonicCombo) {
            constexpr std::array<const wchar_t*, 3> labels{
                L"1.0x", L"1.5x", L"2.0x"};
            SetDlgItemTextW(
                window,
                kSccHarmonicLabel,
                labels[static_cast<std::size_t>(
                    std::clamp(position, 0, 2))]);
            refreshSccPresetPreview(*state, window, true);
            return 0;
        }
        if (!state->opll && id == kSccMergeAmount) {
            swprintf_s(value, L"%d%%", position);
            SetDlgItemTextW(
                window,
                kSccMergeAmountLabel,
                value);
            refreshSccPresetPreview(*state, window, true);
            return 0;
        }
        if (state
            && !state->opll
            && id == kSccBackgroundOpacity) {
            state->scc_background_opacity =
                std::clamp(position, 0, 255);
            static_cast<void>(
                saveSccBackgroundSettings(*state));
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        if (!state->opll
            && id >= kSccBackgroundSliderBase
            && id < kSccBackgroundSliderBase + 4) {
            switch (id - kSccBackgroundSliderBase) {
            case 0: state->scc_background_x = position; break;
            case 1: state->scc_background_y = position; break;
            case 2: state->scc_background_width = position; break;
            default: state->scc_background_height = position; break;
            }
            syncSccBackgroundTransformControls(*state, window);
            static_cast<void>(saveSccBackgroundSettings(*state));
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        if (state->opll) {
            bool changed = true;
            if (id >= kOpllMultiBase && id < kOpllMultiBase + 2) {
                const int operator_index = id - kOpllMultiBase;
                opllOperator(*state->app, operator_index).multiplier =
                    static_cast<std::uint8_t>(position);
                setUnsignedDialogValue(
                    window,
                    kOpllMultiValueBase + operator_index,
                    position);
            } else if (id >= kOpllKslBase && id < kOpllKslBase + 2) {
                const int operator_index = id - kOpllKslBase;
                opllOperator(
                    *state->app,
                    operator_index).key_scale_level =
                    static_cast<std::uint8_t>(position);
                setUnsignedDialogValue(
                    window,
                    kOpllKslValueBase + operator_index,
                    position);
            } else if (id == kOpllTlSlider) {
                state->app->opll_patch.modulator.total_level =
                    static_cast<std::uint8_t>(position);
                setUnsignedDialogValue(
                    window, kOpllTlValue, position);
            } else if (id == kOpllFeedbackSlider) {
                state->app->opll_patch.feedback =
                    static_cast<std::uint8_t>(position);
                setUnsignedDialogValue(
                    window, kOpllFeedbackValue, position);
            } else if (id >= kOpllEnvelopeBase
                       && id < kOpllEnvelopeBase + 8) {
                const int offset = id - kOpllEnvelopeBase;
                const int logical_value = 15 - position;
                setOpllEnvelopeValue(
                    opllOperator(*state->app, offset / 4),
                    offset % 4,
                    static_cast<std::uint8_t>(logical_value));
                swprintf_s(value, L"%d", logical_value);
                SetDlgItemTextW(
                    window,
                    kOpllEnvelopeValueBase + offset,
                    value);
            } else {
                changed = false;
            }
            if (changed) {
                InvalidateRect(window, nullptr, FALSE);
                state->app->auditionEditedProgram(true);
                return 0;
            }
        }
        if (state->opll
            && id >= kOpllRegisterBase
            && id < kOpllRegisterBase + 8) {
            const auto index = static_cast<std::size_t>(
                id - kOpllRegisterBase);
            auto registers =
                mgstc::engine::encodeOpllPatch(state->app->opll_patch);
            registers[index] = static_cast<std::uint8_t>(position);
            state->app->opll_patch =
                mgstc::engine::decodeOpllPatch(registers);
            swprintf_s(value, L"%02X", position);
            SetDlgItemTextW(
                window, kOpllValueBase + static_cast<int>(index), value);
            state->app->auditionEditedProgram(true);
            return 0;
        }
        if (!state->opll
            && id >= kSccSampleBase
            && id < kSccSampleBase + 32) {
            const auto index = static_cast<std::size_t>(
                id - kSccSampleBase);
            state->scc_preview_active = false;
            state->scc_preview_hearing_candidate = true;
            updateSccPreviewAuditionButton(*state, window);
            state->app->scc_wave[index] = static_cast<std::uint8_t>(
                static_cast<std::int8_t>(position));
            setSccDialogValue(
                window,
                static_cast<int>(index),
                state->app->scc_wave[index]);
            state->app->auditionEditedProgram(false);
            return 0;
        }
        break;
    }
    case WM_COMMAND: {
        if (!state) {
            break;
        }
        const int id = LOWORD(wparam);
        if ((id == kEditorUndo || id == kEditorRedo)
            && HIWORD(wparam) == BN_CLICKED) {
            if (id == kEditorUndo) {
                static_cast<void>(undoEditorHistory(*state, window));
            } else {
                static_cast<void>(redoEditorHistory(*state, window));
            }
            return 0;
        }
        if (id == kWaveConvert
            && HIWORD(wparam) == BN_CLICKED) {
            const auto bytes = openWaveFile(window);
            if (bytes) {
                static_cast<void>(
                    applyWaveConversion(*state, window, *bytes));
            }
            return 0;
        }
        if (id == kEditorAuditionButton
            && HIWORD(wparam) == BN_CLICKED) {
            state->app->auditionEditor(state->opll);
            return 0;
        }
        if (id == kTimbreExportNumber
            && HIWORD(wparam) == EN_CHANGE) {
            refreshTimbreDefinitionPreview(*state, window);
            return 0;
        }
        const auto perform_library_load = [&]() {
            const auto selected = selectedLibraryListId(window);
            if (!selected) {
                return;
            }
            const auto* entry =
                state->app->timbre_library.find(*selected);
            if (!entry) {
                return;
            }
            loadLibraryEntry(*state, window, *entry);
            state->app->setStatus(
                std::wstring(state->opll ? L"OPLL「" : L"SCC「")
                + utf8ToWide(entry->name)
                + L"」をライブラリから読み込みました");
        };
        if (id == kLibraryFilter && HIWORD(wparam) == EN_CHANGE) {
            populateLibraryList(
                *state,
                window,
                state->selected_library_id);
            return 0;
        }
        if (id == kLibraryImport && HIWORD(wparam) == BN_CLICKED) {
            const auto contents = openTimbreLibraryFile(window);
            if (!contents) {
                return 0;
            }
            std::string error;
            const auto imported_ids =
                state->app->timbre_library.importSerialized(
                    *contents,
                    unixTimeNow(),
                    &error);
            if (!imported_ids) {
                MessageBoxW(
                    window,
                    L"対応している音色ライブラリファイルを"
                    L"読み込めませんでした。",
                    L"ライブラリインポート",
                    MB_OK | MB_ICONERROR);
                return 0;
            }
            if (imported_ids->empty()) {
                MessageBoxW(
                    window,
                    L"インポートできる音色が含まれていません。",
                    L"ライブラリインポート",
                    MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            const auto current_category = state->opll
                ? mgstc::engine::TimbreCategory::Opll
                : mgstc::engine::TimbreCategory::Scc;
            std::optional<std::uint64_t> first_current_category;
            for (const auto imported_id : *imported_ids) {
                const auto* imported =
                    state->app->timbre_library.find(imported_id);
                if (imported
                    && imported->category == current_category
                    && !first_current_category) {
                    first_current_category = imported_id;
                }
            }
            if (first_current_category) {
                state->selected_library_id = first_current_category;
            }
            refreshLibraryLists(*state->app);
            if (!saveTimbreLibrary(*state->app)) {
                MessageBoxW(
                    window,
                    L"音色はセッションへ追加されましたが、"
                    L"ローカルライブラリファイルを更新できませんでした。",
                    L"ライブラリインポート",
                    MB_OK | MB_ICONERROR);
            } else {
                state->app->setStatus(
                    std::to_wstring(imported_ids->size())
                    + L"件の音色をライブラリへ追加しました"
                    L"（同名音色は自動改名）");
            }
            return 0;
        }
        if (id == kLibraryExportSelected
            && HIWORD(wparam) == BN_CLICKED) {
            const auto selected = selectedLibraryListId(window);
            const auto* entry = selected
                ? state->app->timbre_library.find(*selected)
                : nullptr;
            if (!entry) {
                MessageBoxW(
                    window,
                    L"書き出す音色を一覧から選択してください。",
                    L"ライブラリエクスポート",
                    MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            const auto contents =
                state->app->timbre_library.serializeEntry(*selected);
            if (!contents) {
                return 0;
            }
            const wchar_t* suggested = state->opll
                ? L"opll-timbre.mgstc"
                : L"scc-timbre.mgstc";
            if (saveTimbreLibraryFile(
                    window, suggested, *contents)) {
                state->app->setStatus(
                    L"「" + utf8ToWide(entry->name)
                    + L"」を外部ライブラリファイルへ書き出しました");
            }
            return 0;
        }
        if (id == kLibraryList
            && (HIWORD(wparam) == LBN_SELCHANGE
                || HIWORD(wparam) == LBN_DBLCLK)) {
            state->selected_library_id =
                selectedLibraryListId(window);
            if (HIWORD(wparam) == LBN_DBLCLK) {
                perform_library_load();
            }
            return 0;
        }
        if (id == kLibraryLoad && HIWORD(wparam) == BN_CLICKED) {
            perform_library_load();
            return 0;
        }
        if (id == kLibraryCancelEdit
            && HIWORD(wparam) == BN_CLICKED) {
            restoreEditorBaseline(*state, window);
            state->app->setStatus(
                state->opll
                    ? L"OPLL音色の変更を編集開始時点へ戻しました"
                    : L"SCC音色の変更を編集開始時点へ戻しました");
            return 0;
        }
        if (id == kLibraryNew && HIWORD(wparam) == BN_CLICKED) {
            state->selected_library_id.reset();
            SetDlgItemTextW(window, kLibraryName, L"");
            SetDlgItemTextW(window, kLibraryTags, L"");
            SetDlgItemTextW(window, kLibraryMemo, L"");
            CheckDlgButton(window, kLibraryFavorite, BST_UNCHECKED);
            SendDlgItemMessageW(
                window,
                kLibraryList,
                LB_SETCURSEL,
                static_cast<WPARAM>(
                    std::numeric_limits<UINT_PTR>::max()),
                0);
            if (state->opll) {
                state->app->opll_patch =
                    mgstc::engine::defaultOpllPatch();
                syncOpllEditorControls(*state, window);
                state->app->auditionEditedProgram(true);
            } else {
                captureSccUndo(*state);
                mgstc::engine::SccWaveform empty{};
                applySccWaveform(*state, window, empty);
            }
            setEditorBaseline(*state, window);
            state->app->setStatus(
                state->opll
                    ? L"新しいOPLL単音色を編集中"
                    : L"新しいSCC単音色を編集中");
            return 0;
        }
        if ((id == kLibrarySave || id == kLibrarySaveAs)
            && HIWORD(wparam) == BN_CLICKED) {
            auto entry = captureLibraryEntry(*state, window);
            if (entry.name.empty()) {
                MessageBoxW(
                    window,
                    L"ライブラリへ保存する音色名を入力してください。",
                    L"音色ライブラリ",
                    MB_OK | MB_ICONWARNING);
                SetFocus(GetDlgItem(window, kLibraryName));
                return 0;
            }
            const auto now = unixTimeNow();
            const bool save_as =
                id == kLibrarySaveAs
                || !state->selected_library_id;
            if (save_as) {
                state->selected_library_id =
                    state->app->timbre_library.add(
                        std::move(entry), now);
            } else if (!state->app->timbre_library.update(
                    *state->selected_library_id,
                    entry,
                    now)) {
                MessageBoxW(
                    window,
                    L"選択中の音色を更新できませんでした。",
                    L"音色ライブラリ",
                    MB_OK | MB_ICONERROR);
                return 0;
            }
            refreshLibraryLists(*state->app);
            setEditorBaseline(*state, window);
            if (!saveTimbreLibrary(*state->app)) {
                MessageBoxW(
                    window,
                    L"音色はセッション内へ保存されましたが、"
                    L"ローカルライブラリファイルを更新できませんでした。",
                    L"音色ライブラリ",
                    MB_OK | MB_ICONERROR);
            } else {
                state->app->setStatus(
                    std::wstring(state->opll ? L"OPLL「" : L"SCC「")
                    + utf8ToWide(
                        state->app->timbre_library
                            .find(*state->selected_library_id)->name)
                    + (save_as
                        ? L"」を新規保存しました"
                        : L"」を更新しました"));
            }
            return 0;
        }
        if (id == kLibraryDelete && HIWORD(wparam) == BN_CLICKED) {
            const auto selected = selectedLibraryListId(window);
            const auto* entry = selected
                ? state->app->timbre_library.find(*selected)
                : nullptr;
            if (!entry) {
                return 0;
            }
            const std::wstring question =
                L"「" + utf8ToWide(entry->name)
                + L"」をライブラリから削除しますか？";
            if (MessageBoxW(
                    window,
                    question.c_str(),
                    L"音色ライブラリ",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2)
                != IDYES) {
                return 0;
            }
            state->app->timbre_library.erase(*selected);
            state->selected_library_id.reset();
            refreshLibraryLists(*state->app);
            setEditorBaseline(*state, window);
            if (!saveTimbreLibrary(*state->app)) {
                MessageBoxW(
                    window,
                    L"ローカルライブラリファイルを更新できませんでした。",
                    L"音色ライブラリ",
                    MB_OK | MB_ICONERROR);
            } else {
                state->app->setStatus(L"音色をライブラリから削除しました");
            }
            return 0;
        }
        const auto import_definition =
            [&](std::string_view contents) {
                if (state->opll) {
                    const auto definition =
                        mgstc::engine::parseMgsOpllDefinition(
                            contents);
                    if (!definition) {
                        MessageBoxW(
                            window,
                            L"有効なMGSC @v定義を読み込めませんでした。",
                            L"OPLL音色インポート",
                            MB_OK | MB_ICONERROR);
                        return false;
                    }
                    state->app->opll_patch = definition->patch;
                    syncOpllEditorControls(*state, window);
                    state->app->auditionEditedProgram(true);
                    state->app->setStatus(
                        std::wstring(L"@v")
                        + std::to_wstring(definition->number)
                        + L" をインポート"
                          L"（定義番号は音色に保存しません）");
                    return true;
                }
                const auto definition =
                    mgstc::engine::parseMgsSccDefinition(
                        contents);
                if (!definition) {
                    MessageBoxW(
                        window,
                        L"有効なMGSC @s定義を読み込めませんでした。",
                        L"SCC音色インポート",
                        MB_OK | MB_ICONERROR);
                    return false;
                }
                captureSccUndo(*state);
                applySccWaveform(
                    *state,
                    window,
                    definition->waveform);
                state->app->setStatus(
                    std::wstring(L"@s")
                    + std::to_wstring(definition->number)
                    + L" をインポート"
                      L"（定義番号は音色に保存しません）");
                return true;
            };
        if (id == kTimbreImport
            && HIWORD(wparam) == BN_CLICKED) {
            const auto contents = openMgsTimbreText(window);
            if (!contents) {
                return 0;
            }
            static_cast<void>(import_definition(*contents));
            return 0;
        }
        if (id == kTimbreClipboardPaste
            && HIWORD(wparam) == BN_CLICKED) {
            const auto contents =
                pasteMgsTimbreTextFromClipboard(window);
            if (!contents) {
                MessageBoxW(
                    window,
                    L"クリップボードにテキストがありません。",
                    L"音色貼り付け",
                    MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            static_cast<void>(import_definition(*contents));
            return 0;
        }
        if (id == kTimbreExport
            && HIWORD(wparam) == BN_CLICKED) {
            const auto number =
                timbreExportNumber(window, state->opll);
            if (!number) {
                MessageBoxW(
                    window,
                    state->opll
                        ? L"OPLLの出力番号は15～31で指定してください。"
                        : L"SCCの出力番号は0～31で指定してください。",
                    L"音色エクスポート",
                    MB_OK | MB_ICONWARNING);
                return 0;
            }
            const std::string definition = state->opll
                ? mgstc::engine::formatMgsOpllDefinition(
                    state->app->opll_patch,
                    *number)
                : mgstc::engine::formatMgsSccDefinition(
                    currentSccWaveform(*state->app),
                    *number);
            wchar_t suggested_name[64]{};
            swprintf_s(
                suggested_name,
                state->opll
                    ? L"opll_timbre_v%u.mml"
                    : L"scc_timbre_s%02u.mml",
                static_cast<unsigned int>(*number));
            if (saveMgsTimbreText(
                    window,
                    suggested_name,
                    definition)) {
                state->app->setStatus(
                    std::wstring(state->opll ? L"@v" : L"@s")
                    + std::to_wstring(*number)
                    + L" 定義をエクスポートしました");
            }
            return 0;
        }
        if (id == kTimbreClipboardCopy
            && HIWORD(wparam) == BN_CLICKED) {
            const auto number =
                timbreExportNumber(window, state->opll);
            if (!number) {
                MessageBoxW(
                    window,
                    state->opll
                        ? L"OPLLの出力番号は15～31で指定してください。"
                        : L"SCCの出力番号は0～31で指定してください。",
                    L"音色コピー",
                    MB_OK | MB_ICONWARNING);
                return 0;
            }
            const std::string definition = state->opll
                ? mgstc::engine::formatMgsOpllDefinition(
                    state->app->opll_patch,
                    *number)
                : mgstc::engine::formatMgsSccDefinition(
                    currentSccWaveform(*state->app),
                    *number);
            if (copyMgsTimbreTextToClipboard(
                    window,
                    definition)) {
                state->app->setStatus(
                    std::wstring(state->opll ? L"@v" : L"@s")
                    + std::to_wstring(*number)
                    + L" 定義をクリップボードへコピーしました");
            } else {
                MessageBoxW(
                    window,
                    L"クリップボードへコピーできませんでした。",
                    L"音色コピー",
                    MB_OK | MB_ICONERROR);
            }
            return 0;
        }
        if (!state->opll && id == kSccBackgroundLoad) {
            static_cast<void>(
                chooseSccBackgroundImage(*state, window));
            return 0;
        }
        if (!state->opll
            && id == kSccBackgroundVisible
            && HIWORD(wparam) == BN_CLICKED) {
            state->scc_background_visible =
                IsDlgButtonChecked(window, id) == BST_CHECKED;
            static_cast<void>(
                saveSccBackgroundSettings(*state));
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        if (!state->opll
            && id == kSccPresetCombo
            && HIWORD(wparam) == CBN_SELCHANGE) {
            refreshSccPresetPreview(*state, window, true);
            return 0;
        }
        if (!state->opll
            && (id == kSccMergeCheck
                || id == kSccAutoPhaseCheck
                || id == kSccPolarityCheck
                || id == kSccPreserveVolumeCheck)
            && HIWORD(wparam) == BN_CLICKED) {
            refreshSccPresetPreview(*state, window, true);
            return 0;
        }
        if (!state->opll
            && id == kSccToggleAudition
            && HIWORD(wparam) == BN_CLICKED) {
            if (!state->scc_preview_active) {
                return 0;
            }
            state->scc_preview_hearing_candidate =
                !state->scc_preview_hearing_candidate;
            if (state->scc_preview_hearing_candidate) {
                auditionSccPreview(
                    *state, state->scc_preview_wave, true);
                setSccOperationStatus(
                    window, L"B 候補波形を試聴中");
            } else {
                auditionSccPreview(
                    *state,
                    currentSccWaveform(*state->app),
                    true);
                setSccOperationStatus(
                    window, L"A 確定波形を試聴中");
            }
            updateSccPreviewAuditionButton(*state, window);
            return 0;
        }
        if (!state->opll
            && id == kSccCancelPreset
            && HIWORD(wparam) == BN_CLICKED) {
            if (state->scc_preview_active) {
                state->scc_preview_active = false;
                state->scc_preview_hearing_candidate = true;
                updateSccPreviewAuditionButton(*state, window);
                InvalidateRect(window, nullptr, FALSE);
                auditionSccPreview(
                    *state,
                    currentSccWaveform(*state->app));
            }
            setSccOperationStatus(
                window, L"プレビューを取り消しました");
            return 0;
        }
        if (!state->opll
            && id == kSccApplyPreset
            && HIWORD(wparam) == BN_CLICKED) {
            if (!state->scc_preview_active) {
                refreshSccPresetPreview(*state, window, false);
            }
            const auto replacement = state->scc_preview_wave;
            captureSccUndo(*state);
            applySccWaveform(*state, window, replacement);
            setSccOperationStatus(
                window, L"プレビュー波形を適用しました");
            return 0;
        }
        if (!state->opll
            && ((id >= kSccAverage && id <= kSccUndo)
                || id == kSccRedo)
            && HIWORD(wparam) == BN_CLICKED) {
            const auto current = currentSccWaveform(*state->app);
            if (id == kSccUndo) {
                static_cast<void>(undoEditorHistory(*state, window));
                setSccOperationStatus(window, L"Undo");
                return 0;
            }
            if (id == kSccRedo) {
                static_cast<void>(redoEditorHistory(*state, window));
                setSccOperationStatus(window, L"Redo");
                return 0;
            }
            captureSccUndo(*state);
            mgstc::engine::SccWaveform result{};
            const wchar_t* description{};
            switch (id) {
            case kSccAverage:
                result =
                    mgstc::engine::averageSccWaveform(current);
                description = L"循環3サンプル平均化";
                break;
            case kSccNormalize:
                result =
                    mgstc::engine::normalizeSccWaveform(current);
                description = L"DC除去・ピーク正規化";
                break;
            case kSccInvert:
                result =
                    mgstc::engine::invertSccWaveform(current);
                description = L"極性を反転";
                break;
            case kSccRotateLeft:
                result =
                    mgstc::engine::rotateSccWaveform(current, -1);
                description = L"位相を左へ1サンプル";
                break;
            default:
                result =
                    mgstc::engine::rotateSccWaveform(current, 1);
                description = L"位相を右へ1サンプル";
                break;
            }
            applySccWaveform(*state, window, result);
            setSccOperationStatus(window, description);
            return 0;
        }
        if (id == kImmediateAuditionCheck) {
            const bool enabled =
                IsDlgButtonChecked(window, id) == BST_CHECKED;
            if (state->opll) {
                state->app->opll_immediate_audition = enabled;
            } else {
                state->app->scc_immediate_audition = enabled;
            }
            static_cast<void>(
                saveApplicationSettings(*state->app));
            return 0;
        }
        if (state->opll
            && id == kOpllRomPresetLoad
            && HIWORD(wparam) == BN_CLICKED) {
            const int selection = static_cast<int>(
                SendDlgItemMessageW(
                    window,
                    kOpllRomPresetCombo,
                    CB_GETCURSEL,
                    0,
                    0));
            const auto patch =
                mgstc::engine::ym2413RomPatch(
                    static_cast<std::uint8_t>(selection + 1));
            if (!patch) {
                return 0;
            }
            state->app->opll_patch = *patch;
            syncOpllEditorControls(*state, window);
            state->app->auditionEditedProgram(true);
            wchar_t selected_name[64]{};
            GetDlgItemTextW(
                window,
                kOpllRomPresetCombo,
                selected_name,
                static_cast<int>(std::size(selected_name)));
            state->app->setStatus(
                std::wstring(L"固定音色 ")
                    + selected_name
                    + L" を編集音色へ読み込みました");
            return 0;
        }
        if (state->opll
            && id >= kOpllFlagBase
            && id < kOpllFlagBase + 8
            && HIWORD(wparam) == BN_CLICKED) {
            const int offset = id - kOpllFlagBase;
            auto& parameters = opllOperator(*state->app, offset / 4);
            const bool enabled =
                IsDlgButtonChecked(window, id) == BST_CHECKED;
            switch (offset % 4) {
            case 0: parameters.amplitude_modulation = enabled; break;
            case 1: parameters.pitch_modulation = enabled; break;
            case 2: parameters.sustained_tone = enabled; break;
            default: parameters.key_rate_scaling = enabled; break;
            }
            InvalidateRect(window, nullptr, FALSE);
            state->app->auditionEditedProgram(true);
            return 0;
        }
        if (state->opll
            && id >= kOpllWaveBase
            && id < kOpllWaveBase + 2
            && HIWORD(wparam) == BN_CLICKED) {
            opllOperator(*state->app, id - kOpllWaveBase).waveform =
                IsDlgButtonChecked(window, id) == BST_CHECKED;
            state->app->auditionEditedProgram(true);
            return 0;
        }
        if (HIWORD(wparam) != EN_KILLFOCUS) {
            break;
        }
        wchar_t text[16]{};
        GetDlgItemTextW(window, id, text, 16);
        if (state->opll
            && id >= kOpllMultiValueBase
            && id <= kOpllFeedbackValue) {
            wchar_t* end{};
            const long parsed = wcstol(text, &end, 10);
            int maximum = 0;
            int slider_id = 0;
            if (id < kOpllKslValueBase) {
                maximum = 15;
                slider_id = kOpllMultiBase
                    + (id - kOpllMultiValueBase);
            } else if (id < kOpllTlValue) {
                maximum = 3;
                slider_id = kOpllKslBase
                    + (id - kOpllKslValueBase);
            } else if (id == kOpllTlValue) {
                maximum = 63;
                slider_id = kOpllTlSlider;
            } else {
                maximum = 7;
                slider_id = kOpllFeedbackSlider;
            }
            if (end != text && *end == L'\0'
                && parsed >= 0 && parsed <= maximum) {
                if (id < kOpllKslValueBase) {
                    opllOperator(
                        *state->app,
                        id - kOpllMultiValueBase).multiplier =
                        static_cast<std::uint8_t>(parsed);
                } else if (id < kOpllTlValue) {
                    opllOperator(
                        *state->app,
                        id - kOpllKslValueBase).key_scale_level =
                        static_cast<std::uint8_t>(parsed);
                } else if (id == kOpllTlValue) {
                    state->app->opll_patch.modulator.total_level =
                        static_cast<std::uint8_t>(parsed);
                } else {
                    state->app->opll_patch.feedback =
                        static_cast<std::uint8_t>(parsed);
                }
                SendDlgItemMessageW(
                    window,
                    slider_id,
                    TBM_SETPOS,
                    TRUE,
                    parsed);
                setUnsignedDialogValue(window, id, parsed);
                InvalidateRect(window, nullptr, FALSE);
                state->app->auditionEditedProgram(true);
            } else {
                syncOpllEditorControls(*state, window);
            }
            return 0;
        }
        if (!state->opll
            && id >= kSccBackgroundX
            && id <= kSccBackgroundHeight) {
            wchar_t* end{};
            const long parsed = wcstol(text, &end, 10);
            const bool size_field =
                id == kSccBackgroundWidth
                || id == kSccBackgroundHeight;
            const bool valid = end != text
                && *end == L'\0'
                && parsed >= (size_field ? 1 : -32768)
                && parsed <= 32768;
            if (valid) {
                switch (id) {
                case kSccBackgroundX:
                    state->scc_background_x = static_cast<int>(parsed);
                    break;
                case kSccBackgroundY:
                    state->scc_background_y = static_cast<int>(parsed);
                    break;
                case kSccBackgroundWidth:
                    state->scc_background_width =
                        static_cast<int>(parsed);
                    break;
                default:
                    state->scc_background_height =
                        static_cast<int>(parsed);
                    break;
                }
                static_cast<void>(
                    saveSccBackgroundSettings(*state));
                syncSccBackgroundTransformControls(*state, window);
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }
        if (state->opll
            && id >= kOpllEnvelopeValueBase
            && id < kOpllEnvelopeValueBase + 8) {
            wchar_t* end{};
            const long parsed = wcstol(text, &end, 10);
            if (end != text && *end == L'\0'
                && parsed >= 0 && parsed <= 15) {
                const int offset = id - kOpllEnvelopeValueBase;
                setOpllEnvelopeValue(
                    opllOperator(*state->app, offset / 4),
                    offset % 4,
                    static_cast<std::uint8_t>(parsed));
                SendDlgItemMessageW(
                    window,
                    kOpllEnvelopeBase + offset,
                    TBM_SETPOS,
                    TRUE,
                    15 - parsed);
                InvalidateRect(window, nullptr, FALSE);
                state->app->auditionEditedProgram(true);
            }
            return 0;
        }
        if (state->opll
            && id >= kOpllValueBase
            && id < kOpllValueBase + 8) {
            wchar_t* end{};
            const long parsed = wcstol(text, &end, 16);
            if (end != text && *end == L'\0' && parsed >= 0 && parsed <= 255) {
                const int index = id - kOpllValueBase;
                auto registers = mgstc::engine::encodeOpllPatch(
                    state->app->opll_patch);
                registers[static_cast<std::size_t>(index)] =
                    static_cast<std::uint8_t>(parsed);
                state->app->opll_patch =
                    mgstc::engine::decodeOpllPatch(registers);
                SendDlgItemMessageW(
                    window, kOpllRegisterBase + index,
                    TBM_SETPOS, TRUE, parsed);
                state->app->auditionEditedProgram(true);
            }
            return 0;
        }
        if (!state->opll
            && id >= kSccValueBase
            && id < kSccValueBase + 32) {
            wchar_t* end{};
            const long parsed = wcstol(text, &end, 10);
            const int index = id - kSccValueBase;
            if (end != text && *end == L'\0'
                && parsed >= -128 && parsed <= 127) {
                const auto encoded = static_cast<std::uint8_t>(
                    static_cast<std::int8_t>(parsed));
                if (state->app->scc_wave[
                        static_cast<std::size_t>(index)] != encoded) {
                    captureSccUndo(*state);
                    state->scc_preview_active = false;
                    state->scc_preview_hearing_candidate = true;
                    updateSccPreviewAuditionButton(*state, window);
                    state->app->scc_wave[
                        static_cast<std::size_t>(index)] = encoded;
                    setSccDialogValue(window, index, encoded);
                    InvalidateRect(window, nullptr, FALSE);
                    state->app->auditionEditedProgram(false);
                } else {
                    setSccDialogValue(window, index, encoded);
                }
            } else {
                setSccDialogValue(
                    window,
                    index,
                    state->app->scc_wave[
                        static_cast<std::size_t>(index)]);
            }
            return 0;
        }
        if (!state->opll
            && id >= kSccHexValueBase
            && id < kSccHexValueBase + 32) {
            wchar_t* end{};
            const long parsed = wcstol(text, &end, 16);
            const int index = id - kSccHexValueBase;
            if (end != text && *end == L'\0'
                && parsed >= 0 && parsed <= 255) {
                const auto encoded = static_cast<std::uint8_t>(parsed);
                if (state->app->scc_wave[
                        static_cast<std::size_t>(index)] != encoded) {
                    captureSccUndo(*state);
                    state->scc_preview_active = false;
                    state->scc_preview_hearing_candidate = true;
                    updateSccPreviewAuditionButton(*state, window);
                    state->app->scc_wave[
                        static_cast<std::size_t>(index)] = encoded;
                    setSccDialogValue(window, index, encoded);
                    InvalidateRect(window, nullptr, FALSE);
                    state->app->auditionEditedProgram(false);
                } else {
                    setSccDialogValue(window, index, encoded);
                }
            } else {
                setSccDialogValue(
                    window,
                    index,
                    state->app->scc_wave[
                        static_cast<std::size_t>(index)]);
            }
            return 0;
        }
        break;
    }
    case kOpllPatchChangedMessage:
        if (state && state->opll) {
            recordEditorHistory(*state);
            refreshOpllEnvelopeTrace(*state);
            refreshTimbreDefinitionPreview(*state, window);
            const RECT graphs{24, 500, 750, 622};
            InvalidateRect(window, &graphs, FALSE);
            return 0;
        }
        break;
    case kTimbreChangedMessage:
        if (state) {
            recordEditorHistory(*state);
            refreshTimbreDefinitionPreview(*state, window);
            return 0;
        }
        break;
    case WM_TIMER:
        if (state && state->opll && wparam == kOpllScopeTimer) {
            mgstc::engine::OpllScopeFrame frame{};
            bool received{};
            while (state->app->engine.pollOpllScope(frame)) {
                state->opll_scope = frame;
                received = true;
            }
            if (received) {
                state->opll_scope_available = true;
                const auto graph = opllScopeGraphRect();
                InvalidateRect(window, &graph, FALSE);
            }
            return 0;
        }
        break;
    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
        if (!state || state->opll || !item
            || item->CtlID != kSccPresetCombo
            || item->itemID == static_cast<UINT>(-1)) {
            break;
        }
        const bool selected =
            (item->itemState & ODS_SELECTED) != 0;
        FillRect(
            item->hDC,
            &item->rcItem,
            GetSysColorBrush(selected ? COLOR_HIGHLIGHT : COLOR_WINDOW));
        SetBkMode(item->hDC, TRANSPARENT);
        SetTextColor(
            item->hDC,
            GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT));
        wchar_t label[64]{};
        SendDlgItemMessageW(
            window,
            kSccPresetCombo,
            CB_GETLBTEXT,
            item->itemID,
            reinterpret_cast<LPARAM>(label));
        RECT text_rect = item->rcItem;
        text_rect.left += 82;
        DrawTextW(
            item->hDC,
            label,
            -1,
            &text_rect,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        const auto waveform = mgstc::engine::generateSccPreset(
            static_cast<mgstc::engine::SccWavePreset>(item->itemID),
            mgstc::engine::SccHarmonic::One);
        std::array<POINT, 32> points{};
        const int left = item->rcItem.left + 4;
        const int width = 70;
        const int middle =
            (item->rcItem.top + item->rcItem.bottom) / 2;
        const int half_height = std::max<int>(
            1,
            static_cast<int>(
                (item->rcItem.bottom - item->rcItem.top - 6) / 2));
        for (std::size_t index = 0; index < points.size(); ++index) {
            points[index] = {
                left + static_cast<int>(index) * width / 31,
                middle - static_cast<int>(waveform[index])
                    * half_height / 128,
            };
        }
        HPEN pen = CreatePen(
            PS_SOLID,
            1,
            selected ? GetSysColor(COLOR_HIGHLIGHTTEXT) : RGB(30, 105, 175));
        const auto old_pen = SelectObject(item->hDC, pen);
        Polyline(item->hDC, points.data(), static_cast<int>(points.size()));
        SelectObject(item->hDC, old_pen);
        DeleteObject(pen);
        if (item->itemState & ODS_FOCUS) {
            DrawFocusRect(item->hDC, &item->rcItem);
        }
        return TRUE;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        if (state && state->opll) {
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(40, 44, 50));
            RECT mod_label{24, 504, 370, 524};
            RECT car_label{404, 504, 750, 524};
            const std::wstring trace_note =
                AppState::noteName(
                    state->opll_envelope_trace.midi_note);
            const std::wstring mod_text =
                L"MOD 実効EG "
                + trace_note
                + L" / KO 1s / drag AR|DR|SL|RR";
            const std::wstring car_text =
                L"CAR 実効EG "
                + trace_note
                + L" / KO 1s / drag AR|DR|SL|RR";
            DrawTextW(
                dc, mod_text.c_str(), -1, &mod_label,
                DT_LEFT | DT_SINGLELINE);
            DrawTextW(
                dc, car_text.c_str(), -1, &car_label,
                DT_LEFT | DT_SINGLELINE);
            const auto mod_graph = opllEnvelopeGraphRect(0);
            const auto car_graph = opllEnvelopeGraphRect(1);
            paintOpllEnvelope(
                dc,
                mod_graph,
                state->opll_envelope_trace.modulator,
                state->opll_envelope_trace.valid,
                RGB(76, 126, 196));
            paintOpllEnvelope(
                dc,
                car_graph,
                state->opll_envelope_trace.carrier,
                state->opll_envelope_trace.valid,
                RGB(205, 125, 55));
            HPEN guides = CreatePen(PS_DOT, 1, RGB(178, 182, 188));
            const auto old_pen = SelectObject(dc, guides);
            for (const auto graph : {mod_graph, car_graph}) {
                for (int phase = 0; phase < 4; ++phase) {
                    const int x = graph.left + 41 + phase * 82;
                    MoveToEx(dc, x, graph.top + 1, nullptr);
                    LineTo(dc, x, graph.bottom - 1);
                }
            }
            SelectObject(dc, old_pen);
            DeleteObject(guides);
            RECT scope_label{24, 632, 750, 652};
            DrawTextW(
                dc,
                L"emu2413 OPLL waveform — 800 samples / 1⁄60 sec / auto scale",
                -1,
                &scope_label,
                DT_LEFT | DT_SINGLELINE);
            paintOpllScope(
                dc,
                state->opll_scope,
                state->opll_scope_available);
        } else if (state) {
            paintSccWave(dc, *state);
        }
        EndPaint(window, &paint);
        return 0;
    }
    case WM_CLOSE:
        if (state && state->opll) {
            KillTimer(window, kOpllScopeTimer);
        } else if (state) {
            static_cast<void>(
                saveSccBackgroundSettings(*state));
        }
        ShowWindow(window, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void showEditor(AppState& app, bool opll) {
    app.selectSource(static_cast<std::uint8_t>(opll ? 8 : 3));
    CheckRadioButton(
        app.main_window,
        kPsgSourceButton,
        kOpllSourceButton,
        opll ? kOpllSourceButton : kSccSourceButton);
    HWND& editor = opll ? app.opll_editor : app.scc_editor;
    if (!editor) {
        const wchar_t* caption =
            opll ? L"OPLL 単音色エディタ" : L"SCC 単音色エディタ";
        editor = CreateWindowExW(
            0,
            kEditorClass,
            caption,
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            1130,
            opll ? 880 : 860,
            app.main_window,
            nullptr,
            GetModuleHandleW(nullptr),
            opll
                ? static_cast<void*>(&app.opll_editor_state)
                : static_cast<void*>(&app.scc_editor_state));
    }
    ShowWindow(editor, SW_SHOW);
    if (opll) {
        SetTimer(editor, kOpllScopeTimer, 16, nullptr);
    }
    SetForegroundWindow(editor);
}

bool isEditControl(HWND control) {
    if (!control) {
        return false;
    }
    wchar_t class_name[32]{};
    GetClassNameW(
        control,
        class_name,
        static_cast<int>(std::size(class_name)));
    return _wcsicmp(class_name, L"EDIT") == 0;
}

bool editConsumesPcPerformance(HWND control) {
    if (!isEditControl(control)) {
        return false;
    }
    const int id = GetDlgCtrlID(control);
    return id == kLibraryFilter
        || id == kLibraryName
        || id == kLibraryTags
        || id == kLibraryMemo
        || id == kTimbreDefinitionPreview
        || (id >= kSccHexValueBase && id < kSccHexValueBase + 32);
}

bool isNumericEditorControl(HWND control) {
    if (!isEditControl(control)) {
        return false;
    }
    const int id = GetDlgCtrlID(control);
    return id == kTimbreExportNumber
        || (id >= kOpllValueBase && id < kOpllValueBase + 8)
        || (id >= kSccValueBase && id < kSccValueBase + 32)
        || (id >= kSccHexValueBase && id < kSccHexValueBase + 32)
        || (id >= kOpllEnvelopeValueBase
            && id < kOpllEnvelopeValueBase + 8)
        || (id >= kSccBackgroundX && id <= kSccBackgroundHeight)
        || (id >= kOpllMultiValueBase && id <= kOpllFeedbackValue);
}

EditorState* activeEditorState(AppState& app, HWND focus) {
    const HWND root = focus ? GetAncestor(focus, GA_ROOT) : nullptr;
    const HWND foreground = GetForegroundWindow();
    if (app.opll_editor
        && (root == app.opll_editor || foreground == app.opll_editor)) {
        return &app.opll_editor_state;
    }
    if (app.scc_editor
        && (root == app.scc_editor || foreground == app.scc_editor)) {
        return &app.scc_editor_state;
    }
    return nullptr;
}

void paintMain(HWND window) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    HBRUSH background = CreateSolidBrush(RGB(238, 240, 242));
    FillRect(dc, &client, background);
    DeleteObject(background);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(28, 32, 38));

    RECT title{22, 16, 700, 48};
    DrawTextW(
        dc,
        L"MGS Tone Craft — Native Preview",
        -1,
        &title,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT subtitle{22, 48, 760, 72};
    DrawTextW(
        dc,
        L"MGSDRV互換  PSG / SCC / OPLL 複合音色エディタ",
        -1,
        &subtitle,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    constexpr std::array<const wchar_t*, 3> labels{
        L"PSG SOFTWARE ENVELOPE",
        L"SCC SOFTWARE ENVELOPE",
        L"OPLL SOFTWARE ENVELOPE / PATCH / y"};
    constexpr std::array<COLORREF, 3> colors{
        RGB(100, 138, 198),
        RGB(65, 164, 145),
        RGB(210, 145, 62)};
    for (int index = 0; index < 3; ++index) {
        RECT lane{22, 104 + index * 112, 1140, 198 + index * 112};
        HBRUSH lane_brush = CreateSolidBrush(RGB(250, 250, 250));
        FillRect(dc, &lane, lane_brush);
        DeleteObject(lane_brush);
        FrameRect(dc, &lane, static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
        RECT marker{lane.left, lane.top, lane.left + 7, lane.bottom};
        HBRUSH marker_brush = CreateSolidBrush(colors[index]);
        FillRect(dc, &marker, marker_brush);
        DeleteObject(marker_brush);
        RECT label{lane.left + 18, lane.top + 10, lane.right - 10, lane.top + 34};
        DrawTextW(dc, labels[index], -1, &label, DT_LEFT | DT_SINGLELINE);
        for (int tick = 0; tick <= 32; ++tick) {
            const int x = lane.left + 190 + tick * 27;
            MoveToEx(dc, x, lane.top + 40, nullptr);
            LineTo(dc, x, lane.bottom - 8);
        }
    }
    EndPaint(window, &paint);
}

LRESULT CALLBACK mainProcedure(
    HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam) {
    auto* app = reinterpret_cast<AppState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        const auto* create =
            reinterpret_cast<const CREATESTRUCTW*>(lparam);
        app = static_cast<AppState*>(create->lpCreateParams);
        app->main_window = window;
        SetWindowLongPtrW(
            window,
            GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(app));
        return TRUE;
    }
    case WM_CREATE: {
        const auto instance = GetModuleHandleW(nullptr);
        CreateWindowExW(
            0, L"BUTTON", L"OPLL音色エディタ",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            744, 20, 132, 30,
            window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOpllButton)),
            instance,
            nullptr);
        CreateWindowExW(
            0, L"BUTTON", L"SCC音色エディタ",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            884, 20, 132, 30,
            window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSccButton)),
            instance,
            nullptr);
        CreateWindowExW(
            0, L"BUTTON", L"停止",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            1024, 20, 82, 30,
            window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStopButton)),
            instance,
            nullptr);
        CreateWindowExW(
            0, L"BUTTON", L"PSG",
            WS_CHILD | WS_VISIBLE | WS_GROUP | BS_AUTORADIOBUTTON,
            430, 438, 86, 28,
            window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPsgSourceButton)),
            instance,
            nullptr);
        CreateWindowExW(
            0, L"BUTTON", L"SCC",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            526, 438, 86, 28,
            window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSccSourceButton)),
            instance,
            nullptr);
        CreateWindowExW(
            0, L"BUTTON", L"OPLL",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            622, 438, 86, 28,
            window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOpllSourceButton)),
            instance,
            nullptr);
        CreateWindowExW(
            0, L"STATIC", L"MIDI",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            22, 444, 40, 22,
            window, nullptr, instance, nullptr);
        app->midi_combo = CreateWindowExW(
            0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            64, 438, 252, 240,
            window,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(kMidiInputCombo)),
            instance,
            nullptr);
        app->midi_connect = CreateWindowExW(
            0, L"BUTTON", L"接続",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            324, 438, 88, 28,
            window,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(kMidiConnectButton)),
            instance,
            nullptr);
        app->populateMidiInputs();
        CheckRadioButton(
            window,
            kPsgSourceButton,
            kOpllSourceButton,
            kPsgSourceButton);
        app->keyboard = CreateWindowExW(
            0, kKeyboardClass, L"",
            WS_CHILD | WS_VISIBLE,
            22, 478, 1120, 150,
            window, nullptr, instance, nullptr);
        app->status = CreateWindowExW(
            0, L"STATIC", L"音声を初期化しています…",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            22, 666, 1080, 26,
            window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusLabel)),
            instance,
            nullptr);
        if (!app->prepareDefaultProgram()) {
            app->setStatus(L"初期プログラムの構築に失敗しました");
        } else {
            app->audio_started = app->audio.start(app->engine);
            app->setStatus(
                app->audio_started
                    ? L"Ready — 48 kHz / WASAPI共有モード"
                    : L"WASAPIを開始できませんでした");
        }
        SetTimer(window, kAudioStatusTimer, 100, nullptr);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case kOpllButton:
            showEditor(*app, true);
            return 0;
        case kSccButton:
            showEditor(*app, false);
            return 0;
        case kStopButton:
            app->noteOff();
            static_cast<void>(app->engine.submit(
                mgstc::engine::EngineCommand::stop()));
            return 0;
        case kPsgSourceButton:
            app->selectSource(0);
            return 0;
        case kSccSourceButton:
            app->selectSource(3);
            return 0;
        case kOpllSourceButton:
            app->selectSource(8);
            return 0;
        case kMidiInputCombo:
            if (HIWORD(wparam) == CBN_SELCHANGE && app->midi_input) {
                static_cast<void>(app->openSelectedMidiInput());
            }
            return 0;
        case kMidiConnectButton:
            if (app->midi_input) {
                app->closeMidiInput();
                app->setStatus(L"MIDI入力を切断しました");
            } else {
                static_cast<void>(app->openSelectedMidiInput());
            }
            return 0;
        default:
            break;
        }
        break;
    case kNoteOnMessage:
        app->mouse_audition_active = true;
        app->noteOn(static_cast<std::uint8_t>(wparam));
        return 0;
    case kNoteOffMessage:
        // Mouse release always ends the monophonic graphical-keyboard
        // audition.  Do not reject it because a fast drag changed the note
        // between the final WM_MOUSEMOVE and WM_LBUTTONUP.
        app->mouse_audition_active = false;
        app->noteOff();
        return 0;
    case kMidiInputMessage: {
        const auto packed = static_cast<std::uint32_t>(wparam);
        const auto status = static_cast<std::uint8_t>(packed & 0xF0);
        const auto note = static_cast<std::uint8_t>((packed >> 8) & 0x7F);
        const auto velocity =
            static_cast<std::uint8_t>((packed >> 16) & 0x7F);
        if (note < 24 || note > 119) {
            return 0;
        }
        if (status == 0x90 && velocity != 0) {
            app->mouse_audition_active = false;
            app->noteOn(note);
        } else if (status == 0x80
                   || (status == 0x90 && velocity == 0)) {
            app->noteOff(note);
        }
        return 0;
    }
    case WM_TIMER: {
        if (wparam == kImmediateAuditionTimer) {
            KillTimer(window, kImmediateAuditionTimer);
            if (app->immediate_audition_pending) {
                app->immediate_audition_pending = false;
                app->noteOff();
            }
            return 0;
        }
        if (app->mouse_audition_active
            && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) {
            app->mouse_audition_active = false;
            SendMessageW(app->keyboard, WM_CANCELMODE, 0, 0);
            app->noteOff();
        }
        mgstc::audio::AudioSinkStatus audio_status{};
        while (app->audio.pollStatus(audio_status)) {
            if (audio_status.type
                == mgstc::audio::AudioSinkStatusType::Underrun) {
                app->setStatus(L"Audio underrunを検出しました");
            } else if (audio_status.type
                       == mgstc::audio::AudioSinkStatusType::DeviceError) {
                app->setStatus(
                    L"WASAPIエラー: "
                    + std::to_wstring(audio_status.native_error));
            }
        }
        return 0;
    }
    case WM_PAINT:
        paintMain(window);
        return 0;
    case WM_DESTROY:
        KillTimer(window, kAudioStatusTimer);
        KillTimer(window, kImmediateAuditionTimer);
        static_cast<void>(saveApplicationSettings(*app));
        app->closeMidiInput();
        app->audio.stop();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

bool registerClasses(HINSTANCE instance) {
    WNDCLASSEXW main_class{
        .cbSize = sizeof(WNDCLASSEXW),
        .style = CS_HREDRAW | CS_VREDRAW,
        .lpfnWndProc = mainProcedure,
        .hInstance = instance,
        .hCursor = LoadCursorW(nullptr, IDC_ARROW),
        .hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1),
        .lpszClassName = kMainClass,
    };
    WNDCLASSEXW keyboard_class{
        .cbSize = sizeof(WNDCLASSEXW),
        .style = CS_HREDRAW | CS_VREDRAW,
        .lpfnWndProc = keyboardProcedure,
        .hInstance = instance,
        .hCursor = LoadCursorW(nullptr, IDC_HAND),
        .lpszClassName = kKeyboardClass,
    };
    WNDCLASSEXW editor_class{
        .cbSize = sizeof(WNDCLASSEXW),
        .style = CS_HREDRAW | CS_VREDRAW,
        .lpfnWndProc = editorProcedure,
        .hInstance = instance,
        .hCursor = LoadCursorW(nullptr, IDC_ARROW),
        .hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1),
        .lpszClassName = kEditorClass,
    };
    return RegisterClassExW(&main_class)
        && RegisterClassExW(&keyboard_class)
        && RegisterClassExW(&editor_class);
}

}  // namespace

int WINAPI wWinMain(
    HINSTANCE instance,
    HINSTANCE,
    PWSTR,
    int show_command) {
    const HRESULT com_result =
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninitialize_com = SUCCEEDED(com_result);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX controls{
        .dwSize = sizeof(INITCOMMONCONTROLSEX),
        .dwICC = ICC_STANDARD_CLASSES | ICC_BAR_CLASSES,
    };
    InitCommonControlsEx(&controls);
    if (!registerClasses(instance)) {
        if (uninitialize_com) {
            CoUninitialize();
        }
        return 1;
    }

    AppState app;
    loadApplicationSettings(app);
    const bool library_loaded = loadTimbreLibrary(app);
    RECT window_rect{0, 0, 1164, 720};
    AdjustWindowRectEx(
        &window_rect,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        FALSE,
        0);
    HWND window = CreateWindowExW(
        0,
        kMainClass,
        L"MGS Tone Craft",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        window_rect.right - window_rect.left,
        window_rect.bottom - window_rect.top,
        nullptr,
        nullptr,
        instance,
        &app);
    if (!window) {
        if (uninitialize_com) {
            CoUninitialize();
        }
        return 2;
    }
    if (!library_loaded) {
        MessageBoxW(
            window,
            L"既存の音色ライブラリを読み込めませんでした。"
            L"現在のセッションでは空のライブラリを使用します。",
            L"音色ライブラリ",
            MB_OK | MB_ICONWARNING);
    }
    ShowWindow(window, show_command);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        HWND focus = GetFocus();
        const bool edit_focused = isEditControl(focus);
        const bool text_edit_focused =
            editConsumesPcPerformance(focus);
        EditorState* active_editor = activeEditorState(app, focus);
        const bool control_down =
            (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (message.message == WM_KEYDOWN
            && control_down
            && active_editor
            && !edit_focused) {
            const auto key = static_cast<UINT>(message.wParam);
            HWND editor_window = active_editor->opll
                ? app.opll_editor
                : app.scc_editor;
            if (key == 'Z') {
                static_cast<void>(
                    undoEditorHistory(*active_editor, editor_window));
                continue;
            }
            if (key == 'Y') {
                static_cast<void>(
                    redoEditorHistory(*active_editor, editor_window));
                continue;
            }
            if (key == 'C') {
                SendMessageW(
                    editor_window,
                    WM_COMMAND,
                    MAKEWPARAM(kTimbreClipboardCopy, BN_CLICKED),
                    reinterpret_cast<LPARAM>(
                        GetDlgItem(editor_window, kTimbreClipboardCopy)));
                continue;
            }
            if (key == 'V') {
                if (const auto wave =
                        clipboardWaveBytes(editor_window)) {
                    static_cast<void>(applyWaveConversion(
                        *active_editor,
                        editor_window,
                        *wave));
                    continue;
                }
                if (!active_editor->opll
                    && pasteClipboardImageAsSccBackground(
                        *active_editor,
                        editor_window)) {
                    continue;
                }
                SendMessageW(
                    editor_window,
                    WM_COMMAND,
                    MAKEWPARAM(kTimbreClipboardPaste, BN_CLICKED),
                    reinterpret_cast<LPARAM>(
                        GetDlgItem(editor_window, kTimbreClipboardPaste)));
                continue;
            }
        }
        if ((message.message == WM_KEYDOWN
             || message.message == WM_SYSKEYDOWN)
            && edit_focused) {
            if (text_edit_focused) {
                app.noteOff();
            }
            if (message.wParam == VK_RETURN
                && isNumericEditorControl(focus)) {
                HWND parent = GetParent(focus);
                SendMessageW(
                    parent,
                    WM_COMMAND,
                    MAKEWPARAM(GetDlgCtrlID(focus), EN_KILLFOCUS),
                    reinterpret_cast<LPARAM>(focus));
                continue;
            }
        }
        if (text_edit_focused) {
            if ((app.opll_editor
                 && IsWindowVisible(app.opll_editor)
                 && IsDialogMessageW(app.opll_editor, &message))
                || (app.scc_editor
                    && IsWindowVisible(app.scc_editor)
                    && IsDialogMessageW(app.scc_editor, &message))) {
                continue;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
            continue;
        }
        if (message.message == WM_KEYDOWN
            && (message.wParam == 'Z' || message.wParam == 'X')) {
            const bool repeat = (message.lParam & (1LL << 30)) != 0;
            if (!repeat) {
                app.noteOff();
                app.pc_octave = std::clamp(
                    app.pc_octave + (message.wParam == 'Z' ? -1 : 1),
                    1,
                    8);
                static_cast<void>(saveApplicationSettings(app));
                app.setStatus(
                    L"PC演奏オクターブ: "
                    + std::to_wstring(app.pc_octave)
                    + L"  (Z:下 / X:上)");
                InvalidateRect(app.keyboard, nullptr, FALSE);
            }
            continue;
        }
        const auto pc_note = pcKeyNote(message.wParam, app.pc_octave);
        if ((message.message == WM_KEYDOWN || message.message == WM_KEYUP)
            && pc_note) {
            const auto note = *pc_note;
            if (message.message == WM_KEYDOWN) {
                const bool repeat = (message.lParam & (1LL << 30)) != 0;
                if (!repeat) {
                    app.mouse_audition_active = false;
                    app.noteOn(note);
                }
            } else {
                app.noteOff(note);
            }
            continue;
        }
        if ((app.opll_editor
             && IsWindowVisible(app.opll_editor)
             && IsDialogMessageW(app.opll_editor, &message))
            || (app.scc_editor
                && IsWindowVisible(app.scc_editor)
                && IsDialogMessageW(app.scc_editor, &message))) {
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (uninitialize_com) {
        CoUninitialize();
    }
    return static_cast<int>(message.wParam);
}
