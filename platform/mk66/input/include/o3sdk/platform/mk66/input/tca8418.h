// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The TCA8418 keypad scanner, over an I2cBus (OS-007d). The chip scans the 8 x 10 matrix and debounces by itself and queues key
// events in a ten-deep FIFO behind an open-drain INT line; the host reads them out over I2C. Registers and bits are from the
// TCA8418 datasheet (TI SCPS206).

#include <o3sdk/platform/mk66/input/i2c_bus.h>
#include <o3sdk/platform/mk66/input/input_map.h>

#include <cstdint>

namespace o3sdk::platform::mk66::input {

namespace tca8418 {
inline constexpr std::uint8_t kRegCfg = 0x01;
inline constexpr std::uint8_t kRegIntStat = 0x02;
inline constexpr std::uint8_t kRegKeyLckEc = 0x03;
inline constexpr std::uint8_t kRegKeyEventA = 0x04;
inline constexpr std::uint8_t kRegKpGpio1 = 0x1D;  // which ROWS are keypad (1) rather than GPIO (0): R0..R7
inline constexpr std::uint8_t kRegKpGpio2 = 0x1E;  // COLUMNS C0..C7
inline constexpr std::uint8_t kRegKpGpio3 = 0x1F;  // COLUMNS C8..C9 in bits 1:0

inline constexpr std::uint8_t kCfgAi = 0x80;         // register address auto-increment (set; the driver never relies on it)
// CFG bit 5, "overflow mode" (TI SCPS215G, Table 6 and the CFG register): 1 = enabled, "overflow data shifts with last event pushing first
// event out", i.e. a full FIFO keeps the NEWEST ten events and loses the OLDEST; 0 (the default) = the overflow data, the new event, is
// not stored and lost. Overflow reporting needs bits 3 and 5 both set (the datasheet's Overflow Errata).
inline constexpr std::uint8_t kCfgOvrFlowM = 0x20;
inline constexpr std::uint8_t kCfgIntCfg = 0x10;     // INT re-asserts (after 50 us high) while interrupts are pending
inline constexpr std::uint8_t kCfgOvrFlowIen = 0x08; // overflow raises INT
inline constexpr std::uint8_t kCfgKeIen = 0x01;      // key events raise INT
// The CFG value: key events and overflow interrupt, INT re-asserting, overflow pushes the oldest event out, auto-increment.
inline constexpr std::uint8_t kCfgValue = kCfgAi | kCfgOvrFlowM | kCfgIntCfg | kCfgOvrFlowIen | kCfgKeIen;
// The bits of CFG a read-back is judged by (not AI, which a part may report as it likes): a chip that lost its power reads 0 there.
inline constexpr std::uint8_t kCfgVerifyMask = 0x3F;
inline constexpr std::uint8_t kAllRows = 0xFF;
inline constexpr std::uint8_t kColumnsLow = 0xFF;
inline constexpr std::uint8_t kColumnsHigh = 0x03;

inline constexpr std::uint8_t kIntStatMask = 0x0F;    // INT_STAT bits 3:0: overflow, key lock, GPI, key event (write 1 to clear)
inline constexpr std::uint8_t kIntStatOverflow = 0x08;
inline constexpr std::uint8_t kEventCountMask = 0x0F;  // KEY_LCK_EC bits 3:0: events in the FIFO
inline constexpr std::uint8_t kEventPress = 0x80;      // bit 7 of an event: 1 press, 0 release
inline constexpr std::uint8_t kEventCodeMask = 0x7F;   // 1..80 = key + 1; 97..114 are GPIO events; 0 = FIFO empty
inline constexpr unsigned kFifoDepth = 10;
}  // namespace tca8418

class Tca8418 {
 public:
  explicit Tca8418(I2cBus& bus, std::uint8_t address7) noexcept : bus_(bus), address_(address7) {}

  // The configuration: all eight rows and ten columns are keypad, then CFG as above. The first failed transfer ends it with its result
  // (the chip is then not configured). Clearing the interrupt flags is NOT done here: a flag the chip raised while
  // this driver was away, the overflow flag above all, must survive a reconfiguration for the next drain to read (it clears them itself).
  I2cResult configure() noexcept;

  // Reads CFG back, masked with kCfgVerifyMask: tca8418::kCfgValue & kCfgVerifyMask when the chip is as configured, 0 when it has
  // lost its power and its configuration (its reset value), anything else is a chip in an unexpected state.
  I2cResult read_config(std::uint8_t& masked) noexcept;

  struct Drain {
    unsigned stored = 0;      // events written to `out`
    bool overflow = false;    // the FIFO overflowed since the last drain: events were lost (the OLDEST ones: see kCfgOvrFlowM)
    bool maybe_lost = false;  // a transfer failed while reading an event: the chip may have popped it, so one event may be lost
    bool more = false;        // the FIFO still held events when `max` was reached
    unsigned ignored = 0;     // event codes that are no key of the matrix (GPIO events, a zero)
  };
  // Clears the interrupt flags (reading the overflow flag first), then reads the FIFO: up to `max` events into `out`, oldest first.
  // The flags are cleared BEFORE the FIFO is read so that an event arriving during the read raises INT again instead of being
  // swallowed by a late clear. The first failed transfer ends it with its result; `result` holds what was read until then.
  I2cResult drain(RawKey* out, unsigned max, Drain& result) noexcept;

 private:
  I2cResult write_reg(std::uint8_t reg, std::uint8_t value) noexcept;
  I2cResult read_reg(std::uint8_t reg, std::uint8_t& value) noexcept;

  I2cBus& bus_;
  std::uint8_t address_;
};

}  // namespace o3sdk::platform::mk66::input
