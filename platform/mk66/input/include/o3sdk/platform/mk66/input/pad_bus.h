// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The pad bus (I2C2: PTA12 SCL / PTA13 SDA, the pad key scanner at 0x34 and the pad LED driver at 0x74) as the two drivers on it see it
// (OS-BUG-PADS). One object between them and the platform's I2C master; both drivers talk to it as to any I2cBus. Three jobs:
//
//   * The gate. The bus is not touched, and its module is not even initialised, before ready(): at least kSettleMs after the board's
//     power rail came up (arm()) and after the audio start has finished (audio_started(), success or failure). A later board revision
//     whose pad chips never answered an early first access is the reason; the open firmware that brings them up waits the same way. The
//     gate only ever delays the pads: nothing else (the boot, the control buttons on I2C0, the display) waits for it. Until it opens
//     every call is refused without a hardware access (transfer and read say Nak, recover says Nak) and nothing is recorded; once it has
//     opened it stays open. The drivers ask ready() first and leave their chip alone while it is false.
//   * The module initialisation, lazily: the first call after the gate reads the lines and runs the master's initialisation (clock gate,
//     bus clear, pins, divider, glitch filter), once. After that the master is only re-initialised by recover(), and recover() has a
//     policy: it re-initialises after a Timeout, an arbitration loss or a stuck bus as the latest result, or when SCL or SDA read low;
//     NEVER after a NAK (a chip that does not answer says nothing about the bus: clocking nine pulses and re-enabling the module on every
//     retry of a dead chip is noise on the lines and no help), where it answers Ok at once and counts no clear.
//   * Diagnostics (read only). It forwards every call unchanged and keeps, per chip address, the first and the latest result code and the
//     number of transactions; the result of every bus clear; and the levels of the two bus lines and of the scanner's INT line, read just
//     before the first transaction and again when asked (stats(); record() is the same without the read, for a watch that runs every pass).
//     Nothing here drives a pin and, after that first read, nothing changes a pin's mux either: the levels come from a LineReader, which
//     only reads. Until the master has taken the pins their mux is 0 and they read 0 whatever the line does, so then "now" is not read (kNone).
//
// Pure logic over three seams (the master, the line reader, the clock): host-tested. Main loop only.

#include <o3sdk/hal/clock.h>
#include <o3sdk/hal/input.h>
#include <o3sdk/platform/mk66/input/i2c_bus.h>

#include <cstdint>

namespace o3sdk::platform::mk66::input {

// The levels of PTA12, PTA13 and PTC11 as PadBusStats::kLine* bits (1 = high). Main loop only; reads, never drives.
class LineReader {
 public:
  // The levels now. No register is written: the GPIO input register shows a pin whenever its mux is a digital function (the I2C master's
  // ALT5 included), so this is valid from the master's initialisation on and costs three register reads.
  virtual std::uint8_t read() noexcept = 0;
  // The levels just before the master takes the pins, whose mux is still 0 (disabled, reads 0): the one read that maps SCL and SDA to GPIO
  // (an input, no pull) for the moment and puts them back. Called once, by the pad bus, right before the master's first initialisation.
  virtual std::uint8_t read_before_init() noexcept = 0;

 protected:
  LineReader() = default;
  ~LineReader() = default;
  LineReader(const LineReader&) = delete;
  LineReader& operator=(const LineReader&) = delete;
};

class PadBus final : public I2cBus {
 public:
  // How long after the rail came up the first transaction waits (and, separately, until the audio start is over).
  static constexpr std::uint32_t kSettleMs = 1000;

  PadBus(I2cBus& master, LineReader& lines, const hal::Clock& clock) noexcept : master_(master), lines_(lines), clock_(clock) {}

  // Starts the gate: the board's rail came up at `rail_up_ms` (the low 32 bits of hal::Clock::uptime_ms()). Once, at the input bring-up.
  void arm(std::uint32_t rail_up_ms) noexcept {
    rail_up_ms_ = rail_up_ms;
    armed_ = true;
  }
  // The audio start has finished, whether it worked or not (main.cpp, right after it).
  void audio_started() noexcept { audio_done_ = true; }
  // The gate: armed, the audio start over and kSettleMs since the rail came up. Latches open.
  bool ready() noexcept override;

  I2cResult transfer(std::uint8_t address7, const std::uint8_t* tx, std::uint8_t tx_len, std::uint8_t* rx, std::uint8_t rx_len) noexcept override;
  I2cResult read(std::uint8_t address7, std::uint8_t* rx, std::uint8_t rx_len) noexcept override;
  // The policy above. Ok: the bus is released (cleared just now, or nothing to clear); BusStuck: SDA is still low after the clear; Nak: refused, the gate is closed.
  I2cResult recover() noexcept override;

  // The record so far, with the levels as they are now (three register reads; kNone while the master has not taken the pins yet).
  hal::PadBusStats stats() const noexcept;
  // The record so far without reading the levels (lines_now is kNone): for the watch that runs every pass and asks for stats() when something changed.
  hal::PadBusStats record() const noexcept { return record_; }

 private:
  I2cResult start_master() noexcept;
  bool needs_clear() noexcept;
  void note(std::uint8_t address7, I2cResult result) noexcept;
  void note_clear(I2cResult result) noexcept;

  I2cBus& master_;
  LineReader& lines_;
  const hal::Clock& clock_;
  std::uint32_t rail_up_ms_ = 0;
  bool armed_ = false;
  bool audio_done_ = false;
  bool open_ = false;     // the gate has opened (latched)
  bool started_ = false;  // the master has been initialised (the first call after the gate)
  I2cResult last_ = I2cResult::Ok;  // the latest result of a transaction on the bus (any chip)
  hal::PadBusStats record_;
};

}  // namespace o3sdk::platform::mk66::input
