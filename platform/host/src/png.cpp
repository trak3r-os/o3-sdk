// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/platform/host/png.h>

#include <cstdio>

// Declarations only: the implementation is in stb_image_write_impl.cpp.
#define STBI_WRITE_NO_STDIO
#include <stb/stb_image_write.h>

namespace o3sdk::platform::host {

namespace {
void append(void* context, void* data, int size) {
  auto* out = static_cast<std::vector<std::uint8_t>*>(context);
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  out->insert(out->end(), bytes, bytes + size);
}
}  // namespace

bool encode_png_rgba(const std::uint8_t* rgba, int width, int height, std::vector<std::uint8_t>& out) {
  out.clear();
  if (rgba == nullptr || width <= 0 || height <= 0) {
    return false;
  }
  // The pictures are for looking at and are compared by the hash of their pixels, not of the file: a shorter search for
  // matches (stb's default is 8) gives a somewhat larger PNG in a fraction of the time, which every gallery pays per picture.
  stbi_write_png_compression_level = 2;
  return stbi_write_png_to_func(append, &out, width, height, 4, rgba, width * 4) != 0;
}

bool write_png_rgba(const char* path, const std::uint8_t* rgba, int width, int height) {
  std::vector<std::uint8_t> png;
  if (path == nullptr || !encode_png_rgba(rgba, width, height, png)) {
    return false;
  }
  std::FILE* file = std::fopen(path, "wb");
  if (file == nullptr) {
    return false;
  }
  const bool written = std::fwrite(png.data(), 1, png.size(), file) == png.size();
  return (std::fclose(file) == 0) && written;
}

}  // namespace o3sdk::platform::host
