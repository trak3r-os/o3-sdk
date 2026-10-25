// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// hal::Leds on the IS31FL3731 pad LED driver (OS-007d), over an I2cBus. The chip is a 16 x 9 constant-current matrix driver with an
// 8-bit PWM per LED (datasheet: Lumissil IS31FL3731); the board wires one LED per pad into its first four rows, pad n at row n / 12,
// column n % 12, so its PWM register is 0x24 + 16 * (n / 12) + n % 12 (four registers are skipped a row for the 12 x 4 grid).
//
// Everything is a small state machine advanced by flush(), ONE bus transfer per call, so a call is bounded by one transfer (about
// 0.6 ms at the most, 13 bytes) and nothing ever waits: the setup is about two dozen calls, then flush sends the
// rows that changed, then every kHealthUs it checks that the chip still says "normal operation" (a chip that has reset itself
// is detected) and starts the setup again if not. A failed transfer takes the driver back
// to the setup after a backoff, with a bus recovery from the second failure in a row.
//
// The setup order keeps the pads dark. The chip's registers outlive a reset of the MCU (only its own power resets them), so at power-up
// of the MCU it may still show the last frame of the previous run. The sequence therefore asserts software shutdown FIRST (the LEDs are
// off whatever the registers hold), waits 10 ms, writes the function registers, then, still in shutdown, zeroes the on bits,
// blink and all 144 PWMs, only then sets the pads' on bits, and leaves shutdown last, into a chip
// whose every LED is at PWM 0. That the registers can be written in shutdown is the datasheet's reading (the interface stays
// active, the data is kept); the hardware session checks it (INP-7: no pad lights at power-up, and a unit with the chip never
// accepting a write in shutdown would show `PadLeds::errors()` rising and dark pads).
//
// Brightness. `level` 0..255 is perceptual; the driver applies a gamma of 2 (pwm = level^2 / 255, a nonzero level is at least 1) and
// the highest PWM it ever writes is 255 (the front level 31 = 255). No total-current cap is applied: what the app lights is the app's
// to bound (the levels apps share are kLedDim .. kLedOn in o3sdk/app/pad_leds.h).

#include <o3sdk/hal/clock.h>
#include <o3sdk/hal/leds.h>
#include <o3sdk/platform/mk66/input/i2c_bus.h>

#include <cstdint>

namespace o3sdk::platform::mk66::input {

namespace is31fl3731 {
inline constexpr std::uint8_t kRegCommand = 0xFD;       // selects the register bank (the "page")
inline constexpr std::uint8_t kBankFrame1 = 0x00;       // LED on/off bits, blink bits, PWM
inline constexpr std::uint8_t kBankFunction = 0x0B;     // the function registers
// Function bank registers.
inline constexpr std::uint8_t kFnConfiguration = 0x00;  // 0x00: picture mode
inline constexpr std::uint8_t kFnPictureDisplay = 0x01; // frame shown in picture mode
inline constexpr std::uint8_t kFnAudioSync = 0x06;      // 0: off
inline constexpr std::uint8_t kFnShutdown = 0x0A;       // 0: software shutdown, 1: normal operation
// Frame 1 registers.
inline constexpr std::uint8_t kFrameOnOff = 0x00;       // 18 bytes: one bit per LED of the 16 x 9 matrix
inline constexpr std::uint8_t kFrameBlink = 0x12;       // 18 bytes
inline constexpr std::uint8_t kFramePwm = 0x24;         // 144 bytes: 16 per matrix row
inline constexpr unsigned kMatrixColumns = 16;
inline constexpr unsigned kMatrixRows = 9;
inline constexpr unsigned kOnOffBytes = 18;
inline constexpr unsigned kPadColumns = 12;             // the pads' 12 columns sit in matrix columns 0..11
inline constexpr unsigned kPadRows = 4;                 // and rows 0..3
inline constexpr std::uint8_t kMaxPwm = 255;
}  // namespace is31fl3731

// Matrix position of pad `index`: its PWM register, and the on/off bit.
constexpr std::uint8_t pad_pwm_register(unsigned index) noexcept {
  return static_cast<std::uint8_t>(is31fl3731::kFramePwm + (index / is31fl3731::kPadColumns) * is31fl3731::kMatrixColumns +
                                   index % is31fl3731::kPadColumns);
}
// The PWM a perceptual `level` is shown with: gamma 2, 0 only for 0, 255 at most.
constexpr std::uint8_t pad_pwm_of_level(std::uint8_t level) noexcept {
  if (level == 0u) {
    return 0;
  }
  const unsigned pwm = (static_cast<unsigned>(level) * level + 127u) / 255u;
  return static_cast<std::uint8_t>(pwm == 0u ? 1u : (pwm > is31fl3731::kMaxPwm ? is31fl3731::kMaxPwm : pwm));
}

class PadLeds final : public hal::Leds {
 public:
  PadLeds(I2cBus& bus, const hal::Clock& clock) noexcept;

