// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Test doubles for the input drivers (OS-007d): a bus that routes transfers to modelled chips and can be made to fail, a model of the
// TCA8418 key scanner (register file, ten-deep FIFO, INT line, overflow, power loss) and of the IS31FL3731 LED driver (banks, shutdown
// register, PWM), a manual clock and a scripted ScanHardware. No files, no threads.
//
// The models are models: they follow the datasheets as the drivers rely on them (reading KEY_EVENT_A pops the FIFO, writing 1 to an
// INT_STAT bit clears it, a full FIFO with CFG OVR_FLOW_M set pushes its oldest event out, SCPS215G Table 6; the command register 0xFD
// selects the bank the next registers are in) and each knob breaks one of those.

#include <o3sdk/hal/clock.h>
#include <o3sdk/platform/mk66/input/i2c_bus.h>
#include <o3sdk/platform/mk66/input/input_map.h>
#include <o3sdk/platform/mk66/input/pad_leds.h>
#include <o3sdk/platform/mk66/input/scanner.h>
#include <o3sdk/platform/mk66/input/tca8418.h>

#include <cstdint>

namespace o3sdk::platform::mk66::input::fake {

class ManualClock final : public hal::Clock {
 public:
  core::Tick now() const noexcept override { return us_; }
  std::uint64_t uptime_ms() const noexcept override { return us_ / 1000u; }
  void advance_us(std::uint32_t us) noexcept { us_ += us; }
  void advance_ms(std::uint32_t ms) noexcept { us_ += ms * 1000u; }
  void set(core::Tick us) noexcept { us_ = us; }

 private:
  core::Tick us_ = 1'000'000;  // an hour into the run would hide wrap bugs; a second is enough for the lockout arithmetic
};

class Device {
 public:
  virtual I2cResult handle(const std::uint8_t* tx, std::uint8_t tx_len, std::uint8_t* rx, std::uint8_t rx_len) noexcept = 0;

 protected:
  ~Device() = default;
};

class Bus final : public I2cBus {
 public:
  void attach(std::uint8_t address, Device& device) noexcept {
    address_[count_] = address;
    device_[count_] = &device;
    ++count_;
  }
  I2cResult transfer(std::uint8_t address7, const std::uint8_t* tx, std::uint8_t tx_len, std::uint8_t* rx,
                     std::uint8_t rx_len) noexcept override {
    ++transfers;
    if (log_count < kLogSize) {
      log[log_count++] = Entry{address7, tx_len > 0 ? tx[0] : std::uint8_t{0}, tx_len > 1 ? tx[1] : std::uint8_t{0}, tx_len, rx_len};
    }
    if (dead) {
      return I2cResult::Timeout;
    }
    if (fail_next > 0) {
      if (fail_skip > 0) {
        --fail_skip;  // let this many transfers pass first
      } else {
        --fail_next;
        return fail_with;
      }
    }
    for (unsigned i = 0; i < count_; ++i) {
      if (address_[i] == address7) {
        return device_[i]->handle(tx, tx_len, rx, rx_len);
      }
    }
    return I2cResult::Nak;
  }
  I2cResult recover() noexcept override {
    ++recovers;
    return dead ? I2cResult::BusStuck : I2cResult::Ok;
  }

  // What went over the bus, in order (the first kLogSize transfers): the register, the first value byte, the byte counts.
  struct Entry {
    std::uint8_t address;
    std::uint8_t reg;
    std::uint8_t value;
    std::uint8_t tx_len;
    std::uint8_t rx_len;
  };
  static constexpr unsigned kLogSize = 256;
  Entry log[kLogSize] = {};
  unsigned log_count = 0;

  // knobs and counters
  bool dead = false;                        // every transfer times out and recovery fails
  unsigned fail_next = 0;                   // this many transfers fail with `fail_with` ...
  unsigned fail_skip = 0;                   // ... after this many have gone through (0: the very next ones)
  I2cResult fail_with = I2cResult::Nak;
  unsigned transfers = 0;
  unsigned recovers = 0;

