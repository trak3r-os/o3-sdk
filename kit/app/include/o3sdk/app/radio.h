// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The FM radio an app can tune (docs/M2C-PLAN.md section 5): what Context::radio() hands the app, on top of the Si4703 behind
// hal::capture::CaptureHw (Platform::capture). Its audio reaches the app through the codec's line input, so the receiver is heard by selecting
// AudioIn::select(Source::Radio, gain): that powers it up and switches the analogue path. Frequencies are integers in units of 10 kHz (8750 is 87.50 MHz), the
// regions are hal/capture.h's four (Europe, USA, Australia, Japan: band, spacing, de-emphasis).
//
//   Everything here is a request: tune(), seek() and set_region() return at once, the chip's state machine runs from the main loop one bus transfer a pass and
//   status() says how far it is (Starting about 0.7 s after the power-up, Tuning, Seeking up to a few seconds, then Ready). A request made while it is starting,
//   tuning or seeking is held and runs when it is ready (the last one wins).
//
//   Fails soft. The radio may not be on a unit: when the chip does not answer, status().state is Absent (or Failed, when it answered and then stopped) and
//   nothing else works; the kit does not ask again by itself (power(true) from Absent or Failed is the one retry, and the system Config's AUDIO section and the
//   console's `radio retry` are the people who make it). Without a capture device available() is false and status() says Absent.
//
// The frequency and the region are kept on the card with the kit's other settings (settings.h). RDS is not part of this round (decision D10). Main loop only.

#include <o3sdk/hal/capture.h>

#include <cstdint>

namespace o3sdk::app {

class Runtime;

class Radio {
 public:
  using Region = hal::capture::Region;
  using Status = hal::capture::RadioStatus;

  // The platform has a capture device (it may still have no receiver: status()).
  bool available() const noexcept { return hw_ != nullptr; }

  // power(true) starts the power-up (from Off; from Absent or Failed it starts again from the reset); power(false) puts the chip back in reset. Selecting Source::Radio
  // in AudioIn powers it up too.
  void power(bool on) noexcept;
  // The region (band, spacing and de-emphasis); the frequency is made valid for it. Takes effect now, or when the chip is ready.
  void set_region(Region region) noexcept;
  Region region() const noexcept { return region_; }
  // Tunes to `freq` (10 kHz units), snapped to the nearest channel of the region; step() moves by `channels` channels (clamped to the band).
  void tune(unsigned freq) noexcept;
  void step(int channels) noexcept;
  // The next station up or down, wrapping at the band's end; Status::freq is the station when the state is Ready again, band_limit says none was found.
  void seek(bool up) noexcept;
  // The frequency the radio is on (or going to), 10 kHz units: what was asked for until the chip has answered, then what it reports.
  unsigned freq() const noexcept { return freq_; }

  Status status() const noexcept;
  // The chip answered and is not Absent or Failed: tune(), seek() and the signal figures mean something.
  bool usable() const noexcept {
    const Status::State s = status().state;
    return s != Status::State::Absent && s != Status::State::Failed;
  }

 private:
  friend class Runtime;
  void bind(hal::capture::CaptureHw* hw) noexcept { hw_ = hw; }
  // Every pass of the main loop: follows what the chip reports once it is ready.
  void poll() noexcept;
  // The settings' way in: the saved region and frequency (clamped to the region's band), applied before the source is.
  void restore(Region region, unsigned freq) noexcept;

  hal::capture::CaptureHw* hw_ = nullptr;
  Region region_ = Region::Europe;
  std::uint16_t freq_ = 8750;
  std::uint32_t seen_generation_ = 0;
};

}  // namespace o3sdk::app
