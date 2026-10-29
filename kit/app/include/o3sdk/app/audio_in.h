// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// An app's audio in (docs/M2C-PLAN.md section 5): what Context::audio_in() hands the app, on top of the platform's hal::capture::CaptureHw
// (Platform::capture; null on a platform or a test rig without one: then every call is refused, available() is false, the system Config has no AUDIO
// section, and the runtime and the render are as they were).
//
//   Source. select(source, gain_percent) points the codec's ADC at the line jack (LineMix, LineLeft, LineRight), the microphone (MicLow, MicHigh), the FM
//   receiver (Radio, see radio.h) or the USB return (Usb: no analogue part, the codec is left as it is), with a gain of 0..100 per cent (the platform maps
//   it: the line 0..22.5 dB, the microphone 0..25 or 0..44 dB). The input reaches the render as it always did, as in_l / in_r of a unit and
//   BlockContext::input_l() of a hook. While the source settles (kSettleMs: the ADC's input switch clicks, the analogue switch and the receiver need
//   time) the engine holds those planes silent through its input gate (Engine::set_input_gate: a one-block fade, then nothing), and opens it again by
//   itself. LineMix is the mix of the two line channels (their mean in both planes), LineLeft and LineRight are that channel alone in both planes; the
//   other sources reach the planes as they are. Until an app (or the user, in the Config's AUDIO section) selects a source nothing here touches the codec,
//   the gate or the planes: the board's power-up state is what the app has, and a render with no use of audio in is the one it was.
//
//   Gain. set_gain() moves the gain of the source in use without the switch (no settling).
//
//   Monitor. monitor(level) adds the input to the master bus before the master inserts (Engine::set_input_monitor, decision D9): 0 is off (and free, and
//   what the unit starts with), 1..100 per cent (kMonitorOn, 70, is the "on" of track3r-os's `mon`), linear in amplitude.
//
//   Peaks. peaks() is the peak of each input channel since the previous call (linear, 1.0 = full scale; the gated input, after the gain). The engine
//   measures them only while somebody asks: the first call starts it and it goes on while the app keeps calling at least once a second (kPeaksHoldMs),
//   or while the system Config's AUDIO section is open. The first call returns zeros.
//
//   Failures. A codec write that is not acknowledged is kept (the choice stands, the gate stays closed) and tried again every kRetryMs from the main loop
//   until it works (failed() says so); nothing waits. The retry never re-powers a receiver that did not answer: with the radio as the source and Absent or Failed it waits
//   until the radio is started again (radio().power(true): the Config's F5, the console's `radio retry`), since the platform's select of Radio powers the chip. A select() in App::setup() is such a case on the unit: the audio path starts after setup, the codec is not
//   there yet, and the retry carries the choice through half a second later (the gate stays shut until then).
//
// The kit keeps source, gain and monitor on the card with its other settings (settings.h). Main loop only; the render sees the gate, the monitor and the
// channel map, each one atomic word.

#include <o3sdk/dsp/engine.h>
#include <o3sdk/hal/capture.h>

#include <atomic>
#include <cstdint>

namespace o3sdk::app {

class Runtime;

class AudioIn {
 public:
  using Source = hal::capture::Source;

  static constexpr std::uint32_t kSettleMs = 200;      // the gate stays shut this long after a switch (track3r-os kSourceSettleMs)
  static constexpr std::uint32_t kRetryMs = 500;       // a refused codec write is tried again this often
  static constexpr std::uint32_t kPeaksHoldMs = 1000;  // the peaks keep being measured this long after the app's last peaks()
  static constexpr unsigned kMonitorOn = 70;           // the monitor's "on" in per cent
  static constexpr std::uint32_t kMapDelayMs = 5;      // the new channel map reaches the planes this long after the gate began to shut (two blocks: the fade is done)

  // The platform has a capture device. false: select(), set_gain() and monitor() change nothing and say so, peaks() is zero.
  bool available() const noexcept { return hw_ != nullptr; }

