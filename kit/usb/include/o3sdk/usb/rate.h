// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3sdk/core/audio_clock.h>
#include <o3sdk/usb/usb_types.h>

#include <cstdint>

namespace o3sdk::usb {

// The engine's sample clock and the CPU clock are one clock: MCLK is derived from the system PLL (ADR 0012
// "Engine clock"), so one sample is a fixed, exactly known number of CPU cycles and the device's sample rate
// can be read off the SOF cadence measured in CPU cycles (CYCCNT), with no extra hardware.
//
//   cycles per sample = num / den, the OS-wide constant of core/audio_clock.h: 1800000/441 by default (D-OS-8, exactly 44,100 Hz,
//   180 MHz x 196/3125), 4080/1 with -DO3SDK_RATE_44100=OFF (MCLK = 180 MHz x 16/255, 44,117.647 Hz). THE ONE PLACE TO
//   CHANGE for D-OS-8 is that header (through the CMake option); every constant in this file, the packetiser and the feedback value
//   follow from it. The names below are aliases of the core ones, so the USB code and its tests are unchanged.
using CyclesPerSample = core::CyclesPerSample;
inline constexpr std::uint32_t kCpuHz = core::kCpuHz;
inline constexpr CyclesPerSample kRefCyclesPerSample = core::kRefCyclesPerSample;

// CPU cycles in one SOF interval: 1 ms frame at full speed, 125 us microframe at high speed.
constexpr std::uint32_t nominal_sof_cycles(Speed s) noexcept { return s == Speed::Full ? kCpuHz / 1000u : kCpuHz / 8000u; }

// Frames per SOF interval at a reference rate, Q16.16 (2,891,294 = 44.1176 at FS for the 4080/1 default). The
// optional `ref` lets a test compare both clocks in one binary; the firmware uses the default.
constexpr std::uint32_t nominal_frames_q16(Speed s, CyclesPerSample ref = kRefCyclesPerSample) noexcept {
  return static_cast<std::uint32_t>(
      (static_cast<std::uint64_t>(nominal_sof_cycles(s)) * ref.den * 65536u + ref.num / 2u) / ref.num);
}

// The constant that turns a SOF period in Q8 CPU cycles into frames per SOF in Q16.16 with one multiply and a
// shift by 32: den * 2^40 / num, rounded. Evaluated at compile time, so there is no run-time division. It fits
// 32 bits while num > 256 * den (a clock slower than 256 cycles per sample); wider is reported as 0.
constexpr std::uint32_t frames_multiplier(CyclesPerSample ref) noexcept {
  const std::uint64_t m = ((static_cast<std::uint64_t>(ref.den) << 40) + ref.num / 2u) / ref.num;
  return m < (std::uint64_t{1} << 32) ? static_cast<std::uint32_t>(m) : 0u;
}
inline constexpr std::uint32_t kRefMultiplier = frames_multiplier(kRefCyclesPerSample);
static_assert(kRefMultiplier != 0u, "reference clock slower than 256 CPU cycles per sample is not supported");

// Explicit feedback value for a rate in frames per SOF (Q16.16): Q10.14 in the low 24 bits at full speed
// (USB 2.0 5.12.4.2 for FS: 10.14), Q16.16 in 32 bits at high speed.
constexpr std::uint32_t feedback_value(Speed s, std::uint32_t rate_q16) noexcept {
  if (s == Speed::High) {
    return rate_q16;
  }
  const std::uint32_t v = (rate_q16 >> 2) + ((rate_q16 >> 1) & 1u);  // Q16 -> Q14, rounded to nearest
  return v > 0xFFFFFFu ? 0xFFFFFFu : v;
}
constexpr std::uint32_t feedback_bytes(Speed s) noexcept { return s == Speed::Full ? 3u : 4u; }
static_assert(feedback_value(Speed::Full, nominal_frames_q16(Speed::Full)) == 722824u || kRefCyclesPerSample.num != 4080u,
              "ADR 0012: the nominal FS feedback value at 4080 cycles per sample is 722,824 = 0x0B0788");

// Writes the feedback packet payload little-endian: 3 bytes (Q10.14) at full speed, 4 (Q16.16) at high
// speed. Returns the byte count.
std::uint32_t pack_feedback(Speed s, std::uint32_t value, std::uint8_t* out) noexcept;

// Tracks the true SOF period in CPU cycles from one CYCCNT stamp per SOF and turns it into the device's
// sample rate in frames per SOF.
//
// Why a loop and not a 32 ms average. Each stamp is taken in the SOF ISR, so it carries the ISR's latency
// jitter (the property test uses +/-150 us). The difference of two stamps N SOFs apart has an error that does
// not shrink with N faster than 1/N: a 32-SOF boxcar leaves about 4,000 ppm of noise at +/-150 us, two orders
// of magnitude above the 50 ppm the feedback needs. This is a second-order tracking loop (alpha-beta filter)
// on the SOF phase instead, whose period estimate has an error that falls roughly as the 1.5th power of its
// time constant: alpha = 2^-7, beta = 2^-17, a time constant of about 0.4 s. Measured on the host simulation
// (+/-150 us of jitter, +/-500 ppm, 1.5 million SOFs): under 50 ppm from about 3.5 s after the first SOF, 22.5 ppm
// worst case afterwards (0.3 ppm without jitter). Until it has settled the rate is within the host's +/-500 ppm
// of nominal, which is what hosts tolerate.
//
// Missed SOFs: the hardware frame number says how many intervals passed; up to kMaxGap are bridged, a longer
// gap re-synchronises the phase and keeps the period. A stamp more than a quarter period off the prediction
// is clamped, not trusted.
//
// Context: on_sof() runs in the SOF ISR; the getters may be called from the same ISR (the IN/OUT path) or,
// for diagnostics, from the main loop (single words, torn only across fields). No heap, no floating point,
// 64-bit integer multiply/shift only (no 64-bit division at run time).
class RateEstimator {
 public:
  // `multiplier` is frames_multiplier() of the reference clock; the default is the build's kRefCyclesPerSample.
  explicit RateEstimator(Speed s = Speed::Full, std::uint32_t multiplier = kRefMultiplier) noexcept { reset(s, multiplier); }