 private:
  std::uint8_t address_[4] = {};
  Device* device_[4] = {};
  unsigned count_ = 0;
};

class KeyChip final : public Device {
 public:
  // The host-side view of the matrix: a key goes down or up (as a debounced chip would report it).
  void press(unsigned key) noexcept { push(static_cast<std::uint8_t>(tca8418::kEventPress | (key + 1u))); }
  void release(unsigned key) noexcept { push(static_cast<std::uint8_t>(key + 1u)); }
  void push(std::uint8_t event) noexcept {
    if (fifo_count_ < tca8418::kFifoDepth) {
      fifo_[fifo_count_++] = event;
      reg_[tca8418::kRegIntStat] |= 0x01;  // K_INT
    } else {
      reg_[tca8418::kRegIntStat] |= tca8418::kIntStatOverflow;
      ++lost_events;  // one event is lost either way:
      if ((reg_[tca8418::kRegCfg] & tca8418::kCfgOvrFlowM) != 0u) {
        // OVR_FLOW_M = 1: "overflow data shifts with last event pushing first event out" (Table 6, Table 7): the OLDEST is lost
        for (unsigned i = 1; i < tca8418::kFifoDepth; ++i) {
          fifo_[i - 1] = fifo_[i];
        }
        fifo_[tca8418::kFifoDepth - 1] = event;
        reg_[tca8418::kRegIntStat] |= 0x01;
      }
      // OVR_FLOW_M = 0: "overflow data is not stored and lost": the NEW event is lost, the FIFO is as it was
    }
  }
  // INT is low while an enabled interrupt flag is set.
  bool int_asserted() const noexcept {
    const std::uint8_t status = reg_[tca8418::kRegIntStat];
    const std::uint8_t cfg = reg_[tca8418::kRegCfg];
    return ((status & 0x01u) != 0u && (cfg & tca8418::kCfgKeIen) != 0u) ||
           ((status & tca8418::kIntStatOverflow) != 0u && (cfg & tca8418::kCfgOvrFlowIen) != 0u);
  }
  void power_cycle() noexcept {  // registers back to reset, the FIFO empty: what a supply glitch does, with no bus error
    for (std::uint8_t& r : reg_) {
      r = 0;
    }
    fifo_count_ = 0;
    ++power_cycles;
  }
  unsigned fifo_count() const noexcept { return fifo_count_; }
  std::uint8_t reg(std::uint8_t r) const noexcept { return reg_[r]; }
  bool configured() const noexcept {
    return reg_[tca8418::kRegKpGpio1] == tca8418::kAllRows && reg_[tca8418::kRegKpGpio2] == tca8418::kColumnsLow &&
           reg_[tca8418::kRegKpGpio3] == tca8418::kColumnsHigh && reg_[tca8418::kRegCfg] == tca8418::kCfgValue;
  }

  I2cResult handle(const std::uint8_t* tx, std::uint8_t tx_len, std::uint8_t* rx, std::uint8_t rx_len) noexcept override {
    ++accesses;
    if (tx_len < 1 || tx[0] >= sizeof(reg_)) {
      return I2cResult::Nak;
    }
    const std::uint8_t r = tx[0];
    if (tx_len >= 2) {
      if (r == tca8418::kRegIntStat) {
        reg_[r] = static_cast<std::uint8_t>(reg_[r] & ~tx[1]);  // write 1 to clear
      } else {
        reg_[r] = tx[1];
      }
    }
    if (rx_len >= 1) {
      if (r == tca8418::kRegKeyLckEc) {
        rx[0] = static_cast<std::uint8_t>(fifo_count_);
      } else if (r == tca8418::kRegKeyEventA) {
        if (fifo_count_ == 0) {
          rx[0] = 0;
        } else {
          rx[0] = fifo_[0];
          for (unsigned i = 1; i < fifo_count_; ++i) {
            fifo_[i - 1] = fifo_[i];
          }
          --fifo_count_;
        }
      } else {
        rx[0] = reg_[r];
      }
    }
    return I2cResult::Ok;
  }

  unsigned accesses = 0;
  unsigned lost_events = 0;
  unsigned power_cycles = 0;

