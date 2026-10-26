// SPDX-License-Identifier: GPL-3.0-or-later
//
// The MK66's I2C master: one polled driver for every I2C peripheral the board uses (OS-005a wrote it for I2C0 and the SGTL5000, OS-007d
// made it instance-generic so the input drivers share it: I2C0 carries the codec and the control-button scanner, I2C2 the pad scanner and
// the pad LEDs; there is ONE driver, not one per slice). Written from the K66 Sub-Family Reference Manual chapter "Inter-Integrated
// Circuit (I2C)": a transfer is a START (MST 0 to 1 with TX set), the address byte, data bytes each acknowledged by the slave
// (S[RXAK] = 0), and a STOP (MST 1 to 0). For a read: after the register address a repeated START (C1[RSTA]) and the address with the
// read bit, then receive mode, NACKing the last byte (C1[TXAK] set one byte early) and issuing the STOP before the last byte is read
// (the data register read starts the next transfer). The protocol is the Si4703's and the TCA8418's (two address bytes, a repeated start, a two-byte read); the driver is not the Wire
// library's.
//
// No interrupts, no DMA: this runs in the main thread, never in an ISR (so the codec's register writes and the input scan, both main
// thread, never overlap and need no lock). Every wait is bounded twice, by elapsed CPU cycles and by a poll count, so a hung bus or a
// stopped cycle counter ends in an error code instead of a spin. Two patiences: the codec's register functions wait up to 5 ms for a
// byte, as OS-005a always did; `I2cBus::transfer` (the input drivers, once a millisecond) waits 300 us (i2c_config.h).
#include "internal.h"
#include "regs.h"

#include <o3sdk/platform/mk66/board.h>
#include <o3sdk/platform/mk66/clock_config.h>
#include <o3sdk/platform/mk66/i2c_config.h>

