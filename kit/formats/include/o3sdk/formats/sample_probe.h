// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace o3sdk::formats {

// OS-LOADCH: how many channels a WAV or AIFF file has, from its header alone, so that the Sample load list can tag a stereo file ("ST") before anything is loaded. The file is reached
// through `read`: `n` bytes at byte `offset` into `dst`, false when the file ends first (the caller's seek + read). The walk goes chunk by chunk (RIFF: 'fmt ', FORM: 'COMM'), seeking over the chunks in
// front of it (LIST, bext, ...), at most kProbeHops of them; it stops at the sound data, as the streamed readers do (an AIFF whose SSND comes first is refused by the load, so it is not tagged).
// Returns 1 or 2, or 0 when the file is not one of those, the header is not reached or the count is any other. Pure: no allocation, no state; each hop is one small read.
inline constexpr unsigned kProbeHops = 16;
using ProbeRead = bool (*)(void* ctx, std::uint32_t offset, std::uint8_t* dst, std::size_t n);
unsigned sample_channels(ProbeRead read, void* ctx) noexcept;

}  // namespace o3sdk::formats
