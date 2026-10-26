// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The seam between the drivers that talk to chips over I2C and the one master that owns each I2C peripheral (OS-007d, shared with
// OS-005a). The board has four chips on two buses: on I2C0 (the control bus) the SGTL5000 codec (OS-005a), the control-button
// TCA8418 (OS-007d) and the Si4703 radio (not driven); on I2C2 (the grid bus) the pad TCA8418 and the IS31FL3731 pad LED driver
// (OS-007d). One master per peripheral (src/i2c.cpp, `i2c::bus(instance)`); the codec's register functions and the input drivers
// are both its clients, so there is ONE driver for I2C0, not two. Everything above this interface is pure and tested against fakes.
//
// Rules for the owner of an instance and for its clients:
//   * main loop (thread) context only, never an ISR: a transfer waits (bounded) on the peripheral, and every client of an
//     instance (the codec's register writes and the key scanner on I2C0) is serialised by being in that one context, with no lock;
//   * polled: nothing enables the peripheral's interrupt, so no handler competes for S[IICIF].

#include <cstdint>

namespace o3sdk::platform::mk66 {

enum class I2cResult : std::uint8_t { Ok, Timeout, Nak, ArbitrationLost, BusStuck };

class I2cBus {
 public:
  // Writes `tx_len` bytes (at least one: the register address first) to the 7-bit `address7`; when `rx_len` is not zero, follows with
  // a repeated START and reads `rx_len` bytes, NACKing the last. Every wait is bounded in time. On any failure the bus is left
  // released (a STOP was sent, or `BusStuck` says it could not be) and nothing of `rx` is valid.
  virtual I2cResult transfer(std::uint8_t address7, const std::uint8_t* tx, std::uint8_t tx_len, std::uint8_t* rx,
                             std::uint8_t rx_len) noexcept = 0;
  // A read with no address phase: START, the address with the read bit, `rx_len` bytes (at least one) with the last NACKed, STOP. For a chip whose registers have no
  // address on the wire (the Si4703 radio, OS-011c: a read returns its registers from a fixed one). The same bounds and the same failure rules as transfer(). A bus that
  // cannot do it (a test double that never models such a chip) says Nak.
  virtual I2cResult read(std::uint8_t address7, std::uint8_t* rx, std::uint8_t rx_len) noexcept {
    (void)address7;
    (void)rx;
    (void)rx_len;
    return I2cResult::Nak;
  }
  // Releases a bus a slave is holding (a reset in the middle of a transfer): up to nine SCL pulses and a STOP, then the peripheral
  // is enabled again. Bounded. Ok when SDA reads high afterwards.
  virtual I2cResult recover() noexcept = 0;
  // OS-BUG-PADS: may the chips on this bus be spoken to yet? A bus with a gate (the pad bus: pad_bus.h) says false until its gate opens and then
  // true for good, and refuses every call before that without touching the hardware; a clients' loop that sees false leaves its chip alone and
  // asks again on its next pass. The codec's and the control buttons' bus has no gate.
  virtual bool ready() noexcept { return true; }

 protected:
  I2cBus() = default;
  ~I2cBus() = default;  // never deleted through the interface: no heap
  I2cBus(const I2cBus&) = delete;
  I2cBus& operator=(const I2cBus&) = delete;
};

}  // namespace o3sdk::platform::mk66
