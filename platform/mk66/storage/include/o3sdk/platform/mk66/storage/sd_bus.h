// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The seam between the SD protocol (sd_card.h: the init sequence and the block reads, pure and host-tested) and the controller that puts
// bits on the wires (src/sdhc_bus.cpp for the MK66's SDHC, the fake card of the tests). OS-007e. The bus sends the commands it is given (and
// only those: its argument is a `Cmd`, the allowlist of sd_protocol.h), checks the response's CRC and index as the controller does, and
// returns the response words; what the answer means is the card logic's. Every method blocks for a bounded time (sd_policy.h) and runs in the
// main loop only.

#include <o3sdk/platform/mk66/storage/sd_protocol.h>

#include <cstdint>

namespace o3sdk::platform::mk66::storage::sd {

enum class Result : std::uint8_t {
  Ok = 0,
  NoCard,         // no card in the slot (the detect switch says so), or none answered
  NoResponse,     // the command got no answer within its timeout
  CommandError,   // the response failed its CRC, end-bit or index check
  CardError,      // the card's status (R1) reports an error
  BadResponse,    // an answer that is wrong for the command: the CMD8 echo, an RCA of 0, a CSD this driver cannot read
  NotReady,       // the card was still busy when the time was up (ACMD41's second, the busy of an R1b command)
  DataTimeout,    // the data phase did not come, or did not end, in time
  DataError,      // a data block failed its CRC or end bit, or the FIFO under- or overran
  Refused,        // not a command of the allowlist
  BadArgument,    // a block range outside the card, or a bad block size or count
  Unsupported,    // an SD card this driver does not take (SDUC: 2 TiB and up, a block under 512 bytes, 8-bit only)
  Uninitialised,  // a read before a successful init()
  WriteProtected, // OS-006c: a write to a card whose write-protect switch is set or whose CSD says it is write protected
};

inline constexpr const char* result_name(Result r) noexcept {
  switch (r) {
    case Result::Ok: return "ok";
    case Result::NoCard: return "no card";
    case Result::NoResponse: return "no response";
    case Result::CommandError: return "command error";
    case Result::CardError: return "card error";
    case Result::BadResponse: return "bad response";
    case Result::NotReady: return "card not ready";
    case Result::DataTimeout: return "data timeout";
    case Result::DataError: return "data error";
    case Result::Refused: return "command refused";
    case Result::BadArgument: return "bad argument";
    case Result::Unsupported: return "unsupported card";
    case Result::Uninitialised: return "not initialised";
    case Result::WriteProtected: return "write protected";
  }
  return "?";
}

// What a command answered, as the controller's four response registers (a 48-bit response is in word[0]; see unpack_r2 for the 136-bit ones).
struct Response {
  std::uint32_t word[4] = {0, 0, 0, 0};
};

class SdBus {
 public:
  // The card-detect switch: a card is in the slot. No bus traffic.
  virtual bool card_present() const noexcept = 0;
  // The controller from reset: clocks, pins, interrupts off, the identification clock (at most 400 kHz), one data line, and the 80 clocks
  // the card needs before its first command. The card is already powered (the slot has no power switch).
  virtual Result power_up() noexcept = 0;
  // The highest clock not above `max_hz` the controller can make; returns what it made (0: it could not).
  virtual std::uint32_t set_clock(std::uint32_t max_hz) noexcept = 0;
  // 1 or 4 data lines (the card is told first, with ACMD6).
  virtual void set_bus_width(unsigned bits) noexcept = 0;
  // A command without data. Result::Ok: the response is in `out` (CRC and index checked; R1b waited out). Refused when `cmd` is not known().
  virtual Result command(Cmd cmd, std::uint32_t arg, Response& out) noexcept = 0;
  // A command with a read data phase (reads_data(cmd)): `blocks` blocks of `block_size` bytes into `dst` (any alignment), the command's
  // response in `out`. CMD18 is stopped after the last block. On any failure the controller's data circuits are reset and a multiple-block
  // read is stopped, so the next command finds the bus idle; the Result says what failed.
  virtual Result read_data(Cmd cmd, std::uint32_t arg, void* dst, std::uint32_t block_size, std::uint32_t blocks, Response& out) noexcept = 0;
  // A command with a write data phase (writes_data(cmd), CMD24): `blocks` blocks of `block_size` bytes from `src` (any alignment) to the card, the command's
  // response in `out`, then the card's CRC status and the end of its busy (DAT0 high), all bounded by sd_policy.h. A CRC status that is not "accepted" is a
  // DataError. On any failure the controller's data circuits are reset so the next command finds the bus idle; the Result says what failed. Only the
  // card logic's write path calls it (sd_card.h write_blocks), and only for WriteBlock.
  virtual Result write_data(Cmd cmd, std::uint32_t arg, const void* src, std::uint32_t block_size, std::uint32_t blocks, Response& out) noexcept = 0;
  // The slot's write-protect switch says "protected". A slot without such a contact (the Tracker's: none is known to be wired) says false.
  virtual bool write_protect_switch() const noexcept = 0;
  // Waits `ms` milliseconds (a wait that keeps the display serviced on the target: boot::delay_ms).
  virtual void delay_ms(std::uint32_t ms) noexcept = 0;
  // A free-running 32-bit microsecond counter (wraps; only differences are used).
  virtual std::uint32_t micros() noexcept = 0;

 protected:
  SdBus() = default;
  ~SdBus() = default;
  SdBus(const SdBus&) = delete;
  SdBus& operator=(const SdBus&) = delete;
};

}  // namespace o3sdk::platform::mk66::storage::sd