namespace o3sdk::platform::mk66::i2c {
namespace {

using i2c_cfg::InstanceFacts;

constexpr std::uint32_t kCyclesPerUs = clock::kCoreHz / 1'000'000u;
constexpr std::uint32_t kCodecByteTimeoutCycles = i2c_cfg::kCodecByteTimeoutUs * kCyclesPerUs;
constexpr std::uint32_t kClientByteTimeoutCycles = i2c_cfg::kClientByteTimeoutUs * kCyclesPerUs;
constexpr std::uint32_t kPollLimit = 400'000u;
constexpr unsigned kRecoveryPulses = 9;
constexpr std::uint32_t kRecoveryHalfPeriodUs = 5;
static_assert(clock::kCoreHz % 1'000'000u == 0u, "whole cycles per microsecond");

// The byte registers of an I2C module (RM "I2C memory map"): F (divider) +1, C1 +2, S +3, D +4.
volatile std::uint8_t& reg_f(const InstanceFacts& f) noexcept { return regs::at<std::uint8_t>(f.base + 0x1u); }
volatile std::uint8_t& reg_c1(const InstanceFacts& f) noexcept { return regs::at<std::uint8_t>(f.base + 0x2u); }
volatile std::uint8_t& reg_s(const InstanceFacts& f) noexcept { return regs::at<std::uint8_t>(f.base + 0x3u); }
volatile std::uint8_t& reg_d(const InstanceFacts& f) noexcept { return regs::at<std::uint8_t>(f.base + 0x4u); }

template <typename Predicate>
bool wait_until(Predicate done, std::uint32_t timeout_cycles) noexcept {
  const std::uint32_t start = regs::dwt_cyccnt();
  for (std::uint32_t polls = 0; polls < kPollLimit; ++polls) {
    if (done()) {
      return true;
    }
    if (static_cast<std::uint32_t>(regs::dwt_cyccnt() - start) >= timeout_cycles) {
      // An interrupt (the render, the display DMA) that preempts this loop between the test above and the cycle count can use up the whole patience of a byte that has
      // long since finished (a spurious Timeout, and a read abandoned in the middle leaves the slave holding SDA): look once more before giving up (OS-RAD1).
      return done();
    }
  }
  return done();
}

// STOP: leave master mode, then wait for the bus to go idle.
void stop(const InstanceFacts& f, std::uint32_t timeout_cycles) noexcept {
  reg_c1(f) = regs::kI2cC1Iicen;
  wait_until([&f] { return (reg_s(f) & regs::kI2cSBusy) == 0u; }, timeout_cycles);
}

Result start(const InstanceFacts& f) noexcept {
  if ((reg_s(f) & regs::kI2cSBusy) != 0u) {
    return Result::BusStuck;
  }
  reg_c1(f) = static_cast<std::uint8_t>(regs::kI2cC1Iicen | regs::kI2cC1Mst | regs::kI2cC1Tx);
  return Result::Ok;
}

// Waits for the byte that was just sent or received to complete (S[IICIF]), clears the flag, and reports the slave's answer.
Result byte_done(const InstanceFacts& f, bool check_ack, std::uint32_t timeout_cycles) noexcept {
  if (!wait_until([&f] { return (reg_s(f) & regs::kI2cSIicif) != 0u; }, timeout_cycles)) {
    return Result::Timeout;
  }
  const std::uint8_t status = reg_s(f);
  reg_s(f) = regs::kI2cSIicif;  // write 1 to clear
  if ((status & regs::kI2cSArbl) != 0u) {
    reg_s(f) = regs::kI2cSArbl;
    return Result::ArbitrationLost;
  }
  if (check_ack && (status & regs::kI2cSRxak) != 0u) {
    return Result::Nak;
  }
  return Result::Ok;
}

Result send(const InstanceFacts& f, std::uint8_t byte, std::uint32_t timeout_cycles) noexcept {
  reg_d(f) = byte;
  return byte_done(f, true, timeout_cycles);
}

// The one transfer of every client: `tx_len` bytes after the address, then, if `rx_len` is not zero, a repeated START and `rx_len` bytes. With `tx_len` 0 and `rx_len` not 0
// it is a plain read (OS-011c: the Si4703 has no register address on the wire): START, the address with the read bit, the bytes. A failure leaves the bus released (STOP)
// except when the bus was already busy at the start (nothing was started, nothing to stop: BusStuck).
Result run(const InstanceFacts& f, std::uint8_t address7, const std::uint8_t* tx, std::uint8_t tx_len, std::uint8_t* rx,
           std::uint8_t rx_len, std::uint32_t timeout_cycles) noexcept {
  if (tx_len == 0u && rx_len == 0u) {
    return Result::Nak;  // nothing to send and nothing to read: not a transfer this master makes
  }
  Result r = start(f);
  if (r != Result::Ok) {
    return r;
  }
  if (tx_len != 0u) {
    r = send(f, static_cast<std::uint8_t>(address7 << 1), timeout_cycles);
    for (std::uint8_t i = 0; i < tx_len && r == Result::Ok; ++i) {
      r = send(f, tx[i], timeout_cycles);
    }
    if (r != Result::Ok || rx_len == 0u) {
      stop(f, timeout_cycles);
      return r;
    }
    // Repeated START, then the address with the read bit.
    reg_c1(f) = static_cast<std::uint8_t>(regs::kI2cC1Iicen | regs::kI2cC1Mst | regs::kI2cC1Tx | regs::kI2cC1Rsta);
  }
  r = send(f, static_cast<std::uint8_t>((address7 << 1) | 1u), timeout_cycles);
  if (r != Result::Ok) {
    stop(f, timeout_cycles);
    return r;
  }
  // Receive mode, ACK every byte but the last (a single byte is NACKed from the start). Reading the data register starts the first
  // byte (the value read is stale).
  reg_c1(f) = static_cast<std::uint8_t>(regs::kI2cC1Iicen | regs::kI2cC1Mst | (rx_len == 1u ? regs::kI2cC1Txak : 0u));
  const std::uint8_t stale = reg_d(f);
  static_cast<void>(stale);
  for (std::uint8_t k = 0; k < rx_len; ++k) {
    r = byte_done(f, false, timeout_cycles);
    if (r != Result::Ok) {
      stop(f, timeout_cycles);
      return r;
    }
    if (k + 1u == rx_len) {
      // STOP before the last read: reading the data register of a master receiver would start another byte.
      reg_c1(f) = regs::kI2cC1Iicen;
      rx[k] = reg_d(f);
      wait_until([&f] { return (reg_s(f) & regs::kI2cSBusy) == 0u; }, timeout_cycles);
    } else {
      if (k + 2u == rx_len) {
        // The next byte is the last: NACK it. Reading the data register returns this byte and starts the next.
        reg_c1(f) = static_cast<std::uint8_t>(regs::kI2cC1Iicen | regs::kI2cC1Mst | regs::kI2cC1Txak);
      }
      rx[k] = reg_d(f);
    }
  }
  return Result::Ok;
}

// Drives a pin as GPIO open drain by direction: low = output driving low, high = input (the pull-up does the rest). Nothing here
// ever drives a high level onto an I2C line.
void release(Pin pin) noexcept { regs::gpio_pddr(pin) = regs::gpio_pddr(pin) & ~regs::mask_of(pin); }
void pull_low(Pin pin) noexcept {
  regs::gpio_pcor(pin) = regs::mask_of(pin);
  regs::gpio_pddr(pin) = regs::gpio_pddr(pin) | regs::mask_of(pin);
}

// A slave left holding SDA low (a reset in the middle of a transfer) is released by clocking SCL until it lets go, then a STOP.
bool bus_clear(const InstanceFacts& f) noexcept {
  regs::port_pcr(f.scl) = regs::kPcrMuxGpio | regs::kPcrOdeDseSre;
  regs::port_pcr(f.sda) = regs::kPcrMuxGpio | regs::kPcrOdeDseSre;
  release(f.scl);
  release(f.sda);
  boot::delay_us(kRecoveryHalfPeriodUs);
  for (unsigned i = 0; i < kRecoveryPulses && (regs::gpio_pdir(f.sda) & regs::mask_of(f.sda)) == 0u; ++i) {
    pull_low(f.scl);
    boot::delay_us(kRecoveryHalfPeriodUs);
    release(f.scl);
    boot::delay_us(kRecoveryHalfPeriodUs);
  }
  // STOP: SDA low then high while SCL is high.
  pull_low(f.sda);
  boot::delay_us(kRecoveryHalfPeriodUs);
  release(f.sda);
  boot::delay_us(kRecoveryHalfPeriodUs);
  return (regs::gpio_pdir(f.sda) & regs::mask_of(f.sda)) != 0u;
}

// Clock gate, bus clear, pins to the peripheral, divider, glitch filter, enable. Ok when the bus was released (or was never held).
Result init_instance(const InstanceFacts& f) noexcept {
  regs::at<std::uint32_t>(f.scgc_register) = regs::at<std::uint32_t>(f.scgc_register) | f.scgc_bit;
  reg_c1(f) = 0;
  const bool clear = bus_clear(f);
  const std::uint32_t mux = static_cast<std::uint32_t>(f.mux) << regs::kPcrMuxShift;
  regs::port_pcr(f.scl) = mux | regs::kPcrOdeDseSre;
  regs::port_pcr(f.sda) = mux | regs::kPcrOdeDseSre;
  reg_f(f) = i2c_cfg::kFrequencyDivider;
  if (f.filter != 0u) {  // (OS-BUG-PADS: the grid bus only; the module is still disabled, so the filter takes effect at the enable below)
    regs::at<std::uint8_t>(f.base + i2c_cfg::kRegC2) = 0;
    regs::at<std::uint8_t>(f.base + i2c_cfg::kRegFlt) = f.filter;  // FLT[3:0] = the filter, the STARTF / STOPF flags (write 1 to clear) and SHEN zero
  }
  reg_c1(f) = regs::kI2cC1Iicen;
  reg_s(f) = static_cast<std::uint8_t>(regs::kI2cSIicif | regs::kI2cSArbl);  // clear stale flags
  return clear ? Result::Ok : Result::BusStuck;
}

// The master of one instance as the clients' seam.
class Master final : public I2cBus {
 public:
  explicit constexpr Master(i2c_cfg::Instance which) noexcept : which_(which) {}
  Result transfer(std::uint8_t address7, const std::uint8_t* tx, std::uint8_t tx_len, std::uint8_t* rx, std::uint8_t rx_len) noexcept override {
    return run(i2c_cfg::facts(which_), address7, tx, tx_len, rx, rx_len, kClientByteTimeoutCycles);
  }
  Result read(std::uint8_t address7, std::uint8_t* rx, std::uint8_t rx_len) noexcept override {
    return run(i2c_cfg::facts(which_), address7, nullptr, 0, rx, rx_len, kClientByteTimeoutCycles);
  }
  Result recover() noexcept override { return init_instance(i2c_cfg::facts(which_)); }

