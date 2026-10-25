// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// How long the SD driver may wait for anything (OS-007e), in one place. Every wait in the image's SDHC code (src/sdhc_bus.cpp) is a
// `Deadline` made from one of these, so nothing in the driver can spin without an end, and the longest the main loop can be held by one
// call is a number this file states and a test checks. Pure.
//
// The limits are the spec's, rounded up with margin, or (where the spec gives none) the controller's own: the SDHC raises a command
// timeout 64 SD clocks after a command with no answer, so a command's software limit only has to outlast the longest honest answer
// (a 136-bit response at 400 kHz is 0.4 ms). A data timeout is the one that matters: an absent or hung card does not answer for the
// controller's own 2^27 clocks (3 to 6 s), so the software limit cuts it at a fraction of a second and resets the controller.

#include <cstdint>

namespace o3sdk::platform::mk66::storage::sd::policy {

// ---- Time in the unit the code reads ----------------------------------------------------------------------------------------------------
// A Deadline works on a free-running 32-bit counter of any unit (the DWT cycle counter on the target, a made-up counter in the tests):
// `now - start` in unsigned arithmetic, so a wrap of the counter between start and now does not matter while the span is under half the range.
struct Deadline {
  std::uint32_t start;
  std::uint32_t limit;
  constexpr bool expired(std::uint32_t now) const noexcept { return static_cast<std::uint32_t>(now - start) >= limit; }
};

// Microseconds to counter ticks at `ticks_per_us` (180 on the target), saturating below half the counter range so that `expired` stays
// meaningful (a limit of 2^31 or more would be mistaken for "already expired" after a wrap).
constexpr std::uint32_t to_ticks(std::uint32_t us, std::uint32_t ticks_per_us) noexcept {
  const std::uint64_t t = static_cast<std::uint64_t>(us) * ticks_per_us;
  return t > 0x7FFFFFFFull ? 0x7FFFFFFFu : static_cast<std::uint32_t>(t);
}

// ---- The limits, in microseconds ----------------------------------------------------------------------------------------------------------
inline constexpr std::uint32_t kControllerResetUs = 100'000;   // RSTA / RSTC / RSTD clear themselves within a few clocks
inline constexpr std::uint32_t kClockStableUs = 100'000;       // SDSTB after a divider change
inline constexpr std::uint32_t kInitClocksUs = 10'000;         // the 80 initialisation clocks at 400 kHz take 0.2 ms
inline constexpr std::uint32_t kInhibitUs = 50'000;            // CIHB / CDIHB clear: the previous command's own end
inline constexpr std::uint32_t kCommandUs = 10'000;            // command complete, or the controller's timeout / error flag
inline constexpr std::uint32_t kBusyUs = 500'000;              // an R1b command: the card releases DAT0 (spec: up to 500 ms for the busy kinds)
inline constexpr std::uint32_t kDataBaseUs = 250'000;          // a data command: the first block (the spec's read timeout is 100 ms)
inline constexpr std::uint32_t kDataPerBlockUs = 2'000;        // and each block after it (a 512-byte block is 23 us at 22 MHz on 4 lines)
inline constexpr std::uint32_t kAcmd41TotalUs = 1'000'000;     // the spec: a card is ready within 1 s of ACMD41
inline constexpr std::uint32_t kAcmd41PollMs = 10;             // between two ACMD41s
inline constexpr std::uint32_t kPowerSettleMs = 2;             // after the controller is on and the pins are muxed, before the 80 clocks (spec: 1 ms)

constexpr std::uint32_t data_timeout_us(std::uint32_t blocks) noexcept { return kDataBaseUs + kDataPerBlockUs * blocks; }

// ---- Chunking and retries ---------------------------------------------------------------------------------------------------------------
// One CMD18 moves at most this many blocks (16 KB, about 0.7 ms at 22 MB/s), so a long read is a series of short blocking calls and a
// failed one costs a chunk, not the whole request.
inline constexpr std::uint32_t kMaxBlocksPerCommand = 32;
// A data error that is not a timeout (a CRC, an end bit, a FIFO fault) is retried this many times, each after a reset of the controller's data
// circuits and a CMD12, before the read fails. A timeout is not retried: a card that did not answer for a quarter of a second is not
// going to, and the next try would hold the main loop for as long again.
inline constexpr unsigned kReadRetries = 2;
inline constexpr std::uint32_t kStopBusyUs = 100'000;  // the busy of the CMD12 that ends a failed multi-block read

// The longest one read() call holds the main loop when the card fails: one data timeout (the retries after a CRC error are as short as the
// error is), the stop command and its busy, and the commands of the tries.
constexpr std::uint32_t worst_read_call_us() noexcept {
  return data_timeout_us(kMaxBlocksPerCommand) + kStopBusyUs + 2u * (kReadRetries + 1u) * kCommandUs;
}

// ---- Writes (OS-006c) -----------------------------------------------------------------------------------------------------------------
// One CMD24 is one block, one at a time (CMD25 is not on the allowlist). The controller takes the block from the FIFO as the card accepts it, the
// card answers the data with a CRC status token and holds DAT0 low while it programs the block (the spec: an SDHC/SDXC card within 250 ms), and
// the host asks CMD13 until the card is back in the transfer state. A card that hangs costs one of each bound, once: the write fails, the caller aborts.
inline constexpr std::uint32_t kWriteDataUs = 250'000;    // the block is in the card: the FIFO fills and the transfer-complete flag comes
inline constexpr std::uint32_t kWriteBusyUs = 500'000;    // DAT0 high again (the programming ended)
inline constexpr std::uint32_t kWriteStatusUs = 500'000;  // CMD13 until READY_FOR_DATA and the transfer state

// The longest one write call holds the main loop for ONE block when the card fails: the command, the data phase, the busy, the status polls.
constexpr std::uint32_t worst_write_block_us() noexcept { return 3u * kCommandUs + kWriteDataUs + kWriteBusyUs + kWriteStatusUs; }

// The longest a whole init() can take when the card never gets ready: the 80 clocks, ACMD41 for its full second, nothing after it.
constexpr std::uint32_t worst_init_us() noexcept {
  return kPowerSettleMs * 1000u + kControllerResetUs + kClockStableUs + kInitClocksUs + kAcmd41TotalUs + 8u * kCommandUs;
}

}  // namespace o3sdk::platform::mk66::storage::sd::policy
