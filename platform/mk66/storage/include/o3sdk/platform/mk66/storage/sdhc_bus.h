// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The MK66's SDHC controller as an SdBus (OS-007e): the register policy that the card logic (sd_card.h) runs on. Declared here for every build
// (so the header check passes everywhere); defined only in the image (src/sdhc_bus.cpp), where it writes registers. POLLED, no DMA and no
// interrupt: a command is a few register writes and a poll of IRQSTAT, a data block is read from the controller's FIFO 16 words at a time
// as the watermark flag says they are there (and, OS-006c, written to it the same way for the firmware writer's CMD24). Why not ADMA2: the transfers are 512 bytes to 16 KB in the main loop, which is waiting for them
// anyway; the controller moves a block in 23 us and the loop reads it out in under 7 us (the FIFO is 128 words), so DMA would save no
// wall-clock time, and it would cost a descriptor table, a 4-byte alignment rule on every caller's buffer (FatFs reads straight into the
// caller's), the DMA error and boundary recovery, an SYSMPU master grant and an interrupt line. The interrupt line stays off (IRQSIGEN 0), the
// eDMA table (resources.h) is untouched. A later streaming slice (samples into SDRAM while audio plays) can add ADMA2 behind the same SdBus.
//
// Every wait is bounded (sd_policy.h); no wait is in an interrupt (there is none). Main loop only.

#include <o3sdk/platform/mk66/storage/sd_bus.h>

#include <cstdint>

namespace o3sdk::platform::mk66::storage {

class SdhcBus final : public sd::SdBus {
 public:
  // Configures the card-detect pin (GPIO input with a pull-up). Nothing else is touched until power_up(): the SDHC clock gate stays closed.
  void init_detect_pin() noexcept;

  bool card_present() const noexcept override;
  sd::Result power_up() noexcept override;
  std::uint32_t set_clock(std::uint32_t max_hz) noexcept override;
  void set_bus_width(unsigned bits) noexcept override;
  sd::Result command(sd::Cmd cmd, std::uint32_t arg, sd::Response& out) noexcept override;
  sd::Result read_data(sd::Cmd cmd, std::uint32_t arg, void* dst, std::uint32_t block_size, std::uint32_t blocks, sd::Response& out) noexcept override;
  sd::Result write_data(sd::Cmd cmd, std::uint32_t arg, const void* src, std::uint32_t block_size, std::uint32_t blocks, sd::Response& out) noexcept override;
  bool write_protect_switch() const noexcept override;
  void delay_ms(std::uint32_t ms) noexcept override;
  std::uint32_t micros() noexcept override;

 private:
  sd::Result send(sd::Cmd cmd, std::uint32_t arg, sd::Response& out, std::uint32_t busy_us) noexcept;
  sd::Result wait_idle(bool data_lines) noexcept;
  sd::Result wait_command_done() noexcept;
  void recover_after_data_error(sd::Cmd cmd) noexcept;
};

}  // namespace o3sdk::platform::mk66::storage