 private:
  i2c_cfg::Instance which_;
};

Master g_control_bus(i2c_cfg::Instance::I2c0);
Master g_grid_bus(i2c_cfg::Instance::I2c2);

}  // namespace

I2cBus& bus(i2c_cfg::Instance which) noexcept { return which == i2c_cfg::Instance::I2c0 ? static_cast<I2cBus&>(g_control_bus) : g_grid_bus; }

Result init(i2c_cfg::Instance which) noexcept { return init_instance(i2c_cfg::facts(which)); }

Result init() noexcept { return init(i2c_cfg::Instance::I2c0); }

Result write_reg(std::uint8_t address7, std::uint16_t reg, std::uint16_t value) noexcept {
  const std::uint8_t tx[4] = {static_cast<std::uint8_t>(reg >> 8), static_cast<std::uint8_t>(reg & 0xFFu),
                              static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value & 0xFFu)};
  return run(i2c_cfg::facts(i2c_cfg::Instance::I2c0), address7, tx, 4, nullptr, 0, kCodecByteTimeoutCycles);
}

Result read_reg(std::uint8_t address7, std::uint16_t reg, std::uint16_t& value) noexcept {
  const std::uint8_t tx[2] = {static_cast<std::uint8_t>(reg >> 8), static_cast<std::uint8_t>(reg & 0xFFu)};
  std::uint8_t rx[2] = {};
  const Result r = run(i2c_cfg::facts(i2c_cfg::Instance::I2c0), address7, tx, 2, rx, 2, kCodecByteTimeoutCycles);
  if (r == Result::Ok) {
    value = static_cast<std::uint16_t>((static_cast<std::uint16_t>(rx[0]) << 8) | rx[1]);
  }
  return r;
}

}  // namespace o3sdk::platform::mk66::i2c
