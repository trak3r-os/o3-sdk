// SPDX-License-Identifier: GPL-3.0-or-later
//
// The SDHC register policy (OS-007e): what only a hardware session can prove (README.md, checklist SD-*). Everything above this file is
// pure and tested on the host and on QEMU against a software card; this file is the controller's side of the same SdBus contract.
//
// POLLED, no interrupt, no DMA (sdhc_bus.h says why). Every wait is a Deadline on the DWT cycle counter (sd_policy.h); a wait that runs out
// resets the controller's circuits and reports a typed Result, so a card that is gone or hung costs a bounded time and never a hang.
//
// The sequence of the controller (K66 RM "SDHC", "Initialization / application information"):
//   gate on, source = core clock  ->  pins to GPIO, pulled up  ->  RSTA  ->  400 kHz (clock off, divider, SDSTB, clock on)  ->  pins to SDHC
//   ->  IRQSTATEN, PROCTL (1 line, little endian), WML  ->  settle  ->  INITA (80 clocks)
// A command: wait for CIHB (and CDIHB when it uses DAT0), clear IRQSTAT, CMDARG, XFERTYP (which starts it), poll CC or an error, read the
// four response registers, and for R1b wait for DAT0 to go high. A data read: BLKATTR, WML, the command, then per 16 words wait for BREN
// and read DATPORT, then TC. A data WRITE (OS-006c, one CMD24 block): BLKATTR, the command (DTDSEL 0), then per 16 words wait for BWEN and write DATPORT, then
// TC and DAT0 high (the card's programming busy; a CRC status other than "accepted" is the controller's data CRC error).
#include "../../src/internal.h"
#include "../../src/regs.h"

#include <o3sdk/platform/mk66/platform.h>
#include <o3sdk/platform/mk66/storage/sd_policy.h>
#include <o3sdk/platform/mk66/storage/sdhc_bus.h>
#include <o3sdk/platform/mk66/storage/sdhc_facts.h>

#include <cstring>

