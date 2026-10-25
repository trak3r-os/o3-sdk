// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/platform/mk66/input/pad_leds.h>

#include <o3sdk/platform/mk66/input/pins.h>
#include <o3sdk/platform/mk66/input/scan_config.h>

namespace o3sdk::platform::mk66::input {

namespace {

namespace chip = is31fl3731;

}  // namespace

PadLeds::PadLeds(I2cBus& bus, const hal::Clock& clock) noexcept : bus_(bus), clock_(clock) {}

void PadLeds::set(unsigned index, std::uint8_t level) noexcept {
  if (index < hal::kPadLedCount && !parking_) {  // (a parked driver keeps every pad dark)
    target_[index] = level;
  }
}

void PadLeds::clear() noexcept {
  for (std::uint8_t& level : target_) {
    level = 0;
  }
}

I2cResult PadLeds::write(std::uint8_t reg, const std::uint8_t* data, std::uint8_t length) noexcept {
  std::uint8_t frame[1 + 18];  // the longest write: the on/off or blink bytes, 18, behind the register
  frame[0] = reg;
  for (std::uint8_t i = 0; i < length; ++i) {
    frame[1 + i] = data[i];
  }
  return bus_.transfer(pins::kLedDriverAddress, frame, static_cast<std::uint8_t>(1 + length), nullptr, 0);
}

// A transfer failed: back to the setup after a backoff; the bus is recovered from the second failure in a row.
void PadLeds::fail(I2cResult) noexcept {
  ++errors_;
  ++failures_;
  online_ = false;
  if (failures_ >= scan::kRecoverAfterFailures) {
    (void)bus_.recover();
  }
  backoff_us_ = backoff_us_ == 0u ? scan::kBackoffMinUs
                                  : (backoff_us_ * 2u < scan::kBackoffMaxUs ? backoff_us_ * 2u : scan::kBackoffMaxUs);
  waiting_ = true;
  resume_at_ = clock_.now() + backoff_us_;
  step_ = Step::SelectFunction;
  clear_row_ = 0;
  ++reinits_;
  for (std::uint8_t& pwm : sent_) {
    pwm = 0;  // what the chip holds is unknown until the setup has cleared it again
  }
}

bool PadLeds::all_pwm_current() const noexcept {
  for (unsigned i = 0; i < hal::kPadLedCount; ++i) {
    if (pad_pwm_of_level(target_[i]) != sent_[i]) {
      return false;
    }
  }
  return true;
}

// Writes the next row whose PWM differs from what the chip holds: from its first differing column to its last, in one transfer.
bool PadLeds::send_dirty_span() noexcept {
  for (unsigned n = 0; n < chip::kPadRows; ++n) {
    const unsigned row = (next_row_ + n) % chip::kPadRows;
    unsigned first = chip::kPadColumns;
    unsigned last = 0;
    std::uint8_t pwm[chip::kPadColumns];
    for (unsigned col = 0; col < chip::kPadColumns; ++col) {
      const unsigned index = row * chip::kPadColumns + col;
      pwm[col] = pad_pwm_of_level(target_[index]);
      if (pwm[col] != sent_[index]) {
        first = first < col ? first : col;
        last = col;
      }
    }
    if (first == chip::kPadColumns) {
      continue;  // this row is current
    }
    const unsigned length = last - first + 1u;
    const I2cResult r = write(pad_pwm_register(row * chip::kPadColumns + first), pwm + first, static_cast<std::uint8_t>(length));
    if (r != I2cResult::Ok) {
      fail(r);
      return true;
    }
    for (unsigned col = first; col <= last; ++col) {
      sent_[row * chip::kPadColumns + col] = pwm[col];
    }
    next_row_ = (row + 1u) % chip::kPadRows;
    return true;  // one transfer done
  }
  return false;  // nothing to send
}

I2cResult PadLeds::select_bank(std::uint8_t bank) noexcept { return write(chip::kRegCommand, &bank, 1); }

I2cResult PadLeds::write_function(std::uint8_t reg, std::uint8_t value) noexcept { return write(reg, &value, 1); }

bool PadLeds::flush() noexcept {
  const core::Tick now = clock_.now();
  if (waiting_) {
    if (!core::tick_reached(now, resume_at_)) {
      return true;  // the shutdown pause or the backoff: nothing to send yet
    }
    waiting_ = false;
  }

  I2cResult r = I2cResult::Ok;
  Step next = step_;
  switch (step_) {
    case Step::SelectFunction:
      r = select_bank(chip::kBankFunction);
      next = Step::EnterShutdown;
      break;
    case Step::EnterShutdown:
      r = write_function(chip::kFnShutdown, 0x00);  // dark from here, whatever the registers hold
      next = Step::Configure;
      break;
    case Step::Configure:
      r = write_function(chip::kFnConfiguration, 0x00);  // picture mode
      next = Step::PictureFrame;
      break;
    case Step::PictureFrame:
      r = write_function(chip::kFnPictureDisplay, 0x00);  // show frame 1
      next = Step::AudioSync;
      break;
    case Step::AudioSync:
      r = write_function(chip::kFnAudioSync, 0x00);
      next = Step::SelectFrame;
      break;
    case Step::SelectFrame:
      r = select_bank(chip::kBankFrame1);
      next = Step::OnOffZero;
      break;
    case Step::OnOffZero: {
      const std::uint8_t none[chip::kOnOffBytes] = {};  // every on bit off first
      r = write(chip::kFrameOnOff, none, chip::kOnOffBytes);
      next = Step::BlinkBits;
      break;
    }
    case Step::BlinkBits: {
      const std::uint8_t none[chip::kOnOffBytes] = {};
      r = write(chip::kFrameBlink, none, chip::kOnOffBytes);
      next = Step::ClearPwm;
      clear_row_ = 0;
      break;
    }
    case Step::ClearPwm: {
      const std::uint8_t zeros[chip::kMatrixColumns] = {};
      r = write(static_cast<std::uint8_t>(chip::kFramePwm + clear_row_ * chip::kMatrixColumns), zeros, chip::kMatrixColumns);
      if (r == I2cResult::Ok && ++clear_row_ < chip::kMatrixRows) {
        next = Step::ClearPwm;
      } else {
        next = Step::OnOffBits;
      }
      break;
    }
    case Step::OnOffBits: {
      // Every pad's LED is switched on for good, now that every PWM is 0; its PWM alone decides how bright (0 is dark).
      std::uint8_t bits[chip::kOnOffBytes] = {};
      for (unsigned row = 0; row < chip::kPadRows; ++row) {
        bits[2u * row] = 0xFF;       // matrix columns 0..7
        bits[2u * row + 1u] = 0x0F;  // matrix columns 8..11: the pads' last four
      }
      r = write(chip::kFrameOnOff, bits, chip::kOnOffBytes);
      next = Step::SelectFunctionAgain;
      break;
    }
    case Step::SelectFunctionAgain:
      r = select_bank(chip::kBankFunction);
      next = Step::LeaveShutdown;
      break;
    case Step::LeaveShutdown:
      r = write_function(chip::kFnShutdown, 0x01);  // last: the first thing the chip lights is a frame that is all zero
      next = Step::SelectFrameAgain;
      break;
    case Step::SelectFrameAgain:
      r = select_bank(chip::kBankFrame1);
      next = Step::Ready;
      break;
    case Step::Ready: {
      if (send_dirty_span()) {
        return waiting_ || all_pwm_current();  // a failed transfer starts the backoff: nothing more to send for now
      }
      if (parking_) {  // every PWM is 0 now: the chip goes into software shutdown (OS-POWER P1)
        r = select_bank(chip::kBankFunction);
        next = Step::ParkShutdown;
        break;
      }
      if (!core::tick_reached(now, health_at_)) {
        return true;
      }
      r = select_bank(chip::kBankFunction);
      next = Step::HealthRead;
      break;
    }
    case Step::HealthRead: {
      std::uint8_t value = 0;
      const std::uint8_t reg = chip::kFnShutdown;
      r = bus_.transfer(pins::kLedDriverAddress, &reg, 1, &value, 1);
      health_ok_ = r == I2cResult::Ok && (value & 0x01u) != 0u;
      if (r == I2cResult::Ok) {
        next = Step::HealthRestore;
      }
      break;
    }
    case Step::ParkShutdown:
      r = write_function(chip::kFnShutdown, 0x00);  // dark whatever the registers hold, until the chip's own power goes
      next = Step::Parked;
      break;
    case Step::Parked:
      return true;  // nothing more is ever sent
    case Step::HealthRestore:
      r = select_bank(chip::kBankFrame1);
      if (r == I2cResult::Ok) {
        if (health_ok_) {
          next = Step::Ready;
          health_at_ = now + kHealthUs;
        } else {
          ++reinits_;  // the chip is not in normal operation any more (it reset itself): the whole setup again
          online_ = false;
          next = Step::SelectFunction;
          for (std::uint8_t& pwm : sent_) {
            pwm = 0;
          }
        }
      }
      break;
  }

  if (r != I2cResult::Ok) {
    fail(r);
    return true;
  }
  failures_ = 0;
  backoff_us_ = 0;
  if (step_ == Step::EnterShutdown) {
    waiting_ = true;  // the 10 ms after shutdown, before anything else is written
    resume_at_ = now + kShutdownPauseUs;
  }
  if (next == Step::Ready && step_ != Step::Ready) {
    online_ = true;
    health_at_ = now + kHealthUs;
  }
  step_ = next;
  return step_ == Step::Ready && all_pwm_current();
}

}  // namespace o3sdk::platform::mk66::input