  void reset(Speed s, std::uint32_t multiplier = kRefMultiplier) noexcept;
  // `cyccnt`: DWT->CYCCNT sampled at SOF ISR entry. `frame`: the hardware SOF frame number (FS 11 bits,
  // HS frame * 8 + microframe, 14 bits); only differences matter, modulo the field width.
  void on_sof(std::uint32_t cyccnt, std::uint32_t frame) noexcept;

  // Estimated frames per SOF, Q16.16 (feeds the packetiser and feedback_value()).
  std::uint32_t frames_per_sof_q16() const noexcept;
  // Estimated SOF period in CPU cycles, Q8 (diagnostics).
  std::uint32_t period_cycles_q8() const noexcept { return static_cast<std::uint32_t>(period_q16_ >> 8); }
  bool locked() const noexcept { return run_ >= kLockSofs; }
  std::uint32_t sof_count() const noexcept { return count_; }  // wraps at 2^32 (49 days of SOFs); nothing depends on it
  std::uint32_t resyncs() const noexcept { return resyncs_; }
  // TEST-ONLY: sets the SOF counter, so a test can cross its 2^32 wrap without simulating 49 days.
  void force_sof_count(std::uint32_t count) noexcept { count_ = count; }

  static constexpr std::uint32_t kLockSofs = 4096;  // about 4 s: the loop has pulled in
  static constexpr std::uint32_t kMaxGap = 4;       // missed SOFs bridged without re-synchronising

 private:
  Speed speed_ = Speed::Full;
  std::uint32_t multiplier_ = kRefMultiplier;
  bool started_ = false;      // the first SOF has set the phase
  std::uint32_t frame_mask_ = 0x7FFu;
  std::uint32_t last_stamp_ = 0;
  std::uint32_t last_frame_ = 0;
  std::uint32_t count_ = 0;   // SOFs seen (modulo 2^32; diagnostics only)
  std::uint32_t run_ = 0;     // SOFs since the last (re)synchronisation
  std::uint32_t resyncs_ = 0;
  std::uint64_t t_ = 0;          // unwrapped time of the last SOF, CPU cycles
  std::uint64_t pred_q16_ = 0;   // predicted time of the next SOF, Q16 cycles, wraps mod 2^64 with t_ << 16
  std::int64_t period_q16_ = 0;  // SOF period estimate, Q16 cycles
};

}  // namespace o3sdk::usb
