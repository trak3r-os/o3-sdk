// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The SD memory card as a block device (OS-007e): the identification and selection sequence of the spec, the bus width and the
// speed, then block reads in bounded chunks. OS-006c adds single-block writes (CMD24) for the firmware writer and nothing else. Pure logic over `SdBus` (sd_bus.h), so the host tests and QEMU run the very code the image
// runs against a software model of a card (tests/sd_fake_card.h): the order of the commands, the SDHC / SDSC difference, a card that never
// gets ready, a failed high-speed switch, every error path, and that no command outside the allowlist is ever sent.
//
// The init sequence (the spec's order):
//   power_up (reset, 400 kHz, 80 clocks)  CMD0  CMD8 (0x1AA)  { CMD55 + ACMD41 (HCS | 3.3 V window) } until not busy, at most 1 s
//   CMD2 (CID)  CMD3 (RCA)  CMD9 (CSD, the capacity)  CMD7 (select)  [CMD16 (512) on a byte-addressed card]
//   CMD55 + ACMD6 (4 lines)  25 MHz  CMD6 check, CMD6 switch (high speed)  50 MHz (45 MHz made)
// A card that does not answer CMD8 is a version 1 card (HCS 0, byte addressed). A high-speed step that fails is not an error: the card stays at
// the default speed and `high_speed` says false.
//
// Runs in the main loop; every wait is bounded (sd_policy.h). Nothing here allocates.

#include <o3sdk/platform/mk66/storage/block_device.h>
#include <o3sdk/platform/mk66/storage/sd_bus.h>
#include <o3sdk/platform/mk66/storage/sd_policy.h>
#include <o3sdk/platform/mk66/storage/sd_protocol.h>

#include <cstdint>

namespace o3sdk::platform::mk66::storage::sd {

// Where init() was (and, after a failure, where it stopped).
enum class InitStage : std::uint8_t { None, PowerUp, Idle, IfCond, OpCond, Cid, Rca, Csd, Select, BlockLen, BusWidth, Speed, Done };

inline constexpr const char* stage_name(InitStage s) noexcept {
  switch (s) {
    case InitStage::None: return "not started";
    case InitStage::PowerUp: return "power-up";
    case InitStage::Idle: return "CMD0";
    case InitStage::IfCond: return "CMD8";
    case InitStage::OpCond: return "ACMD41";
    case InitStage::Cid: return "CMD2";
    case InitStage::Rca: return "CMD3";
    case InitStage::Csd: return "CMD9";
    case InitStage::Select: return "CMD7";
    case InitStage::BlockLen: return "CMD16";
    case InitStage::BusWidth: return "ACMD6";
    case InitStage::Speed: return "CMD6";
    case InitStage::Done: return "done";
  }
  return "?";
}

struct CardInfo {
  bool ready = false;            // init() succeeded and the card is selected, in transfer state
  bool version2 = false;         // answered CMD8: a spec 2.00 or later card
  bool block_addressed = false;  // CCS: SDHC or SDXC (the argument of a read is a block number); false: SDSC (a byte address)
  bool high_speed = false;       // the CMD6 switch to 50 MHz took
  std::uint8_t bus_width = 1;
  std::uint32_t clock_hz = 0;    // what the controller made for the card's speed
  std::uint32_t ocr = 0;
  std::uint16_t rca = 0;
  std::uint32_t blocks = 0;      // capacity in 512-byte blocks
  std::uint32_t capacity_mb = 0;
  std::uint8_t csd_version = 0;
  Cid cid{};
  // The two registers as the card sent them (the spec's byte order, the CRC byte zero), for `diag sd`: Linux shows the same 16 bytes in
  // /sys/block/mmcblk0/device/cid and csd (with the CRC7 in the last byte), so a decode that is wrong shows up as a line that does not match.
  std::uint8_t cid_raw[16] = {};
  std::uint8_t csd_raw[16] = {};
  bool csd_write_protected = false;  // PERM_ or TMP_WRITE_PROTECT in the CSD (writes are refused)
};

// What a run did, for `diag sd`. One writer (the main loop), read as plain words.
struct Counters {
  std::uint32_t inits = 0;           // init() calls
  std::uint32_t commands = 0;        // commands sent (CMD55 and its ACMD count as two)
  std::uint32_t reads = 0;           // read() calls
  std::uint32_t blocks = 0;          // blocks delivered
  std::uint32_t retries = 0;         // chunks read again after a data error
  std::uint32_t failures = 0;        // read() calls that ended in an error
  std::uint32_t writes = 0;          // write_blocks() calls (OS-006c)
  std::uint32_t blocks_written = 0;  // blocks the card took and programmed
  std::uint32_t write_failures = 0;  // write_blocks() calls that ended in an error
  std::uint32_t no_response = 0;     // commands that got no answer
  std::uint32_t command_errors = 0;  // CRC, end-bit or index errors of a response
  std::uint32_t card_errors = 0;     // R1 status with an error bit
  std::uint32_t data_timeouts = 0;
  std::uint32_t data_errors = 0;
  Result last_error = Result::Ok;
  InitStage last_error_stage = InitStage::None;  // the stage of the last init() failure
};

class Card final : public BlockDevice {
 public:
  explicit Card(SdBus& bus) noexcept : bus_(bus) {}

  // The whole sequence. Ok: ready() is true. A second call starts over (the card is reset by the power-up and CMD0). Blocks for at most
  // policy::worst_init_us() when the card does not answer, a few tens of milliseconds when it does.
  Result init() noexcept;
  // Forgets the card (it was taken out): ready() is false until init() succeeds again.
  void drop() noexcept { info_ = CardInfo{}; }

  // BlockDevice
  bool present() const noexcept override { return bus_.card_present(); }
  BlockResult start() noexcept override;
  bool ready() const noexcept override { return info_.ready; }
  void release() noexcept override { drop(); }
  std::uint32_t block_count() const noexcept override { return info_.blocks; }
  BlockResult read(std::uint32_t lba, std::uint32_t count, void* dst) noexcept override;
  // The same read with the reason.
  Result read_blocks(std::uint32_t lba, std::uint32_t count, void* dst) noexcept;
  BlockResult write(std::uint32_t lba, std::uint32_t count, const void* src) noexcept override;
  bool write_protected() const noexcept override { return info_.csd_write_protected || bus_.write_protect_switch(); }
  // Writes `count` blocks one CMD24 at a time, each followed by CMD13 until the card is back in the transfer state. WriteProtected (nothing sent) when the
  // switch or the CSD says so. No retry: a failed block ends the call (the firmware writer aborts and deletes its file).
  Result write_blocks(std::uint32_t lba, std::uint32_t count, const void* src) noexcept;

  const CardInfo& info() const noexcept { return info_; }
  const Counters& counters() const noexcept { return counters_; }
  InitStage stage() const noexcept { return stage_; }

 private:
  Result send(Cmd cmd, std::uint32_t arg, Response& out) noexcept;
  Result send_app(Cmd cmd, std::uint32_t arg, Response& out) noexcept;  // CMD55 (this card's RCA), then the application command
  Result read_chunk(std::uint32_t lba, std::uint32_t count, std::uint8_t* dst) noexcept;
  Result write_one(std::uint32_t lba, const std::uint8_t* src) noexcept;
  Result fail(Result r) noexcept;
  void try_high_speed() noexcept;
  void note(Result r) noexcept;

  SdBus& bus_;
  CardInfo info_;
  Counters counters_;
  InitStage stage_ = InitStage::None;
};

}  // namespace o3sdk::platform::mk66::storage::sd
