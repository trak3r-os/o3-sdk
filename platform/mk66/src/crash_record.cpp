// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/platform/mk66/crash_record.h>

namespace o3sdk::platform::mk66::crash {
namespace {

constexpr std::size_t kCrcCovered = offsetof(CrashRecord, crc);

}  // namespace

std::uint32_t crc32(const void* data, std::size_t size) noexcept {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  std::uint32_t crc = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= bytes[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return ~crc;
}

bool valid(const CrashRecord& r) noexcept {
  return r.magic == kMagic && r.version == kVersion && r.size == sizeof(CrashRecord) && r.crc == crc32(&r, kCrcCovered);
}

CrashRecord encode(const FaultSnapshot& snapshot, const CrashRecord& previous) noexcept {
  std::uint32_t count = 1;
  if (valid(previous)) {
    count = previous.count == 0xFFFFFFFFu ? previous.count : previous.count + 1u;
  }
  // Every field is named: no zero-fill call (this runs in a fault handler) and no field left to chance. The fields are whole
  // words or half words that tile the struct, so there is no padding for the CRC to cover.
  CrashRecord r = {kMagic,
                   kVersion,
                   sizeof(CrashRecord),
                   count,
                   snapshot.exception,
                   snapshot.exc_return,
                   snapshot.cfsr,
                   snapshot.hfsr,
                   snapshot.mmfar,
                   snapshot.bfar,
                   snapshot.pc,
                   snapshot.lr,
                   snapshot.xpsr,
                   snapshot.sp,
                   snapshot.context,
                   snapshot.uptime_ms,
                   0};
  r.crc = crc32(&r, kCrcCovered);
  return r;
}

FaultSnapshot snapshot(const FaultRegisters& registers, const std::uint32_t* frame, std::uint32_t frame_address,
                       std::uint32_t pc_if_no_frame) noexcept {
  const bool usable = frame != nullptr && frame_plausible(frame_address);
  // Every field is named in the aggregate: no zero-fill call from a fault handler, and none left to chance.
  return FaultSnapshot{registers.exception,
                       registers.exc_return,
                       registers.cfsr,
                       registers.hfsr,
                       registers.mmfar,
                       registers.bfar,
                       usable ? frame[kFramePc] : pc_if_no_frame,
                       usable ? frame[kFrameLr] : pc_if_no_frame,
                       usable ? frame[kFrameXpsr] : 0u,
                       usable ? frame_address : kInvalidFrame,
                       registers.context,
                       registers.uptime_ms};
}

FaultSnapshot decode(const CrashRecord& r) noexcept {
  return FaultSnapshot{r.exception, r.exc_return, r.cfsr, r.hfsr, r.mmfar, r.bfar,
                       r.pc,        r.lr,         r.xpsr, r.sp,   r.context, r.uptime_ms};
}

}  // namespace o3sdk::platform::mk66::crash
