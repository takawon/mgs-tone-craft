#include "mgstc/engine/gzip_inflate.hpp"

#include <array>
#include <cstddef>
#include <cstring>

namespace mgstc::engine {
namespace {

constexpr std::uint8_t kGzipMagic0 = 0x1F;
constexpr std::uint8_t kGzipMagic1 = 0x8B;
constexpr std::uint8_t kGzipDeflate = 8;
constexpr std::uint8_t kGzipFhcrc = 0x02;
constexpr std::uint8_t kGzipFextra = 0x04;
constexpr std::uint8_t kGzipFname = 0x08;
constexpr std::uint8_t kGzipFcomment = 0x10;

std::uint32_t crc32Update(
    std::uint32_t crc,
    std::span<const std::uint8_t> bytes) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> values{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            auto c = i;
            for (int bit = 0; bit < 8; ++bit) {
                c = (c & 1U) != 0U ? (0xEDB88320U ^ (c >> 1U)) : (c >> 1U);
            }
            values[i] = c;
        }
        return values;
    }();
    crc = ~crc;
    for (const auto byte : bytes) {
        crc = table[(crc ^ byte) & 0xFFU] ^ (crc >> 8U);
    }
    return ~crc;
}

std::uint32_t readU16(std::span<const std::uint8_t> bytes, std::size_t at) {
    return static_cast<std::uint32_t>(bytes[at])
        | (static_cast<std::uint32_t>(bytes[at + 1]) << 8U);
}

std::uint32_t readU32(std::span<const std::uint8_t> bytes, std::size_t at) {
    return readU16(bytes, at)
        | (readU16(bytes, at + 2) << 16U);
}

struct BitReader {
    std::span<const std::uint8_t> data;
    std::size_t byte_pos{};
    std::uint32_t bit_buf{};
    int bit_count{};

    [[nodiscard]] bool getBits(int count, std::uint32_t& value) {
        while (bit_count < count) {
            if (byte_pos >= data.size()) {
                return false;
            }
            bit_buf |= static_cast<std::uint32_t>(data[byte_pos++])
                << bit_count;
            bit_count += 8;
        }
        value = bit_buf & ((1U << count) - 1U);
        bit_buf >>= count;
        bit_count -= count;
        return true;
    }

    void alignByte() {
        bit_buf = 0;
        bit_count = 0;
    }
};

struct Huffman {
    std::array<int, 16> count{};
    std::array<int, 288> symbol{};

    [[nodiscard]] bool build(const std::uint8_t* lengths, int length_count) {
        count.fill(0);
        symbol.fill(0);
        for (int i = 0; i < length_count; ++i) {
            if (lengths[i] > 15) {
                return false;
            }
            ++count[lengths[i]];
        }
        count[0] = 0;
        std::array<int, 16> offsets{};
        int packed = 0;
        for (int bits = 1; bits <= 15; ++bits) {
            offsets[bits] = packed;
            packed += count[bits];
        }
        if (packed > 288) {
            return false;
        }
        for (int i = 0; i < length_count; ++i) {
            const int bits = lengths[i];
            if (bits == 0) {
                continue;
            }
            symbol[offsets[bits]++] = i;
        }
        return true;
    }

    [[nodiscard]] bool decode(BitReader& bits, int& value) const {
        int code = 0;
        int first = 0;
        int index = 0;
        for (int len = 1; len <= 15; ++len) {
            std::uint32_t bit = 0;
            if (!bits.getBits(1, bit)) {
                return false;
            }
            code = (code << 1) | static_cast<int>(bit);
            const int symbols = this->count[len];
            if (code - first < symbols) {
                value = symbol[index + (code - first)];
                return true;
            }
            index += symbols;
            first = (first + symbols) << 1;
        }
        return false;
    }
};

