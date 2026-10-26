// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The clock-follow loop and the stamp-to-timeline map (OS-005c; ADR 0013 D5 "MIDI in", radar card midi-clock-sync, "Slave"). Pure: it is fed
// the time of each MIDI timing clock (24 per quarter note) as the ISR stamped it, and keeps an estimate of the clock's period and phase that
// the sequencer reads, and that SLEWS: it never jumps.
//
// THE LOOP. A second-order delay-locked loop after Adriaensen, "Using a DLL to filter time" (kokkinizita.linuxaudio.org/papers/usingdll.pdf, the
// maths only; the paper states no licence and no code is taken). With e the error between the clock's observed time and the time the loop
// predicted for it, and F the clock rate in clocks per second:
//
//     omega = 2 pi B / F      b = sqrt(2) omega      c = omega^2          (critically damped, B the loop bandwidth in Hz)
//     t0 = t1;   t1 = t1 + b e + T;   T = T + c e                         (t1: the predicted time of the NEXT clock; T: the period)
//
// The output is the prediction made before the clock was seen, so the time the loop gives a clock is never revised by that clock: a microstep
// the sequencer placed between t0 and t1 is final, and the correction a late or early clock brings lands in the following ticks. A constant
// tempo offset is followed with zero steady error; a tempo ramp leaves a constant lag; the residual jitter of the output is about the input's
// times sqrt(2B/F) (the card's arithmetic, measured by the tests of this slice, not assumed).
//
// ACQUISITION. The first clock sets the phase and the next `fit_clocks` (12: half a beat) are fitted by least squares, a straight line through their times, whose
// slope is the first period and whose end is the first phase: much better than the interval between two jittery clocks (the card's own plan, "tempo from the first
// two clocks", is the plain estimate: its error is the jitter over one period, 0.2 % for a DIN master and 4 % for a USB host, which the loop then needs a long
// time to pull out). Then the loop runs at the acquire bandwidth (4 Hz) for `acquire_clocks` clocks (0 by default: the fit is good enough to start tracking) and
// at the tracking bandwidth (0.5 Hz, the FOLLOW NORMAL of the card). omega is capped (at 0.5) so that a fast bandwidth at a slow tempo stays stable. A clock that is missing shows as an observation about k periods after the
// last one: the loop advances k ticks and corrects by the error against the extrapolation (a dropout of up to `max_dropout` clocks). A clock
// that comes less than half a period after the prediction is a glitch and ignored. A tempo that jumps (an error beyond `reacquire_pct` of a
// period for three clocks in a row) re-acquires, keeping the tick count.
//
// ARITHMETIC. Times are SQ32 (Q32.32 samples) relative to an origin sample that the loop owns (origin()), so they never overflow; the loop
// moves the origin on every few hours. The gains are single-precision floats (about ten operations a clock); the state is integer.
//
// STAMPS. The ISR stamps a byte with the 32-bit DWT cycle count. ClockMap turns a stamp into the sample timeline from one reference pair (the
// first sample of a block and the cycle count stamped at that block's boundary), by one division: no clock-domain estimation, the audio clock and
// the cycle counter being one PLL (radar card, finding 2).

#include <o3sdk/core/audio_clock.h>
#include <o3sdk/core/qtime.h>

#include <cstdint>

namespace o3sdk::platform::mk66::midi {

// ---- the stamp map -----------------------------------------------------------------------------------------------------
class ClockMap {
 public:
  // The reference: `sample` is the timeline's sample number at the instant the cycle counter read `cycles` (the DMA ISR's stamp of a block
  // boundary and that block's first sample). Called by the render at the start of every block.
  void set_ref(std::uint64_t sample, std::uint32_t cycles) noexcept {
    ref_sample_ = sample;
    ref_cycles_ = cycles;
  }
  std::uint64_t ref_sample() const noexcept { return ref_sample_; }

  // `cycles` as a time on the sample timeline, in Q32 samples relative to the whole sample `origin` (positive when the stamp is later than the
  // origin). The stamp may be before or after the reference by up to 2^31 cycles (11.9 s at 180 MHz): only differences are meaningful, as for the
  // counter itself.
  core::SQ32 to_rel(std::uint32_t cycles, std::uint64_t origin, core::CyclesPerSample r = core::kRefCyclesPerSample) const noexcept {
    const std::int32_t delta = static_cast<std::int32_t>(cycles - ref_cycles_);
    const std::uint64_t mag = delta < 0 ? static_cast<std::uint64_t>(-static_cast<std::int64_t>(delta)) : static_cast<std::uint64_t>(delta);
    const core::SQ32 dq = static_cast<core::SQ32>(core::q32_from_cycles(mag, r));
    const core::SQ32 base = (static_cast<core::SQ32>(ref_sample_) - static_cast<core::SQ32>(origin)) * static_cast<core::SQ32>(core::kQ32One);
    return base + (delta < 0 ? -dq : dq);
  }

