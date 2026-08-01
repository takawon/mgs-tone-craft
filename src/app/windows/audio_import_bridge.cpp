#include "audio_import_bridge.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <span>
#include <string_view>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objidl.h>
#include <shellapi.h>

namespace mgstc::platform {
namespace {

constexpr std::size_t kMaximumWaveBytes = 64 * 1024 * 1024;

std::string wideToUtf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const auto size = WideCharToMultiByte(
        CP_UTF8,
        0,
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
        0,
        text.data(),
        static_cast<int>(text.size()),
        result.data(),
        size,
        nullptr,
        nullptr);
    return result;
}

std::optional<std::vector<std::uint8_t>> waveBytesFromBuffer(
    const std::uint8_t* data,
    std::size_t size,
    bool require_riff_wave) {
    if (data == nullptr || size < 12 || size > kMaximumWaveBytes) {
        return std::nullopt;
    }

    std::size_t riff_offset{};
    if (require_riff_wave) {
        bool found{};
        const auto search_end =
            std::min<std::size_t>(size - 12, 4096);
        for (; riff_offset <= search_end; ++riff_offset) {
            if (std::memcmp(data + riff_offset, "RIFF", 4) == 0
                && std::memcmp(
                    data + riff_offset + 8, "WAVE", 4) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            return std::nullopt;
        }
    }

    auto copy_size = size - riff_offset;
    if (copy_size >= 12
        && std::memcmp(data + riff_offset, "RIFF", 4) == 0) {
        const auto* riff = data + riff_offset;
        const auto riff_size =
            static_cast<std::uint32_t>(riff[4])
            | (static_cast<std::uint32_t>(riff[5]) << 8)
            | (static_cast<std::uint32_t>(riff[6]) << 16)
            | (static_cast<std::uint32_t>(riff[7]) << 24);
        const auto declared_size =
            static_cast<std::size_t>(riff_size) + 8;
        if (declared_size >= 12 && declared_size <= copy_size) {
            copy_size = declared_size;
        }
    }

    return std::vector<std::uint8_t>(
        data + riff_offset,
        data + riff_offset + copy_size);
}

std::optional<std::vector<std::uint8_t>> waveBytesFromGlobal(
    HGLOBAL memory,
    bool require_riff_wave) {
    if (memory == nullptr) {
        return std::nullopt;
    }
    const auto size = GlobalSize(memory);
    const auto* data = static_cast<const std::uint8_t*>(
        GlobalLock(memory));
    if (data == nullptr) {
        return std::nullopt;
    }
    auto result =
        waveBytesFromBuffer(data, size, require_riff_wave);
    GlobalUnlock(memory);
    return result;
}

} // namespace

std::optional<std::vector<std::uint8_t>> readWaveFileBytes(
    const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        return std::nullopt;
    }
    const auto end = stream.tellg();
    if (end < 12
        || end > static_cast<std::streamoff>(kMaximumWaveBytes)) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes(
        static_cast<std::size_t>(end));
    stream.seekg(0);
    stream.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    return stream
        ? std::optional<std::vector<std::uint8_t>>(std::move(bytes))
        : std::nullopt;
}