constexpr std::array<int, 29> kLengthBase{
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
    67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<int, 29> kLengthExtra{
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5,
    5, 5, 5, 0};
constexpr std::array<int, 32> kDistBase{
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
    769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577, 0, 0};
constexpr std::array<int, 32> kDistExtra{
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10,
    10, 11, 11, 12, 12, 13, 13, 0, 0};

bool inflate(
    std::span<const std::uint8_t> deflate,
    std::vector<std::uint8_t>& output,
    std::string* error) {
    BitReader bits{deflate};
    Huffman lit;
    Huffman dist;
    std::uint8_t lit_len[288];
    std::uint8_t dist_len[32];

    const auto fail = [&](const char* message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };

    while (true) {
        std::uint32_t bfinal = 0;
        std::uint32_t btype = 0;
        if (!bits.getBits(1, bfinal) || !bits.getBits(2, btype)) {
            return fail("truncated deflate stream");
        }
        if (btype == 3) {
            return fail("invalid deflate block type");
        }
        if (btype == 0) {
            bits.alignByte();
            if (bits.byte_pos + 4 > bits.data.size()) {
                return fail("truncated stored deflate block");
            }
            const auto len = readU16(bits.data, bits.byte_pos);
            const auto nlen = readU16(bits.data, bits.byte_pos + 2);
            bits.byte_pos += 4;
            if (static_cast<std::uint16_t>(len ^ 0xFFFFU) != nlen) {
                return fail("invalid stored deflate length");
            }
            if (bits.byte_pos + len > bits.data.size()) {
                return fail("truncated stored deflate payload");
            }
            output.insert(
                output.end(),
                bits.data.begin() + static_cast<std::ptrdiff_t>(bits.byte_pos),
                bits.data.begin()
                    + static_cast<std::ptrdiff_t>(bits.byte_pos + len));
            bits.byte_pos += len;
        } else {
            if (btype == 1) {
                std::memset(lit_len, 0, sizeof(lit_len));
                std::memset(dist_len, 0, sizeof(dist_len));
                for (int i = 0; i <= 143; ++i) {
                    lit_len[i] = 8;
                }
                for (int i = 144; i <= 255; ++i) {
                    lit_len[i] = 9;
                }
                for (int i = 256; i <= 279; ++i) {
                    lit_len[i] = 7;
                }
                for (int i = 280; i <= 287; ++i) {
                    lit_len[i] = 8;
                }
                for (int i = 0; i < 32; ++i) {
                    dist_len[i] = 5;
                }
            } else {
                std::uint32_t hlit = 0;
                std::uint32_t hdist = 0;
                std::uint32_t hclen = 0;
                if (!bits.getBits(5, hlit)
                    || !bits.getBits(5, hdist)
                    || !bits.getBits(4, hclen)) {
                    return fail("truncated dynamic Huffman header");
                }
                hlit += 257;
                hdist += 1;
                hclen += 4;
                if (hlit > 288 || hdist > 32) {
                    return fail("invalid dynamic Huffman counts");
                }
                constexpr int order[19] = {
                    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14,
                    1, 15};
                std::uint8_t clen[19]{};
                for (std::uint32_t i = 0; i < hclen; ++i) {
                    std::uint32_t bits_len = 0;
                    if (!bits.getBits(3, bits_len)) {
                        return fail("truncated code-length codes");
                    }
                    clen[order[i]] = static_cast<std::uint8_t>(bits_len);
                }
                Huffman codes;
                if (!codes.build(clen, 19)) {
                    return fail("invalid code-length Huffman table");
                }
                std::uint8_t combined[320]{};
                int filled = 0;
                const int total = static_cast<int>(hlit + hdist);
                while (filled < total) {
                    int symbol = 0;
                    if (!codes.decode(bits, symbol)) {
                        return fail("invalid code-length symbol");
                    }
                    int repeat = 1;
                    std::uint8_t value = 0;
                    if (symbol <= 15) {
                        value = static_cast<std::uint8_t>(symbol);
                    } else if (symbol == 16) {
                        if (filled == 0) {
                            return fail("invalid repeat of previous length");
                        }
                        std::uint32_t extra = 0;
                        if (!bits.getBits(2, extra)) {
                            return fail("truncated length repeat");
                        }
                        repeat = static_cast<int>(3 + extra);
                        value = combined[filled - 1];
                    } else if (symbol == 17) {
                        std::uint32_t extra = 0;
                        if (!bits.getBits(3, extra)) {
                            return fail("truncated zero run");
                        }
                        repeat = static_cast<int>(3 + extra);
                    } else if (symbol == 18) {
                        std::uint32_t extra = 0;
                        if (!bits.getBits(7, extra)) {
                            return fail("truncated long zero run");
                        }
                        repeat = static_cast<int>(11 + extra);
                    } else {
                        return fail("unknown code-length symbol");
                    }
                    if (filled + repeat > total) {
                        return fail("dynamic Huffman lengths overflow");
                    }
                    for (int i = 0; i < repeat; ++i) {
                        combined[filled++] = value;
                    }
                }
                std::memcpy(lit_len, combined, hlit);
                std::memset(lit_len + hlit, 0, 288 - hlit);
                std::memcpy(dist_len, combined + hlit, hdist);
                std::memset(dist_len + hdist, 0, 32 - hdist);
            }
            if (!lit.build(lit_len, 288) || !dist.build(dist_len, 32)) {
                return fail("invalid Huffman tables");
            }
            while (true) {
                int symbol = 0;
                if (!lit.decode(bits, symbol)) {
                    return fail("truncated Huffman literal");
                }
                if (symbol < 256) {
                    output.push_back(static_cast<std::uint8_t>(symbol));
                    continue;
                }
                if (symbol == 256) {
                    break;
                }
                const int length_index = symbol - 257;
                if (length_index < 0 || length_index >= 29) {
                    return fail("invalid length symbol");
                }
                std::uint32_t extra = 0;
                if (kLengthExtra[static_cast<std::size_t>(length_index)] > 0
                    && !bits.getBits(
                        kLengthExtra[static_cast<std::size_t>(length_index)],
                        extra)) {
                    return fail("truncated length extra bits");
                }
                const int length =
                    kLengthBase[static_cast<std::size_t>(length_index)]
                    + static_cast<int>(extra);
                int dist_symbol = 0;
                if (!dist.decode(bits, dist_symbol) || dist_symbol >= 30) {
                    return fail("invalid distance symbol");
                }
                extra = 0;
                if (kDistExtra[static_cast<std::size_t>(dist_symbol)] > 0
                    && !bits.getBits(
                        kDistExtra[static_cast<std::size_t>(dist_symbol)],
                        extra)) {
                    return fail("truncated distance extra bits");
                }
                const int distance =
                    kDistBase[static_cast<std::size_t>(dist_symbol)]
                    + static_cast<int>(extra);
                if (distance <= 0
                    || static_cast<std::size_t>(distance) > output.size()) {
                    return fail("invalid back-reference distance");
                }
                for (int i = 0; i < length; ++i) {
                    output.push_back(
                        output[output.size()
                            - static_cast<std::size_t>(distance)]);
                }
            }
        }
        if (bfinal != 0U) {
            return true;
        }
    }
}

}  // namespace

