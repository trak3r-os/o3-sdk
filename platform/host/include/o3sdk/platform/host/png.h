// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// PNG output for simulator snapshots (stb_image_write, vendored in third_party/stb). Host only.

#include <cstdint>
#include <vector>

namespace o3sdk::platform::host {

// Encodes `width` x `height` RGBA8 pixels (row-major, 4 bytes per pixel, rows packed) as a PNG.
// The output is deterministic for a given input. false: bad arguments or encoder failure.
bool encode_png_rgba(const std::uint8_t* rgba, int width, int height, std::vector<std::uint8_t>& out);

// encode_png_rgba() into a file (created or truncated). false: encoding or file error.
bool write_png_rgba(const char* path, const std::uint8_t* rgba, int width, int height);

}  // namespace o3sdk::platform::host
