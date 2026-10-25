// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/platform/mk66/input/tca8418.h>

namespace o3sdk::platform::mk66::input {

I2cResult Tca8418::write_reg(std::uint8_t reg, std::uint8_t value) noexcept {
  const std::uint8_t tx[2] = {reg, value};
  return bus_.transfer(address_, tx, 2, nullptr, 0);
}

I2cResult Tca8418::read_reg(std::uint8_t reg, std::uint8_t& value) noexcept {
  std::uint8_t rx = 0;
  const I2cResult r = bus_.transfer(address_, &reg, 1, &rx, 1);
  if (r == I2cResult::Ok) {
    value = rx;
  }
  return r;
}

I2cResult Tca8418::configure() noexcept {
  struct Write {
    std::uint8_t reg;
    std::uint8_t value;
  };
  // The order: the keypad pins, then CFG (which turns the interrupts on). Clearing the flags is left to the first drain.
  const Write sequence[] = {
      {tca8418::kRegKpGpio1, tca8418::kAllRows},
      {tca8418::kRegKpGpio2, tca8418::kColumnsLow},
      {tca8418::kRegKpGpio3, tca8418::kColumnsHigh},
      {tca8418::kRegCfg, tca8418::kCfgValue},
  };
  for (const Write& w : sequence) {
    const I2cResult r = write_reg(w.reg, w.value);
    if (r != I2cResult::Ok) {
      return r;
    }
  }
  return I2cResult::Ok;
}

I2cResult Tca8418::read_config(std::uint8_t& masked) noexcept {
  std::uint8_t cfg = 0;
  const I2cResult r = read_reg(tca8418::kRegCfg, cfg);
  if (r == I2cResult::Ok) {
    masked = static_cast<std::uint8_t>(cfg & tca8418::kCfgVerifyMask);
  }
  return r;
}

I2cResult Tca8418::drain(RawKey* out, unsigned max, Drain& result) noexcept {
  result = Drain{};
  std::uint8_t status = 0;
  I2cResult r = read_reg(tca8418::kRegIntStat, status);
  if (r != I2cResult::Ok) {
    return r;
  }
  status &= tca8418::kIntStatMask;
  result.overflow = (status & tca8418::kIntStatOverflow) != 0u;
  if (status != 0u) {
    r = write_reg(tca8418::kRegIntStat, status);  // write 1 to clear exactly the flags that were seen
    if (r != I2cResult::Ok) {
      return r;
    }
  }
  std::uint8_t count = 0;
  r = read_reg(tca8418::kRegKeyLckEc, count);
  if (r != I2cResult::Ok) {
    return r;
  }
  const unsigned pending = count & tca8418::kEventCountMask;
  const unsigned take = pending < max ? pending : max;
  for (unsigned i = 0; i < take; ++i) {
    std::uint8_t event = 0;
    r = read_reg(tca8418::kRegKeyEventA, event);  // reading A pops the FIFO
    if (r != I2cResult::Ok) {
      result.maybe_lost = true;  // the pop may have happened with the byte lost on the way
      return r;
    }
    const unsigned code = event & tca8418::kEventCodeMask;
    if (code >= 1u && code <= kKeyCount) {
      out[result.stored++] = RawKey{static_cast<std::uint8_t>(code - 1u), (event & tca8418::kEventPress) != 0u};
    } else {
      ++result.ignored;  // 0: an empty FIFO answers zero; 97..114: GPIO events (every pin is keypad, so none is expected)
      if (code == 0u) {
        return I2cResult::Ok;  // the FIFO was emptier than its counter said: stop here
      }
    }
  }
  result.more = pending > take;
  return I2cResult::Ok;
}

}  // namespace o3sdk::platform::mk66::input
