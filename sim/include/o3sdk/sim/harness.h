// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The simulator's core: one app, run on the host with the HAL fakes (sim/README.md).
//
//   * Time moves only through run_ms(): the main loop is polled once a millisecond and the audio is rendered in lockstep, one block
//     each time a block's worth of samples is due at the engine's rate, so a run is deterministic.
//   * The display is the FT81x interpreter (EveRaster) behind hal::Display; the pads' LEDs are a fake hal::Leds.
//   * panel_image() is the screen above a drawing of the 48 pads: each pad lit to its LED level, a held pad framed in blue.
//   * The audio can be recorded and written as a 16-bit stereo WAV at the engine's rate.

#include <o3sdk/app/app.h>
#include <o3sdk/app/panel.h>
#include <o3sdk/app/runtime.h>
#include <o3sdk/dsp/engine.h>
#include <o3sdk/hal/leds.h>
#include <o3sdk/platform/host/fakes.h>
#include <o3sdk/sim/eve_raster.h>
#include <o3sdk/sim/raster_display.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace o3sdk::sim {

// hal::Leds of the simulator: keeps what was staged; flush() always finishes at once.
class FakeLeds final : public hal::Leds {
 public:
  unsigned count() const noexcept override { return hal::kPadLedCount; }
  void set(unsigned index, std::uint8_t level) noexcept override {
    if (index < hal::kPadLedCount) {
      level_[index] = level;
    }
  }
  void clear() noexcept override {
    for (auto& l : level_) {
      l = 0;
    }
  }
  bool flush() noexcept override { return true; }
  bool online() const noexcept override { return true; }
  std::uint8_t level(unsigned index) const noexcept { return index < hal::kPadLedCount ? level_[index] : 0; }

 private:
  std::uint8_t level_[hal::kPadLedCount] = {};
};

class Harness {
 public:
  explicit Harness(app::App& app);
  ~Harness();
  Harness(const Harness&) = delete;
  Harness& operator=(const Harness&) = delete;

  // Runtime::start(): the assets, App::setup, the engine. false: the display refused the assets, or the app's setup had a call that
  // the engine refused (Engine::setup_errors() != 0: an attach, a route or a hook it did not take). Then setup_report() says which,
  // and start() has also written it to stderr, so a test that REQUIREs start() shows why it failed.
  bool start();
  // One line per refused setup call, e.g. "setup refused: set_source(slot 9, unit 0): slot, bus or index out of range\n"; "" when the
  // setup went through (at most Engine's kMaxSetupErrors lines, then a count of the rest).
  std::string setup_report() const;
  // Moves time on by `ms` milliseconds.
  void run_ms(std::uint32_t ms);

  void press(app::Button b);
  void release(app::Button b);
  void pad_down(unsigned pad);
  void pad_up(unsigned pad);
  void turn(int detents);

  // The screen above the pads: RGBA8, row-major.
  std::vector<std::uint8_t> panel_image(int& width, int& height) const;
  bool snapshot(const std::string& path) const;

  // Audio capture: while on, every rendered block is kept (interleaved L, R int16).
  void record(bool on) { recording_ = on; }
  const std::vector<std::int16_t>& audio() const { return audio_; }
  bool write_wav(const std::string& path) const;

  app::Runtime& runtime() { return *runtime_; }
  dsp::Engine& engine() { return *engine_; }
  const EveRaster& raster() const { return *raster_; }
  const RasterDisplay& display() const { return *display_; }
  const FakeLeds& leds() const { return leds_; }
  std::uint64_t uptime_us() const { return clock_.uptime_us(); }
  std::uint64_t samples_rendered() const { return samples_; }

 private:
  void push(core::InputKind kind, std::uint8_t id, std::int16_t value);
  void render_due();

  app::App& app_;
  std::unique_ptr<EveRaster> raster_;
  std::unique_ptr<RasterDisplay> display_;
  platform::host::FakeInput input_;
  platform::host::FakeClock clock_;
  FakeLeds leds_;
  std::unique_ptr<dsp::Engine> engine_;
  std::unique_ptr<app::Platform> platform_;
  std::unique_ptr<app::Runtime> runtime_;
  std::uint64_t samples_ = 0;
  bool recording_ = false;
  std::vector<std::int16_t> audio_;
};

// A 16-bit stereo PCM WAV of interleaved samples at `rate` Hz.
bool write_wav_s16(const std::string& path, const std::vector<std::int16_t>& lr, std::uint32_t rate);

}  // namespace o3sdk::sim
