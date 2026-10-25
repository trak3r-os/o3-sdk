// SPDX-License-Identifier: GPL-3.0-or-later
//
// FT812 access and bring-up. See chip.h for the sequence and for where it follows Riedel's EVE_init() (MIT, Copyright (c)
// 2016-2024 Rudolph Riedel) and where it differs.
#include <o3sdk/platform/mk66/display/chip.h>

namespace o3sdk::platform::mk66::display {

namespace {
using namespace ft812;

// What the first display list is: black, one list, terminated (the same three words as Riedel's EVE_init).
constexpr std::uint32_t kFirstList[3] = {kDlClearColorBlack, kDlClearAll, kDlDisplay};
}  // namespace

void Chip::transact_read_header(std::uint32_t addr) noexcept {
  std::uint8_t header[kReadHeaderBytes];
  read_header(addr, header);
  link_.select();
  for (const std::uint8_t b : header) {
    link_.xfer(b);
  }
}

void Chip::transact_write_header(std::uint32_t addr) noexcept {
  std::uint8_t header[kWriteHeaderBytes];
  write_header(addr, header);
  link_.select();
  for (const std::uint8_t b : header) {
    link_.xfer(b);
  }
}

void Chip::host_command(std::uint8_t command, std::uint8_t parameter) noexcept {
  link_.select();
  link_.xfer(command);
  link_.xfer(parameter);
  link_.xfer(0);
  link_.deselect();
}

std::uint8_t Chip::rd8(std::uint32_t addr) noexcept {
  transact_read_header(addr);
  const std::uint8_t v = link_.xfer(0);
  link_.deselect();
  return v;
}

std::uint16_t Chip::rd16(std::uint32_t addr) noexcept {
  transact_read_header(addr);
  const std::uint16_t lo = link_.xfer(0);
  const std::uint16_t hi = link_.xfer(0);
  link_.deselect();
  return static_cast<std::uint16_t>(lo | (hi << 8));
}

std::uint32_t Chip::rd32(std::uint32_t addr) noexcept {
  transact_read_header(addr);
  std::uint32_t v = 0;
  for (unsigned i = 0; i < 4; ++i) {
    v |= static_cast<std::uint32_t>(link_.xfer(0)) << (8u * i);
  }
  link_.deselect();
  return v;
}

void Chip::wr8(std::uint32_t addr, std::uint8_t value) noexcept {
  transact_write_header(addr);
  link_.xfer(value);
  link_.deselect();
}

void Chip::wr16(std::uint32_t addr, std::uint16_t value) noexcept {
  transact_write_header(addr);
  link_.xfer(static_cast<std::uint8_t>(value));
  link_.xfer(static_cast<std::uint8_t>(value >> 8));
  link_.deselect();
}

void Chip::wr32(std::uint32_t addr, std::uint32_t value) noexcept {
  transact_write_header(addr);
  for (unsigned i = 0; i < 4; ++i) {
    link_.xfer(static_cast<std::uint8_t>(value >> (8u * i)));
  }
  link_.deselect();
}

void Chip::write_block(std::uint32_t addr, const std::uint8_t* bytes, std::size_t count) noexcept {
  if (bytes == nullptr || count == 0) {
    return;
  }
  transact_write_header(addr);
  for (std::size_t i = 0; i < count; ++i) {
    link_.xfer(bytes[i]);
  }
  link_.deselect();
}

void Chip::read_block(std::uint32_t addr, std::uint8_t* out, std::size_t count) noexcept {
  if (out == nullptr || count == 0) {
    return;
  }
  transact_read_header(addr);
  for (std::size_t i = 0; i < count; ++i) {
    out[i] = link_.xfer(0);
  }
  link_.deselect();
}

void Chip::write_reg(const RegWrite& w) noexcept {
  switch (w.width) {
    case 1: wr8(w.addr, static_cast<std::uint8_t>(w.value)); break;
    case 2: wr16(w.addr, static_cast<std::uint16_t>(w.value)); break;
    case 4: wr32(w.addr, w.value); break;
    default: break;
  }
}

void Chip::set_backlight(std::uint8_t duty) noexcept {
  wr8(reg::kPwmDuty, duty > kPwmDutyMax ? kPwmDutyMax : duty);
}

// ---- Coprocessor ----------------------------------------------------------------------------------------------------

void Chip::cmd_recover() noexcept {
  // Guide section 5.6 (p.157): CPURESET = 1, REG_CMD_READ and REG_CMD_WRITE = 0, CPURESET = 0. Riedel's
  // CoprocessorFaultRecover() also clears REG_CMD_DL and waits 10 ms; the BT81x-only restores (patch pointer, PCLK) are not
  // needed on the FT81x.
  wr8(reg::kCpuReset, 1);
  wr16(reg::kCmdRead, 0);
  wr16(reg::kCmdWrite, 0);
  wr16(reg::kCmdDl, 0);
  wr8(reg::kCpuReset, 0);
  link_.delay_ms(kRecoverSettleMs);
  ++faults_;
}

bool Chip::cmd_check() noexcept {
  const bool fault = rd16(reg::kCmdbSpace) % 4u != 0u || rd16(reg::kCmdRead) == kCmdFault;
  if (fault) {
    cmd_recover();
  }
  return fault;
}

bool Chip::cmd_write(const std::uint32_t* words, std::size_t count, std::uint32_t timeout_ms) noexcept {
  constexpr std::size_t kBurstWords = 256;  // 1 KiB: well under the 4 KiB the FIFO takes at once (Riedel: 3840 B)
  const std::uint32_t start = link_.now_ms();
  while (count > 0) {
    const std::uint16_t space = rd16(reg::kCmdbSpace);
    if (space % 4u != 0u) {  // always a multiple of four unless the coprocessor faulted
      cmd_recover();
      return false;
    }
    std::size_t fit = space / 4u;
    if (fit == 0) {
      if (link_.now_ms() - start >= timeout_ms) {
        return false;
      }
      link_.delay_ms(1);
      continue;
    }
    if (fit > count) {
      fit = count;
    }
    if (fit > kBurstWords) {
      fit = kBurstWords;
    }
    transact_write_header(reg::kCmdbWrite);
    for (std::size_t i = 0; i < fit; ++i) {
      for (unsigned b = 0; b < 4; ++b) {
        link_.xfer(static_cast<std::uint8_t>(words[i] >> (8u * b)));
      }
    }
    link_.deselect();
    words += fit;
    count -= fit;
  }
  return true;
}

bool Chip::cmd_wait_idle(std::uint32_t timeout_ms) noexcept {
  const std::uint32_t start = link_.now_ms();
  for (;;) {
    const std::uint16_t read = rd16(reg::kCmdRead);
    if (read == kCmdFault) {
      cmd_recover();
      return false;
    }
    if (read == rd16(reg::kCmdWrite)) {
      return true;
    }
    if (link_.now_ms() - start >= timeout_ms) {
      return false;
    }
    link_.delay_ms(1);
  }
}

// ---- Bring-up -------------------------------------------------------------------------------------------------------

// After PCLK: the scan-out must prove itself. REG_FRAMES has to advance twice (a steady scan, not one glitch) and the first
// swap has to have completed (REG_DLSWAP reads 0), within kFramePollLimit ms.
bool Chip::wait_scanout() noexcept {
  std::uint32_t last = rd32(reg::kFrames);
  unsigned advances = 0;
  for (std::uint16_t i = 0; i < kFramePollLimit; ++i) {
    link_.delay_ms(1);
    const std::uint32_t frames = rd32(reg::kFrames);
    if (frames != last) {
      last = frames;
      ++advances;
    }
    if (advances >= 2 && rd32(reg::kDlSwap) == 0) {
      return true;
    }
  }
  return false;
}

InitReport Chip::init(const InitConfig& config) noexcept {
  InitReport report;
  if (!panel_valid(config.panel)) {  // a timing the controller would mis-scan is never sent to a panel
    report.status = InitStatus::BadConfig;
    return report;
  }
  link_.take_error();  // start clean
  // At least kBootSettleMs between PD# going high (boot::early_pins, at reset) and the first access: the uptime is a lower bound
  // of that time. Bounded by its own constant.
  const std::uint32_t uptime = link_.now_ms();
  if (uptime < kBootSettleMs) {
    link_.delay_ms(kBootSettleMs - uptime);
  }
  link_.set_speed(SpiSpeed::Init);

  // 1. Probe before touching anything: a chip that answers REG_ID with 0x7C is already running (the bootloader left it so,
  //    ADR 0011 lesson 3). Only a diagnostic, but a warm chip may be showing the bootloader's picture, so its backlight goes
  //    off at once.
  report.entry_id = rd8(reg::kId);
  report.warm_start = report.entry_id == kChipId;
  if (report.warm_start) {
    report.entry_clock = rd32(reg::kClock);
    report.entry_frames = rd32(reg::kFrames);
    wr8(reg::kPwmDuty, 0);
  }

  // 2. Power-down pulse, then the reset command (a no-op after a real power-down, the reset if PD# is not wired), then the
  //    optional clock command and ACTIVE.
  link_.power_down(true);
  link_.delay_ms(kPdLowMs);
  link_.power_down(false);
  link_.delay_ms(kPdHighWaitMs);
  if (config.soft_reset) {
    host_command(host::kRstPulse);
  }
  HostClock clock = config.clock;
  if (clock == HostClock::Default && report.warm_start) {
    clock = HostClock::Internal;  // a previous CLKEXT must not survive an unwired PD#
  }
  if (clock == HostClock::External) {
    host_command(host::kClkExt);
  } else if (clock == HostClock::Internal) {
    host_command(host::kClkInt);
  }
  host_command(host::kActive);
  link_.delay_ms(kActiveWaitMs);

  // 3. The chip must answer, and every engine must be out of reset. Both bounded.
  bool ready = false;
  for (std::uint16_t i = 0; i < kIdPollLimit; ++i) {
    link_.delay_ms(1);
    ++report.id_polls;
    if (rd8(reg::kId) == kChipId) {
      ready = true;
      break;
    }
  }
  if (!ready) {
    report.status = InitStatus::NoChip;
    report.bus_error = link_.take_error();
    return report;
  }
  // The chip answers. Its backlight register resets to full brightness and has been counting since ACTIVE (the 60 ms wait above is
  // that window, which cannot be shortened without the chip answering), so the very first register writes are "off" and, if
  // one was asked for, the PWM frequency: before the engines' reset is awaited and before anything else is configured (Riedel
  // writes the duty after all of that).
  wr8(reg::kPwmDuty, 0);
  if (config.backlight_hz != 0) {
    wr16(reg::kPwmHz, config.backlight_hz);
  }
  ready = false;
  for (std::uint16_t i = 0; i < kResetPollLimit; ++i) {
    link_.delay_ms(1);
    ++report.reset_polls;
    if ((rd8(reg::kCpuReset) & 7u) == 0) {
      ready = true;
      break;
    }
  }
  if (!ready) {
    report.status = InitStatus::ResetTimeout;
    report.bus_error = link_.take_error();
    return report;
  }

  // 4. The coprocessor's FIFO pointers must be zero before the first frame (a reset leaves them so; a chip that was never
  //    reset may not). One recovery, then they must read zero.
  if (rd16(reg::kCmdRead) != 0 || rd16(reg::kCmdWrite) != 0) {
    cmd_recover();
    report.recovered_coprocessor = true;
    if (rd16(reg::kCmdRead) != 0 || rd16(reg::kCmdWrite) != 0) {
      report.status = InitStatus::CoprocessorStuck;
      report.bus_error = link_.take_error();
      return report;
    }
  }

  // 5. The panel clock stopped, then the panel's registers (never rewrite the timing under a running clock). The backlight is
  //    already off and its frequency set (above).
  wr8(reg::kPclk, 0);
  for (unsigned i = 0; i < kPanelWriteCount; ++i) {
    write_reg(panel_write(config.panel, config.orientation, i));
  }

  // 6. The synthesiser and audio playback off (the board's audio is the codec's), then the first list and its swap.
  wr8(reg::kVolPb, 0);
  wr8(reg::kVolSound, 0);
  wr16(reg::kSound, kSoundMute);
  for (unsigned i = 0; i < 3; ++i) {
    wr32(kRamDl + 4u * i, kFirstList[i]);
  }
  swap_frame();

  // 7. DISP to the panel, then the pixel clock: the last step, nothing is scanned out before it.
  wr16(reg::kGpioX, static_cast<std::uint16_t>(rd16(reg::kGpioX) | kGpioXDisp));
  wr16(reg::kGpioXDir, static_cast<std::uint16_t>(rd16(reg::kGpioXDir) | kGpioXDisp));
  wr8(reg::kPclk, config.panel.pclk);

  // 8. Prove the scan-out runs, then and only then go to the data speed.
  if (!wait_scanout()) {
    report.status = InitStatus::NoFrames;
    report.bus_error = link_.take_error();
    return report;
  }
  link_.set_speed(SpiSpeed::Data);
  report.status = InitStatus::Ok;
  report.bus_error = link_.take_error();
  return report;
}

}  // namespace o3sdk::platform::mk66::display