  unsigned count() const noexcept override { return hal::kPadLedCount; }
  void set(unsigned index, std::uint8_t level) noexcept override;
  void clear() noexcept override;
  bool flush() noexcept override;
  bool online() const noexcept override { return online_; }

  // OS-POWER P1 (E): the unit is going off. Every pad is set dark and written (the PWM of all of them 0), then the chip is put in software shutdown (kFnShutdown 0) and left there: the health read does
  // not restore it and set() is ignored from here. Advanced by flush() like the rest, one transfer a call; parked() says it has ended. A transfer that fails sends the driver through its setup
  // again (the PWM stay 0, so nothing lights) and the park follows it. Never undone: the run ends in a restart.
  void park() noexcept {
    parking_ = true;
    clear();
  }
  bool parked() const noexcept { return step_ == Step::Parked; }

  std::uint32_t errors() const noexcept { return errors_; }
  std::uint32_t reinits() const noexcept { return reinits_; }
  // OS-HIL (`diag leds`): the level staged for pad `index` (0 past the last pad), and whether the chip holds the PWM of every staged level as far as the driver knows (nothing left to send).
  std::uint8_t level(unsigned index) const noexcept { return index < hal::kPadLedCount ? target_[index] : std::uint8_t{0}; }
  bool in_sync() const noexcept { return all_pwm_current(); }

  static constexpr std::uint32_t kHealthUs = 500'000;      // how often the shutdown register is read back
  static constexpr std::uint32_t kShutdownPauseUs = 10'000;  // between software shutdown and normal operation (10 ms)

 private:
  enum class Step : std::uint8_t {
    SelectFunction,
    EnterShutdown,  // then the 10 ms pause
    Configure,
    PictureFrame,
    AudioSync,
    SelectFrame,
    OnOffZero,
    BlinkBits,
    ClearPwm,  // kMatrixRows transfers
    OnOffBits,
    SelectFunctionAgain,
    LeaveShutdown,
    SelectFrameAgain,
    Ready,
    HealthRead,
    HealthRestore,
    ParkShutdown,  // OS-POWER P1: software shutdown (the function bank was selected by the step before)
    Parked,
  };

  I2cResult write(std::uint8_t reg, const std::uint8_t* data, std::uint8_t length) noexcept;
  I2cResult select_bank(std::uint8_t bank) noexcept;
  I2cResult write_function(std::uint8_t reg, std::uint8_t value) noexcept;
  void fail(I2cResult result) noexcept;
  bool send_dirty_span() noexcept;
  bool all_pwm_current() const noexcept;

  I2cBus& bus_;
  const hal::Clock& clock_;
  Step step_ = Step::SelectFunction;
  unsigned clear_row_ = 0;
  unsigned next_row_ = 0;
  bool online_ = false;
  bool parking_ = false;       // OS-POWER P1: park() was called: the pads go dark and the chip into shutdown, for good
  bool waiting_ = false;       // a deadline is pending (the shutdown pause, or the backoff after a failure)
  core::Tick resume_at_ = 0;
  core::Tick health_at_ = 0;
  bool health_ok_ = true;
  unsigned failures_ = 0;
  std::uint32_t backoff_us_ = 0;
  std::uint32_t errors_ = 0;
  std::uint32_t reinits_ = 0;
  // Word-aligned (ADR 0011): the frame compare below may read them a word at a time.
  alignas(4) std::uint8_t target_[hal::kPadLedCount] = {};  // staged levels
  alignas(4) std::uint8_t sent_[hal::kPadLedCount] = {};    // the PWM the chip holds, as far as the driver knows
};
static_assert(alignof(PadLeds) >= 4, "PadLeds holds word-aligned frame buffers");

}  // namespace o3sdk::platform::mk66::input
