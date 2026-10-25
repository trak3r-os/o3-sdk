// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The FT812 as the display driver sees it: register access over a Link, the bring-up sequence that survives a warm
// chip, the coprocessor command FIFO and its fault recovery. All of it is polled and runs in the main loop; the DMA bulk
// path (display.h) sits beside it and uses Chip only for the small control writes. No heap, every wait is bounded.
//
// The bring-up follows the order of Rudolph Riedel's EmbeddedVideoEngine, `EVE_init()` in EVE_commands.c (MIT, Copyright (c)
// 2016-2024 Rudolph Riedel, pinned in track3r-os third_party/eve-riedel: tag 5.0.9): power-down pulse, reset, ACTIVE, a bounded poll on
// REG_ID (400 ms there) and on REG_CPURESET, display parameters, audio off, a first display list and its swap, DISP, the
// pixel clock last. It adds what ADR 0011 lesson 3 asks for on top: a probe of the chip *before* anything is done to it
// (a warm start shows as REG_ID already reading 0x7C), PCLK stopped before the timing registers are rewritten, the
// coprocessor's FIFO pointers verified (and recovered) before the first frame, a proof that the scan-out actually runs, and
// the backlight left off (the caller raises it after the first real frame). The fault recovery of cmd_recover() is the
// Guide's three steps (section 5.6, p.157) plus Riedel's extras (REG_CMD_DL cleared, a 10 ms settle) from
// CoprocessorFaultRecover(). The differences from EVE_init() are listed in track3r-os tests/eve_riedel_test.cpp, which runs
// the pinned original against the same fake chip.

#include <o3sdk/platform/mk66/display/ft812_regs.h>
#include <o3sdk/platform/mk66/display/link.h>
#include <o3sdk/platform/mk66/display/panel.h>

#include <cstddef>
#include <cstdint>

namespace o3sdk::platform::mk66::display {

// Host command that selects the clock source before ACTIVE. None is needed on a cold chip (the chip's reset default is its internal
// oscillator), so Default sends nothing there. On a warm one
// (REG_ID answered before the PD# pulse) Default sends CLKINT: if PD# is not wired, nothing else would undo a CLKEXT an earlier
// stage may have selected, and CLKINT on a chip already on its internal clock changes nothing.
enum class HostClock : std::uint8_t { Default, Internal, External };

struct InitConfig {
  PanelTiming panel = kPanel800x480;
  Orientation orientation = Orientation::Landscape;
  HostClock clock = HostClock::Default;
  bool soft_reset = true;          // host command RST_PULSE after PD#: belt and braces if PD# is not wired
  std::uint16_t backlight_hz = 0;  // REG_PWM_HZ; 0 = do not write (the chip's 250 Hz)
};

// The waits of the bring-up, in milliseconds. PD#: the chip needs at least 5 ms low and 20 ms before the first access
// (Guide p.11, Riedel 6 and 21); this driver uses 20 and 40. After ACTIVE, Riedel's later releases wait 60 ms
// (raised from 40 after a user report); the Guide allows the self-test up to 300 ms.
// The FT812 needs 20 ms from PD# going high to its first SPI access (Guide p.11). boot::early_pins raised PD# at reset, long
// before the first access of the bring-up; init() still waits until the uptime says at least this long has passed (the uptime
// starts after early_pins, so it is a lower bound of the time since PD# went high).
inline constexpr std::uint32_t kBootSettleMs = 20;
inline constexpr std::uint32_t kPdLowMs = 20;
inline constexpr std::uint32_t kPdHighWaitMs = 40;
inline constexpr std::uint32_t kActiveWaitMs = 60;
inline constexpr std::uint16_t kIdPollLimit = 400;       // x 1 ms: Riedel's bound
inline constexpr std::uint16_t kResetPollLimit = 300;    // x 1 ms: the Guide's 300 ms (Riedel 50)
inline constexpr std::uint16_t kFramePollLimit = 200;    // x 1 ms: two frame starts are 37 ms at the panel's 54 Hz
inline constexpr std::uint32_t kRecoverSettleMs = 10;    // Riedel: "just to be safe"

enum class InitStatus : std::uint8_t {
  Ok,
  BadConfig,         // the panel timing is not a valid one (panel_valid()): nothing was done to the chip
  NoChip,            // REG_ID never read 0x7C: no chip, no wiring, or no power
  ResetTimeout,      // the coprocessor, touch or audio engine did not leave reset
  CoprocessorStuck,  // REG_CMD_READ / WRITE would not clear after the recovery
  NoFrames,          // the pixel clock is on but REG_FRAMES does not advance or the first swap never completes
};

struct InitReport {
  InitStatus status = InitStatus::NoChip;
  bool warm_start = false;           // REG_ID read 0x7C before the chip was touched: the chip was already running
  std::uint8_t entry_id = 0;
  std::uint32_t entry_clock = 0;     // REG_CLOCK and REG_FRAMES at entry, only read when warm (cycles / frames since its reset)
  std::uint32_t entry_frames = 0;
  std::uint16_t id_polls = 0;
  std::uint16_t reset_polls = 0;
  bool recovered_coprocessor = false;
  bool bus_error = false;            // a SPI byte timed out during the bring-up
};

class Chip {
 public:
  explicit Chip(Link& link) noexcept : link_(link) {}

