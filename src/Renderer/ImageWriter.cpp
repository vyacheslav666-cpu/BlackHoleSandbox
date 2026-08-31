#include "Renderer/ImageWriter.hpp"

#include <array>
#include <cstring>
#include <fstream>

namespace bhs::renderer {

namespace {

// --- CRC-32 (PNG chunk checksum) -------------------------------------------
// Standard reflected CRC-32 with polynomial 0xEDB88320, as specified by PNG.
std::array<std::uint32_t, 256> makeCrcTable() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t n = 0; n < 256; ++n) {
        std::uint32_t c = n;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1u) != 0u ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        table[n] = c;
    }
    return table;
}

std::uint32_t crc32(const std::uint8_t* data, std::size_t length) {
    static const std::array<std::uint32_t, 256> table = makeCrcTable();
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < length; ++i) {
        c = table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

// --- Adler-32 (zlib stream checksum) ---------------------------------------
std::uint32_t adler32(const std::uint8_t* data, std::size_t length) {
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (std::size_t i = 0; i < length; ++i) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

void pushBigEndian32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

// A PNG chunk is: length, 4-character type, payload, CRC over (type+payload).
void pushChunk(std::vector<std::uint8_t>& out, const char type[4],
               const std::vector<std::uint8_t>& payload) {
    pushBigEndian32(out, static_cast<std::uint32_t>(payload.size()));
    const std::size_t crcStart = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), payload.begin(), payload.end());
    pushBigEndian32(out, crc32(out.data() + crcStart, out.size() - crcStart));
}

// Wrap raw bytes in a zlib stream built from stored (uncompressed) deflate
// blocks.  Each block carries at most 65535 bytes and is preceded by a
// 5-byte header: 1 flag byte, then LEN and its one's complement, little endian.
std::vector<std::uint8_t> storedZlibStream(const std::vector<std::uint8_t>& raw) {
    std::vector<std::uint8_t> out;
    out.reserve(raw.size() + raw.size() / 65535 * 5 + 16);
    out.push_back(0x78); // CMF: deflate, 32 KiB window.
    out.push_back(0x01); // FLG: no preset dictionary, fastest-compression hint.

    std::size_t offset = 0;
    if (raw.empty()) {
        out.push_back(0x01);
        out.push_back(0x00);
        out.push_back(0x00);
        out.push_back(0xFF);
        out.push_back(0xFF);
    }
    while (offset < raw.size()) {
        const std::size_t remaining = raw.size() - offset;
        const std::uint16_t blockLength = static_cast<std::uint16_t>(remaining > 65535 ? 65535 : remaining);
        const bool isFinal = (offset + blockLength) >= raw.size();
        out.push_back(isFinal ? 0x01 : 0x00);
        out.push_back(static_cast<std::uint8_t>(blockLength & 0xFF));
        out.push_back(static_cast<std::uint8_t>(blockLength >> 8));
        out.push_back(static_cast<std::uint8_t>(~blockLength & 0xFF));
        out.push_back(static_cast<std::uint8_t>((~blockLength >> 8) & 0xFF));
        out.insert(out.end(), raw.begin() + static_cast<std::ptrdiff_t>(offset),
                   raw.begin() + static_cast<std::ptrdiff_t>(offset + blockLength));
        offset += blockLength;
    }
    pushBigEndian32(out, adler32(raw.data(), raw.size()));
    return out;
}

} // namespace

bool writePng(const std::filesystem::path& path, int width, int height,
              const std::vector<std::uint8_t>& rgbPixels) {
    if (width <= 0 || height <= 0) {
        return false;
    }
    const std::size_t expected = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u;
    if (rgbPixels.size() < expected) {
        return false;
    }

    // PNG scanlines are each prefixed by a filter-type byte; 0 means "None".
    std::vector<std::uint8_t> filtered;
    filtered.reserve(expected + static_cast<std::size_t>(height));
    const std::size_t stride = static_cast<std::size_t>(width) * 3u;
    for (int y = 0; y < height; ++y) {
        filtered.push_back(0);
        const std::uint8_t* row = rgbPixels.data() + static_cast<std::size_t>(y) * stride;
        filtered.insert(filtered.end(), row, row + stride);
    }

    std::vector<std::uint8_t> file = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

    std::vector<std::uint8_t> header;
    pushBigEndian32(header, static_cast<std::uint32_t>(width));
    pushBigEndian32(header, static_cast<std::uint32_t>(height));
    header.push_back(8); // Bit depth.
    header.push_back(2); // Colour type 2 = truecolour RGB.
    header.push_back(0); // Deflate compression.
    header.push_back(0); // Adaptive filtering.
    header.push_back(0); // No interlacing.
    pushChunk(file, "IHDR", header);
    pushChunk(file, "IDAT", storedZlibStream(filtered));
    pushChunk(file, "IEND", {});

    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return false;
    }
    stream.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    return stream.good();
}

} // namespace bhs::renderer