std::optional<std::vector<std::uint8_t>> clipboardWaveBytes() {
    if (!OpenClipboard(nullptr)) {
        return std::nullopt;
    }

    std::optional<std::vector<std::uint8_t>> result;
    for (const UINT format : {CF_WAVE, CF_RIFF}) {
        if (!result && IsClipboardFormatAvailable(format)) {
            result = waveBytesFromGlobal(
                GetClipboardData(format), false);
        }
    }
    for (UINT format = 0;
         !result && (format = EnumClipboardFormats(format)) != 0;) {
        if (format != CF_WAVE
            && format != CF_RIFF
            && format != CF_HDROP) {
            result = waveBytesFromGlobal(
                GetClipboardData(format), true);
        }
    }

    std::filesystem::path dropped_path;
    if (!result && IsClipboardFormatAvailable(CF_HDROP)) {
        if (const auto drop = static_cast<HDROP>(
                GetClipboardData(CF_HDROP))) {
            std::array<wchar_t, 32768> path{};
            if (DragQueryFileW(
                    drop,
                    0,
                    path.data(),
                    static_cast<UINT>(path.size())) > 0) {
                dropped_path = path.data();
            }
        }
    }
    CloseClipboard();

    if (!result) {
        IDataObject* data_object{};
        if (SUCCEEDED(OleGetClipboard(&data_object))
            && data_object != nullptr) {
            IEnumFORMATETC* formats{};
            if (SUCCEEDED(data_object->EnumFormatEtc(
                    DATADIR_GET, &formats))
                && formats != nullptr) {
                FORMATETC available{};
                ULONG fetched{};
                while (!result
                    && formats->Next(
                        1, &available, &fetched) == S_OK) {
                    const bool require_riff_wave =
                        available.cfFormat != CF_WAVE
                        && available.cfFormat != CF_RIFF;
                    FORMATETC request{
                        available.cfFormat,
                        nullptr,
                        DVASPECT_CONTENT,
                        -1,
                        TYMED_HGLOBAL | TYMED_ISTREAM,
                    };
                    STGMEDIUM medium{};
                    if (SUCCEEDED(data_object->GetData(
                            &request, &medium))) {
                        if (medium.tymed == TYMED_HGLOBAL) {
                            result = waveBytesFromGlobal(
                                medium.hGlobal,
                                require_riff_wave);
                        } else if (
                            medium.tymed == TYMED_ISTREAM
                            && medium.pstm != nullptr) {
                            STATSTG stat{};
                            if (SUCCEEDED(medium.pstm->Stat(
                                    &stat, STATFLAG_NONAME))
                                && stat.cbSize.QuadPart >= 12
                                && stat.cbSize.QuadPart
                                    <= kMaximumWaveBytes) {
                                LARGE_INTEGER start{};
                                static_cast<void>(
                                    medium.pstm->Seek(
                                        start,
                                        STREAM_SEEK_SET,
                                        nullptr));
                                std::vector<std::uint8_t> bytes(
                                    static_cast<std::size_t>(
                                        stat.cbSize.QuadPart));
                                ULONG read{};
                                if (SUCCEEDED(medium.pstm->Read(
                                        bytes.data(),
                                        static_cast<ULONG>(
                                            bytes.size()),
                                        &read))
                                    && read == bytes.size()) {
                                    result = waveBytesFromBuffer(
                                        bytes.data(),
                                        bytes.size(),
                                        require_riff_wave);
                                }
                            }
                        }
                        ReleaseStgMedium(&medium);
                    }
                    if (available.ptd != nullptr) {
                        CoTaskMemFree(available.ptd);
                    }
                }
                formats->Release();
            }
            data_object->Release();
        }
    }

    if (!result && !dropped_path.empty()) {
        auto extension = dropped_path.extension().wstring();
        std::transform(
            extension.begin(),
            extension.end(),
            extension.begin(),
            [](wchar_t character) {
                return static_cast<wchar_t>(
                    std::towlower(character));
            });
        if (extension == L".wav" || extension == L".wave") {
            result = readWaveFileBytes(dropped_path);
        }
    }
    return result;
}

