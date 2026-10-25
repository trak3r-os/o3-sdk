// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The two seams of the display driver, so everything above the SPI registers runs against a fake in the host
// tests: `Link` is the byte-level wire to the FT812 (chip select, PD#, one SPI byte, time), `BulkEngine` is the DMA path
// that moves a prepared buffer in one chip-select cycle. The MK66 implementations (src/device.cpp) are the only code
// that touches DSPI2 and the eDMA; the tests' fakes are in tests/mk66_display_fakes.h.

#include <cstddef>
#include <cstdint>

namespace o3sdk::platform::mk66::display {

// SPI speed class. Init is the 1.1 MHz class used until the FT812's clock is up (at most 11 MHz, Guide p.11), Data the
// 18 MHz class after (dspi_config.h).
enum class SpiSpeed : std::uint8_t { Init, Data };

// Main loop only: nothing here is for an ISR (the DMA completion handler talks to the engine, not to the Link).
class Link {
 public:
  virtual void select() noexcept = 0;    // chip select low: a transaction begins
  virtual void deselect() noexcept = 0;  // chip select high: it ends (the chip acts on it)
  // One full-duplex byte. Bounded: a bus that does not answer returns 0xFF and sets the sticky error flag, it never spins.
  virtual std::uint8_t xfer(std::uint8_t out) noexcept = 0;
  // PD# line: `asserted` = driven low = the chip is powered down (reset). Released = high.
  virtual void power_down(bool asserted) noexcept = 0;
  virtual void set_speed(SpiSpeed speed) noexcept = 0;
  virtual void delay_ms(std::uint32_t ms) noexcept = 0;
  virtual std::uint32_t now_ms() const noexcept = 0;  // wraps after 49 days; callers subtract
  // Sticky: a byte timed out since the last clear. Read and cleared by the driver at the end of an operation.
  virtual bool take_error() noexcept = 0;

 protected:
  Link() = default;
  ~Link() = default;
  Link(const Link&) = delete;
  Link& operator=(const Link&) = delete;
};

enum class BulkStatus : std::uint8_t {
  Idle,    // nothing started, or the last transfer was collected
  Busy,    // in flight: the source buffer must not be touched, chip select is low
  Done,    // finished cleanly (chip select already released); status() reads Idle from the next call on
  Failed,  // the engine saw an error or was aborted: the chip's memory content is undefined, retry another way
};

// Sends one buffer to the chip in a single chip-select cycle, without the CPU: the buffer is [3-byte write header][data].
// An engine takes `src` by address: it must stay valid and unchanged until status() has reported Done or Failed.
class BulkEngine {
 public:
  // Largest transfer one start() takes (header included): the staging buffer's size, never more than the DMA major loop.
  virtual std::size_t max_len() const noexcept = 0;
  // Begins the transfer. `capacity` is the number of bytes from `src` to the end of the buffer it lies in (the frame buffer and
  // the upload buffer differ in size): the engine refuses a `len` beyond it, so a transfer can never read past its own buffer.
  // false: refused (busy, null, empty, longer than `capacity` or than max_len()): nothing was sent.
  virtual bool start(const std::uint8_t* src, std::size_t len, std::size_t capacity) noexcept = 0;
  // Busy while in flight; Done / Failed once, then Idle. Cheap, called every pass of the main loop.
  virtual BulkStatus poll() noexcept = 0;
  // Stops a transfer in flight (timeout path): the engine is Idle afterwards and chip select is released.
  virtual void abort() noexcept = 0;

 protected:
  BulkEngine() = default;
  ~BulkEngine() = default;
  BulkEngine(const BulkEngine&) = delete;
  BulkEngine& operator=(const BulkEngine&) = delete;
};

}  // namespace o3sdk::platform::mk66::display
