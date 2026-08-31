#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace bhs::renderer {

// Minimal, dependency-free PNG writer used by the offscreen capture mode.
//
// It emits a standard 8-bit RGB PNG.  The zlib stream inside uses "stored"
// (uncompressed) deflate blocks: that is a fully legal deflate encoding, so
// every PNG reader accepts it, and it keeps this file short enough to read.
// The resulting file is larger than a compressed PNG, which is irrelevant for
// development screenshots.
//
// `pixels` must contain height * width * 3 bytes in top-to-bottom row order.
bool writePng(const std::filesystem::path& path,
              int width,
              int height,
              const std::vector<std::uint8_t>& rgbPixels);

} // namespace bhs::renderer