namespace o3sdk::platform::mk66::storage {
namespace {

namespace f = sdhc;
namespace pol = sd::policy;
using sd::Cmd;
using sd::Resp;
using sd::Response;
using sd::Result;

constexpr std::uint32_t kTicksPerUs = clock::kCoreHz / 1'000'000u;

inline volatile std::uint32_t& reg(std::uint32_t offset) noexcept { return regs::at<std::uint32_t>(f::kBase + offset); }

// Deadlines on the cycle counter (running since the reset sequence, boot::enable_dwt()): a microsecond limit, wrap-safe.
inline pol::Deadline deadline_us(std::uint32_t us) noexcept { return pol::Deadline{regs::dwt_cyccnt(), pol::to_ticks(us, kTicksPerUs)}; }
inline bool expired(const pol::Deadline& d) noexcept { return d.expired(regs::dwt_cyccnt()); }

// Resets the circuits in `mask` (RSTA, RSTC, RSTD) and waits for the bit to clear itself.
bool reset_circuits(std::uint32_t mask) noexcept {
  reg(f::kOffSysctl) = reg(f::kOffSysctl) | mask;
  const pol::Deadline d = deadline_us(pol::kControllerResetUs);
  while ((reg(f::kOffSysctl) & mask) != 0u) {
    if (expired(d)) {
      return false;
    }
  }
  return true;
}

Result classify_data_error(std::uint32_t irqstat) noexcept {
  if ((irqstat & f::kIrqDtoe) != 0u) {
    return Result::DataTimeout;
  }
  return Result::DataError;  // CRC, end bit, or the auto CMD12
}

void config_pins(bool sdhc_function) noexcept {
  const Pin lines[] = {f::kD1, f::kD0, f::kCmd, f::kD3, f::kD2};
  for (const Pin& pin : lines) {
    regs::port_pcr(pin) = sdhc_function ? f::kPcrCmdData : f::kPcrCardDetect;
  }
  regs::port_pcr(f::kClk) = sdhc_function ? f::kPcrClk : f::kPcrCardDetect;
}

}  // namespace

void SdhcBus::init_detect_pin() noexcept {
  regs::port_pcr(f::kCardDetect) = f::kPcrCardDetect;
  regs::gpio_pddr(f::kCardDetect) = regs::gpio_pddr(f::kCardDetect) & ~regs::mask_of(f::kCardDetect);  // an input
}

bool SdhcBus::card_present() const noexcept {
  const bool high = (regs::gpio_pdir(f::kCardDetect) & regs::mask_of(f::kCardDetect)) != 0u;
  return high != f::kCardDetectActiveLow;
}

void SdhcBus::delay_ms(std::uint32_t ms) noexcept { boot::delay_ms(ms); }

std::uint32_t SdhcBus::micros() noexcept { return system_clock().now(); }

void SdhcBus::set_bus_width(unsigned bits) noexcept { reg(f::kOffProctl) = f::proctl_bus_width(reg(f::kOffProctl), bits); }

std::uint32_t SdhcBus::set_clock(std::uint32_t max_hz) noexcept {
  const f::ClockPlan plan = f::plan_clock(f::kSdhcSourceHz, max_hz);
  if (!plan.ok) {
    return 0;
  }
  // The divider must not change while the clock runs: off, new fields, wait for it to be stable, on (the RM's order).
  std::uint32_t v = reg(f::kOffSysctl) & ~f::kSysSdclken;
  reg(f::kOffSysctl) = v;
  v = (v & ~(f::kSysDividerMask | f::kSysDtocvMask)) | f::sysctl_fields(plan);
  reg(f::kOffSysctl) = v;
  const pol::Deadline d = deadline_us(pol::kClockStableUs);
  while ((reg(f::kOffPrsstat) & f::kPrsSdstb) == 0u) {
    if (expired(d)) {
      return 0;
    }
  }
  reg(f::kOffSysctl) = v | f::kSysSdclken;
  return plan.hz;
}

Result SdhcBus::power_up() noexcept {
  regs::at<std::uint32_t>(f::kSimScgc3) = regs::at<std::uint32_t>(f::kSimScgc3) | f::kScgc3Sdhc;  // the module's clock gate
  regs::sim_sopt2() = regs::sim_sopt2() & ~f::kSopt2SdhcSrcMask;                                   // SDHCSRC 0: the core clock (read-modify-write: USB's bits stay)
  config_pins(false);  // GPIO, pulled up, while the controller is reset
  if (!reset_circuits(f::kSysRsta)) {
    return Result::NotReady;
  }
  if (set_clock(f::kIdentifyMaxHz) == 0u) {
    return Result::NotReady;
  }
  config_pins(true);
  reg(f::kOffIrqstaten) = f::kIrqEnabled;  // the flags are visible in IRQSTAT; ...
  reg(f::kOffIrqsigen) = 0;                // ... and none raises the interrupt line: polled
  reg(f::kOffIrqstat) = f::kIrqAll;
  reg(f::kOffProctl) = f::proctl_bus_width(reg(f::kOffProctl), 1);
  reg(f::kOffWml) = (f::kWriteWatermarkWords << 16) | f::kReadWatermarkWords;
  boot::delay_ms(pol::kPowerSettleMs);
  reg(f::kOffSysctl) = reg(f::kOffSysctl) | f::kSysInita;  // 80 clocks to the card
  const pol::Deadline d = deadline_us(pol::kInitClocksUs);
  while ((reg(f::kOffSysctl) & f::kSysInita) != 0u) {
    if (expired(d)) {
      return Result::NotReady;
    }
  }
  return Result::Ok;
}

Result SdhcBus::wait_idle(bool data_lines) noexcept {
  const std::uint32_t mask = f::kPrsCihb | (data_lines ? f::kPrsCdihb : 0u);
  pol::Deadline d = deadline_us(pol::kInhibitUs);
  while ((reg(f::kOffPrsstat) & mask) != 0u) {
    if (expired(d)) {
      // Something left the circuits busy (an earlier command that was cut short): reset them once and look again.
      (void)reset_circuits(f::kSysRstc | f::kSysRstd);
      d = deadline_us(pol::kInhibitUs);
      while ((reg(f::kOffPrsstat) & mask) != 0u) {
        if (expired(d)) {
          return Result::NotReady;
        }
      }
      return Result::Ok;
    }
  }
  return Result::Ok;
}

// Waits for the command to complete or fail, and clears what it saw. A failure resets the command circuit (the controller will not take another
// command while one is hung).
Result SdhcBus::wait_command_done() noexcept {
  const pol::Deadline d = deadline_us(pol::kCommandUs);
  for (;;) {
    const std::uint32_t s = reg(f::kOffIrqstat);
    if ((s & f::kIrqCommandErrors) != 0u) {
      reg(f::kOffIrqstat) = s;
      (void)reset_circuits(f::kSysRstc);
      return (s & f::kIrqCtoe) != 0u ? Result::NoResponse : Result::CommandError;
    }
    if ((s & f::kIrqCc) != 0u) {
      reg(f::kOffIrqstat) = f::kIrqCc;
      return Result::Ok;
    }
    if (expired(d)) {
      (void)reset_circuits(f::kSysRstc);
      return Result::NoResponse;
    }
  }
}

Result SdhcBus::send(Cmd cmd, std::uint32_t arg, Response& out, std::uint32_t busy_us) noexcept {
  const Resp type = sd::response_of(cmd);
  const bool busy_command = type == Resp::R1b;
  Result r = wait_idle(busy_command);
  if (r != Result::Ok) {
    return r;
  }
  reg(f::kOffIrqstat) = f::kIrqAll;
  reg(f::kOffCmdarg) = arg;
  reg(f::kOffXfertyp) = f::xfertyp(cmd);  // this write starts the command
  r = wait_command_done();
  if (r != Result::Ok) {
    return r;
  }
  for (unsigned i = 0; i < 4; ++i) {
    out.word[i] = type == Resp::None ? 0u : reg(f::kOffCmdrsp0 + 4u * i);
  }
  if (busy_command) {  // the card holds DAT0 low while it is busy
    const pol::Deadline d = deadline_us(busy_us);
    while ((reg(f::kOffPrsstat) & f::kPrsDat0) == 0u) {
      if (expired(d)) {
        return Result::NotReady;
      }
    }
  }
  return Result::Ok;
}

Result SdhcBus::command(Cmd cmd, std::uint32_t arg, Response& out) noexcept {
  if (!sd::known(cmd) || sd::reads_data(cmd) || sd::writes_data(cmd)) {
    return Result::Refused;  // not on the allowlist (sd_protocol.h), or a command that has a data phase: that is read_data() / write_data()
  }
  return send(cmd, arg, out, pol::kBusyUs);
}

// After a failed data phase: the data and command circuits are reset, the interrupt flags cleared, and a card that is still sending is told to stop
// (CMD12; harmless if it already stopped, the card answers "illegal command" and that answer is dropped). CMD6's status is not a stream: no stop.
void SdhcBus::recover_after_data_error(Cmd cmd) noexcept {
  (void)reset_circuits(f::kSysRstc | f::kSysRstd);
  reg(f::kOffIrqstat) = f::kIrqAll;
  if (cmd != Cmd::SwitchFunc) {
    Response ignored;
    (void)send(Cmd::StopTransmission, 0, ignored, pol::kStopBusyUs);
  }
}

Result SdhcBus::read_data(Cmd cmd, std::uint32_t arg, void* dst, std::uint32_t block_size, std::uint32_t blocks, Response& out) noexcept {
  if (!sd::known(cmd) || !sd::reads_data(cmd)) {
    return Result::Refused;
  }
  if (dst == nullptr || blocks == 0u || blocks > 0xFFFFu || block_size == 0u || block_size > sd::kBlockSize || (block_size % 64u) != 0u ||
      (cmd != Cmd::ReadMultipleBlock && blocks != 1u)) {
    return Result::BadArgument;
  }
  Result r = wait_idle(true);
  if (r != Result::Ok) {
    return r;
  }
  reg(f::kOffIrqstat) = f::kIrqAll;
  reg(f::kOffBlkattr) = f::blkattr(block_size, blocks);
  reg(f::kOffCmdarg) = arg;
  reg(f::kOffXfertyp) = f::xfertyp(cmd);  // the command and, behind it, the data phase
  r = wait_command_done();
  if (r != Result::Ok) {
    recover_after_data_error(cmd);  // a data phase that was armed and will not come: nothing to read, nothing left running
    return r;
  }
  for (unsigned i = 0; i < 4; ++i) {
    out.word[i] = reg(f::kOffCmdrsp0 + 4u * i);
  }

  const pol::Deadline d = deadline_us(pol::data_timeout_us(blocks));
  std::uint8_t* p = static_cast<std::uint8_t*>(dst);
  const std::uint32_t groups = block_size / 4u / f::kReadWatermarkWords * blocks;
  Result failure = Result::Ok;
  for (std::uint32_t g = 0; g < groups && failure == Result::Ok; ++g) {
    for (;;) {
      const std::uint32_t s = reg(f::kOffIrqstat);
      if ((s & f::kIrqDataErrors) != 0u) {
        failure = classify_data_error(s);
        break;
      }
      if ((reg(f::kOffPrsstat) & f::kPrsBren) != 0u) {
        break;
      }
      if (expired(d)) {
        failure = Result::DataTimeout;
        break;
      }
    }
    if (failure != Result::Ok) {
      break;
    }
    for (std::uint32_t w = 0; w < f::kReadWatermarkWords; ++w) {
      const std::uint32_t word = reg(f::kOffDatport);
      std::memcpy(p, &word, 4);  // little-endian mode: the first byte of the card is the lowest byte of the first word; p may be any alignment
      p += 4;
    }
  }
  while (failure == Result::Ok) {  // all data is in: the transfer (and the controller's own stop after a multiple-block read) completes
    const std::uint32_t s = reg(f::kOffIrqstat);
    if ((s & f::kIrqDataErrors) != 0u) {
      failure = classify_data_error(s);
    } else if ((s & f::kIrqTc) != 0u) {
      break;
    } else if (expired(d)) {
      failure = Result::DataTimeout;
    }
  }
  if (failure != Result::Ok) {
    recover_after_data_error(cmd);
    return failure;
  }
  reg(f::kOffIrqstat) = f::kIrqAll;
  return Result::Ok;
}

bool SdhcBus::write_protect_switch() const noexcept {
  // The Tracker's slot: no write-protect contact is known to be wired (no write-protect pin, no use of PRSSTAT[WPSPL]), so there is none to ask. The card's own CSD protection is checked by the card logic. Hardware checklist FW-*.
  return false;
}

// One block to the card. The command starts the transfer, the data phase waits for the FIFO to be filled; the words are written as the controller says
// there is room (BWEN: WRWML words), then the transfer complete flag, then the end of the card's busy. Every wait is a Deadline.
Result SdhcBus::write_data(Cmd cmd, std::uint32_t arg, const void* src, std::uint32_t block_size, std::uint32_t blocks, Response& out) noexcept {
  if (!sd::known(cmd) || !sd::writes_data(cmd)) {
    return Result::Refused;
  }
  if (src == nullptr || blocks != 1u || block_size != sd::kBlockSize) {
    return Result::BadArgument;
  }
  Result r = wait_idle(true);
  if (r != Result::Ok) {
    return r;
  }
  reg(f::kOffIrqstat) = f::kIrqAll;
  reg(f::kOffBlkattr) = f::blkattr(block_size, 1);
  reg(f::kOffCmdarg) = arg;
  reg(f::kOffXfertyp) = f::xfertyp(cmd);  // the command; the data phase follows once the FIFO holds data
  r = wait_command_done();
  if (r != Result::Ok) {
    recover_after_data_error(cmd);
    return r;
  }
  for (unsigned i = 0; i < 4; ++i) {
    out.word[i] = reg(f::kOffCmdrsp0 + 4u * i);
  }

  const pol::Deadline fill = deadline_us(pol::kWriteDataUs);
  const std::uint8_t* p = static_cast<const std::uint8_t*>(src);
  Result failure = Result::Ok;
  for (std::uint32_t g = 0; g < block_size / 4u / f::kWriteWatermarkWords && failure == Result::Ok; ++g) {
    for (;;) {
      const std::uint32_t s = reg(f::kOffIrqstat);
      if ((s & f::kIrqDataErrors) != 0u) {
        failure = classify_data_error(s);
        break;
      }
      if ((reg(f::kOffPrsstat) & f::kPrsBwen) != 0u) {
        break;
      }
      if (expired(fill)) {
        failure = Result::DataTimeout;
        break;
      }
    }
    if (failure != Result::Ok) {
      break;
    }
    for (std::uint32_t w = 0; w < f::kWriteWatermarkWords; ++w) {
      std::uint32_t word;
      std::memcpy(&word, p, 4);  // little-endian mode; p may be any alignment
      reg(f::kOffDatport) = word;
      p += 4;
    }
  }
  const pol::Deadline complete = deadline_us(pol::kWriteDataUs + pol::kWriteBusyUs);  // TC can follow the card's programming busy
  while (failure == Result::Ok) {  // all data is in the card: the transfer completes (a bad CRC status raises the data CRC error instead)
    const std::uint32_t s = reg(f::kOffIrqstat);
    if ((s & f::kIrqDataErrors) != 0u) {
      failure = classify_data_error(s);
    } else if ((s & f::kIrqTc) != 0u) {
      break;
    } else if (expired(complete)) {
      failure = Result::DataTimeout;
    }
  }
  if (failure == Result::Ok) {  // the card programs the block with DAT0 held low
    const pol::Deadline busy = deadline_us(pol::kWriteBusyUs);
    while ((reg(f::kOffPrsstat) & f::kPrsDat0) == 0u) {
      if (expired(busy)) {
        failure = Result::DataTimeout;
        break;
      }
    }
  }
  if (failure != Result::Ok) {
    recover_after_data_error(cmd);
    return failure;
  }
  reg(f::kOffIrqstat) = f::kIrqAll;
  return Result::Ok;
}

}  // namespace o3sdk::platform::mk66::storage