 private:
  std::uint64_t ref_sample_ = 0;
  std::uint32_t ref_cycles_ = 0;
};

// ---- the loop -----------------------------------------------------------------------------------------------------------
class ClockDll {
 public:
  struct Config {
    std::uint32_t acquire_mhz = 4000;   // loop bandwidth while acquiring, millihertz (4 Hz)
    std::uint32_t track_mhz = 500;      // loop bandwidth once locked, millihertz (0.5 Hz: FOLLOW NORMAL; TIGHT 1.0, LOOSE 0.25)
    std::uint32_t fit_clocks = 12;      // clocks fitted by least squares before the loop starts (2..16)
    std::uint32_t acquire_clocks = 0;   // clocks at the acquire bandwidth after the fit
    std::uint32_t reacquire_pct = 20;   // an error beyond this percent of a period, three clocks in a row, re-acquires
    std::uint32_t max_dropout = 24;     // clocks that may be missing between two seen (a beat); more re-acquires
    std::uint32_t lock_pct = 5;         // locked() when every recent error is below this percent of a period
  };

  enum class Result : std::uint8_t {
    Started,     // the first clock after a reset
    Acquiring,   // one of the first clocks, being fitted: the period and phase are provisional
    Tracked,     // the normal case
    Dropout,     // clocks were missing: the loop advanced over them
    Ignored,     // too early to be the next clock (a glitch)
    Reacquired,  // a tempo jump or a long gap: phase and period restart from this clock, the tick count goes on
  };

  struct Stats {
    std::uint32_t clocks = 0;       // accepted
    std::uint32_t ignored = 0;
    std::uint32_t dropouts = 0;     // observations that skipped at least one clock
    std::uint32_t missing = 0;      // clocks inferred missing
    std::uint32_t reacquires = 0;
  };

  ClockDll() = default;
  explicit ClockDll(const Config& c) : cfg_(c) {}
  void configure(const Config& c) noexcept { cfg_ = c; }
  const Config& config() const noexcept { return cfg_; }

  // Forget the lock. `origin` is the whole sample that times are relative to from now on; `nominal` the period (samples per clock, Q32) to
  // assume until two clocks have been seen: the sequencer's current tempo * 8 ticks.
  void reset(std::uint64_t origin, core::Q32 nominal_period) noexcept;

  // A clock observed at `t` (Q32 samples relative to origin(), as ClockMap::to_rel gives it, already corrected for the port's latency).
  Result on_clock(core::SQ32 t) noexcept;

  // ---- what the sequencer reads ---------------------------------------------------------------------------------------
  std::uint64_t origin() const noexcept { return origin_; }
  bool running() const noexcept { return state_ == State::Running; }
  bool has_phase() const noexcept { return state_ != State::Idle; }
  // Index of the last accepted clock (counts from the first after a reset, dropouts included), and its filtered time.
  std::uint64_t tick() const noexcept { return tick_; }
  // The time the loop gives clock `tick`: the filtered time of the last accepted one, the prediction for the next, extrapolated by the period
  // beyond that (and back before it). Q32 relative to origin(). Only meaningful when has_phase().
  core::SQ32 time_of_tick(std::uint64_t tick) const noexcept;
  // Samples per clock, Q32, and the tempo it means, BPM x 100.
  core::Q32 period() const noexcept { return static_cast<core::Q32>(period_); }
  std::uint32_t bpm_x100() const noexcept;
  // The last error (observed minus predicted), samples as a float, for the status line; and the largest of the last eight clocks.
  float last_error_samples() const noexcept;
  bool locked() const noexcept;
  // No clock for more than `clocks` periods before `now` (Q32 relative to origin()): the master has gone quiet.
  bool stale(core::SQ32 now, std::uint32_t clocks) const noexcept {
    return state_ != State::Idle && now - last_obs_ > static_cast<core::SQ32>(period_) * static_cast<core::SQ32>(clocks);
  }
  const Stats& stats() const noexcept { return stats_; }

 private:
  enum class State : std::uint8_t { Idle, Started, Running };
  void rebase_if_needed() noexcept;
  void clamp_period() noexcept;
  void start_fit(core::SQ32 t) noexcept;
  Result fit_clock(core::SQ32 t) noexcept;
  float omega() const noexcept;

  Config cfg_{};
  State state_ = State::Idle;
  std::uint64_t origin_ = 0;
  core::SQ32 t0_ = 0;       // filtered time of tick_
  core::SQ32 t1_ = 0;       // predicted time of tick_ + 1
  core::SQ32 period_ = 0;   // T
  core::SQ32 last_obs_ = 0; // the last accepted observation (for the second clock and for stale())
  core::SQ32 nominal_ = 0;
  std::uint64_t tick_ = 0;
  std::uint32_t since_acquire_ = 0;
  std::uint32_t bad_run_ = 0;
  core::SQ32 fit_t0_ = 0;          // the first clock of the fit
  core::SQ32 fit_u_[16] = {};      // the times of the fitted clocks relative to fit_t0_
  std::uint32_t fit_n_ = 0;
  core::SQ32 err_ = 0;
  core::SQ32 err_hist_[8] = {};
  std::uint32_t err_pos_ = 0;
  std::uint32_t err_n_ = 0;
  Stats stats_{};
};

}  // namespace o3sdk::platform::mk66::midi
