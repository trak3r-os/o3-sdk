// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/formats/sample_probe.h>

#include <cstring>

namespace o3sdk::formats {
namespace {

std::uint32_t le32(const std::uint8_t* p) noexcept {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint32_t be32(const std::uint8_t* p) noexcept {
  return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) | (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

}  // namespace

unsigned sample_channels(ProbeRead read, void* ctx) noexcept {
  std::uint8_t head[12];
  if (read == nullptr || !read(ctx, 0, head, sizeof head)) {
    return 0;
  }
  const bool wav = std::memcmp(head, "RIFF", 4) == 0 && std::memcmp(head + 8, "WAVE", 4) == 0;
  const bool aiff = std::memcmp(head, "FORM", 4) == 0 && (std::memcmp(head + 8, "AIFF", 4) == 0 || std::memcmp(head + 8, "AIFC", 4) == 0);
  if (!wav && !aiff) {
    return 0;
  }
  const char* const header_id = wav ? "fmt " : "COMM";
  const char* const sound_id = wav ? "data" : "SSND";
  std::uint32_t at = 12;
  for (unsigned hop = 0; hop < kProbeHops; ++hop) {
    std::uint8_t chunk[8];
    if (!read(ctx, at, chunk, sizeof chunk)) {
      return 0;
    }
    if (std::memcmp(chunk, header_id, 4) == 0) {
      std::uint8_t two[2];
      if (!read(ctx, at + 8u + (wav ? 2u : 0u), two, sizeof two)) {  // the 'fmt ' chunk: format tag, then channels; COMM: channels first
        return 0;
      }
      const unsigned channels = wav ? (static_cast<unsigned>(two[0]) | (static_cast<unsigned>(two[1]) << 8)) : ((static_cast<unsigned>(two[0]) << 8) | two[1]);
      return channels == 1u || channels == 2u ? channels : 0u;
    }
    if (std::memcmp(chunk, sound_id, 4) == 0) {
      return 0;  // the audio came first: the streamed readers cannot use a header after it
    }
    const std::uint64_t size = wav ? le32(chunk + 4) : be32(chunk + 4);
    const std::uint64_t next = std::uint64_t{at} + 8u + size + (size & 1u);  // chunks are padded to an even length
    if (next > 0xFFFFFFFFull) {
      return 0;
    }
    at = static_cast<std::uint32_t>(next);
  }
  return 0;
}

}  // namespace o3sdk::formats