 private:
  std::uint8_t reg_[0x30] = {};
  std::uint8_t fifo_[tca8418::kFifoDepth] = {};
  unsigned fifo_count_ = 0;
};

class LedChip final : public Device {
 public:
  I2cResult handle(const std::uint8_t* tx, std::uint8_t tx_len, std::uint8_t* rx, std::uint8_t rx_len) noexcept override {
    ++accesses;
    if (tx_len < 1) {
      return I2cResult::Nak;
    }
    const std::uint8_t reg = tx[0];
    if (reg == is31fl3731::kRegCommand) {
      if (tx_len >= 2) {
        bank_ = tx[1];
      }
      return I2cResult::Ok;
    }
    if (bank_ == is31fl3731::kBankFunction) {
      if (reg >= sizeof(function_)) {
        return I2cResult::Nak;
      }
      if (tx_len >= 2) {
        function_[reg] = tx[1];
        if (reg == is31fl3731::kFnShutdown && tx[1] == 0x00u) {
          ++shutdowns;
        }
      }
      if (rx_len >= 1) {
        rx[0] = function_[reg];
      }
      return I2cResult::Ok;
    }
    if (bank_ != is31fl3731::kBankFrame1) {
      return I2cResult::Nak;
    }
    // frame 1: 0x00..0xB3 in one flat file, a write may run on across registers
    for (unsigned i = 1; i < tx_len; ++i) {
      const unsigned at = reg + i - 1u;
      if (at >= sizeof(frame_)) {
        return I2cResult::Nak;
      }
      frame_[at] = tx[i];
      if (at >= is31fl3731::kFramePwm) {
        ++pwm_bytes;
      }
    }
    if (rx_len >= 1) {
      rx[0] = frame_[reg];
    }
    return I2cResult::Ok;
  }
  void reset() noexcept {  // the chip lost its state: everything back to its power-on values (software shutdown)
    for (std::uint8_t& b : frame_) {
      b = 0;
    }
    for (std::uint8_t& b : function_) {
      b = 0;
    }
    bank_ = 0;
  }
  bool normal_operation() const noexcept { return function_[is31fl3731::kFnShutdown] == 0x01; }
  // The chip as an earlier run left it: running, with a pad lit (its registers outlive a reset of the MCU).
  void stale_run(unsigned pad, std::uint8_t pwm_value) noexcept {
    function_[is31fl3731::kFnShutdown] = 0x01;
    const unsigned num = (pad / 12u) * 16u + pad % 12u;
    frame_[is31fl3731::kFrameOnOff + num / 8u] = static_cast<std::uint8_t>(frame_[is31fl3731::kFrameOnOff + num / 8u] | (1u << (num % 8u)));
    frame_[pad_pwm_register(pad)] = pwm_value;
  }
  // Is any LED visibly lit right now: the chip in normal operation with an LED whose on bit and PWM are both set.
  bool lit() const noexcept {
    if (!normal_operation()) {
      return false;
    }
    for (unsigned pad = 0; pad < hal_pad_count; ++pad) {
      if (on(pad) && pwm(pad) != 0u) {
        return true;
      }
    }
    return false;
  }
  static constexpr unsigned hal_pad_count = 48;
  std::uint8_t bank() const noexcept { return bank_; }
  std::uint8_t pwm(unsigned pad) const noexcept { return frame_[pad_pwm_register(pad)]; }
  bool on(unsigned pad) const noexcept {
    const unsigned num = (pad / 12u) * 16u + pad % 12u;
    return ((frame_[is31fl3731::kFrameOnOff + num / 8u] >> (num % 8u)) & 1u) != 0u;
  }
  std::uint8_t raw_frame(unsigned at) const noexcept { return frame_[at]; }

  unsigned accesses = 0;
  unsigned pwm_bytes = 0;
  unsigned shutdowns = 0;

 private:
  std::uint8_t frame_[0xB4] = {};
  std::uint8_t function_[0x0D] = {};
  std::uint8_t bank_ = 0;
};

class Hardware final : public ScanHardware {
 public:
  Hardware(KeyChip& pads, KeyChip& buttons) noexcept : pads_(pads), buttons_(buttons) {}
  bool pads_int_asserted() noexcept override { return !int_dead && pads_.int_asserted(); }
  bool buttons_int_asserted() noexcept override { return !int_dead && buttons_.int_asserted(); }
  std::uint16_t encoder_count() noexcept override { return count; }

  bool int_dead = false;   // an INT line that is not wired as assumed: never asserted
  std::uint16_t count = 0;

 private:
  KeyChip& pads_;
  KeyChip& buttons_;
};

}  // namespace o3sdk::platform::mk66::input::fake