  // ---- Source and gain ------------------------------------------------------------------------------------------------------------------
  // Switches to `source` with `gain_percent` (clamped to 100); the gate shuts for kSettleMs. true: the codec acknowledged. false: no capture device, or `source`
  // is not one (nothing changed), or a write was refused (the choice is kept and tried again, failed() is true).
  bool select(Source source, unsigned gain_percent) noexcept;
  // The gain only, for the source in use. Same answer as select().
  bool set_gain(unsigned gain_percent) noexcept;
  Source source() const noexcept { return source_; }
  unsigned gain() const noexcept { return gain_; }
  // An app or the user chose a source (select() went through, or the card's choice was applied): until then the codec is as the board powered it up.
  bool selected() const noexcept { return selected_; }
  // The input is being switched: the planes are silent until this is false.
  bool settling() const noexcept { return settling_; }
  // A codec write was refused and is being retried.
  bool failed() const noexcept { return pending_ != Pending::None; }

  // ---- Monitor -------------------------------------------------------------------------------------------------------------------------
  // 0 off, 1..100 per cent of the input (clamped). Works without a capture device too (the engine's input is then whatever the platform feeds it).
  void monitor(unsigned level_percent) noexcept;
  unsigned monitor() const noexcept { return monitor_; }

  // ---- Peaks ---------------------------------------------------------------------------------------------------------------------------
  dsp::InputPeaks peaks() noexcept;
  // What the AUDIO section meters: the largest peak of each channel with a fall of about 300 ms, valid while watch(true).
  dsp::InputPeaks meter() const noexcept { return dsp::InputPeaks{meter_l_, meter_r_}; }
  // The system Config's AUDIO section asks for the peaks while it is open.
  void watch(bool on) noexcept { watching_ = on; }

 private:
  friend class Runtime;
  enum class Pending : std::uint8_t { None, Select, Gain };
  // How the line's two channels reach the planes (the render's word): 0 as they are, 1 mix, 2 left, 3 right.
  static constexpr std::uint8_t kMapAsIs = 0;
  static constexpr std::uint8_t kMapMix = 1;
  static constexpr std::uint8_t kMapLeft = 2;
  static constexpr std::uint8_t kMapRight = 3;

  void bind(hal::capture::CaptureHw* hw, dsp::Engine* engine) noexcept {
    hw_ = hw;
    engine_ = engine;
  }
  // The runtime's, every pass of the main loop: the gate's reopening, the retry, the peaks.
  void poll(std::uint64_t now_ms) noexcept;
  // The settings' way in (docs/formats/o3sdk-settings.md): the saved choice, applied over the app's defaults. With a capture device it is select() and monitor();
  // without one the values are only kept, so that a settings file written by a later run with a device does not lose them. Out-of-range values are clamped.
  void restore(Source source, unsigned gain_percent, unsigned monitor_percent) noexcept;
  // The render's: how to treat the line's channels (Runtime::render_block).
  std::uint8_t channel_map() const noexcept { return map_.load(std::memory_order_relaxed); }
  static std::uint8_t map_for(Source s) noexcept;
  bool send(Pending what) noexcept;
  bool radio_is_dead() const noexcept;

  hal::capture::CaptureHw* hw_ = nullptr;
  dsp::Engine* engine_ = nullptr;
  Source source_ = Source::LineMix;
  std::uint8_t gain_ = 0;
  std::uint8_t monitor_ = 0;
  bool settling_ = false;
  bool selected_ = false;
  bool map_pending_ = false;        // the channel map waits for the gate to be shut (poll applies it)
  std::uint8_t map_next_ = kMapAsIs;
  std::uint64_t map_at_ = 0;
  Pending pending_ = Pending::None;
  std::uint64_t settle_until_ = 0;  // uptime ms
  std::uint64_t retry_at_ = 0;
  std::uint64_t now_ms_ = 0;
  std::uint64_t peaks_until_ = 0;   // the app asked for peaks until then (0: not asked)
  std::uint64_t last_poll_ms_ = 0;
  bool watching_ = false;
  bool measuring_ = false;          // the engine's peaks are on
  float held_l_ = 0.0f;             // the largest since the app's last peaks()
  float held_r_ = 0.0f;
  float meter_l_ = 0.0f;
  float meter_r_ = 0.0f;
  std::atomic<std::uint8_t> map_{kMapAsIs};  // written by the main loop, read once a block by the render
};

}  // namespace o3sdk::app