bool looksLikeGzip(std::span<const std::uint8_t> bytes) noexcept {
    return bytes.size() >= 10
        && bytes[0] == kGzipMagic0
        && bytes[1] == kGzipMagic1;
}

std::optional<std::vector<std::uint8_t>> inflateGzip(
    std::span<const std::uint8_t> bytes,
    std::string* error) {
    const auto fail = [&](const char* message)
        -> std::optional<std::vector<std::uint8_t>> {
        if (error != nullptr) {
            *error = message;
        }
        return std::nullopt;
    };
    if (!looksLikeGzip(bytes)) {
        return fail("not a gzip stream");
    }
    if (bytes[2] != kGzipDeflate) {
        return fail("gzip compression method is not deflate");
    }
    const auto flags = bytes[3];
    std::size_t position = 10;
    if ((flags & kGzipFextra) != 0) {
        if (position + 2 > bytes.size()) {
            return fail("truncated gzip extra field");
        }
        const auto extra = readU16(bytes, position);
        position += 2 + extra;
    }
    if ((flags & kGzipFname) != 0) {
        while (position < bytes.size() && bytes[position] != 0) {
            ++position;
        }
        ++position;
    }
    if ((flags & kGzipFcomment) != 0) {
        while (position < bytes.size() && bytes[position] != 0) {
            ++position;
        }
        ++position;
    }
    if ((flags & kGzipFhcrc) != 0) {
        position += 2;
    }
    if (position + 8 > bytes.size()) {
        return fail("truncated gzip stream");
    }
    const auto deflate = bytes.subspan(
        position, bytes.size() - position - 8);
    std::vector<std::uint8_t> output;
    output.reserve(bytes.size() * 2);
    std::string inflate_error;
    if (!inflate(deflate, output, &inflate_error)) {
        return fail(
            inflate_error.empty() ? "gzip inflate failed" : inflate_error.c_str());
    }
    const auto crc_at = bytes.size() - 8;
    const auto crc = readU32(bytes, crc_at);
    const auto isize = readU32(bytes, crc_at + 4);
    if (isize != static_cast<std::uint32_t>(output.size())) {
        return fail("gzip size mismatch");
    }
    if (crc32Update(0, output) != crc) {
        return fail("gzip crc mismatch");
    }
    return output;
}

}  // namespace mgstc::engine
