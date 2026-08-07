#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mgstc::platform {

struct AudacityWaveImportResult {
    std::optional<std::vector<std::uint8_t>> wave;
    std::wstring error;
};

[[nodiscard]] std::optional<std::vector<std::uint8_t>>
readWaveFileBytes(const std::filesystem::path& path);

[[nodiscard]] std::optional<std::vector<std::uint8_t>>
clipboardWaveBytes();

[[nodiscard]] bool copyTextToClipboardUnicodeAndAnsi(
    const std::wstring& unicode_text);

[[nodiscard]] std::optional<std::vector<std::uint8_t>>
clipboardImageBytes();

[[nodiscard]] AudacityWaveImportResult
exportAudacitySelectionToWave();

} // namespace mgstc::platform