AudacityWaveImportResult exportAudacitySelectionToWave() {
    constexpr wchar_t kToAudacityPipe[] =
        L"\\\\.\\pipe\\ToSrvPipe";
    constexpr wchar_t kFromAudacityPipe[] =
        L"\\\\.\\pipe\\FromSrvPipe";

    if (!WaitNamedPipeW(kToAudacityPipe, 1500)) {
        return {
            std::nullopt,
            L"Audacityのスクリプト接続を検出できませんでした。\n\n"
            L"Audacityの「編集」→「環境設定」→「モジュール」で"
            L"mod-script-pipeを有効にし、Audacityを再起動してから"
            L"音声範囲を選択してください。\n\n"
            L"注意: mod-script-pipeを有効にすると、同じPC上の"
            L"他のプログラムからAudacityを操作できる状態になります。"
            L"信頼できないプログラムを実行する環境では"
            L"無効にしてください。",
        };
    }

    HANDLE to_audacity = CreateFileW(
        kToAudacityPipe,
        GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);
    if (to_audacity == INVALID_HANDLE_VALUE) {
        return {
            std::nullopt,
            L"Audacityへの書き込み接続を開けませんでした。",
        };
    }

    if (!WaitNamedPipeW(kFromAudacityPipe, 1500)) {
        CloseHandle(to_audacity);
        return {
            std::nullopt,
            L"Audacityからの応答接続を検出できませんでした。",
        };
    }
    HANDLE from_audacity = CreateFileW(
        kFromAudacityPipe,
        GENERIC_READ,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);
    if (from_audacity == INVALID_HANDLE_VALUE) {
        CloseHandle(to_audacity);
        return {
            std::nullopt,
            L"Audacityからの応答接続を開けませんでした。",
        };
    }

    std::array<wchar_t, MAX_PATH> temporary_directory{};
    std::array<wchar_t, MAX_PATH> reservation_path{};
    if (GetTempPathW(
            static_cast<DWORD>(temporary_directory.size()),
            temporary_directory.data()) == 0
        || GetTempFileNameW(
            temporary_directory.data(),
            L"MGS",
            0,
            reservation_path.data()) == 0) {
        CloseHandle(from_audacity);
        CloseHandle(to_audacity);
        return {
            std::nullopt,
            L"Audacity変換用の一時ファイルを作成できませんでした。",
        };
    }

    const std::wstring wave_path =
        std::wstring(reservation_path.data()) + L".wav";
    const auto cleanup = [&] {
        DeleteFileW(wave_path.c_str());
        DeleteFileW(reservation_path.data());
    };

    std::string command =
        "Export2: Filename=\""
        + wideToUtf8(wave_path)
        + "\" NumChannels=1";
    command.append("\r\n", 2);
    command.push_back('\0');
    DWORD written{};
    const bool sent = WriteFile(
        to_audacity,
        command.data(),
        static_cast<DWORD>(command.size()),
        &written,
        nullptr)
        && written == command.size();
    if (!sent) {
        cleanup();
        CloseHandle(from_audacity);
        CloseHandle(to_audacity);
        return {
            std::nullopt,
            L"Audacityへ書き出し命令を送信できませんでした。",
        };
    }

    std::string response;
    const auto deadline = GetTickCount64() + 30000;
    while (GetTickCount64() < deadline) {
        DWORD available{};
        if (!PeekNamedPipe(
                from_audacity,
                nullptr,
                0,
                nullptr,
                &available,
                nullptr)) {
            break;
        }
        if (available == 0) {
            Sleep(20);
            continue;
        }
        std::array<char, 4096> buffer{};
        DWORD read{};
        if (!ReadFile(
                from_audacity,
                buffer.data(),
                static_cast<DWORD>(
                    std::min<std::size_t>(
                        buffer.size(), available)),
                &read,
                nullptr)) {
            break;
        }
        response.append(buffer.data(), read);
        if (response.find("\r\n\0\r\n\0", 0, 6)
                != std::string::npos
            || response.find("\n\n") != std::string::npos
            || response.find("\r\n\r\n")
                != std::string::npos) {
            break;
        }
    }

    CloseHandle(from_audacity);
    CloseHandle(to_audacity);
    const bool failed =
        response.find("Failed") != std::string::npos
        || response.find("Error") != std::string::npos;
    auto wave = failed
        ? std::optional<std::vector<std::uint8_t>>{}
        : readWaveFileBytes(wave_path);
    cleanup();
    if (wave) {
        return {std::move(wave), {}};
    }
    if (response.empty()) {
        return {
            std::nullopt,
            L"Audacityから30秒以内に応答がありませんでした。",
        };
    }
    return {
        std::nullopt,
        L"Audacityで選択範囲をWAVへ書き出せませんでした。\n\n"
        L"音声トラック上で時間範囲を選択し、"
        L"再生・録音を停止してからもう一度お試しください。",
    };
}

} // namespace mgstc::platform
