#include "audio_import_bridge.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <cwctype>
#include <filesystem>
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

[[nodiscard]] bool isLikelyImageExtension(
    std::wstring extension) {
    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](wchar_t character) {
            return static_cast<wchar_t>(std::towlower(character));
        });
    return extension == L".png"
        || extension == L".jpg"
        || extension == L".jpeg"
        || extension == L".bmp"
        || extension == L".gif"
        || extension == L".tif"
        || extension == L".tiff"
        || extension == L".webp";
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>>
readBinaryFileBytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        return std::nullopt;
    }
    const auto end = stream.tellg();
    if (end <= 0 || end > static_cast<std::streamoff>(64 * 1024 * 1024)) {
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

[[nodiscard]] std::optional<std::vector<std::uint8_t>>
bytesFromClipboardGlobal(HGLOBAL memory) {
    if (memory == nullptr) {
        return std::nullopt;
    }
    const auto size = GlobalSize(memory);
    if (size == 0) {
        return std::nullopt;
    }
    const auto* data = static_cast<const std::uint8_t*>(
        GlobalLock(memory));
    if (data == nullptr) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> result(data, data + size);
    GlobalUnlock(memory);
    return result;
}

[[nodiscard]] DWORD dibPixelOffset(
    const BITMAPINFOHEADER& info) {
    DWORD colors = info.biClrUsed;
    if (colors == 0 && info.biBitCount <= 8) {
        colors = 1u << info.biBitCount;
    }
    DWORD masks = 0;
    // BI_BITFIELDS (3): 16/32bpp DIB はヘッダー直後に色マスクが付く。
    if (info.biSize == sizeof(BITMAPINFOHEADER)
        && info.biCompression == BI_BITFIELDS
        && (info.biBitCount == 16 || info.biBitCount == 32)) {
        masks = 12u;
    }
    return static_cast<DWORD>(
        sizeof(BITMAPFILEHEADER) + info.biSize + masks
        + colors * sizeof(RGBQUAD));
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>>
dibToBmpFileBytes(HGLOBAL memory) {
    if (memory == nullptr) {
        return std::nullopt;
    }
    const auto size = GlobalSize(memory);
    if (size < sizeof(BITMAPINFOHEADER)) {
        return std::nullopt;
    }
    const auto* locked = static_cast<const std::uint8_t*>(
        GlobalLock(memory));
    if (locked == nullptr) {
        return std::nullopt;
    }
    const auto* info =
        reinterpret_cast<const BITMAPINFOHEADER*>(locked);
    if (info->biSize < sizeof(BITMAPINFOHEADER)
        || size < info->biSize) {
        GlobalUnlock(memory);
        return std::nullopt;
    }
    std::vector<std::uint8_t> result(
        sizeof(BITMAPFILEHEADER) + size);
    auto* file_header =
        reinterpret_cast<BITMAPFILEHEADER*>(result.data());
    file_header->bfType = 0x4D42;
    file_header->bfSize = static_cast<DWORD>(result.size());
    file_header->bfReserved1 = 0;
    file_header->bfReserved2 = 0;
    file_header->bfOffBits = dibPixelOffset(*info);
    std::memcpy(
        result.data() + sizeof(BITMAPFILEHEADER),
        locked,
        size);
    GlobalUnlock(memory);
    return result;
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>>
hbitmapToBmpFileBytes(HBITMAP bitmap) {
    BITMAP bm{};
    if (bitmap == nullptr
        || GetObject(bitmap, sizeof(bm), &bm) == 0
        || bm.bmWidth <= 0
        || bm.bmHeight <= 0) {
        return std::nullopt;
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = bm.bmWidth;
    info.bmiHeader.biHeight = -bm.bmHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    const auto row_stride = static_cast<std::size_t>(
        ((bm.bmWidth * 32 + 31) / 32) * 4);
    const auto pixel_bytes =
        row_stride * static_cast<std::size_t>(bm.bmHeight);
    std::vector<std::uint8_t> result(
        sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER)
        + pixel_bytes);
    auto* file_header =
        reinterpret_cast<BITMAPFILEHEADER*>(result.data());
    file_header->bfType = 0x4D42;
    file_header->bfSize = static_cast<DWORD>(result.size());
    file_header->bfOffBits = static_cast<DWORD>(
        sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER));
    auto* info_header = reinterpret_cast<BITMAPINFOHEADER*>(
        result.data() + sizeof(BITMAPFILEHEADER));
    *info_header = info.bmiHeader;
    HDC dc = GetDC(nullptr);
    if (dc == nullptr) {
        return std::nullopt;
    }
    const auto copied = GetDIBits(
        dc,
        bitmap,
        0,
        static_cast<UINT>(bm.bmHeight),
        result.data() + file_header->bfOffBits,
        reinterpret_cast<BITMAPINFO*>(info_header),
        DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    if (copied == 0) {
        return std::nullopt;
    }
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
            L"Audacityからの読み取り接続を開けませんでした。",
        };
    }

    const auto temp_dir = std::filesystem::temp_directory_path();
    const auto wave_path =
        temp_dir / L"mgstc_audacity_selection.wav";
    const auto cleanup = [&] {
        std::error_code ignored;
        std::filesystem::remove(wave_path, ignored);
    };
    cleanup();

    const std::wstring command =
        L"Export2: Filename=\""
        + wave_path.wstring()
        + L"\" NumChannels=1\n";
    DWORD written{};
    const auto utf8 = wideToUtf8(command);
    if (!WriteFile(
            to_audacity,
            utf8.data(),
            static_cast<DWORD>(utf8.size()),
            &written,
            nullptr)
        || written != utf8.size()) {
        CloseHandle(from_audacity);
        CloseHandle(to_audacity);
        cleanup();
        return {
            std::nullopt,
            L"Audacityへ書き出しコマンドを送れませんでした。",
        };
    }

    std::string response;
    response.reserve(4096);
    const auto deadline =
        GetTickCount64() + 30000ULL;
    while (GetTickCount64() < deadline) {
        std::array<char, 1024> buffer{};
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
        DWORD read{};
        if (!ReadFile(
                from_audacity,
                buffer.data(),
                static_cast<DWORD>(std::min<std::size_t>(
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

bool copyTextToClipboardUnicodeAndAnsi(
    const std::wstring& unicode_text) {
    if (!OpenClipboard(nullptr)) {
        return false;
    }
    EmptyClipboard();

    const auto unicode_bytes =
        (unicode_text.size() + 1) * sizeof(wchar_t);
    auto* unicode_global = GlobalAlloc(GMEM_MOVEABLE, unicode_bytes);
    if (unicode_global == nullptr) {
        CloseClipboard();
        return false;
    }
    if (auto* locked = GlobalLock(unicode_global)) {
        std::memcpy(
            locked,
            unicode_text.c_str(),
            unicode_bytes);
        GlobalUnlock(unicode_global);
    } else {
        GlobalFree(unicode_global);
        CloseClipboard();
        return false;
    }
    if (SetClipboardData(CF_UNICODETEXT, unicode_global)
        == nullptr) {
        GlobalFree(unicode_global);
        CloseClipboard();
        return false;
    }

    const int ansi_size = WideCharToMultiByte(
        932,
        0,
        unicode_text.c_str(),
        -1,
        nullptr,
        0,
        nullptr,
        nullptr);
    if (ansi_size > 0) {
        auto* ansi_global = GlobalAlloc(
            GMEM_MOVEABLE, static_cast<SIZE_T>(ansi_size));
        if (ansi_global != nullptr) {
            if (auto* locked = GlobalLock(ansi_global)) {
                WideCharToMultiByte(
                    932,
                    0,
                    unicode_text.c_str(),
                    -1,
                    static_cast<char*>(locked),
                    ansi_size,
                    nullptr,
                    nullptr);
                GlobalUnlock(ansi_global);
                if (SetClipboardData(CF_TEXT, ansi_global)
                    == nullptr) {
                    GlobalFree(ansi_global);
                }
            } else {
                GlobalFree(ansi_global);
            }
        }
    }

    CloseClipboard();
    return true;
}

std::optional<std::vector<std::uint8_t>>
clipboardImageBytes() {
    if (!OpenClipboard(nullptr)) {
        return std::nullopt;
    }

    std::optional<std::vector<std::uint8_t>> result;
    const auto png_format = RegisterClipboardFormatW(L"PNG");
    if (png_format != 0
        && IsClipboardFormatAvailable(png_format)) {
        result = bytesFromClipboardGlobal(
            GetClipboardData(png_format));
    }
    if (!result && IsClipboardFormatAvailable(CF_DIBV5)) {
        result = dibToBmpFileBytes(GetClipboardData(CF_DIBV5));
    }
    if (!result && IsClipboardFormatAvailable(CF_DIB)) {
        result = dibToBmpFileBytes(GetClipboardData(CF_DIB));
    }
    if (!result && IsClipboardFormatAvailable(CF_BITMAP)) {
        result = hbitmapToBmpFileBytes(
            static_cast<HBITMAP>(GetClipboardData(CF_BITMAP)));
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

    if (!result && !dropped_path.empty()
        && isLikelyImageExtension(dropped_path.extension().wstring())) {
        result = readBinaryFileBytes(dropped_path);
    }
    return result;
}

} // namespace mgstc::platform
