#include "mgstc/engine/timbre_tags.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace mgstc::engine {
namespace {

constexpr std::array<std::string_view, 78> kPresetTags{
    "ピアノ",
    "E.ピアノ",
    "オルガン",
    "ハープシコード",
    "クラビネット",
    "アコースティックギター",
    "エレキギター",
    "ディストーションギター",
    "ナイロンギター",
    "スチールギター",
    "ジャズギター",
    "クリーンギター",
    "ミュートギター",
    "アコースティックベース",
    "シンセベース",
    "フレットレスベース",
    "スラップベース",
    "バイオリン",
    "ビオラ",
    "チェロ",
    "コントラバス",
    "ストリングス",
    "ハープ",
    "ピチカート",
    "シンセストリングス",
    "オーケストラ",
    "トランペット",
    "トロンボーン",
    "ホルン",
    "チューバ",
    "ブラス",
    "シンセブラス",
    "フルート",
    "ピッコロ",
    "クラリネット",
    "オーボエ",
    "ファゴット",
    "サックス",
    "リコーダー",
    "パンフルート",
    "ハーモニカ",
    "アコーディオン",
    "バグパイプ",
    "ホイッスル",
    "オカリナ",
    "バスドラム",
    "スネア",
    "タム",
    "ハイハット",
    "シンバル",
    "パーカッション",
    "ティンパニ",
    "マリンバ",
    "ビブラフォン",
    "シロフォン",
    "ベル",
    "ドラムキット",
    "チェレスタ",
    "グロッケン",
    "チューブラーベル",
    "スチールドラム",
    "カリンバ",
    "アゴゴ",
    "ウッドブロック",
    "和太鼓",
    "琴",
    "三味線",
    "尺八",
    "シンセリード",
    "シンセパッド",
    "プラック",
    "アルペジオ",
    "クワイア",
    "ボイス",
    "効果音",
    "チップチューン",
    "ノイズ",
    "ドローン",
};

[[nodiscard]] bool isAsciiSpace(unsigned char value) {
    return std::isspace(value) != 0;
}

[[nodiscard]] std::string trimAscii(std::string_view text) {
    while (!text.empty()
           && isAsciiSpace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty()
           && isAsciiSpace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return std::string(text);
}

[[nodiscard]] std::string asciiFold(std::string_view text) {
    std::string folded(text);
    std::transform(
        folded.begin(),
        folded.end(),
        folded.begin(),
        [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
    return folded;
}

[[nodiscard]] bool sameTag(
    std::string_view left,
    std::string_view right) {
    return asciiFold(left) == asciiFold(right);
}

void appendUnique(
    std::vector<std::string>& tags,
    std::string_view candidate) {
    auto trimmed = trimAscii(candidate);
    if (trimmed.empty()) {
        return;
    }
    const auto duplicate = std::any_of(
        tags.begin(), tags.end(),
        [&trimmed](const std::string& existing) {
            return sameTag(existing, trimmed);
        });
    if (!duplicate) {
        tags.push_back(std::move(trimmed));
    }
}

}  // namespace

std::span<const std::string_view> presetTimbreTags() noexcept {
    return kPresetTags;
}

bool isPresetTimbreTag(std::string_view tag) {
    const auto trimmed = trimAscii(tag);
    return std::any_of(
        kPresetTags.begin(),
        kPresetTags.end(),
        [&trimmed](std::string_view preset) {
            return sameTag(preset, trimmed);
        });
}

bool rewriteTimbreTag(
    std::vector<std::string>& tags,
    std::string_view source,
    std::string_view replacement) {
    const auto normalized_source = trimAscii(source);
    const auto normalized_replacement = trimAscii(replacement);
    if (normalized_source.empty()
        || isPresetTimbreTag(normalized_source)) {
        return false;
    }

    const auto has_source = std::any_of(
        tags.begin(), tags.end(),
        [&normalized_source](const std::string& tag) {
            return sameTag(tag, normalized_source);
        });
    if (!has_source) {
        return false;
    }

    std::vector<std::string> rewritten;
    rewritten.reserve(tags.size());
    for (const auto& tag : tags) {
        if (sameTag(tag, normalized_source)) {
            if (!normalized_replacement.empty()) {
                appendUnique(rewritten, normalized_replacement);
            }
        } else {
            appendUnique(rewritten, tag);
        }
    }
    if (rewritten == tags) {
        return false;
    }
    tags = std::move(rewritten);
    return true;
}

std::vector<std::string> parseTimbreTags(
    std::string_view serialized) {
    std::vector<std::string> tags;
    std::size_t begin = 0;
    for (std::size_t index = 0; index < serialized.size();) {
        std::size_t delimiter_size{};
        const auto value =
            static_cast<unsigned char>(serialized[index]);
        if (value == ',' || value == ';'
            || value == '\r' || value == '\n') {
            delimiter_size = 1;
        } else if (index + 2 < serialized.size()
                   && value == 0xEF
                   && static_cast<unsigned char>(
                          serialized[index + 1]) == 0xBC
                   && static_cast<unsigned char>(
                          serialized[index + 2]) == 0x8C) {
            delimiter_size = 3;
        }
        if (delimiter_size == 0) {
            ++index;
            continue;
        }
        appendUnique(tags, serialized.substr(begin, index - begin));
        index += delimiter_size;
        begin = index;
    }
    appendUnique(tags, serialized.substr(begin));
    return tags;
}

std::string serializeTimbreTags(
    std::span<const std::string> tags) {
    std::vector<std::string> normalized;
    normalized.reserve(tags.size());
    for (const auto& tag : tags) {
        appendUnique(normalized, tag);
    }
    std::string result;
    for (const auto& tag : normalized) {
        if (!result.empty()) {
            result.push_back(',');
        }
        result += tag;
    }
    return result;
}

std::string normalizeTimbreTags(std::string_view serialized) {
    const auto tags = parseTimbreTags(serialized);
    return serializeTimbreTags(tags);
}

bool containsAllTimbreTags(
    std::string_view serialized,
    std::span<const std::string> required_tags) {
    const auto available = parseTimbreTags(serialized);
    return containsAllTimbreTags(available, required_tags);
}

bool containsAllTimbreTags(
    std::span<const std::string> available_tags,
    std::span<const std::string> required_tags) {
    return std::all_of(
        required_tags.begin(),
        required_tags.end(),
        [available_tags](const std::string& required) {
            return std::any_of(
                available_tags.begin(),
                available_tags.end(),
                [&required](const std::string& tag) {
                    return sameTag(tag, required);
                });
        });
}

std::vector<TimbreTagUsage> collectTimbreTagUsage(
    std::span<const std::string> serialized_tag_sets) {
    std::vector<std::vector<std::string>> tag_sets;
    tag_sets.reserve(serialized_tag_sets.size());
    for (const auto& serialized : serialized_tag_sets) {
        tag_sets.push_back(parseTimbreTags(serialized));
    }
    return collectTimbreTagUsage(tag_sets);
}

std::vector<TimbreTagUsage> collectTimbreTagUsage(
    std::span<const std::vector<std::string>> tag_sets) {
    std::vector<TimbreTagUsage> usage;
    for (const auto& tags : tag_sets) {
        for (const auto& tag : tags) {
            const auto found = std::find_if(
                usage.begin(),
                usage.end(),
                [&tag](const TimbreTagUsage& item) {
                    return sameTag(item.name, tag);
                });
            if (found == usage.end()) {
                usage.push_back({tag, 1});
            } else {
                ++found->count;
            }
        }
    }
    std::sort(
        usage.begin(),
        usage.end(),
        [](const TimbreTagUsage& left, const TimbreTagUsage& right) {
            return asciiFold(left.name) < asciiFold(right.name);
        });
    return usage;
}

}  // namespace mgstc::engine