  Link& link() noexcept { return link_; }

  // ---- Access: one chip-select cycle each --------------------------------------------------------------------------
  void host_command(std::uint8_t command, std::uint8_t parameter = 0) noexcept;
  std::uint8_t rd8(std::uint32_t addr) noexcept;
  std::uint16_t rd16(std::uint32_t addr) noexcept;
  std::uint32_t rd32(std::uint32_t addr) noexcept;
  void wr8(std::uint32_t addr, std::uint8_t value) noexcept;
  void wr16(std::uint32_t addr, std::uint16_t value) noexcept;
  void wr32(std::uint32_t addr, std::uint32_t value) noexcept;
  // `count` bytes in one cycle (the polled twin of the DMA bulk path: the fallback and the self-test's read-back partner).
  void write_block(std::uint32_t addr, const std::uint8_t* bytes, std::size_t count) noexcept;
  void read_block(std::uint32_t addr, std::uint8_t* out, std::size_t count) noexcept;

  // ---- Bring-up -------------------------------------------------------------------------------------------------------
  // The whole sequence, ending with the pixel clock running, the first (black) frame scanned out and the SPI at the data
  // speed. The backlight is off. On any failure the chip is left as it is and the status says where it stopped.
  InitReport init(const InitConfig& config) noexcept;

  // ---- Frame path -----------------------------------------------------------------------------------------------------
  // REG_DLSWAP is accessed as a word, as the Guide's examples and Riedel's library do.
  bool dl_swap_pending() noexcept { return rd32(ft812::reg::kDlSwap) != 0; }
  void swap_frame() noexcept { wr32(ft812::reg::kDlSwap, ft812::kDlSwapFrame); }
  void set_backlight(std::uint8_t duty) noexcept;  // 0..128, clamped

  // ---- Coprocessor command FIFO (not used by the frame path: uploads of compressed assets, snapshots) -----------------
  // Writes `count` command words through the REG_CMDB_WRITE window, flow-controlled by REG_CMDB_SPACE. false: a fault was
  // found (and recovered), or the FIFO did not drain within `timeout_ms`; the words after the failure were not written.
  bool cmd_write(const std::uint32_t* words, std::size_t count, std::uint32_t timeout_ms) noexcept;
  // Waits until the coprocessor has executed everything queued (REG_CMD_READ == REG_CMD_WRITE). false: fault (recovered) or timeout.
  bool cmd_wait_idle(std::uint32_t timeout_ms) noexcept;
  // Looks for a coprocessor fault (REG_CMD_READ = 0xFFF, or non-zero low bits of REG_CMDB_SPACE: it is always a multiple
  // of four otherwise) and recovers if there is one. true: there was a fault.
  bool cmd_check() noexcept;
  // The recovery: hold the coprocessor in reset, zero REG_CMD_READ / WRITE / DL, release it, settle. Counted.
  void cmd_recover() noexcept;
  std::uint32_t fault_count() const noexcept { return faults_; }

 private:
  void transact_read_header(std::uint32_t addr) noexcept;
  void transact_write_header(std::uint32_t addr) noexcept;
  void write_reg(const RegWrite& w) noexcept;
  bool wait_scanout() noexcept;

  Link& link_;
  std::uint32_t faults_ = 0;
};

}  // namespace o3sdk::platform::mk66::display
